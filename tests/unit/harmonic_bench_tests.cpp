// The harmonic bench, firmware side.
//
// The Deluge's "brain" functions are pure integer arithmetic mod 12 — no inference, no heuristics.
// Given the same notes and key there is exactly one correct answer, derivable from first principles.
// So they get proved, not auditioned.
//
// harmonic_vectors.h is GENERATED from chroma/spec/harmonic-vectors.json, the same file that drives
// the Companion's bench. One definition of correct, two languages, so the device and the desktop app
// cannot drift apart without a test going red. That drift is not hypothetical: on 2026-08-04 four
// implementations had diverged until 9 of 13 test chords got more than one answer.
//
// Two kinds of check here:
//   VECTORS     the generated spec cases, for the chords the firmware's table can name
//   PROPERTIES  invariants that hold for ALL input and need no name mapping at all, which is where
//               the real breadth comes from — including every one of the 4096 possible chords.

#include "CppUTest/TestHarness.h"
#include "definitions_cxx.hpp"
#include "gui/ui/keyboard/chords.h"
#include "harmonic_vectors.h"
#include "model/scale/note_set.h"
#include <cstdio>
#include <cstring>

using deluge::gui::ui::keyboard::ChromaSpelling;
using deluge::gui::ui::keyboard::effectivePreferFlats;
using deluge::gui::ui::keyboard::gChromaSpelling;
using deluge::gui::ui::keyboard::keyPrefersFlats;
using deluge::gui::ui::keyboard::nameChordFromNotes;
using deluge::gui::ui::keyboard::noteNameInKey;

TEST_GROUP(HarmonicBench){void setup() override{
    // The spelling lean is a synced device-wide param; pin it so tests don't inherit each other's state.
    gChromaSpelling = ChromaSpelling::AUTO;
}
}
;

