// PS2 shelf v3 (AI-assisted): App's shelf v3 screens, part 1: start-up, the 50 Hz clock, fades, sounds, the main menu, the Browser and
// the memory card pages. Included at the end of fe_app.cpp (after fe_ps2ui_impl.h). Numbers from our OSDSYS 2.00 spec (ps2-own spec/*.md).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fe_settings.h"

#include <sys/stat.h>
#include <chrono>
#include <ctime>
#include <dirent.h>
#include <unistd.h>

namespace fe
{

namespace
{
// ---- small maths: 4x4 row-major, column vectors (p' = M p) ----
struct M4
{
	float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};
M4 MMul(const M4& a, const M4& b)
{
	M4 r;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
		{
			float s = 0;
			for (int k = 0; k < 4; k++)
				s += a.m[i * 4 + k] * b.m[k * 4 + j];
			r.m[i * 4 + j] = s;
		}
	return r;
}
M4 MRotX(float a)
{
	M4 r;
	const float c = std::cos(a), s = std::sin(a);
	r.m[5] = c, r.m[6] = -s, r.m[9] = s, r.m[10] = c;
	return r;
}
M4 MRotY(float a)
{
	M4 r;
	const float c = std::cos(a), s = std::sin(a);
	r.m[0] = c, r.m[2] = s, r.m[8] = -s, r.m[10] = c;
	return r;
}
M4 MRotZ(float a)
{
	M4 r;
	const float c = std::cos(a), s = std::sin(a);
	r.m[0] = c, r.m[1] = -s, r.m[4] = s, r.m[5] = c;
	return r;
}
M4 MTrans(float x, float y, float z)
{
	M4 r;
	r.m[3] = x, r.m[7] = y, r.m[11] = z;
	return r;
}
M4 MScale(float x, float y, float z)
{
	M4 r;
	r.m[0] = x, r.m[5] = y, r.m[10] = z;
	return r;
}
void MApply(const M4& M, float x, float y, float z, float& ox, float& oy, float& oz)
{
	ox = M.m[0] * x + M.m[1] * y + M.m[2] * z + M.m[3];
	oy = M.m[4] * x + M.m[5] * y + M.m[6] * z + M.m[7];
	oz = M.m[8] * x + M.m[9] * y + M.m[10] * z + M.m[11];
}

constexpr float kTilt = -0.78f;      // the Browser pages' camera tilt
constexpr float kPi_ = 3.14159265f;

// The Browser's view of a page point (x, y, z): Rx(tilt) then the camera translation.
M4 S3PageView(const float cam[3])
{
	M4 v = MRotX(kTilt);
	v.m[3] = cam[0], v.m[7] = cam[1], v.m[11] = cam[2];
	return v;
}

// The device/card light set (browser.md 5).
s3::Kit::Light S3CardLights(float ambient)
{
	s3::Kit::Light L = {};
	const float d[3][3] = {{0.5f, 0.5f, 0.5f}, {0, -0.4f, -1}, {-0.5f, -0.5f, 0.5f}};
	const float c[3][3] = {{0.48f, 0.48f, 0.43f}, {0.26f, 0.33f, 0.5f}, {0.14f, 0.14f, 0.38f}};
	std::memcpy(L.dir, d, sizeof(d));
	std::memcpy(L.col, c, sizeof(c));
	L.amb[0] = L.amb[1] = L.amb[2] = ambient;
	L.alpha_from_light = true;
	return L;
}

// An icon.sys light set (the save's own), else the card set.
s3::Kit::Light S3SaveLights(const s3::IconSys& sys, float rot_y)
{
	if (!sys.ok)
		return S3CardLights(0.24f);
	s3::Kit::Light L = {};
	const float c = std::cos(-rot_y), s = std::sin(-rot_y);
	for (int i = 0; i < 3; i++)
	{
		// The lights stay with the icon as it turns: into model space by the inverse turn.
		const float* d = sys.light_dir[i];
		L.dir[i][0] = c * d[0] + s * d[2];
		L.dir[i][1] = d[1];
		L.dir[i][2] = -s * d[0] + c * d[2];
		for (int k = 0; k < 3; k++)
			L.col[i][k] = sys.light_col[i][k];
	}
	for (int k = 0; k < 3; k++)
		L.amb[k] = sys.ambient[k];
	// Saves with all-zero lights would be black: the BIOS default set then.
	float sum = L.amb[0] + L.amb[1] + L.amb[2];
	for (int i = 0; i < 3; i++)
		sum += L.col[i][0] + L.col[i][1] + L.col[i][2];
	if (sum < 0.05f)
		return S3CardLights(0.24f);
	return L;
}

std::string S3Ascii(const std::string& s)
{
	// The BIOS font is ASCII: PromptFont glyphs and other UTF-8 are left out.
	std::string out;
	for (size_t i = 0; i < s.size(); i++)
	{
		const unsigned char c = static_cast<unsigned char>(s[i]);
		if (c < 0x80)
			out += static_cast<char>(c);
		else if ((c & 0xE0) == 0xC0)
			i += 1;
		else if ((c & 0xF0) == 0xE0)
			i += 2;
		else if ((c & 0xF8) == 0xF0)
			i += 3;
	}
	while (!out.empty() && out.front() == ' ')
		out.erase(out.begin());
	return out;
}

// Splits a title into two lines that fit `max_w` units at `scale` (the second line gets the rest).
void S3Split(const s3::Kit& kit, const std::string t, float scale, float max_w, std::string& a, std::string& b)
{
	a = t, b.clear();
	if (kit.TextWidth(t, scale) <= max_w)
		return;
	size_t best = std::string::npos;
	for (size_t i = 0; i < t.size(); i++)
		if (t[i] == ' ' && kit.TextWidth(t.substr(0, i), scale) <= max_w)
			best = i;
	if (best == std::string::npos)
		return;
	a = t.substr(0, best);
	b = t.substr(best + 1);
}

std::string S3IniValue(const std::string& path, const std::string& section, const std::string& key)
{
	// gs.ini and settings/<image>.ini: flat "Section/Key=value" lines (fe_settings.cpp), the last one wins.
	FILE* f = std::fopen(path.c_str(), "r");
	if (!f)
		return {};
	const std::string want = section + "/" + key + "=";
	char line[1024];
	std::string out;
	while (std::fgets(line, sizeof(line), f))
	{
		std::string l(line);
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' '))
			l.pop_back();
		while (!l.empty() && l.front() == ' ')
			l.erase(l.begin());
		if (l.compare(0, want.size(), want) == 0)
			out = l.substr(want.size());
	}
	std::fclose(f);
	return out;
}

std::string S3DateText(int64_t t)
{
	if (t <= 0)
		return "--/--/----";
	const std::time_t tt = static_cast<std::time_t>(t);
	std::tm tm = {};
	localtime_r(&tt, &tm);
	char b[32];
	std::snprintf(b, sizeof(b), "%02d/%02d/%04d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
	return b;
}
std::string S3TimeText(int64_t t)
{
	if (t <= 0)
		return "--:--:--";
	const std::time_t tt = static_cast<std::time_t>(t);
	std::tm tm = {};
	localtime_r(&tt, &tm);
	char b[32];
	std::snprintf(b, sizeof(b), "%d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
	return b;
}
std::string S3Duration(uint64_t s)
{
	char b[32];
	std::snprintf(b, sizeof(b), "%llu:%02llu:%02llu", static_cast<unsigned long long>(s / 3600), static_cast<unsigned long long>(s / 60 % 60),
		static_cast<unsigned long long>(s % 60));
	return b;
}
// The card page's space line: "7,898 KB Free" as the PS2; a folder card (no fixed size) gives the space its saves use, "11,341 KB Used".
std::string S3CardSpace(const s3::MemoryCard& card)
{
	const uint32_t kb = card.folder() ? card.used_kb() : card.free_kb();
	const char* what = card.folder() ? "Used" : "Free";
	char b[48];
	if (kb >= 1000000)
		std::snprintf(b, sizeof(b), "%u,%03u,%03u KB %s", kb / 1000000, kb / 1000 % 1000, kb % 1000, what);
	else if (kb >= 1000)
		std::snprintf(b, sizeof(b), "%u,%03u KB %s", kb / 1000, kb % 1000, what);
	else
		std::snprintf(b, sizeof(b), "%u KB %s", kb, what);
	return b;
}

// The scroll arrows (browser.md 6.2: up/down while rows lie above/below), the Library's and the card page's: blue triangles at the
// centre, as measured on a PS2 (up 33 x 21 px over the top row, down 48 x 36 px under the bottom row at 1080 lines), fixed on screen,
// each fading in or out over 0.15 s (P2-21). `Fr` is the 50 Hz clock.
void S3ScrollArrows(s3::Kit& kit, s3::Shelf3::Arrows& a, bool up, bool down, double Fr)
{
	const float step = a.t < 0 || Fr < a.t ? 1.0f : static_cast<float>((Fr - a.t) / 7.5); // 0.15 s = 7.5 ticks
	a.t = Fr;
	a.up = std::clamp(a.up + (up ? step : -step), 0.0f, 1.0f);
	a.down = std::clamp(a.down + (down ? step : -step), 0.0f, 1.0f);
	auto arrow = [&](float cy, float w, float h, bool point_up, float vis) {
		if (vis <= 0)
			return;
		kit.Bind(VK_NULL_HANDLE);
		const float k = kit.H() / 1080.0f, cx = kit.W() * 0.5f;
		const float base = cy + (point_up ? h : -h) * 0.5f * k, tip = cy - (point_up ? h : -h) * 0.5f * k;
		const float xy[6] = {cx - w * 0.5f * k, base, cx + w * 0.5f * k, base, cx, tip};
		const uint32_t c[3] = {s3::Kit::Pack(0.04f, 0.115f, 0.33f, 0.5f * vis), s3::Kit::Pack(0.04f, 0.115f, 0.33f, 0.5f * vis),
			s3::Kit::Pack(0.01f, 0.06f, 0.27f, 0.5f * vis)};
		kit.Tri(xy, nullptr, c, 11, false);
	};
	arrow(kit.Y(50), 33, 21, true, a.up);
	arrow(kit.Y(218.5f), 48, 36, false, a.down); // (low in the gap above the band, clear of the last row)
}

// Pad bits for the 50 Hz edges.
enum : uint32_t
{
	kBUp = 1,
	kBDown = 2,
	kBLeft = 4,
	kBRight = 8,
	kBCross = 16,
	kBCircle = 32,
	kBSquare = 64,
	kBTriangle = 128,
	kBL1 = 256,
	kBR1 = 512,
	kBOptions = 1024
};
uint32_t S3Bits(const Input& in)
{
	return (in.up ? kBUp : 0) | (in.down ? kBDown : 0) | (in.left ? kBLeft : 0) | (in.right ? kBRight : 0) | (in.cross ? kBCross : 0) |
	       (in.circle ? kBCircle : 0) | (in.square ? kBSquare : 0) | (in.triangle ? kBTriangle : 0) | (in.l1 ? kBL1 : 0) | (in.r1 ? kBR1 : 0) |
	       (in.options ? kBOptions : 0);
}
// This tick's presses: new presses, and (for the D-pad) the Browser's repeat: first after 20 frames held, then every 7 (browser.md 12).
struct S3Pad
{
	uint32_t held = 0, now = 0, prev = 0, rep = 0; // rep: new presses plus repeats (directions)
	bool fresh(uint32_t b) const { return (now & b) && !(prev & b); }
	bool repeat(uint32_t b) const { return (rep & b) != 0; }
};
S3Pad g_s3pad;
uint32_t g_s3_latch = 0; // buttons seen down since the last tick (a press shorter than a 50 Hz frame still counts)
int g_s3_hold_frames = 0;
uint32_t g_s3_hold_dir = 0;
} // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------
void App::S3Init()
{
	if (!m_s3.on)
		return;
	const std::string gs = m_cfg.options.gs_ini;
	m_s3.top_dir = gs.empty() ? std::string("/data/PCSX2") : gs.substr(0, gs.find_last_of('/'));
	{
		struct stat st;
		const std::string sd = m_s3.top_dir + "/savestates";
		m_s3.states_dir = stat(sd.c_str(), &st) == 0 ? sd : m_s3.top_dir;
	}
	const auto t0 = std::chrono::steady_clock::now();
	auto ms = [&t0]() { return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()); };
	// The BIOS folders, in order: the one set in Settings (gs.ini PS5SX2/BiosFolder: then the only one, as the core's), else
	// the core's (/data/PCSX2/bios, the one the setup screen names), PCSX2's default.
	m_s3.bios_dirs.clear();
	{
		const std::string own = S3IniValue(gs, "PS5SX2", "BiosFolder");
		const std::vector<std::string> list = !own.empty() ? std::vector<std::string>{own}
		                                                  : std::vector<std::string>{m_cfg.bios_dir, m_s3.top_dir + "/bios"};
		for (const std::string& dr : list)
			if (!dr.empty() && std::find(m_s3.bios_dirs.begin(), m_s3.bios_dirs.end(), dr) == m_s3.bios_dirs.end())
				m_s3.bios_dirs.push_back(dr);
	}
	bool found = false, any_file = false;
	for (const std::string& dr : m_s3.bios_dirs)
	{
		if (m_s3.bios.LoadDir(dr))
		{
			found = true;
			break;
		}
		any_file |= m_s3.bios.problem().rfind("no PS2 BIOS in", 0) != 0;
	}
	// No BIOS file at all (and the open BIOS not in use): the setup screen. A BIOS the shelf can't read keeps the shelf (PCSX2 may run it).
	m_s3.no_bios = !found && !any_file && !(m_cfg.bios_open_in_use && m_cfg.bios_open_in_use());
	if (found)
		std::printf("[shelf3] BIOS assets from %s\n", m_s3.bios.path().c_str());
	else
		std::printf("[shelf3] no BIOS assets: %s%s\n", m_s3.bios.problem().c_str(), m_s3.no_bios ? " (the PS2 BIOS setup screen)" : "");
	const long long t_bios = ms();
	std::fflush(stdout);
	m_s3.kit.Init(m_renderer, m_fonts, &m_s3.bios);
	m_s3.kit_ready = true;
	const long long t_kit = ms();
	// CLOKS, the main menu's ambience (sound.md 4): played by the audio thread while the main menu (and System Configuration) is up.
	if (m_cfg.sound && m_s3.bios.ok() && m_s3.clocks.Load(m_s3.bios, "SNDCLOKS", 45))
	{
		m_s3.clocks_ok = true;
		s3::SeqPlayer* p = &m_s3.clocks;
		m_s3.boots.Load(m_s3.bios, "SNDBOOTS", 66);
		m_s3.tnnls.Load(m_s3.bios, "SNDTNNLS", 42);
		s3::SeqPlayer* b = &m_s3.boots;
		s3::SeqPlayer* t = &m_s3.tnnls;
		m_cfg.sound->SetStream([p, b, t](float* out, int frames) {
			b->Render(out, frames);
			t->Render(out, frames);
			p->Render(out, frames);
		});
	}
	// The shelf's other sounds (the disc question, the BIOS question, ...) as the BIOS's effects too (spec sound.md 3).
	if (m_cfg.sound && m_s3.bios.ok())
	{
		const std::pair<Sfx, int> map[] = {{Sfx::Move, 6}, {Sfx::MoveRepeat, 6}, {Sfx::Select, 4}, {Sfx::Back, 10}, {Sfx::Error, 11}, {Sfx::Open, 4},
			{Sfx::Close, 10}, {Sfx::Page, 5}, {Sfx::Done, 9}, {Sfx::Disc, 7}, {Sfx::Launch, 4}, {Sfx::Edge, 11}};
		for (const auto& [sfx, n] : map)
			if (const s3::Effect* e = m_s3.bios.Sound(n))
				m_cfg.sound->OverrideSfx(sfx, &e->stereo);
	}
	const long long t_sound = ms();
	S3ReadSettings();
	S3ReadRecords();
	std::printf("[shelf3] start-up: BIOS %lld ms, textures %lld ms, sounds %lld ms, records %lld ms\n", t_bios, t_kit - t_bios, t_sound - t_kit, ms() - t_sound);
	std::fflush(stdout);
	// The opening's hand-off (or a fresh start without it): the main menu fades in. Back from a game: that game's page.
	m_return_until = -1;
	m_s3.scr = s3::Scr::Menu;
	m_s3.menu_mode = 2;
	m_s3.menu_c = 0;
	m_s3.menu_d = -100;
	for (int i = 0; i < 7; i++)
		m_s3.orb_phi[i] = static_cast<float>((i * 40503 + 12345) % 65536) / 65536.0f * 2.0f * kPi_;
	if (m_cfg.start_library)
		m_s3.scr = s3::Scr::Library;
	// A fresh start: the PS2's opening, drawn here (boot.md 2), unless Fast Boot (Opening) is on, then the main menu fades in from it.
	else if (m_s3.no_bios)
		; // (the setup screen: S3Build / S3Update)
	else if (!m_cfg.after_bios_startup && m_s3.bios.ok() && S3IniValue(m_cfg.options.gs_ini, "PS5SX2", "StartupAnimation") != "false")
		S3OpeningStart(); // shelf v3: back from a game too, as a PS2 starts again (kurolume: the old boot towers were left there)
	m_s3.open_game_after = m_cfg.returning && !m_games.empty();
	if (m_s3.open_game_after && m_s3.scr != s3::Scr::Opening)
	{
		m_s3.open_game_after = false;
		m_s3.scr = s3::Scr::Library;
		S3GameOpen(m_selected);
	}
}

namespace
{
s3::Ico S3LibraryObject(int kind); // (below: the Library's own object)
s3::Ico S3HddObject(int kind);
}

void App::S3ReadSettings()
{
	const std::string& gs = m_cfg.options.gs_ini;
	m_s3.large = S3IniValue(gs, "PS5SX2", "ShelfLayout") == "large";
	m_s3.wide = S3IniValue(gs, "PS5SX2", "ShelfShape") != "4:3";
	m_s3.demo_towers = S3IniValue(gs, "PS5SX2", "DemoTowers") == "true";
	m_s3.blook = std::clamp(std::atoi(S3IniValue(gs, "PS5SX2", "BrowserLook").c_str()), 1, 19);
	if (S3IniValue(gs, "PS5SX2", "BrowserLook").empty())
		m_s3.blook = 18; // the default (round 3): the fan of the four latest games
	m_s3.disc_always = S3IniValue(gs, "PS5SX2", "DiscSlot") == "always";
	if (m_renderer)
		m_renderer->SetCaseBlue(S3IniValue(gs, "PS5SX2", "CaseColour") == "blue"); // shelf v3: black, or the PAL blue
	{
		const std::string li = S3IniValue(gs, "PS5SX2", "LibraryIcon");
		m_s3.lib_kind = li == "stack" ? 0 : li == "case" ? 1 : li == "card" ? 3 : li == "fan" ? 4 : li == "cycle" ? 5 : li == "open" ? 6 : li == "shelf" ? 2 : 7; // default: the stack of cases (round 3)
		if (m_s3.lib_ico[0].verts.empty())
		{
			for (int k = 0; k < 3; k++)
				m_s3.lib_ico[k] = S3LibraryObject(k);
			for (int k = 0; k < 2; k++)
				m_s3.lib_hdd[k] = S3HddObject(k);
		}
	}
	std::string slot = S3IniValue(gs, "MemoryCards", "Slot1_Filename");
	if (slot.empty())
		slot = "Mcd001.ps2";
	const std::string md = m_cfg.options.memcards_dir.empty() ? m_s3.top_dir + "/memcards" : m_cfg.options.memcards_dir;
	m_s3.memcard_path = slot.find('/') != std::string::npos ? slot : md + "/" + slot;
	std::string slot2 = S3IniValue(gs, "MemoryCards", "Slot2_Filename");
	if (slot2.empty())
		slot2 = "Mcd002.ps2";
	m_s3.memcard2_path = slot2.find('/') != std::string::npos ? slot2 : md + "/" + slot2;
	if (S3IniValue(gs, "MemoryCards", "Slot2_Enable") == "false" || m_s3.memcard2_path == m_s3.memcard_path)
		m_s3.memcard2_path.clear();
}

