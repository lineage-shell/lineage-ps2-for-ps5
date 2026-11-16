// PS2 shelf v3 (AI-assisted): App's shelf v3 screens, part 2: the Library, a game's page, the settings (System Configuration), the launch,
// search and the Library's filter and sort. Included at the end of fe_app.cpp after s3_app.inc.
// SPDX-License-Identifier: GPL-3.0-or-later

namespace fe
{

namespace
{
const char* kKeys[4] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789 -'&"};
fe::Texture* g_s3_flat_src = nullptr;
VkDescriptorSet g_s3_flat_set = VK_NULL_HANDLE;
fe::Texture* g_s3_back_src = nullptr;
VkDescriptorSet g_s3_back_set = VK_NULL_HANDLE;

// A five-point star (ours: the favourite mark) at field (x, y), radius r units.
void S3Star(s3::Kit& kit, float x, float y, float r, bool on, int alpha, s3::Gs fill = s3::kGold, float inner = 0.78f)
{
	kit.Bind(VK_NULL_HANDLE);
	float px[10], py[10];
	for (int i = 0; i < 10; i++)
	{
		const float a = -kPi_ / 2 + i * kPi_ / 5, rr = (i & 1) ? r * 0.45f : r;
		px[i] = kit.X(x + std::cos(a) * rr), py[i] = kit.Y(y + std::sin(a) * rr * 0.53f);
	}
	const float cx = kit.X(x), cy = kit.Y(y);
	auto fan = [&](float scale, uint32_t col) {
		for (int i = 0; i < 10; i++)
		{
			const int j = (i + 1) % 10;
			const float t[6] = {cx, cy, cx + (px[i] - cx) * scale, cy + (py[i] - cy) * scale, cx + (px[j] - cx) * scale, cy + (py[j] - cy) * scale};
			const uint32_t c[3] = {col, col, col};
			kit.Tri(t, nullptr, c, 11, false);
		}
	};
	const float a = alpha / 255.0f;
	if (on)
	{
		fan(1.0f, s3::Kit::Pack(0.08f, 0.06f, 0.02f, a));
		fan(inner, s3::Kit::Pack(fill.r / 255.0f, fill.g / 255.0f, fill.b / 255.0f, a));
	}
	else
	{
		fan(1.0f, s3::Kit::Pack(96 / 255.0f, 96 / 255.0f, 96 / 255.0f, a));
		fan(0.62f, s3::Kit::Pack(0.03f, 0.03f, 0.05f, a * 0.85f));
	}
}
} // namespace

int App::S3Cols() const
{
	if (m_s3.searching || !m_search.applied.empty()) // search results: the BIOS's count rule (browser.md 6.1)
	{
		const int n = static_cast<int>(m_games.size());
		return n <= 1 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 15 ? 4 : 5;
	}
	// Round 3 (kurolume: the cases too small): 6x3 / large 4x2 in 16:9, 5x3 / 3x2 in 4:3.
	return m_s3.wide ? (m_s3.large ? 4 : 6) : (m_s3.large ? 3 : 5);
}
int App::S3Rows() const
{
	if (m_s3.searching || !m_search.applied.empty())
		return std::clamp((static_cast<int>(m_games.size()) + S3Cols() - 1) / S3Cols(), 1, 4);
	return m_s3.large ? 2 : 3;
}

// ---- search, filter and sort (hooked into SearchMatches and ApplyHidden) ----

bool App::S3SearchMatches(const GameInfo& g) const
{
	// Word starts: every word typed must begin a word of the title (or be in the serial). "GR" finds Gran Turismo and Grand Theft Auto.
	const std::string& q = m_search.applied;
	std::string title = SearchLower(g.title), serial;
	for (char c : g.serial)
		if (c != '-')
			serial += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	for (size_t at = 0; at < q.size();)
	{
		size_t end = q.find(' ', at);
		if (end == std::string::npos)
			end = q.size();
		if (end > at)
		{
			const std::string w = q.substr(at, end - at);
			bool found = serial.rfind(w, 0) == 0;
			for (size_t i = 0; !found && i < title.size(); i++)
				if ((i == 0 || !std::isalnum(static_cast<unsigned char>(title[i - 1]))) && title.compare(i, w.size(), w) == 0)
					found = true;
			if (!found)
				return false;
		}
		at = end + 1;
	}
	return true;
}

bool App::S3Keep(const GameInfo& g, int index) const
{
	if (index < 0)
		return true;
	// A multi-disc game is one entry: its first disc (by N) stands for the set, the others stay off the shelf.
	if (DiscNumber(g.file, nullptr) > 0)
	{
		auto it = m_s3.disc_sets.find(g.path);
		if (it == m_s3.disc_sets.end())
			it = m_s3.disc_sets.emplace(g.path, DiscSet(g.path)).first;
		if (it->second.size() > 1 && it->second.front() != g.path)
			return false;
	}
	if (m_s3.filter == 2)
		return S3Favourite(g.serial);
	if (m_s3.filter == 1)
	{
		auto it = m_s3.played.find(g.serial);
		return it != m_s3.played.end() && it->second.last > 0;
	}
	return true;
}

void App::S3SortShelf()
{
	if (m_s3.sort == 0 || m_games.size() < 2)
		return;
	const int keep = m_index[static_cast<size_t>(m_selected)];
	std::vector<size_t> order(m_games.size());
	for (size_t i = 0; i < order.size(); i++)
		order[i] = i;
	auto key = [&](size_t i) -> std::pair<int64_t, uint64_t> {
		auto it = m_s3.played.find(m_games[i].serial);
		if (it == m_s3.played.end())
			return {0, 0};
		return {it->second.last, it->second.seconds};
	};
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		const auto ka = key(a), kb = key(b);
		return m_s3.sort == 1 ? ka.first > kb.first : ka.second > kb.second;
	});
	std::vector<GameInfo> g;
	std::vector<Slot> s;
	std::vector<int> idx;
	for (size_t i : order)
		g.push_back(std::move(m_games[i])), s.push_back(m_slots[i]), idx.push_back(m_index[i]);
	m_games = std::move(g), m_slots = std::move(s), m_index = std::move(idx);
	for (size_t i = 0; i < m_index.size(); i++)
		if (m_index[i] == keep)
			m_selected = static_cast<int>(i);
}

// ---- the Library page ----

void App::S3DrawCase(int pos, float cx, float cy, float h_px, float yaw, float pitch, float alpha, bool flat)
{
	if (pos < 0 || pos >= static_cast<int>(m_games.size()))
		return;
	const Slot& sl = m_slots[static_cast<size_t>(pos)];
	const float W = static_cast<float>(m_renderer->width()), H = static_cast<float>(m_renderer->height());
	const float w_px = h_px * (kBoxHalfW / kBoxHalfH);
	if (!flat)
	{
		if (!sl.set || !g_s3_frame)
			return;
		BoxDraw b;
		b.model = Ps2CaseModel(cx, cy, 0.0f, w_px, yaw, pitch, 0.0f, W, H);
		b.set = sl.set;
		b.cover_mix = sl.cover_mix;
		b.brightness = 0.92f;
		b.alpha = alpha;
		g_s3_frame->boxes.push_back(b);
		return;
	}
	// The game page's big case, drawn over the soft-focus page: the front (the cover, else the painted one) and the spine, in a slight turn.
	Texture* src = sl.has_cover && sl.cover ? sl.cover : sl.placeholder;
	if (src != g_s3_flat_src)
	{
		if (g_s3_flat_set)
			m_renderer->FreeTextureSet(g_s3_flat_set);
		g_s3_flat_set = src ? m_renderer->AllocTextureSet(src, src, src, src) : VK_NULL_HANDLE;
		g_s3_flat_src = src;
	}
	// The back: the player's back cover, else the painted one (2.11's), as its own set.
	Texture* back = sl.back_real ? sl.back_real : sl.back_ph;
	if (back != g_s3_back_src)
	{
		if (g_s3_back_set)
			m_renderer->FreeTextureSet(g_s3_back_set);
		g_s3_back_set = back ? m_renderer->AllocTextureSet(back, back, back, back) : VK_NULL_HANDLE;
		g_s3_back_src = back;
	}
	s3::Kit& kit = m_s3.kit;
	// A whole case (135 x 190 x 14 mm) turned by `yaw` (the right stick spins it right round): six faces, back faces dropped, far first.
	const float hw = w_px * 0.5f, hh = h_px * 0.5f, d = w_px * kBoxHalfD / kBoxHalfW;
	const float c = std::cos(yaw), s = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
	struct Pt
	{
		float x, y, z;
	};
	auto P = [&](float x, float y, float z) {
		const float X = x * c + z * s, Z0 = -x * s + z * c;
		const float Y = y * cp - Z0 * sp, Z = y * sp + Z0 * cp;
		const float k = 1.0f / (1.0f + Z / (H * 1.6f));
		return Pt{cx + X * k, cy + Y * k, Z};
	};
	struct FaceDef
	{
		float v[4][3];
		int kind; // 0 front, 1 back, 2 side
	};
	const FaceDef faces[6] = {
		{{{-hw, -hh, -d}, {hw, -hh, -d}, {hw, hh, -d}, {-hw, hh, -d}}, 0},
		{{{hw, -hh, d}, {-hw, -hh, d}, {-hw, hh, d}, {hw, hh, d}}, 1},
		{{{hw, -hh, -d}, {hw, -hh, d}, {hw, hh, d}, {hw, hh, -d}}, 2},
		{{{-hw, -hh, d}, {-hw, -hh, -d}, {-hw, hh, -d}, {-hw, hh, d}}, 2},
		{{{-hw, -hh, d}, {hw, -hh, d}, {hw, -hh, -d}, {-hw, -hh, -d}}, 2},
		{{{-hw, hh, -d}, {hw, hh, -d}, {hw, hh, d}, {-hw, hh, d}}, 2},
	};
	struct Drawn
	{
		Pt p[4];
		int kind;
		float z;
	};
	std::vector<Drawn> list;
	for (const FaceDef& fd : faces)
	{
		Drawn dr;
		dr.kind = fd.kind;
		dr.z = 0;
		for (int i = 0; i < 4; i++)
			dr.p[i] = P(fd.v[i][0], fd.v[i][1], fd.v[i][2]), dr.z += dr.p[i].z;
		const float cross = (dr.p[1].x - dr.p[0].x) * (dr.p[2].y - dr.p[0].y) - (dr.p[1].y - dr.p[0].y) * (dr.p[2].x - dr.p[0].x);
		if (cross > 0)
			list.push_back(dr);
	}
	std::sort(list.begin(), list.end(), [](const Drawn& a, const Drawn& b) { return a.z > b.z; });
	for (const Drawn& dr : list)
	{
		const float t1[6] = {dr.p[0].x, dr.p[0].y, dr.p[1].x, dr.p[1].y, dr.p[2].x, dr.p[2].y};
		const float t2[6] = {dr.p[0].x, dr.p[0].y, dr.p[2].x, dr.p[2].y, dr.p[3].x, dr.p[3].y};
		const VkDescriptorSet set = dr.kind == 0 ? g_s3_flat_set : dr.kind == 1 ? g_s3_back_set : VK_NULL_HANDLE;
		if (set)
		{
			kit.Bind(set);
			const uint32_t w = s3::Kit::Pack(0.5f, 0.5f, 0.5f, alpha * 0.5f);
			const float u1[6] = {0, 0, 1, 0, 1, 1}, u2[6] = {0, 0, 1, 1, 0, 1};
			const uint32_t k[3] = {w, w, w};
			kit.Tri(t1, u1, k, 9, false), kit.Tri(t2, u2, k, 9, false);
		}
		else
		{
			kit.Bind(VK_NULL_HANDLE);
			const uint32_t sc = dr.kind == 1 ? s3::Kit::Pack(0.12f, 0.12f, 0.14f, alpha * 0.5f) : s3::Kit::Pack(0.05f, 0.05f, 0.06f, alpha * 0.5f);
			const uint32_t k[3] = {sc, sc, sc};
			kit.Tri(t1, nullptr, k, 11, false), kit.Tri(t2, nullptr, k, 11, false);
		}
	}
}

