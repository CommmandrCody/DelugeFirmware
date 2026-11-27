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
#include <string.h>

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
// 1.3.139: a MAJOR third. An augmented triad is root, major third, augmented fifth (C E G#); the table had a minor
// third, so it played C Eb G# and named E G C "EAUG" - found by chroma/tools/chord-oracle against tonal.
PLACE_SDRAM_DATA const Chord kAug = {"AUG",
                                     NoteSet({ROOT, MAJ3, AUG5}),
                                     {{ROOT, MAJ3, AUG5, NONE, NONE, NONE, NONE},
                                      {ROOT, OCT + MAJ3, AUG5, NONE, NONE, NONE, NONE},
                                      {ROOT, OCT + MAJ3, AUG5, -OCT, NONE, NONE, NONE}}};
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
// from ChordList so its menus and lists are untouched. Matched after the table.
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

// THE STANDARD SPELLING (Chroma 1.3.139), for hosts: the Deluge's labels were written for a four-character 7-seg and
// read as codes. Cody, 2026-11-26, beside Scaler, Orchid and Chordion: "it confuses me big time compared to how they
// name their chords". The display keeps its codes; a host is told the names a musician reads. Checked, every one, by
// chroma/tools/chord-oracle against an independent library (tonal).
struct Spelling {
	const char* code;
	const char* standard;
};
static const Spelling kStandardSpelling[] = {
    {"M", ""},
    {"-", "m"},
    {"DIM", "dim"},
    {"FULLDIM", "dim7"},
    {"AUG", "aug"},
    {"SUS2", "sus2"},
    {"SUS4", "sus4"},
    {"7SUS4", "7sus4"},
    {"7SUS2", "7sus2"},
    {"M7", "maj7"},
    {"-7", "m7"},
    {"-2", "m(add9)"},
    {"-4", "m(add11)"},
    {"-M7", "m(maj7)"},
    {"-7" FLAT_CHAR_STR "5", "m7b5"},
    {"-9" FLAT_CHAR_STR "5", "m9b5"},
    {"-7" FLAT_CHAR_STR "5" FLAT_CHAR_STR "9", "m7b5b9"},
    {"M9", "maj9"},
    {"-9", "m9"},
    {"M11", "maj11"},
    {"-11", "m11"},
    {"M13", "maj13"},
    {"M13#11", "maj13#11"},
    {"-13", "m13"},
    {"2", "add9"},
    {"69", "6/9"},
    {"-6", "m6"},
    {"-7" FLAT_CHAR_STR "9", "m7b9"},
    {"7" FLAT_CHAR_STR "9", "7b9"},
    {"7" FLAT_CHAR_STR "5", "7b5"},
    {"7" FLAT_CHAR_STR "13", "7b13"},
    {"13" FLAT_CHAR_STR "9", "13b9"},
    {"ADD9", "add9"},
    {"-ADD9", "m(add9)"},
    {"9SUS4", "9sus4"},
    {"-M9", "m(maj9)"},
};

const char* standardChordSpelling(const char* code) {
	for (const Spelling& s : kStandardSpelling) {
		if (strcmp(s.code, code) == 0) {
			return s.standard;
		}
	}
	return code; // the same in both: 7, 9, 11, 13, 6, 7#9, 7#5, 7#11
}

