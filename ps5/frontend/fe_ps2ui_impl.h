// PS5 port frontend: the PS2 system software's screens (ps2-console-shelf, AI-assisted). Included once, at the end of
// fe_app.cpp (it uses that file's helpers); not a header for anything else.
//
// Home is the PS2 main menu (its orb and thin cyan text on dark fog), the Browser is the PS2 Browser's lit grey field with
// the places games come from floating in it, the Library is the Memory Card screen with the games as floating cases, the
// sheets are System Configuration (the indigo field, glass cubes, the clock's pillar wheel, one focused row with its value
// under it), the disc in the drive takes the disc's own screen, and L1 + R1 + Touch comes back through the boot towers.
// Every screen fills the display: there is no window inside a frame.
//
// Measures are the approved concept's, in pixels of a 1920 x 1080 artboard (`s` scales them to the display).
// Needs proper testing on the console.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_ps2.h"

namespace fe
{
namespace
{
// The camera of the PS2 screens: straight on, from far, with a narrow field, so a case's size on screen barely changes
// with its depth (things float in a flat picture, as on the PS2).
constexpr float kPs2Eye = 30.0f, kPs2Fov = 18.0f;
constexpr float kGridColStep = 232.0f, kGridRowStep = 206.0f, kGridX0 = 300.0f, kGridY0 = 270.0f;
constexpr float kGridCaseW = 118.0f, kGridSelW = 150.0f;

// World units a display pixel is worth at the depth of the cases (z = 0).
float Ps2UnitsPerPixel(float H)
{
	return 2.0f * kPs2Eye * std::tan(Radians(kPs2Fov) * 0.5f) / H;
}

void Ps2Camera(FrameDesc& f, float W, float H)
{
	const Mat4 proj = Mat4::Perspective(Radians(kPs2Fov), W / H, 1.0f, 80.0f);
	const Vec3 eye(0.0f, 0.0f, kPs2Eye);
	f.view_proj = proj * Mat4::LookAt(eye, Vec3(0, 0, 0), Vec3(0, 1, 0));
	f.cam_pos = eye;
}

// A world point at depth z (world units) for a display pixel (sx, sy).
Vec3 Ps2World(float sx, float sy, float z, float W, float H)
{
	const float u = Ps2UnitsPerPixel(H) * (kPs2Eye - z) / kPs2Eye;
	return Vec3((sx - W * 0.5f) * u, -(sy - H * 0.5f) * u, z);
}

// A case's model: centre at display pixel (sx, sy), `width_px` wide on screen, turned (radians) about y, then x, then z.
Mat4 Ps2CaseModel(float sx, float sy, float z, float width_px, float yaw, float pitch, float roll, float W, float H)
{
	const Vec3 p = Ps2World(sx, sy, z, W, H);
	const float scale = width_px * Ps2UnitsPerPixel(H) / (2.0f * kBoxHalfW);
	return Mat4::Translate(p.x, p.y, p.z) * Mat4::RotateZ(roll) * Mat4::RotateX(pitch) * Mat4::RotateY(yaw) * Mat4::Scale(scale, scale, scale);
}

// A slow bob of `amp` pixels with a period of `period` seconds, offset by `phase` (0..1).
float Bob(float t, float period, float phase, float amp)
{
	return std::sin((t / period + phase) * 2.0f * kPi) * amp;
}

std::string TodayText()
{
	const std::time_t now = std::time(nullptr);
	std::tm tm = {};
	localtime_r(&now, &tm);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%d/%m/%Y", &tm);
	return buf;
}

std::string NowText()
{
	const std::time_t now = std::time(nullptr);
	std::tm tm = {};
	localtime_r(&now, &tm);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
	return buf;
}

// The source a game's image comes from: its root folder, and what the Browser calls it.
void SourceOf(const std::string& path, std::string& prefix, std::string& label, int& kind)
{
	auto take = [&](size_t components) {
		size_t at = 0;
		for (size_t i = 0; i < components && at != std::string::npos; i++)
			at = path.find('/', at + 1);
		prefix = at == std::string::npos ? path : path.substr(0, at + 1);
	};
	if (path.compare(0, 8, "/mnt/usb") == 0 && path.size() > 8)
	{
		take(2);
		label = "USB drive " + std::to_string((path[8] >= '0' && path[8] <= '9' ? path[8] - '0' : 0) + 1);
		kind = 2;
	}
	else if (path.compare(0, 8, "/mnt/ext") == 0)
	{
		take(2);
		label = "Extended storage";
		kind = 2;
	}
	else if (path.compare(0, 5, "/nfs/") == 0)
	{
		take(2);
		label = "Network share " + prefix.substr(5, prefix.size() > 6 ? prefix.size() - 6 : 0);
		kind = 3;
	}
	else if (path.compare(0, 6, "/data/") == 0)
	{
		prefix = "/data/";
		label = "PS5 storage";
		kind = 1;
	}
	else
	{
		take(2);
		label = prefix;
		kind = 3;
	}
}
} // namespace

void App::GoTo(Screen screen)
{
	if (screen == m_screen)
		return;
	// P2-02: the old screen fades to black, then the new one fades up (the PS2 goes through black between its screens; before,
	// the new screen came at once at a third of its brightness, so the colours jumped). m_screen_time is when the new one
	// starts to come in, kScreenOut from now; Build draws the old one until then.
	const bool fading = m_time < m_screen_time; // a change while the last one was still fading out: no second fade-out
	if (!fading)
	{
		m_prev_screen = m_screen;
		m_prev_screen_time = m_screen_time;
	}
	// P2-26: between the Browser and the Library the PS2 changes page without black: the object flies at the eye (or back out).
	const int zoom = m_screen == Screen::Browser && screen == Screen::Library ? 1 : m_screen == Screen::Library && screen == Screen::Browser ? -1 : 0;
	if (zoom != 0 && fading) // turned back mid-change: start over from here, no black
	{
		m_prev_screen = m_screen;
		m_prev_screen_time = m_screen_time;
	}
	if (zoom > 0)
		m_zoom_item = m_browser_item;
	m_zoom = zoom;
	m_zoom_time = m_time;
	m_screen = screen;
	m_screen_time = zoom > 0 ? m_time + kZoomFly : zoom < 0 ? m_time + kZoomGap : fading ? m_screen_time : m_time + kScreenOut;
	m_held = 0;
	m_home_version = false;
	m_home_credits = false;
	std::printf("[ps2] screen: %s\n", screen == Screen::Home ? "home" : screen == Screen::Browser ? "browser" : "library");
	std::fflush(stdout);
}

// P2-01: the game played last, where it is on the shelf now; only when it is there to start (not hidden, not left out by a
// search, readable).
bool App::ContinuePos(int& pos) const
{
	pos = m_continue_index >= 0 ? PositionOf(m_continue_index) : -1;
	return pos >= 0 && m_games[static_cast<size_t>(pos)].damaged.empty() && m_search.applied.empty();
}

int App::HomeItems(HomeItem* out) const
{
	int n = 0;
	int pos;
	if (ContinuePos(pos))
		out[n++] = HomeItem::Continue;
	out[n++] = HomeItem::PlayGames;
	out[n++] = HomeItem::Settings;
	if (m_cfg.achievements.state)
		out[n++] = HomeItem::Achievements;
	if (m_drive_on || m_dump_on)
		out[n++] = HomeItem::Disc;
	return n;
}

void App::RebuildSources()
{
	std::vector<Source> found;
	auto count = [&](const GameInfo& g, int index) {
		if (index < 0 || (g.hidden && !m_show_hidden))
			return;
		Source s;
		SourceOf(g.path, s.prefix, s.label, s.kind);
		for (Source& f : found)
			if (f.prefix == s.prefix)
			{
				f.count++;
				return;
			}
		s.count = 1;
		found.push_back(std::move(s));
	};
	for (size_t i = 0; i < m_games.size(); i++)
		count(m_games[i], m_index[i]);
	for (const Shelved& h : m_shelved)
		count(h.game, h.index);
	int total = 0;
	for (const Source& s : found)
		total += s.count;
	const std::string was = m_source > 0 && m_source < static_cast<int>(m_sources.size()) ? m_sources[static_cast<size_t>(m_source)].prefix : std::string();
	m_sources.clear();
	Source all;
	all.label = "Library";
	all.count = total;
	m_sources.push_back(all);
	if (found.size() > 1) // one place only: the Library is all of it
	{
		std::sort(found.begin(), found.end(), [](const Source& a, const Source& b) { return a.kind != b.kind ? a.kind < b.kind : a.label < b.label; });
		for (Source& s : found)
			m_sources.push_back(std::move(s));
	}
	m_source = 0;
	for (size_t i = 1; i < m_sources.size(); i++)
		if (!was.empty() && m_sources[i].prefix == was)
			m_source = static_cast<int>(i);
	m_browser_item = std::min(m_browser_item, BrowserCount() - 1);
}

bool App::SourceMatches(const GameInfo& g) const
{
	if (m_source <= 0 || m_source >= static_cast<int>(m_sources.size()))
		return true;
	const std::string& p = m_sources[static_cast<size_t>(m_source)].prefix;
	return g.path.compare(0, p.size(), p) == 0;
}

void App::SetSource(int source)
{
	if (source == m_source)
		return;
	const int keep = m_games.empty() ? m_search_return : m_index[static_cast<size_t>(m_selected)];
	m_source = source;
	ApplyHidden(keep);
	m_grid_scroll = std::floor(static_cast<float>(m_selected / kGridCols));
	m_grid_scroll_vel = 0;
	m_flip_latched = false;
	std::printf("[ps2] library source: %s (%zu games)\n", m_sources[static_cast<size_t>(source)].label.c_str(), m_games.size());
	std::fflush(stdout);
}

void App::UpdateHome(const Input& in)
{
	auto pressed = [&](bool now, bool before) { return now && !before; };
	const bool any = in.left || in.right || in.cross || in.options || in.l1 || in.r1 || in.up || in.down || in.square || in.triangle || in.circle;
	if (!m_released)
	{
		m_released = !any;
		return;
	}
	if (m_qr_big)
	{
		if (pressed(in.circle, m_prev.circle) || pressed(in.triangle, m_prev.triangle) || pressed(in.cross, m_prev.cross))
		{
			m_qr_big = false;
			Sound(Sfx::Close, 0.0f);
		}
		return;
	}
	HomeItem items[8];
	const int n = HomeItems(items);
	m_home_row = std::max(0, std::min(m_home_row, n - 1));
	// P2-16: Version Information, as the PS2's: Triangle opens it; Circle or Triangle go back; Square shows the credits and licence
	// (one level deeper), Circle back from those. The menu doesn't move or pick while it is up.
	if (m_home_version)
	{
		if (m_home_credits && (pressed(in.circle, m_prev.circle) || pressed(in.square, m_prev.square)))
		{
			m_home_credits = false;
			Sound(Sfx::Back, 0.0f);
		}
		else if (!m_home_credits && pressed(in.square, m_prev.square))
		{
			m_home_credits = true;
			Sound(Sfx::Page, 0.0f);
		}
		else if (pressed(in.circle, m_prev.circle) || pressed(in.triangle, m_prev.triangle))
		{
			m_home_version = m_home_credits = false;
			Sound(Sfx::Back, 0.0f);
		}
		return;
	}
	if (pressed(in.triangle, m_prev.triangle))
	{
		m_home_version = true;
		Sound(Sfx::Page, 0.0f);
		return;
	}
	const int v = pressed(in.up, m_prev.up) ? -1 : pressed(in.down, m_prev.down) ? 1 : 0;
	if (v != 0)
	{
		const int next = std::max(0, std::min(n - 1, m_home_row + v));
		if (next != m_home_row)
		{
			m_home_row = next;
			Sound(Sfx::Move, 0.0f);
		}
	}
	if (!pressed(in.cross, m_prev.cross))
		return;
	switch (items[m_home_row])
	{
		case HomeItem::Continue: // P2-01: straight into the game played last (Home, Cross: one press, where it took three)
		{
			int pos;
			if (!ContinuePos(pos))
				break;
			if (m_source != 0)
			{
				SetSource(0);
				ContinuePos(pos);
			}
			if (pos != m_selected)
				Step(pos - m_selected);
			if (BiosReady(BiosWait::Pick))
			{
				if (m_cfg.game_achievements.cancel)
					m_cfg.game_achievements.cancel();
				m_launching = true;
				m_launch_time = m_time;
				Sound(Sfx::Launch, 0.0f);
			}
			break;
		}
		case HomeItem::PlayGames:
			RebuildSources();
			if (m_dump_on || m_drive_on)
				m_browser_item = BrowserCount() - 1; // the disc slot, as the PS2 Browser puts its disc first
			GoTo(Screen::Browser);
			Sound(Sfx::Select, 0.0f);
			break;
		case HomeItem::Settings:
			OpenSheet(true);
			Sound(Sfx::Open, 0.0f);
			break;
		case HomeItem::Achievements:
			m_account.Open();
			m_ime_field = -1;
			Sound(Sfx::Open, 0.0f);
			break;
		case HomeItem::Disc:
			if (m_dump_on)
			{
				const int pos = DumpPos();
				SetSource(0);
				if (pos >= 0)
					Step(DumpPos() - m_selected);
				GoTo(Screen::Library);
				Sound(Sfx::Select, 0.0f);
			}
			else if (m_cfg.drive_disc_reask)
			{
				m_cfg.drive_disc_reask();
				Sound(Sfx::Select, 0.0f);
			}
			break;
	}
}

void App::UpdateBrowser(const Input& in)
{
	auto pressed = [&](bool now, bool before) { return now && !before; };
	const bool any = in.left || in.right || in.cross || in.options || in.l1 || in.r1 || in.up || in.down || in.square || in.triangle || in.circle;
	if (!m_released)
	{
		m_released = !any;
		return;
	}
	const int n = BrowserCount();
	m_browser_item = std::max(0, std::min(m_browser_item, n - 1));
	const int h = pressed(in.left, m_prev.left) ? -1 : pressed(in.right, m_prev.right) ? 1 : 0;
	if (h != 0)
	{
		const int next = std::max(0, std::min(n - 1, m_browser_item + h));
		if (next != m_browser_item)
		{
			m_browser_item = next;
			Sound(Sfx::Move, 0.18f * static_cast<float>(h));
		}
	}
	if (pressed(in.circle, m_prev.circle))
	{
		GoTo(Screen::Home);
		Sound(Sfx::Back, 0.0f);
		return;
	}
	// P2-17: no Square here (kurolume: "don't need settings button"): Settings is on Home, as on the PS2.
	if (!pressed(in.cross, m_prev.cross))
		return;
	if (m_browser_item < static_cast<int>(m_sources.size()))
	{
		SetSource(m_browser_item);
		m_grid_scroll = std::max(0.0f, std::floor(static_cast<float>(m_selected / kGridCols)) - 1.0f);
		GoTo(Screen::Library);
		Sound(Sfx::Select, 0.0f);
		return;
	}
	// The disc slot.
	if (m_dump_on)
	{
		SetSource(0);
		const int pos = DumpPos();
		if (pos >= 0)
			Step(pos - m_selected);
		GoTo(Screen::Library);
		Sound(Sfx::Select, 0.0f);
	}
	else if (m_cfg.drive_disc_reask)
	{
		m_cfg.drive_disc_reask();
		m_browser_note = m_drive_on ? "Checking the disc..." : "Looking for a disc..."; // P2-17: plain words
		m_browser_note_time = m_time;
		Sound(Sfx::Select, 0.0f);
		std::printf("[ps2] disc slot: %s\n", m_drive_on ? "the disc in the drive, asked about again" : "reload asked (no disc listed)");
		std::fflush(stdout);
	}
	else
	{
		m_browser_note = "No disc found";
		m_browser_note_time = m_time;
		Sound(Sfx::Error, 0.0f);
	}
}

void App::SetupFog(FrameDesc& f, int palette, float W, float H) const
{
	BgParams& bg = f.bg;
	bg = BgParams();
	auto set = [](float* v, float r, float g, float b, float w) {
		v[0] = r;
		v[1] = g;
		v[2] = b;
		v[3] = w;
	};
	set(bg.misc, W / H, 0.0f, 0.45f, static_cast<float>(m_time) + 20.0f);
	switch (palette)
	{
		case 0: // the main menu: nearly black, a faint navy haze
			set(bg.fog_lo, 0.0f, 0.0f, 0.006f, 0.0f);
			set(bg.fog_hi, 0.055f, 0.075f, 0.20f, 0.6f);
			set(bg.motes, 0.75f, 0.80f, 0.95f, 0.0f); // P2-13: no starfield; the PS2's menu is dark fog only
			break;
		case 1: // the Browser's lit field. P2-27: the real one's (bg.frag): still, no motes (it has none), no vignette (its gradient is all)
			set(bg.fog_lo, 0.0f, 0.0f, 0.02f, 1.0f);
			set(bg.fog_hi, 0.05f, 0.07f, 0.19f, 0.6f);
			set(bg.motes, 1.0f, 1.0f, 1.0f, 0.0f);
			bg.misc[2] = 0.0f;
			break;
		case 5: // P2-27: a page in the Browser (the Library, as the memory card's page): the same field ~25/255 darker
			set(bg.fog_lo, 1.0f, 0.0f, 0.02f, 1.0f);
			set(bg.fog_hi, 0.05f, 0.07f, 0.19f, 0.6f);
			set(bg.motes, 1.0f, 1.0f, 1.0f, 0.0f);
			bg.misc[2] = 0.0f;
			break;
		case 2: // System Configuration's indigo (P2-19: marbled, no motes, as the real PS2's: ~#392d4d..#37275c at the edges)
			// P2-19e: as sampled out at the edges (~#40306a..#402e6a at 0.8..0.9 H from the hub); the sheets darken the middle.
			set(bg.fog_lo, 0.120f, 0.085f, 0.250f, 0.0f);
			set(bg.fog_hi, 0.300f, 0.220f, 0.480f, 0.0f);
			set(bg.motes, 0.0f, 0.0f, 0.0f, -1.0f);
			bg.misc[1] = 0.04f;
			bg.misc[2] = 0.0f;
			break;
		case 3: // the disc's navy
			set(bg.fog_lo, 0.004f, 0.004f, 0.04f, 0.0f);
			set(bg.fog_hi, 0.086f, 0.18f, 0.51f, 0.85f);
			set(bg.motes, 0.67f, 0.80f, 1.0f, 0.6f);
			break;
		default: // the boot fog
			set(bg.fog_lo, 0.0f, 0.0f, 0.024f, 0.0f);
			set(bg.fog_hi, 0.07f, 0.157f, 0.55f, 1.0f);
			set(bg.motes, 0.55f, 0.78f, 1.0f, 0.7f);
			break;
	}
}

void App::BuildBottomLines(std::vector<UiVertex>& ui, float W, float H)
{
	const float k = H / 2160.0f;
	BuildBiosLine(ui, W, k);
	BuildGameDbLine(ui, W, k);
	// The HD texture pack on its way: one line and a thin glowing bar, bottom left (above the prompts).
	using State = TexturePackStatus::State;
	if (m_cfg.texture_packs && m_cfg.texture_packs.activity)
	{
		const TexturePackActivity a = m_cfg.texture_packs.activity();
		const float s = H / 1080.0f;
		if (a.active)
		{
			const bool measured = a.state == State::Checking || a.state == State::Downloading || a.state == State::Unpacking;
			std::string line = std::string(Tr(Str::HdTextures)) + ": " + a.title;
			if (measured)
				line += "  \xC2\xB7  " + std::string(a.state == State::Unpacking ? "Unpacking " : a.state == State::Downloading ? "Downloading " : "") +
					std::to_string(static_cast<int>(a.fraction * 100.0)) + " %";
			if (a.waiting > 0)
				line += "  (+" + std::to_string(a.waiting) + ")";
			const float x = 110.0f * s, y = 232.0f * s;
			ps2::Text(ui, *m_fonts, Fit(*m_fonts, line, 26.0f * s, 900.0f * s).c_str(), x, y, 26.0f * s, ps2::Detail());
			if (measured)
				ps2::Bar(ui, x, y + 18.0f * s, 460.0f * s, static_cast<float>(a.fraction), s);
		}
	}
}

// ---- Home: the PS2 main menu ----------------------------------------------------------------------------------------

// P2-23: the main menu's lights' one clock. The first caller starts it: Home with the fly-in, anything else already formed.
float App::LightsTime(bool fly_in)
{
	if (m_lights_t0 < 0)
		m_lights_t0 = fly_in ? m_time : m_time - 2.0;
	return static_cast<float>(m_time - m_lights_t0);
}

double App::SheetExit() const
{
	const double ex = m_time - m_sheet_close_time;
	return !m_sheet_open && m_sheet_over_home && m_screen == Screen::Home && ex >= 0.0 && ex < kSheetBack ? ex : -1.0;
}

// P2-23: where the lights are between Home's place (0) and the sheet's hub (1): they glide over on the way in (0.6 s, as the field
// starts to grow) and back at the end of the way out (0.9..1.5 s, on black, before Home's words), as the PS2's do.
float App::SheetLightsAt() const
{
	const double ex = SheetExit();
	if (ex >= 0.0)
		return 1.0f - Smoothstep(0.9f, 1.5f, static_cast<float>(ex));
	if (m_sheet_open && m_sheet_over_home)
		return Smoothstep(0.0f, 0.6f, static_cast<float>(m_time - m_sheet_open_time));
	return m_sheet_open ? 1.0f : 0.0f;
}

void App::BuildHome(FrameDesc& f, float W, float H, const std::string& clock)
{
	(void)clock;
	const float s = H / 1080.0f;
	SetupFog(f, 0, W, H);
	std::vector<UiVertex>& ui = f.ui;
	const float in = Smoothstep(0.0f, 0.45f, static_cast<float>(m_time - m_screen_time));
	HomeItem items[8];
	const int n = HomeItems(items);
	m_home_row = std::max(0, std::min(m_home_row, n - 1));
	const float px = 46.0f * s, step = 62.0f * s;
	const float cx = W * 0.62f, top = H * 0.45f - (static_cast<float>(n - 1) * step) * 0.5f;
	if (m_home_row != m_home_lit) // P2-13: the highlight moved
	{
		m_home_lit_prev = m_home_lit < 0 ? -1 : m_home_lit;
		m_home_lit_time = m_home_lit < 0 ? -10.0 : m_time;
		m_home_lit = m_home_row;
	}
	const float hl = Smoothstep(0.0f, 0.2f, static_cast<float>(m_time - m_home_lit_time));
	// P2-15: the lights' ring sits where the real one does (measured): its centre 0.365 of the screen height left of the menu's
	// centre line, level with the menu's middle, radius 0.143 of the height.
	// P2-23: on the one clock that never restarts (the fly-in only the first time Home shows); coming back from the sheet they glide
	// from its hub to here, then the words fade up, as the PS2's System Configuration hands back to its main menu.
	const float mid = top + (static_cast<float>(n - 1) * step) * 0.5f;
	m_home_orb[0] = cx - 0.365f * H;
	m_home_orb[1] = mid;
	m_home_orb[2] = 0.143f * H;
	{
		const float g = SheetLightsAt();
		float o[3];
		for (int i = 0; i < 3; i++)
			o[i] = m_home_orb[i] + (m_hub_orb[0] < 0 ? 0.0f : (m_hub_orb[i] - m_home_orb[i]) * g);
		ps2::Orb(ui, o[0], o[1], o[2], LightsTime(true), 1.0f);
	}
	const size_t words_begin = ui.size();
	// P2-16: Version Information takes the menu's place, as on the PS2 (the lights stay).
	for (int i = 0; i < n && !m_home_version; i++)
	{
		const char* label = "";
		switch (items[i])
		{
			case HomeItem::Continue: label = "Continue"; break;
			case HomeItem::PlayGames: label = "Browser"; break;               // P2-16b: the PS2's names
			case HomeItem::Settings: label = "System Configuration"; break;
			case HomeItem::Achievements: label = "RetroAchievements"; break;
			case HomeItem::Disc: label = m_dump_on ? "PS2 Disc (copying)" : "PS2 Disc"; break;
		}
		// P2-13: grey to light blue and back over ~0.2 s, as the PS2 crossfades its menu items.
		const float lit = i == m_home_lit ? hl : i == m_home_lit_prev ? 1.0f - hl : 0.0f;
		const uint32_t gc = ps2::Grey(1.0f), cc = ps2::Cyan(1.0f);
		auto ch = [&](int sh) { return static_cast<float>(((gc >> sh) & 0xff) * (1.0f - lit) + ((cc >> sh) & 0xff) * lit) / 255.0f; };
		ps2::Text(ui, *m_fonts, label, cx, top + i * step + px * 0.35f, px, ps2::Rgba(ch(0), ch(8), ch(16), in), Fonts::Center, lit);
	}
	// P2-13: the orb stays where the PS2's does, left of the menu; moving up and down only changes the highlight.
	// This replaces P2-07's spring glide to the focused item (and the jump before it).
	// P2-15: the lights' ring sits where the real one does (measured): its centre 0.365 of the screen height left of the menu's
	// centre line, level with the menu's middle, radius 0.143 of the height. Their own clock starts when Home comes in.
	// P2-16: no line under the menu (the PS2's has none). Triangle: Version Information, a few labelled lines as the PS2's
	// (labels grey on the left, values white); Square there: the credits and licence, one level deeper.
	if (m_home_version)
	{
		const float rpx = 32.0f * s, rstep = 46.0f * s, gap = 16.0f * s;
		if (!m_home_credits)
		{
			const bool open_bios = m_bios_open_in_use;
			const std::pair<const char*, std::string> rows[] = {
				{"Shelf", m_cfg.app_title},
				{"Build", m_cfg.build_tag.empty() ? std::string("-") : m_cfg.build_tag},
				{"Emulator", "PCSX2"},
				{"BIOS", m_bios_missing ? std::string("None found") : open_bios ? std::string("Open BIOS") : std::string("Your own")},
			};
			const int rn = static_cast<int>(sizeof(rows) / sizeof(rows[0]));
			const float y0 = mid - (static_cast<float>(rn - 1) * rstep) * 0.5f + rpx * 0.35f;
			ps2::Text(ui, *m_fonts, "Version Information", cx, y0 - rstep * 1.4f, rpx, ps2::Cyan(in), Fonts::Center);
			for (int i = 0; i < rn; i++)
			{
				const float y = y0 + static_cast<float>(i) * rstep;
				ps2::Text(ui, *m_fonts, rows[i].first, cx - gap, y, rpx, ps2::Grey(in), Fonts::Right);
				ps2::Text(ui, *m_fonts, Fit(*m_fonts, rows[i].second, rpx, W * 0.30f).c_str(), cx + gap, y, rpx, ps2::White(in));
			}
		}
		else
		{
			const char* lines[] = {"PS5SX2 by sword.pdf", "PCSX2, GPL-3.0-or-later", "A PS2-style shelf, not made or endorsed by Sony"};
			const float y0 = mid - rstep + rpx * 0.35f;
			ps2::Text(ui, *m_fonts, "Credits", cx, y0 - rstep * 1.4f, rpx, ps2::Cyan(in), Fonts::Center);
			for (int i = 0; i < 3; i++)
				ps2::Text(ui, *m_fonts, lines[i], cx, y0 + static_cast<float>(i) * rstep, 28.0f * s, ps2::Grey(in), Fonts::Center);
		}
	}
	if (!m_refused_text.empty() && m_time - m_refused_time < 5.0)
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, m_refused_text, 28.0f * s, W * 0.8f).c_str(), W * 0.5f, H * 0.70f, 28.0f * s, ps2::Warn(), Fonts::Center);
	// P2-16: the two prompts as the PS2's (measured from its main menu): on one baseline at 0.80 of the height, each centred
	// 0.28 of the height either side of the menu's centre line.
	{
		const float ppx = 38.0f * s, off = 0.28f * H, base = H * 0.80f;
		auto centred = [&](const ps2::Prompt& p, float at) {
			const float w = ps2::PromptsWidth(*m_fonts, &p, 1, ppx); // P2-18
			ps2::Prompts(ui, *m_fonts, &p, 1, at - w * 0.5f, base, ppx, false);
		};
		if (!m_home_version)
		{
			centred({icon::Cross, "Enter"}, cx - off);
			centred({icon::Triangle, "Version"}, cx + off);
		}
		else
		{
			centred({icon::Circle, "Back"}, cx - off);
			if (!m_home_credits)
				centred({icon::Square, "Credits"}, cx + off);
		}
	}
	{
		const double ex = SheetExit(); // P2-23: back from the sheet: the words come up after the lights have arrived
		if (ex >= 0.0)
			FadeRange(ui, words_begin, ui.size(), Smoothstep(static_cast<float>(kSheetOut) + 0.15f, static_cast<float>(kSheetBack), static_cast<float>(ex)));
	}
	BuildBottomLines(ui, W, H);
	if (m_launching) // P2-01: Continue: the menu fades out with the picture as the game starts
		FadeRange(ui, 0, ui.size(), 1.0f - Smoothstep(0.1f, 0.6f, static_cast<float>(m_time - m_launch_time)));
}

