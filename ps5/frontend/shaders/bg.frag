#version 450
#extension GL_GOOGLE_include_directive : require
// PS5 port frontend: the background. ps2-console-shelf (AI-assisted): the PS2 system software's ground, never a flat
// colour: moving black-to-navy fog (two layers of value noise drifting against each other, three octaves each), slow
// light motes, the Browser's lit grey field (lit from the top-left corner; P2-27: as the real one, still and smooth) mixed in by `fog_lo.w`, a
// soft white bloom behind the selection and an optional second, bluish one. Cheap on purpose (no texture reads, a few
// hashes a pixel): the shelf must not cost a game anything on the way out.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.glsl"

layout(push_constant) uniform Push
{
	vec4 glow_color;  // rgb, w = strength (the white bloom behind the selected object)
	vec4 glow_pos;    // x, y (0..1 of the screen), radius x, radius y (fractions of the height)
	vec4 glow2;       // x, y (0..1), radius (height units), w = strength (a bluish bloom: the disc)
	vec4 fog_lo;      // rgb: the fog's dark colour; w: how much of the Browser's lit grey field (0 fog .. 1 field)
	vec4 fog_hi;      // rgb: the fog's light colour; w: how much the fog gathers in the middle (0 even .. 1 centre)
	vec4 motes;       // rgb: the motes' colour; w: their strength (0 none)
	vec4 misc;        // x = aspect (w/h), y = top-left corner light, z = vignette, w = time (s)
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float hash12(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float vnoise(vec2 p)
{
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	float a = hash12(i), b = hash12(i + vec2(1.0, 0.0)), c = hash12(i + vec2(0.0, 1.0)), d = hash12(i + vec2(1.0, 1.0));
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p)
{
	return (vnoise(p) + 0.5 * vnoise(p * 2.03 + 17.1) + 0.25 * vnoise(p * 4.01 + 31.7)) / 1.75;
}

// One layer of motes: a grid of cells, one mote a cell drifting inside it (kept off the cell's edge, so no neighbour
// lookups), twinkling at its own pace; about one cell in five holds one.
float motes(vec2 p, float t, float cells, float seed)
{
	vec2 g = p * cells;
	vec2 id = floor(g);
	vec2 f = fract(g);
	float h = hash12(id + seed);
	float h2 = hash12(id + seed + 7.3);
	vec2 c = vec2(0.5) + 0.32 * vec2(sin(t * (0.07 + 0.08 * h) + h * 6.28), cos(t * (0.05 + 0.07 * h2) + h2 * 6.28));
	vec2 q = f - c;
	float size = 0.03 + 0.045 * h2;
	float tw = 0.35 + 0.65 * abs(sin(t * (0.6 + 0.9 * h) + h2 * 6.28));
	return step(0.78, h) * tw * exp(-dot(q, q) / (size * size));
}

void main()
{
	vec2 uv = v_uv;
	float t = pc.misc.w;
	vec2 p = (uv - 0.5) * vec2(pc.misc.x, 1.0);

	// The fog: two drifting layers gathering towards the middle.
	vec2 drift = vec2(sin(t * 0.05), cos(t * 0.04)) * 0.35;
	float n1 = fbm(p * 2.2 + drift + vec2(t * 0.012, 0.0));
	float n2 = fbm(p * 3.1 - drift * 1.3 + vec2(0.0, t * 0.009) + 9.0);
	float n = n1 * 0.75 + n2 * 0.45;
	float rad = max(0.0, 1.0 - length(p) * 1.15);
	float fog = pow(max(0.0, n * 1.25 - 0.28), 1.5) * mix(1.0, rad * 1.6, pc.fog_hi.w);
	// The configuration field's light from the top-left corner.
	float corner = max(0.0, 1.0 - length(uv * vec2(1.0, 1.1)) * 0.95);
	fog += corner * corner * pc.misc.y;
	vec3 col = mix(pc.fog_lo.rgb, pc.fog_hi.rgb, clamp(fog, 0.0, 1.0));

	// P2-27: the Browser's field as the real PS2's (yt 1G4yRe288Os, fitted to the frame to within 1/255 rms): one still, smooth light
	// falling from the top-left, a quadratic in the screen's x and y; no texture, motes or lines. Neutral grey where lit, a little olive
	// (blue ~7/255 down) towards the dark far corner. fog_lo.r: 0 the Browser itself, 1 a page in it (the Library: ~25/255 darker).
	vec2 q = uv;
	vec3 kb = vec3(149.4, -132.4, -68.0), kp = vec3(124.8, -137.0, -66.0);
	vec3 k2b = vec3(21.3, 6.0, 27.1), k2p = vec3(32.0, 7.0, 33.0);
	float page = clamp(pc.fog_lo.r, 0.0, 1.0);
	vec3 k = mix(kb, kp, page), k2 = mix(k2b, k2p, page);
	float lv = max(k.x + k.y * q.x + k.z * q.y + k2.x * q.x * q.x + k2.y * q.y * q.y + k2.z * q.x * q.y, 2.0) / 255.0;
	vec3 field = vec3(lv, lv, max(0.0, lv - 0.028 * (1.0 - smoothstep(0.08, 0.45, lv))));
	col = mix(col, field, pc.fog_lo.w);

	// P2-19: motes.w < 0: System Configuration's marbled indigo instead of motes: a still, fine mottle with darker veins, as
	// the real PS2's ground (two more noise lookups a pixel, three octaves each).
	if (pc.motes.w < 0.0)
	{
		float m1 = fbm(p * 8.0 + 2.7), m2 = fbm(p * 19.0 - 5.1);
		float vein = 1.0 - smoothstep(0.0, 0.10, abs(m1 - 0.5));
		col *= mix(1.0, 0.80 + 0.36 * m1 + 0.24 * (m2 - 0.5) - 0.10 * vein, -pc.motes.w);
	}
	else
	{
		// The motes: a fine layer and a coarse one, drifting slowly.
		float m = motes(p + vec2(t * 0.006, -t * 0.004), t, 9.0, 3.0) + 0.7 * motes(p + vec2(-t * 0.004, t * 0.003), t, 5.0, 11.0);
		col += pc.motes.rgb * m * pc.motes.w;
	}

	// The bloom behind the selection: a bright core and a wide skirt.
	vec2 d = (uv - pc.glow_pos.xy) * vec2(pc.misc.x, 1.0);
	float r = length(d / max(pc.glow_pos.zw, vec2(1e-3)));
	col += pc.glow_color.rgb * (exp(-r * r * 2.6) * 0.9 + exp(-r * 1.8) * 0.22) * pc.glow_color.w;
	// The second bloom.
	vec2 d2 = (uv - pc.glow2.xy) * vec2(pc.misc.x, 1.0);
	float r2 = length(d2) / max(pc.glow2.z, 1e-3);
	col += vec3(0.45, 0.70, 1.0) * (exp(-r2 * r2 * 2.0) * 0.8 + exp(-r2 * 1.6) * 0.2) * pc.glow2.w;

	// Vignette.
	col *= 1.0 - pc.misc.z * smoothstep(0.35, 1.25, length(p));

	col += fe_dither(gl_FragCoord.xy) / 255.0;
	o_color = vec4(max(col, vec3(0.0)), 1.0);
}
