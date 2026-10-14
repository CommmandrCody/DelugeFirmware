/*
 * Copyright © 2022-2023 Synthstrom Audible Limited
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

#include "util/container/array/resizeable_array.h"
#include <cstdint>

class CStringArray : public ResizeableArray {
public:
	CStringArray(int32_t newElementSize) : ResizeableArray(newElementSize) {}
	void sortForStrings();
	// Sort descending by a uint32 key stored at keyOffset bytes into each element (e.g. FileItem::dateModified).
	// Plain insertion sort — only used on the small, windowed load list, so it's never hot.
	void sortByU32KeyDescending(uint32_t keyOffset);
	int32_t search(char const* searchString, bool* foundExact = nullptr);

private:
	int32_t partitionForStrings(int32_t low, int32_t high);
	void quickSortForStrings(int32_t low, int32_t high);
};