// ---- The Browser: where games live ----------------------------------------------------------------------------------

void App::BuildBrowser(FrameDesc& f, float W, float H, const std::string& clock)
{
	const float s = H / 1080.0f, t = static_cast<float>(m_time);
	SetupFog(f, 1, W, H);
	Ps2Camera(f, W, H);
	std::vector<UiVertex>& ui = f.ui;
	const int n = BrowserCount();
	// P2-26: the PS2's page change (yt 1G4yRe288Os 1:18, 25 fps). Into a page the picked object tips back and flies at the eye
	// (accelerating, 0.36 s), everything else rushing out with it as the eye closes in; its bloom goes by 0.12 s, the words by 0.16 s; it
	// fills the screen and the field is left. Back, the same played backwards (slowing as it lands): the header first, the prompts and
	// the bloom as it settles. `fly` 0 .. 1; `back` the seconds since coming back (-1 otherwise).
	const float fly = BrowserFly();
	const bool into = m_zoom > 0 && m_screen == Screen::Library;
	const float back = m_zoom < 0 && m_screen == Screen::Browser ? static_cast<float>(m_time - m_screen_time) : -1.0f;
	const float in = back >= 0.0f || into ? 1.0f : Smoothstep(0.0f, 0.4f, static_cast<float>(m_time - m_screen_time));
	const float since_fly = static_cast<float>(m_time - m_zoom_time);
	const float head_a = into ? 1.0f - Smoothstep(0.04f, 0.16f, since_fly) : back >= 0.0f ? Smoothstep(0.0f, 0.1f, back) : 1.0f;
	const float foot_a = into ? 1.0f - Smoothstep(0.04f, 0.16f, since_fly) : back >= 0.0f ? Smoothstep(0.28f, 0.40f, back) : 1.0f;
	const float glow_a = into ? 1.0f - Smoothstep(0.0f, 0.12f, since_fly) : back >= 0.0f ? Smoothstep(0.32f, 0.50f, back) : 1.0f;
	const float obj_a = 1.0f - Smoothstep(0.85f, 1.0f, fly); // filling the screen, it gives way to the field
	// The eye's approach, fitted to the real one's size at 25 fps: x1.35 at a third of the way, x2.6, x4, x7, x14, then ~x30 filling
	// the screen (slow at first, then rushing; back, it slows as it lands).
	const float zoom = std::exp(3.4f * std::pow(fly, 2.2f));
	const float tip = Smoothstep(0.0f, 0.45f, fly);          // the tip back is mostly done in the first third
	m_browser_item = std::max(0, std::min(m_browser_item, n - 1));
	const float spacing = std::min(330.0f * s, (W - 400.0f * s) / static_cast<float>(std::max(1, n)));
	const float y = H * 0.47f;
	const float zx = W * 0.5f + (static_cast<float>(m_zoom_item) - static_cast<float>(n - 1) * 0.5f) * spacing, zy = y;
	auto zp = [&](float px, float py) { return Vec2{zx + (px - zx) * zoom, zy + (py - zy) * zoom}; }; // P2-26: a point as the eye closes in
	// P2-17: every object floats on the shelf's own clock, focused or not, so a move never restarts it. The focus only eases in: a little lift,
	// the brightness, the bloom behind it (which glides over) and the disc's turn speed.
	constexpr int kMixes = static_cast<int>(sizeof(m_browser_focus) / sizeof(m_browser_focus[0]));
	const float since = m_browser_anim_time < 0 ? 1.0f : static_cast<float>(m_time - m_browser_anim_time);
	const float adt = Clamp(since, 0.0f, 0.1f);
	m_browser_anim_time = m_time;
	for (int i = 0; i < kMixes; i++)
	{
		const float want = i == m_browser_item ? 1.0f : 0.0f;
		m_browser_focus[i] = since > 0.25f ? want : m_browser_focus[i] + (want - m_browser_focus[i]) * (1.0f - std::exp(-adt * 8.0f));
	}
	auto focus_mix = [&](int i) { return i < kMixes ? m_browser_focus[i] : (i == m_browser_item ? 1.0f : 0.0f); };
	m_browser_disc_turn += adt * Mix(0.5f, 2.2f, focus_mix(n - 1));
	float glow_w = 0, glow_x = 0, glow_y = 0;
	std::string name, detail;
	for (int i = 0; i < n; i++)
	{
		const float x = W * 0.5f + (static_cast<float>(i) - static_cast<float>(n - 1) * 0.5f) * spacing;
		const bool focus = i == m_browser_item;
		const float fm = Smoothstep(0.0f, 1.0f, focus_mix(i));
		const float oy = y + Bob(t, 5.5f, i * 0.31f, 5.0f * s) - 10.0f * s * fm;
		const bool flown = fly > 0.0f && i == m_zoom_item; // P2-26: the one the eye flies into tips back
		const float otip = flown ? tip : 0.0f;
		const Vec2 zo = zp(x, oy);
		const float oa = in * obj_a;
		std::string label;
		if (i < static_cast<int>(m_sources.size()))
		{
			const Source& src = m_sources[static_cast<size_t>(i)];
			label = src.label;
			if (i == 0)
			{
				// The Library: three cases of the games near the last one played, in a loose stack. P2-17: one slow sway, focused or not, and
				// each case a clear step nearer than the one on its left, so the stack's order never changes (the right one in front).
				const float sway = Radians(-14.0f + 7.0f * std::sin(t * 2.0f * kPi / 9.0f));
				const int base = std::max(0, std::min(m_selected - 1, static_cast<int>(m_games.size()) - 3));
				const float offs[3][3] = {{-95, 18, -8}, {10, 0, 0}, {95, -14, 7}};
				int drawn = 0;
				for (int c = 0; c < 3; c++)
				{
					const int pos = base + c;
					if (pos >= static_cast<int>(m_games.size()) || !m_slots[static_cast<size_t>(pos)].set)
						continue;
					BoxDraw b;
					const Vec2 cp = zp(x + offs[c][0] * s * 0.75f, oy + offs[c][1] * s);
					b.model = Ps2CaseModel(cp.x, cp.y, 0.35f * c, 100.0f * s * zoom, sway * (1.0f - otip), Radians(6.0f + 70.0f * otip),
						Radians(offs[c][2]) * (1.0f - otip), W, H);
					b.set = m_slots[static_cast<size_t>(pos)].set;
					b.cover_mix = m_slots[static_cast<size_t>(pos)].cover_mix;
					b.brightness = Mix(0.8f, 1.0f, fm);
					b.alpha = oa;
					f.boxes.push_back(b);
					drawn++;
				}
				if (drawn == 0)
					ps2::Cube(ui, zo.x, zo.y, 90.0f * s * zoom, t * 0.3f, 0.5f + otip, 0.2f, oa);
			}
			else
				ps2::Drive(ui, zo.x, zo.y, 120.0f * s * zoom, focus ? 0.5f + 0.5f * std::sin(t * 5.0f) : 0.0f, oa);
			if (focus)
			{
				name = src.label;
				detail = std::to_string(src.count) + (src.count == 1 ? " title" : " titles");
				if (i == 0 && !m_cfg.games_folder_note.empty())
					detail = m_cfg.games_folder_note + "  \xC2\xB7  " + detail;
				else if (i > 0)
					detail = src.prefix + "  \xC2\xB7  " + detail;
			}
		}
		else
		{
			// The disc slot: the PS2 Browser's silver disc. Empty, it is see-through (Reload PS2 Disc); with a disc in the drive or being copied,
			// it is that game's disc: its own disc art, else its front printed as the label (P2-17), else the painted PS2 disc.
			// P2-26: flown into, the disc grows and opens out rounder as the eye comes over it (as the real one, 1:46).
			const float r = Mix(85.0f, 105.0f, fm) * s * zoom;
			const float ca = std::cos(Radians(-8.0f)), sa = std::sin(Radians(-8.0f)), squash = std::cos(Radians(58.0f - 22.0f * otip));
			const Vec2 ax = {r * ca, r * sa}, ay = {-r * sa * squash, r * ca * squash};
			if (m_drive_on || m_dump_on)
			{
				ps2::Glow(ui, zo.x, zo.y, r * 1.6f, r * 1.2f, ps2::Rgba(0.45f, 0.65f, 1.0f, 0.25f * oa * glow_a), 2.0f);
				if (m_dump_disc_set)
					DiscArtQuad(ui, zo.x, zo.y, ax, ay, m_browser_disc_turn, oa);
				else
					ps2::Disc(ui, zo.x, zo.y, ax, ay, m_browser_disc_turn, 1.0f, oa);
				label = m_dump_on ? "Copying" : "PS2 Disc";
				if (focus)
				{
					name = m_dump_on ? m_dump_title : m_drive_title;
					detail = m_dump_on ? "Copying to the library  \xC2\xB7  " + std::to_string(m_dump_pct) + " %"
					                   : "PS2 Disc" + (m_drive_serial.empty() ? std::string() : "  \xC2\xB7  " + m_drive_serial);
				}
				if (m_dump_on)
					ps2::Bar(ui, x - 90.0f * s, oy + 92.0f * s, 180.0f * s, m_dump_pct / 100.0f, s, in * foot_a);
			}
			else
			{
				ps2::EmptySlot(ui, zo.x, zo.y, ax, ay, m_browser_disc_turn, oa * Mix(0.8f, 1.0f, fm));
				label = "Reload PS2 Disc";
				if (focus)
				{
					name = "Reload PS2 Disc";
					detail = "No disc found";
				}
			}
		}
		glow_w += fm;
		glow_x += fm * x;
		glow_y += fm * oy;
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, label, 26.0f * s, spacing - 20.0f * s).c_str(), x, y + 150.0f * s, 26.0f * s,
			focus ? ps2::Cyan(in * foot_a) : ps2::Grey(in * foot_a), Fonts::Center, focus ? 0.8f : 0.0f);
	}
	if (glow_w > 0.001f) // the bloom behind the focus, gliding from one object to the next
	{
		f.bg.glow_color[0] = f.bg.glow_color[1] = f.bg.glow_color[2] = 1.0f;
		f.bg.glow_color[3] = 0.85f * in * glow_a * std::min(1.0f, glow_w);
		f.bg.glow_pos[0] = glow_x / glow_w / W;
		f.bg.glow_pos[1] = glow_y / glow_w / H;
		f.bg.glow_pos[2] = f.bg.glow_pos[3] = 0.17f;
	}
	// The header: where this is, top left; the focused object's name in yellow top right, its detail under it.
	const size_t head_begin = ui.size();
	ps2::Text(ui, *m_fonts, "PS5SX2", 110.0f * s, 128.0f * s, 40.0f * s, ps2::White(in));
	if (!clock.empty())
		ps2::Text(ui, *m_fonts, clock.c_str(), 110.0f * s, 172.0f * s, 28.0f * s, ps2::Detail(0.8f * in));
	ps2::Text(ui, *m_fonts, Fit(*m_fonts, name, 48.0f * s, W * 0.55f).c_str(), W - 110.0f * s, 132.0f * s, 48.0f * s, ps2::Yellow(in), Fonts::Right);
	ps2::Text(ui, *m_fonts, Fit(*m_fonts, detail, 28.0f * s, W * 0.6f).c_str(), W - 110.0f * s, 180.0f * s, 28.0f * s, ps2::Detail(in), Fonts::Right);
	if (!m_browser_note.empty() && m_time - m_browser_note_time < 4.0)
		ps2::Text(ui, *m_fonts, m_browser_note.c_str(), W - 110.0f * s, H * 0.70f, 30.0f * s, ps2::White(), Fonts::Right);
	else if (m_cfg.preselect >= 0 && m_browser_item == 0)
	{
		for (size_t i = 0; i < m_games.size(); i++)
			if (m_index[i] == m_cfg.preselect && m_cfg.preselect > 0)
			{
				ps2::Text(ui, *m_fonts, Fit(*m_fonts, "Last played: " + m_games[i].title, 30.0f * s, W * 0.6f).c_str(), W - 110.0f * s, H * 0.70f,
					30.0f * s, ps2::Detail(in), Fonts::Right);
				break;
			}
	}
	FadeRange(ui, head_begin, ui.size(), head_a);
	const size_t foot_begin = ui.size();
	const ps2::Prompt prompts[] = {{icon::Cross, "Enter"}, {icon::Circle, "Back"}}; // P2-17: as the PS2 Browser's (04-browser)
	ps2::Prompts(ui, *m_fonts, prompts, 2, W - 110.0f * s, H - 110.0f * s, 38.0f * s, true);
	FadeRange(ui, foot_begin, ui.size(), foot_a);
	BuildBottomLines(ui, W, H);
}

