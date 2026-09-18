/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "gui/colour/rgb.h"
#include <cstdint>

namespace deluge::gui::colour {

/**
 * SHOW THAT A PAD IS BEING HELD, whatever colour the view happens to have painted it.
 *
 * Returns `c` shifted in whichever direction is MORE VISIBLE, by `strength` (0-255).
 *
 * There is no brightness threshold, and that is the whole point. Three different thresholds were
 * tried and each was wrong about some layout's palette - the last one because in-key draws its
 * INACTIVE root at half brightness, so "bright pads dim, dark pads lift" put the root in the lift
 * branch, and lifting a pad that is already half lit does nearly nothing. Cody: "column presses only
 * light up the unlit pads - the solid one at the root, no change is seen whatsoever."
 *
 * So it stops guessing. Compute both candidates, take whichever lands further from where the pad
 * already is. A near-black pad has nowhere to go but up, a bright one nowhere but down, and
 * everything between picks the side with more room - without anyone needing to know what any view's
 * palette looks like.
 *
 * STRENGTH separates the pad under your finger from its octaves. Cody: "the lit keys going up the
 * octave are way too bright - the ones we hit should be brightest, the others half as bright." If
 * every pad of a pitch class moved the same amount, the surface would tell you the pitch class and
 * lose the one thing you already knew: which note you actually played.
 *
 * It lives here, apart from the LED driver, because it is a colour operation and because keeping it
 * free of GUI dependencies is what lets it be tested at all. See tests/unit/press_feedback_tests.cpp.
 */
[[nodiscard]] RGB pressedColour(RGB c, uint8_t strength);

} // namespace deluge::gui::colour