void App::S3ReadRecords()
{
	m_s3.favourites.clear();
	if (FILE* f = std::fopen((m_s3.top_dir + "/shelf/favourites.txt").c_str(), "r"))
	{
		char line[128];
		while (std::fgets(line, sizeof(line), f))
		{
			std::string s(line);
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
				s.pop_back();
			if (!s.empty())
				m_s3.favourites.push_back(s);
		}
		std::fclose(f);
	}
	m_s3.played.clear();
	if (FILE* f = std::fopen((m_s3.top_dir + "/playtime.dat").c_str(), "r"))
	{
		char line[256];
		while (std::fgets(line, sizeof(line), f))
		{
			if (std::strlen(line) < 34)
				continue;
			std::string serial(line, 32);
			while (!serial.empty() && serial.back() == ' ')
				serial.pop_back();
			char* end = nullptr;
			s3::Shelf3::Played p;
			p.seconds = std::strtoull(line + 33, &end, 10);
			p.last = end ? std::strtoll(end, nullptr, 10) : 0;
			if (!serial.empty())
				m_s3.played[serial] = p;
		}
		std::fclose(f);
	}
}

bool App::S3Favourite(const std::string& serial) const
{
	return !serial.empty() && std::find(m_s3.favourites.begin(), m_s3.favourites.end(), serial) != m_s3.favourites.end();
}

void App::S3SetFavourite(const std::string& serial, bool on)
{
	if (serial.empty())
		return;
	auto it = std::find(m_s3.favourites.begin(), m_s3.favourites.end(), serial);
	if (on && it == m_s3.favourites.end())
		m_s3.favourites.push_back(serial);
	else if (!on && it != m_s3.favourites.end())
		m_s3.favourites.erase(it);
	mkdir((m_s3.top_dir + "/shelf").c_str(), 0777);
	if (FILE* f = std::fopen((m_s3.top_dir + "/shelf/favourites.txt").c_str(), "w"))
	{
		for (const std::string& s : m_s3.favourites)
			std::fprintf(f, "%s\n", s.c_str());
		std::fclose(f);
	}
}

void App::S3Sfx(int effect)
{
	const s3::Effect* e = m_s3.bios.Sound(effect);
	if (m_cfg.sound && e && !e->stereo.empty())
		m_cfg.sound->PlayClip(&e->stereo);
	else if (m_cfg.sound)
	{
		// No BIOS: the console sounds (or the synthesized ones).
		const Sfx s = effect == 6 || effect == 5 ? Sfx::Move : effect == 4 ? Sfx::Select : effect == 10 ? Sfx::Back : effect == 11 ? Sfx::Error
		                                                                                     : effect == 9 ? Sfx::Done : Sfx::Select;
		m_cfg.sound->Play(s, 0.0f);
	}
}

void App::S3Go(s3::Scr to, int fade_frames)
{
	m_s3.next = to;
	m_s3.fade_len = std::max(1, fade_frames);
	m_s3.fade = fade_frames;
	m_s3.fading_out = fade_frames > 0;
	if (fade_frames <= 0)
	{
		m_s3.scr = to;
		m_s3.fade = 0;
	}
}

// The game a save belongs to: its folder names the serial after the region letters ("BESLES-52541GTASA" -> SLES-52541). Its place in
// the Library, -2 when it is the disc in the drive, -1 when neither (or a system save).
int App::S3SaveGame(const s3::Save& sv) const
{
	if (sv.builtin || sv.dir.size() < 11 || sv.dir[0] != 'B')
		return -1;
	auto norm = [](const std::string& x) {
		std::string o;
		for (char c : x)
			if (std::isalnum(static_cast<unsigned char>(c)))
				o += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return o;
	};
	const std::string tail = norm(sv.dir.substr(2));
	if (tail.size() < 9)
		return -1;
	const std::string want = tail.substr(0, 9); // SLES52541
	for (size_t i = 0; i < m_games.size(); i++)
		if (m_index[i] >= 0 && norm(m_games[i].serial) == want)
			return static_cast<int>(i);
	if (m_s3.disc_in && norm(m_s3.disc_serial) == want)
		return -2;
	return -1;
}

void App::S3ReadCard()
{
	m_s3.card.Open(m_s3.memcard_path);
	if (!m_s3.memcard2_path.empty())
		m_s3.card2.Open(m_s3.memcard2_path);
	m_s3.card_read = m_time;
	m_s3.kit.DropCached("card:");
}

void App::S3PollDisc()
{
	std::string title, serial;
	const bool in = m_cfg.drive_disc_poll && m_cfg.drive_disc_poll(title, serial);
	if (in != m_s3.disc_in)
	{
		m_s3.disc_in = in;
		if (in)
		{
			m_s3.b_appear[2] = 0; // browser.md 6.2: the item grows in, 14 frames
			m_s3.disc_leave = 0;
			m_s3.checking = false;
			// The BIOS plays nothing when its row changes (browser.md 12); the closest of its effects: Display mode's "show" pair (0 + 1).
			if (m_s3.scr == s3::Scr::Browser)
				S3Sfx(0), S3Sfx(1);
		}
		else
		{
			// Taken out: it shrinks away (the appear backwards), the row closes up; the cursor leaves it for the Library.
			if (!m_s3.disc_always)
			{
				m_s3.disc_leave = 14;
				if (m_s3.b_cur == 2)
					m_s3.b_cur = 1;
			}
			if (m_s3.scr == s3::Scr::Browser)
				S3Sfx(2), S3Sfx(3); // Display mode's "hide" pair
		}
		std::printf("[shelf3] disc %s %s\n", in ? "in:" : "out", in ? title.c_str() : "");
	}
	m_s3.disc_title = title;
	m_s3.disc_serial = serial;
	if (in && !serial.empty())
		EnsureDiscArt(serial); // the disc's own art for the Browser's disc object (cached by the cover service, like covers)
}

// ---------------------------------------------------------------------------------------------------------------------------------------
void App::S3Update(double dt, const Input& in)
{
	if (!m_s3.kit_ready)
		S3Init();
	m_s3.acc += std::min(dt, 0.25);
	m_s3.sim_t += dt;
	if (m_s3.resync)
	{
		g_s3pad.now = g_s3pad.prev = S3Bits(in);
		g_s3_latch = 0;
		m_s3.resync = false;
	}
	else
		g_s3_latch |= S3Bits(in);
	// The folder picker, the QR page: their own input (fe_app.cpp), over this shelf.
	if (m_picker.open)
	{
		UpdatePicker(dt, in);
		return;
	}
	if (m_s3.no_bios)
	{
		// The "PS2 BIOS needed" screen: Cross looks again (a BIOS found: the normal start), Triangle opens the BIOS folder picker.
		const bool cross = in.cross && !m_prev.cross, tri = in.triangle && !m_prev.triangle;
		m_prev = in;
		m_s3.acc = 0;
		if (cross)
		{
			m_s3.bios_checked = m_time;
			for (const std::string& dr : m_s3.bios_dirs)
				if (m_s3.bios.LoadDir(dr))
				{
					std::printf("[shelf3] BIOS found in %s: starting\n", dr.c_str());
					if (m_cfg.bios_present)
						m_cfg.bios_present();
					m_bios_missing = false;
					m_s3.no_bios = false;
					m_s3.kit_ready = false; // S3Init again: the opening, the menu
					m_s3.resync = true;     // the Cross still held is not a press: the opening would take it as "skip"
					Sound(Sfx::Select, 0.0f);
					return;
				}
			Sound(Sfx::Error, 0.0f);
		}
		else if (tri)
		{
			OpenPicker(OptionsSheet::Kind::BiosFolder);
			m_s3.kit_ready = false; // (back from the picker: S3Init reads the folder setting again)
			Sound(Sfx::Move, 0.0f);
		}
		return;
	}
	while (m_s3.acc >= 0.02)
	{
		m_s3.acc -= 0.02;
		m_s3.F++;
		S3Tick(in);
	}
	// Everything steps on the PAL 50 Hz tick from real elapsed time, whatever the display's rate (60, 120 Hz); frames between two
	// ticks draw the in-between (fr), so motion stays smooth instead of repeating every 5th frame at 60 Hz.
	m_s3.fr = static_cast<float>(std::clamp(m_s3.acc / 0.02, 0.0, 1.0));
	m_s3.Fr = static_cast<double>(m_s3.F) + m_s3.fr;
}

// The menu clock's local time in seconds since the epoch (with the zone's offset, so day/hour maths works on it directly).
static double S3RealClock()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	std::tm tm = {};
	localtime_r(&ts.tv_sec, &tm);
	return static_cast<double>(ts.tv_sec) + static_cast<double>(tm.tm_gmtoff) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void S3ClockParts(double t, float& Sec, float& M, float& Hh)
{
	const double day = t - 86400.0 * std::floor(t / 86400.0);
	const int h = static_cast<int>(day / 3600.0), m = static_cast<int>((day - h * 3600.0) / 60.0);
	Sec = static_cast<float>(day - h * 3600.0 - m * 60.0);
	M = m + Sec / 60.0f;
	Hh = (h % 12) + M / 60.0f;
}

// One tick of the clock (mainmenu.md 1.2) and of what follows it per frame: the orbs' smoothed hands and radius (4.1).
void S3ClockTick(s3::Shelf3& S)
{
	static const bool sim = std::getenv("S3_SIM_CLOCK") != nullptr; // the preview: the clock runs on ticks only (scripted time)
	const double rtc = S3RealClock();
	if (S.clk < 0 || (!sim && std::fabs(rtc - S.clk) > 3.0))
		S.clk = rtc;
	else
	{
		S.clk += 0.02;
		if (!sim) // slew a little towards the real clock, never past it
		{
			const double e = rtc - S.clk;
			S.clk += std::clamp(e, -0.004, 0.004);
		}
	}
	float Sec, M, Hh;
	S3ClockParts(S.clk, Sec, M, Hh);
	auto wrap = [](float a) { return a - 2.0f * kPi_ * std::floor((a + kPi_) / (2.0f * kPi_)); };
	const float ah_t = Hh * 2.0f * kPi_ / 12.0f, as_t = Sec * 2.0f * kPi_ / 60.0f;
	if (S.orb_m < 0)
		S.orb_m = M / 60.0f, S.orb_ah = ah_t, S.orb_as = as_t;
	S.orb_ah += wrap(ah_t - S.orb_ah) * 0.1f;
	S.orb_as += wrap(as_t - S.orb_as) * 0.1f;
	S.orb_m += (M / 60.0f - S.orb_m) * 0.005f;
}

void App::S3Tick(const Input& in)
{
	S3Pad& p = g_s3pad;
	p.prev = p.now;
	p.now = S3Bits(in) | g_s3_latch;
	g_s3_latch = 0;
	p.rep = p.now & ~p.prev;
	const uint32_t dirs = p.now & (kBUp | kBDown | kBLeft | kBRight);
	if (dirs && dirs == g_s3_hold_dir)
	{
		g_s3_hold_frames++;
		if (g_s3_hold_frames >= 20 && (g_s3_hold_frames - 20) % 7 == 0)
			p.rep |= dirs;
	}
	else
		g_s3_hold_frames = 0, g_s3_hold_dir = dirs;
	if (m_qr_big)
	{
		if (p.fresh(kBCircle) || p.fresh(kBCross))
			m_qr_big = false, S3Sfx(10);
		return;
	}
	static const bool timing_log = std::getenv("S3_TIMING") != nullptr;
	if (timing_log && (static_cast<int>(m_s3.scr) != m_s3.log_scr || m_s3.menu_mode != m_s3.log_mode))
	{
		std::fprintf(stderr, "[s3t] t=%.3f F=%llu builds=%llu scr=%d mode=%d\n", m_s3.sim_t, static_cast<unsigned long long>(m_s3.F),
			static_cast<unsigned long long>(m_s3.builds), static_cast<int>(m_s3.scr), m_s3.menu_mode);
		m_s3.log_scr = static_cast<int>(m_s3.scr), m_s3.log_mode = m_s3.menu_mode;
	}
	S3ClockTick(m_s3);
	// The Browser disc art's spin: faster while focused, continuous across focus changes.
	m_s3.disc_spin_prev = m_s3.disc_spin;
	m_s3.disc_spin += (m_s3.scr == s3::Scr::Browser && m_s3.b_cur == 2) ? 0.012 : 0.004;
	if (m_s3.disc_spin > 6.283185307179586 * 1000)
		m_s3.disc_spin -= 6.283185307179586 * 1000, m_s3.disc_spin_prev -= 6.283185307179586 * 1000;
	// Per-tick parts of drawn things (they were stepped per drawn frame, so they ran faster at 60 and 120 Hz):
	{
		s3::Shelf3& S = m_s3;
		// the Browser row closing up after the disc left (6.1),
		const bool disc_shown = S.disc_in || S.disc_always || m_dump_on;
		for (int i = 0; i < 3; i++)
		{
			const int laid = disc_shown ? 3 : 2;
			const float target = i < laid ? (i - (laid - 1) * 0.5f) * 24.0f : S.b_x[i];
			if (disc_shown || i == 2)
				S.b_x[i] = target;
			else if (S.disc_leave == 0)
				S.b_x[i] += (target - S.b_x[i]) * 0.2f;
		}
		// the disc question's detail counter (0..25, from 0 each time it opens),
		S.ask_dd0 = m_ask_anim > 0.5f ? std::min(25, S.ask_dd0 + 1) : 0;
		// the question's disc: 0.048 a tick, and after Play faster and faster, as a drive spinning a disc up,
		S.ask_w = S.spin_t0 >= 0 ? std::min(0.9f, S.ask_w * 1.045f + 0.002f) : 0.048f;
		S.ask_rot = std::fmod(S.ask_rot + S.ask_w, 6.2831853f * 100.0f);
		// the empty Library's wandering light (a new target every 300 frames).
		if (S.scr == s3::Scr::Library && m_games.empty())
		{
			S.wander_t++;
			if (S.wander_t % 300 == 1)
				S.wander_to[0] = static_cast<float>(static_cast<int>(S.F * 7919 % 81) - 40), S.wander_to[1] = static_cast<float>(static_cast<int>(S.F * 104729 % 41) - 20);
			for (int k = 0; k < 2; k++)
				S.wander[k] += (S.wander_to[k] - S.wander[k]) / 24.0f;
		}
	}
	if (m_s3.F % 25 == 0)
		S3PollDisc();
	if (m_s3.was_copying && !m_dump_on)
		S3Sfx(9); // the copy is done: the BIOS's "completed" effect (browser.md 12: success)
	m_s3.was_copying = m_dump_on;
	// The right stick spins the focused case (Library) or the game page's case right round; let go and it turns back to its front.
	// R3 turns it round to its back to read (kurolume, 2026-11-16), R3 again back to its front; let go of the stick and it settles on the
	// side R3 chose. The back cover comes with the turn (the cover service's).
	{
		s3::Shelf3& S = m_s3;
		static bool r3_prev = false;
		if (in.r3 && !r3_prev)
		{
			S.case_hold = !S.case_hold; // (now: show the back)
			m_flip_wanted = true;       // (the back may be downloaded)
			S3Sfx(6);
		}
		r3_prev = in.r3;
		const bool spinning = std::fabs(in.rx) > 0.2f;
		if (spinning)
		{
			S.case_spin += in.rx * 0.09f;
			m_flip_wanted = true; // (the back may be downloaded)
		}
		else
		{
			// to the nearest front (or back, after R3), eased
			const float side = S.case_hold ? kPi_ : 0.0f;
			const float target = std::round((S.case_spin - side) / (2 * kPi_)) * 2 * kPi_ + side;
			S.case_spin += (target - S.case_spin) * 0.15f;
			if (std::fabs(target - S.case_spin) < 0.002f)
				S.case_spin = S.case_hold ? kPi_ : 0;
		}
		static int sel_was = -1;
		// The case left behind turns back to its front, eased, instead of jumping (kurolume); the new one starts at its front.
		if (m_selected != sel_was)
		{
			if (std::fabs(S.case_spin) > 0.002f && sel_was >= 0)
				S.prev_spin = S.case_spin, S.prev_spin_pos = sel_was;
			S.case_spin = 0, S.case_hold = false, sel_was = m_selected;
		}
		if (S.prev_spin_pos >= 0)
		{
			const float target = std::round(S.prev_spin / (2 * kPi_)) * 2 * kPi_;
			S.prev_spin += (target - S.prev_spin) * 0.15f;
			if (std::fabs(target - S.prev_spin) < 0.002f || S.prev_spin_pos == m_selected)
				S.prev_spin = 0, S.prev_spin_pos = -1;
		}
	}
	// Fades between screens: out over fade frames, swap, then the new screen's own entry.
	if (m_s3.fade > 0)
	{
		m_s3.fade--;
		if (m_s3.fade == 0 && m_s3.fading_out)
		{
			m_s3.scr = m_s3.next;
			m_s3.fading_out = false;
			m_s3.fade = m_s3.next == s3::Scr::Menu ? 0 : 20;
			m_s3.fade_len = 20;
			if (m_s3.scr == s3::Scr::Menu)
			{
				m_s3.menu_mode = 2, m_s3.menu_c = 0, m_s3.menu_d = -100, m_s3.menu_words = 0, m_s3.menu_spot = 0;
				m_s3.orb_all_fly = false;
			}
			if (m_s3.scr == s3::Scr::Browser)
				m_s3.fade = 75, m_s3.fade_len = 75, m_s3.dcc = 21, m_s3.dcc_dir = 0, S3ReadSettings();
		}
		if (m_s3.fading_out)
			return;
	}
	switch (m_s3.scr)
	{
		case s3::Scr::Menu: S3MenuTick(in); break;
		case s3::Scr::Browser: S3BrowserTick(in); break;
		case s3::Scr::Card: S3CardTick(in); break;
		case s3::Scr::Library: S3LibraryTick(in); break;
		case s3::Scr::Game: S3GameTick(in); break;
		case s3::Scr::Settings: S3SettingsTick(in); break;
		case s3::Scr::Opening: S3OpeningTick(in); break;
	}
}

FrameDesc* g_s3_frame = nullptr; // where S3DrawCase puts its boxes (this frame's)

