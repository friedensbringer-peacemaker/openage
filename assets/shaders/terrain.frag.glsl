#version 330

in vec2 tex_pos;
in vec3 world_pos;

layout(location=0) out vec4 out_col;

uniform sampler2D tex;

// XR fork (vision): fog of war, one texel per tile (red: 0 unexplored,
// 0.75 explored, 1 visible), u = ne / ne tiles, v = se / se tiles
uniform sampler2D fog;
// (1 / ne tiles, 1 / se tiles, 1 = fog on)
uniform vec3 fog_params;

// fog value at a tile position (ne, se); clamped to the centres of the border
// texels, so the bilinear filter never wraps around the map
float fog_at(vec2 tile) {
	vec2 t = clamp(tile * fog_params.xy, 0.5 * fog_params.xy, vec2(1.0) - 0.5 * fog_params.xy);
	return texture(fog, t).r;
}

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

    // XR fork (vision): world x = se, world z = -ne. Four bilinear taps half a
    // tile around the point blend the tile states over about two tiles (soft
    // edges); visible = 1 (unchanged), explored = 0.75, unexplored = black.
    if (fog_params.z > 0.5) {
        vec2 tile = vec2(-world_pos.z, world_pos.x);
        float f = 0.25 * (fog_at(tile + vec2(-0.5, -0.5)) + fog_at(tile + vec2(0.5, -0.5))
                          + fog_at(tile + vec2(-0.5, 0.5)) + fog_at(tile + vec2(0.5, 0.5)));
        shade *= f;
    }
    out_col = vec4(tex_val.rgb * shade, tex_val.a);
}
