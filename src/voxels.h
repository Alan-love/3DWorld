// 3D World - Voxel Header
// by Frank Gennari
// 2/25/12
#ifndef _VOXELS_H_
#define _VOXELS_H_

#include "3DWorld.h"
#include "model3d.h"

struct coll_tquad;

enum {VB_SHAPE_CUBE=0, VB_SHAPE_CONSTANT, VB_SHAPE_LINEAR, VB_SHAPE_QUADRATIC, NUM_VB_SHAPES};


struct voxel_params_t {

	// generation parameters
	unsigned xsize, ysize, zsize, num_blocks; // num_blocks is in x and y
	float isolevel, elasticity, mag, freq, atten_thresh, tex_scale, noise_scale, noise_freq, tex_mix_saturate, z_gradient, height_eval_freq, radius_val;
	float ao_radius, ao_weight_scale, ao_atten_power, spec_mag, spec_exp;
	bool make_closed_surface, invert, remove_under_mesh, add_cobjs, normalize_to_1, top_tex_used, detail_normal_map;
	unsigned remove_unconnected; // 0=never, 1=init only, 2=always, 3=always, including interior holes
	unsigned atten_at_edges; // 0=no atten, 1=top only, 2=all 5 edges (excludes the bottom), 3=sphere (outer), 4=sphere (inner and outer), 5=sphere (inner and outer, excludes the bottom)
	unsigned keep_at_scene_edge; // 0=don't keep, 1=always keep, 2=only when scrolling
	unsigned atten_top_mode; // 0=constant, 1=current mesh, 2=2d surface mesh
	unsigned enable_falling; // 0=never, 1=edit mode only, 2=game mode only, 3=always
	int geom_rseed;
	
	// rendering parameters
	int texture_rseed;
	unsigned tids[3];
	colorRGBA colors[2];
	colorRGBA base_color;

	voxel_params_t() : xsize(0), ysize(0), zsize(0), num_blocks(12), isolevel(0.0), elasticity(0.5), mag(1.0), freq(1.0), atten_thresh(1.0), tex_scale(1.0), noise_scale(0.1),
		noise_freq(1.0), tex_mix_saturate(5.0), z_gradient(0.0), height_eval_freq(1.0), radius_val(0.5), ao_radius(1.0), ao_weight_scale(2.0), ao_atten_power(1.0),
		spec_mag(0.0), spec_exp(1.0), make_closed_surface(1), invert(0), remove_under_mesh(0), add_cobjs(1), normalize_to_1(1), top_tex_used(0), detail_normal_map(1),
		remove_unconnected(1), atten_at_edges(0), keep_at_scene_edge(0), atten_top_mode(0), enable_falling(1), geom_rseed(123), texture_rseed(321), base_color(WHITE)
	{
			tids[0] = tids[1] = tids[2] = 0; colors[0] = colors[1] = WHITE;
	}
	bool atten_sphere_mode() const {return (atten_at_edges >= 3 && atten_at_edges <= 4);}
};


struct voxel_brush_params_t {

	int shape, weight_exp;
	unsigned delay, radius;
	float weight_scale;

	voxel_brush_params_t() : shape(VB_SHAPE_LINEAR), weight_exp(0), delay(0), radius(1), weight_scale(1.0) {}
	float get_world_space_radius() const;
	void draw(point const &pos) const;
};

struct voxel_brush_t : public voxel_brush_params_t {

	point pos;

	voxel_brush_t() {}
	voxel_brush_t(voxel_brush_params_t const &params, point const &pos_) : voxel_brush_params_t(params), pos(pos_) {}
};


class voxel_query_tree {