// The "PS2 BIOS needed" setup screen (no BIOS anywhere): our own look, no Sony graphics; the app's own font (Roboto) and button glyphs.
void App::S3NoBiosBuild(FrameDesc& f, float W, float H)
{
	s3::Kit& kit = m_s3.kit;
	const float g[4][3] = {{18, 20, 34}, {8, 9, 16}, {14, 16, 30}, {4, 4, 8}};
	kit.Gradient(0, 0, W, H, g, 1.0f);
	const float s = H / 1080.0f, x = W * 0.5f - 560 * s;
	// The "Why?" box above the buttons (kurolume, 2026-11-16): set apart from the instructions.
	const float wy = H - 360 * s;
	kit.FillPx(x - 40 * s, wy - 36 * s, x + 1160 * s, wy + 150 * s, 0.10f, 0.12f, 0.22f, 0.85f);
	kit.End();
	std::vector<UiVertex>& ui = f.ui;
	const uint32_t white = ps2::Rgba(0.94f, 0.95f, 1.0f), muted = ps2::Rgba(0.62f, 0.66f, 0.76f), accent = ps2::Rgba(0.38f, 0.66f, 1.0f),
	               dim = ps2::Rgba(0.42f, 0.45f, 0.55f);
	float y = 250 * s;
	m_fonts->AddText(ui, "PS2 BIOS needed", x, y, 72 * s, white, 0.25f);
	y += 70 * s;
	// Kept simple (kurolume, 2026-11-16): one instruction and the folder; the other folders still searched are not listed.
	m_fonts->AddText(ui, "Copy your own PS2 BIOS into this folder:", x, y, 32 * s, muted);
	y += 72 * s;
	if (!m_s3.bios_dirs.empty())
		m_fonts->AddText(ui, m_s3.bios_dirs[0].c_str(), x + 40 * s, y, 40 * s, accent, 0.2f);
	if (m_s3.bios_checked >= 0 && m_time - m_s3.bios_checked < 3.0)
	{
		y += 60 * s;
		m_fonts->AddText(ui, "No BIOS found yet.", x, y, 30 * s, ps2::Rgba(1.0f, 0.62f, 0.45f));
	}
	m_fonts->AddText(ui, "Why?", x, wy, 30 * s, muted);
	m_fonts->AddText(ui, "Sony's artwork, fonts and sounds are copyrighted. We've rebuilt the menus to work on PS5,", x, wy + 44 * s, 28 * s, dim);
	m_fonts->AddText(ui, "but we can't share Sony's assets, so they come from your own BIOS.", x, wy + 82 * s, 28 * s, dim);
	m_fonts->AddText(ui, "Not sure how to get your own PS2 BIOS? Search online.", x, wy + 120 * s, 28 * s, dim);
	// The actions, bottom: the PS button glyphs of the app's icon font.
	const float by = H - 150 * s;
	float bx = x;
	bx += m_fonts->AddText(ui, icon::Cross, bx, by, 40 * s, ps2::Rgba(0.55f, 0.65f, 1.0f)) + 14 * s;
	bx += m_fonts->AddText(ui, "Check again", bx, by, 34 * s, white) + 70 * s;
	bx += m_fonts->AddText(ui, icon::Triangle, bx, by, 40 * s, ps2::Rgba(0.4f, 0.85f, 0.6f)) + 14 * s;
	m_fonts->AddText(ui, "Choose the BIOS folder", bx, by, 34 * s, white);
}

void App::S3Build(FrameDesc& f, float W, float H, const std::string& clock)
{
	g_s3_frame = &f;
	if (!m_s3.kit_ready)
		S3Init();
	s3::Kit& kit = m_s3.kit;
	m_s3.builds++;
	SetupFog(f, 0, W, H);
	f.bg.fog_hi[0] = f.bg.fog_hi[1] = f.bg.fog_hi[2] = 0.0f;
	f.bg.fog_lo[3] = 0.0f;
	f.fade = 1.0f;
	f.blur = 0.0f;
	f.ghost_end = 0, f.ghost_reset = false; // (only the opening feeds the afterimage; the frame object lives on, so every frame says so)
	kit.Begin(f, W, H, m_s3.wide); // shelf v3 round 2: the memory card pages too follow the screen shape (16:9 rule)
	if (m_s3.no_bios && !m_picker.open)
	{
		S3NoBiosBuild(f, W, H);
		kit.End();
		return;
	}
	switch (m_s3.scr)
	{
		case s3::Scr::Menu: S3MenuBuild(f); break;
		case s3::Scr::Browser: S3BrowserBuild(f); break;
		case s3::Scr::Card: S3CardBuild(f); break;
		case s3::Scr::Library: S3LibraryBuild(f, false); break;
		case s3::Scr::Game: S3GameBuild(f); break;
		case s3::Scr::Settings: S3SettingsBuild(f, clock); break;
		case s3::Scr::Opening: S3OpeningBuild(f); break;
	}
	kit.Sides();
	// Screen fades (to and from black).
	if (m_s3.fade > 0)
	{
		const float a = static_cast<float>(m_s3.fade) / static_cast<float>(m_s3.fade_len);
		kit.Black(m_s3.fading_out ? 1.0f - a : a);
	}
	// A game started off the game page: the whole page (the choice too) fades to black, full black before the shelf ends (fe_app).
	if (m_launching && m_s3.launch_fade_t0 >= 0)
	{
		const float t = std::clamp(static_cast<float>((m_time - m_s3.launch_fade_t0) / kLaunchFade), 0.0f, 1.0f);
		kit.Black(t * t * (3.0f - 2.0f * t));
	}
	kit.End();
	if (m_picker.open)
	{
		const float s = H / 1080.0f, k = H / 2160.0f;
		kit.FillPx(0, 0, W, H, 0.02f, 0.02f, 0.08f, 0.92f);
		kit.End();
		BuildPicker(f.ui, 160.0f * s, W - 320.0f * s, 120.0f * s, H - 240.0f * s, k * 1.2f, ps2::Cyan());
	}
}

// ---- the main menu (spec mainmenu.md) -----------------------------------------------------------------------------------------------

void App::S3MenuTick(const Input& in)
{
	(void)in;
	s3::Shelf3& S = m_s3;
	const S3Pad& p = g_s3pad;
	if (S.menu_wait > 0) // black between the opening and the menu's own entry
	{
		S.menu_wait--;
		return;
	}
	// Entry: c 0 -> 128 (one a frame); the spotlight from c = 62 (66 frames), the words from c = 120 (8 frames).
	if (S.menu_mode == 2 && S.menu_c == 0 && S.clocks_ok && !S.clocks.playing())
		S.clocks.Start(); // the ambience starts with the menu's entry (0x22e7f0)
	if (S.menu_mode == 2)
	{
		S.menu_c++;
		if (S.menu_c >= 128)
			S.menu_c = 128, S.menu_mode = 0;
	}
	else if (S.menu_mode == 3)
	{
		S.menu_c -= 2; // leaving: twice the BIOS's pace (its 128 frames, then the Browser's own 75)
		if (S.menu_c <= 0)
		{
			S.menu_c = 0;
			if (S.menu_leave_to == 0)
			{
				S.scr = s3::Scr::Browser;
				S.fade = 75, S.fade_len = 75, S.fading_out = false;
				S.dcc = 21, S.dcc_dir = 0;
				S.b_appear[0] = S.b_appear[1] = S.b_appear[2] = 0;
				S.flare_rgb[0] = 48, S.flare_rgb[1] = 98, S.flare_rgb[2] = 128, S.flare_rgb[3] = 60;
				S3ReadSettings();
			}
			return;
		}
	}
	if (S.menu_c >= 62 && S.menu_mode != 3)
		S.menu_spot = std::min(66, S.menu_spot + 1);
	else if (S.menu_mode == 3)
		S.menu_spot = std::max(0, S.menu_spot - 1);
	if (S.menu_c >= 120 && S.menu_mode != 3 && !S.version_open)
		S.menu_words = std::min(8, S.menu_words + 1);
	else
		S.menu_words = std::max(0, S.menu_words - 1);
	S.menu_d *= 0.97f;
	// Input: only with the words full and nothing open.
	if (S.version_open)
	{
		// Version Information (sysconf.md 10): the list rebuilt for 4 frames, then 8 frames in; Circle 8 frames out.
		if (S.ver_wait > 0)
			S.ver_wait--;
		else if (S.ver_closing)
		{
			if (--S.ver_t <= 0)
				S.ver_t = 0, S.version_open = false, S.ver_closing = 0;
			return;
		}
		else
			S.ver_t = std::min(8, S.ver_t + 1);
		if (S.ver_wait > 0)
			return;
		// Plain information here (kurolume): no cursor and no PlayStation Driver dialog (its settings do nothing in an emulator); Circle closes.
		if (p.fresh(kBCircle))
			S.ver_closing = 1, S3Sfx(10);
		return;
	}
	if (S.menu_words < 8 || S.menu_mode != 0)
		return;
	if (p.fresh(kBUp) && S.menu_cur > 0)
		S.menu_cur--, S3Sfx(6);
	else if (p.fresh(kBDown) && S.menu_cur < 1)
		S.menu_cur++, S3Sfx(6);
	else if (p.fresh(kBCross))
	{
		S3Sfx(4);
		if (S.menu_cur == 0)
		{
			S.menu_mode = 3;
			S.menu_leave_to = 0;
			if (S.clocks_ok)
				S.clocks.Stop(); // leaving for the Browser: the ambience stops (its notes ring out)
		}
		else
		{
			S.settings_from = s3::Scr::Menu;
			S3SettingsOpen(true);
		}
	}
	else if (p.fresh(kBTriangle))
		S.version_open = true, S.ver_t = 0, S.ver_wait = 4, S.ver_cur = 3, S.ver_sub = 0, S.ver_sub_t = 0, S.ver_closing = 0, S3Sfx(4);
}

// The main scene: the tunnel (5.1), the orbs (4), the spotlight (5.3). `wall` = System Configuration's +0..40 brightening; `spot` the
// vignette's 0..66 timer; `scene` false skips the tunnel (the entry fade has none).
namespace
{
struct S3Cam
{
	float r[9]; // rows of the view rotation (world -> camera)
	float eye[3];
};
S3Cam S3MenuCamera(float d)
{
	// Rc = Ry(0.145) Rx(0.031); eye = Rc (10.436, 0, -103 + d); look-at with dir Rc z, up Rc y.
	M4 rc = MMul(MRotY(0.145f), MRotX(0.031f));
	S3Cam c;
	float ex, ey, ez;
	MApply(rc, 10.436f, 0, -103.0f + d, ex, ey, ez);
	c.eye[0] = ex, c.eye[1] = ey, c.eye[2] = ez;
	// Camera axes are Rc's columns: x = Rc x, y = Rc y, z = Rc z; the view rotation is their transpose.
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			c.r[i * 3 + j] = rc.m[j * 4 + i];
	return c;
}
void S3ToCam(const S3Cam& c, float x, float y, float z, float& cx, float& cy, float& cz)
{
	const float dx = x - c.eye[0], dy = y - c.eye[1], dz = z - c.eye[2];
	cx = c.r[0] * dx + c.r[1] * dy + c.r[2] * dz;
	cy = c.r[3] * dx + c.r[4] * dy + c.r[5] * dz;
	cz = c.r[6] * dx + c.r[7] * dy + c.r[8] * dz;
}
} // namespace

void S3DrawMainScene(App* app, s3::Kit& kit, s3::Shelf3& S, int wall, int spot, bool tunnel, float orb_alpha, float hub_out[2]);

void App::S3MenuBuild(FrameDesc& f)
{
	s3::Shelf3& S = m_s3;
	s3::Kit& kit = S.kit;
	float hub[2];
	if (S.menu_wait > 0)
	{
		kit.FillPx(0, 0, kit.W(), kit.H(), 0, 0, 0, 1);
		return;
	}
	S3DrawMainScene(this, kit, S, 0, S.menu_spot, S.menu_mode == 0, 1.0f, hub);
	// The fade quad: black, 128 - c.
	kit.FillPx(0, 0, kit.W(), kit.H(), 0, 0, 0, (128 - S.menu_c) / 128.0f);
	kit.MarkScene();
	f.blur = 2.2f; // the five down/up passes that soften the whole picture (5.6)
	// The words (6.1): centred on 430, y 110 / 128.
	const int alpha = S.menu_words * 128 / 8 * S.menu_c / 128;
	if (alpha > 15)
	{
		const char* words[2] = {"Browser", "System Configuration"};
		for (int i = 0; i < 2; i++)
			kit.Text(words[i], 430, 110.0f + 18.0f * i, i == S.menu_cur ? s3::kCyan : s3::kDark, 1.0f, 1, alpha);
		kit.Hints({{s3::Button::Cross, "Enter"}, {s3::Button::Triangle, "Version"}}, alpha, true);
	}
	if (S.version_open)
	{
		// Version Information (sysconf.md 10.2), 1:1: title (string 90) 0.8 yellow centred on 442 at pen 89; rows 0.8 at pen 128 + 13 r,
		// labels right-aligned to 442 - 13, values from 442 + 13; alpha t/8 x 128. The 16:9 rule slides it out with its group (x 442 > 427).
		// The rows are the registered modules of the BIOS (10.3); the loader only accepts SCPH-70004 v2.00 (ROMVER 0200EC20040614),
		// whose list is this one.
		const float x = kit.Ox(442);
		const int ta = S.ver_t * 128 / 8;
		const int la = ta;
		static const char* kRows[5][2] = {{"Console", "SCPH-70004"}, {"Browser", "1.40"}, {"CD Player", "2.00"},
			{"PlayStation\x04 Driver", "1.11"}, {"DVD Player", "3.10E"}};
		if (la > 15)
		{
			kit.Text("Version Information", x, 89, s3::kYellow, 0.8f, 1, la);
			for (int r = 0; r < 5; r++) // (all in the rows' normal colour: no cursor here)
			{
				kit.Text(kRows[r][0], x - 13, 128.0f + 13 * r, s3::kDark, 0.8f, 2, la);
				kit.Text(kRows[r][1], x + 13, 128.0f + 13 * r, s3::kDark, 0.8f, 0, la);
			}
			// The credits, beneath the list in the same font, a size smaller (ours; the BIOS's list ends at DVD Player).
			static const char* kCredits[3][2] = {{"UI", "@kurolume"}, {"PS5 Port", "PS5SX2 (@Swordpdf)"}, {"Emulator", "PCSX2"}};
			// (One scale for the three, the widest fitted inside the screen: 4:3 has 640 field columns, 16:9 the widened 747.)
			float cs = 0.6f;
			for (int r = 0; r < 3; r++)
				cs = std::min(cs, ((S.wide ? 731.0f : 628.0f) - (x + 13)) / std::max(1.0f, static_cast<float>(kit.TextWidth(kCredits[r][1], 1.0f))));
			for (int r = 0; r < 3; r++)
			{
				kit.Text(kCredits[r][0], x - 13, 194.0f + 9.5f * r, s3::kDark, cs, 2, la);
				kit.Text(kCredits[r][1], x + 13, 194.0f + 9.5f * r, s3::kDark, cs, 0, la);
			}
		}
		kit.Hints({{s3::Button::Circle, "Back"}}, ta, true);
	}
}

