// PS2 shelf v3 (AI-assisted): the PS2 menu's music sequencer (spec sound.md 4 and 7): an SSsq sequence (CLOKS, the main menu's ambience)
// played with the BIOS's BOOT bank instruments (SNDBOOTH / SNDBOOTB) from the player's own BIOS. Rendered on the audio thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "s3_bios.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace s3
{
class SeqPlayer
{
public:
	// The bank and the sequence member ("SNDCLOKS"); false without them.
	bool Load(const Bios& bios, const char* seq, int seq_volume); // the 0x5013 volume (sound.md 1: BOOTS 66, CLOKS 45, ...)
	// Main thread: start from the top, or key every voice off (the notes ring out) and stop.
	void Start();
	void Stop();
	bool playing() const { return m_playing.load(); }
	// Audio thread: adds `frames` stereo float frames at 48 kHz into `out`.
	void Render(float* out, int frames);
	float gain = 1.0f;

private:
	struct Tone
	{
		int lo, hi, centre, fine;
		uint32_t addr;
		uint16_t adsr1, adsr2;
		int vol, pan;
	};
	struct Program
	{
		int mode = 0, vol = 127;
		std::vector<Tone> tones;
	};
	struct Sample
	{
		std::vector<int16_t> pcm;
		int loop = -1;
		bool loops = false;
	};
	struct Voice
	{
		bool on = false;
		int ch = 0, note = 0;
		const Sample* s = nullptr;
		double pos = 0, step = 1;
		float gl = 0, gr = 0;
		SpuEnvelope env;
	};
	const Sample* SampleAt(uint32_t addr);
	void KeyOn(int ch, int note, int vel);
	void KeyOff(int ch, int note);
	void Tick(); // one driver tick (60 Hz): the song advances resolution x BPM / 3600 MIDI ticks
	bool NextEvent();

	std::vector<uint8_t> m_body, m_seq;
	std::vector<Program> m_prog; // by program number
	std::deque<std::pair<uint32_t, Sample>> m_samples; // (a deque: the voices keep pointers into it)
	int m_res = 480, m_bpm = 120, m_seqvol = 66;
	int m_chprog[16] = {}, m_chvol[16] = {}, m_chpan[16] = {};
	size_t m_pos = 0, m_loop_pos = 0;
	int m_loop_count = 0;
	double m_wait = 0;      // MIDI ticks until the next event
	double m_tick_acc = 0;  // samples into the current driver tick
	bool m_status_ok = false;
	uint8_t m_status = 0;
	bool m_first = true;
	Voice m_voice[24];
	// A small reverb in place of the SPU2's (type 4, return 5074 / 32767): four combs and two all-passes a side.
	struct Comb
	{
		std::vector<float> buf;
		size_t i = 0;
		float fb = 0.8f, damp = 0.25f, last = 0;
	};
	struct AllPass
	{
		std::vector<float> buf;
		size_t i = 0;
	};
	Comb m_comb[2][4];
	AllPass m_ap[2][2];
	bool m_rev_init = false;
	float Reverb(int side, float in);
	std::atomic<bool> m_playing{false};
	std::atomic<int> m_cmd{0}; // 1 start, 2 stop (taken by the audio thread)
	bool m_loaded = false;
};
} // namespace s3
