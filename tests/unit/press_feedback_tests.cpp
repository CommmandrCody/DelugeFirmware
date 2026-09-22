#include "CppUTest/TestHarness.h"
#include "gui/colour/press_feedback.h"

#include <cstdint>

// SHOWING THAT A PAD IS HELD, in whatever colour the view painted it.
//
// This is the piece that was got wrong four times, and every time the mistake was the same shape:
// a brightness threshold chosen against one layout's palette, which then did nothing on another's.
// Cody, after the third: "column presses only light up the unlit pads - the solid one at the root,
// no change is seen whatsoever." And after the fourth: "how freaking hard is it to dimly lit or
// otherwise let me know I am pressing the octave root note on the piano mode - we have tried to get
// this done how many times?"
//
// So these tests do not check any particular constant. They check the PROPERTY the fix is built on:
// whatever colour goes in, something visibly different comes out. That is the promise being made to
// someone with their hands on the instrument, and it is the only one worth pinning down - the exact
// amounts are taste and will be tuned again.

using deluge::gui::colour::pressedColour;

namespace {

uint32_t gap(RGB a, RGB b) {
	const int32_t dr = (int32_t)a.r - b.r, dg = (int32_t)a.g - b.g, db = (int32_t)a.b - b.b;
	return (uint32_t)((dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db));
}

// Enough total channel movement that a person looking at the grid would see it. Deliberately low:
// the claim under test is "visibly different", not "a specific amount".
constexpr uint32_t kNoticeable = 30;

// The two candidate directions, restated here as the SPEC rather than borrowed from the code. These
// are what the implementation has to choose between, and the choice is the thing worth pinning.
// BRIGHTEN, do not whiten. Restated here as the spec: raise the brightest channel by `amount` of
// its remaining headroom, and carry the others up in proportion so the ratio between them - the hue -
// is unchanged. Pure black has no ratio and no hue, so it alone gets a neutral lift.
//
// The first version added towards 255 on each channel independently, which brightens by destroying
// the colour: a mute pad at (40,0,0) became (187,175,175), white with a red cast. Cody, on the
// audition column: "I thought it muted, it turns white ... it really confuses the user."
RGB lift(RGB c, uint8_t amount) {
	const uint8_t peak = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
	if (peak == 0) {
		const uint8_t v = (uint8_t)((uint32_t)amount * 160 / 255);
		return RGB(v, v, v);
	}
	const uint32_t target = peak + ((255u - peak) * (uint32_t)amount) / 255u;
	return c.transform([peak, target](uint8_t ch) -> uint8_t { return (uint8_t)((uint32_t)ch * target / peak); });
}
RGB dim(RGB c, uint8_t amount) {
	return c.transform([amount](uint8_t ch) -> uint8_t { return (uint8_t)(ch - ((ch * (uint32_t)amount) / 255)); });
}

} // namespace

TEST_GROUP(PressFeedbackTest){};

// THE DECISIVE ONE. Everything else here is satisfied by the old, broken rule too - on a grey pad
// both a lift and a dim move a long way, so "did it move" cannot tell a good implementation from a
// bad one. This can.
//
// The design is "take whichever direction lands FURTHER from where the pad already is". The rule it
// replaced was "peak > 150 dims, otherwise lifts", and a fixed threshold picks the WORSE direction
// for a whole band of colours - around mid-grey the pad has more room downward than up, and the
// threshold sends it up anyway. That is the bug Cody kept seeing and it is invisible to any test
// that only asks whether something changed.
TEST(PressFeedbackTest, alwaysTakesTheDirectionWithMoreRoom) {
	for (int r = 0; r <= 255; r += 5) {
		for (int g = 0; g <= 255; g += 51) {
			for (int b = 0; b <= 255; b += 51) {
				const RGB c = RGB{(uint8_t)r, (uint8_t)g, (uint8_t)b};
				const RGB up = lift(c, 175);
				const RGB down = dim(c, 195);
				const uint32_t best = gap(up, c) > gap(down, c) ? gap(up, c) : gap(down, c);
				CHECK_TEXT(gap(pressedColour(c, 255), c) == best,
				           "press feedback moved the pad the lesser of the two ways");
			}
		}
	}
}

// THE ONE THAT KEPT FAILING ON HARDWARE.
//
// in-key draws its inactive root with adjust(255, 2) - half brightness, so the peak channel tops out
// around 127. Under a "peak > 150 dims, otherwise lifts" rule that landed in the LIFT branch, and
// lifting a pad that is already half lit does almost nothing. The pad stayed put and Cody saw no
// change at all on the one pad he was most likely to press.
TEST(PressFeedbackTest, aHalfLitPadMovesVisibly) {
	const RGB root = RGB{127, 0, 0}; // in-key's inactive root, near enough
	CHECK(gap(pressedColour(root, 255), root) >= kNoticeable);
}

TEST(PressFeedbackTest, everyGreyFromBlackToWhiteMovesVisibly) {
	// The property, swept. No threshold can satisfy this by accident: there is no value of `v` where
	// the function is allowed to sit still.
	for (int v = 0; v <= 255; v++) {
		const RGB c = RGB{(uint8_t)v, (uint8_t)v, (uint8_t)v};
		const uint32_t moved = gap(pressedColour(c, 255), c);
		CHECK_TEXT(moved >= kNoticeable, "a grey pad did not move enough to be seen");
	}
}

