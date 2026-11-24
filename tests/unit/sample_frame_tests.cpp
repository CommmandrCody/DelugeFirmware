#include "CppUTest/TestHarness.h"
#include "hid/sample_frame.h"
#include <cstdint>
#include <cstring>
#include <string>

using namespace deluge::hid;

namespace {
/// A frame is only worth sending if every byte between F0 and F7 is 7-bit. One high byte in the
/// middle ends the message early at the far end and takes the rest of the report with it.
void checkSevenBit(const uint8_t* frame, int32_t length) {
	LONGS_EQUAL(0xf0, frame[0]);
	LONGS_EQUAL(0xf7, frame[length - 1]);
	for (int32_t i = 1; i < length - 1; i++) {
		CHECK_TRUE(frame[i] < 0x80);
	}
}

uint64_t position(const uint8_t* at) {
	uint64_t value = 0;
	for (int32_t i = 0; i < 5; i++) {
		value = (value << 7) | at[i];
	}
	return value;
}

sample_frame::Focus kick() {
	sample_frame::Focus focus;
	focus.present = true;
	focus.source = sample_frame::kKitRow;
	focus.oscMask = 1;
	focus.row = 3;
	focus.ampRelease = 12;
	focus.name = "KICK";
	focus.osc[0].mode = 1;
	focus.osc[0].loaded = true;
	focus.osc[0].rate = 44100;
	focus.osc[0].channels = 2;
	focus.osc[0].length = 88200;
	focus.osc[0].start = 100;
	focus.osc[0].end = 88200;
	focus.osc[0].zones = 1;
	focus.osc[0].path = "SAMPLES/DH/Kick 01.wav";
	return focus;
}
} // namespace

TEST_GROUP(SampleFrame){};

TEST(SampleFrame, nothingInFocusIsElevenBytes) {
	uint8_t frame[sample_frame::kMaxFrame];
	sample_frame::Focus focus;
	const int32_t length = sample_frame::pack(focus, frame);
	LONGS_EQUAL(11, length);
	const uint8_t expected[] = {0xf0, 0x00, 0x21, 0x7b, 0x01, 0x02, 0x55, 1, 0, 0, 0xf7};
	MEMCMP_EQUAL(expected, frame, 11);
}

TEST(SampleFrame, aSoundWithNoSampleOscillatorIsNotPresent) {
	uint8_t frame[sample_frame::kMaxFrame];
	sample_frame::Focus focus = kick();
	focus.oscMask = 0;
	LONGS_EQUAL(11, sample_frame::pack(focus, frame));
	LONGS_EQUAL(0, frame[9]);
}

TEST(SampleFrame, oneSampleLaysOutAsDocumented) {
	uint8_t frame[sample_frame::kMaxFrame];
	const sample_frame::Focus focus = kick();
	const int32_t length = sample_frame::pack(focus, frame);
	const int32_t pathLength = (int32_t)strlen(focus.osc[0].path);
	LONGS_EQUAL(7 + 3 + 8 + sample_frame::kOscRecord + pathLength + 1 + 4 + 1, length);
	checkSevenBit(frame, length);
	LONGS_EQUAL(1, frame[9]);                      // present
	LONGS_EQUAL(sample_frame::kKitRow, frame[10]); // source
	LONGS_EQUAL(1, frame[13]);                     // oscMask
	LONGS_EQUAL(0, frame[14]);                     // state
	LONGS_EQUAL(12, frame[15]);                    // amp release
	LONGS_EQUAL(3, (frame[16] << 7) | frame[17]);  // row
	const uint8_t* osc = frame + 18;
	LONGS_EQUAL(sample_frame::kFileLoaded, osc[0]); // flags: loaded, nothing else
	LONGS_EQUAL(1, osc[1]);                         // ONCE
	LONGS_EQUAL(8192, (osc[2] << 7) | osc[3]);      // transpose 0
	LONGS_EQUAL(64, osc[4]);                        // cents 0
	LONGS_EQUAL(64, osc[5]);                        // speed 0
	LONGS_EQUAL(44100, (osc[6] << 14) | (osc[7] << 7) | osc[8]);
	LONGS_EQUAL(2, osc[9]);
	CHECK_TRUE(position(osc + 10) == 88200); // length
	CHECK_TRUE(position(osc + 15) == 100);   // start
	CHECK_TRUE(position(osc + 20) == 88200); // end
	CHECK_TRUE(position(osc + 25) == 0);     // loop start
	CHECK_TRUE(position(osc + 30) == 0);     // loop end
	LONGS_EQUAL(0, osc[35]);                 // zone
	LONGS_EQUAL(1, osc[36]);                 // zones
	LONGS_EQUAL(sample_frame::fileKey(focus.osc[0].path), (osc[37] << 7) | osc[38]);
	LONGS_EQUAL(pathLength, osc[39]);
	MEMCMP_EQUAL(focus.osc[0].path, osc + 40, pathLength);
	const uint8_t* name = osc + 40 + pathLength;
	LONGS_EQUAL(4, name[0]);
	MEMCMP_EQUAL("KICK", name + 1, 4);
}

TEST(SampleFrame, whatCouldNotBeReadIsSaidNotGuessed) {
	uint8_t frame[sample_frame::kMaxFrame];
	sample_frame::Focus focus = kick();
	focus.ampRelease = -1;
	focus.row = -1;
	focus.osc[0].loaded = false;
	const int32_t length = sample_frame::pack(focus, frame);
	checkSevenBit(frame, length);
	LONGS_EQUAL(0x7f, frame[15]);
	LONGS_EQUAL(0x7f, frame[16]);
	LONGS_EQUAL(0x7f, frame[17]);
	LONGS_EQUAL(0, frame[18] & sample_frame::kFileLoaded);
}