// P2-17: the disc's picture (m_dump_disc_set) on the axes ax, ay (as ps2::Disc), turned by `spin` in its own plane: its own disc art as it is, or
// the game's front cut round as the label of a silver disc (ui.frag's mode 6). The picture is bound in the atlas' place for these six vertices.
void App::DiscArtQuad(std::vector<UiVertex>& ui, float cx, float cy, Vec2 ax, Vec2 ay, float spin, float alpha)
{
	if (!m_dump_disc_set || alpha <= 0.003f)
		return;
	const float a = std::max(0.05f, m_dump_disc_aspect);
	// Picture units per disc radius: the front is cut (the disc as wide as its narrow side); disc art is shown whole.
	const bool cut = m_dump_disc_front;
	const float kx = (a < 1.0f) == cut ? 0.5f : 0.5f / a, ky = (a < 1.0f) == cut ? 0.5f * a : 0.5f;
	const float c = std::cos(spin), sn = std::sin(spin);
	const uint32_t col = ps2::Rgba(1, 1, 1, alpha);
	const float art = m_dump_disc_front ? 0.0f : 1.0f;
	auto v = [&](float u, float w) {
		const float ru = u * c - w * sn, rw = u * sn + w * c;
		return UiVertex{cx + ax.x * ru + ay.x * rw, cy + ax.y * ru + ay.y * rw, 0.5f + u * kx, 0.5f + w * ky, col, kx, ky, art, 6};
	};
	const uint32_t first = static_cast<uint32_t>(ui.size());
	const UiVertex q0 = v(-1, -1), q1 = v(1, -1), q2 = v(1, 1), q3 = v(-1, 1);
	ui.insert(ui.end(), {q0, q1, q2, q0, q2, q3});
	m_achievement_images.push_back({first, 6, m_dump_disc_set});
}

