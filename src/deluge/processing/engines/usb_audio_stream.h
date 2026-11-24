/*
 * Copyright © 2026 Synthstrom Audible Limited
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

struct StereoSample;

namespace deluge::processing::engines {

/// USB Audio Class: eight channels of individual tracks out to a host, and four channels back in.
///
/// Out, each track's audio is taken as the difference it makes to the mix while it renders, so nothing renders
/// twice. In, the returning audio is summed into the song's mix or taken as a track's input. Both directions cost
/// nothing while no host has the interface open.
class USBAudioStream {
public:
	/// Keeps both isochronous pipes serviced. From the scheduler, and from the audio engine so a card load, which
	/// starves every other task, does not starve the stream.
	static void service();

	/* ---- Out ---- */

	static constexpr uint32_t kStemChannels = 8;

	/// Whether a host is streaming, and so whether stems are worth capturing.
	static bool stemsWanted();

	/// Chroma (0x5B): the stream's state and running totals, for a host's health report. Reads only.
	struct Health {
		bool streaming;        ///< a host has the streaming interface open
		bool timerRunning;     ///< the write timer is running
		bool transferInFlight; ///< the driver holds a transfer
		bool dmaBusy;          ///< a packet is with the DMA controller
		bool dmaConfigured;
		bool primed;
		uint32_t started;       ///< packets of audio handed to the controller, ever
		uint32_t standby;       ///< packets of standby silence, ever
		uint32_t retired;       ///< packets committed to the pipe, ever
		uint32_t fifoNotReady;  ///< write attempts that found the pipe not ready, ever
		uint32_t dmaBusyPasses; ///< write attempts that found the last packet still moving, ever
		uint32_t recoveries;    ///< abandoned transfers ended by force, ever
		uint32_t resyncs;       ///< times the reader was thrown forward, ever
		uint32_t underruns;     ///< packets sent as silence for want of audio, ever
		int32_t lead;           ///< frames the engine is ahead of the packet builder
	};
	static void readHealth(Health& out);

	/// Clears the stem accumulators and settles the return's window. Once per render window, before any track.
	static void beginRender(uint32_t numSamples);

	/// The mix before one track renders into it.
	static void snapshotBeforeTrack(const int32_t* mixNow, uint32_t numSamples);

	/// Copies one track to its UsbRoute destinations: the mono sum to a channel, left and right to a pair.
	/// Several tracks on one channel add.
	static void captureStem(uint16_t route, const int32_t* mixNow, uint32_t numSamples);

	/// Restores the snapshot, taking the track that just rendered back out of the mix.
	static void removeTrackFromMix(int32_t* mixNow, uint32_t numSamples);

	/// Hands numSamples of the captured stems to the stream as they reach the codec. renderOffset is where they
	/// sit in the render window, which is output across several calls.
	static void feedMix(uint32_t numSamples, uint32_t renderOffset);

	/* ---- In ---- */

	/// Sums the master return pair into the song's mix, unless a track has taken it this window.
	static void mixReturn(StereoSample* buffer, uint32_t numSamples);

	/// Level of the master return, 0-50 in 1.2 dB steps. 0 is silence, 40 unity, 50 is +12 dB.
	static constexpr uint32_t kReturnLevelMax = 50;
	static constexpr uint32_t kReturnLevelUnity = 40;
	static constexpr uint32_t kReturnLevelDefault = kReturnLevelUnity;
	static void setReturnLevel(uint32_t level);
	static uint32_t getReturnLevel();

	static uint32_t numReturnPairs();

	/// Which pair is summed into the song's mix, 1-based, or zero for none.
	static void setMasterReturnPair(uint32_t pair);
	static uint32_t getMasterReturnPair();

	/// Adds one returning pair to a track's render at the track's amplitude, and takes that pair off the master for
	/// this window. Returns false when nothing is arriving; the pair is claimed either way.
	static bool readReturnPair(uint32_t pair, StereoSample* buffer, uint32_t numSamples, int32_t amplitudeStart,
	                           int32_t amplitudeEnd);

	/// The same for one channel, 1-based, arriving as mono on both sides. Claims the whole pair it belongs to.
	static bool readReturnChannel(uint32_t channel, StereoSample* buffer, uint32_t numSamples, int32_t amplitudeStart,
	                              int32_t amplitudeEnd);

	/// How many frames the last output pass took off the return. A recorder of a USB input owes this many, as
	/// silence where the return ran dry.
	static uint32_t consumedReturnFrames();

	/// Copies part of that batch out at track input scale. chL and chR are 0-based; the same one twice is mono.
	/// Returns how many were written.
	static uint32_t readConsumedReturn(uint32_t chL, uint32_t chR, StereoSample* dst, uint32_t offset,
	                                   uint32_t maxSamples);

	/* ---- Compensation ---- */

	/// Delays the Deluge's own summed tracks and their reverb send, before the return joins them, so audio sent
	/// through an external processor comes back in time with audio that never left. Everything played on the
	/// Deluge is heard that much later. Zero is off and costs nothing.
	static constexpr uint32_t kCompensationMaxMs = 100;
	static void setCompensationMs(uint32_t ms);
	static uint32_t getCompensationMs();
	static void compensate(StereoSample* buffer, int32_t* reverbSend, uint32_t numSamples);
};

} // namespace deluge::processing::engines

extern "C" {
/// Task-scheduler entry point.
void usbAudioStreamRoutine();
}
