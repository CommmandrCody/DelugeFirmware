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

#include "gui/colour/press_feedback.h"

namespace deluge::gui::colour {

namespace {

// BRIGHTEN THE COLOUR THE PAD ALREADY HAS - do not add white to it.
//
// The first version added `amount` towards 255 on every channel independently, which brightens by
// destroying the hue: a mute pad at (40,0,0) pressed became (187,175,175), white with a faint red
// cast, and an unlit pad became flat grey. Cody, on the audition column: "I thought it muted, it
// turns white ... it really confuses the user." He was right - the pad stopped looking like itself.
//
// Scaling preserves the ratio between channels, so a red pad goes bright RED and a green one bright
// green. Pure black has no ratio to preserve and no hue to keep, so it alone gets a neutral lift -
// and that is the unlit pad, which is the one case that always read correctly anyway.
RGB padLift(RGB c, uint8_t amount) {
	const uint8_t peak = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
	if (peak == 0) {
		const uint8_t v = (uint8_t)((uint32_t)amount * 160 / 255); // an unlit pad, lifted to legible
		return RGB(v, v, v);
	}
	// How far the brightest channel can still travel, shared out in proportion.
	const uint32_t headroom = 255u - peak;
	const uint32_t add = (headroom * (uint32_t)amount) / 255u;
	const uint32_t target = peak + add;
	return c.transform([peak, target](uint8_t ch) -> uint8_t { return (uint8_t)((uint32_t)ch * target / peak); });
}

RGB padDim(RGB c, uint8_t amount) {
	return c.transform([amount](uint8_t ch) -> uint8_t { return (uint8_t)(ch - ((ch * (uint32_t)amount) / 255)); });
}

// How far apart two colours are, summed across the channels. Crude on purpose: the question is only
// "would anyone notice the difference", and a sum of the three distances answers exactly that.
uint32_t padGap(RGB a, RGB b) {
	const int32_t dr = (int32_t)a.r - b.r, dg = (int32_t)a.g - b.g, db = (int32_t)a.b - b.b;
	return (uint32_t)((dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db));
}

} // namespace

RGB pressedColour(RGB c, uint8_t strength) {
	const RGB up = padLift(c, (uint8_t)(175 * strength / 255));
	const RGB down = padDim(c, (uint8_t)(195 * strength / 255));
	return (padGap(down, c) > padGap(up, c)) ? down : up;
}

} // namespace deluge::gui::colour