// ---- The Library: the Memory Card screen ----------------------------------------------------------------------------

// P2-26 (replaces P2-03's drift up): how far a case has grown (0 .. 1). As the PS2's pages: each item grows from a point at its own place
// in 0.2 s, easing out, one after another in reading order (0.04 s apart here; the real memory card's icons come as they load). Leaving
// for the Browser they all shrink back to points together in 0.16 s.
float App::LibraryArrive(int pos) const
{
	if (m_zoom < 0 && m_screen == Screen::Browser)
	{
		const float u = Clamp(static_cast<float>((m_time - m_zoom_time) / kZoomShrink), 0.0f, 1.0f);
		return 1.0f - u * u;
	}
	const float since = static_cast<float>(m_time - m_screen_time);
	if (since > 1.5f)
		return 1.0f;
	const int top = static_cast<int>(std::floor(m_grid_scroll));
	const int order = std::max(0, pos - top * kGridCols); // the visible cases in reading order
	const float delay = 0.04f * static_cast<float>(std::min(order, kGridCols * kGridRowsShown));
	const float u = Clamp((since - delay) / 0.2f, 0.0f, 1.0f);
	return 1.0f - (1.0f - u) * (1.0f - u); // eases out
}

// P2-26: how far the Browser's picked object has flown at the eye: 1 -> Library from 0 over kZoomFly; back, 1 -> 0 once the Library has gone.
float App::BrowserFly() const
{
	if (m_zoom > 0 && m_screen == Screen::Library)
		return Clamp(static_cast<float>((m_time - m_zoom_time) / kZoomFly), 0.0f, 1.0f);
	if (m_zoom < 0 && m_screen == Screen::Browser)
		return 1.0f - Clamp(static_cast<float>((m_time - m_screen_time) / kZoomFly), 0.0f, 1.0f);
	return 0.0f;
}

void App::GridPose(int pos, float t, Mat4& model, float& width, Vec2& screen) const
{
	const float W = static_cast<float>(m_renderer->width()), H = static_cast<float>(m_renderer->height()), s = H / 1080.0f;
	const int col = pos % kGridCols, row = pos / kGridCols;
	const float jx = static_cast<float>((pos * 37) % 23 - 11), jy = static_cast<float>((pos * 53) % 19 - 9);
	const bool sel = pos == m_selected;
	width = (sel ? kGridSelW : kGridCaseW) * s;
	// The grid's columns are centred on the display (the concept's x0 of 300 is for a 1920-wide screen).
	const float grid_w = (kGridCols - 1) * kGridColStep + 40.0f + kGridCaseW;
	const float x0 = (W / s - grid_w) * 0.5f;
	const float x = (x0 + col * kGridColStep + ((row & 1) ? 40.0f : 0.0f) + jx + kGridCaseW * 0.5f) * s;
	float y = (kGridY0 + (static_cast<float>(row) - m_grid_scroll) * kGridRowStep + jy + kGridCaseW * 0.7f) * s +
	          Bob(t, 5.5f, pos * 0.133f, 5.0f * s);
	width *= LibraryArrive(pos); // P2-26: growing from a point (or shrinking back to one)
	screen = {x, y};
	float yaw, pitch;
	if (sel)
	{
		yaw = Radians(-5.0f + 23.0f * std::sin(t * 2.0f * kPi / 7.0f)) * (1.0f - std::min(1.0f, std::fabs(m_flip) * 3.0f)) + FlipTurn(0.0f);
		pitch = Radians(4.0f + 2.0f * std::sin(t * 2.0f * kPi / 7.0f));
	}
	else
	{
		yaw = Radians(static_cast<float>((pos * 29) % 40 - 26));
		pitch = Radians(4.0f);
	}
	if (m_launching && sel && !m_system_menu)
		width *= 1.0f + 0.6f * Smoothstep(0.0f, 0.6f, static_cast<float>(m_time - m_launch_time));
	model = Ps2CaseModel(x, y, sel ? 0.4f : 0.0f, width, yaw, pitch, 0.0f, W, H);
}

