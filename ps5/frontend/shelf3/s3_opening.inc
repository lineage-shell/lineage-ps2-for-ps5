// PS2 shelf v3 (AI-assisted): the PS2's opening, drawn by the shelf from our spec (ps2-own spec/boot.md 2: the camera path, the fog
// layers, the four lights with their trails, the five floating cubes, the SCE logo, the history towers from the play time, the resample
// blur and the fade), with BOOTS and TNNLS from the BIOS's own instruments; then the main menu fades in, in the same process (round 2:
// the BIOS run through the emulator left seconds of black while the app restarted). Included at the end of s3_app.inc.
// SPDX-License-Identifier: GPL-3.0-or-later

namespace fe
{
namespace
{
constexpr float kOpK = 1.2f; // the camera path's PAL time scale (boot.md 2.2)

struct S3OpCam
{
	float P[3];
	float R;
};
// World -> opening camera: from P, looking along +z, rolled by R.
void S3OpToCam(const S3OpCam& c, float x, float y, float z, float& cx, float& cy, float& cz)
{
	const float dx = x - c.P[0], dy = y - c.P[1], dz = z - c.P[2];
	const float co = std::cos(c.R), si = std::sin(c.R);
	cx = dx * co - dy * si, cy = dx * si + dy * co, cz = dz;
}
// Field px of a camera-space point (x 1024, y 538.902 = 1024 x 0.526271).
void S3OpProj(float cx, float cy, float cz, float& fx, float& fy)
{
	const float z = std::max(cz, 0.5f);
	fx = 320.0f + 1024.0f * cx / z, fy = 128.0f + 538.902f * cy / z;
}

// The light map of the towers (2.8): 20 x 20, built from two soft lights and a fixed integer pattern.
float S3TowerLight(int j, int i)
{
	static float M[20][20];
	static bool init = false;
	if (!init)
	{
		const float R = std::sqrt(5202.0f);
		for (int ii = 0; ii < 20; ii++)
			for (int jj = 0; jj < 20; jj++)
			{
				const float gx = 5.1f * (jj - 10) + 2.55f, gy = 5.1f * (ii - 10) + 2.55f;
				const float d1 = std::hypot(-5.1f - gx, 0.0f - gy);
				float v = std::clamp((R - 2 * d1) * 255.0f / R, 32.0f, 255.0f);
				const float d2 = std::hypot(10.2f - gx, 5.1f - gy);
				const float w = (R - 4 * d2) * 255.0f / R / 2.0f;
				v += std::clamp(w, 32.0f, 255.0f);
				v = 0.85f * v - 10.0f * ((((ii + jj) * jj) / (ii + 1)) % 11 - 5);
				M[jj][ii] = std::clamp(v, 32.0f, 220.0f);
			}
		init = true;
	}
	return M[std::clamp(j, 0, 19)][std::clamp(i, 0, 19)];
}
} // namespace

void App::S3OpeningStart()
{
	s3::Shelf3& S = m_s3;
	S.scr = s3::Scr::Opening;
	S.op_cnt = 0, S.op_stage = 0, S.op_logo = 0, S.op_logo_dir = 0, S.op_skip = 0;
	S.op_P[0] = S.op_P[1] = 0, S.op_P[2] = 16;
	S.op_V[0] = S.op_V[1] = 0, S.op_V[2] = 0.04f;
	for (int k = 0; k < 3; k++)
		S.op_A[k] = S.op_J[k] = 0;
	S.op_R = -0.12f, S.op_W = 0.001f, S.op_Q = 0;
	S.op_Pp[0] = S.op_Pp[1] = 0, S.op_Pp[2] = 16, S.op_Rp = S.op_R;
	for (float* c : S.op_Ch)
		c[0] = c[1] = 0, c[2] = 16, c[3] = S.op_R;
	for (float& v : S.op_scroll)
		v = 0;
	S.op_trail_n = 0, S.op_trail_w = 0;
	// The towers' source, as the BIOS's: the card's history file (B?DATA-SYSTEM/history, 21 x 22 bytes).
	S.history.clear(), S.history_used.clear();
	if (!S.card.ok())
		S3ReadCard();
	for (const s3::Save& sv : S.card.saves())
		if (sv.dir.size() == 13 && sv.dir[0] == 'B' && sv.dir.compare(2, 11, "DATA-SYSTEM") == 0)
		{
			std::vector<s3::MemoryCard::SaveFile> files;
			if (!S.card.ReadSaveFiles(sv.dir, files))
				continue;
			for (const s3::MemoryCard::SaveFile& fl : files)
				if (fl.name == "history" && fl.data.size() >= 21 * 22)
					for (int i = 0; i < 21; i++)
					{
						const uint8_t* e = fl.data.data() + i * 22;
						s3::MemoryCard::HistoryEntry h;
						h.id.assign(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 16));
						h.count = e[0x10], h.mask = e[0x11] & 0x3F, h.cell = e[0x12];
						S.history.push_back(h);
						S.history_used.push_back(!h.id.empty());
					}
			break;
		}
	if (S.clocks_ok)
		S.boots.Start(); // BOOTS: the opening's music (sound.md 4), from the first frame
	std::printf("[shelf3] the opening (drawn from the spec), then the main menu\n");
}

// Light i's place at scene counter n (2.6); n may fall between ticks.
static void S3OpLight(int i, float cnt, float* w)
{
	const float n = cnt + 4002.0f;
	const float s = std::cos((n + 17 * i) * 0.01f * (i + 10) * 0.1f), c = std::sin((n + 15 * i) * 0.005f * (i + 10) * 0.1f);
	w[0] = (10 - i) * s, w[1] = (i + 3) * c, w[2] = 12 * s + 88;
}

