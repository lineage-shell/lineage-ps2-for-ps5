// PS2 shelf v3 (AI-assisted): the menu music sequencer (see s3_seq.h).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "s3_seq.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace s3
{
namespace
{
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return static_cast<uint32_t>(p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24); }

// The driver's pitch table (sound.md 7): T[i] = floor(4096 x 2^((i - 208)/192) + 0.06).
int PitchT(int i)
{
	i = std::clamp(i, 80, 511);
	return static_cast<int>(std::floor(4096.0 * std::pow(2.0, (i - 208) / 192.0) + 0.06));
}
// The pan table (sound.md 7) by clamp(chpan + tonepan - 64, 0, 127) >> 2: (left, right) of 128.
void PanLR(int idx, int& l, int& r)
{
	if (idx <= 14)
		l = 128, r = 8 * idx;
	else if (idx <= 16)
		l = r = 120;
	else
		l = 112 - 8 * (idx - 17), r = 128;
}
} // namespace

bool SeqPlayer::Load(const Bios& bios, const char* seq, int seq_volume)
{
	m_seqvol = seq_volume;
	const std::vector<uint8_t>* head = bios.Raw("SNDBOOTH");
	const std::vector<uint8_t>* body = bios.Raw("SNDBOOTB");
	const std::vector<uint8_t>* sq = bios.Raw(seq);
	if (!head || !body || !sq || head->size() < 0x20 || sq->size() < 0x112)
		return false;
	m_body = *body;
	m_seq = *sq;
	const uint8_t* h = head->data();
	const uint32_t P = U32(h + 0x10);
	if (P + 2 > head->size())
		return false;
	const int last = U16(h + P);
	m_prog.assign(static_cast<size_t>(last) + 1, Program());
	for (int p = 0; p <= last; p++)
	{
		const size_t at = P + 2 + static_cast<size_t>(p) * 2;
		if (at + 2 > head->size())
			break;
		const uint16_t rel = U16(h + at);
		if (rel == 0xFFFF || P + rel + 8 > head->size())
			continue;
		const uint8_t* pr = h + P + rel;
		Program& g = m_prog[static_cast<size_t>(p)];
		g.mode = pr[0];
		g.vol = pr[1];
		if (g.mode == 0xFF)
			continue; // drum maps: no BOOT sequence uses one
		const int n = (g.mode & 0x7F) + 1;
		for (int t = 0; t < n; t++)
		{
			const size_t to = P + rel + 8 + static_cast<size_t>(t) * 16;
			if (to + 16 > head->size())
				break;
			const uint8_t* q = h + to;
			Tone tone;
			tone.lo = q[0], tone.hi = q[1], tone.centre = q[2], tone.fine = static_cast<int8_t>(q[3]);
			tone.addr = static_cast<uint32_t>(U16(q + 4)) * 8;
			tone.adsr1 = U16(q + 6), tone.adsr2 = U16(q + 8);
			tone.vol = q[0xB], tone.pan = q[0xC];
			g.tones.push_back(tone);
		}
	}
	m_res = U16(m_seq.data() + 2) ? U16(m_seq.data() + 2) : 480;
	m_bpm = U16(m_seq.data() + 4) ? U16(m_seq.data() + 4) : 120;
	m_loaded = true;
	return true;
}

const SeqPlayer::Sample* SeqPlayer::SampleAt(uint32_t addr)
{
	for (auto& [a, s] : m_samples)
		if (a == addr)
			return &s;
	if (addr >= m_body.size())
		return nullptr;
	Sample s;
	s.pcm = SpuAdpcm(m_body, addr, &s.loop, &s.loops);
	m_samples.emplace_back(addr, std::move(s));
	return &m_samples.back().second;
}

void SeqPlayer::Start()
{
	if (m_loaded)
		m_cmd = 1;
}
void SeqPlayer::Stop()
{
	m_cmd = 2;
}