void App::BuildLibrary(FrameDesc& f, float W, float H, const std::string& clock)
{
	(void)clock;
	const float s = H / 1080.0f, t = static_cast<float>(m_time);
	SetupFog(f, 5, W, H); // P2-27: the page's darker field
	Ps2Camera(f, W, H);
	std::vector<UiVertex>& ui = f.ui;
	float in = Smoothstep(0.0f, 0.4f, static_cast<float>(m_time - m_screen_time));
	const bool leaving = m_zoom < 0 && m_screen == Screen::Browser;
	float foot = 1.0f;
	if (leaving) // P2-26: leaving for the Browser: the words go in 0.12 s as the cases shrink
		foot = in = 1.0f - Smoothstep(0.0f, 0.12f, static_cast<float>(m_time - m_zoom_time));
	else if (m_zoom > 0) // P2-26: from the Browser (no black): the header comes up 0.15 .. 0.35 s into the page, the prompts 0.45 .. 0.65 s
	{
		in = Smoothstep(0.15f, 0.35f, static_cast<float>(m_time - m_screen_time));
		foot = Smoothstep(0.45f, 0.65f, static_cast<float>(m_time - m_screen_time));
	}
	const float case_in = m_zoom != 0 ? 1.0f : in; // P2-26: from the Browser the cases grow, they don't fade
	const int n = static_cast<int>(m_games.size());
	const int first = std::max(0, (static_cast<int>(std::floor(m_grid_scroll)) - 1) * kGridCols);
	const int last = std::min(n - 1, (static_cast<int>(std::ceil(m_grid_scroll)) + kGridRowsShown + 1) * kGridCols - 1);
	for (int i = first; i <= last; i++)
	{
		const Slot& sl = m_slots[static_cast<size_t>(i)];
		if (!sl.set && m_index[static_cast<size_t>(i)] >= 0)
			continue;
		Mat4 model;
		float width;
		Vec2 sc;
		GridPose(i, t, model, width, sc);
		// Rows sliding in or out at the edges of the field fade.
		const float rowpos = static_cast<float>(i / kGridCols) - m_grid_scroll;
		const float vis = Clamp(1.0f - std::max(-rowpos - 0.15f, rowpos - (kGridRowsShown - 1) - 0.15f) * 1.6f, 0.0f, 1.0f) *
		                  Smoothstep(0.0f, 0.05f, LibraryArrive(i)); // P2-26: a point is nothing
		if (vis <= 0.01f)
			continue;
		const bool sel = i == m_selected;
		if (sl.set)
		{
			BoxDraw b;
			b.model = model;
			b.set = sl.set;
			b.cover_mix = sl.cover_mix;
			b.brightness = (sel ? 1.0f : 0.86f) * (m_games[static_cast<size_t>(i)].hidden ? 0.45f : 1.0f);
			b.selected = sel ? 0.6f : 0.0f;
			b.alpha = vis * case_in;
			if (sl.shown_at >= 0) // P2-04: a case whose art has just arrived fades in over a third of a second
				b.alpha *= Smoothstep(0.0f, 0.35f, static_cast<float>(m_time - sl.shown_at));
			f.boxes.push_back(b);
		}
		const int idx = m_index[static_cast<size_t>(i)];
		if (idx == kDumpIndex || idx == kDriveIndex)
		{
			// The disc being copied, or the one in the drive: the disc itself, turning, in the case's place.
			const float r = width * 0.62f;
			const float spin = m_dump_spin * (idx == kDumpIndex ? 2.2f : (sel ? 1.2f : 0.3f));
			if (m_dump_disc_set) // the disc's own art (LaunchBox or covers/<serial>_disc.png), or its front as the label (P2-17), turning
				DiscArtQuad(ui, sc.x, sc.y, {r, 0}, {0, r}, spin, vis * case_in);
			else
				ps2::Disc(ui, sc.x, sc.y, {r, 0}, {0, r}, spin, 1.0f, vis * case_in);
			if (idx == kDumpIndex)
			{
				char pct[16];
				std::snprintf(pct, sizeof(pct), "%d %%", m_dump_pct);
				ps2::Text(ui, *m_fonts, pct, sc.x, sc.y + r + 34.0f * s, 26.0f * s, ps2::White(vis * in), Fonts::Center);
			}
		}
		if (sel)
		{
			f.bg.glow_color[0] = f.bg.glow_color[1] = f.bg.glow_color[2] = 1.0f;
			f.bg.glow_color[3] = 0.95f * in * vis;
			f.bg.glow_pos[0] = sc.x / W;
			f.bg.glow_pos[1] = sc.y / H;
			f.bg.glow_pos[2] = f.bg.glow_pos[3] = 0.16f;
		}
	}
	f.glow[0] = f.glow[1] = f.glow[2] = 1.0f;
	f.glow[3] = 0.6f;

	// P2-21: the PS2's scroll arrows: solid blue triangles centred over the grid, fixed on the screen (the rows move, they don't), no pulse
	// or bob. As measured on the PS2 (720p capture, scaled to 1080): up 33 x 21 px just above the top row, down 48 x 36 px under the bottom
	// row; the wide end a little lighter than the tip. They fade with the screen and in and out in 0.15 s (App::Update).
	auto arrow = [&](float cy, float w, float h, bool up, float a) {
		if (a <= 0.01f)
			return;
		const float cx = W * 0.5f, base = cy + (up ? h : -h) * 0.5f * s, tip = cy - (up ? h : -h) * 0.5f * s;
		ps2::Tri(ui, {cx - w * 0.5f * s, base}, {cx + w * 0.5f * s, base}, {cx, tip}, ps2::Rgba(0.08f, 0.23f, 0.66f, a));
		ui.back().color = ps2::Rgba(0.02f, 0.12f, 0.54f, a); // the tip
	};
	arrow(234.0f * s, 33.0f, 21.0f, true, m_grid_up * in);
	arrow(896.0f * s, 48.0f, 36.0f, false, m_grid_down * in);

	// The header: the card's icon and its name top left, the picked game's title in yellow top right.
	const Source& src = m_sources.empty() ? Source() : m_sources[static_cast<size_t>(std::min(m_source, static_cast<int>(m_sources.size()) - 1))];
	Fonts::AddRoundedRect(ui, 110.0f * s, 96.0f * s, 62.0f * s, 62.0f * s, 4.0f * s, ps2::Rgba(0.02f, 0.02f, 0.03f, 0.85f * in));
	Fonts::AddRoundedRect(ui, 113.0f * s, 99.0f * s, 56.0f * s, 3.0f * s, 1.0f * s, ps2::Rgba(1, 1, 1, 0.15f * in));
	const std::string head = (m_sources.empty() ? std::string("Library") : src.label) + " (PS5)/1";
	ps2::Text(ui, *m_fonts, head.c_str(), 196.0f * s, 136.0f * s, 46.0f * s, ps2::White(in));
	int real = 0;
	for (int i = 0; i < n; i++)
		real += m_index[static_cast<size_t>(i)] >= 0 ? 1 : 0;
	std::string count = std::to_string(real) + (real == 1 ? " title" : " titles");
	if (n > 1) // P2-09: where the picked case is, up with the count (it was 22 px in the bottom corner)
		count = real == n ? std::to_string(m_selected + 1) + " of " + count : std::to_string(m_selected + 1) + " of " + std::to_string(n) + "  \xC2\xB7  " + count;
	if (!m_search.applied.empty())
		count += "  \xC2\xB7  search \xE2\x80\x9C" + m_search.text + "\xE2\x80\x9D";
	ps2::Text(ui, *m_fonts, Fit(*m_fonts, count, 30.0f * s, W * 0.4f).c_str(), 198.0f * s, 180.0f * s, 30.0f * s, ps2::Detail(in));
	if (n > 0)
	{
		const GameInfo& g = m_games[static_cast<size_t>(m_selected)];
		const int idx = m_index[static_cast<size_t>(m_selected)];
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, g.title, 48.0f * s, W * 0.5f).c_str(), W - 110.0f * s, 136.0f * s, 48.0f * s, ps2::Yellow(in), Fonts::Right);
		std::string info;
		auto add = [&](const std::string& part) {
			if (part.empty())
				return;
			if (!info.empty())
				info += "  \xC2\xB7  ";
			info += part;
		};
		add(g.serial);
		add(Region(g.region));
		if (idx == kDumpIndex)
		{
			char cp[48];
			std::snprintf(cp, sizeof(cp), Tr(Str::CopyDumpPct), m_dump_pct);
			add(cp);
			if (m_dump_eta > 0)
			{
				char e[48];
				if (m_dump_eta < 60)
					std::snprintf(e, sizeof(e), "%s", Tr(Str::CopyEtaLessMin));
				else
					std::snprintf(e, sizeof(e), Tr(Str::CopyEtaMin), (m_dump_eta + 59) / 60);
				add(e);
			}
			add(m_dump_speed);
		}
		else if (idx == kDriveIndex)
			add("In the drive");
		else
			add(SizeText(g.bytes));
		for (const std::string& b : g.badges)
			add(b);
		if (!g.damaged.empty())
			add("can't be read");
		if (g.hidden)
			add("hidden");
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, info, 28.0f * s, W * 0.55f).c_str(), W - 110.0f * s, 182.0f * s, 28.0f * s, ps2::Detail(in), Fonts::Right);
	}
	else
	{
		int scan_done = 0, scan_total = 0;
		std::string none;
		if (m_cfg.scan_progress && m_cfg.scan_progress(scan_done, scan_total))
		{
			char line[160];
			std::snprintf(line, sizeof(line), Tr(Str::ShelfScanning), scan_done, scan_total);
			none = line;
		}
		else if (!m_search.applied.empty())
			none = "No game matches \xE2\x80\x9C" + m_search.text + "\xE2\x80\x9D";
		else
		{
			none = Tr(Str::NoGames);
			static const char kDefaultPath[] = "/data/PCSX2/games";
			const size_t at = m_cfg.games_folder_note.empty() ? std::string::npos : none.find(kDefaultPath);
			if (at != std::string::npos)
				none.replace(at, sizeof(kDefaultPath) - 1, m_cfg.games_folder_note);
		}
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, none, 40.0f * s, W * 0.85f).c_str(), W * 0.5f, H * 0.5f, 40.0f * s, ps2::White(in), Fonts::Center);
	}
	// A note or a refusal, for a few seconds, under the title.
	if (!m_note_text.empty() && m_time - m_note_time < 7.0 && m_note_time >= m_refused_time)
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, m_note_text, 26.0f * s, W * 0.7f).c_str(), W - 110.0f * s, 222.0f * s, 26.0f * s, ps2::Cyan(), Fonts::Right);
	else if (!m_refused_text.empty() && m_time - m_refused_time < 6.0)
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, m_refused_text, 26.0f * s, W * 0.7f).c_str(), W - 110.0f * s, 222.0f * s, 26.0f * s, ps2::Warn(), Fonts::Right);
	// The prompts: the buttons on the right; the covers' status in a corner. P2-22 (kurolume: "doesn't need a prompt for turn case"): no
	// R3 prompt; R3 and the right stick still turn the case.
	const bool drive = n > 0 && m_index[static_cast<size_t>(m_selected)] == kDriveIndex;
	const ps2::Prompt prompts[] = {{icon::Cross, drive ? "Play or copy" : "Start"}, {icon::Circle, m_search.applied.empty() ? "Back" : "Show all"},
		{icon::Square, "Settings"}, {icon::Triangle, "Search"}};
	const size_t foot_begin = ui.size();
	if (!m_search.typing) // (the search keyboard's own prompts go there while it is up)
		ps2::Prompts(ui, *m_fonts, prompts, 4, W - 110.0f * s, H - 110.0f * s, 38.0f * s, true);
	FadeRange(ui, foot_begin, ui.size(), foot);
	const std::string status = m_covers ? m_covers->Status() : std::string();
	if (!status.empty())
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, status, 26.0f * s, W * 0.4f).c_str(), 110.0f * s, H - 60.0f * s, 26.0f * s, ps2::Grey(0.8f));
	BuildBottomLines(ui, W, H);
}

// P2-19/P2-20b: the date and time in the field's corners, drawn after the field so the soft focus leaves them sharp.
void App::BuildSysconfHeader(std::vector<UiVertex>& ui, float W, float H)
{
	const float s = H / 1080.0f, head = m_field_head;
	// The date and time, as the PS2's corners (the clock with its little sun).
	ps2::Text(ui, *m_fonts, TodayText().c_str(), 110.0f * s, 104.0f * s, 38.0f * s, ps2::Detail(head));
	const float tw = ps2::Text(ui, *m_fonts, NowText().c_str(), W - 110.0f * s, 104.0f * s, 38.0f * s, ps2::Detail(head), Fonts::Right);
	{
		const float cx = W - 110.0f * s - tw - 34.0f * s, cy = 91.0f * s;
		ps2::Glow(ui, cx, cy, 9.0f * s, 9.0f * s, ps2::Detail(head), 4.0f);
		for (int k = 0; k < 8; k++)
		{
			const float a = k * kPi / 4.0f;
			ps2::Line(ui, {cx + std::cos(a) * 11.0f * s, cy + std::sin(a) * 11.0f * s}, {cx + std::cos(a) * 15.0f * s, cy + std::sin(a) * 15.0f * s},
				2.0f * s, ps2::Detail(head));
		}
	}
}

// ---- The sheets: System Configuration -------------------------------------------------------------------------------

// P2-19: the field, measured from footage of the real System Configuration (design notes, ps2/audit/p219/study.md). In 1080
// lines: the hub ~(800, 515) (the PS2's sits 44 % across its 4:3 picture, the words to its right), twelve rods from ~165 px out to
// ~480, cubes ~150 px on a zig-zag down the left from 11 to 7 o'clock, one per item, the focused one deep blue. On entry (as the
// PS2's, from its main menu): the rods grow from a third of their size on black in ~1.25 s, the indigo fades in 0.45..1.0 s, the
// cubes pop in 0.7..1.2 s, the words after. Everything else runs on the shelf's clock, so a move never restarts it.
float App::BuildSysconfField(FrameDesc& f, float W, float H, int cubes, int lit, float in)
{
	const float s = H / 1080.0f, t = static_cast<float>(m_time);
	// P2-23: back to Home the entry plays backwards: the words go, the cubes, the indigo to black, the rods shrink away.
	const double ex = SheetExit();
	const float te = ex >= 0.0 ? std::max(0.0f, static_cast<float>(kSheetOut - ex)) : static_cast<float>(m_time - m_sheet_open_time);
	std::vector<UiVertex>& ui = f.ui;
	// The marbled indigo (bg.frag: motes.w < 0), darker round the hub.
	SetupFog(f, 2, W, H);
	const Vec2 hub = {W * 0.5f - 160.0f * s, 515.0f * s};
	// The field's entry: on black first.
	const float bg_in = Smoothstep(0.45f, 1.0f, te);
	if (bg_in < 1.0f)
		Fonts::AddRoundedRect(ui, 0, 0, W, H, 0, ps2::Rgba(0, 0, 0, (1.0f - bg_in) * in));
	// P2-19e: the dark pool fills most of the screen, as measured in the footage (median brightness by distance from the hub,
	// 1080 lines: near black to ~300 px, then rising steadily: ~#1c1439 at 420, ~#271c48 at 540, ~#33214f at 660, ~#40306a at 900).
	ps2::Glow(ui, hub.x - 110.0f * s, hub.y + 30.0f * s, 1000.0f * s, 1000.0f * s, ps2::Rgba(0.0f, 0.0f, 0.01f, 0.92f * in), 3.4f); // (darker to the left)
	ps2::Glow(ui, hub.x, hub.y, 360.0f * s, 360.0f * s, ps2::Rgba(0.0f, 0.0f, 0.01f, 0.70f * in), 1.6f);
	const float grow = 1.0f - std::pow(1.0f - Clamp(te / 1.25f, 0.0f, 1.0f), 3.0f);
	std::time_t now = std::time(nullptr);
	std::tm tm = {};
	localtime_r(&now, &tm);
	ps2::ClockRods(ui, hub, 165.0f * s, t, tm.tm_hour % 12, 0.3f + 0.7f * grow, Smoothstep(0.0f, 0.35f, te) * in);
	// The main menu's lights, at home in the hub (the PS2 reuses them here). P2-23: the same lights on the same clock, gliding in
	// from Home's place and back.
	m_hub_orb[0] = hub.x;
	m_hub_orb[1] = hub.y;
	m_hub_orb[2] = 118.0f * s;
	{
		const float g = m_home_orb[0] < 0 || !m_sheet_over_home ? 1.0f : SheetLightsAt();
		float o[3];
		for (int i = 0; i < 3; i++)
			o[i] = m_hub_orb[i] + (m_home_orb[0] < 0 ? 0.0f : (m_home_orb[i] - m_hub_orb[i]) * (1.0f - g));
		ps2::Orb(ui, o[0], o[1], o[2], LightsTime(false), m_sheet_over_home ? 1.0f : in);
	}

	// The cubes: as measured for five (relative to the hub, 1080 lines), stretched along the same zig-zag for more.
	cubes = std::max(1, std::min(cubes, 8));
	{
		const float dt = m_cube_lit_time < 0 ? 1.0f : std::min(0.1f, static_cast<float>(m_time - m_cube_lit_time));
		m_cube_lit_time = m_time;
		const float a = 1.0f - std::exp(-dt * 11.0f); // ~0.3 s, as the PS2's blue moves
		for (int i = 0; i < 8; i++)
			m_cube_lit[i] += ((i == lit ? 1.0f : 0.0f) - m_cube_lit[i]) * a;
	}
	const float pop = Clamp((te - 0.7f) / 0.5f, 0.0f, 1.0f);
	const float pop_scale = pop >= 1.0f ? 1.0f : 1.0f + 2.2f * std::pow(pop - 1.0f, 3.0f) + 1.2f * std::pow(pop - 1.0f, 2.0f); // a little overshoot
	for (int i = 0; i < cubes; i++)
	{
		const float u = cubes == 1 ? 0.5f : static_cast<float>(i) / static_cast<float>(cubes - 1);
		const float near = (i % 2) == 0 ? 1.0f : 0.0f; // the zig-zag: by the hub, then out to the left
		const float x = hub.x + Mix(-450.0f, -180.0f, near) * s, y = hub.y + Mix(-262.0f, 305.0f, u) * s;
		const float size = (cubes <= 5 ? 138.0f : 138.0f * 4.0f / static_cast<float>(cubes - 1)) * s * Mix(1.05f, 0.95f, near) * pop_scale;
		const float period = 8.0f + 1.7f * static_cast<float>((i * 3) % 5); // each tumbles at its own pace
		const float ph = static_cast<float>(i) * 1.37f;
		// P2-19e: they tumble right over (the footage shows every face in turn), not just rock.
		ps2::GlassCube(ui, x, y, size, t * 2.0f * kPi / period + ph, t * 2.0f * kPi / (period * 1.6f) + ph * 2.3f,
			0.25f * std::sin(t * 2.0f * kPi / (period * 2.3f) + ph * 2.0f), m_cube_lit[i], Smoothstep(0.0f, 0.4f, pop) * in);
	}

	m_field_head = Smoothstep(1.0f, 1.3f, te) * in; // P2-20b: the corners are drawn over the soft focus (BuildSysconfHeader)
	return Smoothstep(1.0f, 1.3f, te);
}

