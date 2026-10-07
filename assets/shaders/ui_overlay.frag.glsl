#version 330

// XR fork: game user interface canvas (straight alpha, blended by the pass)

in vec2 tex_pos;

out vec4 out_col;

uniform sampler2D tex;

void main() {
	out_col = texture(tex, tex_pos);
}
