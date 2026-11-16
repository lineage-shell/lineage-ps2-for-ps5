// PS5 port frontend: the shelf's sounds (see fe_sound.h).
//
// Every sound is built from one ingredient, a key tap: a short pitched "pok" whose pitch drops as
// it sounds (the body), a burst of band-passed noise (the key bottoming out) and a faint ring (the
// plastic). A step adds the softer, higher tap of the key coming back up. The clips get a top cut
// and a touch of room (a few early reflections, different on each side), so they sound close and
// soft rather than sharp. vk-285-48 made them softer at the user's ask: 5-6 dB less loud, a rounder
// onset, a smaller and lower click and a darker top (12 dB less above 2.5 kHz).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_sound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fe
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;
constexpr float kRate = static_cast<float>(SoundBank::kRate);
constexpr float kLevel = 0.34f; // every clip's peak scales by this (vk-285-47 was 1.0)

struct Rng
{
	uint32_t s;
	// -1..1
	float Next()
	{
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return static_cast<float>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
	}
};

struct Biquad
{
	float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
	float Run(float x)
	{
		const float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
};

// The RBJ cookbook band-pass, 0 dB at the centre.
Biquad BandPass(float hz, float q)
{
	const float w = static_cast<float>(kTwoPi) * hz / kRate, alpha = std::sin(w) / (2.0f * q), a0 = 1.0f + alpha;
	Biquad f;
	f.b0 = alpha / a0;
	f.b2 = -alpha / a0;
	f.a1 = -2.0f * std::cos(w) / a0;
	f.a2 = (1.0f - alpha) / a0;
	return f;
}

void LowPass(std::vector<float>& x, float hz)
{
	const float a = 1.0f - std::exp(-static_cast<float>(kTwoPi) * hz / kRate);
	float y = 0;
	for (float& v : x)
	{
		y += a * (v - y);
		v = y;
	}
}

struct Tap
{
	float body_hz = 640;          // where the body's pitch settles
	float glide = 0.30f;          // it starts this much higher ...
	float glide_s = 0.0035f;      // ... and falls with this time constant
	float body_s = 0.020f;        // the body's decay time constant
	float body_attack_s = 0.0015f;
	float click_hz = 3200, click_q = 0.9f, click_s = 0.0018f, click = 0.12f;
	float click_attack_s = 0.0006f;
	float ring_hz = 2350, ring_s = 0.006f, ring = 0.03f;
	uint32_t seed = 1;
};

void Grow(std::vector<float>& out, size_t size)
{
	if (out.size() < size)
		out.resize(size, 0.0f);
}

// Adds `tap` at `at` seconds.
void AddTap(std::vector<float>& out, float at, const Tap& tap, float gain)
{
	const float longest = std::max(tap.body_s, std::max(tap.click_s, tap.ring_s));
	const size_t n = static_cast<size_t>(longest * 7.0f * kRate); // to about -60 dB
	const size_t start = static_cast<size_t>(at * kRate);
	Grow(out, start + n);
	Rng rng{tap.seed * 0x9e3779b9u + 0x7f4a7c15u};
	Biquad bp = BandPass(tap.click_hz, tap.click_q);
	double phase = 0, ring_phase = 0;
	for (size_t i = 0; i < n; i++)
	{
		const float t = static_cast<float>(i) / kRate;
		const float hz = tap.body_hz * (1.0f + tap.glide * std::exp(-t / tap.glide_s));
		phase += kTwoPi * hz / kRate;
		ring_phase += kTwoPi * tap.ring_hz / kRate;
		const float body = static_cast<float>(std::sin(phase)) * (1.0f - std::exp(-t / tap.body_attack_s)) * std::exp(-t / tap.body_s);
		const float noise = bp.Run(rng.Next()) * 3.0f;
		const float click = noise * (1.0f - std::exp(-t / tap.click_attack_s)) * std::exp(-t / tap.click_s);
		const float ring = static_cast<float>(std::sin(ring_phase)) * (1.0f - std::exp(-t / 0.0003f)) * std::exp(-t / tap.ring_s);
		out[start + i] += gain * (body + tap.click * click + tap.ring * ring);
	}
}

// A soft bell-like note: the fundamental, a quieter octave and a brief shimmer.
void AddChime(std::vector<float>& out, float at, float hz, float decay_s, float gain)
{
	const size_t n = static_cast<size_t>(decay_s * 5.5f * kRate);
	const size_t start = static_cast<size_t>(at * kRate);
	Grow(out, start + n);
	for (size_t i = 0; i < n; i++)
	{
		const float t = static_cast<float>(i) / kRate;
		const double w = kTwoPi * hz * t;
		const float env = (1.0f - std::exp(-t / 0.008f)) * std::exp(-t / decay_s);
		const float v = static_cast<float>(std::sin(w)) + 0.28f * std::exp(-t / 0.06f) * static_cast<float>(std::sin(2.0 * w)) +
						0.02f * std::exp(-t / 0.02f) * static_cast<float>(std::sin(5.1 * w));
		out[start + i] += gain * env * v;
	}
}

// Mono in, stereo out: the top cut, the early reflections, a short fade at the end, and the level
// set so the loudest sample is `peak`.
std::vector<float> Finish(std::vector<float> mono, float peak, float top_hz = 5000.0f)
{
	peak *= kLevel;
	LowPass(mono, top_hz);
	std::vector<float> dark = mono;
	LowPass(dark, 4000.0f);
	struct Echo
	{
		float ms, gain;
	};
	const Echo left[2] = {{3.1f, 0.16f}, {7.3f, 0.09f}}, right[2] = {{4.3f, 0.16f}, {8.9f, 0.09f}};
	const size_t frames = mono.size() + static_cast<size_t>(0.010f * kRate);
	std::vector<float> lr(frames * 2, 0.0f);
	auto echo = [&](const Echo* taps, size_t i) {
		float sum = 0;
		for (int k = 0; k < 2; k++)
		{
			const size_t d = static_cast<size_t>(taps[k].ms * kRate / 1000.0f);
			if (i >= d && i - d < dark.size())
				sum += taps[k].gain * dark[i - d];
		}
		return sum;
	};
	for (size_t i = 0; i < frames; i++)
	{
		const float dry = i < mono.size() ? mono[i] : 0.0f;
		lr[2 * i] = dry + echo(left, i);
		lr[2 * i + 1] = dry + echo(right, i);
	}
	float loudest = 0;
	for (float v : lr)
		loudest = std::max(loudest, std::fabs(v));
	const float k = loudest > 0 ? peak / loudest : 0.0f;
	const size_t fade = static_cast<size_t>(0.004f * kRate);
	for (size_t i = 0; i < frames; i++)
	{
		const float g = i + fade > frames ? k * static_cast<float>(frames - i) / static_cast<float>(fade) : k;
		lr[2 * i] *= g;
		lr[2 * i + 1] *= g;
	}
	return lr;
}

// The key coming back up after a step: higher, shorter and softer than going down.
Tap KeyUp(const Tap& down, uint32_t seed)
{
	Tap up = down;
	up.body_hz = down.body_hz * 1.55f;
	up.glide = 0.25f;
	up.body_s = 0.007f;
	up.click_hz = 3800.0f;
	up.ring = 0.0f;
	up.seed = seed;
	return up;
}
} // namespace

