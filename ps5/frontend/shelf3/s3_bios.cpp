// PS2 shelf v3 (AI-assisted): the BIOS asset loader (see s3_bios.h). A port of our own public/tools/bios_loader.py.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "s3_bios.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace s3
{
const GlyphMetric kAsciiGlyphs[96] = {{0, 13}, {11, 10}, {9, 14}, {4, 25}, {5, 23}, {2, 29}, {4, 23}, {12, 9}, {9, 13}, {10, 13}, {9, 14},
	{3, 26}, {11, 10}, {8, 16}, {11, 10}, {6, 19}, {5, 23}, {5, 23}, {5, 23}, {5, 23}, {5, 23}, {5, 23}, {5, 23}, {5, 23}, {5, 23}, {5, 23},
	{11, 10}, {11, 10}, {7, 18}, {4, 25}, {7, 18}, {6, 21}, {3, 26}, {4, 25}, {5, 23}, {4, 25}, {5, 24}, {5, 22}, {6, 21}, {3, 26}, {4, 24},
	{11, 10}, {7, 19}, {4, 24}, {6, 21}, {3, 27}, {4, 24}, {3, 27}, {5, 22}, {3, 27}, {5, 23}, {4, 24}, {5, 23}, {5, 23}, {5, 24}, {0, 32},
	{5, 23}, {4, 24}, {5, 23}, {10, 13}, {7, 19}, {10, 13}, {5, 23}, {6, 21}, {11, 11}, {6, 21}, {6, 21}, {6, 20}, {6, 21}, {6, 20}, {9, 15},
	{6, 20}, {7, 19}, {11, 10}, {10, 12}, {6, 20}, {12, 9}, {3, 27}, {7, 19}, {6, 21}, {6, 21}, {5, 21}, {9, 15}, {7, 19}, {9, 15}, {6, 20},
	{7, 20}, {3, 27}, {6, 20}, {6, 20}, {6, 19}, {8, 15}, {11, 10}, {9, 15}, {8, 16}, {6, 21}};
const GlyphMetric kOsdGlyphs[32] = {{5, 23}, {0, 32}, {8, 16}, {8, 16}, {3, 24}, {9, 15}, {1, 29}, {1, 29}, {6, 19}, {6, 19}, {0, 32},
	{0, 32}, {0, 32}, {0, 32}, {0, 33}, {5, 21}, {2, 60}, {0, 30}, {11, 15}, {1, 30}, {3, 26}, {5, 22}, {2, 60}, {0, 30}, {5, 23}, {0, 13},
	{0, 13}, {0, 13}, {0, 13}, {0, 13}, {0, 13}, {0, 13}};

namespace
{
// ---- SHA-1 (for the container checks) ----
struct Sha1
{
	uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
	void Block(const uint8_t* p)
	{
		uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
		for (int i = 16; i < 80; i++)
			w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++)
		{
			uint32_t f, k;
			if (i < 20)
				f = (b & c) | (~b & d), k = 0x5A827999;
			else if (i < 40)
				f = b ^ c ^ d, k = 0x6ED9EBA1;
			else if (i < 60)
				f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
			else
				f = b ^ c ^ d, k = 0xCA62C1D6;
			const uint32_t t = rol(a, 5) + f + e + k + w[i];
			e = d, d = c, c = rol(b, 30), b = a, a = t;
		}
		h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
	}
	std::string Hex(const uint8_t* data, size_t n)
	{
		size_t i = 0;
		for (; i + 64 <= n; i += 64)
			Block(data + i);
		uint8_t tail[128] = {};
		const size_t rest = n - i;
		std::memcpy(tail, data + i, rest);
		tail[rest] = 0x80;
		const size_t blocks = rest + 9 > 64 ? 2 : 1;
		const uint64_t bits = static_cast<uint64_t>(n) * 8;
		for (int k = 0; k < 8; k++)
			tail[blocks * 64 - 1 - k] = static_cast<uint8_t>(bits >> (8 * k));
		for (size_t b = 0; b < blocks; b++)
			Block(tail + b * 64);
		char out[41];
		for (int k = 0; k < 5; k++)
			std::snprintf(out + k * 8, 9, "%08x", h[k]);
		return out;
	}
};
std::string Sha1Hex(const uint8_t* p, size_t n)
{
	Sha1 s;
	return s.Hex(p, n);
}

struct Member
{
	size_t off, size;
};
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return static_cast<uint32_t>(p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24); }
float F32(const uint8_t* p)
{
	const uint32_t u = U32(p);
	float f;
	std::memcpy(&f, &u, 4);
	return f;
}