TEST(SampleFrame, loopMarksAreFlaggedOnlyWhenSet) {
	uint8_t frame[sample_frame::kMaxFrame];
	sample_frame::Focus focus = kick();
	focus.osc[0].loopEnd = 4000;
	(void)sample_frame::pack(focus, frame);
	LONGS_EQUAL(sample_frame::kLoopEndSet, frame[18] & (sample_frame::kLoopStartSet | sample_frame::kLoopEndSet));
	focus.osc[0].loopStart = 2000;
	(void)sample_frame::pack(focus, frame);
	LONGS_EQUAL(sample_frame::kLoopStartSet | sample_frame::kLoopEndSet,
	            frame[18] & (sample_frame::kLoopStartSet | sample_frame::kLoopEndSet));
}

TEST(SampleFrame, aLongPathKeepsItsEndAndSaysItWasCut) {
	uint8_t frame[sample_frame::kMaxFrame];
	sample_frame::Focus focus = kick();
	std::string path(200, 'a');
	path += "/Snare.wav";
	focus.osc[0].path = path.c_str();
	const int32_t length = sample_frame::pack(focus, frame);
	checkSevenBit(frame, length);
	const uint8_t* osc = frame + 18;
	CHECK_TRUE((osc[0] & sample_frame::kPathNotExact) != 0);
	LONGS_EQUAL(sample_frame::kMaxPath, osc[39]);
	MEMCMP_EQUAL("/Snare.wav", osc + 40 + sample_frame::kMaxPath - 10, 10);
	// The key is of the WHOLE path, so two long paths with the same ending are still two files.
	std::string other(200, 'b');
	other += "/Snare.wav";
	CHECK_TRUE(sample_frame::fileKey(path.c_str()) != sample_frame::fileKey(other.c_str()));
}

TEST(SampleFrame, aPathThatIsNotPlainTextIsCarriedSafelyAndMarked) {
	uint8_t frame[sample_frame::kMaxFrame];
	sample_frame::Focus focus = kick();
	focus.osc[0].path = "SAMPLES/Caf\xc3\xa9.wav";
	const int32_t length = sample_frame::pack(focus, frame);
	checkSevenBit(frame, length);
	CHECK_TRUE((frame[18] & sample_frame::kPathNotExact) != 0);
}

TEST(SampleFrame, theLargestFrameFitsItsBuffer) {
	uint8_t frame[sample_frame::kMaxFrame + 16];
	memset(frame, 0xee, sizeof(frame));
	sample_frame::Focus focus = kick();
	const std::string path(300, 'p');
	const std::string name(60, 'n');
	focus.oscMask = 3;
	focus.osc[1] = focus.osc[0];
	focus.osc[0].path = path.c_str();
	focus.osc[1].path = path.c_str();
	focus.osc[0].length = ~0ull; // beyond 35 bits: capped, not wrapped into the next byte
	focus.osc[0].transpose = 99999;
	focus.osc[0].rate = 0xffffffffu;
	focus.name = name.c_str();
	const int32_t length = sample_frame::pack(focus, frame);
	LONGS_EQUAL(sample_frame::kMaxFrame, length);
	checkSevenBit(frame, length);
	LONGS_EQUAL(0xee, frame[sample_frame::kMaxFrame]); // nothing written past the end
	CHECK_TRUE(position(frame + 18 + 10) == (1ull << 35) - 1);
}

TEST(SampleFrame, theIdentNamesTheSoundAndNotItsSettings) {
	uint8_t a[sample_frame::kMaxFrame];
	uint8_t b[sample_frame::kMaxFrame];
	sample_frame::Focus focus = kick();
	(void)sample_frame::pack(focus, a);
	focus.osc[0].mode = 0; // CUT instead of ONCE
	focus.osc[0].start = 5000;
	(void)sample_frame::pack(focus, b);
	LONGS_EQUAL((a[11] << 7) | a[12], (b[11] << 7) | b[12]); // same sound
	focus.row = 4;
	(void)sample_frame::pack(focus, b);
	CHECK_TRUE(((a[11] << 7) | a[12]) != ((b[11] << 7) | b[12])); // another row is another sound
}

TEST(SampleFrame, noNumberHoweverWrongCanBreakTheFrame) {
	uint8_t frame[sample_frame::kMaxFrame + 16];
	const int32_t extremes[] = {INT32_MIN, -1, 0, 0xff, INT32_MAX};
	for (int32_t value : extremes) {
		memset(frame, 0xee, sizeof(frame));
		sample_frame::Focus focus = kick();
		focus.can = 0xff;
		focus.source = 0xff;
		focus.state = 0xff;
		focus.oscMask = 0xff;
		focus.ampRelease = value;
		focus.row = value;
		focus.name = "\xff\x80\x7f";
		for (auto& osc : focus.osc) {
			osc = kick().osc[0];
			osc.mode = 0xff;
			osc.transpose = value;
			osc.cents = value;
			osc.speed = value;
			osc.rate = (uint32_t)value;
			osc.channels = 0xff;
			osc.length = (uint64_t)(int64_t)value;
			osc.start = (uint64_t)(int64_t)value;
			osc.end = (uint64_t)(int64_t)value;
			osc.loopStart = (uint64_t)(int64_t)value;
			osc.loopEnd = (uint64_t)(int64_t)value;
			osc.zone = 0xff;
			osc.zones = 0xff;
		}
		const int32_t length = sample_frame::pack(focus, frame);
		CHECK_TRUE(length <= sample_frame::kMaxFrame);
		checkSevenBit(frame, length);
		LONGS_EQUAL(0xee, frame[sample_frame::kMaxFrame]);
	}
}
