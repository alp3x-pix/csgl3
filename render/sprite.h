#ifndef SPRITE_H
#define SPRITE_H

namespace Render
{

void spriteInit();

// gl3_debug 1: prints a sprite summary to the console every couple of seconds
struct SpriteStats
{
    int added;      // sprites handed to us by the engine
    int drawn;
    int noFrame;    // frame lookup failed
    int culled;
    int billboard;  // billboard setup failed (upright sprites looking straight up / down)
    int attached;   // sprites positioned with an attachment point
    int followMoved;
};

extern SpriteStats g_spriteStats;

void spriteDebugReport();

void spriteBegin(bool alphaBlend);
void spriteDraw(cl_entity_t *entity, float blend);
void spriteEnd();

}

#endif
