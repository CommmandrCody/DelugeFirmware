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
} // namespace HIDSysex
