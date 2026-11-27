#include "CppUTest/TestHarness.h"
#include "definitions_cxx.hpp"
#include "gui/ui/keyboard/chords.h"
#include <string>
#include <vector>

using deluge::gui::ui::keyboard::ChordList;
using deluge::gui::ui::keyboard::ChordSelection;
using deluge::gui::ui::keyboard::describeChordInKey;
using deluge::gui::ui::keyboard::MAJ3;
using deluge::gui::ui::keyboard::nameChordFromNotes;
using deluge::gui::ui::keyboard::NONE;
using deluge::gui::ui::keyboard::P5;
using deluge::gui::ui::keyboard::resolveChordNotes;
using deluge::gui::ui::keyboard::ROOT;
using deluge::gui::ui::keyboard::Voicing;

TEST_GROUP(ChordTests) {
	ChordList chordList;
};

namespace {
// A C major triad voicing with all unused slots explicitly NONE.
Voicing majorTriadVoicing() {
	Voicing v{};
	v.offsets[0] = ROOT;
	v.offsets[1] = MAJ3;
	v.offsets[2] = P5;
	for (int i = 3; i < kMaxChordKeyboardSize; i++) {
		v.offsets[i] = NONE;
	}
	return v;
}
} // namespace

TEST(ChordTests, resolveChordNotesMajorTriad) {
	ChordSelection sel{.rootNote = 60, .voicing = majorTriadVoicing()};
	int16_t notes[kMaxChordKeyboardSize];
	uint8_t n = resolveChordNotes(sel, notes, kMaxChordKeyboardSize);

	CHECK_EQUAL(3, n);
	CHECK_EQUAL(60, notes[0]); // C
	CHECK_EQUAL(64, notes[1]); // E
	CHECK_EQUAL(67, notes[2]); // G
}

TEST(ChordTests, resolveChordNotesSkipsNoneHoles) {
	// A NONE in the middle of the offsets array must be skipped, not terminate the chord.
	Voicing v{};
	v.offsets[0] = ROOT;
	v.offsets[1] = NONE;
	v.offsets[2] = P5;
	for (int i = 3; i < kMaxChordKeyboardSize; i++) {
		v.offsets[i] = NONE;
	}
	ChordSelection sel{.rootNote = 48, .voicing = v};
	int16_t notes[kMaxChordKeyboardSize];
	uint8_t n = resolveChordNotes(sel, notes, kMaxChordKeyboardSize);

	CHECK_EQUAL(2, n);
	CHECK_EQUAL(48, notes[0]);
	CHECK_EQUAL(55, notes[1]);
}

TEST(ChordTests, resolveChordNotesRespectsMaxNotes) {
	ChordSelection sel{.rootNote = 60, .voicing = majorTriadVoicing()};
	int16_t notes[kMaxChordKeyboardSize];
	uint8_t n = resolveChordNotes(sel, notes, 2); // cap below the 3 available

	CHECK_EQUAL(2, n);
	CHECK_EQUAL(60, notes[0]);
	CHECK_EQUAL(64, notes[1]);
}

TEST(ChordTests, getChordBoundsCheck) {
	// For each chord, iterate through all voicing offsets and then some
	for (int chordNo = 0; chordNo < kUniqueChords; chordNo++) {
		// iterate through -5 to twice the number of possible voicings to check bounds
		for (int voicingOffset = -5; voicingOffset < 2 * kUniqueVoicings; voicingOffset++) {
			// Set the voicing offset, even if it's out of bounds
			chordList.voicingOffset[chordNo] = voicingOffset;
			// Get the voicing, should return between voicing 0 and last valid voicing
			Voicing voicing = chordList.getChordVoicing(chordNo);

			// Check that the voicing is a valid voicing (at least one offset is not NONE)
			bool valid = false;
			for (int i = 0; i < kMaxChordKeyboardSize; i++) {
				int32_t offset = voicing.offsets[i];
				if (offset != NONE) {
					valid = true;
				}
			}
			CHECK(valid);
		}
	}
}

