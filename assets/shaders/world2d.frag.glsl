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
		col = player_color(u_player);
	}
	else if (alpha == 252) {
		col = vec4(0.0f, 1.0f, 0.0f, 1.0f);
	}
	else if (alpha == 250) {
		col = vec4(0.0f, 0.0f, 1.0f, 1.0f);
	}
	else {
		col = tex_val;
	}
	id = u_id;
}