void App::S3LibraryTick(const Input& in)
{
	(void)in;
	s3::Shelf3& S = m_s3;
	const S3Pad& p = g_s3pad;
	S.flare_phase += 0.03f;
	S.flare_h += (788.0f + 200.0f * std::sin(S.flare_phase) - S.flare_h) / 44.0f;
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
			S.scr = S.page_to;
			S.dcc = 0, S.dcc_dir = 1;
			S.cam[0] = 0, S.cam[1] = 2.7f, S.cam[2] = 0;
		}
		return;
	}
	const int n = static_cast<int>(m_games.size());
	const int cols = S3Cols(), rows = S3Rows();
	S.scroll += (static_cast<float>(S.top) - S.scroll) * 0.25f;
	auto settle = [&]() {
		const int row = m_selected / std::max(1, cols);
		if (row < S.top)
			S.top = row;
		else if (row >= S.top + rows)
			S.top = row - rows + 1;
	};
	if (S.options_open)
	{
		if (p.fresh(kBUp) && S.options_row > 0)
			S.options_row--, S3Sfx(6);
		else if (p.fresh(kBDown) && S.options_row < 1)
			S.options_row++, S3Sfx(6);
		else if (p.fresh(kBLeft) || p.fresh(kBRight))
		{
			int& v = S.options_row == 0 ? S.filter : S.sort;
			const int nv = std::clamp(v + (p.fresh(kBLeft) ? -1 : 1), 0, 2);
			if (nv != v)
			{
				v = nv;
				S3Sfx(6);
				ApplyHidden(n > 0 ? m_index[static_cast<size_t>(m_selected)] : -1);
				S.top = 0, settle();
			}
		}
		else if (p.fresh(kBCircle) || p.fresh(kBTriangle) || p.fresh(kBCross))
			S.options_open = false, S3Sfx(10);
		return;
	}
	if (S.searching)
	{
		int r = S.key_row, c = S.key_col;
		if (p.repeat(kBUp))
			r = (r + 3) % 4;
		else if (p.repeat(kBDown))
			r = (r + 1) % 4;
		else if (p.repeat(kBLeft))
			c = (c + 9) % 10;
		else if (p.repeat(kBRight))
			c = (c + 1) % 10;
		if (r != S.key_row || c != S.key_col)
			S.key_row = r, S.key_col = c, S3Sfx(6);
		bool changed = false;
		if (p.fresh(kBCross) && S.query.size() < 24)
			S.query += kKeys[S.key_row][S.key_col], changed = true, S3Sfx(4);
		else if (p.fresh(kBSquare) && !S.query.empty())
			S.query.pop_back(), changed = true, S3Sfx(10);
		else if (p.fresh(kBTriangle))
		{
			S.searching = false, S3Sfx(4);
			m_selected = 0;
			S.top = 0;
		}
		else if (p.fresh(kBCircle))
		{
			S.searching = false, S.query.clear(), S3Sfx(10);
			SetSearch(std::string());
			S.top = 0, settle();
		}
		if (changed)
		{
			SetSearch(S.query);
			m_selected = 0;
			S.top = 0;
		}
		return;
	}
	if (n > 0)
	{
		int cur = m_selected;
		if (p.repeat(kBLeft) && cur > 0)
			cur--;
		else if (p.repeat(kBRight) && cur < n - 1)
			cur++;
		else if (p.repeat(kBUp) && cur >= cols)
			cur -= cols;
		else if (p.repeat(kBDown) && cur / cols < (n - 1) / cols)
			cur = std::min(n - 1, cur + cols);
		if (cur != m_selected)
		{
			const bool fresh = p.fresh(kBLeft) || p.fresh(kBRight) || p.fresh(kBUp) || p.fresh(kBDown);
			Step(cur - m_selected);
			settle();
			if (fresh)
				S3Sfx(6);
		}
		if (p.fresh(kBCross) && m_index[static_cast<size_t>(m_selected)] < 0)
			S3Sfx(11); // the game being copied: not yet
		else if (p.fresh(kBCross))
		{
			S3Sfx(4);
			S3GameOpen(m_selected);
			return;
		}
	}
	if (p.fresh(kBSquare))
		S.searching = true, S.query = m_search.text, S3Sfx(4);
	else if (p.fresh(kBTriangle))
		S.options_open = true, S.options_row = 0, S3Sfx(4);
	else if (p.fresh(kBCircle) && !m_search.applied.empty())
	{
		// A search narrows the Library: Circle shows every game again first.
		S3Sfx(10);
		S.query.clear();
		SetSearch(std::string());
		S.top = 0, settle();
	}
	else if (p.fresh(kBCircle))
	{
		S3Sfx(10);
		S.page_to = s3::Scr::Browser;
		S.dcc_dir = -1, S.dcc = 21;
	}
}