TEST(ChordTests, adjustChordRowOffsetBoundsCheck) {
	// Test that the chord row offset is bounded by 0 and kOffScreenChords
	// Test the lower bound
	chordList.chordRowOffset = 0;
	chordList.adjustChordRowOffset(-1);
	CHECK_EQUAL(0, chordList.chordRowOffset);

	// Test the upper bound
	chordList.chordRowOffset = kOffScreenChords;
	chordList.adjustChordRowOffset(1);
	CHECK_EQUAL(kOffScreenChords, chordList.chordRowOffset);

	// Test that doing a 0 offset doesn't change the value
	chordList.chordRowOffset = kUniqueChords / 2;
	chordList.adjustChordRowOffset(0);
	CHECK_EQUAL(kUniqueChords / 2, chordList.chordRowOffset);

	// Test that a 1 offset increases the value by 1
	chordList.chordRowOffset = 0;
	chordList.adjustChordRowOffset(1);
	CHECK_EQUAL(1, chordList.chordRowOffset);

	// Test that a -1 offset decreases the value by 1
	chordList.chordRowOffset = kOffScreenChords;
	chordList.adjustChordRowOffset(-1);
	CHECK_EQUAL(kOffScreenChords - 1, chordList.chordRowOffset);
}

TEST(ChordTests, adjustVoicingOffsetBoundsCheck) {
	for (int chordNo = 0; chordNo < kUniqueChords; chordNo++) {
		// Test that the voicing offset is bounded by 0 and kUniqueVoicings - 1
		// Test the lower bound
		chordList.voicingOffset[chordNo] = 0;
		chordList.adjustVoicingOffset(chordNo, -1);
		CHECK_EQUAL(0, chordList.voicingOffset[chordNo]);

		// Test the upper bound
		chordList.voicingOffset[chordNo] = kUniqueVoicings - 1;
		chordList.adjustVoicingOffset(chordNo, 1);
		CHECK_EQUAL(kUniqueVoicings - 1, chordList.voicingOffset[chordNo]);

		// Test that doing a 0 offset doesn't change the value
		chordList.voicingOffset[chordNo] = kUniqueVoicings / 2;
		chordList.adjustVoicingOffset(chordNo, 0);
		CHECK_EQUAL(kUniqueVoicings / 2, chordList.voicingOffset[chordNo]);

		// Test that a 1 offset increases the value by 1
		chordList.voicingOffset[chordNo] = 0;
		chordList.adjustVoicingOffset(chordNo, 1);
		CHECK_EQUAL(1, chordList.voicingOffset[chordNo]);

		// Test that a -1 offset decreases the value by 1
		chordList.voicingOffset[chordNo] = kUniqueVoicings - 1;
		chordList.adjustVoicingOffset(chordNo, -1);
		CHECK_EQUAL(kUniqueVoicings - 2, chordList.voicingOffset[chordNo]);
	}
}

// DYADS (1.3.117). Cody, 2026-11-24, holding A6 C7 C8 on the keyboard: "shows nothing, include dyads".
namespace {
const NoteSet kCMajor({0, 2, 4, 5, 7, 9, 11});
struct Named {
	bool found;
	std::string abs;
	std::string roman;
};
Named name(std::initializer_list<uint8_t> held, NoteSet scale = kCMajor, uint8_t keyRoot = 0) {
	std::vector<uint8_t> notes(held);
	char abs[40] = "unchanged";
	char roman[16] = "unchanged";
	const bool found = describeChordInKey(notes.data(), (int32_t)notes.size(), keyRoot, scale, abs, roman);
	return {found, abs, roman};
}
} // namespace

TEST(ChordTests, theReportedCaseAMinorThirdWithAnOctaveDoubled) {
	// A6 C7 C8, middle C (60) being C3: A6 = 93, C7 = 96, C8 = 108.
	const Named n = name({93, 96, 108});
	CHECK_TRUE(n.found);
	STRCMP_EQUAL("A m3", n.abs.c_str());
	STRCMP_EQUAL("vi", n.roman.c_str());
}

