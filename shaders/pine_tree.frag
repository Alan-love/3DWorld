uniform sampler2D branch_tex;
uniform float min_alpha = 0.0;
uniform float min_noise = 0.0;
uniform float max_noise = 1.0;
uniform vec4 color_scale= vec4(1.0);

in float world_space_zval;
in vec2 tc;

void main() {
	vec4 texel = texture(branch_tex, tc);
	if (texel.a <= min_alpha) discard;
#ifndef NO_NOISE
	check_noise_and_maybe_discard(min_noise, max_noise);
#endif
	fg_FragColor = apply_fog_scaled(vec4(texel.rgb*gl_Color.rgb*color_scale.rgb, 1.0), world_space_zval);
}
