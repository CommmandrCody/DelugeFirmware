/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
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
#include "definitions_cxx.hpp"
#include "gui/ui/keyboard/chords.h"

class Output; // bass-follow target; only ever held as a re-validated pointer, never dereferenced here

#include "gui/ui/keyboard/layout/column_control_state.h"
#include "storage/flash_storage.h"

namespace deluge::gui::ui::keyboard {

constexpr int32_t kDefaultIsometricRowInterval = 5;
struct KeyboardStateIsomorphic {
	int32_t scrollOffset = (60 - (kDisplayHeight >> 2) * kDefaultIsometricRowInterval);
	int32_t rowInterval = kDefaultIsometricRowInterval;
};

struct KeyboardStateDrums {
	int32_t scroll_offset = 0;
	int32_t zoom_level = 8;
};

constexpr int32_t kDefaultInKeyRowInterval = 3;
struct KeyboardStateInKey {
	// Init scales have 7 elements, multipled by three octaves gives us C1 as first pad
	int32_t scrollOffset = (7 * 3);
	int32_t rowInterval = kDefaultInKeyRowInterval;
};

struct KeyboardStatePiano {
	// default octave = 1 (0 = -2oct), use a vertical scroll to change it
	int32_t scrollOffset = 3;
	// default note=0 (C)
	int32_t noteOffset = 0;
};

struct KeyboardStateChordLibrary {
	int32_t rowInterval = kOctaveSize;
	int32_t scrollOffset = 0;
	int32_t noteOffset = (rowInterval * 4);
	int32_t rowColorMultiplier = 5;
	ChordList chordList{};
};

struct KeyboardStateChord {
	int32_t noteOffset = (kOctaveSize * 4);
	int32_t modOffset = 0;
	int32_t scaleOffset = 0;
	bool autoVoiceLeading = false;
};

// Harmonic layout: degree columns × richness rows. Horizontal scroll moves through scale degrees;
// octaveBase sets the register of the tonic (MIDI tonic = octaveBase*12 + key root pitch class).
struct KeyboardStateHarmonic {
	int32_t scrollSteps = 0;   // horizontal scroll, in scale-degree steps (0 = tonic at column 0)
	int32_t octaveBase = 3;    // PALETTE octave: register the chords are built in (pal-ctrl OCT+/- buttons)
	int32_t isoOctave = 3;     // ISO octave SHOWN: the iso scrolls independently (vertical encoder)
	bool isoChromatic = false; // right panel: false = in-key (matches In-Key kbd), true = standard chromatic iso
	// (stickyChord removed 2026-09-09.) A picked chord ALWAYS stays loaded - that is the loop
	// (pick -> hear -> twist -> hear -> bank) and it is what gives spread, inversion and the voicing
	// dial something to act on. It was never a real choice: nothing wrote the field, its toggle pad
	// was deleted with the iso-control column, and no menu item replaced it. A setting that cannot
	// be set is not a setting.
	//
	// Loaded does NOT mean audible. You hear the pick and you hear each change; between them it is
	// silent. That distinction is what made this feel like the instrument was fighting you.
	// Next-chord suggestions, OFF by default (2026-09-10). Cody: "the flashing suggested chords are
	// annoying me... half the time I ignore them and try for myself." Suggestion is an invitation,
	// and an invitation that arrives unasked on every pick is nagging - especially as flashing pads
	// in peripheral vision are hard to ignore even when you have decided to.
	//
	// It is a PAD, not a menu setting (kBtnCalc, top of the palette-control column), so turning it
	// on is one press when you actually want a second opinion. That asymmetry is the point: ask for
	// help, rather than being helped continuously.
	bool calculatorOn = false;
	bool showChord = true;  // true = light the selected chord's shape on the iso; false = clean grid
	bool latticeOn = false; // true = light the FULL chord lattice (every repeat) with an upward fade
	bool diffOn = false; // DIFF/voice-leading view: prev->current motion (common hold, arriving breathe, leaving ghost)
	uint16_t prevChordPcMask = 0; // pitch-class set of the PREVIOUS chord — drives the DIFF / voice-leading view
	int8_t prevSelDeg = -1;       // degree of the PREVIOUS chord — so DIFF ghosts wear that chord's palette colour
	bool spreadRows = false;      // true = bottom 2 iso rows become the spread/voicing strip; false = full iso
	bool editVoicing = false;     // true = iso taps TOGGLE notes in/out of the selected chord's voicing
	uint8_t voiceOctaves = 0x08;  // octave STACK bitmask: bits 0..6 = octave offsets -3..+3; bit3 (base) always on
	bool stackPick = false;       // true = iso bottom row is the octave-PICKER strip (toggle which octaves)
	int8_t voiceSpread = 0;       // SPREAD: 0..3 lowest notes dropped an octave to open the voicing (drop-root)
	int8_t voiceInversion = 0;    // INVERSION: rotate the chord — move the lowest N notes up an octave
	int8_t voicingWalk = 0; // VOICING DIAL: walk the voicing ±N one-note-at-a-time; springs to voicingHome on release
	// SPLIT MODE (Orchid parity). The voicing dial has two modes, toggled by clicking it:
	//
	//   OCTAVE (voicingSplitMode = false) - the walk above: lift the lowest note an octave, re-sort,
	//           cascade through inversions. Moves the chord ALONG the keyboard.
	//   SPLIT  (voicingSplitMode = true)  - a pitch PIVOT. Notes above the split play an octave
	//           HIGHER, notes below play an octave LOWER. Opens the chord AROUND a point.
	//
	// Split is the spread we never had: the old SPREAD only pushed notes DOWN, which piles energy in
	// the bass and muddies, and its top step was a no-op on a triad. A pivot puts air BETWEEN the
	// bottom and top voices instead - which is what actually changes a chord's tone.
	//
	// The split counts VOICES, not semitones: the lowest `voicingSplit` notes drop an octave and the
	// rest rise one. A semitone pivot spends most of its travel in the gaps between chord tones, so
	// the dial would sit dead for several clicks - the same deadness that made the old SPREAD feel
	// broken. By voice, every click moves something, and it keeps its meaning across chords.
	bool voicingSplitMode = false;
	int8_t voicingSplit = 0;
	int8_t voicingHome = 0;      // the voicing the dial springs back to (hold the encoder in and turn to dial it)
	bool isoFollowsChord = true; // SNAP: on pick, jump iso to the chord's octave; off = leave iso parked (riff high)
	// PROGRESSION WALKER (MVP): HOLD purple row 2, then dial — vertical wheel picks which progression, horizontal
	// wheel walks the chords. Holding sustains the current step; release leaves the chord loaded. No grid strip.
	int8_t progPreset = -1; // loaded preset index (-1 = none dialled yet)
	// BASS FOLLOW. The chord's root, sounded an octave below the voicing, on a SYNTH track of the
	// user's choosing so it gets its own patch instead of thickening the chord.
	//
	// Deliberately inert until bound: an unbound BASS pad does nothing but say so. Nothing is
	// auto-created and nothing is borrowed, because a feature that silently starts playing notes
	// through someone else's track is worse than a feature that waits to be told where to go.
	//
	// Held as a raw pointer, so it MUST be re-validated against the song's output list before every
	// use - the track can be deleted, or the whole song swapped, while this still points at it.
	// Which octave the bass voice sounds in, as a MIDI octave number. Five positions, matching the
	// Orchid's bass dial (measured: D#0/D#1/D#2/D#3/D#4 - always the ROOT, only the octave moving).
	// It spans sub-bass to inside the chord, which is what makes it a voicing control and not just
	// a bass: at the top it sits ABOVE the chord's own lowest note.
	int8_t bassOctave = 1;
	Output* bassOutput = nullptr;
	bool bassOn = false;       // does the bass sound? only meaningful once bound
	int32_t bassLastNote = -1; // note currently sounding, so it can be turned off before the next
	int8_t progStep = 0;       // current step in the loaded progression
	bool progVoiced = true;    // VOICED = load each step with its baked-in voicing; BARE = plain chord (CLEAR toggles)
};
/// Please note that saving and restoring currently needs to be added manually in instrument_clip.cpp and all layouts
/// share one struct for storage
struct KeyboardState {
	KeyboardLayoutType currentLayout = FlashStorage::defaultKeyboardLayout;

	KeyboardStateIsomorphic isomorphic;
	KeyboardStateDrums drums;
	KeyboardStateInKey inKey;
	KeyboardStatePiano piano;
	KeyboardStateChord chord;
	KeyboardStateChordLibrary chordLibrary;
	KeyboardStateHarmonic harmonic;

	layout::ColumnControlState columnControl;
};

}; // namespace deluge::gui::ui::keyboard