namespace {
// A NAMED CHORD, before it is spelled: which note is its root, which is in the bass, and what it is. Spelled twice
// from this one answer - for the display and for a host - so the two can never name different chords.
struct NamedChord {
	int32_t root = -1;
	int32_t bass = -1;
	const char* code = ""; // the table's label, or an interval for two notes
	NoteSet intervals;     // the named chord's notes, from its root
	bool dyad = false;
};

NoteSet intervalsFrom(const uint8_t* notes, int32_t count, int32_t rootPc) {
	NoteSet intervals;
	intervals.clear();
	for (int32_t i = 0; i < count; i++) {
		intervals.add((((notes[i] % 12) - rootPc) + 12) % 12);
	}
	return intervals;
}

ChordList& chordTable() {
	static ChordList chordList; // the named-chord table, built once
	return chordList;
}

// An exact name from `rootPc`: the chord table, then the named-only chords.
bool exactFrom(const uint8_t* notes, int32_t count, int32_t rootPc, NamedChord& out) {
	const NoteSet intervals = intervalsFrom(notes, count, rootPc);
	ChordList& table = chordTable();
	for (int32_t c = 0; c < kUniqueChords; c++) {
		const Chord& chord = table.chords[c];
		if (chord.name[0] != '\0' && intervals == chord.intervalSet) {
			out.root = rootPc;
			out.code = chord.name;
			out.intervals = chord.intervalSet;
			return true;
		}
	}
	for (const NamedOnlyChord& chord : kNamedOnlyChords) {
		if (intervals == chord.intervals) {
			out.root = rootPc;
			out.code = chord.name;
			out.intervals = chord.intervals;
			return true;
		}
	}
	return false;
}

// THE NOTES A PLAYER LEAVES OUT, and still plays the chord (Chroma 1.3.139). The fifth of anything that has one; the
// 9th and 11th of a 13th chord; the 3rd and 9th of an 11th chord. Anything else left out is a different chord, and
// is not named as this one - 1.3.137 allowed any one or two notes, and named C Db Eb "C-7b5b9" (Gb and Bb absent).
bool onlyStandardOmissions(const NoteSet& chord, const NoteSet& played) {
	if (!played.isSubsetOf(chord)) {
		return false;
	}
	const bool thirteenth = chord.has(MAJ6) && (chord.has(MIN7) || chord.has(MAJ7)) && chord.has(MAJ2);
	const bool eleventh = chord.has(P4) && chord.has(MIN7) && chord.has(MAJ2);
	for (int32_t iv = 1; iv < 12; iv++) {
		if (!chord.has(iv) || played.has(iv)) {
			continue;
		}
		const bool ok =
		    iv == P5 || (thirteenth && (iv == MAJ2 || iv == P4)) || (eleventh && (iv == MAJ3 || iv == MAJ2));
		if (!ok) {
			return false;
		}
	}
	return true;
}

// The chord from `rootPc` holding every note, with only standard omissions, the fewest left out.
bool nearFrom(const uint8_t* notes, int32_t count, int32_t rootPc, NamedChord& out) {
	const NoteSet intervals = intervalsFrom(notes, count, rootPc);
	ChordList& table = chordTable();
	int32_t best = 99;
	auto consider = [&](const char* name, const NoteSet& set) {
		if (name[0] == '\0' || !onlyStandardOmissions(set, intervals)) {
			return;
		}
		const int32_t missing = set.count() - intervals.count();
		if (missing < best) {
			best = missing;
			out.root = rootPc;
			out.code = name;
			out.intervals = set;
		}
	};
	for (int32_t c = 0; c < kUniqueChords; c++) {
		consider(table.chords[c].name, table.chords[c].intervalSet);
	}
	for (const NamedOnlyChord& chord : kNamedOnlyChords) {
		consider(chord.name, chord.intervals);
	}
	return best < 99;
}

// THE ORDER (Chroma 1.3.139), each step proven against tonal by chroma/tools/chord-oracle:
//   1. an exact name from the bass                                   E G B D F    -> Em7b9
//   2. four notes or more: from the bass, standard omissions only    C E G Bb A   -> C13 (no 9th)
//   3. an exact name from another note, as a slash chord             C E A        -> Am/C (not C6 without its 5th)
//   4. from the bass, standard omissions only                        C E Bb       -> C7 (no 5th)
//   5. from another note, standard omissions only, as a slash chord
//   Two different notes are an interval, named from the lower.
bool nameChord(const uint8_t* notes, int32_t count, NamedChord& out) {
	if (count < 2) {
		return false;
	}
	uint8_t bass = notes[0];
	NoteSet present;
	present.clear();
	for (int32_t i = 0; i < count; i++) {
		bass = notes[i] < bass ? notes[i] : bass;
		present.add(notes[i] % 12);
	}
	out.bass = bass % 12;
	const int32_t distinct = present.count();
	if (distinct == 2) {
		int32_t other = out.bass;
		for (int32_t pc = 0; pc < 12; pc++) {
			other = (pc != out.bass && present.has(pc)) ? pc : other;
		}
		static const char* const kIntervalName[12] = {"",    " m2", " M2", " m3", " M3", " P4",
		                                              " TT", "5",   " m6", " M6", " m7", " M7"};
		out.root = out.bass;
		out.code = kIntervalName[((other - out.bass) + 12) % 12];
		out.intervals = intervalsFrom(notes, count, out.bass);
		out.dyad = true;
		return true;
	}
	if (distinct < 2) {
		return false;
	}
	if (exactFrom(notes, count, out.bass, out)) {
		return true;
	}
	if (distinct >= 4 && nearFrom(notes, count, out.bass, out)) {
		return true;
	}
	for (int32_t pc = 0; pc < 12; pc++) {
		if (pc != out.bass && present.has(pc) && exactFrom(notes, count, pc, out)) {
			return true;
		}
	}
	if (nearFrom(notes, count, out.bass, out)) {
		return true;
	}
	for (int32_t pc = 0; pc < 12; pc++) {
		if (pc != out.bass && present.has(pc) && nearFrom(notes, count, pc, out)) {
			return true;
		}
	}
	return false;
}

// HOW A NOTE IS SPELLED IN A KEY (Chroma 1.3.139): the key's own notes as the key spells them; a note off the key the
// way its degree is - flattened from the step above, sharpened when it lies above the seventh. So Bb in C major is
// Bb, bVII, never "A#  bVII" (found by the firmware's own tests: the name and its degree spelled one note two ways).
// `keyRoot` < 0: no key, the caller's lean decides.
struct Spell {
	bool flats;
	int32_t keyRoot = -1;
	NoteSet scale;
};
const char* spellNote(int32_t pc, const Spell& sp) {
	if (sp.keyRoot < 0 || sp.scale.count() != 7) {
		return noteNameInKey(pc, sp.flats);
	}
	const int32_t fromKey = ((pc - sp.keyRoot) % 12 + 12) % 12;
	if (sp.scale.has(fromKey)) {
		return noteNameInKey(pc, sp.flats);
	}
	for (int32_t i = fromKey + 1; i < 12; i++) {
		if (sp.scale.has(i)) {
			return noteNameInKey(pc, true);
		}
	}
	return noteNameInKey(pc, false);
}

// The name, spelled for the display (the Deluge's own labels) or for a host (the standard ones). A chord whose bass
// is not its root says so, "C/G", in both (1.3.139: an inversion read as root position before).
void spellChord(const NamedChord& n, const Spell& sp, bool forHost, char* out) {
	const char* quality = (forHost && !n.dyad) ? standardChordSpelling(n.code) : n.code;
	if (!n.dyad && n.bass != n.root) {
		sprintf(out, "%s%s/%s", spellNote(n.root, sp), quality, spellNote(n.bass, sp));
	}
	else {
		sprintf(out, "%s%s", spellNote(n.root, sp), quality);
	}
}

// THE DEGREE: the step of the key's OWN scale the root is on (Chroma 1.3.139). In A minor C is III; 1.3.138 and
// before counted every scale from the major one and called it bIII. A root outside the scale is flattened from the
// step above it (bVII in C major), or sharpened from the step below when there is none above. Upper case for a major
// third (and for a chord with no third), lower case for a minor one. For the display the label follows, as before
// ("iiFULLDIM"); for a host one quality mark - o diminished, h half-diminished, + augmented, spelled as the symbols
// by the bridge - and no extensions ("vii" "o"). Seven-note scales only; empty otherwise, or for a dyad that is not a
// third or a fifth.
void chordDegree(const NamedChord& n, uint8_t keyRootPc, NoteSet scale, bool forHost, char* out) {
	out[0] = '\0';
	uint8_t steps[12];
	int32_t stepCount = 0;
	for (int32_t i = 0; i < 12; i++) {
		if (scale.has(i)) {
			steps[stepCount++] = i;
		}
	}
	if (stepCount != 7) {
		return;
	}
	const NoteSet& iv = n.intervals;
	if (n.dyad && !iv.has(MIN3) && !iv.has(MAJ3) && !iv.has(P5)) {
		return;
	}
	static const char* const kRoman[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
	const int32_t fromKey = ((n.root - keyRootPc) % 12 + 12) % 12;
	char acc = 0;
	int32_t step = -1;
	for (int32_t d = 0; d < 7; d++) {
		step = steps[d] == fromKey ? d : step;
	}
	if (step < 0 && n.dyad) {
		return; // an interval off the key does not say which borrowed chord it belongs to (1.3.117)
	}
	if (step < 0) {
		for (int32_t d = 0; d < 7 && step < 0; d++) {
			if (steps[d] > fromKey) {
				step = d;
				acc = 'b';
			}
		}
		if (step < 0) {
			step = 6;
			acc = '#';
		}
	}
	const bool lower = iv.has(MIN3) && !iv.has(MAJ3);
	int32_t k = 0;
	if (acc) {
		out[k++] = acc;
	}
	for (const char* r = kRoman[step]; *r; r++) {
		out[k++] = lower ? (char)(*r - 'A' + 'a') : *r;
	}
	out[k] = '\0';
	if (n.dyad) {
		if (iv.has(P5) && !forHost) {
			strcat(out, "5");
		}
		return;
	}
	if (!forHost) {
		strcat(out, n.code);
		return;
	}
	if (iv.has(MIN3) && iv.has(DIM5) && !iv.has(P5)) {
		strcat(out, iv.has(MIN7) ? "h" : "o");
	}
	else if (iv.has(MAJ3) && iv.has(AUG5) && !iv.has(P5)) {
		strcat(out, "+");
	}
}
} // namespace

bool nameChordFromNotes(const uint8_t* notes, int32_t count, char* out, bool preferFlats) {
	NamedChord n;
	if (!nameChord(notes, count, n) || n.dyad) {
		return false; // two notes are an interval, named by describeChordInKey
	}
	spellChord(n, Spell{preferFlats}, false, out);
	return true;
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
	NamedChord n;
	if (!nameChord(notes, count, n)) {
		return false;
	}
	const Spell sp{effectivePreferFlats(keyRootPc, scale), keyRootPc, scale};
	spellChord(n, sp, false, absOut);
	chordDegree(n, keyRootPc, scale, false, romanOut);
	return true;
}

bool nameHeldChord(const uint8_t* notes, int32_t count, uint8_t keyRootPc, NoteSet scale, char* displayOut,
                   char* hostOut) {
	displayOut[0] = hostOut[0] = '\0';
	NoteSet present;
	present.clear();
	for (int32_t i = 0; i < count; i++) {
		present.add(notes[i] % 12);
	}
	if (present.count() < 2) {
		return false;
	}
	const Spell sp{effectivePreferFlats(keyRootPc, scale), keyRootPc, scale};
	NamedChord n;
	if (nameChord(notes, count, n)) {
		char name[40], degree[24];
		for (int32_t host = 0; host < 2; host++) {
			char* out = host ? hostOut : displayOut;
			spellChord(n, sp, host, name);
			chordDegree(n, keyRootPc, scale, host, degree);
			if (degree[0] != '\0') {
				snprintf(out, 64, "%s  %s", name, degree); // the name first, then its place in the key (1.3.138)
			}
			else {
				snprintf(out, 64, "%s", name);
			}
		}
		return true;
	}
	// NEVER NO NAME FOR A HELD CHORD (1.3.136): the notes themselves, lowest first, each pitch once.
	uint8_t sorted[16];
	const int32_t m = count < 16 ? count : 16;
	for (int32_t k = 0; k < m; k++) {
		sorted[k] = notes[k];
	}
	for (int32_t a = 1; a < m; a++) {
		for (int32_t b = a; b > 0 && sorted[b - 1] > sorted[b]; b--) {
			const uint8_t t = sorted[b];
			sorted[b] = sorted[b - 1];
			sorted[b - 1] = t;
		}
	}
	int32_t p = 0;
	uint16_t seen = 0;
	for (int32_t k = 0; k < m && p < 56; k++) {
		const int32_t pc = sorted[k] % 12;
		if (!(seen & (1u << pc))) {
			seen |= (uint16_t)(1u << pc);
			p += snprintf(displayOut + p, (size_t)(64 - p), "%s%s", p ? " " : "", spellNote(pc, sp));
		}
	}
	strcpy(hostOut, displayOut);
	return p > 0;
}

} // namespace deluge::gui::ui::keyboard