void App::S3LibraryBuild(FrameDesc& f, bool behind)
{
	s3::Shelf3& S = m_s3;
	s3::Kit& kit = S.kit;
	g_s3_frame = &f;
	kit.BrowserField(kSet2);
	if (!behind)
		kit.MarkUnder(); // (behind the game page all of it stays under that page's case: S3GameBuild marks it)
	const int n = static_cast<int>(m_games.size());
	const int cols = S3Cols(), rows = S3Rows();
	const bool rule = S.searching || !m_search.applied.empty();
	const float px = rule ? 24.0f : (S.large ? 36.0f : 26.0f), py = rule ? 27.5f : (S.large ? 37.0f : 30.0f);
	const float case_h = rule ? 16.5f : S.large ? 24.0f : 19.5f;
	// While typing, the results sit right of the keypad.
	float cam[3] = {S.searching ? (130.0f + 40.0f * (rows - 1)) * 0.17f : 0.0f, 2.7f, rule ? 160.0f + 40.0f * (rows - 1) : (S.large ? 190.0f : 230.0f)};
	const float cx = (cols * px + 6) / 2.0f, cy = (rows * py + 4) / 2.0f;
	const float grow_all = behind ? 1.0f : S.dcc_dir > 0 ? std::min(1.0f, S.dcc / 14.0f) : S.dcc_dir < 0 ? std::max(0.0f, (S.dcc - 10) / 10.0f) : 1.0f;
	const int first = std::max(0, (static_cast<int>(std::floor(S.scroll)) - 1) * cols);
	const int last = std::min(n - 1, (static_cast<int>(std::ceil(S.scroll)) + rows + 1) * cols - 1);
	const float phi = static_cast<float>(S.Fr) * 0.013f;
	float sel_x = -1, sel_y = 0, sel_h = 0;
	for (int i = first; i <= last; i++)
	{
		const float row = static_cast<float>(i / cols) - S.scroll;
		if (row < -0.9f || row > rows - 0.1f)
			continue;
		const float vis = std::clamp(1.0f - std::max(-row, row - (rows - 1)) * 1.2f, 0.0f, 1.0f);
		const float x = -cx + 15 + (i % cols) * px, y = -cy + 14 + row * py + (rule ? 0.0f : S.large ? 5.4f : 1.85f); // (even room above the first row and under the last) // (round 3: the bigger cases start under the header)
		float fx, fy, zc;
		S3PagePoint(cam, x, y, 0, fx, fy, zc);
		const float h_px = case_h * 553.47f / zc * kit.SY() * grow_all * vis;
		if (h_px < 1)
			continue;
		const float yaw = 0.25f * std::cos(phi + i * 0.7f) * 0.6f + (i == m_selected ? S.case_spin : i == S.prev_spin_pos ? S.prev_spin : 0.0f),
			pitch = 0.06f * std::sin(2 * phi + i * 0.7f) + 0.08f;
		if (m_index[static_cast<size_t>(i)] == kDumpIndex)
		{
			// The game being copied: a dimmed blank case with the copy's bar under it.
			const float w = h_px * kBoxHalfW / kBoxHalfH, cxp = kit.X(fx), cyp = kit.Y(fy);
			kit.FillPx(cxp - w / 2, cyp - h_px / 2, cxp + w / 2, cyp + h_px / 2, 0.16f, 0.16f, 0.18f, 0.75f * vis);
			kit.FillPx(cxp - w / 2 + 3, cyp - h_px / 2 + 3, cxp + w / 2 - 3, cyp - h_px / 2 + h_px * 0.08f, 0.05f, 0.05f, 0.06f, 0.75f * vis);
			const float by = cyp + h_px / 2 + kit.SY() * 2, bh = kit.SY() * 1.4f;
			kit.FillPx(cxp - w / 2, by, cxp + w / 2, by + bh, 0.05f, 0.05f, 0.06f, 0.8f * vis);
			kit.FillPx(cxp - w / 2, by, cxp - w / 2 + w * std::clamp(m_dump_pct, 0, 100) / 100.0f, by + bh, 47 / 128.0f, 87 / 128.0f, 127 / 128.0f, vis);
		}
		else
		{
			if (behind)
			{
				// Behind the game page: the cover flat (under that page's backdrop, which only its own case comes over).
				const Slot& sl = m_slots[static_cast<size_t>(i)];
				if (sl.set)
				{
					const float w = h_px * kBoxHalfW / kBoxHalfH;
					kit.Bind(sl.set);
					kit.Quad(kit.X(fx) - w / 2, kit.Y(fy) - h_px / 2, kit.X(fx) + w / 2, kit.Y(fy) + h_px / 2, 0, 0, 1, 1,
						s3::Kit::Pack(0.5f, 0.5f, 0.5f, 0.22f * vis), 9, false); // (faint: the BIOS's page is blurred under it, not busy)
				}
			}
			else
				S3DrawCase(i, kit.X(fx), kit.Y(fy), h_px, yaw, pitch, vis, false);
		}
		if (i == m_selected)
			sel_x = kit.X(fx), sel_y = kit.Y(fy), sel_h = h_px;
	}
	if (sel_x >= 0 && !behind && !S.options_open)
		// The flare, as the BIOS's over a card's icon, but toned down over a cover so the game stays readable (round 2): the halo at
		// about half the grey, centred under the case's lower half.
		kit.Flare(sel_x, sel_y + sel_h * 0.25f, S.flare_h / 16.0f * grow_all * 1.1f, 58, 128);
	// Empty: the BIOS's "There is no data." with the wandering dim-blue light.
	if (n == 0 && !behind)
	{
		kit.Flare(kit.X(320 + 1024 * S.wander[0] / 200), kit.Y(128 + 553.47f * S.wander[1] / 200), 40, 60, 60);
	}
	// The scroll arrows (browser.md 6.2: up/down while rows lie above/below): blue triangles at the centre, as measured on a PS2
	// (up 33 x 21 px over the top row, down 48 x 36 px under the bottom row at 1080 lines).
	if (!behind && n > 0)
	{
		S3ScrollArrows(kit, S.lib_arrows, S.top > 0, (S.top + rows) * cols < n, S.Fr);
	}
	kit.Bands();
	// Header: the card ico (blue finish: the Library) and "Library / n Games".
	const int ta = behind ? 0 : (S.dcc > 8 ? 127 * (S.dcc - 8) / 13 : 0);
	if (const s3::Ico* card = S.bios.Icon("ICOBPS2M"))
	{
		// The header's icon (as the card page's, 15.2): the Library's own object (round 2), or the card ico.
		M4 model = MMul(MTrans(0, 0, 400), MMul(MRotX(kPi_ / 4), MTrans(0, 10, 0)));
		const float hx = kit.Ox(320 - 271) - 320;
		const float a = behind ? 0.35f : std::max(0.15f, ta / 127.0f);
		if (S.lib_kind < 3 || S.lib_kind >= 4)
			kit.DrawIco(S.lib_ico[S.lib_kind < 3 ? S.lib_kind : 1], model.m, 320 + hx, 128 - 81 * 1.15f, S3CardLights(0.24f), 0, nullptr, a);
		else
			kit.DrawIco(*card, model.m, 320 + hx, 128 - 81 * 1.15f, S3CardLights(0.24f), static_cast<float>(S.Fr), &kit.Cached("bios:ICOBPS2M", card->tex), a);
	}
	if (behind)
		return;
	kit.MarkScene();
	kit.Text("Library", kit.Ox(76), 34, s3::kBrowserText, 0.77f, 0, ta);
	std::string line2;
	{
		char b[48];
		if (!S.query.empty() && (S.searching || !m_search.applied.empty()))
			std::snprintf(b, sizeof(b), "%d Found", n);
		else if (S.filter == 2)
			std::snprintf(b, sizeof(b), "%d Favourite%s", n, n == 1 ? "" : "s");
		else if (S.filter == 1)
			std::snprintf(b, sizeof(b), "%d Recent", n);
		else
			std::snprintf(b, sizeof(b), "%d Game%s", n, n == 1 ? "" : "s");
		line2 = b;
	}
	kit.Text(line2, kit.Ox(76), 48, s3::kBrowserText, 0.6f, 0, ta);
	// The focused title top-right (2 lines, yellow); while searching the query sits there with the first match under it.
	if (S.searching)
	{
		kit.Text(S.query + "_", kit.Ox(616), 34, s3::kYellow, 0.8f, 2, 128);
		if (n > 0)
			kit.Text(S3Ascii(m_games[0].title), kit.Ox(616), 48, s3::kBrowserText, kit.FitScale(S3Ascii(m_games[0].title), 0.8f, 330), 2, 128);
	}
	else if (n > 0)
	{
		std::string a, b;
		S3Split(kit, S3Ascii(m_games[static_cast<size_t>(m_selected)].title), 0.8f, 330, a, b);
		if (m_index[static_cast<size_t>(m_selected)] == kDumpIndex)
			a = S3Ascii(m_dump_title), b = "Copying " + std::to_string(m_dump_pct) + "%";
		kit.Text(a, kit.Ox(616), 34, s3::kYellow, kit.FitScale(a, 0.8f, 330), 2, ta);
		if (!b.empty())
			kit.Text(b, kit.Ox(616), 48, s3::kYellow, kit.FitScale(b, 0.8f, 330), 2, ta);
	}
	if (n == 0)
	{
		const std::string none = S.filter == 2 ? "There is no data." : "There is no data.";
		const float nx = S.searching ? kit.Ox(470) : 320.0f; // beside the keypad while searching
		kit.Text(none, nx, 104 * 1.15f, s3::kBrowserText, kit.FitScale(none, 1.0f, 592), 1, ta);
		std::string where = m_cfg.games_folder_note.empty() ? std::string("Put disc images (.iso, .chd) in /data/PCSX2/games or a USB drive.")
		                                                   : "Put disc images (.iso, .chd) in " + m_cfg.games_folder_note + ".";
		if (S.filter != 0)
			where = "Triangle: Options, Show All.";
		if (!m_search.applied.empty())
			where = "No game matches \"" + m_search.text + "\". Circle: every game.";
		if (!S.searching)
			kit.Text(S3Ascii(where), 320, 104 * 1.15f + 18, s3::kBrowserText, kit.FitScale(S3Ascii(where), 0.6f, 600), 1, ta);
	}
	if (S.searching)
	{
		// The keypad: BIOS glyphs, the focused key cyan with the flare under it.
		const float x0 = kit.Ox(36), y0 = 82;
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 10; c++)
			{
				const char k = kKeys[r][c];
				const std::string s = k == ' ' ? std::string("_") : std::string(1, k);
				const bool on = r == S.key_row && c == S.key_col;
				const float kx = x0 + c * 23.0f, ky = y0 + r * 30.0f;
				if (on)
					kit.Flare(kit.X(kx + 8), kit.Y(ky + 4), 22, 118, 128);
				kit.Text(s, kx + 8, ky, on ? s3::kCyan : s3::kBrowserText, 0.9f, 1, 128);
			}
		kit.Hints({{s3::Button::Square, "Delete"}, {s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}, {s3::Button::Triangle, "Done"}});
	}
	else if (S.options_open)
	{
		kit.FillPx(0, kit.Y(32), kit.W(), kit.Y(224), 40 / 255.0f, 40 / 255.0f, 120 / 255.0f, 0.9f);
		f.blur = 1.3f;
		kit.Text("Options", 432, 92, s3::kYellow, 1.0f, 1);
		const char* labels[2] = {"Show", "Sort"};
		const char* vals[2][3] = {{"All", "Recent", "Favourites"}, {"A-Z", "Last Played", "Play Time"}};
		for (int r = 0; r < 2; r++)
		{
			const float y = 125.0f + r * 30.0f;
			kit.Text(labels[r], 432, y - 12, r == S.options_row ? s3::kCyan : s3::kBrowserText, 0.8f, 1);
			const int cur = r == 0 ? S.filter : S.sort;
			std::string line;
			int w = 0;
			for (int k = 0; k < 3; k++)
				w += kit.TextWidth(vals[r][k], 0.8f) + (k < 2 ? kit.TextWidth("    ", 0.8f) : 0);
			float x = 432 - w / 2.0f;
			for (int k = 0; k < 3; k++)
			{
				kit.Text(vals[r][k], x, y + 4, k == cur ? s3::kBlue : s3::kDark, 0.8f, 0);
				x += kit.TextWidth(vals[r][k], 0.8f) + kit.TextWidth("    ", 0.8f);
			}
		}
		kit.Hints({{s3::Button::Circle, "Back"}});
	}
	else if (n > 0)
		kit.Hints({{s3::Button::Square, "Search"}, {s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}, {s3::Button::Triangle, "Options"}}, ta);
	else
		kit.Hints({{s3::Button::Square, "Search"}, {s3::Button::Circle, "Back"}, {s3::Button::Triangle, "Options"}}, ta);
}

// ---- a game's page (the Browser's save menu, browser.md 7, 15.4) ----

void App::S3GameOpen(int pos)
{
	s3::Shelf3& S = m_s3;
	if (pos < 0 || pos >= static_cast<int>(m_games.size()))
		return;
	Step(pos - m_selected);
	S.g_index = m_index[static_cast<size_t>(pos)];
	S.g_cur = 0, S.g_confirm = false, S.g_dd0 = 0;
	S.big_rot = 0;
	S3ReadRecords();
	// The save states: savestates/<serial> (<crc>).<nn>.p2s and .resume.p2s (Auto), newest first.
	S.states.clear();
	const GameInfo& g = m_games[static_cast<size_t>(pos)];
	// Its discs (a multi-disc game is one entry): the set, disc 1 picked; each disc's serial from the entries kept off the shelf.
	S.g_discs = {g.path};
	if (DiscNumber(g.file, nullptr) > 0)
	{
		auto it = S.disc_sets.find(g.path);
		if (it == S.disc_sets.end())
			it = S.disc_sets.emplace(g.path, DiscSet(g.path)).first;
		if (it->second.size() > 1)
			S.g_discs = it->second;
	}
	S.g_disc = 0, S.launch_index = -1;
	S.g_disc_serials.assign(S.g_discs.size(), std::string());
	S.g_disc_serials[0] = g.serial;
	for (size_t k = 1; k < S.g_discs.size(); k++)
		for (const Shelved& h : m_shelved)
			if (h.game.path == S.g_discs[k])
				S.g_disc_serials[k] = h.game.serial;
	for (size_t dk = 0; dk < S.g_discs.size(); dk++)
	if (!S.g_disc_serials[dk].empty() && (dk == 0 || S.g_disc_serials[dk] != g.serial))
		if (DIR* d = opendir(S.states_dir.c_str()))
		{
			const std::string pre = S.g_disc_serials[dk] + " (";
			while (dirent* e = readdir(d))
			{
				const std::string name = e->d_name;
				if (name.rfind(pre, 0) != 0 || name.size() < 5 || name.compare(name.size() - 4, 4, ".p2s") != 0)
					continue;
				s3::State st;
				st.disc = static_cast<int>(dk);
				st.path = S.states_dir + "/" + name;
				const std::string tail = name.substr(name.find(')') + 1);
				if (tail == ".resume.p2s")
					st.slot = 0;
				else if (tail.size() == 7 && tail[0] == '.') // ".NN.p2s"
					st.slot = std::atoi(tail.c_str() + 1);
				else
					continue;
				struct stat fs;
				if (stat(st.path.c_str(), &fs) != 0 || fs.st_size <= 0)
					continue;
				st.mtime = static_cast<int64_t>(fs.st_mtime);
				S.states.push_back(st);
			}
			closedir(d);
		}
	std::sort(S.states.begin(), S.states.end(), [](const s3::State& a, const s3::State& b) { return a.mtime > b.mtime; });
	if (S.states.size() > 19)
		S.states.resize(19);
	s3::State fresh;
	fresh.slot = -1;
	S.states.insert(S.states.begin(), fresh);
	// Play's choice (kurolume: the game's own saves first, a save state only if wanted): the newest save state, any slot or Auto, of any disc.
	S.g_resume = S.states.size() > 1 ? 1 : -1;
	// The game's own save icon from the memory card.
	S.g_has_ico = false;
	S.g_sys = s3::IconSys();
	if (m_time - S.card_read > 5.0 || !S.card.ok())
		S3ReadCard();
	for (const s3::Save* sv : S.card.ForSerial(g.serial))
		if (!sv->ico.empty() && s3::ParseIco(sv->ico.data(), sv->ico.size(), S.g_ico))
		{
			S.g_has_ico = true;
			S.g_sys = sv->sys;
			S.kit.DropCached("game:");
			break;
		}
	S.scr = s3::Scr::Game;
	S.fade = 0;
}