void SoundBank::Build()
{
	// Six keys a little apart in pitch, so a run of steps doesn't sound like a machine.
	const float pitch[6] = {1.00f, 1.07f, 0.94f, 1.12f, 0.97f, 1.04f};
	m_move.clear();
	m_repeat.clear();
	for (int i = 0; i < 6; i++)
	{
		Tap down;
		down.body_hz = 640.0f * pitch[i];
		down.ring_hz = 2350.0f * pitch[i];
		down.seed = 10 + static_cast<uint32_t>(i);
		std::vector<float> m;
		AddTap(m, 0.0f, down, 1.0f);
		AddTap(m, 0.072f + 0.004f * static_cast<float>(i), KeyUp(down, 20 + static_cast<uint32_t>(i)), 0.15f);
		m_move.push_back(Finish(std::move(m), 0.30f));

		Tap quick = down;
		quick.body_s = 0.013f;
		quick.seed = 30 + static_cast<uint32_t>(i);
		std::vector<float> q;
		AddTap(q, 0.0f, quick, 1.0f);
		m_repeat.push_back(Finish(std::move(q), 0.20f));
	}

	// L1/R1: three quick taps rising (on) or falling (back), then the key coming up.
	auto jump = [](bool rising) {
		const float hz[3] = {560.0f, 680.0f, 830.0f}, gain[3] = {0.65f, 0.8f, 1.0f};
		std::vector<float> m;
		Tap last;
		for (int i = 0; i < 3; i++)
		{
			Tap t;
			t.body_hz = rising ? hz[i] : hz[2 - i];
			t.ring_hz = t.body_hz * 3.6f;
			t.body_s = 0.012f;
			t.seed = 40 + static_cast<uint32_t>(i) + (rising ? 0u : 3u);
			AddTap(m, 0.03f * static_cast<float>(i), t, gain[i]);
			last = t;
		}
		AddTap(m, 0.15f, KeyUp(last, 50), 0.14f);
		return Finish(std::move(m), 0.31f);
	};
	m_jump_left = jump(false);
	m_jump_right = jump(true);

	// The end of the shelf: a muted, lower "tuk" and a small bounce.
	{
		Tap t;
		t.body_hz = 290.0f;
		t.glide = 0.2f;
		t.body_s = 0.026f;
		t.click = 0.12f;
		t.click_hz = 2200.0f;
		t.ring = 0.0f;
		t.seed = 60;
		Tap bounce = t;
		bounce.body_hz = 320.0f;
		bounce.body_s = 0.014f;
		bounce.seed = 61;
		std::vector<float> m;
		AddTap(m, 0.0f, t, 1.0f);
		AddTap(m, 0.065f, bounce, 0.3f);
		m_edge = Finish(std::move(m), 0.34f, 3500.0f);
	}

	// A game picked: a fuller "enter" key, then two soft chime notes going up (G5, D6).
	{
		Tap enter;
		enter.body_hz = 380.0f;
		enter.glide = 0.35f;
		enter.body_s = 0.032f;
		enter.click = 0.15f;
		enter.click_hz = 3300.0f;
		enter.ring_hz = 1800.0f;
		enter.ring = 0.07f;
		enter.seed = 70;
		Tap rattle;
		rattle.body_hz = 950.0f;
		rattle.glide = 0.2f;
		rattle.body_s = 0.004f;
		rattle.click = 0.5f;
		rattle.click_hz = 3500.0f;
		rattle.ring = 0.0f;
		rattle.seed = 71;
		std::vector<float> m;
		AddTap(m, 0.0f, enter, 1.0f);
		AddTap(m, 0.011f, rattle, 0.10f);
		AddChime(m, 0.075f, 783.99f, 0.11f, 0.40f);
		AddChime(m, 0.155f, 1174.66f, 0.12f, 0.34f);
		m_launch = Finish(std::move(m), 0.42f);
	}
}

