// PS2 shelf v3 (AI-assisted): the drawing kit. Everything is laid out in the PS2 menu's own field units (a PAL field, 640 x 256, as our
// spec gives every number) and drawn at the display's native resolution as UI triangles: the BIOS font, the button prompts, the Browser
// gradients, bands and flare, 3D icons (ico) and the main menu's textures. 16:9 widens the field to x -107..747 (the 4:3 frame stays
// 2.25 px a unit at 1080 lines; groups anchored in an outer third slide 64 units out: Ox()). 4:3 keeps x 0..640 with black sides.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../fe_renderer.h"
#include "../fe_text.h"
#include "s3_bios.h"

#include <map>
#include <string>
#include <vector>

namespace s3
{
// GS colours (0x80 = 1.0) of the BIOS palette (spec/text.md 4).
struct Gs
{
	int r, g, b;
};
constexpr Gs kGrey{96, 96, 96}, kYellow{110, 110, 0}, kBlue{47, 87, 127}, kDark{44, 44, 44}, kCyan{30, 110, 156}, kWhite{128, 128, 128};
constexpr Gs kBrowserText{0x60, 0x60, 0x60};
constexpr Gs kGold{150, 112, 24}; // ours: the per-game override marker and the favourite star

// FNTEXOSD symbols in a string: the byte (1..31) is the symbol's index; 0x7F is symbol 0 (the down arrow).
constexpr char kSymUpDown = 20, kSymLeft = 2, kSymRight = 3, kSymDown = 0x7F;

struct KitTex
{
	fe::Texture* tex = nullptr;
	VkDescriptorSet set = VK_NULL_HANDLE;
	int w = 0, h = 0;
};

enum class Button
{
	Square,
	Triangle,
	Cross,
	Circle,
	L1,
	R1
};

class Kit
{
public:
	void Init(fe::Renderer* r, const fe::Fonts* fonts, const Bios* bios);
	void Shutdown();
	bool has_bios() const { return m_bios && m_bios->ok(); }
	const Bios* bios() const { return m_bios; }

	// Starts a frame into `f`: `wide` true = 16:9 rule, false = 4:3 (black sides).
	void Begin(fe::FrameDesc& f, float W, float H, bool wide);
	// The end of the background drawn under the cases (FrameDesc::ui_under), of the scene part (ui_scene), and the frame.
	void MarkUnder();
	void MarkScene();
	void MarkGhost(bool reset); // the UI so far is the PS2 opening's fed-back layer (FrameDesc::ghost_end)
	void End();
	std::vector<fe::UiVertex>& ui() { return m_f->ui; }

	// Field units -> pixels.
	float X(float x) const { return m_x0 + x * m_sx; }
	float Y(float y) const { return y * m_sy; }
	float SX() const { return m_sx; } // pixels per unit across
	float SY() const { return m_sy; } // pixels per unit down
	bool wide() const { return m_wide; }
	float Ox(float x, float k = 64) const { return !m_wide ? x : x < 213 ? x - k : x > 427 ? x + k : x; }
	float left() const { return m_wide ? -107.0f : 0.0f; }  // the visible field's edges
	float right() const { return m_wide ? 747.0f : 640.0f; }
	float W() const { return m_W; }
	float H() const { return m_H; }

	// Primitives in pixels. `mode` as ui.frag: 9 texture (clamped), 10 texture (repeating), 11 flat; `add` draws additively.
	void Bind(VkDescriptorSet set); // VK_NULL_HANDLE: the SDF atlas
	void Tri(const float* xy, const float* uv, const uint32_t* rgba, int mode, bool add);
	// A triangle whose params.xy carry a second coordinate per corner (mode 14: the mask's ST).
	void TriP(const float* xy, const float* uv, const float* st, const uint32_t* rgba, int mode, bool add);
	// A set sampling `a` (binding 0) and `b` (binding 1), made once per pair.
	VkDescriptorSet PairSet(const KitTex& a, const KitTex& b);
	void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t rgba, int mode, bool add);
	void Fill(float x0, float y0, float x1, float y1, Gs c, int a = 128); // flat, normal blend (GS colours)
	void FillPx(float x0, float y0, float x1, float y1, float r, float g, float b, float a); // 0..1 colours
	// Gouraud quad, corners TL TR BL BR (GS colours 0..255 as displayed: 255 = white), alpha 0..1.
	void Gradient(float x0, float y0, float x1, float y1, const float c[4][3], float alpha = 1.0f);
	void Sprite(const KitTex& t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Gs c, int a, bool add);
	static uint32_t Pack(float r, float g, float b, float a); // 0..1 -> RGBA8

