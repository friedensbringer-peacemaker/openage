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
	switch (player % 8u) {
		case 0u:
			return vec4(0.0f, 0.3f, 1.0f, 1.0f);
		case 1u:
			return vec4(1.0f, 0.0f, 0.0f, 1.0f);
		case 2u:
			return vec4(0.0f, 1.0f, 0.0f, 1.0f);
		case 3u:
			return vec4(1.0f, 1.0f, 0.0f, 1.0f);
		case 4u:
			return vec4(0.0f, 1.0f, 1.0f, 1.0f);
		case 5u:
			return vec4(1.0f, 0.0f, 1.0f, 1.0f);
		case 6u:
			return vec4(0.6f, 0.6f, 0.6f, 1.0f);
		default:
			return vec4(1.0f, 0.5f, 0.0f, 1.0f);
	}
}

void main() {
	// computed here, GLSL ES only allows constant global initializers
	vec2 uv = vec2(
		vert_uv.x * tile_params.z + tile_params.x,
		vert_uv.y * tile_params.w + tile_params.y
	);

	vec4 tex_val = texture(tex, uv);
	int alpha = int(round(tex_val.a * 255.0));
	switch (alpha) {
		case 0:
			col = tex_val;
			discard;

			// do not save the ID
			return;
		case 254:
			col = player_color(u_player);
			break;
		case 252:
			col = vec4(0.0f, 1.0f, 0.0f, 1.0f);
			break;
		case 250:
			col = vec4(0.0f, 0.0f, 1.0f, 1.0f);
			break;
		default:
			col = tex_val;
			break;
	}
	id = u_id;
}
