// 3D World - Reflection Texture Creation
// by Frank Gennari
// 3/1/16

#include "physics_objects.h"
#include "draw_utils.h"
#include "model3d.h"
#include "shaders.h"

bool  const ENABLE_CUBE_MAP_MIPMAPS = 1;
unsigned const MAX_CUBE_MAP_TEX_SZ  = 768;
float const CUBE_MAP_ANISO          = 4.0;

bool enable_clip_plane_z(0);
unsigned reflection_tid(0);
float clip_plane_z(0.0);
vector3d pre_ref_cview_dir(plus_z);
point pre_ref_camera_pos(zero_vector);
reflect_plane_selector reflect_planes;
reflective_cobjs_t reflective_cobjs;

extern bool combined_gu, show_lightning, begin_motion, use_interior_cube_map_refl;
extern int display_mode, window_width, window_height, camera_coll_id;
extern float NEAR_CLIP, FAR_CLIP, water_plane_z, perspective_fovy;
extern coll_obj_group coll_objects;
extern vector<shadow_sphere> shadow_objs;


void setup_sun_moon_light_pos();
void do_look_at();
void draw_sun_moon_stars(bool no_update);
void draw_sun_flare(int ignore_cobj=-1, float intensity=1.0);
void draw_universe_bkg(bool reflection_mode=0, bool disable_asteroid_dust=0);


bool  enable_all_reflections () {return ((display_mode & 0x10) != 0);}
bool  enable_reflection_plane() {return (enable_all_reflections() && !reflect_planes.empty());}
bool  use_reflection_plane   () {return (enable_reflection_plane() && reflect_planes.enabled() && get_camera_pos().z > reflect_planes.get_selected().d[2][0]);}
float get_reflection_plane   () {return reflect_planes.get_refl_plane();}

bool use_reflect_plane_for_cobj(coll_obj const &c) {
	if (c.type != COLL_CUBE && (c.type != COLL_CYLINDER || (c.cp.surfs & 1))) return 0;
	if (!c.is_wet() && c.cp.spec_color.get_luminance() < 0.25) return 0;
	if (c.is_reflective()) return 0; // use cube map reflections instead
	cube_t const &bc(reflect_planes.get_selected());
	return (c.intersects(bc) && c.d[2][1] <= bc.d[2][1] && camera_pdu.cube_visible(c));
}

void proc_refl_bcube(cube_t const &c, cube_t &bcube, float &min_camera_dist, bool &bcube_set) {

	point const camera(get_camera_pos());
	float const dist(p2p_dist(camera, c.closest_pt(camera)));
		
	if (bcube_set) {
		bcube.union_with_cube(c);
		min_camera_dist = min(dist, min_camera_dist);
	}
	else {
		bcube = c;
		bcube_set = 1;
		min_camera_dist = dist;
	}
}

bool get_reflection_plane_bounds(cube_t &bcube, float &min_camera_dist) {

	reflect_planes.select_best_reflection_plane();
	if (!use_reflection_plane()) return 0;
	bool bcube_set(0);
	point const camera(get_camera_pos());

	for (cobj_id_set_t::const_iterator i = coll_objects.drawn_ids.begin(); i != coll_objects.drawn_ids.end(); ++i) {
		unsigned cix(*i);
		coll_obj const &c(coll_objects.get_cobj(cix));
		if (c.no_draw() || c.d[2][1] >= camera.z || !use_reflect_plane_for_cobj(c)) continue;
		cube_t cc(c);
		cc.d[2][0] = cc.d[2][1]; // shrink to top surface only
		proc_refl_bcube(cc, bcube, min_camera_dist, bcube_set);
	}
	cube_t const models_refl_bcube(get_all_models_bcube(1));
	
	if (!models_refl_bcube.is_zero_area() && models_refl_bcube.intersects(reflect_planes.get_selected())) {
		proc_refl_bcube(models_refl_bcube, bcube, min_camera_dist, bcube_set);
	}
	return bcube_set;
}


