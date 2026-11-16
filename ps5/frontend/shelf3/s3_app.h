// PS2 shelf v3 (AI-assisted): the shelf's screens in the PS2 menu's own language (OSDSYS 2.00, rebuilt from our spec): the opening's
// hand-off into the main menu, the main menu (tunnel, clock orbs, Browser / System Configuration), the Browser (Memory Card, Library,
// Disc), the memory card pages, the Library, the game page and the settings. The state lives here; the code is App's (s3_app.inc,
// included by fe_app.cpp), so it can use the shelf's games, covers, settings sheet and disc watcher.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "s3_bios.h"
#include "s3_kit.h"
#include "s3_memcard.h"
#include "s3_seq.h"

#include <map>
#include <string>
#include <vector>

namespace s3
{
enum class Scr
{
	Menu,     // the main menu
	Browser,  // the Browser's top level
	Card,     // Memory Card (PS2)/1: the card page
	Library,  // the Library page
	Game,     // a game's page
	Settings, // System Configuration (settings for all games, or one game's)
	Opening,  // the PS2's opening (boot.md 2), then the main menu fades in
};

// A save state of a game (PCSX2's savestates/<serial> (<crc>).<nn>.p2s, or .resume.p2s = Auto).
struct State
{
	std::string path;
	int slot = 0; // 1..; 0 = Auto (the resume state); -1 = New (a clean boot)
	int64_t mtime = 0;
	int disc = 0; // which of the game's discs it was saved from (the game page's g_discs)
};

struct Shelf3
{
	bool on = true;
	Bios bios;
	Kit kit;
	SeqPlayer clocks; // CLOKS: the main menu's ambience (sound.md 4)
	SeqPlayer boots, tnnls; // the opening's music (BOOTS) and its end (TNNLS, no disc)
	bool clocks_ok = false;
	bool kit_ready = false;
	std::string top_dir;      // /data/PCSX2
	std::string states_dir;   // /data/PCSX2/savestates
	std::string memcard_path; // the slot 1 card
	MemoryCard card;
	MemoryCard card2;          // slot 2's card: the Copy destination
	std::string memcard2_path;
	int copy_t = 0;            // the copy's progress counter (40 frames, browser.md 8)
	double card_read = -100;

	// Settings of ours (gs.ini PS5SX2/ShelfLayout, ShelfShape, DiscSlot), read when a screen opens.
	bool large = false; // Library layout B (5x3)
	bool wide = true;   // 16:9 (else 4:3)
	bool disc_always = false;
	bool demo_towers = false;
	bool no_bios = false;               // no BIOS file found anywhere: the "PS2 BIOS needed" setup screen instead of the shelf
	std::vector<std::string> bios_dirs; // where it looked (shown on that screen)
	double bios_checked = -1;           // when Check again last ran (its note)
	std::vector<std::string> fan_pick; // the fan's four games, picked at random once per launch (serials)
	double disc_spin = 0, disc_spin_prev = 0; // the Browser disc art's angle, accumulated per tick (its rate follows focus; never resets)
	int blook = 1; // gs.ini PS5SX2/BrowserLook (1..6): the Browser top level's alternatives (mockups 03br-*); 1 = the shipped one
	// The opening's history (boot.md 2.8): the card's /B?DATA-SYSTEM/history when it has one (21 entries: id, play count, extra-cell
	// mask, main cell), read when the opening starts.
	std::vector<s3::MemoryCard::HistoryEntry> history;
	std::vector<bool> history_used; // gs.ini PS5SX2/DemoTowers=true: the opening's towers from a demo set (no play time is written or shown)
	int lib_kind = 0;   // the Library's object on the Browser (gs.ini PS5SX2/LibraryIcon: stack, case, shelf, card; fan, cycle, open)
	Ico lib_ico[3];
	Ico lib_hdd[2];        // our own low-poly hard drives for the Library object (BrowserLook 15-17): a 3.5" drive, a bay module

