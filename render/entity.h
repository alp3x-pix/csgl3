#ifndef ENTITY_H
#define ENTITY_H

namespace Render
{

// sort translucent entities based on distance to viewer
void entitySortTranslucents(const Vector3 &cameraPosition);

// draw the world and solid brush entities
void entityDrawSolidBrushes();

// draw solid studio models and sprites
void entityDrawSolidEntities();

// draw all translucent models
void entityDrawTranslucentEntities(const Vector3 &origin, const Vector3 &forward);

// draw the viewmodel, done last
void entityDrawViewmodel(int drawFlags);

// also used by beams
int entityUpdateRenderAmt(cl_entity_t *entity, const Vector3 &origin, const Vector3 &forward);

// clears all entities marked to draw this frame
void entityClearBuckets();

// emulates the engine's CL_MoveAiments for the entities we took over from the engine:
// the engine only walks its own visible entity list, so MOVETYPE_FOLLOW entities that
// we draw (sprites, models...) would never get the origin of their aiment otherwise
void entityMoveAiments();

// returns the world space position of an entity's attachment (1 based), or the entity's
// origin when attachmentIndex is 0. in first person, the local player's attachments are
// taken straight from the viewmodel so effects (muzzleflashes etc.) line up with the gun
Vector3 entityGetAttachmentPoint(cl_entity_t *entity, int attachmentIndex);

// get beam entities added for this frame
cl_entity_t **entityGetBeams(int &count);

}

#endif