void apply_z_mirror(float zval) {

	fgMatrixMode(FG_MODELVIEW);
	fgPushMatrix();
	fgTranslate(0.0, 0.0, 2*zval); // translate to zval and back
	fgScale(1.0, 1.0, -1.0); // scale in z
	//mirror_about_plane(plus_z, point(0.0, 0.0, zval));
}

void setup_viewport_and_proj_matrix(unsigned xsize, unsigned ysize) {

	glViewport(0, 0, xsize, ysize);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	fgMatrixMode(FG_PROJECTION);
	fgPushMatrix();
	set_perspective(PERSP_ANGLE, 1.0);
	do_look_at();
}

void render_to_texture(unsigned tid, unsigned xsize, unsigned ysize) {

	bind_2d_texture(tid);
	glReadBuffer(GL_BACK);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, xsize, ysize); // glCopyTexSubImage2D copies the frame buffer to the bound texture
}

void render_to_texture_cube_map(unsigned tid, unsigned tex_size, unsigned face_ix) {

	assert(face_ix < 6);
	bind_cube_map_texture(tid);
	glReadBuffer(GL_BACK);
	glCopyTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face_ix, 0, 0, 0, 0, 0, tex_size, tex_size);
}

void restore_matrices_and_clear() {

	restore_prev_mvm_pjm_state();
	set_standard_viewport();
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}


struct face_draw_params_t {

	pos_dir_up pdu;
	unsigned face_id;
	vector<unsigned> &to_draw;

	face_draw_params_t(pos_dir_up const &pdu_, unsigned face_id_) : pdu(pdu_), face_id(face_id_), to_draw(coll_objects.get_draw_stream(face_id)) {}

	void draw_scene(unsigned tid, unsigned tex_size, int cobj_id, bool is_indoors) const {
		cview_dir  = pdu.dir;
		up_vector  = pdu.upv;
		camera_pdu = pdu;
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		//if (combined_gu) {draw_universe_bkg(1, 1);} // FIXME: infinite universe as background with no asteroid dust
		coll_objects.cur_draw_stream_id = face_id;
		vector3d const eye(pdu.pos - 0.001*cview_dir);
		fgLookAt(eye.x, eye.y, eye.z, pdu.pos.x, pdu.pos.y, pdu.pos.z, up_vector.x, up_vector.y, up_vector.z);
		setup_sun_moon_light_pos();
		bool const inc_mesh(cview_dir != plus_z), inc_grass_water(inc_mesh && !is_indoors);
		draw_scene_from_custom_frustum(camera_pdu, cobj_id, 2, inc_mesh, inc_grass_water, inc_grass_water); // reflection_pass=2 (cube map)
		render_to_texture_cube_map(tid, tex_size, face_id); // render reflection to texture
	}
};

bool check_occlusion(pos_dir_up const &pdu, cube_t const &cube) {
	return ((display_mode & 0x08) != 0 && have_occluders() && cube_cobj_occluded(pdu.pos, cube));
}
void create_cobj_draw_streams(vector<face_draw_params_t> const &faces) { // reflection_pass==2

	//timer_t t("create_draw_streams");
	if (faces.empty()) return;
	coll_objects.cur_draw_stream_id = faces.front().face_id; // first face
	coll_objects.set_cur_draw_stream_from_drawn_ids();
	vector<unsigned> &to_draw(coll_objects.get_cur_draw_stream());
	for (auto i = faces.begin()+1; i != faces.end(); ++i) {i->to_draw = to_draw;} // deep copy list of all drawable cobjs

#pragma omp parallel for schedule(static,64) num_threads(3)
	for (int i = 0; i < (int)to_draw.size(); ++i) {
		coll_obj const &c(coll_objects.get_cobj(to_draw[i]));
		assert(c.cp.draw);
		bool skip_all(c.no_draw());
		if (!skip_all && c.group_id >= 0) continue; // grouped cobjs can't be culled
		unsigned is_occluded(faces.size() == 6 ? check_occlusion(faces.front().pdu, c) : 2); // 2 = unknown; precompute if all 6 sides updated because one will be visible

		for (auto f = faces.begin(); f != faces.end(); ++f) {
			bool skip(0);
			if (skip_all || !c.check_pdu_visible(f->pdu)) {skip = 1;} // VFC
			else { // if we get this far and haven't determined visibility, do occlusion culling and cache the results
				if (is_occluded == 2) {is_occluded = check_occlusion(faces.front().pdu, c);}
				skip = skip_all = (is_occluded != 0);
			}
			if (skip) {f->to_draw[i] = TO_DRAW_SKIP_VAL;} // mark as skipped
		}
	}
}

