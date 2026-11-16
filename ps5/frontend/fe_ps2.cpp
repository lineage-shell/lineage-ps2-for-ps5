// PS5 port frontend: the PS2 system software's look (see fe_ps2.h). ps2-console-shelf, AI-assisted; needs proper testing
// on the console (the UI shader's new modes have only run on a PC's Vulkan).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_ps2.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fe
{
namespace ps2
{
uint32_t Rgba(float r, float g, float b, float a)
{
	auto c = [](float v) { return static_cast<uint32_t>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
	return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

uint32_t Alpha(uint32_t color, float a)
{
	const uint32_t al = static_cast<uint32_t>(static_cast<float>(color >> 24) * Clamp(a, 0.0f, 1.0f) + 0.5f);
	return (color & 0x00FFFFFFu) | (al << 24);
}

uint32_t White(float a) { return Rgba(0.925f, 0.933f, 0.957f, a); }
uint32_t Grey(float a) { return Rgba(0.553f, 0.561f, 0.600f, a); }
uint32_t Cyan(float a) { return Rgba(0.435f, 0.827f, 0.969f, a); }
uint32_t Yellow(float a) { return Rgba(0.894f, 0.918f, 0.486f, a); }
uint32_t Detail(float a) { return Rgba(0.784f, 0.792f, 0.831f, a); }
uint32_t Warn(float a) { return Rgba(1.0f, 0.62f, 0.60f, a); }

namespace
{
UiVertex V(float x, float y, float u, float v, uint32_t c, float p0, float p1, float p2, float mode)
{
	return UiVertex{x, y, u, v, c, p0, p1, p2, mode};
}
} // namespace

float Text(std::vector<UiVertex>& ui, const Fonts& fonts, const char* text, float x, float baseline, float px, uint32_t color,
	Fonts::Align align, float glow)
{
	if (!text || !*text)
		return 0.0f;
	thread_local std::vector<UiVertex> scratch;
	scratch.clear();
	const float w = fonts.AddText(scratch, text, x, baseline, px, color, -0.18f, align);
	const float a = static_cast<float>(color >> 24) / 255.0f;
	if (a <= 0.003f)
		return w;
	const size_t n = scratch.size();
	ui.reserve(ui.size() + n * (glow > 0.0f ? 4 : 3));
	// The drop shadow: down and right, soft.
	const float off = std::max(1.0f, px * 0.045f);
	for (size_t i = 0; i < n; i++)
	{
		UiVertex v = scratch[i];
		v.x += off;
		v.y += off;
		v.p0 -= 0.10f;
		v.p1 = std::max(v.p1 * 2.5f, 0.05f);
		v.color = Rgba(0, 0, 0, 0.55f * a);
		ui.push_back(v);
	}
	// The cyan bloom of the focus: a wide, soft field.
	if (glow > 0.0f)
		for (size_t i = 0; i < n; i++)
		{
			UiVertex v = scratch[i];
			v.p0 = 0.30f;
			v.p1 = 0.16f;
			v.color = Cyan(0.42f * glow * a);
			ui.push_back(v);
		}
	// The thin dark outline.
	for (size_t i = 0; i < n; i++)
	{
		UiVertex v = scratch[i];
		v.p0 -= 0.075f;
		v.color = Rgba(0.0f, 0.0f, 0.02f, 0.80f * a);
		ui.push_back(v);
	}
	ui.insert(ui.end(), scratch.begin(), scratch.end());
	return w;
}

void Glow(std::vector<UiVertex>& ui, float cx, float cy, float rx, float ry, uint32_t color, float falloff)
{
	const UiVertex a = V(cx - rx, cy - ry, -1, -1, color, falloff, 0, 0, 3), b = V(cx + rx, cy - ry, 1, -1, color, falloff, 0, 0, 3);
	const UiVertex c = V(cx + rx, cy + ry, 1, 1, color, falloff, 0, 0, 3), d = V(cx - rx, cy + ry, -1, 1, color, falloff, 0, 0, 3);
	ui.insert(ui.end(), {a, b, c, a, c, d});
}

void Line(std::vector<UiVertex>& ui, Vec2 a, Vec2 b, float width, uint32_t color, float glow_px, float glow)
{
	const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
	if (len < 0.01f)
		return;
	const float hw = width * 0.5f, reach = hw + 1.0f + (glow > 0.0f ? glow_px * 2.0f : 0.0f);
	const float nx = -dy / len, ny = dx / len, tx = dx / len * 0.5f, ty = dy / len * 0.5f; // half a pixel past the ends
	auto p = [&](Vec2 o, float side, float ext) {
		return V(o.x + nx * side * reach + tx * ext, o.y + ny * side * reach + ty * ext, 0, side * reach, color, hw, glow_px, glow, 4);
	};
	const UiVertex q0 = p(a, -1, -1), q1 = p(b, -1, 1), q2 = p(b, 1, 1), q3 = p(a, 1, -1);
	ui.insert(ui.end(), {q0, q1, q2, q0, q2, q3});
}

void Tri(std::vector<UiVertex>& ui, Vec2 a, Vec2 b, Vec2 c, uint32_t color)
{
	ui.push_back(V(a.x, a.y, 0, 0, color, 1e5f, 1, 0, 4));
	ui.push_back(V(b.x, b.y, 0, 0, color, 1e5f, 1, 0, 4));
	ui.push_back(V(c.x, c.y, 0, 0, color, 1e5f, 1, 0, 4));
}

void Quad(std::vector<UiVertex>& ui, Vec2 a, Vec2 b, Vec2 c, Vec2 d, uint32_t color)
{
	Tri(ui, a, b, c, color);
	Tri(ui, a, c, d, color);
}

void Disc(std::vector<UiVertex>& ui, float cx, float cy, Vec2 ax, Vec2 ay, float angle, float backing, float alpha)
{
	const uint32_t c = Rgba(1, 1, 1, alpha);
	auto p = [&](float u, float v) { return V(cx + ax.x * u + ay.x * v, cy + ax.y * u + ay.y * v, u, v, c, angle, backing, 0, 5); };
	const UiVertex a = p(-1, -1), b = p(1, -1), d = p(1, 1), e = p(-1, 1);
	ui.insert(ui.end(), {a, b, d, a, d, e});
}

void Cube(std::vector<UiVertex>& ui, float cx, float cy, float size, float yaw, float pitch, float roll, float alpha)
{
	if (alpha <= 0.003f)
		return;
	// The corners, turned, then a mild perspective (a camera 4 edges away).
	const float h = 0.5f;
	Vec3 p[8];
	const float cyw = std::cos(yaw), syw = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
	Vec2 s[8];
	for (int i = 0; i < 8; i++)
	{
		Vec3 v((i & 1) ? h : -h, (i & 2) ? h : -h, (i & 4) ? h : -h);
		Vec3 a(v.x * cyw + v.z * syw, v.y, -v.x * syw + v.z * cyw);         // yaw
		Vec3 b(a.x, a.y * cp - a.z * sp, a.y * sp + a.z * cp);              // pitch
		Vec3 c(b.x * cr - b.y * sr, b.x * sr + b.y * cr, b.z);              // roll
		p[i] = c;
		const float persp = 4.0f / (4.0f - c.z);
		s[i] = {cx + c.x * size * persp, cy - c.y * size * persp};
	}
	// The faces, the far ones first: dim glass with a light inside; the edges bright, glowing.
	static const int kFaces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
	int order[6] = {0, 1, 2, 3, 4, 5};
	float depth[6];
	for (int f = 0; f < 6; f++)
	{
		depth[f] = 0;
		for (int k = 0; k < 4; k++)
			depth[f] += p[kFaces[f][k]].z;
	}
	std::sort(order, order + 6, [&](int a, int b) { return depth[a] < depth[b]; });
	for (int oi = 0; oi < 6; oi++)
	{
		const int* f = kFaces[order[oi]];
		const float facing = Clamp(depth[order[oi]] * 0.5f + 0.5f, 0.0f, 1.0f); // nearer faces a little brighter
		Quad(ui, s[f[0]], s[f[1]], s[f[2]], s[f[3]], Rgba(0.55f, 0.75f, 1.0f, alpha * (0.05f + 0.07f * facing)));
	}
	static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
	for (const auto& e : kEdges)
	{
		const float z = (p[e[0]].z + p[e[1]].z) * 0.5f;
		const float a = alpha * (0.40f + 0.35f * Clamp(z + 0.5f, 0.0f, 1.0f));
		Line(ui, s[e[0]], s[e[1]], std::max(1.2f, size * 0.012f), Rgba(0.80f, 0.92f, 1.0f, a), size * 0.05f, 0.35f);
	}
	Glow(ui, cx, cy, size * 0.55f, size * 0.55f, Rgba(0.55f, 0.78f, 1.0f, alpha * 0.18f), 2.0f);
}

void Pillar(std::vector<UiVertex>& ui, Vec2 hub, float angle, float inner, float length, float width, float lit, float alpha)
{
	if (alpha <= 0.003f)
		return;
	const float dx = std::sin(angle), dy = -std::cos(angle);
	const float nx = -dy, ny = dx;
	const Vec2 a = {hub.x + dx * inner, hub.y + dy * inner}, b = {hub.x + dx * (inner + length), hub.y + dy * (inner + length)};
	const float hw = width * 0.5f;
	// The glass: a dark blue body, a lighter core; lit, it is nearly white with a wide cyan glow.
	const Vec2 c0 = {a.x - nx * hw, a.y - ny * hw}, c1 = {b.x - nx * hw, b.y - ny * hw}, c2 = {b.x + nx * hw, b.y + ny * hw},
			   c3 = {a.x + nx * hw, a.y + ny * hw};
	const float l = Clamp(lit, 0.0f, 1.0f);
	if (l > 0.0f) // the lit one glows all round: soft blobs along it
		for (int i = 0; i <= 4; i++)
		{
			const float u = static_cast<float>(i) / 4.0f;
			Glow(ui, Mix(a.x, b.x, u), Mix(a.y, b.y, u), width * 1.6f, width * 1.6f, Rgba(0.55f, 0.88f, 1.0f, alpha * 0.30f * l), 2.2f);
		}
	Quad(ui, c0, c1, c2, c3, Rgba(Mix(0.16f, 0.62f, l), Mix(0.24f, 0.90f, l), Mix(0.66f, 1.0f, l), alpha * Mix(0.35f, 0.75f, l)));
	Line(ui, a, b, width * 0.34f, Rgba(Mix(0.43f, 0.91f, l), Mix(0.55f, 0.99f, l), 1.0f, alpha * Mix(0.45f, 1.0f, l)), width * 0.3f, 0.4f);
	Line(ui, c0, c1, 1.5f, Rgba(0.59f, 0.71f, 1.0f, alpha * Mix(0.45f, 0.8f, l)));
	Line(ui, c3, c2, 1.5f, Rgba(0.59f, 0.71f, 1.0f, alpha * Mix(0.45f, 0.8f, l)));
}

// P2-15: the main menu's lights, as measured from captures of the real PS2 menu (see the commit). Eight lights share one ring that
// lies tilted, seen from above. A cycle of ~60 s: RING (~51 s) the lights spread 45 degrees apart, drifting round while the ring
// spins about a diameter every ~3.4 s (so it reads as a circle, an ellipse, a line, every ~1.7 s); GATHER (~3 s) they close into a
// chain that slides round; COMET (~2.7 s) all merge into one bright orb with a thin trail, swinging ~190 degrees one way, lingering,
// and swinging back; UNFURL (~3 s) the chain opens into the ring again. On entering the menu they fly in and form the ring, and the
// chain and the comet follow within a few seconds, as on the console.
namespace {
constexpr float kOrbCycle = 60.0f, kGather = 3.0f, kComet = 2.7f, kUnfurl = 3.0f, kOrbLead = 2.0f;
constexpr int kOrbLights = 8;

float Ease(float e0, float e1, float x)
{
	x = Clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

struct OrbState
{
	float spread;   // 1: the ring, lights 45 degrees apart; 0: merged into one orb
	float tumble;   // how much the ring spins about its diameter (0 in the comet)
	float phi;      // the leading light's angle on the ring (radians)
};

OrbState OrbAt(float te)
{
	const float t = te + kOrbCycle - kOrbLead; // the gather starts kOrbLead s after the menu comes in
	const float cyc = std::floor(t / kOrbCycle), u = t - cyc * kOrbCycle;
	const float c0 = kGather, c1 = kGather + kComet, c2 = c1 + kUnfurl;
	OrbState o;
	o.spread = u < c0 ? 1.0f - Ease(0.0f, c0, u) : u < c1 ? 0.0f : Ease(c1, c2, u);
	o.tumble = Ease(0.55f, 1.0f, o.spread);
	// Drift ~25 deg/s; the gather and unfurl add 1.5 turns along the way (a smooth ramp, so the angle never jumps); the comet swings.
	const float ramp = 0.5f * Ease(0.0f, c0, u) + 0.5f * Ease(c1, c2, u); // the comet itself only swings (and drifts)
	const float swing = u > c0 - 0.5f && u < c1 + 0.5f ? std::sin(2.0f * kPi * (u - c0) / 3.4f) * (1.0f - Ease(0.0f, 0.25f, o.spread)) : 0.0f;
	o.phi = 0.44f * te + 3.0f * kPi * (cyc + ramp) + 1.65f * swing;
	return o;
}

// Light k's place on screen and its depth (-1 back .. 1 front). `R`: the ring's radius in pixels.
Vec2 OrbLight(const OrbState& o, int k, float te, float cx, float cy, float R, float& depth)
{
	const float gap = (0.044f + (2.0f * kPi / kOrbLights - 0.044f) * o.spread);
	const float a = o.phi - k * gap;
	float x = std::cos(a), y = std::sin(a), z = 0.0f;
	// The spin about a diameter (period ~3.4 s, its axis slowly turning), faded in with the spread.
	const float axis = 0.35f + 0.25f * std::sin(te * 0.05f), spin = 2.0f * kPi * te / 3.4f;
	{
		const float ca = std::cos(axis), sa = std::sin(axis);
		const float px = x * ca + y * sa, py = -x * sa + y * ca; // into the axis frame: px along the axis
		const float cs = std::cos(spin), ss = std::sin(spin);
		const float qy = py * cs, qz = py * ss;
		const float sx = px * ca - qy * sa, sy = px * sa + qy * ca;
		x = x + (sx - x) * o.tumble;
		y = y + (sy - y) * o.tumble;
		z = qz * o.tumble;
	}
	// The ring's rest pose: tilted back (seen from above: 0.75 as tall as wide) and rolled a little, slowly.
	const float tilt = 0.72f, roll = -0.30f + 0.12f * std::sin(te * 0.07f);
	const float y2 = y * std::cos(tilt) - z * std::sin(tilt), z2 = y * std::sin(tilt) + z * std::cos(tilt);
	const float cr = std::cos(roll), sr = std::sin(roll);
	depth = -z2;
	return {cx + R * (x * cr - y2 * sr), cy + R * (x * sr + y2 * cr)};
}
} // namespace

void Orb(std::vector<UiVertex>& ui, float cx, float cy, float R, float te, float alpha)
{
	const OrbState o = OrbAt(te);
	const float merge = 1.0f - Ease(0.0f, 0.35f, o.spread); // 1: one orb
	const float in = Ease(0.0f, 1.6f, te);                  // the lights fly in and form the ring
	const float lr = R * 0.10f;                              // a light's core (~0.014 of the screen height)
	// The comet's thin trail: the leading light a moment ago, faint (the "J" in stills of the menu).
	if (merge > 0.01f)
	{
		float d;
		Vec2 prev = OrbLight(o, 0, te, cx, cy, R, d);
		for (int i = 1; i <= 22; i++)
		{
			const float u = i / 22.0f, tb = te - u * 0.9f;
			const Vec2 p = OrbLight(OrbAt(tb), 0, tb, cx, cy, R, d);
			const float f = (1.0f - u) * (1.0f - u);
			Line(ui, prev, p, R * 0.025f * (1.0f - 0.6f * u) + 0.8f, Rgba(0.40f, 0.62f, 1.0f, alpha * in * merge * 0.5f * f), R * 0.05f, 0.5f);
			prev = p;
		}
	}
	for (int k = kOrbLights - 1; k >= 0; k--)
	{
		float depth;
		Vec2 p = OrbLight(o, k, te, cx, cy, R, depth);
		if (in < 1.0f) // scattered start: each light comes from its own place outside the ring
		{
			const float a = 2.4f * k + 0.7f, r = R * (2.2f + 0.5f * std::sin(k * 1.7f));
			const Vec2 from = {cx + r * std::cos(a), cy + r * 0.6f * std::sin(a)};
			p = {from.x + (p.x - from.x) * in, from.y + (p.y - from.y) * in};
		}
		const float near = 0.85f + 0.15f * depth, a = alpha * in * (0.75f + 0.25f * depth) * (1.0f - 0.55f * merge);
		Glow(ui, p.x, p.y, lr * 2.6f * near, lr * 2.6f * near, Rgba(0.35f, 0.62f, 1.0f, a * 0.5f), 2.2f);
		Glow(ui, p.x, p.y, lr * near, lr * near, Rgba(0.82f, 0.94f, 1.0f, a), 3.0f);
	}
	if (merge > 0.01f) // merged, the lights read as one bright orb
	{
		float d;
		const Vec2 h = OrbLight(o, kOrbLights / 2, te, cx, cy, R, d);
		const float pulse = 0.9f + 0.1f * std::sin(te * 2.2f), m = merge * alpha * in;
		Glow(ui, h.x, h.y, R * 0.42f * pulse, R * 0.42f * pulse, Rgba(0.35f, 0.65f, 1.0f, m * 0.45f), 2.2f);
		Glow(ui, h.x, h.y, R * 0.17f, R * 0.17f, Rgba(0.80f, 0.94f, 1.0f, m * 0.9f), 3.0f);
		Glow(ui, h.x, h.y, R * 0.07f, R * 0.07f, Rgba(1.0f, 1.0f, 1.0f, m), 2.0f);
	}
}

// ---- P2-19: System Configuration's 3D field, measured from footage of the real PS2's ---------------------------------------
namespace
{
Vec3 Cross3(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Norm3(Vec3 a)
{
	const float l = std::sqrt(Dot3(a, a));
	return l > 1e-6f ? a * (1.0f / l) : Vec3(0, 0, 1);
}
Vec2 Mix2(Vec2 a, Vec2 b, float t) { return {Mix(a.x, b.x, t), Mix(a.y, b.y, t)}; }
// v turned about the unit axis k by `angle` (Rodrigues).
Vec3 Turn(Vec3 v, Vec3 k, float angle)
{
	const float c = std::cos(angle), s = std::sin(angle);
	return v * c + Cross3(k, v) * s + k * (Dot3(k, v) * (1.0f - c));
}
// A triangle and a quad with a colour per corner (the shading runs along a rod, across a cube's face).
void TriV(std::vector<UiVertex>& ui, Vec2 a, Vec2 b, Vec2 c, uint32_t ca, uint32_t cb, uint32_t cc)
{
	auto v = [](Vec2 p, uint32_t col) { return V(p.x, p.y, 0, 0, col, 1e5f, 1, 0, 4); };
	ui.insert(ui.end(), {v(a, ca), v(b, cb), v(c, cc)});
}
void QuadV(std::vector<UiVertex>& ui, Vec2 a, Vec2 b, Vec2 c, Vec2 d, uint32_t ca, uint32_t cb, uint32_t cc, uint32_t cd)
{
	auto v = [](Vec2 p, uint32_t col) { return V(p.x, p.y, 0, 0, col, 1e5f, 1, 0, 4); };
	ui.insert(ui.end(), {v(a, ca), v(b, cb), v(c, cc), v(a, ca), v(c, cc), v(d, cd)});
}
// P2-19d: a frosted-glass quad (ui.frag mode 7): one colour with a fine grain fixed to the face, `cells` grain cells across it.
void FrostQuad(std::vector<UiVertex>& ui, Vec2 a, Vec2 b, Vec2 c, Vec2 d, float cells, uint32_t col, float grain)
{
	auto v = [&](Vec2 p, float u, float w) { return V(p.x, p.y, u, w, col, grain, 0, 0, 7); };
	ui.insert(ui.end(), {v(a, 0, 0), v(b, cells, 0), v(c, cells, cells), v(a, 0, 0), v(c, cells, cells), v(d, 0, cells)});
}
void HsvToRgb(float h, float s, float v, float* rgb)
{
	h = h - std::floor(h);
	const float i = std::floor(h * 6.0f), f = h * 6.0f - i, p = v * (1 - s), q = v * (1 - s * f), u = v * (1 - s * (1 - f));
	const int k = static_cast<int>(i) % 6;
	const float r[6] = {v, q, p, p, u, v}, g[6] = {u, v, v, q, p, p}, b[6] = {p, p, u, v, v, q};
	rgb[0] = r[k];
	rgb[1] = g[k];
	rgb[2] = b[k];
}
float Hash1(int n)
{
	const float x = std::sin(static_cast<float>(n) * 12.9898f + 4.1414f) * 43758.5453f;
	return x - std::floor(x);
}
const Vec3 kLight = Norm3(Vec3(-0.45f, 0.65f, 0.62f));
const Vec3 kFill = Norm3(Vec3(0.35f, -0.70f, 0.55f)); // P2-19d: the cubes' weaker light from below right // from the top left, in front: the rods' and cubes' lit faces
} // namespace

void ClockRods(std::vector<UiVertex>& ui, Vec2 hub, float R, float t, int lit_hour, float zoom, float alpha)
{
	if (alpha <= 0.003f)
		return;
	constexpr int kRods = 12;
	// The glass's hue drifts teal (195) .. blue .. violet (255) and back, ~26 s a round (the footage: teal at 0:39 and 1:05,
	// violet at 0:50 and 1:15).
	float glass[3];
	HsvToRgb((225.0f + 30.0f * std::sin(t * 2.0f * kPi / 26.0f)) / 360.0f, 0.80f, 0.46f, glass); // P2-19b: deep and saturated, as sampled
	const int lit = ((lit_hour % 12) + 12) % 12;
	// Once every 75 s the ring turns right round about the lit rod's line in ~10 s (seen edge-on part way: the footage's
	// 0:52-1:00); in between it only sways a little.
	const float cyc = std::fmod(t + 45.0f, 75.0f); // (first 30 s after the shelf starts)
	const float flip = cyc < 10.0f ? 2.0f * kPi * Smoothstep(0.0f, 1.0f, cyc / 10.0f) : 0.0f;
	const float yaw = Radians(-30.0f) + Radians(3.0f) * std::sin(t * 2.0f * kPi / 21.0f);
	const float pitch = Radians(-14.0f) + Radians(2.0f) * std::sin(t * 2.0f * kPi / 17.0f + 1.0f);
	const Vec3 axis = Vec3(std::sin(lit * kPi / 6.0f), std::cos(lit * kPi / 6.0f), 0.0f);
	auto place = [&](Vec3 v) {
		v = Turn(v, axis, flip);
		v = Vec3(v.x * std::cos(yaw) - v.z * std::sin(yaw), v.y, v.x * std::sin(yaw) + v.z * std::cos(yaw)); // the right comes nearer: its rods point at the eye (short), the left ones lie across it (long)
		return Vec3(v.x, v.y * std::cos(pitch) - v.z * std::sin(pitch), v.y * std::sin(pitch) + v.z * std::cos(pitch)); // and the bottom
	};
	const float D = 7.0f;
	auto proj = [&](Vec3 v) {
		const float k = R * zoom * D / (D - v.z);
		return Vec2{hub.x + v.x * k, hub.y - v.y * k};
	};
	struct Rod
	{
		Vec3 p0, p1, d;
		float z;
		int k;
	};
	Rod rods[kRods];
	for (int k = 0; k < kRods; k++)
	{
		const float a = k * kPi / 6.0f;
		const Vec3 u(std::sin(a), std::cos(a), 0.0f);
		const float cone = Radians(20.0f); // the rods lean out towards the eye: their outer ends show
		const Vec3 d = place(Norm3(u * std::cos(cone) + Vec3(0, 0, 1) * std::sin(cone)));
		const float r0 = 1.0f + 0.10f * Hash1(k * 3 + 1), len = 1.75f + 0.25f * Hash1(k * 7 + 2);
		const Vec3 p0 = place(u * r0);
		rods[k] = {p0, p0 + d * len, d, 0, k};
		rods[k].z = (p0.z + rods[k].p1.z) * 0.5f;
	}
	std::sort(rods, rods + kRods, [](const Rod& a, const Rod& b) { return a.z < b.z; });
	// P2-19b: the glass as the footage shows it, close up: deep, saturated faces (blue ~#0c143c..#283373, teal ~#08384a..#59bee2 at
	// its bright edge), no outlines. The facets come from shading alone: each face the eye sees is a grid of shaded points
	// (5 across, 8 along) carrying a fresnel rim, soft light streaks where faces meet (the refraction the PS2's rods show), a
	// highlight sliding slowly along the length, darker into the hub and brighter at the end, and faint banding along it.
	const float w = 0.20f; // the hexagon's radius (a rod is ~0.065 of the screen's height across, as measured close up)
	const Vec3 half = Norm3(kLight + Vec3(0, 0, 1));
	for (const Rod& rod : rods)
	{
		const bool is_lit = rod.k == lit;
		float body[3] = {glass[0], glass[1], glass[2]};
		if (is_lit) // the hour: pale cyan, glowing
		{
			body[0] = 0.40f;
			body[1] = 0.74f;
			body[2] = 1.0f;
		}
		const float bmax = std::max(body[0], std::max(body[1], body[2]));
		const float hi[3] = {Mix(body[0] / bmax, 1.0f, 0.35f), Mix(body[1] / bmax, 1.0f, 0.35f), Mix(body[2] / bmax, 1.0f, 0.35f)}; // the glass's own light
		const Vec3 d = rod.d;
		Vec3 e1 = Cross3(d, Vec3(0, 0, 1));
		if (Dot3(e1, e1) < 1e-4f)
			e1 = Vec3(1, 0, 0);
		e1 = Norm3(e1);
		const Vec3 e2 = Cross3(d, e1);
		const float spin = Hash1(rod.k * 5 + 3) * kPi / 3.0f, phase = Hash1(rod.k * 13 + 7) * 6.28f;
		const float len = std::sqrt(Dot3(rod.p1 - rod.p0, rod.p1 - rod.p0));
		const float slide = std::fmod(t / 7.0f + Hash1(rod.k * 11 + 5), 1.0f) * 1.3f - 0.15f; // the highlight's place along the rod
		Vec3 c0[6], c1[6];
		for (int j = 0; j < 6; j++)
		{
			const float a = spin + j * kPi / 3.0f;
			const Vec3 o = e1 * (w * std::cos(a)) + e2 * (w * std::sin(a));
			c0[j] = rod.p0 + o;
			c1[j] = rod.p1 + o;
		}
		float facing[6];
		Vec3 nrm[6];
		int order[6];
		for (int j = 0; j < 6; j++)
		{
			const float a = spin + (j + 0.5f) * kPi / 3.0f;
			nrm[j] = e1 * std::cos(a) + e2 * std::sin(a);
			facing[j] = nrm[j].z;
			order[j] = j;
		}
		std::sort(order, order + 6, [&](int a, int b) { return facing[a] < facing[b]; });
		const float body_a = alpha * (is_lit ? 0.84f : 0.76f); // P2-19c: see-through: the field shows through, as in the footage
		auto colour = [&](float r, float g, float b, float a) { return Rgba(r, g, b, a); };
		// P2-19d: both ends, always: each hexagon faded and lit by how squarely it faces the eye (|n.z|), so nothing pops as the rod
		// turns; an end turned away is drawn before the body (seen through it), one turned towards the eye after.
		auto end_cap = [&](bool outer) {
			const Vec3 nrm_c = outer ? d : d * -1.0f;
			const Vec3 centre = outer ? rod.p1 : rod.p0;
			const Vec3* ring = outer ? c1 : c0;
			const float k = Smoothstep(0.0f, 0.6f, std::fabs(nrm_c.z));
			const float ca = body_a * 0.85f * k * (nrm_c.z < 0.0f ? 0.5f : 1.0f);
			if (ca <= 0.004f)
				return;
			const float cl = 0.22f + 0.55f * std::max(0.0f, Dot3(nrm_c, kLight));
			const Vec2 mid = proj(centre);
			const uint32_t cm = colour(body[0] * cl, body[1] * cl, body[2] * cl, ca);
			for (int j = 0; j < 6; j++)
			{
				const int n = (j + 1) % 6;
				const float rj = 0.18f * k * std::max(0.0f, Dot3(Norm3(ring[j] - centre), kLight));
				const float rn = 0.18f * k * std::max(0.0f, Dot3(Norm3(ring[n] - centre), kLight));
				TriV(ui, mid, proj(ring[j]), proj(ring[n]), cm, colour(body[0] * cl + hi[0] * rj, body[1] * cl + hi[1] * rj, body[2] * cl + hi[2] * rj, ca),
					colour(body[0] * cl + hi[0] * rn, body[1] * cl + hi[1] * rn, body[2] * cl + hi[2] * rn, ca));
			}
		};
		if (d.z < 0.0f)
			end_cap(true);
		else
			end_cap(false);
		for (int oi = 0; oi < 6; oi++)
		{
			const int j = order[oi], n = (j + 1) % 6;
			if (facing[j] <= 0.0f) // turned away: only seen dimly through the near faces
			{
				QuadV(ui, proj(c0[j]), proj(c1[j]), proj(c1[n]), proj(c0[n]), colour(body[0] * 0.08f, body[1] * 0.08f, body[2] * 0.1f, alpha * 0.30f),
					colour(body[0] * 0.30f, body[1] * 0.30f, body[2] * 0.36f, alpha * 0.30f), colour(body[0] * 0.30f, body[1] * 0.30f, body[2] * 0.36f, alpha * 0.30f),
					colour(body[0] * 0.08f, body[1] * 0.08f, body[2] * 0.1f, alpha * 0.30f));
				continue;
			}
			const float f = facing[j], diff = std::max(0.0f, Dot3(nrm[j], kLight));
			const float spec = std::pow(std::max(0.0f, Dot3(nrm[j], half)), 6.0f);
			const float fres = std::pow(1.0f - f, 3.0f);
			// How bright each long edge of this face is: brighter where the neighbouring face turns away (the silhouette).
			const float e_prev = 0.25f + 0.75f * std::pow(1.0f - std::max(0.0f, facing[(j + 5) % 6]), 2.0f);
			const float e_next = 0.25f + 0.75f * std::pow(1.0f - std::max(0.0f, facing[n]), 2.0f);
			constexpr int kW = 5, kU = 8;
			static const float ws[kW] = {0.0f, 0.12f, 0.5f, 0.88f, 1.0f};
			Vec2 pt[kW][kU];
			uint32_t col[kW][kU];
			for (int a = 0; a < kW; a++)
				for (int b = 0; b < kU; b++)
				{
					const float wv = ws[a], u = static_cast<float>(b) / (kU - 1);
					const Vec3 p0 = c0[j] + (c0[n] - c0[j]) * wv, p1 = c1[j] + (c1[n] - c1[j]) * wv;
					pt[a][b] = proj(p0 + (p1 - p0) * u);
					const float grad = Mix(0.30f, 1.0f, Smoothstep(0.0f, 0.75f, u)) + 0.3f * Smoothstep(0.82f, 1.0f, u);
					const float band = 1.0f + 0.12f * std::sin(u * len * 7.0f + wv * 2.2f + phase);
					const float edge = std::exp(-(wv * wv) / 0.012f) * e_prev + std::exp(-((1.0f - wv) * (1.0f - wv)) / 0.012f) * e_next;
					const float streak = std::exp(-((u - slide) * (u - slide)) / 0.012f) * (0.25f + spec);
					const float lum = (0.22f + 0.60f * diff) * grad * band * (0.75f + 0.5f * fres); // the face towards the eye darkest
					const float light = (0.55f * fres * grad + 0.30f * edge * (0.4f + diff) + 0.45f * streak + 0.30f * spec * grad) * (is_lit ? 1.4f : 1.0f);
					col[a][b] = colour(body[0] * lum + hi[0] * light, body[1] * lum + hi[1] * light, body[2] * lum + hi[2] * light, body_a);
				}
			for (int a = 0; a + 1 < kW; a++)
				for (int b = 0; b + 1 < kU; b++)
					QuadV(ui, pt[a][b], pt[a][b + 1], pt[a + 1][b + 1], pt[a + 1][b], col[a][b], col[a][b + 1], col[a + 1][b + 1], col[a + 1][b]);
		}
		if (d.z >= 0.0f)
			end_cap(true);
		else
			end_cap(false);
		if (is_lit) // the hour glows all along
			for (int i = 0; i <= 4; i++)
			{
				const Vec2 p = proj(rod.p0 + (rod.p1 - rod.p0) * (0.2f + 0.2f * i));
				Glow(ui, p.x, p.y, R * zoom * 0.42f, R * zoom * 0.42f, Rgba(0.45f, 0.80f, 1.0f, alpha * 0.16f), 2.2f);
			}
	}
}

void GlassCube(std::vector<UiVertex>& ui, float cx, float cy, float size, float yaw, float pitch, float roll, float lit, float alpha)
{
	if (alpha <= 0.003f || size < 1.0f)
		return;
	const float l = Clamp(lit, 0.0f, 1.0f);
	const float cyw = std::cos(yaw), syw = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
	auto turn = [&](Vec3 v) {
		const Vec3 a(v.x * cyw + v.z * syw, v.y, -v.x * syw + v.z * cyw);
		const Vec3 b(a.x, a.y * cp - a.z * sp, a.y * sp + a.z * cp);
		return Vec3(b.x * cr - b.y * sr, b.x * sr + b.y * cr, b.z);
	};
	auto proj = [&](Vec3 c) {
		const float persp = 4.0f / (4.0f - c.z);
		return Vec2{cx + c.x * size * persp, cy - c.y * size * persp};
	};
	static const int kFaces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
	// P2-19d: frosted glass, as the footage shows it (design notes, ps2/audit/p219d): a translucent lavender block with a fine
	// grain on its faces, each face lit flat (lighter turned to the light, darker away), mild perspective, the field and the
	// rods showing through. Six real faces, far ones first, near ones over them: what shows dark through a face is only what is
	// behind the block. A few edges catch the light as thin, crisp glints (from their two faces' half normal). The focused cube is
	// solid saturated blue and glows. No windows, no bands, no inner geometry, no outlines.
	static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
	Vec3 p[8];
	Vec2 s[8];
	for (int i = 0; i < 8; i++)
	{
		p[i] = turn(Vec3((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f));
		s[i] = proj(p[i]);
	}
	const float glass[3] = {Mix(0.64f, 0.16f, l), Mix(0.52f, 0.38f, l), Mix(0.92f, 0.98f, l)};
	const Vec3 half = Norm3(kLight + Vec3(0, 0, 1));
	Vec3 nrm[6];
	float depth[6];
	int order[6];
	for (int f = 0; f < 6; f++)
	{
		Vec3 c(0, 0, 0);
		for (int k = 0; k < 4; k++)
			c = c + p[kFaces[f][k]];
		nrm[f] = Norm3(c);
		depth[f] = c.z;
		order[f] = f;
	}
	std::sort(order, order + 6, [&](int a, int b) { return depth[a] < depth[b]; });
	const float cells = std::max(4.0f, size / 2.6f); // the grain: ~2.6 px a cell at this size
	for (int oi = 0; oi < 6; oi++)
	{
		const int f = order[oi];
		const int* q = kFaces[f];
		const bool front = nrm[f].z > 0.0f;
		// The far faces are lit on their inner side (the light passing through), dimmer; the near ones flat by their normal.
		const Vec3 nf = front ? nrm[f] : nrm[f] * -1.0f;
		// The key light from the top left and a weaker fill from below (the footage lights downward faces lavender too).
		const float diff = std::max(std::max(0.0f, Dot3(nf, kLight)), 0.45f * std::max(0.0f, Dot3(nf, kFill)));
		// A face turned to the light is lavender; turned away, a dim grey-violet (sampled ~#2d2a37 .. #78699f in the footage).
		const float lit_f = std::pow(diff, 1.15f); // P2-19e: graded, as the footage's faces
		const float grey[3] = {Mix(0.17f, 0.07f, l), Mix(0.16f, 0.17f, l), Mix(0.22f, 0.55f, l)};
		const float lum = front ? 0.45f + 0.85f * lit_f : 0.40f + 0.25f * lit_f; // P2-19e: more contrast lit / unlit
		const float mixl = front ? lit_f : 0.3f * lit_f;
		const float rgb[3] = {Mix(grey[0], glass[0], mixl) * lum, Mix(grey[1], glass[1], mixl) * lum, Mix(grey[2], glass[2], mixl) * lum};
		const float a = alpha * (front ? Mix(0.38f + 0.36f * lit_f, 0.86f, l) : Mix(0.22f, 0.75f, l)); // P2-19e: see-through, the field and rods behind showing
		const float g = Mix(0.03f, 0.02f, l); // P2-19e: smooth glass, the grain barely there
		FrostQuad(ui, s[q[0]], s[q[1]], s[q[2]], s[q[3]], cells, Rgba(rgb[0], rgb[1], rgb[2], a), g);
	}
	// The glints: an edge whose half normal meets the light's, thin and crisp, not reaching the corners.
	for (const auto& e : kEdges)
	{
		int fs[2], n = 0;
		for (int f = 0; f < 6 && n < 2; f++)
		{
			int hits = 0;
			for (int k = 0; k < 4; k++)
				hits += kFaces[f][k] == e[0] || kFaces[f][k] == e[1];
			if (hits == 2)
				fs[n++] = f;
		}
		if (n < 2 || (nrm[fs[0]].z <= 0.0f && nrm[fs[1]].z <= 0.0f))
			continue;
		const float spec = std::pow(std::max(0.0f, Dot3(Norm3(nrm[fs[0]] + nrm[fs[1]]), half)), 14.0f); // P2-19e: wider, so a few at a time
		if (spec < 0.06f)
			continue;
		Line(ui, Mix2(s[e[0]], s[e[1]], 0.06f), Mix2(s[e[0]], s[e[1]], 0.94f), std::max(1.2f, size * 0.014f),
			Rgba(Mix(0.97f, 0.80f, l), Mix(0.95f, 0.94f, l), 1.0f, alpha * std::min(1.0f, spec * 1.5f)), size * 0.03f, 0.35f);
	}
	if (l > 0.01f)
		Glow(ui, cx, cy, size * 0.85f, size * 0.85f, Rgba(0.30f, 0.55f, 1.0f, alpha * 0.30f * l), 2.0f); // the focused one glows
}

void Bar(std::vector<UiVertex>& ui, float x, float y, float w, float frac, float s, float alpha)
{
	const float h = 4.0f * s;
	Line(ui, {x, y}, {x + w, y}, h, Rgba(0.55f, 0.59f, 0.69f, 0.25f * alpha));
	frac = Clamp(frac, 0.0f, 1.0f);
	if (frac > 0.0f)
		Line(ui, {x, y}, {x + w * frac, y}, h, Rgba(0.60f, 0.88f, 1.0f, alpha), 8.0f * s, 0.7f);
}

void Drive(std::vector<UiVertex>& ui, float cx, float cy, float w, float lit, float alpha)
{
	// A small dark slab with a metal plug and an activity light, seen a little from above.
	const float h = w * 0.6f, skew = w * 0.12f;
	const Vec2 a = {cx - w * 0.5f + skew, cy - h * 0.5f}, b = {cx + w * 0.5f + skew, cy - h * 0.5f}, c = {cx + w * 0.5f - skew, cy + h * 0.5f},
			   d = {cx - w * 0.5f - skew, cy + h * 0.5f};
	Quad(ui, a, b, c, d, Rgba(0.16f, 0.17f, 0.20f, alpha));
	Quad(ui, a, b, {b.x - skew * 0.3f, b.y + h * 0.18f}, {a.x - skew * 0.3f, a.y + h * 0.18f}, Rgba(0.42f, 0.44f, 0.50f, alpha));
	Line(ui, a, b, 1.5f, Rgba(1, 1, 1, 0.25f * alpha));
	const float px = cx + w * 0.5f, ph = h * 0.36f;
	Quad(ui, {px + 0.0f, cy - ph * 0.5f}, {px + w * 0.25f, cy - ph * 0.5f}, {px + w * 0.25f - skew * 0.2f, cy + ph * 0.5f},
		{px - skew * 0.2f, cy + ph * 0.5f}, Rgba(0.62f, 0.64f, 0.68f, alpha));
	Glow(ui, cx - w * 0.28f, cy + h * 0.05f, w * 0.07f, w * 0.07f, Rgba(0.48f, 0.82f, 1.0f, alpha * (0.55f + 0.45f * lit)), 2.0f);
}

void EmptySlot(std::vector<UiVertex>& ui, float cx, float cy, Vec2 ax, Vec2 ay, float angle, float alpha)
{
	// P2-17: the PS2 Browser's silver disc, ghosted (see-through): no disc is in. Its glints still turn.
	Glow(ui, cx, cy, std::hypot(ax.x, ax.y) * 1.25f, std::hypot(ay.x, ay.y) * 1.6f, Rgba(1, 1, 1, 0.10f * alpha), 2.0f);
	Disc(ui, cx, cy, ax, ay, angle, 0.0f, 0.42f * alpha);
}

// P2-18: the PS2's prompts are the button's symbol alone in its colour, then the word (no plate under it; see
// refs/board/05-memcard-list.jpg). The symbols are drawn as strokes, not PromptFont glyphs (those are filled discs with
// the symbol cut out, which made Circle a red button and left the others off the centre of their plates), so each one is
// centred exactly on the word's cap height. Shoulder names ("#L1") sit on the words' baseline, their capitals as tall as
// the symbols.
namespace
{
constexpr float kSymbolSlot = 0.74f; // a symbol's room, in em of the words
constexpr float kSymbolGap = 0.24f;  // from the symbol to its word
constexpr float kCapHeight = 0.711f; // Roboto's capitals, in em
constexpr float kShoulderPx = 0.86f; // a shoulder name's size: its capitals as tall as the symbols

float PromptGlyphWidth(const Fonts& fonts, const char* g, float px)
{
	return g[0] == '#' ? fonts.Measure(g + 1, px * kShoulderPx) : px * kSymbolSlot;
}

// One face button's symbol centred at (cx, cy) for words `px` tall: a soft shadow, a thin dark outline, then the colour.
void Symbol(std::vector<UiVertex>& ui, const char* g, float cx, float cy, float px)
{
	enum
	{
		Cross,
		Circle,
		Triangle,
		Square,
		None
	} kind = !std::strcmp(g, icon::Cross) ? Cross : !std::strcmp(g, icon::Circle) ? Circle : !std::strcmp(g, icon::Triangle) ? Triangle
		: !std::strcmp(g, icon::Square) ? Square : None;
	if (kind == None)
		return;
	static const uint32_t colors[] = {Rgba(0.50f, 0.56f, 0.98f), Rgba(0.95f, 0.36f, 0.40f), Rgba(0.33f, 0.82f, 0.55f), Rgba(0.93f, 0.50f, 0.80f)};
	// Outer sizes (the stroke included), set by eye against the PS2's: a square reads bigger than a circle of its width.
	// Whole pixels for the stroke and the straight edges, so the square's sides and the triangle's base stay sharp.
	const float stroke = std::max(2.0f, std::round(px * 0.095f)), hs = stroke * 0.5f;
	auto snap = [](float centre, float half) { return std::round(centre - half) + half; }; // a centre whose edges (centre +- half) are whole
	Vec2 pts[40];
	int n = 0;
	switch (kind)
	{
		case Circle:
		{
			const float r = px * 0.35f - hs;
			cx = std::round(cx);
			cy = std::round(cy);
			for (; n < 40; n++)
				pts[n] = {cx + std::cos(2.0f * kPi * n / 40) * r, cy + std::sin(2.0f * kPi * n / 40) * r};
			break;
		}
		case Square:
		{
			const float o = std::round(px * 0.30f), h = o - hs;
			cx = snap(cx, o);
			cy = snap(cy, o);
			pts[n++] = {cx - h, cy - h};
			pts[n++] = {cx + h, cy - h};
			pts[n++] = {cx + h, cy + h};
			pts[n++] = {cx - h, cy + h};
			break;
		}
		case Triangle:
		{
			// Its box centred a little high (a triangle's weight is in its base), as the PS2's: the base near the words' baseline,
			// the tip at their cap height. The mitred corners reach 2 half strokes past the tip and 1.73 past the base's ends.
			const float m = cy - px * 0.025f, ob = std::round(m + px * 0.33f), ot = ob - px * 0.66f, w = px * 0.37f - hs * 1.732f;
			cx = std::round(cx);
			pts[n++] = {cx, ot + hs * 2.0f};
			pts[n++] = {cx + w, ob - hs};
			pts[n++] = {cx - w, ob - hs};
			break;
		}
		default:
			break;
	}
	// A closed outline through pts, `width` wide, mitred: each side is one quad between the corners' mitre lines, its
	// coordinate across the stroke kept for Line's soft edge (shader mode 4), so the corners close with no overlap.
	auto outline = [&](float dx, float dy, float width, uint32_t color) {
		const float hw = width * 0.5f, reach = hw + 1.0f;
		auto normal = [&](int i) { // the outward normal of side i (pts[i] to pts[i + 1]; the points go clockwise on screen)
			const Vec2 a = pts[i], b = pts[(i + 1) % n];
			const float ex = b.x - a.x, ey = b.y - a.y, l = std::sqrt(ex * ex + ey * ey);
			return Vec2{ey / l, -ex / l};
		};
		auto mitre = [&](int i) { // the offset at corner i for a distance of 1 out
			const Vec2 p = normal((i + n - 1) % n), q = normal(i);
			const float k = 1.0f / (1.0f + p.x * q.x + p.y * q.y);
			return Vec2{(p.x + q.x) * k, (p.y + q.y) * k};
		};
		for (int i = 0; i < n; i++)
		{
			const int j = (i + 1) % n;
			const Vec2 mi = mitre(i), mj = mitre(j);
			auto v = [&](int c, Vec2 mv, float side) {
				return V(pts[c].x + dx + mv.x * side * reach, pts[c].y + dy + mv.y * side * reach, 0, side * reach, color, hw, 0, 0, 4);
			};
			const UiVertex q0 = v(i, mi, 1), q1 = v(j, mj, 1), q2 = v(j, mj, -1), q3 = v(i, mi, -1);
			ui.insert(ui.end(), {q0, q1, q2, q0, q2, q3});
		}
	};
	auto cross = [&](float dx, float dy, float width, uint32_t color) {
		const float h = px * 0.30f - hs * 0.7071f; // the ends' outer corners reach the box
		const float x = std::round(cx) + dx, y = std::round(cy) + dy;
		Line(ui, {x - h, y - h}, {x + h, y + h}, width, color);
		Line(ui, {x + h, y - h}, {x - h, y + h}, width, color);
	};
	auto draw = [&](float dx, float dy, float width, uint32_t color) {
		if (kind == Cross)
			cross(dx, dy, width, color);
		else
			outline(dx, dy, width, color);
	};
	const float off = std::max(1.0f, std::round(px * 0.045f)), rim = std::max(1.0f, std::round(px * 0.03f));
	draw(off, off, stroke + rim, Rgba(0, 0, 0, 0.45f)); // the shadow, as Text's: down and right
	draw(0, 0, stroke + rim * 2.0f, Rgba(0.0f, 0.0f, 0.02f, 0.75f)); // the thin dark outline
	draw(0, 0, stroke, colors[kind]);
}
} // namespace

float PromptsWidth(const Fonts& fonts, const Prompt* prompts, int count, float px)
{
	float total = 0;
	for (int i = 0; i < count; i++)
		total += PromptGlyphWidth(fonts, prompts[i].glyph, px) + px * kSymbolGap + fonts.Measure(prompts[i].label, px) + (i + 1 < count ? px * 1.5f : 0.0f);
	return total;
}

float Prompts(std::vector<UiVertex>& ui, const Fonts& fonts, const Prompt* prompts, int count, float x, float baseline, float px,
	bool right_aligned)
{
	const float total = PromptsWidth(fonts, prompts, count, px);
	float cx = right_aligned ? x - total : x;
	for (int i = 0; i < count; i++)
	{
		const char* g = prompts[i].glyph;
		const float gw = PromptGlyphWidth(fonts, g, px);
		if (g[0] == '#')
			Text(ui, fonts, g + 1, cx, baseline, px * kShoulderPx, Detail());
		else
			Symbol(ui, g, cx + gw * 0.5f, baseline - px * kCapHeight * 0.5f, px);
		cx += gw + px * kSymbolGap;
		cx += Text(ui, fonts, prompts[i].label, cx, baseline, px, White());
		cx += px * 1.5f;
	}
	return total;
}
} // namespace ps2
} // namespace fe