void App::S3Launch(const std::string& state)
{
	s3::Shelf3& S = m_s3;
	S.chosen_state = state;
	// The disc that starts: a save state's own, else the one picked on the game page; another than the entry's starts by its own
	// (shelved) entry, so the launch, the settings and the in-game disc set all follow that image.
	int disc = S.g_disc;
	for (const s3::State& st : S.states)
		if (!state.empty() && st.path == state)
			disc = st.disc;
	S.launch_index = -1;
	std::string disc_path = m_games[static_cast<size_t>(m_selected)].path;
	if (disc > 0 && disc < static_cast<int>(S.g_discs.size()))
		for (const Shelved& h : m_shelved)
			if (h.game.path == S.g_discs[static_cast<size_t>(disc)])
				S.launch_index = h.index, disc_path = h.game.path;
	mkdir((S.top_dir + "/logs").c_str(), 0777);
	const std::string note = S.top_dir + "/logs/shelf-state.txt";
	if (state.empty())
		unlink(note.c_str());
	else if (FILE* f = std::fopen(note.c_str(), "w"))
	{
		std::fprintf(f, "%s\n%s\n", disc_path.c_str(), state.c_str());
		std::fclose(f);
	}
	if (!BiosReady(BiosWait::Pick))
		return;
	if (m_cfg.game_achievements.cancel)
		m_cfg.game_achievements.cancel();
	m_launching = true;
	m_launch_time = m_time;
	std::printf("[shelf3] start %s from %s\n", m_games[static_cast<size_t>(m_selected)].title.c_str(), state.empty() ? "a clean boot" : state.c_str());
	std::fflush(stdout);
}

void App::S3GameTick(const Input& in)
{
	(void)in;
	s3::Shelf3& S = m_s3;
	const S3Pad& p = g_s3pad;
	S.g_dd0 = std::min(25, S.g_dd0 + 1);
	S.big_rot += 0.008f;
	S.flare_phase += 0.03f;
	S.flare_h += (600.0f + 120.0f * std::sin(S.flare_phase) - S.flare_h) / 44.0f;
	if (m_launching || m_games.empty())
		return;
	const GameInfo& g = m_games[static_cast<size_t>(m_selected)];
	if (S.g_confirm)
	{
		// Play with save states: "Start normally" (the memory card, first) or "Load save state" with Left/Right through the states (newest
		// first; its own disc). The states are kept either way.
		const int ns = static_cast<int>(S.states.size());
		if (p.fresh(kBUp) && S.yes_no != 0)
			S.yes_no = 0, S3Sfx(6);
		else if (p.fresh(kBDown) && S.yes_no != 1)
			S.yes_no = 1, S3Sfx(6);
		else if (S.yes_no == 1 && ns > 2 && (p.fresh(kBLeft) || p.fresh(kBRight)))
		{
			const int k = S.g_resume - 1 + (p.fresh(kBLeft) ? ns - 2 : 1); // 1 .. ns-1, wrapping
			S.g_resume = 1 + k % (ns - 1);
			S3Sfx(6);
		}
		else if (p.fresh(kBCross))
		{
			S3Sfx(4);
			// (The choice stays on screen while the game starts: clearing it here flashed the menu back for the launch fade. S3GameOpen
			// clears it.)
			if (S.yes_no == 1 && S.g_resume >= 1 && S.g_resume < ns)
				S3Launch(S.states[static_cast<size_t>(S.g_resume)].path);
			else
				S3Launch(std::string());
			if (m_launching)
				S.launch_fade_t0 = m_time; // the page fades to black (S3Build), then the game
		}
		else if (p.fresh(kBCircle))
			S.g_confirm = false, S3Sfx(10);
		return;
	}
	// The options: Play, Favourite, Settings; a multi-disc game: Left/Right on Play pick the disc it boots.
	const int nd = static_cast<int>(S.g_discs.size());
	const int nopt = 3;
	const int id = S.g_cur == 0 ? 0 : S.g_cur + 1; // 0 Play, 2 Favourite, 3 Settings
	if (p.fresh(kBUp) && S.g_cur > 0)
		S.g_cur--, S3Sfx(6);
	else if (p.fresh(kBDown) && S.g_cur < nopt - 1)
		S.g_cur++, S3Sfx(6);
	else if (id == 0 && nd > 1 && (p.fresh(kBLeft) || p.fresh(kBRight)))
		S.g_disc = (S.g_disc + (p.fresh(kBLeft) ? nd - 1 : 1)) % nd, S3Sfx(6); // which disc Play boots
	else if (p.fresh(kBCross))
	{
		S3Sfx(4);
		if (id == 0 && S.states.size() > 1)
			S.g_confirm = true, S.yes_no = 0, S.g_resume = 1; // save states exist: the choice, Start normally first
		else if (id == 0)
		{
			S3Launch(std::string()); // a clean boot
			if (m_launching)
				S.launch_fade_t0 = m_time; // the page fades to black (S3Build), then the game
		}
		else if (id == 2)
			S3SetFavourite(g.serial, !S3Favourite(g.serial));
		else
		{
			S.settings_from = s3::Scr::Game;
			S3SettingsOpen(false);
		}
	}
	else if (p.fresh(kBCircle))
	{
		S3Sfx(10);
		S.scr = s3::Scr::Library;
		S.dcc = 21, S.dcc_dir = 0;
		if (g_s3_flat_set)
			m_renderer->FreeTextureSet(g_s3_flat_set), g_s3_flat_set = VK_NULL_HANDLE, g_s3_flat_src = nullptr;
	}
}

void App::S3GameBuild(FrameDesc& f)
{
	s3::Shelf3& S = m_s3;
	s3::Kit& kit = S.kit;
	// The Library behind (its covers flat, under the backdrop). Round 3: no soft focus here, so the game's own case can be the Library's
	// very case model (the renderer's box, lit the same) at the front, sharp.
	S3LibraryBuild(f, true);
	f.blur = 0.0f;
	if (m_games.empty())
	{
		kit.MarkUnder();
		kit.MarkScene();
		return;
	}
	const GameInfo& g = m_games[static_cast<size_t>(m_selected)];
	// The icon.sys backdrop (2.4): the game's own card save colours at its alpha, else dark blue (20,20,60) at 115.
	{
		float bg[4][3];
		float a;
		if (S.g_has_ico && S.g_sys.ok && S.g_sys.bg_alpha > 0)
		{
			for (int c = 0; c < 4; c++)
				for (int k = 0; k < 3; k++)
					bg[c][k] = std::min(255.0f, S.g_sys.bg[c][k]); // direct, as the BIOS's backdrop quad
			a = S.g_sys.bg_alpha / 128.0f;
		}
		else
		{
			for (int c = 0; c < 4; c++)
				bg[c][0] = bg[c][1] = 20, bg[c][2] = 60;
			a = 115.0f / 128.0f;
		}
		kit.Gradient(0, kit.Y(32), kit.W(), kit.Y(224), bg, a * S.g_dd0 / 25.0f);
	}
	kit.MarkUnder();
	const int da = std::clamp(255 * (S.g_dd0 - 12) / 25, 0, 128);
	// The case, left of the text block: the Library's case model (one renderer, the same materials and light), larger, a rigid box however
	// it turns. (The save row under it is gone: save states load from the in-game menu, the Auto one from Play's question.)
	S3DrawCase(m_selected, kit.X(152), kit.Y(122), 128 * kit.SY(),
		-0.22f + 0.05f * std::sin(static_cast<float>(S.Fr) * 0.01f) + S.case_spin, 0.04f, S.g_dd0 / 25.0f, false);
	kit.MarkScene();
	// The texts (15.4): "Library", the title in two yellow lines, the date line, the options.
	kit.Text("Library", 432, 41 * 1.15f, s3::kBrowserText, 0.77f, 1, da);
	std::string a, b;
	S3Split(kit, S3Ascii(g.title), 0.875f, 300, a, b);
	const bool two = !b.empty();
	if (S3Favourite(g.serial))
	{
		// The favourite mark (ours, round 4): a star in the title's yellow just before its first line, as tall as its capitals (a glyph's
		// capitals span y + 2.2 s .. y + 13.85 s) and centred on them, a space's width from the first letter.
		const float ty = two ? 55 * 1.15f : 56 * 1.15f, ts = two ? kit.FitScale(a, 0.875f, 300) : kit.FitScale(a, 1.0f, 300);
		const float cap_top = ty + 2.2f * ts, cap_h = 11.65f * ts;
		// The yellow (its inner 0.86, inside the dark rim as the letters' outline) as tall as the capitals: (1 + sin 54) r, y squashed by
		// 0.53 (S3Star).
		const float r = cap_h / (1.809f * 0.53f * 0.86f);
		const float right = 432 - kit.TextWidth(a, ts) / 2.0f - kit.TextWidth(" ", ts) + 0.951f * r * 0.14f; // (the yellow's edge)
		const float cy = cap_top + cap_h / 2.0f + 0.0955f * r * 0.53f; // its bounding box's middle on the capitals' middle
		S3Star(kit, right - 0.951f * r, cy, r, true, da, s3::kYellow, 0.86f);
	}
	if (two)
	{
		kit.Text(a, 432, 55 * 1.15f, s3::kYellow, kit.FitScale(a, 0.875f, 300), 1, da);
		kit.Text(b, 432, 69 * 1.15f, s3::kYellow, kit.FitScale(b, 0.875f, 300), 1, da);
	}
	else
		kit.Text(a, 432, 56 * 1.15f, s3::kYellow, kit.FitScale(a, 1.0f, 300), 1, da);
	std::string line;
	{
		// (a multi-disc game: all its discs' play time, the latest of their dates)
		int64_t last = 0;
		uint64_t secs = 0;
		for (size_t k = 0; k < std::max<size_t>(1, S.g_disc_serials.size()); k++)
		{
			const std::string& ser = k < S.g_disc_serials.size() ? S.g_disc_serials[k] : g.serial;
			if (k > 0 && ser == g.serial)
				continue;
			auto it = m_s3.played.find(ser);
			if (it != m_s3.played.end())
				last = std::max(last, it->second.last), secs += it->second.seconds;
		}
		if (last > 0 || secs > 0) // never played: no date line at all
			line = S3DateText(last) + "   " + S3Duration(secs);
		if (S.g_discs.size() > 1) // and how many discs
			line += (line.empty() ? "" : "   ") + std::string("Disc ") + std::to_string(S.g_disc + 1) + "/" + std::to_string(S.g_discs.size());
	}
	// The last line over the options (the date line, else the title's last line) and its scale.
	float last_y = two ? 69 * 1.15f : 56 * 1.15f, last_s = two ? kit.FitScale(b, 0.875f, 300) : kit.FitScale(a, 1.0f, 300);
	if (!line.empty())
	{
		kit.Text(line, 432, (two ? 86 : 78) * 1.15f, s3::kBrowserText, 0.65f, 1, da);
		last_y = (two ? 86 : 78) * 1.15f, last_s = 0.65f;
	}
	// The options follow the lines above as one group (round 4, kurolume: the save menu's fixed y 121 x 1.15 left them too low under the
	// title): Play's capitals start 11 units under the last line's (a glyph's capitals span y + 2.2 s .. y + 13.85 s), then 22 x 1.15 steps.
	const float opt_y0 = last_y + 13.85f * last_s + 26.0f - 2.2f; // (26: between the old fixed place and 11, kurolume round 5)
	if (S.g_confirm && S.g_resume >= 1 && S.g_resume < static_cast<int>(S.states.size()))
	{
		// Play's choice in the options' place, at their pitch: Start normally, then Load save state with the state (Left/Right).
		const s3::State& st = S.states[static_cast<size_t>(S.g_resume)];
		std::string when = S3DateText(st.mtime);
		{
			const std::time_t tt = static_cast<std::time_t>(st.mtime);
			std::tm tm = {};
			localtime_r(&tt, &tm);
			char b[16];
			std::snprintf(b, sizeof(b), "  %02d:%02d", tm.tm_hour, tm.tm_min);
			when += b;
		}
		const std::string slot = st.slot == 0 ? std::string("Auto") : "Slot " + std::to_string(st.slot);
		const bool more = S.states.size() > 2;
		const std::string l2 = "Load save state";
		const std::string l3 = (more ? std::string(1, s3::kSymLeft) + " " : std::string()) + slot + "  " + when + (more ? " " + std::string(1, s3::kSymRight) : std::string());
		const float step = 22.0f * 1.15f;
		kit.Text("Start normally", 432, opt_y0, S.yes_no == 0 ? s3::kCyan : s3::kDark, kit.FitScale("Start normally", 1.0f, 380), 1, 128);
		kit.Text(l2, 432, opt_y0 + step, S.yes_no == 1 ? s3::kCyan : s3::kDark, kit.FitScale(l2, 1.0f, 380), 1, 128);
		kit.Text(l3, 432, opt_y0 + step + 19.0f * 1.15f, S.yes_no == 1 ? s3::kBrowserText : s3::kDark, 0.65f, 1, 128);
		kit.Hints({{s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}});
		return;
	}
	// Play, Favourite, Settings at the BIOS's place and pitch. A multi-disc game: focused Play shows the disc it boots, Left/Right change it.
	const int nd = static_cast<int>(S.g_discs.size());
	const bool disc_pick = nd > 1 && S.g_cur == 0;
	std::vector<std::pair<int, std::string>> opts = {{0, disc_pick ? "Play  " + std::string(1, s3::kSymLeft) + " Disc " + std::to_string(S.g_disc + 1) + "/" +
		std::to_string(nd) + " " + std::string(1, s3::kSymRight) : std::string("Play")}};
	opts.push_back({2, "Favourite"});
	opts.push_back({3, "Settings"});
	const float step = 22.0f * 1.15f;
	for (int i = 0; i < static_cast<int>(opts.size()); i++)
	{
		const std::string& label = opts[static_cast<size_t>(i)].second;
		const s3::Gs c = i == S.g_cur ? s3::kCyan : s3::kDark;
		const float y = opt_y0 + step * i;
		kit.Text(label, 428, y, c, 1.0f, 1, da);
	}
	kit.Hints({{s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}}, da);
}

