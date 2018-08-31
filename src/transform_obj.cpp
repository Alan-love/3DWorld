// 3D World - OpenGL CS184 Computer Graphics Project
// by Frank Gennari
// 3/10/02
#include "3DWorld.h"
#include "mesh.h"
#include "transform_obj.h"
#include "physics_objects.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>


extern float base_gravity, tstep, fticks;
extern obj_type object_types[];


// *** xform_matrix ***


float       *xform_matrix::get_ptr()       {glm::mat4       &m(*this); return glm::value_ptr(m);}
float const *xform_matrix::get_ptr() const {glm::mat4 const &m(*this); return glm::value_ptr(m);}


void xform_matrix::get_as_doubles(double md[16]) const {
	float const *const ptr(get_ptr());
	for (unsigned i = 0; i < 16; ++i) {md[i] = ptr[i];}
}


void xform_matrix::normalize() {

	float *m(get_ptr());

	for (unsigned i = 0; i < 3; ++i) { // renormalize matrix to account for fp error
		float const dist(sqrt(m[i+0]*m[i+0] + m[i+4]*m[i+4] + m[i+8]*m[i+8]));
		m[i+0] /= dist;
		m[i+4] /= dist;
		m[i+8] /= dist;
	}
}

// Note: ignores w component
void xform_matrix::apply_to_vector3d(vector3d &v) const {
	glm::vec4 const vx(*this * (glm::vec4(vec3_from_vector3d(v), 1.0)));
	v = vector3d_from_vec3(glm::vec3(vx));
}


matrix_stack_t mvm_stack; // modelview matrix
matrix_stack_t pjm_stack; // projection matrix
int matrix_mode(0); // 0 = MVM, 1 = PJM
bool mvm_changed(0);

matrix_stack_t &get_matrix_stack() {return ((matrix_mode == FG_PROJECTION) ? pjm_stack : mvm_stack);}

void mark_matrix_changed() {
	if (matrix_mode == FG_MODELVIEW) {mvm_changed = 1;}
}

void fgMatrixMode(int val) {
	assert(val == FG_PROJECTION || val == FG_MODELVIEW);
	matrix_mode = val;
}

void fgPushMatrix  () {get_matrix_stack().push();} // matrix not changed
void fgPopMatrix   () {get_matrix_stack().pop();      mark_matrix_changed();}
void fgLoadIdentity() {get_matrix_stack().identity(); mark_matrix_changed();}
void fgPushIdentityMatrix() {get_matrix_stack().push_identity(); mark_matrix_changed();}

void assign_cur_matrix(glm::mat4 const &m) {
	get_matrix_stack().assign(m);
	mark_matrix_changed();
}

void fgTranslate(float x, float y, float z) {
	if (x == 0.0 && y == 0.0 && z == 0.0) return; // zero translate optimization
	assign_cur_matrix(glm::translate(get_matrix_stack().top(), glm::vec3(x, y, z)));
}
void fgScale(float x, float y, float z) {
	if (x == 1.0 && y == 1.0 && z == 1.0) return; // identity scale optimization
	assign_cur_matrix(glm::scale(get_matrix_stack().top(), glm::vec3(x, y, z)));
}
void fgScale(float s) {fgScale(s, s, s);}

void fgRotateRadians(float angle, float x, float y, float z) {
	if (angle == 0.0) return; // zero rotate optimization
	if (x == 0.0 && y == 0.0 && z == 0.0) return; // degenerate rotate, glm doesn't handle this well
	assign_cur_matrix(glm::rotate(get_matrix_stack().top(), angle, glm::vec3(x, y, z)));
}
void fgRotate(float angle, float x, float y, float z) {fgRotateRadians(TO_RADIANS*angle, x, y, z);}

void fgPerspective(float fov_y, float aspect, float near_clip, float far_clip) {
	assign_cur_matrix(glm::perspective(TO_RADIANS*fov_y, aspect, near_clip, far_clip));
}

void fgOrtho(float left, float right, float bottom, float top, float zNear, float zFar) {
	assign_cur_matrix(glm::ortho(left, right, bottom, top, zNear, zFar));
}

