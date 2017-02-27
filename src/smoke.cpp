// 3D World - dynamic volumetric smoke effects code
// by Frank Gennari
// 4/30/11
#include "3DWorld.h"
#include "mesh.h"
#include "lightmap.h"
#include "gl_ext_arb.h"


bool const DYNAMIC_SMOKE     = 1; // looks cool
int const SMOKE_SKIPVAL      = 8;
int const SMOKE_SEND_SKIP    = 8;
int const INDIR_LT_SEND_SKIP = 12;

float const SMOKE_DENSITY    = 1.0;
float const SMOKE_MAX_CELL   = 0.125;
float const SMOKE_MAX_VAL    = 100.0;
float const SMOKE_DIS_XY     = 0.05;
float const SMOKE_DIS_ZU     = 0.08;
float const SMOKE_DIS_ZD     = 0.03;
float const SMOKE_THRESH     = 1.0/255.0;


bool smoke_visible(0), smoke_exists(0), have_indir_smoke_tex(0);
unsigned smoke_tid(0), last_smoke_update(0);
colorRGB const_indir_color(BLACK);
cube_t cur_smoke_bb;
vector<unsigned char> smoke_tex_data; // several MB

extern bool no_smoke_over_mesh, no_sun_lpos_update;
extern unsigned create_voxel_landscape;
extern int animate2, display_mode, scrolling, game_mode;
extern float czmin0;
extern colorRGB cur_ambient, cur_diffuse;
extern lmap_manager_t lmap_manager;
extern vector<cube_t> smoke_bounds;
extern llv_vect local_light_volumes;



bool check_smoke_bounds(point const &pt) {

	for (vector<cube_t>::const_iterator i = smoke_bounds.begin(); i != smoke_bounds.end(); ++i) {
		if (i->contains_pt(pt)) return 1;
	}
	return smoke_bounds.empty(); // if empty we assume unbounded
}


class smoke_grid_t {
	vector<unsigned char> mask; // which x/y grid stacks contain smoke (acceleration structure)
public:
	void set_has_smoke(int x, int y, bool val) {
		assert(!point_outside_mesh(x, y));
		if (mask.empty()) {mask.resize(XY_MULT_SIZE, 0);} else {assert(mask.size() == XY_MULT_SIZE);}
		mask[y*MESH_X_SIZE + x] = val;
	}
	bool get_has_smoke(int x, int y) const {
		if (mask.empty() || point_outside_mesh(x, y)) return 0;
		return (mask[y*MESH_X_SIZE + x] != 0);
	}
};

smoke_grid_t smoke_grid;


struct smoke_manager {
	bool enabled, smoke_vis;
	float tot_smoke;
	cube_t bbox;

	smoke_manager() {reset();}
	inline bool is_smoke_visible(point const &pos) const {return camera_pdu.sphere_visible_test(pos, HALF_DXY);} // could use cube_visible()
	
	void reset() {
		for (unsigned i = 0; i < 3; ++i) { // set backwards so that nothing intersects
			bbox.d[i][0] =  SCENE_SIZE[i];
			bbox.d[i][1] = -SCENE_SIZE[i];
		}
		tot_smoke = 0.0;
		enabled   = 0;
		smoke_vis = 0;
	}
	void add_smoke(int x, int y, int z, float smoke_amt) {
		if (smoke_amt == 0) return; // can't happen?
		point const pos(get_xval(x), get_yval(y), get_zval(z));

		if (is_smoke_visible(pos) && check_smoke_bounds(pos)) {
			bbox.union_with_pt(pos);
			cur_smoke_bb.union_with_pt(pos);
			smoke_vis = 1;
		}
		tot_smoke += smoke_amt;
		enabled    = 1;
	}
	void adj_bbox() {
		for (unsigned i = 0; i < 3; ++i) {
			float const dval(SCENE_SIZE[i]/MESH_SIZE[i]);
			bbox.d[i][0] -= dval;
			bbox.d[i][1] += dval;
		}
	}
};

smoke_manager smoke_man, next_smoke_man;


inline void adjust_smoke_val(float &val, float delta) {val = max(0.0f, min(SMOKE_MAX_VAL, (val + delta)));}

