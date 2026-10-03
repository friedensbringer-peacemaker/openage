#version 330

in vec2 tex_pos;
in vec3 world_pos;

layout(location=0) out vec4 out_col;

uniform sampler2D tex;

void main()
{
    vec4 tex_val = texture(tex, tex_pos);

    // XR fork: slope shading for hills (the terrain has no lighting otherwise).
    // Face normal from the screen-space derivatives of the world position:
    // constant per mesh triangle. Light from the top left of the screen
    // (world -x). On flat terrain dFdx/dFdy have no y component, so the
    // normal is exactly (0, +-1, 0) and the colour stays unchanged.
    vec3 face = cross(dFdx(world_pos), dFdy(world_pos));
    float len = length(face);
    float shade = 1.0;
    if (len > 0.0) {
        vec3 n = face / len;
        if (n.y < 0.0) {
            n = -n;
        }
        shade = 1.0 - 0.8 * n.x;
    }
    out_col = vec4(tex_val.rgb * shade, tex_val.a);
}
