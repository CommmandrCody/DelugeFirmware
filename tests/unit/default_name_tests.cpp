#include "CppUTest/TestHarness.h"
#include "gui/ui/browser/default_name.h"
#include "util/name_compare.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using deluge::gui::browser::familyStemLength;
using deluge::gui::browser::FileListView;
using deluge::gui::browser::nextDefaultName;
using deluge::gui::browser::sameFamily;

namespace {
/// Stands in for a folder on the card. Both queries mirror Browser::surveyNameVariations: a name only counts when its
/// suffix runs to the extension, and only when that extension is one the browser lists. Diverging here would let a
/// test pass on a name the firmware would skip.
class FakeFileList : public FileListView {
public:
	explicit FakeFileList(std::vector<std::string> names) : names_{std::move(names)} {}

	std::string highestNumberedName(char const* prefix) const override {
		std::string highest;
		forEachVariationOf(prefix, [&](std::string const& name, size_t suffixAt, size_t suffixEnd) {
			for (size_t i = suffixAt; i < suffixEnd; i++) {
				if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
					return;
				}
			}
			if (highest.empty() || strcmpspecial(name.c_str(), highest.c_str()) > 0) {
				highest = name;
			}
		});
		return highest;
	}

	uint32_t takenLetterSuffixes(char const* stem) const override {
		uint32_t taken = 0;
		forEachVariationOf(stem, [&](std::string const& name, size_t suffixAt, size_t suffixEnd) {
			if (suffixEnd - suffixAt != 1) {
				return;
			}
			char letter = std::toupper(static_cast<unsigned char>(name[suffixAt]));
			if (letter >= 'A' && letter <= 'Z') {
				taken |= 1U << (letter - 'A');
			}
		});
		return taken;
	}

private:
	/// Calls `consider(name, suffixAt, suffixEnd)` for every listed file whose name is `stem` plus a non-empty
	/// suffix running to the extension.
	template <typename Fn>
	void forEachVariationOf(char const* stem, Fn&& consider) const {
		size_t stemLength = strlen(stem);
		for (auto const& name : names_) {
			size_t dot = name.rfind('.');
			if (dot == std::string::npos || dot <= stemLength || !hasListedExtension(name)) {
				continue;
			}
			if (strncasecmp(name.c_str(), stem, stemLength) != 0) {
				continue;
			}
			consider(name, stemLength, dot);
		}
	}

	/// allowedFileExtensionsXML, as the song and preset browsers set it.
	static bool hasListedExtension(std::string const& name) {
		size_t dot = name.rfind('.');
		if (dot == std::string::npos) {
			return false;
		}
		std::string extension = name.substr(dot + 1);
		return strcasecmp(extension.c_str(), "XML") == 0 || strcasecmp(extension.c_str(), "Json") == 0;
	}

	std::vector<std::string> names_;
};

/// A card holding SONG001.XML .. SONGnnn.XML, as a vanilla card looks.
FakeFileList cardWithSongsUpTo(int32_t highest) {
	std::vector<std::string> names;
	for (int32_t i = 1; i <= highest; i++) {
		char buf[32];
		snprintf(buf, sizeof buf, "SONG%03d.XML", i);
		names.emplace_back(buf);
	}
	return FakeFileList{std::move(names)};
}
} // namespace

TEST_GROUP(DefaultNameTests){void setup() override{shouldInterpretNoteNames = false;
octaveStartsFromA = false;
}
}
;

// --- Issue #1069: the actual regression -------------------------------------------------

TEST(DefaultNameTests, issue1069_defaultNamedSongGetsALetterSuffix) {
	auto files = cardWithSongsUpTo(185);
	CHECK_EQUAL(std::string{"SONG185A"}, nextDefaultName("SONG185", "SONG", files));
}

TEST(DefaultNameTests, issue1069_neverEmitsTheSlotNumberAsASuffix) {
	// The bug produced "185_186": the slot number leaking in through a wrong string offset.
	auto files = cardWithSongsUpTo(185);
	std::string result = nextDefaultName("SONG185", "SONG", files);
	CHECK(result.find("186") == std::string::npos);
	CHECK(result.find('_') == std::string::npos);
}

TEST(DefaultNameTests, letterSuffixesAdvance) {
	FakeFileList files{{"SONG185.XML", "SONG185A.XML", "SONG185B.XML"}};
	CHECK_EQUAL(std::string{"SONG185C"}, nextDefaultName("SONG185", "SONG", files));
	CHECK_EQUAL(std::string{"SONG185C"}, nextDefaultName("SONG185B", "SONG", files));
}

TEST(DefaultNameTests, letterSuffixFillsTheFirstGap) {
	FakeFileList files{{"SONG185.XML", "SONG185A.XML", "SONG185C.XML"}};
	CHECK_EQUAL(std::string{"SONG185B"}, nextDefaultName("SONG185", "SONG", files));
}