const std::vector<float>* SoundBank::Pick(Sfx sfx)
{
	auto pick = [this](const std::vector<std::vector<float>>& set, int& last) -> const std::vector<float>* {
		if (set.empty())
			return nullptr;
		Rng r{m_rng};
		const int n = static_cast<int>(set.size());
		int i = std::min(n - 1, static_cast<int>((r.Next() * 0.5f + 0.5f) * static_cast<float>(n)));
		m_rng = r.s;
		if (i == last)
			i = (i + 1) % n;
		last = i;
		return &set[static_cast<size_t>(i)];
	};
	// ps2-console-shelf: a console sound read from its file wins. Move and MoveRepeat share move.wav, Launch is select.wav,
	// L1/R1's jumps are page.wav, the end of the shelf is error.wav.
	{
		Sfx file = sfx;
		switch (sfx)
		{
			case Sfx::MoveRepeat: file = Sfx::Move; break;
			case Sfx::Launch: file = Sfx::Select; break;
			case Sfx::JumpLeft:
			case Sfx::JumpRight: file = Sfx::Page; break;
			case Sfx::Edge: file = Sfx::Error; break;
			default: break;
		}
		const std::vector<float>& f = m_file[static_cast<int>(file)];
		if (!f.empty())
			return &f;
	}
	switch (sfx)
	{
		case Sfx::Move: return pick(m_move, m_last_move);
		case Sfx::MoveRepeat: return pick(m_repeat, m_last_repeat);
		case Sfx::JumpLeft: return &m_jump_left;
		case Sfx::JumpRight: return &m_jump_right;
		case Sfx::Edge: return &m_edge;
		case Sfx::Launch: return &m_launch;
		// ps2-console-shelf: the console sounds without their file fall back to the nearest key sound (or stay silent).
		case Sfx::Select: return pick(m_move, m_last_move);
		case Sfx::Back: return pick(m_repeat, m_last_repeat);
		case Sfx::Error: return &m_edge;
		case Sfx::Open: return pick(m_move, m_last_move);
		case Sfx::Close: return pick(m_repeat, m_last_repeat);
		case Sfx::Page: return &m_jump_right;
		case Sfx::Done: return &m_launch;
		case Sfx::Startup:
		case Sfx::Disc: return nullptr;
	}
	return nullptr;
}


