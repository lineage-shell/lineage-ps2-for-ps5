#version 450
// PS5 port frontend: signed-distance-field glyphs (params.w = 0: x = edge, y = softness in field
// units) and rounded rectangles (params.w = 1: uv = pixels from the centre, x/y = half size,
// z = corner radius, softness one pixel).
// ps2-console-shelf (AI-assisted) adds three shapes for the PS2 look:
//   params.w = 3: a soft glow: uv = -1..1 across the ellipse, x = falloff (bigger is tighter).
//   params.w = 4: a line or a fill: uv.y = pixels across it from its middle, x = half width (core), y = glow width in
//                 pixels, z = glow strength. A fill is a "line" with uv.y = 0 everywhere.
//   params.w = 5: a PS2 disc: uv = -1..1 across it, x = its turn (radians), y = how much of its blue backing shows.
//   params.w = 7 (P2-19d): frosted glass: the colour with a fine grain fixed to the surface (uv = grain cells across it) and a
//                 softer mottle; x = the grain's strength.
//   params.w = 6 (P2-17): a disc with a picture bound in the atlas' place: uv = the picture's coordinates, x/y = the picture's units per
//                 disc radius (the disc is (uv - 0.5) / xy), z = 1: the picture is the disc's own art (its alpha kept), 0: it is the
//                 game's front, printed as the label of a silver disc around the clear hub and its hole.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

layout(set = 1, binding = 0) uniform sampler2D u_atlas;
layout(set = 1, binding = 1) uniform sampler2D u_second; // shelf v3: mode 14's mask (the set's second texture)

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec4 v_params;
layout(location = 0) out vec4 o_color;

