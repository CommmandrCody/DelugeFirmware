#include "hid/hid_sysex.h"
#include "extern.h" // Chroma (0x4D): sdRoutineLock
#include "gui/l10n/l10n.h"
#include "gui/menu_item/submenu.h"
#include "gui/menu_item/value_scaling.h" // Chroma (0x4B): the 0-50 the menus show           // Chroma (0x4A): the open menu's entries and cursor
#include "gui/ui/keyboard/chords.h"          // Chroma: the synced spelling lean (gChromaSpelling) packed into 0x43
#include "gui/ui/keyboard/keyboard_screen.h" // Chroma (0x48): keyboardScreen.noteRange()
#include "gui/ui/sound_editor.h"             // Chroma (0x4A): which menu item is current
#include "gui/ui/ui.h"
#include "gui/ui_timer_manager.h"
#include "gui/views/view.h"
#include "hid/display/oled.h"
#include "hid/display/seven_segment.h"
#include "hid/encoder_input.h" // Chroma (0x5B): gold knob move counts
#include "hid/led/pad_leds.h"
#include "hid/matrix/matrix_driver.h"
#include "hid/sample_frame.h" // Chroma (0x55): the packing of the sample in focus
#include "io/midi/midi_device.h"
#include "io/midi/midi_engine.h"
#include "io/midi/sysex.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/instrument_clip.h"              // Chroma (0x48): keyboardState.currentLayout
#include "model/drum/midi_drum.h"                    // Chroma (0x58): a MIDI row's channel and note
#include "model/drum/non_audio_drum.h"               // Chroma (0x58): a gate row's channel
#include "model/instrument/kit.h"                    // Chroma (0x55): the kit's selected row
#include "model/instrument/midi_instrument.h"        // Chroma (0x49): channelSuffix, MIDI only
#include "model/instrument/non_audio_instrument.h"   // Chroma (0x49): getChannel(), MIDI and CV
#include "model/output.h"                            // Chroma (0x49): the track list - name, colour, usbRouting
#include "model/sample/sample.h"                     // Chroma (0x55): length, rate
#include "model/sample/sample_holder_for_voice.h"    // Chroma (0x55): start, end, loop
#include "model/scale/preset_scales.h"               // Chroma (0x48): getScaleName
#include "model/settings/runtime_feature_settings.h" // Chroma (0x4D): the Features settings
#include "model/song/song.h"
#include "model/usb_route.h"                 // Chroma (0x4E): the routing mask
#include "modulation/arpeggiator.h"          // Chroma (0x59): the arpeggiator's settings
#include "modulation/params/param.h"         // Chroma (0x4B): which parameters make up a sound
#include "modulation/params/param_manager.h" // Chroma (0x4B)
#include "modulation/params/param_set.h" // Chroma (0x4B)                       // Chroma: resync — read the live key/scale to push on handshake
#include "playback/playback_handler.h" // Chroma: transport push - is it running, and where
#include "processing/engines/audio_engine.h"
#include "processing/engines/usb_audio_stream.h"   // Chroma (0x5B): the stream's own counters
#include "processing/sound/sound_drum.h"           // Chroma (0x4B): a kit row's name
#include "processing/sound/sound_instrument.h"     // Chroma (0x4B): the track's own sound
#include "storage/flash_storage.h"                 // Chroma (0x4D): pad brightness
#include "storage/multi_range/multisample_range.h" // Chroma (0x55): a sample oscillator's zones
#include "util/pack.h"
#include <cstring>
#include <string>

MIDICable* midiDisplayCable = nullptr;
int32_t midiDisplayUntil = 0;
uint8_t* oledDeltaImage = nullptr;
bool oledDeltaForce = true;

// Chroma: the last cable that handshaked with us over HID SysEx. LEARN context events go out on this. The
// host registers it by sending any HID request (e.g. the 7-seg request the screen mirror already uses).
static MIDICable* lastHidCable = nullptr;

// Chroma: last view slug pushed to the host (0x45 de-dupe). File-scope so the handshake RESYNC can force a re-push.
static UIType lastSentView = UIType::NONE;
// Chroma: whether we've pushed the "fx" context for the current gold-knob session (re-armed on any real view change).
static bool fxContextSent = false;
// Chroma (0x4E): defined beside the command that needs it; called from the display poll.
static void saveFeaturesIfNeeded();
static bool usbSettingsNeedSaving = false; // 1.3.135: a host changed a saved USB audio setting; routine() writes it

// Forward-declared: the resync handler below re-pushes the current view directly, and the table that
// maps a UIType to its slug is defined further down with the rest of the view code.
static const char* viewSlug(UIType t);
static void requestPads(bool subscribe); // 1.3.134: the pad request only records what is wanted (see routine())

void HIDSysex::sysexReceived(MIDICable& cable, uint8_t* data, int32_t len) {
	lastHidCable = &cable;
	if (len < 3) {
		// Chroma RESYNC: the bare handshake (the bridge sends it on connect + every ~5s). Push the CURRENT
		// global key+scale (no notes) so the CT snaps into sync immediately and can't sit stale. Harmless if
		// there's no song yet.
		if (currentSong != nullptr) {
			sendChordState((uint8_t)(((currentSong->key.rootNote % 12) + 12) % 12), nullptr, 0, "", 0, 0,
			               (uint8_t)currentSong->getCurrentScale());
		}
		// Also re-push the current on-screen view so a just-connected Companion's Learn tab syncs at once.
		//
		// This used to clear lastSentView and call sendActiveViewIfChanged(), which ALSO reset
		// fxContextSent - and the bridge sends this handshake every ~5 seconds, not just on connect.
		// So every five seconds, untouched, the Deluge announced its real view and then announced "fx"
		// again the moment anything polled the knobs:
		//
		//     17:54:45  changed -> instrument_clip
		//     17:54:45  changed -> fx
		//     17:54:50  changed -> instrument_clip
		//     17:54:50  changed -> fx
		//
		// A host that follows the view was being walked between two pages twice a minute while the
		// instrument sat still. The firmware cannot tell a new host from a keepalive, so it must not
		// treat every handshake as a fresh connection.
		//
		// Re-push the view DIRECTLY instead: the host gets its sync, the de-dupe survives, and the fx
		// context stays armed until a REAL view change re-arms it.
		{
			UI* ui = getCurrentUI();
			const char* slug = viewSlug(ui ? ui->getUIType() : UIType::NONE);
			if (slug != nullptr) {
				sendActiveView(slug);
			}
		}
		// ...and the gold knobs as they currently stand. A host that connected after the Deluge booted
		// has never seen a knob move and would otherwise draw nothing until someone touched one.
		pushGoldKnobs();
		sendTransportIfChanged(true); // a host that connects mid-song must not be blank until the next beat
		// ...and which keyboard, in what key, showing which notes. Same reason, and the report that
		// produced it: "it didn't draw - I toggled to piano roll and back and it showed". The push is
		// de-duped, so a host that connects while the instrument sits still is told nothing until
		// something happens to change. A state signal has to go in the hello as well as on change.
		sendKeyboardStateIfChanged(true);
		// ...and which tracks exist and where they go. Same reason again: a host that connects to a
		// song already loaded has nothing to draw until a track changes, which may be never.
		sendTrackStateIfChanged(true);
		// ...and the menu it has open, or that none is. Same reason as every line above.
		sendMenuContextIfChanged(true);
		// ...and the sound in focus, or that there is none.
		sendSoundStateIfChanged(true);
		// ...and the device's own state: current track, pad brightness and the Features settings.
		sendDeviceStateIfChanged(true);
		// ...and the front panel: which button lights are on.
		sendPanelStateIfChanged(true);
		// ...and which note each pad plays, or that the pads play none.
		sendPadNotesIfChanged(true);
		// ...and the velocity each pad plays at, where position decides it.
		sendPadVelocitiesIfChanged(true);
		// ...and what the keyboard's sidebar columns are.
		sendKeyboardColumnsIfChanged(true);
		// ...and the sample the sound in focus plays, or that there is none.
		sendSampleStateIfChanged(true);
		// ...and whether the USB audio stream is really sending.
		sendHealthIfDue(true);
		// ...and the chord being held, or that there is none.
		sendHeldChordIfChanged(true);
		sendKitRowsIfChanged(true);
		sendEffectsIfChanged(true);
		sendMidiInputsIfChanged(true);
		return;
	}
	// first three bytes are already used, next is command
	switch (data[1]) {
	case 0:
		requestOLEDDisplay(cable, data, len);
		break;

	case SysEx::SysexCommands::MixerSet:
		receiveMixerSet(cable, data, len);
		break;

	case 1:
		request7SegDisplay(cable, data, len);
		break;

	case 2:
		readBlock(cable);
		break;

	case 3:
		// THE PAD REQUEST ONLY RECORDS WHAT IS WANTED (1.3.134). This runs inside the "check usb midi" task, which
		// holds the USB resource - the same one the USB audio stream task needs every millisecond. Building the grid
		// (144 x prepareColour), packing it and running every change check here starved the audio stream: measured
		// 2026-11-25, with a host polling 12.5 times a second the Deluge's USB audio stalled within minutes of notes
		// being played, and without the poll it did not stall in 12 minutes. The work is done in routine(), a task
		// of its own that holds no USB resource.
		//   03 01   subscribe: send the grid now and then whatever changes, for 5 s - renewed by asking again
		//   03 00   (older hosts) one grid, as before
		requestPads(len >= 3 && data[2] == 1);
		break;

	default:
		break;
	}
}

void HIDSysex::requestOLEDDisplay(MIDICable& cable, uint8_t* data, int32_t len) {
	//	if (data[4] == 0 or data[4] == 1) {
	if (data[2] == 0 or data[2] == 1) {
		// sendOLEDData(device, (data[4] == 1)); // Adjustting by -2 to correct for payload offset.
		sendOLEDData(cable, (data[2] == 1));
	}
	// else if (data[4] == 2 || data[4] == 3) {
	else if (data[2] == 2 || data[2] == 3) {
		// bool force = (data[4] == 3);
		bool force = (data[2] == 3);
		midiDisplayCable = &cable;
		// two seconds
		midiDisplayUntil = AudioEngine::audioSampleTimer + 2 * kSampleRate;
		if (display->haveOLED()) {
			if (force) {
				oledDeltaForce = true;
			}

			if (oledDeltaImage == nullptr) {
				oledDeltaImage = (uint8_t*)GeneralMemoryAllocator::get().allocMaxSpeed(
				    sizeof(uint8_t[OLED_MAIN_HEIGHT_PIXELS >> 3][OLED_MAIN_WIDTH_PIXELS]));
			}
		}
		sendDisplayIfChanged();
		if (force && display->have7SEG()) {
			send7SegData(cable);
		}
	}
	// else if (data[4] == 4) { // SWAP
	else if (data[2] == 4) { // SWAP
		deluge::hid::display::swapDisplayType();
		oledDeltaForce = true;
	}
}

void HIDSysex::sendDisplayIfChanged() {
	// Chroma: piggyback the active-view check on the display poll. This runs on every redraw (a view change always
	// redraws), it self-gates on lastHidCable + de-dupes, and it sits BEFORE the display-mirror gate below so it
	// works even for hosts that never requested screen mirroring (the Companion arms lastHidCable on connect).
	sendActiveViewIfChanged();
	sendTransportIfChanged();     // Chroma: same piggyback, same reasoning - it self-gates and de-dupes
	sendKeyboardStateIfChanged(); // Chroma (0x48): layout, key and note range - same piggyback again
	sendTrackStateIfChanged();    // Chroma (0x49): the track list - de-duped by a hash of the whole list
	sendMenuContextIfChanged();   // Chroma (0x4A): the open menu - same piggyback, de-duped the same way
	sendSoundStateIfChanged();    // Chroma (0x4B): the sound in focus - same piggyback, de-duped the same way
	sendDeviceStateIfChanged();   // Chroma (0x4D): current track, pad brightness, Features - likewise
	flushGoldKnobs();             // Chroma (0x46): a knob value held back by the 40 ms gap
	sendSampleStateIfChanged();   // Chroma (0x55): the sample in focus - likewise
	saveFeaturesIfNeeded();       // Chroma (0x4E): write a host's Features change to the card, from here only
	// NB: timer is only used for throttling, under good conditions sending
	// is driven by the display subsystem only
	uiTimerManager.unsetTimer(TimerName::SYSEX_DISPLAY);
	if (midiDisplayCable == nullptr || AudioEngine::audioSampleTimer > midiDisplayUntil) {
		return;
	}
	// not exact, but if more than half than the serial buffer is still full,
	// we need to slow down a little. (USB buffer is larger and should be consumed much quicker)
	if (midiDisplayCable->sendBufferSpace() < 512) {
		uiTimerManager.setTimer(TimerName::SYSEX_DISPLAY, 100);
		return;
	}

	if (display->haveOLED()) {
		sendOLEDDataDelta(*midiDisplayCable, false);
	}
	if (display->have7SEG()) {
		send7SegData(*midiDisplayCable);
	}
}

void HIDSysex::sendOLEDData(MIDICable& cable, bool rle) {
	if (display->haveOLED()) {
		const int32_t data_size = 768;
		const int32_t max_packed_size = 922;
		// 		uint8_t reply_hdr[5] = {0xf0, 0x7d, 0x02, 0x40, rle ? 0x01_u8 : 0x00_u8};
		uint8_t reply_hdr[8] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x40, rle ? 0x01_u8 : 0x00_u8};
		uint8_t* reply = midiEngine.sysex_fmt_buffer;
		// 		memcpy(reply, reply_hdr, 5);
		memcpy(reply, reply_hdr, 8); //
		                             //		reply[5] = 0; // nominally 32*data[5] is start pos for a delta
		reply[8] = 0;                // nominally 32*data[8] is start pos for a delta //

		int32_t packed;
		if (rle) {
			packed =
			    //				pack_8to7_rle(reply + 6, max_packed_size,
			    // deluge::hid::display::OLED::oledCurrentImage[0], data_size);
			    pack_8to7_rle(reply + 9, max_packed_size, deluge::hid::display::OLED::oledCurrentImage[0], data_size);
		}
		else {
			//			packed = pack_8bit_to_7bit(reply + 6, max_packed_size,
			// deluge::hid::display::OLED::oledCurrentImage[0], 			                           data_size);
			packed = pack_8bit_to_7bit(reply + 9, max_packed_size, deluge::hid::display::OLED::oledCurrentImage[0],
			                           data_size); //
		}
		if (packed < 0) {
			display->popupTextTemporary("error: fail");
		}
		//		reply[6 + packed] = 0xf7; // end of transmission
		reply[9 + packed] = 0xf7;            // end of transmission
		                                     // 		device->sendSysex(reply, packed + 7); //
		cable.sendSysex(reply, packed + 10); //
	}
}

void HIDSysex::request7SegDisplay(MIDICable& cable, uint8_t* data, int32_t len) {
	if (data[2] == 0) { // was 4
		send7SegData(cable);
	}
	else if (data[2] == 1) { // Chroma: request the real text behind the segments
		send7SegText(cable);
	}
}

void HIDSysex::send7SegData(MIDICable& cable) {
	if (display->have7SEG()) {
		// aschually 8 segments if you count the dot
		auto data = display->getLast();
		const int32_t packed_data_size = 5;
		//		uint8_t reply[12] = {0xf0, 0x7d, 0x02, 0x41, 0x00, 0x00};
		uint8_t reply[15] = {0xf0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x41, 0x00, 0x00};
		//  	pack_8bit_to_7bit(reply + 6, packed_data_size, data.data(), data.size());
		// int32_t pack_8bit_to_7bit(uint8_t* dst, int32_t dst_size, uint8_t* src, int32_t src_len);
		pack_8bit_to_7bit(reply + 9, packed_data_size, data.data(), data.size());
		//		reply[6 + packed_data_size] = 0xf7; // end of transmission
		reply[9 + packed_data_size] = 0xf7; // end of transmission
		                                    //		device->sendSysex(reply, packed_data_size + 7);
		cable.sendSysex(reply, packed_data_size + 10);
	}
}

// Chroma: reply with the actual text on the 7-seg as 7-bit ASCII, so the host shows "BASS" instead of
// reverse-engineering ambiguous segments (S/5 and Z/2 share a pattern). The display kept the literal
// string when it was set, so there's nothing to guess.
//   reply: F0 00 21 7B 01 02 42 <n> <ascii x n> F7
void HIDSysex::send7SegText(MIDICable& cable) {
	if (!display->have7SEG()) {
		return;
	}
	std::string_view text = display->getLastTextForHost();
	uint8_t reply[8 + 64 + 1] = {0xf0, 0x00, 0x21, 0x7b, 0x01, 0x02, 0x42, 0x00};
	uint8_t n = 0;
	for (size_t i = 0; i < text.size() && n < 64; i++) {
		reply[8 + n++] = (uint8_t)text[i] & 0x7f; // 7-bit ASCII rides SysEx unescaped
	}
	reply[7] = n; // count
	reply[8 + n] = 0xf7;
	cable.sendSysex(reply, 9 + n);
}