// ---- settings: System Configuration (sysconf.md 3-7) with L1/R1 tabs ----

namespace
{
const char* kVideoGroups[] = {"Display", "Graphics", "Crop", "On screen", "Hardware fixes"};
bool S3IsGameRow(const OptionsSheet::Row& r)
{
	if (r.kind == OptionsSheet::Kind::TexturePack)
		return true;
	return r.kind == OptionsSheet::Kind::Option && r.def &&
	       (r.def->key == "PS5SX2/AutoSave" || r.def->key == "EmuCore/EnableFastBoot" || r.def->key == "EmuCore/EnableWideScreenPatches");
}
bool S3IsOursRow(const OptionsSheet::Row& r)
{
	return r.kind == OptionsSheet::Kind::Option && r.def &&
	       (r.def->key == "PS5SX2/ShelfLayout" || r.def->key == "PS5SX2/DiscSlot" || r.def->key == "PS5SX2/ShelfShape");
}
} // namespace

void App::S3SettingsOpen(bool global)
{
	s3::Shelf3& S = m_s3;
	OpenSheet(global);
	S.st_global = m_sheet_global;
	S.st_tab = 0, S.st_row = 0, S.st_edit = false, S.st_confirm = 0, S.st_open = 0;
	S.prev_item = -1, S.item_alpha = 128, S.prev_alpha = 0;
	S.scr = s3::Scr::Settings;
	S.fade = 0;
}

void App::S3SettingsTick(const Input& in)
{
	(void)in;
	s3::Shelf3& S = m_s3;
	const S3Pad& p = g_s3pad;
	if (S.st_closing)
	{
		// The close (sysconf.md 3): the text goes in 8 frames, the cubes shrink, the wall darkens; then the screen behind.
		S.st_open = std::max(0, S.st_open - 3);
		if (S.st_open <= 0)
			S3SettingsLeave();
		return;
	}
	S.st_open = std::min(74, S.st_open + 1);
	S.item_alpha = std::min(128, S.item_alpha + 8);
	S.prev_alpha = std::max(0, S.prev_alpha - 8);
	if (S.st_ring_anim > 0)
		S.st_ring_anim--;
	// The tabs and their rows.
	S.st_tab_names.clear();
	if (!S.st_global)
		S.st_tab_names.push_back("Game");
	S.st_tab_names.insert(S.st_tab_names.end(), {"Video", "Audio", "Controls", "System"});
	S.st_tab = std::clamp(S.st_tab, 0, static_cast<int>(S.st_tab_names.size()) - 1);
	const std::string tab = S.st_tab_names[static_cast<size_t>(S.st_tab)];
	const int want_tab = tab == "Controls" ? kTabControls : kTabSettings;
	if (m_sheet.tab() != want_tab)
		m_sheet.SetTab(want_tab);
	std::vector<int> rows;
	{
		const auto& all = m_sheet.rows();
		std::string group;
		std::vector<int> ours;
		for (size_t i = 0; i < all.size(); i++)
		{
			const OptionsSheet::Row& r = all[i];
			if (r.kind == OptionsSheet::Kind::Header)
			{
				group = r.label;
				continue;
			}
			const bool video = std::find_if(std::begin(kVideoGroups), std::end(kVideoGroups), [&](const char* g) { return group == g; }) != std::end(kVideoGroups);
			bool take = false;
			if (tab == "Game")
				take = S3IsGameRow(r);
			else if (tab == "Controls")
				take = true;
			else if (tab == "Video")
				take = video && !(S3IsGameRow(r) && !S.st_global);
			else if (tab == "Audio")
				take = group == "Audio";
			else
				take = !video && group != "Audio" && !(S3IsGameRow(r) && !S.st_global) && r.kind != OptionsSheet::Kind::ResetAll;
			if (take && S3IsOursRow(r))
				ours.push_back(static_cast<int>(i));
			else if (take)
				rows.push_back(static_cast<int>(i));
		}
		rows.insert(rows.begin(), ours.begin(), ours.end()); // ours on top of System
		if (tab == "Game") // the design's order: Auto Save, Fast Boot, Widescreen, HD Textures
		{
			auto rank = [&](int i) {
				const OptionsSheet::Row& r = all[static_cast<size_t>(i)];
				if (r.kind == OptionsSheet::Kind::TexturePack)
					return 3;
				const std::string& k = r.def ? r.def->key : std::string();
				return k == "PS5SX2/AutoSave" ? 0 : k == "EmuCore/EnableFastBoot" ? 1 : 2;
			};
			std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) { return rank(a) < rank(b); });
		}
	}
	S.st_rows.assign(1, rows);
	const int n = static_cast<int>(rows.size());
	S.st_row = std::clamp(S.st_row, 0, std::max(0, n - 1));
	if (S.st_open < 74)
		return;
	const double now = m_time;
	if (S.st_confirm == 1)
	{
		if (p.fresh(kBLeft) && S.yes_no != 0)
			S.yes_no = 0, S3Sfx(6);
		else if (p.fresh(kBRight) && S.yes_no != 1)
			S.yes_no = 1, S3Sfx(6);
		else if (p.fresh(kBCross))
		{
			S3Sfx(4);
			if (S.yes_no == 0)
			{
				for (int t : {kTabSettings, kTabControls})
				{
					m_sheet.SetTab(t);
					const auto all = m_sheet.rows();
					for (const OptionsSheet::Row& r : all)
						if (r.kind == OptionsSheet::Kind::Option && m_sheet.Source(r) == OptionsSheet::From::Own)
							m_sheet.Reset(r);
				}
				m_sheet.SetTab(want_tab);
				S3Sfx(9);
			}
			S.st_confirm = 0;
		}
		else if (p.fresh(kBCircle))
			S.st_confirm = 0, S3Sfx(10);
		return;
	}
	const OptionsSheet::Row* row = n > 0 ? &m_sheet.rows()[static_cast<size_t>(rows[static_cast<size_t>(S.st_row)])] : nullptr;
	if (S.st_edit && row)
	{
		if (p.fresh(kBLeft) || p.fresh(kBRight))
		{
			if (m_sheet.Step(*row, p.fresh(kBLeft) ? -1 : 1))
				S3Sfx(6);
		}
		else if (p.fresh(kBCross))
		{
			S.st_edit = false, S3Sfx(4);
			if (row->def && (row->def->key == "PS5SX2/ShelfLayout" || row->def->key == "PS5SX2/ShelfShape" || row->def->key == "PS5SX2/DiscSlot"))
				S3ReadSettings();
		}
		else if (p.fresh(kBCircle))
		{
			// Cancel: back to the value it had.
			for (int guard = 0; guard < 64 && m_sheet.Value(*row) != S.st_edit_was; guard++)
				if (!m_sheet.Step(*row, 1))
					break;
			S.st_edit = false, S3Sfx(10);
		}
		return;
	}
	if (p.fresh(kBL1) || p.fresh(kBR1))
	{
		const int nt = static_cast<int>(S.st_tab_names.size());
		S.st_tab = (S.st_tab + (p.fresh(kBL1) ? nt - 1 : 1)) % nt;
		S.st_row = 0;
		S.prev_item = -1;
		S3Sfx(5);
		return;
	}
	if (n > 0 && (p.fresh(kBUp) || p.fresh(kBDown)))
	{
		S.prev_item = S.st_row;
		S.prev_alpha = S.item_alpha;
		S.item_alpha = 0;
		S.st_ring_dir = p.fresh(kBUp) ? -1 : 1;
		S.st_row = (S.st_row + (p.fresh(kBUp) ? n - 1 : 1)) % n;
		S.st_ring_pos = (S.st_ring_pos + S.st_ring_dir + 60) % 60; // the wheel's own slot (60 of them): it turns on through a wrap
		S.st_ring_anim = 19;
		S3Sfx(5);
	}
	else if (row && p.fresh(kBCross))
	{
		using K = OptionsSheet::Kind;
		switch (row->kind)
		{
			case K::Option:
				S.st_edit = true;
				S.st_edit_was = m_sheet.Value(*row);
				S3Sfx(4);
				break;
			case K::GameFolders:
			case K::BiosFolder:
			case K::NfsShares:
			case K::TexturesFolder:
				OpenPicker(row->kind);
				S3Sfx(4);
				break;
			case K::WebSettings:
				m_qr_big = true;
				S3Sfx(4);
				break;
			default:
				S3Sfx(m_sheet.Activate(*row, now) ? 4 : 6);
				if (m_sheet.TakeSystemMenu())
				{
					CloseSheet();
					if (BiosReady(BiosWait::SystemMenu))
					{
						m_system_menu = true;
						m_launching = true;
						m_launch_time = m_time;
					}
				}
				break;
		}
	}
	else if (row && p.fresh(kBSquare) && !S.st_global && row->kind == OptionsSheet::Kind::Option)
	{
		if (m_sheet.Reset(*row))
			S3Sfx(4);
	}
	else if (p.fresh(kBTriangle) && !S.st_global)
		S.st_confirm = 1, S.yes_no = 1, S3Sfx(4);
	else if (p.fresh(kBCircle))
	{
		S3Sfx(10);
		S.st_closing = true;
	}
}

