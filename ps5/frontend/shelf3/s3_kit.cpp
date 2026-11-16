// PS2 shelf v3 (AI-assisted): the drawing kit (see s3_kit.h).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "s3_kit.h"
#include <map>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace s3
{
namespace
{
const char* kKitTextures[] = {"TEXBBTTN", "TEXBNAV1", "TEXBNAV2", "TEXCMARU", "TEXCKABE", "TEXCBLUR", "TEXCNAVI", "TEXCBUMP", "TEXCREFA",
	"TEXOWAL0", "TEXOSCE", "TEXOFLAR", "TEXCFLOW", "TEXOFOG1", "TEXOFOG2", "TEXOFOG4", "TEXOCRBL", "TEXOREF", "TEXOBLP"};
inline float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
} // namespace

uint32_t Kit::Pack(float r, float g, float b, float a)
{
	auto c = [](float v) { return static_cast<uint32_t>(Clamp01(v) * 255.0f + 0.5f); };
	return c(r) | c(g) << 8 | c(b) << 16 | c(a) << 24;
}

void Kit::Init(fe::Renderer* r, const fe::Fonts* fonts, const Bios* bios)
{
	m_r = r;
	m_fonts = fonts;
	m_bios = bios;
	if (!bios || !bios->ok())
		return;
	for (const char* n : kKitTextures)
		if (const Image* im = bios->Tex(n))
			m_tex[n] = Upload(*im);
	if (const Image* f = bios->Font("FNTASCII"))
		m_tex["FNTASCII"] = Upload(*f);
	if (const Image* f = bios->Font("FNTEXOSD"))
		m_tex["FNTEXOSD"] = Upload(*f);
}

void Kit::Shutdown()
{
	for (auto& [k, t] : m_tex)
		Free(t);
	for (auto& [k, t] : m_cached)
		Free(t);
	m_tex.clear();
	m_cached.clear();
}

KitTex Kit::Upload(const Image& im)
{
	KitTex t;
	if (!m_r || !im.ok())
		return t;
	t.tex = m_r->CreateTexture(static_cast<uint32_t>(im.w), static_cast<uint32_t>(im.h), VK_FORMAT_R8G8B8A8_UNORM, im.rgba.data());
	if (t.tex)
		t.set = m_r->AllocTextureSet(t.tex, t.tex, t.tex, t.tex);
	t.w = im.w, t.h = im.h;
	return t;
}

void Kit::Free(KitTex& t)
{
	if (m_r && t.set)
		m_r->FreeTextureSet(t.set);
	if (m_r && t.tex)
		m_r->DestroyTexture(t.tex);
	t = KitTex();
}

const KitTex& Kit::Cached(const std::string& key, const Image& im)
{
	auto it = m_cached.find(key);
	if (it != m_cached.end())
		return it->second;
	return m_cached[key] = Upload(im);
}

void Kit::DropCached(const std::string& prefix)
{
	for (auto it = m_cached.begin(); it != m_cached.end();)
		if (it->first.rfind(prefix, 0) == 0)
		{
			Free(it->second);
			it = m_cached.erase(it);
		}
		else
			++it;
}

const KitTex& Kit::T(const char* name) const
{
	auto it = m_tex.find(name);
	return it == m_tex.end() ? m_none : it->second;
}

void Kit::Begin(fe::FrameDesc& f, float W, float H, bool wide)
{
	m_f = &f;
	m_W = W, m_H = H;
	m_wide = wide;
	m_sy = H / 256.0f;
	if (wide)
	{
		m_sx = W / 854.0f;
		m_x0 = W * 0.5f - 320.0f * m_sx;
	}
	else
	{
		m_sx = (H * 4.0f / 3.0f) / 640.0f;
		m_x0 = (W - H * 4.0f / 3.0f) * 0.5f;
	}
	m_bound = VK_NULL_HANDLE;
	m_range_first = static_cast<uint32_t>(f.ui.size());
}

void Kit::CloseRange()
{
	if (!m_f)
		return;
	const uint32_t n = static_cast<uint32_t>(m_f->ui.size());
	if (m_bound && n > m_range_first)
		m_f->ui_images.push_back({m_range_first, n - m_range_first, m_bound});
	m_range_first = n;
}

void Kit::Bind(VkDescriptorSet set)
{
	if (set == m_bound)
		return;
	CloseRange();
	m_bound = set;
}

void Kit::MarkUnder()
{
	CloseRange();
	m_f->ui_under = static_cast<uint32_t>(m_f->ui.size());
}

void Kit::MarkGhost(bool reset)
{
	CloseRange();
	m_f->ghost_end = static_cast<uint32_t>(m_f->ui.size());
	m_f->ghost_reset = reset;
}

void Kit::MarkScene()
{
	CloseRange();
	m_f->ui_scene = static_cast<uint32_t>(m_f->ui.size());
	if (m_f->ui_under > m_f->ui_scene)
		m_f->ui_under = m_f->ui_scene;
}

void Kit::End()
{
	CloseRange();
	m_bound = VK_NULL_HANDLE;
}

void Kit::Tri(const float* xy, const float* uv, const uint32_t* rgba, int mode, bool add)
{
	const float m = static_cast<float>(mode + (add ? 16 : 0));
	for (int i = 0; i < 3; i++)
		m_f->ui.push_back({xy[i * 2], xy[i * 2 + 1], uv ? uv[i * 2] : 0.0f, uv ? uv[i * 2 + 1] : 0.0f, rgba[i], 0, 0, 0, m});
}

void Kit::TriP(const float* xy, const float* uv, const float* st, const uint32_t* rgba, int mode, bool add)
{
	const float m = static_cast<float>(mode + (add ? 16 : 0));
	for (int i = 0; i < 3; i++)
		m_f->ui.push_back({xy[i * 2], xy[i * 2 + 1], uv[i * 2], uv[i * 2 + 1], rgba[i], st[i * 2], st[i * 2 + 1], 0, m});
}

VkDescriptorSet Kit::PairSet(const KitTex& a, const KitTex& b)
{
	if (!m_r || !a.tex || !b.tex)
		return VK_NULL_HANDLE;
	static std::map<std::pair<fe::Texture*, fe::Texture*>, VkDescriptorSet> sets;
	auto it = sets.find({a.tex, b.tex});
	if (it == sets.end())
		it = sets.emplace(std::make_pair(a.tex, b.tex), m_r->AllocTextureSet(a.tex, b.tex, b.tex, b.tex)).first;
	return it->second;
}

void Kit::Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t rgba, int mode, bool add)
{
	const float m = static_cast<float>(mode + (add ? 16 : 0));
	std::vector<fe::UiVertex>& ui = m_f->ui;
	ui.push_back({x0, y0, u0, v0, rgba, 0, 0, 0, m});
	ui.push_back({x1, y0, u1, v0, rgba, 0, 0, 0, m});
	ui.push_back({x1, y1, u1, v1, rgba, 0, 0, 0, m});
	ui.push_back({x0, y0, u0, v0, rgba, 0, 0, 0, m});
	ui.push_back({x1, y1, u1, v1, rgba, 0, 0, 0, m});
	ui.push_back({x0, y1, u0, v1, rgba, 0, 0, 0, m});
}