// Chroma: F0 00 21 7B 01 4C <region> <x> <y> <n> <contextId ASCII...> F7  (see chroma-schema SCHEMA.md 2.1).
// contextId is 7-bit ASCII so it rides SysEx unescaped. No-op until a host has handshaked over HID SysEx.
void HIDSysex::sendLearnContext(uint8_t region, uint8_t x, uint8_t y, const char* contextId) {
	if (lastHidCable == nullptr || contextId == nullptr) {
		return;
	}
	uint8_t msg[11 + 127];
	msg[0] = 0xf0;
	msg[1] = 0x00;
	msg[2] = 0x21;
	msg[3] = 0x7b;
	msg[4] = 0x01;
	msg[5] = SysEx::SysexCommands::LearnContext; // 0x4C
	msg[6] = region & 0x7f;
	msg[7] = x & 0x7f;
	msg[8] = y & 0x7f;
	uint8_t n = 0;
	for (const char* c = contextId; *c && n < 127; c++) {
		msg[10 + n++] = (uint8_t)(*c) & 0x7f; // mask to 7-bit; contextIds are ASCII by contract
	}
	msg[9] = n;
	msg[10 + n] = 0xf7;
	lastHidCable->sendSysex(msg, 11 + n);
}

// Chroma: F0 00 21 7B 01 43 <keyRoot> <nNotes> <note...> <nCtx> <ctx ASCII> F7.
// Pushed on every NORMAL palette pick. Carries the runtime truth (voiced MIDI notes) + the graph key, so the
// host renders the real chord regardless of what the 7-seg shows. No-op until a host handshakes.
void HIDSysex::sendChordState(uint8_t keyRoot, const int16_t* notes, uint8_t numNotes, const char* contextId,
                              int8_t spread, int8_t inversion, uint8_t scale, const char* chordName, int8_t voicingWalk,
                              int8_t voicingHome, uint8_t voiceOctaves, int8_t octaveBase, bool voicingSplitMode,
                              int8_t voicingSplit) {
	if (lastHidCable == nullptr) {
		return;
	}
	uint8_t msg[200];
	uint8_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = SysEx::SysexCommands::ChordState; // 0x43
	msg[i++] = keyRoot & 0x7f;
	uint8_t cnt = (numNotes > 24) ? 24 : numNotes;
	msg[i++] = cnt;
	for (uint8_t k = 0; k < cnt; k++) {
		msg[i++] = (uint8_t)(notes[k]) & 0x7f;
	}
	uint8_t nc = 0;
	if (contextId != nullptr) {
		for (const char* c = contextId; *c && nc < 120; c++) {
			nc++;
		}
	}
	msg[i++] = nc;
	for (uint8_t k = 0; k < nc; k++) {
		msg[i++] = (uint8_t)(contextId[k]) & 0x7f;
	}
	// Chroma complete-sync: extensible voicing-params block after the contextId (old hosts ignore the
	// tail). Format: [count][p0][p1]... positional: 0=spread, 1=inversion. Add more by bumping count.
	// extras count. positional: 0=spread, 1=inversion, 2=scale/mode index, 3=spelling lean,
	// 4=voicingWalk, 5=voicingHome, 6=voiceOctaves bitmask, 7=octaveBase.
	//
	// 4..7 complete the FIVE voice-shaping fields. Spread and inversion were the only two that ever
	// left the instrument, so no surface could show the voicing dial or the voice it springs to -
	// even after a HOLD gesture was added for exactly that. Old hosts read the count and skip the
	// tail, which is what this block was built for.
	// 8 = voicing MODE (0 octave/walk, 1 split), 9 = the split pivot in semitones above the chord's
	// lowest note. A surface cannot draw the voicing without knowing WHICH mode produced it.
	msg[i++] = 10;
	msg[i++] = (uint8_t)spread & 0x7f;
	msg[i++] = (uint8_t)inversion & 0x7f;
	msg[i++] = scale & 0x7f;
	msg[i++] = (uint8_t)deluge::gui::ui::keyboard::gChromaSpelling & 0x7f; // 0=Auto,1=Flats,2=Sharps (synced spelling)
	// BIAS-64: these two are signed (-24..24) and every other extra is raw 7-bit unsigned. Biasing
	// keeps them positive on the wire instead of relying on a sign-extend the reader must guess at.
	msg[i++] = (uint8_t)(voicingWalk + 64) & 0x7f;
	msg[i++] = (uint8_t)(voicingHome + 64) & 0x7f;
	msg[i++] = voiceOctaves & 0x7f; // bits 0..6 = octave offsets -3..+3; already fits
	msg[i++] = (uint8_t)octaveBase & 0x7f;
	msg[i++] = voicingSplitMode ? 1 : 0;
	msg[i++] = (uint8_t)voicingSplit & 0x7f; // 0..24, always positive - no bias needed
	// Chroma "Delly is the truth": the Deluge's OWN chord-name string, appended after the extras as
	// [len][ASCII...]. Every host renders this verbatim instead of re-deriving. Old hosts ignore the tail.
	uint8_t nn = 0;
	if (chordName != nullptr) {
		for (const char* c = chordName; *c && nn < 16; c++) {
			nn++;
		}
	}
	msg[i++] = nn;
	for (uint8_t k = 0; k < nn; k++) {
		msg[i++] = (uint8_t)(chordName[k]) & 0x7f;
	}
	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// Chroma: F0 00 21 7B 01 45 <n> <viewId ASCII...> F7.
// A tiny outbound push naming the on-screen view, so the Companion's Learn tab auto-follows what the user is doing.
// viewId is a short 7-bit ASCII slug ("clip","song","arranger","kit","fx-menu",...). No-op until a host handshakes.
void HIDSysex::sendActiveView(const char* viewId) {
	if (lastHidCable == nullptr || viewId == nullptr) {
		return;
	}
	uint8_t msg[8 + 32];
	msg[0] = 0xf0;
	msg[1] = 0x00;
	msg[2] = 0x21;
	msg[3] = 0x7b;
	msg[4] = 0x01;
	msg[5] = SysEx::SysexCommands::ActiveView; // 0x45
	uint8_t n = 0;
	for (const char* c = viewId; *c && n < 31; c++) {
		msg[7 + n++] = (uint8_t)(*c) & 0x7f; // slugs are ASCII by contract
	}
	msg[6] = n;
	msg[7 + n] = 0xf7;
	lastHidCable->sendSysex(msg, 8 + n);
}

// Map the on-screen UI to a stable, host-neutral view slug. The Companion owns the slug->hub mapping, so its
// hub taxonomy can change without a firmware update. Returns nullptr for views with no useful "learn this"
// target (context menus, transient/unknown) — the caller then leaves the host on whatever view it last showed.
static const char* viewSlug(UIType t) {
	switch (t) {
	case UIType::SESSION:
		return "session";
	case UIType::ARRANGER:
		return "arranger";
	case UIType::INSTRUMENT_CLIP:
		return "instrument_clip";
	case UIType::AUDIO_CLIP:
		return "audio_clip";
	case UIType::KEYBOARD_SCREEN:
		return "keyboard";
	case UIType::AUTOMATION:
		return "automation";
	case UIType::PERFORMANCE:
		return "performance";
	case UIType::SOUND_EDITOR:
		return "sound_editor";
	case UIType::SAMPLE_BROWSER:
		return "sample_browser";
	case UIType::DX_BROWSER:
		return "dx_browser";
	case UIType::SAMPLE_MARKER_EDITOR:
		return "sample_marker";
	case UIType::SLICER:
		return "slicer";
	case UIType::AUDIO_RECORDER:
		return "audio_recorder";
	case UIType::LOAD_SONG:
		return "load_song";
	case UIType::LOAD_INSTRUMENT_PRESET:
		return "load_preset";
	case UIType::LOAD_PATTERN:
		return "load_pattern";
	case UIType::LOAD_MIDI_DEVICE_DEFINITION:
		return "load_midi";
	case UIType::SAVE_SONG:
		return "save_song";
	case UIType::SAVE_INSTRUMENT_PRESET:
		return "save_preset";
	case UIType::SAVE_KIT_ROW:
		return "save_kitrow";
	case UIType::SAVE_PATTERN:
		return "save_pattern";
	case UIType::SAVE_MIDI_DEVICE_DEFINITION:
		return "save_midi";
	case UIType::RENAME:
		return "rename";
	default:
		return nullptr; // CONTEXT_MENU, NONE, etc. — keep the host on its last view
	}
}

// Chroma: broadcast the active view when it changes, so the Companion's Learn tab auto-follows what's on screen.
// Called from the display passenger poll (sendDisplayIfChanged), which already runs on every redraw — and a view
// change always redraws. Cheap: a getUIType() read + int compare per call, de-duped so 0x45 only goes out on an
// actual change. A view with no hub (nullptr slug) is skipped WITHOUT updating the de-dupe, so returning to the
// underlying view doesn't re-fire.
void HIDSysex::sendActiveViewIfChanged() {
	UI* ui = getCurrentUI();
	UIType t = ui ? ui->getUIType() : UIType::NONE;
	if (t == lastSentView) {
		return;
	}
	const char* slug = viewSlug(t);
	if (slug == nullptr) {
		return;
	}
	lastSentView = t;
	fxContextSent = false; // a real view change re-arms the fx context, so the next knob turn pushes "fx" again
	sendActiveView(slug);
}

// Chroma: a gold-knob (FX) turn isn't a view change, so the view poll never fires for it. Push an "fx" context so a
// following host (the Companion's Learn tab) jumps to FX help. Sent WITHOUT touching lastSentView, so the view poll
// stays de-duped and doesn't snap back; fired once per fx-session, re-armed when the real view next changes.
void HIDSysex::sendFxContext() {
	if (!fxContextSent) {
		sendActiveView("fx");
		fxContextSent = true;
	}
}

// Chroma (0x46): what the gold knobs are actually doing.
//
// Cody, on the app's header: "the gold knobs still don't work, look bigger, or provide ANY feedback."
// They could not. Nothing on the wire carried a gold-knob position, so the host had nothing to draw
// them from and they sat dark on every page but Design, where a local event happened to exist.
//
// The level sent is the one the PHYSICAL four-LED column is showing - the same number, from the same
// place, at the same moment - so the app's knob and the instrument's knob cannot disagree. That is
// the whole point of mirroring a control rather than modelling it.
//
//   F0 00 21 7B 01 46 <knob 0|1> <level 0..127> <bipolar 0|1> <nameLen> <ascii name...> F7
//
// De-duped on (knob, level, name): setKnobIndicatorLevel is called on redraws as well as turns, and
// re-sending an unchanged value at redraw rate would be a stream of nothing.
// TWO caches, and the difference between them is the whole bug Cody found: "gold knobs work, but
// only after you start turning them."
//
//   cur*  - what the knob IS. Updated on every indicator change, INCLUDING while no host is
//           connected, because the instrument has been sitting at some position since boot and that
//           is the truth a host needs the moment it arrives.
//   sent* - what was last put on the wire. Only for de-duping.
//
// Keeping one cache meant the value was only recorded when it was sent, and it was only sent when a
// host was already listening - so a Cockpit that connected after the Deluge booted had nothing to
// receive and the knobs stayed dark until someone touched one. A mirror that only works once you
// move is not a mirror, it is an echo.
static uint8_t curKnobLevel[2] = {0xff, 0xff};
// 1.3.122: the per-knob report gap, so a chattering encoder cannot flood USB (see sendGoldKnob).
constexpr uint32_t kKnobGapSamples = kSampleRate * 40 / 1000;
static uint32_t knobSentAt[2] = {0, 0};
static bool knobPending[2] = {false, false};
static bool knobBipolar[2] = {false, false};
static char curKnobName[2][32] = {{0}, {0}};
static uint8_t sentKnobLevel[2] = {0xff, 0xff};
static char sentKnobName[2][32] = {{0}, {0}};

static void emitGoldKnob(uint8_t whichKnob, uint8_t lv, bool bipolar, const char* name);

void HIDSysex::sendGoldKnob(uint8_t whichKnob, int32_t level, bool bipolar, const char* paramName) {
	if (whichKnob > 1) {
		return;
	}
	// The indicator level is 0..128; 128 would set the 8th bit and corrupt the frame, so it is clamped
	// into 7 bits here rather than being allowed to become a different number somewhere downstream.
	int32_t lv = level;
	if (lv < 0) {
		lv = 0;
	}
	if (lv > 127) {
		lv = 127;
	}
	char name[32];
	uint8_t n = 0;
	if (paramName != nullptr) {
		for (const char* c = paramName; *c && n < 31; c++) {
			name[n++] = (char)((uint8_t)(*c) & 0x7f);
		}
	}
	name[n] = 0;

	// Record what the knob IS first, whether or not anyone is listening. This is the line that makes
	// a late-connecting host able to be told anything at all.
	curKnobLevel[whichKnob] = (uint8_t)lv;
	strncpy(curKnobName[whichKnob], name, 31);
	curKnobName[whichKnob][31] = 0;

	if (lastHidCable == nullptr) {
		return; // nobody to tell yet - but we now know what to tell them when they turn up
	}
	if (sentKnobLevel[whichKnob] == (uint8_t)lv && strncmp(sentKnobName[whichKnob], name, 32) == 0) {
		knobPending[whichKnob] = false;
		return; // nothing moved and nothing was reassigned
	}
	// AT MOST ONE REPORT PER KNOB EVERY 40 MS (1.3.122). This is called on every encoder tick, and a worn
	// encoder chatters: Cody's upper gold knob is mechanically bumped, and turning cutoff on it killed the
	// USB audio stream within seconds while the lower knob (resonance) never did. Each tick went out over
	// USB at once. Now a tick inside the gap only marks the knob pending; flushGoldKnobs() sends the
	// latest value when the gap has passed, so the host still ends on the true position.
	knobBipolar[whichKnob] = bipolar;
	if ((uint32_t)(AudioEngine::audioSampleTimer - knobSentAt[whichKnob]) < kKnobGapSamples) {
		knobPending[whichKnob] = true;
		return;
	}
	knobPending[whichKnob] = false;
	knobSentAt[whichKnob] = AudioEngine::audioSampleTimer;
	emitGoldKnob(whichKnob, (uint8_t)lv, bipolar, name);
}

// Sends a knob value held back by the 40 ms gap, once the gap has passed. From the pad poll and the display
// poll, which run often enough that the last position of a turn reaches the host within a poll.
void HIDSysex::flushGoldKnobs() {
	if (lastHidCable == nullptr) {
		return;
	}
	for (uint8_t k = 0; k < 2; k++) {
		if (!knobPending[k] || curKnobLevel[k] == 0xff) {
			continue;
		}
		if ((uint32_t)(AudioEngine::audioSampleTimer - knobSentAt[k]) < kKnobGapSamples) {
			continue;
		}
		knobPending[k] = false;
		knobSentAt[k] = AudioEngine::audioSampleTimer;
		emitGoldKnob(k, curKnobLevel[k], knobBipolar[k], curKnobName[k]);
	}
}

// Push BOTH knobs as they currently stand, ignoring the de-dupe. Called from the handshake, which
// fires when a host connects and every few seconds after - so a Cockpit started at any point gets
// the real positions without anyone having to touch the instrument first.
void HIDSysex::pushGoldKnobs() {
	if (lastHidCable == nullptr) {
		return;
	}
	for (uint8_t k = 0; k < 2; k++) {
		if (curKnobLevel[k] == 0xff) {
			continue; // this knob has never reported a level; saying nothing beats inventing one
		}
		emitGoldKnob(k, curKnobLevel[k], false, curKnobName[k]);
	}
}

static void emitGoldKnob(uint8_t whichKnob, uint8_t lv, bool bipolar, const char* name) {
	if (lastHidCable == nullptr) {
		return;
	}
	uint8_t n = 0;
	while (name[n] != 0 && n < 31) {
		n++;
	}
	sentKnobLevel[whichKnob] = lv;
	strncpy(sentKnobName[whichKnob], name, 31);
	sentKnobName[whichKnob][31] = 0;

	uint8_t msg[10 + 32];
	msg[0] = 0xf0;
	msg[1] = 0x00;
	msg[2] = 0x21;
	msg[3] = 0x7b;
	msg[4] = 0x01;
	msg[5] = SysEx::SysexCommands::GoldKnob; // 0x46
	msg[6] = whichKnob;
	msg[7] = lv;
	msg[8] = bipolar ? 1 : 0;
	msg[9] = n;
	for (uint8_t i = 0; i < n; i++) {
		msg[10 + i] = (uint8_t)name[i];
	}
	msg[10 + n] = 0xf7;
	lastHidCable->sendSysex(msg, 11 + n);
}

// Chroma (0x47): WHERE WE ARE.
//
// Cody: "I want all of it - where we are, what we're playing, and the levels and settings." Three of
// those four already reach a host. This is the fourth, and it is the one a four-character display
// genuinely cannot give you: is it running, how fast, and how far in.
//
//   F0 00 21 7B 01 47 <playing 0|1> <bpm hi> <bpm lo> <bar hi> <bar lo> <beat 0..3> F7
//
// bpm is BPM x 10 in two 7-bit halves (so 128.0 travels as 1280, which is 10,0). bar is 1-based and
// also split across two 7-bit bytes, which covers 16383 bars - long enough for anything anyone will
// play.
//
// DE-DUPED ON THE BEAT, not the tick. The playhead moves hundreds of times a second and a host wants
// to know where you ARE, not to re-render at audio rate; four messages a bar is enough to show a
// position and cheap enough to ignore. The same reasoning as the gold knobs: send when it changes,
// and let "changes" mean something a person could see.
static int32_t lastBar = -1;
static int8_t lastBeat = -1;
static int8_t lastPlaying = -1;
static int32_t lastBpm10 = -1;

void HIDSysex::sendTransportIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	const bool playing = playbackHandler.isEitherClockActive();
	int32_t bpm10 = 0;
	if (currentSong != nullptr) {
		const float bpm = playbackHandler.calculateBPMForDisplay();
		bpm10 = (int32_t)(bpm * 10.0f + 0.5f);
		if (bpm10 < 0) {
			bpm10 = 0;
		}
		if (bpm10 > 16383) {
			bpm10 = 16383;
		}
	}

	// Position, in bars and quarters, from the swung tick count. The bar length comes from the SONG
	// rather than from a hardcoded four beats, so a song that is not in four still reports a position
	// that matches what the player is counting.
	//
	// Stopped reports nothing rather than wherever it happened to halt: a stopped transport showing a
	// position reads exactly like a running one, which is the wrong answer to "where are we".
	int32_t bar = 0;
	int8_t beat = 0;
	if (playing && currentSong != nullptr) {
		const uint32_t barLen = currentSong->getBarLength();
		if (barLen > 0) {
			const int64_t ticks = playbackHandler.getActualSwungTickCount();
			const int64_t pos = ticks < 0 ? 0 : ticks;
			bar = (int32_t)(pos / barLen) + 1;
			beat = (int8_t)((pos % barLen) * 4 / barLen);
			if (bar > 16383) {
				bar = 16383;
			}
		}
	}

	if (!force && playing == (lastPlaying == 1) && bar == lastBar && beat == lastBeat && bpm10 == lastBpm10) {
		return;
	}
	lastPlaying = playing ? 1 : 0;
	lastBar = bar;
	lastBeat = beat;
	lastBpm10 = bpm10;

	uint8_t msg[13];
	msg[0] = 0xf0;
	msg[1] = 0x00;
	msg[2] = 0x21;
	msg[3] = 0x7b;
	msg[4] = 0x01;
	msg[5] = SysEx::SysexCommands::Transport; // 0x47
	msg[6] = playing ? 1 : 0;
	msg[7] = (uint8_t)((bpm10 >> 7) & 0x7f);
	msg[8] = (uint8_t)(bpm10 & 0x7f);
	msg[9] = (uint8_t)((bar >> 7) & 0x7f);
	msg[10] = (uint8_t)(bar & 0x7f);
	msg[11] = (uint8_t)(beat & 0x7f);
	msg[12] = 0xf7;
	lastHidCable->sendSysex(msg, 13);
}