void App::S3SettingsLeave()
{
	s3::Shelf3& S = m_s3;
	S.st_closing = false;
	{
		CloseSheet();
		m_sheet_anim = 0;
		S3ReadSettings();
		ApplyHidden(m_games.empty() ? -1 : m_index[static_cast<size_t>(m_selected)]);
		if (S.settings_from == s3::Scr::Game)
		{
			const int pos = PositionOf(S.g_index);
			if (pos >= 0)
			{
				const int keep_cur = S.g_cur;
				S3GameOpen(pos);
				S.g_cur = keep_cur;
				S.g_dd0 = 25;
			}
			else
				S.scr = s3::Scr::Library;
		}
		else if (S.settings_from == s3::Scr::Browser)
			S.scr = s3::Scr::Browser, S.dcc = 21, S.dcc_dir = 0;
		else
		{
			S.scr = s3::Scr::Menu;
			S.menu_mode = 2, S.menu_c = 64, S.menu_words = 0, S.orb_all_fly = false;
		}
	}
}

namespace
{
// ---- System Configuration's glass (sysconf.md 4.2, 7.1), drawn with the frame's own scene as what the glass refracts ----
// A face: 4 vertices in camera space (counter-clockwise seen from outside), its (unnormalised) normal, mesh ST.
struct S3Face
{
	float v[4][3];
	float n[3];
	float st[4][2];
};
struct S3FaceOut
{
	float x[4], y[4];  // field px (absolute)
	float u[4], w[4];  // refraction uv (field px, absolute)
	float ru[4], rv[4]; // reflection uv (0..1)
	bool front;
	float p;
	int T;
};
// Projection of the glass (screen distance 512, ay 0.5405), the facing flag, the view term and its facing add T (k: the gain), the
// refraction uv with the object's centre (cx, cy: its projected origin, relative, times s) and the reflection uv.
S3FaceOut S3GlassFaceOf(const S3Face& f, float k, float cx, float cy)
{
	S3FaceOut o;
	float fx[4], fy[4];
	for (int i = 0; i < 4; i++)
	{
		const float z = std::max(f.v[i][2], 0.5f);
		fx[i] = 512.0f * f.v[i][0] / z, fy[i] = 512.0f * 0.5405f * f.v[i][1] / z;
		o.x[i] = 320 + fx[i], o.y[i] = 128 + fy[i];
		const float q = 1.0f / z;
		o.u[i] = std::max(0.0f, 320 + (fx[i] - cx) * 0.95f + cx - f.n[0] * 1000.0f * q);
		o.w[i] = 128 + (fy[i] - cy) * 0.95f + cy - f.n[1] * 500.0f * q;
		float e[3] = {f.v[i][0], f.v[i][1], f.v[i][2]};
		const float el = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
		for (float& c : e)
			c /= std::max(el, 1e-4f);
		const float d = e[0] * f.n[0] + e[1] * f.n[1] + e[2] * f.n[2];
		const float rx = e[0] - 2 * d * f.n[0], ry = e[1] - 2 * d * f.n[1];
		o.ru[i] = 32.0f * (rx + 1) / 64.0f, o.rv[i] = 16.0f * (ry + 1) / 64.0f;
	}
	o.front = (fx[2] - fx[0]) * (fy[1] - fy[0]) - (fy[2] - fy[0]) * (fx[1] - fx[0]) <= 0;
	float e0[3] = {f.v[0][0], f.v[0][1], f.v[0][2]};
	const float l0 = std::sqrt(e0[0] * e0[0] + e0[1] * e0[1] + e0[2] * e0[2]);
	o.p = 1.0f - std::fabs((e0[0] * f.n[0] + e0[1] * f.n[1] + e0[2] * f.n[2]) / std::max(l0, 1e-4f));
	float T = std::trunc(k * 10.0f * std::pow(std::max(o.p, 0.0f), 4.0f));
	if (o.p > 0.9f)
		T = std::trunc(T * (1.0f - std::cos(kPi_ * (1.0f - o.p) / 0.1f)) / 2.0f);
	o.T = static_cast<int>(T);
	return o;
}
// One face as two textured triangles: the scene refracted, times the lit colour (GS modulate), or a flat colour (scene null).
void S3GlassEmit(s3::Kit& kit, const S3FaceOut& o, VkDescriptorSet tex, int mode, const float* uu, const float* vv, float r, float g, float b, float a,
	bool add, float tex_w, float tex_h)
{
	kit.Bind(tex);
	float xy[8], uv[8];
	for (int i = 0; i < 4; i++)
	{
		xy[i * 2] = kit.X(o.x[i]), xy[i * 2 + 1] = kit.Y(o.y[i]);
		uv[i * 2] = uu ? uu[i] / tex_w : 0, uv[i * 2 + 1] = vv ? vv[i] / tex_h : 0;
	}
	const uint32_t c = s3::Kit::Pack(r, g, b, a);
	const uint32_t cc[3] = {c, c, c};
	const float t1[6] = {xy[0], xy[1], xy[2], xy[3], xy[4], xy[5]}, u1[6] = {uv[0], uv[1], uv[2], uv[3], uv[4], uv[5]};
	const float t2[6] = {xy[0], xy[1], xy[4], xy[5], xy[6], xy[7]}, u2[6] = {uv[0], uv[1], uv[4], uv[5], uv[6], uv[7]};
	kit.Tri(t1, u1, cc, mode, add), kit.Tri(t2, u2, cc, mode, add);
}
// The cube mesh (sysconf.md 7: 6 faces, half-size 2.64), with a model matrix (identity view: camera space).
void S3CubeFaces(const M4& M, S3Face out[6])
{
	static const float kN[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}};
	static const int kV[6][4][3] = {{{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}}, {{1, -1, 1}, {-1, -1, 1}, {-1, 1, 1}, {1, 1, 1}},
		{{-1, -1, 1}, {-1, -1, -1}, {-1, 1, -1}, {-1, 1, 1}}, {{1, -1, -1}, {1, -1, 1}, {1, 1, 1}, {1, 1, -1}},
		{{-1, -1, 1}, {1, -1, 1}, {1, -1, -1}, {-1, -1, -1}}, {{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}}};
	for (int f = 0; f < 6; f++)
	{
		for (int i = 0; i < 4; i++)
			MApply(M, kV[f][i][0] * 2.64f, kV[f][i][1] * 2.64f, kV[f][i][2] * 2.64f, out[f].v[i][0], out[f].v[i][1], out[f].v[i][2]);
		// The normal turned by the model (no renormalising, as the BIOS).
		out[f].n[0] = M.m[0] * kN[f][0] + M.m[1] * kN[f][1] + M.m[2] * kN[f][2];
		out[f].n[1] = M.m[4] * kN[f][0] + M.m[5] * kN[f][1] + M.m[6] * kN[f][2];
		out[f].n[2] = M.m[8] * kN[f][0] + M.m[9] * kN[f][1] + M.m[10] * kN[f][2];
		const float l = std::sqrt(out[f].n[0] * out[f].n[0] + out[f].n[1] * out[f].n[1] + out[f].n[2] * out[f].n[2]);
		for (float& c : out[f].n)
			c /= std::max(l, 1e-4f) / 1.0f;
	}
}
// A glass cube (7.1): the faces seen from inside refract the scene with the lit colour (colour + T), then the faces seen from outside do,
// then the environment map TEXCREFA (120,120,120) on the outside faces, additive; `colour` 0..255 GS (current (0,150,200), others 100).
void S3GlassCube(s3::Kit& kit, VkDescriptorSet scene, float W, float H, const M4& M, const float colour[3], float alpha)
{
	S3Face faces[6];
	S3CubeFaces(M, faces);
	float ox, oy, oz;
	MApply(M, 0, 0, 0, ox, oy, oz);
	const float cx = 0.35f * 512.0f * ox / std::max(oz, 1.0f), cy = 0.35f * 512.0f * 0.5405f * oy / std::max(oz, 1.0f);
	S3FaceOut o[6];
	for (int f = 0; f < 6; f++)
		o[f] = S3GlassFaceOf(faces[f], 200.0f, cx, cy);
	// The scene image's texel size in field px.
	const float tw = W / kit.SX(), th = H / kit.SY();
	const float x0 = -kit.X(0) / kit.SX(); // field x at screen x 0
	for (int pass = 0; pass < 2; pass++)
		for (int f = 0; f < 6; f++)
		{
			if (o[f].front != (pass == 1))
				continue;
			float uu[4], vv[4];
			for (int i = 0; i < 4; i++)
				uu[i] = o[f].u[i] - x0, vv[i] = o[f].w[i];
			const float lr = std::min(255.0f, colour[0] + o[f].T), lg = std::min(255.0f, colour[1] + o[f].T), lb = std::min(255.0f, colour[2] + o[f].T);
			if (scene)
				S3GlassEmit(kit, o[f], scene, 9, uu, vv, lr / 255.0f, lg / 255.0f, lb / 255.0f, 0.5f * alpha, false, tw, th);
			else
				S3GlassEmit(kit, o[f], VK_NULL_HANDLE, 11, nullptr, nullptr, lr / 510.0f, lg / 510.0f, lb / 510.0f, 0.35f * alpha, false, 1, 1);
			// The rim (pass 4): trunc(colour x p^2), seen from inside.
			if (pass == 0)
			{
				const float pp = o[f].p * o[f].p;
				S3GlassEmit(kit, o[f], VK_NULL_HANDLE, 11, nullptr, nullptr, colour[0] * pp / 510.0f, colour[1] * pp / 510.0f, colour[2] * pp / 510.0f,
					0.25f * alpha, true, 1, 1);
			}
		}
	const s3::KitTex& refa = kit.T("TEXCREFA");
	if (refa.set)
		for (int f = 0; f < 6; f++)
			if (o[f].front)
				S3GlassEmit(kit, o[f], refa.set, 9, o[f].ru, o[f].rv, 120 / 255.0f, 120 / 255.0f, 120 / 255.0f, 0.5f * alpha, true, 1, 1);
}