TEST(PressFeedbackTest, saturatedColoursMoveToo) {
	// A saturated blue is BRIGHT to the eye even though two channels are near zero. An averaging
	// rule calls it dark and lifts it into pastel; these just have to move.
	const RGB colours[] = {
	    RGB{0, 0, 255},   RGB{0, 255, 0},   RGB{255, 0, 0},  RGB{255, 128, 0},
	    RGB{0, 255, 255}, RGB{255, 0, 255}, RGB{64, 0, 128},
	};
	for (const RGB c : colours) {
		CHECK(gap(pressedColour(c, 255), c) >= kNoticeable);
	}
}

TEST(PressFeedbackTest, aBlackPadCanOnlyLift) {
	// Nowhere to go but up. This is the unlit pad in every layout, and it is the case that always
	// worked - which is exactly why it hid the others: Cody could see SOMETHING happening.
	const RGB lit = pressedColour(RGB{0, 0, 0}, 255);
	CHECK(lit.r > 0);
	CHECK(lit.g > 0);
	CHECK(lit.b > 0);
}

TEST(PressFeedbackTest, aWhitePadCanOnlyDim) {
	const RGB dimmed = pressedColour(RGB{255, 255, 255}, 255);
	CHECK(dimmed.r < 255);
	CHECK(dimmed.g < 255);
	CHECK(dimmed.b < 255);
}

// STRENGTH IS THE OCTAVE RULE. Cody: "the lit keys going up the octave are way too bright - the ones
// we hit should be brightest, the others half as bright."
//
// If every pad of a pitch class moved the same amount, the grid would tell you the pitch class and
// lose the one thing you already knew: which note you actually played.
TEST(PressFeedbackTest, halfStrengthMovesLessThanFull) {
	for (int v = 0; v <= 255; v += 15) {
		const RGB c = RGB{(uint8_t)v, (uint8_t)v, (uint8_t)v};
		const uint32_t full = gap(pressedColour(c, 255), c);
		const uint32_t half = gap(pressedColour(c, 128), c);
		CHECK_TEXT(half < full, "an octave sibling moved as far as the pad under the finger");
	}
}

TEST(PressFeedbackTest, halfStrengthStillMovesAtAll) {
	// Context that cannot be seen is not context. The octaves have to register.
	for (int v = 0; v <= 255; v += 15) {
		const RGB c = RGB{(uint8_t)v, (uint8_t)v, (uint8_t)v};
		CHECK(gap(pressedColour(c, 128), c) > 0);
	}
}

TEST(PressFeedbackTest, zeroStrengthLeavesThePadAlone) {
	// The identity case. An unheld pad must come out of prepareColour exactly as the view painted
	// it, or the mirror and the hardware would disagree about every pad on the grid.
	const RGB colours[] = {RGB{0, 0, 0}, RGB{127, 0, 0}, RGB{255, 255, 255}, RGB{12, 200, 60}};
	for (const RGB c : colours) {
		const RGB out = pressedColour(c, 0);
		CHECK_EQUAL(c.r, out.r);
		CHECK_EQUAL(c.g, out.g);
		CHECK_EQUAL(c.b, out.b);
	}
}

TEST(PressFeedbackTest, neverWrapsAround) {
	// Channel arithmetic on uint8_t. A lift that overflows or a dim that underflows would turn a
	// bright pad black - on the instrument, mid-performance, on the pad being pressed.
	for (int v = 0; v <= 255; v++) {
		for (const uint8_t strength : {(uint8_t)255, (uint8_t)128, (uint8_t)1}) {
			const RGB c = RGB{(uint8_t)v, (uint8_t)v, (uint8_t)v};
			const RGB out = pressedColour(c, strength);
			const bool lifted = out.r >= c.r;
			// Every channel moves the same way as the others: a lift lifts all three, a dim dims all
			// three. A mixed result would mean the hue shifted, and the pad would change colour
			// rather than brightness - a chord tone must stay the chord's colour.
			CHECK_EQUAL(lifted, out.g >= c.g);
			CHECK_EQUAL(lifted, out.b >= c.b);
		}
	}
}

// A PRESSED PAD STILL LOOKS LIKE ITSELF.
//
// The complaint that produced this: pressing a pad in the audition column turned it white, so it
// read as a mode change rather than as a press - "I thought it muted". Two fixes came out of it.
// The sidebar no longer gets feedback at all (it is controls, and their colour already means
// something). And the lift brightens the pad's own colour instead of adding white to it, which is
// what these check.
TEST(PressFeedbackTest, aPressedPadKeepsItsColour) {
	struct {
		uint8_t r, g, b;
	} cases[] = {{40, 0, 0}, {0, 80, 0}, {0, 0, 200}, {120, 30, 0}};
	for (const auto& c : cases) {
		const RGB in = RGB{c.r, c.g, c.b};
		const RGB out = pressedColour(in, 255);
		// A channel that was off stays off. Adding white lit all three and turned a red pad pink.
		if (c.r == 0) {
			CHECK_EQUAL(0, out.r);
		}
		if (c.g == 0) {
			CHECK_EQUAL(0, out.g);
		}
		if (c.b == 0) {
			CHECK_EQUAL(0, out.b);
		}
	}
}

TEST(PressFeedbackTest, aMutePadGoesBrightRedNotWhite) {
	// The exact pad from the report. Under the old rule this was (187,175,175).
	const RGB out = pressedColour(RGB{40, 0, 0}, 255);
	CHECK(out.r > 150);
	CHECK_EQUAL(0, out.g);
	CHECK_EQUAL(0, out.b);
}