// Chroma (0x48): WHICH KEYBOARD, IN WHAT KEY, SHOWING WHICH NOTES.
//
// Three things a host could not previously know, all of which a player reads off the instrument at a
// glance and had no way to see on a second screen:
//
//   the LAYOUT   all eight keyboard layouts report as the one view slug "keyboard" over 0x45, so
//                isomorphic, in-key, piano and the rest were indistinguishable.
//   the KEY      root note and scale escaped only via 0x43, which fires on a harmonic palette pick.
//                In an ordinary keyboard layout nothing ever said what key you were in.
//   the RANGE    the span of notes the grid is currently showing. It moves as you scroll and is the
//                thing that tells you where on the instrument you are.
//
// Cody: "I want the mode of the keyboard displayed clearly and the current root note/scale selected
// ... also note the range of notes C1-N".
//
// De-duped like the transport push: this is called on every redraw, and a surface does not need to
// be told the same thing sixty times a second.
// Chroma (0x49): WHICH TRACKS EXIST, AND WHERE THEY GO.
//
// Cody, on the USB audio routing menus: "I want the companion to be able to set the track properties
// for things just like this." Setting comes later and is a separate decision; this is the half that
// has to exist first, because a panel that cannot SHOW you where a track currently goes has no
// business offering to change it.
//
// Today the only way to learn a track's USB routing is to walk into that track's menu, one track at a
// time, on a four-character display. The song already knows all of it.
//
//   F0 00 21 7B 01 02 49 <nTracks> [ track ... ] F7
//   track = <idx> <type> <colourHi> <colourLo> <usbHi> <usbLo> <flags> <nameLen> <name ascii...>
//                                                                       [ <channel> <chanSuffix> ]
//
// Everything is 7-bit, so the two 16-bit fields travel as high/low halves. `flags` is bit 0 muted,
// bit 1 soloing, bit 2 armed for recording, and BIT 6 says this record carries a channel.
//
// THE CHANNEL IS OPTIONAL, PER RECORD. Only MIDI and CV tracks have one - both inherit it from
// NonAudioInstrument - so a synth, kit or audio track does not set bit 6 and pays nothing. Cody,
// after routing USB audio by hand: "its a struggle and menu diving ... I think we need a routing
// page for midi and audio." The audio half was already here in `usbRouting`; this is the other half.
//
// THE MARKER IS A FLAGS BIT, not a frame-version byte after the count. A version byte would be read
// as the first track's INDEX by any decoder that predated it, and index 0 is a legitimate value, so
// the ambiguity would be silent. A spare flags bit cannot be mistaken for anything.
//
// `channelSuffix` is -1 on the instrument for "none" and travels as 0x7f, because every field here
// is 7-bit and a negative number has nowhere to go.
void HIDSysex::sendTrackStateIfChanged(bool force) {
	if (lastHidCable == nullptr || currentSong == nullptr) {
		return;
	}

	// BOUNDED, because this is the first push whose size depends on the SONG rather than on the
	// protocol. The incoming sysex buffer is 1024 bytes; 24 tracks at 8 + 12 bytes each is 480, which
	// leaves room and still covers any song anyone plays. A song with more tracks reports the first
	// 24 rather than overrunning - a truncated list is a visible absence, a buffer overrun is not.
	constexpr uint8_t kMaxTracks = 24;
	constexpr uint8_t kMaxName = 12;

	// 10, not 8: a MIDI or CV record carries two more bytes than the fixed header above. Worst case is
	// every track being a MIDI track, and a buffer sized for the old record would be overrun by 48
	// bytes - silently, on the stack, in an audio-thread-adjacent path. 8 + 24 * 22 = 536, which is
	// still comfortably inside the 1024-byte sysex buffer.
	uint8_t msg[8 + kMaxTracks * (12 + kMaxName)]; // +2 per track for the mixer
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02; // the longer frame form, which is what new tags use
	msg[i++] = SysEx::SysexCommands::TrackState;
	const int32_t countAt = i;
	msg[i++] = 0; // filled in once we know how many we actually wrote

	// A cheap running hash of everything we are about to send, so an unchanged song stays quiet. The
	// handshake repeats every five seconds; without this, a host following the track list would be
	// told the same thing twelve times a minute forever.
	uint32_t hash = 2166136261u;
	auto mix = [&hash](uint8_t b) { hash = (hash ^ b) * 16777619u; };

	uint8_t n = 0;
	for (Output* output = currentSong->firstOutput; output != nullptr && n < kMaxTracks; output = output->next, n++) {
		const char* name = output->name.get();
		uint8_t nameLen = 0;
		while (name != nullptr && name[nameLen] != 0 && nameLen < kMaxName) {
			nameLen++;
		}
		const uint16_t usb = output->usbRouting;
		const uint16_t colour = (uint16_t)output->colour;
		uint8_t flags = 0;
		if (output->mutedInArrangementMode) {
			flags |= 1 << 0;
		}
		if (output->soloingInArrangementMode) {
			flags |= 1 << 1;
		}
		if (output->armedForRecording) {
			flags |= 1 << 2;
		}

		// THE CHANNEL, for the tracks that have one.
		//
		// MIDI and CV both inherit `channel` from NonAudioInstrument, so one cast covers both. A
		// synth, kit or audio track has no channel at all and does not set the marker - reporting 0
		// for one would be a fabricated value, and 0 is a real channel, which makes it the kind of
		// wrong answer that looks exactly like a right one.
		//
		// `channelSuffix` lives on MIDIInstrument rather than on NonAudioInstrument, and is -1 for
		// "none". 0x7f carries that, because every field in this frame is 7-bit.
		bool hasChannel = false;
		uint8_t chanByte = 0;
		uint8_t suffixByte = 0x7f;
		if (output->type == OutputType::MIDI_OUT || output->type == OutputType::CV) {
			auto* nai = static_cast<NonAudioInstrument*>(output);
			hasChannel = true;
			chanByte = (uint8_t)(nai->getChannel() & 0x7f);
			if (output->type == OutputType::MIDI_OUT) {
				const int32_t suffix = static_cast<MIDIInstrument*>(output)->channelSuffix;
				suffixByte = (suffix < 0) ? 0x7f : (uint8_t)(suffix & 0x7f);
			}
		}
		if (hasChannel) {
			flags |= 1 << 6;
		}
		// THE MIXER: this record carries a level and a mute/solo byte (firmware 1.3.94+). A marker bit like
		// the channel's, so an older reader that stops at the name still reads the frame it always has.
		flags |= 1 << 5;
		const uint8_t mixLevel = output->mixerLevel & 0x7f;
		const uint8_t mixFlags = (output->mixerMute ? 1 : 0) | (output->mixerSolo ? 2 : 0);

		msg[i++] = n;
		msg[i++] = (uint8_t)output->type & 0x7f;
		msg[i++] = (colour >> 7) & 0x7f;
		msg[i++] = colour & 0x7f;
		msg[i++] = (usb >> 7) & 0x7f;
		msg[i++] = usb & 0x7f;
		msg[i++] = flags;
		msg[i++] = nameLen;
		for (uint8_t k = 0; k < nameLen; k++) {
			// 7-bit and printable. A stray high byte in a user-typed name would end the sysex frame
			// early and the host would see a truncated, valid-looking list.
			const uint8_t c = (uint8_t)name[k] & 0x7f;
			msg[i++] = (c >= 32) ? c : '?';
		}
		// After the name, so a reader that does not know about them simply stops at the name and the
		// frame is the one it has always seen.
		if (hasChannel) {
			msg[i++] = chanByte;
			msg[i++] = suffixByte;
		}
		msg[i++] = mixLevel;
		msg[i++] = mixFlags;
		mix(mixLevel);
		mix(mixFlags);

		mix(n);
		mix((uint8_t)output->type);
		mix((uint8_t)(colour >> 7));
		mix((uint8_t)colour);
		mix((uint8_t)(usb >> 7));
		mix((uint8_t)usb);
		mix(flags);
		mix(nameLen);
		for (uint8_t k = 0; k < nameLen; k++) {
			mix((uint8_t)name[k]);
		}
		// INTO THE HASH TOO, or the dedupe defeats the whole point: move a MIDI track from channel 1
		// to channel 2 and nothing else about it changes, so an unchanged hash would keep the host on
		// the old channel until something else in the song happened to move. `flags` already carries
		// the marker bit, so a track gaining a channel is caught either way - this is for a channel
		// that CHANGES.
		if (hasChannel) {
			mix(chanByte);
			mix(suffixByte);
		}
	}
	msg[countAt] = n;
	mix(n);

	static uint32_t lastHash = 0;
	static bool everSent = false;
	if (!force && everSent && hash == lastHash) {
		return;
	}
	lastHash = hash;
	everSent = true;

	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

void HIDSysex::sendKeyboardStateIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr || currentSong == nullptr) {
		return;
	}

	// ONLY WHEN THE KEYBOARD IS ACTUALLY ON SCREEN.
	//
	// `keyboardState.currentLayout` is a property of the CLIP, not of what you are looking at - it
	// remembers whichever layout you last chose in the keyboard. Reading it unconditionally meant the
	// Cockpit announced "Isomorphic" while Cody was in the piano roll, which is the same fault this
	// whole push was written to fix: a plausible value reported for something that is not the case.
	//
	// 127 says "no keyboard here", and the host hides the panel rather than showing a stale layout.
	// It cannot collide with a real layout: the enum has eight entries.
	UI* activeUI = getCurrentUI();
	const bool onKeyboard = activeUI != nullptr && activeUI->getUIType() == UIType::KEYBOARD_SCREEN;
	const uint8_t layout = onKeyboard ? (uint8_t)clip->keyboardState.currentLayout : 127;
	const uint8_t root = (uint8_t)(currentSong->key.rootNote % 12);
	const Scale scale = currentSong->getCurrentScale();
	const char* scaleName = getScaleName(scale);

	// THE RANGE, asked of the layout itself rather than computed here. Only the layout knows how its
	// pads map to notes - that is the whole difference between these layouts - and duplicating any of
	// that arithmetic would be a second, drifting copy of the thing this message exists to report.
	//
	// A layout with no notes (drums, the chord library) answers -1, and then there is no range to
	// send rather than a made-up one.
	int32_t lo = -1, hi = -1;
	if (onKeyboard) {
		keyboardScreen.noteRange(lo, hi);
	}

	static uint8_t lastLayout = 0xff;
	static uint8_t lastRoot = 0xff;
	static int32_t lastLo = -2, lastHi = -2;
	static uint8_t lastScaleLen = 0xff;
	uint8_t nameLen = 0;
	while (scaleName != nullptr && scaleName[nameLen] != 0 && nameLen < 20) {
		nameLen++;
	}
	if (!force && layout == lastLayout && root == lastRoot && lo == lastLo && hi == lastHi && nameLen == lastScaleLen) {
		return;
	}
	lastLayout = layout;
	lastRoot = root;
	lastLo = lo;
	lastHi = hi;
	lastScaleLen = nameLen;

	uint8_t msg[32];
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02; // the longer frame form, which is what new tags use
	msg[i++] = SysEx::SysexCommands::KeyboardState;
	msg[i++] = layout;
	msg[i++] = root;
	// A note is 0..127 and fits a single 7-bit byte; 127 means "no range", which is distinct from any
	// real note because a layout that HAS notes always reports a low below its high.
	msg[i++] = (uint8_t)(lo < 0 ? 127 : (lo & 0x7f));
	msg[i++] = (uint8_t)(hi < 0 ? 127 : (hi & 0x7f));
	msg[i++] = nameLen;
	for (uint8_t k = 0; k < nameLen; k++) {
		msg[i++] = (uint8_t)(scaleName[k]) & 0x7f; // scale names are ASCII by contract
	}
	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// Chroma WRITE direction (0x44): a one-slot inbox for an inbound voicing-mod. The MIDI-receive context stashes it;
// the Harmonic layout drains it on the UI/graphics thread (KeyboardLayoutHarmonic::renderPads), where re-voicing
// and the 0x43 re-broadcast already run safely. Last-write-wins; a single bool flag, no lock needed — both producer
// and consumer run in the Deluge's cooperative main loop. Worst case is a dropped/late mod, never a crash. This path
// deliberately never touches audio; the mod is HEARD next time the chord is played (buildVoicing reads the new state).
static volatile bool gChordModPending = false;
static uint8_t gChordModType = 0;
static uint8_t gChordModValue = 0;

// Chroma (0x4A): WHICH MENU IS OPEN, WHAT IS IN IT, AND WHERE THE CURSOR IS.
//
// The view push (0x45) says "the sound editor" and stops. It never said which menu, so a host could
// not follow the player through the instrument's own menus - the Cockpit's body had nothing to draw
// from, and the only description of where you were was four characters on the display.
//
//   F0 00 21 7B 01 02 4A 00 F7                                        no menu is open
//   F0 00 21 7B 01 02 4A 01 <kind> <depth> <total> <cursor> <first> <count>
//                           <titleLen> <title...> { <nameLen> <name...> } x count F7
//
// NAMES GO OUT IN FULL ENGLISH, even on a 7-segment unit whose own display abbreviates them. The
// item's l10n id is looked up in the English table directly. An item that builds its name at run
// time (a patch cable, say) has no id to look up, and sends whatever it calls itself.
//
// ONLY THE ENTRIES THE PLAYER CAN REACH. The same isRelevant() test the menu itself uses when the
// select encoder turns, so the list a host draws is the list the encoder walks - not a longer one
// with entries that cannot be selected on this sound.
//
// A LONG MENU IS A WINDOW around the cursor. Sixteen entries of twenty characters is 336 bytes; the
// sysex buffer is 1024 and this must never be the frame that overruns it.
void HIDSysex::sendMenuContextIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	constexpr int32_t kMaxItems = 16;
	constexpr int32_t kMaxName = 20;
	constexpr int32_t kMaxTitle = 24;
	constexpr int32_t kMaxRelevant = 64;

	uint8_t msg[16 + 1 + kMaxTitle + kMaxItems * (1 + kMaxName)];
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02;
	msg[i++] = SysEx::SysexCommands::MenuContext;

	uint32_t hash = 2166136261u;
	auto put = [&](uint8_t b) {
		msg[i++] = b;
		hash = (hash ^ b) * 16777619u;
	};
	auto putText = [&](std::string_view text, int32_t max) {
		int32_t n = (int32_t)text.size();
		if (n > max) {
			n = max;
		}
		put((uint8_t)n);
		for (int32_t k = 0; k < n; k++) {
			// 7-bit and printable: a stray high byte would end the sysex frame early.
			const uint8_t c = (uint8_t)text[k] & 0x7f;
			put((c >= 32) ? c : '?');
		}
	};
	// The item's own name if it builds one at run time, otherwise the English for its l10n id.
	auto englishName = [](const MenuItem* item) -> std::string_view {
		std::string_view shown = item->getName();
		if (shown == deluge::l10n::getView(item->name)) {
			return deluge::l10n::getView(deluge::l10n::built_in::english, item->name);
		}
		return shown;
	};
	auto englishTitle = [](const MenuItem* item) -> std::string_view {
		std::string_view shown = item->getTitle();
		if (shown == deluge::l10n::getView(item->title)) {
			return deluge::l10n::getView(deluge::l10n::built_in::english, item->title);
		}
		return shown;
	};

	MenuItem* current = (getCurrentUI() == &soundEditor) ? soundEditor.getCurrentMenuItem() : nullptr;
	if (current == nullptr) {
		put(0); // no menu is open - a real answer, so a host can clear its list
	}
	else {
		put(1);
		if (current->isSubmenu()) {
			auto* sub = static_cast<deluge::gui::menu_item::Submenu*>(current);
			const MenuItem* selected = sub->hostCurrentItem();
			const MenuItem* relevant[kMaxRelevant];
			int32_t total = 0;
			int32_t cursor = -1;
			for (MenuItem* item : sub->hostItems()) {
				if (total >= kMaxRelevant) {
					break;
				}
				if (!item->isRelevant(soundEditor.currentModControllable, soundEditor.currentSourceIndex)) {
					continue;
				}
				if (item == selected) {
					cursor = total;
				}
				relevant[total++] = item;
			}
			int32_t first = 0;
			int32_t count = total;
			if (total > kMaxItems) {
				count = kMaxItems;
				first = ((cursor < 0) ? 0 : cursor) - kMaxItems / 2;
				if (first < 0) {
					first = 0;
				}
				if (first > total - kMaxItems) {
					first = total - kMaxItems;
				}
			}
			put(0); // kind: a list
			put((uint8_t)(soundEditor.navigationDepth & 0x7f));
			put((uint8_t)total);
			put((cursor < 0) ? 0x7f : (uint8_t)cursor);
			put((uint8_t)first);
			put((uint8_t)count);
			putText(englishTitle(current), kMaxTitle);
			for (int32_t k = 0; k < count; k++) {
				putText(englishName(relevant[first + k]), kMaxName);
			}
		}
		else {
			// A single item being edited. Its VALUE is already on the wire as the display text
			// (0x42), so this says only what the value belongs to.
			put(1); // kind: an item
			put((uint8_t)(soundEditor.navigationDepth & 0x7f));
			put(0);
			put(0x7f);
			put(0);
			put(0);
			putText(englishTitle(current), kMaxTitle);
		}
	}

	static uint32_t lastHash = 0;
	static bool everSent = false;
	if (!force && everSent && hash == lastHash) {
		return;
	}
	lastHash = hash;
	everSent = true;

	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// THE CLIP THE PLAYER HAS SELECTED (1.3.123): the one the gold knobs are aimed at, found the way 0x4D finds the
