// PS2 shelf v3 (AI-assisted): PCSX2 memory cards (see s3_memcard.h).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "s3_memcard.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>

namespace s3
{
namespace
{
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return static_cast<uint32_t>(p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24); }
float F32(const uint8_t* p)
{
	const uint32_t u = U32(p);
	float f;
	std::memcpy(&f, &u, 4);
	return f;
}
void PutU16(uint8_t* p, uint16_t v) { p[0] = v & 255, p[1] = v >> 8; }
void PutU32(uint8_t* p, uint32_t v) { p[0] = v & 255, p[1] = v >> 8 & 255, p[2] = v >> 16 & 255, p[3] = v >> 24; }

bool ReadAll(const std::string& path, std::vector<uint8_t>& out, size_t max = 64u << 20)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long n = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (n < 0 || static_cast<size_t>(n) > max)
	{
		std::fclose(f);
		return false;
	}
	out.resize(static_cast<size_t>(n));
	const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
	std::fclose(f);
	return ok;
}

bool ParseIconSys(const std::vector<uint8_t>& d, IconSys& s)
{
	s = IconSys();
	if (d.size() < 964 || std::memcmp(d.data(), "PS2D", 4) != 0)
		return false;
	const uint16_t brk = U16(&d[6]);
	s.bg_alpha = U32(&d[0x0C]);
	for (int c = 0; c < 4; c++)
		for (int k = 0; k < 3; k++)
			s.bg[c][k] = static_cast<float>(U32(&d[0x10 + c * 16 + k * 4]));
	for (int l = 0; l < 3; l++)
		for (int k = 0; k < 3; k++)
		{
			s.light_dir[l][k] = F32(&d[0x50 + l * 16 + k * 4]);
			s.light_col[l][k] = F32(&d[0x80 + l * 16 + k * 4]);
		}
	for (int k = 0; k < 3; k++)
		s.ambient[k] = F32(&d[0xB0 + k * 4]);
	size_t tl = 0;
	while (tl < 68 && d[0xC0 + tl])
		tl++;
	const size_t b = std::min<size_t>(brk, tl);
	s.title1 = SjisToAscii(&d[0xC0], b);
	s.title2 = SjisToAscii(&d[0xC0 + b], tl - b);
	auto trim = [](std::string& t) {
		while (!t.empty() && t.back() == ' ')
			t.pop_back();
		while (!t.empty() && t.front() == ' ')
			t.erase(t.begin());
	};
	trim(s.title1), trim(s.title2);
	if (s.title1.empty() && !s.title2.empty())
		std::swap(s.title1, s.title2);
	const char* fn = reinterpret_cast<const char*>(&d[0x104]);
	s.icon_file.assign(fn, strnlen(fn, 64));
	s.ok = true;
	return true;
}

void Tod(const uint8_t* t, Save& s)
{
	s.second = t[1], s.minute = t[2], s.hour = t[3], s.day = t[4], s.month = t[5], s.year = U16(t + 6);
}

// Folder cards (PCSX2's FolderMemoryCard): a card name the host can't hold is stored "cleaned" (PCSX2's CleanMemcardFilename: \ % : | " < >
// and trailing dots/spaces become '_'), the real name then kept in _pcsx2_meta/<cleaned> (a 512-byte card entry); PCSX2's own files start "_pcsx2_".
std::string CleanFolderName(const std::string& name)
{
	std::string c = name;
	for (char& ch : c)
		if (std::strchr("\\%:|\"<>", ch))
			ch = '_';
	for (size_t k = c.size(); k > 0 && (c[k - 1] == ' ' || c[k - 1] == '.'); k--)
		c[k - 1] = '_';
	return c;
}

bool IsPcsx2File(const char* name) { return std::strncmp(name, "_pcsx2_", 7) == 0; }

// A save folder's "modified" time as PCSX2 keeps it: _pcsx2_meta_directory (the card entry), else _pcsx2_index's $ROOT timeModified (the
// stored fields as UTC seconds, MemoryCardFileEntryDateTime::ToTime). False when neither is there.
bool FolderSaveTime(const std::string& dir, Save& s)
{
	std::vector<uint8_t> meta;
	if (ReadAll(dir + "/_pcsx2_meta_directory", meta, 4096) && meta.size() >= 0x60 && U16(&meta[0]) != 0)
	{
		Tod(&meta[0x18], s);
		return s.year > 0;
	}
	std::vector<uint8_t> idx;
	if (!ReadAll(dir + "/_pcsx2_index", idx, 1u << 20))
		return false;
	const std::string y(idx.begin(), idx.end());
	size_t at = y.find("$ROOT");
	if (at == std::string::npos)
		at = y.find("%ROOT");
	if (at == std::string::npos)
		return false;
	size_t end = at;
	while ((end = y.find('\n', end + 1)) != std::string::npos && end + 1 < y.size() && (y[end + 1] == ' ' || y[end + 1] == '\t'))
		;
	const size_t tm = y.find("timeModified", at);
	if (tm == std::string::npos || (end != std::string::npos && tm > end))
		return false;
	const size_t colon = y.find(':', tm);
	if (colon == std::string::npos)
		return false;
	const time_t t = static_cast<time_t>(std::strtoll(y.c_str() + colon + 1, nullptr, 10));
	struct tm tmv;
	if (t <= 0 || !gmtime_r(&t, &tmv))
		return false;
	s.year = tmv.tm_year + 1900, s.month = tmv.tm_mon + 1, s.day = tmv.tm_mday, s.hour = tmv.tm_hour, s.minute = tmv.tm_min, s.second = tmv.tm_sec;
	return true;
}