	// The BIOS font (spec/text.md 3). `x`, `y`: field units (pen y as in the spec); align 0 left, 1 centre, 2 right. Returns the width.
	int TextWidth(const std::string& s, float scale) const;
	int Text(const std::string& s, float x, float y, Gs c, float scale, int align = 0, int alpha = 128);
	// Fits `s` into `max_w` units by the BIOS's auto-fit scales from `scale` down (1, .875, .8, .77, .73, .7, .67, .61).
	float FitScale(const std::string& s, float scale, float max_w) const;

	// Button prompt: the Browser's TEXBBTTN (menu = false) or the main menu's TEXCMARU (24 / 25 x 13.75 units), then its label.
	void PromptAt(Button b, const std::string& label, float x, float y, int alpha = 128, bool menu = false);
	// The PS2's prompt row at y 230 (en slots: Square 24, Cross 213, Circle 335, Triangle right-aligned to 616), outer slots slid in 16:9.
	struct Hint
	{
		Button b;
		std::string label;
	};
	void Hints(const std::vector<Hint>& hints, int alpha = 128, bool menu = false, float y = 230.0f);

	// Browser flare (browser.md 5, 15.3): TEXBNAV2 halo, half size (h, h/2) field px at (cx, cy) pixels, additive.
	void Flare(float cx, float cy, float h, int grey = 118, int alpha = 128);
	// The Browser field: gradient set (browser.md 2.1) and the black bands (rows 0..31, 224..255).
	void BrowserField(const float corners[4][3]);
	void Bands();
	// A black fade over everything drawn so far (alpha 0..1).
	void Black(float alpha);
	// 4:3: black sides.
	void Sides();

	// Textures.
	const KitTex& T(const char* name) const; // a BIOS texture (an empty KitTex when missing)
	KitTex Upload(const Image& im);
	void Free(KitTex& t);
	// A cached texture for an image owned elsewhere (save icons): created once per key.
	const KitTex& Cached(const std::string& key, const Image& im);
	void DropCached(const std::string& prefix);

	// A 3D icon (ico) through the BIOS projection: `M` = model->view (column vectors, row-major 4x4: m[r*4+c]), projection
	// sx = cx + 1024 X/Z, sy = cy + 553.47 Y/Z (field px, cx/cy the projection centre in field units). Lit by 3 lights + ambient
	// (dirs model space; colours 0..1+), animated at time `t` (frames). `tex` the ico's texture (or none). Painter-sorted, back faces dropped.
	struct Light
	{
		float dir[3][3];
		float col[3][3];
		float amb[3];
		bool alpha_from_light = false; // device sets: alpha = 128 x (1 + light sum) (spec 15.2); icon.sys sets: opaque
	};
	void DrawIco(const Ico& ico, const float M[16], float cx, float cy, const Light& light, float frame, const KitTex* tex, float alpha = 1.0f,
		float tint_r = 1, float tint_g = 1, float tint_b = 1);

private:
	struct Glyph
	{
		int page; // 0 ascii, 1 osd
		int cx, cy, xo, w;
	};
	bool GlyphOf(unsigned char c, Glyph& g) const;
	void CloseRange();

	fe::Renderer* m_r = nullptr;
	const fe::Fonts* m_fonts = nullptr;
	const Bios* m_bios = nullptr;
	fe::FrameDesc* m_f = nullptr;
	float m_W = 1920, m_H = 1080, m_x0 = 0, m_sx = 2.25f, m_sy = 4.21875f;
	bool m_wide = true;
	VkDescriptorSet m_bound = VK_NULL_HANDLE;
	uint32_t m_range_first = 0;
	std::map<std::string, KitTex> m_tex;
	std::map<std::string, KitTex> m_cached;
	KitTex m_none;
	std::vector<KitTex> m_retire;
};
} // namespace s3