void S3DrawMainScene(App* app, s3::Kit& kit, s3::Shelf3& S, int wall, int spot, bool tunnel, float orb_alpha, float hub_out[2])
{
	(void)app;
	const S3Cam cam = S3MenuCamera(S.menu_d);
	auto proj = [&](float wx, float wy, float wz, float& sx, float& sy, float& zc) {
		float cx, cy, cz;
		S3ToCam(cam, wx, wy, wz, cx, cy, cz);
		zc = cz;
		const float z = std::max(cz, 1.0f);
		sx = kit.X(320.0f + 512.0f * cx / z);
		sy = kit.Y(128.0f + 512.0f * 0.5405f * cy / z);
	};
	kit.FillPx(0, 0, kit.W(), kit.H(), 0, 0, 0, 1);
	const double F = S.Fr; // (between ticks: smooth at any display rate)
	// Tunnel: 16 segments x 33 rings, TEXCKABE, tinted (55,40,60)/128.
	const s3::KitTex& kabe = kit.T("TEXCKABE");
	if (tunnel && kabe.set)
	{
		kit.Bind(kabe.set);
		struct V
		{
			float x, y, z;
			uint32_t c;
			float u, v;
			bool ok;
		};
		static V ring[33][17];
		for (int j = 0; j <= 32; j++)
		{
			const float w = 1.0f + 0.05f * std::sin(static_cast<float>(std::fmod(100.0 * F + 0x1400 * j, 65536.0)) * 2.0f * kPi_ / 65536.0f);
			const float cj = std::floor(std::pow(32.0f - j, 3.0f) / 1024.0f);
			for (int a = 0; a <= 16; a++)
			{
				const float ang = a * 2.0f * kPi_ / 16.0f;
				const float h = std::floor((std::cos(ang) + 1.0f) * 10.0f);
				float sx, sy, zc;
				proj(6000.0f * std::sin(ang) * w, 6000.0f * std::cos(ang) * w, 1250.0f * j - 2500.0f, sx, sy, zc);
				const float R = std::floor(cj * 230.0f / 32.0f) + wall + h, G = std::floor(cj * 260.0f / 32.0f) + wall + h;
				// The tint copy's (55,40,60)/128 folded into the vertex colour (GS: tex x c / 128).
				V& v = ring[j][a];
				v.x = sx, v.y = sy, v.z = zc;
				v.c = s3::Kit::Pack(std::min(255.0f, R) * 55.0f / 128.0f / 255.0f, std::min(255.0f, G) * 40.0f / 128.0f / 255.0f,
					std::min(255.0f, G) * 60.0f / 128.0f / 255.0f, 0.5f);
				v.u = 3.0f * a / 16.0f;
				v.v = 3.0f * (j / 32.0f + static_cast<float>(std::fmod(F, 5000.0)) * 0.0002f);
				v.ok = zc > 2050.0f && std::fabs(sx - kit.W() * 0.5f) < 1000.0f * kit.SX() * 2 && std::fabs(sy - kit.H() * 0.5f) < 1000.0f * kit.SY();
			}
		}
		// The far cap (5.1): closes the tube at z 38750 in the wall level (b, b, b) (0 in the main menu: black).
		{
			float sx, sy, zc;
			proj(0, 0, 38750.0f, sx, sy, zc);
			const float b = static_cast<float>(wall);
			const uint32_t cc = s3::Kit::Pack(b * 55.0f / 128.0f / 255.0f, b * 40.0f / 128.0f / 255.0f, b * 60.0f / 128.0f / 255.0f, 0.5f);
			for (int a = 0; a < 16; a++)
			{
				const V &p0 = ring[32][a], &p1 = ring[32][a + 1];
				const float xy[6] = {sx, sy, p0.x, p0.y, p1.x, p1.y}, uv[6] = {1.5f, 3.0f, p0.u, p0.v, p1.u, p1.v};
				const uint32_t c3[3] = {cc, p0.c, p1.c};
				kit.Tri(xy, uv, c3, 10, false);
			}
		}
		for (int j = 32; j > 0; j--) // far first
			for (int a = 0; a < 16; a++)
			{
				const V &p0 = ring[j - 1][a], &p1 = ring[j - 1][a + 1], &p2 = ring[j][a], &p3 = ring[j][a + 1];
				if (!p0.ok || !p1.ok || !p2.ok || !p3.ok)
					continue;
				const float xy1[6] = {p0.x, p0.y, p1.x, p1.y, p3.x, p3.y}, uv1[6] = {p0.u, p0.v, p1.u, p1.v, p3.u, p3.v};
				const uint32_t c1[3] = {p0.c, p1.c, p3.c};
				kit.Tri(xy1, uv1, c1, 10, false);
				const float xy2[6] = {p0.x, p0.y, p3.x, p3.y, p2.x, p2.y}, uv2[6] = {p0.u, p0.v, p3.u, p3.v, p2.u, p2.v};
				const uint32_t c2[3] = {p0.c, p3.c, p2.c};
				kit.Tri(xy2, uv2, c2, 10, false);
			}
	}
	// The orbs (4.1 - 4.5).
	// The clock (mainmenu.md 2): seconds with their fraction from the real clock. (Before: the whole seconds plus the 50 Hz counter's
	// own fraction, out of step with them, so every second the orbs and rods jumped.)
	// (Now the shelf's own menu clock, stepped per tick as the BIOS's and drawn at this frame's place between ticks; the hands'
	// smoothing is per tick in S3ClockTick.)
	if (S.clk < 0)
		S3ClockTick(S);
	float Sec, M, Hh;
	S3ClockParts(S.clk + 0.02 * S.fr, Sec, M, Hh);
	const float r = 10.0f + 7.25f * S.orb_m;
	const int c = S.scr == s3::Scr::Menu ? S.menu_c : 128;
	const bool entering = S.scr == s3::Scr::Menu && S.menu_mode == 2;
	const bool leaving = S.scr == s3::Scr::Menu && S.menu_mode == 3;
	static const int kEntry[7][3] = {{0, 0, 128}, {0, 128, 0}, {0, 128, 128}, {128, 0, 0}, {128, 0, 68}, {128, 68, 0}, {128, 128, 128}};
	struct O
	{
		float sx, sy, z;
		int r, g, b;
	};
	O orbs[7];
	float hub_x = 0, hub_y = 0, hub_z = 0;
	proj(0, 0, 0, hub_x, hub_y, hub_z);
	hub_out[0] = hub_x, hub_out[1] = hub_y;
	for (int i = 0; i < 7; i++)
	{
		const float spin = std::fmod(M * 1100.0f / 60.0f, 1.0f) * 2.0f * kPi_;
		const float roll = std::fmod((21.0f + i) * Sec / 60.0f, 1.0f) * 2.0f * kPi_;
		M4 m = MMul(MRotZ(S.orb_ah), MRotY(S.orb_as));
		m = MMul(m, MRotZ(kPi_));
		m = MMul(m, MRotY(spin));
		m = MMul(m, MRotX(roll));
		m = MMul(m, MTrans(0, r, 0));
		float wx, wy, wz;
		MApply(m, 0, 0, 0, wx, wy, wz);
		O& o = orbs[i];
		proj(wx, wy, wz, o.sx, o.sy, o.z);
		// Colours (4.3): entry colours to the base (48,98,128) during the fade-in.
		const int* e = kEntry[i];
		if (entering || (leaving && i == 0))
			o.r = (e[0] * (128 - c) + 48 * c) >> 7, o.g = (e[1] * (128 - c) + 98 * c) >> 7, o.b = (e[2] * (128 - c) + 128 * c) >> 7;
		else
			o.r = 48, o.g = 98, o.b = 128;
		// Fly-in / fly-out (4.6).
		if ((entering && (S.orb_all_fly || i == 0)) || (leaving && i == 0))
		{
			const float ee = 1.0f - std::sin(c * kPi_ / 256.0f);
			const float dx = kit.W() * 0.5f * std::cos(S.orb_phi[i]) * ee, dy = kit.H() * 0.5f * std::sin(S.orb_phi[i]) * ee;
			if (leaving)
				o.sx = hub_x + (o.sx - hub_x) * c / 128.0f + 1.5f * dx, o.sy = hub_y + (o.sy - hub_y) * c / 128.0f + 1.5f * dy;
			else
				o.sx += dx, o.sy += dy;
		}
	}
	// Trails: one sample slot per orb rewritten every frame, the head advancing every 3rd frame (50 samples).
	// The head advances every 3rd 50 Hz frame (4.5), whatever the display's rate.
	const bool advance = S.F >= S.trail_F + 3;
	if (advance)
		S.trail_F = S.F;
	for (int i = 0; i < 7; i++)
	{
		s3::Shelf3::Trail& t = S.trails[i];
		if (advance)
		{
			t.head = (t.head + 1) % 50;
			t.count = std::min(50, t.count + 1);
		}
		t.x[t.head] = orbs[i].sx, t.y[t.head] = orbs[i].sy, t.z[t.head] = orbs[i].z;
		t.r[t.head] = static_cast<uint8_t>(orbs[i].r), t.g[t.head] = static_cast<uint8_t>(orbs[i].g), t.b[t.head] = static_cast<uint8_t>(orbs[i].b);
	}
	int order[7] = {0, 1, 2, 3, 4, 5, 6};
	std::sort(order, order + 7, [&](int a, int b) { return orbs[a].z > orbs[b].z; });
	const s3::KitTex& blur = kit.T("TEXCBLUR");
	const s3::KitTex& navi = kit.T("TEXCNAVI");
	const float lw = std::max(1.0f, kit.SY() * 0.9f); // the trails as the blur passes leave them: wider, dimmer
	for (int oi : order)
	{
		const O& o = orbs[oi];
		const s3::Shelf3::Trail& t = S.trails[oi];
		kit.Bind(VK_NULL_HANDLE);
		for (int k = 0; k + 1 < std::min(t.count, 49); k++)
		{
			const int i0 = (t.head - k + 50) % 50, i1 = (t.head - k - 1 + 50) % 50;
			const float a = std::max(0.0f, 128.0f - 3.0f * (k + 1)) / 128.0f;
			if (a <= 0)
				break;
			const float R = t.r[i1] * a * a * a * a, G = t.g[i1] * a * a, B = t.b[i1] * a;
			const uint32_t col = s3::Kit::Pack(R / 255.0f, G / 255.0f, B / 255.0f, 0.5f * 0.3f * orb_alpha);
			const float dx = t.x[i1] - t.x[i0], dy = t.y[i1] - t.y[i0], len = std::sqrt(dx * dx + dy * dy);
			if (len < 0.01f)
				continue;
			const float nx = -dy / len * lw, ny = dx / len * lw;
			const float xy1[6] = {t.x[i0] + nx, t.y[i0] + ny, t.x[i1] + nx, t.y[i1] + ny, t.x[i1] - nx, t.y[i1] - ny};
			const float xy2[6] = {t.x[i0] + nx, t.y[i0] + ny, t.x[i1] - nx, t.y[i1] - ny, t.x[i0] - nx, t.y[i0] - ny};
			const uint32_t cc[3] = {col, col, col};
			kit.Tri(xy1, nullptr, cc, 11, true);
			kit.Tri(xy2, nullptr, cc, 11, true);
		}
		const float s = 6.5e-6f * (16777471.0f / std::max(o.z, 1.0f) - 255.0f);
		if (blur.set)
			kit.Sprite(blur, o.sx - 30 * s * kit.SX(), o.sy - 15 * s * kit.SY(), o.sx + 30 * s * kit.SX(), o.sy + 15 * s * kit.SY(), 0, 0, 64, 64,
				{o.r, o.g, o.b}, static_cast<int>(60 * orb_alpha), true);
		// The core (4.5s, 2.25s): drawn as the five blur passes leave it, about twice as wide and half as bright.
		if (navi.set)
			kit.Sprite(navi, o.sx - 9.0f * s * kit.SX(), o.sy - 4.5f * s * kit.SY(), o.sx + 9.0f * s * kit.SX(), o.sy + 4.5f * s * kit.SY(), 0, 0,
				64, 64, {128, 128, 128}, static_cast<int>(70 * orb_alpha), true);
		if (!blur.set)
			kit.Flare(o.sx, o.sy, 30 * s, 90, static_cast<int>(100 * orb_alpha));
	}
	// The spotlight vignette (5.3): an ellipse at (266, H/2), rx 74, ry 42.55; outside darkened by A, a soft band between k 1 and 1.5.
	const int A = spot * 128 / 66;
	if (A > 0)
	{
		kit.Bind(VK_NULL_HANDLE);
		const float cx = kit.X(266), cy = kit.Y(128), rx = 74 * kit.SX(), ry = 42.55f * kit.SY();
		const float far = std::max(kit.W(), kit.H()) * 3.0f;
		for (int i = 0; i < 32; i++)
		{
			const float a0 = i * 2.0f * kPi_ / 32.0f, a1 = (i + 1) * 2.0f * kPi_ / 32.0f;
			auto pt = [&](float k, float a, float& x, float& y) {
				x = cx + k * rx * std::sin(a), y = cy + k * ry * std::cos(a);
			};
			float in0x, in0y, in1x, in1y, m0x, m0y, m1x, m1y, o0x, o0y, o1x, o1y;
			pt(1.0f, a0, in0x, in0y), pt(1.0f, a1, in1x, in1y), pt(1.5f, a0, m0x, m0y), pt(1.5f, a1, m1x, m1y);
			o0x = cx + far * std::sin(a0), o0y = cy + far * std::cos(a0), o1x = cx + far * std::sin(a1), o1y = cy + far * std::cos(a1);
			const uint32_t c0 = s3::Kit::Pack(0, 0, 0, 0), cA = s3::Kit::Pack(0, 0, 0, 0.5f * A / 128.0f);
			const float t1[6] = {in0x, in0y, in1x, in1y, m1x, m1y}, t2[6] = {in0x, in0y, m1x, m1y, m0x, m0y};
			const uint32_t k1[3] = {c0, c0, cA}, k2[3] = {c0, cA, cA};
			kit.Tri(t1, nullptr, k1, 11, false), kit.Tri(t2, nullptr, k2, 11, false);
			const float t3[6] = {m0x, m0y, m1x, m1y, o1x, o1y}, t4[6] = {m0x, m0y, o1x, o1y, o0x, o0y};
			const uint32_t k3[3] = {cA, cA, cA};
			kit.Tri(t3, nullptr, k3, 11, false), kit.Tri(t4, nullptr, k3, 11, false);
		}
	}
}

// ---- the Library's own object for the Browser (ours, in the BIOS icons' manner: flat-shaded 3D, lit by the card set) ----------------
// Built in a display frame (x right, y down, z away from the eye, as the grid's rest pose and page tilt show a card) and stored in the
// ico's model frame: model = (x, y, -z). Sizes in the card ico's units (a card is about 11 wide).
namespace
{
struct S3MeshBuilder
{
	s3::Ico ico;
	S3MeshBuilder()
	{
		ico.shapes = 1;
		ico.tex_type = 0;
		ico.frame_length = 1;
	}
	void Tri(const float a[3], const float b[3], const float c[3], const float n[3], const uint8_t col[3])
	{
		const float* p[3] = {a, b, c};
		// Keep the winding the ico renderer's back-face test expects: counter-clockwise on a y-down screen seen from outside.
		const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
		const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
		const bool flip = cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2] > 0;
		const int order[3] = {0, flip ? 2 : 1, flip ? 1 : 2};
		for (int k : order)
		{
			s3::Ico::Vertex v;
			v.pos = {p[k][0], p[k][1], -p[k][2]};
			v.nx = n[0], v.ny = n[1], v.nz = -n[2];
			v.r = col[0], v.g = col[1], v.b = col[2], v.a = 128;
			ico.verts.push_back(v);
		}
	}
	// An axis-aligned box (display frame) turned by `yaw` about the vertical, its six faces in `faces` colours (-x +x -y +y -z +z).
	bool upright = false; // build facing the eye (x right, y down the screen, z away): the page's frame has y at the eye, z down the screen
	void Box(float cx, float cy, float cz, float hx, float hy, float hz, float yaw, const uint8_t faces[6][3])
	{
		const float c = std::cos(yaw), s = std::sin(yaw);
		auto P = [&](float x, float y, float z, float* o) {
			const float X = cx + x * c + z * s, Y = cy + y, Z = cz - x * s + z * c;
			o[0] = X, o[1] = upright ? Z : Y, o[2] = upright ? Y : Z;
		};
		auto N = [&](float x, float y, float z, float* o) {
			const float X = x * c + z * s, Y = y, Z = -x * s + z * c;
			o[0] = X, o[1] = upright ? Z : Y, o[2] = upright ? Y : Z;
		};
		const float sgn[2] = {-1, 1};
		for (int axis = 0; axis < 3; axis++)
			for (int side = 0; side < 2; side++)
			{
				float n[3] = {0, 0, 0}, nn[3];
				n[axis] = sgn[side];
				N(n[0], n[1], n[2], nn);
				const int u = (axis + 1) % 3, v = (axis + 2) % 3;
				const float h[3] = {hx, hy, hz};
				float q[4][3];
				for (int k = 0; k < 4; k++)
				{
					float l[3];
					l[axis] = sgn[side] * h[axis];
					l[u] = (k == 1 || k == 2 ? 1 : -1) * h[u];
					l[v] = (k >= 2 ? 1 : -1) * h[v];
					P(l[0], l[1], l[2], q[k]);
				}
				Tri(q[0], q[1], q[2], nn, faces[axis * 2 + side]);
				Tri(q[0], q[2], q[3], nn, faces[axis * 2 + side]);
			}
	}
};
void S3Faces(uint8_t f[6][3], const uint8_t body[3], const uint8_t front[3])
{
	for (int i = 0; i < 6; i++)
		std::memcpy(f[i], body, 3);
	std::memcpy(f[4], front, 3); // -z: the side facing the eye
}
// A PS2 case standing: 13.5 x 19 x 1.4 cm as 8.6 x 12 x 0.9 units; the dark blue body, the front's light-blue face and its white band.
void S3CaseStanding(S3MeshBuilder& m, float cx, float cy, float cz, float yaw, const uint8_t face[3])
{
	const uint8_t body[3] = {22, 26, 60}, band[3] = {120, 120, 124};
	uint8_t f[6][3];
	S3Faces(f, body, face);
	m.Box(cx, cy + 0.6f, cz, 4.3f, 5.4f, 0.45f, yaw, f);
	uint8_t fb[6][3];
	S3Faces(fb, body, band);
	m.Box(cx, cy - 5.4f, cz, 4.3f, 0.6f, 0.45f, yaw, fb); // the "PlayStation 2" band along the top
}
// A case lying flat: its face up.
void S3CaseFlat(S3MeshBuilder& m, float cx, float cy, float cz, float yaw, const uint8_t face[3])
{
	const uint8_t body[3] = {22, 26, 60};
	uint8_t f[6][3];
	for (int i = 0; i < 6; i++)
		std::memcpy(f[i], body, 3);
	std::memcpy(f[2], face, 3); // -y: the top, facing up
	f[4][0] = 70, f[4][1] = 70, f[4][2] = 80; // the spine's edge towards the eye
	m.Box(cx, cy, cz, 4.3f, 0.45f, 6.0f, yaw, f);
}
// 0 a stack of three cases, 1 a case standing, 2 a small bookcase with four cases.
s3::Ico S3LibraryObject(int kind)
{
	S3MeshBuilder m;
	const uint8_t blue[3] = {60, 110, 200}, red[3] = {170, 60, 50}, green[3] = {60, 140, 80}, gold[3] = {170, 140, 50};
	m.upright = kind != 0;
	if (kind == 1)
		S3CaseStanding(m, 0, 0, 0, -0.35f, blue);
	else if (kind == 2)
	{
		const uint8_t wood[3] = {60, 62, 70};
		uint8_t f[6][3];
		for (auto& c : f)
			std::memcpy(c, wood, 3);
		m.Box(-5.4f, 0, 0, 0.4f, 6.2f, 2.6f, 0, f); // sides
		m.Box(5.4f, 0, 0, 0.4f, 6.2f, 2.6f, 0, f);
		m.Box(0, -6.0f, 0, 5.8f, 0.4f, 2.6f, 0, f); // top
		m.Box(0, 6.0f, 0, 5.8f, 0.4f, 2.6f, 0, f);  // bottom
		m.Box(0, 0, 2.4f, 5.4f, 5.6f, 0.2f, 0, f);  // back
		// four cases standing spine out (the spines face the eye)
		const uint8_t* cols[4] = {blue, red, green, gold};
		for (int i = 0; i < 4; i++)
		{
			uint8_t g[6][3];
			const uint8_t body[3] = {22, 26, 60};
			S3Faces(g, body, cols[i]);
			m.Box(-3.5f + i * 2.25f, 0.7f, -0.2f, 0.95f, 5.1f, 2.0f, 0, g);
		}
	}
	else
	{
		S3CaseFlat(m, 0, 4.2f, 0, 0.12f, green);
		S3CaseFlat(m, 0.3f, 3.2f, 0, -0.18f, red);
		S3CaseFlat(m, -0.2f, 2.2f, 0, 0.05f, blue);
	}
	return m.ico;
}

// Our own hard drives in the BIOS icon family (BrowserLook 15-17): the card object's size, dark plastic and metal, flat-shaded boxes.
// 0: a 3.5" drive (top cover, label, four screws, the connector edge); 1: a bay module (a dark slab, a ribbed vent top, an activity
// light; the light's glow is drawn by the caller).
s3::Ico S3HddObject(int kind)
{
	S3MeshBuilder m;
	m.upright = true;
	auto all = [](uint8_t f[6][3], const uint8_t c[3]) {
		for (int i = 0; i < 6; i++)
			std::memcpy(f[i], c, 3);
	};
	uint8_t f[6][3];
	if (kind == 0)
	{
		const uint8_t body[3] = {30, 30, 34}, cover[3] = {58, 60, 66}, label[3] = {96, 96, 100}, screw[3] = {120, 120, 126}, black[3] = {14, 14, 16},
					  gold[3] = {170, 140, 50};
		all(f, body), m.Box(0, 0, 0, 3.8f, 5.3f, 0.95f, 0, f);          // the frame
		all(f, cover), m.Box(0, -0.25f, -0.97f, 3.5f, 4.85f, 0.05f, 0, f); // the top cover
		all(f, label), m.Box(0, -2.0f, -1.03f, 2.6f, 1.8f, 0.03f, 0, f);   // the label
		for (int k = 0; k < 4; k++)
			all(f, screw), m.Box(k % 2 ? 3.1f : -3.1f, k / 2 ? 4.2f : -4.7f, -1.04f, 0.22f, 0.22f, 0.04f, 0, f);
		all(f, black), m.Box(0, 5.15f, 0, 3.3f, 0.25f, 0.7f, 0, f); // the connector edge
		for (int k = 0; k < 10; k++)
			all(f, gold), m.Box(-2.7f + k * 0.6f, 5.38f, 0, 0.12f, 0.05f, 0.35f, 0, f);
	}
	else
	{
		const uint8_t body[3] = {30, 30, 34}, rib[3] = {66, 66, 72}, trim[3] = {52, 52, 58}, light[3] = {70, 130, 255};
		all(f, body), m.Box(0, 0, 0, 4.2f, 5.2f, 1.3f, 0, f);              // the slab
		all(f, trim), m.Box(0, 4.6f, -1.32f, 4.0f, 0.45f, 0.05f, 0, f);     // a darker band at the foot
		for (int k = 0; k < 9; k++)                                          // the ribbed vent
			all(f, rib), m.Box(0, -4.3f + k * 0.85f, -1.36f, 3.5f, 0.18f, 0.08f, 0, f);
		all(f, light), m.Box(3.3f, 4.6f, -1.40f, 0.3f, 0.18f, 0.06f, 0, f); // the activity light
	}
	return m.ico;
}
} // namespace

// ---- the Browser (browser.md 2-6) ---------------------------------------------------------------------------------------------------

namespace
{
const float kSet0[4][3] = {{0xAC, 0xAC, 0xAC}, {0x52, 0x52, 0x48}, {0x7A, 0x7A, 0x7A}, {0x2A, 0x2A, 0x20}};
const float kSet2[4][3] = {{0x98, 0x98, 0x98}, {0x3E, 0x3E, 0x34}, {0x66, 0x66, 0x66}, {0x16, 0x16, 0x0C}};

// Projected field point of a page point (z 0) for the camera.
void S3PagePoint(const float cam[3], float x, float y, float z, float& fx, float& fy, float& zc)
{
	float X, Y, Z;
	MApply(S3PageView(cam), x, y, z, X, Y, Z);
	zc = Z;
	const float zz = std::max(Z, 1.0f);
	fx = 320.0f + 1024.0f * X / zz;
	fy = 128.0f + 553.47f * Y / zz;
}
} // namespace