// selected track. The sound and sample reports used the song's CURRENT clip, which is the clip last OPENED and does
// not follow a different track picked in the song grid - Cody, 2026-11-24: switching to another synth left the
// previous synth's sample on the Sound page, "and that's a bug in my eyes". Null when the whole song is selected.
// The knob focus pointer is only COMPARED against the song's own clips, never followed.
static Clip* selectedClip() {
	if (currentSong == nullptr) {
		return nullptr;
	}
	TimelineCounter* focus =
	    view.activeModControllableModelStack.toWithTimelineCounter()->getTimelineCounterAllowNull();
	if (focus == nullptr) {
		return currentSong->getCurrentClip();
	}
	if (focus == currentSong) {
		return nullptr;
	}
	for (int32_t c = 0; c < currentSong->sessionClips.getNumElements(); c++) {
		Clip* clip = currentSong->sessionClips.getClipAtIndex(c);
		if (clip == focus) {
			return clip;
		}
	}
	for (int32_t c = 0; c < currentSong->arrangementOnlyClips.getNumElements(); c++) {
		Clip* clip = currentSong->arrangementOnlyClips.getClipAtIndex(c);
		if (clip == focus) {
			return clip;
		}
	}
	return currentSong->getCurrentClip();
}

// Chroma (0x4B): the sound in focus, as the instrument holds it.
//
// A host that draws a sound from its own copy of a patch is drawing a guess. This is what lets it
// draw the oscillators, filters, envelopes and LFOs the player is actually hearing. Every level is
// the 0-50 the menus show, so the numbers on a host match the numbers on the instrument.
//
// WHICH sound: the one open in the sound editor if there is one (that is how a kit row is reached),
// otherwise the synth on the track the player is on. Anything else is "none", said out loud.
void HIDSysex::sendSoundStateIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	using namespace deluge::modulation::params;

	constexpr int32_t kMaxName = 24;
	uint8_t msg[7 + 39 + 1 + kMaxName + 1 + 1];
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02;
	msg[i++] = SysEx::SysexCommands::SoundState;

	uint32_t hash = 2166136261u;
	auto put = [&](uint8_t b) {
		msg[i++] = b & 0x7f;
		hash = (hash ^ b) * 16777619u;
	};

	// What the player calls this sound: the kit row's name when a row is open in the editor,
	// otherwise the track's. Appended after the fixed fields, which an older reader ignores.
	const char* soundName = "";
	std::string drumName; // owns a kit row's name for as long as soundName points into it
	Sound* sound = nullptr;
	ParamManager* paramManager = nullptr;
	uint8_t source = 0;
	if (getCurrentUI() == &soundEditor && soundEditor.currentSound != nullptr
	    && soundEditor.currentParamManager != nullptr) {
		sound = soundEditor.currentSound;
		paramManager = soundEditor.currentParamManager;
		source = 1;
		if (soundEditor.editingKitRow()) {
			drumName = static_cast<SoundDrum*>(sound)->getDrumName();
			soundName = drumName.c_str();
		}
		else if (selectedClip() != nullptr && selectedClip()->output != nullptr) {
			soundName = selectedClip()->output->name.get();
		}
	}
	else if (currentSong != nullptr) {
		Clip* clip = selectedClip();
		if (clip != nullptr && clip->output != nullptr && clip->output->type == OutputType::SYNTH) {
			sound = static_cast<SoundInstrument*>(clip->output);
			paramManager = &clip->paramManager;
			soundName = clip->output->name.get();
		}
		// A KIT: the row selected on the Deluge, with no editor open (1.3.125), the way 0x55 finds it. Cody: "kits are
		// not showing up in the sound section AT ALL ... we need to be able to handle kits as easily as synths".
		// Membership first - getDrumIndex only compares pointers - and only a row whose sound has its own
		// parameters (they live with its notes) is reported.
		else if (clip != nullptr && clip->output != nullptr && clip->output->type == OutputType::KIT
		         && clip->type == ClipType::INSTRUMENT) {
			Kit* kit = static_cast<Kit*>(clip->output);
			if (kit->selectedDrum != nullptr && kit->getDrumIndex(kit->selectedDrum) >= 0
			    && kit->selectedDrum->type == DrumType::SOUND) {
				SoundDrum* drum = static_cast<SoundDrum*>(kit->selectedDrum);
				NoteRow* noteRow = static_cast<InstrumentClip*>(clip)->getNoteRowForDrum(drum);
				if (noteRow != nullptr) {
					sound = drum;
					paramManager = &noteRow->paramManager;
					source = 2;
					soundName = drum->getName();
				}
			}
		}
	}
	if (sound != nullptr && (paramManager == nullptr || !paramManager->containsAnyMainParamCollections())) {
		sound = nullptr; // a sound whose parameters cannot be read is not one a host can draw
	}

	if (sound == nullptr) {
		put(0);
	}
	else {
		PatchedParamSet* params = paramManager->getPatchedParamSet();
		auto level = [&](int32_t p) {
			int32_t v = computeCurrentValueForStandardMenuItem(params->getValue(p));
			if (v < 0) {
				v = 0;
			}
			if (v > kMaxMenuValue) {
				v = kMaxMenuValue;
			}
			put((uint8_t)v);
		};
		put(1);
		put(source);
		put(util::to_underlying(sound->getSynthMode()));
		put(util::to_underlying(sound->polyphonic));
		put(util::to_underlying(sound->sources[0].oscType));
		put(util::to_underlying(sound->sources[1].oscType));
		level(LOCAL_OSC_A_VOLUME);
		level(LOCAL_OSC_B_VOLUME);
		level(LOCAL_NOISE_VOLUME);
		put(util::to_underlying(sound->lpfMode));
		level(LOCAL_LPF_FREQ);
		level(LOCAL_LPF_RESONANCE);
		put(util::to_underlying(sound->hpfMode));
		level(LOCAL_HPF_FREQ);
		level(LOCAL_HPF_RESONANCE);
		for (int32_t e = 0; e < 4; e++) {
			level(LOCAL_ENV_0_ATTACK + e);
			level(LOCAL_ENV_0_DECAY + e);
			level(LOCAL_ENV_0_SUSTAIN + e);
			level(LOCAL_ENV_0_RELEASE + e);
		}
		// LFO 1 and 3 are global, 2 and 4 are per voice; the wire carries them in the order a player counts.
		static const int32_t lfoRate[4] = {GLOBAL_LFO_FREQ_1, LOCAL_LFO_LOCAL_FREQ_1, GLOBAL_LFO_FREQ_2,
		                                   LOCAL_LFO_LOCAL_FREQ_2};
		for (int32_t l = 0; l < 4; l++) {
			put(util::to_underlying(sound->lfoConfig[l].waveType));
			level(lfoRate[l]);
		}
		int32_t n = (int32_t)strlen(soundName);
		if (n > kMaxName) {
			n = kMaxName;
		}
		put((uint8_t)n);
		for (int32_t k = 0; k < n; k++) {
			const uint8_t c = (uint8_t)soundName[k] & 0x7f;
			put((c >= 32) ? c : '?'); // 7-bit and printable: a stray high byte would end the frame early
		}
		// Card state of the instrument this sound belongs to (for a kit row, the kit):
		//   bit 0  edited since it was last loaded from or saved to the card
		//   bit 1  believed to have a file on the card at all
		uint8_t cardState = 0;
		if (selectedClip() != nullptr) {
			Output* owner = selectedClip()->output;
			if (owner != nullptr && owner->type != OutputType::AUDIO) {
				auto* instrument = static_cast<Instrument*>(owner);
				cardState = (instrument->changedSinceCard ? 1 : 0) | (instrument->mightExistOnCard ? 2 : 0);
			}
		}
		put(cardState);
	}

	static uint32_t lastHash = 0;
	static bool everSent = false;
	if (!force && everSent && hash == lastHash) {
		return;
	}
	lastHash = hash;
	everSent = true;

	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// Chroma (0x55): THE SAMPLE IN FOCUS - what file the sound plays, how a note plays it (CUT, ONCE, LOOP
// or STRETCH), and where it starts, ends and loops. The frame is laid out in hid/sample_frame.h.
//
// Cody, 2026-11-24: "now with the sound tab looking so good we need to talk sampling". Before this a
// host could say only that an oscillator WAS a sample. The setting that decides whether a hit stops
// with its step or rings out was visible in one place, three menus deep, one row at a time.
//
// WHICH sound: the one in the sound editor if it is open (0x4B's choice), otherwise the synth on the
// current track (0x4B's choice again), otherwise - new here - the selected row of the current kit, so
// hitting a kit pad is enough to see what that row will do.
//
// READS ONLY, and only of what is already in memory: no zone is created, no file is opened, nothing
// is loaded. Every pointer on the way down is checked, and every downcast is proven by the object
// itself (its type field or its own virtual), not by what screen is showing.
//
// WHAT THE CARD LOCK DOES AND DOES NOT DO. The walk is skipped while the card routine holds its lock,
// which covers the one moment a kit's row list is unlinked and relinked: a save. It does NOT cover
// loads - this also runs in the middle of one, from display ticks and from MIDI serviced while the
// loader yields. It is safe there because loaders build into objects that are not attached to the
// current song until they are whole (a preset into a spare instrument, a song into preLoadedSong).
// A skipped report is NOT marked as sent - a forced one is owed until it goes.
void HIDSysex::sendSampleStateIfChanged(bool force) {
	static bool owed = false; // a forced send that could not go yet
	static uint32_t lastHash = 0;
	static bool everSent = false;

	if (lastHidCable == nullptr) {
		return;
	}
	owed = owed || force;
	if (sdRoutineLock) {
		return;
	}
	using namespace deluge::hid;
	using namespace deluge::modulation::params;

	sample_frame::Focus focus;
	Sound* sound = nullptr;
	ParamManager* paramManager = nullptr;
	Clip* clip = selectedClip();
	Output* output = (clip != nullptr) ? clip->output : nullptr;
	Kit* kit = (output != nullptr && output->type == OutputType::KIT) ? static_cast<Kit*>(output) : nullptr;
	Drum* rowDrum = nullptr; // the kit row this sound is, when it is one
	NoteRow* noteRow = nullptr;
	int32_t noteRowIndex = -1;

	if (getCurrentUI() == &soundEditor && soundEditor.currentSound != nullptr
	    && soundEditor.currentParamManager != nullptr) {
		sound = soundEditor.currentSound;
		paramManager = soundEditor.currentParamManager;
		focus.source = sample_frame::kSoundEditor;
		if (sound->isDrum()) { // asked of the object: only a SoundDrum answers yes
			SoundDrum* drum = static_cast<SoundDrum*>(sound);
			rowDrum = drum;
			focus.name = drum->getName(); // the row's own storage: no copy, so no allocation on every poll
		}
		else if (output != nullptr) {
			focus.name = output->name.get();
		}
	}
	else if (output != nullptr && output->type == OutputType::SYNTH) {
		sound = static_cast<SoundInstrument*>(output);
		paramManager = &clip->paramManager;
		focus.source = sample_frame::kSynthTrack;
		focus.name = output->name.get();
	}
	// Membership first: getDrumIndex only compares pointers, so it is the one check that is safe to make
	// on a selection that no longer exists. Only a row found in this kit is then looked at.
	else if (kit != nullptr && kit->selectedDrum != nullptr && kit->getDrumIndex(kit->selectedDrum) >= 0
	         && kit->selectedDrum->type == DrumType::SOUND) {
		SoundDrum* drum = static_cast<SoundDrum*>(kit->selectedDrum);
		sound = drum;
		rowDrum = drum;
		focus.source = sample_frame::kKitRow;
		focus.name = drum->getName();
	}
	// WHICH ROW: its place among the clip's note rows, which is the order the pads show them in. NOT its
	// place in the kit's own list of drums - every save relinks that list, so the number would change
	// under a player who touched nothing.
	if (rowDrum != nullptr && kit != nullptr && clip->type == ClipType::INSTRUMENT) {
		noteRow = static_cast<InstrumentClip*>(clip)->getNoteRowForDrum(rowDrum, &noteRowIndex);
		// A row's parameters live with its notes. Without them the row is still reported; only the
		// amp release is left out.
		if (noteRow != nullptr && paramManager == nullptr) {
			paramManager = &noteRow->paramManager;
		}
	}
	if (paramManager != nullptr && !paramManager->containsAnyMainParamCollections()) {
		paramManager = nullptr;
	}

	if (sound != nullptr && sound->getSynthMode() != SynthMode::FM) {
		focus.row = (noteRow != nullptr) ? noteRowIndex : -1;
		for (int32_t s = 0; s < kNumSources && s < 2; s++) {
			Source& source = sound->sources[s];
			// A sample oscillator's zones are MultisampleRanges. Anything else in that array is a
			// wavetable's, and must not be read as one.
			if (source.oscType != OscType::SAMPLE || source.ranges.elementSize != sizeof(MultisampleRange)) {
				continue;
			}
			const int32_t zones = source.ranges.getNumElements();
			if (zones <= 0) {
				continue; // a sample oscillator with no zone yet has nothing to report, and none is made
			}
			int32_t zone = 0;
			if (focus.source == sample_frame::kSoundEditor && soundEditor.currentSourceIndex == s
			    && soundEditor.currentMultiRangeIndex > 0 && soundEditor.currentMultiRangeIndex < zones) {
				zone = soundEditor.currentMultiRangeIndex; // the zone the editor is on
			}
			auto* range = static_cast<MultisampleRange*>(source.ranges.getElement(zone));
			if (range == nullptr) {
				continue;
			}
			SampleHolderForVoice& holder = range->sampleHolder;
			if (holder.audioFileType != AudioFileType::SAMPLE) {
				continue; // the holder says what it holds; anything else is not read as a sample
			}
			sample_frame::Osc& osc = focus.osc[s];
			osc.mode = (uint8_t)util::to_underlying(source.repeatMode);
			osc.reversed = source.sampleControls.reversed;
			osc.pitchSpeedIndependent = source.sampleControls.pitchAndSpeedAreIndependent;
			osc.speed = source.timeStretchAmount;
			osc.transpose = holder.transpose;
			osc.cents = holder.cents;
			osc.loopLocked = holder.loopLocked;
			osc.start = holder.startPos;
			osc.loopStart = holder.loopStartPos;
			osc.loopEnd = holder.loopEndPos;
			osc.zone = (uint8_t)((zone > 127) ? 127 : zone);
			osc.zones = (uint8_t)((zones > 127) ? 127 : zones);
			osc.path = holder.filePath.get();
			const auto* sample = (holder.audioFile != nullptr && holder.audioFile->type == AudioFileType::SAMPLE)
			                         ? static_cast<const Sample*>(holder.audioFile)
			                         : nullptr;
			if (sample != nullptr) {
				osc.loaded = true;
				osc.length = sample->lengthInSamples;
				osc.rate = sample->sampleRate;
				osc.channels = sample->numChannels;
				// The end as it will play: the stored one may sit beyond the end of the file.
				osc.end = (holder.endPos < sample->lengthInSamples) ? holder.endPos : sample->lengthInSamples;
			}
			else {
				osc.end = holder.endPos; // as stored: with no file to measure against it may be a placeholder
			}
			focus.oscMask |= (uint8_t)(1 << s);
		}
		focus.present = (focus.oscMask != 0);
	}

	if (focus.present) {
		if (paramManager != nullptr) {
			PatchedParamSet* params = paramManager->getPatchedParamSet();
			if (params != nullptr) {
				int32_t release = computeCurrentValueForStandardMenuItem(params->getValue(LOCAL_ENV_0_RELEASE));
				focus.ampRelease = (release < 0) ? 0 : ((release > kMaxMenuValue) ? kMaxMenuValue : release);
			}
		}
		if (output != nullptr && output->type != OutputType::AUDIO
		    && static_cast<Instrument*>(output)->changedSinceCard) {
			focus.state |= sample_frame::kStateEdited;
		}
		// Whether this sound obeys note length is NOT reported yet (kStateTailsKnown stays clear):
		// asking needs a model stack built for the row, which is a change for a flash of its own.
	}

	uint8_t msg[sample_frame::kMaxFrame];
	const int32_t length = sample_frame::pack(focus, msg);
	uint32_t hash = 2166136261u;
	for (int32_t k = 0; k < length; k++) {
		hash = (hash ^ msg[k]) * 16777619u;
	}
	if (!owed && everSent && hash == lastHash) {
		return;
	}
	// Whole or not at all, and never remembered as sent when it was not.
	// (A queue that has overflowed reports its room as a negative number, which arrives here as an
	// enormous one. More room than the queue has is no room.)
	const size_t space = lastHidCable->sendBufferSpace();
	if (space < (size_t)length + 64 || space > 0x10000) {
		return;
	}
	lastHash = hash;
	everSent = true;
	owed = false;
	lastHidCable->sendSysex(msg, length);
}

