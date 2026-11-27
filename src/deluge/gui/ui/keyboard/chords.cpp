/*
 * Copyright (c) 2014-2024 Synthstrom Audible Limited
 * Copyright © 2024 Madeline Scyphers
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

#include "gui/ui/keyboard/chords.h"
#include "definitions_cxx.hpp"
#include "hid/display/display.h"
#include "io/debug/log.h"
#include "util/functions.h"
#include <array>
#include <stdio.h>
#include <stdlib.h>

namespace deluge::gui::ui::keyboard {

ChordQuality getChordQuality(NoteSet& notes) {
	if (notes.count() < 3) {
		return ChordQuality::OTHER;
	}
	else if (notes.has(MAJ3) && notes.has(P5)) {
		if (notes.has(MIN7)) {
			return ChordQuality::DOMINANT;
		}
		else {
			return ChordQuality::MAJOR;
		}
	}
	else if (notes.has(MIN3) && notes.has(P5)) {
		return ChordQuality::MINOR;
	}
	else if (notes.has(MIN3) && notes.has(DIM5)) {
		return ChordQuality::DIMINISHED;
	}
	else if (notes.has(MAJ3) && notes.has(AUG5)) {
		return ChordQuality::AUGMENTED;
	}
	else {
		return ChordQuality::OTHER;
	}
}

ChordList::ChordList()
    : chords{
          kEmptyChord, kMajor,  kMinor,  k6,      k2,   k69,      kSus2,    kSus4,   k7,         k7Sus4,      k7Sus2,
          kM7,         kMinor7, kMinor2, kMinor4, kDim, kFullDim, kAug,     kMinor6, kMinorMaj7, kMinor7b5,   kMinor9b5,
          kMinor7b5b9, k9,      kM9,     kMinor9, k11,  kM11,     kMinor11, k13,     kM13,       kM13Sharp11, kMinor13,
      } {
}

Voicing ChordList::getChordVoicing(int8_t chordNo) {
	// Check if chord number is valid
	chordNo = validateChordNo(chordNo);

	int8_t voicingNo = voicingOffset[chordNo];
	if (voicingNo <= 0) {
		return chords[chordNo].voicings[0];
	}
	// Check if voicing is valid
	// voicings after the first should default to 0
	// So if the voicing is all 0, we should return the voicing before it
	else if (voicingNo > 0) {
		// voicingOffset can be set out of bounds (the UI lets it run past the end), so clamp to the last real voicing
		// before indexing - voicings[] only has kUniqueVoicings entries and reading past them is a buffer overflow.
		if (voicingNo >= kUniqueVoicings) {
			voicingNo = kUniqueVoicings - 1;
		}
		for (int voicingN = voicingNo; voicingN >= 0; voicingN--) {
			Voicing voicing = chords[chordNo].voicings[voicingN];

			bool valid = false;
			for (int j = 0; j < kMaxChordKeyboardSize; j++) {
				if (voicing.offsets[j] != 0) {
					valid = true;
				}
			}
			if (valid) {
				return voicing;
			}
		}
		D_PRINTLN("Voicing is invalid, returning default voicing");
		return chords[0].voicings[0];
	}
	return chords[chordNo].voicings[0];
}

void ChordList::adjustChordRowOffset(int8_t offset) {
	if (offset > 0) {
		chordRowOffset = std::min<int8_t>(kOffScreenChords, chordRowOffset + offset);
	}
	else {
		chordRowOffset = std::max<int8_t>(0, chordRowOffset + offset);
	}
}

void ChordList::adjustVoicingOffset(int8_t chordNo, int8_t offset) {
	// Check if chord number is valid
	chordNo = validateChordNo(chordNo);

	if (offset > 0) {
		voicingOffset[chordNo] = std::min<int8_t>(kUniqueVoicings - 1, voicingOffset[chordNo] + offset);
	}
	else {
		voicingOffset[chordNo] = std::max<int8_t>(0, voicingOffset[chordNo] + offset);
	}
}

int8_t ChordList::validateChordNo(int8_t chordNo) {
	if (chordNo < 0) {
		D_PRINTLN("Chord number is negative, returning chord 0");
		chordNo = 0;
	}
	else if (chordNo >= kUniqueChords) {
		D_PRINTLN("Chord number is too high, returning last chord");
		chordNo = kUniqueChords - 1;
	}
	return chordNo;
}

uint8_t resolveChordNotes(const ChordSelection& selection, int16_t* notesOut, uint8_t maxNotes) {
	uint8_t count = 0;
	for (int32_t i = 0; i < kMaxChordKeyboardSize && count < maxNotes; i++) {
		int8_t offset = selection.voicing.offsets[i];
		if (offset == NONE) {
			continue;
		}
		notesOut[count++] = static_cast<int16_t>(selection.rootNote + offset);
	}
	return count;
}
// ChordList
PLACE_SDRAM_DATA const Chord kEmptyChord = {"", NoteSet({ROOT}), {{0, NONE, NONE, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMajor = {"M",
                                       NoteSet({ROOT, MAJ3, P5}),
                                       {{ROOT, MAJ3, P5, NONE, NONE, NONE, NONE},
                                        {ROOT, OCT + MAJ3, P5, NONE, NONE, NONE, NONE},
                                        {ROOT, OCT + MAJ3, P5, -OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor = {"-",
                                       NoteSet({ROOT, MIN3, P5}),
                                       {{ROOT, MIN3, P5, NONE, NONE, NONE, NONE},
                                        {ROOT, OCT + MIN3, P5, NONE, NONE, NONE, NONE},
                                        {ROOT, OCT + MIN3, P5, -OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kDim = {"DIM",
                                     NoteSet({ROOT, MIN3, DIM5}),
                                     {{ROOT, MIN3, DIM5, NONE, NONE, NONE, NONE},
                                      {ROOT, OCT + MIN3, DIM5, NONE, NONE, NONE, NONE},
                                      {ROOT, OCT + MIN3, DIM5, -OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kFullDim = {
    "FULLDIM", NoteSet({ROOT, MIN3, DIM5, DIM7}), {{ROOT, MIN3, DIM5, DIM7, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kAug = {"AUG",
                                     NoteSet({ROOT, MIN3, AUG5}),
                                     {{ROOT, MIN3, AUG5, NONE, NONE, NONE, NONE},
                                      {ROOT, OCT + MIN3, AUG5, NONE, NONE, NONE, NONE},
                                      {ROOT, OCT + MIN3, AUG5, -OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kSus2 = {"SUS2",
                                      NoteSet({ROOT, MAJ2, P5}),
                                      {{ROOT, MAJ2, P5, NONE, NONE, NONE, NONE},
                                       {ROOT, MAJ2 + OCT, P5, NONE, NONE, NONE, NONE},
                                       {ROOT, MAJ2 + OCT, P5, -OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kSus4 = {"SUS4",
                                      NoteSet({ROOT, P4, P5}),
                                      {{ROOT, P4, P5, NONE, NONE, NONE, NONE},
                                       {ROOT, P4 + OCT, P5, NONE, NONE, NONE, NONE},
                                       {ROOT, P4 + OCT, P5, -OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k7 = {"7",
                                   NoteSet({ROOT, MAJ3, P5, MIN7}),
                                   {{ROOT, MAJ3, P5, MIN7, NONE, NONE, NONE},
                                    {ROOT, MAJ3 + OCT, P5, MIN7, NONE, NONE, NONE},
                                    {ROOT, MAJ3 + OCT, P5, MIN7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k7Sus4 = {"7SUS4",
                                       NoteSet({ROOT, P4, P5, MIN7}),
                                       {{ROOT, P4, P5, MIN7, NONE, NONE, NONE},
                                        {ROOT, P4 + OCT, P5, MIN7, NONE, NONE, NONE},
                                        {ROOT, P4 + OCT, P5, MIN7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k7Sus2 = {"7SUS2",
                                       NoteSet({ROOT, MAJ2, P5, MIN7}),
                                       {{ROOT, MAJ2, P5, MIN7, NONE, NONE, NONE},
                                        {ROOT, MAJ2 + OCT, P5, MIN7, NONE, NONE, NONE},
                                        {ROOT, MAJ2 + OCT, P5, MIN7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kM7 = {"M7",
                                    NoteSet({ROOT, MAJ3, P5, MAJ7}),
                                    {{ROOT, MAJ3, P5, MAJ7, NONE, NONE, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MAJ7, NONE, NONE, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MAJ7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor7 = {"-7",
                                        NoteSet({ROOT, MIN3, P5, MIN7}),
                                        {{ROOT, MIN3, P5, MIN7, NONE, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5, MIN7, NONE, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5, MIN7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor2 = {"-2",
                                        NoteSet({ROOT, MIN3, P5, MAJ2}),
                                        {{ROOT, MIN3, P5, MAJ2, NONE, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5, MAJ2, NONE, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5 + OCT, MAJ2, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor4 = {"-4",
                                        NoteSet({ROOT, MIN3, P5, P4}),
                                        {{ROOT, MIN3, P5, P4, NONE, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5, P4, NONE, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5 + OCT, P4, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinorMaj7 = {"-M7",
                                           NoteSet({ROOT, MIN3, P5, MAJ7}),
                                           {{ROOT, MIN3, P5, MAJ7, NONE, NONE, NONE},
                                            {ROOT, MIN3 + OCT, P5, MAJ7, NONE, NONE, NONE},
                                            {ROOT, MIN3 + OCT, P5, MAJ7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor7b5 = {"-7" FLAT_CHAR_STR "5",
                                          NoteSet({ROOT, MIN3, DIM5, MIN7}),
                                          {{ROOT, MIN3, DIM5, MIN7, NONE, NONE, NONE},
                                           {ROOT, MIN3 + OCT, DIM5, MIN7, NONE, NONE, NONE},
                                           {ROOT, MIN3 + OCT, DIM5, MIN7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor9b5 = {
    "-9" FLAT_CHAR_STR "5", NoteSet({ROOT, MIN3, DIM5, MIN7, MAJ2}), {{ROOT, MIN3, DIM5, MIN7, MAJ9, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor7b5b9 = {"-7" FLAT_CHAR_STR "5" FLAT_CHAR_STR "9",
                                            NoteSet({ROOT, MIN3, DIM5, MIN7, MIN2}),
                                            {{ROOT, MIN3, DIM5, MIN7, MIN9, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k9 = {"9",
                                   NoteSet({ROOT, MAJ3, P5, MIN7, MAJ2}),
                                   {{ROOT, MAJ3, P5, MIN7, MAJ9, NONE, NONE},
                                    {ROOT, MAJ3 + OCT, P5, MIN7, MAJ9, NONE, NONE},
                                    {ROOT, MAJ3 + OCT, P5, MIN7 + OCT, MAJ9, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kM9 = {"M9",
                                    NoteSet({ROOT, MAJ3, P5, MAJ7, MAJ2}),
                                    {{ROOT, MAJ3, P5, MAJ7, MAJ9, NONE, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MAJ7, MAJ9, NONE, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MAJ7 + OCT, MAJ9, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor9 = {"-9",
                                        NoteSet({ROOT, MIN3, P5, MIN7, MAJ2}),
                                        {{ROOT, MIN3, P5, MIN7, MAJ9, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5, MIN7, MAJ9, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5, MIN7 + OCT, MAJ9, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k11 = {"11",
                                    NoteSet({ROOT, MAJ3, P5, MIN7, MAJ2, P4}),
                                    {{ROOT, MAJ3, P5, MIN7, MAJ9, P11, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MIN7, MAJ9, P11, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MIN7 + OCT, MAJ9, P11, NONE}}};
PLACE_SDRAM_DATA const Chord kM11 = {"M11",
                                     NoteSet({ROOT, MAJ3, P5, MAJ7, MAJ2, P4}),
                                     {{ROOT, MAJ3, P5, MAJ7, MAJ9, P11, NONE},
                                      {ROOT, MAJ3 + OCT, P5, MAJ7, MAJ9, P11, NONE},
                                      {ROOT, MAJ3 + OCT, P5, MAJ7 + OCT, MAJ9, P11, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor11 = {"-11",
                                         NoteSet({ROOT, MIN3, P5, MIN7, MAJ2, P4}),
                                         {{ROOT, MIN3, P5, MIN7, MAJ9, P11, NONE},
                                          {{ROOT, P4, MIN7, MIN3 + OCT, P5 + OCT, NONE, NONE}, "SO WHAT"},
                                          {ROOT, MIN3 + OCT, P5, MIN7, MAJ9, P11, NONE},
                                          {ROOT, MIN3 + OCT, P5, MIN7 + OCT, MAJ9, P11, NONE}}};
// 11th are often omitted in 13th and M13th chords because they clash with the major 3rd
// if anything, the 11th is often played as a #11
PLACE_SDRAM_DATA const Chord k13 = {"13",
                                    NoteSet({ROOT, MAJ3, P5, MIN7, MAJ2, MAJ6}),
                                    {{ROOT, MAJ3, P5, MIN7, MAJ9, MAJ13, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MIN7, MAJ9, MAJ13, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MIN7 + OCT, MAJ9, MAJ13, NONE}}};
PLACE_SDRAM_DATA const Chord kM13 = {"M13",
                                     NoteSet({ROOT, MAJ3, P5, MAJ7, MAJ2, MAJ6}),
                                     {{ROOT, MAJ3, P5, MAJ7, MAJ9, MAJ13, NONE},
                                      {ROOT, MAJ3 + OCT, P5, MAJ7, MAJ9, MAJ13, NONE},
                                      {ROOT, MAJ3 + OCT, P5, MAJ7 + OCT, MAJ9, MAJ13, NONE}}};
PLACE_SDRAM_DATA const Chord kM13Sharp11 = {"M13#11",
                                            NoteSet({ROOT, MAJ3, P5, MAJ7, MAJ2, MAJ6, AUG4}),
                                            {{ROOT, MAJ3, P5, MAJ7, MAJ9, MAJ13, AUG11},
                                             {ROOT, MAJ3 + OCT, P5, MAJ7, MAJ9, MAJ13, AUG11},
                                             {ROOT, MAJ3 + OCT, P5, MAJ7 + OCT, MAJ9, MAJ13, AUG11}}};
PLACE_SDRAM_DATA const Chord kMinor13 = {"-13",
                                         NoteSet({ROOT, MIN3, P5, MIN7, MAJ2, P4, MAJ6}),
                                         {{ROOT, MIN3, P5, MIN7, MAJ9, P11, MAJ13},
                                          {ROOT, MIN3 + OCT, P5, MIN7, MAJ9, P11, MAJ13},
                                          {ROOT, MIN3 + OCT, P5, MIN7 + OCT, MAJ9, P11, MAJ13}}};
PLACE_SDRAM_DATA const Chord k6 = {"6",
                                   NoteSet({ROOT, MAJ3, P5, MAJ6}),
                                   {
                                       {ROOT, MAJ3, P5, MAJ6, NONE, NONE, NONE},
                                   }};
PLACE_SDRAM_DATA const Chord k2 = {"2",
                                   NoteSet({ROOT, MAJ3, P5, MAJ2}),
                                   {
                                       {{ROOT, MAJ3 - OCT, P5, MAJ2, NONE, NONE, NONE}, "Open Mu"},
                                       {{ROOT, MAJ3, P5, MAJ2, NONE, NONE, NONE}, "Mu"},
                                   }};
PLACE_SDRAM_DATA const Chord k69 = {"69",
                                    NoteSet({ROOT, MAJ3, P5, MAJ6, MAJ2}),
                                    {
                                        {ROOT, MAJ3, P5, MAJ6, MAJ9, NONE, NONE},
                                    }};
PLACE_SDRAM_DATA const Chord kMinor6 = {"-6",
                                        NoteSet({ROOT, MIN3, P5, MAJ6}),
                                        {
                                            {ROOT, MIN3, P5, MAJ6, NONE, NONE, NONE},
                                        }};

PLACE_SDRAM_DATA const std::array<const Chord, 10> majorChords = {kMajor, kM7,  k6,    k2,    k69,
                                                                  kM9,    kM13, kSus4, kSus2, kM13Sharp11};

PLACE_SDRAM_DATA const std::array<const Chord, 10> minorChords = {
    kMinor, kMinor7, kMinor4, kMinor11, kMinor6, kMinor2, kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord,
};

PLACE_SDRAM_DATA const std::array<const Chord, 10> dominantChords = {
    kMajor, k7, k69, k9, k7Sus4, k7Sus2, k11, k13, kEmptyChord, kEmptyChord,
};

PLACE_SDRAM_DATA const std::array<const Chord, 10> diminishedChords = {
    kDim,        kMinor7b5,   kMinor7b5b9, kEmptyChord, kEmptyChord,
    kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord,
};

PLACE_SDRAM_DATA const std::array<const Chord, 10> augmentedChords = {
    kAug,        kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord,
    kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord,
};

PLACE_SDRAM_DATA const std::array<const Chord, 10> otherChords = {
    kSus2,       kSus4,       kEmptyChord, kEmptyChord, kEmptyChord,
    kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord,
};

// Name a pitch class with flats or sharps to match the key signature.
const char* noteNameInKey(uint8_t pitchClass, bool preferFlats) {
	static const char* const kSharpNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	static const char* const kFlatNames[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
	return (preferFlats ? kFlatNames : kSharpNames)[pitchClass % 12];
}

// Does this key spell with flats? Determined from the circle of fifths: find the key's relative major
// (minor keys -> +3 semitones) and check whether that major key is a flat key (F/Bb/Eb/Ab/Db).
bool keyPrefersFlats(uint8_t keyRootPc, NoteSet scale) {
	bool isMinor = scale.has(MIN3) && !scale.has(MAJ3); // minor-ish scale (minor 3rd, no major 3rd)
	uint8_t majorRoot = isMinor ? (uint8_t)((keyRootPc + 3) % 12) : (uint8_t)(keyRootPc % 12);
	switch (majorRoot) {
	case 5:  // F
	case 10: // Bb
	case 3:  // Eb
	case 8:  // Ab
	case 1:  // Db
		return true;
	default:
		return false; // C/G/D/A/E/B and F# -> sharps (or no accidentals)
	}
}

// Device-wide spelling lean (synced across surfaces). Defaults to AUTO = follow the key.
ChromaSpelling gChromaSpelling = ChromaSpelling::AUTO;

bool effectivePreferFlats(uint8_t keyRootPc, NoteSet scale) {
	switch (gChromaSpelling) {
	case ChromaSpelling::FLATS:
		return true;
	case ChromaSpelling::SHARPS:
		return false;
	default:
		return keyPrefersFlats(keyRootPc, scale); // AUTO: the key decides
	}
}

// CHORDS NAMED BUT NOT PLAYED (Chroma 1.3.137): common extended chords the chord keyboards do not offer, kept apart
// from ChordList so its menus and lists are untouched. Matched exactly, after the table and before a near match.
// Cody, 2026-11-26: "there has to be a logical name for the chord" - E G B D F is Em7b9, every note accounted for.
struct NamedOnlyChord {
	const char* name;
	NoteSet intervals;
};
static const NamedOnlyChord kNamedOnlyChords[] = {
    {"-7" FLAT_CHAR_STR "9", NoteSet({ROOT, MIN3, P5, MIN7, MIN2})},
    {"7" FLAT_CHAR_STR "9", NoteSet({ROOT, MAJ3, P5, MIN7, MIN2})},
    {"7#9", NoteSet({ROOT, MAJ3, P5, MIN7, MIN3})},
    {"7" FLAT_CHAR_STR "5", NoteSet({ROOT, MAJ3, DIM5, MIN7})},
    {"7#5", NoteSet({ROOT, MAJ3, AUG5, MIN7})},
    {"7#11", NoteSet({ROOT, MAJ3, P5, MIN7, AUG4})},
    {"7" FLAT_CHAR_STR "13", NoteSet({ROOT, MAJ3, P5, MIN7, MIN6})},
    {"13" FLAT_CHAR_STR "9", NoteSet({ROOT, MAJ3, P5, MIN7, MIN2, MAJ6})},
    {"ADD9", NoteSet({ROOT, MAJ3, P5, MAJ2})},
    {"-ADD9", NoteSet({ROOT, MIN3, P5, MAJ2})},
    {"9SUS4", NoteSet({ROOT, P4, P5, MIN7, MAJ2})},
    {"-M9", NoteSet({ROOT, MIN3, P5, MAJ7, MAJ2})},
};

bool nameChordFromNotes(const uint8_t* notes, int32_t count, char* out, bool preferFlats) {
	if (count < 2) {
		return false;
	}
	static ChordList chordList; // the named-chord table, built once

	// Bass = lowest absolute note; the set of pitch classes present.
	uint8_t bass = notes[0];
	NoteSet present;
	present.clear();
	for (int32_t i = 0; i < count; i++) {
		if (notes[i] < bass) {
			bass = notes[i];
		}
		present.add(notes[i] % 12);
	}
	const int32_t bassPc = bass % 12;
	auto intervalsFrom = [&](int32_t rootPc) {
		NoteSet intervals;
		intervals.clear();
		for (int32_t i = 0; i < count; i++) {
			intervals.add((((notes[i] % 12) - rootPc) + 12) % 12);
		}
		return intervals;
	};
	auto write = [&](int32_t rootPc, const char* name, bool slash) {
		if (slash && rootPc != bassPc) {
			sprintf(out, "%s%s/%s", noteNameInKey(rootPc, preferFlats), name, noteNameInKey(bassPc, preferFlats));
		}
		else {
			sprintf(out, "%s%s", noteNameInKey(rootPc, preferFlats), name);
		}
		return true;
	};
	// An exact name from `rootPc`: the chord table, then the named-only chords.
	auto exactFrom = [&](int32_t rootPc, bool slash) {
		const NoteSet intervals = intervalsFrom(rootPc);
		for (int32_t c = 0; c < kUniqueChords; c++) {
			const Chord& chord = chordList.chords[c];
			if (chord.name[0] != '\0' && intervals == chord.intervalSet) {
				return write(rootPc, chord.name, false); // the table's own inversions read as the chord, as before
			}
		}
		for (const NamedOnlyChord& chord : kNamedOnlyChords) {
			if (intervals == chord.intervals) {
				return write(rootPc, chord.name, slash);
			}
		}
		return false;
	};
	// The chord from `rootPc` that holds every note with the fewest of its own left out, at most `maxMissing`.
	auto nearFrom = [&](int32_t rootPc, int32_t maxMissing, int32_t& missingOut) -> const char* {
		const NoteSet intervals = intervalsFrom(rootPc);
		const char* best = nullptr;
		int32_t bestMissing = maxMissing + 1;
		for (int32_t c = 0; c < kUniqueChords; c++) {
			const Chord& chord = chordList.chords[c];
			if (chord.name[0] == '\0' || !intervals.isSubsetOf(chord.intervalSet)) {
				continue;
			}
			const int32_t missing = chord.intervalSet.count() - intervals.count();
			if (missing < bestMissing) {
				bestMissing = missing;
				best = chord.name;
			}
		}
		missingOut = bestMissing;
		return best;
	};

	// THE ORDER (Chroma 1.3.137). Cody, 2026-11-26: "there has to be a logical name for the chord".
	//   1. an exact name from the bass                       E G B D F    -> Em7b9
	//   2. a near name from the bass, one note left out      C E G Bb A   -> C13 (no 9th)
	//   3. an exact name from another note, as a slash chord
	//   4. the nearest chord holding every note, any root, two left out at most, as a slash chord
	// Three different notes at least for a near name: two are an interval, named elsewhere.
	if (exactFrom(bassPc, true)) {
		return true;
	}
	int32_t missing = 0;
	if (present.count() >= 3) {
		if (const char* name = nearFrom(bassPc, 1, missing)) {
			return write(bassPc, name, false);
		}
	}
	for (int32_t rootPc = 0; rootPc < 12; rootPc++) {
		if (rootPc != bassPc && present.has(rootPc) && exactFrom(rootPc, true)) {
			return true;
		}
	}
	if (present.count() < 3) {
		return false;
	}
	const char* bestName = nullptr;
	int32_t bestRoot = -1, bestMissing = 3;
	for (int32_t rootPc = 0; rootPc < 12; rootPc++) {
		if (!present.has(rootPc)) {
			continue;
		}
		const char* name = nearFrom(rootPc, 2, missing);
		if (name != nullptr && missing < bestMissing) {
			bestMissing = missing;
			bestRoot = rootPc;
			bestName = name;
		}
	}
	return bestName != nullptr && write(bestRoot, bestName, true);
}

// Voice-leading distance: total minimal semitone movement to get from chord `a` to chord `b`
// (each note of `a` finds its nearest note in `b`).
static int32_t voiceDistance(const uint8_t* a, int32_t na, const uint8_t* b, int32_t nb) {
	int32_t total = 0;
	for (int32_t i = 0; i < na; i++) {
		int32_t best = 12;
		for (int32_t j = 0; j < nb; j++) {
			int32_t d = ((a[i] - b[j]) + 12) % 12;
			if (d > 6) {
				d = 12 - d;
			}
			if (d < best) {
				best = d;
			}
		}
		total += best;
	}
	return total;
}

// Build the diatonic 7th on scale degree `deg`: fill pcsOut[4] with its pitch classes and return the
// matching ChordList index (or -1 if its quality isn't in the table).
static int8_t diatonicChord(uint8_t keyRoot, const uint8_t* scaleIv, int deg, uint8_t* pcsOut, ChordList& chordList) {
	uint8_t rootPc = (keyRoot + scaleIv[deg]) % 12;
	NoteSet intervals;
	intervals.clear();
	for (int32_t k = 0; k < 4; k++) {
		uint8_t pc = (keyRoot + scaleIv[(deg + 2 * k) % 7]) % 12;
		pcsOut[k] = pc;
		intervals.add(((pc - rootPc) + 12) % 12);
	}
	for (int32_t c = 0; c < kUniqueChords; c++) {
		if (chordList.chords[c].name[0] != '\0' && intervals == chordList.chords[c].intervalSet) {
			return c;
		}
	}
	return -1;
}

int suggestNextChords(uint8_t keyRoot, NoteSet scaleNotes, uint8_t scaleCount, uint8_t currentRootPc,
                      ChordSuggestion* out, int32_t maxOut) {
	if (scaleCount != 7) {
		return 0; // the pull table is defined for 7-note scales (minor/major/modes)
	}
	static ChordList chordList;

	// scale intervals from root, in order
	uint8_t scaleIv[7];
	int32_t idx = 0;
	for (int32_t i = 0; i < 12 && idx < 7; i++) {
		if (scaleNotes.has(i)) {
			scaleIv[idx++] = i;
		}
	}
	if (idx != 7) {
		return 0;
	}

	int32_t curDeg = -1;
	for (int32_t d = 0; d < 7; d++) {
		if (((keyRoot + scaleIv[d]) % 12) == currentRootPc) {
			curDeg = d;
		}
	}
	if (curDeg < 0) {
		return 0; // current chord isn't a diatonic root
	}

	uint8_t curPcs[4];
	diatonicChord(keyRoot, scaleIv, curDeg, curPcs, chordList);

	// deep-house functional pull: weight[from][to] (baseline 1, 0 on the diagonal). Ported from
	// chord_suggest.py's DH_PULL — modal loops over classical V-I.
	static const int8_t DH_PULL[7][7] = {
	    {0, 1, 2, 4, 3, 5, 5}, {3, 0, 1, 2, 5, 1, 2}, {2, 1, 0, 2, 1, 4, 3}, {4, 2, 1, 0, 1, 3, 5},
	    {5, 1, 1, 2, 0, 4, 2}, {3, 2, 1, 4, 1, 0, 5}, {5, 1, 1, 3, 2, 4, 0},
	};

	int32_t scores[7];
	int32_t degs[7];
	int32_t n = 0;
	for (int32_t d = 0; d < 7; d++) {
		if (d == curDeg) {
			continue;
		}
		uint8_t pcs[4];
		diatonicChord(keyRoot, scaleIv, d, pcs, chordList);
		int32_t vd = voiceDistance(curPcs, 4, pcs, 4);
		scores[n] = 10 * DH_PULL[curDeg][d] + 8 * (6 - vd); // W_PULL=1.0, W_VOICE=0.8 (x10)
		degs[n] = d;
		n++;
	}
	// selection sort, descending by score
	for (int32_t i = 0; i < n; i++) {
		for (int32_t j = i + 1; j < n; j++) {
			if (scores[j] > scores[i]) {
				int32_t ts = scores[i];
				scores[i] = scores[j];
				scores[j] = ts;
				int32_t td = degs[i];
				degs[i] = degs[j];
				degs[j] = td;
			}
		}
	}

	int32_t count = 0;
	for (int32_t i = 0; i < n && count < maxOut; i++) {
		uint8_t pcs[4];
		int8_t cn = diatonicChord(keyRoot, scaleIv, degs[i], pcs, chordList);
		out[count].rootNote = (keyRoot + scaleIv[degs[i]]) % 12;
		out[count].chordNo = cn;
		count++;
	}
	return count;
}

bool describeChordInKey(const uint8_t* notes, int32_t count, uint8_t keyRootPc, NoteSet scale, char* absOut,
                        char* romanOut) {
	romanOut[0] = '\0';
	if (count < 2) {
		return false;
	}
	static ChordList chordList;
	bool flats = effectivePreferFlats(keyRootPc, scale);

	uint8_t bass = notes[0];
	NoteSet present;
	present.clear();
	for (int32_t i = 0; i < count; i++) {
		if (notes[i] < bass) {
			bass = notes[i];
		}
		present.add(notes[i] % 12);
	}
	int32_t bassPc = bass % 12;

	// scale degrees as intervals from the key root (for the Roman numeral)
	uint8_t scaleIv[12];
	int32_t scaleCount = 0;
	for (int32_t i = 0; i < 12; i++) {
		if (scale.has(i)) {
			scaleIv[scaleCount++] = i;
		}
	}
	static const char* const kRoman[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
	static const int8_t kMajorRef[7] = {0, 2, 4, 5, 7, 9, 11};
	// The numeral for a root that is a degree of a seven-note scale, upper case for a major third, then the
	// suffix. Writes nothing when the root is not a degree, or the scale does not have seven notes.
	auto writeRoman = [&](int32_t rootPc, bool upper, const char* suffix) {
		if (scaleCount != 7) {
			return;
		}
		for (int32_t d = 0; d < 7; d++) {
			if (((keyRootPc + scaleIv[d]) % 12) != (uint32_t)rootPc) {
				continue;
			}
			int32_t acc = scaleIv[d] - kMajorRef[d]; // -1 = flat, +1 = sharp degree
			char num[8];
			int32_t n = 0;
			if (acc < 0) {
				num[n++] = 'b';
			}
			else if (acc > 0) {
				num[n++] = '#';
			}
			const char* numeral = kRoman[d];
			for (int32_t k = 0; numeral[k] != '\0'; k++) {
				num[n++] = upper ? numeral[k] : (char)(numeral[k] - 'A' + 'a');
			}
			num[n] = '\0';
			sprintf(romanOut, "%s%s", num, suffix);
			return;
		}
	};

	for (int32_t attempt = -1; attempt < 12; attempt++) {
		int32_t rootPc = (attempt < 0) ? bassPc : attempt;
		if ((attempt >= 0 && rootPc == bassPc) || !present.has(rootPc)) {
			continue;
		}
		NoteSet intervals;
		intervals.clear();
		for (int32_t i = 0; i < count; i++) {
			intervals.add((((notes[i] % 12) - rootPc) + 12) % 12);
		}
		for (int32_t c = 0; c < kUniqueChords; c++) {
			const Chord& chord = chordList.chords[c];
			if (chord.name[0] == '\0' || !(intervals == chord.intervalSet)) {
				continue;
			}
			// Absolute name, spelled to the key (e.g. "Db M7").
			sprintf(absOut, "%s%s", noteNameInKey(rootPc, flats), chord.name);

			// Roman numeral: only for 7-note scales when the root is a scale degree.
			writeRoman(rootPc, chord.intervalSet.has(MAJ3), chord.name);
			return true;
		}
	}

	// TWO DIFFERENT NOTES (Chroma, 1.3.117): an interval, named from the lowest note. However many octaves of
	// each are held, two pitch classes are a dyad - Cody, 2026-11-24, holding A6 C7 C8: "shows nothing, include
	// dyads". None of the chord table's shapes has two notes, so before this a dyad named nothing at all.
	//
	// From the bass, as it is played: C under A is a major sixth, not an inverted A minor - an interval does not
	// say which triad it belongs to, so this does not guess. A fifth is written the way players write it ("A5").
	// A third or a fifth on a degree of the key gets its numeral, lower case for a minor third: those three
	// imply the chord on that degree. The other intervals get none.
	if (present.count() == 2) {
		int32_t otherPc = bassPc;
		for (int32_t pc = 0; pc < 12; pc++) {
			if (pc != bassPc && present.has(pc)) {
				otherPc = pc;
			}
		}
		const int32_t interval = ((otherPc - bassPc) + 12) % 12;
		static const char* const kIntervalName[12] = {"",    " m2", " M2", " m3", " M3", " P4",
		                                              " TT", "5",   " m6", " M6", " m7", " M7"};
		sprintf(absOut, "%s%s", noteNameInKey(bassPc, flats), kIntervalName[interval]);
		if (interval == MIN3) {
			writeRoman(bassPc, false, "");
		}
		else if (interval == MAJ3) {
			writeRoman(bassPc, true, "");
		}
		else if (interval == P5) {
			writeRoman(bassPc, true, "5");
		}
		return true;
	}
	return false;
}

} // namespace deluge::gui::ui::keyboard
