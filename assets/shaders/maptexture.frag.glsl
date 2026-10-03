#version 330
// total basic standard texture drawing fragment shader

// the texture data
uniform sampler2D tex;

// interpolated texture coordinates received from vertex shader
in vec2 tex_position;

out vec4 out_col;

void main (void) {
	// this sets the fragment color to the corresponding texel.
	out_col = texture(tex, tex_position);
}