// Chroma (0x5B): HEALTH - is the USB audio stream really sending.
//
//   F0 00 21 7B 01 02 5B <ver=1> <flags> <interval ms x2> <started x2> <standby x2> <retired x2>
//                        <notReady x2> <dmaBusy x2> <recoveries> <resyncs> <underruns x2> <lead x2, +8192>
//                        [1.3.120: <watchdog> <pipeCtr x3> <d0FifoSel x3> <d0FifoCtr x3>] F7
//
// Cody, 2026-11-24: "after a while the scope and waveform gets stuck ... the wire is silent yet the scope is
// stuck showing data". Measured that day: the computer was receiving the same quarter second of his last
// note over and over, bit for bit. That is what a host does when an audio endpoint stops answering - it
// replays what it has. Nothing on the wire said the stream had stopped, so a host drew the replay as live.
//
// The counts are what happened since the previous report, and <interval> is how long that was, so a host
// can tell "a thousand packets a second" from "none". <flags>: 1 a host has the stream open, 2 the write
// timer is running, 4 the driver holds a transfer, 8 a packet is with the DMA controller, 16 the DMA port
// is set up, 32 the reader is primed. Counts stop at 16383, the two single-byte ones at 127.
//
// Reads only. About once a second, on the pad poll. Says nothing while no host is streaming and the flags
// have not changed.
void HIDSysex::sendHealthIfDue(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	using deluge::processing::engines::USBAudioStream;
	static bool everSent = false;
	static uint32_t lastAt = 0;
	static uint8_t lastFlags = 0;
	static USBAudioStream::Health last{};

	const uint32_t now = AudioEngine::audioSampleTimer;
	const uint32_t elapsed = now - lastAt;
	if (!force && everSent && elapsed < kSampleRate) {
		return;
	}
	USBAudioStream::Health health{};
	USBAudioStream::readHealth(health);
	const uint8_t flags =
	    (uint8_t)((health.streaming ? 1 : 0) | (health.timerRunning ? 2 : 0) | (health.transferInFlight ? 4 : 0)
	              | (health.dmaBusy ? 8 : 0) | (health.dmaConfigured ? 16 : 0) | (health.primed ? 32 : 0)
	              | (health.returnActive ? 64 : 0));
	// A flag that only flickers (a transfer in flight, a packet with the controller) is not news by itself.
	const uint8_t steady = flags & (1 | 2 | 16 | 32 | 64);
	if (!force && everSent && !health.streaming && !health.returnActive
	    && steady == (lastFlags & (1 | 2 | 16 | 32 | 64))) {
		lastAt = now;
		last = health;
		return;
	}
	if (lastHidCable->sendBufferSpace() < 96 || lastHidCable->sendBufferSpace() > 0x10000) {
		return; // not sent, so not remembered: the counts carry over to the next one
	}

	uint8_t msg[80];
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02;
	msg[i++] = SysEx::SysexCommands::Health;
	auto put = [&](uint32_t b) { msg[i++] = (uint8_t)(b & 0x7f); };
	auto put14 = [&](uint32_t v) {
		if (v > 0x3fff) {
			v = 0x3fff;
		}
		put(v >> 7);
		put(v);
	};
	auto put7 = [&](uint32_t v) { put((v > 0x7f) ? 0x7f : v); };
	put(1);
	put(flags);
	put14(everSent ? (uint32_t)(((uint64_t)elapsed * 1000u) / kSampleRate) : 0u);
	put14(health.started - last.started);
	put14(health.standby - last.standby);
	put14(health.retired - last.retired);
	put14(health.fifoNotReady - last.fifoNotReady);
	put14(health.dmaBusyPasses - last.dmaBusyPasses);
	put7(health.recoveries - last.recoveries);
	put7(health.resyncs - last.resyncs);
	put14(health.underruns - last.underruns);
	int32_t lead = health.lead;
	lead = (lead < -8192) ? -8192 : ((lead > 8191) ? 8191 : lead);
	put14((uint32_t)(lead + 8192));
	// 1.3.120: the watchdog's count since the last report, and three registers read as the report was made, so a
	// stall can be seen from the hardware's side. 16 bits each, sent as 3 x 7.
	put7(health.watchdog - last.watchdog);
	auto put16 = [&](uint16_t v) {
		put(v >> 14);
		put(v >> 7);
		put(v);
	};
	put16(health.pipeControl);
	put16(health.fifoSelect);
	put16(health.fifoControl);
	// 1.3.123: each gold knob's moves and direction reversals since the last report, lower knob first.
	{
		using namespace deluge::hid::encoders;
		static uint32_t lastMoves[2] = {0, 0};
		static uint32_t lastReversals[2] = {0, 0};
		for (int32_t k = 0; k < 2; k++) {
			put14(modEncoderMoves[k] - lastMoves[k]);
			put14(modEncoderReversals[k] - lastReversals[k]);
			lastMoves[k] = modEncoderMoves[k];
			lastReversals[k] = modEncoderReversals[k];
		}
	}
	// 1.3.127: THE ENGINE AND MEMORY. Cody, 2026-11-24, on a dense chord across his BOD patches: "I can tell this
	// track is clipping ... the BOD patches are costly for CPU" and "a way to track Delly CPU and memory usage while
	// we're watching the device". The Deluge's own strain figure (cpuDireness, 0-14: what it culls voices by), the
	// voices sounding, renders that cut voices and output samples at the ceiling since the last report, and the RAM
	// still free, in KB: internal (its own SRAM) and external (SDRAM, where samples and their cache live).
	{
		static uint32_t lastCulls = 0, lastClipped = 0;
		put7((uint32_t)(AudioEngine::cpuDireness < 0 ? 0 : AudioEngine::cpuDireness));
		put7((uint32_t)AudioEngine::getNumVoices());
		put14(AudioEngine::healthCulls - lastCulls);
		put14(AudioEngine::healthClipped - lastClipped);
		lastCulls = AudioEngine::healthCulls;
		lastClipped = AudioEngine::healthClipped;
		auto freeKb = [](std::initializer_list<int32_t> which) {
			uint64_t bytes = 0;
			for (int32_t r : which) {
				auto& spaces = GeneralMemoryAllocator::get().regions[r].emptySpaces;
				for (int32_t k = 0; k < spaces.getNumElements(); k++) {
					bytes += static_cast<EmptySpaceRecord*>(spaces.getElementAddress(k))->length;
				}
			}
			return (uint32_t)(bytes / 1024);
		};
		const uint32_t internalKb = freeKb({MEMORY_REGION_INTERNAL, MEMORY_REGION_INTERNAL_SMALL});
		const uint32_t externalKb =
		    freeKb({MEMORY_REGION_EXTERNAL, MEMORY_REGION_EXTERNAL_SMALL, MEMORY_REGION_STEALABLE});
		put14(internalKb);
		const uint32_t ext = (externalKb > 0x1fffff) ? 0x1fffff : externalKb;
		put(ext >> 14);
		put(ext >> 7);
		put(ext);
	}
	// 1.3.128: THE RETURN, audio the computer sends the Deluge. Cody, 2026-11-24: "I can make the inputs work but
	// the outputs do not". Frames received and frames dropped since the last report, the loudest sample since the
	// last report (0-32767: frames of silence still count as frames), then the settings that decide where it goes:
	// the pair mixed into the master (0 none), the return level (0-50), the latency compensation in ms, and how
	// many pairs the return carries.
	{
		static uint32_t lastRxFrames = 0, lastRxDropped = 0;
		auto put21 = [&](uint32_t v) {
			if (v > 0x1fffff) {
				v = 0x1fffff;
			}
			put(v >> 14);
			put(v >> 7);
			put(v);
		};
		put21(health.rxFrames - lastRxFrames);
		put14(health.rxDropped - lastRxDropped);
		put16(health.rxPeak);
		lastRxFrames = health.rxFrames;
		lastRxDropped = health.rxDropped;
		put7(USBAudioStream::getMasterReturnPair());
		put7(USBAudioStream::getReturnLevel());
		put7(USBAudioStream::getCompensationMs());
		put7(USBAudioStream::numReturnPairs());
		put7(USBAudioStream::getMainPair());   // 1.3.132: the pair carrying the main mix, 0 none
		put7(USBAudioStream::getReverbPair()); // 1.3.133: the pair carrying the reverb return, 0 the main mix
	}
	msg[i++] = 0xf7;

	everSent = true;
	lastAt = now;
	lastFlags = flags;
	last = health;
	lastHidCable->sendSysex(msg, i);
}

// Chroma (0x57): THE CHORD BEING HELD on the keyboard, in the words the instrument's own display uses.
//
//   F0 00 21 7B 01 02 57 <len> <text> F7        len 0: no chord is held
//
// Cody, 2026-11-24, of the live note names beside the pads: "can we also include the calculated chord".
// The Deluge already works the name out - two or more held notes are matched against its chord table and
// scrolled across the display as "IV  Fmaj7". A host that named the chord itself could disagree with the
// instrument it is mirroring; this sends the instrument's answer instead.
//
// Asked on the pad poll, like the pad notes. Reads the held notes and the key; draws and changes nothing.
void HIDSysex::sendHeldChordIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	char name[64];
	name[0] = '\0';
	UI* ui = getCurrentUI();
	if (currentSong != nullptr && ui != nullptr && ui->getUIType() == UIType::KEYBOARD_SCREEN) {
		keyboardScreen.heldChordName(name);
	}
	name[63] = '\0';

	uint8_t msg[7 + 1 + 64 + 1];
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02;
	msg[i++] = SysEx::SysexCommands::HeldChord;
	uint32_t hash = 2166136261u;
	const int32_t n = (int32_t)strlen(name);
	msg[i++] = (uint8_t)n;
	for (int32_t k = 0; k < n; k++) {
		const uint8_t c = (uint8_t)name[k];
		const uint8_t safe = (c >= 32 && c < 127) ? c : '?'; // 7-bit and printable, or the frame ends early
		msg[i++] = safe;
		hash = (hash ^ safe) * 16777619u;
	}
	hash = (hash ^ (uint8_t)n) * 16777619u;

	static uint32_t lastHash = 0;
	static bool everSent = false;
	if (!force && everSent && hash == lastHash) {
		return;
	}
	lastHash = hash;
	everSent = true;
	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// Chroma (0x5C): THE ANALOGUE PROBE - a diagnostic, not a feature.
//
//   F0 00 21 7B 01 02 5C <ver=1> 8 x (<hi> <lo>) F7      each a 14-bit reading, AN0 first
//
// The converter's eight inputs, as deluge.cpp last read them (it converts one every 200 ms, so a full round
// takes 1.6 s). Built to answer one question - does turning the panel's VOLUME knob move any of them - and
// to be taken out again once it is answered. Sent when any reading moves by more than a small margin, so
// the noise on an input with nothing connected does not flood the wire.
// Chroma (0x58): EVERY ROW OF THE CURRENT KIT, bottom row first - the order the grid stacks them.
//
//   F0 00 21 7B 01 02 58 01 <total> <first> <count> <selected> <scroll>
//        count x ( <flags> <channel> <note> <len> <name...> ) F7
//
//   total     rows in the kit (7F capped); 0 with nothing after it: the track in focus is not a kit
//   first     the index of the first row in this frame; a kit with more rows than fit is sent in pages
//   selected  the row the Deluge has selected (7F none); scroll: the bottom row the grid shows
//   flags     bits 0-1 kind (0 sound, 1 MIDI, 2 gate, 3 none), bit 2 muted, bit 3 its oscillator A plays a sample
//   channel   MIDI/gate rows: the channel (0-15); note: a MIDI row's note. 0 for a sound row
//
// Cody, 2026-11-24: "this is kit management here ... organize the tab around ALL the sounds and collapse them".
// 0x4B and 0x55 describe ONE row, the selected one; this is the list a host draws the others from.
//
// READS ONLY, of what is in memory. Skipped while the card routine holds its lock (a save relinks the rows), and
// a skipped report stays owed. De-duped by a hash of everything sent.
void HIDSysex::sendKitRowsIfChanged(bool force) {
	static bool owed = false;
	static uint32_t lastHash = 0;
	static bool everSent = false;
	if (lastHidCable == nullptr) {
		return;
	}
	owed = owed || force;
	if (sdRoutineLock) {
		return;
	}
	constexpr int32_t kMaxName = 12;
	constexpr int32_t kPage = 16;
	constexpr int32_t kMaxRows = 127;

	Clip* clip = selectedClip();
	InstrumentClip* kitClip = nullptr;
	Kit* kit = nullptr;
	if (clip != nullptr && clip->type == ClipType::INSTRUMENT && clip->output != nullptr
	    && clip->output->type == OutputType::KIT) {
		kitClip = static_cast<InstrumentClip*>(clip);
		kit = static_cast<Kit*>(clip->output);
	}

	auto describe = [&](int32_t r, uint8_t& flags, uint8_t& channel, uint8_t& note, const char*& name) {
		NoteRow* row = kitClip->noteRows.getElement(r);
		flags = 3;
		channel = 0;
		note = 0;
		name = "";
		if (row == nullptr) {
			return;
		}
		Drum* drum = row->drum;
		if (drum != nullptr && kit->getDrumIndex(drum) >= 0) {
			if (drum->type == DrumType::SOUND) {
				SoundDrum* sound = static_cast<SoundDrum*>(drum);
				flags = 0;
				if (sound->sources[0].oscType == OscType::SAMPLE) {
					flags |= 8;
				}
				name = sound->getName();
			}
			else if (drum->type == DrumType::MIDI) {
				flags = 1;
				channel = static_cast<MIDIDrum*>(drum)->channel & 0x0f;
				note = static_cast<MIDIDrum*>(drum)->note & 0x7f;
			}
			else if (drum->type == DrumType::GATE) {
				flags = 2;
				channel = static_cast<NonAudioDrum*>(drum)->channel & 0x0f;
			}
		}
		if (row->muted) {
			flags |= 4;
		}
	};

	int32_t total = 0;
	uint8_t selected = 0x7f;
	uint8_t scroll = 0;
	uint32_t hash = 2166136261u;
	auto mix = [&](uint8_t b) { hash = (hash ^ b) * 16777619u; };
	if (kitClip != nullptr) {
		total = kitClip->noteRows.getNumElements();
		if (total > kMaxRows) {
			total = kMaxRows;
		}
		scroll = (uint8_t)((kitClip->yScroll < 0) ? 0 : ((kitClip->yScroll > 126) ? 126 : kitClip->yScroll));
		for (int32_t r = 0; r < total; r++) {
			uint8_t flags, channel, note;
			const char* name;
			describe(r, flags, channel, note, name);
			NoteRow* row = kitClip->noteRows.getElement(r);
			if (row != nullptr && row->drum != nullptr && row->drum == kit->selectedDrum) {
				selected = (uint8_t)r;
			}
			mix(flags);
			mix(channel);
			mix(note);
			for (const char* c = name; *c != 0 && (c - name) < kMaxName; c++) {
				mix((uint8_t)*c);
			}
			mix(0);
		}
	}
	mix((uint8_t)total);
	mix(selected);
	mix(scroll);
	mix(kitClip != nullptr ? 1 : 0);
	if (!owed && everSent && hash == lastHash) {
		return;
	}
	const int32_t pages = (total == 0) ? 1 : (total + kPage - 1) / kPage;
	if (lastHidCable->sendBufferSpace() < pages * 300 || lastHidCable->sendBufferSpace() > 0x10000) {
		owed = true; // not sent, so not remembered
		return;
	}

	uint8_t msg[7 + 6 + kPage * (4 + kMaxName) + 1];
	for (int32_t first = 0; first == 0 || first < total; first += kPage) {
		int32_t i = 0;
		msg[i++] = 0xf0;
		msg[i++] = 0x00;
		msg[i++] = 0x21;
		msg[i++] = 0x7b;
		msg[i++] = 0x01;
		msg[i++] = 0x02;
		msg[i++] = SysEx::SysexCommands::KitRows;
		msg[i++] = 1;
		msg[i++] = (uint8_t)total;
		if (kitClip != nullptr) {
			const int32_t count = (total - first < kPage) ? (total - first) : kPage;
			msg[i++] = (uint8_t)first;
			msg[i++] = (uint8_t)count;
			msg[i++] = selected;
			msg[i++] = scroll;
			for (int32_t r = first; r < first + count; r++) {
				uint8_t flags, channel, note;
				const char* name;
				describe(r, flags, channel, note, name);
				msg[i++] = flags;
				msg[i++] = channel;
				msg[i++] = note;
				int32_t n = 0;
				while (n < kMaxName && name[n] != 0) {
					n++;
				}
				msg[i++] = (uint8_t)n;
				for (int32_t k = 0; k < n; k++) {
					const uint8_t c = (uint8_t)name[k] & 0x7f;
					msg[i++] = (c >= 32) ? c : '?';
				}
			}
		}
		msg[i++] = 0xf7;
		lastHidCable->sendSysex(msg, i);
		if (kitClip == nullptr) {
			break;
		}
	}
	owed = false;
	everSent = true;
	lastHash = hash;
}