void App::S3BrowserTick(const Input& in)
{
	(void)in;
	s3::Shelf3& S = m_s3;
	const S3Pad& p = g_s3pad;
	for (int& a : S.b_appear)
		a = std::min(14, a + 1);
	if (S.disc_leave > 0)
		S.disc_leave--;
	S.flare_phase += 0.03f;
	S.flare_h += (1088.0f + 288.0f * std::sin(S.flare_phase) - S.flare_h) / 44.0f;
	static const int kGrey[4] = {118, 118, 118, 128};
	for (int k = 0; k < 4; k++)
		S.flare_rgb[k] += S.flare_rgb[k] < kGrey[k] ? std::min(2, kGrey[k] - S.flare_rgb[k]) : -std::min(2, S.flare_rgb[k] - kGrey[k]);
	// The page change: the counter runs down (old page out), the swap, then up (the new one in).
	if (S.dcc_dir < 0)
	{
		S.dcc--;
		for (int k = 0; k < 3; k++)
		{
			const float tx = S.b_cur == 0 ? 0 : 0; // the item's place is folded in by the zoom below
			(void)tx;
		}
		if (S.dcc <= 0)
		{
			S.dcc = 0, S.dcc_dir = 0;
			S.scr = S.page_to;
			S.cam[0] = 0, S.cam[1] = 2.7f, S.cam[2] = 160;
			if (S.scr == s3::Scr::Library)
			{
				S.dcc = 0, S.dcc_dir = 1;
				S3ReadSettings();
				ApplyHidden(m_index.empty() ? -1 : m_index[static_cast<size_t>(m_selected)]);
				S.top = std::max(0, m_selected / S3Cols() - (S3Rows() - 1));
				S.scroll = static_cast<float>(S.top);
			}
			else if (S.scr == s3::Scr::Card)
			{
				S.dcc = 0, S.dcc_dir = 1;
				S.c_detail = 0, S.c_cur = 0, S.c_top = 0;
				S3ReadCard();
			}
		}
		return;
	}
	if (S.dcc_dir > 0)
	{
		S.dcc = std::min(21, S.dcc + 1);
		S.cam[2] += (160.0f - S.cam[2]) / 18.0f;
		if (S.dcc >= 21)
			S.dcc_dir = 0;
	}
	const bool disc_shown = S.disc_in || S.disc_always;
	const int n = disc_shown ? 3 : 2;
	if (!disc_shown && S.b_cur == 2)
		S.b_cur = 1;
	if (S.checking && m_time - S.checking_since > 4.0)
	{
		S.checking = false;
		if (!S.disc_in)
		{
			S.note = "No disc found.";
			S.note_time = m_time;
			S3Sfx(11);
		}
	}
	if (S.fade > 0)
		return;
	if (S.eject_ask)
	{
		if (!S.disc_in)
			S.eject_ask = false;
		else if (p.fresh(kBLeft) && S.yes_no != 0)
			S.yes_no = 0, S3Sfx(6);
		else if (p.fresh(kBRight) && S.yes_no != 1)
			S.yes_no = 1, S3Sfx(6);
		else if (p.fresh(kBCross))
		{
			S.eject_ask = false;
			if (S.yes_no == 0)
			{
				std::string why;
				const bool ok = m_cfg.drive_eject && m_cfg.drive_eject(why);
				S3Sfx(ok ? 4 : 11);
				S.note = ok ? "Ejecting the disc..." : "The disc could not be ejected (" + why + ").";
				S.note_time = m_time;
			}
			else
				S3Sfx(10);
		}
		else if (p.fresh(kBCircle))
			S.eject_ask = false, S3Sfx(10);
		return;
	}
	if (p.fresh(kBSquare) && S.b_cur == 2 && S.disc_in && !m_dump_on && m_cfg.drive_eject)
	{
		S.eject_ask = true, S.yes_no = 1, S3Sfx(4);
		return;
	}
	// No disc showing: Square looks at the drive again from anywhere on the Browser (kurolume, 2026-11-16), as Cross on the empty disc does.
	if (p.fresh(kBSquare) && !S.disc_in && !m_dump_on && !S.checking)
	{
		S3Sfx(4);
		S.checking = true;
		S.checking_since = m_time;
		if (m_cfg.drive_disc_reask)
			m_cfg.drive_disc_reask();
		return;
	}
	// A move sounds on a press only (no sound on the held repeat, none at an edge).
	if (p.repeat(kBLeft) && S.b_cur > 0)
	{
		S.b_cur--;
		if (p.fresh(kBLeft))
			S3Sfx(6);
	}
	else if (p.repeat(kBRight) && S.b_cur < n - 1)
	{
		S.b_cur++;
		if (p.fresh(kBRight))
			S3Sfx(6);
	}
	else if (p.fresh(kBCross))
	{
		if (S.b_cur == 2)
		{
			if (S.disc_in)
			{
				// The disc: the fork's disc question (play it from the disc, or copy it first), only now that it was picked.
				S3Sfx(7), S3Sfx(8);
				S.ask_allowed_until = m_time + 15.0;
				if (m_cfg.drive_disc_reask)
					m_cfg.drive_disc_reask();
			}
			else if (!S.checking)
			{
				S3Sfx(4);
				S.checking = true;
				S.checking_since = m_time;
				if (m_cfg.drive_disc_reask)
					m_cfg.drive_disc_reask();
			}
			return;
		}
		S3Sfx(4);
		S.page_to = S.b_cur == 0 ? s3::Scr::Card : s3::Scr::Library;
		S.dcc_dir = -1;
	}
	else if (p.fresh(kBCircle))
	{
		if (S.checking)
			S.checking = false, S3Sfx(10);
		else
		{
			S3Sfx(10);
			S3Go(s3::Scr::Menu, 75);
		}
	}
	else if (p.fresh(kBTriangle))
	{
		S3Sfx(4);
		S.settings_from = s3::Scr::Browser;
		S3SettingsOpen(true);
	}
}

