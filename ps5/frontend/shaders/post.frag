#version 450
// PS5 port frontend: FXAA over the rendered scene, into the display image.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

layout(push_constant) uniform Push
{
	vec4 texel;  // 1/width, 1/height, fade (0 black .. 1 full), unused
} pc;

layout(set = 1, binding = 0) uniform sampler2D u_scene;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main()
{
	// FXAA 3.11-style edge search, compact (the texel size comes from the push constants: the PS5
	// shader compiler refuses image queries).
	vec2 t = pc.texel.xy;
	vec3 rgbM = texture(u_scene, v_uv).rgb;
	float lumaM = luma(rgbM);
	float lumaNW = luma(texture(u_scene, v_uv + vec2(-1.0, -1.0) * t).rgb);
	float lumaNE = luma(texture(u_scene, v_uv + vec2(1.0, -1.0) * t).rgb);
	float lumaSW = luma(texture(u_scene, v_uv + vec2(-1.0, 1.0) * t).rgb);
	float lumaSE = luma(texture(u_scene, v_uv + vec2(1.0, 1.0) * t).rgb);
	float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
	float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
	vec3 result = rgbM;
	if (lumaMax - lumaMin > max(0.0312, lumaMax * 0.125))
	{
		vec2 dir;
		dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
		dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));
		float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.125), 1.0 / 128.0);
		float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
		dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * t;
		vec3 rgbA = 0.5 * (texture(u_scene, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +
		                   texture(u_scene, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
		vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(u_scene, v_uv + dir * -0.5).rgb +
		                                 texture(u_scene, v_uv + dir * 0.5).rgb);
		float lumaB = luma(rgbB);
		result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
	}
	// P2-19e: the soft focus behind the sheets' words (texel.w 0 sharp .. 1 soft): the scene holds only the field there (the
	// words are drawn after this pass), so it all goes softly out of focus, as on the PS2. Twelve taps on two rings, ~4 px at
	// 1080 lines at full strength; uniform per frame, so the branch costs nothing when off.
	if (pc.texel.w > 0.001)
	{
		float r = pc.texel.w * 4.0 * (1.0 / pc.texel.y) / 1080.0;
		vec3 acc = rgbM * 0.10;
		// shelf v3 (AI-assisted): a third ring, so the wider blur (texel.w up to 3: the PS2 menus' five passes) stays smooth.
		for (int i = 0; i < 6; i++)
		{
			float a = float(i) * 1.0472;
			vec2 o = vec2(cos(a), sin(a));
			acc += texture(u_scene, v_uv + o * r * t).rgb * 0.05;
			acc += texture(u_scene, v_uv + vec2(-o.y, o.x) * 0.5 * r * t).rgb * 0.055;
			acc += texture(u_scene, v_uv + vec2(o.x * 0.866 - o.y * 0.5, o.x * 0.5 + o.y * 0.866) * 0.75 * r * t).rgb * 0.045;
		}
		result = mix(result, acc, min(1.0, pc.texel.w * 1.5));
	}
	o_color = vec4(result * pc.texel.z, 1.0);
}
