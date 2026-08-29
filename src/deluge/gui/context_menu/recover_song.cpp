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

#include "gui/context_menu/recover_song.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/load/load_song_ui.h"
#include "hid/display/display.h"
#include "model/song/song.h"
#include "storage/storage_manager.h"

namespace deluge::gui::context_menu {

RecoverSong recoverSong{};

char const* RecoverSong::getTitle() {
	using enum l10n::String;
	return l10n::get(STRING_FOR_RECOVER_UNSAVED_QMARK);
}

std::span<char const*> RecoverSong::getOptions() {
	using enum l10n::String;
	static char const* options[] = {l10n::get(STRING_FOR_RECOVER)};
	return {options, 1};
}

bool RecoverSong::acceptCurrentOption() {
	// Close ourselves BEFORE loading. openUI() pushes onto the navigation hierarchy, so loading with
	// this menu still on the stack would leave it sitting underneath the recovered song, waiting to
	// reappear. Pop first, then load into the space we just left.
	//
	// Returning true afterwards is deliberate: a false return makes ContextMenu::buttonAction call
	// close() itself, which would be a second close on an already-closed menu.
	display->setNextTransitionDirection(-1);
	close();

	// The card is not a promise. Between boot and this button press the user could have pulled it,
	// so re-check rather than trusting the existence test that put this prompt on screen.
	if (!StorageManager::fileExists("SYSTEM/RECOVER.XML")) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_ERROR_FILE_NOT_FOUND));
		return true;
	}

	currentSong->setSongFullPath("SYSTEM/RECOVER.XML");
	if (openUI(&loadSongUI)) {
		loadSongUI.performLoad();
		// Loaded out of SYSTEM/, so without this the Deluge would call the song "RECOVER" and try to
		// save it back into SYSTEM/. Clear the name so Save asks for a real one, and point it at
		// SONGS/ so the recovered jam saves like any other song.
		currentSong->name.clear();
		currentSong->dirPath.set("SONGS");
	}

	return true;
}

} // namespace deluge::gui::context_menu