// A rod (sysconf.md 4): our own hexagonal prism (the BIOS's mesh is in its OSD program, not loaded), radius 2.2, from y 0 to 46; drawn in the
// scene (under the soft focus) as tinted glass with the facing add, and the TEXCFLOW glow from the reflected view (x 30/128, additive).
// What a rod refracts (W1: the scene before the menu's tint) is mostly the bright tunnel: its texel and Gouraud colour at a field
// point, from the tunnel's own mapping (mainmenu.md 5.1) run backwards from the vanishing point.
struct S3TunnelLook
{
	float vx, vy, eye_z;
	uint64_t F;
	int wall;
};
void S3TunnelAt(const S3TunnelLook& t, float U, float V, float& u, float& v, float rgb[3])
{
	const float dx = U - t.vx, dy = (V - t.vy) / 0.5405f;
	const float r = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
	const float z = 512.0f * 6000.0f / r + t.eye_z;
	const float j = std::clamp((z + 2500.0f) / 1250.0f, 0.0f, 32.0f);
	const float a = std::atan2(dx, dy);
	u = 3.0f * a / (2 * kPi_);
	v = 3.0f * (j / 32.0f + static_cast<float>(t.F % 5000) * 0.0002f);
	const float cj = std::floor(std::pow(32.0f - j, 3.0f) / 1024.0f), h = std::floor((std::cos(a) + 1.0f) * 10.0f);
	rgb[0] = std::min(255.0f, std::floor(cj * 230.0f / 32.0f) + t.wall + h);
	rgb[1] = rgb[2] = std::min(255.0f, std::floor(cj * 260.0f / 32.0f) + t.wall + h);
}

void S3GlassRod(s3::Kit& kit, const S3TunnelLook& look, const S3Cam& cam, const M4& M, const float colour[3], float k, float alpha)
{
	const int sides = 6;
	const float r = 2.6f, len = 30.0f;
	S3Face faces[8];
	int nf = 0;
	auto to_cam = [&](float x, float y, float z, float* out) {
		float wx, wy, wz;
		MApply(M, x, y, z, wx, wy, wz);
		S3ToCam(cam, wx, wy, wz, out[0], out[1], out[2]);
	};
	auto dir_cam = [&](float x, float y, float z, float* out) {
		const float wx = M.m[0] * x + M.m[1] * y + M.m[2] * z, wy = M.m[4] * x + M.m[5] * y + M.m[6] * z, wz = M.m[8] * x + M.m[9] * y + M.m[10] * z;
		out[0] = cam.r[0] * wx + cam.r[1] * wy + cam.r[2] * wz;
		out[1] = cam.r[3] * wx + cam.r[4] * wy + cam.r[5] * wz;
		out[2] = cam.r[6] * wx + cam.r[7] * wy + cam.r[8] * wz;
	};
	for (int i = 0; i < sides; i++)
	{
		const float a0 = i * 2 * kPi_ / sides, a1 = (i + 1) * 2 * kPi_ / sides, am = (a0 + a1) / 2;
		S3Face& f = faces[nf++];
		to_cam(r * std::cos(a0), 0, r * std::sin(a0), f.v[0]);
		to_cam(r * std::cos(a1), 0, r * std::sin(a1), f.v[1]);
		to_cam(r * std::cos(a1), len, r * std::sin(a1), f.v[2]);
		to_cam(r * std::cos(a0), len, r * std::sin(a0), f.v[3]);
		dir_cam(std::cos(am), 0, std::sin(am), f.n);
	}
	S3FaceOut o[8];
	float ox, oy, oz;
	{
		float c3[3];
		to_cam(0, len * 0.5f, 0, c3);
		ox = c3[0], oy = c3[1], oz = c3[2];
	}
	const float cx = 0.9f * 512.0f * ox / std::max(oz, 1.0f), cy = 0.9f * 512.0f * 0.5405f * oy / std::max(oz, 1.0f);
	for (int f = 0; f < nf; f++)
	{
		bool behind = false;
		for (int i = 0; i < 4; i++)
			behind |= faces[f].v[i][2] < 1.0f;
		if (behind)
			return;
		o[f] = S3GlassFaceOf(faces[f], k, cx, cy);
	}
	// Passes a/c: the tunnel (as W1 holds it) refracted, times the lit colour: texel x tunnel colour / 128 x lit / 128 per vertex.
	const s3::KitTex& kabe = kit.T("TEXCKABE");
	for (int pass = 0; pass < 2; pass++)
		for (int f = 0; f < nf; f++)
			if (o[f].front == (pass == 1))
			{
				const float lit[3] = {std::min(255.0f, colour[0] + o[f].T), std::min(255.0f, colour[1] + o[f].T), std::min(255.0f, colour[2] + o[f].T)};
				if (!kabe.set)
				{
					S3GlassEmit(kit, o[f], VK_NULL_HANDLE, 11, nullptr, nullptr, lit[0] / 510.0f, lit[1] / 510.0f, lit[2] / 510.0f, 0.24f * alpha, false, 1, 1);
					continue;
				}
				kit.Bind(kabe.set);
				float xy[8], uv[8];
				uint32_t col[4];
				float u0 = 0;
				for (int i = 0; i < 4; i++)
				{
					float tu, tv, rgb[3];
					S3TunnelAt(look, o[f].u[i], o[f].w[i], tu, tv, rgb);
					if (i == 0)
						u0 = tu;
					else if (tu - u0 > 1.5f) // keep the face on one side of the angle's wrap
						tu -= 3.0f;
					else if (u0 - tu > 1.5f)
						tu += 3.0f;
					xy[i * 2] = kit.X(o[f].x[i]), xy[i * 2 + 1] = kit.Y(o[f].y[i]);
					uv[i * 2] = tu, uv[i * 2 + 1] = tv;
					col[i] = s3::Kit::Pack(std::min(1.0f, rgb[0] * lit[0] / 16384.0f * 0.5f), std::min(1.0f, rgb[1] * lit[1] / 16384.0f * 0.5f),
						std::min(1.0f, rgb[2] * lit[2] / 16384.0f * 0.5f), 0.5f * alpha);
				}
				const float t1[6] = {xy[0], xy[1], xy[2], xy[3], xy[4], xy[5]}, u1[6] = {uv[0], uv[1], uv[2], uv[3], uv[4], uv[5]};
				const float t2[6] = {xy[0], xy[1], xy[4], xy[5], xy[6], xy[7]}, u2[6] = {uv[0], uv[1], uv[4], uv[5], uv[6], uv[7]};
				const uint32_t c1[3] = {col[0], col[1], col[2]}, c2[3] = {col[0], col[2], col[3]};
				kit.Tri(t1, u1, c1, 10, false), kit.Tri(t2, u2, c2, 10, false);
			}
	const s3::KitTex& flow = kit.T("TEXCFLOW");
	if (flow.set)
		for (int f = 0; f < nf; f++)
			if (o[f].front)
				S3GlassEmit(kit, o[f], flow.set, 12, o[f].ru, o[f].rv, 60 / 255.0f, 60 / 255.0f, 60 / 255.0f, 0.5f * alpha * 30 / 128.0f, true, 1, 1); // glow x 30/128
}
} // namespace

