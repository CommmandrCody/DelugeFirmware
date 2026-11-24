#include "definitions_cxx.hpp"
#include "io/midi/midi_device_manager.h"

namespace HIDSysex {
void requestOLEDDisplay(MIDICable& cable, uint8_t* data, int32_t len);
void request7SegDisplay(MIDICable& cable, uint8_t* data, int32_t len);
void sysexReceived(MIDICable& cable, uint8_t* data, int32_t len);
void sendOLEDData(MIDICable& cable, bool rle);
void sendOLEDDataDelta(MIDICable& cable, bool force);
void send7SegData(MIDICable& cable);
// Chroma: reply with the real text behind the 7-seg segments as ASCII (so the host mirrors "BASS", not "BA55").
void send7SegText(MIDICable& cable);
void sendDisplayIfChanged();
void readBlock(MIDICable& cable);
// Chroma: emit a LEARN context event to the host (Companion renders the docs). Sent on the last cable that
// talked to us over HID SysEx; a no-op until a host has handshaked. See chroma-schema SCHEMA.md 2.1.
void sendLearnContext(uint8_t region, uint8_t x, uint8_t y, const char* contextId);
// Chroma: outbound chord-state push on every NORMAL palette pick — host renders the chord (key + voiced
// notes + ctx), immune to the note-name overwrite that defeats the text mirror. No-op until a host handshakes.
// voicingWalk/voicingHome are BIAS-64 encoded on the wire (value + 64), because they are signed
// (-24..24) and everything else in the extras block is a raw 7-bit unsigned byte. voiceOctaves is a
// bitmask that already fits in 7 bits; octaveBase is small and positive.
void sendChordState(uint8_t keyRoot, const int16_t* notes, uint8_t numNotes, const char* contextId, int8_t spread = 0,
                    int8_t inversion = 0, uint8_t scale = 0, const char* chordName = nullptr, int8_t voicingWalk = 0,
                    int8_t voicingHome = 0, uint8_t voiceOctaves = 0x08, int8_t octaveBase = 3,
                    bool voicingSplitMode = false, int8_t voicingSplit = 0);
// Chroma: outbound active-view push (0x45). Sent when the on-screen view changes, so the Companion's Learn tab can
// auto-follow what the user is looking at. Payload is a short 7-bit-ASCII view slug (e.g. "clip", "song", "kit").
// No-op until a host handshakes; last-value de-duped by the caller so it only fires on an actual change.
void sendActiveView(const char* viewId);
// Chroma: poll the current UIType and, if it changed, broadcast the view slug (0x45). Cheap + de-duped; called
// from the display passenger poll so it catches every view change without hooking the UI navigation stack.
void sendActiveViewIfChanged();
// Chroma: push the "fx" context (0x45) when a gold knob is turned, so a following host jumps to FX. Throttled to
// once per fx-session (re-armed when the real view changes) and doesn't disturb the view de-dupe.
void sendFxContext();
// Chroma: outbound gold-knob push (0x46) — whichKnob (0=upper, 1=lower), the LED level (0..128, the SAME number
// the physical four-LED column is showing, so the host cannot disagree with the hardware), whether the parameter
// is bipolar, and its display name. Sent whenever the indicator level is set, which covers both a turn and a
// context change, so a host that has just connected gets the current positions rather than waiting for a turn.
// De-duped on (knob, level, name) so a redraw does not flood the wire.
void sendGoldKnob(uint8_t whichKnob, int32_t level, bool bipolar, const char* paramName);
// Push both gold knobs as they currently stand, ignoring the de-dupe. Called from the handshake, so a
// host that connects long after the Deluge booted gets the real positions instead of two dark knobs
// waiting for someone to touch the instrument.
void pushGoldKnobs();
// Chroma: outbound transport push (0x47) - whether the clock is running, the tempo, and the
// playhead's bar and beat. De-duped on the BEAT rather than the tick, so a host gets a message four
// times a bar instead of hundreds: a second screen wants to know where you are, not to re-render at
// audio rate. Forced on the handshake so a Cockpit that connects mid-song is not blank until the
// next beat.
void sendTransportIfChanged(bool force = false);

/**
 * Which keyboard layout, what key and scale, and the span of notes on the grid.
 *
 * All eight keyboard layouts collapse to the single view slug "keyboard" over 0x45, so a host could
 * not tell isomorphic from in-key from piano; and the key escaped only through 0x43, which fires on
 * a harmonic palette pick and never in an ordinary layout. De-duped internally - safe to call on
 * every redraw.
 */
void sendKeyboardStateIfChanged(bool force = false);

/// Chroma (0x49): every track in the song - name, colour, USB routing, mute/solo/armed.
/// De-duped like the other state pushes, and sent in the hello as well as on change.
void sendTrackStateIfChanged(bool force = false);
/// Chroma (0x4A): which menu is open, its reachable entries and the cursor. De-duped by a hash, so
/// it is silent while the player is not navigating.
void sendMenuContextIfChanged(bool force = false);
void sendSoundStateIfChanged(bool force = false);
void receiveMixerSet(MIDICable& cable, uint8_t* data, int32_t len);
void sendDeviceStateIfChanged(bool force = false);
void sendPanelStateIfChanged(bool force = false);
void sendPadNotesIfChanged(bool force = false);
void sendPadVelocitiesIfChanged(bool force = false);
void sendKeyboardColumnsIfChanged(bool force = false);
/// Chroma (0x55): the sample the sound in focus plays - file, repeat mode, start, end and loop.
/// De-duped like the other state pushes; not sent, and not marked as sent, while the card is busy.
void sendSampleStateIfChanged(bool force = false);
/// Chroma (0x5B): about once a second, what the USB audio stream sent in that time. Silent while no host is
/// streaming and nothing has changed.
void sendHealthIfDue(bool force = false);
/// Chroma (0x57): the chord being held on the keyboard, in the words the display uses. De-duped.
void sendHeldChordIfChanged(bool force = false);
/// Chroma (0x5C): a diagnostic - the raw reading of each of the converter's eight inputs, when one moves.
void sendAnalogueProbeIfChanged(bool force = false);
// Chroma complete-sync WRITE direction: a host (CT/Companion) sends a voicing-mod (0x44); the Deluge applies it to
// its OWN held chord and re-broadcasts 0x43, so every surface stays in sync. The MIDI-receive context only stashes
// it in a one-slot inbox; the Harmonic layout drains it on the UI/graphics thread (where re-voicing + broadcast run
// safely). This receive path NEVER sounds notes — state + re-broadcast only. modType 0=spread, 1=inversion.
void receiveChordApply(uint8_t modType, uint8_t value);
bool hasChordMod();                                  // UI poll: is a voicing-mod waiting to be applied?
bool takeChordMod(uint8_t* modType, uint8_t* value); // UI drain: pop the pending mod (returns false if none)
// Chroma: mirror the actual pad-LED grid (8 rows x 18 cols RGB) the user sees, so the host shows EXACTLY
// the device surface and reflects every control change. Reads the live LED buffer; polled by the host.
void sendPadGrid(MIDICable& cable);
} // namespace HIDSysex