unsigned create_reflection_cube_map(unsigned tid, unsigned tex_size, int cobj_id, point const &center,
	float near_plane, float far_plane, bool only_front_facing, bool is_indoors, unsigned skip_mask)
{
	//RESET_TIME;
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	check_gl_error(530);
	assert(tid);
	pre_ref_cview_dir  = cview_dir;
	pre_ref_camera_pos = camera_pdu.pos;
	pos_dir_up const prev_camera_pdu(camera_pdu);
	vector3d const prev_up_vector(up_vector);
	glViewport(0, 0, tex_size, tex_size);
	fgMatrixMode(FG_PROJECTION);
	fgPushMatrix();
	perspective_fovy = 90.0;
	set_perspective_near_far(near_plane, far_plane, 1.0); // AR = 1.0
	unsigned faces_drawn(0);
	vector<face_draw_params_t> faces;

	for (unsigned dim = 0; dim < 3; ++dim) {
		for (unsigned dir = 0; dir < 2; ++dir) {
			unsigned const dir_mask(EFLAGS[dim][dir]), face_id(2*dim + !dir);
			if ((skip_mask & dir_mask) != 0) {faces_drawn |= dir_mask; continue;} // mark it as drawn but skip it
			if (only_front_facing && ((prev_camera_pdu.pos[dim] > center[dim]) ^ dir)) continue; // back facing
			vector3d view_dir(zero_vector), upv(-plus_y);
			view_dir[dim] = (dir ? 1.0 : -1.0);
			if (dim == 1) {upv = (dir ? plus_z : -plus_z);} // Note: in OpenGL, the cube map top/bottom is in Y, and up dir is special in this dim
			float const angle(0.5*perspective_fovy*TO_RADIANS); // 90 degree FOV
			faces.push_back(face_draw_params_t(pos_dir_up(center, view_dir, upv, angle, near_plane, far_plane, 1.0, 1), face_id));
			faces_drawn |= dir_mask;
		} // for dir
	} // for dim
	create_cobj_draw_streams(faces);
	for (int i = 0; i < (int)faces.size(); ++i) {faces[i].draw_scene(tid, tex_size, cobj_id, is_indoors);}
	coll_objects.cur_draw_stream_id = 0; // restore to default value
	if (ENABLE_CUBE_MAP_MIPMAPS) {gen_mipmaps(6);}
	camera_pdu = prev_camera_pdu;
	up_vector  = prev_up_vector;
	cview_dir  = pre_ref_cview_dir;
	fgMatrixMode(FG_PROJECTION);
	fgPopMatrix();
	fgMatrixMode(FG_MODELVIEW);
	setup_viewport_and_proj_matrix(window_width, window_height); // restore
	update_shadow_matrices(); // restore
	setup_sun_moon_light_pos();
	check_gl_error(531);
	//PRINT_TIME("Create Reflection Cube Map");
	return faces_drawn;
}

unsigned create_reflection_cube_map(unsigned tid, unsigned tex_size, int cobj_id, cube_t const &cube, bool only_front_facing, bool is_indoors, unsigned skip_mask) {
	return create_reflection_cube_map(tid, tex_size, cobj_id, cube.get_cube_center(), max(NEAR_CLIP, 0.5f*cube.max_len()), FAR_CLIP, only_front_facing, is_indoors, skip_mask);
}

