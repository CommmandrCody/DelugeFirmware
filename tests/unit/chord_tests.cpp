#include "CppUTest/TestHarness.h"
#include "definitions_cxx.hpp"
#include "gui/ui/keyboard/chords.h"

using deluge::gui::ui::keyboard::AUG5;
using deluge::gui::ui::keyboard::ChordList;
using deluge::gui::ui::keyboard::ChordQuality;
using deluge::gui::ui::keyboard::getChordQuality;
using deluge::gui::ui::keyboard::kAug;
using deluge::gui::ui::keyboard::MAJ3;
using deluge::gui::ui::keyboard::MIN3;
using deluge::gui::ui::keyboard::NONE;
using deluge::gui::ui::keyboard::ROOT;
using deluge::gui::ui::keyboard::Voicing;

TEST_GROUP(ChordTests) {
	ChordList chordList;
};

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

// An augmented triad is root + MAJOR third + augmented fifth. kAug said MIN3, and {0,3,8} is not an
// augmented chord at all -- it is the first inversion of a major triad. So a real augmented chord
// matched nothing in the table, and the AUG voicing PLAYED a major triad inversion.
TEST(ChordTests, augIntervalSetIsMajorThirdAndAugmentedFifth) {
	NoteSet expected({ROOT, MAJ3, AUG5});
	CHECK_TRUE(kAug.intervalSet == expected);
	CHECK_FALSE(kAug.intervalSet.has(MIN3));
}

// The file already disagrees with itself: getChordQuality(), a hundred lines above the table in the
// same file, classifies AUGMENTED as MAJ3 + AUG5. kAug's declared set {ROOT, MIN3, AUG5} matches
// none of its branches and falls through to OTHER, so the chord named AUG does not classify as
// augmented in its own firmware. This is the test that catches that.
TEST(ChordTests, augClassifiesAsAugmented) {
	NoteSet intervals = kAug.intervalSet;
	CHECK_EQUAL(static_cast<int>(ChordQuality::AUGMENTED), static_cast<int>(getChordQuality(intervals)));
}

TEST(ChordTests, augVoicingsPlayAMajorThird) {
	// The name is only half of it; the notes the chord actually sounds have to be augmented too.
	// Only the declared voicings are checked: the array is kUniqueVoicings long and chords declare
	// fewer, so the trailing slots are value-initialised to all-zero and sound nothing.
	for (int v = 0; v < kUniqueVoicings; v++) {
		const Voicing& voicing = kAug.voicings[v];
		bool populated = false;
		bool hasMajorThird = false;
		for (int i = 0; i < kMaxChordKeyboardSize; i++) {
			int8_t o = voicing.offsets[i];
			if (o == NONE) {
				continue;
			}
			if (o != 0) {
				populated = true;
			}
			int8_t interval = ((o % 12) + 12) % 12;
			CHECK_TEXT(interval != MIN3, "an augmented voicing must not contain a minor third");
			if (interval == MAJ3) {
				hasMajorThird = true;
			}
		}
		if (populated) {
			CHECK_TRUE(hasMajorThird);
		}
	}
}