void add_smoke(point const &pos, float val) {

	if (!DYNAMIC_SMOKE || (display_mode & 0x80) || !game_mode || val == 0.0 || pos.z >= czmax) return;
	lmcell *const lmc(lmap_manager.get_lmcell(pos));
	if (!lmc) return;
	int const xpos(get_xpos(pos.x)), ypos(get_ypos(pos.y));
	if (point_outside_mesh(xpos, ypos) || pos.z >= v_collision_matrix[ypos][xpos].zmax || pos.z < mesh_height[ypos][xpos]) return; // above all cobjs/outside
	if (no_smoke_over_mesh && !is_mesh_disabled(xpos, ypos)) return;
	if (!check_smoke_bounds(pos)) return;
	//if (!check_coll_line(pos, point(pos.x, pos.y, czmax), cindex, -1, 1, 0)) return; // too slow
	adjust_smoke_val(lmc->smoke, SMOKE_DENSITY*val);
	smoke_exists |= smoke_man.is_smoke_visible(pos);
	smoke_grid.set_has_smoke(xpos, ypos, 1);
}


void diffuse_smoke(int x, int y, int z, lmcell &adj, float pos_rate, float neg_rate, int dim, int dir) {

	float delta(0.0); // Note: not using fticks due to instability
	
	if (lmap_manager.is_valid_cell(x, y, z)) {
		lmcell &lmc(lmap_manager.get_lmcell(x, y, z));
		unsigned char const flow(dir ? adj.pflow[dim] : lmc.pflow[dim]);
		if (flow == 0) return;
		float const cur_smoke(lmc.smoke);
		delta  = (flow/255.0)*(adj.smoke - cur_smoke); // diffusion out of current cell and into cell xyz (can be negative)
		delta *= ((delta < 0.0) ? neg_rate : pos_rate);
		adjust_smoke_val(lmc.smoke, delta);
		delta  = (lmc.smoke - cur_smoke); // actual change
		if (lmc.smoke > 0.0) {smoke_grid.set_has_smoke(x, y, 1);}
	}
	else { // edge cell has infinite smoke capacity and zero total smoke
		delta = 0.5*(pos_rate + neg_rate);
	}
	adjust_smoke_val(adj.smoke, -delta);
}


void distribute_smoke() { // called at most once per frame

	//RESET_TIME;
	if (!DYNAMIC_SMOKE || !smoke_exists || !animate2) return;
	assert(SMOKE_SKIPVAL > 0);
	static int cur_skip(0);
	static rand_gen_t rgen;
	
	if (cur_skip == 0) {
		//cout << "tot_smoke: " << smoke_man.tot_smoke << ", enabled: " << smoke_exists << ", visible: " << smoke_visible << endl;
		smoke_man     = next_smoke_man;
		smoke_man.adj_bbox();
		smoke_visible = smoke_man.smoke_vis;
		smoke_exists  = smoke_man.enabled;
		next_smoke_man.reset();
	}
	/*if ((display_mode & 0x10) && !smoke_bounds.empty()) {
		cur_smoke_bb = smoke_bounds[0];
		for (vector<cube_t>::const_iterator i = smoke_bounds.begin()+1; i != smoke_bounds.end(); ++i) {cur_smoke_bb.union_with_cube(*i);}
	}*/
	float const xy_rate(SMOKE_DIS_XY*SMOKE_SKIPVAL);
	int const dx(rgen.rand() & 1), dy(rgen.rand() & 1); // randomize the processing order
	
	// openmp doesn't really help here
	for (int y = cur_skip; y < MESH_Y_SIZE; y += SMOKE_SKIPVAL) { // split the computation across several frames
		for (int x = 0; x < MESH_X_SIZE; ++x) {
			lmcell *vldata(lmap_manager.get_column(x, y));
			if (vldata == NULL) continue;
			if (!smoke_grid.get_has_smoke(x, y)) continue;
			bool any_z_has_smoke(0);
			
			for (int z = 0; z < MESH_SIZE[2]; ++z) {
				lmcell &lmc(vldata[z]);
				if (lmc.smoke < SMOKE_THRESH) {lmc.smoke = 0.0;}
				if (lmc.smoke == 0.0) continue;
				//if (get_zval(z) > v_collision_matrix[y][x].zmax) {lmc.smoke = 0.0; continue;} // open space above - smoke goes up
				next_smoke_man.add_smoke(x, y, z, lmc.smoke);
				diffuse_smoke(x+(dx ?  1 : -1), y, z, lmc, xy_rate, xy_rate, 0,  dx);
				diffuse_smoke(x+(dx ? -1 :  1), y, z, lmc, xy_rate, xy_rate, 0, !dx);
				diffuse_smoke(x, y+(dy ?  1 : -1), z, lmc, xy_rate, xy_rate, 1,  dy);
				diffuse_smoke(x, y+(dy ? -1 :  1), z, lmc, xy_rate, xy_rate, 1, !dy);
				diffuse_smoke(x, y, (z - 1),  lmc, SMOKE_DIS_ZD, SMOKE_DIS_ZU, 2, 0);
				diffuse_smoke(x, y, (z + 1),  lmc, SMOKE_DIS_ZU, SMOKE_DIS_ZD, 2, 1);
				any_z_has_smoke = 1;
			} // for z
			if (!any_z_has_smoke) {smoke_grid.set_has_smoke(x, y, 0);} // mark this xy as not having smoke
		} // for x
	} // for y
	cur_skip = (cur_skip+1) % SMOKE_SKIPVAL;
	//PRINT_TIME("Distribute Smoke");
}


