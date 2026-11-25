/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
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

#pragma once

#include "definitions_cxx.hpp"
#include "gui/ui/keyboard/notes_state.h"
#include "gui/ui/root_ui.h"
#include "gui/ui/ui.h"
#include "hid/button.h"
#include "model/clip/instrument_clip_minder.h"
#include "model/instrument/instrument.h"

class ModelStack;
class Instrument;

namespace deluge::gui::ui::keyboard {

constexpr int32_t kMaxNumKeyboardPadPresses = 10;

class KeyboardScreen final : public RootUI, public InstrumentClipMinder {
public:
	KeyboardScreen();

	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	/// The lowest and highest MIDI notes the grid is currently showing, or false if there is none to
	/// report. Chroma (0x48) sends this so a host can say "C1-C4" instead of guessing.
	///
	/// Asked of the LAYOUT, because only the layout knows how its pads map to notes - that is the
	/// whole difference between isomorphic, in-key and piano. Any copy of that arithmetic elsewhere
	/// would be a second version of the thing drifting away from the first.
	bool noteRange(int32_t& lo, int32_t& hi);
	void soundingPads(uint8_t columns[kDisplayWidth]);
	void playingPads(uint8_t columns[kDisplayWidth]);
	bool padNotes(uint8_t notes[kDisplayHeight][kDisplayWidth]);
	/// The chord being held, named as the display names it; false and "" when there is none. Reads only.
	bool heldChordName(char full[64]);
	bool padVelocities(uint8_t velocities[kDisplayHeight][kDisplayWidth]);

	ActionResult verticalEncoderAction(int32_t offset, bool inCardRoutine) override;
	ActionResult horizontalEncoderAction(int32_t offset) override;
	void selectEncoderAction(int8_t offset) override;

	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth],
	                    bool drawUndefinedArea = false) override;
	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) override;

	void flashDefaultRootNote();
	void openedInBackground();
	void exitAuditionMode();

	uint8_t highlightedNotes[kHighestKeyboardNote] = {0};
	/// A note being held over MIDI right now (1.3.131): highlightedNotes keeps an incoming note at its velocity,
	/// 1-253, while it is held. 254 is a playback preview and 255 a recalled chord - neither is a held note.
	inline bool midiHeld(int32_t n) const {
		return n >= 0 && n < kHighestKeyboardNote && highlightedNotes[n] != 0 && highlightedNotes[n] < 254;
	}
	uint8_t nornsNotes[kHighestKeyboardNote] = {0};

	inline void requestRendering() { uiNeedsRendering(this, 0xFFFFFFFF, 0xFFFFFFFF); }

	// Clear the persistent white chord-memory "shape" highlight (the 255 sentinels) so it gets out of
	// the way as soon as the player starts playing. Leaves velocity-tinted incoming-note highlights alone.
	// Put out every playback note-preview highlight (the 254 sentinels). Called whenever the keyboard starts
	// showing a clip, so pads lit by whatever was playing on the last one cannot carry over. Notes still
	// sounding on the new clip light again on their next note-on.
	inline void clearPlaybackPreviewHighlight() {
		for (uint8_t& v : highlightedNotes) {
			if (v == 254) {
				v = 0;
			}
		}
		for (uint32_t& t : previewExpiry) {
			t = 0;
		}
	}
	// WHEN A TAIL-LESS NOTE'S PREVIEW GOES OUT. A pluck or a one-shot has no note tail, so the sequencer never
	// sends it a note-off and never "stops" it - which was the only thing that put a preview pad out. Measured
	// on the instrument: 13 pads left at monochrome(110) with the song stopped (fw 1.3.95). So a note with no
	// tail carries its own end: the audio-sample time at which its length is up. 0 means "ends by note-off".
	uint32_t previewExpiry[kHighestKeyboardNote] = {0};
	void expirePlaybackPreview();
	inline void clearChordShapeHighlight() {
		for (uint8_t& v : highlightedNotes) {
			if (v == 255) {
				v = 0;
			}
		}
	}

	void killColumnSwitchKey(int32_t column);

	// ui
	UIType getUIType() override { return UIType::KEYBOARD_SCREEN; }
	UIType getUIContextType() override { return UIType::INSTRUMENT_CLIP; }
	UIModControllableContext getUIModControllableContext() override { return UIModControllableContext::CLIP; }
	void checkNewInstrument(Instrument* newInstrument);

private:
	bool opened() override;
	void focusRegained() override;
	void displayOrLanguageChanged() final;

	void evaluateActiveNotes();
	void updateActiveNotes();

	void noteOff(ModelStack& modelStack, Instrument& activeInstrument, bool clipIsActiveOnInstrument, int32_t note);

	ClipMinder* toClipMinder() override { return this; }
	void setLedStates();
	void graphicsRoutine() override;
	bool getAffectEntire() override;

	void unscrolledPadAudition(int32_t velocity, int32_t note, bool shiftButtonDown);

	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override {
		InstrumentClipMinder::renderOLED(canvas);
	}

	void selectLayout(int8_t offset);
	void enterScaleMode(int32_t selectedRootNote = kDefaultCalculateRootNote);
	void exitScaleMode();
	void drawNoteCode(int32_t noteCode);
	bool drawHeldChordName(); // name a manually-played chord (2+ held notes) — Roman + absolute

	PressedPad pressedPads[kMaxNumKeyboardPadPresses];
	NotesState lastNotesState;
	NotesState currentNotesState;

	bool keyboardButtonActive = false;
	bool keyboardButtonUsed = false;
	bool yEncoderActive = false;
	bool xEncoderActive = false;
};
}; // namespace deluge::gui::ui::keyboard

// TODO: should get moved into namespace once project namespacing is complete
extern deluge::gui::ui::keyboard::KeyboardScreen keyboardScreen;