void SeqPlayer::KeyOn(int ch, int note, int vel)
{
	const int pn = m_chprog[ch];
	if (pn < 0 || pn >= static_cast<int>(m_prog.size()))
		return;
	const Program& g = m_prog[static_cast<size_t>(pn)];
	const bool layered = (g.mode & 0x80) != 0;
	for (const Tone& t : g.tones)
	{
		if (note < t.lo || note > t.hi)
			continue;
		const Sample* s = SampleAt(t.addr);
		if (!s || s->pcm.empty())
			continue;
		Voice* v = nullptr;
		for (Voice& c : m_voice)
			if (!c.on)
			{
				v = &c;
				break;
			}
		if (!v) // steal the quietest
		{
			v = &m_voice[0];
			for (Voice& c : m_voice)
				if (c.env.level < v->env.level)
					v = &c;
		}
		// Pitch (0x18914): the driver's table, an octave a shift; sample rate 44.1 kHz x T / 4096.
		const int d = note - t.centre;
		double ratio;
		if (d >= 0)
			ratio = PitchT(16 * (d % 12) + 208 + t.fine) / 4096.0 * static_cast<double>(1 << (d / 12));
		else
			ratio = PitchT(16 * (12 - ((-d) % 12)) + 208 + t.fine) / 4096.0 / static_cast<double>(1 << ((-d) / 12 + 1));
		v->step = ratio * 44100.0 / 48000.0;
		// Volume (0x18C04): ((seqvol x chvol x progvol x velocity) >> 14) x (tonevol x P) >> 14, of 0x3FFF.
		int pl, pr;
		PanLR(std::clamp(m_chpan[ch] + t.pan - 64, 0, 127) >> 2, pl, pr);
		const int base = (m_seqvol * m_chvol[ch] * g.vol * vel) >> 14;
		v->gl = static_cast<float>((base * (t.vol * pl)) >> 14) / 16383.0f;
		v->gr = static_cast<float>((base * (t.vol * pr)) >> 14) / 16383.0f;
		v->on = true, v->ch = ch, v->note = note, v->s = s, v->pos = 0;
		v->env = SpuEnvelope(t.adsr1, t.adsr2);
		if (!layered)
			break;
	}
}

void SeqPlayer::KeyOff(int ch, int note)
{
	for (Voice& v : m_voice)
		if (v.on && v.ch == ch && v.note == note && v.env.phase != 3)
			v.env.KeyOff();
}

static bool ReadVar(const std::vector<uint8_t>& s, size_t& p, uint32_t& out)
{
	out = 0;
	for (int i = 0; i < 4 && p < s.size(); i++)
	{
		const uint8_t b = s[p++];
		out = out << 7 | (b & 0x7F);
		if (!(b & 0x80))
			return true;
	}
	return false;
}

bool SeqPlayer::NextEvent()
{
	// The stream (sound.md 4): event, then its delta; running status; FF 51 lo hi tempo, FF 2F 00 the end; CC 99 = 20 loop start,
	// CC 6 the loop count (127 forever), CC 99 = 30 loop end.
	if (m_pos >= m_seq.size())
		return false;
	uint8_t b = m_seq[m_pos];
	if (b & 0x80)
		m_status = b, m_pos++;
	const uint8_t st = m_status;
	if (st == 0xFF)
	{
		if (m_pos + 1 >= m_seq.size())
			return false;
		const uint8_t type = m_seq[m_pos];
		if (type == 0x2F)
			return false;
		if (type == 0x51 && m_pos + 2 < m_seq.size())
		{
			m_bpm = U16(&m_seq[m_pos + 1]);
			m_pos += 3;
		}
		else
			m_pos += 2;
	}
	else
	{
		const int ch = st & 15, kind = st & 0xF0;
		const uint8_t d1 = m_pos < m_seq.size() ? m_seq[m_pos] : 0;
		const bool two = kind != 0xC0 && kind != 0xD0;
		const uint8_t d2 = two && m_pos + 1 < m_seq.size() ? m_seq[m_pos + 1] : 0;
		m_pos += two ? 2 : 1;
		switch (kind)
		{
			case 0x80: KeyOff(ch, d1); break;
			case 0x90:
				if (d2 == 0)
					KeyOff(ch, d1);
				else
					KeyOn(ch, d1, d2);
				break;
			case 0xB0:
				if (d1 == 7)
					m_chvol[ch] = d2;
				else if (d1 == 10)
					m_chpan[ch] = d2;
				else if (d1 == 99 && d2 == 20)
					m_loop_pos = m_pos, m_loop_count = -1;
				else if (d1 == 6 && m_loop_count == -1)
					m_loop_count = d2;
				else if (d1 == 99 && d2 == 30 && m_loop_pos)
				{
					if (m_loop_count == 127 || m_loop_count > 0)
					{
						if (m_loop_count > 0 && m_loop_count != 127)
							m_loop_count--;
						m_pos = m_loop_pos;
					}
				}
				break;
			case 0xC0: m_chprog[ch] = d1; break;
			default: break;
		}
	}
	uint32_t delta = 0;
	if (!ReadVar(m_seq, m_pos, delta))
		return false;
	m_wait += delta;
	return true;
}