float get_smoke_at_pos(point const &pos) {

	if (!DYNAMIC_SMOKE  || !smoke_exists)  return 0.0;
	if (pos.z <= czmin0 || pos.z >= czmax) return 0.0;
	int const x(get_xpos(pos.x)), y(get_ypos(pos.y)), z(get_zpos(pos.z));
	if (point_outside_mesh(x, y) || z < 0 || z >= MESH_SIZE[2]) return 0.0;
	lmcell const *const vldata(lmap_manager.get_column(x, y));
	return ((vldata == NULL) ? 0.0 : vldata[z].smoke);
}


void reset_smoke_tex_data() {smoke_tex_data.clear();}


void update_smoke_row(vector<unsigned char> &data, vector<unsigned> const &llvol_ixs, lmcell const &default_lmc,
	unsigned x_start, unsigned x_end, unsigned z_start, unsigned z_end, unsigned y, bool update_lighting)
{
	unsigned const zsize(MESH_SIZE[2]), ncomp(4);
	float const smoke_scale(1.0/SMOKE_MAX_CELL);
	bool const do_lighting(update_lighting || lmap_manager.was_updated);
	colorRGB default_color;
	default_lmc.get_final_color(default_color, 1.0);

	for (unsigned x = x_start; x < x_end; ++x) {
		lmcell const *const vlm(lmap_manager.get_column(x, y));
		if (vlm == NULL && !update_lighting) continue; // x/y pairs that get into here should also be constant
		unsigned const off(zsize*(y*MESH_X_SIZE + x));
		bool const check_z_thresh((display_mode & 0x01) && !is_mesh_disabled(x, y));
		float const mh(mesh_height[y][x]);
		unsigned llv_ix_s(0), llv_ix_e(0);

		if (do_lighting) {
			for (unsigned i = 0; i < llvol_ixs.size(); ++i) {
				if (local_light_volumes[llvol_ixs[i]]->check_xy_bounds(x, y)) {llv_ix_s = min(i, llv_ix_s); llv_ix_e = max(i+1, llv_ix_e);}
			}
		}
		for (unsigned z = z_start; z < z_end; ++z) {
			unsigned const off2(ncomp*(off + z));
			if (vlm == NULL || vlm[z].smoke == 0.0) {data[off2+3] = 0;}
			else {data[off2+3] = (unsigned char)(255*CLIP_TO_01(smoke_scale*vlm[z].smoke));} // alpha: smoke
			if (!do_lighting) continue; // lighting not needed
				
			if (check_z_thresh && get_zval(z+1) < mh) { // adjust by one because GPU will interpolate the texel
				UNROLL_3X(data[off2+i_] = 0;)
			}
			else {
				colorRGB color;

				if (create_voxel_landscape) {
					float const indir_scale(get_voxel_terrain_ao_lighting_val(get_xyz_pos(x, y, z)));
					if (vlm == NULL) {color = default_color*indir_scale;} else {vlm[z].get_final_color(color, 1.0, 1.0, indir_scale);}
				}
				else {
					if (vlm == NULL) {color = default_color;} else {vlm[z].get_final_color(color, 1.0, 1.0);}
				}
				for (unsigned i = llv_ix_s; i < llv_ix_e; ++i) {local_light_volumes[llvol_ixs[i]]->add_lighting(color, x, y, z);} // add local light volumes
				UNROLL_3X(data[off2+i_] = (unsigned char)(255*CLIP_TO_01(color[i_]));) // lmc.pflow[i_]
			}
		} // for z
	} // for x
}


