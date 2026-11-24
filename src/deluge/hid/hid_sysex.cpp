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
#include "hid/led/pad_leds.h"
#include "hid/matrix/matrix_driver.h"
#include "io/midi/midi_device.h"
#include "io/midi/midi_engine.h"
#include "io/midi/sysex.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/instrument_clip.h"              // Chroma (0x48): keyboardState.currentLayout
#include "model/instrument/midi_instrument.h"        // Chroma (0x49): channelSuffix, MIDI only
#include "model/instrument/non_audio_instrument.h"   // Chroma (0x49): getChannel(), MIDI and CV
#include "model/output.h"                            // Chroma (0x49): the track list - name, colour, usbRouting
#include "model/scale/preset_scales.h"               // Chroma (0x48): getScaleName
#include "model/settings/runtime_feature_settings.h" // Chroma (0x4D): the Features settings
#include "model/song/song.h"
#include "model/usb_route.h"                 // Chroma (0x4E): the routing mask
#include "modulation/params/param.h"         // Chroma (0x4B): which parameters make up a sound
#include "modulation/params/param_manager.h" // Chroma (0x4B)
#include "modulation/params/param_set.h" // Chroma (0x4B)                       // Chroma: resync — read the live key/scale to push on handshake
#include "playback/playback_handler.h" // Chroma: transport push - is it running, and where
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"       // Chroma (0x4B): a kit row's name
#include "processing/sound/sound_instrument.h" // Chroma (0x4B): the track's own sound
#include "storage/flash_storage.h"             // Chroma (0x4D): pad brightness
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

// Forward-declared: the resync handler below re-pushes the current view directly, and the table that
// maps a UIType to its slug is defined further down with the rest of the view code.
static const char* viewSlug(UIType t);

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
		sendPadGrid(cable); // Chroma: mirror the live pad-LED grid
		// The panel rides the pad poll: a button light can change with no display redraw, and the pads
		// are the one thing a host asks for continuously. De-duped, so an unchanged panel costs nothing.
		sendPanelStateIfChanged();
		// ...and so does the device state: selecting a track in the song grid redraws no display either.
		sendDeviceStateIfChanged();
		// ...and the pads' notes: scrolling a keyboard layout moves every note and redraws no display.
		sendPadNotesIfChanged();
		sendPadVelocitiesIfChanged(); // zooming or scrolling the drums layout moves every velocity
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
		return; // nothing moved and nothing was reassigned
	}
	emitGoldKnob(whichKnob, (uint8_t)lv, bipolar, name);
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
		else if (currentSong != nullptr && currentSong->getCurrentClip() != nullptr
		         && currentSong->getCurrentClip()->output != nullptr) {
			soundName = currentSong->getCurrentClip()->output->name.get();
		}
	}
	else if (currentSong != nullptr) {
		Clip* clip = currentSong->getCurrentClip();
		if (clip != nullptr && clip->output != nullptr && clip->output->type == OutputType::SYNTH) {
			sound = static_cast<SoundInstrument*>(clip->output);
			paramManager = &clip->paramManager;
			soundName = clip->output->name.get();
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
		if (currentSong != nullptr && currentSong->getCurrentClip() != nullptr) {
			Output* owner = currentSong->getCurrentClip()->output;
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
//   F0 00 21 7B 01 02 4D <track> <brightness> <count> then count x (<hashHi> <hashLo> <value>) F7
//
// <track> is the position of the current clip's track in the 0x49 list, 7F for none. <brightness> is 1-25.
// Each setting is named by settingNameHash() of its stored name; <value> is its value, capped at 127.
void HIDSysex::sendDeviceStateIfChanged(bool force) {
	if (lastHidCable == nullptr) {
		return;
	}
	uint8_t msg[7 + 3 + RuntimeFeatureSettingType::MaxElement * 3 + 1];
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
void HIDSysex::sendPadGrid(MIDICable& cable) {
	const int32_t W = kDisplayWidth + kSideBarWidth; // 18
	const int32_t H = kDisplayHeight;                // 8
	uint8_t raw[8 * 18 * 3 + 18 + 18];
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
	uint8_t reply_hdr[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x50};
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	memcpy(reply, reply_hdr, 7);
	int32_t packed = pack_8bit_to_7bit(reply + 7, 600, raw, p);
	if (packed < 0) {
		return;
	}
	reply[7 + packed] = 0xf7;
	cable.sendSysex(reply, packed + 8);
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