namespace
{
uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
} // namespace

bool SoundBank::ReadWav(const std::string& path, std::vector<float>& out, std::string& why, double max_seconds)
{
	out.clear();
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
	{
		why = "missing";
		return false;
	}
	std::vector<uint8_t> d;
	{
		uint8_t buf[65536];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		{
			d.insert(d.end(), buf, buf + n);
			if (d.size() > (64u << 20))
				break;
		}
		std::fclose(f);
	}
	if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4))
	{
		why = "not a RIFF/WAVE file";
		return false;
	}
	int format = 0, channels = 0, bits = 0;
	uint32_t rate = 0;
	const uint8_t* pcm = nullptr;
	size_t pcm_bytes = 0;
	for (size_t at = 12; at + 8 <= d.size();)
	{
		const uint32_t size = Le32(d.data() + at + 4);
		const uint8_t* body = d.data() + at + 8;
		const size_t have = std::min<size_t>(size, d.size() - at - 8);
		if (!std::memcmp(d.data() + at, "fmt ", 4) && have >= 16)
		{
			format = Le16(body);
			channels = Le16(body + 2);
			rate = Le32(body + 4);
			bits = Le16(body + 14);
			if (format == 0xFFFE && have >= 26) // WAVE_FORMAT_EXTENSIBLE: the sub-format's first two bytes
				format = Le16(body + 24);
		}
		else if (!std::memcmp(d.data() + at, "data", 4))
		{
			pcm = body;
			pcm_bytes = have;
		}
		at += 8 + static_cast<size_t>(size) + (size & 1);
	}
	if (format != 1 || bits != 16 || (channels != 1 && channels != 2))
	{
		why = "not 16-bit PCM mono/stereo (format " + std::to_string(format) + ", " + std::to_string(bits) + " bits, " + std::to_string(channels) +
			" channels)";
		return false;
	}
	if (rate != 22050 && rate != 44100 && rate != 48000 && rate != 32000 && rate != 24000 && rate != 16000)
	{
		why = "sample rate " + std::to_string(rate) + " not supported";
		return false;
	}
	if (!pcm || pcm_bytes < static_cast<size_t>(channels) * 2)
	{
		why = "no samples";
		return false;
	}
	size_t frames = pcm_bytes / (static_cast<size_t>(channels) * 2);
	frames = std::min(frames, static_cast<size_t>(max_seconds * rate));
	// Trailing silence (below about -66 dB) is cut: it would only hold a voice.
	auto sample = [&](size_t i, int c) { return static_cast<int16_t>(Le16(pcm + (i * static_cast<size_t>(channels) + static_cast<size_t>(c)) * 2)); };
	while (frames > 1)
	{
		const int16_t l = sample(frames - 1, 0), r = sample(frames - 1, channels - 1);
		if (std::abs(l) > 16 || std::abs(r) > 16)
			break;
		frames--;
	}
	// Linear resampling to kRate (the files are short UI sounds: good enough, and it is done once at start).
	const double step = static_cast<double>(rate) / kRate;
	const size_t out_frames = static_cast<size_t>(static_cast<double>(frames) / step);
	out.resize(out_frames * 2);
	for (size_t i = 0; i < out_frames; i++)
	{
		const double src = static_cast<double>(i) * step;
		const size_t i0 = static_cast<size_t>(src);
		const size_t i1 = std::min(i0 + 1, frames - 1);
		const float t = static_cast<float>(src - static_cast<double>(i0));
		for (int c = 0; c < 2; c++)
		{
			const int ch = channels == 2 ? c : 0;
			const float a = sample(i0, ch) / 32768.0f, b = sample(i1, ch) / 32768.0f;
			out[2 * i + static_cast<size_t>(c)] = a + (b - a) * t;
		}
	}
	return !out.empty();
}