namespace {
NoteSet scaleFrom(const uint8_t* intervals) {
	NoteSet s;
	s.clear();
	for (int i = 0; i < 7; i++) {
		s.add(intervals[i]);
	}
	return s;
}

// The root a name starts with, as a pitch class, or -1 if it doesn't start with a note name.
// Longest match first so "C#" is never read as "C".
int rootPcOfName(const char* name) {
	static const char* kSharp[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	static const char* kFlat[] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
	int best = -1;
	size_t bestLen = 0;
	for (int pc = 0; pc < 12; pc++) {
		for (const char* cand : {kSharp[pc], kFlat[pc]}) {
			size_t len = strlen(cand);
			if (len > bestLen && strncmp(name, cand, len) == 0) {
				best = pc;
				bestLen = len;
			}
		}
	}
	return best;
}
} // namespace

// ── VECTORS ─────────────────────────────────────────────────────────────────────────────────────

TEST(HarmonicBench, namesEverySpecChordCorrectly) {
	char out[64];
	char msg[160];
	for (int i = 0; i < kChordVectorCount; i++) {
		const HarmonicChordVector& v = kChordVectors[i];
		snprintf(msg, sizeof(msg), "vector %d: notes %d %d %d %d %d (count %d) expected %s", i, v.notes[0], v.notes[1],
		         v.notes[2], v.notes[3], v.notes[4], v.count, v.expectSharp);
		CHECK_TRUE_TEXT(nameChordFromNotes(v.notes, v.count, out, false), msg);
		STRCMP_EQUAL_TEXT(v.expectSharp, out, msg);
	}
}

TEST(HarmonicBench, namesEverySpecChordCorrectlyInFlats) {
	char out[64];
	for (int i = 0; i < kChordVectorCount; i++) {
		const HarmonicChordVector& v = kChordVectors[i];
		CHECK_TRUE_TEXT(nameChordFromNotes(v.notes, v.count, out, true), v.expectFlat);
		STRCMP_EQUAL_TEXT(v.expectFlat, out, v.expectFlat);
	}
}

TEST(HarmonicBench, spellsEveryPitchClass) {
	for (int i = 0; i < kSpellVectorCount; i++) {
		const HarmonicSpellVector& v = kSpellVectors[i];
		STRCMP_EQUAL_TEXT(v.expect, noteNameInKey(v.pc, v.preferFlats), v.expect);
	}
}

TEST(HarmonicBench, keySignatureDecidesSharpsOrFlats) {
	for (int i = 0; i < kLeanVectorCount; i++) {
		const HarmonicLeanVector& v = kLeanVectors[i];
		CHECK_EQUAL(v.expectFlats, keyPrefersFlats(v.keyRoot, scaleFrom(v.scale)));
	}
}

TEST(HarmonicBench, forcedLeanOverridesTheKey) {
	// AUTO defers to the key; FLATS/SHARPS force it. This is a synced param, so a surface that
	// ignores the override spells differently from every other surface.
	NoteSet cMajor = scaleFrom(kLeanVectors[0].scale);
	gChromaSpelling = ChromaSpelling::FLATS;
	CHECK_TRUE(effectivePreferFlats(0, cMajor));
	gChromaSpelling = ChromaSpelling::SHARPS;
	CHECK_FALSE(effectivePreferFlats(0, cMajor));
	gChromaSpelling = ChromaSpelling::AUTO;
	CHECK_EQUAL(keyPrefersFlats(0, cMajor), effectivePreferFlats(0, cMajor));
}

// ── PROPERTIES ──────────────────────────────────────────────────────────────────────────────────
// These hold for ALL input and need no knowledge of the naming format, which is what lets them
// cover the whole domain rather than the subset the table can name.

TEST(HarmonicBench, neverCrashesOnAnyPossibleChord) {
	// The domain is finite: 2^12 pitch-class sets. This is not sampling, it is all of them.
	char out[64];
	for (int mask = 0; mask < 4096; mask++) {
		uint8_t notes[12];
		int32_t n = 0;
		for (int b = 0; b < 12; b++) {
			if (mask & (1 << b)) {
				notes[n++] = 60 + b;
			}
		}
		if (n < 2) {
			continue;
		}
		out[0] = '\0';
		nameChordFromNotes(notes, n, out, false); // must not crash or overrun
	}
}

TEST(HarmonicBench, everyAnswerIsReproducible) {
	char a[64], b[64];
	for (int mask = 0; mask < 4096; mask++) {
		uint8_t notes[12];
		int32_t n = 0;
		for (int i = 0; i < 12; i++) {
			if (mask & (1 << i)) {
				notes[n++] = 60 + i;
			}
		}
		if (n < 2) {
			continue;
		}
		a[0] = b[0] = '\0';
		bool ra = nameChordFromNotes(notes, n, a, false);
		bool rb = nameChordFromNotes(notes, n, b, false);
		CHECK_EQUAL(ra, rb);
		if (ra) {
			STRCMP_EQUAL(a, b);
		}
	}
}

TEST(HarmonicBench, noteOrderIsIrrelevant) {
	char fwd[64], rev[64];
	for (int i = 0; i < kChordVectorCount; i++) {
		const HarmonicChordVector& v = kChordVectors[i];
		uint8_t reversed[8];
		for (int32_t k = 0; k < v.count; k++) {
			reversed[k] = v.notes[v.count - 1 - k];
		}
		CHECK_TRUE(nameChordFromNotes(v.notes, v.count, fwd, false));
		CHECK_TRUE(nameChordFromNotes(reversed, v.count, rev, false));
		STRCMP_EQUAL_TEXT(fwd, rev, "note order changed the answer");
	}
}

TEST(HarmonicBench, transposingAChordTransposesItsRoot) {
	// Move every note up k semitones and the root must move with it; nothing else may change.
	char base[64], moved[64];
	for (int i = 0; i < kChordVectorCount; i++) {
		const HarmonicChordVector& v = kChordVectors[i];
		if (!nameChordFromNotes(v.notes, v.count, base, false)) {
			continue;
		}
		for (int k : {1, 5, 7}) {
			uint8_t up[8];
			for (int32_t j = 0; j < v.count; j++) {
				up[j] = v.notes[j] + k;
			}
			CHECK_TRUE(nameChordFromNotes(up, v.count, moved, false));
			CHECK_EQUAL_TEXT((rootPcOfName(base) + k) % 12, rootPcOfName(moved),
			                 "root did not follow the transposition");
		}
	}
}

TEST(HarmonicBench, octaveSpreadDoesNotChangeTheChord) {
	// Lift every voice except the bass an octave. Same chord, wider voicing.
	char tight[64], wide[64];
	for (int i = 0; i < kChordVectorCount; i++) {
		const HarmonicChordVector& v = kChordVectors[i];
		if (!nameChordFromNotes(v.notes, v.count, tight, false)) {
			continue;
		}
		// find the bass so we keep it put: the bass decides the root
		int32_t bassIdx = 0;
		for (int32_t j = 1; j < v.count; j++) {
			if (v.notes[j] < v.notes[bassIdx]) {
				bassIdx = j;
			}
		}
		uint8_t spread[8];
		for (int32_t j = 0; j < v.count; j++) {
			spread[j] = (j == bassIdx) ? v.notes[j] : v.notes[j] + 12;
		}
		CHECK_TRUE(nameChordFromNotes(spread, v.count, wide, false));
		STRCMP_EQUAL_TEXT(tight, wide, "spreading the voicing changed the chord");
	}
}

TEST(HarmonicBench, theBassDecidesTheRootWhenTemplatesTie) {
	// THE regression. A-C-E-G and C-E-G-A are the same pitch classes; only the bass distinguishes
	// Am7 from C6. An implementation that discards octave information before choosing cannot be
	// right, and that is exactly the bug that shipped in the Companion.
	char out[64];
	const uint8_t am7[] = {57, 60, 64, 67}; // A C E G, A in the bass
	const uint8_t c6[] = {60, 64, 67, 69};  // C E G A, C in the bass
	CHECK_TRUE(nameChordFromNotes(am7, 4, out, false));
	CHECK_EQUAL_TEXT(9, rootPcOfName(out), "A C E G with A in the bass must root on A");
	CHECK_TRUE(nameChordFromNotes(c6, 4, out, false));
	CHECK_EQUAL_TEXT(0, rootPcOfName(out), "C E G A with C in the bass must root on C");
}

TEST(HarmonicBench, rootIsAlwaysANoteThatIsActuallyPlayed) {
	// A root the player isn't holding is never a defensible answer, whatever the shape.
	char out[64];
	for (int mask = 0; mask < 4096; mask++) {
		uint8_t notes[12];
		int32_t n = 0;
		bool present[12] = {false};
		for (int b = 0; b < 12; b++) {
			if (mask & (1 << b)) {
				notes[n++] = 60 + b;
				present[b] = true;
			}
		}
		if (n < 2) {
			continue;
		}
		if (!nameChordFromNotes(notes, n, out, false)) {
			continue; // not naming it is allowed; naming it wrongly is not
		}
		int pc = rootPcOfName(out);
		CHECK_TRUE_TEXT(pc >= 0 && present[pc], out);
	}
}

// KNOWN LIMITATION, asserted so it is a documented fact rather than a surprise.
//
// The firmware matches interval sets EXACTLY, so a voicing with the 5th left out matches nothing and
// cannot be named at all. Those voicings are the deep-house default, and the Companion's engine does
// name them (it matches the largest CONTAINED template). If this test starts failing, the firmware
// gained that ability and the bench should start requiring it.
TEST(HarmonicBench, cannotYetNameFifthOmittedVoicings) {
	char out[64];
	const uint8_t cmaj7no5[] = {60, 64, 71}; // C E B
	const uint8_t cm7no5[] = {60, 63, 70};   // C Eb Bb
	CHECK_FALSE_TEXT(nameChordFromNotes(cmaj7no5, 3, out, false), "C E B is nameable now - update the bench");
	CHECK_FALSE_TEXT(nameChordFromNotes(cm7no5, 3, out, false), "C Eb Bb is nameable now - update the bench");
}

TEST(HarmonicBench, refusesToNameLessThanTwoNotes) {
	char out[64];
	const uint8_t one[] = {60};
	CHECK_FALSE(nameChordFromNotes(one, 1, out, false));
	CHECK_FALSE(nameChordFromNotes(one, 0, out, false));
}

// ── suggestNextChords: "the brain" ──────────────────────────────────────────────────────────────
//
// score(from -> to) = 10 * DH_PULL[from][to] + 8 * (6 - voiceDistance(chord(from), chord(to)))
//
// The DH_PULL weights are a musical OPINION (deep-house modal loops preferred over classical V-I)
// and there is no objective test for an opinion. Everything AROUND them is arithmetic, and that is
// what these check: the structure has to be sound whatever the weights say.
//
// The strongest of these is transposition invariance. Suggestion is defined purely on scale DEGREES,
// so the same degree in any key must produce the same degrees back. If it doesn't, some pitch-class
// arithmetic has leaked into what should be key-independent logic.

using deluge::gui::ui::keyboard::ChordSuggestion;
using deluge::gui::ui::keyboard::suggestNextChords;

namespace {
NoteSet majorScale() {
	return NoteSet({0, 2, 4, 5, 7, 9, 11});
}

// The degrees suggested for `fromDegree` in a given key, in rank order.
void suggestedDegrees(uint8_t keyRoot, int fromDegree, int* degreesOut, int& n) {
	static const uint8_t IV[7] = {0, 2, 4, 5, 7, 9, 11};
	ChordSuggestion s[8];
	uint8_t fromPc = (uint8_t)((keyRoot + IV[fromDegree]) % 12);
	n = suggestNextChords(keyRoot, majorScale(), 7, fromPc, s, 8);
	for (int i = 0; i < n; i++) {
		int deg = -1;
		for (int d = 0; d < 7; d++) {
			if (((keyRoot + IV[d]) % 12) == s[i].rootNote) {
				deg = d;
			}
		}
		degreesOut[i] = deg;
	}
}
} // namespace

TEST(HarmonicBench, suggestNeverProposesTheChordYouAreAlreadyOn) {
	// The pull table's diagonal is zero, but the real guarantee is that the current degree is skipped
	// entirely -- "go to where you already are" is never a suggestion.
	int degs[8], n;
	for (uint8_t key = 0; key < 12; key++) {
		for (int from = 0; from < 7; from++) {
			suggestedDegrees(key, from, degs, n);
			for (int i = 0; i < n; i++) {
				CHECK_TEXT(degs[i] != from, "suggested the chord you are already on");
			}
		}
	}
}

TEST(HarmonicBench, suggestReturnsTheOtherSixDegreesWithNoDuplicates) {
	int degs[8], n;
	for (uint8_t key = 0; key < 12; key++) {
		for (int from = 0; from < 7; from++) {
			suggestedDegrees(key, from, degs, n);
			CHECK_EQUAL(6, n); // seven degrees minus the one you're on
			bool seen[7] = {false};
			for (int i = 0; i < n; i++) {
				CHECK_TRUE(degs[i] >= 0 && degs[i] < 7);
				CHECK_TEXT(!seen[degs[i]], "the same degree was suggested twice");
				seen[degs[i]] = true;
			}
		}
	}
}

TEST(HarmonicBench, suggestionsAreRankedBestFirst) {
	// The caller shows the top few, so the ORDER is the whole feature. Recompute the documented score
	// independently -- 10*pull + 8*(6 - voiceDistance) over diatonic 7ths built by stacking scale
	// thirds -- and require it never increases down the list.
	static const int8_t DH_PULL[7][7] = {
	    {0, 1, 2, 4, 3, 5, 5}, {3, 0, 1, 2, 5, 1, 2}, {2, 1, 0, 2, 1, 4, 3}, {4, 2, 1, 0, 1, 3, 5},
	    {5, 1, 1, 2, 0, 4, 2}, {3, 2, 1, 4, 1, 0, 5}, {5, 1, 1, 3, 2, 4, 0},
	};
	static const uint8_t IV[7] = {0, 2, 4, 5, 7, 9, 11};

	auto chordOf = [&](uint8_t key, int deg, uint8_t* pcs) {
		for (int k = 0; k < 4; k++) {
			pcs[k] = (uint8_t)((key + IV[(deg + 2 * k) % 7]) % 12);
		}
	};
	auto voiceDist = [](const uint8_t* a, const uint8_t* b) {
		int total = 0;
		for (int i = 0; i < 4; i++) {
			int best = 12;
			for (int j = 0; j < 4; j++) {
				int d = ((a[i] - b[j]) + 12) % 12;
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
	};

	for (uint8_t key = 0; key < 12; key++) {
		for (int from = 0; from < 7; from++) {
			int degs[8], n;
			suggestedDegrees(key, from, degs, n);
			uint8_t cur[4];
			chordOf(key, from, cur);
			int prev = 1 << 30;
			for (int i = 0; i < n; i++) {
				uint8_t to[4];
				chordOf(key, degs[i], to);
				int score = 10 * DH_PULL[from][degs[i]] + 8 * (6 - voiceDist(cur, to));
				CHECK_TEXT(score <= prev, "suggestions are not in descending score order");
				prev = score;
			}
		}
	}
}

TEST(HarmonicBench, theSameDegreeSuggestsTheSameDegreesInEveryKey) {
	// Suggestion is defined on DEGREES, so the key must not change the answer -- only which pitch
	// classes those degrees land on. This is the property that catches pitch-class arithmetic leaking
	// into degree logic.
	for (int from = 0; from < 7; from++) {
		int refDegs[8], refN;
		suggestedDegrees(0, from, refDegs, refN); // C major as the reference
		for (uint8_t key = 1; key < 12; key++) {
			int degs[8], n;
			suggestedDegrees(key, from, degs, n);
			CHECK_EQUAL(refN, n);
			for (int i = 0; i < n; i++) {
				CHECK_EQUAL_TEXT(refDegs[i], degs[i], "the key changed which degrees were suggested");
			}
		}
	}
}

TEST(HarmonicBench, suggestRespectsMaxOut) {
	// It writes into a caller-supplied buffer. Overrunning it would be a memory bug on the device.
	ChordSuggestion s[8];
	for (int32_t maxOut = 0; maxOut <= 8; maxOut++) {
		for (int i = 0; i < 8; i++) {
			s[i].rootNote = 0xEE; // sentinel
		}
		int n = suggestNextChords(0, majorScale(), 7, 0, s, maxOut);
		CHECK_TRUE(n <= maxOut);
		for (int i = n; i < 8; i++) {
			CHECK_EQUAL_TEXT(0xEE, s[i].rootNote, "wrote past the count it returned");
		}
	}
}

TEST(HarmonicBench, suggestRefusesWhatItCannotAnswer) {
	ChordSuggestion s[8];
	// The pull table is only defined for 7-note scales.
	CHECK_EQUAL(0, suggestNextChords(0, NoteSet({0, 4, 7}), 3, 0, s, 8));
	// A current root that isn't a degree of the key has no row in the table.
	CHECK_EQUAL(0, suggestNextChords(0, majorScale(), 7, 1, s, 8)); // C# is not in C major
}

TEST(HarmonicBench, everySuggestedRootIsDiatonic) {
	ChordSuggestion s[8];
	static const uint8_t IV[7] = {0, 2, 4, 5, 7, 9, 11};
	for (uint8_t key = 0; key < 12; key++) {
		for (int from = 0; from < 7; from++) {
			int n = suggestNextChords(key, majorScale(), 7, (uint8_t)((key + IV[from]) % 12), s, 8);
			for (int i = 0; i < n; i++) {
				bool inKey = false;
				for (int d = 0; d < 7; d++) {
					if (((key + IV[d]) % 12) == s[i].rootNote) {
						inKey = true;
					}
				}
				CHECK_TRUE_TEXT(inKey, "suggested a chord outside the key");
			}
		}
	}
}

TEST(HarmonicBench, suggestIsDeterministic) {
	ChordSuggestion a[8], b[8];
	for (int from = 0; from < 7; from++) {
		static const uint8_t IV[7] = {0, 2, 4, 5, 7, 9, 11};
		int na = suggestNextChords(0, majorScale(), 7, IV[from], a, 8);
		int nb = suggestNextChords(0, majorScale(), 7, IV[from], b, 8);
		CHECK_EQUAL(na, nb);
		for (int i = 0; i < na; i++) {
			CHECK_EQUAL(a[i].rootNote, b[i].rootNote);
			CHECK_EQUAL(a[i].chordNo, b[i].chordNo);
		}
	}
}

// ── describeChordInKey: the roman numeral the Deluge broadcasts ─────────────────────────────────
//
// This is what the Companion's Voicing tab displays, so a wrong roman here is wrong on every screen.
// Spec section 5: a roman numeral asserts a FUNCTION WITHIN THE KEY, so it is only earned when every
// tone of the chord is diatonic. Root-in-key is not enough.

using deluge::gui::ui::keyboard::describeChordInKey;

TEST(HarmonicBench, romanOnlyForFullyDiatonicChords) {
	char abs[64], roman[64];
	NoteSet cMajor({0, 2, 4, 5, 7, 9, 11});

	// Dm7 in C major: D F A C, every tone diatonic. This IS the ii chord.
	const uint8_t dm7[] = {62, 65, 69, 72};
	CHECK_TRUE(describeChordInKey(dm7, 4, 0, cMajor, abs, roman));
	CHECK_TEXT(roman[0] != '\0', "a fully diatonic chord should get a roman");

	// D7 in C major: D F# A C. The ROOT is the 2nd degree, but F# is not in the key, so this is a
	// borrowed chord and emphatically not the diatonic ii. Naming it "ii7" claims a function it
	// does not have.
	const uint8_t d7[] = {62, 66, 69, 72};
	describeChordInKey(d7, 4, 0, cMajor, abs, roman);
	STRCMP_EQUAL_TEXT("", roman, "a borrowed chord must not be given a roman numeral");
}

TEST(HarmonicBench, romanMatchesTheCompanionForEveryDiatonicTriad) {
	// The device and the app must agree; two screens disagreeing about the same chord is the whole
	// class of bug this bench exists to stop.
	char abs[64], roman[64];
	static const uint8_t MAJ[7] = {0, 2, 4, 5, 7, 9, 11};
	static const char* const EXPECT[7] = {"I", "ii", "iii", "IV", "V", "vi", "vii"};
	NoteSet cMajor({0, 2, 4, 5, 7, 9, 11});

	for (int d = 0; d < 7; d++) {
		uint8_t notes[3];
		for (int k = 0; k < 3; k++) {
			notes[k] = (uint8_t)(60 + MAJ[(d + 2 * k) % 7] + (((d + 2 * k) >= 7) ? 12 : 0));
		}
		CHECK_TRUE(describeChordInKey(notes, 3, 0, cMajor, abs, roman));
		// Compare the numeral only, ignoring the quality suffix the firmware appends.
		char numeral[8];
		int n = 0;
		for (int i = 0; roman[i] && n < 7; i++) {
			char c = roman[i];
			if (c == 'I' || c == 'V' || c == 'i' || c == 'v' || c == 'b' || c == '#') {
				numeral[n++] = c;
			}
			else {
				break;
			}
		}
		numeral[n] = '\0';
		STRCMP_EQUAL_TEXT(EXPECT[d], numeral, abs);
	}
}