	template <typename T> struct bvh_tree_group : public vector<T> {

		bool bcube_valid;
		cube_t bcube;

		bvh_tree_group() : bcube_valid(0) {}
		
		bool get_root_bcube(cube_t &bc) const {
			if (bcube_valid) {bc = bcube;}
			return bcube_valid;
		}
		void update_bcube(unsigned ix) {
			assert(ix < size());
			cube_t bc;
			if (!operator[](ix).get_root_bcube(bc)) return; // invalid/empty bcube
			if (!bcube_valid) {bcube = bc;} else {bcube.union_with_cube(bc);}
			bcube_valid = 1;
		}
		void calc_bcube() {
			bcube_valid = 0;
			for (const_iterator i = begin(); i != end(); ++i) {update_bcube(i);}
		}
	};

	struct bvh_tree_row : public bvh_tree_group<cobj_bvh_tree> {
		void init(coll_obj_group const *cobjs, unsigned sz);
	};

	struct bvh_tree_matrix : public bvh_tree_group<bvh_tree_row> {
		void init(coll_obj_group const *cobjs, unsigned sz, unsigned row_sz);
	};

	coll_obj_group const *cobjs;
	bvh_tree_matrix tree_matrix;

public:
	voxel_query_tree(coll_obj_group const *cobjs_) : cobjs(cobjs_) {}
	void clear() {tree_matrix.clear();}

	void init(unsigned nx, unsigned ny) {
		clear();
		tree_matrix.init(cobjs, ny, nx);
	}
	void add_cobjs_for_block(vector<unsigned> const &cids, unsigned block_x, unsigned block_y);
	bool check_coll_line(point const &p1, point const &p2, point &cpos, vector3d &cnorm, int &cindex, int ignore_cobj, bool exact) const;
	void get_coll_sphere_cobjs(point const &center, float radius, int ignore_cobj, vert_coll_detector &vcd) const;
};


// stored internally in yxz order
template<typename V> class voxel_grid : public vector<V> {
	void init_grid(unsigned nx_, unsigned ny_, unsigned nz_, V default_val, unsigned num_blocks);
public:
	unsigned nx, ny, nz, xblocks, yblocks;
	vector3d vsz; // size of a voxel in x,y,z
	point center, lo_pos;

	voxel_grid() : nx(0), ny(0), nz(0), xblocks(0), yblocks(0), vsz(zero_vector) {}
	void init(unsigned nx_, unsigned ny_, unsigned nz_, vector3d const &vsz_, point const &center_, V const &default_val, unsigned num_blocks=1);
	void init(unsigned nx_, unsigned ny_, unsigned nz_, cube_t const &bcube, V const &default_val, unsigned num_blocks=1);
	void init_from_heightmap(float **height, unsigned mesh_nx, unsigned mesh_ny, unsigned zsteps, float mesh_xsize, float mesh_ysize, unsigned num_blocks=1, bool invert=0);
	void downsample_2x();
	bool is_valid_range(int i[3]) const {return (i[0] >= 0 && i[1] >= 0 && i[2] >= 0 && i[0] < (int)nx && i[1] < (int)ny && i[2] < (int)nz);}
	float get_xv(int x) const {return (x*vsz.x + lo_pos.x);}
	float get_yv(int y) const {return (y*vsz.y + lo_pos.y);}
	float get_zv(int z) const {return (z*vsz.z + lo_pos.z);}

	void get_xyz(point const &p, int xyz[3]) const { // returns whether or not the point was inside the voxel volume
		UNROLL_3X(xyz[i_] = int((p[i_] - lo_pos[i_])/vsz[i_]);); // convert to voxel space
	}
	bool get_ix(point const &p, unsigned &ix) const { // returns whether or not the point was inside the voxel volume
		int i[3]; // x,y,z
		get_xyz(p, i);
		if (!is_valid_range(i)) return 0;
		ix = i[2] + (i[0] + i[1]*nx)*nz;
		return 1;
	}
	unsigned get_ix(unsigned x, unsigned y, unsigned z) const {
		//assert(x < nx && y < ny && z < nz);
		return (z + (x + y*nx)*nz);
	}
	void get_bcube_ix_bounds(cube_t const &bcube, int llc[3], int urc[3]) const;
	point get_pt_at(unsigned x, unsigned y, unsigned z) const  {return (point(x, y, z)*vsz + lo_pos);}
	V const &get   (unsigned x, unsigned y, unsigned z) const  {return operator[](get_ix(x, y, z));}
	V &get_ref     (unsigned x, unsigned y, unsigned z)        {return operator[](get_ix(x, y, z));}
	void set       (unsigned x, unsigned y, unsigned z, V const &val) {operator[](get_ix(x, y, z)) = val;}
	cube_t get_raw_bbox() const {return cube_t(lo_pos, center + (center - lo_pos));}
	bool read(FILE *fp);
	bool write(FILE *fp) const;
};