int SoundBank::LoadDir(const std::string& dir)
{
	static const char* const kNames[kSfxCount] = {
		"move", nullptr, nullptr, nullptr, nullptr, nullptr, // Move (and MoveRepeat), then the key sounds that borrow another file
		"select", "back", "error", "open", "close", "page", "startup", "disc", "done"};
	static_assert(static_cast<int>(Sfx::Select) == 6, "kNames follows Sfx");
	int loaded = 0;
	for (int i = 0; i < kSfxCount; i++)
	{
		if (!kNames[i])
			continue;
		std::string why;
		const std::string path = dir + "/" + kNames[i] + ".wav";
		if (ReadWav(path, m_file[i], why))
		{
			for (float& v : m_file[i])
				v *= 0.8f; // a little under full scale, like the key sounds' headroom
			loaded++;
		}
		else if (why != "missing")
			std::printf("[frontend] sound: %s skipped: %s\n", path.c_str(), why.c_str());
	}
	std::string why;
	if (ReadWav(dir + "/ambient.wav", m_ambient, why, 120.0))
		loaded++;
	else if (why != "missing")
		std::printf("[frontend] sound: %s/ambient.wav skipped: %s\n", dir.c_str(), why.c_str());
	std::printf("[frontend] sound: %d console sounds from %s%s\n", loaded, dir.c_str(), m_ambient.empty() ? "" : " (with the ambient drone)");
	return loaded;
}

void Mixer::SetAmbient(bool on)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_ambient_on = on;
}

void Mixer::Play(Sfx sfx, float pan)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::vector<float>* clip = m_override[static_cast<int>(sfx)] ? m_override[static_cast<int>(sfx)] : m_bank.Pick(sfx); // shelf v3
	if (!clip || clip->empty())
		return;
	pan = std::max(-1.0f, std::min(1.0f, pan));
	Voice v;
	v.clip = clip;
	v.pos = 0;
	v.gl = (pan > 0.0f ? 1.0f - pan : 1.0f) * m_volume;
	v.gr = (pan < 0.0f ? 1.0f + pan : 1.0f) * m_volume;
	if (m_voices.size() >= kMaxVoices)
		m_voices.erase(m_voices.begin());
	m_voices.push_back(v);
}

// shelf v3 (AI-assisted): a clip of the caller's (a BIOS sound effect), centred.
void Mixer::PlayClip(const std::vector<float>* clip)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!clip || clip->empty())
		return;
	Voice v;
	v.clip = clip;
	v.pos = 0;
	v.gl = v.gr = m_volume;
	if (m_voices.size() >= kMaxVoices)
		m_voices.erase(m_voices.begin());
	m_voices.push_back(v);
}

