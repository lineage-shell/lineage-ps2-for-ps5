// PS5 port frontend: the shelf's sounds. Small key-press sounds, synthesized when the frontend
// starts (nothing is loaded from disk), and a voice mixer that the platform's audio thread pulls
// from. The shelf only reports what happened; whether anything plays is the platform's business.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace fe
{
enum class Sfx
{
	Move,       // one step along the shelf
	MoveRepeat, // a step while the direction is held (quieter, shorter)
	JumpLeft,   // L1: five back
	JumpRight,  // R1: five on
	Edge,       // a step past either end
	Launch,     // a game picked
	// ps2-console-shelf (AI-assisted): the PS2 system sounds, from /data/PCSX2/console-sounds/<name>.wav when there
	// (SoundBank::LoadDir); a missing file falls back to the nearest synthesized sound, or to silence.
	Select,     // select.wav: Cross on a menu item
	Back,       // back.wav: Circle, a screen closed
	Error,      // error.wav: refused (the BIOS line, an empty slot)
	Open,       // open.wav: a sheet or panel opened
	Close,      // close.wav: a sheet or panel closed
	Page,       // page.wav: a group / page turned (L1/R1, L2/R2)
	Startup,    // startup.wav: the first time the shelf appears
	Disc,       // disc.wav: a PS2 disc detected in the drive
	Done,       // done.wav: an install or unpack finished
};
constexpr int kSfxCount = static_cast<int>(Sfx::Done) + 1;

class SoundSink
{
public:
	virtual ~SoundSink() = default;
	// pan: -1 left .. 1 right.
	virtual void Play(Sfx sfx, float pan) = 0;
	// ps2-console-shelf: the quiet browser drone on or off (default: no drone).
	virtual void SetAmbient(bool) {}
	// shelf v3 (AI-assisted): a clip of its own (interleaved stereo float at 48 kHz; it must outlive the play), e.g. a BIOS sound effect.
	virtual void PlayClip(const std::vector<float>* clip) { (void)clip; }
	// shelf v3: `sfx` plays this clip from now on (null: its own again); the clip must outlive the sink. Every Play(sfx) then sounds as the BIOS.
	virtual void OverrideSfx(Sfx sfx, const std::vector<float>* clip) { (void)sfx, (void)clip; }
	// shelf v3: a source the audio thread adds after the sounds (`frames` stereo float frames at 48 kHz into `out`): the menu's music.
	virtual void SetStream(std::function<void(float* out, int frames)> fn) { (void)fn; }
};

// Interleaved stereo float clips at kRate.
class SoundBank
{
public:
	static constexpr int kRate = 48000;

	void Build();
	// ps2-console-shelf: the console sounds in `dir` (move.wav, select.wav, ...; see Sfx), 16-bit PCM, mono or stereo, at
	// 22.05, 44.1 or 48 kHz (resampled to kRate). Returns how many were read; the rest keep their synthesized fallback.
	// ambient.wav (the browser drone) is kept apart: Ambient().
	int LoadDir(const std::string& dir);
	// A clip for `sfx`; the key sounds come in a few variants, never the same one twice running.
	const std::vector<float>* Pick(Sfx sfx);
	const std::vector<float>& Ambient() const { return m_ambient; }

	// One WAV file as interleaved stereo float at kRate; false (and `why`) when it isn't one this reads.
	static bool ReadWav(const std::string& path, std::vector<float>& out, std::string& why, double max_seconds = 60.0);

private:
	std::vector<float> m_file[kSfxCount]; // loaded console sounds (empty: none)
	std::vector<float> m_ambient;
	std::vector<std::vector<float>> m_move, m_repeat;
	std::vector<float> m_jump_left, m_jump_right, m_edge, m_launch;
	uint32_t m_rng = 0x6d2b79f5u;
	int m_last_move = -1, m_last_repeat = -1;
};

class Mixer final : public SoundSink
{
public:
	explicit Mixer(float volume = 1.0f) : m_volume(volume) {}

	void Build() { m_bank.Build(); }
	int LoadDir(const std::string& dir) { return m_bank.LoadDir(dir); } // before the audio thread starts
	void Play(Sfx sfx, float pan) override; // any thread
	void PlayClip(const std::vector<float>* clip) override; // shelf v3
	void OverrideSfx(Sfx sfx, const std::vector<float>* clip) override; // shelf v3
	void SetStream(std::function<void(float* out, int frames)> fn) override; // shelf v3
	// ps2-console-shelf: the ambient drone (ambient.wav, looped quietly) fades in while on is true; nothing without the file.
	void SetAmbient(bool on) override;
	// The device's real rate (Hz; set before the audio thread starts). Everything here is made at kRate (48 kHz, as the PS2's SPU2
	// and the PS5's port); any other device rate is converted at the output, so pitch and tempo never follow the device.
	void SetOutputRate(int hz) { m_out_rate = hz > 0 ? hz : kNative; }
	int OutputRate() const { return m_out_rate; }
	// The audio thread: `frames` of interleaved stereo float at the output rate into `out`.
	void Mix(float* out, int frames);
	bool Idle();

private:
	struct Voice
	{
		const std::vector<float>* clip;
		size_t pos;
		float gl, gr;
	};
	static constexpr size_t kMaxVoices = 12;

	std::mutex m_mutex;
	std::vector<Voice> m_voices;
	SoundBank m_bank;
	const std::vector<float>* m_override[kSfxCount] = {}; // shelf v3
	std::function<void(float*, int)> m_stream;           // shelf v3
	void MixNative(float* out, int frames); // at kRate
	static constexpr int kNative = SoundBank::kRate;
	int m_out_rate = kNative;
	std::vector<float> m_rs;  // native frames for the output converter
	double m_rs_pos = 0;      // read position in m_rs (native frames)
	float m_volume;
	bool m_ambient_on = false;
	size_t m_ambient_pos = 0;
	float m_ambient_gain = 0.0f; // follows m_ambient_on, slowly
};
} // namespace fe