bool Matches(const std::string& dir, const std::string& serial)
{
	// "SLES-52541" -> compare letters and digits only: "SLES52541" in "BESLES-52541GTASA".
	std::string want, have;
	for (char c : serial)
		if (std::isalnum(static_cast<unsigned char>(c)))
			want += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	for (char c : dir)
		if (std::isalnum(static_cast<unsigned char>(c)))
			have += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return want.size() >= 6 && have.find(want) != std::string::npos;
}
} // namespace

std::string SjisToAscii(const uint8_t* s, size_t n)
{
	static const struct
	{
		uint16_t code;
		char c;
	} kMap[] = {{0x8140, ' '}, {0x8141, ','}, {0x8142, '.'}, {0x8143, ','}, {0x8144, '.'}, {0x8145, '.'}, {0x8146, ':'}, {0x8147, ';'},
		{0x8148, '?'}, {0x8149, '!'}, {0x814D, '`'}, {0x814F, '^'}, {0x8151, '_'}, {0x815B, '-'}, {0x815C, '-'}, {0x815D, '-'},
		{0x815E, '/'}, {0x815F, '\\'}, {0x8160, '~'}, {0x8162, '|'}, {0x8165, '\''}, {0x8166, '\''}, {0x8167, '"'}, {0x8168, '"'},
		{0x8169, '('}, {0x816A, ')'}, {0x816D, '['}, {0x816E, ']'}, {0x816F, '{'}, {0x8170, '}'}, {0x817B, '+'}, {0x817C, '-'},
		{0x8181, '='}, {0x8183, '<'}, {0x8184, '>'}, {0x8190, '$'}, {0x8193, '%'}, {0x8194, '#'}, {0x8195, '&'}, {0x8196, '*'},
		{0x8197, '@'}};
	std::string out;
	for (size_t i = 0; i < n; i++)
	{
		const uint8_t c = s[i];
		if (c < 0x80)
		{
			out += static_cast<char>(c);
			continue;
		}
		if (((c >= 0x81 && c <= 0x9F) || c >= 0xE0) && i + 1 < n)
		{
			const uint16_t code = static_cast<uint16_t>(c << 8 | s[i + 1]);
			i++;
			char m = '?';
			if (code >= 0x824F && code <= 0x8258)
				m = static_cast<char>('0' + code - 0x824F);
			else if (code >= 0x8260 && code <= 0x8279)
				m = static_cast<char>('A' + code - 0x8260);
			else if (code >= 0x8281 && code <= 0x829A)
				m = static_cast<char>('a' + code - 0x8281);
			else
				for (const auto& e : kMap)
					if (e.code == code)
						m = e.c;
			out += m;
			continue;
		}
		out += '?';
	}
	return out;
}

void Ecc128(const uint8_t* chunk, uint8_t out[3])
{
	static uint8_t parity[256], colmask[256];
	static bool init = false;
	if (!init)
	{
		for (int b = 0; b < 256; b++)
			parity[b] = static_cast<uint8_t>(__builtin_popcount(static_cast<unsigned>(b)) & 1);
		const uint8_t cp[7] = {0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0};
		for (int b = 0; b < 256; b++)
		{
			uint8_t m = 0;
			for (int i = 0; i < 7; i++)
				m = static_cast<uint8_t>(m | parity[b & cp[i]] << i);
			colmask[b] = m;
		}
		init = true;
	}
	uint8_t col = 0x77, lp0 = 0x7F, lp1 = 0x7F;
	for (int i = 0; i < 128; i++)
	{
		col ^= colmask[chunk[i]];
		if (parity[chunk[i]])
		{
			lp0 = static_cast<uint8_t>(lp0 ^ ~i);
			lp1 = static_cast<uint8_t>(lp1 ^ i);
		}
	}
	out[0] = col, out[1] = lp0 & 0x7F, out[2] = lp1;
}

bool MemoryCard::Open(const std::string& path)
{
	m_ok = m_folder = m_formatted = false;
	m_path = path;
	m_saves.clear();
	m_root_entries.clear();
	m_img.clear();
	m_free_kb = m_used_kb = 0;
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
	{
		m_problem = "no memory card at " + path;
		return false;
	}
	if (S_ISDIR(st.st_mode))
		return m_ok = OpenFolder();
	return m_ok = OpenFile();
}

