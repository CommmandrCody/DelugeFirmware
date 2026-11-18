/*
 * Copyright © 2021-2023 Synthstrom Audible Limited
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

#include "settings.h"
#include "devSysexSetting.h"
#include "emulated_display.h"
#include "hid/display/display.h"
#include "hid/display/oled_canvas/canvas.h"
#include "setting.h"
#include "shift_is_sticky.h"
#include <array>

extern deluge::gui::menu_item::runtime_feature::Setting runtimeFeatureSettingMenuItem;

namespace deluge::gui::menu_item::runtime_feature {

// A SettingToggle that is only relevant (and thus only shown) when an OLED display is
// active — real or emulated. Used for features that have no effect in 7-segment mode.
class OledOnlySettingToggle : public SettingToggle {
public:
	using SettingToggle::SettingToggle;
	bool isRelevant(ModControllableAudio*, int32_t) const override { return display->haveOLED(); }
};

// The RoundedCorners toggle additionally pushes its value into the Canvas drawing flag,
// so the low-level primitives don't have to read runtime feature settings themselves.
class RoundedCornersSettingToggle final : public OledOnlySettingToggle {
public:
	using OledOnlySettingToggle::OledOnlySettingToggle;
	void writeCurrentValue() override {
		OledOnlySettingToggle::writeCurrentValue();
		hid::display::oled_canvas::Canvas::roundedCornersEnabled =
		    runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::RoundedCorners);
	}
};

// Generic menu item instances
SettingToggle menuDrumRandomizer(RuntimeFeatureSettingType::DrumRandomizer);
SettingToggle menuFineTempo(RuntimeFeatureSettingType::FineTempoKnob);
SettingToggle menuQuantize(RuntimeFeatureSettingType::Quantize);
SettingToggle menuCatchNotes(RuntimeFeatureSettingType::CatchNotes);
SettingToggle menuDeleteUnusedKitRows(RuntimeFeatureSettingType::DeleteUnusedKitRows);
SettingToggle menuAltGoldenKnobDelayParams(RuntimeFeatureSettingType::AltGoldenKnobDelayParams);
Setting menuSyncScalingAction(RuntimeFeatureSettingType::SyncScalingAction);
DevSysexSetting menuDevSysexAllowed(RuntimeFeatureSettingType::DevSysexAllowed);
SettingToggle menuHighlightIncomingNotes(RuntimeFeatureSettingType::HighlightIncomingNotes);
SettingToggle menuDisplayNornsLayout(RuntimeFeatureSettingType::DisplayNornsLayout);
ShiftIsSticky menuShiftIsSticky{};
SettingToggle menuLightShiftLed(RuntimeFeatureSettingType::LightShiftLed);
SettingToggle menuEnableDX7Engine(RuntimeFeatureSettingType::EnableDX7Engine);
EmulatedDisplay menuEmulatedDisplay{};
SettingToggle menuEnableKeyboardViewSidebarMenuExit(RuntimeFeatureSettingType::EnableKeyboardViewSidebarMenuExit);
SettingToggle menuEnableLaunchEventPlayhead(RuntimeFeatureSettingType::EnableLaunchEventPlayhead);
SettingToggle menuDisplayChordLayout(RuntimeFeatureSettingType::DisplayChordKeyboard);
SettingToggle menuChordBrush(RuntimeFeatureSettingType::ChordBrush);
SettingToggle menuAlternativePlaybackStartBehaviour(RuntimeFeatureSettingType::AlternativePlaybackStartBehaviour);
SettingToggle menuEnableGridViewLoopPads(RuntimeFeatureSettingType::EnableGridViewLoopPads);
SettingToggle menuAlternativeTapTempoBehaviour(RuntimeFeatureSettingType::AlternativeTapTempoBehaviour);
OledOnlySettingToggle menuHorizontalMenus(RuntimeFeatureSettingType::HorizontalMenus);
SettingToggle menuTrimFromStartOfAudioClip(RuntimeFeatureSettingType::TrimFromStartOfAudioClip);
SettingToggle menuShowBatteryLevel(RuntimeFeatureSettingType::ShowBatteryLevel);
RoundedCornersSettingToggle menuRoundedCorners(RuntimeFeatureSettingType::RoundedCorners);
SettingToggle menuShortcutOverlay(RuntimeFeatureSettingType::ShortcutOverlay);
SettingToggle menuRepairSongDates(RuntimeFeatureSettingType::RepairSongDates);
SettingToggle menuRetrospectiveCapture(RuntimeFeatureSettingType::RetrospectiveCapture);
SettingToggle menuSongsNewestFirst(RuntimeFeatureSettingType::SongsNewestFirst);
SettingToggle menuKitHitsOneStep(RuntimeFeatureSettingType::KitHitsOneStep);

std::array<MenuItem*, RuntimeFeatureSettingType::MaxElement - kNonTopLevelSettings> subMenuEntries{
    &menuDrumRandomizer, &menuFineTempo, &menuQuantize, &menuCatchNotes, &menuDeleteUnusedKitRows,
    &menuAltGoldenKnobDelayParams, &menuDevSysexAllowed, &menuSyncScalingAction, &menuHighlightIncomingNotes,
    &menuDisplayNornsLayout, &menuShiftIsSticky, &menuLightShiftLed, &menuEnableDX7Engine, &menuEmulatedDisplay,
    &menuEnableKeyboardViewSidebarMenuExit, &menuEnableLaunchEventPlayhead, &menuDisplayChordLayout, &menuChordBrush,
    &menuRetrospectiveCapture, &menuAlternativePlaybackStartBehaviour, &menuEnableGridViewLoopPads,
    &menuAlternativeTapTempoBehaviour, &menuHorizontalMenus, &menuRoundedCorners, &menuTrimFromStartOfAudioClip,
    &menuShowBatteryLevel,
    // menuRetrospectiveCapture is already listed above, with the other Chroma entries - it came
    // through the clean half of the merge. Listing it twice puts the same menu item on screen twice.
    &menuShortcutOverlay, &menuRepairSongDates, &menuSongsNewestFirst, &menuKitHitsOneStep};

// EVERY SLOT MUST HOLD A MENU ITEM. The array is sized from the enum, so adding a setting without adding
// its item here leaves a null at the end, and the menu follows it. That compiles, boots, and locks the
// instrument up the moment Features is opened (1.3.91). Counted at build time so it cannot ship again.
constexpr size_t kFeatureMenuItemsListed = 30;
static_assert(
    kFeatureMenuItemsListed == RuntimeFeatureSettingType::MaxElement - kNonTopLevelSettings,
    "a runtime feature setting has no menu item (or one is listed twice): update subMenuEntries AND this count");

Settings::Settings(l10n::String name, l10n::String title) : menu_item::Submenu(name, title, subMenuEntries) {
}

} // namespace deluge::gui::menu_item::runtime_feature