void Kit::Fill(float x0, float y0, float x1, float y1, Gs c, int a)
{
	Bind(VK_NULL_HANDLE);
	Quad(X(x0), Y(y0), X(x1), Y(y1), 0, 0, 0, 0, Pack(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, a / 255.0f), 11, false);
}

void Kit::FillPx(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
{
	Bind(VK_NULL_HANDLE);
	Quad(x0, y0, x1, y1, 0, 0, 0, 0, Pack(r * 0.5f, g * 0.5f, b * 0.5f, a * 0.5f), 11, false);
}

void Kit::Gradient(float x0, float y0, float x1, float y1, const float c[4][3], float alpha)
{
	Bind(VK_NULL_HANDLE);
	uint32_t k[4];
	for (int i = 0; i < 4; i++)
		k[i] = Pack(c[i][0] / 510.0f, c[i][1] / 510.0f, c[i][2] / 510.0f, alpha * 0.5f);
	const float a[6] = {x0, y0, x1, y0, x1, y1};
	const uint32_t ca[3] = {k[0], k[1], k[3]};
	Tri(a, nullptr, ca, 11, false);
	const float b[6] = {x0, y0, x1, y1, x0, y1};
	const uint32_t cb[3] = {k[0], k[3], k[2]};
	Tri(b, nullptr, cb, 11, false);
}

void Kit::Sprite(const KitTex& t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Gs c, int a, bool add)
{
	if (!t.set)
		return;
	Bind(t.set);
	Quad(x0, y0, x1, y1, u0 / t.w, v0 / t.h, u1 / t.w, v1 / t.h, Pack(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, a / 255.0f), 9, add);
}

// ---- text ----

bool Kit::GlyphOf(unsigned char c, Glyph& g) const
{
	if (c >= 0x20 && c < 0x80 && c != 0x7F)
	{
		const int i = c - 0x20;
		g = {0, i % 8, i / 8, kAsciiGlyphs[i].x, kAsciiGlyphs[i].w};
		return true;
	}
	if ((c >= 1 && c < 32) || c == 0x7F)
	{
		const int i = c == 0x7F ? 0 : c;
		g = {1, i % 16, i / 16, kOsdGlyphs[i].x, kOsdGlyphs[i].w};
		return true;
	}
	return false;
}

int Kit::TextWidth(const std::string& s, float scale) const
{
	int w = 0;
	Glyph g;
	for (unsigned char c : s)
		if (GlyphOf(c, g))
			w += static_cast<int>(scale * g.w) + static_cast<int>(scale * -3.0f);
	return w;
}

float Kit::FitScale(const std::string& s, float scale, float max_w) const
{
	static const float kFit[] = {1.0f, 0.875f, 0.8f, 0.77f, 0.73f, 0.7f, 0.67f, 0.61f};
	for (float f : kFit)
		if (f <= scale + 1e-4f && TextWidth(s, f) <= max_w)
			return f;
	return 0.61f;
}

int Kit::Text(const std::string& s, float x, float y, Gs c, float scale, int align, int alpha)
{
	if (alpha <= 0 || s.empty())
		return 0;
	const int w = TextWidth(s, scale);
	int pen = static_cast<int>(x) - (align == 1 ? w / 2 : align == 2 ? w : 0);
	const KitTex& a = T("FNTASCII");
	const KitTex& o = T("FNTEXOSD");
	if (!a.set)
	{
		// No BIOS: the eboot's own font at the BIOS font's size (cap height ~14.5 units at scale 1).
		Bind(VK_NULL_HANDLE);
		const float px = 15.0f * scale * m_sy * 1.42f;
		const uint32_t col = Pack(std::min(1.0f, c.r / 128.0f), std::min(1.0f, c.g / 128.0f), std::min(1.0f, c.b / 128.0f), alpha / 128.0f);
		const float pxw = static_cast<float>(w) * m_sx;
		std::string plain;
		for (unsigned char ch : s)
			plain += ch == kSymUpDown ? '|' : ch == kSymLeft ? '<' : ch == kSymRight ? '>' : ch == static_cast<unsigned char>(kSymDown) ? 'v'
			                                                                                     : (ch < 32 ? ' ' : static_cast<char>(ch));
		const float tw = m_fonts ? m_fonts->Measure(plain.c_str(), px) : pxw;
		const float sx = X(static_cast<float>(pen)) + (pxw - tw) * 0.5f;
		if (m_fonts)
			m_fonts->AddText(m_f->ui, plain.c_str(), sx, Y(y + 11.0f * scale), px, col, 0.1f, fe::Fonts::Left);
		return w;
	}
	const float h16 = std::floor(static_cast<float>(static_cast<int>(scale * 16.0f) * 20) * 1.15f);
	const float top = std::floor((y + scale * -3.5f * 1.15f) * 16.0f) / 16.0f;
	const float bottom = top + h16 / 16.0f;
	const uint32_t col = Pack(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, alpha / 255.0f);
	Glyph g;
	for (unsigned char ch : s)
	{
		if (!GlyphOf(ch, g))
			continue;
		const KitTex& t = g.page == 0 ? a : o;
		const int gw = static_cast<int>(scale * g.w);
		if (t.set && gw > 0 && ch != ' ')
		{
			Bind(t.set);
			const float u0 = static_cast<float>(g.cx * 32 + g.xo), v0 = static_cast<float>(g.cy * 40);
			Quad(X(static_cast<float>(pen)), Y(top), X(static_cast<float>(pen + gw)), Y(bottom), u0 / t.w, v0 / t.h, (u0 + g.w) / t.w,
				(v0 + 40.0f) / t.h, col, 9, false);
		}
		pen += gw + static_cast<int>(scale * -3.0f);
	}
	return w;
}

// ---- prompts, flare, fields ----

void Kit::PromptAt(Button b, const std::string& label, float x, float y, int alpha, bool menu)
{
	const KitTex& t = T(menu ? "TEXCMARU" : "TEXBBTTN");
	const float bw = menu ? 25.0f : 24.0f;
	if (b == Button::L1 || b == Button::R1)
	{
		Text(b == Button::L1 ? "L1" : "R1", x, y, kGrey, 0.8f, 0, alpha);
		Text(label, x + 28, y, kGrey, 0.8f, 0, alpha);
		return;
	}
	if (t.set)
	{
		const int q = static_cast<int>(b);
		const float u = (q == 1 || q == 3) ? 32.0f : 0.0f, v = q >= 2 ? 32.0f : 0.0f;
		Sprite(t, X(x), Y(y + 0.56f), X(x + bw), Y(y + 0.56f + 13.75f), u + 0.5f, v + 0.5f, u + 31.5f, v + 31.5f, kWhite, alpha, false);
	}
	else
	{
		static const char* kSym[] = {"[]", "/\\", "X", "O"};
		Text(kSym[static_cast<int>(b)], x + 4, y, kGrey, 0.8f, 0, alpha);
	}
	Text(label, x + 28, y, kGrey, 0.8f, 0, alpha);
}

void Kit::Hints(const std::vector<Hint>& hints, int alpha, bool menu, float y)
{
	for (const Hint& h : hints)
	{
		float x;
		switch (h.b)
		{
			case Button::Square: x = Ox(24); break;
			case Button::Cross: x = 213; break;
			case Button::Circle: x = 335; break;
			default:
			{
				const float tw = static_cast<float>(TextWidth(h.label, 0.8f) - static_cast<int>(0.8f * -3.0f));
				x = (m_wide ? 618.0f + 64.0f : 618.0f) - tw - 28.0f;
				break;
			}
		}
		PromptAt(h.b, h.label, x, y, alpha, menu);
	}
}

void Kit::Flare(float cx, float cy, float h, int grey, int alpha)
{
	const KitTex& t = T("TEXBNAV2");
	if (t.set)
	{
		Sprite(t, cx - h * m_sx, cy - h * 0.5f * m_sy, cx + h * m_sx, cy + h * 0.5f * m_sy, 0, 0, 64, 64, {grey, grey, grey}, alpha, true);
		return;
	}
	// No BIOS: a soft white glow (ui.frag mode 3).
	Bind(VK_NULL_HANDLE);
	const float rx = h * m_sx, ry = h * 0.5f * m_sy;
	const uint32_t col = Pack(grey / 128.0f, grey / 128.0f, grey / 128.0f, 0.0f);
	std::vector<fe::UiVertex>& ui = m_f->ui;
	const fe::UiVertex v[4] = {{cx - rx, cy - ry, -1, -1, col, 3.0f, 0, 0, 19}, {cx + rx, cy - ry, 1, -1, col, 3.0f, 0, 0, 19},
		{cx + rx, cy + ry, 1, 1, col, 3.0f, 0, 0, 19}, {cx - rx, cy + ry, -1, 1, col, 3.0f, 0, 0, 19}};
	for (int i : {0, 1, 2, 0, 2, 3})
		ui.push_back(v[i]);
	// mode 3 + 16 draws the glow additively; its alpha (colour alpha x falloff) is the strength.
	for (size_t i = ui.size() - 6; i < ui.size(); i++)
		ui[i].color = Pack(grey / 160.0f, grey / 160.0f, grey / 160.0f, alpha / 128.0f);
}

void Kit::BrowserField(const float corners[4][3])
{
	Gradient(0, 0, m_W, m_H, corners, 1.0f);
}

void Kit::Bands()
{
	FillPx(0, 0, m_W, Y(32), 0, 0, 0, 1);
	FillPx(0, Y(224), m_W, m_H, 0, 0, 0, 1);
}

void Kit::Black(float alpha)
{
	if (alpha > 0.002f)
		FillPx(0, 0, m_W, m_H, 0, 0, 0, std::min(1.0f, alpha));
}

void Kit::Sides()
{
	if (m_wide)
		return;
	FillPx(0, 0, X(0), m_H, 0, 0, 0, 1);
	FillPx(X(640), 0, m_W, m_H, 0, 0, 0, 1);
}

// ---- ico ----

void Kit::DrawIco(const Ico& ico, const float M[16], float cx, float cy, const Light& L, float frame, const KitTex* tex, float alpha,
	float tr, float tg, float tb)
{
	const size_t n = ico.verts.size();
	if (n < 3)
		return;
	// Shape weights (spec browser.md 6.3): t = (F speed + offset) mod length, keys interpolated linearly.
	std::vector<float> wts(static_cast<size_t>(ico.shapes), 0.0f);
	bool any = false;
	if (ico.shapes > 1 && !ico.frames.empty())
	{
		const float len = static_cast<float>(ico.frame_length);
		float t = std::fmod(frame * ico.anim_speed + ico.anim_offset, len);
		if (t < 0)
			t += len;
		for (int s = 0; s < ico.shapes && s < static_cast<int>(ico.frames.size()); s++)
		{
			const auto& k = ico.frames[static_cast<size_t>(s)];
			if (k.empty())
				continue;
			// The keys loop: after the last key the weight runs on to the first key one length later, and before the first it comes
			// from the last key one length earlier (kurolume: Crazy Taxi's icon grew for a frame at the loop's end, two shapes at full
			// weight with the ends held).
			float v = k.front().value;
			if (k.size() == 1)
				v = k.front().value;
			else if (t >= k.back().time || t < k.front().time)
			{
				const float t0 = k.back().time, t1 = k.front().time + len;
				const float tt = t < k.front().time ? t + len : t;
				const float f = (tt - t0) / std::max(1e-4f, t1 - t0);
				v = k.back().value + (k.front().value - k.back().value) * std::clamp(f, 0.0f, 1.0f);
			}
			else
				for (size_t i = 0; i + 1 < k.size(); i++)
					if (t >= k[i].time && t < k[i + 1].time)
					{
						const float f = (t - k[i].time) / std::max(1e-4f, k[i + 1].time - k[i].time);
						v = k[i].value + (k[i + 1].value - k[i].value) * f;
						break;
					}
			wts[static_cast<size_t>(s)] = v;
			any = true;
		}
		// The shapes morph: their weights sum to one, so a loop's overlap never scales the icon up.
		float sum = 0;
		for (float w : wts)
			sum += w;
		if (sum > 1e-4f)
			for (float& w : wts)
				w /= sum;
		else
			any = false;
	}
	if (!any)
		wts[0] = 1.0f;
	// Lights as the VU sees them: -normalize(d).
	float Ld[3][3];
	for (int i = 0; i < 3; i++)
	{
		const float* d = L.dir[i];
		const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		for (int k = 0; k < 3; k++)
			Ld[i][k] = len > 1e-6f ? -d[k] / len : 0.0f;
	}
	struct P
	{
		float x, y, z;
		uint32_t c;
		float u, v;
	};
	std::vector<P> pv(n);
	for (size_t i = 0; i < n; i++)
	{
		const Ico::Vertex& v = ico.verts[i];
		float px = 0, py = 0, pz = 0;
		for (int s = 0; s < ico.shapes; s++)
		{
			const float w = wts[static_cast<size_t>(s)];
			if (w == 0.0f)
				continue;
			px += v.pos[static_cast<size_t>(s) * 3] * w, py += v.pos[static_cast<size_t>(s) * 3 + 1] * w, pz += v.pos[static_cast<size_t>(s) * 3 + 2] * w;
		}
		const float X = M[0] * px + M[1] * py + M[2] * pz + M[3];
		const float Y = M[4] * px + M[5] * py + M[6] * pz + M[7];
		const float Z = M[8] * px + M[9] * py + M[10] * pz + M[11];
		const float zz = std::max(Z, 1.0f);
		P& o = pv[i];
		o.x = this->X(cx + 1024.0f * X / zz);
		o.y = this->Y(cy + 553.47f * Y / zz);
		o.z = Z;
		float lr = L.amb[0], lg = L.amb[1], lb = L.amb[2], sum = 0;
		for (int l = 0; l < 3; l++)
		{
			const float d = std::max(0.0f, v.nx * Ld[l][0] + v.ny * Ld[l][1] + v.nz * Ld[l][2]);
			lr += d * L.col[l][0], lg += d * L.col[l][1], lb += d * L.col[l][2];
			sum += d;
		}
		const float r = std::min(255.0f, v.r * lr * tr), g = std::min(255.0f, v.g * lg * tg), b = std::min(255.0f, v.b * lb * tb);
		const float a = L.alpha_from_light ? std::min(255.0f, 128.0f * (1.0f + sum)) : 128.0f;
		(void)a;
		o.c = Pack(r / 255.0f, g / 255.0f, b / 255.0f, 0.5f * alpha);
		o.u = v.u, o.v = v.v;
	}
	struct T3
	{
		uint32_t i;
		float z;
	};
	std::vector<T3> tris;
	tris.reserve(n / 3);
	for (uint32_t t = 0; t + 2 < n; t += 3)
	{
		const P &a = pv[t], &b = pv[t + 1], &c = pv[t + 2];
		if (a.z < 1 || b.z < 1 || c.z < 1)
			continue;
		const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
		if (cross >= 0) // back faces (the VU's ADC rule)
			continue;
		tris.push_back({t, a.z + b.z + c.z});
	}
	std::sort(tris.begin(), tris.end(), [](const T3& a, const T3& b) { return a.z > b.z; });
	const bool textured = tex && tex->set && (ico.tex_type & 4);
	Bind(textured ? tex->set : VK_NULL_HANDLE);
	for (const T3& t : tris)
	{
		float xy[6], uv[6];
		uint32_t c[3];
		for (int k = 0; k < 3; k++)
		{
			const P& p = pv[t.i + static_cast<uint32_t>(k)];
			xy[k * 2] = p.x, xy[k * 2 + 1] = p.y;
			uv[k * 2] = p.u, uv[k * 2 + 1] = p.v;
			c[k] = p.c;
		}
		Tri(xy, uv, c, textured ? 9 : 11, false);
	}
}
} // namespace s3