TEST(DefaultNameTests, letterSuffixExhaustedReturnsInputUnchanged) {
	std::vector<std::string> names{"SONG185.XML"};
	for (char c = 'A'; c <= 'Z'; c++) {
		names.push_back(std::string{"SONG185"} + c + ".XML");
	}
	FakeFileList files{std::move(names)};
	CHECK_EQUAL(std::string{"SONG185"}, nextDefaultName("SONG185", "SONG", files));
}

// --- Behaviour that must be preserved (this is what OLED does today) ---------------------

TEST(DefaultNameTests, renamedSongGetsANumericSuffix) {
	FakeFileList files{{"MYTRACK.XML"}};
	CHECK_EQUAL(std::string{"MYTRACK 2"}, nextDefaultName("MYTRACK", "SONG", files));
}

TEST(DefaultNameTests, numericSuffixAdvances) {
	FakeFileList files{{"MYTRACK.XML", "MYTRACK 2.XML", "MYTRACK 3.XML"}};
	CHECK_EQUAL(std::string{"MYTRACK 4"}, nextDefaultName("MYTRACK", "SONG", files));
}

// --- Names are derived from the whole family, never by probing candidates one at a time ---

TEST(DefaultNameTests, numericSuffixAdvancesPastEleven) {
	// Guards the reported symptom: saving over "<name> 11" must offer 12, not 2. A caller that tests "<name> 2",
	// "<name> 3", ... in turn gets this wrong the moment the low members fall outside the file list's window; the
	// family's highest member is asked for directly so that there is nothing for a window to hide.
	std::vector<std::string> folder;
	for (int32_t i = 1; i <= 11; i++) {
		folder.push_back("MY PROJECT " + std::to_string(i) + ".XML");
	}
	FakeFileList files{folder};
	CHECK_EQUAL(std::string{"MY PROJECT 12"}, nextDefaultName("MY PROJECT 11", "SONG", files));
}

TEST(DefaultNameTests, letterSuffixAdvancesPastAWholeAlphabetOfVariations) {
	// The same property on the letter path. Testing A, B, C ... one at a time against a windowed view goes wrong
	// once a family runs past the window: a taken letter reads as free and the UI offers to overwrite a real song.
	std::vector<std::string> folder{"SONG185.XML"};
	for (char c = 'A'; c <= 'L'; c++) {
		folder.push_back(std::string{"SONG185"} + c + ".XML");
	}
	FakeFileList files{std::move(folder)};
	CHECK_EQUAL(std::string{"SONG185M"}, nextDefaultName("SONG185", "SONG", files));
}

TEST(DefaultNameTests, letterSuffixesAreCaseInsensitive) {
	// The card may hold either case; they are the same file to FatFS and must be the same variation here.
	FakeFileList files{{"SONG185.XML", "song185a.xml"}};
	CHECK_EQUAL(std::string{"SONG185B"}, nextDefaultName("SONG185", "SONG", files));
}

TEST(DefaultNameTests, aLongerSuffixIsNotALetterVariation) {
	// "SONG185AB" is its own name, not the "A" variation of SONG185, so it must not make A look taken.
	FakeFileList files{{"SONG185.XML", "SONG185AB.XML"}};
	CHECK_EQUAL(std::string{"SONG185A"}, nextDefaultName("SONG185", "SONG", files));
}

TEST(DefaultNameTests, numericSuffixNeverGoesBelowTheNameBeingSavedOver) {
	// If the folder survey cannot answer - a card error, say - the name we are saving over is still known to exist,
	// so its own number floors the result. Without this floor an unanswered survey would send "MY PROJECT 11" to 2.
	class SilentFileList : public FileListView {
		std::string highestNumberedName(char const*) const override { return {}; }
		uint32_t takenLetterSuffixes(char const*) const override { return 0; }
	} files;
	CHECK_EQUAL(std::string{"MY PROJECT 12"}, nextDefaultName("MY PROJECT 11", "SONG", files));
}

TEST(DefaultNameTests, numericSuffixCountsFromTheHighestSiblingNotTheLowestGap) {
	// Gaps are left alone: a deleted "MYTRACK 3" must not be handed back to the next save, which would quietly
	// overwrite it if the user restored it. This also keeps the answer independent of how much of the folder is in
	// the window.
	FakeFileList files{{"MYTRACK.XML", "MYTRACK 2.XML", "MYTRACK 5.XML"}};
	CHECK_EQUAL(std::string{"MYTRACK 6"}, nextDefaultName("MYTRACK 2", "SONG", files));
}