void App::S3OpeningTick(const Input& in)
{
	(void)in;
	s3::Shelf3& S = m_s3;
	for (int k = 0; k < 3; k++)
		S.op_Pp[k] = S.op_P[k];
	S.op_Rp = S.op_R;
	const S3Pad& p = g_s3pad;
	// Any button hurries it: a short fade to black, then the menu (the BIOS's own can't be skipped; ours can).
	if (!S.op_skip && (p.now & ~p.prev))
		S.op_skip = 1;
	if (S.op_skip)
		S.op_skip++;
	// The camera path (2.2): V += (2A + J)k/2; A += Jk; W += Qk; R += (2W + Q)k/2; P += (2V + A)k/2.
	if (S.op_stage == 0 && S.op_P[2] > 16)
		S.op_stage = 1;
	if (S.op_stage == 1)
		S.op_J[2] = 4.0e-7f;
	if (S.op_stage == 1 && S.op_cnt > 200) // the disc type known (no disc: at cnt 200 in the reference)
	{
		S.op_stage = 2;
		if (S.clocks_ok)
			S.tnnls.Start(); // TNNLS: stage 2 with no disc (boot.md 7)
	}
	if (S.op_stage == 2)
		S.op_A[2] = 0.0099f, S.op_Q = 0.000195f;
	for (int k = 0; k < 3; k++)
	{
		S.op_V[k] += (2 * S.op_A[k] + S.op_J[k]) * kOpK / 2;
		S.op_A[k] += S.op_J[k] * kOpK;
	}
	S.op_W += S.op_Q * kOpK;
	S.op_R += (2 * S.op_W + S.op_Q) * kOpK / 2;
	S.op_R = std::remainder(S.op_R, 2 * kPi_);
	for (int k = 0; k < 3; k++)
		S.op_P[k] += (2 * S.op_V[k] + S.op_A[k]) * kOpK / 2;
	for (int j = 4; j > 0; j--)
		std::copy(S.op_Ch[j - 1], S.op_Ch[j - 1] + 4, S.op_Ch[j]);
	S.op_Ch[0][0] = S.op_P[0], S.op_Ch[0][1] = S.op_P[1], S.op_Ch[0][2] = S.op_P[2], S.op_Ch[0][3] = S.op_R;
	// The fog's scroll (2.7) and the logo (2.4): from the first frame with z > 18, alpha +4 a frame to 240, then back.
	for (int L = 0; L < 6; L++)
	{
		S.op_scroll[L] += (14 - L) * 1e-4f * (L + 1) * 0.5f;
		if (S.op_scroll[L] > 1.0f)
			S.op_scroll[L] -= 1.0f;
	}
	if (S.op_logo_dir == 0 && S.op_P[2] > 18)
		S.op_logo_dir = 1, (std::getenv("S3_TIMING") ? std::fprintf(stderr, "[s3t] logo starts at cnt %d (z %.3f)\n", S.op_cnt, S.op_P[2]) : 0);
	if (S.op_logo_dir == 1 && (S.op_logo += 4) >= 240)
		S.op_logo = 240, S.op_logo_dir = -1;
	else if (S.op_logo_dir == -1 && S.op_logo > 0)
		S.op_logo = std::max(0, S.op_logo - 4);
	S.op_cnt++;
	// The lights' trail ring (2.6): one sample a frame (a 50 Hz tick, not a drawn frame), through this tick's camera.
	if (S.op_P[2] < (S.wide ? 104.0f : 73.0f)) // (16:9: the lights go on to the end, so their trails do too)
	{
		const S3OpCam cam = {{S.op_P[0], S.op_P[1], S.op_P[2]}, S.op_R};
		const int slot = S.op_trail_w % 128;
		for (int i = 0; i < 4; i++)
		{
			float w[3], cx, cy, cz;
			S3OpLight(i, static_cast<float>(S.op_cnt), w);
			S3OpToCam(cam, w[0], w[1], w[2], cx, cy, cz);
			S3OpProj(cx, cy, cz, S.op_trail[i][slot][0], S.op_trail[i][slot][1]);
			S.op_trail[i][slot][2] = cz;
		}
		S.op_trail_w++;
		S.op_trail_n = std::min(S.op_trail_n + 1, 128);
	}
	// The end: z > 104 (black by then), or a skip's fade done: the main menu's own entry (128 frames from black, orbs flying in).
	if (S.op_P[2] > 104 || S.op_skip > 20)
	{
		S.scr = s3::Scr::Menu;
		S.menu_mode = 2, S.menu_c = 0, S.menu_d = -100, S.menu_words = 0, S.menu_spot = 0;
		S.menu_wait = S.op_skip ? 0 : 25; // (mainmenu.md 7: the menu loop starts 23 frames after the hand-off, c = 1 two frames later)
		S.orb_all_fly = true;
		if (S.clocks_ok)
			S.tnnls.Stop(); // as soon as the main menu starts (sound.md 4)
		if (S.open_game_after) // back from a game: the opening, then that game's page (as before, without the old towers)
		{
			S.open_game_after = false;
			S.scr = s3::Scr::Library;
			S3GameOpen(m_selected);
		}
	}
}