void fgLookAt(float eyex, float eyey, float eyez, float centerx, float centery, float centerz, float upx, float upy, float upz) {
	assign_cur_matrix(glm::lookAt(glm::vec3(eyex, eyey, eyez), glm::vec3(centerx, centery, centerz), glm::vec3(upx, upy, upz)));
}

void fgMultMatrix(xform_matrix const &m) {
	assign_cur_matrix(get_matrix_stack().top() * m);
}

xform_matrix const &fgGetMVM() {return mvm_stack.top();}
xform_matrix const &fgGetPJM() {return pjm_stack.top();}


// *** mesh2d ***


void mesh2d::clear() {
	
	pmap.clear();
	rmap.clear();
	ptsh.clear();
	size = 0;
}


void mesh2d::set_size(unsigned sz) {

	assert(sz > 0);
	assert(size == 0 || sz == size);
	clear();
	size = sz;
}


void mesh2d::add_random(float mag, float min_mag, float max_mag, unsigned skipval) {

	if (pmap.empty()) {alloc_pmap();}
	unsigned const num(get_num());

	for (unsigned i = (rand()%(skipval+1)); i < num; i += (skipval+1)) {
		pmap[i] = max(min_mag, min(max_mag, (pmap[i] + mag*signed_rand_float())));
	}
}


void mesh2d::mult_by(float val) {

	if (pmap.empty()) return;
	unsigned const num(get_num());
	for (unsigned i = 0; i < num; ++i) {pmap[i] *= val;}
}


void mesh2d::unset_rand_rmap(unsigned num_remove) {

	if (rmap.empty()) {rmap.resize(get_num(), 1);}
	for (unsigned i = 0; i < num_remove; ++i) {rmap[choose_rand()] = 0;} // doesn't check for already removed elements
}


void mesh2d::set_rand_expand(float mag, unsigned num_exp) {

	if (emap.empty()) {emap.resize(get_num(), 0.0);}
	for (unsigned i = 0; i < num_exp; ++i) {emap[choose_rand()] += mag;} // doesn't check for already removed elements
}


void mesh2d::set_rand_translate(point const &tp, unsigned num_trans) {

	if (tp == all_zeros) return;
	if (ptsh.empty()) {ptsh.resize(get_num(), all_zeros);}
	for (unsigned i = 0; i < num_trans; ++i) {ptsh[choose_rand()] += tp;} // doesn't check for already translated elements
}


void mesh2d::draw_perturbed_sphere(point const &pos, float radius, int ndiv, bool tex_coord) const {

	if (pmap.empty() && rmap.empty() && emap.empty() && ptsh.empty() && expand == 0.0) {
		draw_sphere_vbo(pos, radius, ndiv, 1);
	}
	else { // ndiv unused
		if (!pmap.empty() || !rmap.empty() || !emap.empty() || !ptsh.empty()) {assert(size > 0);}
		point const camera(get_camera_all());
		draw_subdiv_sphere(pos, radius, size, camera, (pmap.empty() ? nullptr : &pmap.front()), tex_coord, 1,
			(rmap.empty() ? nullptr : &rmap.front()), (emap.empty() ? nullptr : &emap.front()), (ptsh.empty() ? nullptr : &ptsh.front()), expand);
	}
}


// *** transform_data ***


void transform_data::set_perturb_size(unsigned i, unsigned sz) {

	assert(i < perturb_maps.size());

	if (!perturb_maps[i].pmap.empty()) {
		assert(perturb_maps[i].get_size() == sz);
	}
	else {
		perturb_maps[i].set_size(sz);
		perturb_maps[i].alloc_pmap();
	}
}


void transform_data::add_perturb_at(unsigned s, unsigned t, unsigned i, float val, float min_mag, float max_mag) {

	assert(i < perturb_maps.size());
	perturb_maps[i].set_val(s, t, max(min_mag, min(max_mag, (perturb_maps[i].get_val(s, t) + val))));
}