	// The 50 Hz clock every BIOS animation steps on.
	double acc = 0;
	uint64_t F = 0;
	// Drawing between ticks (any display rate): fr = how far into the next 50 Hz tick this frame is (0..1); Fr = F + fr.
	float fr = 0;
	double Fr = 0;
	// The menu clock (mainmenu.md 1.2): local time stepped 20 ms a tick, slewed to the real clock (set at once when > 3 s off).
	double clk = -1;
	double sim_t = 0;     // real seconds fed to S3Update (timing log, S3_TIMING)
	uint64_t builds = 0;  // frames built (timing log)
	int log_scr = -1, log_mode = -1;

	// Screen and fades.
	Scr scr = Scr::Menu;
	Scr next = Scr::Menu;
	int fade = 0;            // frames of the black fade still to go
	int fade_len = 1;
	bool fading_out = false;
	Scr settings_from = Scr::Menu;

	// The opening (boot.md 2).
	int op_cnt = 0, op_stage = 0, op_logo = 0, op_logo_dir = 0;
	float op_P[3] = {0, 0, 16}, op_V[3] = {0, 0, 0.04f}, op_A[3] = {}, op_J[3] = {};
	float op_R = -0.12f, op_W = 0.001f, op_Q = 0;
	float op_Pp[3] = {0, 0, 16}, op_Rp = -0.12f; // the camera a tick ago (drawn in between)
	float op_Ch[5][4] = {}; // the camera (x, y, z, roll) now and 1..4 ticks ago: the lights' smear copies go through their own tick's camera (boot.md 2.6)
	float op_scroll[6] = {};
	float op_hist[4][5][3] = {}; // the lights' last projections (smear)
	float op_trail[4][128][3] = {}; // projected light positions (x, y, z)
	int op_trail_n = 0, op_trail_w = 0;
	int op_skip = 0;
	struct TowerFace
	{
		float v[4][3];
		uint8_t c[4];
		float uv[4][2];
	};
	std::vector<TowerFace> towers;

	// Main menu (spec mainmenu.md).
	int menu_cur = 0;
	int menu_c = 0;            // fade count 0..128
	int menu_mode = 2;         // 2 entering, 0 idle, 3 leaving
	int menu_wait = 0;         // black frames before the entry starts (after the opening: f362 -> loop f385, c = 1 at f387)
	// Multi-disc games (one Library entry each): the image paths of an entry's discs, by path (DiscSet, cached); the game page's discs
	// and the one picked to boot; the full-list index of what starts (another disc than the entry's), -1 = the entry.
	mutable std::map<std::string, std::vector<std::string>> disc_sets;
	std::vector<std::string> g_discs;
	std::vector<std::string> g_disc_serials;
	int g_disc = 0;
	int launch_index = -1;
	double launch_fade_t0 = -1; // a game started off the game page (m_time): the page, choice and all, fades to black (kLaunchFade)
	int menu_words = 0;        // 0..8
	int menu_spot = 0;         // 0..66
	float menu_d = -100;       // dolly
	float orb_ah = 0, orb_as = 0, orb_m = -1;
	float orb_phi[7] = {};
	bool orb_all_fly = true;
	struct Trail
	{
		float x[50], y[50], z[50];
		uint8_t r[50], g[50], b[50];
		int head = 0, count = 0;
	};
	Trail trails[7];
	int trail_tick = 0;
	uint64_t trail_F = 0;
	double ask_allowed_until = -1;
	bool resync = false; // another panel had the pad: its held buttons are not new presses // the disc question shows only after X on the disc object
	bool version_open = false;
	int ver_t = 0, ver_wait = 0, ver_cur = 3, ver_closing = 0; // Version Information (sysconf.md 10): fade 0..8 after 4 frames, cursor
	int ver_sub = 0, ver_sub_t = 0, ver_sub_cur = 0;             // its PlayStation Driver dialog (10.5): open, fade, cursor
	int menu_leave_to = 0; // 0 Browser, 1 settings