void update_smoke_indir_tex_range(unsigned x_start, unsigned x_end, unsigned y_start, unsigned y_end, unsigned z_start, unsigned z_end, bool update_lighting) {

	if (smoke_tex_data.empty()) return; // not allocated
	//timer_t timer("smoke update");
	if (z_end == 0) {z_end = MESH_SIZE[2];}
	assert(y_start < y_end && y_end <= (unsigned)MESH_Y_SIZE);
	assert(z_start < z_end && z_end <= (unsigned)MESH_SIZE[2]);
	unsigned const ncomp(4);
	lmcell default_lmc;
	default_lmc.set_outside_colors();
	default_lmc.get_final_color(const_indir_color, 1.0);
	bool const no_parallel(lmap_manager.was_updated || !update_lighting); // if running with multiple threads, don't use openmp
	vector<unsigned> llvol_ixs;
	
	for (unsigned i = 0; i < local_light_volumes.size(); ++i) { // sparse active optimization
		if (local_light_volumes[i]->is_active()) {llvol_ixs.push_back(i);}
	}
	#pragma omp parallel for schedule(static,1) if (!no_parallel)
	for (int y = y_start; y < (int)y_end; ++y) { // split the computation across several frames
		update_smoke_row(smoke_tex_data, llvol_ixs, default_lmc, x_start, x_end, z_start, z_end, y, update_lighting);
	}
	if (smoke_tid == 0) { // create texture
		static bool was_printed(0);
		if (!was_printed) {cout << "Allocating " << MESH_SIZE[2] << " by " << MESH_X_SIZE << " by " << MESH_Y_SIZE << " smoke texture of " << smoke_tex_data.size() << " bytes." << endl;}
		was_printed = 1;
		smoke_tid = create_3d_texture(MESH_SIZE[2], MESH_X_SIZE, MESH_Y_SIZE, ncomp, smoke_tex_data, GL_LINEAR, GL_CLAMP_TO_EDGE);
	}
	else { // update region/sync texture
		unsigned const off(ncomp*(z_start + (x_start + y_start*MESH_X_SIZE)*MESH_SIZE[2]));
		assert(off < smoke_tex_data.size());
		glPixelStorei(GL_UNPACK_ROW_LENGTH,   MESH_SIZE[2]);
		glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, MESH_X_SIZE);
		update_3d_texture(smoke_tid, z_start, x_start, y_start, (z_end - z_start), (x_end - x_start), (y_end - y_start), ncomp, &smoke_tex_data[off]); // stored as {z, x, y}
		glPixelStorei(GL_UNPACK_ROW_LENGTH,   0); // reset to 0
		glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0); // reset to 0
	}
}


bool upload_smoke_indir_texture() {

	//RESET_TIME;
	if (!lmap_manager.is_allocated()) {
		have_indir_smoke_tex = 0;
		return 0;
	}
	assert((MESH_Y_SIZE%SMOKE_SEND_SKIP) == 0);
	// ok when texture z size is not a power of 2
	unsigned const sz(MESH_X_SIZE*MESH_Y_SIZE*MESH_SIZE[2]), ncomp(4);

	if (sz == 0) {
		have_indir_smoke_tex = 0;
		return 0;
	}
	if (smoke_tex_data.empty()) {
		free_texture(smoke_tid);
		smoke_tex_data.resize(ncomp*sz, 0);
	}
	else { // will recreate the texture
		assert(smoke_tex_data.size() == ncomp*sz); // sz should be constant (per config file/3DWorld session)
	}
	check_for_lighting_finished();
	static colorRGB last_cur_ambient(BLACK), last_cur_diffuse(BLACK);
	bool lighting_changed(cur_ambient != last_cur_ambient || cur_diffuse != last_cur_diffuse);
	
	for (auto i = local_light_volumes.begin(); i != local_light_volumes.end(); ++i) { // check to see if any local light volumes have changed
		if ((*i)->needs_update()) {(*i)->mark_updated(); lighting_changed = 1;}
	}
	bool const full_update(smoke_tid == 0 || (!no_sun_lpos_update && lighting_changed));
	bool const could_have_smoke(smoke_exists || last_smoke_update > 0);
	if (!full_update && !could_have_smoke && !lmap_manager.was_updated && !lighting_changed) return 0; // return 1?
	if (full_update ) {last_cur_ambient  = cur_ambient; last_cur_diffuse = cur_diffuse;}
	if (smoke_exists) {last_smoke_update = SMOKE_SEND_SKIP;} else if (last_smoke_update > 0) {--last_smoke_update;}
	static int cur_block(0);
	unsigned const skipval(could_have_smoke ? SMOKE_SEND_SKIP : INDIR_LT_SEND_SKIP);
	unsigned const block_size(MESH_Y_SIZE/skipval);
	unsigned const y_start(full_update ? 0           :  cur_block*block_size);
	unsigned const y_end  (full_update ? MESH_Y_SIZE : (y_start + block_size));
	update_smoke_indir_tex_range(0, MESH_X_SIZE, y_start, y_end, 0, MESH_SIZE[2], full_update);
	cur_block = (full_update ? 0 : (cur_block+1) % skipval);
	if (cur_block == 0) {lmap_manager.was_updated = 0;} // only stop updating after we wrap around to the beginning again
	have_indir_smoke_tex = 1;
	//PRINT_TIME("Smoke + Indir Upload");
	return 1;
}