void main()
{
	// Sampled outside the branch on purpose: the PS5 shader compiler (psbc) moves the coordinate of
	// a sample inside divergent control flow to the top and builds it from the input's first
	// component twice, so the atlas was read at (u, u) and every glyph came out as bars (vk-285-41).
	// shelf v3 (AI-assisted): params.w >= 16 draws additively (the PS2's Cs*As + Cd: the colour premultiplied, alpha 0), mode - 16.
	// Modes 9 / 10 / 11: the PS2 GS's texture modulate (texel x colour x 2, as 0x80 = 1.0), clamped / repeating uv, and a flat colour x 2.
	float raw_mode = v_params.w;
	bool additive = raw_mode > 15.5;
	float mode = additive ? raw_mode - 16.0 : raw_mode;
	// Mode 12 / 13 (shelf v3): as 10 / 9, but the texel's alpha is ignored (the GS with TCC off: the opening's 16-bit fog, wall and
	// reflection textures carry no alpha).
	vec2 suv = ((mode > 9.5 && mode < 10.5) || (mode > 11.5 && mode < 12.5) || (mode > 14.5 && mode < 15.5)) ? fract(v_uv) : v_uv;
	vec4 sample_color = texture(u_atlas, suv);
	// Mode 14 (shelf v3): as 13, times the second texture's alpha at params.xy: the opening's reflection weighted by the destination alpha
	// its TEXOBLP / TEXOBLPR pass wrote (GS (Cs-0)*Ad + Cd). Sampled here, outside the branches, as the atlas is.
	float second_a = texture(u_second, v_params.xy).a;
	float field = sample_color.r;
	float a;
	vec3 rgb = v_color.rgb;
	if (mode < 0.5)
	{
		a = smoothstep(v_params.x - v_params.y, v_params.x + v_params.y, field);
	}
	else if (mode < 1.5)
	{
		vec2 q = abs(v_uv) - v_params.xy + vec2(v_params.z);
		float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_params.z;
		a = 1.0 - smoothstep(-0.75, 0.75, d);
	}
	else if (mode < 2.5)
	{
		a = 1.0; // (the badge: its own colour below)
	}
	else if (mode < 3.5)
	{
		float r2 = dot(v_uv, v_uv);
		a = exp(-r2 * v_params.x) * (1.0 - smoothstep(0.82, 1.0, r2));
	}
	else if (mode < 4.5)
	{
		float d = abs(v_uv.y);
		float core = 1.0 - smoothstep(v_params.x - 0.7, v_params.x + 0.7, d);
		float g = max(d - v_params.x, 0.0) / max(v_params.y, 0.01);
		a = min(1.0, core + v_params.z * exp(-g * g));
	}
	else if (mode > 14.5)
	{
		// Mode 15 (shelf v3): the GS's integer path for an additive FIX layer (the opening's fog, boot.md 2.7): Cs = (T x V) >> 7 per
		// channel (MODULATE, texel alpha ignored, repeating uv), then (Cs x FIX) >> 7 added to the frame (params.x = FIX, draw it
		// additively): each layer adds whole levels, as on the console.
		vec3 tv = floor(sample_color.rgb * 255.0 + 0.5), vv = floor(v_color.rgb * 255.0 + 0.5);
		vec3 cs = min(floor(tv * vv / 128.0), vec3(255.0));
		rgb = floor(cs * v_params.x / 128.0) / 255.0;
		a = 1.0;
	}
	else if (mode > 8.5)
	{
		vec4 tc = (mode > 10.5 && mode < 11.5) ? vec4(1.0) : (mode > 11.5 ? vec4(sample_color.rgb, 1.0) : sample_color);
		vec4 c = min(tc * v_color * 2.0, vec4(1.0));
		rgb = c.rgb * (mode > 13.5 ? second_a : 1.0);
		a = c.a;
	}
	else if (mode > 6.5)
	{
		vec2 cell = floor(v_uv);
		vec3 h3 = fract(vec3(cell.xyx) * 0.1031);
		h3 += dot(h3, h3.yzx + 33.33);
		float grain = fract((h3.x + h3.y) * h3.z);
		vec2 m = v_uv * 0.17, mi = floor(m), mf = fract(m);
		mf = mf * mf * (3.0 - 2.0 * mf);
		vec4 c4 = vec4(mi, mi + 1.0);
		vec4 hx = fract(vec4(c4.x, c4.z, c4.x, c4.z) * 0.1031 + vec4(c4.y, c4.y, c4.w, c4.w) * 0.1123);
		hx = fract(hx * (hx + 33.33) * 1.37);
		float mott = mix(mix(hx.x, hx.y, mf.x), mix(hx.z, hx.w, mf.x), mf.y);
		rgb = v_color.rgb * (1.0 + v_params.x * ((grain - 0.5) * 0.9 + (mott - 0.5) * 0.7));
		a = 1.0;
	}
	else if (mode > 5.5)
	{
		vec2 d = (v_uv - 0.5) / max(v_params.xy, vec2(0.001));
		float r = length(d);
		float ang = atan(d.y, d.x);
		float edge = 1.0 - smoothstep(0.985, 1.0, r);
		float hole = smoothstep(0.075, 0.09, r);
		float hub = 1.0 - smoothstep(0.30, 0.31, r);
		float ring = smoothstep(0.30, 0.31, r) * (1.0 - smoothstep(0.34, 0.35, r));
		float label = smoothstep(0.355, 0.365, r) * (1.0 - smoothstep(0.955, 0.965, r));
		float glint = pow(max(0.0, cos(ang - 0.6)), 14.0) + 0.7 * pow(max(0.0, cos(ang + 2.54)), 14.0);
		vec3 silver = vec3(0.74, 0.76, 0.80) + glint * vec3(0.24, 0.24, 0.22);
		vec3 printed = sample_color.rgb * 0.94 + glint * 0.08;
		vec3 hubc = vec3(0.55, 0.58, 0.64) * (0.6 + 0.4 * r / 0.3);
		vec3 disc = mix(mix(mix(silver, printed, label), vec3(0.80, 0.83, 0.90), ring), hubc, hub);
		float art = step(0.5, v_params.z);
		rgb = mix(disc, sample_color.rgb, art) * v_color.rgb;
		a = mix(edge * hole * mix(1.0, 0.55, hub * (1.0 - ring)), sample_color.a, art);
	}
	else
	{
		// The disc: the clear hub and its hole, the silver ring, the data area tinted by the blue backing, two glints
		// that turn with it and a faint rainbow.
		float r = length(v_uv);
		float ang = atan(v_uv.y, v_uv.x) - v_params.x;
		float edge = 1.0 - smoothstep(0.985, 1.0, r);
		float hole = smoothstep(0.075, 0.09, r);
		float hub = 1.0 - smoothstep(0.30, 0.31, r);
		float ring = smoothstep(0.30, 0.31, r) * (1.0 - smoothstep(0.34, 0.35, r));
		vec3 data = mix(vec3(0.70, 0.72, 0.78), vec3(0.16, 0.20, 0.62) + 0.10 * vec3(sin(ang * 2.0), sin(ang * 2.0 + 2.1), sin(ang * 2.0 + 4.2)),
			v_params.y);
		float glint = pow(max(0.0, cos(ang)), 14.0) + 0.7 * pow(max(0.0, cos(ang + 3.14159)), 14.0);
		data += glint * vec3(0.55, 0.6, 0.7) * (0.5 + 0.5 * r);
		vec3 hubc = vec3(0.55, 0.58, 0.64) * (0.6 + 0.4 * r / 0.3);
		rgb = mix(mix(data, vec3(0.80, 0.83, 0.90), ring), hubc, hub) * v_color.rgb;
		a = edge * hole * mix(1.0, 0.55, hub * (1.0 - ring));
	}
	// Mode 2 draws RGBA achievement badges using the same vertex pipeline (AI-assisted).
	vec4 o = (mode > 1.5 && mode < 2.5) ? sample_color * v_color : vec4(rgb, (mode > 8.5 ? 1.0 : v_color.a) * a);
	// shelf v3: the UI pipeline blends premultiplied (ONE, ONE_MINUS_SRC_ALPHA): the same as before for every mode, and additive with alpha 0.
	o_color = additive ? vec4(o.rgb * o.a, 0.0) : vec4(o.rgb * o.a, o.a);
}
