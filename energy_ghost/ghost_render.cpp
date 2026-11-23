// Energy Ghost's mascot: a single ghost illustration whose FILL WEIGHT reads
// the grid's carbon intensity - hollow and light for a clean grid, solid
// black and jagged for a dirty one. Deliberately not a numeric dashboard
// first ("face first, numbers second") - the shape alone should say "good time" or "bad time"
// before anyone reads a word.
//
// The six mood illustrations themselves are AI-generated (Draw Things,
// see tools/gen_ghost_art.py),
// thresholded to 1bpp and packed into ghost_art.h at build time. The
// device only ever draws a pre-baked bitmap - Draw Things is a one-time
// art tool, never a runtime dependency.
#include "ghost_render.h"
#include "ghost_art.h"
#include "energy_config.h"
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <vector>

#define CANVAS_SIZE 200
#define ART_TOP 26

GhostMood mood_from_index(const String &index_word) {
  String w = index_word;
  w.toLowerCase();
  if (w == INDEX_VERY_LOW) return GHOST_VERY_LOW;
  if (w == INDEX_LOW) return GHOST_LOW;
  if (w == INDEX_MODERATE) return GHOST_MODERATE;
  if (w == INDEX_HIGH) return GHOST_HIGH;
  if (w == INDEX_VERY_HIGH) return GHOST_VERY_HIGH;
  return GHOST_UNKNOWN;
}

static void draw_wrapped(GFXcanvas1 *c, const String &text, const GFXfont *font, int top_y, int max_width) {
  c->setFont(font);
  std::vector<String> lines;
  String line;
  int start = 0, len = text.length();
  while (start < len) {
    int space_idx = text.indexOf(' ', start);
    String word = (space_idx == -1) ? text.substring(start) : text.substring(start, space_idx);
    String candidate = line.length() ? (line + " " + word) : word;
    int16_t x1, y1;
    uint16_t w, h;
    c->getTextBounds(candidate, 0, 0, &x1, &y1, &w, &h);
    if (w > (uint16_t)max_width && line.length()) {
      lines.push_back(line);
      line = word;
    } else {
      line = candidate;
    }
    start = (space_idx == -1) ? len : space_idx + 1;
  }
  if (line.length()) lines.push_back(line);

  int line_height = font ? font->yAdvance : 10;
  int cursor_top = top_y;
  for (auto &ln : lines) {
    int16_t x1, y1;
    uint16_t w, h;
    c->getTextBounds(ln, 0, 0, &x1, &y1, &w, &h);
    int cx = (CANVAS_SIZE - (int)w) / 2 - x1;
    c->setCursor(cx, cursor_top - y1);
    c->print(ln);
    cursor_top += line_height;
  }
}

void render_ghost(GFXcanvas1 *canvas, GhostMood mood, int intensity,
                   const String &recommendation, const char *tag) {
  canvas->fillScreen(0);
  canvas->setTextColor(1);
  canvas->setTextWrap(false);

  // Band word, top banner - adaptive text size.
  static const char *BAND_WORDS[] = {"VERY LOW", "LOW", "MODERATE", "HIGH", "VERY HIGH", "NO READING"};
  const char *band = BAND_WORDS[mood];
  static const GFXfont *TITLE_FONTS[] = {&FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b};
  int16_t x1, y1;
  uint16_t w, h;
  const GFXfont *title_font = TITLE_FONTS[2];
  for (int i = 0; i < 3; i++) {
    canvas->setFont(TITLE_FONTS[i]);
    canvas->getTextBounds(band, 0, 0, &x1, &y1, &w, &h);
    if ((int)w <= CANVAS_SIZE - 20) {
      title_font = TITLE_FONTS[i];
      break;
    }
  }
  canvas->setFont(title_font);
  canvas->getTextBounds(band, 0, 0, &x1, &y1, &w, &h);
  canvas->setCursor((CANVAS_SIZE - (int)w) / 2 - x1, 20 - y1);
  canvas->print(band);
  canvas->setFont(nullptr);

  int art_x = (CANVAS_SIZE - ART_SIZE) / 2;
  canvas->drawBitmap(art_x, ART_TOP, GHOST_ART[mood], ART_SIZE, ART_SIZE, 1);

  canvas->setTextSize(2);
  if (mood == GHOST_UNKNOWN) {
    canvas->setCursor(38, 138);
    canvas->print("no data yet");
  } else {
    char numbers[24];
    snprintf(numbers, sizeof(numbers), "%d gCO2/kWh", intensity);
    canvas->getTextBounds(numbers, 0, 0, &x1, &y1, &w, &h);
    canvas->setCursor((CANVAS_SIZE - (int)w) / 2 - x1, 138);
    canvas->print(numbers);
  }

  // Recommendation can run to 3 lines once it carries a region suffix (e.g.
  // "AVOID IF POSSIBLE - 412 gCO2/kWh, no relief in sight (South West
  // England)") - starting at 154 with 10px lines leaves room for 4 before
  // running off the 200px panel, comfortable headroom above the worst case.
  canvas->setTextSize(1);
  draw_wrapped(canvas, recommendation, nullptr, 154, CANVAS_SIZE - 16);

  if (tag[0]) {
    canvas->setFont(nullptr);
    canvas->setTextSize(1);
    canvas->getTextBounds(tag, 0, 0, &x1, &y1, &w, &h);
    canvas->setCursor(CANVAS_SIZE - (int)w - 4, CANVAS_SIZE - (int)h - 4);
    canvas->print(tag);
  }
}