bool MemoryCard::OpenFolder()
{
	m_folder = m_formatted = true;
	DIR* d = opendir(m_path.c_str());
	if (!d)
		return false;
	std::vector<std::string> names;
	while (dirent* e = readdir(d))
		if (e->d_name[0] != '.' && std::strncmp(e->d_name, "_pcsx2", 6) != 0)
			names.push_back(e->d_name);
	closedir(d);
	std::sort(names.begin(), names.end());
	uint64_t used = 0;
	for (const std::string& n : names)
	{
		const std::string dir = m_path + "/" + n;
		struct stat st;
		if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
			continue;
		Save s;
		s.dir = n;
		if (!FolderSaveTime(dir, s))
		{
			struct tm tmv;
			const time_t mt = st.st_mtime;
			if (localtime_r(&mt, &tmv))
				s.year = tmv.tm_year + 1900, s.month = tmv.tm_mon + 1, s.day = tmv.tm_mday, s.hour = tmv.tm_hour, s.minute = tmv.tm_min,
				s.second = tmv.tm_sec;
		}
		if (DIR* sd = opendir(dir.c_str()))
		{
			while (dirent* e = readdir(sd))
			{
				struct stat fs;
				if (e->d_name[0] != '.' && !IsPcsx2File(e->d_name) && stat((dir + "/" + e->d_name).c_str(), &fs) == 0 && S_ISREG(fs.st_mode))
					s.bytes += static_cast<uint32_t>(fs.st_size);
			}
			closedir(sd);
		}
		used += (s.bytes + 1023) / 1024;
		std::vector<uint8_t> sys;
		if (ReadAll(dir + "/icon.sys", sys, 4096) && ParseIconSys(sys, s.sys))
		{
			if (n.size() == 13 && n[0] == 'B' && n.compare(2, 11, "DATA-SYSTEM") == 0 && s.sys.icon_file.rfind("_SCE", 0) == 0)
				s.builtin = true, s.builtin_index = std::atoi(s.sys.icon_file.c_str() + 4);
			else if (!ReadAll(dir + "/" + CleanFolderName(s.sys.icon_file), s.ico, 1u << 20))
				s.corrupted = true;
		}
		else
			s.corrupted = true;
		m_saves.push_back(std::move(s));
	}
	// A folder card has no fixed size (PCSX2 shows each game only its own saves): the space used, not the space free.
	m_used_kb = static_cast<uint32_t>(std::min<uint64_t>(used, 0xFFFFFFFFu));
	m_free_kb = 0x7FFFFFFF;
	return true;
}

bool MemoryCard::ReadPage(uint32_t page, uint8_t* out) const
{
	const size_t off = PageOffset(page);
	if (off + m_page > m_img.size())
		return false;
	std::memcpy(out, &m_img[off], m_page);
	return true;
}

bool MemoryCard::ReadCluster(uint32_t abs, std::vector<uint8_t>& out) const
{
	out.resize(static_cast<size_t>(m_page) * m_ppc);
	for (uint32_t p = 0; p < m_ppc; p++)
		if (!ReadPage(abs * m_ppc + p, &out[p * m_page]))
			return false;
	return true;
}

bool MemoryCard::Fat(uint32_t rel, uint32_t& value, size_t* raw_offset) const
{
	const uint32_t per = m_page * m_ppc / 4; // entries per cluster (256)
	const uint32_t fat_index = rel / per, ifc_index = fat_index / per;
	if (ifc_index >= 32)
		return false;
	std::vector<uint8_t> c;
	if (!ReadCluster(m_ifc[ifc_index], c))
		return false;
	const uint32_t fat_cluster = U32(&c[(fat_index % per) * 4]);
	const uint32_t byte = (rel % per) * 4;
	const uint32_t page = fat_cluster * m_ppc + byte / m_page;
	const size_t off = PageOffset(page) + byte % m_page;
	if (off + 4 > m_img.size())
		return false;
	value = U32(&m_img[off]);
	if (raw_offset)
		*raw_offset = off;
	return true;
}

bool MemoryCard::Chain(uint32_t first, std::vector<uint32_t>& out) const
{
	out.clear();
	uint32_t c = first;
	while (out.size() < m_clusters)
	{
		out.push_back(c);
		uint32_t v;
		if (!Fat(c, v))
			return false;
		if (v == 0xFFFFFFFF || !(v & 0x80000000))
			break;
		c = v & 0x7FFFFFFF;
	}
	return true;
}

bool MemoryCard::ReadDir(uint32_t first, uint32_t count, std::vector<Entry>& out) const
{
	out.clear();
	std::vector<uint32_t> chain;
	if (!Chain(first, chain))
		return false;
	const uint32_t per_cluster = m_page * m_ppc / 512;
	for (uint32_t i = 0; i < count; i++)
	{
		const uint32_t ci = i / per_cluster;
		if (ci >= chain.size())
			break;
		const uint32_t abs = chain[ci] + m_alloc_off;
		const uint32_t byte = (i % per_cluster) * 512;
		const size_t off = PageOffset(abs * m_ppc + byte / m_page) + byte % m_page;
		if (off + 512 > m_img.size())
			return false;
		const uint8_t* e = &m_img[off];
		Entry en;
		en.mode = U16(e);
		en.length = U32(e + 4);
		en.cluster = U32(e + 0x10);
		std::memcpy(en.mtime, e + 0x18, 8);
		en.name.assign(reinterpret_cast<const char*>(e + 0x40), strnlen(reinterpret_cast<const char*>(e + 0x40), 32));
		en.raw_offset = off;
		out.push_back(en);
	}
	return true;
}