void App::S3SettingsBuild(FrameDesc& f, const std::string& clock)
{
	s3::Shelf3& S = m_s3;
	s3::Kit& kit = S.kit;
	// The field: the main menu's tunnel brightened (+0..40 over 33 frames), the orbs at the hub, the twelve rods and the cube ring.
	const int wall = std::min(40, S.st_open * 40 / 33);
	float hub[2];
	S3DrawMainScene(this, kit, S, wall, 0, true, 1.0f, hub);
	const float s = kit.H() / 1080.0f;
	const float zoom = std::min(1.0f, S.st_open / 20.0f);
	(void)s;
	{
		// The twelve rods (4): R_n = Rz(Ah) Ry(As) Rz(n/12 turn - half) T(0, 20, 0) Ry(4 As); the hour's rod (Hl + B)/2 with gain 200, the
		// others B with 160. B walks the hue table every 9th frame (here: one table step every ~25 s), Hl between two pale blues.
		std::time_t now = std::time(nullptr);
		std::tm tm = {};
		localtime_r(&now, &tm);
		static const float kHue[5][3] = {{45, 85, 102}, {68, 47, 102}, {44, 71, 96}, {51, 73, 111}, {51, 111, 113}};
		const float ph = static_cast<float>(std::fmod(S.Fr, 6250.0)) / 1250.0f;
		const int h0 = static_cast<int>(ph) % 5, h1 = (h0 + 1) % 5;
		const float fr = ph - std::floor(ph);
		float B[3], Hl[3];
		for (int c = 0; c < 3; c++)
			B[c] = kHue[h0][c] + (kHue[h1][c] - kHue[h0][c]) * fr;
		const float hf = 0.5f + 0.5f * std::sin(static_cast<float>(std::fmod(S.Fr, 6283.0 * 250.0)) * 0.004f);
		Hl[0] = 202 + (187 - 202) * hf, Hl[1] = 246 + (235 - 246) * hf, Hl[2] = 231 + (255 - 231) * hf;
		const S3Cam cam = S3MenuCamera(S.menu_d);
		S3TunnelLook look;
		{
			float cx, cy, cz;
			S3ToCam(cam, 0, 0, 38750.0f, cx, cy, cz);
			look.vx = 320.0f + 512.0f * cx / cz, look.vy = 128.0f + 512.0f * 0.5405f * cy / cz;
			look.eye_z = cam.eye[2];
			look.F = S.F;
			look.wall = wall;
		}
		struct RodDraw
		{
			M4 m;
			float z;
			bool hour;
		};
		std::vector<RodDraw> rods;
		for (int n = 0; n < 12; n++)
		{
			M4 m = MMul(MRotZ(S.orb_ah), MRotY(S.orb_as));
			m = MMul(m, MRotZ(n * 2 * kPi_ / 12 - kPi_));
			m = MMul(m, MTrans(0, 20, 0));
			m = MMul(m, MRotY(4 * S.orb_as));
			m = MMul(m, MScale(1, zoom, 1));
			float wx, wy, wz, cx, cy, cz;
			MApply(m, 0, 0, 0, wx, wy, wz);
			S3ToCam(cam, wx, wy, wz, cx, cy, cz);
			rods.push_back({m, cz, n == 0});
		}
		std::sort(rods.begin(), rods.end(), [](const RodDraw& a, const RodDraw& b) { return a.z > b.z; });
		for (const RodDraw& r : rods)
		{
			float col[3];
			for (int c = 0; c < 3; c++)
				col[c] = r.hour ? (Hl[c] + B[c]) / 2 : B[c];
			S3GlassRod(kit, look, cam, r.m, col, r.hour ? 200.0f : 160.0f, zoom);
		}
	}
	// The cube ring (7): six cubes on a vertical wheel at x -10 / -20, y 60 cos(phi), z 47.5 (camera space); the current one blue.
	const int n = S.st_rows.empty() ? 0 : static_cast<int>(S.st_rows[0].size());
	const float cube_in = std::clamp((S.st_open - 33) / 33.0f, 0.0f, 1.0f);
	kit.MarkScene(); // the rods and the tunnel go under the soft focus; the cubes come after it (sysconf.md 8), refracting the scene
	if (cube_in > 0 && n > 0)
	{
		const float turn = S.st_ring_anim / 19.0f;
		for (int k = 0; k < 6; k++)
		{
			const int item = (S.st_row + k - 2 + n * 6) % n;
			(void)item;
			// (Its slot on the 60-slot wheel, sysconf.md 7: the wheel turns one slot a step and on through the list's wrap, so no
			// cube jumps when the cursor goes from the last item to the first.)
			const int sidx = (S.st_ring_pos + k - 2 + 60) % 60;
			// The ring turns one slot in 19 frames, fast then slowing (sysconf.md 7): the cubes come from where they were.
			const float u = turn * turn;
			const float phi = (k - 2 + S.st_ring_dir * u) * 6.0f * kPi_ / 180.0f + kPi_ / 2;
			const float X = (sidx % 2 == 0) ? -10.0f : -20.0f, Y = 60.0f * std::cos(phi), Z = 47.5f;
			// Turned by A = spin + slot x 7000 about X, Y and Z (16-bit angles), scaled by the cube animator.
			const float A = static_cast<float>(std::fmod(S.Fr * 30.0, 65536.0)) / 65536.0f * 2 * kPi_ + sidx * 7000.0f / 65536.0f * 2 * kPi_;
			const float fadeout = k == 0 || k == 5 ? 0.5f : 1.0f;
			const float lit = k == 2 ? 1.0f - u : (k == 2 - S.st_ring_dir ? u : 0.0f);
			const float col[3] = {100 * (1 - lit), 100 + 50 * lit, 100 + 100 * lit};
			M4 m = MMul(MTrans(X, Y, Z), MMul(MRotX(A), MMul(MRotY(A), MMul(MRotZ(A), MScale(cube_in, cube_in, cube_in)))));
			S3GlassCube(kit, m_renderer->SceneSet(), kit.W(), kit.H(), m, col, cube_in * fadeout);
		}
	}
	f.blur = 2.2f; // the five blur passes over the rods and the wall (sysconf.md 8)
	// The date and time corners (or the game's name) in grey, scale 0.83.
	const int ta = std::clamp(S.st_open - 66, 0, 8) * 128 / 8;
	kit.FillPx(0, 0, kit.W(), kit.Y(30.7f), 0, 0, 0, 1);
	kit.FillPx(0, kit.Y(225.3f), kit.W(), kit.H(), 0, 0, 0, 1);
	if (!S.st_global)
		kit.Text(S3Ascii(m_sheet.title()), kit.Ox(22), 14 * 1.15f, s3::kGrey, kit.FitScale(S3Ascii(m_sheet.title()), 0.83f, 380), 0, ta);
	else
		kit.Text(TodayText(), kit.Ox(22), 14 * 1.15f, s3::kGrey, 0.83f, 0, ta);
	{
		(void)clock;
		std::time_t now = std::time(nullptr);
		std::tm tm = {};
		localtime_r(&now, &tm);
		char b[16];
		std::snprintf(b, sizeof(b), "%2d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec); // (mainmenu.md 6.3: 24 h, hour space-padded)
		kit.Text(b, kit.Ox(618), 14 * 1.15f, s3::kGrey, 0.83f, 2, ta);
	}
	// The tab strip (ours) at y 80: L1 ... R1, the active tab yellow.
	{
		const float sc = 0.7f;
		int w = kit.TextWidth("L1  ", sc) + kit.TextWidth("  R1", sc);
		for (size_t i = 0; i < S.st_tab_names.size(); i++)
			w += kit.TextWidth(S.st_tab_names[i], sc) + (i + 1 < S.st_tab_names.size() ? kit.TextWidth("   ", sc) : 0);
		float x = kit.Ox(430) - w / 2.0f;
		x += kit.Text("L1  ", x, 76, s3::kGrey, sc, 0, ta);
		for (size_t i = 0; i < S.st_tab_names.size(); i++)
		{
			x += kit.Text(S.st_tab_names[i], x, 76, static_cast<int>(i) == S.st_tab ? s3::kYellow : s3::kDark, sc, 0, ta);
			if (i + 1 < S.st_tab_names.size())
				x += kit.TextWidth("   ", sc);
		}
		kit.Text("  R1", x, 76, s3::kGrey, sc, 0, ta);
	}
	const float ax = kit.Ox(430);
	kit.Text(S.st_global ? "Settings" : "Settings for this game", ax, 101, s3::kYellow, 1.0f, 1, ta);
	if (n == 0)
	{
		kit.Text("Nothing to set here.", ax, 128, s3::kGrey, 1.0f, 1, ta);
		kit.Hints({{s3::Button::Circle, "Back"}}, ta, true);
		return;
	}
	const auto& all = m_sheet.rows();
	auto draw_item = [&](int idx, int alpha, bool current) {
		const OptionsSheet::Row& r = all[static_cast<size_t>(S.st_rows[0][static_cast<size_t>(idx)])];
		const std::string name = S3Ascii(r.label.empty() && r.def ? r.def->label : r.label);
		const float sc = kit.FitScale(name, 1.0f, 380);
		const int a = alpha * ta / 128;
		kit.Text(name, ax, 128, S.st_edit && current ? s3::kGrey : s3::kCyan, sc, 1, a);
		const bool own = !S.st_global && r.kind == OptionsSheet::Kind::Option && m_sheet.Source(r) == OptionsSheet::From::Own;
		if (own)
		{
			// The gold override marker (ours) before the name.
			const float mx = ax - kit.TextWidth(name, sc) / 2.0f - 10;
			kit.Flare(kit.X(mx), kit.Y(131), 7, 150, a);
			S3Star(kit, mx, 131, 3.0f, true, a);
		}
		if (current && !S.st_edit)
		{
			const int pulse = static_cast<int>(std::fabs(std::sin(m_time * 3.72)) * 128) * a / 128;
			kit.Text(std::string(1, s3::kSymUpDown), ax + kit.TextWidth(name, sc) / 2.0f + 10, 128, s3::kGrey, 1.0f, 0, pulse);
		}
		// The value: one grey line; editing: the options in a row (chosen cyan) when they fit, else the one with arrows.
		std::string value = S3Ascii(r.kind == OptionsSheet::Kind::Option || current ? m_sheet.Value(r) : m_sheet.Value(r));
		if (current && S.st_edit && r.def && !r.def->choices.empty())
		{
			std::vector<std::string> ch;
			for (const OptionChoice& c : r.def->choices)
				ch.push_back(S3Ascii(c.label));
			int w = 0;
			const float vs = ch.size() > 3 ? 0.72f : 1.0f;
			for (size_t i = 0; i < ch.size(); i++)
				w += kit.TextWidth(ch[i], vs) + (i + 1 < ch.size() ? kit.TextWidth(" ", vs) * 2 : 0);
			if (w < 560 && ch.size() <= 4)
			{
				float x = ax - w / 2.0f;
				for (size_t i = 0; i < ch.size(); i++)
				{
					x += kit.Text(ch[i], x, 149, ch[i] == value ? s3::kCyan : s3::kDark, vs, 0, a);
					x += kit.TextWidth(" ", vs) * 2;
				}
			}
			else
			{
				const float vsc = kit.FitScale(value, 1.0f, 360);
				const int vw = kit.TextWidth(value, vsc);
				kit.Text(value, ax, 149, s3::kCyan, vsc, 1, a);
				const int pulse = static_cast<int>(std::fabs(std::sin(m_time * 3.72)) * 128) * a / 128;
				kit.Text(std::string(1, s3::kSymLeft), ax - vw / 2.0f - 22, 149, {127, 127, 127}, 1.0f, 0, pulse);
				kit.Text(std::string(1, s3::kSymRight), ax + vw / 2.0f + 8, 149, {127, 127, 127}, 1.0f, 0, pulse);
			}
		}
		else
			kit.Text(value, ax, 149, s3::kGrey, kit.FitScale(value, 1.0f, 400), 1, a);
		if (current)
		{
			// The help line (ours, 0.6), up to two lines; on a game's sheet an overridden row names the value for all games.
			std::string help = S3Ascii(m_sheet.Help(r));
			if (own && r.def)
			{
				std::string def = S3Ascii(r.def->def);
				for (const OptionChoice& c : r.def->choices)
					if (c.value == def)
						def = S3Ascii(c.label);
				help = "Overrides the setting for all games. " + help;
			}
			std::string l1, l2;
			S3Split(kit, help, 0.6f, 430, l1, l2);
			if (kit.TextWidth(l2, 0.6f) > 430)
			{
				std::string l3;
				S3Split(kit, l2, 0.6f, 430, l2, l3);
				if (!l3.empty())
					l2 += "...";
			}
			kit.Text(l1, ax, 172, s3::kGrey, 0.6f, 1, a);
			kit.Text(l2, ax, 184, s3::kGrey, 0.6f, 1, a);
		}
	};
	if (S.st_confirm == 1)
	{
		kit.Text("Reset to Global", ax, 128, s3::kYellow, 1.0f, 1, ta);
		kit.Text("Are you sure?", ax, 149, s3::kGrey, 1.0f, 1, ta);
		const int w1 = kit.TextWidth("Yes", 1.0f), w2 = kit.TextWidth("    ", 1.0f), w3 = kit.TextWidth("No", 1.0f);
		const float x0 = ax - (w1 + w2 + w3) / 2;
		kit.Text("Yes", x0, 170, S.yes_no == 0 ? s3::kBlue : s3::kDark, 1.0f, 0, ta);
		kit.Text("No", x0 + w1 + w2, 170, S.yes_no == 1 ? s3::kBlue : s3::kDark, 1.0f, 0, ta);
		kit.Hints({{s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}}, ta, true);
		return;
	}
	draw_item(S.st_row, S.item_alpha, true);
	if (S.prev_item >= 0 && S.prev_alpha > 0 && S.prev_item < n)
		draw_item(S.prev_item, S.prev_alpha, false);
	// Gold pips (ours) under the text: which rows of this tab the game overrides.
	if (!S.st_global)
	{
		int own = 0;
		for (int i = 0; i < n && i < 40; i++)
		{
			const OptionsSheet::Row& r = all[static_cast<size_t>(S.st_rows[0][static_cast<size_t>(i)])];
			if (r.kind == OptionsSheet::Kind::Option && m_sheet.Source(r) == OptionsSheet::From::Own)
				kit.Flare(kit.X(ax - n * 3.0f + i * 6.0f), kit.Y(198), 3, 150, ta), own++;
		}
		(void)own;
	}
	if (S.st_edit)
		kit.Hints({{s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}}, ta, true);
	else if (!S.st_global)
		kit.Hints({{s3::Button::Square, "Reset"}, {s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}, {s3::Button::Triangle, "Reset All"}}, ta,
			true);
	else
		kit.Hints({{s3::Button::Cross, "Enter"}, {s3::Button::Circle, "Back"}}, ta, true);
}
} // namespace fe