TEST(ChordTests, aDyadIsNamedFromItsLowestNoteAsPlayed) {
	// C under A is a major sixth, not an inverted A minor: an interval does not say which triad it belongs to.
	const Named n = name({60, 69});
	STRCMP_EQUAL("C M6", n.abs.c_str());
	STRCMP_EQUAL("", n.roman.c_str());
}

TEST(ChordTests, aFifthIsAPowerChordWithItsNumeral) {
	const Named n = name({55, 62}); // G3 D4
	STRCMP_EQUAL("G5", n.abs.c_str());
	STRCMP_EQUAL("V5", n.roman.c_str());
}

TEST(ChordTests, aMajorThirdIsUpperCaseAndAMinorThirdLower) {
	STRCMP_EQUAL("I", name({60, 64}).roman.c_str());   // C E
	STRCMP_EQUAL("ii", name({62, 65}).roman.c_str());  // D F
	STRCMP_EQUAL("E M3", name({64, 68}).abs.c_str());  // E G#: not in the key
	STRCMP_EQUAL("III", name({64, 68}).roman.c_str()); // E is still a degree of C major
}

TEST(ChordTests, everyIntervalHasAName) {
	const char* expected[12] = {nullptr, "C m2", "C M2", "C m3", "C M3", "C P4",
	                            "C TT",  "C5",   "C m6", "C M6", "C m7", "C M7"};
	for (uint8_t i = 1; i < 12; i++) {
		const Named n = name({60, (uint8_t)(60 + i)});
		CHECK_TRUE(n.found);
		STRCMP_EQUAL(expected[i], n.abs.c_str());
	}
}

TEST(ChordTests, theSameNoteInTwoOctavesIsNotADyad) {
	CHECK_FALSE(name({57, 69}).found);
}

TEST(ChordTests, aTriadStillNamesAsItsChordAndNotAsADyad) {
	const Named n = name({57, 60, 64}); // A C E
	CHECK_TRUE(n.found);
	CHECK_TRUE(n.abs.find("m3") == std::string::npos);
	CHECK_TRUE(n.abs.rfind("A", 0) == 0);
}

TEST(ChordTests, aDyadOnANoteOutsideTheKeyGetsNoNumeral) {
	const Named n = name({61, 64}); // C# E in C major
	STRCMP_EQUAL("", n.roman.c_str());
	CHECK_TRUE(n.found);
}

// THE NEAR MATCH (1.3.137). Cody, 2026-11-26, on a chord-memory slot of E G B D F: "it shows a chord shape but no
// name???" - no chord in the table is exactly those five pitches. The chord that holds them all with the fewest of its
// own left out is G13 (missing its 9th), and E is in the bass.
namespace {
std::string nameOf(std::vector<uint8_t> notes) {
	char out[48] = {0};
	if (!nameChordFromNotes(notes.data(), (int32_t)notes.size(), out, false)) {
		return "";
	}
	return out;
}
} // namespace

TEST(ChordTests, theReportedVoicingEGBDFIsEm7b9) {
	// Every note accounted for from the bass: minor 3rd, 5th, minor 7th, flat 9th. Named by the named-only list, before
	// a near match (G13/E, missing its 9th) is ever tried.
	STRCMP_EQUAL("E-7\x81"
	             "9",
	             nameOf({52, 55, 59, 62, 65}).c_str()); // E3 G3 B3 D4 F4
}

// (A first-inversion triad, E G C, reads "EAUG": kAug's interval set has a MINOR third - the known table bug with a
// fix branch since 2026-09. Not this change's; fixing kAug is the next step, and a test for E G C -> CM goes with it.)

TEST(ChordTests, anExactNameFromAnotherNoteIsASlashChord) {
	// G B D F Ab is G7b9; with B in the bass it is G7b9/B.
	STRCMP_EQUAL("G7\x81"
	             "9/B",
	             nameOf({59, 62, 65, 67, 68}).c_str());
}

TEST(ChordTests, anExactMatchStillWinsOverANearOne) {
	STRCMP_EQUAL("CM", nameOf({48, 52, 55}).c_str());      // C E G
	STRCMP_EQUAL("A-7", nameOf({57, 60, 64, 67}).c_str()); // A C E G
}