// Chroma (0x59): THE EFFECTS OF THE SOUND IN FOCUS - the same sound 0x4B describes (the sound editor's, else the
// current synth, else the selected kit row), as the numbers its own menus show.
//
//   F0 00 21 7B 01 02 59 01 <has> [ fields ] F7              has 0: no sound in focus, nothing follows
//
//   fields, in order (0-50 unless said):
//     volume  pan(0-64, 32 centre: the menu's -32..32 plus 32)
//     delay: amount  rate  pingpong(0/1)  analog(0/1)  sync level(enum)  sync type(enum)
//     reverb amount
//     mod FX: type(enum)  rate  depth  feedback  offset
//     EQ: bass  treble  bass freq  treble freq
//     decimation  bitcrush  wavefold  saturation(the menu's raw value)
//     sidechain shape  portamento
//     arp: mode(enum)  rate  gate  octaves(raw)  preset(enum)
//
// Cody, 2026-11-24: "we need to add effects to the sound screen and arp and all the other settings from the Deluge
// front panel ... I want this all cleanly exposed to avoid ANY menu diving". READS ONLY. De-duped by a hash.
void HIDSysex::sendEffectsIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	using namespace deluge::modulation::params;
	Sound* sound = nullptr;
	ParamManager* paramManager = nullptr;
	Clip* clip = selectedClip();
	if (getCurrentUI() == &soundEditor && soundEditor.currentSound != nullptr
	    && soundEditor.currentParamManager != nullptr) {
		sound = soundEditor.currentSound;
		paramManager = soundEditor.currentParamManager;
	}
	else if (clip != nullptr && clip->output != nullptr && clip->output->type == OutputType::SYNTH) {
		sound = static_cast<SoundInstrument*>(clip->output);
		paramManager = &clip->paramManager;
	}
	else if (!sdRoutineLock && clip != nullptr && clip->output != nullptr && clip->output->type == OutputType::KIT
	         && clip->type == ClipType::INSTRUMENT) {
		Kit* kit = static_cast<Kit*>(clip->output);
		if (kit->selectedDrum != nullptr && kit->getDrumIndex(kit->selectedDrum) >= 0
		    && kit->selectedDrum->type == DrumType::SOUND) {
			SoundDrum* drum = static_cast<SoundDrum*>(kit->selectedDrum);
			NoteRow* noteRow = static_cast<InstrumentClip*>(clip)->getNoteRowForDrum(drum);
			if (noteRow != nullptr) {
				sound = drum;
				paramManager = &noteRow->paramManager;
			}
		}
	}
	if (sound != nullptr && (paramManager == nullptr || !paramManager->containsAnyMainParamCollections())) {
		sound = nullptr;
	}

	// THE SONG ITSELF (1.3.132): with no sound in focus and the gold knobs on the song, its own effects are reported -
	// `has` 2 - with the master compressor, the reverb room and the song's filters after the shared fields. Cody,
	// 2026-11-25: "show only master stuff ... so we can look at the compressor and FX stuff".
	bool songFocus = false;
	if (sound == nullptr && currentSong != nullptr && currentSong->paramManager.containsAnyMainParamCollections()) {
		TimelineCounter* focus =
		    view.activeModControllableModelStack.toWithTimelineCounter()->getTimelineCounterAllowNull();
		songFocus = (focus == currentSong);
	}

	uint8_t msg[7 + 2 + 64 + 1];
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02;
	msg[i++] = SysEx::SysexCommands::Effects;
	uint32_t hash = 2166136261u;
	auto put = [&](int32_t v) {
		const uint8_t b = (uint8_t)((v < 0) ? 0 : ((v > 127) ? 127 : v));
		msg[i++] = b;
		hash = (hash ^ b) * 16777619u;
	};
	put(1);
	if (songFocus) {
		put(2);
		GlobalEffectableForSong& g = currentSong->globalEffectable;
		UnpatchedParamSet* u = currentSong->paramManager.getUnpatchedParamSet();
		auto flat = [&](int32_t p) { put(computeCurrentValueForStandardMenuItem(u->getValue(p))); };
		auto put21 = [&](uint32_t v) {
			v = (v > 0x1fffff) ? 0x1fffff : v;
			msg[i++] = (uint8_t)((v >> 14) & 0x7f);
			msg[i++] = (uint8_t)((v >> 7) & 0x7f);
			msg[i++] = (uint8_t)(v & 0x7f);
			hash = (hash ^ v) * 16777619u;
		};
		// The fields a sound has, in the same places: what the song has none of is 0, its arpeggiator 7F.
		flat(UNPATCHED_VOLUME);
		put(computeCurrentValueForPan(u->getValue(UNPATCHED_PAN)) + 32);
		flat(UNPATCHED_DELAY_AMOUNT);
		flat(UNPATCHED_DELAY_RATE);
		put(g.delay.pingPong ? 1 : 0);
		put(g.delay.analog ? 1 : 0);
		put(util::to_underlying(g.delay.syncLevel));
		put(util::to_underlying(g.delay.syncType));
		flat(UNPATCHED_REVERB_SEND_AMOUNT);
		put(util::to_underlying(g.getModFXType()));
		flat(UNPATCHED_MOD_FX_RATE);
		flat(UNPATCHED_MOD_FX_DEPTH);
		flat(UNPATCHED_MOD_FX_FEEDBACK);
		flat(UNPATCHED_MOD_FX_OFFSET);
		flat(UNPATCHED_BASS);
		flat(UNPATCHED_TREBLE);
		flat(UNPATCHED_BASS_FREQ);
		flat(UNPATCHED_TREBLE_FREQ);
		flat(UNPATCHED_SAMPLE_RATE_REDUCTION);
		flat(UNPATCHED_BITCRUSHING);
		put(0); // wavefold: a sound's alone
		put(g.clippingAmount);
		flat(UNPATCHED_SIDECHAIN_SHAPE);
		put(0); // portamento: a sound's alone
		put(0x7f);
		put(0);
		put(0);
		put(0);
		put(0);
		// THE MASTER COMPRESSOR, as its menu shows it: threshold 0-50, then the display values - ratio x100 (": 1"),
		// attack ms x100, release ms x10, sidechain high-pass in Hz, blend in %.
		put(computeCurrentValueForHalfPrecisionMenuItem(u->getValue(UNPATCHED_COMPRESSOR_THRESHOLD)));
		put21((uint32_t)(g.compressor.getRatioForDisplay() * 100.0f + 0.5f));
		put21((uint32_t)(g.compressor.getAttackMS() * 100.0f + 0.5f));
		put21((uint32_t)(g.compressor.getReleaseMS() * 10.0f + 0.5f));
		put21((uint32_t)(g.compressor.getSidechainForDisplay() + 0.5f));
		put((int32_t)g.compressor.getBlendForDisplay());
		// THE REVERB ROOM (one for the whole Deluge), as its menu shows it.
		put((int32_t)std::round(AudioEngine::reverb.getRoomSize() * kMaxMenuValue));
		put((int32_t)std::round(AudioEngine::reverb.getDamping() * kMaxMenuValue));
		put((int32_t)std::round(AudioEngine::reverb.getWidth() * kMaxMenuValue));
		put(computeCurrentValueForPan(AudioEngine::reverbPan) + 32);
		put(util::to_underlying(AudioEngine::reverb.getModel()));
		put((int32_t)std::round(AudioEngine::reverb.getLPF() * kMaxMenuValue));
		put((int32_t)std::round(AudioEngine::reverb.getHPF() * kMaxMenuValue));
		// THE SONG'S FILTERS.
		put(util::to_underlying(g.lpfMode));
		flat(UNPATCHED_LPF_FREQ);
		flat(UNPATCHED_LPF_RES);
		put(util::to_underlying(g.hpfMode));
		flat(UNPATCHED_HPF_FREQ);
		flat(UNPATCHED_HPF_RES);
	}
	else if (sound == nullptr) {
		put(0);
	}
	else {
		put(1);
		PatchedParamSet* patched = paramManager->getPatchedParamSet();
		UnpatchedParamSet* unpatched = paramManager->getUnpatchedParamSet();
		auto level = [&](int32_t p) { put(computeCurrentValueForStandardMenuItem(patched->getValue(p))); };
		auto flat = [&](int32_t p) {
			put((unpatched != nullptr) ? computeCurrentValueForStandardMenuItem(unpatched->getValue(p)) : 0);
		};
		level(GLOBAL_VOLUME_POST_FX);
		put(computeCurrentValueForPan(patched->getValue(LOCAL_PAN)) + 32);
		level(GLOBAL_DELAY_FEEDBACK);
		level(GLOBAL_DELAY_RATE);
		put(sound->delay.pingPong ? 1 : 0);
		put(sound->delay.analog ? 1 : 0);
		put(util::to_underlying(sound->delay.syncLevel));
		put(util::to_underlying(sound->delay.syncType));
		level(GLOBAL_REVERB_AMOUNT);
		put(util::to_underlying(sound->getModFXType()));
		level(GLOBAL_MOD_FX_RATE);
		level(GLOBAL_MOD_FX_DEPTH);
		flat(UNPATCHED_MOD_FX_FEEDBACK);
		flat(UNPATCHED_MOD_FX_OFFSET);
		flat(UNPATCHED_BASS);
		flat(UNPATCHED_TREBLE);
		flat(UNPATCHED_BASS_FREQ);
		flat(UNPATCHED_TREBLE_FREQ);
		flat(UNPATCHED_SAMPLE_RATE_REDUCTION);
		flat(UNPATCHED_BITCRUSHING);
		level(LOCAL_FOLD);
		put(sound->clippingAmount);
		flat(UNPATCHED_SIDECHAIN_SHAPE);
		flat(UNPATCHED_PORTAMENTO);
		// A synth's arpeggiator lives with its clip, a kit row's with the row: asked only of the clip that owns it.
		ArpeggiatorSettings* arp = nullptr;
		if (sound->isDrum()) {
			arp = sound->getArpSettings(nullptr);
		}
		else if (clip != nullptr && clip->type == ClipType::INSTRUMENT && clip->output != nullptr
		         && clip->output->type == OutputType::SYNTH
		         && static_cast<Sound*>(static_cast<SoundInstrument*>(clip->output)) == sound) {
			arp = sound->getArpSettings(static_cast<InstrumentClip*>(clip));
		}
		if (arp != nullptr) {
			put(util::to_underlying(arp->mode));
			level(GLOBAL_ARP_RATE);
			flat(UNPATCHED_ARP_GATE);
			put(arp->numOctaves);
			put(util::to_underlying(arp->preset));
		}
		else {
			put(0x7f);
			put(0);
			put(0);
			put(0);
			put(0);
		}
	}

	static uint32_t lastHash = 0;
	static bool everSent = false;
	if (!force && everSent && hash == lastHash) {
		return;
	}
	if (lastHidCable->sendBufferSpace() < 64 || lastHidCable->sendBufferSpace() > 0x10000) {
		return; // not sent, so not remembered
	}
	lastHash = hash;
	everSent = true;
	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// Chroma (0x5D): THE MIDI CHANNEL EACH TRACK LISTENS ON - its learned MIDI input, in track-list (0x49) order.
//
//   F0 00 21 7B 01 02 5D <count> count x <channel> F7       channel 0-15; 16/17 an MPE zone; 7F none (or audio)
//
// Cody, 2026-11-24: "I need to be able to get Chordion to send signals to one of my tracks ... an easy MIDI routing
// table". A host can only aim a controller at a track if it knows which channel that track hears. De-duped.
void HIDSysex::sendMidiInputsIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	constexpr int32_t kMaxTracks = 24;
	uint8_t msg[7 + 1 + kMaxTracks + 1];
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02;
	msg[i++] = SysEx::SysexCommands::MidiInputs;
	const int32_t countAt = i++;
	uint32_t hash = 2166136261u;
	int32_t n = 0;
	if (currentSong != nullptr) {
		for (Output* output = currentSong->firstOutput; output != nullptr && n < kMaxTracks;
		     output = output->next, n++) {
			uint8_t ch = 0x7f;
			if (output->type != OutputType::AUDIO) {
				Instrument* instrument = static_cast<Instrument*>(output);
				if (instrument->midiInput.containsSomething() && instrument->midiInput.channelOrZone < 18) {
					ch = (uint8_t)instrument->midiInput.channelOrZone;
				}
			}
			msg[i++] = ch;
			hash = (hash ^ ch) * 16777619u;
		}
	}
	msg[countAt] = (uint8_t)n;
	hash = (hash ^ (uint8_t)n) * 16777619u;
	static uint32_t lastHash = 0;
	static bool everSent = false;
	if (!force && everSent && hash == lastHash) {
		return;
	}
	lastHash = hash;
	everSent = true;
	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// The 14-bit hash a host names a Features setting by: FNV-1a over the name it is stored under on the card.
// Positions in the settings enum move between firmware builds; the stored name does not.
static uint16_t settingNameHash(std::string_view name) {
	uint32_t h = 2166136261u;
	for (char ch : name) {
		h = (h ^ (uint8_t)ch) * 16777619u;
	}
	return (uint16_t)((h ^ (h >> 14) ^ (h >> 28)) & 0x3fff);
}

// A Features change made by a host is applied at once and written to the card a moment later, when the card
// is free - the same call the Deluge's own menu makes when it is left. NEVER from the command itself: that
// arrives on the MIDI input path, and the card must not be touched from there. saveFeaturesIfNeeded() runs
// from the display poll, which is the UI's.
static bool featuresNeedSaving = false;
static void saveFeaturesIfNeeded() {
	if (featuresNeedSaving && !sdRoutineLock) {
		featuresNeedSaving = false;
		runtimeFeatureSettings.writeSettingsToFile();
	}
}

