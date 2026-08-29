/*
 * Copyright © 2020-2023 Synthstrom Audible Limited
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

#include "hid/encoders.h"
#include "OSLikeStuff/timers_interrupts/timers_interrupts.h"
#include <algorithm>

extern "C" {
#include "RZA1/gpio/gpio.h"
#include "RZA1/intc/devdrv_intc.h"
}

namespace deluge::hid::encoders {

TaskID EncoderTaskID = -1;

// ── DetentedEncoder ────────────────────────────────────────────────────────

void DetentedEncoder::applyEdges(int8_t edges) {
	// Two A-pin edges per quadrature cycle, one quadrature cycle per detent click.
	edgeAccumulator += edges;
	int8_t ticks = edgeAccumulator / 2;
	edgeAccumulator -= ticks * 2;
	pos.fetch_add(ticks, std::memory_order_relaxed);
}

int32_t DetentedEncoder::take() {
	return pos.exchange(0, std::memory_order_relaxed);
}

// -- ContinuousEncoder ------------------------------------------------------

int32_t ContinuousEncoder::take() {
	return pos.exchange(0, std::memory_order_relaxed);
}

double ContinuousEncoder::calcNextKnobSpeed(int32_t offset) {
	// - inertia and acceleration control how fast the knob speeds up in horizontal menus
	// - speedScale controls how we go from "raw speed" to speed used as offset multiplier
	// - min and max speed clamp the max effective speed
	//
	// current values have been tuned to be slow enough to feel easy to control, but fast
	// enough to go from 0 to 50 with one fast turn of the encoder. speedScale and min/max
	// could potentially be user-configurable in a small range.
	constexpr double acceleration = 0.1;
	constexpr double inertia = 1.0 - acceleration;
	constexpr double speed_scale = 0.15;
	constexpr double min_speed = 1.0;
	constexpr double max_speed = 3.0;
	constexpr double reset_speed_time_threshold = 0.3;

	// lastOffset and lastEncoderTime keep track of our direction and time
	static int32_t last_offset = 0;
	static double last_encoder_time = 0.0;
	const double time = getSystemTime();

	if (time - last_encoder_time >= reset_speed_time_threshold || offset != last_offset) {
		// too much time passed, or the knob direction changed, reset the speed
		currentKnobSpeed = 0.0;
	}
	else {
		// moving in the same direction, update speed
		currentKnobSpeed = currentKnobSpeed * inertia + 1.0 / (time - last_encoder_time) * acceleration;
	}
	last_encoder_time = time;
	last_offset = offset;
	return std::clamp((currentKnobSpeed * speed_scale), min_speed, max_speed);
}

// ── Named encoder globals ──────────────────────────────────────────────────

DetentedEncoder scrollY;
DetentedEncoder scrollX;
DetentedEncoder tempo;
DetentedEncoder select;
ContinuousEncoder mod0;
ContinuousEncoder mod1;

DetentedEncoder& functionEncoderAt(size_t i) {
	static DetentedEncoder* const table[] = {&scrollY, &scrollX, &tempo, &select};
	return *table[i];
}

ContinuousEncoder& modEncoderAt(size_t i) {
	static ContinuousEncoder* const table[] = {&mod0, &mod1};
	return *table[i];
}

// ── IRQ infrastructure ────────────────────────────────────────────────────

namespace {

struct EncoderIrqEntry {
	/// @brief  A-side pin that's routed via PFC alt-2 to RZ/A1L IRQn.
	uint8_t irqPin;

	/// @brief Companion (B-side) pin, read as plain GPIO inside the ISR.
	uint8_t compPin;
	uint8_t irqNum;

	/// @brief Flip the direction sense when the A/B wiring is swapped.
	bool invert;
};

constexpr size_t kNumEncoders = 6;
constexpr EncoderIrqEntry kEncoderIrqMap[kNumEncoders] = {
    /* scrollY */ {.irqPin = 8, .compPin = 10, .irqNum = 0, .invert = false},
    /* scrollX */ {.irqPin = 11, .compPin = 12, .irqNum = 3, .invert = false},
    /* tempo   */ {.irqPin = 6, .compPin = 7, .irqNum = 2, .invert = true},
    /* select  */ {.irqPin = 3, .compPin = 2, .irqNum = 7, .invert = true},
    /* mod1    */ {.irqPin = 5, .compPin = 4, .irqNum = 1, .invert = false},
    /* mod0    */ {.irqPin = 0, .compPin = 15, .irqNum = 4, .invert = false},
};