bool MemoryCard::ReadFile(const Entry& e, std::vector<uint8_t>& out) const
{
	out.clear();
	std::vector<uint32_t> chain;
	if (e.length > (1u << 22) || !Chain(e.cluster, chain))
		return false;
	std::vector<uint8_t> c;
	for (uint32_t cl : chain)
	{
		if (out.size() >= e.length)
			break;
		if (!ReadCluster(cl + m_alloc_off, c))
			return false;
		out.insert(out.end(), c.begin(), c.end());
	}
	if (out.size() < e.length)
		return false;
	out.resize(e.length);
	return true;
}

bool MemoryCard::OpenFile()
{
	if (!ReadAll(m_path, m_img, 16u << 20) || m_img.size() < 0x200)
	{
		m_problem = "cannot read " + m_path;
		return false;
	}
	if (std::memcmp(m_img.data(), "Sony PS2 Memory Card Format ", 28) != 0)
	{
		m_problem = "unformatted";
		return true; // a card, not formatted
	}
	const uint8_t* sb = m_img.data();
	m_page = U16(sb + 0x28);
	m_ppc = U16(sb + 0x2A);
	m_clusters = U32(sb + 0x30);
	m_alloc_off = U32(sb + 0x34);
	m_alloc_end = U32(sb + 0x38);
	m_root = U32(sb + 0x3C);
	for (int i = 0; i < 32; i++)
		m_ifc[i] = U32(sb + 0x50 + i * 4);
	if (m_page != 512 || m_ppc < 1 || m_ppc > 16 || m_clusters == 0)
	{
		m_problem = "unknown card layout";
		return false;
	}
	const size_t pages = static_cast<size_t>(m_clusters) * m_ppc;
	m_raw_page = m_img.size() >= pages * (m_page + 16) ? m_page + 16 : m_page;
	m_formatted = true;
	// The root directory: its "." entry says how many entries it holds.
	std::vector<Entry> first;
	if (!ReadDir(m_root, 1, first) || first.empty())
	{
		m_problem = "unreadable root directory";
		return false;
	}
	if (!ReadDir(m_root, first[0].length, m_root_entries))
		return false;
	for (const Entry& e : m_root_entries)
	{
		if (!(e.mode & 0x8000) || !(e.mode & 0x0020) || e.name == "." || e.name == "..")
			continue;
		Save s;
		s.dir = e.name;
		Tod(e.mtime, s);
		std::vector<Entry> files;
		if (!ReadDir(e.cluster, e.length, files))
			continue;
		const Entry* sys = nullptr;
		s.bytes = (static_cast<uint32_t>(files.size()) * 512 + 1023) / 1024 * 1024; // the BIOS's size: the folder's entries ...
		for (const Entry& f : files)
			if ((f.mode & 0x8000) && (f.mode & 0x0010))
			{
				s.bytes += (f.length + 1023) / 1024 * 1024; // ... and each file in whole clusters
				if (f.name == "icon.sys")
					sys = &f;
			}
		std::vector<uint8_t> data;
		if (sys && ReadFile(*sys, data) && ParseIconSys(data, s.sys))
		{
			// A built-in icon ("_SCEn") only for a "B?DATA-SYSTEM" folder (browser.md 10); elsewhere the file must exist (else: corrupted).
			const bool system_dir = e.name.size() == 13 && e.name[0] == 'B' && e.name.compare(2, 11, "DATA-SYSTEM") == 0;
			if (system_dir && s.sys.icon_file.rfind("_SCE", 0) == 0)
				s.builtin = true, s.builtin_index = std::atoi(s.sys.icon_file.c_str() + 4);
			else
			{
				s.corrupted = true;
				for (const Entry& f : files)
					if ((f.mode & 0x8000) && f.name == s.sys.icon_file && ReadFile(f, s.ico))
						s.corrupted = false;
			}
		}
		else
			s.corrupted = true;
		m_saves.push_back(std::move(s));
	}
	// The BIOS lists a card's saves newest first (checked in PCSX2).
	std::stable_sort(m_saves.begin(), m_saves.end(), [](const Save& a, const Save& b) {
		return std::tie(a.year, a.month, a.day, a.hour, a.minute, a.second) > std::tie(b.year, b.month, b.day, b.hour, b.minute, b.second);
	});
	// Free space: free FAT entries in the allocatable area.
	uint32_t free_clusters = 0;
	for (uint32_t c = 0; c + m_alloc_off < m_alloc_end && c < m_clusters; c++)
	{
		uint32_t v;
		if (Fat(c, v) && !(v & 0x80000000))
			free_clusters++;
	}
	m_free_kb = free_clusters * m_page * m_ppc / 1024;
	// The BIOS reports 95 KB less than the free clusters on a standard 8 MB card (checked in PCSX2: 7,998 formatted, 7,914 with the two test saves).
	if (m_clusters == 8192)
		m_free_kb = m_free_kb > 95 ? m_free_kb - 95 : 0;
	return true;
}