typedef voxel_grid<float> float_voxel_grid;


class voxel_manager : public float_voxel_grid {

protected:
	bool use_mesh;
	voxel_params_t params;
	voxel_grid<unsigned char> outside;
	vector<unsigned> temp_work; // used in remove_unconnected_outside_range()/flood_fill()
	typedef vert_norm vertex_type_t;
	typedef vntc_vect_block_t<vertex_type_t> tri_data_t;
	typedef vertex_map_t<vertex_type_t> vertex_map_type_t;

	struct vert_ix_cache_entry {
		int ix[3]; // x, y, z
		vert_ix_cache_entry() {ix[0] = ix[1] = ix[2] = -1;}
	};

	typedef voxel_grid<vert_ix_cache_entry> voxel_ix_cache;

	struct pt_ix_t {
		point pt;
		unsigned ix;
		pt_ix_t(point const &pt_=all_zeros, unsigned const ix_=0) : pt(pt_), ix(ix_) {}
	};

	point interpolate_pt(float isolevel, point const &pt1, point const &pt2, float const val1, float const val2) const;
	void calc_outside_val(unsigned x, unsigned y, unsigned z, bool is_under_mesh);
	void flood_fill_range(unsigned x1, unsigned y1, unsigned x2, unsigned y2, vector<unsigned> &work, unsigned char fill_val, unsigned char bit_mask);
	void remove_unconnected_outside_range(bool keep_at_edge, unsigned x1, unsigned y1, unsigned x2, unsigned y2,
		vector<unsigned> *xy_updated, vector<pt_ix_t> *updated_pts, bool mark_only=0);
	unsigned add_triangles_for_voxel(tri_data_t::value_type &tri_verts, voxel_ix_cache &vix_cache,
		unsigned x, unsigned y, unsigned z, unsigned block_x0, unsigned block_y0, bool count_only, unsigned lod_level) const;
	void add_cobj_voxels(coll_obj &cobj, float filled_val);
	void make_voxel_outside(unsigned ix);
	void make_voxel_inside(unsigned ix);

public:
	voxel_manager(bool use_mesh_=0) : use_mesh(use_mesh_) {}
	void set_params(voxel_params_t const &p) {params = p;}
	void clear();
	void create_procedural(float mag, float freq, vector3d const &offset, bool normalize_to_1, int rseed1, int rseed2, int gen_mode);
	void create_from_cobjs(coll_obj_group &cobjs, float filled_val=1.0);
	void atten_at_edges(float val);
	void atten_at_top_only(float val);
	void atten_to_sphere(float val, float inner_radius, bool atten_inner, bool no_atten_zbot);
	void determine_voxels_outside();
	void remove_unconnected_outside();
	void remove_interior_holes();
	bool is_outside(unsigned ix) const {assert(ix < outside.size()); return((outside[ix]&3) != 0);}
	bool point_inside_volume(point const &pos) const;
	bool point_intersect(point const &center, point *int_pt) const;
	bool sphere_intersect(point const &center, float radius, point *int_pt) const;
	bool line_intersect(point const &p1, point const &p2, point *int_pt) const;
	unsigned upload_to_3d_texture(int wrap) const;
	voxel_params_t const &get_params() const {return params;}
};


class noise_texture_manager_t {