// Quadrature transition table, indexed by (previous << 2) | current, where each state is (A << 1) | B.
// A gray-code encoder only ever moves between adjacent states, so the PAIR of states says which way it
// went. Invalid pairs - a double transition, which is what a missed edge looks like - yield 0 rather
// than a guess.
//
// This replaces reading both pins at interrupt time and inferring direction from (a == b). That works
// while the knob is slow, because the pins are still in the state that caused the edge. Turn fast and
// the encoder has moved on before the ISR is serviced, so the comparison reports the WRONG DIRECTION -
// which is why turning a gold knob down quickly sent the parameter up. Comparing against the previous
// state instead is immune to being late: whatever we read, the transition from where we were is still
// the truth.
constexpr int8_t kQuadratureTable[16] = {
    /* 00->00 */ 0,  /* 00->01 */ +1, /* 00->10 */ -1, /* 00->11 */ 0,
    /* 01->00 */ -1, /* 01->01 */ 0,  /* 01->10 */ 0,  /* 01->11 */ +1,
    /* 10->00 */ +1, /* 10->01 */ 0,  /* 10->10 */ 0,  /* 10->11 */ -1,
    /* 11->00 */ 0,  /* 11->01 */ -1, /* 11->10 */ +1, /* 11->11 */ 0,
};

template <size_t IDX>
void encoderIrqHandler(uint32_t /*sense*/) {
	constexpr EncoderIrqEntry m = kEncoderIrqMap[IDX];
	bool a = readInput(1, m.irqPin) != 0;
	bool b = readInput(1, m.compPin) != 0;

	// Per-encoder previous state. Seeded from the first reading so we never emit a phantom edge on boot.
	static uint8_t prevState = 0xff;
	const uint8_t state = static_cast<uint8_t>((a ? 2 : 0) | (b ? 1 : 0));
	if (prevState == 0xff) {
		prevState = state;
		clearIRQInterrupt(m.irqNum);
		return;
	}
	int8_t inc = kQuadratureTable[(prevState << 2) | state];
	prevState = state;
	if (m.invert) {
		inc = -inc;
	}
	if (inc == 0) {
		// No movement, or a transition we cannot trust. Better to drop it than to invent a direction.
		clearIRQInterrupt(m.irqNum);
		return;
	}

	if constexpr (IDX == 0) {
		scrollY.applyEdges(inc);
	}
	else if constexpr (IDX == 1) {
		scrollX.applyEdges(inc);
	}
	else if constexpr (IDX == 2) {
		tempo.applyEdges(inc);
	}
	else if constexpr (IDX == 3) {
		select.applyEdges(inc);
	}
	else if constexpr (IDX == 4) {
		mod1.applyEdges(inc);
	}
	else if constexpr (IDX == 5) {
		mod0.applyEdges(inc);
	}

	// Wake the (otherwise self-blocked) encoder task so it actions this movement.
	unblockTask(EncoderTaskID);

	clearIRQInterrupt(m.irqNum);
}

using IrqHandler = void (*)(uint32_t);
constexpr IrqHandler kEncoderIrqHandlers[kNumEncoders] = {
    &encoderIrqHandler<0>, &encoderIrqHandler<1>, &encoderIrqHandler<2>,
    &encoderIrqHandler<3>, &encoderIrqHandler<4>, &encoderIrqHandler<5>,
};

constexpr uint8_t kEncoderIrqPriority = 14;

void encoderSetPins(uint8_t port, uint8_t pinA, uint8_t pinB) {
	setPinAsInput(port, pinA);
	setPinAsInput(port, pinB);
}

void initInterrupts() {
	for (size_t i = 0; i < kNumEncoders; i++) {
		const auto& m = kEncoderIrqMap[i];

		// Route the A-side pin to its IRQn input via PFC alt-function 2.
		setPinMux(1, m.irqPin, 2);
		enableInputBuffer(1, m.irqPin);

		setPinAsInput(1, m.compPin);

		setIRQInterruptBothEdges(m.irqNum);

		clearIRQInterrupt(m.irqNum);
		setupAndEnableInterrupt(kEncoderIrqHandlers[i], INTC_ID_IRQ0 + m.irqNum, kEncoderIrqPriority);
	}
}

} // namespace

void init() {
	encoderSetPins(1, 11, 12); // scrollX
	encoderSetPins(1, 7, 6);   // tempo
	encoderSetPins(1, 0, 15);  // mod0
	encoderSetPins(1, 5, 4);   // mod1
	encoderSetPins(1, 8, 10);  // scrollY
	encoderSetPins(1, 2, 3);   // select

	initInterrupts();
}

} // namespace deluge::hid::encoders