// render scene reflection to texture (ground mode and tiled terrain mode)
void create_gm_reflection_texture(unsigned tid, unsigned xsize, unsigned ysize, float zval, cube_t const &bcube, float min_camera_dist) {

	//RESET_TIME;
	// Note: we need to transform the camera frustum here, even though it's also done when drawing, because we need to get the correct projection matrix
	enable_clip_plane_z = 1;
	clip_plane_z        = zval + 1.0E-6; // hack to tell the shader setup code to use this z clip plane
	pre_ref_cview_dir   = cview_dir;
	pre_ref_camera_pos  = camera_pdu.pos;
	pos_dir_up const old_camera_pdu(camera_pdu);
	camera_pdu.apply_z_mirror(zval); // setup reflected camera frustum
	// FIXME: use x/y bcube bounds to clip reflected view frustum
	camera_pdu.near_ = max(camera_pdu.near_, min_camera_dist); // move near clip plane to closest edge of ref plane bcube (optimization)
	//if (combined_gu) {draw_universe_bkg(1, 1);} // FIXME: infinite universe as background with no asteroid dust
	pos_dir_up const refl_camera_pdu(camera_pdu);
	setup_viewport_and_proj_matrix(xsize, ysize);
	apply_z_mirror(zval); // setup mirror transform
	setup_sun_moon_light_pos();
	draw_scene_from_custom_frustum(refl_camera_pdu, camera_coll_id, 1, 0, 0, 0); // reflection_pass=1 (planar), no grass/mesh/water
	render_to_texture(tid, xsize, ysize); // render reflection to texture
	camera_pdu = old_camera_pdu;
	restore_matrices_and_clear(); // reset state
	update_shadow_matrices(); // restore
	setup_sun_moon_light_pos();
	enable_clip_plane_z = 0;
	//PRINT_TIME("Create Reflection Texture");
}

void create_tt_reflection_texture(unsigned tid, unsigned xsize, unsigned ysize, float terrain_zmin) {

	//RESET_TIME;
	pos_dir_up const old_camera_pdu(camera_pdu); // reflect camera frustum used for VFC
	camera_pdu.apply_z_mirror(water_plane_z); // setup reflected camera frustum
	pos_dir_up const refl_camera_pdu(camera_pdu);
	pre_draw_tiled_terrain(1);
	setup_viewport_and_proj_matrix(xsize, ysize);
	apply_z_mirror(water_plane_z); // setup mirror transform
	camera_pdu = refl_camera_pdu; // reset reflected PDU

	// draw partial scene
	if (!combined_gu) {
		draw_sun_moon_stars(1);
		draw_sun_flare(-1, 1.5);
	}
	draw_cloud_planes(terrain_zmin, 1, 1, 0); // slower but a nice effect
	
	if (get_camera_pos().z <= get_tt_cloud_level()) { // camera is below the clouds
		if (show_lightning) {draw_tiled_terrain_lightning(1);}
		draw_tiled_terrain(1);
		if (show_lightning) {end_tiled_terrain_lightning();}
		draw_tiled_terrain_clouds(1);
	}
	render_to_texture(tid, xsize, ysize); // render reflection to texture
	camera_pdu = old_camera_pdu; // restore camera_pdu
	restore_matrices_and_clear(); // reset state
	//PRINT_TIME("Create Reflection Texture");
}


// Note: reflection_tid is shared between tiled terrain mode and ground mode, since it's the same size and only one can be used at a time
void setup_reflection_texture(unsigned &tid, unsigned xsize, unsigned ysize) {

	static unsigned last_xsize(0), last_ysize(0);

	if (last_xsize != xsize || last_ysize != ysize) {
		free_texture(tid);
		last_xsize = xsize;
		last_ysize = ysize;
	}
	if (!tid) {
		bool const wrap = 0; // set to 1 for debugging
		setup_texture(tid, 0, wrap, wrap);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, xsize, ysize, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	}
	assert(glIsTexture(tid));
}

void setup_cube_map_reflection_texture(unsigned &tid, unsigned tex_size) {

	static unsigned last_size(0);

	if (last_size != tex_size) {
		free_texture(tid);
		last_size = tex_size;
	}
	if (!tid) {setup_cube_map_texture(tid, tex_size, 1, ENABLE_CUBE_MAP_MIPMAPS, CUBE_MAP_ANISO);} // allocate=1
	assert(glIsTexture(tid));
}