TEST(DefaultNameTests, numericSuffixIgnoresSiblingsThatOnlyLookNumbered) {
	// "MYTRACK NOTES.XML" shares the prefix but carries no number, so it must not be read as one.
	FakeFileList files{{"MYTRACK.XML", "MYTRACK NOTES.XML"}};
	CHECK_EQUAL(std::string{"MYTRACK 2"}, nextDefaultName("MYTRACK", "SONG", files));
}

TEST(DefaultNameTests, numericSuffixIgnoresDateSuffixedNeighbours) {
	// "JAM 2024-01-05.XML" starts with "JAM " and a digit, but its number does not run to the extension, so it is a
	// neighbour and not a member of the "JAM " family. Counted as one it would read as 2024 and send the next save
	// to "JAM 2025". Date-suffixed names are common on real cards.
	FakeFileList files{{"JAM 3.XML", "JAM 2024-01-05.XML"}};
	CHECK_EQUAL(std::string{"JAM 4"}, nextDefaultName("JAM 3", "SONG", files));
}

TEST(DefaultNameTests, numericSuffixIgnoresFilesTheBrowserWouldNotList) {
	// A stray "MYTRACK 99.TXT" is not a song and cannot be overwritten by one, so it must not push the count to 100.
	FakeFileList files{{"MYTRACK 2.XML", "MYTRACK 99.TXT"}};
	CHECK_EQUAL(std::string{"MYTRACK 3"}, nextDefaultName("MYTRACK 2", "SONG", files));
}

TEST(DefaultNameTests, existingUnderscoreDelimiterIsReused) {
	// A name that already carries an "_<number>" suffix keeps the underscore.
	FakeFileList files{{"TRACK_1.XML"}};
	CHECK_EQUAL(std::string{"TRACK_2"}, nextDefaultName("TRACK_1", "SONG", files));
}

TEST(DefaultNameTests, nonNumericNameSharingThePrefixIsNotTreatedAsASlot) {
	// "SONGIDEA" starts with SONG but has no digits - it is just a name.
	FakeFileList files{{"SONGIDEA.XML"}};
	CHECK_EQUAL(std::string{"SONGIDEA 2"}, nextDefaultName("SONGIDEA", "SONG", files));
}

// --- Zero-padded names (vanilla cards write songs 1-99 as SONG001..SONG099) -------------

TEST(DefaultNameTests, zeroPaddedSongGetsALetterSuffix) {
	FakeFileList files{{"SONG001.XML"}};
	CHECK_EQUAL(std::string{"SONG001A"}, nextDefaultName("SONG001", "SONG", files));
}

TEST(DefaultNameTests, zeroPaddedLetterSuffixesAdvance) {
	FakeFileList files{{"SONG001.XML", "SONG001A.XML"}};
	CHECK_EQUAL(std::string{"SONG001B"}, nextDefaultName("SONG001A", "SONG", files));
}

TEST(DefaultNameTests, numberPartSkipsZeroPadding) {
	// getSlot() reads a leading '0' as a whole one-digit slot and then chokes on the next digit, so it rejects "001"
	// outright. Everything feeding it must skip the padding first, or 7SEG scrolls "SONG001" instead of showing "1".
	STRCMP_EQUAL("1", deluge::gui::browser::numberPartOf("SONG001", "SONG"));
	STRCMP_EQUAL("10", deluge::gui::browser::numberPartOf("SONG010", "SONG"));
	STRCMP_EQUAL("185", deluge::gui::browser::numberPartOf("SONG185", "SONG"));
	STRCMP_EQUAL("1A", deluge::gui::browser::numberPartOf("SONG001A", "SONG"));
	// The last digit always survives, so slot 0 is still reachable.
	STRCMP_EQUAL("0", deluge::gui::browser::numberPartOf("SONG000", "SONG"));
	STRCMP_EQUAL("0", deluge::gui::browser::numberPartOf("SONG0", "SONG"));
	// A name that does not carry the prefix has no number part.
	CHECK(deluge::gui::browser::numberPartOf("MYTRACK", "SONG") == nullptr);
}

// --- Extensions -------------------------------------------------------------------------

TEST(DefaultNameTests, aJsonFileAlsoTakesTheName) {
	// The browser lists .XML and .Json alike, and saving picks between them via writeJsonFlag. Probing only .XML would
	// hand back SONG185A when SONG185A.Json already exists on the card.
	FakeFileList files{{"SONG185.XML", "SONG185A.Json"}};
	CHECK_EQUAL(std::string{"SONG185B"}, nextDefaultName("SONG185", "SONG", files));
}

TEST(DefaultNameTests, presetsKeepTheNumericSuffix) {
	// Only songs get letter suffixes. Presets pass an empty slotPrefix, so SYNT005 takes the
	// numeric path - preserving current OLED preset behaviour. See Global Constraints.
	FakeFileList files{{"SYNT005.XML"}};
	CHECK_EQUAL(std::string{"SYNT005 2"}, nextDefaultName("SYNT005", "", files));
}

