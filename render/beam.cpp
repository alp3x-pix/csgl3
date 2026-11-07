#include "stdafx.h"
#include "beam.h"
#include "entity.h"
#include "immediate.h"
#include "random.h"
#include "particle.h"
#include "internal.h"
#include "hudgl3.h"
#include "triapigl3.h"

#define NOT_IMPL() \
    do \
    { \
        static bool bitched; \
        if (!bitched) \
            g_engfuncs.Con_Printf("%s not implemented\n", __FUNCTION__); \
        bitched = true; \
    } while (0)

namespace Render
{

constexpr auto MaxBeams = 128;

static BEAM s_beams[MaxBeams];

// the BEAM struct has no room for these so keep them on the side, indexed like s_beams
constexpr int MaxTrailPoints = 128;
constexpr float TrailPointSpacing = 12.0f;

struct TrailPoint
{
    Vector3 origin;
    float die;
};

static TrailPoint s_trailPoints[MaxBeams][MaxTrailPoints]; // newest first
static int s_trailCount[MaxBeams];
static float s_beamBirth[MaxBeams];
static float s_beamLastLive[MaxBeams]; // last time the followed entity was part of a server update
static float s_beamLife[MaxBeams];

// returns -1 for beams that don't live in the pool (entity beams)
static int BeamIndex(const BEAM &beam)
{
    uintptr_t address = reinterpret_cast<uintptr_t>(&beam);
    uintptr_t first = reinterpret_cast<uintptr_t>(&s_beams[0]);
    uintptr_t last = reinterpret_cast<uintptr_t>(&s_beams[MaxBeams - 1]);

    if (address < first || address > last)
    {
        return -1;
    }

    return static_cast<int>(&beam - s_beams);
}

// a trail whose entity has been missing from the server updates for this long is considered orphaned
// (entities buried inside a wall or outside the PVS are not sent for a while, that must not end the trail)
constexpr float BeamFollowGrace = 0.25f;

// an entity that jumps further than this between two frames is a different entity (reused slot) or a teleport
constexpr float MaxTrailJump = 512.0f;

// is the entity part of the most recent server update?
// cl_entities slots keep their old origin and model forever after the entity is gone
static bool BeamEntityIsLive(const cl_entity_t *entity)
{
    cl_entity_t *local = g_engfuncs.GetLocalPlayer();
    if (!entity || !local)
    {
        return false;
    }

    if (entity == local)
    {
        return true;
    }

    return entity->curstate.messagenum == local->curstate.messagenum;
}

// stop following the entity but let the trail that is left fade away (TE_BEAMFOLLOW stores the life in amplitude)
static void BeamDetachFollow(BEAM &beam, float now)
{
    beam.flags &= ~(FBEAM_STARTENTITY | FBEAM_STARTVISIBLE | FBEAM_FOREVER);
    beam.die = now + Q_max(beam.amplitude, 0.1f);
}

// need to use these shitty lists to stay compatible
static BEAM *s_freeBeams;
static BEAM *s_activeBeams;

constexpr int NoiseCount = 128;
static float s_noise[NoiseCount + 1];

void beamInit()
{
    // nothing currently
}

void beamClear()
{
    s_activeBeams = nullptr;

    s_freeBeams = &s_beams[0];

    for (int i = 0; i < MaxBeams; i++)
    {
        s_beams[i].next = &s_beams[i + 1];
    }

    s_beams[MaxBeams - 1].next = nullptr;
}

BEAM *beamAllocate()
{
    BEAM *beam = s_freeBeams;
    if (!beam)
    {
        return nullptr;
    }

    s_freeBeams = s_freeBeams->next;

    beam->next = s_activeBeams;
    s_activeBeams = beam;

    int index = BeamIndex(*beam);
    s_trailCount[index] = 0;
    s_beamBirth[index] = g_engfuncs.GetClientTime();
    s_beamLastLive[index] = s_beamBirth[index];
    s_beamLife[index] = 0;

    return beam;
}

void beamSetup(
    BEAM &beam,
    const Vector3 &start,
    const Vector3 &end,
    int modelIndex,
    float life,
    float width,
    float amplitude,
    float brightness,
    float speed)
{
    model_t *model = g_engineStudio.GetModelByIndex(modelIndex);
    if (!model)
    {
        // make sure the (already allocated) beam gets thrown away instead of drawing stale data
        beam.modelIndex = -1;
        beam.flags = 0;
        beam.die = 0;
        return;
    }

    float clientTime = g_engfuncs.GetClientTime();

    {
        int index = BeamIndex(beam);
        if (index >= 0)
        {
            s_beamBirth[index] = clientTime;
            s_beamLastLive[index] = clientTime;
            s_beamLife[index] = life;
        }
    }

    beam.type = TE_BEAMPOINTS;
    beam.modelIndex = modelIndex;
    beam.frame = 0;
    beam.frameRate = 0;
    beam.frameCount = model->numframes; // would break with studio models
    beam.source = start;
    beam.target = end;
    beam.delta = end - start;
    beam.freq = clientTime * speed;
    beam.die = clientTime + life;
    beam.width = width;
    beam.amplitude = amplitude;
    beam.speed = speed;
    beam.brightness = brightness;

    if (amplitude >= 0.5f)
    {
        beam.segments = (int)(VectorLength(beam.delta) * 0.25f + 3.0f);
    }
    else
    {
        beam.segments = (int)(VectorLength(beam.delta) * 0.075f + 3.0f);
    }

    beam.flags = 0;
    beam.pFollowModel = nullptr;
}

static void FractalNoise(float *noise, int current)
{
    if (current < 2)
    {
        return;
    }

    int next = current / 2;

    float jitter = randomFloat(-0.125f, 0.125f) * (float)current;
    noise[next] = (noise[0] + noise[current]) * 0.5f + jitter;

    FractalNoise(&noise[next], next);
    FractalNoise(noise, next);
}

static void SineNoise(float *dest, int count)
{
    float a = 0;
    float b = F_PI / count;

    for (int i = 0; i < count; i++)
    {
        dest[i] = sinf(a);
        a += b;
    }
}

cl_entity_t *beamGetBeamEntity(int index)
{
    if (index < 0)
    {
        // not used by COUNTERSTRIKE so not implemented
        //return cl_funcs.pGetUserEntity(BEAMENT_ENTITY(-index));
        return nullptr;
    }

    return g_engfuncs.GetEntityByIndex(BEAMENT_ENTITY(index));
}

static bool FrustumCullBeam(const Vector3 &start, const Vector3 &end)
{
    // don't perform a line cull, it causes popping
    // use sloppy sphere test, although it can be inefficient...
    Vector3 center = VectorLerp(start, end, 0.5f);
    float radius = PointDistance(center, start);
    return g_state.viewFrustum.CullSphere(center, radius);
}

static Vector3 ScreenTransform(const Vector3 &world)
{
    float x, y;
    hudWorldToScreen(world, x, y);
    return { x, y, 0 };
}

// biirredää vähä segsiä :DDD
static void DrawSegs(const Vector3 &source, const Vector3 &delta, float width, float scale, float freq, float speed, int segments, int flags)
{
    if (segments <= 1)
    {
        return;
    }

    segments = Q_min(segments, NoiseCount);

    float length = Q_max(VectorLength(delta) * 0.01f, 0.5f);

    float div = 1.0f / (float)(segments - 1);
    float vStep = length * div;

    float vLast = fmodf(freq * speed, 1.0f);

    if (flags & FBEAM_SINENOISE)
    {
        segments = Q_max(segments, 16);
        length = (float)segments * 0.1f;
        scale *= 100.0f;
    }
    else
    {
        scale *= length;
    }

    Vector3 last1, last2;
    Vector3 screen, screenLast;
    {
        screenLast = ScreenTransform(source);
        screen = ScreenTransform(source + delta * div);

        Vector3 tmp = screen - screenLast;
        VectorNormalize(tmp);

        Vector3 normal = (g_state.viewUp * tmp.x) - (g_state.viewRight * tmp.y);

        last1 = source + normal * width;
        last2 = source - normal * width;
    }

    int noiseIndex = (flags & FBEAM_SINENOISE) ? 0 : (int)(div * NoiseCount * 65536.0f);
    float brightness = (flags & FBEAM_SHADEIN) ? 0.0f : 1.0f;

    for (int i = 1; i < segments; i++)
    {
        float fraction = (float)(i * div);

        g_triapiGL3.Brightness(brightness);
        g_triapiGL3.TexCoord2f(0, vLast);
        g_triapiGL3.Vertex3fv(&last1.x);

        g_triapiGL3.Brightness(brightness);
        g_triapiGL3.TexCoord2f(1, vLast);
        g_triapiGL3.Vertex3fv(&last2.x);

        if (flags & FBEAM_SHADEIN)
        {
            brightness = fraction;
        }
        else if (flags & FBEAM_SHADEOUT)
        {
            brightness = 1.0f - fraction;
        }

        Vector3 point = source + delta * fraction;

        if (scale != 0.0f)
        {
            float jitter = scale * s_noise[noiseIndex / 65536];
            if (flags & FBEAM_SINENOISE)
            {
                point += (g_state.viewUp * sinf(F_PI * fraction * length + freq) * jitter);
                point += (g_state.viewRight * cosf(F_PI * fraction * length + freq) * jitter);
            }
            else
            {
                point += (g_state.viewUp * jitter);
                point += (g_state.viewRight * cosf(F_PI * fraction * 3.0f + freq) * jitter);
            }
        }

        screen = ScreenTransform(&point.x);

        Vector3 tmp = screen - screenLast;
        VectorNormalize(tmp);

        screenLast = screen;

        Vector3 normal = (g_state.viewUp * tmp.x) - (g_state.viewRight * tmp.y);

        last1 = point + normal * width;
        last2 = point - normal * width;

        vLast = vLast + vStep;
        g_triapiGL3.Brightness(brightness);
        g_triapiGL3.TexCoord2f(1, vLast);
        g_triapiGL3.Vertex3fv(&last2.x);
        g_triapiGL3.Brightness(brightness);
        g_triapiGL3.TexCoord2f(0, vLast);
        g_triapiGL3.Vertex3fv(&last1.x);

        vLast = fmodf(vLast, 1.0f);
        noiseIndex += (int)(div * NoiseCount * 65536.0f);
    }
}

// ---------------------------------------------------------------------------
// circle beams (TE_BEAMTORUS / TE_BEAMDISK / TE_BEAMCYLINDER / TE_BEAMRING)
// not a port of the engine's code, written from how the effects are documented
// ("expands to max radius over lifetime"), so details may differ a bit
// ---------------------------------------------------------------------------

constexpr int CircleSegments = 64;

static float BeamElapsed(const BEAM &beam)
{
    int index = BeamIndex(beam);
    if (index < 0)
    {
        return 0;
    }

    return Q_max(g_engfuncs.GetClientTime() - s_beamBirth[index], 0.0f);
}

// the engine uses freq * delta[2] as the radius, with freq being the beam's age
static float CircleRadius(const BEAM &beam)
{
    float rate = fabsf(beam.delta.z);
    if (rate < 0.001f)
    {
        rate = VectorLength(beam.delta);
    }

    return BeamElapsed(beam) * rate;
}

// whole number of texture repeats around a circle so there's no visible seam
static float CircleRepeats(float radius, float tileLength)
{
    return Q_max(1.0f, floorf((2.0f * F_PI * radius) / tileLength));
}

static void DrawTorus(const BEAM &beam)
{
    float radius = CircleRadius(beam);
    if (radius < 0.5f)
    {
        return;
    }

    float repeats = CircleRepeats(radius, 64.0f);
    const float width = beam.width;

    for (int i = 0; i <= CircleSegments; i++)
    {
        float fraction = (float)i / (float)CircleSegments;
        float angle = 2.0f * F_PI * fraction;
        float s = sinf(angle);
        float c = cosf(angle);

        Vector3 point = beam.source + Vector3{ s * radius, c * radius, 0 };
        Vector3 tangent = { c, -s, 0 };

        // screen aligned like the beams, thick band that always faces the viewer
        Vector3 screen0 = ScreenTransform(point);
        Vector3 screen1 = ScreenTransform(point + tangent * 8.0f);

        Vector3 tmp = screen1 - screen0;
        tmp.z = 0;
        if (VectorLengthSquared(tmp) < 1e-8f)
        {
            tmp = { 1, 0, 0 };
        }
        VectorNormalize(tmp);

        Vector3 normal = (g_state.viewUp * tmp.x) - (g_state.viewRight * tmp.y);

        Vector3 v1 = point + normal * width;
        Vector3 v2 = point - normal * width;

        float v = fraction * repeats;

        g_triapiGL3.TexCoord2f(0, v);
        g_triapiGL3.Vertex3fv(&v1.x);
        g_triapiGL3.TexCoord2f(1, v);
        g_triapiGL3.Vertex3fv(&v2.x);
    }
}

static void DrawDisk(const BEAM &beam)
{
    float radius = CircleRadius(beam);
    if (radius < 0.5f)
    {
        return;
    }

    float repeats = CircleRepeats(radius, 64.0f);

    for (int i = 0; i <= CircleSegments; i++)
    {
        float fraction = (float)i / (float)CircleSegments;
        float angle = 2.0f * F_PI * fraction;

        Vector3 center = beam.source;
        Vector3 edge = beam.source + Vector3{ sinf(angle) * radius, cosf(angle) * radius, 0 };

        float v = fraction * repeats;

        g_triapiGL3.TexCoord2f(0, v);
        g_triapiGL3.Vertex3fv(&center.x);
        g_triapiGL3.TexCoord2f(1, v);
        g_triapiGL3.Vertex3fv(&edge.x);
    }
}

static void DrawCylinder(const BEAM &beam)
{
    float radius = CircleRadius(beam);
    if (radius < 0.5f)
    {
        return;
    }

    float repeats = CircleRepeats(radius, 64.0f);
    const float height = Q_max(beam.width, 1.0f);

    for (int i = 0; i <= CircleSegments; i++)
    {
        float fraction = (float)i / (float)CircleSegments;
        float angle = 2.0f * F_PI * fraction;

        float x = sinf(angle) * radius;
        float y = cosf(angle) * radius;

        Vector3 top = beam.source + Vector3{ x, y, height };
        Vector3 bottom = beam.source + Vector3{ x, y, -height };

        float v = fraction * repeats;

        g_triapiGL3.TexCoord2f(1, v);
        g_triapiGL3.Vertex3fv(&top.x);
        g_triapiGL3.TexCoord2f(0, v);
        g_triapiGL3.Vertex3fv(&bottom.x);
    }
}

// ring between two points: a circle facing the viewer whose diameter is the line source -> source + delta
static void DrawRing(const BEAM &beam)
{
    float diameter = VectorLength(beam.delta);
    if (diameter < 1.0f)
    {
        return;
    }

    float radius = diameter * 0.5f;
    Vector3 center = beam.source + beam.delta * 0.5f;

    Vector3 axis = beam.delta * (1.0f / diameter);

    Vector3 side = Cross(axis, g_state.viewForward);
    if (VectorLengthSquared(side) < 1e-6f)
    {
        side = Cross(axis, g_state.viewUp);
    }
    VectorNormalize(side);

    const int segments = 48;
    float repeats = CircleRepeats(radius, 64.0f);
    const float width = beam.width;

    for (int i = 0; i <= segments; i++)
    {
        float fraction = (float)i / (float)segments;
        float angle = 2.0f * F_PI * fraction;

        // outward direction of the ring at this point
        Vector3 outward = axis * cosf(angle) + side * sinf(angle);
        Vector3 point = center + outward * radius;

        Vector3 v1 = point + outward * width;
        Vector3 v2 = point - outward * width;

        float v = fraction * repeats;

        g_triapiGL3.TexCoord2f(0, v);
        g_triapiGL3.Vertex3fv(&v1.x);
        g_triapiGL3.TexCoord2f(1, v);
        g_triapiGL3.Vertex3fv(&v2.x);
    }
}

// ---------------------------------------------------------------------------
// TE_BEAMFOLLOW: a trail of fading segments behind an entity
// R_BeamFollow stores the segment lifetime in amplitude
// ---------------------------------------------------------------------------

// trail geometry closer than this to the camera is not drawn, it fades in over the next TrailFadeDistance units
constexpr float TrailNearDistance = 12.0f;
constexpr float TrailFadeDistance = 36.0f;

// the ribbon is never wider than this fraction of the screen height (per side), so trails passing close to
// the camera stay thin instead of filling the screen
constexpr float TrailMaxHalfWidth = 0.02f;

static void DrawFollowRun(const Vector3 *positions, const float *brightness, const float *depth, int nodes, float width)
{
    if (nodes < 2)
    {
        return;
    }

    // world units -> pixels at a depth of one unit
    SCREENINFO screenInfo{};
    screenInfo.iSize = sizeof(screenInfo);
    g_engfuncs.pfnGetScreenInfo(&screenInfo);
    float pixelsPerUnit = g_state.projectionMatrix.m11 * screenInfo.iHeight * 0.5f;
    float maxHalfPixels = TrailMaxHalfWidth * screenInfo.iHeight;

    // screen space direction of every segment
    Vector3 screen[MaxTrailPoints + 4];
    for (int i = 0; i < nodes; i++)
    {
        screen[i] = ScreenTransform(positions[i]);
    }

    Vector3 segmentDir[MaxTrailPoints + 4];
    for (int i = 0; i < nodes - 1; i++)
    {
        Vector3 dir = screen[i + 1] - screen[i];
        dir.z = 0;
        if (VectorLengthSquared(dir) < 1e-8f)
        {
            dir = (i > 0) ? segmentDir[i - 1] : Vector3{ 1, 0, 0 };
        }
        VectorNormalize(dir);
        segmentDir[i] = dir;
    }

    // per node normals, averaged between the adjoining segments so the trail has no gaps
    Vector3 normals[MaxTrailPoints + 4];
    float halfWidth[MaxTrailPoints + 4];
    float alpha[MaxTrailPoints + 4];
    for (int i = 0; i < nodes; i++)
    {
        Vector3 dir;
        if (i == 0)
        {
            dir = segmentDir[0];
        }
        else if (i == nodes - 1)
        {
            dir = segmentDir[nodes - 2];
        }
        else
        {
            dir = segmentDir[i - 1] + segmentDir[i];
            if (VectorLengthSquared(dir) < 1e-8f)
            {
                dir = segmentDir[i];
            }
            VectorNormalize(dir);
        }

        normals[i] = (g_state.viewUp * dir.x) - (g_state.viewRight * dir.y);

        halfWidth[i] = Q_min(width, maxHalfPixels * depth[i] / Q_max(pixelsPerUnit, 1e-3f));
        alpha[i] = brightness[i] * Q_clamp((depth[i] - TrailNearDistance) / TrailFadeDistance, 0.0f, 1.0f);
    }

    for (int i = 0; i < nodes - 1; i++)
    {
        Vector3 a1 = positions[i] + normals[i] * halfWidth[i];
        Vector3 a2 = positions[i] - normals[i] * halfWidth[i];
        Vector3 b1 = positions[i + 1] + normals[i + 1] * halfWidth[i + 1];
        Vector3 b2 = positions[i + 1] - normals[i + 1] * halfWidth[i + 1];

        // every segment maps the whole texture once
        g_triapiGL3.Brightness(alpha[i]);
        g_triapiGL3.TexCoord2f(0, 0);
        g_triapiGL3.Vertex3fv(&a1.x);

        g_triapiGL3.Brightness(alpha[i]);
        g_triapiGL3.TexCoord2f(1, 0);
        g_triapiGL3.Vertex3fv(&a2.x);

        g_triapiGL3.Brightness(alpha[i + 1]);
        g_triapiGL3.TexCoord2f(1, 1);
        g_triapiGL3.Vertex3fv(&b2.x);

        g_triapiGL3.Brightness(alpha[i + 1]);
        g_triapiGL3.TexCoord2f(0, 1);
        g_triapiGL3.Vertex3fv(&b1.x);
    }
}

static void DrawBeamFollow(BEAM &beam, float frametime)
{
    int index = BeamIndex(beam);
    if (index < 0)
    {
        return;
    }

    float now = g_engfuncs.GetClientTime();
    float life = Q_max(beam.amplitude, 0.1f);

    TrailPoint *points = s_trailPoints[index];
    int count = s_trailCount[index];

    // oldest points are at the end of the list
    while (count > 0 && points[count - 1].die < now)
    {
        count--;
    }

    // still attached to a live entity?
    bool tracking = (beam.flags & FBEAM_STARTENTITY) != 0;

    if (tracking)
    {
        // the beam lives as long as the entity does
        beam.die = now + life;

        // the entity jumped (slot reused by a new entity, teleport): don't draw a line across the map
        if (count > 0 && VectorLength(points[0].origin - beam.source) > MaxTrailJump)
        {
            count = 0;
        }

        if (count == 0 || VectorLength(points[0].origin - beam.source) >= TrailPointSpacing)
        {
            if (count == MaxTrailPoints)
            {
                count--;
            }

            for (int i = count; i > 0; i--)
            {
                points[i] = points[i - 1];
            }

            points[0].origin = beam.source;
            points[0].die = now + life;
            count++;
        }
    }

    s_trailCount[index] = count;

    if (!tracking && count == 0)
    {
        // entity is gone and the whole trail has faded out
        beam.die = now - 1.0f;
        return;
    }

    Vector3 positions[MaxTrailPoints + 1];
    float brightness[MaxTrailPoints + 1];
    float depth[MaxTrailPoints + 1];
    int nodes = 0;

    if (tracking)
    {
        positions[nodes] = beam.source;
        brightness[nodes] = 1.0f;
        nodes++;
    }

    for (int i = 0; i < count; i++)
    {
        positions[nodes] = points[i].origin;
        brightness[nodes] = Q_clamp((points[i].die - now) / life, 0.0f, 1.0f);
        nodes++;
    }

    if (nodes < 2)
    {
        return;
    }

    // distance in front of the camera, along the view direction
    for (int i = 0; i < nodes; i++)
    {
        depth[i] = Dot(positions[i] - g_state.viewOrigin, g_state.viewForward);
    }

    // Parts of the trail that are right in front of (or behind) the camera, like the muzzle of the first person
    // weapon, project to huge, unstable screen coordinates. They are cut off and the trail fades in with distance.
    // The result is split into runs so that a trail that leaves and re-enters the view isn't joined together.
    Vector3 runPositions[MaxTrailPoints + 4];
    float runBrightness[MaxTrailPoints + 4];
    float runDepth[MaxTrailPoints + 4];
    int runCount = 0;

    for (int i = 0; i < nodes; i++)
    {
        bool in = depth[i] >= TrailNearDistance;

        if (i > 0)
        {
            bool prevIn = depth[i - 1] >= TrailNearDistance;

            if (in != prevIn)
            {
                // the segment crosses the near limit
                float t = (TrailNearDistance - depth[i - 1]) / (depth[i] - depth[i - 1]);

                runPositions[runCount] = VectorLerp(positions[i - 1], positions[i], t);
                runBrightness[runCount] = brightness[i - 1] + (brightness[i] - brightness[i - 1]) * t;
                runDepth[runCount] = TrailNearDistance;
                runCount++;

                if (prevIn)
                {
                    // leaving the visible region: that ends the run
                    DrawFollowRun(runPositions, runBrightness, runDepth, runCount, beam.width);
                    runCount = 0;
                }
            }
        }

        if (in)
        {
            runPositions[runCount] = positions[i];
            runBrightness[runCount] = brightness[i];
            runDepth[runCount] = depth[i];
            runCount++;
        }
    }

    DrawFollowRun(runPositions, runBrightness, runDepth, runCount, beam.width);
}

void beamDetachFollowers(int entity)
{
    float now = g_engfuncs.GetClientTime();

    for (BEAM *beam = s_activeBeams; beam; beam = beam->next)
    {
        if (beam->type != TE_BEAMFOLLOW || !(beam->flags & FBEAM_STARTENTITY))
        {
            continue;
        }

        if (BEAMENT_ENTITY(beam->startEntity) != entity)
        {
            continue;
        }

        // several trails started together for one entity (core + glow) must not detach each other
        int index = BeamIndex(*beam);
        if (index >= 0 && now - s_beamBirth[index] < 0.05f)
        {
            continue;
        }

        BeamDetachFollow(*beam, now);
    }
}

void beamKillEntity(int deadEntity)
{
    float now = g_engfuncs.GetClientTime();

    for (BEAM *beam = s_activeBeams; beam; beam = beam->next)
    {
        bool killed = false;

        if ((beam->flags & FBEAM_STARTENTITY) && BEAMENT_ENTITY(beam->startEntity) == deadEntity)
        {
            beam->flags &= ~(FBEAM_STARTENTITY | FBEAM_STARTVISIBLE);
            killed = true;
        }

        if ((beam->flags & FBEAM_ENDENTITY) && BEAMENT_ENTITY(beam->endEntity) == deadEntity)
        {
            beam->flags &= ~(FBEAM_ENDENTITY | FBEAM_ENDVISIBLE);
            killed = true;
        }

        if (!killed)
        {
            continue;
        }

        beam->flags &= ~FBEAM_FOREVER;

        if (beam->type == TE_BEAMFOLLOW)
        {
            // let the trail fade out instead of vanishing
            beam->die = now + Q_max(beam->amplitude, 0.1f);
        }
        else
        {
            beam->die = now - 1.0f;
        }
    }
}

static Vector3 GetAttachmentPoint(cl_entity_t *entity, int attachmentIndex)
{
    if (attachmentIndex)
    {
        // local player: viewmodel attachments in first person
        return entityGetAttachmentPoint(entity, attachmentIndex);
    }

    if (entity == g_engfuncs.GetLocalPlayer())
    {
        // FIXME: correct? try irl
        return g_state.simOrigin;
    }

    return entity->origin;
}

static void DrawBeam(BEAM &beam, float frametime)
{
    float clientTime = g_engfuncs.GetClientTime();

    model_t *model = g_engineStudio.GetModelByIndex(beam.modelIndex);
    if (!model)
    {
        return;
    }

    if (beam.flags & FBEAM_SOLID)
    {
        g_triapiGL3.RenderMode(kRenderNormal);
    }
    else
    {
        g_triapiGL3.RenderMode(kRenderTransAdd);
    }

    beam.freq += frametime;

    s_noise[0] = 0;
    s_noise[NoiseCount] = 0;

    if (beam.amplitude != 0.0f)
    {
        if (beam.flags & FBEAM_SINENOISE)
        {
            SineNoise(s_noise, NoiseCount);
        }
        else
        {
            FractalNoise(s_noise, NoiseCount);
        }
    }

    if (beam.flags & (FBEAM_STARTENTITY | FBEAM_ENDENTITY))
    {
        if (beam.flags & FBEAM_STARTENTITY)
        {
            cl_entity_t *startEntity = beamGetBeamEntity(beam.startEntity);
            if (!startEntity)
            {
                return;
            }

            bool following = true;

            if (beam.type == TE_BEAMFOLLOW)
            {
                int index = BeamIndex(beam);

                if (index >= 0 && BeamEntityIsLive(startEntity))
                {
                    s_beamLastLive[index] = clientTime;
                }
                else
                {
                    // The entity is not in the latest server update: it has not arrived yet (brand new trail), is
                    // temporarily not sent, or it is gone and its cl_entities slot only holds stale data.
                    // Never read the stale slot. Keep the last known head for a moment, then let go of the entity.
                    following = false;

                    if (index >= 0 && clientTime - s_beamLastLive[index] > BeamFollowGrace)
                    {
                        BeamDetachFollow(beam, clientTime);
                    }
                }
            }

            if (!following)
            {
                // nothing to update, a trail that never saw its entity has nothing to draw (STARTVISIBLE check below)
            }
            else if (startEntity->model && (!beam.pFollowModel || beam.pFollowModel == startEntity->model))
            {
                beam.source = GetAttachmentPoint(startEntity, BEAMENT_ATTACHMENT(beam.startEntity));
                beam.flags |= FBEAM_STARTVISIBLE;

                if (!beam.pFollowModel)
                {
                    beam.pFollowModel = startEntity->model;
                }
            }
            else
            {
                if (!(beam.flags & FBEAM_FOREVER))
                {
                    beam.flags &= ~FBEAM_STARTENTITY;
                }
            }
        }

        if (beam.flags & FBEAM_ENDENTITY)
        {
            cl_entity_t *endEntity = beamGetBeamEntity(beam.endEntity);
            if (!endEntity)
            {
                return;
            }

            if (endEntity->model)
            {
                beam.target = GetAttachmentPoint(endEntity, BEAMENT_ATTACHMENT(beam.endEntity));
                beam.flags |= FBEAM_ENDVISIBLE;
            }
            else
            {
                if (!(beam.flags & FBEAM_FOREVER))
                {
                    beam.flags &= ~FBEAM_ENDENTITY;
                    beam.die = clientTime;
                }

                return;
            }
        }

        if ((beam.flags & (FBEAM_STARTENTITY | FBEAM_STARTVISIBLE)) == FBEAM_STARTENTITY)
        {
            return;
        }

        Vector3 delta = beam.target - beam.source;
        float deltaLength = VectorLength(delta);

        if (deltaLength > 1e-7f)
        {
            beam.delta = delta;
        }
        else
        {
            deltaLength = VectorLength(beam.delta);
        }

        /* FIXME: dupe code? */
        if (beam.amplitude < 0.5f)
        {
            beam.segments = static_cast<int>(deltaLength * 0.075f + 3.0f);
        }
        else
        {
            beam.segments = static_cast<int>(deltaLength * 0.25f + 3.0f);
        }
    }

    if (beam.type == TE_BEAMPOINTS && FrustumCullBeam(beam.source, beam.target))
    {
        return;
    }

    int frame = static_cast<int>(beam.frameRate * clientTime + beam.frame) % Q_max(beam.frameCount, 1);
    if (!g_triapiGL3.SpriteTexture(model, frame))
    {
        return;
    }

    beam.t = (beam.die - clientTime) + beam.freq;
    if (beam.t != 0.0f)
    {
        beam.t = 1.0f - (beam.freq / beam.t);
    }

    if (beam.flags & FBEAM_FADEIN)
    {
        g_triapiGL3.Color4f(beam.r, beam.g, beam.b, beam.t * beam.brightness);
    }
    else if (beam.flags & FBEAM_FADEOUT)
    {
        g_triapiGL3.Color4f(beam.r, beam.g, beam.b, (1.0f - beam.t) * beam.brightness);
    }
    else
    {
        g_triapiGL3.Color4f(beam.r, beam.g, beam.b, beam.brightness);
    }

    switch (beam.type)
    {
    case TE_BEAMPOINTS:
        g_triapiGL3.Begin(TRI_QUADS);
        DrawSegs(
            beam.source,
            beam.delta,
            beam.width,
            beam.amplitude,
            beam.freq,
            beam.speed,
            beam.segments,
            beam.flags);
        g_triapiGL3.End();
        break;

    case TE_BEAMTORUS:
        g_triapiGL3.Begin(TRI_QUAD_STRIP);
        DrawTorus(beam);
        g_triapiGL3.End();
        break;

    case TE_BEAMDISK:
        g_triapiGL3.Begin(TRI_QUAD_STRIP);
        DrawDisk(beam);
        g_triapiGL3.End();
        break;

    case TE_BEAMCYLINDER:
        g_triapiGL3.Begin(TRI_QUAD_STRIP);
        DrawCylinder(beam);
        g_triapiGL3.End();
        break;

    case TE_BEAMFOLLOW:
        g_triapiGL3.Begin(TRI_QUADS);
        DrawBeamFollow(beam, frametime);
        g_triapiGL3.End();
        break;

    case TE_BEAMRING:
        g_triapiGL3.Begin(TRI_QUAD_STRIP);
        DrawRing(beam);
        g_triapiGL3.End();
        break;
    }
}

static void DrawEntity(cl_entity_t *entity, float frametime)
{
    float amplitude = (float)entity->curstate.body * 0.01f;
    float brightness = (float)entityUpdateRenderAmt(entity, g_state.viewOrigin, g_state.viewForward) / 255.0f;
    float speed = entity->curstate.animtime;

    BEAM beam{};
    beamSetup(beam,
        entity->origin,
        entity->curstate.angles,
        entity->curstate.movetype,
        0.0f,
        entity->curstate.scale,
        amplitude,
        brightness,
        speed);

    beam.frame = (float)(int)entity->curstate.frame;
    beam.r = entity->curstate.rendercolor.r / 255.0f;
    beam.g = entity->curstate.rendercolor.g / 255.0f;
    beam.b = entity->curstate.rendercolor.b / 255.0f;

    // FIXME: no constant for the mask?
    int type = entity->curstate.rendermode & 0xF;

    if (type == BEAM_ENTPOINT)
    {
        beam.type = TE_BEAMPOINTS;
        beam.flags = FBEAM_ENDENTITY;
        beam.startEntity = 0;
        beam.endEntity = entity->curstate.skin;
    }
    else if (type == BEAM_ENTS)
    {
        beam.type = TE_BEAMPOINTS;
        beam.flags = FBEAM_STARTENTITY | FBEAM_ENDENTITY;
        beam.startEntity = entity->curstate.sequence;
        beam.endEntity = entity->curstate.skin;
    }

    int flags = entity->curstate.rendermode;

    if (flags & BEAM_FSINE)
    {
        beam.flags |= FBEAM_SINENOISE;
    }

    if (flags & BEAM_FSOLID)
    {
        beam.flags |= FBEAM_SOLID;
    }

    if (flags & BEAM_FSHADEIN)
    {
        beam.flags |= FBEAM_SHADEIN;
    }

    if (flags & BEAM_FSHADEOUT)
    {
        beam.flags |= FBEAM_SHADEOUT;
    }

    if (beam.modelIndex >= 0)
    {
        DrawBeam(beam, frametime);
    }
}

// got confused by valve's shitty code so this was rewritten, in practice should work the same
static void FreeDeadBeams(float clientTime)
{
    BEAM *prev = nullptr;
    BEAM *current = s_activeBeams;

    while (current)
    {
        if (current->die < clientTime && !(current->flags & FBEAM_FOREVER))
        {
            BEAM *next = current->next;

            if (prev)
            {
                prev->next = next;
            }
            else
            {
                s_activeBeams = next;
            }

            current->next = s_freeBeams;
            s_freeBeams = current;

            current = next;
        }
        else
        {
            prev = current;
            current = current->next;
        }
    }
}

void beamDraw()
{
    int entityCount;
    cl_entity_t **entities = entityGetBeams(entityCount);
    if (!entityCount && !s_activeBeams)
    {
        return;
    }

    int framecount;
    double curtime, oldtime;
    g_engineStudio.GetTimes(&framecount, &curtime, &oldtime);

    float clientTime = static_cast<float>(curtime);
    float frametime = static_cast<float>(curtime - oldtime);

    triapiBegin();
    g_triapiGL3.CullFace(TRI_NONE);

    FreeDeadBeams(clientTime);

    for (BEAM *beam = s_activeBeams; beam; beam = beam->next)
    {
        if (beam->modelIndex < 0)
        {
            // FIXME: does this actually happen? weird
            beam->die = clientTime;
            continue;
        }

        DrawBeam(*beam, frametime);
    }

    for (int i = 0; i < entityCount; i++)
    {
        DrawEntity(entities[i], frametime);
    }

    /* currently immediateDrawEnd resets state */
    triapiEnd();
}

}
