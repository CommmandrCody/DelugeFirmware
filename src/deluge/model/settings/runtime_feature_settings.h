/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
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

#include "gui/l10n/strings.h"
#include "util/container/array/resizeable_array.h"
#include "util/containers.h"
#include "util/d_string.h"
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace deluge::gui::menu_item::runtime_feature {
class Setting;
class Settings;
class DevSysexSetting;

} // namespace deluge::gui::menu_item::runtime_feature

// State declarations
enum RuntimeFeatureStateToggle : uint32_t { Off = 0, On = 1 };

// Declare additional enums for specific multi state settings (e.g. like RuntimeFeatureStateTrackLaunchStyle)
enum RuntimeFeatureStateSyncScalingAction : uint32_t { SyncScaling = 0, Fill = 1 };

enum RuntimeFeatureStateEmulatedDisplay : uint32_t { Hardware = 0, Toggle = 1, OnBoot = 2 };

/// Every setting needs to be declared in here
enum RuntimeFeatureSettingType : uint32_t {
	DrumRandomizer,
	Quantize,
	FineTempoKnob,
	CatchNotes,
	DeleteUnusedKitRows,
	AltGoldenKnobDelayParams,
	DevSysexAllowed,
	SyncScalingAction,
	HighlightIncomingNotes,
	DisplayNornsLayout,
	ShiftIsSticky,
	LightShiftLed,
	EnableDX7Engine,
	EmulatedDisplay,
	EnableKeyboardViewSidebarMenuExit,
	EnableLaunchEventPlayhead,
	DisplayChordKeyboard,
	AlternativePlaybackStartBehaviour,
	EnableGridViewLoopPads,
	AlternativeTapTempoBehaviour,
	HorizontalMenus,
	TrimFromStartOfAudioClip,
	ShowBatteryLevel,
	RoundedCorners,
	ShortcutOverlay,
	// Chroma's, appended AFTER upstream's on purpose. This enum's order is the on-flash layout of the
	// runtime feature settings, so inserting anything above an existing entry silently reassigns every
	// setting below it to a different stored byte. That has bitten this instrument before: after a big
	// firmware jump, stale bytes at moved offsets broke every encoder and survived four wrong code
	// fixes before anyone suspected the settings rather than the code.
	//
	// Keeping upstream's entries first also means a future rebase onto upstream appends rather than
	// re-shuffles. FACTORY-RESET SETTINGS after flashing across this change.
	RepairSongDates,
	ChordBrush,
	KeyboardNotePreview,
	RetrospectiveCapture,
	// SongsNewestFirst lived here from 1.3.91 to 1.3.100 and is GONE (1.3.101): a SHIFT tap in the song
	// browser switches the order on the spot, so a switch three menus deep had nothing left to do. Settings
	// are stored by NAME, so removing one moves no other setting's value; a card that still lists
	// "songsNewestFirst" has it carried along as an unknown name and written back untouched.
	//
	// What 1.3.91 taught stays true: every entry here needs its menu item in
	// gui/menu_item/runtime_feature/settings.cpp, and the count there is checked at build time.
	KitHitsOneStep, // appended last, as the note above requires
	MaxElement      // Keep as boundary
};

/// Definition for selectable options
struct RuntimeFeatureSettingOption {
	std::string_view displayName;
	uint32_t value; // Value to be defined as typed Enum above
};

/// Every setting keeps its metadata and value in here
struct RuntimeFeatureSetting {
	deluge::l10n::String displayName;
	std::string_view xmlName;
	uint32_t value;
	// Limited to safe memory
	deluge::vector<RuntimeFeatureSettingOption> options;
};

class Serializer;
class Deserializer;

/// Encapsulating class
class RuntimeFeatureSettings {
public:
	RuntimeFeatureSettings();

	// Traded type safety for option values for code simplicity and size, use enum from above to compare
	inline uint32_t get(RuntimeFeatureSettingType type) { return settings[type].value; };
	inline bool isOn(RuntimeFeatureSettingType type) { return get(type) == RuntimeFeatureStateToggle::On; }

	/**
	 * Set a runtime feature setting.
	 *
	 * Make sure when you use this the settings are eventually written back to the SDCard!
	 */
	inline void set(RuntimeFeatureSettingType type, uint32_t value) { settings[type].value = value; }

	/// Chroma (0x4D / 0x4E): one setting, read-only, for reporting it to a host and checking what a host asks
	/// for against the options it really has. `t` must be below MaxElement.
	[[nodiscard]] inline const RuntimeFeatureSetting& hostSetting(uint32_t t) const { return settings[t]; }

	inline const char* getStartupSong() { return startupSong.get(); }
	void init();
	void factoryReset(bool showPopup = true);
	void readSettingsFromFile();
	void writeSettingsToFile();

protected:
	std::array<RuntimeFeatureSetting, RuntimeFeatureSettingType::MaxElement> settings = {};
	String startupSong;

private:
	ResizeableArray unknownSettings;

public:
	friend class deluge::gui::menu_item::runtime_feature::Setting;
	friend class deluge::gui::menu_item::runtime_feature::Settings;
	friend class deluge::gui::menu_item::runtime_feature::DevSysexSetting;
};

/// Static instance for external access
extern RuntimeFeatureSettings runtimeFeatureSettings;