void Mixer::OverrideSfx(Sfx sfx, const std::vector<float>* clip)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_override[static_cast<int>(sfx)] = clip;
}

void Mixer::SetStream(std::function<void(float* out, int frames)> fn)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_stream = std::move(fn);
}

void Mixer::Mix(float* out, int frames)
{
	if (m_out_rate == kNative)
	{
		MixNative(out, frames);
		return;
	}
	// Another device rate: the native mix, linearly converted (pos steps kNative / m_out_rate native frames an output frame).
	const double step = static_cast<double>(kNative) / m_out_rate;
	for (int i = 0; i < frames; i++)
	{
		size_t i0 = static_cast<size_t>(m_rs_pos);
		while (i0 + 1 >= m_rs.size() / 2)
		{
			// Keep the last frame, add 256 new native frames.
			std::vector<float> keep;
			if (m_rs.size() >= 2)
				keep.assign(m_rs.end() - 2, m_rs.end()), m_rs_pos -= static_cast<double>(m_rs.size() / 2 - 1);
			else
				m_rs_pos = 0;
			m_rs = keep;
			const size_t base = m_rs.size();
			m_rs.resize(base + 512);
			MixNative(m_rs.data() + base, 256);
			i0 = static_cast<size_t>(m_rs_pos);
		}
		const float t = static_cast<float>(m_rs_pos - static_cast<double>(i0));
		out[2 * i] = m_rs[2 * i0] + (m_rs[2 * i0 + 2] - m_rs[2 * i0]) * t;
		out[2 * i + 1] = m_rs[2 * i0 + 1] + (m_rs[2 * i0 + 3] - m_rs[2 * i0 + 1]) * t;
		m_rs_pos += step;
	}
}

void Mixer::MixNative(float* out, int frames)
{
	std::fill(out, out + frames * 2, 0.0f);
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (Voice& v : m_voices)
		{
			const std::vector<float>& c = *v.clip;
			const size_t n = std::min((c.size() - v.pos) / 2, static_cast<size_t>(frames));
			for (size_t i = 0; i < n; i++)
			{
				out[2 * i] += c[v.pos + 2 * i] * v.gl;
				out[2 * i + 1] += c[v.pos + 2 * i + 1] * v.gr;
			}
			v.pos += 2 * n;
		}
		m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [](const Voice& v) { return v.pos >= v.clip->size(); }),
			m_voices.end());
		// ps2-console-shelf: the ambient drone, looped, quiet, with a slow fade (about a second) in and out.
		const std::vector<float>& amb = m_bank.Ambient();
		if (!amb.empty() && (m_ambient_on || m_ambient_gain > 0.0f))
		{
			constexpr float kAmbientLevel = 0.30f;
			const float target = m_ambient_on ? kAmbientLevel * m_volume : 0.0f;
			const float rate = kAmbientLevel / static_cast<float>(SoundBank::kRate); // full level in about one second
			for (int i = 0; i < frames; i++)
			{
				if (m_ambient_gain < target)
					m_ambient_gain = std::min(target, m_ambient_gain + rate);
				else if (m_ambient_gain > target)
					m_ambient_gain = std::max(target, m_ambient_gain - rate);
				if (m_ambient_pos + 1 >= amb.size())
					m_ambient_pos = 0;
				out[2 * i] += amb[m_ambient_pos] * m_ambient_gain;
				out[2 * i + 1] += amb[m_ambient_pos + 1] * m_ambient_gain;
				m_ambient_pos += 2;
			}
		}
		if (m_stream) // shelf v3: the menu's music
			m_stream(out, frames);
	}
	// Several voices at once can add up: bend anything past 0.8 smoothly towards 1.
	for (int i = 0; i < frames * 2; i++)
	{
		const float x = out[i], a = std::fabs(x);
		if (a > 0.8f)
			out[i] = std::copysign(0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f), x);
	}
}

bool Mixer::Idle()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_voices.empty() && !m_stream; // shelf v3: not while the menu's music is set
}
} // namespace fe
