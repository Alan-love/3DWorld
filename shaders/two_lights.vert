uniform vec4 emission = vec4(0,0,0,1);

void main()
{
	gl_Position = fg_ftransform();
	vec3 normal = normalize(fg_NormalMatrix * fg_Normal); // eye space
	vec4 color  = emission;
	if (enable_light0) color += add_light_comp0(normal);
	if (enable_light1) color += add_light_comp1(normal);
	fg_Color_vf = color;
} 
