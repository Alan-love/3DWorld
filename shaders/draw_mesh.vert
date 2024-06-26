uniform float snow_cov_amt = 0.0;

out vec4 epos;
out vec3 vpos, normal; // world space
out vec3 eye_norm;

void main()
{
	setup_texgen_st();
	eye_norm = fg_NormalMatrix * fg_Normal; // eye space, not normalized
	normal   = normalize(fg_Normal);
	vpos     = fg_Vertex.xyz;
	epos     = fg_ModelViewMatrix * fg_Vertex;
	gl_Position = fg_ProjectionMatrix * epos;
	fg_Color_vf = mix(fg_Color, vec4(1.0), snow_cov_amt);
} 