unsigned create_gm_z_reflection() {

	cube_t bcube;
	float min_camera_dist(0.0);
	if (!get_reflection_plane_bounds(bcube, min_camera_dist)) return 0; // no reflective surfaces
	float zval(get_reflection_plane());
	zval = max(bcube.d[2][0], min(bcube.d[2][1], zval)); // clamp to bounds of actual reflecting cobj top surfaces
	unsigned const xsize(window_width/2), ysize(window_height/2);
	setup_reflection_texture(reflection_tid, xsize, ysize);
	create_gm_reflection_texture(reflection_tid, xsize, ysize, zval, bcube, min_camera_dist);
	check_gl_error(999);
	return reflection_tid;
}

unsigned create_cube_map_reflection(unsigned &tid, unsigned &tsize, int cobj_id, point const &center, float near_plane, float far_plane, bool only_front_facing, bool is_indoors, unsigned skip_mask) {

	unsigned const max_tex_size(min(window_width, window_height));
	assert(max_tex_size > 0);
	tsize = 1;
	while (2*tsize <= max_tex_size) {tsize *= 2;} // find the max power of 2 <= max_tex_size
	tsize = min(tsize, MAX_CUBE_MAP_TEX_SZ); // clamp to 768 to limit runtime and memory usage
	setup_cube_map_reflection_texture(tid, tsize);
	unsigned const faces_drawn(create_reflection_cube_map(tid, tsize, cobj_id, center, near_plane, FAR_CLIP, only_front_facing, is_indoors, skip_mask));
	check_gl_error(998);
	return faces_drawn;
}

unsigned create_cube_map_reflection(unsigned &tid, unsigned &tsize, int cobj_id, cube_t const &cube, bool only_front_facing, bool is_indoors, unsigned skip_mask) {
	float const nclip(use_interior_cube_map_refl ? NEAR_CLIP : max(NEAR_CLIP, 0.5f*cube.max_len())); // slightly more than the cube half width in max dim
	return create_cube_map_reflection(tid, tsize, cobj_id, cube.get_cube_center(), nclip, FAR_CLIP, only_front_facing, is_indoors, skip_mask);
}

unsigned create_tt_reflection(float terrain_zmin) {

	if (do_zoom || (display_mode & 0x20)) return 0; // reflections not enabled
	unsigned const xsize(window_width/2), ysize(window_height/2);
	setup_reflection_texture(reflection_tid, xsize, ysize);
	create_tt_reflection_texture(reflection_tid, xsize, ysize, terrain_zmin);
	check_gl_error(999);
	return reflection_tid;
}


void setup_shader_cube_map_params(shader_t &shader, cube_t const &bcube, unsigned tid, unsigned tsize) {

	assert(tid > 0);
	shader.add_uniform_vector3d("cube_map_center", bcube.get_cube_center()); // world space
	shader.add_uniform_float("cube_map_near_clip", 0.5f*bcube.max_len());
	shader.add_uniform_int("cube_map_texture_size", tsize);
	bind_texture_tu(tid, 14, 1); // tu_id=14, is_cube_map=1
}


void reflect_plane_selector::select_best_reflection_plane() {

	if (empty()) return;
	point const camera(get_camera_pos());
	float best_dist(0.0);
	sel_cube = -1; // reset to invalid

	// find the closest plane below the player
	for (unsigned i = 0; i < bcubes.size(); ++i) {
		// Note: reflection planes should not overlap in z; if this holds, we can use either z1 or z2 for determining ordering
		cube_t const &c(bcubes[i]);
		float const zval(c.d[2][0]), dist(camera.z - zval);
		//cout << TXT(dist) << endl;
		if (dist <= 0.0) continue; // above the camera
		if (best_dist == 0.0 || dist < best_dist) {best_dist = dist; sel_cube = i;}
	}
	//cout << TXT(sel_cube) << endl;
}


void reflective_cobjs_t::add_cobj(unsigned cid) {
	cobjs.insert(make_pair(cid, map_val_t())); // insert with empty tid; okay if already exists
}