TEST(ChordTests, aNearMatchKeepsTheBassAsRootWhereItCan) {
	// C E G Bb D F A minus the 11th and 9th: C E G Bb A is a 13 from C, left out two - and C is the bass.
	STRCMP_EQUAL("C13", nameOf({48, 52, 55, 58, 57}).c_str());
}

TEST(ChordTests, twoNotesAreNotForcedIntoAChord) {
	STRCMP_EQUAL("", nameOf({48, 51}).c_str());
}

// THE CORPUS FOR THE INDEPENDENT CHECK (Chroma 1.3.139). Cody, 2026-11-27: "I do NOT trust our calculator and naming
// tools, I want real tests and ways to validate". Does nothing unless CHORD_CORPUS_OUT names a file; then it writes,
// one JSON line per case, what the firmware's own namers say for every set of 2-6 pitch classes from every one of its
// notes as the bass, in C major and in A minor. chroma/tools/chord-oracle reads it and checks every name against an
// independent reference (tonal): the notes a name means must be the notes that were played.
#include <cstdio>
#include <cstdlib>
namespace {
void jsonString(FILE* f, const char* s) {
	fputc('"', f);
	for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			fprintf(f, "\\%c", *p);
		}
		else if (*p < 0x20 || *p >= 0x7f) {
			fprintf(f, "\\u%04x", *p);
		}
		else {
			fputc(*p, f);
		}
	}
	fputc('"', f);
}
} // namespace

TEST(ChordTests, writesTheCorpusForTheIndependentCheckWhenAsked) {
	const char* path = std::getenv("CHORD_CORPUS_OUT");
	if (path == nullptr || path[0] == '\0') {
		return;
	}
	FILE* f = std::fopen(path, "w");
	CHECK(f != nullptr);
	struct Key {
		const char* name;
		uint8_t root;
		NoteSet scale;
	};
	const Key keys[] = {{"C major", 0, NoteSet({0, 2, 4, 5, 7, 9, 11})},
	                    {"A minor", 9, NoteSet({0, 2, 3, 5, 7, 8, 10})}};
	int32_t cases = 0;
	for (uint32_t bits = 1; bits < 4096; bits++) {
		const int32_t size = __builtin_popcount(bits);
		if (size < 2 || size > 6) {
			continue;
		}
		for (int32_t bassPc = 0; bassPc < 12; bassPc++) {
			if (!((bits >> bassPc) & 1)) {
				continue;
			}
			// The bass in octave 3, every other note once in the octave above it.
			uint8_t notes[6];
			int32_t n = 0;
			notes[n++] = (uint8_t)(48 + bassPc);
			for (int32_t pc = 0; pc < 12; pc++) {
				if (pc != bassPc && ((bits >> pc) & 1)) {
					notes[n++] = (uint8_t)(60 + pc);
				}
			}
			char general[64] = "";
			const bool g = nameChordFromNotes(notes, n, general, true);
			fprintf(f, "{\"notes\":[");
			for (int32_t k = 0; k < n; k++) {
				fprintf(f, "%s%d", k ? "," : "", notes[k]);
			}
			fprintf(f, "],\"general\":");
			if (g) {
				jsonString(f, general);
			}
			else {
				fprintf(f, "null");
			}
			fprintf(f, ",\"inKey\":{");
			for (int32_t kk = 0; kk < 2; kk++) {
				char abs[64] = "", roman[16] = "";
				const bool ok = describeChordInKey(notes, n, keys[kk].root, keys[kk].scale, abs, roman);
				fprintf(f, "%s\"%s\":", kk ? "," : "", keys[kk].name);
				if (ok) {
					fprintf(f, "{\"name\":");
					jsonString(f, abs);
					fprintf(f, ",\"degree\":");
					jsonString(f, roman);
					fprintf(f, "}");
				}
				else {
					fprintf(f, "null");
				}
			}
			fprintf(f, "}}\n");
			cases++;
		}
	}
	std::fclose(f);
	CHECK(cases > 10000);
}