bool MemoryCard::WriteBytes(size_t off, const uint8_t* data, size_t n)
{
	if (off + n > m_img.size())
		return false;
	std::memcpy(&m_img[off], data, n);
	const size_t page = off / m_raw_page;
	if (std::find(m_dirty_pages.begin(), m_dirty_pages.end(), page) == m_dirty_pages.end())
		m_dirty_pages.push_back(page);
	return true;
}

bool MemoryCard::Delete(const std::string& dir)
{
	if (!m_ok || !m_formatted)
		return false;
	if (m_folder)
	{
		const std::string p = m_path + "/" + dir;
		DIR* d = opendir(p.c_str());
		if (!d)
			return false;
		bool ok = true;
		auto is_dot = [](const char* n) { return n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])); };
		while (dirent* e = readdir(d))
			if (!is_dot(e->d_name))
			{
				const std::string f = p + "/" + e->d_name;
				struct stat st;
				if (stat(f.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) // PCSX2's _pcsx2_meta/
				{
					if (DIR* md = opendir(f.c_str()))
					{
						while (dirent* m = readdir(md))
							if (!is_dot(m->d_name))
								ok = (unlink((f + "/" + m->d_name).c_str()) == 0) && ok;
						closedir(md);
					}
					ok = rmdir(f.c_str()) == 0 && ok;
				}
				else
					ok = (unlink(f.c_str()) == 0) && ok;
			}
		closedir(d);
		ok = rmdir(p.c_str()) == 0 && ok;
		Open(m_path);
		return ok;
	}
	const Entry* root = nullptr;
	for (const Entry& e : m_root_entries)
		if ((e.mode & 0x8000) && (e.mode & 0x0020) && e.name == dir)
			root = &e;
	if (!root)
		return false;
	// Which pages' ECC can be kept right: only when the card's own ECC matches ours (else the spare area is left alone).
	auto ecc_ok = [&](size_t page) {
		if (m_raw_page == m_page)
			return false;
		const uint8_t* p = &m_img[page * m_raw_page];
		for (int c = 0; c < 4; c++)
		{
			uint8_t e[3];
			Ecc128(p + c * 128, e);
			if (std::memcmp(e, p + m_page + c * 3, 3) != 0)
				return false;
		}
		return true;
	};
	std::vector<uint8_t> before = m_img;
	m_dirty_pages.clear();
	std::vector<Entry> files;
	if (!ReadDir(root->cluster, root->length, files))
		return false;
	auto free_chain = [&](uint32_t first) {
		std::vector<uint32_t> chain;
		if (!Chain(first, chain))
			return false;
		for (uint32_t c : chain)
		{
			size_t off;
			uint32_t v;
			if (!Fat(c, v, &off))
				return false;
			uint8_t fv[4];
			PutU32(fv, 0x7FFFFFFF);
			WriteBytes(off, fv, 4);
		}
		return true;
	};
	for (const Entry& f : files)
	{
		if (!(f.mode & 0x8000) || f.name == "." || f.name == "..")
			continue;
		if ((f.mode & 0x0010) && f.length > 0 && !free_chain(f.cluster))
			return false;
		uint8_t m[2];
		PutU16(m, static_cast<uint16_t>(f.mode & ~0x8000));
		WriteBytes(f.raw_offset, m, 2);
	}
	if (!free_chain(root->cluster))
		return false;
	uint8_t m[2];
	PutU16(m, static_cast<uint16_t>(root->mode & ~0x8000));
	WriteBytes(root->raw_offset, m, 2);
	// ECC for the pages written (where the card keeps a matching one).
	for (size_t page : m_dirty_pages)
	{
		std::vector<uint8_t> now(m_img.begin() + static_cast<long>(page * m_raw_page), m_img.begin() + static_cast<long>(page * m_raw_page + m_raw_page));
		std::memcpy(&m_img[page * m_raw_page], &before[page * m_raw_page], m_raw_page);
		const bool keep = ecc_ok(page);
		std::memcpy(&m_img[page * m_raw_page], now.data(), m_raw_page);
		if (keep)
			for (int c = 0; c < 4; c++)
				Ecc128(&m_img[page * m_raw_page + c * 128], &m_img[page * m_raw_page + m_page + c * 3]);
	}
	// A backup of the card as it was, then the card.
	{
		const std::string bak = m_path + ".shelf-backup";
		if (FILE* f = std::fopen(bak.c_str(), "wb"))
		{
			std::fwrite(before.data(), 1, before.size(), f);
			std::fclose(f);
		}
		const std::string tmp = m_path + ".shelf-tmp";
		FILE* f = std::fopen(tmp.c_str(), "wb");
		if (!f)
			return false;
		const bool ok = std::fwrite(m_img.data(), 1, m_img.size(), f) == m_img.size();
		std::fclose(f);
		if (!ok || std::rename(tmp.c_str(), m_path.c_str()) != 0)
		{
			unlink(tmp.c_str());
			return false;
		}
	}
	Open(m_path);
	return true;
}