// ROMDIR: 16-byte entries {char name[10]; u16 extinfo; u32 size}, starting at "RESET" followed by "ROMDIR".
bool RomDir(const uint8_t* d, size_t n, std::map<std::string, Member>& out, std::string& why)
{
	size_t i = 0;
	for (; i + 32 <= n; i += 16)
		if (std::memcmp(d + i, "RESET\0\0\0\0\0", 10) == 0 && std::memcmp(d + i + 16, "ROMDIR", 6) == 0)
			break;
	if (i + 32 > n)
	{
		why = "no ROMDIR found (not a PS2 BIOS image)";
		return false;
	}
	size_t off = 0;
	while (i + 16 <= n && d[i] != 0)
	{
		char name[11] = {};
		std::memcpy(name, d + i, 10);
		const uint32_t size = U32(d + i + 12);
		if (!out.count(name))
			out[name] = {off, size};
		off += (size + 15) & ~static_cast<size_t>(15);
		i += 16;
	}
	if (i + 16 > n)
	{
		why = "corrupt ROMDIR";
		return false;
	}
	return true;
}

bool Lz(const uint8_t* src, size_t n, std::vector<uint8_t>& out)
{
	if (n < 4)
		return false;
	const uint32_t size = U32(src);
	out.clear();
	out.reserve(size);
	size_t p = 4;
	while (out.size() < size)
	{
		if (p + 4 > n)
			return false;
		const uint32_t flags = (uint32_t)src[p] << 24 | (uint32_t)src[p + 1] << 16 | (uint32_t)src[p + 2] << 8 | src[p + 3];
		p += 4;
		const int k = flags & 3;
		const uint32_t mask = 0x3FFFu >> k;
		const int shift = 14 - k;
		for (int bit = 31; bit >= 2 && out.size() < size; bit--)
		{
			if (flags >> bit & 1)
			{
				if (p + 2 > n)
					return false;
				const uint32_t v = static_cast<uint32_t>(src[p]) << 8 | src[p + 1];
				p += 2;
				const size_t dist = (v & mask) + 1, cnt = (v >> shift) + 3;
				if (dist > out.size())
					return false;
				for (size_t c = 0; c < cnt; c++)
					out.push_back(out[out.size() - dist]);
			}
			else
			{
				if (p >= n)
					return false;
				out.push_back(src[p++]);
			}
		}
	}
	out.resize(size);
	return true;
}

struct Container
{
	const char* name;
	size_t size;
	const char* sha1;
};
const Container kContainers[] = {
	{"FNTIMAGE", 208408, "3a20a1ef93202c366b5fc744eb8bbfda54c897e8"},
	{"SNDIMAGE", 407060, "79baca7658d02297259b0dd15fca7de3cf2df319"},
	{"TEXIMAGE", 276708, "d5a1c08096c6e929012d017cda2d58d4082926b9"},
	{"ICOIMAGE", 71853, "aa1148ad2973448882abffa4628086b156024d8c"},
};

uint8_t A2(uint32_t a) { return static_cast<uint8_t>(std::min<uint32_t>(255, a * 2)); }

const uint32_t kFontClut[16] = {0x0, 0x19000000, 0x32000000, 0x4b000000, 0x64000000, 0x32333333, 0x4b333333, 0x64333333, 0x7d333333,
	0x4b666666, 0x64666666, 0x7d666666, 0x64999999, 0x7d999999, 0x7dcccccc, 0x7dffffff};