void SeqPlayer::Tick()
{
	m_wait -= static_cast<double>(m_res) * m_bpm / 3600.0;
	int guard = 0;
	while (m_wait <= 0 && guard++ < 256)
		if (!NextEvent())
		{
			m_playing = false;
			for (Voice& v : m_voice)
				if (v.on)
					v.env.KeyOff();
			break;
		}
}

float SeqPlayer::Reverb(int side, float in)
{
	if (!m_rev_init)
	{
		const int comb_len[4] = {1557, 1617, 1491, 1422}, ap_len[2] = {556, 441};
		for (int sd = 0; sd < 2; sd++)
		{
			for (int k = 0; k < 4; k++)
				m_comb[sd][k].buf.assign(static_cast<size_t>(comb_len[k] * 48 / 44 + sd * 23), 0.0f);
			for (int k = 0; k < 2; k++)
				m_ap[sd][k].buf.assign(static_cast<size_t>(ap_len[k] * 48 / 44 + sd * 23), 0.0f);
		}
		m_rev_init = true;
	}
	float acc = 0;
	for (Comb& c : m_comb[side])
	{
		const float y = c.buf[c.i];
		c.last = y * (1.0f - c.damp) + c.last * c.damp;
		c.buf[c.i] = in + c.last * c.fb;
		c.i = (c.i + 1) % c.buf.size();
		acc += y;
	}
	for (AllPass& a : m_ap[side])
	{
		const float b = a.buf[a.i];
		a.buf[a.i] = acc + b * 0.5f;
		acc = b - acc;
		a.i = (a.i + 1) % a.buf.size();
	}
	return acc;
}

void SeqPlayer::Render(float* out, int frames)
{
	const int cmd = m_cmd.exchange(0);
	if (cmd == 1)
	{
		for (Voice& v : m_voice)
			v.on = false;
		for (int c = 0; c < 16; c++)
			m_chprog[c] = 0, m_chvol[c] = 100, m_chpan[c] = 64;
		// The channel records (+0x10, 16 bytes each): +1 channel, +2 program (0xFF unused), +3 volume, +4 pan.
		for (int r = 0; r < 16; r++)
		{
			const uint8_t* c = m_seq.data() + 0x10 + r * 16;
			const int ch = c[1] & 15;
			if (c[2] != 0xFF)
				m_chprog[ch] = c[2], m_chvol[ch] = c[3], m_chpan[ch] = c[4];
		}
		m_pos = 0x110, m_loop_pos = 0, m_loop_count = 0, m_wait = 0, m_tick_acc = 0, m_status = 0;
		NextEvent(); // the first event has no delta before it
		m_playing = true;
	}
	else if (cmd == 2)
	{
		m_playing = false;
		for (Voice& v : m_voice)
			if (v.on)
				v.env.KeyOff();
	}
	bool any = m_playing;
	for (const Voice& v : m_voice)
		any |= v.on;
	if (!any && m_rev_init && std::fabs(m_comb[0][0].last) < 1e-6f)
		return;
	for (int i = 0; i < frames; i++)
	{
		if (m_playing)
		{
			m_tick_acc += 1.0;
			if (m_tick_acc >= 800.0) // 48 kHz / 60 Hz
			{
				m_tick_acc -= 800.0;
				Tick();
			}
		}
		float l = 0, r = 0;
		for (Voice& v : m_voice)
		{
			if (!v.on)
				continue;
			const std::vector<int16_t>& pcm = v.s->pcm;
			size_t i0 = static_cast<size_t>(v.pos);
			if (i0 + 1 >= pcm.size())
			{
				if (v.s->loops && v.s->loop >= 0 && v.s->loop + 1 < static_cast<int>(pcm.size()))
				{
					v.pos = v.s->loop + (v.pos - static_cast<double>(pcm.size() - 1));
					i0 = static_cast<size_t>(v.pos);
				}
				else
				{
					v.on = false;
					continue;
				}
			}
			const float fr = static_cast<float>(v.pos - static_cast<double>(i0));
			const float smp = (pcm[i0] * (1.0f - fr) + pcm[std::min(i0 + 1, pcm.size() - 1)] * fr) / 32768.0f;
			const float e = v.env.Next();
			if (v.env.Done())
			{
				v.on = false;
				continue;
			}
			l += smp * e * v.gl;
			r += smp * e * v.gr;
			v.pos += v.step;
		}
		const float in = (l + r) * 0.015f;
		out[i * 2] += (l + Reverb(0, in)) * gain;
		out[i * 2 + 1] += (r + Reverb(1, in)) * gain;
	}
}
} // namespace s3
