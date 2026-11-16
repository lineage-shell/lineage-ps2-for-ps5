// PS2 shelf v3 (AI-assisted): the PS2 menu's own artwork, read at run time from the player's own BIOS image. Nothing from the
// BIOS is in the repo: this file holds only offsets, sizes, hashes and decode recipes (our own spec: ps2-own public/LOADER-SPEC.md).
// Only OSDSYS 2.00 Europe (SCPH-70004 family, ROMVER 0200EC20040614) is accepted; anything else is refused with a one-line reason
// and the shelf draws its generic artwork instead.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace s3
{
struct Image
{
	int w = 0, h = 0;
	std::vector<uint8_t> rgba; // straight alpha; GS alpha 0x80 (= 1.0) is stored as 255
	bool ok() const { return w > 0 && h > 0 && rgba.size() == static_cast<size_t>(w) * h * 4; }
};

// A PS2 3D icon (the save icon format; the BIOS's ICOB* members use it too).
struct Ico
{
	struct Vertex
	{
		std::vector<float> pos; // shape_count x (x, y, z), units of 1/1024 (the VU1 path's scale)
		float nx = 0, ny = 0, nz = 0;
		float u = 0, v = 0;
		uint8_t r = 128, g = 128, b = 128, a = 128;
	};
	struct Key
	{
		float time, value;
	};
	int shapes = 0;
	uint32_t tex_type = 0;
	std::vector<Vertex> verts;
	float anim_speed = 1, anim_offset = 0;
	uint32_t frame_length = 1;
	std::vector<std::vector<Key>> frames; // per shape (may be empty)
	Image tex;                            // 128x128 (none when tex_type lacks bit 2)
	bool ok() const { return shapes > 0 && !verts.empty(); }
};
// Parses `data` (a .ico file or an ICOB* member); false when it is not one.
bool ParseIco(const uint8_t* data, size_t size, Ico& out);

// One of the menu's sound effects (SNDOSDDH/SNDOSDDB): interleaved stereo float at 48 kHz, shaped by its ADSR.
struct Effect
{
	std::vector<float> stereo;
};

class Bios
{
public:
	// Looks for a supported image among the files of `dir` (any 4 MiB file). True when one was loaded.
	bool LoadDir(const std::string& dir);
	bool LoadFile(const std::string& path);
	bool ok() const { return m_ok; }
	const std::string& problem() const { return m_problem; } // why there are no BIOS assets ("" when ok)
	const std::string& path() const { return m_path; }

	const Image* Tex(const char* name) const; // TEXB*, TEXC*, TEXO* (decoded); null when absent
	const Image* Font(const char* name) const; // FNTASCII, FNTEXOSD
	const Ico* Icon(const char* name) const;  // ICOB*
	const Effect* Sound(int n) const;         // effect 0..13
	const std::vector<uint8_t>* Raw(const char* name) const;

private:
	bool Load(const std::vector<uint8_t>& rom);
	bool m_ok = false;
	std::string m_problem, m_path;
	std::map<std::string, std::vector<uint8_t>> m_raw;
	std::map<std::string, Image> m_tex, m_font;
	std::map<std::string, Ico> m_ico;
	std::vector<Effect> m_sfx;
};

// SPU2 ADPCM from byte `off` of `body` until the end flag, as 16-bit samples. `loop_start` (optional): the sample the block flagged
// "loop start" begins at, or -1; `loops`: the end block says "repeat".
std::vector<int16_t> SpuAdpcm(const std::vector<uint8_t>& body, size_t off, int* loop_start = nullptr, bool* loops = nullptr);

// The SPU2 envelope (ADSR1/ADSR2), one step per 48 kHz sample; KeyOff starts the release.
struct SpuEnvelope
{
	int phase = 0; // 0 attack, 1 decay, 2 sustain, 3 release
	int32_t level = 0;
	uint32_t counter = 0;
	uint16_t a1 = 0, a2 = 0;
	SpuEnvelope() = default;
	SpuEnvelope(uint16_t adsr1, uint16_t adsr2) : a1(adsr1), a2(adsr2) {}
	void KeyOff() { phase = 3; }
	float Next();
	bool Done() const { return phase == 3 && level <= 0; }

private:
	void Step(int rate, bool decrease, bool expo);
};

// The font metrics (spec/text.md 2; notes/global/glyphs.tsv): x offset in the 32-px cell and width, FNTASCII 0x20..0x7F (8 cells a row)
// and FNTEXOSD symbols 0..31 (16 cells a row). Cells are 40 texels high.
struct GlyphMetric
{
	uint8_t x, w;
};
extern const GlyphMetric kAsciiGlyphs[96];
extern const GlyphMetric kOsdGlyphs[32];
} // namespace s3
