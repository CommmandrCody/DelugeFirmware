/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
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

class MIDICable;
#include "definitions_cxx.hpp"

namespace Debug {

void sysexReceived(MIDICable& cable, uint8_t* data, int32_t len);
void sysexDebugPrint(MIDICable& cable, const char* msg, bool nl);
#ifdef ENABLE_SYSEX_LOAD
void loadPacketReceived(uint8_t* data, int32_t len);
void loadCheckAndRun(uint8_t* data, int32_t len);
#endif

} // namespace Debug

namespace SysEx {

const uint8_t SYSEX_START = 0xF0;
const uint8_t DELUGE_SYSEX_ID_BYTE0 = 0x00;
const uint8_t DELUGE_SYSEX_ID_BYTE1 = 0x21;
const uint8_t DELUGE_SYSEX_ID_BYTE2 = 0x7B;
const uint8_t DELUGE_SYSEX_ID_BYTE3 = 0x01;

const uint8_t SYSEX_UNIVERSAL_NONRT = 0x7E;
const uint8_t SYSEX_UNIVERSAL_RT = 0x7F;
const uint8_t SYSEX_UNIVERSAL_IDENTITY = 0x06;

const uint8_t SYSEX_END = 0xF7;

enum SysexCommands : uint8_t {
	Ping,      // reply with pong
	Popup,     // display info in popup
	HID,       // HID access
	Debug,     // Debugging
	Json,      // Json Request
	JsonReply, // Json Response
	LearnContext =
	    0x4C,          // Chroma: outbound LEARN context event (host renders the docs) — see chroma-schema SCHEMA.md 2.1
	ChordState = 0x43, // Chroma: outbound chord-state push on every NORMAL palette pick (key + voiced notes + ctx)
	ChordApply = 0x44, // Chroma: INBOUND voicing-mod apply (host -> Deluge). Write direction; see hid_sysex.cpp.
	ActiveView = 0x45, // Chroma: outbound active-view push on UI change (host auto-follows: Learn jumps to the screen)
	Transport = 0x47,  // Chroma: outbound transport push - playing, tempo, and where the playhead is.
	                   // "Show me where we are" is the one question a 4-character display cannot
	                   // answer and a second screen exists to.
	KeyboardState = 0x48, // Chroma: outbound keyboard-state push - which layout, what key and scale, and
	                      // the span of notes the grid is currently showing. All eight keyboard
	                      // layouts report as the single view slug "keyboard" over 0x45, so a host had
	                      // no way to tell isomorphic from in-key from piano; and root/scale only
	                      // escaped via 0x43, which fires on harmonic palette picks alone. Cody: "I
	                      // want the mode of the keyboard displayed clearly and the current root
	                      // note/scale selected ... also note the range of notes C1-N".
	GoldKnob = 0x46,      // Chroma: outbound gold-knob push — which knob, the level its LED column shows, and the
	                      // parameter's name. The host's panel header draws the real knobs; without this it had
	                      // nothing to draw them FROM and sat dark on every page but Design.
	Pong = 0x7F           // Pong reply
};

} // namespace SysEx
