/*
 * Copyright © 2016-2024 Synthstrom Audible Limited
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

#include "gui/ui/keyboard/layout/harmonic.h"
#include "gui/colour/colour.h"
#include "gui/ui/keyboard/chords.h"
#include "hid/display/display.h"
#include "model/settings/runtime_feature_settings.h"
#include "processing/engines/audio_engine.h"
#include <stdio.h>

namespace deluge::gui::ui::keyboard::layout {

namespace {

inline int32_t floordiv(int32_t a, int32_t b) {
	int32_t q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0))) {
		q--;
	}
	return q;
}
inline int32_t floormod(int32_t a, int32_t b) {
	int32_t r = a % b;
	if (r != 0 && ((r < 0) != (b < 0))) {
		r += b;
	}
	return r;
}

// ── Column model (the SINGLE source of truth for where each block lives) ───────────────────────────────
// Layout is 16 wide: PALETTE(7) | palette-ctrl(1) | iso-ctrl(1) | ISO(7). The handedness SWAP mirrors the
// two blocks AND their bound control columns, so each control column always sits next to the grid it drives.
enum Region : uint8_t { REG_PAL, REG_PAL_CTRL, REG_ISO_CTRL, REG_ISO, REG_NONE };
struct ColInfo {
	Region region;
	int32_t local; // index within a 7-wide block (0..6); 0 for control columns
};
constexpr int32_t kBlockWidth = 7;

inline ColInfo colInfoFor(int32_t x, bool swapped) {
	int32_t palStart = swapped ? 9 : 0;
	int32_t isoStart = swapped ? 0 : 9;
	int32_t palCtrl = swapped ? 8 : 7;
	int32_t isoCtrl = swapped ? 7 : 8;
	if (x == palCtrl) {
		return {REG_PAL_CTRL, 0};
	}
	if (x == isoCtrl) {
		return {REG_ISO_CTRL, 0};
	}
	if (x >= palStart && x < palStart + kBlockWidth) {
		return {REG_PAL, x - palStart};
	}
	if (x >= isoStart && x < isoStart + kBlockWidth) {
		return {REG_ISO, x - isoStart};
	}
	return {REG_NONE, 0};
}
inline int32_t isoStartCol(bool swapped) {
	return swapped ? 0 : 9;
}

// Additive richness ladder: y=0 (bottom) = the ROOT alone (bass/anchor lane), building UP by stacking
// thirds → 13th at the top. `steps` are diatonic scale-degree offsets from the column's degree. sus2/sus4
// left the ladder — they're ALTERATIONS (voicing edits: toggle the 3rd off, the 2nd/4th on), not richness.
struct Richness {
	const char* suffix;
	int8_t steps[kMaxChordKeyboardSize];
	uint8_t count;
	int8_t add; // chromatic semitone to add above the root (-1 = none); used for the BRIGHT major-6th
};
const Richness kLadder[kDisplayHeight] = {
    {"", {0, 0, 0, 0, 0, 0, 0}, 1, -1},     // row0 ROOT  — single note; the bass / "where it starts" anchor
    {"5", {0, 4, 0, 0, 0, 0, 0}, 2, -1},    // row1 DYAD  — root + 5th (power)
    {"", {0, 2, 4, 0, 0, 0, 0}, 3, -1},     // row2 TRIAD
    {"6", {0, 2, 4, 0, 0, 0, 0}, 3, 9},     // row3 6th  — triad + a BRIGHT major 6th (root+9), borrowed if needed
    {"7", {0, 2, 4, 6, 0, 0, 0}, 4, -1},    // row4 7th
    {"9", {0, 2, 4, 6, 8, 0, 0}, 5, -1},    // row5 9th
    {"11", {0, 2, 4, 6, 8, 10, 0}, 6, -1},  // row6 11th
    {"13", {0, 2, 4, 6, 8, 10, 12}, 7, -1}, // row7 13th
};

// One colour per scale degree, ordered so ADJACENT columns jump across the colour wheel (warm/cool
// interleaved) — never grouped, so neighbours always contrast hard. renderPads shades each light->dark.
const RGB kDegreeHue[7] = {
    RGB{.r = 255, .g = 0, .b = 0},   // i   red
    RGB{.r = 0, .g = 220, .b = 220}, // ii  cyan     (opposite red)
    RGB{.r = 235, .g = 220, .b = 0}, // III yellow
    RGB{.r = 45, .g = 65, .b = 255}, // iv  blue
    RGB{.r = 0, .g = 220, .b = 0},   // v   green
    RGB{.r = 235, .g = 0, .b = 235}, // VI  magenta
    RGB{.r = 255, .g = 120, .b = 0}, // VII orange
};

// Brightness ramp for the richness rows (bottom=triad brightest -> top=complex dimmest). Brightened
// floor so the upper (lusher) rows still read clearly on the LEDs — "big and bright".
const uint8_t kRichBright[kDisplayHeight] = {255, 195, 150, 115, 88, 68, 54, 42};

// DEFAULT key-mood palette: the iso tints to the current key's colour so you can SEE the key (brightness
// still carries the chord relationships). Mood-based, not a flat wheel — bright/dark per key's feel. This
// is just the default; a custom + saveable per-user table is the natural next step. Indexed by root pc.
const RGB kKeyColour[12] = {
    RGB{.r = 255, .g = 150, .b = 110}, // C   open / warm       — warm coral (NOT white; white = the chord)
    RGB{.r = 130, .g = 55, .b = 180},  // C#  dark / mysterious — deep violet
    RGB{.r = 255, .g = 225, .b = 50},  // D   bright / joyful   — yellow
    RGB{.r = 190, .g = 90, .b = 50},   // D#  noble / warm-dark — deep red-gold
    RGB{.r = 25, .g = 190, .b = 70},   // E   radiant           — deeper green (less blue so it doesn't wash white)
    RGB{.r = 120, .g = 195, .b = 95},  // F   calm / pastoral   — soft green
    RGB{.r = 25, .g = 160, .b = 175},  // F#  deep / mysterious — teal
    RGB{.r = 255, .g = 140, .b = 30},  // G   warm / friendly   — orange
    RGB{.r = 75, .g = 70, .b = 170},   // G#  dark / deep       — indigo
    RGB{.r = 255, .g = 190, .b = 20},  // A   energy / bright   — gold
    RGB{.r = 220, .g = 110, .b = 90},  // A#  rich / warm       — rose-amber
    RGB{.r = 55, .g = 130, .b = 255},  // B   brilliant / intense — bright blue
};

const char* const kNumerals[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
const uint8_t kMajorIv[7] = {0, 2, 4, 5, 7, 9, 11};

// The Calculator marks each suggested degree's standard core: triad + 7th. The user chooses richer voicings
// themselves. (Additive ladder: kLadder[2] = triad, kLadder[4] = "7".)
constexpr int32_t kRowTriad = 2;
constexpr int32_t kRow7th = 4;

// ── Control columns ────────────────────────────────────────────────────────────────────────────────────
// Each control column hosts toggles at the TOP; dark pads below act as CLEAR pads. Achromatic (white = on,
// dim grey = off) so they never blend with the colourful palette. The modifiers adjust the VISUALS.
//   ISO control (bound to the iso): in-key/chromatic view, show-chord, sticky-voicing.
//   PALETTE control (bound to the explorer): Calculator on/off, handedness swap.
constexpr int32_t kBtnIsoView = kDisplayHeight - 1;   // iso-ctrl: in-key <-> chromatic
constexpr int32_t kBtnShowChord = kDisplayHeight - 2; // iso-ctrl: show / hide the chord shape
constexpr int32_t kBtnSticky = kDisplayHeight - 3;    // iso-ctrl: sticky chord voicing
constexpr int32_t kBtnLattice = kDisplayHeight - 4;   // iso-ctrl: full chord lattice on/off
constexpr int32_t kBtnStack = 3;                      // iso-ctrl: octave STACK level (tap to cycle 0..3)
constexpr int32_t kBtnSpread = 2;                     // iso-ctrl: SPREAD level (drop-root open, tap to cycle 0..3)
constexpr int32_t kBtnEdit = 1;          // iso-ctrl: voice-edit toggle (the bottom TWO pads = edit + audition)
constexpr int32_t kBtnAudition = 0;      // iso-ctrl: AUDITION the voicing — momentary, hold to hear the chord
constexpr uint8_t kIsoCtrlClearMask = 0; // iso-ctrl is full now (clear lives on the pal-ctrl column)

constexpr int32_t kBtnCalc = kDisplayHeight - 1;       // pal-ctrl: next-chord Calculator on/off
constexpr int32_t kBtnSwap = kDisplayHeight - 2;       // pal-ctrl: swap the two sides (handedness)
constexpr int32_t kBtnPalOctUp = kDisplayHeight - 3;   // pal-ctrl: PALETTE octave up
constexpr int32_t kBtnPalOctDown = kDisplayHeight - 4; // pal-ctrl: PALETTE octave down
constexpr uint8_t kPalCtrlClearMask = (uint8_t)((1u << (kDisplayHeight - 4)) - 1); // rows below = clear

// Reserved control-zone colours — a "control family" (PINK + PURPLE) used NOWHERE else in Chroma. The two
// centre columns get DIFFERENT hues so they're instantly distinguishable: palette-control = PINK,
// iso-control = PURPLE. On = bright, off = dim, whole column faintly tinted so each reads as its own zone.
const RGB kCtrlHue = RGB{.r = 255, .g = 30, .b = 145};    // palette-control = PINK
const RGB kCtrlHueIso = RGB{.r = 150, .g = 80, .b = 255}; // iso-control = PURPLE
// The iso-control column reads in three BANDS (SEE → SHAPE → HEAR), each a distinct shade of the purple
// family so the column groups at a glance instead of one undifferentiated list.
const RGB kHueSee = RGB{.r = 90, .g = 115, .b = 255};   // SEE   band (lens / overlays): bluer purple
const RGB kHueShape = RGB{.r = 165, .g = 70, .b = 255}; // SHAPE band (voicing engine): mid violet
const RGB kHueHear = RGB{.r = 215, .g = 65, .b = 235};  // HEAR  band (audition): warmer pink-purple
constexpr uint8_t kCtrlOn = 235;
constexpr uint8_t kCtrlOff = 90;
constexpr uint8_t kCtrlFaint = 45; // brighter zone tint so the control columns clearly read as zones

} // namespace

uint8_t KeyboardLayoutHarmonic::getScaleIntervals(uint8_t* ivOut) {
	NoteSet& scale = getScaleNotes();
	uint8_t count = 0;
	for (uint8_t i = 0; i < kOctaveSize && count < 12; i++) {
		if (scale.has(i)) {
			ivOut[count++] = i;
		}
	}
	return count;
}

int32_t KeyboardLayoutHarmonic::isoNoteAt(int32_t localX, int32_t y) {
	uint8_t sc = getScaleNoteCount();
	if (sc == 0) {
		return getRootNote();
	}
	// In-Key keyboard mapping (scale-step layout + colours). Anchor the panel ONE OCTAVE BELOW the chord
	// register (octaveBase) so the voicing — built at octaveBase — lands up in the middle of the panel.
	int32_t padIndex = (getState().harmonic.isoOctave - 1) * (int32_t)sc + localX + y * getState().inKey.rowInterval;
	if (padIndex < 0) {
		padIndex = 0;
	}
	int32_t octave = padIndex / sc;
	int32_t idx = padIndex % sc;
	return octave * 12 + getRootNote() + getScaleNotes()[idx];
}

int32_t KeyboardLayoutHarmonic::isoNoteChromatic(int32_t localX, int32_t y) {
	// Standard chromatic isomorphic mapping (semitone per column, rowInterval per row), anchored ONE
	// OCTAVE BELOW the chord register so the voicing lands in the middle of the panel, not at the bottom.
	return (getState().harmonic.isoOctave - 1) * 12 + getRootNote() + localX + y * getState().isomorphic.rowInterval;
}

uint8_t KeyboardLayoutHarmonic::buildVoicing(int16_t* out, uint8_t maxOut) {
	// Gather + sort the base chord notes.
	int16_t tmp[kMaxChordKeyboardSize];
	uint8_t n = 0;
	for (uint8_t i = 0; i < chordNoteCount && n < kMaxChordKeyboardSize; i++) {
		tmp[n++] = chordNotes[i];
	}
	for (uint8_t i = 1; i < n; i++) {
		int16_t v = tmp[i];
		int32_t j = (int32_t)i - 1;
		while (j >= 0 && tmp[j] > v) {
			tmp[j + 1] = tmp[j];
			j--;
		}
		tmp[j + 1] = v;
	}
	// SPREAD: drop the lowest notes down an octave to open the voicing (drop-root).
	int8_t spread = getState().harmonic.voiceSpread;
	for (int8_t k = 0; k < spread && k < (int8_t)n; k++) {
		if (tmp[k] - 12 >= 0) {
			tmp[k] = (int16_t)(tmp[k] - 12);
		}
	}
	// STACK: copy the chord into each SELECTED octave (bits 0..6 = offsets -3..+3; bit3 = base). Dedup + clamp.
	uint8_t octaves = getState().harmonic.voiceOctaves;
	uint8_t cnt = 0;
	for (int8_t b = 0; b < 7; b++) {
		if (!(octaves & (uint8_t)(1u << b))) {
			continue;
		}
		int32_t off = ((int32_t)b - 3) * 12;
		for (uint8_t i = 0; i < n; i++) {
			int32_t v = (int32_t)tmp[i] + off;
			if (v < 0 || v > 127 || cnt >= maxOut) {
				continue;
			}
			bool dup = false;
			for (uint8_t k = 0; k < cnt; k++) {
				if (out[k] == v) {
					dup = true;
					break;
				}
			}
			if (!dup) {
				out[cnt++] = (int16_t)v;
			}
		}
	}
	return cnt;
}

void KeyboardLayoutHarmonic::recomputeSuggestions(uint8_t keyRoot, const uint8_t* iv, uint8_t sc, uint8_t homeRootPc) {
	numSuggestions = 0;
	topDeg = -1;
	for (uint8_t d = 0; d < 7; d++) {
		degBright[d] = 0;
	}
	// Off when the Calculator is toggled off; otherwise diatonic-7-note only (like the Chord Library's Calculator).
	if (!getState().harmonic.calculatorOn || sc != 7 || !getScaleModeEnabled()) {
		return;
	}
	// Exactly like Chord Library: take the TOP 3 next chords only (not all degrees), so just those few
	// columns light. degBright shades them by rank (strongest brightest); the rest stay dark.
	numSuggestions = (uint8_t)suggestNextChords(keyRoot, getScaleNotes(), sc, homeRootPc, suggestions, 3);
	for (uint8_t s = 0; s < numSuggestions; s++) {
		for (uint8_t d = 0; d < sc; d++) {
			if ((uint8_t)((keyRoot + iv[d]) % 12) == suggestions[s].rootNote) {
				degBright[d] = (numSuggestions > 1) ? (uint8_t)(235 - (uint32_t)s * 165 / (numSuggestions - 1)) : 235;
				if (s == 0) {
					topDeg = (int8_t)d;
				}
				break;
			}
		}
	}
	// The chord you're on (home) stays full so you always see where you are.
	for (uint8_t d = 0; d < sc; d++) {
		if ((uint8_t)((keyRoot + iv[d]) % 12) == homeRootPc) {
			degBright[d] = 255;
			break;
		}
	}
}

uint8_t KeyboardLayoutHarmonic::buildChordAtDegree(uint8_t deg, int32_t y, const uint8_t* iv, uint8_t sc,
                                                   uint8_t keyRoot, int16_t* notesOut, uint8_t maxNotes,
                                                   uint8_t* rootPcOut, char* romanOut, char* absOut) {
	if (romanOut) {
		romanOut[0] = '\0';
	}
	if (absOut) {
		absOut[0] = '\0';
	}
	if (sc == 0) {
		return 0;
	}
	int32_t anchor = getState().harmonic.octaveBase * 12 + keyRoot;
	uint8_t rootPc = (uint8_t)((keyRoot + iv[deg]) % 12);
	if (rootPcOut) {
		*rootPcOut = rootPc;
	}

	int32_t yc = (y < 0) ? 0 : (y >= kDisplayHeight ? (kDisplayHeight - 1) : y);
	const Richness& rich = kLadder[yc];
	uint8_t count = 0;
	for (uint8_t k = 0; k < rich.count && count < maxNotes; k++) {
		int32_t st = (int32_t)deg + rich.steps[k];
		int32_t midi = anchor + floordiv(st, sc) * 12 + iv[floormod(st, sc)];
		if (midi < 0) {
			midi = 0;
		}
		if (midi > 127) {
			midi = 127;
		}
		notesOut[count++] = (int16_t)midi;
	}
	// Chromatic add (the BRIGHT major-6th): always a real major 6th above the root, borrowed if the scale
	// only has the flat 6 — so the "6" rung is a bright minor-6/major-6, not a dark b6.
	if (rich.add >= 0 && count > 0 && count < maxNotes) {
		int32_t midi = (int32_t)notesOut[0] + rich.add;
		if (midi > 127) {
			midi = 127;
		}
		notesOut[count++] = (int16_t)midi;
	}

	bool flats = keyPrefersFlats(keyRoot, getScaleNotes());
	if (absOut) {
		sprintf(absOut, "%s%s", noteNameInKey(rootPc, flats), rich.suffix);
	}
	if (romanOut && sc == 7) {
		int8_t third = (int8_t)(((int32_t)iv[(deg + 2) % 7] - iv[deg] + 12) % 12);
		int8_t fifth = (int8_t)(((int32_t)iv[(deg + 4) % 7] - iv[deg] + 12) % 12);
		bool minorish = (third == 3);
		bool dim = (fifth == 6);
		bool aug = (fifth == 8);
		int8_t accidental = (int8_t)iv[deg] - (int8_t)kMajorIv[deg];
		char buf[32];
		int p = 0;
		for (int8_t a = 0; a < -accidental; a++) {
			buf[p++] = 'b';
		}
		for (int8_t a = 0; a < accidental; a++) {
			buf[p++] = '#';
		}
		for (const char* c = kNumerals[deg]; *c; c++) {
			buf[p++] = (minorish || dim) ? (char)(*c - 'A' + 'a') : *c;
		}
		if (dim) {
			buf[p++] = 'o';
		}
		else if (aug) {
			buf[p++] = '+';
		}
		buf[p] = '\0';
		sprintf(romanOut, "%s%s", buf, rich.suffix);
	}
	return count;
}

void KeyboardLayoutHarmonic::drawName(const char* roman, const char* abs) {
	char full[80];
	if (roman && roman[0]) {
		sprintf(full, "%s  %s", abs, roman);
	}
	else {
		sprintf(full, "%s", abs);
	}
	if (display->haveOLED()) {
		display->popupTextTemporary(full);
	}
	else {
		display->setScrollingText(full, 0);
	}
}

void KeyboardLayoutHarmonic::evaluatePads(PressedPad presses[kMaxNumKeyboardPadPresses]) {
	currentNotesState = NotesState{}; // Erase active notes
	uint8_t iv[12];
	uint8_t sc = getScaleIntervals(iv);
	uint8_t keyRoot = (uint8_t)getRootNote();
	uint8_t numCols = (sc > 7) ? 7 : sc;
	bool swapped = getState().harmonic.swapped;
	heldCols = 0;

	uint8_t palCtrlNow = 0;
	uint8_t isoCtrlNow = 0;
	uint64_t isoNowMask = 0; // iso pads pressed this frame (bit = localX*8+y) — for voice-edit rising edge
	bool isoPlayed = false;
	bool leftPicked = false;
	for (int32_t idx = kMaxNumKeyboardPadPresses - 1; idx >= 0; --idx) {
		PressedPad pressed = presses[idx];
		if (!pressed.active || pressed.x >= kDisplayWidth) {
			continue;
		}
		ColInfo ci = colInfoFor(pressed.x, swapped);
		if (ci.region == REG_PAL_CTRL) {
			if (pressed.y >= 0 && pressed.y < kDisplayHeight) {
				palCtrlNow |= (uint8_t)(1u << pressed.y);
			}
			continue;
		}
		if (ci.region == REG_ISO_CTRL) {
			if (pressed.y >= 0 && pressed.y < kDisplayHeight) {
				isoCtrlNow |= (uint8_t)(1u << pressed.y);
			}
			continue;
		}
		if (ci.region == REG_ISO) {
			// The iso panel. Sound the held note (free-play, or feedback while voice-editing). In EDIT mode a
			// rising-edge press toggles this note in/out of the voicing (handled after the loop).
			isoPlayed = true;
			if (ci.local >= 0 && ci.local < kBlockWidth && pressed.y >= 0 && pressed.y < kDisplayHeight) {
				isoNowMask |= (uint64_t)1u << (ci.local * kDisplayHeight + pressed.y);
			}
			// Free play sounds the tapped note. In EDIT mode we DON'T sound single taps — the whole voicing
			// drones (after the loop) so you hear the CHORD you're sculpting, not isolated pings.
			bool stripPad = (getState().harmonic.stackPick && pressed.y == 0); // bottom row = octave picker
			if (!getState().harmonic.editVoicing && !stripPad) {
				int32_t note = getState().harmonic.isoChromatic ? isoNoteChromatic(ci.local, pressed.y)
				                                                : isoNoteAt(ci.local, pressed.y);
				if (note >= 0 && note <= 127) {
					enableNote((uint8_t)note, velocity);
				}
			}
			continue;
		}
		if (ci.region == REG_PAL && ci.local < numCols) {
			// The chord explorer — columns are the diatonic degrees, in order.
			uint8_t deg = (uint8_t)ci.local;
			int16_t notes[kMaxChordKeyboardSize];
			uint8_t rootPc = 0;
			char roman[32], abs[32];
			uint8_t n =
			    buildChordAtDegree(deg, pressed.y, iv, sc, keyRoot, notes, kMaxChordKeyboardSize, &rootPc, roman, abs);
			drawName(roman, abs);
			// Remember the EXACT voiced notes so the iso panel lights this one voicing.
			chordNoteCount = 0;
			for (uint8_t i = 0; i < n; i++) {
				enableNote((uint8_t)notes[i], velocity);
				chordNotes[chordNoteCount++] = notes[i];
			}
			heldCols |= (uint16_t)(1u << ci.local);
			leftPicked = true;
			getState().harmonic.showChord = true; // picking a chord always shows it (no silent hidden state)
			// Persist the selection so the highlight + iso shape stay after release, and ask the Calculator
			// where to go next (suggested degree columns flash in renderPads).
			selDeg = (int8_t)deg;
			selRichness =
			    (int8_t)((pressed.y < 0) ? 0 : (pressed.y >= kDisplayHeight ? kDisplayHeight - 1 : pressed.y));
			recomputeSuggestions(keyRoot, iv, sc, rootPc);
		}
	}

	KeyboardStateHarmonic& hs = getState().harmonic;

	auto clearSelection = [&]() {
		chordNoteCount = 0;
		selDeg = -1;
		selRichness = -1;
		numSuggestions = 0;
		topDeg = -1;
		for (uint8_t d = 0; d < 7; d++) {
			degBright[d] = 0;
		}
	};

	// OCTAVE PICKER: in stack-pick mode, rising bottom-row iso pads toggle which octaves are in the stack
	// (local 0..6 = offsets -3..+3; local 3 = base octave, always on).
	if (hs.stackPick) {
		uint64_t risingPads = isoNowMask & ~isoHeldMask;
		for (int32_t lx = 0; lx < kBlockWidth; lx++) {
			if ((risingPads & ((uint64_t)1u << (lx * kDisplayHeight))) && lx != 3) {
				hs.voiceOctaves ^= (uint8_t)(1u << lx);
			}
		}
		hs.voiceOctaves |= (uint8_t)(1u << 3);
	}

	// ISO voice-EDIT: a rising-edge iso press toggles that note IN/OUT of the selected chord's voicing.
	if (hs.editVoicing) {
		uint64_t risingIsoPads = isoNowMask & ~isoHeldMask;
		bool edited = false;
		for (int32_t b = 0; b < kBlockWidth * kDisplayHeight; b++) {
			if (!(risingIsoPads & ((uint64_t)1u << b))) {
				continue;
			}
			int32_t lx = b / kDisplayHeight, yy = b % kDisplayHeight;
			if (hs.stackPick && yy == 0) {
				continue; // bottom row belongs to the octave picker
			}
			int32_t note = hs.isoChromatic ? isoNoteChromatic(lx, yy) : isoNoteAt(lx, yy);
			if (note < 0 || note > 127) {
				continue;
			}
			int32_t found = -1;
			for (uint8_t i = 0; i < chordNoteCount; i++) {
				if (chordNotes[i] == note) {
					found = (int32_t)i;
					break;
				}
			}
			if (found >= 0) { // remove this note from the voicing
				for (uint8_t i = (uint8_t)found; i + 1 < chordNoteCount; i++) {
					chordNotes[i] = chordNotes[i + 1];
				}
				chordNoteCount--;
			}
			else if (chordNoteCount < kMaxChordKeyboardSize) { // add it
				chordNotes[chordNoteCount++] = (int16_t)note;
			}
			edited = true;
		}
		// Each edit, CALCULATE + name the chord you've built so you can SEE what you made (works even when
		// you started from nothing). No auto-hold — tap AUDITION to strike the voicing and hear it ring.
		if (edited && chordNoteCount > 0) {
			uint8_t nu[kMaxChordKeyboardSize];
			uint8_t nc = 0;
			for (uint8_t i = 0; i < chordNoteCount && nc < kMaxChordKeyboardSize; i++) {
				if (chordNotes[i] >= 0 && chordNotes[i] <= 127) {
					nu[nc++] = (uint8_t)chordNotes[i];
				}
			}
			char nm[48];
			if (nameChordFromNotes(nu, nc, nm, keyPrefersFlats(keyRoot, getScaleNotes()))) {
				drawName(nullptr, nm);
			}
		}
	}
	// Free play on the iso (without picking a chord) resets out of "chord mode" — unless sticky is on (or
	// voice-edit, handled above, which never clears).
	else if (isoPlayed && !leftPicked && !hs.stickyChord && !hs.stackPick) {
		clearSelection();
	}
	isoHeldMask = isoNowMask;

	// ── ISO control column (rising-edge so holding doesn't repeat) ──
	uint8_t risingIso = (uint8_t)(isoCtrlNow & ~isoCtrlHeldMask);
	if (risingIso & (uint8_t)(1u << kBtnIsoView)) {
		hs.isoChromatic = !hs.isoChromatic;
		display->displayPopup(hs.isoChromatic ? "CHRO" : "KEY");
	}
	if (risingIso & (uint8_t)(1u << kBtnShowChord)) {
		hs.showChord = !hs.showChord;
		display->displayPopup(hs.showChord ? "SHOW" : "HIDE");
	}
	if (risingIso & (uint8_t)(1u << kBtnSticky)) {
		hs.stickyChord = !hs.stickyChord;
		display->displayPopup(hs.stickyChord ? "HOLD" : "FREE");
	}
	if (risingIso & (uint8_t)(1u << kBtnLattice)) {
		hs.latticeOn = !hs.latticeOn;
		if (hs.latticeOn) {
			hs.showChord = true; // lattice needs the chord shown — turn it on so it can't silently do nothing
		}
		display->displayPopup(hs.latticeOn ? "LATT" : "ONE");
	}
	if (risingIso & (uint8_t)(1u << kBtnStack)) {
		hs.stackPick = !hs.stackPick;          // iso bottom row becomes the octave-picker strip
		hs.voiceOctaves |= (uint8_t)(1u << 3); // base octave always on
		display->displayPopup(hs.stackPick ? "OCT" : "STAK");
	}
	if (risingIso & (uint8_t)(1u << kBtnSpread)) {
		hs.voiceSpread = (int8_t)((hs.voiceSpread + 1) % 4); // open the voicing (drop the lowest notes)
		char buf[8];
		sprintf(buf, "SPR%d", (int)hs.voiceSpread);
		display->displayPopup(buf);
	}
	if (risingIso & (uint8_t)(1u << kBtnEdit)) {
		hs.editVoicing = !hs.editVoicing;
		if (hs.editVoicing) {
			hs.showChord = true;   // editing needs the chord shown
			hs.stickyChord = true; // and HELD — so the chord survives editing/playing, doesn't vanish on a tap
		}
		display->displayPopup(hs.editVoicing ? "EDIT" : "PLAY");
	}
	if (risingIso & kIsoCtrlClearMask) {
		clearSelection();
		display->displayPopup("CLR");
	}
	// AUDITION (momentary): while held, sound the current VOICING (base chord through spread + stack).
	if (isoCtrlNow & (uint8_t)(1u << kBtnAudition)) {
		int16_t voiced[kMaxVoice];
		uint8_t vn = buildVoicing(voiced, kMaxVoice);
		for (uint8_t i = 0; i < vn; i++) {
			if (voiced[i] >= 0 && voiced[i] <= 127) {
				enableNote((uint8_t)voiced[i], velocity);
			}
		}
	}
	isoCtrlHeldMask = isoCtrlNow;

	// ── PALETTE control column ──
	uint8_t risingPal = (uint8_t)(palCtrlNow & ~palCtrlHeldMask);
	if (risingPal & (uint8_t)(1u << kBtnCalc)) {
		hs.calculatorOn = !hs.calculatorOn;
		if (!hs.calculatorOn) {
			numSuggestions = 0;
			topDeg = -1;
			for (uint8_t d = 0; d < 7; d++) {
				degBright[d] = 0;
			}
		}
		display->displayPopup(hs.calculatorOn ? "CALC" : "OFF");
	}
	if (risingPal & (uint8_t)(1u << kBtnSwap)) {
		hs.swapped = !hs.swapped;
		display->displayPopup(hs.swapped ? "SWAP" : "NORM");
	}
	if (risingPal & (uint8_t)(1u << kBtnPalOctUp)) {
		if (hs.octaveBase < 8) {
			hs.octaveBase++;
		}
		char buf[8];
		sprintf(buf, "OCT%d", (int)hs.octaveBase);
		display->displayPopup(buf);
	}
	if (risingPal & (uint8_t)(1u << kBtnPalOctDown)) {
		if (hs.octaveBase > 1) {
			hs.octaveBase--;
		}
		char buf[8];
		sprintf(buf, "OCT%d", (int)hs.octaveBase);
		display->displayPopup(buf);
	}
	if (risingPal & kPalCtrlClearMask) {
		clearSelection();
		display->displayPopup("CLR");
	}
	palCtrlHeldMask = palCtrlNow;

	ColumnControlsKeyboard::evaluatePads(presses);
}

void KeyboardLayoutHarmonic::handleVerticalEncoder(int32_t offset) {
	if (verticalEncoderHandledByColumns(offset)) {
		return;
	}
	// Vertical encoder scrolls the ISO independently (like the native iso keyboard) — the "octave shown".
	// The PALETTE octave is separate (pal-ctrl OCT+/- buttons), so you set chord register + iso view apart.
	KeyboardStateHarmonic& state = getState().harmonic;
	state.isoOctave += offset;
	if (state.isoOctave < 1) {
		state.isoOctave = 1;
	}
	if (state.isoOctave > 8) {
		state.isoOctave = 8;
	}
	precalculate();
}

void KeyboardLayoutHarmonic::handleHorizontalEncoder(int32_t offset, bool shiftEnabled,
                                                     PressedPad presses[kMaxNumKeyboardPadPresses],
                                                     bool encoderPressed) {
	horizontalEncoderHandledByColumns(offset, shiftEnabled);
}

void KeyboardLayoutHarmonic::renderPads(RGB image[][kDisplayWidth + kSideBarWidth]) {
	uint8_t iv[12];
	uint8_t sc = getScaleIntervals(iv);
	uint8_t keyRoot = (uint8_t)getRootNote();
	uint8_t numCols = (sc > 7) ? 7 : sc;
	bool chromatic = getState().harmonic.isoChromatic;
	bool showChord = getState().harmonic.showChord;
	bool sticky = getState().harmonic.stickyChord;
	bool calc = getState().harmonic.calculatorOn;
	bool swapped = getState().harmonic.swapped;
	bool latticeOn = getState().harmonic.latticeOn;
	bool editVoicing = getState().harmonic.editVoicing;
	bool stackPick = getState().harmonic.stackPick;
	uint8_t voiceOctaves = getState().harmonic.voiceOctaves;
	int8_t voiceSpread = getState().harmonic.voiceSpread;
	int32_t isoStart = isoStartCol(swapped);
	// The highlighted chord on the iso wears its PALETTE colour (the selected degree's hue) — bright primary,
	// faded repeats. Falls back to white when there's no degree (e.g. a voicing built from scratch in EDIT).
	RGB chordHue = (selDeg >= 0 && selDeg < 7) ? kDegreeHue[selDeg] : RGB{.r = 255, .g = 255, .b = 255};
	(void)iv;

	// Breathing pulse for the Calculator's next-chord suggestions (same cadence as the Chord Library).
	uint8_t phase = (AudioEngine::audioSampleTimer >> 7) & 0xFF;     // sawtooth, full cycle ~0.75s
	uint8_t tri = (phase < 128) ? (phase * 2) : ((255 - phase) * 2); // triangle 0..255..0
	uint8_t pulse = 30 + (uint8_t)((uint32_t)tri * 225 / 255);       // breathe between dim and full

	// The actual VOICING (base chord expanded through SPREAD + STACK) — this is what's shown + played.
	int16_t voiced[kMaxVoice];
	uint8_t voicedN = buildVoicing(voiced, kMaxVoice);
	// Match a pad's note against the EXACT voiced notes (not pitch classes).
	auto inChordExact = [&](int32_t note) {
		for (uint8_t i = 0; i < voicedN; i++) {
			if (voiced[i] == note) {
				return true;
			}
		}
		return false;
	};
	// Pitch-class versions for the full LATTICE (every octave/position of a chord tone, not just the voicing).
	auto inChordPc = [&](uint8_t pcq) {
		for (uint8_t i = 0; i < chordNoteCount; i++) {
			if ((uint8_t)(((chordNotes[i] % 12) + 12) % 12) == pcq) {
				return true;
			}
		}
		return false;
	};
	auto isCorePc = [&](uint8_t pcq) {
		uint8_t coreCount = (chordNoteCount < 4) ? chordNoteCount : 4;
		for (uint8_t i = 0; i < coreCount; i++) {
			if ((uint8_t)(((chordNotes[i] % 12) + 12) % 12) == pcq) {
				return true;
			}
		}
		return false;
	};

	// The voicing repeats up the iso grid. Pick ONE pad per voiced note — the lowest (bottom-most, then
	// left-most) occurrence — to draw at full brightness; repeats render dimmer, so one clean shape reads.
	bool primary[kDisplayHeight][kDisplayWidth] = {};
	for (uint8_t i = 0; i < voicedN; i++) {
		bool placed = false;
		for (int32_t yy = 0; yy < kDisplayHeight && !placed; yy++) {
			for (int32_t lx = 0; lx < kBlockWidth && !placed; lx++) {
				int32_t nn = chromatic ? isoNoteChromatic(lx, yy) : isoNoteAt(lx, yy);
				if (nn == voiced[i]) {
					primary[yy][isoStart + lx] = true;
					placed = true;
				}
			}
		}
	}

	for (int32_t x = 0; x < kDisplayWidth; x++) {
		ColInfo ci = colInfoFor(x, swapped);

		if (ci.region == REG_PAL) {
			// The chord explorer. Each column a distinct hue; the triad (bottom) is lightest and the column
			// darkens upward as the chord grows lusher. Calculator flashes suggested degrees; selected cell pops.
			int32_t local = ci.local;
			if (local >= numCols) {
				for (int32_t y = 0; y < kDisplayHeight; y++) {
					image[y][x] = RGB{};
				}
				continue;
			}
			RGB hue = kDegreeHue[local % 7];
			bool haveCalc = (numSuggestions > 0);
			for (int32_t y = 0; y < kDisplayHeight; y++) {
				RGB c = hue.adjustFractional(kRichBright[y], 255);
				if (local == selDeg && y == selRichness) {
					// Selected chord cell: bright near-white tint of the column colour.
					c = RGB{.r = (uint8_t)((c.r + 255) >> 1),
					        .g = (uint8_t)((c.g + 255) >> 1),
					        .b = (uint8_t)((c.b + 255) >> 1)};
				}
				else if (haveCalc && local != selDeg && degBright[local] > 0 && (y == kRowTriad || y == kRow7th)) {
					c = RGB::monochrome((uint8_t)((uint32_t)pulse * degBright[local] / 255));
				}
				image[y][x] = c;
			}
		}
		else if (ci.region == REG_PAL_CTRL) {
			// Palette-bound controls: Calculator on/off, handedness swap. Reserved PINK zone; clear pads below.
			for (int32_t y = 0; y < kDisplayHeight; y++) {
				RGB c = kCtrlHue.adjustFractional(kCtrlFaint, 255); // faint pink marks the control zone
				if (y == kBtnCalc) {
					c = kCtrlHue.adjustFractional(calc ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnSwap) {
					c = kCtrlHue.adjustFractional(swapped ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnPalOctUp || y == kBtnPalOctDown) {
					c = kCtrlHue.adjustFractional(kCtrlOn, 255); // steady-bright palette octave +/- buttons
				}
				image[y][x] = c;
			}
		}
		else if (ci.region == REG_ISO_CTRL) {
			// Iso-bound controls, banded SEE (rows 7-4) → SHAPE (3-1) → HEAR (0), each its own purple shade.
			for (int32_t y = 0; y < kDisplayHeight; y++) {
				RGB grp = (y >= 4) ? kHueSee : (y >= 1) ? kHueShape : kHueHear;
				RGB c = grp.adjustFractional(kCtrlFaint, 255); // faint band tint marks the group
				if (y == kBtnIsoView) {
					c = grp.adjustFractional(chromatic ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnShowChord) {
					c = grp.adjustFractional(showChord ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnSticky) {
					c = grp.adjustFractional(sticky ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnLattice) {
					c = grp.adjustFractional(latticeOn ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnStack) {
					c = grp.adjustFractional(stackPick ? kCtrlOn : kCtrlOff, 255); // octave-picker mode
				}
				else if (y == kBtnSpread) {
					c = grp.adjustFractional(voiceSpread ? (uint8_t)(60 + voiceSpread * 58) : kCtrlOff, 255);
				}
				else if (y == kBtnEdit) {
					c = grp.adjustFractional(editVoicing ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnAudition) {
					// AUDITION = the play button for the voicing; steady bright when a chord is loaded.
					c = grp.adjustFractional(chordNoteCount > 0 ? kCtrlOn : kCtrlOff, 255);
				}
				image[y][x] = c;
			}
		}
		else if (ci.region == REG_ISO) {
			// The iso play surface, tinted to the key's mood colour; brightness carries the relationships:
			// chord CORE (root/3/5/7) brightest, extensions/repeats dimmer, steady tonic anchor, scale backdrop.
			int32_t local = ci.local;
			for (int32_t y = 0; y < kDisplayHeight; y++) {
				// OCTAVE PICKER strip on the bottom row: local 0..6 = octaves -3..+3, local 3 = base.
				if (stackPick && y == 0) {
					bool on = (voiceOctaves & (uint8_t)(1u << local)) != 0;
					uint8_t pb = (local == 3) ? 255 : (on ? 200 : 28);
					image[y][x] = chordHue.adjustFractional(pb, 255);
					continue;
				}
				int32_t note = chromatic ? isoNoteChromatic(local, y) : isoNoteAt(local, y);
				if (note < 0 || note > 127) {
					image[y][x] = RGB{}; // off the top/bottom of MIDI — no real note here, stay dark
					continue;
				}
				int32_t clamped = note;
				uint8_t pc = (uint8_t)(((clamped % 12) + 12) % 12);
				uint8_t within = (uint8_t)(((pc + kOctaveSize) - keyRoot) % kOctaveSize); // 0 = scale tonic
				bool inScale = getScaleNotes().has(within);
				bool playing = false;
				for (uint8_t i = 0; i < currentNotesState.count; i++) {
					if (currentNotesState.notes[i].note == note) {
						playing = true;
						break;
					}
				}
				uint8_t hi = getHighlightedNotes()[clamped];
				RGB key = kKeyColour[keyRoot % 12];
				RGB out;
				// RULE: WHITE is reserved for THE CHORD (voiced shape + its lattice). Everything else lives in
				// the key's COLOUR — the root pops as the brightest expression of that colour, never white.
				if (showChord && latticeOn && chordNoteCount > 0 && inChordPc(pc)) {
					// Chord-lattice overlay in the chord's PALETTE colour, but VERY light — a faint glow of
					// every position the chord makes available, fading upward; never competes with the voicing.
					uint8_t base = isCorePc(pc) ? 55 : 30;
					uint8_t fade = (uint8_t)((uint32_t)base * (uint32_t)(kDisplayHeight - y) / kDisplayHeight);
					if (fade < 6) {
						fade = 6;
					}
					out = chordHue.adjustFractional(fade, 255);
				}
				else if (showChord && inChordExact(note)) {
					// Voiced-chord overlay: WHITE. Primary shape brightest, repeats dimmer, extensions faint.
					// The WHOLE voicing is bright white (every voiced note, incl. 9/11/13). Primary occurrence
					// full-bright; isomorphic repeats a touch dimmer so one clean shape still reads.
					out = chordHue.adjustFractional(primary[y][x] ? 255 : 110, 255);
				}
				else if (playing) {
					out = key.adjustFractional(255, 255); // live free-play notes glow in the KEY colour
				}
				else if (hi != 0
				         && (hi >= 254
				             || runtimeFeatureSettings.get(RuntimeFeatureSettingType::HighlightIncomingNotes)
				                    == RuntimeFeatureStateToggle::On)) {
					out = key.adjustFractional((hi >= 254) ? (hi == 255 ? 235 : 120) : 65, 255);
				}
				else if (within == 0) {
					// ROOT/tonic: the BRIGHTEST expression of the key's own colour — pops in-hue, never white.
					out = key.adjustFractional(150, 255);
				}
				else if (!chromatic || inScale) {
					out = key.adjustFractional(12, 255); // faint scale backdrop (dim, to let the root pop)
				}
				else {
					out = RGB{}; // chromatic off-scale: dark
				}
				image[y][x] = out;
			}
		}
		else {
			for (int32_t y = 0; y < kDisplayHeight; y++) {
				image[y][x] = RGB{};
			}
		}
	}
}

bool KeyboardLayoutHarmonic::allowSidebarType(ColumnControlFunction sidebarType) {
	if (sidebarType == ColumnControlFunction::CHORD) {
		return false;
	}
	return true;
}

} // namespace deluge::gui::ui::keyboard::layout
