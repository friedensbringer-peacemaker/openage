#version 330

// XR fork: textured quad of the game user interface (renderer/stages/ui),
// placed by rect (NDC: left, bottom, right, top)

layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;

uniform vec4 rect;

out vec2 tex_pos;

void main() {
	vec2 t = (position + 1.0) * 0.5;
	gl_Position = vec4(mix(rect.xy, rect.zw, t), 0.0, 1.0);
	// the CPU canvas stores row 0 at the top
	tex_pos = vec2(uv.x, 1.0 - uv.y);
}
