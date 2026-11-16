// PS5 port frontend: the PS2 system software's look (ps2-console-shelf, AI-assisted): its palette, its thin outlined text
// with the cyan glow on the focus, the button prompts along the bottom, and the things that float in its screens (glass
// cubes, the clock's pillars, the disc, the main menu's orb, the boot towers), all as UI triangles in screen pixels for
// the renderer's UI pass (shaders/ui.frag modes 3 to 5).
//
// Measures are given in "concept pixels": the 1920 x 1080 artboards of the approved design. Scale() turns them into
// the display's pixels.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_math.h"
#include "fe_text.h"

#include <cstdint>
#include <vector>

namespace fe
{
namespace ps2
{
uint32_t Rgba(float r, float g, float b, float a = 1.0f);
uint32_t Alpha(uint32_t color, float a); // the colour's alpha times a

// The palette (the concept's design values).
uint32_t White(float a = 1.0f);  // #eceef4: items, details
uint32_t Grey(float a = 1.0f);   // #8d8f99: idle rows
uint32_t Cyan(float a = 1.0f);   // #6fd3f7: the focus
uint32_t Yellow(float a = 1.0f); // #e4ea7c: titles
uint32_t Detail(float a = 1.0f); // #c8cad4: the line under a title
uint32_t Warn(float a = 1.0f);   // a soft red for failures

// Thin OSD text: a soft drop shadow, a thin dark outline, the letters (light), and with `glow` > 0 a cyan bloom round
// them (the focused item). Returns the width. No allocation per call after the first (a scratch buffer is kept).
float Text(std::vector<UiVertex>& ui, const Fonts& fonts, const char* text, float x, float baseline, float px, uint32_t color,
	Fonts::Align align = Fonts::Left, float glow = 0.0f);

// A soft round or oval glow centred at (cx, cy), radii rx and ry; `falloff` 1 is wide, 4 tight.
void Glow(std::vector<UiVertex>& ui, float cx, float cy, float rx, float ry, uint32_t color, float falloff = 2.5f);
// A line from a to b, `width` pixels, with a glow `glow_px` wide at strength `glow` (0 none).
void Line(std::vector<UiVertex>& ui, Vec2 a, Vec2 b, float width, uint32_t color, float glow_px = 0.0f, float glow = 0.0f);
// A filled triangle and a filled quad (corners in order).
void Tri(std::vector<UiVertex>& ui, Vec2 a, Vec2 b, Vec2 c, uint32_t color);
void Quad(std::vector<UiVertex>& ui, Vec2 a, Vec2 b, Vec2 c, Vec2 d, uint32_t color);
// A PS2 disc, centre (cx, cy): `ax` and `ay` its two half axes (a tilted disc is an ellipse), turned by `angle`, `backing`
// 0 silver .. 1 the blue-backed PlayStation 2 disc.
void Disc(std::vector<UiVertex>& ui, float cx, float cy, Vec2 ax, Vec2 ay, float angle, float backing, float alpha);
// A glass cube of edge `size` pixels at (cx, cy), turned by yaw, pitch and roll: translucent faces, bright edges.
void Cube(std::vector<UiVertex>& ui, float cx, float cy, float size, float yaw, float pitch, float roll, float alpha);
// One of the clock screen's glass pillars round a hub: from `inner` to `inner + length` pixels out at `angle` (radians,
// 0 up, clockwise), `width` wide. `lit`: the bright one, the hand.
void Pillar(std::vector<UiVertex>& ui, Vec2 hub, float angle, float inner, float length, float width, float lit, float alpha);
// P2-19: System Configuration's clock, as the real PS2 draws it: twelve hexagonal glass rods round (hub), one an hour, on a ring
// tilted so its left and top come nearer, leaning out towards the eye. `R`: pixels for the ring's inner radius; `t`: the shelf's
// clock in seconds (never restarted); `lit_hour` 0..11: the pale, glowing rod (the hour of the day); `zoom`: the entry's scale.
// The glass's hue drifts teal..violet (~26 s) and the ring turns once round its lit rod's line every 75 s.
void ClockRods(std::vector<UiVertex>& ui, Vec2 hub, float R, float t, int lit_hour, float zoom, float alpha);
// P2-19: one of System Configuration's glass cubes: a lavender glass shell with a dark core, bright rims; `lit` 1 is the
// focused item's cube, deep blue.
void GlassCube(std::vector<UiVertex>& ui, float cx, float cy, float size, float yaw, float pitch, float roll, float lit, float alpha);
// The main menu's lights (P2-15): eight lights on a tilted ring round (cx, cy), radius R, that spread, spin, gather into a comet and
// open again on a ~60 s cycle. `te`: seconds since the menu came in.
void Orb(std::vector<UiVertex>& ui, float cx, float cy, float R, float te, float alpha);
// A thin glowing progress bar, `frac` 0..1.
void Bar(std::vector<UiVertex>& ui, float x, float y, float w, float frac, float s, float alpha = 1.0f);
// A memory-card-like source object (a USB drive) at (cx, cy), `w` wide, lit by `lit` 0..1.
void Drive(std::vector<UiVertex>& ui, float cx, float cy, float w, float lit, float alpha);
// The empty disc slot (P2-17): the PS2 Browser's silver disc, see-through, on the axes ax, ay (as Disc).
void EmptySlot(std::vector<UiVertex>& ui, float cx, float cy, Vec2 ax, Vec2 ay, float angle, float alpha);

// The button prompts along the bottom, as the PS2's (P2-18): the button's symbol in its PS2 colour, then the word. `glyph` is one of
// fe::icon's PromptFont glyphs, or a shoulder button's name starting with '#' ("#L1"). Laid out from `x` to the right
// (right_aligned false) or ending at `x` (true). Returns the row's width.
struct Prompt
{
	const char* glyph;
	const char* label;
};
float Prompts(std::vector<UiVertex>& ui, const Fonts& fonts, const Prompt* prompts, int count, float x, float baseline, float px,
	bool right_aligned);
// The width Prompts would take for that row.
float PromptsWidth(const Fonts& fonts, const Prompt* prompts, int count, float px);
} // namespace ps2
} // namespace fe