void App::BuildSheetPs2(FrameDesc& f, float W, float H, const std::string& clock)
{
	(void)clock;
	const float s = H / 1080.0f, t = static_cast<float>(m_time);
	const float k = H / 2160.0f;
	SetupFog(f, 2, W, H);
	Ps2Camera(f, W, H);
	std::vector<UiVertex>& ui = f.ui;
	const float in = SheetExit() >= 0.0 ? 1.0f : Smoothstep(0.0f, 1.0f, m_sheet_anim); // P2-23: the way out is the field's own
	const bool global = m_sheet_global;
	const auto& rows = m_sheet.rows();
	const GameInfo* game = !global && !m_games.empty() ? &m_games[static_cast<size_t>(m_selected)] : nullptr;

	// The HD texture pack row focused with a pack on its way or in: the save-detail screen (A6): the field takes the cover's colour,
	// the case turns slowly on the left, in place of the cubes.
	bool pack_detail = false;
	float words = 1.0f; // P2-19: the words come in after the field
	if (!global && m_cfg.texture_packs && game && m_sheet_row >= 0 && m_sheet_row < static_cast<int>(rows.size()) &&
		rows[static_cast<size_t>(m_sheet_row)].kind == OptionsSheet::Kind::TexturePack)
	{
		using TS = TexturePackStatus::State;
		const TS st = m_cfg.texture_packs.status(game->serial).state;
		pack_detail = st == TS::Queued || st == TS::Checking || st == TS::Downloading || st == TS::Verifying || st == TS::Unpacking || st == TS::Installed;
	}
	if (pack_detail)
	{
		const Slot& sl = m_slots[static_cast<size_t>(m_selected)];
		const float* c = sl.has_glow ? sl.glow : m_glow;
		SetupFog(f, 3, W, H);
		f.bg.fog_lo[0] = c[0] * 0.06f;
		f.bg.fog_lo[1] = c[1] * 0.06f;
		f.bg.fog_lo[2] = c[2] * 0.08f;
		f.bg.fog_hi[0] = c[0] * 0.55f;
		f.bg.fog_hi[1] = c[1] * 0.55f;
		f.bg.fog_hi[2] = c[2] * 0.60f;
		f.bg.misc[1] = 0.35f;
		if (sl.set)
		{
			BoxDraw b;
			b.model = Ps2CaseModel(565.0f * s, 470.0f * s + Bob(t, 5.5f, 0.0f, 6.0f * s), 0.3f, 190.0f * s, t * 2.0f * kPi / 9.0f, Radians(8.0f), 0.0f, W, H);
			b.set = sl.set;
			b.cover_mix = sl.cover_mix;
			b.alpha = in;
			f.boxes.push_back(b);
		}
		f.bg.glow_color[0] = f.bg.glow_color[1] = f.bg.glow_color[2] = 1.0f;
		f.bg.glow_color[3] = 0.45f * in;
		f.bg.glow_pos[0] = 565.0f * s / W;
		f.bg.glow_pos[1] = 470.0f * s / H;
		f.bg.glow_pos[2] = f.bg.glow_pos[3] = 0.22f;
		ps2::Text(ui, *m_fonts, game->serial.c_str(), 110.0f * s, 104.0f * s, 34.0f * s, ps2::Detail(in));
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, m_sheet.title(), 40.0f * s, W * 0.5f).c_str(), W - 110.0f * s, 104.0f * s, 40.0f * s, ps2::Yellow(in),
			Fonts::Right);
	}
	else
	{
		// P2-19: both sheets stand in System Configuration's field (it was cubes and the case for a game, a wheel of flat
		// pillars for all games).
		// P2-20: a cube a section (the PS2's one an item), the focused section's lit.
		if (m_sheet.tab() != kTabAchievements)
			SheetSections();
		const bool sections = m_sheet.tab() != kTabAchievements && !m_sections.empty();
		words = BuildSysconfField(f, W, H, sections ? static_cast<int>(m_sections.size()) : 5, sections ? m_sheet_section : 0, in);
	}
	size_t words_begin = ui.size();
	// P2-19e: the field behind the words is softly out of focus (the post pass blurs it; the words are drawn sharp over it),
	// easing in with the words and out with the sheet. Not behind the texture pack's case (A6). P2-20b: in a section's table the
	// field dims and blurs further (eased ~0.25 s).
	{
		const float dt = m_table_time < 0 ? 1.0f : std::min(0.1f, static_cast<float>(m_time - m_table_time));
		m_table_time = m_time;
		const float want = m_sheet_level == 1 && m_sheet.tab() != kTabAchievements ? 1.0f : 0.0f;
		m_table_anim += (want - m_table_anim) * (1.0f - std::exp(-dt * 12.0f));
	}
	if (!pack_detail)
	{
		if (m_table_anim > 0.003f)
		{
			Fonts::AddRoundedRect(ui, 0, 0, W, H, 0, ps2::Rgba(0.0f, 0.0f, 0.01f, 0.45f * m_table_anim * in));
			words_begin = ui.size();
		}
		f.ui_scene = static_cast<uint32_t>(words_begin);
		f.blur = (0.55f + 0.35f * m_table_anim) * words * in;
		BuildSysconfHeader(ui, W, H);
	}

	// P2-20: System Configuration's words, as the real PS2 lays them out (in 1080 lines, on one centre line ~330 px right of the
	// screen's middle): the title in yellow at ~482, the item in cyan at ~597 with the up/down arrows pulsing at its right (a 2 s
	// cosine), its value under it at ~684. Only the focused item shows; a move fades the new one in over the old in place (~0.3 s),
	// as the PS2's "turning" list does. The sheet opens on its sections (the title is the game, or "Settings for all games"); in a
	// section the title is the section's name, the sheet's above it, small, and Left / Right arrows flank a value that changes.
	const float rx = W - 150.0f * s;
	float y = 250.0f * s;
	{
		// "Settings · Controls · Achievements" (L2 / R2), the shown one in cyan. (P2-20b: no This game / All games: a game's sheet is
		// that game's only, the settings for all games come from Home's System Configuration.)
		const char* pages[3] = {Tr(Str::HintSettings), Tr(Str::SheetControls), Tr(Str::Achievements)};
		const int page = m_sheet.tab() == kTabControls ? 1 : m_sheet.tab() == kTabAchievements ? 2 : 0;
		float x = W - 110.0f * s; // P2-20: under the clock, small (the PS2 has no pages)
		const float y = 160.0f * s;
		const float px = 24.0f * s, gap = 26.0f * s;
		for (int i = 2; i >= 0; i--)
		{
			x -= ps2::Text(ui, *m_fonts, pages[i], x, y, px, i == page ? ps2::Cyan(in) : ps2::Grey(in), Fonts::Right, i == page ? 0.5f : 0.0f);
			x -= gap;
		}
		ps2::Text(ui, *m_fonts, "L2 R2", x, y, px * 0.85f, ps2::Detail(0.7f * in), Fonts::Right);
	}
	if (m_sheet.tab() == kTabAchievements)
	{
		const float ax = W * 0.43f, aw = rx - ax;
		BuildGameAchievements(ui, ax, y, aw, H - y - 170.0f * s, k, ps2::Cyan());
		const ps2::Prompt p[] = {{icon::Circle, "Back"}, {icon::Triangle, Tr(Str::AchievementRefresh)}};
		ps2::Prompts(ui, *m_fonts, p, 2, W - 110.0f * s, H - 70.0f * s, 36.0f * s, true);
		FadeRange(ui, words_begin, ui.size(), words); // P2-19
		FadeRange(ui, 0, ui.size(), in);
		return;
	}
	if (m_picker.open)
	{
		const float px = W * 0.48f, pw = rx - px + 40.0f * s;
		Fonts::AddRoundedRect(ui, px - 10.0f * s, y - 10.0f * s, pw + 20.0f * s, H - y - 150.0f * s, 18.0f * s, ps2::Rgba(0.02f, 0.01f, 0.08f, 0.55f));
		BuildPicker(ui, px, pw, y - 150.0f * s, H - y + 10.0f * s, k * kPickerScale, ps2::Cyan()); // P2-12: larger
		if (m_picker.osk)
			BuildPickerKeyboard(ui, W, H, k * kPanelScale, ps2::Cyan()); // P2-12
		else
		{
			const ps2::Prompt p[] = {{icon::Cross, "Enter"}, {icon::Circle, "Back"}, {icon::Triangle, "Remove"}};
			ps2::Prompts(ui, *m_fonts, p, 3, W - 110.0f * s, H - 70.0f * s, 36.0f * s, true);
		}
		return;
	}

	const int focus = std::max(0, std::min(m_sheet_row, static_cast<int>(rows.size()) - 1));
	auto row_value = [&](const OptionsSheet::Row& r, uint32_t& vc, bool& busy_bar, float& frac) {
		std::string value = m_sheet.Value(r);
		vc = m_sheet.Source(r) == OptionsSheet::From::Own ? ps2::Cyan() : ps2::White();
		busy_bar = false;
		frac = 0;
		if (m_sheet.Armed(r, m_time))
			value = "Press again";
		else if (r.kind == OptionsSheet::Kind::ResetAll)
			value.clear();
		if (r.kind == OptionsSheet::Kind::OnlinePatches)
			value = OnlinePatchValue();
		if (r.kind == OptionsSheet::Kind::HomeTile)
		{
			value = HomeTileValue();
			using TP = gametile::Phase;
			const TP p = m_tile_view.phase;
			if (p == TP::Failed || p == TP::NoAnswer || p == TP::Stuck || p == TP::NoHelper)
				vc = ps2::Warn();
		}
		if (r.kind == OptionsSheet::Kind::SystemUpdate || r.kind == OptionsSheet::Kind::LogUpload)
			value = UpdateRowValue(r);
		if (r.kind == OptionsSheet::Kind::ReloadTile && m_cfg.reload_tile_on)
			value = Tr(m_cfg.reload_tile_on() ? Str::ReloadTileOn : Str::ReloadTileOff);
		if (r.kind == OptionsSheet::Kind::WebSettings) // P2-16: where the page is (it was Home's detail line)
			value = m_qr_size > 0 ? (m_web_shown.empty() ? m_web_url : m_web_shown) : std::string(Tr(Str::NoNetwork));
		if (r.kind == OptionsSheet::Kind::TexturePack && m_cfg.texture_packs && !m_games.empty())
		{
			const std::string& serial = m_games[static_cast<size_t>(m_selected)].serial;
			const TexturePackStatus pack = m_cfg.texture_packs.status(serial);
			const auto it = m_texpack_pick.find(serial);
			value = TexturePackValue(pack, it != m_texpack_pick.end() ? it->second : std::max(0, pack.job_pack));
			if (m_texpack_armed == serial && m_time <= m_texpack_armed_until)
				value = "Press again";
			using TS = TexturePackStatus::State;
			if (pack.state == TS::Failed || pack.state == TS::NeedSpace)
				vc = ps2::Warn();
			if ((pack.state == TS::Checking || pack.state == TS::Downloading || pack.state == TS::Unpacking) && pack.total > 0)
			{
				busy_bar = true;
				frac = Clamp(static_cast<float>(pack.done) / static_cast<float>(pack.total), 0.0f, 1.0f);
			}
		}
		std::string glyph, name;
		if (SplitSymbol(value, glyph, name))
			value = name; // the Controls page: the button's name (its glyph is drawn beside it)
		return value;
	};
	auto label_of = [&](const OptionsSheet::Row& r) {
		std::string glyph, name;
		return SplitSymbol(r.label, glyph, name) ? name : r.label;
	};
	const bool listing = m_sheet_level == 0;
	const SheetSection* sec = m_sections.empty() ? nullptr : &m_sections[static_cast<size_t>(std::clamp(m_sheet_section, 0, static_cast<int>(m_sections.size()) - 1))];
	const bool open_list = listing && sec && sec->rows.size() > 1; // the focus is a section to go into
	const float cx = W * 0.5f + 330.0f * s;
	const std::string heading = global || !game ? std::string("Settings for all games") : game->title;
	std::string label, value, help;
	uint32_t vc = ps2::Detail();
	bool bar = false, changes = false;
	float frac = 0;
	using K = OptionsSheet::Kind;
	if (open_list)
	{
		label = sec->name;
		value = sec->summary;
	}
	else if (focus < static_cast<int>(rows.size()) && rows[static_cast<size_t>(focus)].kind != K::Header)
	{
		const OptionsSheet::Row& r = rows[static_cast<size_t>(focus)];
		label = label_of(r);
		value = row_value(r, vc, bar, frac);
		if (r.kind == K::NewCard)
			value += "  \xC2\xB7  Create";
		if (value.empty() && (r.kind == K::Recommended || r.kind == K::ResetAll || r.kind == K::SystemMenu))
			value = "Press \xE2\x9C\x95";
		if (r.kind == K::GameFolders || r.kind == K::BiosFolder || r.kind == K::TexturesFolder || r.kind == K::NfsShares)
			value += "  \xE2\x80\xBA";
		vc = vc == ps2::White() ? ps2::Detail() : vc; // set here: cyan; followed or a default: the PS2's grey-white
		changes = r.kind == K::Option || r.kind == K::Card || r.kind == K::Patch || r.kind == K::Cheat || r.kind == K::ElfDisc ||
		          r.kind == K::CheatDisc || r.kind == K::TexturePack;
	}
	// What the focused row does.
	if (focus < static_cast<int>(rows.size()))
	{
		const OptionsSheet::Row& r = rows[static_cast<size_t>(focus)];
		help = m_sheet.Help(r);
		if (r.kind == OptionsSheet::Kind::TexturePack && m_cfg.texture_packs && !m_games.empty())
		{
			const std::string& serial = m_games[static_cast<size_t>(m_selected)].serial;
			const TexturePackStatus pack = m_cfg.texture_packs.status(serial);
			const auto it = m_texpack_pick.find(serial);
			help = TexturePackHelp(pack, it != m_texpack_pick.end() ? it->second : std::max(0, pack.job_pack), serial);
		}
		if (r.kind == OptionsSheet::Kind::HomeTile)
			help = HomeTileHelp();
		if (r.kind == OptionsSheet::Kind::SystemUpdate || r.kind == OptionsSheet::Kind::LogUpload)
			help = UpdateRowHelp(r);
		if (r.kind == OptionsSheet::Kind::ReloadTile)
			help = Tr(Str::ReloadTileHelp);
		if (r.kind == OptionsSheet::Kind::WebSettings) // P2-16
			help = "Cross: the settings page's QR code, full screen. Scan it with a phone or open it on a PC on the same network to "
			       "edit each game's settings, or the settings for all games, from there.";
	}
	if (open_list)
		help.clear();
	if (!listing && sec)
	{
		// P2-20b: inside a section, a plain table the sofa can read: the setting's name left, its value right, the focused row lit
		// (Left / Right change it, Circle goes back to the sections); the section's groups as small headings; the help for the focused
		// row under it. The field stays behind, dimmed and soft. The PS2's scroll arrows (P2-21) over and under it when it runs on.
		const float x0 = 300.0f * s, x1 = W - 300.0f * s, top = 320.0f * s, bottom = 850.0f * s;
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, heading, 26.0f * s, x1 - x0).c_str(), x0, 176.0f * s, 26.0f * s, ps2::Detail(0.85f * in));
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, sec->name, 48.0f * s, x1 - x0).c_str(), x0, 236.0f * s, 48.0f * s, ps2::Yellow(in));
		// Lay the rows out (a heading where the group changes), in 1080-line units from the table's top.
		std::vector<std::string> group_of(rows.size());
		{
			std::string g;
			for (size_t i = 0; i < rows.size(); i++)
			{
				if (rows[i].kind == K::Header)
					g = rows[i].label;
				group_of[i] = g;
			}
		}
		struct Item
		{
			int row; // -1: a heading
			float y; // its middle, from the table's top
			std::string text;
		};
		std::vector<Item> items;
		float yy = 0.0f, focus_y = 0.0f;
		std::string last;
		const bool many_groups = [&] {
			for (int r : sec->rows)
				if (group_of[static_cast<size_t>(r)] != group_of[static_cast<size_t>(sec->rows.front())])
					return true;
			return false;
		}();
		for (int r : sec->rows)
		{
			const std::string& g = group_of[static_cast<size_t>(r)];
			if (many_groups && g != last && !g.empty())
			{
				std::string up = g;
				for (char& c : up)
					c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
				items.push_back({-1, yy + 22.0f, up});
				yy += 48.0f;
			}
			last = g;
			if (r == focus)
				focus_y = yy + 31.0f;
			items.push_back({r, yy + 31.0f, {}});
			yy += 62.0f;
		}
		const float view = (bottom - top) / s;
		{
			// Keep the focused row two rows inside the window; eased (snapped on entering a section).
			const float want = Clamp(focus_y - view * 0.5f, 0.0f, std::max(0.0f, yy - view));
			if (m_table_section != m_sheet_section)
			{
				m_table_scroll = want;
				m_table_section = m_sheet_section;
			}
			const float dt = std::min(0.1f, static_cast<float>(m_time - m_table_scroll_time));
			m_table_scroll_time = m_time;
			m_table_scroll += (want - m_table_scroll) * (1.0f - std::exp(-dt * 14.0f));
		}
		for (const Item& it : items)
		{
			const float y_mid = top + (it.y - m_table_scroll) * s;
			const float edge = std::min(y_mid - top, bottom - y_mid) / (30.0f * s);
			const float a = Clamp(edge + 0.3f, 0.0f, 1.0f) * in; // gone just past the window's edge
			if (a <= 0.01f)
				continue;
			if (it.row < 0)
			{
				ps2::Text(ui, *m_fonts, it.text.c_str(), x0 + 10.0f * s, y_mid + 8.0f * s, 22.0f * s, ps2::Rgba(0.67f, 0.69f, 0.80f, 0.75f * a));
				continue;
			}
			const OptionsSheet::Row& r = rows[static_cast<size_t>(it.row)];
			const bool on = it.row == focus;
			uint32_t rc;
			bool rbar;
			float rfrac;
			std::string v = row_value(r, rc, rbar, rfrac);
			if (r.kind == K::NewCard)
				v += "  \xC2\xB7  Create";
			if (v.empty() && (r.kind == K::Recommended || r.kind == K::ResetAll || r.kind == K::SystemMenu))
				v = on ? "Press \xE2\x9C\x95" : "";
			if (r.kind == K::GameFolders || r.kind == K::BiosFolder || r.kind == K::TexturesFolder || r.kind == K::NfsShares)
				v += "  \xE2\x80\xBA";
			const uint32_t vcol = rc == ps2::White() ? ps2::Detail(a) : ps2::Alpha(rc, a);
			if (on)
			{
				Fonts::AddRoundedRect(ui, x0 - 20.0f * s, y_mid - 29.0f * s, x1 - x0 + 40.0f * s, 58.0f * s, 8.0f * s, ps2::Rgba(0.22f, 0.42f, 0.95f, 0.30f * a));
				Fonts::AddRoundedRect(ui, x0 - 20.0f * s, y_mid - 29.0f * s, 5.0f * s, 58.0f * s, 2.0f * s, ps2::Cyan(0.9f * a));
			}
			std::string glyph, name;
			float nx = x0 + 10.0f * s;
			if (SplitSymbol(r.label, glyph, name) && glyph != icon::Blank)
			{
				m_fonts->AddText(ui, glyph.c_str(), nx + 18.0f * s, y_mid + 12.0f * s, 40.0f * s, on ? ps2::White(a) : ps2::Detail(a), 0.2f, Fonts::Center);
				nx += 52.0f * s;
			}
			const float vx = x1 - (on && changes ? 40.0f : 10.0f) * s;
			const float vw = ps2::Text(ui, *m_fonts, Fit(*m_fonts, v, 34.0f * s, (x1 - x0) * 0.45f).c_str(), vx, y_mid + 12.0f * s, 34.0f * s, vcol, Fonts::Right);
			ps2::Text(ui, *m_fonts, Fit(*m_fonts, label_of(r), 34.0f * s, vx - vw - nx - 40.0f * s).c_str(), nx, y_mid + 12.0f * s, 34.0f * s,
				on ? ps2::White(a) : ps2::Rgba(0.80f, 0.81f, 0.86f, a), Fonts::Left, on ? 0.35f : 0.0f);
			if (on && changes) // Left / Right
			{
				const float h = 10.0f * s, ay = y_mid;
				const uint32_t ac = ps2::Rgba(0.86f, 0.88f, 0.94f, 0.95f * a);
				const float lx = vx - vw - 18.0f * s, rx2 = x1 - 14.0f * s;
				ps2::Tri(ui, {lx - h * 1.3f, ay}, {lx, ay - h}, {lx, ay + h}, ac);
				ps2::Tri(ui, {rx2, ay}, {rx2 - h * 1.3f, ay + h}, {rx2 - h * 1.3f, ay - h}, ac);
			}
		}
		// P2-21's arrows: solid blue, over and under the table while it runs on.
		auto arrow = [&](float cy, float w, float h, bool up) {
			const float ax = (x0 + x1) * 0.5f, base = cy + (up ? h : -h) * 0.5f * s, tip = cy - (up ? h : -h) * 0.5f * s;
			ps2::Tri(ui, {ax - w * 0.5f * s, base}, {ax + w * 0.5f * s, base}, {ax, tip}, ps2::Rgba(0.08f, 0.23f, 0.66f, in));
			ui.back().color = ps2::Rgba(0.02f, 0.12f, 0.54f, in);
		};
		if (m_table_scroll > 4.0f)
			arrow(top - 26.0f * s, 33.0f, 21.0f, true);
		if (m_table_scroll < yy - view - 4.0f)
			arrow(bottom + 30.0f * s, 48.0f, 36.0f, false);
		float hy = bottom + 76.0f * s;
		if (bar)
		{
			ps2::Bar(ui, x0, hy - 20.0f * s, x1 - x0, frac, s, in);
			hy += 20.0f * s;
		}
		for (const std::string& line : Wrap(*m_fonts, help, 27.0f * s, x1 - x0, 2))
		{
			ps2::Text(ui, *m_fonts, line.c_str(), x0, hy, 27.0f * s, ps2::Rgba(0.72f, 0.73f, 0.80f, in));
			hy += 36.0f * s;
		}
	}
	else
	{
	ps2::Text(ui, *m_fonts, Fit(*m_fonts, heading, 50.0f * s, 900.0f * s).c_str(), cx, 482.0f * s, 50.0f * s, ps2::Yellow(in), Fonts::Center);
	// The turning list: the item before fades out where the new one fades in. A value that steps fades the same way (not the
	// ones that count up on their own: a download's progress).
	if (m_xf_label[0] != label)
	{
		m_xf_label[1] = m_xf_label[0];
		m_xf_label[0] = label;
		m_xf_value[1] = m_xf_value[0];
		m_xf_value[0] = value;
		m_xf_label_time = m_xf_value_time = m_time;
	}
	else if (m_xf_value[0] != value)
	{
		m_xf_value[1] = changes && !bar ? m_xf_value[0] : std::string();
		m_xf_value[0] = value;
		m_xf_value_time = changes && !bar ? m_time : -1.0;
	}
	const float ul = Smoothstep(0.0f, 0.3f, static_cast<float>(m_time - m_xf_label_time));
	const float uv = Smoothstep(0.0f, 0.3f, static_cast<float>(m_time - m_xf_value_time));
	const float label_px = 50.0f * s, value_px = (open_list ? 36.0f : 46.0f) * s, words_w = 760.0f * s; // (a section's summary smaller)
	if (ul < 1.0f && !m_xf_label[1].empty())
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, m_xf_label[1], label_px, words_w).c_str(), cx, 597.0f * s, label_px, ps2::Cyan((1.0f - ul) * in), Fonts::Center);
	ps2::Text(ui, *m_fonts, Fit(*m_fonts, label, label_px, words_w).c_str(), cx, 597.0f * s, label_px, ps2::Cyan(ul * in), Fonts::Center, 0.6f * ul);
	if (uv < 1.0f && !m_xf_value[1].empty())
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, m_xf_value[1], value_px, words_w).c_str(), cx, 684.0f * s, value_px, ps2::Alpha(vc, (1.0f - uv) * in), Fonts::Center);
	const float vw = ps2::Text(ui, *m_fonts, Fit(*m_fonts, value, value_px, words_w).c_str(), cx, 684.0f * s, value_px, ps2::Alpha(vc, uv * in), Fonts::Center);
	if (changes && !listing) // Left / Right change it
	{
		const float ax = vw * 0.5f + 30.0f * s, ay = 668.0f * s, h = 11.0f * s;
		const uint32_t ac = ps2::Rgba(0.86f, 0.88f, 0.94f, 0.9f * in);
		ps2::Tri(ui, {cx - ax, ay}, {cx - ax + h * 1.3f, ay - h}, {cx - ax + h * 1.3f, ay + h}, ac);
		ps2::Tri(ui, {cx + ax, ay}, {cx + ax - h * 1.3f, ay + h}, {cx + ax - h * 1.3f, ay - h}, ac);
	}
	{
		// The up/down arrows: there is more above or below (the PS2's pulse, a 2 s cosine, nearly out at its low).
		const int at = listing ? m_sheet_section : (sec ? static_cast<int>(std::find(sec->rows.begin(), sec->rows.end(), focus) - sec->rows.begin()) : 0);
		const int count = listing ? static_cast<int>(m_sections.size()) : (sec ? static_cast<int>(sec->rows.size()) : 1);
		const float pulse = 0.12f + 0.88f * (0.5f + 0.5f * std::cos(static_cast<float>(m_time) * kPi));
		const float ax = cx + 430.0f * s, h = 11.0f * s;
		if (at > 0)
			ps2::Tri(ui, {ax - h * 1.4f, 572.0f * s}, {ax + h * 1.4f, 572.0f * s}, {ax, 572.0f * s - h * 1.5f}, ps2::Rgba(0.80f, 0.82f, 0.92f, pulse * in));
		if (at < count - 1)
			ps2::Tri(ui, {ax - h * 1.4f, 586.0f * s}, {ax, 586.0f * s + h * 1.5f}, {ax + h * 1.4f, 586.0f * s}, ps2::Rgba(0.80f, 0.82f, 0.92f, pulse * in));
		if (count > 1) // where in the list: small, under the arrows
			ps2::Text(ui, *m_fonts, (std::to_string(at + 1) + " / " + std::to_string(count)).c_str(), ax, 632.0f * s, 22.0f * s, ps2::Grey(0.8f * in), Fonts::Center);
	}
	float hy = 684.0f * s + 30.0f * s;
	if (bar)
	{
		ps2::Bar(ui, cx - 350.0f * s, hy, 700.0f * s, frac, s, in);
		hy += 30.0f * s;
	}
	if (!help.empty())
	{
		const float help_w = 860.0f * s;
		hy += 52.0f * s;
		for (const std::string& line : Wrap(*m_fonts, help, 27.0f * s, help_w, 4))
		{
			ps2::Text(ui, *m_fonts, line.c_str(), cx, hy, 27.0f * s, ps2::Rgba(0.70f, 0.71f, 0.78f, in), Fonts::Center);
			hy += 37.0f * s;
		}
	}
	}
	if (!m_sheet_status.empty() && m_time - m_sheet_status_time < 3.0)
	{
		const float a = 1.0f - Smoothstep(2.2f, 3.0f, static_cast<float>(m_time - m_sheet_status_time));
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, m_sheet_status, 26.0f * s, W * 0.5f).c_str(), 110.0f * s, H - 140.0f * s, 26.0f * s, ps2::Cyan(a));
	}


	// The prompts say what the buttons do on this row.
	const char* cross = "Change";
	const char* tri = "Reset";
	if (focus < static_cast<int>(rows.size()))
	{
		const OptionsSheet::Row& r = rows[static_cast<size_t>(focus)];
		using K = OptionsSheet::Kind;
		switch (r.kind)
		{
			case K::HomeTile:
				cross = m_tile_view.phase == gametile::Phase::On ? "Remove" : "Add";
				tri = "Remove";
				break;
			case K::GameFolders:
			case K::BiosFolder:
			case K::TexturesFolder:
			case K::NfsShares: cross = "Open"; break;
			case K::TexturePack:
			{
				cross = "Download";
				tri = "Cancel";
				if (m_cfg.texture_packs && !m_games.empty())
				{
					using TS = TexturePackStatus::State;
					const TexturePackStatus st = m_cfg.texture_packs.status(m_games[static_cast<size_t>(m_selected)].serial);
					if (st.state == TS::Installed && st.ours)
						tri = "Delete";
				}
				break;
			}
			case K::Recommended:
			case K::ResetAll:
			case K::SystemMenu:
			case K::NewCard: cross = "Select"; tri = nullptr; break;
			case K::OnlinePatches: cross = "Get"; tri = nullptr; break;
			case K::SystemUpdate: cross = "Check"; tri = nullptr; break;
			case K::LogUpload:
			case K::ReloadTile: cross = "Switch"; tri = nullptr; break;
			case K::WebSettings: cross = "Open"; tri = nullptr; break; // P2-16
			default: break;
		}
	}
	if (m_sheet_level == 0 && !m_sections.empty() && m_sections[static_cast<size_t>(std::clamp(m_sheet_section, 0, static_cast<int>(m_sections.size()) - 1))].rows.size() > 1)
	{
		cross = "Enter"; // P2-20: into the section
		tri = nullptr;
	}
	// P2-20: from the left, as System Configuration's.
	ps2::Prompt p[3] = {{icon::Cross, cross}, {icon::Circle, "Back"}, {icon::Triangle, tri ? tri : ""}};
	ps2::Prompts(ui, *m_fonts, p, tri ? 3 : 2, 110.0f * s, H - 70.0f * s, 36.0f * s, false);
	FadeRange(ui, words_begin, ui.size(), words); // P2-19
	FadeRange(ui, 0, ui.size(), in);
}