	unsigned noise_tid, tsize;
	voxel_manager voxels;

public:
	noise_texture_manager_t() : noise_tid(0), tsize(0) {}
	void make_private_copy() {noise_tid = 0;} // Note: to be called *only* after a deep copy
	void procedural_gen(unsigned size, int rseed=321, float mag=1.0, float freq=1.0, vector3d const &offset=zero_vector);
	void ensure_tid();
	void bind_texture(unsigned tu_id) const;
	void clear();
	float eval_at(point const &pos) const;
};


class voxel_model : public voxel_manager {

protected:
	bool volume_added;
	vector<tri_data_t> tri_data; // one per LOD level
	noise_texture_manager_t *noise_tex_gen;
	std::set<unsigned> modified_blocks, next_frame_modified_blocks;
	voxel_grid<unsigned char> ao_lighting;

	struct step_dir_t {
		unsigned nsteps;
		int dir[3], dist_per_step;
		step_dir_t(int x, int y, int z, unsigned n, int d) : nsteps(n), dist_per_step(d) {dir[0] = x; dir[1] = y; dir[2] = z;}
	};
	vector<step_dir_t> ao_dirs;
	vector<vector<pt_ix_t> > pt_to_ix;

	struct merge_vn_t {
		vertex_type_t *vn[4];
		unsigned num;
		vector3d normal;

		merge_vn_t() : num(0), normal(zero_vector) {vn[0] = vn[1] = vn[2] = vn[3] = NULL;}
		//void add   (vertex_type_t &v) {assert(num < 4); vn[num++] = &v;}
		void add   (vertex_type_t &v) {if (num < 4) {vn[num++] = &v;}} // allow degenerate vertices where more than 4 quads meet
		void update(vertex_type_t &v) {if (normal == zero_vector) {normal = v.n;} else {v.n = normal;}}
		void finalize();
	};

	typedef map<point, merge_vn_t> vert_norm_map_t;
	vector<vert_norm_map_t> boundary_vnmap;

	struct comp_by_dist {
		point const p;
		comp_by_dist(point const &p_) : p(p_) {}
		bool operator()(pt_ix_t const &a, pt_ix_t const &b) const {return (p2p_dist_sq(a.pt, p) < p2p_dist_sq(b.pt, p));}
	};

	void remove_unconnected_outside_modified_blocks(bool postproc_brushes_mode);
	unsigned get_block_ix(unsigned voxel_ix) const;
	virtual bool clear_block(unsigned block_ix);
	unsigned create_block(voxel_ix_cache &vix_cache, unsigned block_ix, bool first_create, bool count_only, unsigned lod_level);
	unsigned create_block_all_lods(unsigned block_ix, bool first_create, bool count_only);
	void update_boundary_normals_for_block(unsigned block_ix, bool calc_average);
	void finalize_boundary_vmap();
	void calc_ao_dirs();
	virtual void calc_ao_lighting_for_block(unsigned block_ix, bool increase_only);
	void calc_ao_lighting();

	virtual void maybe_create_fragments(point const &center, float radius, int shooter, unsigned num_fragments, bool directly_from_update) const {} // do nothing
	virtual void create_block_hook(unsigned block_ix) {}
	virtual void update_blocks_hook(vector<unsigned> const &blocks_to_update, unsigned num_added) {}
	virtual void pre_build_hook() {}
	virtual void pre_render(bool is_shadow_pass) {}

public:
	voxel_model(noise_texture_manager_t *ntg, bool use_mesh_, unsigned num_lod_levels);
	virtual ~voxel_model() {}
	void clear();
	bool update_voxel_sphere_region(point const &center, float radius, float val_at_center, bool spherical, int falloff_exp,
		point *damage_pos=NULL, int shooter=-1, unsigned num_fragments=0);
	unsigned get_texture_at(point const &pos) const;
	void proc_pending_updates(bool postproc_brushes_mode=0);
	void build(bool verbose, bool do_ao_lighting=1);
	virtual void setup_tex_gen_for_rendering(shader_t &s);
	void core_render(shader_t &s, unsigned lod_level, bool is_shadow_pass, bool no_vfc=0);
	void render(unsigned lod_level, bool is_shadow_pass);
	virtual void free_context();
	float eval_noise_texture_at(point const &pos) const;
	float get_ao_lighting_val(point const &pos) const;
	cube_t get_bcube() const {return ((tri_data[0].empty()) ? cube_t(center, center) : tri_data[0].get_bcube());}
	sphere_t get_bsphere() const;
	bool has_triangles() const;
	bool has_filled_at_edges() const;
	bool from_file(string const &fn);
	bool to_file(string const &fn) const;
	bool has_modified_blocks() const {return !modified_blocks.empty();}
};