bool MemoryCard::FatSet(uint32_t rel, uint32_t value)
{
	uint32_t old;
	size_t off;
	if (!Fat(rel, old, &off))
		return false;
	uint8_t b[4];
	PutU32(b, value);
	return WriteBytes(off, b, 4);
}

bool MemoryCard::AllocCluster(uint32_t& rel)
{
	for (uint32_t c = 0; c + m_alloc_off < m_alloc_end && c < m_clusters; c++)
	{
		uint32_t v;
		if (Fat(c, v) && !(v & 0x80000000))
		{
			rel = c;
			return FatSet(c, 0xFFFFFFFF);
		}
	}
	return false;
}

bool MemoryCard::WriteCluster(uint32_t rel, const uint8_t* data)
{
	const uint32_t abs = rel + m_alloc_off;
	for (uint32_t p = 0; p < m_ppc; p++)
		if (!WriteBytes(PageOffset(abs * m_ppc + p), data + p * m_page, m_page))
			return false;
	return true;
}

size_t MemoryCard::EntryOffset(const std::vector<uint32_t>& chain, uint32_t index) const
{
	const uint32_t per = m_page * m_ppc / 512;
	if (index / per >= chain.size())
		return 0;
	const uint32_t abs = chain[index / per] + m_alloc_off;
	const uint32_t byte = (index % per) * 512;
	return PageOffset(abs * m_ppc + byte / m_page) + byte % m_page;
}

void MemoryCard::FixEcc(const std::vector<uint8_t>& before)
{
	if (m_raw_page == m_page)
		return;
	for (size_t page : m_dirty_pages)
	{
		// Only where the card's own ECC matched the old data (else the spare area is left as it was).
		bool matched = true;
		for (int c = 0; c < 4 && matched; c++)
		{
			uint8_t e[3];
			Ecc128(&before[page * m_raw_page + c * 128], e);
			matched = std::memcmp(e, &before[page * m_raw_page + m_page + c * 3], 3) == 0;
		}
		if (matched)
			for (int c = 0; c < 4; c++)
				Ecc128(&m_img[page * m_raw_page + c * 128], &m_img[page * m_raw_page + m_page + c * 3]);
	}
}

namespace
{
void NowTod(uint8_t tod[8])
{
	const std::time_t now = std::time(nullptr) + 9 * 3600; // the card keeps Japan time
	std::tm t = {};
	gmtime_r(&now, &t);
	tod[0] = 0, tod[1] = static_cast<uint8_t>(t.tm_sec), tod[2] = static_cast<uint8_t>(t.tm_min), tod[3] = static_cast<uint8_t>(t.tm_hour);
	tod[4] = static_cast<uint8_t>(t.tm_mday), tod[5] = static_cast<uint8_t>(t.tm_mon + 1);
	tod[6] = static_cast<uint8_t>((t.tm_year + 1900) & 255), tod[7] = static_cast<uint8_t>((t.tm_year + 1900) >> 8);
}
void MakeEntry(uint8_t* e, uint16_t mode, uint32_t length, uint32_t cluster, uint32_t dir_entry, const std::string& name, const uint8_t tod[8])
{
	std::memset(e, 0, 512);
	PutU16(e, mode);
	PutU32(e + 4, length);
	std::memcpy(e + 8, tod, 8);
	PutU32(e + 0x10, cluster);
	PutU32(e + 0x14, dir_entry);
	std::memcpy(e + 0x18, tod, 8);
	std::memcpy(e + 0x40, name.data(), std::min<size_t>(31, name.size()));
}
} // namespace

bool MemoryCard::Has(const std::string& dir) const
{
	for (const Save& s : m_saves)
		if (s.dir == dir)
			return true;
	return false;
}

