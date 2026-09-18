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

RGB padLift(RGB c, uint8_t amount) {
	return c.transform(
	    [amount](uint8_t ch) -> uint8_t { return (uint8_t)(ch + (((255 - ch) * (uint32_t)amount) / 255)); });
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