void transform_data::reset_perturb_if_set(unsigned i) {
	
	assert(i < perturb_maps.size());

	if (!perturb_maps[i].pmap.empty()) { // reset pmap
		perturb_maps[i].pmap.clear();
		perturb_maps[i].alloc_pmap();
	}
}


// *** deformation code ***


void apply_obj_mesh_roll(xform_matrix &matrix, point const &pos, point const &lpos, float radius, float a_add, float a_mult) {

	if (!dist_less_than(pos, lpos, 0.01*radius)) {
		vector3d normal(zero_vector);

		if (world_mode == WMODE_INF_TERRAIN) {
			normal = get_interpolated_terrain_normal(pos);
		}
		else {
			int const xpos(get_xpos(pos.x)), ypos(get_ypos(pos.y));
			if (!point_outside_mesh(xpos, ypos)) {normal = surface_normals[ypos][xpos];}
		}
		if (normal != zero_vector) {
			vector3d const delta(pos, lpos);
			float const dmag(delta.mag()), angle(a_mult*(dmag/radius) + a_add);
			vector3d const vrot(cross_product(normal, delta/dmag));

			if (vrot.mag() > TOLERANCE) {
				matrix.normalize();
				matrix = glm::rotate(glm::mat4(1.0), angle, vec3_from_vector3d(vrot)) * matrix;
			}
		}
	}
	fgMultMatrix(matrix);
}


void deform_obj(dwobject &obj, vector3d const &norm, vector3d const &v0) { // apply collision deformations

	float const deform(object_types[obj.type].deform);
	if (deform == 0.0) return;
	assert(deform > 0.0 && deform < 1.0);
	vector3d const vd(obj.velocity, v0);
	float const vthresh(base_gravity*GRAVITY*tstep*object_types[obj.type].gravity), vd_mag(vd.mag());

	if (vd_mag > max(2.0f*vthresh, 12.0f/fticks) && (fabs(v0.x) + fabs(v0.y)) > 0.01) { // what about when it hits the ground/mesh?
		float const deform_mag(SQRT3*deform*min(1.0, 0.05*vd_mag));
		UNROLL_3X(obj.vdeform[i_] -= fabs(norm[i_])*deform_mag;)
		obj.vdeform *= SQRT3/obj.vdeform.mag(); // normalize the volume
		float vdmin(1.0 - deform);
		UNROLL_3X(vdmin = min(vdmin, obj.vdeform[i_]);)

		if (vdmin < (1.0 - deform)) {
			UNROLL_3X(obj.vdeform[i_] += ((1.0 - deform) - vdmin);)
			obj.vdeform *= SQRT3/obj.vdeform.mag(); // re-normalize
		}
	}
}


void update_deformation(dwobject &obj) {

	if (obj.vdeform != all_ones && object_types[obj.type].def_recover > 0.0) {
		obj.vdeform += all_ones*(fticks*object_types[obj.type].def_recover);
		obj.vdeform *= SQRT3/obj.vdeform.mag(); // normalize the volume
	}
}


void mirror_about_plane(vector3d const &norm, point const &pt) { // applies to GL state

	float const dp(dot_product(pt, norm));
	float const m[16] = {1-2*norm.x*norm.x,  -2*norm.x*norm.y,  -2*norm.x*norm.z, 0.0,
			              -2*norm.x*norm.y, 1-2*norm.y*norm.y,  -2*norm.y*norm.z, 0.0,
		                  -2*norm.x*norm.z,  -2*norm.y*norm.z, 1-2*norm.z*norm.z, 0.0,
		                   2*dp*norm.x,       2*dp*norm.y,       2*dp*norm.z,     1.0};
	fgMultMatrix(glm::make_mat4(m));
}


void print_matrix(float const *const m, std::string const &prefix) {

	if (!prefix.empty()) {cout << prefix << endl;}

	for (unsigned i = 0; i < 4; ++i) {
		for (unsigned j = 0; j < 4; ++j) {cout << m[4*i+j] << " ";}
		cout << endl;
	}
}

// Note: instance_render_t::draw_and_clear() is defined in shaders.cpp