bool MemoryCard::ReadSaveFiles(const std::string& dir, std::vector<SaveFile>& out) const
{
	out.clear();
	if (!m_ok || !m_formatted)
		return false;
	if (m_folder)
	{
		const std::string p = m_path + "/" + dir;
		DIR* d = opendir(p.c_str());
		if (!d)
			return false;
		while (dirent* e = readdir(d))
		{
			struct stat st;
			const std::string f = p + "/" + e->d_name;
			if (e->d_name[0] == '.' || IsPcsx2File(e->d_name) || stat(f.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			SaveFile sf;
			sf.name = e->d_name;
			if (!ReadAll(f, sf.data, 8u << 20))
			{
				closedir(d);
				return false;
			}
			std::vector<uint8_t> meta; // PCSX2's own entry for it: the card name (when cleaned) and mode
			if (ReadAll(p + "/_pcsx2_meta/" + e->d_name, meta, 4096) && meta.size() >= 0x60 && (U16(&meta[0]) & 0x8010) == 0x8010)
			{
				sf.mode = U16(&meta[0]);
				const char* nm = reinterpret_cast<const char*>(&meta[0x40]);
				if (nm[0])
					sf.name.assign(nm, strnlen(nm, 32));
			}
			out.push_back(std::move(sf));
		}
		closedir(d);
		return true;
	}
	for (const Entry& r : m_root_entries)
		if ((r.mode & 0x8000) && (r.mode & 0x0020) && r.name == dir)
		{
			std::vector<Entry> files;
			if (!ReadDir(r.cluster, r.length, files))
				return false;
			for (const Entry& f : files)
				if ((f.mode & 0x8000) && (f.mode & 0x0010))
				{
					SaveFile sf;
					sf.name = f.name;
					sf.mode = f.mode;
					if (f.length > 0 && !ReadFile(f, sf.data))
						return false;
					out.push_back(std::move(sf));
				}
			return true;
		}
	return false;
}

bool MemoryCard::AddDirInImage(const std::string& dir, const std::vector<SaveFile>& files, bool* no_space)
{
	const uint32_t csize = m_page * m_ppc, per = csize / 512;
	// Room: the folder's entries (., .. and the files) and each file in whole clusters (plus a root cluster, perhaps).
	uint32_t need = (static_cast<uint32_t>(files.size()) + 2 + per - 1) / per + 1;
	for (const SaveFile& f : files)
		need += static_cast<uint32_t>((f.data.size() + csize - 1) / csize);
	uint32_t free_clusters = 0;
	for (uint32_t c = 0; c + m_alloc_off < m_alloc_end && c < m_clusters; c++)
	{
		uint32_t v;
		if (Fat(c, v) && !(v & 0x80000000))
			free_clusters++;
	}
	if (free_clusters < need)
	{
		if (no_space)
			*no_space = true;
		return false;
	}
	uint8_t tod[8];
	NowTod(tod);
	std::vector<uint32_t> root_chain;
	if (!Chain(m_root, root_chain) || m_root_entries.empty())
		return false;
	const uint32_t index = m_root_entries[0].length; // the root's "." counts its entries
	// The files' clusters and data.
	std::vector<uint32_t> first(files.size(), 0);
	std::vector<uint8_t> buf(csize);
	for (size_t i = 0; i < files.size(); i++)
	{
		const std::vector<uint8_t>& d = files[i].data;
		uint32_t prev = 0xFFFFFFFF;
		for (size_t at = 0; at < d.size(); at += csize)
		{
			uint32_t c;
			if (!AllocCluster(c))
				return false;
			if (prev == 0xFFFFFFFF)
				first[i] = c;
			else if (!FatSet(prev, 0x80000000u | c))
				return false;
			std::fill(buf.begin(), buf.end(), 0);
			std::memcpy(buf.data(), d.data() + at, std::min<size_t>(csize, d.size() - at));
			if (!WriteCluster(c, buf.data()))
				return false;
			prev = c;
		}
	}
	// The folder: its entries, `per` to a cluster.
	const uint32_t n = static_cast<uint32_t>(files.size()) + 2;
	std::vector<uint32_t> dchain;
	for (uint32_t k = 0; k < (n + per - 1) / per; k++)
	{
		uint32_t c;
		if (!AllocCluster(c))
			return false;
		if (!dchain.empty() && !FatSet(dchain.back(), 0x80000000u | c))
			return false;
		dchain.push_back(c);
	}
	for (uint32_t k = 0; k < dchain.size(); k++)
	{
		std::fill(buf.begin(), buf.end(), 0);
		for (uint32_t e = 0; e < per; e++)
		{
			const uint32_t idx = k * per + e;
			uint8_t* ent = buf.data() + e * 512;
			if (idx == 0)
				MakeEntry(ent, 0x8427, 0, 0, index, ".", tod); // as the BIOS writes a save's "." (length and cluster 0)
			else if (idx == 1)
				MakeEntry(ent, 0xA426, 0, 0, 0, "..", tod);
			else if (idx < n)
			{
				const SaveFile& f = files[idx - 2];
				MakeEntry(ent, f.mode ? f.mode : 0x8497, static_cast<uint32_t>(f.data.size()), f.data.empty() ? 0 : first[idx - 2], 0, f.name, tod);
			}
		}
		if (!WriteCluster(dchain[k], buf.data()))
			return false;
	}
	// The root's new entry (a new root cluster when the last one is full), and its count.
	if (index / per >= root_chain.size())
	{
		uint32_t extra;
		if (!AllocCluster(extra) || !FatSet(root_chain.back(), 0x80000000u | extra))
			return false;
		root_chain.push_back(extra);
		std::fill(buf.begin(), buf.end(), 0);
		if (!WriteCluster(extra, buf.data()))
			return false;
	}
	uint8_t re[512];
	MakeEntry(re, 0x8427, n, dchain[0], 0, dir, tod);
	const size_t off = EntryOffset(root_chain, index);
	if (!off || !WriteBytes(off, re, 512))
		return false;
	uint8_t len[4];
	PutU32(len, index + 1);
	return WriteBytes(m_root_entries[0].raw_offset + 4, len, 4);
}

bool MemoryCard::SaveImage(const std::string& out_path, const std::vector<uint8_t>& before, bool backup)
{
	FixEcc(before);
	if (backup)
		if (FILE* f = std::fopen((out_path + ".shelf-backup").c_str(), "wb"))
		{
			std::fwrite(before.data(), 1, before.size(), f);
			std::fclose(f);
		}
	const std::string tmp = out_path + ".tmp";
	FILE* f = std::fopen(tmp.c_str(), "wb");
	bool ok = f && std::fwrite(m_img.data(), 1, m_img.size(), f) == m_img.size();
	if (f)
		std::fclose(f);
	ok = ok && std::rename(tmp.c_str(), out_path.c_str()) == 0;
	if (!ok)
		unlink(tmp.c_str());
	return ok;
}

bool MemoryCard::AddSave(const std::string& dir, const std::vector<SaveFile>& files, bool* no_space)
{
	if (no_space)
		*no_space = false;
	if (!m_ok || !m_formatted || Has(dir))
		return false;
	if (m_folder)
	{
		uint64_t bytes = 0;
		for (const SaveFile& f : files)
			bytes += f.data.size();
		if (bytes / 1024 + 2 > m_free_kb)
		{
			if (no_space)
				*no_space = true;
			return false;
		}
		const std::string p = m_path + "/" + CleanFolderName(dir);
		if (mkdir(p.c_str(), 0777) != 0)
			return false;
		bool ok = true;
		for (const SaveFile& f : files)
		{
			const std::string host = CleanFolderName(f.name);
			FILE* o = std::fopen((p + "/" + host).c_str(), "wb");
			ok = o && std::fwrite(f.data.data(), 1, f.data.size(), o) == f.data.size() && ok;
			if (o)
				std::fclose(o);
			if (host != f.name || f.mode != 0x8497) // as PCSX2's FileAccessHelper::WriteMetadata: the card entry beside it
			{
				mkdir((p + "/_pcsx2_meta").c_str(), 0777);
				uint8_t ent[512] = {};
				PutU16(ent, f.mode);
				PutU32(ent + 4, static_cast<uint32_t>(f.data.size()));
				std::memcpy(ent + 0x40, f.name.data(), std::min<size_t>(31, f.name.size()));
				if (FILE* m = std::fopen((p + "/_pcsx2_meta/" + host).c_str(), "wb"))
				{
					ok = std::fwrite(ent, 1, sizeof(ent), m) == sizeof(ent) && ok;
					std::fclose(m);
				}
			}
		}
		Open(m_path);
		return ok;
	}
	const std::vector<uint8_t> before = m_img;
	m_dirty_pages.clear();
	if (!AddDirInImage(dir, files, no_space))
	{
		m_img = before;
		m_dirty_pages.clear();
		return false;
	}
	const bool ok = SaveImage(m_path, before, true);
	Open(m_path);
	return ok;
}

bool MemoryCard::WriteHistoryCopy(const std::vector<HistoryEntry>& entries, const std::string& out_path)
{
	if (!m_ok || m_folder || !m_formatted)
		return false;
	const std::vector<uint8_t> before = m_img;
	m_dirty_pages.clear();
	auto done = [&](bool ok) {
		m_img = before; // this card stays as it was
		m_dirty_pages.clear();
		return ok;
	};
	SaveFile hist;
	hist.name = "history";
	hist.data.assign(21 * 22, 0);
	for (size_t i = 0; i < entries.size() && i < 21; i++)
	{
		uint8_t* e = hist.data.data() + i * 22;
		std::memcpy(e, entries[i].id.data(), std::min<size_t>(15, entries[i].id.size()));
		e[0x10] = static_cast<uint8_t>(std::clamp(entries[i].count, 1, 255));
		e[0x11] = static_cast<uint8_t>(entries[i].mask & 0x3F);
		e[0x12] = static_cast<uint8_t>(std::clamp(entries[i].cell, 0, 5));
	}
	const std::string dirname = "BEDATA-SYSTEM";
	// An existing BEDATA-SYSTEM/history: its data is replaced in place.
	for (const Entry& r : m_root_entries)
		if ((r.mode & 0x8000) && (r.mode & 0x0020) && r.name == dirname)
		{
			std::vector<Entry> files;
			if (!ReadDir(r.cluster, r.length, files))
				return done(false);
			for (const Entry& f : files)
				if ((f.mode & 0x8000) && f.name == "history")
				{
					std::vector<uint32_t> chain;
					if (!Chain(f.cluster, chain) || chain.empty())
						return done(false);
					std::vector<uint8_t> c(m_page * m_ppc, 0);
					std::memcpy(c.data(), hist.data.data(), hist.data.size());
					uint8_t len[4];
					PutU32(len, static_cast<uint32_t>(hist.data.size()));
					if (!WriteCluster(chain[0], c.data()) || !WriteBytes(f.raw_offset + 4, len, 4))
						return done(false);
					return done(SaveImage(out_path, before, false));
				}
			return done(false); // the folder without the file: leave it
		}
	if (!AddDirInImage(dirname, {hist}, nullptr))
		return done(false);
	return done(SaveImage(out_path, before, false));
}

std::vector<const Save*> MemoryCard::ForSerial(const std::string& serial) const
{
	std::vector<const Save*> out;
	for (const Save& s : m_saves)
		if (Matches(s.dir, serial))
			out.push_back(&s);
	return out;
}
} // namespace s3