	// Browser root (browser.md 5, 6).
	int b_cur = 1;         // 0 memory card, 1 library, 2 disc
	int b_appear[3] = {14, 14, 14};
	bool disc_in = false;
	int disc_leave = 0;
	bool eject_ask = false;
	bool was_copying = false; // a disc copy was running (its end plays the BIOS's "completed" effect) // Square on the disc: "Eject disc?" Yes / No (No first)     // a disc taken out shrinks away over 14 frames (the appear motion backwards)
	float b_x[3] = {-12, 12, 24}; // the objects' x, eased when the row closes up after the disc left
	std::string disc_title, disc_serial;
	int dcc = 21, dcc_dir = 0; // page counter (21 shown .. 0 hidden) and its direction
	Scr page_to = Scr::Browser;
	float flare_h = 1088, flare_phase = 0;
	int flare_rgb[4] = {48, 98, 128, 60};
	bool checking = false;
	double checking_since = -10;
	std::string note;
	double note_time = -10;
	float cam[3] = {0, 2.7f, 160};

	// Card page and save menu.
	int c_cur = 0, c_top = 0;
	int c_detail = 0;        // 0 page, 1 save menu, 2 delete confirm, 3 deleting, 4 message
	bool open_game_after = false; // back from a game: its page once the opening is over
	int c_shown = 0;           // the last open save view (its look while it fades out)
	int dd0 = 0;             // the detail counter 0..25
	int c_opt = 0;           // Copy / Delete
	int yes_no = 1;          // 0 Yes, 1 No
	int del_t = 0;
	std::string msg;
	float big_rot = 0;
	float wander[3] = {0, 0, 200}, wander_to[3] = {0, 0, 200};
	int wander_t = 0;
	// The blue scroll arrows (browser.md 6.2, P2-21) of the Library and the card page: how far each has faded in (0..1), and when.
	struct Arrows
	{
		float up = 0, down = 0;
		double t = -1;
	};
	Arrows lib_arrows, card_arrows;

	// Library.
	int filter = 0;          // 0 all, 1 recent, 2 favourites
	int sort = 0;            // 0 A-Z, 1 last played, 2 play time
	bool options_open = false;
	int options_row = 0;
	bool searching = false;
	int key_row = 0, key_col = 0;
	std::string query;
	float scroll = 0;        // top row, eased
	int top = 0;

	// Game page.
	int g_index = -1;        // Init's index of the game
	int g_cur = 0;           // 0 Play, 1 Favourite, 2 Settings
	std::vector<State> states;
	int g_resume = -1;       // the save state Play's choice offers (index in states, 1 = newest), -1 none
	bool g_confirm = false;  // Play's choice is up (Start normally / Load save state)
	std::vector<uint8_t> g_ico_bytes; // the game's own card save icon
	Ico g_ico;
	IconSys g_sys;
	bool g_has_ico = false;
	int g_dd0 = 0;
	float case_spin = 0, case_spin_vel = 0; // the right stick turns the focused case (Library) and the game page's case
	bool case_hold = false;                 // R3: the case stays turned
	std::string chosen_state; // the save state a pick starts from ("" = a clean boot)

	// Settings (System Configuration).
	bool st_global = true;
	int st_tab = 0;
	std::vector<std::vector<int>> st_rows; // per tab: rows of m_sheet (indices), in order
	std::vector<std::string> st_tab_names;
	int st_row = 0;
	bool st_edit = false;
	std::string st_edit_was;
	int st_confirm = 0;      // 0 none, 1 Reset to Global (Yes/No)
	int st_open = 0;         // 0..74 the open counter
	int st_ring_anim = 0;
	int st_ring_pos = 0;      // the settings cube wheel's slot (0..59)
	float prev_spin = 0;      // the case left behind, turning back to its front
	int prev_spin_pos = -1;
	int st_ring_dir = 0;      // +1 down, -1 up: the way the cube ring turns (19 frames a step, sysconf.md 7)
	bool st_closing = false;  // Circle: the open plays backwards, then the screen behind
	int c_props = 0;          // the card page's Properties (Triangle) 0..25
	int ask_dd0 = 0;          // the disc question's detail counter
	float ask_rot = 0, ask_w = 0.048f; // the question's disc: its turn and its speed per tick (browser.md 7: 0.048)
	double spin_t0 = -1;      // Play picked on the question: the disc spins up (from this m_time), then its game starts (kurolume, 2026-11-16)
	int prev_item = -1, item_alpha = 128, prev_alpha = 0;
	double st_pulse = 0;

