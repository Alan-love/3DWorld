uniform sampler2D frame_buffer_tex;
uniform vec2 xy_step;
uniform int dim_val = 0;

const int BLOOM_RADIUS = 8;

in vec2 tc;

void main() {
	vec3 base_color  = vec3(0.0);
	vec3 bloom_color = vec3(0.0);
	float falloff    = 0.5;
	float min_val    = 0.75;
	float tot_w      = 0.0;
	float mult       = 1.0/(1.0 - min_val);

	for (int v = -BLOOM_RADIUS; v <= BLOOM_RADIUS; ++v) {
		float weight = exp(-falloff*abs(v)); // Gaussian - Note: could use a lookup table, but doesn't make much difference
		vec2 pos     = tc + vec2(v*(1.0 - dim_val), v*dim_val)*xy_step;
		vec3 color   = weight*texture(frame_buffer_tex, pos).rgb;
		if (v == 0) {base_color = color;}
		//float val    = mult*(color.b - min_val); // blue, for sky
		float val    = mult*(max(max(color.r, color.g), color.b) - min_val); // max across all colors
		bloom_color += weight*color*clamp(val, 0.0, 1.0);
		tot_w       += weight;
	}
	bloom_color *= 0.5/tot_w;
	fg_FragColor = vec4(min(vec3(1.0), (base_color + bloom_color)), 1.0);
}
