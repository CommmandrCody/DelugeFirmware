/*
 * Copyright © 2026 Commander Cody
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

#include "hid/sample_frame.h"
#include <cstring>

namespace deluge::hid::sample_frame {

namespace {
constexpr uint32_t kFnvStart = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;
constexpr uint64_t kMaxPosition = (1ull << 35) - 1; // five 7-bit bytes

uint32_t fnv(uint32_t hash, uint8_t byte) {
	return (hash ^ byte) * kFnvPrime;
}

uint32_t fnvText(uint32_t hash, const char* text) {
	if (text != nullptr) {
		for (const char* c = text; *c != 0; c++) {
			hash = fnv(hash, (uint8_t)*c);
		}
	}
	return fnv(hash, 0); // so "ab","c" and "a","bc" are different
}

int32_t clamp(int32_t value, int32_t low, int32_t high) {
	return (value < low) ? low : ((value > high) ? high : value);
}
} // namespace

uint16_t fold14(uint32_t hash) {
	return (uint16_t)((hash ^ (hash >> 14) ^ (hash >> 28)) & 0x3fff);
}

uint16_t fileKey(const char* path) {
	return fold14(fnvText(kFnvStart, path));
}

int32_t pack(const Focus& focus, uint8_t* out) {
	int32_t i = 0;
	auto put = [&](uint32_t byte) { out[i++] = (uint8_t)(byte & 0x7f); };
	auto put14 = [&](uint32_t value) {
		put(value >> 7);
		put(value);
	};
	auto putPosition = [&](uint64_t value) {
		if (value > kMaxPosition) {
			value = kMaxPosition;
		}
		for (int32_t shift = 28; shift >= 0; shift -= 7) {
			put((uint32_t)(value >> shift));
		}
	};

	out[i++] = 0xf0;
	out[i++] = 0x00;
	out[i++] = 0x21;
	out[i++] = 0x7b;
	out[i++] = 0x01;
	out[i++] = 0x02;
	out[i++] = 0x55;
	put(kVersion);
	put(focus.can);

	const uint8_t mask = focus.oscMask & 3;
	if (!focus.present || mask == 0) {
		put(0);
		out[i++] = 0xf7;
		return i;
	}
	put(1);

	// WHICH sound this is, for a host that later asks for a change to "the one I was shown": where it
	// was found, which row, which oscillators are samples and what files they play. Not the mode or
	// the marks - those are what a change changes, and the name of a thing must outlive a change to it.
	uint32_t ident = kFnvStart;
	ident = fnv(ident, focus.source);
	ident = fnv(ident, (uint8_t)(focus.row & 0xff));
	ident = fnv(ident, (uint8_t)((focus.row >> 8) & 0xff));
	ident = fnv(ident, mask);
	for (int32_t s = 0; s < 2; s++) {
		if (mask & (1 << s)) {
			ident = fnvText(ident, focus.osc[s].path);
		}
	}

	put(focus.source);
	put14(fold14(ident));
	put(mask);
	put(focus.state);
	put((focus.ampRelease < 0) ? 0x7f : (uint32_t)clamp(focus.ampRelease, 0, 0x7e));
	if (focus.row < 0 || focus.row >= 0x3fff) {
		put(0x7f);
		put(0x7f);
	}
	else {
		put14((uint32_t)focus.row);
	}

	for (int32_t s = 0; s < 2; s++) {
		if (!(mask & (1 << s))) {
			continue;
		}
		const Osc& osc = focus.osc[s];
		const char* path = (osc.path != nullptr) ? osc.path : "";
		const int32_t fullLength = (int32_t)strlen(path);
		const int32_t length = (fullLength > kMaxPath) ? kMaxPath : fullLength;
		const char* shown = path + (fullLength - length); // the END: the file's own name is what a player reads
		bool exact = (fullLength == length);
		for (int32_t k = 0; k < length && exact; k++) {
			const uint8_t c = (uint8_t)shown[k];
			exact = (c >= 32 && c < 127);
		}

		uint8_t flags = 0;
		flags |= osc.reversed ? kReversed : 0;
		flags |= osc.loopLocked ? kLoopLocked : 0;
		flags |= (osc.loopStart != 0) ? kLoopStartSet : 0;
		flags |= (osc.loopEnd != 0) ? kLoopEndSet : 0;
		flags |= osc.loaded ? kFileLoaded : 0;
		flags |= osc.pitchSpeedIndependent ? kPitchSpeedIndependent : 0;
		flags |= exact ? 0 : kPathNotExact;
		put(flags);
		put((osc.mode > 3) ? 0x7f : osc.mode);
		put14((uint32_t)(clamp(osc.transpose, -8192, 8191) + 8192));
		put((uint32_t)(clamp(osc.cents, -64, 63) + 64));
		put((uint32_t)(clamp(osc.speed, -64, 63) + 64));
		const uint32_t rate = (osc.rate > 0x1fffff) ? 0x1fffff : osc.rate;
		put(rate >> 14);
		put(rate >> 7);
		put(rate);
		put((osc.channels > 0x7f) ? 0x7f : osc.channels);
		putPosition(osc.length);
		putPosition(osc.start);
		putPosition(osc.end);
		putPosition(osc.loopStart);
		putPosition(osc.loopEnd);
		put((osc.zone > 0x7f) ? 0x7f : osc.zone);
		put((osc.zones > 0x7f) ? 0x7f : osc.zones);
		put14(fileKey(path));
		put((uint32_t)length);
		for (int32_t k = 0; k < length; k++) {
			const uint8_t c = (uint8_t)shown[k];
			put((c >= 32 && c < 127) ? c : '?'); // 7-bit and printable: a stray high byte would end the frame early
		}
	}

	const char* name = (focus.name != nullptr) ? focus.name : "";
	int32_t nameLength = (int32_t)strlen(name);
	if (nameLength > kMaxName) {
		nameLength = kMaxName;
	}
	put((uint32_t)nameLength);
	for (int32_t k = 0; k < nameLength; k++) {
		const uint8_t c = (uint8_t)name[k];
		put((c >= 32 && c < 127) ? c : '?');
	}

	out[i++] = 0xf7;
	return i;
}

} // namespace deluge::hid::sample_frame