// Chroma (0x4D): the device's own state - which track the player is on, how bright the pads are, and every
// Features setting. Three things a host otherwise learns only by walking a menu or reading the card.
//
//   F0 00 21 7B 01 02 4D <track> <brightness> <count> then count x (<hashHi> <hashLo> <value>) <master> F7
//
// <track> is the position of the current clip's track in the 0x49 list, 7F for none. <brightness> is 1-25.
// Each setting is named by settingNameHash() of its stored name; <value> is its value, capped at 127.
void HIDSysex::sendDeviceStateIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	uint8_t msg[7 + 3 + RuntimeFeatureSettingType::MaxElement * 3 + 2 + 1];
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02;
	msg[i++] = SysEx::SysexCommands::DeviceState;
	uint32_t hash = 2166136261u;
	auto put = [&](uint8_t b) {
		msg[i++] = b & 0x7f;
		hash = (hash ^ b) * 16777619u;
	};

	// THE TRACK THE PLAYER HAS SELECTED (1.3.98): what the gold knobs are aimed at right now. That is a
	// clip's track while a clip is selected - held in the song grid, or open in its own view - and the
	// whole song when nothing is. Cody, 2026-11-23: "I want it to follow the selected track in delly ...
	// if we're in the song mode then master". The song is sent as 7E; 7F stays "none".
	//
	// 1.3.97 sent the song's CURRENT clip, which is the clip last opened and does not move back to
	// "nothing" when the player returns to the song grid - so the mark sat on a track nobody was on.
	//
	// The knob focus is a pointer the view holds, and it is only ever COMPARED here, never followed:
	// a clip is looked up among the song's own clips, so a stale pointer matches nothing.
	uint8_t track = 0x7f;
	if (currentSong != nullptr) {
		TimelineCounter* focus =
		    view.activeModControllableModelStack.toWithTimelineCounter()->getTimelineCounterAllowNull();
		Output* selected = nullptr;
		if (focus == currentSong) {
			track = 0x7e;
		}
		else if (focus != nullptr) {
			for (int32_t c = 0; c < currentSong->sessionClips.getNumElements() && selected == nullptr; c++) {
				Clip* clip = currentSong->sessionClips.getClipAtIndex(c);
				if (clip == focus) {
					selected = clip->output;
				}
			}
			for (int32_t c = 0; c < currentSong->arrangementOnlyClips.getNumElements() && selected == nullptr; c++) {
				Clip* clip = currentSong->arrangementOnlyClips.getClipAtIndex(c);
				if (clip == focus) {
					selected = clip->output;
				}
			}
		}
		if (selected != nullptr) {
			uint8_t n = 0;
			for (Output* output = currentSong->firstOutput; output != nullptr && n < 0x7e; output = output->next, n++) {
				if (output == selected) {
					track = n;
					break;
				}
			}
		}
	}
	put(track);
	put(FlashStorage::defaultPadBrightness);
	put((uint8_t)RuntimeFeatureSettingType::MaxElement);
	for (uint32_t t = 0; t < RuntimeFeatureSettingType::MaxElement; t++) {
		const auto& setting = runtimeFeatureSettings.hostSetting(t);
		const uint16_t name = settingNameHash(setting.xmlName);
		put((uint8_t)(name >> 7));
		put((uint8_t)(name & 0x7f));
		put((uint8_t)((setting.value > 127) ? 127 : setting.value));
	}

	// THE SONG'S MASTER LEVEL (1.3.115), one byte after the settings: the level of the whole song, the
	// one the LEVEL knob sets in song view, as the 0-50 the display shows for it. 7F when there is no
	// song to ask. Cody, 2026-11-24: "add the master output level knob to the upper left of the app ...
	// and track it just like the gold knobs". This rides the pad poll, so a turn is seen within a poll.
	//
	// NOT the VOLUME knob on the panel: that one is analogue, sits after the converter, and is not
	// wired to anything this firmware can read.
	uint8_t master = 0x7f;
	if (currentSong != nullptr && currentSong->paramManager.containsAnyMainParamCollections()) {
		UnpatchedParamSet* unpatched = currentSong->paramManager.getUnpatchedParamSet();
		if (unpatched != nullptr) {
			int32_t level = computeCurrentValueForStandardMenuItem(
			    unpatched->getValue(deluge::modulation::params::UNPATCHED_VOLUME));
			master = (uint8_t)((level < 0) ? 0 : ((level > kMaxMenuValue) ? kMaxMenuValue : level));
		}
	}
	put(master);
	// THE DEFAULT KEYBOARD LAYOUT (1.3.128), after the master level: the layout a new track opens in, the same
	// KeyboardLayoutType number 0x48 sends for the current one. Cody: "a setting for setting the default keyboard
	// view so that I don't have to rotate through them". Read here; set from the Deluge's own menu.
	put((uint8_t)util::to_underlying(FlashStorage::defaultKeyboardLayout));

	static uint32_t lastHash = 0;
	static bool everSent = false;
	if (!force && everSent && hash == lastHash) {
		return;
	}
	lastHash = hash;
	everSent = true;
	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// Chroma (0x51): THE FRONT PANEL - which button lights are on, and which buttons a finger is on.
//
//   F0 00 21 7B 01 02 51 <cols> <rows> cols x <lit> cols x <held> F7
//
// One byte per column of the button matrix, bit y set for row y; the lights first, then the buttons
// held. A button and its light share a coordinate, so the two halves line up.
//
// Cody, 2026-11-23: "an exact replica of the device ... mirror exactly what delly shows down to the lit
// buttons. this is helpful for teaching". The pads, the display and the gold knobs were already
// reported; the lights were not, so a drawing of the panel could only have guessed at them.
//
// A blinking light is reported as it blinks - on, off, on - because that is what it is doing.
namespace indicator_leds {
extern bool ledStates[];
}
namespace Buttons {
extern bool buttonStates[NUM_BUTTON_COLS + 1][NUM_BUTTON_ROWS];
}
void HIDSysex::sendPanelStateIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	uint8_t msg[7 + 2 + NUM_LED_COLS * 2 + 1];
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02;
	msg[i++] = SysEx::SysexCommands::PanelState;
	uint32_t hash = 2166136261u;
	auto put = [&](uint8_t b) {
		msg[i++] = b & 0x7f;
		hash = (hash ^ b) * 16777619u;
	};
	put(NUM_LED_COLS);
	put(NUM_LED_ROWS);
	for (int32_t x = 0; x < NUM_LED_COLS; x++) {
		uint8_t lit = 0;
		for (int32_t y = 0; y < NUM_LED_ROWS; y++) {
			if (indicator_leds::ledStates[x + y * NUM_LED_COLS]) {
				lit |= (1 << y);
			}
		}
		put(lit);
	}
	for (int32_t x = 0; x < NUM_LED_COLS; x++) {
		uint8_t held = 0;
		for (int32_t y = 0; y < NUM_LED_ROWS; y++) {
			if (x < NUM_BUTTON_COLS && y < NUM_BUTTON_ROWS && Buttons::buttonStates[x][y]) {
				held |= (1 << y);
			}
		}
		put(held);
	}

	static uint32_t lastHash = 0;
	static bool everSent = false;
	if (!force && everSent && hash == lastHash) {
		return;
	}
	lastHash = hash;
	everSent = true;
	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// Chroma (0x52): WHICH NOTE EACH PAD PLAYS, and how the instrument spells it.
//
//   F0 00 21 7B 01 02 52 <cols> <rows> <flats> <root> then rows x cols note bytes, bottom row first F7
//
// <cols> <rows> are 16 and 8 in a keyboard layout that has notes, and 0 0 anywhere else - and then the
// frame ends after <root>. <flats> is 1 when the instrument is set to spell with flats (its "use sharps" default,
// with Auto following the key) comes out as flats, so a host spells a note the way this instrument does
// and never has an opinion of its own. <root> is the key's root as a pitch class. A pad that plays no
// note is 7F.
//
// Cody, 2026-11-23: "I want to know the exact note I play, and I want a toggle where we see the root
// note names on the grid and the other notes lightly ... flats and sharps as configured". A host is told
// each pad's colour, but only the layout knows which pad is which note.
void HIDSysex::sendPadNotesIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	uint8_t msg[7 + 4 + kDisplayWidth * kDisplayHeight + 1];
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02;
	msg[i++] = SysEx::SysexCommands::PadNotes;
	uint32_t hash = 2166136261u;
	auto put = [&](uint8_t b) {
		msg[i++] = b & 0x7f;
		hash = (hash ^ b) * 16777619u;
	};

	uint8_t notes[kDisplayHeight][kDisplayWidth];
	bool any = false;
	UI* ui = getCurrentUI();
	if (currentSong != nullptr && ui != nullptr && ui->getUIType() == UIType::KEYBOARD_SCREEN) {
		any = keyboardScreen.padNotes(notes);
	}
	const uint8_t root = (currentSong != nullptr) ? (uint8_t)(((currentSong->key.rootNote % 12) + 12) % 12) : 0;
	// THE INSTRUMENT'S OWN SETTING: the "use sharps" default that noteCodeToString() spells every note on
	// its display with. 1.3.106 sent Chroma's key-following lean here, which is a different choice and
	// came out as sharps on an instrument set to flats - Cody: "the notes need to follow delly, looks
	// like flats not sharps".
	const bool flats = !FlashStorage::defaultUseSharps;
	put(any ? kDisplayWidth : 0);
	put(any ? kDisplayHeight : 0);
	put(flats ? 1 : 0);
	put(root);
	if (any) {
		for (int32_t y = 0; y < kDisplayHeight; y++) {
			for (int32_t x = 0; x < kDisplayWidth; x++) {
				put(notes[y][x]);
			}
		}
	}

	static uint32_t lastHash = 0;
	static bool everSent = false;
	if (!force && everSent && hash == lastHash) {
		return;
	}
	lastHash = hash;
	everSent = true;
	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// Chroma (0x53): THE VELOCITY EACH PAD PLAYS AT, where the pad's position decides it.
//
//   F0 00 21 7B 01 02 53 <cols> <rows> then rows x cols velocity bytes, bottom row first F7
//
// 16 and 8 in the velocity drums layout; 0 0 anywhere else, and the frame ends there. A velocity is 1-127;
// 0 is a pad that plays nothing. Cody, 2026-11-23: "a toggle like the notes for the velocities".
void HIDSysex::sendPadVelocitiesIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	uint8_t msg[7 + 2 + kDisplayWidth * kDisplayHeight + 1];
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02;
	msg[i++] = SysEx::SysexCommands::PadVelocities;
	uint32_t hash = 2166136261u;
	auto put = [&](uint8_t b) {
		msg[i++] = b & 0x7f;
		hash = (hash ^ b) * 16777619u;
	};

	uint8_t velocities[kDisplayHeight][kDisplayWidth];
	bool any = false;
	UI* ui = getCurrentUI();
	if (currentSong != nullptr && ui != nullptr && ui->getUIType() == UIType::KEYBOARD_SCREEN) {
		any = keyboardScreen.padVelocities(velocities);
	}
	put(any ? kDisplayWidth : 0);
	put(any ? kDisplayHeight : 0);
	if (any) {
		for (int32_t y = 0; y < kDisplayHeight; y++) {
			for (int32_t x = 0; x < kDisplayWidth; x++) {
				put(velocities[y][x]);
			}
		}
	}

	static uint32_t lastHash = 0;
	static bool everSent = false;
	if (!force && everSent && hash == lastHash) {
		return;
	}
	lastHash = hash;
	everSent = true;
	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// Chroma (0x54): WHAT THE KEYBOARD'S TWO SIDEBAR COLUMNS ARE SET TO, and the value on each of their pads.
//
//   F0 00 21 7B 01 02 54 <left> <right> <leftHas> <rightHas> 8 x <left value> 8 x <right value> F7
//
// <left> <right> are each column's function: 0 velocity, 1 mod, 2 chord, 3 song chord memory, 4 chord
// memory, 5 scale mode, 6 DX, 7 session; 7F 7F when the keyboard is not on screen. <leftHas> / <rightHas>
// are 1 when that column's pads are VALUES (velocity, mod), and then its eight bytes are the value each
// pad sets, bottom pad first; otherwise the eight bytes are 0 and mean nothing. (1.3.111 sent only the
// first two bytes.)
//
// The two columns are eight unlabelled pads each. Cody, 2026-11-23: "velocity, mod are not labelled ...
// I wanted specifically to have visual labels of the velocities and mod values".
void HIDSysex::sendKeyboardColumnsIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	uint8_t msg[7 + 4 + 2 * kDisplayHeight + 1];
	int32_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = 0x02;
	msg[i++] = SysEx::SysexCommands::KeyboardColumns;
	uint32_t hash = 2166136261u;
	auto put = [&](uint8_t b) {
		msg[i++] = b & 0x7f;
		hash = (hash ^ b) * 16777619u;
	};

	uint8_t func[2] = {0x7f, 0x7f};
	uint8_t has[2] = {0, 0};
	uint8_t values[2][kDisplayHeight] = {};
	UI* ui = getCurrentUI();
	if (currentSong != nullptr && ui != nullptr && ui->getUIType() == UIType::KEYBOARD_SCREEN) {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip != nullptr) {
			auto& columns = clip->keyboardState.columnControl;
			func[0] = (uint8_t)columns.leftColFunc & 0x7f;
			func[1] = (uint8_t)columns.rightColFunc & 0x7f;
			deluge::gui::ui::keyboard::controls::ControlColumn* column[2] = {columns.leftCol, columns.rightCol};
			for (int32_t c = 0; c < 2; c++) {
				for (int32_t y = 0; y < kDisplayHeight; y++) {
					const int32_t v = (column[c] != nullptr) ? column[c]->valueAtPad(y) : -1;
					if (v >= 0) {
						has[c] = 1;
						values[c][y] = (uint8_t)(v > 127 ? 127 : v);
					}
				}
			}
		}
	}
	put(func[0]);
	put(func[1]);
	put(has[0]);
	put(has[1]);
	for (int32_t c = 0; c < 2; c++) {
		for (int32_t y = 0; y < kDisplayHeight; y++) {
			put(values[c][y]);
		}
	}

	static uint32_t lastHash = 0;
	static bool everSent = false;
	if (!force && everSent && hash == lastHash) {
		return;
	}
	lastHash = hash;
	everSent = true;
	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// Chroma (0x4E in, 0x4F out): a host changes one thing on the instrument, and is told what happened.
//
//   in   F0 00 21 7B 01 02 4E <seq> <a> <b> <what> <value> [<value2>] F7
//   out  F0 00 21 7B 01 02 4F <seq> <result> F7
//
// <what>  0 track level (value 0-127, 127 = unity)      a = track, b = hash of the track's name
//         1 track mute (0/1)                            a = track, b = hash of the track's name
//         2 track solo (0/1)                            a = track, b = hash of the track's name
//         3 track USB routing (value, value2 = mask)    a = track, b = hash of the track's name
//         4 a Features setting (value)                  a, b = the setting's 14-bit name hash
//         5 pad brightness (value 1-25)                 a, b unused
// <result>  0 applied   1 no such track or setting   2 that track is not the one the host meant   3 bad request
//
// THE NAME GUARD. A track is addressed by its position, and positions move when tracks are added, deleted or
// reordered. So the host also sends a 7-bit hash of the NAME of the track it means; if the track now at that
// position has a different name, the command is refused rather than applied to the wrong track. It is a
// guard, not an identity: two tracks with the same name are indistinguishable to it.
//
// THE OUTCOME IS NOT THE STATE. It says only what became of the command. The new value is reported the way
// it always is - the track list (0x49) or the device state (0x4D) - straight after a change.
//
// TWO SETTINGS ARE NOT OFFERED: the develop-sysex key and the emulated display. One opens the instrument to
// firmware loading and the other can take its physical display away; neither is a host's to flip.
void HIDSysex::receiveMixerSet(MIDICable& cable, uint8_t* data, int32_t len) {
	// data[0] = 0x02 (HID), data[1] = 0x4E, then seq, a, b, what, value, [value2]
	const uint8_t seq = (len >= 3) ? (data[2] & 0x7f) : 0;
	uint8_t result = 3;
	bool tracksChanged = false;
	bool deviceChanged = false;
	bool midiInputsChanged = false;
	if (len >= 7) {
		const uint8_t a = data[3] & 0x7f;
		const uint8_t b = data[4] & 0x7f;
		const uint8_t what = data[5] & 0x7f;
		const uint8_t value = data[6] & 0x7f;

		if (what <= 3) {
			Output* output = (currentSong != nullptr) ? currentSong->firstOutput : nullptr;
			for (uint8_t n = 0; output != nullptr && n < a; n++) {
				output = output->next;
			}
			if (output == nullptr) {
				result = 1;
			}
			else {
				uint32_t h = 2166136261u; // the same FNV-1a the host computes, over the name as 0x49 sends it
				const char* name = output->name.get();
				for (int32_t k = 0; name != nullptr && name[k] != 0 && k < 12; k++) {
					const uint8_t c = (uint8_t)name[k] & 0x7f;
					h = (h ^ ((c >= 32) ? c : '?')) * 16777619u;
				}
				if ((uint8_t)((h ^ (h >> 7) ^ (h >> 14) ^ (h >> 21)) & 0x7f) != b) {
					result = 2;
				}
				else if (what == 0) {
					output->mixerLevel = value; // 0-127 by construction; 127 is unity
					result = 0;
				}
				else if (what == 1 && value <= 1) {
					output->mixerMute = (value == 1);
					result = 0;
				}
				else if (what == 2 && value <= 1) {
					output->mixerSolo = (value == 1);
					result = 0;
				}
				else if (what == 3 && len >= 8) {
					// Two 7-bit halves, masked: a bit this firmware does not define routes nowhere.
					output->usbRouting = (uint16_t)((((uint16_t)value << 7) | (data[7] & 0x7f)) & UsbRoute::ALL);
					result = 0;
				}
				tracksChanged = (result == 0);
			}
		}
		else if (what == 4) {
			const uint16_t want = (uint16_t)(((uint16_t)a << 7) | b);
			result = 1;
			for (uint32_t t = 0; t < RuntimeFeatureSettingType::MaxElement; t++) {
				if (settingNameHash(runtimeFeatureSettings.hostSetting(t).xmlName) != want) {
					continue;
				}
				if (t == RuntimeFeatureSettingType::DevSysexAllowed
				    || t == RuntimeFeatureSettingType::EmulatedDisplay) {
					result = 3; // not a host's to change
					break;
				}
				// Only a value the setting actually offers.
				bool offered = false;
				for (const auto& option : runtimeFeatureSettings.hostSetting(t).options) {
					if (option.value == value) {
						offered = true;
						break;
					}
				}
				if (!offered) {
					result = 3;
					break;
				}
				runtimeFeatureSettings.set((RuntimeFeatureSettingType)t, value);
				featuresNeedSaving = true; // written to the card from sendDeviceStateIfChanged, when it is free
				result = 0;
				deviceChanged = true;
				break;
			}
		}
		else if (what == 7) {
			// A TRACK'S MIDI INPUT (1.3.126): the channel it listens on, 0-15, from any device; 127 clears it. What
			// holding the track's pad and playing a note does on the Deluge (MIDI learn), from a host. Cody,
			// 2026-11-24: "I need to be able to get Chordion to send signals to one of my tracks ... an easy MIDI
			// routing table". Addressed like a mixer command: position and name hash, refused if they disagree.
			Output* output = (currentSong != nullptr) ? currentSong->firstOutput : nullptr;
			for (uint8_t n = 0; output != nullptr && n < a; n++) {
				output = output->next;
			}
			if (output == nullptr) {
				result = 1;
			}
			else {
				uint32_t h = 2166136261u;
				const char* name = output->name.get();
				for (int32_t k = 0; name != nullptr && name[k] != 0 && k < 12; k++) {
					const uint8_t c = (uint8_t)name[k] & 0x7f;
					h = (h ^ ((c >= 32) ? c : '?')) * 16777619u;
				}
				if ((uint8_t)((h ^ (h >> 7) ^ (h >> 14) ^ (h >> 21)) & 0x7f) != b) {
					result = 2;
				}
				else if (output->type == OutputType::AUDIO || (value > 15 && value != 127)) {
					result = 3;
				}
				else {
					Instrument* instrument = static_cast<Instrument*>(output);
					if (value == 127) {
						instrument->midiInput.clear();
					}
					else {
						instrument->midiInput.cable = nullptr; // from any device
						instrument->midiInput.channelOrZone = value;
					}
					result = 0;
					midiInputsChanged = true;
				}
			}
		}
		else if (what == 9) {
			// THE REVERB RETURN ON A USB PAIR (1.3.133): 0 the main mix, 1-4 the pair instead. Reported in 0x5B.
			if (value <= deluge::processing::engines::USBAudioStream::kStemChannels / 2u) {
				deluge::processing::engines::USBAudioStream::setReverbPair(value);
				FlashStorage::usbAudioReverbPair = value; // kept across a restart (1.3.135), saved from routine()
				usbSettingsNeedSaving = true;
				result = 0;
			}
		}
		else if (what == 8) {
			// THE MAIN MIX ON A USB PAIR (1.3.132): 0 none, 1-4 the pair. Reported back in 0x5B. Not saved: the
			// Deluge starts with none, so a pair is only ever taken over because a host asked.
			if (value <= deluge::processing::engines::USBAudioStream::kStemChannels / 2u) {
				deluge::processing::engines::USBAudioStream::setMainPair(value);
				FlashStorage::usbAudioMainPair = value; // kept across a restart (1.3.135), saved from routine()
				usbSettingsNeedSaving = true;
				result = 0;
			}
		}
		else if (what == 6) {
			// THE SONG'S MASTER LEVEL (1.3.124), 0-50 as the display shows it - the value 0x4D reports. Set the way
			// turning the gold knob on the song does it (as user input, so the Deluge treats it like a hand on the
			// knob), and reported back through 0x4D; the outcome never carries the value. Cody, 2026-11-24: "make
			// that master in the app adjustable and mappable".
			if (currentSong != nullptr && value <= kMaxMenuValue) {
				char modelStackMemory[MODEL_STACK_MAX_SIZE];
				ModelStackWithThreeMainThings* stack =
				    currentSong->setupModelStackWithSongAsTimelineCounter(modelStackMemory);
				ModelStackWithAutoParam* param =
				    stack->getUnpatchedAutoParamFromId(deluge::modulation::params::UNPATCHED_VOLUME);
				if (param != nullptr && param->autoParam != nullptr) {
					param->autoParam->setCurrentValueInResponseToUserInput(computeFinalValueForStandardMenuItem(value),
					                                                       param);
					result = 0;
					deviceChanged = true;
				}
			}
		}
		else if (what == 5) {
			if (value >= kMinLedBrightness && value <= kMaxLedBrightness) {
				FlashStorage::defaultPadBrightness = value;
				PadLEDs::setBrightnessLevel(value);
				result = 0;
				deviceChanged = true;
			}
		}
	}
	uint8_t reply[10] = {0xf0, 0x00, 0x21, 0x7b, 0x01, 0x02, SysEx::SysexCommands::CommandOutcome, seq, result, 0xf7};
	if (midiInputsChanged) {
		sendMidiInputsIfChanged();
	}
	cable.sendSysex(reply, sizeof(reply));
	if (tracksChanged) {
		sendTrackStateIfChanged(); // the state itself, from the one place it is always reported
	}
	if (deviceChanged) {
		sendDeviceStateIfChanged();
	}
}

