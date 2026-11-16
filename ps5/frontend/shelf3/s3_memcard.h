// PS2 shelf v3 (AI-assisted): the player's PCSX2 memory cards, read as the PS2 Browser shows them: the saves (folders with an icon.sys),
// their titles, dates, sizes, icon.sys colours and lights, and their 3D icons; and deleting a save. Both PCSX2 card kinds: a card file
// (8 MB .ps2, with or without ECC) and a folder card (a directory holding one folder per save). Own code (the mcfs layout is public).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "s3_bios.h"

#include <cstdint>
#include <string>
#include <vector>

namespace s3
{
struct IconSys
{
	bool ok = false;
	std::string title1, title2; // the two title lines (Shift-JIS turned into ASCII where it can)
	uint32_t bg_alpha = 0;
	float bg[4][3] = {};     // corner colours TL TR BL BR, 0..255 (GS scale: 128 = 1.0)
	float light_dir[3][3] = {};
	float light_col[3][3] = {};
	float ambient[3] = {};
	std::string icon_file;   // the list icon's file name
};

struct Save
{
	std::string dir;         // the folder name on the card ("BESLES-52541GTASA")
	IconSys sys;
	uint32_t bytes = 0;      // the files' sizes (KB = bytes / 1024 rounded up per the BIOS)
	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0; // modified (as stored)
	bool corrupted = false;  // no icon.sys or no icon
	bool builtin = false;    // "_SCEn" icon (the BIOS's system saves)
	int builtin_index = -1;
	std::vector<uint8_t> ico; // the list icon file's bytes (parsed by the caller)
};

class MemoryCard
{
public:
	// Opens a card file or a folder card. False when it is neither (`problem` says why).
	bool Open(const std::string& path);
	bool ok() const { return m_ok; }
	bool formatted() const { return m_formatted; }
	const std::string& problem() const { return m_problem; }
	const std::string& path() const { return m_path; }
	const std::vector<Save>& saves() const { return m_saves; }
	uint32_t free_kb() const { return m_free_kb; }
	// A folder card (PCSX2's folder memory card: no fixed size): the PS2 shows free space, the shelf shows the space its saves use.
	bool folder() const { return m_folder; }
	uint32_t used_kb() const { return m_used_kb; }

	// Deletes save `dir` (its files and folder). A card file is copied to "<path>.shelf-backup" first. True when done.
	bool Delete(const std::string& dir);

	// The PS2 opening's history file (/BEDATA-SYSTEM/history, 21 entries of 22 bytes: id[16], play count, extra-cell mask, main cell),
	// written into a copy of this card at `out_path` (this card is not changed). True when the copy was written.
	struct HistoryEntry
	{
		std::string id;
		int count = 1, mask = 0, cell = 0;
	};
	bool WriteHistoryCopy(const std::vector<HistoryEntry>& entries, const std::string& out_path);

	// A save's files, for a copy to another card.
	struct SaveFile
	{
		std::string name;
		std::vector<uint8_t> data;
		uint16_t mode = 0x8497;
	};
	bool ReadSaveFiles(const std::string& dir, std::vector<SaveFile>& out) const;
	// Adds a save folder with these files. A card file: a copy of the card goes to "<path>.shelf-backup" first. False when it does not fit
	// (`no_space` then true) or the folder is there already.
	bool AddSave(const std::string& dir, const std::vector<SaveFile>& files, bool* no_space = nullptr);
	bool Has(const std::string& dir) const;

	// The saves whose folder name holds a game's serial ("SLES-52541" matches "BESLES-52541GTASA" and "BESLES52541..." forms).
	std::vector<const Save*> ForSerial(const std::string& serial) const;

private:
	struct Entry
	{
		uint16_t mode = 0;
		uint32_t length = 0, cluster = 0;
		uint8_t mtime[8] = {};
		std::string name;
		size_t raw_offset = 0; // where its 512 bytes start in the file (card files)
	};
	bool OpenFile();
	bool OpenFolder();
	bool ReadPage(uint32_t page, uint8_t* out) const;
	bool ReadCluster(uint32_t abs_cluster, std::vector<uint8_t>& out) const;
	bool Fat(uint32_t rel, uint32_t& value, size_t* raw_offset = nullptr) const;
	bool Chain(uint32_t first, std::vector<uint32_t>& out) const;
	bool ReadDir(uint32_t first, uint32_t count, std::vector<Entry>& out) const;
	bool ReadFile(const Entry& e, std::vector<uint8_t>& out) const;
	size_t PageOffset(uint32_t page) const { return static_cast<size_t>(page) * m_raw_page; }
	bool WriteBytes(size_t file_offset, const uint8_t* data, size_t n);
	bool FatSet(uint32_t rel, uint32_t value);
	bool AllocCluster(uint32_t& rel);
	bool WriteCluster(uint32_t rel, const uint8_t* data); // a whole cluster of the allocatable area
	size_t EntryOffset(const std::vector<uint32_t>& chain, uint32_t index) const;
	void FixEcc(const std::vector<uint8_t>& before);
	bool AddDirInImage(const std::string& dir, const std::vector<SaveFile>& files, bool* no_space); // card files: into m_img only
	bool SaveImage(const std::string& out_path, const std::vector<uint8_t>& before, bool backup);

	bool m_ok = false, m_folder = false, m_formatted = false;
	std::string m_path, m_problem;
	std::vector<uint8_t> m_img; // a card file, whole (8-9 MB)
	uint32_t m_page = 512, m_raw_page = 528, m_ppc = 2, m_alloc_off = 0, m_alloc_end = 0, m_root = 0, m_clusters = 0;
	uint32_t m_ifc[32] = {};
	uint32_t m_free_kb = 0, m_used_kb = 0;
	std::vector<Save> m_saves;
	std::vector<Entry> m_root_entries;
	std::vector<size_t> m_dirty_pages;
};

// Shift-JIS (full-width ASCII and the common punctuation) as plain ASCII; other characters become '?'.
std::string SjisToAscii(const uint8_t* s, size_t n);
// PS2 memory-card ECC for one 128-byte chunk (3 bytes).
void Ecc128(const uint8_t* chunk, uint8_t out[3]);
} // namespace s3