Image T4(const std::vector<uint8_t>& b, int w, int h)
{
	Image im;
	im.w = w, im.h = h;
	im.rgba.resize(static_cast<size_t>(w) * h * 4);
	for (size_t i = 0; i < static_cast<size_t>(w) * h / 2 && i < b.size(); i++)
		for (int nib = 0; nib < 2; nib++)
		{
			const uint32_t c = kFontClut[nib ? b[i] >> 4 : b[i] & 15];
			uint8_t* o = &im.rgba[(i * 2 + nib) * 4];
			o[0] = c & 255, o[1] = c >> 8 & 255, o[2] = c >> 16 & 255, o[3] = A2(c >> 24);
		}
	return im;
}
Image Ct16(const uint8_t* b, size_t n, int w, int h, bool force_opaque = false)
{
	Image im;
	im.w = w, im.h = h;
	im.rgba.resize(static_cast<size_t>(w) * h * 4);
	for (size_t i = 0; i < static_cast<size_t>(w) * h && i * 2 + 1 < n; i++)
	{
		const uint16_t v = U16(b + i * 2);
		uint8_t* o = &im.rgba[i * 4];
		o[0] = static_cast<uint8_t>((v & 31) << 3), o[1] = static_cast<uint8_t>((v >> 5 & 31) << 3), o[2] = static_cast<uint8_t>((v >> 10 & 31) << 3);
		o[3] = force_opaque || (v & 0x8000) ? 255 : 0;
	}
	return im;
}
Image Ct32(const uint8_t* b, size_t n, int w, int h)
{
	Image im;
	im.w = w, im.h = h;
	im.rgba.assign(b, b + std::min(n, static_cast<size_t>(w) * h * 4));
	im.rgba.resize(static_cast<size_t>(w) * h * 4);
	for (size_t i = 3; i < im.rgba.size(); i += 4)
		im.rgba[i] = A2(im.rgba[i]);
	return im;
}
Image Grey(const std::vector<uint8_t>& b, int w, int h, uint8_t alpha)
{
	Image im;
	im.w = w, im.h = h;
	im.rgba.resize(static_cast<size_t>(w) * h * 4);
	for (size_t i = 0; i < static_cast<size_t>(w) * h && i < b.size(); i++)
		im.rgba[i * 4] = im.rgba[i * 4 + 1] = im.rgba[i * 4 + 2] = b[i], im.rgba[i * 4 + 3] = alpha;
	return im;
}
Image Mask(const std::vector<uint8_t>& b, int w, int h, uint8_t c)
{
	Image im;
	im.w = w, im.h = h;
	im.rgba.resize(static_cast<size_t>(w) * h * 4);
	for (size_t i = 0; i < static_cast<size_t>(w) * h && i < b.size(); i++)
		im.rgba[i * 4] = im.rgba[i * 4 + 1] = im.rgba[i * 4 + 2] = c, im.rgba[i * 4 + 3] = A2(b[i]);
	return im;
}
Image GreyAlpha(const std::vector<uint8_t>& b, int w, int h)
{
	Image im;
	im.w = w, im.h = h;
	im.rgba.resize(static_cast<size_t>(w) * h * 4);
	for (size_t i = 0; i < static_cast<size_t>(w) * h && i * 2 + 1 < b.size(); i++)
		im.rgba[i * 4] = im.rgba[i * 4 + 1] = im.rgba[i * 4 + 2] = b[i * 2], im.rgba[i * 4 + 3] = A2(b[i * 2 + 1]);
	return im;
}
Image Kabe(const std::vector<uint8_t>& b)
{
	Image im;
	im.w = im.h = 128;
	im.rgba.resize(128 * 128 * 4);
	for (int y = 0; y < 128; y++)
		for (int x = 0; x < 128; x++)
		{
			const size_t s = static_cast<size_t>((y & 63) * 64 + (x & 63)) * 3;
			uint8_t* o = &im.rgba[static_cast<size_t>(y * 128 + x) * 4];
			if (s + 2 < b.size())
				o[0] = b[s], o[1] = b[s + 1], o[2] = b[s + 2];
			o[3] = 0xFE;
		}
	return im;
}
Image HeaderTex(const std::vector<uint8_t>& b)
{
	if (b.size() < 20)
		return {};
	const uint32_t t = U32(&b[4]) & 7;
	const int ws = U16(&b[16]), h = U16(&b[18]);
	if (t == 2)
		return Ct16(&b[20], b.size() - 20, ws, h);
	if (t == 4)
		return Ct32(&b[20], b.size() - 20, ws / 2, h);
	return {};
}
Image DecodeTex(const std::string& name, const std::vector<uint8_t>& b)
{
	if (name.rfind("TEXOPNG", 0) == 0 || name.rfind("TEXCKL", 0) == 0)
		return {}; // the disc warning plates and first-run logos: not used by the shelf
	if (name == "TEXOSCE")
		return GreyAlpha(b, 256, 64);
	if (name == "TEXOCRLE" || name == "TEXOCRBL" || name == "TEXCMARU")
		return Ct32(b.data(), b.size(), 64, 64);
	if (name == "TEXOBLP" || name == "TEXOBLPR")
		return Mask(b, 64, 64, 0);
	if (name == "TEXCKABE")
		return Kabe(b);
	if (name == "TEXCSTSL")
		return GreyAlpha(b, 64, 64);
	if (name == "TEXCFLOW" || name == "TEXCBUMP" || name == "TEXCBINV" || name == "TEXCREFA")
		return Grey(b, 64, 64, 0xFE);
	if (name == "TEXCSMOK" || name == "TEXCNAVI" || name == "TEXCBLUR")
		return Mask(b, 64, 64, 255);
	return HeaderTex(b);
}

} // namespace