void HIDSysex::receiveChordApply(uint8_t modType, uint8_t value) {
	gChordModType = modType & 0x7f;
	gChordModValue = value & 0x7f;
	gChordModPending = true; // set fields before the flag so the consumer never reads a half-written mod
}

bool HIDSysex::hasChordMod() {
	return gChordModPending;
}

bool HIDSysex::takeChordMod(uint8_t* modType, uint8_t* value) {
	if (!gChordModPending) {
		return false;
	}
	if (modType != nullptr) {
		*modType = gChordModType;
	}
	if (value != nullptr) {
		*value = gChordModValue;
	}
	gChordModPending = false;
	return true;
}

// Chroma: F0 00 21 7B 01 02 50 <packed 8x18 RGB, 8-bit->7-bit> F7. Mirrors the live pad-LED grid the user
// sees — exact colours, and it reflects ANY control change because the host just renders the lit pads.
// Reads PadLEDs::image (the buffer that drives the physical LEDs, always valid, incl. on 7-seg units).
namespace {
constexpr int32_t kPadRawBytes = 8 * 18 * 3 + 18 + 18 + 18; // colours, then held, sounding and playing columns
uint32_t padLeaseUntil = 0;                                 // audio-sample time a subscription runs to
bool padSubscribed = false;
bool padOneShotOwed = false; // an older host's single request, answered by routine()
bool padFullOwed = false;    // the next push is a whole grid (a new subscription)
uint8_t padLastSent[kPadRawBytes];
bool padHaveLast = false;
uint32_t padLastPushAt = 0, checksLastAt = 0;
} // namespace

static void requestPads(bool subscribe) {
	if (subscribe) {
		if (!padSubscribed) {
			padFullOwed = true; // a host that has just (re)subscribed has nothing to apply a delta to
		}
		padSubscribed = true;
		padLeaseUntil = AudioEngine::audioSampleTimer + 5 * kSampleRate;
	}
	else {
		padOneShotOwed = true;
	}
}

// The grid as a pad sees it, into `raw`; returns the bytes written.
static int32_t buildPadFrame(uint8_t* raw);

void HIDSysex::sendPadGrid(MIDICable& cable) {
	uint8_t raw[kPadRawBytes];
	const int32_t p = buildPadFrame(raw);
	uint8_t reply_hdr[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x50};
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	memcpy(reply, reply_hdr, 7);
	int32_t packed = pack_8bit_to_7bit(reply + 7, 600, raw, p);
	if (packed < 0) {
		return;
	}
	reply[7 + packed] = 0xf7;
	cable.sendSysex(reply, packed + 8);
	memcpy(padLastSent, raw, (size_t)p);
	padHaveLast = true;
}

// THE PADS THAT CHANGED (0x5E, 1.3.134): F0 00 21 7B 01 02 5E <packed: ver=1, n, n x (index, r, g, b), held[18],
// sounding[18], playing[18]> F7. Index is y * 18 + x, as in the grid. A note press changes a handful of pads, so this
// is a tenth of a whole grid; past 64 changed pads the whole grid is cheaper and is sent instead.
static void pushPadsIfChanged(MIDICable& cable) {
	uint8_t raw[kPadRawBytes];
	const int32_t p = buildPadFrame(raw);
	if (!padHaveLast || padFullOwed) {
		padFullOwed = false;
		HIDSysex::sendPadGrid(cable);
		return;
	}
	constexpr int32_t kColourBytes = 8 * 18 * 3;
	uint8_t delta[2 + 64 * 4 + 54];
	int32_t n = 0, d = 2;
	for (int32_t k = 0; k < 144; k++) {
		const uint8_t* now = &raw[k * 3];
		const uint8_t* was = &padLastSent[k * 3];
		if (now[0] == was[0] && now[1] == was[1] && now[2] == was[2]) {
			continue;
		}
		if (++n > 64) {
			HIDSysex::sendPadGrid(cable);
			return;
		}
		delta[d++] = (uint8_t)k;
		delta[d++] = now[0];
		delta[d++] = now[1];
		delta[d++] = now[2];
	}
	const bool columnsMoved = memcmp(&raw[kColourBytes], &padLastSent[kColourBytes], (size_t)(p - kColourBytes)) != 0;
	if (n == 0 && !columnsMoved) {
		return; // nothing changed: nothing is sent
	}
	delta[0] = 1;
	delta[1] = (uint8_t)n;
	memcpy(&delta[d], &raw[kColourBytes], (size_t)(p - kColourBytes));
	d += p - kColourBytes;
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	const uint8_t hdr[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, SysEx::SysexCommands::PadDelta};
	memcpy(reply, hdr, 7);
	const int32_t packed = pack_8bit_to_7bit(reply + 7, 600, delta, d);
	if (packed < 0) {
		return;
	}
	reply[7 + packed] = 0xf7;
	cable.sendSysex(reply, packed + 8);
	memcpy(padLastSent, raw, (size_t)p);
}

// THE PUSHES' OWN TASK (1.3.134), registered in deluge.cpp at low priority with no USB resource. While a host holds a
// pad subscription: the pads whenever they change (at most every 40 ms), and the other change checks every 100 ms -
// the clock the host's pad poll used to provide, now the instrument's own. An older host's one-off request is
// answered here too, with the same checks it always brought.
void HIDSysex::routine() {
	// A USB audio setting a host changed is written to flash here, in a task with no USB resource, never in the
	// command handler that runs inside "check usb midi" (1.3.135).
	if (usbSettingsNeedSaving && !sdRoutineLock) {
		usbSettingsNeedSaving = false;
		FlashStorage::writeSettings();
	}
	if (lastHidCable == nullptr) {
		return;
	}
	const uint32_t now = AudioEngine::audioSampleTimer;
	if (padSubscribed && (int32_t)(now - padLeaseUntil) > 0) {
		padSubscribed = false; // not renewed: the host has gone, and nothing is sent to nobody
	}
	const bool oneShot = padOneShotOwed;
	padOneShotOwed = false;
	if (oneShot) {
		sendPadGrid(*lastHidCable);
	}
	else if (padSubscribed && (uint32_t)(now - padLastPushAt) >= kSampleRate / 25) {
		padLastPushAt = now;
		pushPadsIfChanged(*lastHidCable);
	}
	if (!oneShot && !(padSubscribed && (uint32_t)(now - checksLastAt) >= kSampleRate / 10)) {
		return;
	}
	checksLastAt = now;
	// The panel: a button light can change with no display redraw. De-duped, so an unchanged panel costs nothing.
	sendPanelStateIfChanged();
	sendDeviceStateIfChanged();     // selecting a track in the song grid redraws no display either
	sendPadNotesIfChanged();        // scrolling a keyboard layout moves every note and redraws no display
	sendPadVelocitiesIfChanged();   // zooming or scrolling the drums layout moves every velocity
	sendKeyboardColumnsIfChanged(); // a column's function changes with a button and no redraw
	sendSampleStateIfChanged();     // hitting a kit pad selects its row and may redraw nothing
	sendHealthIfDue();              // about once a second
	sendHeldChordIfChanged();       // a chord is held and let go with no display redraw a host can count on
	sendKitRowsIfChanged();         // a mute or a selected row redraws nothing a host can count on
	sendEffectsIfChanged();         // the effects of the sound in focus, when one moves
	flushGoldKnobs();               // a knob value held back by the 40 ms gap
	sendMidiInputsIfChanged();      // a learn on the Deluge redraws nothing a host can count on
}

static int32_t buildPadFrame(uint8_t* raw) {
	const int32_t W = kDisplayWidth + kSideBarWidth; // 18
	const int32_t H = kDisplayHeight;                // 8
	int32_t p = 0;
	for (int32_t y = 0; y < H; y++) {
		for (int32_t x = 0; x < W; x++) {
			// prepareColour, NOT the raw image: this is the same function every pad passes through on
			// its way to the PIC, so the Cockpit receives the colour the pad is ACTUALLY showing -
			// flash cursor, press feedback and greyout included.
			//
			// Cody: "I want device to EXACTLY reflect the current state of the Deluge ... this is the
			// cockpit of a plane, you can't have speeds misrepresented, attitudes off - you will
			// crash." Reading `image` here was a second, quieter rendering of the grid that agreed
			// with the hardware only for as long as nothing was happening.
			RGB c = PadLEDs::prepareColour(x, y, PadLEDs::image[y][x]);
			raw[p++] = c.r;
			raw[p++] = c.g;
			raw[p++] = c.b;
		}
	}
	// WHICH PADS A FINGER IS ON (1.3.98): one byte per column after the colours, bit y set while that
	// pad is physically held. The colour above already shifts under a finger, but on a screen that shift
	// reads as one more lit pad among the others a keyboard layout lights for the same note. Cody:
	// "make more bold the ACTUAL note(s) being pressed". A reader that wants 432 bytes still gets them.
	for (int32_t x = 0; x < W; x++) {
		uint8_t held = 0;
		for (int32_t y = 0; y < H; y++) {
			if (matrixDriver.padStates[x][y]) {
				held |= (1 << y);
			}
		}
		raw[p++] = held;
	}
	// WHICH PADS PLAY A NOTE THAT IS HELD (1.3.99): eighteen more bytes, the same shape. In a keyboard
	// layout these are the pressed note's copies across the grid. All zero anywhere else.
	uint8_t sounding[kDisplayWidth] = {0};
	UI* padUI = getCurrentUI();
	if (padUI != nullptr && padUI->getUIType() == UIType::KEYBOARD_SCREEN) {
		keyboardScreen.soundingPads(sounding);
	}
	for (int32_t x = 0; x < W; x++) {
		raw[p++] = (x < kDisplayWidth) ? sounding[x] : 0;
	}
	// WHICH PADS THE SEQUENCE IS PLAYING (1.3.113): eighteen more, the same shape. In a keyboard layout
	// these are the pads lit by the clip's recorded notes as they play back. All zero anywhere else.
	uint8_t playing[kDisplayWidth] = {0};
	if (padUI != nullptr && padUI->getUIType() == UIType::KEYBOARD_SCREEN) {
		keyboardScreen.playingPads(playing);
	}
	for (int32_t x = 0; x < W; x++) {
		raw[p++] = (x < kDisplayWidth) ? playing[x] : 0;
	}
	return p;
}

void HIDSysex::sendOLEDDataDelta(MIDICable& cable, bool force) {
	const int32_t data_size = 768;
	const int32_t max_packed_size = 922;

	uint8_t* current = deluge::hid::display::OLED::oledCurrentImage[0];

	int32_t first_change = 9000;
	int32_t last_change = 0;
	int32_t* blkdata_new = (int32_t*)current;
	int32_t* blkdata_old = (int32_t*)oledDeltaImage;

	const int32_t word_size = data_size >> 2;

	if (force || oledDeltaForce) {
		first_change = 0;
		last_change = word_size - 1;
	}
	else {
		for (int32_t blk = 0; blk < word_size; blk++) {
			if (blkdata_new[blk] != blkdata_old[blk]) {
				if (first_change > blk) {
					first_change = blk;
				}
				last_change = blk;
			}
		}
	}

	if (first_change > word_size) {
		return;
	}

	int start = first_change / 2;
	int len = (last_change / 2) - start + 1;
	//	int8_t reply_hdr[5] = {0xf0, 0x7d, 0x02, 0x40, 0x02};
	uint8_t reply_hdr[8] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x40, 0x02};
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	memcpy(reply, reply_hdr, sizeof(reply_hdr));
	//	reply[5] = start;
	reply[sizeof(reply_hdr) + 0] = start;
	//  reply[6] = len;
	reply[sizeof(reply_hdr) + 1] = len;
	// 	int32_t packed = pack_8to7_rle(reply + 7, max_packed_size, current + 8 * start, 8 * len);
	int32_t packed = pack_8to7_rle(reply + 10, max_packed_size, current + 8 * start, 8 * len);
	if (packed <= 0) {
		return;
	}
	memcpy(oledDeltaImage + (8 * start), current + (8 * start), 8 * len);
	oledDeltaForce = false;
	// reply[7 + packed] = 0xf7; // end of transmission
	reply[10 + packed] = 0xf7; // end of transmission //
	// device->sendSysex(reply, packed + 8);
	cable.sendSysex(reply, packed + 11);
}

void HIDSysex::readBlock(MIDICable& cable) {
	const int32_t data_size = 768;
	const int32_t max_packed_size = 922;
	uint8_t reply_hdr[8] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x40, 0x00};
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	// 		memcpy(reply, reply_hdr, 5);
	memcpy(reply, reply_hdr, 8); //
	                             //		reply[5] = 0; // nominally 32*data[5] is start pos for a delta
	reply[8] = 0;                // nominally 32*data[8] is start pos for a delta //

	int32_t packed;

	//			packed = pack_8bit_to_7bit(reply + 6, max_packed_size,
	// deluge::hid::display::OLED::oledCurrentImage[0],
	uint8_t* srcBlock = (uint8_t*)smDeserializer.fileClusterBuffer; //  deluge::hid::display::OLED::oledCurrentImage[0];

	packed = pack_8bit_to_7bit(reply + 9, max_packed_size, srcBlock,
	                           data_size); //
	if (packed < 0) {
		display->popupTextTemporary("error: fail");
	}
	//		reply[6 + packed] = 0xf7; // end of transmission
	reply[9 + packed] = 0xf7;            // end of transmission
	                                     // 		device->sendSysex(reply, packed + 7); //
	cable.sendSysex(reply, packed + 10); //
}
