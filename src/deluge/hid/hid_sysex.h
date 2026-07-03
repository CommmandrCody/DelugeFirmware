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
void sendChordState(uint8_t keyRoot, const int16_t* notes, uint8_t numNotes, const char* contextId, int8_t spread = 0,
                    int8_t inversion = 0);
// Chroma: mirror the actual pad-LED grid (8 rows x 18 cols RGB) the user sees, so the host shows EXACTLY
// the device surface and reflects every control change. Reads the live LED buffer; polled by the host.
void sendPadGrid(MIDICable& cable);
} // namespace HIDSysex
