#ifndef GHOST_RENDER_H
#define GHOST_RENDER_H

#include <Adafruit_GFX.h>

enum GhostMood {
  GHOST_VERY_LOW,
  GHOST_LOW,
  GHOST_MODERATE,
  GHOST_HIGH,
  GHOST_VERY_HIGH,
  GHOST_UNKNOWN,  // no reading at all yet - cold boot, never synced
};

GhostMood mood_from_index(const String &index_word);

// tag is one of "(live)", "(cached)", "" (unknown/never synced) - drawn
// bottom-right, honest provenance: live data vs a cached reading.
void render_ghost(GFXcanvas1 *canvas, GhostMood mood, int intensity,
                   const String &recommendation, const char *tag);

#endif  // !GHOST_RENDER_H