bool reflective_cobjs_t::remove_cobj(unsigned cid) { // okay if it doesn't exist/was already removed

	auto i(cobjs.find(cid));
	if (i == cobjs.end()) return 0; // doesn't exist
	free_texture(i->second.tid);
	cobjs.erase(i);
	return 1;
}

void reflective_cobjs_t::mark_faces_invalid() {
	for (auto i = cobjs.begin(); i != cobjs.end(); ++i) {i->second.faces_valid = 0;}
}

void reflective_cobjs_t::free_textures() {
	for (auto i = cobjs.begin(); i != cobjs.end(); ++i) {free_texture(i->second.tid); i->second.faces_valid = 0;}
}

bool enable_reflection_dynamic_updates() {
	// unclear how well this works, and if it even makes sense, considering the player casts shadows and reclections and is always visible in any reflective surface that is visible to the camera
	if (begin_motion == 0) return 0; // no dynamic objects enabled
	return (!shadow_objs.empty()); // check if there are any dynamic objects casting a shadow
}

void reflective_cobjs_t::create_textures() {

	if (!enable_all_reflections()) return;
	bool const dynamic_update(enable_reflection_dynamic_updates());
	vector<unsigned> to_remove;
	point const camera(get_camera_pos());

	for (auto i = cobjs.begin(); i != cobjs.end(); ++i) {
		unsigned &tid(i->second.tid);
		coll_obj const &cobj(coll_objects.get_cobj(i->first));
		if (cobj.disabled() || !cobj.is_reflective()) {to_remove.push_back(i->first); continue;} // no longer reflective - mark for removal
		cube_t const &bcube(cobj);
		bool const cobj_moved(i->second.bcube != bcube), no_update_needed(tid && !dynamic_update && !cobj_moved);
		if (no_update_needed && i->second.faces_valid == EF_ALL) continue; // reflection texture is valid, cobj has not moved, and scene has not changed
		if (tid && !cobj.is_cobj_visible())                      continue; // reflection texture is valid but cobj is not visible (approximate)

		if (cobj.type == COLL_CUBE) {
			int const sides((int)cobj.cp.surfs);
			bool visible(0);

			for (unsigned fi = 0; fi < 6; ++fi) { // Note: assumes the camera isn't inside the cube
				unsigned const dim(fi>>1), dir(fi&1);
				if (!(sides & EFLAGS[dim][dir]) && ((camera[dim] < cobj.d[dim][dir]) ^ dir)) {visible = 1; break;} // side is visible
			}
			if (!visible) continue;
			//skip_mask = cobj.cp.surfs; // incorrect - requires cube sides for the image border to capture perspective
		}
		// enable back face culling when texture is created or the cobj has moved, or on the final frame following a transition from dynamic updates (in case a new face comes into view)
		// also, semi transparent objects need back face refraction + reflection
		// also, curved objects (sphere, cylinder, torus, capsule) can reflect in back facing directions and need to have all faces updated
		bool const bfc(tid && !cobj_moved && !no_update_needed && !cobj.is_semi_trans() && cobj.has_hard_edges());
		unsigned skip_mask(0); // {z1, z2, y1, y2, x1, x2}
		i->second.faces_valid = create_cube_map_reflection(tid, i->second.tsize, i->first, bcube, bfc, cobj.is_indoors(), skip_mask);
		i->second.bcube       = bcube;
	} // for i
	for (auto i = to_remove.begin(); i != to_remove.end(); ++i) {remove_cobj(*i);}
}

unsigned reflective_cobjs_t::get_tid_for_cid(unsigned cid) const {
	auto i(cobjs.find(cid));
	assert(i != cobjs.end()); // too strong?
	return ((i == cobjs.end()) ? 0 : i->second.tid);
}
unsigned reflective_cobjs_t::get_tsize_for_cid(unsigned cid) const {
	auto i(cobjs.find(cid));
	assert(i != cobjs.end()); // too strong?
	return ((i == cobjs.end()) ? 0 : i->second.tsize);
}