void SpuEnvelope::Step(int rate, bool decrease, bool expo)
{
	const int shift = rate >> 2;
	int step = decrease ? -8 + (rate & 3) : 7 - (rate & 3);
	uint32_t cycles = 1u << std::max(0, shift - 11);
	step <<= std::max(0, 11 - shift);
	if (expo && !decrease && level > 0x6000)
		cycles *= 4;
	if (expo && decrease)
		step = static_cast<int>(static_cast<int64_t>(step) * level >> 15);
	if (++counter < cycles)
		return;
	counter = 0;
	level = std::clamp(level + step, 0, 0x7FFF);
}

float SpuEnvelope::Next()
{
	if (phase == 0)
	{
		Step(a1 >> 8 & 0x7F, false, (a1 & 0x8000) != 0);
		if (level >= 0x7FFF)
			phase = 1;
	}
	else if (phase == 1)
	{
		Step(((a1 >> 4) & 0xF) << 2, true, true);
		if (level <= static_cast<int32_t>(((a1 & 0xF) + 1) * 0x800))
			phase = 2;
	}
	else if (phase == 2)
		Step(a2 >> 6 & 0x7F, (a2 & 0x4000) != 0, (a2 & 0x8000) != 0);
	else
		Step((a2 & 0x1F) << 2, true, (a2 & 0x20) != 0);
	return level / 32767.0f;
}