class voxel_model_ground : public voxel_model {

	bool add_cobjs, add_as_fixed;
	noise_texture_manager_t private_ntg;
	voxel_query_tree cobj_tree;

	struct data_block_t {
		vector<unsigned> cids; // references into coll_objects
		//unsigned tri_data_ix;
		void clear() {cids.clear();}
	};
	vector<data_block_t> data_blocks;

	void calc_indir_lighting_for_block(point const &cur_sun_pos, unsigned block_ix);
	void calc_indir_lighting(point const &cur_sun_pos);

	virtual bool clear_block(unsigned block_ix);
	virtual void maybe_create_fragments(point const &center, float radius, int shooter, unsigned num_fragments, bool directly_from_update) const;
	virtual void create_block_hook(unsigned block_ix);
	virtual void update_blocks_hook(vector<unsigned> const &blocks_to_update, unsigned num_added);
	virtual void pre_build_hook();

public:
	voxel_model_ground(unsigned num_lod_levels=1);
	void clear();
	void build(bool add_cobjs_, bool add_as_fixed_, bool verbose);
	bool check_coll_line(point const &p1, point const &p2, point &cpos, vector3d &cnorm, int &cindex, int ignore_cobj, bool exact) const {
		return cobj_tree.check_coll_line(p1, p2, cpos, cnorm, cindex, ignore_cobj, exact);
	}
	void get_coll_sphere_cobjs(point const &center, float radius, int ignore_cobj, vert_coll_detector &vcd) const {
		cobj_tree.get_coll_sphere_cobjs(center, radius, ignore_cobj, vcd);
	}
	virtual void setup_tex_gen_for_rendering(shader_t &s);
};


class voxel_model_rock : public voxel_model {

	virtual void calc_ao_lighting_for_block(unsigned block_ix, bool increase_only) {} // do nothing

public:
	voxel_model_rock(noise_texture_manager_t *ntg, unsigned num_lod_levels) : voxel_model(ntg, 0, num_lod_levels) {}
	void build(bool verbose) {voxel_model::build(verbose, 0);}
};


class voxel_model_space : public voxel_model {

	unsigned ao_tid, shadow_tid;
	vector<triangle> shadow_edge_tris;

	void free_ao_and_shadow_texture() {free_texture(ao_tid); free_texture(shadow_tid);}
	virtual void calc_ao_lighting_for_block(unsigned block_ix, bool increase_only);
	void calc_shadows(voxel_grid<unsigned char> &shadow_data) const;
	void extract_shadow_edges(voxel_grid<unsigned char> const &shadow_data);

public:
	voxel_model_space(noise_texture_manager_t *ntg, unsigned num_lod_levels) : voxel_model(ntg, 0, num_lod_levels), ao_tid(0), shadow_tid(0) {}
	void clear() {voxel_model::clear(); shadow_edge_tris.clear();}
	virtual void free_context() {voxel_model::free_context(); free_ao_and_shadow_texture();}
	virtual void setup_tex_gen_for_rendering(shader_t &s);
	vector<triangle> const &get_shadow_edge_tris() const {return shadow_edge_tris;}
	void clone(voxel_model_space &dest) const;
};


#endif

