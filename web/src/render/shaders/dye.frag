#version 300 es
precision highp float;

// Continuum dye fragment shader.
//
//   * Bilinear sampling on the dye texture (handled by GL_LINEAR).
//   * Cheap 5×5 box-blur bloom on the bright tail of the colour.
//   * Reinhard tone-map so we never blow out highlights.
//   * Optional vorticity overlay tinting.

uniform sampler2D u_dye;
uniform vec2  u_resolution;     // texture resolution in pixels
uniform float u_bloom;          // 0..2
uniform float u_exposure;       // 0..2 — overall brightness
uniform int   u_show_vorticity; // 0/1

in  vec2 v_uv;
out vec4 fragColor;

void main() {
    vec3 dye = texture(u_dye, v_uv).rgb * u_exposure;

    if (u_bloom > 0.0) {
        vec2 texel = 1.0 / u_resolution;
        vec3 bloom = vec3(0.0);
        const int R = 2;
        for (int dy = -R; dy <= R; dy++) {
            for (int dx = -R; dx <= R; dx++) {
                vec3 s = texture(u_dye, v_uv + vec2(float(dx), float(dy)) * texel * 2.0).rgb;
                bloom += max(s - 0.45, vec3(0.0));
            }
        }
        bloom /= float((2 * R + 1) * (2 * R + 1));
        dye += bloom * u_bloom;
    }

    if (u_show_vorticity == 1) {
        // Pseudo-curl from a Sobel of luminance — purely cosmetic.
        vec2 texel = 1.0 / u_resolution;
        float l = dot(texture(u_dye, v_uv + vec2( texel.x, 0.0)).rgb, vec3(0.299,0.587,0.114))
                - dot(texture(u_dye, v_uv + vec2(-texel.x, 0.0)).rgb, vec3(0.299,0.587,0.114));
        float k = dot(texture(u_dye, v_uv + vec2(0.0,  texel.y)).rgb, vec3(0.299,0.587,0.114))
                - dot(texture(u_dye, v_uv + vec2(0.0, -texel.y)).rgb, vec3(0.299,0.587,0.114));
        float curl = abs(l) + abs(k);
        dye += vec3(curl * 0.6, curl * 0.3, curl * 1.4);
    }

    // Reinhard tone-map.
    dye = dye / (dye + vec3(1.0));
    fragColor = vec4(dye, 1.0);
}
