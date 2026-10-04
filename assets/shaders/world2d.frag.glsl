#version 330

in vec2 vert_uv;

layout(location=0) out vec4 col;
layout(location=1) out uint id;

uniform sampler2D tex;
uniform uint u_id;
// owning player (XR fork): color of the player color pixels
uniform uint u_player;

// position (top left corner) and size: (x, y, width, height)
uniform vec4 tile_params;

// player colors of the classic games: blue, red, green, yellow, cyan, purple, gray, orange
vec4 player_color(uint player) {
	uint p = player % 8u;
	if (p == 0u) return vec4(0.0f, 0.3f, 1.0f, 1.0f);
	if (p == 1u) return vec4(1.0f, 0.0f, 0.0f, 1.0f);
	if (p == 2u) return vec4(0.0f, 1.0f, 0.0f, 1.0f);
	if (p == 3u) return vec4(1.0f, 1.0f, 0.0f, 1.0f);
	if (p == 4u) return vec4(0.0f, 1.0f, 1.0f, 1.0f);
	if (p == 5u) return vec4(1.0f, 0.0f, 1.0f, 1.0f);
	if (p == 6u) return vec4(0.6f, 0.6f, 0.6f, 1.0f);
	return vec4(1.0f, 0.5f, 0.0f, 1.0f);
}

void main() {
	// computed here, GLSL ES only allows constant global initializers
	vec2 uv = vec2(
		vert_uv.x * tile_params.z + tile_params.x,
		vert_uv.y * tile_params.w + tile_params.y
	);

	vec4 tex_val = texture(tex, uv);
	int alpha = int(round(tex_val.a * 255.0));
	// if/else instead of switch: some GL drivers (Apple, upstream PR #1817)
	// crash compiling switch statements in this shader
	if (alpha == 0) {
		col = tex_val;
		discard;
	}
	else if (alpha == 254) {
		// XR fork: the converter stores the shade (0 dark .. 7 light) of the
		// player color pixel in green; a flat color made e.g. the roof trims of
		// the town centre look like a painted edge
		float shade = clamp(round(tex_val.g * 255.0) / 7.0, 0.0, 1.0);
		vec3 base = player_color(u_player).rgb;
		vec3 shaded = shade < 0.57 ? base * (0.35 + 1.14 * shade)
		                           : mix(base, vec3(1.0), (shade - 0.57) * 1.6);
		col = vec4(shaded, 1.0);
	}
	else if (alpha == 252 || alpha == 250) {
		// XR fork: outline pixels of the converter (252 outline, 250 special
		// outline) were debug colors (green, blue) around every sprite. The
		// classic games only show outlines of units hidden behind buildings,
		// which needs an occlusion pass; until then they are not drawn.
		col = tex_val;
		discard;
	}
	else {
		col = tex_val;
	}
	id = u_id;
}
