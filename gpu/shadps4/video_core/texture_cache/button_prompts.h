// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: keyboard and mouse pictures on the games' button icon sheets and prompts. The
// PlayStation buttons are redrawn as the keys bound in input.ini (BB_INPUT_CONFIG) when the game
// uploads them, while the keyboard or mouse was used last; after controller input the game's own
// pictures come back. BB_BUTTON_PROMPTS=0 keeps the PlayStation buttons.

#pragma once

#include <vector>

#include "common/types.h"

namespace VideoCore::ButtonPrompts {

enum class Format { Rgba8, Bc1 };

/// New contents for a rectangle of one mip level: RGBA8 pixels or BC1 blocks.
struct Patch {
    u32 mip, x, y, width, height;
    std::vector<u8> data;
};

/// Whether an image of this size and format may be a known picture (checked before hashing).
bool Candidate(u32 width, u32 height, Format format);

/// Input thread: the player used a controller (true) or the keyboard and mouse (false) last.
void SetController(bool controller);

/// Whether the keyboard pictures are shown now, and a count of the switches between the two.
bool KeyboardShown();
u32 Switches();

/// The patches for the picture whose mip 0 guest data hashes to `hash`, null for other images.
const std::vector<Patch>* Find(u64 hash, u32 width, u32 height, u32 levels, Format format);

} // namespace VideoCore::ButtonPrompts
