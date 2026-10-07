#pragma once

#include <cstdint>

#include "ui/radar_geometry.h"

namespace ui::tags {

/**
 * Aircraft tag placement. Each frame: beginFrame(), register everything a tag must not
 * cover, then place() the tags nearest-first. A tag tries eight slots around its icon and
 * takes the cheapest one that fits. While last frame's slot still fits, the tag only leaves it
 * for a slot that has been cheaper (by more than the keep bonus) for several frames in a row,
 * so tags don't flick between slots. A tag also keeps last frame's anchor until its icon has
 * drifted more than kTagHoldPx, so sub-pixel motion doesn't step it every frame.
 */

using radar::ScreenRect;

constexpr int kNoOwner = -1;

/** Default preference order for aircraft west of centre; mirrored east of it. */
enum class Slot : uint8_t { Right, Left, UpperRight, LowerRight, UpperLeft, LowerLeft, Above, Below };

/** How the tag's lines line up: toward the icon. */
enum class Align : uint8_t { Left, Center, Right };
Align slotAlign(Slot slot);

/** Clears the obstacles and ages the slot memory. */
void beginFrame();

/** Obstacles a tag may never cover. A circle's owner is skipped while placing that owner's tag. */
void addRect(const ScreenRect& rect);
void addCircle(int x, int y, int radius, int owner = kNoOwner);
void addSegment(int x0, int y0, int x1, int y1, float half_width);
/** An obstacle a tag may cover if nothing better is free (airport labels). */
void addSoftRect(const ScreenRect& rect);

struct TagRequest {
  const char* hex;   // key for the slot memory; empty = none
  int owner;         // the icon's addCircle owner
  int x;             // icon centre, rounded
  int y;
  float fx;          // icon centre, unrounded: decides when a held anchor moves
  float fy;
  int icon_radius;
  bool moving;       // has a track and groundspeed: avoid the side the speed vector points to
  float track_deg;
  int w;             // tag size
  int h;
  bool full;         // chosen for a full tag (remembered even if only the short one fits)
};

/** Whether the aircraft's tag was chosen for a full tag when last placed. */
bool wasFull(const char* hex);

/**
 * Finds the best free slot. On success the tag's box becomes an obstacle for later tags,
 * the slot is remembered for the next frame, and *out / *slot are set.
 */
bool place(const TagRequest& req, ScreenRect* out, Slot* slot);

}  // namespace ui::tags