// The games most recently played (Library positions), newest first; topped up from the Library's order.
std::vector<int> App::S3RecentGames(int n) const
{
	std::vector<std::pair<int64_t, int>> by;
	for (int i = 0; i < static_cast<int>(m_games.size()); i++)
	{
		auto it = m_s3.played.find(m_games[static_cast<size_t>(i)].serial);
		by.emplace_back(it == m_s3.played.end() ? 0 : it->second.last, i);
	}
	std::stable_sort(by.begin(), by.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	std::vector<int> out;
	for (size_t k = 0; k < by.size() && static_cast<int>(out.size()) < n; k++)
		if (m_slots[static_cast<size_t>(by[k].second)].set)
			out.push_back(by[k].second);
	return out;
}

// Library object options (round 3, LibraryIcon fan / cycle / open): PS2 cases with the player's own covers, drawn with the Library's
// case renderer at the place and size of the card-shaped object they replace (M: that object's view matrix, shape: its ico).
void App::S3DrawLibraryCases(const float* M, const s3::Ico* shape, float ry, float scale, float tip)
{
	s3::Shelf3& S = m_s3;
	s3::Kit& kit = S.kit;
	if (!shape || shape->verts.empty() || scale <= 0.01f)
		return;
	float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
	for (const s3::Ico::Vertex& v : shape->verts)
	{
		float X, Y, Z;
		MApply(*reinterpret_cast<const M4*>(M), v.pos[0], v.pos[1], v.pos[2], X, Y, Z);
		const float zz = std::max(Z, 1.0f);
		const float sx = kit.X(320 + 1024 * X / zz), sy = kit.Y(128 + 553.47f * Y / zz);
		x0 = std::min(x0, sx), x1 = std::max(x1, sx), y0 = std::min(y0, sy), y1 = std::max(y1, sy);
	}
	const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, h = (y1 - y0) * 1.15f;
	const std::vector<int> recent = S3RecentGames(S.lib_kind == 5 ? 6 : S.lib_kind == 7 ? 4 : 3);
	if (recent.empty())
	{
		// No covers yet (an empty Library): the fan below with four plain PS2-blue cases; the other looks: the blue case object.
		if (S.blook < 18)
		{
			kit.DrawIco(S.lib_ico[1], M, 320, 128, S3CardLights(0.24f), static_cast<float>(S.Fr), nullptr, std::min(1.0f, scale));
			return;
		}
	}
	if (S.blook >= 18)
	{
		// The default (round 3, mockups 03fan-1 / 03fan-2): four of the Library's games (picked at random per launch) fanned out like a hand of cards, all front-on, the
		// pivot near the bottom, the newest at the front on the right; the Library's case renderer at the Memory Card object's size,
		// a slow sway, no turning. 19: a tighter fan, turned a little so the spines show.
		const float W = static_cast<float>(m_renderer->width()), H = static_cast<float>(m_renderer->height());
		const float t = static_cast<float>(std::fmod(S.Fr * 0.02, 10000.0));
		const bool tight = S.blook == 19;
		const float sc = (y1 - y0) * 0.95f * Ps2UnitsPerPixel(H) / (2.0f * kBoxHalfH);
		const Vec3 pivot = Ps2World(cx, cy + (y1 - y0) * 0.36f, 0.0f, W, H); // (the fan's middle on the card's and the disc's row)
		const Vec3 centre = Ps2World(cx, cy, 0.0f, W, H); // (what the fan tips and turns about when the Browser leaves for the Library)
		const float step = (tight ? 7.0f : 12.5f) * kPi_ / 180.0f, sway = 0.02f * std::sin(t * 0.6f);
		kit.MarkUnder(); // (the cases between the field and the icons drawn after)
		// Four games picked at random from the Library once per launch (the same all session); fewer when the Library is smaller.
		if (S.fan_pick.empty() && !m_games.empty())
		{
			std::vector<std::string> all;
			for (const GameInfo& g : m_games)
				if (!g.serial.empty())
					all.push_back(g.serial);
			uint32_t seed = static_cast<uint32_t>(std::time(nullptr)) ^ 0x9E3779B9u;
			for (size_t i = all.size(); i > 1; i--) // Fisher-Yates with a small LCG
			{
				seed = seed * 1664525u + 1013904223u;
				std::swap(all[i - 1], all[(seed >> 8) % i]);
			}
			all.resize(std::min<size_t>(4, all.size()));
			S.fan_pick = all;
		}
		std::vector<int> picked;
		for (const std::string& ser : S.fan_pick)
			for (int i = 0; i < static_cast<int>(m_games.size()); i++)
				if (m_games[static_cast<size_t>(i)].serial == ser)
				{
					picked.push_back(i);
					break;
				}
		std::vector<int> recent_fan = picked.empty() ? recent : picked;
		// Always four cases: with fewer games, the covers repeat (the same fan whatever the Library's size).
		for (size_t k = 0; !recent_fan.empty() && recent_fan.size() < 4; k++)
			recent_fan.push_back(recent_fan[k % recent_fan.size()]);
		while (recent_fan.size() < 4)
			recent_fan.push_back(-1); // (an empty Library: plain blue cases)
		static s3::KitTex blue;
		if (!blue.set)
		{
			s3::Image im;
			im.w = im.h = 2;
			im.rgba.assign(16, 0);
			for (int q = 0; q < 4; q++)
				im.rgba[q * 4] = 34, im.rgba[q * 4 + 1] = 64, im.rgba[q * 4 + 2] = 150, im.rgba[q * 4 + 3] = 255;
			blue = kit.Upload(im);
		}
		const int nr = std::min(4, static_cast<int>(recent_fan.size()));
		for (int k = nr - 1; k >= 0; k--) // the oldest first (at the back, on the left), the newest last (at the front, on the right)
		{
			const int pos = recent_fan[static_cast<size_t>(k)];
			const bool plain = pos < 0 || pos >= static_cast<int>(m_slots.size()) || !m_slots[static_cast<size_t>(pos)].set;
			if (plain && !blue.set)
				continue;
			const int slot = nr - 1 - k; // 0 = leftmost
			const float ang = -(slot - (nr - 1) * 0.5f) * step + sway; // (positive turns the top to the left)
			BoxDraw b;
			b.model = Mat4::Translate(centre.x, centre.y, centre.z) * Mat4::RotateY(-ry) * Mat4::RotateX(-tip) * Mat4::Translate(pivot.x - centre.x, pivot.y - centre.y, pivot.z - centre.z) *
				Mat4::RotateZ(ang) * Mat4::Translate(0, kBoxHalfH * sc * 0.9f, slot * kBoxHalfD * sc * 2.2f) *
				Mat4::RotateX(tight ? -0.12f : 0.0f) * Mat4::RotateY(tight ? 0.38f : 0.0f) * Mat4::Scale(sc, sc, sc);
			b.set = plain ? blue.set : m_slots[static_cast<size_t>(pos)].set;
			b.cover_mix = plain ? 0.0f : m_slots[static_cast<size_t>(pos)].cover_mix;
			b.brightness = 0.92f;
			b.alpha = std::min(1.0f, scale);
			g_s3_frame->boxes.push_back(b);
		}
		return;
	}
	if (S.blook >= 15)
	{
		// The Library as a hard drive of our own in the BIOS icon family (mockups 03hd-1..3): the card's size, its light, a slow sway.
		const s3::Ico& hd = S.lib_hdd[S.blook == 15 ? 0 : 1];
		const float t = static_cast<float>(std::fmod(S.Fr * 0.02, 10000.0));
		const float a = std::min(1.0f, scale);
		float sx = 1, ex = 1;
		{
			float x0b = 1e9f, x1b = -1e9f;
			for (const s3::Ico::Vertex& v : shape->verts)
				x0b = std::min(x0b, v.pos[0]), x1b = std::max(x1b, v.pos[0]);
			for (const s3::Ico::Vertex& v : hd.verts)
				sx = std::min(sx, v.pos[0]), ex = std::max(ex, v.pos[0]);
			const float k = (x1b - x0b) / std::max(1e-3f, ex - sx) * 0.92f;
			sx = k;
		}
		const M4 model = MMul(*reinterpret_cast<const M4*>(M), MMul(MRotZ(0.0f), MMul(MRotY(0.35f * std::sin(t * 0.45f)), MScale(sx, sx, sx))));
		kit.DrawIco(hd, model.m, 320, 128, S3CardLights(0.24f), static_cast<float>(S.Fr), nullptr, a);
		if (S.blook >= 16)
		{
			// The activity light's glow (blue), blinking gently while focused (17); and the game count on its label (17).
			float X, Y, Z;
			MApply(model, 3.3f, -1.40f, -4.6f, X, Y, Z); // (the builder's box point (x, y, z) is the ico's (x, z, -y))
			const float zz = std::max(Z, 1.0f);
			const float lx = kit.X(320 + 1024 * X / zz), ly = kit.Y(128 + 553.47f * Y / zz);
			const bool focus = S.b_cur == 1;
			const float blink = S.blook == 17 && focus ? 0.55f + 0.45f * std::sin(t * 5.0f) : 0.8f;
			const s3::KitTex& blur = kit.T("TEXCBLUR");
			if (blur.set)
				kit.Sprite(blur, lx - h * 0.12f, ly - h * 0.12f, lx + h * 0.12f, ly + h * 0.12f, 0, 0, blur.w, blur.h, s3::Gs{40, 80, 128},
					static_cast<int>(120 * a * blink), true);
			if (S.blook == 17)
			{
				const float fx2 = (cx - kit.X(0)) / kit.SX(), fy2 = (cy - kit.Y(0)) / kit.SY();
				kit.Text(std::to_string(m_games.size()), fx2, fy2 + 3, s3::kGrey, 0.7f, 1, static_cast<int>(100 * a));
			}
		}
		return;
	}
	if (S.blook >= 7)
	{
		// Library object explorations (mockups 03bx-1..8, BrowserLook 7..14): not cases. All drawn flat in the UI at the place and size of
		// the card-shaped object, with the BIOS's flare, font and icons.
		const std::vector<int> rc = S3RecentGames(9);
		const float t = static_cast<float>(std::fmod(S.Fr * 0.02, 10000.0)); // seconds
		const float a = std::min(1.0f, scale);
		auto cover = [&](int pos, const float q[8], float bright) { // a cover on any 4-corner shape (TL, TR, BL, BR)
			if (pos < 0 || pos >= static_cast<int>(m_slots.size()) || !m_slots[static_cast<size_t>(pos)].set)
				return;
			kit.Bind(m_slots[static_cast<size_t>(pos)].set);
			const uint32_t c = s3::Kit::Pack(0.5f * bright, 0.5f * bright, 0.5f * bright, 0.5f * a);
			const float t1[6] = {q[0], q[1], q[2], q[3], q[4], q[5]}, t2[6] = {q[2], q[3], q[6], q[7], q[4], q[5]};
			const float u1[6] = {0, 0, 1, 0, 0, 1}, u2[6] = {1, 0, 1, 1, 0, 1};
			const uint32_t cc[3] = {c, c, c};
			kit.Tri(t1, u1, cc, 9, false), kit.Tri(t2, u2, cc, 9, false);
		};
		auto hue = [&](int pos, float out[3]) { // a per-game colour (from its serial: cover sampling is not here yet)
			unsigned hs = 2166136261u;
			for (char ch : m_games[static_cast<size_t>(pos)].serial)
				hs = (hs ^ static_cast<uint8_t>(ch)) * 16777619u;
			const float hh = (hs % 360) / 60.0f, x = 1 - std::fabs(std::fmod(hh, 2.0f) - 1);
			const int k = static_cast<int>(hh);
			const float rgb[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
			for (int c = 0; c < 3; c++)
				out[c] = 0.35f + 0.65f * rgb[k % 6][c];
		};
		const float cw = h * 0.36f, ch = cw * kBoxHalfH / kBoxHalfW; // a small cover's size
		switch (S.blook)
		{
			case 7: // (1) a turning ring of small covers, as the main menu's orb ring
			{
				const int nr = std::min(8, static_cast<int>(rc.size()));
				std::vector<std::pair<float, int>> order;
				for (int k = 0; k < nr; k++)
					order.push_back({std::cos(t * 0.5f + k * 6.2832f / nr), k});
				std::sort(order.begin(), order.end());
				for (const auto& [depth, k] : order)
				{
					const float ang = t * 0.5f + k * 6.2832f / nr;
					const float px = cx + std::sin(ang) * h * 0.55f, py = cy + std::cos(ang) * h * 0.12f, sc = 0.75f + 0.25f * depth;
					const float w = cw * sc * 1.15f, hh2 = ch * sc * 1.15f;
					const float q[8] = {px - w / 2, py - hh2 / 2, px + w / 2, py - hh2 / 2, px - w / 2, py + hh2 / 2, px + w / 2, py + hh2 / 2};
					cover(rc[static_cast<size_t>(k)], q, 0.55f + 0.45f * (depth + 1) / 2);
				}
				break;
			}
			case 8: // (2) a 2x2 collage on one tilted floating panel
			{
				const float pw = h * 0.95f, ph = h * 0.95f, sk = 0.22f * pw, lift = 0.08f * ph * std::sin(t * 0.8f) * 0.1f;
				auto P = [&](float u, float v, float& x, float& y) { x = cx - pw / 2 + u * pw + (1 - v) * sk * 0.5f - sk * 0.25f; y = cy - ph / 2 + v * ph * 0.82f + lift + u * ph * 0.08f; };
				float fx[4], fy[4];
				P(0, 0, fx[0], fy[0]), P(1, 0, fx[1], fy[1]), P(0, 1, fx[2], fy[2]), P(1, 1, fx[3], fy[3]);
				kit.Bind(VK_NULL_HANDLE);
				const uint32_t fc = s3::Kit::Pack(0.03f, 0.03f, 0.05f, 0.45f * a);
				const float t1[6] = {fx[0] - 6, fy[0] - 6, fx[1] + 6, fy[1] - 6, fx[2] - 6, fy[2] + 6}, t2[6] = {fx[1] + 6, fy[1] - 6, fx[3] + 6, fy[3] + 6, fx[2] - 6, fy[2] + 6};
				const uint32_t c3[3] = {fc, fc, fc};
				kit.Tri(t1, nullptr, c3, 11, false), kit.Tri(t2, nullptr, c3, 11, false);
				for (int k = 0; k < 4 && k < static_cast<int>(rc.size()); k++)
				{
					const float u0 = (k % 2) * 0.5f + 0.02f, v0 = (k / 2) * 0.5f + 0.02f, u1 = u0 + 0.46f, v1 = v0 + 0.46f;
					float q[8];
					P(u0, v0, q[0], q[1]), P(u1, v0, q[2], q[3]), P(u0, v1, q[4], q[5]), P(u1, v1, q[6], q[7]);
					cover(rc[static_cast<size_t>(k)], q, 1.0f);
				}
				break;
			}
			case 9: // (3) a small stack of PS2 discs, the newest on top with its cover as the label
				if (const s3::Ico* disc = S.bios.Icon("ICOBPS2D"))
				{
					const s3::KitTex* dt = &kit.Cached("bios:ICOBPS2D", disc->tex);
					for (int k = 3; k >= 0; k--)
					{
						M4 m = MMul(*reinterpret_cast<const M4*>(M), MMul(MTrans(k * 0.6f, k * 1.2f, 0), MRotY(0.5f * k + t * 0.1f)));
						kit.DrawIco(*disc, m.m, 320, 128, S3CardLights(0.30f), static_cast<float>(S.Fr), dt, a);
					}
				}
				break;
			case 10: // (4) a glass cube with the recent covers floating inside it
			{
				for (int k = 0; k < 3 && k < static_cast<int>(rc.size()); k++)
				{
					const float ox = (k - 1) * cw * 0.45f, oy = (1 - k) * ch * 0.12f + std::sin(t + k) * 2.0f;
					const float q[8] = {cx + ox - cw / 2, cy + oy - ch / 2, cx + ox + cw / 2, cy + oy - ch / 2, cx + ox - cw / 2, cy + oy + ch / 2, cx + ox + cw / 2, cy + oy + ch / 2};
					cover(rc[static_cast<size_t>(k)], q, 0.8f);
				}
				// the cube: three faces seen, pale glass with bright edges
				const float e = h * 0.5f, d = h * 0.22f;
				const float A[2] = {cx - e, cy - e + d}, B[2] = {cx + e - d, cy - e + d}, C[2] = {cx + e - d, cy + e}, Dd[2] = {cx - e, cy + e};
				const float A2[2] = {A[0] + d, A[1] - d}, B2[2] = {B[0] + d, B[1] - d}, C2[2] = {C[0] + d, C[1] - d};
				kit.Bind(VK_NULL_HANDLE);
				auto quad = [&](const float* p0, const float* p1, const float* p2, const float* p3, float r, float g, float b, float al) {
					const uint32_t c = s3::Kit::Pack(r, g, b, al * a);
					const float t1[6] = {p0[0], p0[1], p1[0], p1[1], p3[0], p3[1]}, t2[6] = {p1[0], p1[1], p2[0], p2[1], p3[0], p3[1]};
					const uint32_t c3[3] = {c, c, c};
					kit.Tri(t1, nullptr, c3, 11, false), kit.Tri(t2, nullptr, c3, 11, false);
				};
				quad(A, B, C, Dd, 0.36f, 0.4f, 0.5f, 0.18f);
				quad(A2, B2, B, A, 0.45f, 0.5f, 0.6f, 0.22f);
				quad(B, B2, C2, C, 0.3f, 0.34f, 0.44f, 0.22f);
				break;
			}
			case 11: // (5) a cluster of orbs, one per recent game, each in its own colour
			{
				const int nr = std::min(7, static_cast<int>(rc.size()));
				for (int k = 0; k < nr; k++)
				{
					float col[3];
					hue(rc[static_cast<size_t>(k)], col);
					const float ang = k * 2.4f + t * 0.3f, rr = h * 0.2f * std::sqrt(static_cast<float>(k) + 0.6f);
					const float px = cx + std::cos(ang) * rr, py = cy + std::sin(ang) * rr * 0.6f;
					const s3::KitTex& blur = kit.T("TEXCBLUR");
					if (blur.set)
						kit.Sprite(blur, px - h * 0.18f, py - h * 0.18f, px + h * 0.18f, py + h * 0.18f, 0, 0, blur.w, blur.h,
							s3::Gs{static_cast<uint8_t>(128 * col[0]), static_cast<uint8_t>(128 * col[1]), static_cast<uint8_t>(128 * col[2])}, static_cast<int>(110 * a), true);
					kit.FillPx(px - 2, py - 2, px + 2, py + 2, col[0], col[1], col[2], a);
				}
				break;
			}
			case 12: // (6) a crystal card: the card shape as tinted glass, the recent games' colours glowing through
				if (shape)
				{
					float col[3] = {0.6f, 0.8f, 1.6f};
					if (!rc.empty())
						hue(rc[0], col);
					for (int k = 0; k < 3 && k < static_cast<int>(rc.size()); k++)
					{
						float c2[3];
						hue(rc[static_cast<size_t>(k)], c2);
						kit.Flare(cx + (k - 1) * h * 0.18f, cy, h * 0.45f, static_cast<int>(30 + 30 * c2[k % 3]), static_cast<int>(40 * a));
					}
					kit.DrawIco(*shape, M, 320, 128, S3CardLights(0.24f), static_cast<float>(S.Fr), nullptr, 0.5f * a, 0.25f + 0.5f * col[0], 0.25f + 0.5f * col[1], 0.5f + 0.6f * col[2]);
				}
				break;
			case 13: // (7) no object: the word in the BIOS font with the flare behind it, the count under it
			{
				const float fx2 = (cx - kit.X(0)) / kit.SX(), fy2 = (cy - kit.Y(0)) / kit.SY();
				kit.Flare(cx, cy, h * 0.8f, 70, static_cast<int>(60 * a));
				kit.Text("Library", fx2, fy2 + 4, s3::kWhite, 1.0f, 1, static_cast<int>(128 * a));
				const int count = static_cast<int>(m_games.size());
				kit.Text(std::to_string(count) + " Games", fx2, fy2 + 16, s3::kGrey, 0.65f, 1, static_cast<int>(128 * a));
				break;
			}
			default: // (8) a flat icon in the BIOS's 2D style: a 3x3 grid of squares, the middle one lit
			{
				const float g = h * 0.22f, gap = h * 0.05f, x0g = cx - 1.5f * g - gap, y0g = cy - 1.5f * g - gap;
				for (int k = 0; k < 9; k++)
				{
					const float bx = x0g + (k % 3) * (g + gap), by = y0g + (k / 3) * (g + gap);
					kit.FillPx(bx + 3, by + 3, bx + g + 3, by + g + 3, 0, 0, 0, 0.35f * a);
					const bool lit = k == 4;
					kit.FillPx(bx, by, bx + g, by + g, lit ? 0.3f : 0.18f, lit ? 0.55f : 0.34f, lit ? 0.85f : 0.62f, a);
					kit.FillPx(bx, by, bx + g, by + g * 0.18f, 0.6f, 0.7f, 0.9f, 0.35f * a);
				}
				break;
			}
		}
		return;
	}
	kit.MarkUnder(); // the cases go between the field and the icons drawn after
	const float wobble = 0.06f * std::sin(static_cast<float>(S.Fr) * 0.02f);
	if (S.lib_kind == 7)
	{
		// The default (round 3): four cases lying flat in a neat pile, each turned a few degrees off the one below, the newest on top;
		// the Library's own case renderer (covers on top, spines on the sides), in a fixed pose, sized to the Memory Card object.
		const float W = static_cast<float>(m_renderer->width()), H = static_cast<float>(m_renderer->height());
		const float wpx = (x1 - x0) * 0.95f * 0.82f; // (round 3: 82 % of the card object's width, so the pile looks the card's size)
		const float sc = wpx * Ps2UnitsPerPixel(H) / (2.0f * kBoxHalfW);
		const Vec3 at = Ps2World(cx, cy + h * 0.04f, 0.0f, W, H);
		// A fixed pose (no turning): rolled like "\" (22 deg, top to the left), turned 25 deg and tipped 20 deg to the camera so the
		// spines show; only a slow sway of a degree or two.
		const float tsec = static_cast<float>(std::fmod(S.Fr * 0.02, 1000.0));
		const float spin = 0.44f + 0.03f * std::sin(tsec * 0.9f) + ry;
		const float roll = 0.38f + 0.02f * std::sin(tsec * 0.7f + 1.0f);
		auto push = [&](int pos, const Mat4& model) {
			if (pos < 0 || pos >= static_cast<int>(m_slots.size()) || !m_slots[static_cast<size_t>(pos)].set)
				return;
			BoxDraw b;
			b.model = model;
			b.set = m_slots[static_cast<size_t>(pos)].set;
			b.cover_mix = m_slots[static_cast<size_t>(pos)].cover_mix;
			b.brightness = 0.92f;
			b.alpha = std::min(1.0f, scale);
			g_s3_frame->boxes.push_back(b);
		};
		if (S.blook == 3) // (alternative 3) one upright case leaning like "", the newest cover the right way up, its spine showing
		{
			const float s1 = sc * 0.95f;
			push(recent[0], Mat4::Translate(at.x, at.y, at.z) * Mat4::RotateZ(0.36f) * Mat4::RotateX(0.06f) * Mat4::RotateY(0.55f + 0.03f * std::sin(tsec * 0.9f)) *
				Mat4::Scale(s1, s1, s1));
			return;
		}
		static const float kBookYaw = std::getenv("S3_BOOKYAW") ? static_cast<float>(std::atof(std::getenv("S3_BOOKYAW"))) : 1.35f;
		if (S.blook == 4) // (alternative 4) three or four cases standing like books on a shelf, spines to the camera, leaning a little
		{
			const int nb = std::min(4, static_cast<int>(recent.size()));
			const float s1 = sc * 1.05f;
			for (int k = nb - 1; k >= 0; k--)
			{
				const float off = (k - (nb - 1) * 0.5f) * kBoxHalfD * 2.6f;
				push(recent[static_cast<size_t>(k)], Mat4::Translate(at.x, at.y, at.z) * Mat4::RotateZ(0.12f) * Mat4::RotateY(kBookYaw + 0.03f * std::sin(tsec * 0.9f)) *
					Mat4::Scale(s1, s1, s1) * Mat4::Translate(0, 0, off) * Mat4::RotateZ(k == 0 ? -0.18f : 0.0f));
			}
			return;
		}
		const bool tall = S.blook == 6; // (alternative 6) the pile stood on its short edge, leaning hard
		const float lroll = tall ? 0.75f : roll, ltip = tall ? 1.25f : 0.35f;
		const int nr = std::min(4, static_cast<int>(recent.size()));
		static const float kTwist[4] = {0.10f, -0.06f, 0.07f, -0.03f}, kDx[4] = {0.05f, -0.04f, 0.03f, 0.0f};
		for (int k = 0; k < nr; k++) // bottom first: the oldest of the four at the bottom, the newest on top
		{
			const int pos = recent[static_cast<size_t>(nr - 1 - k)];
			if (pos < 0 || pos >= static_cast<int>(m_slots.size()) || !m_slots[static_cast<size_t>(pos)].set)
				continue;
			const float lift = (k - (nr - 1) * 0.5f) * kBoxHalfD * 2.1f;
			BoxDraw b;
			b.model = Mat4::Translate(at.x, at.y, at.z) * Mat4::RotateZ(lroll) * Mat4::RotateX(ltip) * Mat4::RotateY(spin) *
				Mat4::Scale(sc, sc, sc) * Mat4::Translate(kDx[k], lift, 0.0f) * Mat4::RotateY(kTwist[k]) * Mat4::RotateX(-kPi_ / 2);
			b.set = m_slots[static_cast<size_t>(pos)].set;
			b.cover_mix = m_slots[static_cast<size_t>(pos)].cover_mix;
			b.brightness = 0.92f;
			b.alpha = std::min(1.0f, scale);
			g_s3_frame->boxes.push_back(b);
		}
		return;
	}
	if (S.lib_kind == 4)
	{
		// (1) A small fan of three cases: the newest in front, the two before it leaning out behind.
		const int nr = static_cast<int>(recent.size());
		const float dx[3] = {-0.42f, 0.42f, 0.0f}, yaw[3] = {0.5f, -0.5f, 0.0f}, dy[3] = {-0.03f, -0.03f, 0.03f};
		const int who[3] = {nr > 1 ? recent[1] : -1, nr > 2 ? recent[2] : -1, recent[0]};
		for (int k = 0; k < 3; k++)
			if (who[k] >= 0)
				S3DrawCase(who[k], cx + dx[k] * h, cy + dy[k] * h, h * 0.86f, yaw[k] + ry + wobble, 0.04f, 1.0f, false);
	}
	else if (S.lib_kind == 5)
	{
		// (2) One case turning slowly; the cover changes while it is edge-on, through the recent games.
		const float yaw = static_cast<float>(std::fmod(S.Fr * 0.012, 2.0 * kPi_ * 1000.0)) + ry;
		const int turn = static_cast<int>(std::floor((yaw + kPi_ / 2) / kPi_));
		const int pos = recent[static_cast<size_t>(turn % static_cast<int>(recent.size()))];
		S3DrawCase(pos, cx, cy, h, yaw, 0.04f, 1.0f, false);
	}
	else
	{
		// (3) The newest game's case, the disc half out of its right side (the BIOS's PS2 disc ico, behind the case).
		if (const s3::Ico* disc = S.bios.Icon("ICOBPS2D"))
		{
			kit.MarkUnder();
			const float fx = (cx + h * 0.36f - kit.X(320)) / kit.SX(), fy = (cy - kit.Y(128)) / kit.SY();
			const float z = 160.0f;
			const M4 turn = MMul(MRotY(0.25f + ry), MRotX(kPi_ / 2));
			float d0 = 1e9f, d1 = -1e9f; // the disc's own height on screen at scale 1, to size it to 3/4 of the case
			for (const s3::Ico::Vertex& v : disc->verts)
			{
				float X, Y, Z;
				MApply(turn, v.pos[0], v.pos[1], v.pos[2], X, Y, Z);
				d0 = std::min(d0, kit.SY() * 553.47f * Y / z), d1 = std::max(d1, kit.SY() * 553.47f * Y / z);
			}
			const float k = d1 > d0 ? 0.75f * h / (d1 - d0) : 1.0f;
			const M4 dm = MMul(MTrans(fx * z / 1024.0f, fy * z / 553.47f, z), MMul(turn, MScale(k, k, k)));
			kit.DrawIco(*disc, dm.m, 320, 128, S3CardLights(0.30f), static_cast<float>(S.Fr), &kit.Cached("bios:ICOBPS2D", disc->tex));
			kit.MarkUnder();
		}
		S3DrawCase(recent[0], cx - h * 0.12f, cy, h, -0.32f + ry + wobble, 0.04f, 1.0f, false);
	}
}

// The disc ico's disc face on screen (the ellipse its vertices span under M): its centre and its two half-axes, for the game's disc art.
static void S3DiscFace(s3::Kit& kit, const s3::Ico& disc, const M4& M, float& mx, float& my, Vec2& ax, Vec2& ay)
{
	mx = 0, my = 0;
	std::vector<std::pair<float, float>> pts;
	for (const s3::Ico::Vertex& v : disc.verts)
	{
		float X, Y, Z;
		MApply(M, v.pos[0], v.pos[1], v.pos[2], X, Y, Z);
		const float zz = std::max(Z, 1.0f);
		pts.emplace_back(kit.X(320 + 1024 * X / zz), kit.Y(128 + 553.47f * Y / zz));
		mx += pts.back().first, my += pts.back().second;
	}
	mx /= pts.size(), my /= pts.size();
	float sxx = 0, syy = 0, sxy = 0;
	for (auto& q : pts)
		sxx += (q.first - mx) * (q.first - mx), syy += (q.second - my) * (q.second - my), sxy += (q.first - mx) * (q.second - my);
	const float ang = 0.5f * std::atan2(2 * sxy, sxx - syy);
	const float ux = std::cos(ang), uy = std::sin(ang);
	float ra = 0, rb = 0;
	for (auto& q : pts)
	{
		const float dx = q.first - mx, dy = q.second - my;
		ra = std::max(ra, std::fabs(dx * ux + dy * uy));
		rb = std::max(rb, std::fabs(-dx * uy + dy * ux));
	}
	ax = {ux * ra * 0.97f, uy * ra * 0.97f};
	ay = {-uy * rb * 0.97f, ux * rb * 0.97f};
}

void App::S3BrowserBuild(FrameDesc& f)
{
	(void)f;
	s3::Shelf3& S = m_s3;
	s3::Kit& kit = S.kit;
	kit.BrowserField(kSet0);
	const bool disc_shown = S.disc_in || S.disc_always || m_dump_on; // (a disc being copied stays, with its progress)
	const int n = disc_shown || S.disc_leave > 0 ? 3 : 2; // (a leaving disc is still drawn, shrinking)
	// (The row's x, re-laid when a disc comes and eased closed when it has gone, steps per tick in S3Tick.)
	// Leaving for a page: the camera eases at the item, the picked item turns face-on and grows, the others shrink.
	const float vis = S.dcc_dir < 0 ? std::max(0.0f, (S.dcc - 10) / 10.0f) : 1.0f;
	float cam[3] = {S.cam[0], S.cam[1], S.cam[2]};
	if (S.dcc_dir < 0)
	{
		const float k = 1.0f - std::pow(17.0f / 18.0f, static_cast<float>(21 - S.dcc));
		const float ix = S.b_x[S.b_cur];
		cam[0] = -1.1f * ix * k, cam[1] = 2.7f, cam[2] = 160.0f * (1.0f - k) + 51.0f * k;
	}
	const s3::Ico* card = S.bios.Icon("ICOBPS2M");
	const s3::Ico* disc = S.bios.Icon("ICOBPS2D");
	const s3::KitTex* card_tex = card ? &kit.Cached("bios:ICOBPS2M", card->tex) : nullptr;
	const s3::KitTex* disc_tex = disc ? &kit.Cached("bios:ICOBPS2D", disc->tex) : nullptr;
	const s3::Kit::Light L = S3CardLights(S.disc_in ? 0.30f : 0.24f);
	// Alternatives 2 (the BIOS's grid rule: 2 columns, the disc on a second row; camera 160 + 40 (rows - 1)) and 5 (the Library bigger and
	// centred, the card and the disc smaller either side).
	const int grid_rows = S.blook == 2 ? (n > 2 ? 2 : 1) : 1;
	if (S.blook == 2 && S.dcc_dir >= 0)
		cam[2] = 160.0f + 40.0f * (grid_rows - 1);
	for (int i = 0; i < n; i++)
	{
		float x = S.b_x[i], y = 0.0f, look_scale = 1.0f;
		if (S.blook == 2)
			x = -27.0f + 15.0f + (i % 2) * 24.0f, y = -(grid_rows * 24.0f + 4) / 2.0f + 14.0f + (i / 2) * 24.0f;
		else if (S.blook == 5)
			x = i == 1 ? 0.0f : i == 0 ? -34.0f : 34.0f, look_scale = i == 1 ? 1.35f : 0.75f;
		float scale = S.b_appear[i] / 14.0f * look_scale;
		if (i == 2 && S.disc_leave > 0)
			scale = S.disc_leave / 14.0f;
		float rx = kPi_ / 2, ry = 0;
		if (S.dcc_dir < 0)
		{
			if (i == S.b_cur)
			{
				const float t = (21 - S.dcc) / 21.0f;
				scale *= (31 - S.dcc) / 10.0f;
				rx = kPi_ / 2 + (-0.64f - kPi_ / 2) * t, ry = kPi_ / 2 * t;
			}
			else
				scale *= vis;
		}
		if (i == 2)
			ry += kPi_ / 8;
		if (scale <= 0.01f)
			continue;
		M4 model = MMul(MTrans(x, y, 0), MMul(MRotZ(0), MMul(MRotY(ry), MMul(MRotX(rx), MScale(scale, scale, scale)))));
		M4 M = MMul(S3PageView(cam), model);
		float fx, fy, zc;
		S3PagePoint(cam, x, y, 0, fx, fy, zc);
		if (i == 2 && !S.disc_in && !m_dump_on && S.disc_leave == 0)
		{
			// Always show, no disc: the PS2 disc ico grey and a third see-through (ours).
			if (disc)
				kit.DrawIco(*disc, M.m, 320, 128, L, static_cast<float>(S.Fr), disc_tex, 0.33f, 0.55f, 0.55f, 0.6f);
		}
		else if (i == 2 && disc)
		{
			kit.DrawIco(*disc, M.m, 320, 128, L, static_cast<float>(S.Fr), disc_tex);
			// The game's own disc art on it (the cover service's: LaunchBox's disc image, or covers/<serial>_disc.png; else the front
			// cover printed as the label around the hub and its hole), laid on the ico's disc face: the ellipse its vertices span.
			if (m_dump_disc_set && !disc->verts.empty())
			{
				float mx, my;
				Vec2 ax, ay;
				S3DiscFace(kit, *disc, M, mx, my, ax, ay);
				kit.Bind(VK_NULL_HANDLE);
				DiscArtQuad(f.ui, mx, my, ax, ay,
					static_cast<float>(std::fmod(S.disc_spin_prev + (S.disc_spin - S.disc_spin_prev) * S.fr, 6.283185307179586)), 1.0f);
			}
		}
		else if (card)
		{
			// The Library: the card ico in a blue coloured-card finish (ours), so it is not read as a second card.
			if (i == 1 && S.lib_kind >= 4)
			{
				// (The object's size and place from its resting pose, so the cases keep their shape; the leaving tip and turn are given
				// to the fan as one rigid body, as the Memory Card's own: rx from pi/2 toward -0.64, ry toward pi/2.)
				const M4 rest = MMul(S3PageView(cam), MMul(MTrans(x, y, 0), MMul(MRotX(kPi_ / 2), MScale(scale, scale, scale))));
				S3DrawLibraryCases(rest.m, card, ry, scale, rx - kPi_ / 2); // round 3 options: the player's own recent covers
			}
			else if (i == 1 && S.lib_kind < 3)
				kit.DrawIco(S.lib_ico[S.lib_kind], M.m, 320, 128, L, static_cast<float>(S.Fr), nullptr); // the Library's own object (ours)
			else if (i == 1)
				kit.DrawIco(*card, M.m, 320, 128, L, static_cast<float>(S.Fr), card_tex, 1.0f, 0.42f, 0.6f, 1.9f);
			else
				kit.DrawIco(*card, M.m, 320, 128, L, static_cast<float>(S.Fr), card_tex);
		}
		else
		{
			// No BIOS: a plain card shape.
			const float w = 9 * kit.SX() * 160 / zc, h = 11 * kit.SY() * 160 / zc;
			kit.FillPx(kit.X(fx) - w, kit.Y(fy) - h, kit.X(fx) + w, kit.Y(fy) + h, i == 1 ? 0.15f : 0.12f, i == 1 ? 0.2f : 0.12f, i == 1 ? 0.6f : 0.12f, scale);
		}
		if (i == 2 && m_dump_on)
		{
			// The copy's progress under the disc: a thin bar (the BIOS's blue on its dark track).
			const float bx0 = kit.X(fx - 26), bx1 = kit.X(fx + 26), by = kit.Y(fy + 22), bh = kit.SY() * 2.2f;
			kit.FillPx(bx0, by, bx1, by + bh, 0.05f, 0.05f, 0.06f, 0.8f);
			kit.FillPx(bx0, by, bx0 + (bx1 - bx0) * std::clamp(m_dump_pct, 0, 100) / 100.0f, by + bh, 47 / 128.0f, 87 / 128.0f, 127 / 128.0f, 1.0f);
		}
		if (i == S.b_cur && S.dcc_dir >= 0)
		{
			const float hpx = S.flare_h / 16.0f * (S.b_appear[i] / 14.0f) * std::clamp((S.dcc - 10) / 10.0f, 0.0f, 1.0f);
			kit.Flare(kit.X(fx), kit.Y(fy), hpx, (i == 2 && S.disc_in && m_dump_disc_set) || (i == 1 && S.lib_kind >= 4) ? S.flare_rgb[0] / 2 : S.flare_rgb[0], S.flare_rgb[3]); // (softer over the disc art)
		}
	}
	kit.Bands();
	kit.MarkScene();
	// Texts: "Browser" top-left, the focused object's name top-right in yellow (fading with the page counter).
	const int ta = S.dcc > 8 ? 127 * (S.dcc - 8) / 13 : 0;
	kit.Text("Browser", kit.Ox(24), 34, s3::kBrowserText, kit.FitScale("Browser", 0.77f, 292), 0, ta);
	std::string t1, t2;
	if (S.b_cur == 0)
		t1 = "Memory Card (PS2)/1";
	else if (S.b_cur == 1)
	{
		t1 = "Library";
		int count = static_cast<int>(m_games.size() + m_shelved.size());
		for (const Shelved& h : m_shelved) // (a multi-disc game's other discs are not games of their own)
			if (DiscNumber(h.game.file, nullptr) > 1)
			{
				auto it = S.disc_sets.find(h.game.path);
				if (it == S.disc_sets.end())
					it = S.disc_sets.emplace(h.game.path, DiscSet(h.game.path)).first;
				count -= it->second.size() > 1 && it->second.front() != h.game.path ? 1 : 0;
			}
		t2 = std::to_string(count) + (count == 1 ? " Game" : " Games");
	}
	else if (m_dump_on)
	{
		t1 = "Copying to Library " + std::to_string(m_dump_pct) + "%";
		t2 = S3Ascii(m_dump_title);
	}
	else if (S.disc_in)
		S3Split(kit, S3Ascii(S.disc_title.empty() ? "PlayStation2-DVD/CD-ROM" : S.disc_title), 0.8f, 300, t1, t2);
	else
		t1 = "No Disc";
	kit.Text(t1, kit.Ox(616), 34, s3::kYellow, 0.8f, 2, ta);
	if (!t2.empty())
		kit.Text(t2, kit.Ox(616), 48, s3::kYellow, 0.8f, 2, ta);
	if (S.checking)
		kit.Text("Reading disc...", 320, 179 * 1.15f, s3::kBrowserText, kit.FitScale("Reading disc...", 0.875f, 592), 1,
			static_cast<int>(127 * std::fabs(std::sin(m_time * 2.0))));
	else if (m_time - S.note_time < 3.0)
		kit.Text(S.note, 320, 179 * 1.15f, s3::kBrowserText, 0.875f, 1, 127);
	if (S.eject_ask)
	{
		kit.Text("Eject disc?", 320, 160 * 1.15f, s3::kBrowserText, 1.0f, 1, 128);
		const int w1 = kit.TextWidth("Yes", 1.0f), w2 = kit.TextWidth("    ", 1.0f), w3 = kit.TextWidth("No", 1.0f);
		const float x0 = 320 - (w1 + w2 + w3) / 2;
		kit.Text("Yes", x0, 179 * 1.15f, S.yes_no == 0 ? s3::kBlue : s3::kDark, 1.0f, 0, 128);
		kit.Text("No", x0 + w1 + w2, 179 * 1.15f, S.yes_no == 1 ? s3::kBlue : s3::kDark, 1.0f, 0, 128);
		kit.Hints({{s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}}, ta);
	}
	else if (S.checking)
		kit.Hints({{s3::Button::Circle, "Back"}}, ta);
	else if (S.b_cur == 2 && S.disc_in && !m_dump_on && m_cfg.drive_eject)
		kit.Hints({{s3::Button::Square, "Eject"}, {s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}, {s3::Button::Triangle, "Options"}}, ta);
	else if (S.b_cur == 2 && !S.disc_in)
		kit.Hints({{s3::Button::Square, "Reload Disc"}, {s3::Button::Cross, "Check for Disc"}, {s3::Button::Circle, "Back"}, {s3::Button::Triangle, "Options"}}, ta);
	else if (!S.disc_in && !m_dump_on)
		kit.Hints({{s3::Button::Square, "Reload Disc"}, {s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}, {s3::Button::Triangle, "Options"}}, ta);
	else
		kit.Hints({{s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}, {s3::Button::Triangle, "Options"}}, ta);
}

// The disc question (the fork's watcher asks: play from the disc, or copy it to the Library first) as the Browser's save menu: the page
// behind out of focus, the disc ico turning at the big icon's place, the disc's name and title, the two answers (App::UpdateDiscAsk).
void App::S3DiscAskBuild(FrameDesc& f)
{
	s3::Shelf3& S = m_s3;
	s3::Kit& kit = S.kit;
	const float dd = S.ask_dd0 / 25.0f;
	kit.End();
	kit.Begin(f, kit.W(), kit.H(), S.wide);
	f.blur = 1.3f * dd; // the save menu's two half-res passes
	float bg[4][3];
	for (int c = 0; c < 4; c++)
		bg[c][0] = bg[c][1] = 20, bg[c][2] = 60;
	kit.Gradient(0, kit.Y(32), kit.W(), kit.Y(224), bg, 115.0f / 128.0f * dd);
	if (const s3::Ico* disc = S.bios.Icon("ICOBPS2D"))
	{
		// browser.md 7: disc icons rot x 0.848 with rot y += 0.048 (PS2 disc), at the big icon's place.
		const float rot = S.ask_rot + S.ask_w * S.fr;
		M4 model = MMul(MTrans(-17.4f, 4.4f, 112), MMul(MRotX(0.848f), MMul(MRotY(rot), MScale(dd * 1.1f, dd * 1.1f, dd * 1.1f))));
		kit.DrawIco(*disc, model.m, 320, 128, S3CardLights(0.30f), static_cast<float>(S.Fr), &kit.Cached("bios:ICOBPS2D", disc->tex));
		// The game's own disc art on it, as on the Browser's disc (kurolume, 2026-11-16).
		if (m_dump_disc_set && !disc->verts.empty() && dd > 0.05f)
		{
			float mx, my;
			Vec2 ax, ay;
			S3DiscFace(kit, *disc, model, mx, my, ax, ay);
			kit.Bind(VK_NULL_HANDLE);
			DiscArtQuad(f.ui, mx, my, ax, ay, std::fmod(rot, 6.2831853f), dd);
		}
	}
	const int da = std::clamp(255 * (S.ask_dd0 - 12) / 25, 0, 128);
	kit.Text("PlayStation2-DVD/CD-ROM", 432, 41 * 1.15f, s3::kBrowserText, kit.FitScale("PlayStation2-DVD/CD-ROM", 0.77f, 300), 1, da);
	std::string a, b;
	S3Split(kit, S3Ascii(m_ask_title.empty() ? std::string("PS2 disc") : m_ask_title), 0.875f, 300, a, b);
	kit.Text(a, 432, (b.empty() ? 56 : 55) * 1.15f, s3::kYellow, kit.FitScale(a, b.empty() ? 1.0f : 0.875f, 300), 1, da);
	if (!b.empty())
		kit.Text(b, 432, 69 * 1.15f, s3::kYellow, kit.FitScale(b, 0.875f, 300), 1, da);
	kit.Text(S3Ascii(m_ask_serial), 432, (b.empty() ? 78 : 86) * 1.15f, s3::kBrowserText, 0.65f, 1, da);
	const char* opts[2] = {"Play", "Copy to Library"};
	for (int i = 0; i < 2; i++)
		kit.Text(opts[i], 428, (121 + 22 * i) * 1.15f, i == m_ask_sel ? s3::kCyan : s3::kDark, kit.FitScale(opts[i], 1.0f, 300), 1, da);
	kit.Bands();
	kit.Hints({{s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}}, S.spin_t0 >= 0 ? 0 : da);
	kit.Sides();
	if (S.spin_t0 >= 0) // spun up: the last half second fades to black, then the game
	{
		const double t = m_time - S.spin_t0;
		kit.Black(static_cast<float>(std::clamp((t - (kDiscSpinUp - 0.5)) / 0.5, 0.0, 1.0)));
	}
	kit.End();
}

// ---- Memory Card (PS2)/1: the card page, the save menu, delete (browser.md 6.5, 7, 8) --------------------------------------------------

void App::S3CardTick(const Input& in)
{
	(void)in;
	s3::Shelf3& S = m_s3;
	const S3Pad& p = g_s3pad;
	S.flare_phase += 0.03f;
	S.flare_h += (1088.0f * 0.72f + 288.0f * 0.72f * std::sin(S.flare_phase) - S.flare_h) / 44.0f;
	S.big_rot += 0.008f;
	const int n = static_cast<int>(S.card.saves().size());
	if (S.dcc_dir > 0)
	{
		S.dcc = std::min(21, S.dcc + 1);
		if (S.dcc >= 21)
			S.dcc_dir = 0;
	}
	if (S.dcc_dir < 0)
	{
		S.dcc--;
		if (S.dcc <= 0)
		{
			S.scr = s3::Scr::Browser;
			S.dcc = 0, S.dcc_dir = 1;
			S.cam[0] = 0, S.cam[1] = 2.7f, S.cam[2] = 0;
		}
		return;
	}
	// The wandering light of an empty card: a new target every 300 frames.
	if (++S.wander_t >= 300 || S.wander_t == 1)
	{
		S.wander_t = 1;
		S.wander_to[0] = static_cast<float>(static_cast<int>(S.F * 7919 % 81) - 40);
		S.wander_to[1] = static_cast<float>(static_cast<int>(S.F * 104729 % 41) - 20);
	}
	for (int k = 0; k < 2; k++)
		S.wander[k] += (S.wander_to[k] - S.wander[k]) / 24.0f;
	// The detail counter (0..25) follows the open save menu.
	if (S.c_detail >= 1)
		S.c_shown = S.c_detail;
	if (S.c_detail >= 1)
		S.dd0 = std::min(25, S.dd0 + 1);
	else
		S.dd0 = std::max(0, S.dd0 - 1);
	if (S.c_detail == 3)
	{
		if (++S.del_t >= 33)
		{
			const std::string dir = S.card.saves()[static_cast<size_t>(S.c_cur)].dir;
			const bool ok = S.card.Delete(dir);
			std::printf("[shelf3] delete %s from %s: %s\n", dir.c_str(), S.memcard_path.c_str(), ok ? "done" : "failed");
			S3Sfx(ok ? 9 : 11);
			S.msg = ok ? "Delete completed." : "Delete failed.";
			S.c_detail = 4;
			S3ReadCard();
		}
		return;
	}
	const int cols = 5;
	if (S.c_detail == 0)
	{
		if (n > 0)
		{
			int cur = S.c_cur;
			if (p.repeat(kBLeft) && cur > 0)
				cur--;
			else if (p.repeat(kBRight) && cur < n - 1)
				cur++;
			else if (p.repeat(kBUp) && cur >= cols)
				cur -= cols;
			else if (p.repeat(kBDown) && cur + cols < n)
				cur += cols;
			if (cur != S.c_cur)
			{
				S.c_cur = cur;
				if (p.fresh(kBLeft) || p.fresh(kBRight) || p.fresh(kBUp) || p.fresh(kBDown))
					S3Sfx(6);
				const int row = cur / cols;
				if (row < S.c_top)
					S.c_top = row;
				else if (row > S.c_top + 3)
					S.c_top = row - 3;
			}
			if (p.fresh(kBCross))
				S.c_detail = 1, S.c_opt = 0, S3Sfx(4), S.big_rot = 0;
			else if (p.fresh(kBTriangle))
				S.c_detail = 5, S3Sfx(4), S.big_rot = 0; // Properties
		}
		if (p.fresh(kBCircle))
			S3Sfx(10), S.dcc_dir = -1, S.dcc = 21;
		return;
	}
	if (S.c_detail == 1)
	{
		// Round 3: Play (the save's game, from the Library or the disc in the drive) and Delete. (Copy went: one card in this app.)
		if (p.fresh(kBUp) && S.c_opt > 0)
			S.c_opt--, S3Sfx(6);
		else if (p.fresh(kBDown) && S.c_opt < 1)
			S.c_opt++, S3Sfx(6);
		else if (p.fresh(kBCross))
		{
			const s3::Save& sv = S.card.saves()[static_cast<size_t>(S.c_cur)];
			if (S.c_opt == 0)
			{
				const int pos = S3SaveGame(sv);
				if (pos >= 0)
				{
					S3Sfx(4);
					Step(pos - m_selected);
					S3Launch(std::string()); // the game boots and loads its save itself
				}
				else if (pos == -2)
				{
					S3Sfx(7), S3Sfx(8); // the disc in the drive: its question (play from it / copy it)
					S.ask_allowed_until = m_time + 15.0;
					if (m_cfg.drive_disc_reask)
						m_cfg.drive_disc_reask();
				}
				else
					S3Sfx(11);
			}
			else
				S.c_detail = 2, S.yes_no = 1, S3Sfx(4);
		}
		else if (p.fresh(kBCircle))
			S.c_detail = 0, S3Sfx(10);
		return;
	}
	if (S.c_detail == 2)
	{
		if (p.fresh(kBLeft) && S.yes_no != 0)
			S.yes_no = 0, S3Sfx(6);
		else if (p.fresh(kBRight) && S.yes_no != 1)
			S.yes_no = 1, S3Sfx(6);
		else if (p.fresh(kBCross))
		{
			S3Sfx(4);
			if (S.yes_no == 0)
				S.c_detail = 3, S.del_t = 0;
			else
				S.c_detail = 1;
		}
		else if (p.fresh(kBCircle))
			S.c_detail = 1, S3Sfx(10);
		return;
	}
	if (S.c_detail == 6) // the destination (one: Memory Card (PS2)/2)
	{
		if (p.fresh(kBCross))
		{
			const s3::Save& sv = S.card.saves()[static_cast<size_t>(S.c_cur)];
			if ((sv.bytes + 1023) / 1024 + 2 > S.card2.free_kb())
			{
				S3Sfx(11);
				S.msg = "Insufficient free space available.";
				S.c_detail = 4;
			}
			else
				S.c_detail = 7, S.yes_no = 0, S3Sfx(4);
		}
		else if (p.fresh(kBCircle))
			S.c_detail = 1, S3Sfx(10);
		return;
	}
	if (S.c_detail == 7 || S.c_detail == 8) // Copy -> destination: Are you sure? (Yes first); overwrite: No first
	{
		if (p.fresh(kBLeft) && S.yes_no != 0)
			S.yes_no = 0, S3Sfx(6);
		else if (p.fresh(kBRight) && S.yes_no != 1)
			S.yes_no = 1, S3Sfx(6);
		else if (p.fresh(kBCross))
		{
			S3Sfx(4);
			const std::string dir = S.card.saves()[static_cast<size_t>(S.c_cur)].dir;
			if (S.yes_no == 1)
				S.c_detail = 1;
			else if (S.c_detail == 7 && S.card2.Has(dir))
				S.c_detail = 8, S.yes_no = 1;
			else
				S.c_detail = 9, S.copy_t = 0;
		}
		else if (p.fresh(kBCircle))
			S.c_detail = 1, S3Sfx(10);
		return;
	}
	if (S.c_detail == 9) // copying: the small icon slides down; the copy runs once it has (40 frames)
	{
		if (++S.copy_t >= 40)
		{
			const std::string dir = S.card.saves()[static_cast<size_t>(S.c_cur)].dir;
			std::vector<s3::MemoryCard::SaveFile> files;
			bool no_space = false, ok = S.card.ReadSaveFiles(dir, files);
			if (ok && S.card2.Has(dir))
				ok = S.card2.Delete(dir);
			ok = ok && S.card2.AddSave(dir, files, &no_space);
			std::printf("[shelf3] copy %s to %s: %s\n", dir.c_str(), S.memcard2_path.c_str(), ok ? "done" : "failed");
			S3Sfx(ok ? 9 : 11);
			S.msg = ok ? "Copy completed." : no_space ? "Insufficient free space available." : "Copy failed.";
			S.c_detail = 4;
		}
		return;
	}
	if (S.c_detail == 5) // properties
	{
		if (p.fresh(kBCircle) || p.fresh(kBTriangle))
			S.c_detail = 0, S3Sfx(10);
		return;
	}
	if (S.c_detail == 4 && (p.fresh(kBCircle) || p.fresh(kBCross)))
	{
		S3Sfx(10);
		S.c_detail = 0;
		S.c_cur = std::min(S.c_cur, std::max(0, static_cast<int>(S.card.saves().size()) - 1));
	}
}

void App::S3CardBuild(FrameDesc& f)
{
	s3::Shelf3& S = m_s3;
	s3::Kit& kit = S.kit;
	kit.BrowserField(kSet2);
	const auto& saves = S.card.saves();
	const int n = static_cast<int>(saves.size());
	const int ta = S.dcc > 8 ? 127 * (S.dcc - 8) / 13 : 0;
	const s3::Ico* card = S.bios.Icon("ICOBPS2M");
	auto save_ico = [&](int i, const s3::Ico*& ico, const s3::KitTex*& tex) {
		const s3::Save& sv = saves[static_cast<size_t>(i)];
		static std::map<std::string, s3::Ico> parsed;
		const std::string key = "card:" + sv.dir;
		auto it = parsed.find(key);
		if (it == parsed.end() || S.card_read > 0)
		{
			s3::Ico ic;
			if (!sv.ico.empty() && s3::ParseIco(sv.ico.data(), sv.ico.size(), ic))
				parsed[key] = std::move(ic);
			else
				parsed.erase(key);
			it = parsed.find(key);
		}
		if (it != parsed.end())
		{
			ico = &it->second;
			tex = &kit.Cached(key, it->second.tex);
			return;
		}
		// A built-in icon ("_SCEn", browser.md 10: the BIOS's own saves, n indexes its 13 icons), else the generic or the broken one.
		static const char* kBuiltin[13] = {"ICOBDISC", "ICOBCDDA", "ICOBPS1M", "ICOBPS2M", "ICOBPKST", "ICOBPS1D", "ICOBPS2D", "ICOBDVDD",
			"ICOBYSYS", "ICOBFSCE", "ICOBFNOR", "ICOBFBRK", "ICOBQUES"};
		const char* name = sv.builtin ? kBuiltin[std::clamp(sv.builtin_index, 0, 12)] : sv.corrupted ? "ICOBFBRK" : "ICOBFNOR";
		ico = S.bios.Icon(name);
		tex = ico ? &kit.Cached(std::string("bios:") + name, ico->tex) : nullptr;
	};
	// The page grid (6.1) with the page camera.
	const int cols = n <= 1 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 15 ? 4 : 5;
	const int rows = std::clamp((std::min(n, 20) + cols - 1) / std::max(1, cols), 1, 4);
	float cam[3] = {0, 2.7f, 160.0f + 40.0f * (rows - 1)};
	const float cx = (cols * 24 + 6) / 2.0f, cy = (rows * 24 + 4) / 2.0f;
	const float grow = S.dcc_dir > 0 ? std::min(1.0f, S.dcc / 14.0f) : S.dcc_dir < 0 ? std::max(0.0f, (S.dcc - 10) / 10.0f) : 1.0f;
	const s3::Kit::Light L = S3CardLights(0.24f);
	for (int i = S.c_top * cols; i < n && i < (S.c_top + 4) * cols; i++)
	{
		const int v = i - S.c_top * cols;
		const float x = -cx + 15 + (v % cols) * 24, y = -cy + 14 + (v / cols) * 24;
		const s3::Ico* ico = nullptr;
		const s3::KitTex* tex = nullptr;
		save_ico(i, ico, tex);
		float scale = grow;
		if (S.c_detail == 3 && i == S.c_cur)
			scale *= (33 - S.del_t) / 33.0f;
		if (!ico || scale <= 0.01f)
			continue;
		M4 model = MMul(MTrans(x, y, 0), MMul(MRotX(kPi_ / 2), MScale(scale, scale, scale)));
		kit.DrawIco(*ico, MMul(S3PageView(cam), model).m, 320, 128, L, static_cast<float>(S.Fr + i), tex);
		if (i == S.c_cur && S.c_detail == 0)
		{
			float fx, fy, zc;
			S3PagePoint(cam, x, y, 0, fx, fy, zc);
			kit.Flare(kit.X(fx), kit.Y(fy), S.flare_h / 16.0f * grow, 118, 128);
		}
	}
	// The scroll arrows while saves lie above/below the four rows shown (the Library's, browser.md 6.2); not over the save menu.
	{
		const bool page = n > 0 && S.dd0 == 0 && S.dcc_dir == 0;
		S3ScrollArrows(kit, S.card_arrows, page && S.c_top > 0, page && (S.c_top + 4) * cols < n, S.Fr);
	}
	if (n == 0 && S.dcc > 8)
	{
		float fx, fy, zc;
		const float c0[3] = {0, 0, 0};
		MApply(M4(), S.wander[0], S.wander[1], 200, fx, fy, zc);
		(void)c0;
		kit.Flare(kit.X(320 + 1024 * fx / 200), kit.Y(128 + 553.47f * fy / 200), 40, 60, 60);
	}
	// The save menu (7): the page under two half-res blurs (the post pass), the icon.sys backdrop, the big icon, the texts.
	if (S.dd0 > 0 && n > 0)
	{
		// (While it fades out after closing, it keeps the look of the view that was open: Properties stayed Properties, not the save menu.)
		const int cd = S.c_detail ? S.c_detail : S.c_shown;
		kit.MarkScene();
		f.blur = 1.3f * S.dd0 / 25.0f; // the save menu's two half-res passes (15.5)
		const s3::Save& sv = saves[static_cast<size_t>(S.c_cur)];
		float bg[4][3];
		float a;
		if (sv.sys.ok && !sv.builtin && sv.sys.bg_alpha > 0)
		{
			for (int c = 0; c < 4; c++)
				for (int k = 0; k < 3; k++)
					bg[c][k] = std::min(255.0f, sv.sys.bg[c][k]); // the backdrop quad takes its colours directly (no x2)
			a = sv.sys.bg_alpha / 128.0f;
		}
		else
		{
			for (int c = 0; c < 4; c++)
				bg[c][0] = bg[c][1] = 20, bg[c][2] = 60;
			a = 115.0f / 128.0f;
		}
		kit.Gradient(0, 0, kit.W(), kit.H(), bg, a * S.dd0 / 25.0f);
		const s3::Ico* ico = nullptr;
		const s3::KitTex* tex = nullptr;
		save_ico(S.c_cur, ico, tex);
		if (ico)
		{
			float scale = S.dd0 / 25.0f;
			float rot = S.big_rot;
			if (cd == 3)
				scale *= (33 - S.del_t) / 33.0f, rot += 0.192f * S.del_t;
			// Where it stands (measured on the PCSX2 captures, browser del_q / del_run / cp_dest): a PS2 save's icon stands on y 12.0
			// (box y 151-188 field; spec 7's "4.4" is another item kind's), the system icon (variant 8) on 8.4; Delete keeps it there
			// (the question and the shrinking run alike); only Copy moved it up-left (-25.4, -4.4, 164), and Copy is gone.
			const bool moved = cd >= 6 && cd <= 9;
			float px = moved ? -25.4f : -17.4f, py = moved ? -4.4f : (sv.builtin && sv.builtin_index == 8 ? 8.4f : 12.0f), pz = moved ? 164.0f : 112.0f;
			// Properties: the icon beside the title (spec 7: box x 189-231, y 59-94). An icon stands on its origin (as the save menu's
			// stands on y 12.0 = its box's bottom, 188), so the origin goes to the box's bottom, field y 94: (94 - 128) * 200 / 553.47.
			if (cd == 5)
				px = -21.5f, py = -12.3f, pz = 200.0f, scale *= 0.7f;
			M4 model = MMul(MTrans(px, py, pz), MMul(MRotY(rot), MScale(scale, scale, scale)));
			const float slide = cd == 5 ? 0.0f : kit.Ox(161) - 161; // the 16:9 rule: the big icon's group slides out
			// Built-in and broken icons light with the card set (browser.md 10), the others with their icon.sys lights.
			const s3::Kit::Light lt = sv.builtin || sv.corrupted ? S3CardLights(0.24f) : S3SaveLights(sv.sys, rot);
			kit.DrawIco(*ico, model.m, 320 + slide, 128, lt, static_cast<float>(S.Fr), tex);
			if (cd == 9) // the copy icon slides from the source down to the bottom-left (15.2)
			{
				const float k = std::min(1.0f, S.copy_t / 40.0f), cs = 0.5f + 0.5f * k;
				M4 cm = MMul(MTrans(-25.4f, -4.4f + 24.8f * k, 164.0f), MMul(MRotY(rot), MScale(cs, cs, cs)));
				kit.DrawIco(*ico, cm.m, 320 + kit.Ox(161) - 161, 128, S3SaveLights(sv.sys, rot), static_cast<float>(S.Fr * 3), tex);
			}
		}
		const int da = std::clamp(255 * (S.dd0 - 12) / 25, 0, 128);
		if (cd == 5)
		{
			// Properties (browser.md 7): the title beside its icon, labels right-aligned to 249, values from 269.
			const uint32_t kb5 = (sv.bytes + 1023) / 1024;
			char size5[32], date5[48];
			std::snprintf(size5, sizeof(size5), kb5 >= 1000 ? "Size %u,%03u KB" : "Size %u KB", kb5 >= 1000 ? kb5 / 1000 : kb5, kb5 % 1000);
			std::tm jt = {};
			jt.tm_year = sv.year - 1900, jt.tm_mon = sv.month - 1, jt.tm_mday = sv.day, jt.tm_hour = sv.hour, jt.tm_min = sv.minute, jt.tm_sec = sv.second;
			const std::time_t utc = timegm(&jt) - 9 * 3600;
			std::tm lt = {};
			localtime_r(&utc, &lt);
			std::snprintf(date5, sizeof(date5), "%02d/%02d/%04d  %d:%02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900, lt.tm_hour, lt.tm_min, lt.tm_sec);
			kit.Text(sv.corrupted ? std::string("Corrupted Data") : sv.sys.title1, 269, 66, s3::kYellow, 0.875f, 0, da);
			kit.Text(sv.sys.title2, 269, 81, s3::kYellow, 0.875f, 0, da);
			const std::string rows5[5][2] = {{"Location", "Memory Card (PS2)/1"},
				{"File Type", sv.corrupted ? "Corrupted Data" : sv.builtin ? "Settings File (PS2)" : "Saved Data (PlayStation2)"},
				{"File Size", size5}, {"Last Updated", date5}, {"File Protection", sv.builtin || sv.corrupted ? "Copying Prohibited" : "None"}};
			const float ys[5] = {104, 122, 138, 156, 172};
			for (int r = 0; r < 5; r++)
			{
				kit.Text(rows5[r][0], 249, ys[r], s3::kBrowserText, 0.77f, 2, da);
				kit.Text(rows5[r][1], 269, ys[r], s3::kBrowserText, kit.FitScale(rows5[r][1], 0.77f, 340), 0, da);
			}
		}
		const std::string devname = "Memory Card (PS2)/1";
		if (cd != 5)
		{
		kit.Text(devname, kit.Ox(432), 41 * 1.15f, s3::kBrowserText, kit.FitScale(devname, 0.77f, 300), 1, da);
		const bool two = !sv.sys.title2.empty() && !sv.corrupted; // a corrupted save: one line, "Corrupted Data" (browser.md 10)
		if (two)
		{
			kit.Text(sv.sys.title1, kit.Ox(432), 55 * 1.15f, s3::kYellow, 0.875f, 1, da);
			kit.Text(sv.sys.title2, kit.Ox(432), 69 * 1.15f, s3::kYellow, 0.875f, 1, da);
		}
		else
			kit.Text(sv.corrupted ? std::string("Corrupted Data") : sv.sys.title1, kit.Ox(432), 56 * 1.15f, s3::kYellow, 1.0f, 1, da);
		char date[64];
		{
			// The card keeps Japan time (UTC+9); shown in local time, as the BIOS does with the console's time zone.
			std::tm jt = {};
			jt.tm_year = sv.year - 1900, jt.tm_mon = sv.month - 1, jt.tm_mday = sv.day, jt.tm_hour = sv.hour, jt.tm_min = sv.minute, jt.tm_sec = sv.second;
			const std::time_t utc = timegm(&jt) - 9 * 3600;
			std::tm lt = {};
			localtime_r(&utc, &lt);
			std::snprintf(date, sizeof(date), "%02d/%02d/%04d   %d:%02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900, lt.tm_hour, lt.tm_min, lt.tm_sec);
		}
		const uint32_t kb = (sv.bytes + 1023) / 1024;
		char size[32];
		if (kb >= 1000)
			std::snprintf(size, sizeof(size), "%u,%03u KB", kb / 1000, kb % 1000);
		else
			std::snprintf(size, sizeof(size), "%u KB", kb);
		kit.Text(date, kit.Ox(432), (two ? 86 : 78) * 1.15f, s3::kBrowserText, 0.65f, 1, da);
		kit.Text(size, kit.Ox(432), (two ? 96 : 88) * 1.15f, s3::kBrowserText, 0.65f, 1, da);
		if (cd == 1)
		{
			// Play (greyed when the game is in neither the Library nor the drive) and Delete, at the BIOS's option places.
			const bool playable = S3SaveGame(sv) != -1;
			const char* opts[2] = {"Play", "Delete"};
			for (int i = 0; i < 2; i++)
			{
				const bool on = i == S.c_opt;
				// Greyed Play: a dim cyan when focused, darker than Delete when not.
				const s3::Gs c = i == 0 && !playable ? (on ? s3::Gs{15, 55, 78} : s3::Gs{24, 24, 24}) : on ? s3::kCyan : s3::kDark;
				kit.Text(opts[i], kit.Ox(428), (121 + 22 * i) * 1.15f, c, 1.0f, 1, da);
			}
			if (S.c_opt == 0 && !playable)
			{
				const std::string help = sv.builtin ? "This is the console's own data, not a game's." : "This game isn't in your Library.";
				kit.Text(help, kit.Ox(432), 179 * 1.15f, s3::kBrowserText, kit.FitScale(help, 0.65f, 400), 1, da);
			}
		}
		else if (cd == 2 || cd == 3)
		{
			const int add = two ? 6 : 0;
			kit.Text("Delete", kit.Ox(428), (103 + add) * 1.15f, s3::kYellow, 1.0f, 1, 128);
			kit.Text("Are you sure?", kit.Ox(432), (128 + add) * 1.15f, s3::kBrowserText, 1.0f, 1, 128);
			const int w1 = kit.TextWidth("Yes", 1.0f), w2 = kit.TextWidth("    ", 1.0f), w3 = kit.TextWidth("No", 1.0f);
			const float x0 = kit.Ox(432) - (w1 + w2 + w3) / 2;
			const int ya = cd == 3 ? 32 : 128;
			kit.Text("Yes", x0, (148 + add) * 1.15f, S.yes_no == 0 ? s3::kBlue : s3::kDark, 1.0f, 0, ya);
			kit.Text("No", x0 + w1 + w2, (148 + add) * 1.15f, S.yes_no == 1 ? s3::kBlue : s3::kDark, 1.0f, 0, ya);
			if (cd == 3)
				kit.Text("Deleting... Do not remove memory card.", 320, 179 * 1.15f, s3::kBrowserText,
					kit.FitScale("Deleting... Do not remove memory card.", 0.875f, 592), 1, 127);
		}
		else if (cd == 6)
		{
			// The destination (8.2): "Copy", a down arrow, the destination in blue between the arrows, its free space under it.
			const int add = two ? 6 : 0;
			kit.Text("Copy", kit.Ox(428), (103 + add) * 1.15f, s3::kYellow, 1.0f, 1, 128);
			kit.Text(std::string(1, s3::kSymDown), kit.Ox(432), (118 + add) * 1.15f, {60, 60, 60}, 1.0f, 1, 128);
			const std::string dest = "Memory Card (PS2)/2";
			const int dw = kit.TextWidth(dest, 0.875f);
			kit.Text(dest, kit.Ox(432), (134 + add) * 1.15f, s3::kBlue, 0.875f, 1, 128);
			kit.Text(std::string(1, s3::kSymLeft), kit.Ox(432) - dw / 2.0f - 18, (134 + add) * 1.15f, s3::kWhite, 0.875f, 0, 128);
			kit.Text(std::string(1, s3::kSymRight), kit.Ox(432) + dw / 2.0f + 4, (134 + add) * 1.15f, s3::kWhite, 0.875f, 0, 128);
			kit.Text(S3CardSpace(S.card2), kit.Ox(432), (148 + add) * 1.15f, s3::kBrowserText, 0.65f, 1, 128);
		}
		else if (cd == 7 || cd == 8 || cd == 9)
		{
			const int add = two ? 6 : 0;
			if (cd == 8)
			{
				const std::string q = "Do you wish to overwrite previous data?";
				kit.Text(q, kit.Ox(432), (128 + add) * 1.15f, s3::kBrowserText, kit.FitScale(q, 1.0f, 330), 1, 128);
			}
			else
			{
				// "Copy > destination" (the OSD arrow), then Are you sure?
				const std::string line = std::string("Copy ") + std::string(1, 1) + " Memory Card (PS2)/2";
				kit.Text(line, kit.Ox(432), (103 + add) * 1.15f, s3::kYellow, kit.FitScale(line, 1.0f, 330), 1, 128);
				kit.Text("Are you sure?", kit.Ox(432), (128 + add) * 1.15f, s3::kBrowserText, 1.0f, 1, 128);
			}
			const int w1 = kit.TextWidth("Yes", 1.0f), w2 = kit.TextWidth("    ", 1.0f), w3 = kit.TextWidth("No", 1.0f);
			const float x0 = kit.Ox(432) - (w1 + w2 + w3) / 2;
			const int ya = cd == 9 ? 32 : 128;
			kit.Text("Yes", x0, (148 + add) * 1.15f, S.yes_no == 0 ? s3::kBlue : s3::kDark, 1.0f, 0, ya);
			kit.Text("No", x0 + w1 + w2, (148 + add) * 1.15f, S.yes_no == 1 ? s3::kBlue : s3::kDark, 1.0f, 0, ya);
			if (cd == 9)
				kit.Text("Copying... Do not remove memory cards.", 320, 179 * 1.15f, s3::kBrowserText,
					kit.FitScale("Copying... Do not remove memory cards.", 0.875f, 592), 1, static_cast<int>(127 * std::fabs(std::sin(S.Fr * 0.06))));
		}
		else if (cd == 4)
			kit.Text(S.msg, 320, 179 * 1.15f, s3::kBrowserText, kit.FitScale(S.msg, 0.875f, 592), 1, 127);
		}
	}
	kit.Bands();
	if (S.dd0 == 0)
		kit.MarkScene();
	// Header: the small card icon and "Memory Card (PS2)/1", free space; the selected save's title top-right.
	if (card && S.dd0 < 25)
	{
		M4 model = MMul(MTrans(0, 0, 400), MMul(MRotX(kPi_ / 4), MTrans(0, 10, 0)));
		kit.DrawIco(*card, model.m, kit.Ox(320 - 271), 128 - 81 * 1.15f, S3CardLights(0.24f), static_cast<float>(S.Fr), &kit.Cached("bios:ICOBPS2M", card->tex),
			ta / 127.0f);
	}
	const int ha = S.dd0 > 0 ? 0 : ta;
	kit.Text("Memory Card (PS2)/1", kit.Ox(76), 34, s3::kBrowserText, kit.FitScale("Memory Card (PS2)/1", 0.77f, 240), 0, ha);
	if (S.card.formatted())
		kit.Text(S3CardSpace(S.card), kit.Ox(76), 48, s3::kBrowserText, 0.6f, 0, ha);
	else
		kit.Text(S.card.ok() ? "Unformatted" : "No memory card", kit.Ox(76), 48, s3::kBrowserText, 0.6f, 0, ha);
	if (n > 0 && S.dd0 == 0)
	{
		const s3::Save& sv = saves[static_cast<size_t>(S.c_cur)];
		kit.Text(sv.corrupted ? std::string("Corrupted Data") : sv.sys.title1, kit.Ox(616), 34, s3::kYellow, 0.8f, 2, 128);
		if (!sv.corrupted)
			kit.Text(sv.sys.title2, kit.Ox(616), 48, s3::kYellow, 0.8f, 2, 128);
	}
	if (n == 0)
		kit.Text("There is no data.", 320, 104 * 1.15f, s3::kBrowserText, kit.FitScale("There is no data.", 1.0f, 592), 1, ta);
	if (S.c_detail == 0)
		kit.Hints(n > 0 ? std::vector<s3::Kit::Hint>{{s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}, {s3::Button::Triangle, "Options"}}
		                : std::vector<s3::Kit::Hint>{{s3::Button::Circle, "Back"}},
			ta);
	else if (S.c_detail == 1 || S.c_detail == 2 || S.c_detail == 6 || S.c_detail == 7 || S.c_detail == 8)
		kit.Hints({{s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}});
	else if (S.c_detail == 4 || S.c_detail == 5)
		kit.Hints({{s3::Button::Circle, "Back"}});
	S.card_read = -100;
}
} // namespace fe

#include "s3_opening.inc"
