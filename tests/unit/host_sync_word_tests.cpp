// THE HOST'S SYNC WORD (Chroma 1.3.144), the format the Deluge Commander plugin and the firmware share
// (src/deluge/processing/engines/host_sync_word.h). Cody, 2026-11-27: "it's a plugin". Every property the firmware
// relies on, proven here: a word survives the trip whole, a damaged one is refused, a word split across render windows
// still reads, and silence or music is never mistaken for one.
#include "CppUTest/TestHarness.h"
#include "processing/engines/host_sync_word.h"
#include <vector>

using namespace deluge::hostsync;

TEST_GROUP(HostSyncWord){};

namespace {
Word sample() {
	Word w;
	w.seq = 0xBEEF;
	w.flags = kFlagPlaying | kFlagEcho;
	w.tempoMilliBpm = 113500;             // 113.5 BPM
	w.position = -((int64_t)3 << 24) - 5; // a negative position (a count-in) must survive too
	return w;
}
} // namespace

TEST(HostSyncWord, aWordEncodedIsReadBackExactly) {
	uint16_t s[kWordSamples];
	encode(sample(), s);
	Decoder d;
	bool done = false;
	for (uint32_t i = 0; i < kWordSamples; i++) {
		done = d.push(s[i]);
		CHECK_EQUAL(i == kWordSamples - 1, done);
	}
	CHECK_EQUAL(0xBEEF, d.word().seq);
	CHECK_EQUAL(kFlagPlaying | kFlagEcho, d.word().flags);
	CHECK_EQUAL(113500u, d.word().tempoMilliBpm);
	CHECK(d.word().position == sample().position);
	CHECK_EQUAL(1u, d.good);
	CHECK_EQUAL(0u, d.bad);
}

TEST(HostSyncWord, anyOneBitWrongIsRefused) {
	uint16_t s[kWordSamples];
	encode(sample(), s);
	for (uint32_t at = 2; at < kWordSamples; at++) { // past the magics: every payload and CRC sample
		for (uint32_t bit = 0; bit < 16; bit++) {
			uint16_t t[kWordSamples];
			for (uint32_t i = 0; i < kWordSamples; i++) {
				t[i] = s[i];
			}
			t[at] ^= (uint16_t)(1u << bit);
			Decoder d;
			bool any = false;
			for (uint32_t i = 0; i < kWordSamples; i++) {
				any = d.push(t[i]) || any;
			}
			CHECK_FALSE(any);
			CHECK_EQUAL(1u, d.bad);
		}
	}
}

TEST(HostSyncWord, wordsAmongSilenceAndSplitAcrossCallsAllRead) {
	std::vector<uint16_t> stream;
	for (uint16_t k = 0; k < 50; k++) {
		for (int32_t z = 0; z < 37; z++) {
			stream.push_back(0); // silence between words, an odd length so words fall anywhere in a window
		}
		Word w = sample();
		w.seq = k;
		uint16_t s[kWordSamples];
		encode(w, s);
		for (uint16_t v : s) {
			stream.push_back(v);
		}
	}
	Decoder d;
	uint16_t expect = 0;
	for (size_t i = 0; i < stream.size(); i += 128) { // render windows of 128
		for (size_t j = i; j < i + 128 && j < stream.size(); j++) {
			if (d.push(stream[j])) {
				CHECK_EQUAL(expect, d.word().seq);
				expect++;
			}
		}
	}
	CHECK_EQUAL(50u, d.good);
	CHECK_EQUAL(0u, d.bad);
}

TEST(HostSyncWord, aMissingSampleCostsThatWordOnlyAndTheNextReads) {
	// The return's drift catch-up can drop one frame from the echo: that word fails, the next one is whole.
	uint16_t a[kWordSamples], b[kWordSamples];
	Word w = sample();
	encode(w, a);
	w.seq++;
	encode(w, b);
	Decoder d;
	for (uint32_t i = 0; i < kWordSamples; i++) {
		if (i != 6) {
			d.push(a[i]);
		}
	}
	d.push(0);
	d.push(0);
	bool got = false;
	for (uint32_t i = 0; i < kWordSamples; i++) {
		got = d.push(b[i]) || got;
	}
	CHECK(got);
	CHECK_EQUAL(0xBEEF + 1, d.word().seq);
}

TEST(HostSyncWord, musicIsNotAWord) {
	// A full-scale ramp through every 16-bit value, twice: it contains both magics, never with a matching CRC after.
	Decoder d;
	for (int32_t pass = 0; pass < 2; pass++) {
		for (uint32_t v = 0; v < 65536; v++) {
			CHECK_FALSE(d.push((uint16_t)v));
		}
	}
	CHECK_EQUAL(0u, d.good);
}