std::vector<int16_t> SpuAdpcm(const std::vector<uint8_t>& body, size_t off, int* loop_start, bool* loops)
{
	if (loop_start)
		*loop_start = -1;
	if (loops)
		*loops = false;
	static const int kFilt[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
	std::vector<int16_t> pcm;
	int s1 = 0, s2 = 0;
	while (off + 16 <= body.size())
	{
		const int sh = body[off] & 15, f = std::min(body[off] >> 4, 4), fl = body[off + 1];
		if ((fl & 4) && loop_start && *loop_start < 0)
			*loop_start = static_cast<int>(pcm.size());
		for (int i = 2; i < 16; i++)
			for (int half = 0; half < 2; half++)
			{
				const int nib = half ? body[off + i] >> 4 : body[off + i] & 15;
				int v = (nib << 12) - ((nib & 8) ? 0x10000 : 0);
				v = (v >> sh) + (s1 * kFilt[f][0] + s2 * kFilt[f][1] + 32) / 64;
				v = std::clamp(v, -32768, 32767);
				pcm.push_back(static_cast<int16_t>(v));
				s2 = s1, s1 = v;
			}
		off += 16;
		if (fl & 1)
		{
			if (loops)
				*loops = (fl & 2) != 0;
			break;
		}
	}
	return pcm;
}

bool ParseIco(const uint8_t* d, size_t n, Ico& out)
{
	out = Ico();
	if (n < 20 || U32(d) != 0x10000)
		return false;
	const uint32_t ns = U32(d + 4), tt = U32(d + 8), nv = U32(d + 16);
	if (ns < 1 || ns > 16 || nv > 20000)
		return false;
	size_t p = 20;
	const size_t per = 8 * ns + 8 + 8;
	if (p + per * nv > n)
		return false;
	out.shapes = static_cast<int>(ns);
	out.tex_type = tt;
	out.verts.resize(nv);
	for (uint32_t i = 0; i < nv; i++)
	{
		Ico::Vertex& v = out.verts[i];
		v.pos.resize(ns * 3);
		for (uint32_t s = 0; s < ns; s++)
		{
			v.pos[s * 3 + 0] = static_cast<int16_t>(U16(d + p)) / 1024.0f;
			v.pos[s * 3 + 1] = static_cast<int16_t>(U16(d + p + 2)) / 1024.0f;
			v.pos[s * 3 + 2] = static_cast<int16_t>(U16(d + p + 4)) / 1024.0f;
			p += 8;
		}
		v.nx = static_cast<int16_t>(U16(d + p)) / 4096.0f;
		v.ny = static_cast<int16_t>(U16(d + p + 2)) / 4096.0f;
		v.nz = static_cast<int16_t>(U16(d + p + 4)) / 4096.0f;
		p += 8;
		v.u = static_cast<int16_t>(U16(d + p)) / 4096.0f;
		v.v = static_cast<int16_t>(U16(d + p + 2)) / 4096.0f;
		v.r = d[p + 4], v.g = d[p + 5], v.b = d[p + 6], v.a = d[p + 7];
		p += 8;
	}
	// Animation header and frames.
	if (p + 20 <= n)
	{
		out.frame_length = std::max<uint32_t>(1, U32(d + p + 4));
		out.anim_speed = F32(d + p + 8);
		out.anim_offset = static_cast<float>(U32(d + p + 12));
		const uint32_t nf = U32(d + p + 16);
		p += 20;
		out.frames.assign(ns, {});
		for (uint32_t f = 0; f < nf && f < 64; f++)
		{
			if (p + 8 > n)
				return false;
			const uint32_t shape = U32(d + p), nk = U32(d + p + 4);
			p += 8;
			if (p + 8ull * nk > n)
				return false;
			std::vector<Ico::Key> keys;
			for (uint32_t k = 0; k < nk; k++)
				keys.push_back({F32(d + p + 8 * k), F32(d + p + 8 * k + 4)});
			p += 8ull * nk;
			if (shape < ns)
				out.frames[shape] = std::move(keys);
		}
	}
	if (tt & 4)
	{
		std::vector<uint16_t> tex;
		if (tt & 8)
		{
			if (p + 4 > n)
				return true;
			const uint32_t size = U32(d + p);
			p += 4;
			const size_t end = std::min(n, p + size);
			while (p + 2 <= end && tex.size() < 16384)
			{
				const uint16_t c = U16(d + p);
				p += 2;
				if (c & 0x8000)
				{
					const uint32_t cnt = 0x10000u - c;
					for (uint32_t k = 0; k < cnt && p + 2 <= end; k++, p += 2)
						tex.push_back(U16(d + p));
				}
				else
				{
					if (p + 2 > end)
						break;
					const uint16_t v = U16(d + p);
					p += 2;
					tex.insert(tex.end(), c, v);
				}
			}
		}
		else if (n >= 32768)
		{
			for (size_t i = 0; i < 16384; i++)
				tex.push_back(U16(d + n - 32768 + i * 2));
		}
		tex.resize(16384, 0);
		out.tex = Ct16(reinterpret_cast<const uint8_t*>(tex.data()), tex.size() * 2, 128, 128, true);
	}
	return true;
}

bool Bios::LoadDir(const std::string& dir)
{
	m_ok = false;
	m_problem = "no PS2 BIOS in " + dir;
	DIR* d = opendir(dir.c_str());
	if (!d)
		return false;
	std::vector<std::string> files;
	while (dirent* e = readdir(d))
	{
		const std::string p = dir + "/" + e->d_name;
		struct stat st;
		if (e->d_name[0] != '.' && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 4 * 1024 * 1024)
			files.push_back(p);
	}
	closedir(d);
	std::sort(files.begin(), files.end());
	std::string last;
	for (const std::string& f : files)
	{
		if (LoadFile(f))
			return true;
		last = m_problem;
	}
	if (!last.empty())
		m_problem = last;
	return false;
}

bool Bios::LoadFile(const std::string& path)
{
	m_ok = false;
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
	{
		m_problem = path + ": cannot be read";
		return false;
	}
	std::vector<uint8_t> rom(4 * 1024 * 1024 + 1);
	const size_t n = std::fread(rom.data(), 1, rom.size(), f);
	std::fclose(f);
	rom.resize(n);
	m_path = path;
	if (!Load(rom))
	{
		m_problem = path + ": " + m_problem + ". Using generic artwork.";
		return false;
	}
	m_problem.clear();
	return m_ok = true;
}

bool Bios::Load(const std::vector<uint8_t>& rom)
{
	m_raw.clear(), m_tex.clear(), m_font.clear(), m_ico.clear(), m_sfx.clear();
	if (rom.size() != 4 * 1024 * 1024)
	{
		m_problem = "size " + std::to_string(rom.size()) + " is not a 4 MiB PS2 BIOS";
		return false;
	}
	std::map<std::string, Member> rd;
	if (!RomDir(rom.data(), rom.size(), rd, m_problem))
		return false;
	for (const char* need : {"ROMVER", "OSDVER", "EXTINFO"})
		if (!rd.count(need) || rd[need].off + rd[need].size > rom.size())
		{
			m_problem = std::string("missing ") + need + " (not a PS2 BIOS image)";
			return false;
		}
	if (std::memcmp(&rom[rd["ROMVER"].off], "0200EC20040614", 14) != 0)
	{
		m_problem = "ROMVER is '" + std::string(reinterpret_cast<const char*>(&rom[rd["ROMVER"].off]), 14) +
		            "'; only 0200EC20040614 (v2.00 Europe, SCPH-70004 family) is supported";
		return false;
	}
	if (std::memcmp(&rom[rd["OSDVER"].off], "0200????", 8) != 0)
	{
		m_problem = "OSDVER mismatch";
		return false;
	}
	{
		const std::string ext(reinterpret_cast<const char*>(&rom[rd["EXTINFO"].off]), rd["EXTINFO"].size);
		if (ext.find("20040614-100914,ROMconf,PS20200EC20040614.bin") == std::string::npos)
		{
			m_problem = "EXTINFO mismatch";
			return false;
		}
	}
	for (const Container& c : kContainers)
	{
		auto it = rd.find(c.name);
		if (it == rd.end() || it->second.size != c.size || it->second.off + c.size > rom.size())
		{
			m_problem = std::string(c.name) + " missing or wrong size";
			return false;
		}
		const uint8_t* cd = &rom[it->second.off];
		if (Sha1Hex(cd, c.size) != c.sha1)
		{
			m_problem = std::string(c.name) + " does not match the v2.00 EUR data (modified or different dump)";
			return false;
		}
		std::map<std::string, Member> members;
		std::string why;
		if (!RomDir(cd, c.size, members, why))
		{
			m_problem = std::string(c.name) + ": " + why;
			return false;
		}
		for (auto& [name, m] : members)
		{
			if (name == "RESET" || name == "ROMDIR" || name == "EXTINFO" || name == "-" || m.off + m.size > c.size)
				continue;
			std::vector<uint8_t> data;
			if (!Lz(cd + m.off, m.size, data))
			{
				m_problem = "member " + std::string(c.name) + "/" + name + " failed to unpack";
				return false;
			}
			m_raw[name] = std::move(data);
		}
	}
	// Decode what the shelf draws.
	for (auto& [name, data] : m_raw)
	{
		if (name.rfind("TEX", 0) == 0)
		{
			Image im = DecodeTex(name, data);
			if (im.ok())
				m_tex[name] = std::move(im);
		}
		else if (name == "FNTASCII")
			m_font[name] = T4(data, 256, static_cast<int>(data.size() * 2 / 256));
		else if (name == "FNTEXOSD")
			m_font[name] = T4(data, 512, static_cast<int>(data.size() * 2 / 512));
		else if (name.rfind("ICOB", 0) == 0)
		{
			Ico ico;
			if (ParseIco(data.data(), data.size(), ico))
				m_ico[name] = std::move(ico);
		}
	}
	if (!m_font.count("FNTASCII") || !m_tex.count("TEXBBTTN") || !m_ico.count("ICOBPS2M"))
	{
		m_problem = "menu members missing";
		return false;
	}
	// The sound effects (spec/sound.md 2): 64-byte records from the table at header +0x2C, samples SPU2 ADPCM in the body.
	auto h = m_raw.find("SNDOSDDH"), b = m_raw.find("SNDOSDDB");
	if (h != m_raw.end() && b != m_raw.end() && h->second.size() >= 0x30)
	{
		const std::vector<uint8_t>& head = h->second;
		const uint32_t t = U32(&head[0x2C]);
		if (t + 8 <= head.size())
		{
			const uint32_t last = std::min<uint32_t>(U32(&head[t + 4]), 31);
			for (uint32_t i = 0; i <= last && t + 0x20 + 64 * i + 20 <= head.size(); i++)
			{
				const uint8_t* r = &head[t + 0x20 + 64 * i];
				// VOLL = volume x table volume (28 after 0x5201) >> 7, of the SPU2's 0x3FFF (sound.md 2).
				const float vl = (U16(r) * 28 >> 7) / 16383.0f, vr = (U16(r + 2) * 28 >> 7) / 16383.0f;
				const double rate = U16(r + 4) * 44100.0 / 4096.0;
				const uint16_t adsr1 = U16(r + 8), adsr2 = U16(r + 10);
				const std::vector<int16_t> pcm = SpuAdpcm(b->second, U32(r + 16));
				Effect e;
				SpuEnvelope env(adsr1, adsr2);
				const double step = rate / 48000.0;
				const size_t max_out = 48000 * 3;
				for (size_t k = 0; k < max_out; k++)
				{
					const double pos = k * step;
					const size_t i0 = static_cast<size_t>(pos);
					if (i0 + 1 >= pcm.size())
						break;
					const float fr = static_cast<float>(pos - i0);
					const float s = (pcm[i0] * (1.0f - fr) + pcm[i0 + 1] * fr) / 32768.0f * env.Next();
					if (env.phase == 2 && env.level < 16)
						break;
					e.stereo.push_back(s * vl);
					e.stereo.push_back(s * vr);
				}
				m_sfx.push_back(std::move(e));
			}
		}
	}
	return true;
}

const Image* Bios::Tex(const char* name) const
{
	auto it = m_tex.find(name);
	return it == m_tex.end() ? nullptr : &it->second;
}
const Image* Bios::Font(const char* name) const
{
	auto it = m_font.find(name);
	return it == m_font.end() ? nullptr : &it->second;
}
const Ico* Bios::Icon(const char* name) const
{
	auto it = m_ico.find(name);
	return it == m_ico.end() ? nullptr : &it->second;
}
const Effect* Bios::Sound(int n) const { return n >= 0 && n < static_cast<int>(m_sfx.size()) ? &m_sfx[static_cast<size_t>(n)] : nullptr; }
const std::vector<uint8_t>* Bios::Raw(const char* name) const
{
	auto it = m_raw.find(name);
	return it == m_raw.end() ? nullptr : &it->second;
}
} // namespace s3
