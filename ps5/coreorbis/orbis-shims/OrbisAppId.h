// PS5 port: the app's own title ID and folder, in one place (ps2-console-shelf fork, AI-assisted).
//
// The fork "PS5SX2 PS2" installs next to the live PS5SX2 (PPSA99203) under its own title ID, so both can be on the console at once. It keeps the
// shared data folder /data/PCSX2 (BIOS, saves, settings, games). Build with -DPS5SX2_TITLE_ID=\"PPSA99203\" to get the upstream identity back.
// The disc tiles (PPSA9923x) and the Disc Helper still belong to the live app: they launch PPSA99203, not this one.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef PS5SX2_TITLE_ID
#define PS5SX2_TITLE_ID "PPSA99204"
#endif

// The app folder on /data, as the jailbreak sees it.
#define PS5SX2_APP_DIR "/data/homebrew/" PS5SX2_TITLE_ID
// The live upstream app, whose folder may hold the Disc Helper ELF the fork can also load.
#define PS5SX2_UPSTREAM_TITLE_ID "PPSA99203"
#define PS5SX2_UPSTREAM_APP_DIR "/data/homebrew/" PS5SX2_UPSTREAM_TITLE_ID

// The fork never updates itself from upstream's GitHub releases: the installer installs Swordpdf's release into PPSA99203 and the
// session logger uploads logs to upstream. 1 turns both back on (only for the upstream identity).
#ifndef PS5SX2_SELF_UPDATE
#define PS5SX2_SELF_UPDATE 0
#endif
