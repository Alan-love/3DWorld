uniform sampler2D tex0;
uniform float opacity      = 1.0;
uniform float tex_scale_s  = 1.0;
uniform float tex_scale_t  = 1.0;
uniform vec3 ambient_color = vec3(0.0);
uniform vec4 color_scale   = vec4(1.0);

in vec4 epos;
in vec3 normal; // eye space
in vec2 tc;

void main() {
#ifdef ENABLE_OPACITY
	check_noise_and_maybe_discard((1.0 - opacity), 1.0); // inverted value
#endif
	vec4 texel   = texture(tex0, (tc * vec2(tex_scale_s, tex_scale_t)));
	vec3 color   = do_shadowed_lighting(vec4(0.0), epos, normalize(normal), gl_Color, 1.0, 1.0);
	fg_FragColor = apply_fog_epos(texel*vec4(color*color_scale.rgb, 1.0), epos);
}
