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

#pragma once

#include <cstdint>

/// Chroma (0x55): THE SAMPLE IN FOCUS, packed for a host.
///
/// The packing lives here, away from the instrument's objects, so it can be checked on a computer: it
/// takes plain numbers and writes bytes. hid_sysex.cpp does the looking; this does the writing.
///
///   F0 00 21 7B 01 02 55 <ver=1> <can> <present>
///   if present:
///     <source> <identHi> <identLo> <oscMask> <state> <ampRelease|7F> <rowHi> <rowLo>   (7F 7F = no row)
///     one record per oscillator named in oscMask:
///       <flags> <mode> <transpose x2, +8192> <cents+64> <speed+64> <rate x3> <channels>
///       <length x5> <start x5> <end x5> <loopStart x5> <loopEnd x5>
///       <zone> <zones> <fileKeyHi> <fileKeyLo> <pathLen> <path>
///     <nameLen> <name>
///   F7
///
/// Every byte is 7-bit. A position is 35 bits in 5 bytes, most significant first, in sample frames at
/// the file's own rate.
namespace deluge::hid::sample_frame {

constexpr uint8_t kVersion = 1;
constexpr int32_t kMaxPath = 96; ///< the END of a longer path is kept, and the record says it was cut
constexpr int32_t kMaxName = 24;
constexpr int32_t kOscRecord = 40;                                                        ///< without the path
constexpr int32_t kMaxFrame = 7 + 3 + 8 + 2 * (kOscRecord + kMaxPath) + 1 + kMaxName + 1; ///< 316

/// Where the sound in focus was found.
enum Source : uint8_t {
	kSynthTrack = 0,  ///< the synth on the track the player is on
	kSoundEditor = 1, ///< the sound open in the sound editor
	kKitRow = 2,      ///< the kit's selected row, with no editor open
};

/// <state>
constexpr uint8_t kStateEdited = 1;            ///< changed since it was loaded from or saved to the card
constexpr uint8_t kStateTailsKnown = 2;        ///< the next bit means something
constexpr uint8_t kStateIgnoresNoteLength = 4; ///< a note plays the sample out whatever its length

/// <flags> of one oscillator
constexpr uint8_t kReversed = 1;
constexpr uint8_t kLoopLocked = 2;
constexpr uint8_t kLoopStartSet = 4;
constexpr uint8_t kLoopEndSet = 8;
constexpr uint8_t kFileLoaded = 16;
constexpr uint8_t kPitchSpeedIndependent = 32;
constexpr uint8_t kPathNotExact = 64; ///< cut short, or a character was replaced: not a path to open

struct Osc {
	bool reversed = false;
	bool loopLocked = false;
	bool loaded = false; ///< the file is in memory, so length, rate and channels are real
	bool pitchSpeedIndependent = false;
	uint8_t mode = 0; ///< SampleRepeatMode: 0 CUT, 1 ONCE, 2 LOOP, 3 STRETCH
	int32_t transpose = 0;
	int32_t cents = 0;
	int32_t speed = 0;
	uint32_t rate = 0;
	uint8_t channels = 0;
	uint64_t length = 0;
	uint64_t start = 0;
	uint64_t end = 0;
	uint64_t loopStart = 0; ///< 0 = not set, as the instrument holds it
	uint64_t loopEnd = 0;   ///< 0 = not set
	uint8_t zone = 0;
	uint8_t zones = 0;
	const char* path = nullptr; ///< null is the same as empty
};

struct Focus {
	uint8_t can = 0; ///< what this firmware can do for a host beyond reporting; nothing yet
	bool present = false;
	uint8_t source = kSynthTrack;
	uint8_t oscMask = 0; ///< bit 0 oscillator 1, bit 1 oscillator 2
	uint8_t state = 0;
	int32_t ampRelease = -1; ///< 0-50 as the menu shows it, -1 when it could not be read
	int32_t row = -1;        ///< the kit row's position in the kit, -1 when there is none
	Osc osc[2];
	const char* name = nullptr;
};

/// FNV-1a folded to 14 bits: the fold the Features names use, so a host needs one.
[[nodiscard]] uint16_t fold14(uint32_t hash);

/// The 14-bit key of a file path. Over the WHOLE path, however much of it the frame carries.
[[nodiscard]] uint16_t fileKey(const char* path);

/// Writes the whole frame, F0 to F7, into `out` (at least kMaxFrame bytes). Returns its length.
[[nodiscard]] int32_t pack(const Focus& focus, uint8_t* out);

} // namespace deluge::hid::sample_frame
