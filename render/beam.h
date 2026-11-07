#ifndef R_BEAM_H
#define R_BEAM_H

namespace Render
{

void beamInit();
void beamClear();

BEAM *beamAllocate();

void beamSetup(
    BEAM &beam,
    const Vector3 &start,
    const Vector3 &end,
    int modelIndex,
    float life,
    float width,
    float amplitude,
    float brightness,
    float speed);

cl_entity_t *beamGetBeamEntity(int index);

// TE_KILLBEAM: detach/retire every beam attached to the given entity
void beamKillEntity(int deadEntity);

// a new TE_BEAMFOLLOW is about to start on this entity index: older trails still attached to the
// same index belong to an entity that no longer exists (the slot got reused), so let them go
void beamDetachFollowers(int entity);

void beamDraw();

}

#endif // R_BEAM_H