// --- Which names are versions of one song: the song browser's LAST order -----------------
//
// LAST shows one row per song, its newest version. "Version" must mean exactly what saving a new version
// produces, so these are checked against nextDefaultName() itself, not against a second idea of the rule.

TEST(DefaultNameTests, aSavedVariationIsInTheSameFamilyAsItsOriginal) {
	// Whatever nextDefaultName() makes of a name, the two are one song.
	struct Case {
		char const* name;
		std::vector<std::string> card;
	};
	std::vector<Case> cases = {
	    {"SONG185", {"SONG185.XML"}},     {"SONG185A", {"SONG185.XML", "SONG185A.XML"}},
	    {"MYTRACK", {"MYTRACK.XML"}},     {"MYTRACK 2", {"MYTRACK.XML", "MYTRACK 2.XML"}},
	    {"TRACK_1", {"TRACK_1.XML"}},     {"12_3", {"12_3.XML"}},
	    {"Deep 12_4", {"Deep 12_4.XML"}}, {"Ref 4", {"Ref.XML", "Ref 4.XML"}},
	};
	for (auto const& c : cases) {
		FakeFileList files{c.card};
		std::string next = nextDefaultName(c.name, "SONG", files);
		CHECK_TRUE(next != c.name);
		CHECK_TRUE_TEXT(sameFamily((std::string{c.name} + ".XML").c_str(), (next + ".XML").c_str(), "SONG"), c.name);
	}
}

TEST(DefaultNameTests, theStemLeavesOffExactlyTheVariationSuffix) {
	auto stem = [](char const* name) { return std::string{name, familyStemLength(name, "SONG")}; };
	CHECK_EQUAL(std::string{"SONG185"}, stem("SONG185"));
	CHECK_EQUAL(std::string{"SONG185"}, stem("SONG185A"));
	CHECK_EQUAL(std::string{"song185"},
	            stem("song185b")); // the stem is the name's own characters; comparing ignores case
	CHECK_EQUAL(std::string{"MYTRACK"}, stem("MYTRACK"));
	CHECK_EQUAL(std::string{"MYTRACK"}, stem("MYTRACK 3"));
	CHECK_EQUAL(std::string{"TRACK"}, stem("TRACK_2"));
	CHECK_EQUAL(std::string{"12"}, stem("12_4"));
	CHECK_EQUAL(std::string{"Deep 12"}, stem("Deep 12_4"));
	CHECK_EQUAL(std::string{"A_B"}, stem("A_B 3"));
}

TEST(DefaultNameTests, aNumberThatSimplyEndsANameIsNotAVersion) {
	// Or every default-named song on the card would fold into one row.
	CHECK_FALSE(sameFamily("SONG185.XML", "SONG186.XML", "SONG"));
	CHECK_FALSE(sameFamily("SONG185A.XML", "SONG186A.XML", "SONG"));
	CHECK_EQUAL(size_t{5}, familyStemLength("TR909", "SONG"));
	CHECK_FALSE(sameFamily("TR909.XML", "TR808.XML", "SONG"));
}

TEST(DefaultNameTests, onlyTheSuffixesSavingProducesCount) {
	// A dash is not a delimiter nextDefaultName() uses, and a letter only follows a SONG slot number.
	CHECK_FALSE(sameFamily("Ref.XML", "Ref-3.XML", "SONG"));
	CHECK_FALSE(sameFamily("MIX 2.XML", "MIX 2B.XML", "SONG"));
	CHECK_FALSE(sameFamily("JAM.XML", "JAM 2024-01-05.XML", "SONG"));
	CHECK_FALSE(sameFamily("SONG1850.XML", "SONG185.XML", "SONG")); // four digits is not a slot
	CHECK_FALSE(sameFamily("Ref.XML", "Refs.XML", "SONG"));
}

TEST(DefaultNameTests, familiesIgnoreCaseAndExtension) {
	CHECK_TRUE(sameFamily("ref 2.xml", "REF 9.XML", "SONG"));
	CHECK_TRUE(sameFamily("Ref.XML", "Ref 4.Json", "SONG"));
	CHECK_TRUE(sameFamily("12_3.XML", "12_4.XML", "SONG"));
	CHECK_FALSE(sameFamily("12_3.XML", "13_3.XML", "SONG"));
}

TEST(DefaultNameTests, withoutASlotPrefixOnlyTheNumericFormCounts) {
	// Presets pass an empty prefix, as they do to nextDefaultName().
	CHECK_FALSE(sameFamily("SONG185.XML", "SONG185A.XML", ""));
	CHECK_TRUE(sameFamily("PAD.XML", "PAD 2.XML", ""));
}