// ---- The disc in the drive ------------------------------------------------------------------------------------------

void App::BuildDiscAskPs2(FrameDesc& f, float W, float H)
{
	const float s = H / 1080.0f;
	const float e = Smoothstep(0.0f, 1.0f, m_ask_anim);
	SetupFog(f, 3, W, H);
	std::vector<UiVertex>& ui = f.ui;
	ui.clear();
	f.boxes.clear();
	m_achievement_images.clear(); // (the texture ranges of what was drawn before point into the vertices just dropped)
	const float cx = 1450.0f * s, cy = 520.0f * s, r = 140.0f * s;
	f.bg.glow2[0] = cx / W;
	f.bg.glow2[1] = cy / H;
	f.bg.glow2[2] = 0.26f;
	f.bg.glow2[3] = 0.9f * e;
	f.bg.glow_color[0] = f.bg.glow_color[1] = f.bg.glow_color[2] = 1.0f;
	f.bg.glow_color[3] = 0.5f * e;
	f.bg.glow_pos[0] = cx / W;
	f.bg.glow_pos[1] = cy / H;
	f.bg.glow_pos[2] = f.bg.glow_pos[3] = 0.15f;
	const float ca = std::cos(Radians(-8.0f)), sa = std::sin(Radians(-8.0f)), squash = std::cos(Radians(58.0f));
	ps2::Disc(ui, cx, cy, {r * ca, r * sa}, {-r * sa * squash, r * ca * squash}, m_dump_spin * 2.0f, 1.0f, e);
	ps2::Text(ui, *m_fonts, "PS5SX2", 110.0f * s, 128.0f * s, 40.0f * s, ps2::White(e));
	ps2::Text(ui, *m_fonts, Fit(*m_fonts, m_ask_title, 48.0f * s, W * 0.6f).c_str(), W - 110.0f * s, 132.0f * s, 48.0f * s, ps2::Yellow(e), Fonts::Right);
	const std::string detail = "PS2 disc in the drive" + (m_ask_serial.empty() ? std::string() : "  \xC2\xB7  " + m_ask_serial);
	ps2::Text(ui, *m_fonts, detail.c_str(), W - 110.0f * s, 180.0f * s, 30.0f * s, ps2::Detail(e), Fonts::Right);
	const bool play = m_ask_sel == 0;
	ps2::Text(ui, *m_fonts, "Play from the disc", W - 150.0f * s, 740.0f * s, play ? 50.0f * s : 40.0f * s, play ? ps2::Cyan(e) : ps2::Grey(e),
		Fonts::Right, play ? 1.0f : 0.0f);
	ps2::Text(ui, *m_fonts, "Copy to the library first", W - 150.0f * s, 804.0f * s, play ? 40.0f * s : 50.0f * s, play ? ps2::Grey(e) : ps2::Cyan(e),
		Fonts::Right, play ? 0.0f : 1.0f);
	ps2::Text(ui, *m_fonts,
		play ? "Starts right now. The disc stays in the drive." : "A few minutes, then it plays. A copy plays without the drive and stays in the library.",
		W - 150.0f * s, 860.0f * s, 26.0f * s, ps2::Rgba(0.68f, 0.70f, 0.75f, e), Fonts::Right);
	const ps2::Prompt prompts[] = {{icon::Cross, "Enter"}, {icon::Circle, "Not now"}};
	ps2::Prompts(ui, *m_fonts, prompts, 2, W - 110.0f * s, H - 110.0f * s, 38.0f * s, true);
}