	// Per-game records.
	std::vector<std::string> favourites;
	struct Played
	{
		uint64_t seconds = 0;
		int64_t last = 0;
	};
	std::map<std::string, Played> played; // by serial (playtime.dat)

	// Input edges.
	bool held_valid = false;
	int rep_dir = 0, rep_frames = 0;
};
} // namespace s3

// The App members of the shelf v3 (fe_app.h expands this inside class App).
#define S3_APP_MEMBERS                                                                                                                  \
	s3::Shelf3 m_s3;                                                                                                                    \
	bool S3Active() const { return m_s3.on; }                                                                                           \
	void S3Init();                                                                                                                      \
	void S3NoBiosBuild(FrameDesc& f, float W, float H);                                                                                 \
	void S3ReadSettings();                                                                                                              \
	void S3ReadRecords();                                                                                                               \
	bool S3Favourite(const std::string& serial) const;                                                                                  \
	void S3SetFavourite(const std::string& serial, bool on);                                                                            \
	void S3Update(double dt, const Input& in);                                                                                          \
	void S3Tick(const Input& in);                                                                                                       \
	void S3Build(FrameDesc& f, float W, float H, const std::string& clock);                                                             \
	void S3Go(s3::Scr to, int fade_frames);                                                                                             \
	void S3Sfx(int effect);                                                                                                             \
	void S3MenuTick(const Input& in);                                                                                                   \
	void S3MenuBuild(FrameDesc& f);                                                                                                     \
	void S3BrowserTick(const Input& in);                                                                                                \
	void S3BrowserBuild(FrameDesc& f);                                                                                                  \
	void S3CardTick(const Input& in);                                                                                                   \
	void S3CardBuild(FrameDesc& f);                                                                                                     \
	void S3LibraryTick(const Input& in);                                                                                                \
	void S3LibraryBuild(FrameDesc& f, bool behind);                                                                                     \
	void S3GameOpen(int pos);                                                                                                           \
	void S3GameTick(const Input& in);                                                                                                   \
	void S3GameBuild(FrameDesc& f);                                                                                                     \
	void S3SettingsOpen(bool global);                                                                                                   \
	void S3SettingsTick(const Input& in);                                                                                               \
	void S3SettingsBuild(FrameDesc& f, const std::string& clock);                                                                       \
	void S3Launch(const std::string& state);                                                                                            \
	bool S3SearchMatches(const GameInfo& g) const;                                                                                      \
	bool S3Keep(const GameInfo& g, int index) const;                                                                                    \
	void S3SortShelf();                                                                                                                 \
	void S3ReadCard();                                                                                                                  \
	int S3Cols() const;                                                                                                                 \
	int S3Rows() const;                                                                                                                 \
	void S3DrawCase(int pos, float cx, float cy, float h_px, float yaw, float pitch, float alpha, bool flat);                            \
	void S3PollDisc();                                                                                                                  \
	void S3SettingsLeave();                                                                                                             \
	void S3DiscAskBuild(FrameDesc& f);                                                                                                  \
	int S3SaveGame(const s3::Save& sv) const;                                                                                           \
	std::vector<int> S3RecentGames(int n) const;                                                                                        \
	void S3DrawLibraryCases(const float* M, const s3::Ico* shape, float ry, float scale, float tip = 0);                                           \
	void S3OpeningStart();                                                                                                              \
	void S3OpeningTick(const Input& in);                                                                                                \
	void S3OpeningBuild(FrameDesc& f);
