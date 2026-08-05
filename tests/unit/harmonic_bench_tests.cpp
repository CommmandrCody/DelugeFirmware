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