void App::S3OpeningBuild(FrameDesc& f)
{
	s3::Shelf3& S = m_s3;
	s3::Kit& kit = S.kit;
	// Drawn between the last two ticks (fr), so the flight is smooth at 60 or 120 Hz; nc = the scene counter at that point.
	const float fr = S.fr;
	const float nc = static_cast<float>(S.op_cnt - 1) + fr;
	const S3OpCam cam = {{S.op_Pp[0] + (S.op_P[0] - S.op_Pp[0]) * fr, S.op_Pp[1] + (S.op_P[1] - S.op_Pp[1]) * fr, S.op_Pp[2] + (S.op_P[2] - S.op_Pp[2]) * fr},
		S.op_Rp + std::remainder(S.op_R - S.op_Rp, 2 * kPi_) * fr};
	const float z = cam.P[2];
	kit.FillPx(0, 0, kit.W(), kit.H(), 0, 0, 0, 1);

	// ---- the towers (2.8): a tower per game of the play time, from PCSX2's playtime.dat (the 21 most played) ----
	const bool card_history = std::find(S.history_used.begin(), S.history_used.end(), true) != S.history_used.end();
	if (card_history || !S.played.empty() || (S.demo_towers && !m_games.empty()))
	{
		static const int kCells[21][6][2] = {{{0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, 0}, {1, 1}}, {{1, 2}, {2, 0}, {2, 1}, {2, 2}, {3, 0}, {4, 0}},
			{{4, 1}, {5, 0}, {5, 1}, {6, 0}, {6, 1}, {6, 2}}, {{7, 0}, {7, 1}, {8, 0}, {8, 1}, {8, 2}, {8, 3}},
			{{9, 0}, {9, 1}, {10, 0}, {10, 1}, {10, 2}, {11, 0}}, {{12, 0}, {12, 1}, {13, 0}, {13, 1}, {13, 2}, {13, 3}},
			{{1, 3}, {2, 3}, {3, 1}, {3, 2}, {3, 3}, {4, 2}}, {{5, 2}, {5, 3}, {5, 4}, {6, 3}, {7, 2}, {7, 3}},
			{{9, 2}, {9, 3}, {9, 5}, {10, 3}, {10, 4}, {10, 5}}, {{11, 1}, {11, 2}, {11, 3}, {11, 4}, {12, 2}, {12, 3}},
			{{0, 4}, {1, 4}, {2, 4}, {2, 5}, {2, 6}, {3, 4}}, {{3, 5}, {3, 6}, {4, 3}, {4, 4}, {4, 5}, {4, 6}},
			{{5, 5}, {5, 6}, {6, 4}, {6, 5}, {7, 4}, {7, 5}}, {{8, 4}, {8, 5}, {8, 6}, {9, 4}, {9, 6}, {9, 7}},
			{{12, 4}, {13, 4}, {13, 5}, {13, 6}, {13, 7}, {13, 8}}, {{0, 5}, {0, 6}, {1, 5}, {1, 6}, {1, 7}, {2, 7}},
			{{0, 7}, {0, 8}, {1, 8}, {2, 8}, {3, 7}, {3, 8}}, {{4, 7}, {4, 8}, {5, 7}, {5, 8}, {6, 6}, {6, 7}},
			{{6, 8}, {7, 6}, {7, 7}, {7, 8}, {8, 7}, {8, 8}}, {{9, 8}, {10, 6}, {10, 7}, {10, 8}, {11, 7}, {11, 8}},
			{{11, 5}, {11, 6}, {12, 5}, {12, 6}, {12, 7}, {12, 8}}};
		struct ZEx
		{
			int r, c;
			float z;
		};
		static const ZEx kZ[] = {{0, 4, -6.5517f}, {1, 3, -0.9330f}, {2, 2, -3.2422f}, {2, 4, -3.1781f}, {3, 3, -3.3705f}, {3, 4, -4.0119f},
			{3, 8, -3.2422f}, {4, 3, -5.4054f}, {4, 4, -4.4491f}, {4, 6, -3.5629f}, {5, 3, -5.4097f}, {5, 4, -3.8195f}, {5, 6, -3.5629f},
			{6, 3, -3.3705f}, {6, 4, -4.4491f}, {6, 5, -3.8195f}, {6, 6, -0.0309f}, {7, 4, -3.8195f}, {7, 7, -4.5251f}, {8, 2, -5.0383f},
			{8, 3, -4.5892f}, {8, 4, -3.5629f}, {8, 5, -4.5766f}, {8, 6, -2.8536f}, {8, 7, -0.6190f}, {9, 4, -3.3654f}, {9, 5, -2.8799f},
			{10, 2, 0.7348f}, {10, 5, -2.8554f}, {10, 6, -4.2670f}, {11, 3, -3.0466f}, {11, 4, -3.6204f}, {11, 5, -2.8554f}, {12, 5, -3.1741f}};
		// The entries (2.8): the card's history file as it is (entry i owns the cells of table i); without one, PS5SX2's own play time
		// (PCSX2's playtime.dat: the 21 most played, launches counted as one per two hours, an extra cell per 25 hours past 25), or
		// PS5SX2/DemoTowers' made-up set (eight of the Library's games, a spread of counts, a few extra cells).
		std::vector<s3::MemoryCard::HistoryEntry> entries;
		std::vector<bool> used;
		if (card_history)
			entries = S.history, used = S.history_used;
		else
		{
			std::vector<std::pair<std::string, uint64_t>> games;
			if (S.demo_towers)
			{
				static const int kDemoHours[8] = {120, 64, 30, 18, 9, 6, 3, 1};
				for (size_t i = 0; i < m_games.size() && games.size() < 8; i++)
					games.push_back({m_games[i].serial.empty() ? m_games[i].title : m_games[i].serial, kDemoHours[games.size()] * 3600ull});
			}
			else
			{
				for (const auto& [serial, pl] : S.played)
					games.push_back({serial, pl.seconds});
				std::sort(games.begin(), games.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
				if (games.size() > 21)
					games.resize(21);
			}
			for (const auto& [id, secs] : games)
			{
				const int hours = static_cast<int>(secs / 3600);
				s3::MemoryCard::HistoryEntry h;
				h.id = id;
				h.count = std::clamp(1 + hours / 2, 1, 255);
				unsigned hsh = 0;
				for (char ch : id)
					hsh = hsh * 31 + static_cast<unsigned char>(ch);
				h.cell = static_cast<int>(hsh % 6);
				for (int k = 1, extra = std::clamp((hours - 25) / 25, 0, 3); k < 6 && extra > 0; k++, extra--)
					h.mask |= 1 << ((h.cell + k) % 6);
				entries.push_back(h);
				used.push_back(true);
			}
		}
		struct Cell
		{
			bool on = false;
			float A = 1, h = 1;
		};
		Cell grid[14][9];
		for (size_t e = 0; e < entries.size() && e < 21; e++)
		{
			if (!used[e])
				continue;
			const int count = entries[e].count, main_cell = entries[e].cell, mask = entries[e].mask;
			for (int k = 0; k < 6; k++)
			{
				const int r = kCells[e][k][0], c = kCells[e][k][1];
				if (k == main_cell)
				{
					const int m = count < 14 ? count : (count - 14) % 10 + 4;
					grid[r][c] = {true, std::min(1.0f, 0.2f * (m + 1)), m < 5 ? 0.1f : 0.1f * (m - 3)};
				}
				else if (mask >> k & 1)
					grid[r][c] = {true, 1.0f, 1.0f};
			}
		}
		const s3::KitTex& wall = kit.T("TEXOWAL0");
		struct Face
		{
			float x[4], y[4], u[4], v[4];
			uint32_t col[4];
			float depth;
			float key = 0; // the tower's nearest corner (all its faces draw together, by it)
			int lod = 0;   // which of TEXOWAL0's mip levels it samples
			int tower = 0;
			bool end = false;
			float wind = 1;
			float fx[4] = {}, fy[4] = {}; // field coordinates (S3_TOWER_DUMP)
			int iv[4] = {};
		};
		std::vector<Face> faces;
		for (int r = 0; r < 14; r++)
			for (int c = 0; c < 9; c++)
			{
				const Cell& cl = grid[r][c];
				if (!cl.on)
					continue;
				float bx = -13.7428f + 1.3f * r + (r == 0 && c == 3 ? 0.6f : r == 1 && c == 3 ? 0.3f : 0.0f), by = 11.7512f - 1.3f * c, bz = -2.5366f;
				for (const ZEx& zx : kZ)
					if (zx.r == r && zx.c == c)
						bz = zx.z;
				const float L = std::max(30.0f * cl.h, 3.0f);
				const float X = (bx + 4.8f) * 4, Y = (by - 6.5f) * 4, Z = (bz + 4) * 12 + 150 + 30 * cl.A - L;
				// The turn: k x 90 deg + the sway (A < 1), cos exact and sin = sign x sqrt(1 - cos^2) as the library takes it.
				const int kq = ((r + c + 9) * (r + 3) / (c + 7)) % 4;
				const float sway = cl.A < 1 ? 10.0f * std::sin((std::fmod(nc, 360.0f) - 180.0f) * kPi_ / 180.0f) : 0.0f;
				const float ang = (kq * 90.0f + sway) * kPi_ / 180.0f;
				const float co = std::cos(ang), si = (ang >= 0 ? 1.0f : -1.0f) * std::sqrt(std::max(0.0f, 1 - co * co));
				const float q = cl.A < 1 ? std::trunc((1 - cl.A) * 128) / 128 : L / 30.0f;
				const float base = S3TowerLight(r + 3, c + 6) * q;
				const float s0 = static_cast<float>((r + c + 9) * (r + 8) / (c + 7) + (r + c + 9) * (r + 7) / (c + 9)) / 256.0f;
				// Five faces (corner x, y at +-2, z at +-L), normals pointing inwards; ST corners (0,0) (1,0) (0,1) (1,1).
				struct FD
				{
					float v[4][3];
					float n[3];
					bool end;
				};
				const FD fds[5] = {{{{2, -2, -L}, {2, -2, L}, {-2, -2, -L}, {-2, -2, L}}, {0, 1, 0}, false},
					{{{2, 2, L}, {2, 2, -L}, {-2, 2, L}, {-2, 2, -L}}, {0, -1, 0}, false},
					{{{2, -2, L}, {2, 2, L}, {2, -2, -L}, {2, 2, -L}}, {-1, 0, 0}, false},
					{{{-2, -2, -L}, {-2, 2, -L}, {-2, -2, L}, {-2, 2, L}}, {1, 0, 0}, false},
					{{{2, -2, -L}, {-2, -2, -L}, {2, 2, -L}, {-2, 2, -L}}, {0, 0, 1}, true}};
				static const float kST[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
				const size_t first = faces.size();
				for (const FD& fd : fds)
				{
					const float nx = fd.n[0] * co - fd.n[1] * si, ny = fd.n[0] * si + fd.n[1] * co, nz = fd.n[2];
					const float lit = 0.4f + std::max(0.0f, nz) + 0.8f * std::max(0.0f, -0.70711f * (nx + ny)) + 0.8f * std::max(0.0f, 0.70711f * (nx + ny));
					Face F;
					F.depth = 0;
					bool behind = false;
					for (int k = 0; k < 4; k++)
					{
						const float lx = fd.v[k][0] * co - fd.v[k][1] * si, ly = fd.v[k][0] * si + fd.v[k][1] * co;
						float cx, cy, cz;
						S3OpToCam(cam, X + lx, Y + ly, Z + fd.v[k][2], cx, cy, cz);
						behind |= cz < 1;
						float fx, fy;
						S3OpProj(cx, cy, cz, fx, fy);
						F.x[k] = kit.X(fx), F.y[k] = kit.Y(fy);
						F.fx[k] = fx, F.fy[k] = fy;
						F.depth += cz;
						const bool near_end = fd.v[k][2] < 0;
						float v = near_end ? (fd.end ? base : 0.8f * base) : 0.0f;
						const int iv = static_cast<int>(v * lit) & 255; // only the low 8 bits are kept (the brightest wrap)
						F.col[k] = s3::Kit::Pack(iv / 255.0f, iv / 255.0f, iv / 255.0f, 0.5f);
						F.iv[k] = iv;
						F.u[k] = s0 + kST[k][0], F.v[k] = kST[k][1];
					}
					F.end = fd.end;
					F.wind = (&fd == &fds[2] || &fd == &fds[3]) ? -1.0f : 1.0f; // (the two x sides' corner order winds the other way)
					if (!behind)
						faces.push_back(F);
				}
				// One tower = one convex box: only its faces turned to the camera (the winding of its near end's), all keyed by its
				// nearest corner, so neighbouring towers never swap faces between frames (the BIOS has a Z buffer; we sort).
				static const char* dump_all = std::getenv("S3_TOWER_DUMP");
				if (dump_all && fr < 0.01f)
					if (FILE* df = std::fopen(dump_all, "a"))
					{
						for (size_t k = first; k < faces.size(); k++)
							for (int q = 0; q < 4; q++)
								std::fprintf(df, "%d %.4f %.4f %d\n", S.op_cnt - 1, faces[k].fx[q], faces[k].fy[q], faces[k].iv[q]);
						std::fclose(df);
					}
				auto area = [](const Face& F) { return (F.x[1] - F.x[0]) * (F.y[2] - F.y[0]) - (F.y[1] - F.y[0]) * (F.x[2] - F.x[0]); };
				float front = 0, key = 1e9f;
				for (size_t k = first; k < faces.size(); k++)
				{
					if (faces[k].end)
						front = area(faces[k]);
					key = std::min(key, faces[k].depth);
				}
				size_t w = first;
				for (size_t k = first; k < faces.size(); k++)
					if (front == 0 || area(faces[k]) * faces[k].wind * front > 0)
					{
						faces[k].key = key, faces[k].tower = r * 9 + c;
						faces[w++] = faces[k];
					}
				faces.resize(w);
			}
		// (S3_TOWER_DUMP=<file>: every tower corner of the frame, all five faces, before the facing test: state cnt, field x y, grey.)
		// Far towers first; one tower's faces together (front faces of a convex box never overlap); ties by grid place.
		std::stable_sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.key != b.key ? a.key > b.key : a.tower < b.tower; });
		// TEXOWAL0 is mipmapped on the PS2 (levels 128 and 64 uploaded with it, 2.8: LOD per pixel). Sampled at full size, the far and
		// fast-turning faces sparkle from frame to frame; here each face takes the level its texel density asks for (box-filtered
		// levels of the BIOS's own texture, made once).
		static std::string mip_for;
		static s3::KitTex mips[4];
		if (wall.set && mip_for.empty())
			if (const s3::Image* im = S.bios.Tex("TEXOWAL0"))
			{
				s3::Image cur = *im;
				for (int l = 1; l < 4 && cur.w >= 2 && cur.h >= 2; l++)
				{
					s3::Image nx;
					nx.w = cur.w / 2, nx.h = cur.h / 2;
					nx.rgba.resize(static_cast<size_t>(nx.w) * nx.h * 4);
					for (int y = 0; y < nx.h; y++)
						for (int x = 0; x < nx.w; x++)
							for (int ch = 0; ch < 4; ch++)
							{
								auto at = [&](int xx, int yy) { return cur.rgba[(static_cast<size_t>(yy) * cur.w + xx) * 4 + ch]; };
								nx.rgba[(static_cast<size_t>(y) * nx.w + x) * 4 + ch] =
									static_cast<uint8_t>((at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
							}
					mips[l] = kit.Upload(nx);
					cur = std::move(nx);
				}
				mip_for = "TEXOWAL0";
			}
		// The BIOS's LOD (2.8, TEX1 LCM 0, K = -4.0625, MXL 2): log2(w) - 4.0625 with w the view depth; levels 0..2, the nearest here
		// per face (the GS blends between levels per pixel).
		for (Face& F : faces)
			F.lod = std::clamp(static_cast<int>(std::lround(std::log2(std::max(1.0f, F.depth / 4.0f)) - 4.0625f)), 0, 2);
		int bound = -1;
		for (const Face& F : faces)
		{
			const int lv = mips[F.lod].set ? F.lod : 0;
			if (lv != bound)
				kit.Bind(lv == 0 ? wall.set : mips[lv].set), bound = lv;
			// The strip's two triangles (corner order 0 1 2 3: 0-1-2 and 1-3-2).
			const float t1[6] = {F.x[0], F.y[0], F.x[1], F.y[1], F.x[2], F.y[2]}, u1[6] = {F.u[0], F.v[0], F.u[1], F.v[1], F.u[2], F.v[2]};
			const float t2[6] = {F.x[1], F.y[1], F.x[3], F.y[3], F.x[2], F.y[2]}, u2[6] = {F.u[1], F.v[1], F.u[3], F.v[3], F.u[2], F.v[2]};
			const uint32_t c1[3] = {F.col[0], F.col[1], F.col[2]}, c2[3] = {F.col[1], F.col[3], F.col[2]};
			kit.Tri(t1, u1, c1, wall.set ? 13 : 11, false), kit.Tri(t2, u2, c2, wall.set ? 13 : 11, false);
		}
	}

	// The towers are the layer the afterimage feeds back (2.3: drawn, then 80/128 of the buffer over them, then copied in at half width).
	kit.MarkGhost(S.op_cnt <= 1);

	// ---- the fog (2.7): six 16 x 16 grids at z 134 - 5L, additive 20/128, deep blue, brightest left of centre ----
	{
		const char* tex[6] = {"TEXOFOG4", "TEXOFOG2", "TEXOFOG1", "TEXOFOG4", "TEXOFOG2", "TEXOFOG1"};
		const float R = std::sqrt(5202.0f);
		static float vx[17][17], vy[17][17], vc[17][17];
		for (int L = 0; L < 6; L++)
		{
			const s3::KitTex& t = kit.T(tex[L]);
			if (!t.set)
				continue;
			kit.Bind(t.set);
			bool cz_ok[17][17];
			for (int iy = 0; iy <= 16; iy++)
				for (int ix = 0; ix <= 16; ix++)
				{
					float cx, cy, cz;
					S3OpToCam(cam, 6.0f * ix - 48 + 2.0f, 6.0f * iy - 48, 134.0f - 5 * L, cx, cy, cz);
					cz_ok[iy][ix] = cz > 1;
					S3OpProj(cx, cy, cz, vx[iy][ix], vy[iy][ix]);
					const float dx = -5.1f - (6.0f * ix - 45), dy = 45 - 6.0f * iy;
					vc[iy][ix] = std::floor(std::clamp((R - 4 * std::hypot(dx, dy)) * 96.0f / R, 0.0f, 127.0f));
				}
			for (int qy = 0; qy < 16; qy++)
				for (int qx = 0; qx < 16; qx++)
				{
					bool in = false, ok = true;
					for (int a = 0; a < 2; a++)
						for (int b = 0; b < 2; b++)
						{
							const float x = vx[qy + a][qx + b], y = vy[qy + a][qx + b];
							in |= x > kit.left() && x < kit.right() && y > 16 && y < 240;
							ok &= cz_ok[qy + a][qx + b];
						}
					if (!in || !ok)
						continue;
					float xy[8], uv[8];
					uint32_t col[4];
					for (int k = 0; k < 4; k++)
					{
						const int a = k >> 1, b = k & 1, ix = qx + b, iy = qy + a;
						xy[k * 2] = kit.X(vx[iy][ix]), xy[k * 2 + 1] = kit.Y(vy[iy][ix]);
						uv[k * 2] = 0.5f * b + 0.5f * (qx % 2) - S.op_scroll[L], uv[k * 2 + 1] = 0.5f * a + 0.5f * (qy % 2);
						const float c = vc[iy][ix];
						col[k] = s3::Kit::Pack(std::floor(c / 4) / 255.0f, std::floor(2 * c / 5) / 255.0f, c / 255.0f, 1.0f);
					}
					const float t1[6] = {xy[0], xy[1], xy[2], xy[3], xy[4], xy[5]}, u1[6] = {uv[0], uv[1], uv[2], uv[3], uv[4], uv[5]};
					const float t2[6] = {xy[2], xy[3], xy[6], xy[7], xy[4], xy[5]}, u2[6] = {uv[2], uv[3], uv[6], uv[7], uv[4], uv[5]};
					const uint32_t c1[3] = {col[0], col[1], col[2]}, c2[3] = {col[1], col[3], col[2]};
					// The GS's own integer path (ui mode 15): (T x V) >> 7, then x FIX 20 >> 7, added to the frame.
					static const float fix[6] = {20, 0, 20, 0, 20, 0};
					kit.TriP(t1, u1, fix, c1, 15, true), kit.TriP(t2, u2, fix, c2, 15, true);
				}
		}
	}

	// ---- the lights (2.6): four soft discs with a 4-frame smear and a trail, while z < 73 ----
	// 16:9 (ours, kurolume 2026-11-16): the PS2 cuts the lights at z 73, which a 4:3 picture hides under its bars; on a full 16:9 screen
	// they would pop in view, so here they fly on toward the edges and go with the rest of the picture in the fade to black (z 104).
	if (z < (S.wide ? 104.0f : 73.0f))
	{
		static const int kCol[4][3] = {{32, 128, 0}, {128, 32, 64}, {128, 0, 0}, {64, 32, 128}};
		// (The trail ring is filled per tick in S3OpeningTick.)
		kit.Bind(VK_NULL_HANDLE);
		const int N = std::min(S.op_trail_n - 2, 125);
		for (int i = 0; i < 4 && N > 8; i++)
		{
			float px = 0, py = 0;
			bool have = false;
			for (int u = 0; u <= N; u += 8)
			{
				const int idx = ((S.op_trail_w - 1 - u) % 128 + 128) % 128;
				const float x = S.op_trail[i][idx][0], y = S.op_trail[i][idx][1];
				const int m = (N - u) * 64 / N;
				// (A vertex off the window, x 0..640 and y 16..240, or nearer than z 5, goes in without a kick: the line to it isn't drawn.)
				if (have && S.op_trail[i][idx][2] >= 5 && x >= kit.left() && x <= kit.right() && y >= 16 && y <= 240)
				{
					const float X0 = kit.X(px), Y0 = kit.Y(py), X1 = kit.X(x), Y1 = kit.Y(y);
					const float dx = X1 - X0, dy = Y1 - Y0, len = std::max(0.01f, std::hypot(dx, dy)), lw = kit.SY() * 0.5f;
					const float nx = -dy / len * lw, ny = dx / len * lw;
					const uint32_t c = s3::Kit::Pack(kCol[i][0] * m / 128.0f / 255.0f, kCol[i][1] * m / 128.0f / 255.0f, kCol[i][2] * m / 128.0f / 255.0f, 0.5f);
					const float a1[6] = {X0 + nx, Y0 + ny, X1 + nx, Y1 + ny, X1 - nx, Y1 - ny}, a2[6] = {X0 + nx, Y0 + ny, X1 - nx, Y1 - ny, X0 - nx, Y0 - ny};
					const uint32_t cc[3] = {c, c, c};
					kit.Tri(a1, nullptr, cc, 11, true), kit.Tri(a2, nullptr, cc, 11, true);
				}
				px = x, py = y, have = true;
			}
		}
		const s3::KitTex& disc = kit.T("TEXOCRBL");
		for (int k = 1; k <= 4; k++) // oldest first: where it was 3, 2, 1, 0 frames ago, each through the camera of that frame
		{
			// (The camera then: between ticks j + 1 and j ago, as the newest copy's is between the last two. The camera flies
			// on while the lights hardly move, so near the end the older copies sit nearer the centre: the smear streaks outward.)
			const int j = 4 - k;
			const float* a = S.op_Ch[j + 1];
			const float* b = S.op_Ch[j];
			const S3OpCam cam_k = {{a[0] + (b[0] - a[0]) * fr, a[1] + (b[1] - a[1]) * fr, a[2] + (b[2] - a[2]) * fr},
				a[3] + std::remainder(b[3] - a[3], 2 * kPi_) * fr};
			for (int i = 0; i < 4; i++)
			{
				float w[3], cx, cy, cz;
				S3OpLight(i, nc - j, w);
				S3OpToCam(cam_k, w[0], w[1], w[2], cx, cy, cz);
				if (cz < 1)
					continue;
				for (int part = 0; part < 2; part++)
				{
					const float h = part ? 0.25f : 0.8f;
					const int base = part ? 12 : 24;
					float x0, y0, x1, y1;
					S3OpProj(cx - h, cy - h, cz, x0, y0);
					S3OpProj(cx + h, cy + h, cz, x1, y1);
					// Skipped unless a corner is strictly inside the window (x 0..640, y 16..240; the fog's test).
					const bool in = std::max(x0, x1) > kit.left() && std::min(x0, x1) < kit.right() && std::max(y0, y1) > 16 && std::min(y0, y1) < 240;
					if (!in)
						continue;
					const s3::Gs col = part ? s3::Gs{128, 128, 128} : s3::Gs{kCol[i][0] / 2, kCol[i][1] / 2, kCol[i][2] / 2};
					if (disc.set)
						kit.Sprite(disc, kit.X(x0), kit.Y(y0), kit.X(x1), kit.Y(y1), 0, 0, 64, 64, col, base * k / 5 * 2, true);
				}
			}
		}
	}
	kit.MarkScene();
	// The resample blur (2.9): levels 1..3 as z passes 56 + 12 n.
	f.blur = z > 56 ? std::min(3.0f, std::floor((z - 56) / 12.0f)) * 0.9f : 0.0f;

	// ---- the floating cubes (2.5), while z < 73: glass refracting the frame (the scene image), with the TEXOREF reflection ----
	if (z < 73)
	{
		static const float kT[5][3] = {{3.5679f, 0.5447f, 2.5932f}, {-0.9042f, -1.1173f, 3.7952f}, {3.2639f, -2.6491f, 4.1075f},
			{-3.7296f, -2.3677f, 4.3654f}, {-3.1017f, 2.2409f, 4.5429f}};
		static const int kF[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 6, 2}, {2, 3, 6, 7}, {1, 5, 3, 7}, {4, 5, 0, 1}};
		const VkDescriptorSet scene = m_renderer->SceneSet();
		const s3::KitTex& ref = kit.T("TEXOREF");
		const float tw = kit.W() / kit.SX(), th = kit.H() / kit.SY(), x0 = -kit.X(0) / kit.SX();
		for (int i = 0; i < 5; i++)
		{
			float fi = 0.8f * (i - 2);
			if (fi == 0)
				fi = 0.9f;
			const float start = 0.285599f + 3.7f * fi * (i % 3);
			const float ax = start + nc * 0.0031f / fi, ay = start + nc * 0.0022f * fi, az = start + nc * (0.0013f + fi / 1000.0f);
			const M4 rot = MMul(MRotX(ax), MMul(MRotY(ay), MRotZ(az)));
			const float C[3] = {3.5f * kT[i][0], 3.5f * kT[i][1], 150 - 15 * kT[i][2]};
			float cam_c[8][3], sx[8], sy[8];
			bool inside = false, ok = true;
			for (int v = 0; v < 8; v++)
			{
				float px, py, pz;
				MApply(rot, (v & 1 ? 1.8f : -1.8f), (v & 2 ? 1.8f : -1.8f), (v & 4 ? 1.8f : -1.8f), px, py, pz);
				S3OpToCam(cam, C[0] + px, C[1] + py, C[2] + pz, cam_c[v][0], cam_c[v][1], cam_c[v][2]);
				ok &= cam_c[v][2] > 1;
				S3OpProj(cam_c[v][0], cam_c[v][1], cam_c[v][2], sx[v], sy[v]);
				inside |= sx[v] > kit.left() && sx[v] < kit.right() && sy[v] > 16 && sy[v] < 240;
			}
			if (!inside || !ok)
				continue;
			float ccx, ccy, ccz, pcx, pcy;
			S3OpToCam(cam, C[0], C[1], C[2], ccx, ccy, ccz);
			S3OpProj(ccx, ccy, ccz, pcx, pcy);
			for (int pass = 0; pass < 2; pass++) // back faces, then front faces
				for (int fc = 0; fc < 6; fc++)
				{
					const int* q = kF[fc];
					const float cross = (sx[q[1]] - sx[q[0]]) * (sy[q[2]] - sy[q[0]]) - (sy[q[1]] - sy[q[0]]) * (sx[q[2]] - sx[q[0]]);
					const bool back = cross < 0;
					if (back != (pass == 0))
						continue;
					// The face normal in camera space (the first two edges' cross product, unit).
					float e1[3], e2[3], n[3];
					for (int k = 0; k < 3; k++)
						e1[k] = cam_c[q[1]][k] - cam_c[q[0]][k], e2[k] = cam_c[q[2]][k] - cam_c[q[0]][k];
					n[0] = e1[1] * e2[2] - e1[2] * e2[1], n[1] = e1[2] * e2[0] - e1[0] * e2[2], n[2] = e1[0] * e2[1] - e1[1] * e2[0];
					const float nl = std::max(1e-5f, std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]));
					for (float& c : n)
						c /= nl;
					const float kd = back ? 0.0f : -0.084f, gain = back ? 122.0f : 240.0f, refk = back ? 42.0f : 64.0f, kr = back ? -0.25f : 0.5f;
					float xy[8], uv[8], ruv[8];
					uint32_t col[4], rcol[4];
					for (int k = 0; k < 4; k++)
					{
						const int v = q[k];
						float D[3] = {cam_c[v][0], cam_c[v][1], cam_c[v][2]};
						const float dl = std::sqrt(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
						for (float& c : D)
							c /= std::max(dl, 1e-4f);
						const float F = 1 - std::fabs(n[0] * D[0] + n[1] * D[1] + n[2] * D[2]), P = F * F / 2;
						const float qq = 1.0f / cam_c[v][2];
						float u = sx[v] + n[0] * 320 * qq * 4 + (sx[v] - pcx) * kd, w = sy[v] + n[1] * 112 * qq * 4 + (sy[v] - pcy) * kd;
						u = std::clamp(u, 1.0f, 639.0f), w = std::clamp(w, 1.0f, 255.0f);
						xy[k * 2] = kit.X(sx[v]), xy[k * 2 + 1] = kit.Y(sy[v]);
						uv[k * 2] = (u - x0) / tw, uv[k * 2 + 1] = w / th;
						const float cr = std::min(128.0f, std::floor((112 + 32 * P) * gain / 128)), cb = std::min(128.0f, std::floor((127 + 32 * P) * gain / 128));
						col[k] = s3::Kit::Pack(cr / 255.0f, cr / 255.0f, cb / 255.0f, 0.5f);
						ruv[k * 2] = D[1] - kr * n[1] + 0.5f, ruv[k * 2 + 1] = D[0] - kr * n[0] + 0.5f;
						const float rr = std::floor(112 * (0.24f * P + 0.2f) * refk / 128), rb = std::floor(127 * (0.24f * P + 0.2f) * refk / 128);
						rcol[k] = s3::Kit::Pack(rr / 255.0f, rr / 255.0f, rb / 255.0f, 0.5f);
					}
					const float t1[6] = {xy[0], xy[1], xy[2], xy[3], xy[4], xy[5]}, t2[6] = {xy[2], xy[3], xy[6], xy[7], xy[4], xy[5]};
					const float u1[6] = {uv[0], uv[1], uv[2], uv[3], uv[4], uv[5]}, u2[6] = {uv[2], uv[3], uv[6], uv[7], uv[4], uv[5]};
					const uint32_t c1[3] = {col[0], col[1], col[2]}, c2[3] = {col[1], col[3], col[2]};
					if (scene)
					{
						kit.Bind(scene);
						kit.Tri(t1, u1, c1, 9, false), kit.Tri(t2, u2, c2, 9, false);
					}
					if (ref.set)
					{
						// Passes 2-5 / 7-10: TEXOBLP then TEXOBLPR write the destination alpha (a mask at the face's ST, shifted), and the
						// reflection after each adds Cs x Ad: twice, each weighted by its mask.
						const float r1[6] = {ruv[0], ruv[1], ruv[2], ruv[3], ruv[4], ruv[5]}, r2[6] = {ruv[2], ruv[3], ruv[6], ruv[7], ruv[4], ruv[5]};
						const uint32_t d1[3] = {rcol[0], rcol[1], rcol[2]}, d2[3] = {rcol[1], rcol[3], rcol[2]};
						const float sh = back ? 0.00375f : 0.0075f;
						for (int pass = 0; pass < 2; pass++)
						{
							const s3::KitTex& mk = kit.T(pass == 0 ? "TEXOBLP" : "TEXOBLPR");
							const float ox = pass == 0 ? -sh : sh, oy = pass == 0 ? sh : -sh;
							const float st[8] = {0 + ox, 0 + oy, 1 + ox, 0 + oy, 0 + ox, 1 + oy, 1 + ox, 1 + oy};
							const float s1[6] = {st[0], st[1], st[2], st[3], st[4], st[5]}, s2[6] = {st[2], st[3], st[6], st[7], st[4], st[5]};
							if (const VkDescriptorSet set = mk.set ? kit.PairSet(ref, mk) : VK_NULL_HANDLE)
							{
								kit.Bind(set);
								kit.TriP(t1, r1, s1, d1, 14, true), kit.TriP(t2, r2, s2, d2, 14, true);
							}
						}
					}
				}
		}
	}

	// ---- the SCE logo (2.4): two sprites at (120, 120) and (326, 120), 256 x 18, alpha min(t, 112) ----
	const s3::KitTex& sce = kit.T("TEXOSCE");
	if (sce.set && S.op_logo > 0)
	{
		const int a = std::min(S.op_logo, 112);
		kit.Sprite(sce, kit.X(120), kit.Y(120), kit.X(376), kit.Y(138), 0, 1, 256, 31, s3::kWhite, a, false);
		kit.Sprite(sce, kit.X(326), kit.Y(120), kit.X(582), kit.Y(138), 0, 33, 256, 63, s3::kWhite, a, false);
	}
	// The letterbox with the 4:3 shape (rows 0..33 and 223..255, 2.3), and the black: full while cnt < 2, (z - 72) x 4 past z 72, a skip's fade.
	if (!S.wide)
	{
		kit.FillPx(0, 0, kit.W(), kit.Y(34), 0, 0, 0, 1);
		kit.FillPx(0, kit.Y(223), kit.W(), kit.H(), 0, 0, 0, 1);
	}
	float black = S.op_cnt < 2 ? 1.0f : z > 72 ? std::min(1.0f, (z - 72) * 4 / 128.0f) : 0.0f;
	if (S.op_skip)
		black = std::max(black, std::min(1.0f, S.op_skip / 20.0f));
	kit.Black(black);
}
} // namespace fe
