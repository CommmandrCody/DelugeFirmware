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

// THE HOST'S SYNC WORD (Chroma 1.3.144): how the Deluge Commander plugin tells the Deluge where the host's timeline is,
// carried INSIDE the USB audio the plugin sends it, so it arrives on a known sample. Cody, 2026-11-27: "we're not
// treating the Deluge as an external synth, it's a plugin" - Ableton's transport drives it the way it drives any
// plugin, with no MIDI clock and no sync settings.
//
// ONE FILE, BOTH ENDS: the firmware decodes with this header and the plugin (commander-plugin, beside this tree)
// encodes with it, so the format cannot be written twice and disagree. Header only, no firmware dependencies.
//
// THE WORD: twelve 16-bit samples on the return's fourth channel, silence between words.
//   0  0x5A3C            magic
//   1  0xC3A5            magic
//   2  sequence          counts up by one per word, wrapping
//   3  flags             bit 0 the host is playing; bit 1 echo request (step 1: measure the round trip)
//   4  tempo, high       milli-BPM, 32 bits
//   5  tempo, low
//   6  position          the host's position in beats at THIS WORD'S FIRST SAMPLE, signed 64-bit, 1/2^24 beat
//   7  ..
//   8  ..
//   9  .. (low)
//   10 reserved          0
//   11 CRC-16/CCITT      over samples 0-10, as 16-bit words, high byte first
// A word is accepted only whole, with both magics and its CRC; anything else is counted and dropped.
#pragma once

#include <cstdint>

namespace deluge::hostsync {

constexpr uint16_t kMagicA = 0x5A3Cu;
constexpr uint16_t kMagicB = 0xC3A5u;
constexpr uint32_t kWordSamples = 12u;
constexpr uint32_t kChannel = 3u; ///< the return's fourth channel (0-based)
constexpr uint16_t kFlagPlaying = 1u;
constexpr uint16_t kFlagEcho = 2u;

struct Word {
	uint16_t seq = 0;
	uint16_t flags = 0;
	uint32_t tempoMilliBpm = 0;
	int64_t position = 0; ///< beats * 2^24 at the word's first sample
};

/// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over 16-bit words, high byte first.
inline uint16_t crc16(const uint16_t* words, uint32_t n) {
	uint16_t crc = 0xFFFFu;
	for (uint32_t i = 0; i < n; i++) {
		for (int32_t half = 1; half >= 0; half--) {
			crc ^= (uint16_t)(((words[i] >> (8 * half)) & 0xFFu) << 8);
			for (int32_t b = 0; b < 8; b++) {
				crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
			}
		}
	}
	return crc;
}

/// The twelve samples of a word, as 16-bit patterns (write them as int16 audio samples, bit for bit).
inline void encode(const Word& w, uint16_t out[kWordSamples]) {
	const uint64_t p = (uint64_t)w.position;
	out[0] = kMagicA;
	out[1] = kMagicB;
	out[2] = w.seq;
	out[3] = w.flags;
	out[4] = (uint16_t)(w.tempoMilliBpm >> 16);
	out[5] = (uint16_t)(w.tempoMilliBpm & 0xFFFFu);
	out[6] = (uint16_t)(p >> 48);
	out[7] = (uint16_t)(p >> 32);
	out[8] = (uint16_t)(p >> 16);
	out[9] = (uint16_t)(p & 0xFFFFu);
	out[10] = 0;
	out[11] = crc16(out, kWordSamples - 1u);
}

/// Reads words out of a stream of samples, one sample at a time, across any number of calls. push() returns true on
/// the sample that completes a valid word; word() is then that word, and `good`/`bad` count what was seen.
class Decoder {
public:
	bool push(uint16_t sample) {
		if (n_ == 0) {
			if (sample == kMagicA) {
				buf_[n_++] = sample;
			}
			return false;
		}
		if (n_ == 1 && sample != kMagicB) {
			n_ = (sample == kMagicA) ? 1u : 0u; // a repeated first magic restarts the word on itself
			return false;
		}
		buf_[n_++] = sample;
		if (n_ < kWordSamples) {
			return false;
		}
		n_ = 0;
		if (crc16(buf_, kWordSamples - 1u) != buf_[kWordSamples - 1u]) {
			bad++;
			return false;
		}
		word_.seq = buf_[2];
		word_.flags = buf_[3];
		word_.tempoMilliBpm = ((uint32_t)buf_[4] << 16) | buf_[5];
		word_.position = (int64_t)(((uint64_t)buf_[6] << 48) | ((uint64_t)buf_[7] << 32) | ((uint64_t)buf_[8] << 16)
		                           | (uint64_t)buf_[9]);
		good++;
		return true;
	}
	const Word& word() const { return word_; }
	void reset() { n_ = 0; }
	uint32_t good = 0;
	uint32_t bad = 0;

private:
	uint16_t buf_[kWordSamples] = {};
	uint32_t n_ = 0;
	Word word_;
};

} // namespace deluge::hostsync