// ---- L1 + R1 + Touch: back to the shelf through the boot towers -----------------------------------------------------

void App::BuildReturn(FrameDesc& f, float W, float H)
{
	const float s = H / 1080.0f;
	const float left = static_cast<float>(m_return_until - m_time);
	const float age = 2.4f - left;
	SetupFog(f, 4, W, H);
	std::vector<UiVertex>& ui = f.ui;
	f.boxes.clear();
	const float rise = Smoothstep(0.0f, 1.1f, age), out = Smoothstep(0.0f, 0.5f, left);
	// The towers, seen from above as the PS2 shows them at boot, taller for more time played; lit faces towards the middle.
	const float cx = W * 0.5f, cy = H * 0.5f + 10.0f * s, fl = 760.0f * s, cam = 11.0f, rot = static_cast<float>(m_time) * 0.04f;
	const float cs = std::cos(rot), sn = std::sin(rot);
	struct P
	{
		float x, z, h;
	};
	P items[64];
	int n = 0;
	for (const Tower& tw : m_towers)
	{
		if (n >= 64)
			break;
		items[n++] = {tw.x * cs - tw.z * sn, tw.x * sn + tw.z * cs, tw.h * rise};
	}
	std::sort(items, items + n, [](const P& a, const P& b) { return a.h < b.h; });
	auto proj = [&](float x, float y, float z) {
		const float kk = fl / (cam - y);
		return Vec2{cx + x * kk, cy + z * kk};
	};
	for (int i = 0; i < n; i++)
	{
		const P& o = items[i];
		const float sz = 0.24f;
		const float c4[4][2] = {{-sz, -sz}, {sz, -sz}, {sz, sz}, {-sz, sz}};
		Vec2 bot[4], top[4];
		for (int q = 0; q < 4; q++)
		{
			bot[q] = proj(o.x + c4[q][0], 0.0f, o.z + c4[q][1]);
			top[q] = proj(o.x + c4[q][0], o.h, o.z + c4[q][1]);
		}
		const float dist = std::sqrt(o.x * o.x + o.z * o.z) + 1e-4f, light = std::min(1.0f, o.h / 3.0f);
		for (int e = 0; e < 4; e++)
		{
			const int nx = (e + 1) % 4;
			const float mx = (c4[e][0] + c4[nx][0]) * 0.5f, mz = (c4[e][1] + c4[nx][1]) * 0.5f;
			const float facing = -(mx * o.x + mz * o.z) / (sz * dist);
			if (facing < -0.05f)
				continue; // seen from above the middle, only the sides that face the middle show
			const float lum = (40.0f + 60.0f * std::max(0.0f, facing) + 50.0f * light) / 255.0f;
			ps2::Quad(ui, bot[e], bot[nx], top[nx], top[e], ps2::Rgba(lum * 0.85f, lum * 0.9f, std::min(1.0f, lum * 1.15f), 0.92f * out));
		}
		const float tl = (150.0f + 90.0f * light) / 255.0f;
		ps2::Quad(ui, top[0], top[1], top[2], top[3], ps2::Rgba(tl * 0.9f, tl * 0.95f, std::min(1.0f, tl * 1.1f), out));
		for (int e = 0; e < 4; e++)
			ps2::Line(ui, top[e], top[(e + 1) % 4], 1.2f, ps2::Rgba(0.86f, 0.92f, 1.0f, 0.35f * out));
	}
	// The startup arc of lights, sweeping once.
	const float ph = Clamp((age - 0.6f) / 1.4f, 0.0f, 1.0f) * 1.4f;
	for (int i = 0; i < 9; i++)
	{
		const float u = i / 8.0f, a = -1.9f + u * 2.2f + ph * 1.2f;
		const float px = cx + std::cos(a) * 250.0f * s, py = cy - 150.0f * s + std::sin(a) * 90.0f * s - 60.0f * s * u; // P2-10: above the words
		const float al = std::max(0.0f, 1.0f - std::fabs(ph - 0.6f) * 1.2f) * (0.35f + 0.65f * u) * out;
		ps2::Glow(ui, px, py, 26.0f * s, 26.0f * s, ps2::Rgba(0.86f, 0.96f, 1.0f, al), 2.2f);
	}
	const float ta = Smoothstep(0.3f, 0.9f, age) * out;
	// P2-10: the arc of lights sweeps above the words (it ran through them), and a soft shadow keeps them clear of the towers.
	ps2::Glow(ui, W * 0.5f, H * 0.5f + 10.0f * s, 520.0f * s, 120.0f * s, ps2::Rgba(0.0f, 0.0f, 0.03f, 0.75f * ta), 1.6f);
	ps2::Text(ui, *m_fonts, "Back to the shelf", W * 0.5f, H * 0.5f, 56.0f * s, ps2::White(ta), Fonts::Center);
	if (!m_games.empty())
		ps2::Text(ui, *m_fonts, Fit(*m_fonts, m_games[static_cast<size_t>(m_selected)].title, 30.0f * s, W * 0.7f).c_str(), W * 0.5f, H * 0.5f + 64.0f * s,
			30.0f * s, ps2::Rgba(0.67f, 0.69f, 0.78f, ta), Fonts::Center);
	ps2::Text(ui, *m_fonts, "L1 + R1 + Touch", W - 110.0f * s, H - 70.0f * s, 28.0f * s, ps2::Rgba(0.49f, 0.53f, 0.64f, out), Fonts::Right);
}
} // namespace fe
