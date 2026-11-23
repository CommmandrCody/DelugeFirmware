/*
 * Which part of a file name is the song, and which part is the version.
 *
 * Used by the song browser's "latest" order, which shows one row per song: its newest version.
 * Cody, 2026-11-23: "collapse versions to the latest".
 */
#pragma once

#include <cstdint>
#include <cstring>

namespace deluge::version_name {

inline bool isDigit(char c) {
	return c >= '0' && c <= '9';
}
inline bool isSeparator(char c) {
	return c == ' ' || c == '_' || c == '-';
}
inline char lower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/// How many leading characters of `name` are the song's own name, with the extension and any version
/// suffix left off.
///
/// Two kinds of suffix count as a version, because both are in use on this instrument:
///
///   "TRACK 4", "TRACK_4", "TRACK-4"   a number after a separator - how a player numbers their saves
///   "SONG751A"                         one capital letter straight after a digit - the Deluge's own
///                                      "save as new version"
///
/// A number that is simply the end of the name is NOT a version: "SONG751" and "SONG752" are two
/// songs, and treating them as one would fold every default-named song on the card into a single row.
/// A name that would be left empty is kept whole.
inline int32_t baseLength(char const* name) {
	char const* dot = strrchr(name, '.');
	int32_t full = dot != nullptr ? (int32_t)(dot - name) : (int32_t)strlen(name);
	int32_t len = full;
	if (len >= 2 && name[len - 1] >= 'A' && name[len - 1] <= 'Z' && isDigit(name[len - 2])) {
		len--; // the Deluge's letter: "SONG751A" -> "SONG751", "MIX 2B" -> "MIX 2"
	}
	int32_t k = len;
	while (k > 0 && isDigit(name[k - 1])) {
		k--;
	}
	if (k < len && k > 0 && isSeparator(name[k - 1])) {
		len = k;
		while (len > 0 && isSeparator(name[len - 1])) {
			len--;
		}
	}
	return len > 0 ? len : full;
}

/// True when two file names are versions of the same song. Case does not matter; the card's does not.
inline bool sameSong(char const* a, char const* b) {
	int32_t la = baseLength(a), lb = baseLength(b);
	if (la != lb) {
		return false;
	}
	for (int32_t i = 0; i < la; i++) {
		if (lower(a[i]) != lower(b[i])) {
			return false;
		}
	}
	return true;
}

} // namespace deluge::version_name
