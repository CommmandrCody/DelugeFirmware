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

#include "gui/context_menu/context_menu.h"

namespace deluge::gui::context_menu {

/// Offered at startup when SYSTEM/RECOVER.XML exists, i.e. the Deluge lost power with unsaved work.
///
/// The startup song has ALREADY loaded by the time this appears. That ordering is the whole point:
/// declining leaves the user exactly where they expected to be, and if this prompt somehow never
/// renders they still get their own song rather than someone else's. Recovery is offered, never
/// imposed.
///
/// Derives from ContextMenuForLoading so LOAD is the accept button, which reads correctly here:
/// LOAD to bring the jam back, BACK to carry on.
class RecoverSong final : public ContextMenu {
public:
	RecoverSong() = default;

	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool acceptCurrentOption() override;

	deluge::hid::Button getAcceptButton() override { return deluge::hid::button::LOAD; }
};

extern RecoverSong recoverSong;
} // namespace deluge::gui::context_menu
