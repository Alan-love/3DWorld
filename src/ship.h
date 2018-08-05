// 3D World - Ship models for universe mode
// by Frank Gennari
// 8/25/05

#ifndef _SHIP_H_
#define _SHIP_H_

#include "universe_base.h"
#include "mesh2d.h"
#include "function_registry.h"
#include "string"
#include "deque"
#include <iostream>
#include <fstream>

using std::string;
using std::deque;
using std::ifstream;
using std::cerr;

typedef map<string, unsigned> kw_map;
struct string_to_color_map_t;


bool const VERIFY_REFS         = 0;
bool const MOVE_PLAYER_RPOS    = 1;

float const FAST_SPEED_FACTOR  = 1.0;
float const SLOW_SPEED_FACTOR  = 0.04;
float const SBODY_COLL_ELASTIC = 0.15;
float const OBJ_COLL_ELASTIC   = 0.7;
float const EXP_COLL_ELASTIC   = 1.0;
float const VISIBLE_THRESH     = 0.25;
float const SHIP_REQ_CREW      = 0.5;
float const TEMP_FACTOR        = 2.0; // damage temperature factor (ratio of damage to warning temperature)
float const FOBJ_TEMP_SCALE    = 5.0;
float const NDIV_SCALE_U       = 1200.0;

unsigned const W_SELF_ARM_T      = unsigned(TICKS_PER_SECOND/2);
unsigned const S_SELF_ARM_T      = unsigned(2*TICKS_PER_SECOND);
unsigned const FREE_OBJ_MAX_NDIV = 3*N_SPHERE_DIV/2;


// ship classes
enum {USC_FIGHTER=0, USC_X1EXTREME, USC_FRIGATE, USC_DESTROYER, USC_LCRUISER, USC_HCRUISER, USC_BCRUISER, USC_ENFORCER,
	  USC_CARRIER, USC_ARMAGEDDON, USC_SHADOW, USC_DEFSAT, USC_STARBASE, USC_BCUBE, USC_BSPHERE, USC_BTCUBE, USC_BSPH_SM,
	  USC_BSHUTTLE, USC_TRACTOR, USC_GUNSHIP, USC_NIGHTMARE, USC_DWCARRIER, USC_DWEXTERM, USC_WRAITH, USC_ABOMIN, USC_REAPER,
	  USC_DEATHORB, USC_SUPPLY, USC_ANTI_MISS, USC_JUGGERNAUT, USC_SAUCER, USC_SAUCER_V2, USC_MOTHERSHIP, USC_HUNTER,
	  USC_SEIGE, USC_COLONY, USC_ARMED_COL, USC_HW_COL, USC_STARPORT, USC_HW_SPORT, NUM_US_CLASS};

// universe ship weapons
enum {UWEAP_NONE=0, UWEAP_TARGET, UWEAP_QUERY, UWEAP_RENAME, UWEAP_DESTROY, UWEAP_PBEAM, UWEAP_EBEAM, UWEAP_REPULSER,
	  UWEAP_TRACTORB, UWEAP_G_HOOK, UWEAP_LRCPA, UWEAP_ENERGY, UWEAP_ATOMIC, UWEAP_ROCKET, UWEAP_NUKEDEV, UWEAP_TORPEDO,
	  UWEAP_EMP, UWEAP_PT_DEF, UWEAP_DFLARE, UWEAP_CHAFF, UWEAP_FIGHTER, UWEAP_B_BAY, UWEAP_CRU_BAY, UWEAP_SOD_BAY,
	  UWEAP_BOARDING, UWEAP_NM_BAY, UWEAP_RFIRE, UWEAP_FUSCUT, UWEAP_SHIELDD, UWEAP_THUNDER, UWEAP_ESTEAL, UWEAP_WRAI_BAY,
	  UWEAP_STAR, UWEAP_HUNTER, UWEAP_DEATHORB, UWEAP_LITNING, UWEAP_INFERNO, UWEAP_PARALYZE, UWEAP_MIND_C, UWEAP_SAUC_BAY,
	  UWEAP_SEIGEC, UWEAP_HYPER, NUM_UWEAP};

// damage types
enum {DAMAGE_HEAT=0, DAMAGE_COLL, DAMAGE_EXP, DAMAGE_PROJ, DAMAGE_BEAM, DAMAGE_DESTROY};

// ship AIs
// 0 = ignore everything, 1 = run when fired at, 2 = attack when fired at, 3 = attack all enemies, 4 = attack everything
enum {AI_IGNORE=0, AI_RETREAT, AI_ATT_WAIT, AI_ATT_ENEMY, AI_ATT_ALL, AI_SEEKING, AI_NONE, NUM_AI};
unsigned const AI_BASE_TYPE = 0x00FF;
unsigned const AI_KAMIKAZE  = 0x0100;
unsigned const AI_GUARDIAN  = 0x0200;

// ship alignments
enum {ALIGN_NEUTRAL=0, ALIGN_PLAYER, ALIGN_GOV, ALIGN_PIRATE, ALIGN_RED, ALIGN_BLUE, ALIGN_ORANGE, ALIGN_PURPLE, NUM_ALIGNMENT};

// targeting mode
enum {TARGET_CLOSEST=0, TARGET_ATTACKER, TARGET_LAST, TARGET_PARENT, NUM_TARGET};

// stationary_obj types
enum {SO_ASTEROID=0, SO_BLACK_HOLE, NUM_SO_TYPES};

// particle types
enum {PTYPE_SPHERE=0, PTYPE_TRIANGLE, PTYPE_GLOW, NUM_PTYPES};

// speeds
enum {FSPEED_HYPER=0, FSPEED_FAST, FSPEED_SLOW, NUM_FSPEEDS};

// asteroid models
enum {AS_MODEL_SPHERE=0, AS_MODEL_ROCK1, AS_MODEL_ROCK2, AS_MODEL_HMAP, AS_MODEL_VOXEL, AS_MODEL_SHADER, NUM_AS_MODELS};

// spawned object types
enum {SPO_COMET, NUM_SPO_TYPES};


// flag bits
unsigned const OBJ_FLAGS_SHIP = 0x0001; // is a ship
unsigned const OBJ_FLAGS_PART = 0x0002; // is a particle
unsigned const OBJ_FLAGS_TARG = 0x0004; // can be targetted
unsigned const OBJ_FLAGS_NCOL = 0x0008; // no collision detection
unsigned const OBJ_FLAGS_PROJ = 0x0010; // is a projectile
unsigned const OBJ_FLAGS_NOPC = 0x0020; // no projectile collisions
unsigned const OBJ_FLAGS_NOC2 = 0x0040; // no collision with other objects that have this flag set
unsigned const OBJ_FLAGS_FITR = 0x0080; // is a fighter
unsigned const OBJ_FLAGS_BAD_ = 0x0100; // is bad (equivalent to !is_ok())
unsigned const OBJ_FLAGS_NEXD = 0x0200; // no damage from explosions
unsigned const OBJ_FLAGS_STAT = 0x0400; // ships and stationary objects - for targetting
unsigned const OBJ_FLAGS_NEW_ = 0x0800; // time == 0
unsigned const OBJ_FLAGS_NOLT = 0x1000; // no light models on this object
unsigned const OBJ_FLAGS_DECY = 0x2000; // is a decoy
unsigned const OBJ_FLAGS_DIST = 0x4000; // distant object - use faster physics
unsigned const OBJ_FLAGS_ORBT = 0x8000; // orbiting object
unsigned const OBJ_FLAGS_PARC = 0x10000;// particle cloud

// collision detection object types
int const OBJ_TYPE_UOBJ  = 0x01; // planets, moons, etc.
int const OBJ_TYPE_SHIP  = 0x02; // ships
int const OBJ_TYPE_PROJ  = 0x04; // porjectiles
int const OBJ_TYPE_STAT  = 0x08; // static objects (asteroids)
int const OBJ_TYPE_FREE  = OBJ_TYPE_SHIP | OBJ_TYPE_PROJ; // free objects
int const OBJ_TYPE_FOBJ  = OBJ_TYPE_FREE | OBJ_TYPE_STAT; // free objects and static objects (free_obj)
int const OBJ_TYPE_SOBJ  = OBJ_TYPE_UOBJ | OBJ_TYPE_STAT; // stellar objects
int const OBJ_TYPE_ALL   = OBJ_TYPE_SOBJ | OBJ_TYPE_FREE; // all objects
int const OBJ_TYPE_LARGE = OBJ_TYPE_STAT | OBJ_TYPE_SHIP; // static objects and ships (large free_obj)
int const OBJ_TYPE_LGU   = OBJ_TYPE_SOBJ | OBJ_TYPE_SHIP; // large objects including planets, moons, etc

colorRGBA const TC_RED   (1.0, 0.5, 0.5, 1.0);
colorRGBA const TC_BLUE  (0.5, 0.5, 1.0, 1.0);
colorRGBA const TC_ORANGE(1.0, 0.6, 0.2, 1.0);
colorRGBA const TC_PURPLE(0.6, 0.2, 0.7, 1.0);

colorRGBA const alignment_colors[NUM_ALIGNMENT] = {YELLOW, LT_GREEN, WHITE, DK_BROWN, TC_RED, TC_BLUE, TC_ORANGE, TC_PURPLE};
string    const align_names[NUM_ALIGNMENT] = {"Neutral", "Player", "Government", "Pirate", "Red", "Blue", "Orange", "Purple"};


// forward references
template<typename T> class free_obj_block;
template<typename T> class free_obj_allocator;

class s_object;
class urev_body;
class free_obj;
class u_ship_base;
class u_ship;
class ship_weapon;


extern bool player_enemy, begin_motion; // required for efficiency
extern u_ship *player_ship_ptr;


inline bool player_ship_inited() {return (player_ship_ptr != NULL);}

inline u_ship &player_ship() {
	assert(player_ship_inited());
	return *player_ship_ptr;
}

inline bool TEAM_ALIGNED(unsigned const a) {
	return (a != ALIGN_NEUTRAL && a != ALIGN_GOV && (a != ALIGN_PLAYER || player_enemy));
}

inline void end_texture() {
	select_texture(WHITE_TEX); // texturing is always enabled
}

void merge_weapons(vector<ship_weapon> &weapons, ship_weapon const &w); // has to be here in the header
bool have_excess_credits(unsigned align);
float get_wealthy_value (unsigned align);


class ushadow_volume {

public:
	bool invalid;
	ushadow_volume() : invalid(0) {}
	virtual ~ushadow_volume() {}
	virtual void draw(upos_point_type const &pos) const = 0;
	void draw_geom(upos_point_type const &pos) const {if (!invalid) {draw(pos);}}
};

class ushadow_sphere : public ushadow_volume { // currently only supports spheres/cylinder shadow projections

	int nsides;
	double rad[2];
	upos_point_type spos[2];
	float const *pmap;

public:
	ushadow_sphere(upos_point_type const &sobj_pos, float sobj_r, upos_point_type const &cur_pos, float cur_radius,
		point const &sun_pos, int ndiv, bool player, free_obj const *const obj=NULL, float rmin=0.0);
	void set_pmap(float const *const pmap_) {pmap = pmap_;}
	void draw(upos_point_type const &pos) const;
};

class ushadow_polygon : public ushadow_volume { // currently only supports triangles and quads

	unsigned npts;
	upos_point_type p[2][4];

public:
	ushadow_polygon(upos_point_type const *const pts, unsigned np, upos_point_type const &cur_pos, float cur_radius,
		point const &sun_pos, bool player, free_obj const *const obj=NULL, float rmin=0.0);
	void draw(upos_point_type const &pos) const;
};

class ushadow_triangle_mesh : public ushadow_volume {

	struct voxel_triangle_t {
		upos_point_type p[2][3];
	};
	typedef vector<voxel_triangle_t> tri_vect_t;
	static tri_vect_t tris; // static to avoid excessive memory allocations

	void set_triangle(unsigned t, upos_point_type const pts[3], point const &sun_pos, upos_point_type const &cur_pos, double min_dist, double cur_radius);

public:
	ushadow_triangle_mesh(vector<triangle> const &triangles, upos_point_type const &cur_pos, float cur_radius,
		point const &sun_pos, float obj_radius, point const &obj_center, free_obj const *const obj=NULL);
	ushadow_triangle_mesh(point const &circle_center, float circle_radius, vector3d const &circle_normal, unsigned ndiv,
		upos_point_type const &cur_pos, float cur_radius, point const &sun_pos, free_obj const *const obj=NULL);
	void draw(upos_point_type const &pos) const;
};

struct quad_batch_draw;

class uobj_draw_data {

public:
	int ndiv;
	unsigned time, on_time, lifetime, eflags, nengines;
	bool powered, specular_en, dlights, first_pass, final_pass, phase1, phase2;
	float t_exp, dist, radius, crs, cloakval;
	upos_point_type pos;
	vector3d vel, dir, upv, tdir;
	colorRGBA color_a, color_b, engine_color;
	free_obj const *obj;
	shader_t *shader;
	vpc_shader_t *upc_shader;
	static quad_batch_draw qbd;

	uobj_draw_data(free_obj const *const obj_, shader_t *shader_, vpc_shader_t *upc_shader_, int ndiv_, unsigned t, bool power, bool spec_en, float texp,
		upos_point_type const &pos_, vector3d const &vel_, vector3d const &dir_, vector3d const &upv_, float dist_,
		float radius_, float crs_, bool dlights_, bool first, bool final, bool p1, bool p2)
		: ndiv(ndiv_), time(t), on_time(t), lifetime(0), eflags(0), nengines(0), powered(power), specular_en(spec_en),
		dlights(dlights_), first_pass(first), final_pass(final), phase1(p1), phase2(p2), t_exp(texp), dist(dist_), radius(radius_),
		crs(crs_), cloakval(0.0), pos(pos_), vel(vel_), dir(dir_), upv(upv_), tdir(dir),
		color_a(WHITE), color_b(WHITE), engine_color(BLACK), obj(obj_), shader(shader_), upc_shader(upc_shader_) {}

	inline bool is_moving() const {return (powered && (vel.mag_sq() > TOLERANCE));}
	bool can_have_engine_lights() const;
	bool draw_as_pt(float rscale=1.0) const {return (NDIV_SCALE_U*rscale*radius < dist);}

	void draw_ship_flares(colorRGBA const &color, int tid=BLUR_TEX) const;
	void setup_colors_draw_flare(point const &pos, point const &xlate, float xsize, float ysize, colorRGBA const &color, int flare_tex=BLUR_TEX) const;

	colorRGBA apply_cloak(colorRGBA const &color) const;
	void set_color(colorRGBA const &color) const;
	void set_cloak_color(colorRGBA const &color) const {set_color(apply_cloak(color));}
	void draw_bounding_sphere(colorRGBA const &color) const;
	void setup_exp_scale() const;
	void setup_exp_texture() const;
	void end_exp_texture() const;
	void enable_normal_map(string const &name) const;
	void disable_normal_map() const;
	void set_uobj_specular(float spec, float shine) const;
	void end_specular()    const {set_uobj_specular(0.0, 1.0);}
	void inverse_rotate()  const;
	void draw_engine(int eix, colorRGBA const &trail_color, point const &draw_pos, float escale=1.0,
		float ar=1.0, vector3d const &stretch_dir=all_zeros) const;
	void draw_engine_trail(int eix, point const &offset, float width, float w2s, float len, colorRGBA const &color) const;
	void draw_ehousing_pairs(float length, float r1, float r2, float lcone, float dx, float dy, bool texture,
		point const &offset, point const &per_pair_off=zero_vector, unsigned num_pairs=1) const;
	void draw_engine_pairs(colorRGBA const &color, unsigned eflags_ix, float escale, float dx, float dy, float dz,
		point const &per_pair_off=zero_vector, unsigned num_pairs=1, float ar=1.0, vector3d const &stretch_dir=zero_vector) const;
	void light_engine_pair(colorRGBA const &color, unsigned eflags_off, float escale, float dx, float dy, float dz) const;
	void unlight_engine_pair() const;
	void add_light_source(point const &lpos, float lradius, colorRGBA const &color) const;
	void draw_colored_flash(colorRGBA const &color, bool symmetric) const;
	void invert_z()          const;
	void setup_draw_ship()   const;

	void draw_one_triangle(vector3d const &rot_axis, float rot_deg) const;
	void draw_rocket_base(colorRGBA const &cb, colorRGBA const &cn, colorRGBA const &ce,
		float length, float width, float esize, float tailw) const;
	void draw_usw_rocket()   const;
	void draw_usw_nukedev()  const;
	void draw_usw_torpedo()  const;
	void draw_spherical_shot(colorRGBA const &color, bool glow=1) const;
	void draw_usw_energy()   const;
	void draw_usw_atomic()   const;
	void draw_usw_emp()      const;
	void draw_usw_dflare()   const;
	void draw_usw_chaff()    const;
	void draw_usw_rfire()    const;
	void draw_usw_shieldd()  const;
	void draw_usw_thunder()  const;
	void draw_usw_star_int(unsigned ndiv_, point const &lpos, point const &lpos0, float size,
		float rad, float instability, bool lit) const;
	void draw_usw_star()     const;
	void draw_usw_seige()    const;

	void draw_base_fighter(vector3d const &scale) const;
	void draw_us_fighter()   const {draw_base_fighter(vector3d(1.0, 1.0, 1.0));}
	void draw_x1_extreme()   const {draw_base_fighter(vector3d(1.3, 1.1, 0.7));}
	void draw_us_frigate()   const;
	void draw_us_destroyer() const;
	void draw_us_cruiser(bool heavy) const;
	void draw_us_lcruiser()  const {draw_us_cruiser(0);}
	void draw_us_hcruiser()  const {draw_us_cruiser(1);}
	void draw_us_bcruiser()  const;
	void draw_us_enforcer()  const;
	void draw_us_carrier()   const;
	void draw_armageddon(mesh2d const &surface_mesh) const;
	void draw_us_shadow()    const;
	void draw_defsat()       const;
	void draw_starbase()     const;
	void draw_borg(bool is_cube, bool is_small) const;
	void draw_bshuttle()     const;
	void draw_tractor()      const;
	void draw_gunship()      const;
	void draw_nightmare()    const;
	void draw_dwcarrier()    const;
	void draw_dwexterm()     const;
	void draw_wraith_tail(float r, int ndiv2, float rscale) const;
	void draw_wraith()       const;
	void draw_abomination()  const;
	void draw_reaper()       const;
	void draw_death_orb()    const;
	void draw_supply()       const;
	void draw_anti_miss()    const;
	void draw_juggernaut()   const;
	void draw_saucer(bool rotated, bool mothership) const;
	void draw_headhunter()   const;
	void draw_seige()        const;
	void draw_colony(bool armed, bool hw, bool starport) const;
	void draw_default_ship() const;

	void draw_asteroid(int tex_id) const;
	void draw_black_hole()   const;
};


struct ship_explosion {

  float bradius, damage;
  point pos;
  
  ship_explosion() : bradius(0.0), damage(0.0), pos(all_zeros) {}
  ship_explosion(float r, float d, point const &p) : bradius(r), damage(d), pos(p) {}
};


struct temp_source {

	float temp, radius;
	point pos;
	free_obj const *source;

	temp_source() {}
	temp_source(point const &pos_, float radius_, float temp_, free_obj const *source_)
		: temp(temp_), radius(radius_), pos(pos_), source(source_)
	{
		assert(radius > TOLERANCE && temp > 0.0);
	}
};


class ship_coll_obj {

	float dscale;

public:
	ship_coll_obj(float dscale_=1.0) : dscale(dscale_) {}
	virtual ~ship_coll_obj() {}
	float get_dscale() const {return dscale;}
	virtual ship_coll_obj* clone()   const = 0;
	virtual void translate(point const &p) = 0;
	virtual void draw(unsigned ndiv) const = 0;
	virtual void draw_cylin(unsigned ndiv, bool textured, float tex_scale_len=1.0) const {assert(0);}
	virtual bool line_intersect(point const &lp1, point const &lp2, float &t, bool calc_t) const = 0;
	virtual bool sphere_intersect(point const &sc, float sr, point const &p_last, point &p_int, vector3d &norm, bool calc_int) const = 0;
	virtual void draw_svol(point const &targ_pos, float cur_radius, point const &sun_pos, int ndiv, bool player, free_obj const *const obj=NULL) const = 0;
	virtual bool is_concave() const {return 0;}
	virtual void get_bounding_sphere(point &c, float &r) const = 0;
	point get_center() const;
	virtual string get_name()  const = 0;
	virtual float get_volume() const = 0;
	virtual float get_s_area() const = 0;
};

class ship_cylinder : public cylinder_3dw, public ship_coll_obj { // more accurately a truncated cone

	bool check_ends;

public:
	ship_cylinder() {}
	ship_cylinder(cylinder_3dw const &c, bool check_ends_) : cylinder_3dw(c), check_ends(check_ends_) {}
	ship_cylinder(point const &p1_, point const &p2_, float r1_, float r2_, bool check_ends_, float ds=1.0)
		: cylinder_3dw(p1_, p2_, r1_, r2_), ship_coll_obj(ds), check_ends(check_ends_) {}
	ship_cylinder* clone() const {return new ship_cylinder(*this);}
	void translate(point const &p) {cylinder_3dw::translate(p);}
	void draw_cylin(unsigned ndiv, bool textured, float tex_scale_len=1.0) const;
	void draw(unsigned ndiv) const {draw_cylin(ndiv, 0, 1.0);}
	bool line_intersect(point const &lp1, point const &lp2, float &t, bool calc_t) const;
	bool sphere_intersect(point const &sc, float sr, point const &p_last, point &p_int, vector3d &norm, bool calc_int) const;
	void get_bounding_sphere(point &c, float &r) const;
	void draw_svol(point const &tpos, float cur_radius, point const &spos, int ndiv, bool player, free_obj const *const obj=NULL) const;
	string get_name()  const {return "Cylinder";}
	float get_volume() const {return cylinder_3dw::get_volume();}
	float get_s_area() const {return PI*((check_ends ? (r1*r1 + r2*r2) : 0.0) + (r1 + r2)*get_length());}
};

class ship_cube : public ship_coll_obj, public cube_t {

public:
	ship_cube(float x1=0.0, float x2=0.0, float y1=0.0, float y2=0.0, float z1=0.0, float z2=0.0, float ds=1.0)
		: ship_coll_obj(ds), cube_t(x1, x2, y1, y2, z1, z2) {}
	ship_cube* clone() const {return new ship_cube(*this);}
	void translate(point const &p);
	void draw(unsigned ndiv) const;
	bool line_intersect(point const &lp1, point const &lp2, float &t, bool calc_t) const;
	bool sphere_intersect(point const &sc, float sr, point const &p_last, point &p_int, vector3d &norm, bool calc_int) const;
	void get_bounding_sphere(point &c, float &r) const;
	void draw_svol(point const &tpos, float cur_radius, point const &spos, int ndiv, bool player,  free_obj const *const obj=NULL) const;
	string get_name()   const {return "Cube";}
	float const delta(unsigned i) const {return fabs(d[i][1] - d[i][0]);}
	float get_volume() const {return cube_t::get_volume();}
	float get_s_area() const {return 2.0*(delta(0)*delta(1) + delta(0)*delta(2) + delta(1)*delta(2));}
};

class ship_sphere : public sphere_t, public ship_coll_obj {

public:
	ship_sphere(point const &c=all_zeros, float r=0.0, float ds=1.0) : sphere_t(c, r), ship_coll_obj(ds) {}
	ship_sphere* clone() const {return new ship_sphere(*this);}
	void translate(point const &p) {pos += p;}
	void draw(unsigned ndiv) const;
	bool line_intersect(point const &lp1, point const &lp2, float &t, bool calc_t) const;
	bool sphere_intersect(point const &sc, float sr, point const &p_last, point &p_int, vector3d &norm, bool calc_int) const;
	void get_bounding_sphere(point &c, float &r) const;
	void draw_svol(point const &tpos, float cur_radius, point const &spos, int ndiv, bool player, free_obj const *const obj=NULL) const;
	string get_name()  const {return "Sphere";}
	float get_volume() const {return sphere_t::get_volume();}
	float get_s_area() const {return sphere_t::get_surf_area();}
};

class ship_torus : public ship_coll_obj {

	point center;
	float ri, ro;

public:
	ship_torus(point const &c=all_zeros, float ri_=0.0, float ro_=0.0, float ds=1.0) : ship_coll_obj(ds), center(c), ri(ri_), ro(ro_) {}
	ship_torus* clone() const {return new ship_torus(*this);}
	void translate(point const &p) {center += p;}
	void draw(unsigned ndiv) const;
	bool line_intersect(point const &lp1, point const &lp2, float &t, bool calc_t) const;
	bool sphere_intersect(point const &sc, float sr, point const &p_last, point &p_int, vector3d &norm, bool calc_int) const;
	void get_bounding_sphere(point &c, float &r) const;
	void draw_svol(point const &tpos, float cur_radius, point const &spos, int ndiv, bool player, free_obj const *const obj=NULL) const;
	bool is_concave()  const {return 1;}
	string get_name()  const {return "Torus";}
	float get_volume() const {return 2.0*PI*PI*ri*ri*ro;}
	float get_s_area() const {return 4.0*PI*PI*ri*ro;}
};

class ship_bounded_cylinder : public ship_cylinder { // cylinder AND cube (can almost inherit from ship_cube as well)

	ship_cube bcube;

public:
	ship_bounded_cylinder() {}
	ship_bounded_cylinder(ship_cylinder const &cylin, ship_cube const &cube, float dscale=1.0)
		: ship_cylinder(cylin), bcube(cube) {}
	ship_bounded_cylinder* clone() const {return new ship_bounded_cylinder(*this);}
	void translate(point const &p) {ship_cylinder::translate(p); bcube.translate(p);}
	void draw(unsigned ndiv) const;
	void draw_cylin(unsigned ndiv, bool textured, float tex_scale_len=1.0) const {assert(0);}
	bool line_intersect(point const &lp1, point const &lp2, float &t, bool calc_t) const;
	bool sphere_intersect(point const &sc, float sr, point const &p_last, point &p_int, vector3d &norm, bool calc_int) const;
	void get_bounding_sphere(point &c, float &r) const;
	void draw_svol(point const &tpos, float cur_radius, point const &spos, int ndiv, bool player, free_obj const *const obj=NULL) const;
	string get_name()  const {return "Bounded Cylinder";}
	float get_volume() const {return min(ship_cylinder::get_volume(),   bcube.get_volume());} // ???
	float get_s_area() const {return 0.5*(ship_cylinder::get_s_area() + bcube.get_s_area());} // ???
};

// Note: we generally use r1 == r1 for a true cylinder (not a truncated cone)
class ship_capsule : public cylinder_3dw, public ship_coll_obj { // cylinder with hemispheres at either end

public:
	ship_capsule() {}
	ship_capsule(point const &p1_, point const &p2_, float radius_, float ds=1.0)
		: cylinder_3dw(p1_, p2_, radius_, radius_), ship_coll_obj(ds) {}
	ship_capsule* clone() const {return new ship_capsule(*this);}
	void translate(point const &p) {cylinder_3dw::translate(p);}
	void draw(unsigned ndiv) const;
	bool line_intersect(point const &lp1, point const &lp2, float &t, bool calc_t) const;
	bool sphere_intersect(point const &sc, float sr, point const &p_last, point &p_int, vector3d &norm, bool calc_int) const;
	void get_bounding_sphere(point &c, float &r) const;
	void draw_svol(point const &tpos, float cur_radius, point const &spos, int ndiv, bool player, free_obj const *const obj=NULL) const;
	string get_name()  const {return "Capsule";}
	float get_volume() const {return cylinder_3dw::get_volume() + (2.0/3.0)*PI*(r1*r1*r1 + r2*r2*r2);} // cylinder + two hemispheres
	float get_s_area() const {return PI*(r1 + r2)*get_length() + 2.0*PI*(r1*r1 + r2*r2);}
};

// triangle_list inherits from sphere (used as bounding sphere) and only overrides some functions
class ship_triangle_list : public ship_sphere {

	vector<triangle> triangles;
	vector<vert_norm> verts; // for drawing

public:
	ship_triangle_list(ship_sphere const &ss) : ship_sphere(ss) {}
	ship_triangle_list* clone() const {return new ship_triangle_list(*this);}
	void add_triangle(triangle const &tri) {triangles.push_back(tri);}
	void translate(point const &p);
	void finalize();
	void draw(unsigned ndiv) const;
	bool line_intersect(point const &lp1, point const &lp2, float &t, bool calc_t) const;
	bool sphere_intersect(point const &sc, float sr, point const &p_last, point &p_int, vector3d &norm, bool calc_int) const;
	void get_bounding_sphere(point &c, float &r) const {return ship_sphere::get_bounding_sphere(c, r);}
	void draw_svol(point const &tpos, float cur_radius, point const &spos, int ndiv, bool player, free_obj const *const obj=NULL) const;
	string get_name()  const {return "Triangle List";}
	float get_volume() const {return ship_sphere::get_volume();}
	float get_s_area() const {return ship_sphere::get_s_area();}
};


class us_class {

public:
	bool inited, has_pt_def;
	string name;
	float radius, cr_scale, mass, cargo, exp_scale, accel, decel, roll_rate, max_speed, max_turn, stability;
	float max_shields, max_armor, shield_re, armor_re, max_t, hull_str, damage_abs;
	float min_att_dist, min_app_dist, sensor_dist, fire_dist, stray_dist;
	mutable float offense, defense, weap_range; // cached
	bool reversible, stoppable, has_hyper, has_fast_speed, mpredict, has_cloak, regen_fighters, regen_ammo, regen_crew;
	bool parallel_fire, symmetric, self_shadow, cont_frag, for_boarding, can_board, orbiting_dock, dynamic_cobjs, uses_tdir;
	bool emits_light, suicides, kamikaze, no_disable, uses_mesh2d, mesh_deform, mesh_remove, mesh_expand;
	bool mu_expand, mesh_trans, exp_disint;
	int turreted;
	unsigned sclass, weap_spread, shield_sects, draw_passes, fire_speed, exp_type, exp_subtype, death_delay, regen_delay;
	unsigned cost, ncrew, nengines, engine_lights;
	colorRGBA base_color, engine_color;
	ship_sphere bnd_sphere;
	cobj_vector_t cobjs;
	vector<triangle> cobj_triangles;
	vector<ship_weapon> weapons;

	us_class() : inited(0), has_pt_def(0), offense(-1.0), defense(-1.0), weap_range(-1.0), fire_speed(FSPEED_SLOW) {}
	void clear_cobjs();
	bool read_from_ifstream(ifstream &in, string_to_color_map_t const &string_to_color);
	void setup(unsigned sclass_);
	void add_weapon(ship_weapon const &w);
	void set_mesh_params(bool deform, bool remove, bool expand, bool mu_exp, bool trans);
	void add_bcube(float x1, float x2, float y1, float y2, float z1, float z2, float dscale);
	void add_bcylinder(ship_cylinder const &c);
	void add_bsphere(point const &center, float r, float dscale);
	void add_btorus(point const &center, float ri, float ro, float dscale);
	void add_bcylin_cube(ship_cylinder const &c, float x1, float x2, float y1, float y2, float z1, float z2);
	void add_bcapsule(point const &p1, point const &p2, float r, float dscale);
	void add_triangle(triangle const &tri);
	float offense_rating() const;
	float defense_rating() const;
	unsigned weap_cost()   const;
	unsigned ammo_cost()   const;
	float used_mass()      const;
	bool can_attack()      const;
	bool can_move()        const {return (max_speed > 0.0 && accel > 0.0);}
	unsigned req_crew()    const;
	float get_weap_range() const;
	float get_min_weap_range() const;
	float calc_cradius()   const {return radius*cr_scale;}
	void draw_bounding_volume(unsigned ndiv) const;
};

extern vector<us_class> sclasses; // required for specs()


struct beam_weap_params {

	colorRGBA brc[2], beamc[2];
	float bw_escale;
	bool energy_drain, temp_src, paralyze, mind_control, multi_segment;

	beam_weap_params() : bw_escale(0.0), energy_drain(0), temp_src(0),
		paralyze(0), mind_control(0), multi_segment(0) {} // start at an invalid value
	bool read(ifstream &in, string_to_color_map_t const &string_to_color);
};


class us_weapon {

	beam_weap_params bwp; // not used in all cases

public:
	bool inited;
	string name;
	int btime;
	unsigned wclass, def_ammo, ammo_type, exp_type, nshots, cost, ammo_cost;
	float lifetime, radius, c_radius, bradius, damage, range, speed, seek_dist, fire_delay, firing_error, regen_time;
	float max_t, mass, w_mass, a_mass, force, f_inv, armor, preference;
	bool seeking, hit_proj, hit_all, c2_flag, no_exp_dam, is_beam, secondary, hyper_fire, symmetric, is_fighter, do_regen;
	bool no_coll, const_dam, no_ffire, point_def, is_decoy, ignores_shields, shield_d_only, no_light, parallel_fire;
	bool det_on_exp, no_ship_vel;
	int turreted, auto_orient;

	us_weapon() : inited(0) {}
	bool read_from_ifstream(ifstream &in);
	bool read_beam_params_from_ifstream(ifstream &in, string_to_color_map_t const &string_to_color);
	void setup(unsigned wclass_);
	void calc_preference();
	bool need_ammo()       const {return (is_fighter || def_ammo > 0);}
	bool can_lead_shot()   const {return (!is_beam && !is_fighter && speed > 0.0);}
	float offense_rating() const;
	float defense_rating() const;
	bool is_beamlike()     const {return (is_beam || bwp.bw_escale > 0.0);}
	beam_weap_params const &get_beam_params() const {assert(bwp.bw_escale > 0.0); return bwp;}
};


struct intersect_params {

	bool calc_int, calc_dscale;
	point const p_last;
	point p_int;
	vector3d norm;
	float dscale;

	intersect_params() : calc_int(0), calc_dscale(0), p_last(all_zeros), p_int(all_zeros), norm(plus_z), dscale(1.0) {}
	intersect_params(point const &p_last_, point const &p_int_=all_zeros, vector3d const &norm_=plus_z)
		: calc_int(1), calc_dscale(1), p_last(p_last_), p_int(p_int_), norm(norm_), dscale(1.0) {}
		
	void update_int_pn(point const &p_int_, vector3d const &norm_, float dscale_) {
		p_int = p_int_; norm = norm_; dscale = dscale_;
	}
};

extern intersect_params def_int_params; // default value for option function arguments


class free_obj : public uobject { // freely moving object

protected:
	bool near_b_hole;
	unsigned flags, reset_timer, time, obj_id;
	int sobj_coll_tid;
	mutable int shadow_val;
	float speed_factor, max_sfactor, temperature, extra_mass, rot_rate, sobj_dist, draw_rscale, ambient_scale;
	point reset_pos;
	vector3d velocity, upv, dir, dvel, rot_axis, gvect;
	free_obj const *target_obj, *parent;
	unsigned exp_lights[6], num_exp_lights; // NUM_EXP_LIGHTS=6 (explosion.h not included)
	unsigned alignment;
	float c_radius;

	static unsigned next_obj_id;

	// no virtual function call for efficiency
	bool invalid_priv() const {return (!is_ok() || is_resetting());}
	bool powered_priv() const {return ((is_player_ship() || begin_motion) && !invalid_priv());}

	// cached data
	mutable float ra1, ra2;
	mutable vector3d rv1, rv2;
	void invalidate_rotv() {rv1 = rv2 = zero_vector;}

private:
	free_obj(free_obj const &); // forbidden
	void operator=(free_obj const &); // forbidden

public:
	free_obj(point const &init_pos=all_zeros) : flags(OBJ_FLAGS_TARG), speed_factor(1.0), max_sfactor(1.0),
		reset_pos(init_pos), alignment(ALIGN_NEUTRAL), c_radius(0.0) {reset();}
	
	void fix_upv();
	void accelerate(float speed, float accel);
	void decelerate(float speed, float decel);
	void do_rotate(float pitch, float yaw);
	void arb_rotate_about(float rangle, vector3d const &raxis);
	void tilt(float val);
	void apply_torque_force(point const &fpos, vector3d const &fdir, float fmag, float mass);
	float coll_physics(point const &copos, vector3d const &vcoll, float obj_mass, float obj_radius,
		free_obj const *const source, float elastic, float *dscale=NULL);
	void set_max_speed(float max_speed);
	void set_dir(vector3d const &dir_) {dir       = dir_;}
	void set_upv(vector3d const &upv_) {upv       = upv_;}
	void set_vel(vector3d const &vel ) {velocity  = vel;}
	void set_align(unsigned align)     {alignment = align;}
	void set_sobj_dist(float dist)     {sobj_dist = dist;}
	void set_sobj_coll_tid(int tid)    {sobj_coll_tid = tid;}
	void reset_after(unsigned nticks) {if (reset_timer == 0) reset_timer = nticks;}
	void reset_lights() {num_exp_lights = 0;}
	void set_parent(free_obj const *p) {parent = p;}
	void add_light(unsigned index);
	vector3d get_orient() const;
	void calc_rotation_vectors() const;
	void force_calc_rotation_vectors() const;
	template<typename T> void rotate_point    (pointT<T> &pt) const;
	template<typename T> void xform_point     (pointT<T> &pt) const;
	template<typename T> void rotate_point_inv(pointT<T> &pt) const;
	template<typename T> void xform_point_inv (pointT<T> &pt) const;
	void xform_point_inv_multi(upos_point_type *pts, unsigned npts) const;
	void xform_point_x2(point &p1, point &p2) const;
	vector3d calc_angular_vel(point const &cpos, vector3d const &axis, float rate) const;
	void large_obj_visibility_test(point const &lpos, float exp_radius, vector<uobject const*> &sobjs, bool get_all) const;
	void rotate() const;
	void inverse_rotate() const;
	void draw_shadow_volumes_from(uobject const *sobj, point const &sun_pos, float dscale, int ndiv) const;
	void transform_and_draw_obj(uobj_draw_data &udd, bool specular, bool first_pass, bool final_pass) const;
	void draw(shader_t &shader, vpc_shader_t &upc_shader) const;
	void draw_and_reset_lights(shader_t &shader, vpc_shader_t &upc_shader) {draw(shader, upc_shader); reset_lights();}

	void invalidate_permanently() {status = 2;} // status set to anything other than 0 or 1 makes this object invalid
	void verify_status() const {assert(status == 0 || status == 1);}
	void check_distant();
	
	void add_flag(unsigned f)             {flags       |= f;}
	void set_speed_factor(float s_fact)   {speed_factor = s_fact;}
	void set_max_sf(float sf)             {max_sfactor  = sf;}
	void set_time(unsigned time_)         {time         = time_;}
	void move_to(upos_point_type const &pos_) {pos      = pos_;}
	void move_reset_by(point const &pos_) {reset_pos   += pos_;}
	void add_mass(float const m)          {extra_mass  += m; assert(extra_mass >= 0.0);} // can this be negative?
	virtual void add_gravity_swp(vector3d const &gravity, vector3d const &swp, float gscale, bool near_bh);

	vector3d const &get_velocity() const {return velocity;}
	vector3d const &get_dir()      const {return dir;}
	vector3d const &get_up()       const {return upv;}
	point get_center_of_mass()     const {return pos;} // always at the object origin (Note: converts from 64-bit to 32-bit float)
	float           get_c_radius() const {return c_radius;}
	float    get_bounding_radius() const {return c_radius;}
	float           get_temp()     const {return temperature;}
	float           get_true_temp()const {return temperature/FOBJ_TEMP_SCALE;}
	float           get_sfactor()  const {return speed_factor;}
	float           get_max_sf()   const {return max_sfactor;}
	float        get_draw_radius() const {return draw_rscale*radius;}
	unsigned        get_align()    const {return alignment;}
	unsigned        get_time()     const {return time;}
	unsigned        get_flags()    const {return flags;}
	int          get_shadow_val()  const {return shadow_val;}
	free_obj const *get_target()   const {return target_obj;}
	free_obj const *get_parent()   const {return parent;}
	free_obj const *get_root_parent() const;
	int get_owner() const {return alignment;}
	bool is_resetting()   const {return (reset_timer > 0);}
	bool is_burning()     const {return (temperature > get_max_t());}
	bool is_ship()        const {return ((flags & OBJ_FLAGS_SHIP) != 0);}
	bool is_fighter()     const {return ((flags & OBJ_FLAGS_FITR) != 0);}
	bool is_stationary()  const {return ((flags & OBJ_FLAGS_STAT) != 0);}
	bool is_particle()    const {return ((flags & OBJ_FLAGS_PART) != 0);}
	bool is_part_cloud()  const {return ((flags & OBJ_FLAGS_PARC) != 0);}
	bool is_proj()        const {return ((flags & OBJ_FLAGS_PROJ) != 0);}
	bool no_proj_coll()   const {return ((flags & OBJ_FLAGS_NOPC) != 0);}
	bool no_coll2()       const {return ((flags & OBJ_FLAGS_NOC2) != 0);}
	bool is_target()      const {return ((flags & OBJ_FLAGS_TARG) != 0);}
	bool no_coll()        const {return ((flags & OBJ_FLAGS_NCOL) != 0);}
	bool no_light()       const {return ((flags & OBJ_FLAGS_NOLT) != 0);}
	bool is_decoy()       const {return ((flags & OBJ_FLAGS_DECY) != 0);}
	bool is_orbiting()    const {return ((flags & OBJ_FLAGS_ORBT) != 0);}
	bool has_lights()     const {return (num_exp_lights > 0);}
	bool is_player_ship() const {return (this == (free_obj const *const)(&player_ship()));}
	bool to_be_removed()  const {return (status != 0 && !is_player_ship() && !is_resetting());}
	bool is_related(free_obj const *fobj)   const {assert(fobj); return (fobj->get_root_parent() == get_root_parent());}
	bool target_valid(free_obj const *targ) const {return (targ != NULL && targ != this && targ != parent && !is_related(targ) && !targ->invalid());}
	bool is_invisible()   const {return (visibility() < VISIBLE_THRESH);}
	float get_over_temp_factor() const {return max(0.0f, (temperature - TEMP_FACTOR*get_max_t()));}
	free_obj *get_closest_ship(point const &pos, float min_dist, float max_dist, bool enemy,
		bool attack_all, bool req_shields=0, bool decoy_tricked=0, bool dir_pref=0) const;
	bool is_parent_of_docking_fighter(free_obj const *const fobj) const {return (fobj != NULL && fobj->get_parent() == this && fobj->get_target() == this);}
	unsigned get_obj_id() const {return obj_id;}

	virtual ~free_obj() {verify_status(); invalidate_permanently();}
	virtual void reset();
	virtual void check_ref_objs();
	virtual void move_by(point const &pos_) {pos += pos_;} // reset_pos?
	virtual void clear_context() {}
	virtual int   auto_orient()    const {return 0;}
	virtual float get_max_t()      const = 0;
	virtual float get_mass()       const {return (S_BODY_DENSITY*MASS_SCALE*radius*radius*radius + extra_mass);}
	virtual float get_surf_area()  const {return PI*radius*radius;} // assumes spherical - circle cross sectional area
	virtual float get_elasticity() const {return OBJ_COLL_ELASTIC;}
	virtual float get_min_damage() const {return 0.0;}
	virtual int   get_src_sclass() const {if (parent != NULL) return parent->get_src_sclass(); else return SWCLASS_UNDEF;}
	virtual free_obj const *get_src() const {return NULL;}
	virtual vector3d get_tot_vel_at(point const &cpos) const;
	virtual bool collision(point const &copos, vector3d const &vcoll, float obj_mass, float obj_radius, free_obj *source, float elastic) {return 0;}
	virtual float damage(float val, int type, point const &hit_pos, free_obj const *source, int wc);
	virtual void draw_obj(uobj_draw_data &ddata) const = 0;
	virtual void draw_flares_only() const {assert(0);}
	virtual void set_temp(float temp, point const &tcenter, free_obj const *source=NULL);
	virtual void ai_action() {} // default: no AI
	virtual void first_frame_hook() {}
	virtual void apply_physics();
	virtual void advance_time(float timestep);
	virtual int get_gravity(vector3d &vgravity, point const &mpos) const {return 0;}
	virtual bool fire_weapon(vector3d const &fire_dir, float target_dist) {return 0;} // default - do nothing (should this be here?)
	virtual bool dec_ref() {return 0;} // could be const?
	virtual void reset_target() {target_obj = NULL;}
	virtual bool source_is_player()            const {return ((parent == NULL) ? 0 : parent->source_is_player());}
	virtual u_ship_base const *get_ship_base() const {assert(0); return NULL;} // sort of a hack
	virtual ship_explosion get_explosion()     const {assert(0); return ship_explosion();} // shouldn't be here
	virtual void next_frame() {} // empty
	virtual void acknowledge_kill() {} // empty
	virtual bool dock_fighter(u_ship *ship) {assert(0); return 0;} // shouldn't be here
	virtual bool orbital_dock(u_ship *ship) {assert(0); return 0;} // shouldn't be here
	virtual bool do_boarding(u_ship *ship)  {assert(0); return 0;} // shouldn't be here
	virtual bool board_ship(u_ship *ship)   {assert(0); return 0;} // shouldn't be here
	virtual void near_sobj(s_object &clobj, int coll) {assert(0);} // shouldn't be here
	virtual vector<ship_weapon> const *get_weapons() const {assert(0); return NULL;} // shouldn't be here
	virtual bool can_board()     const {return 0;}
	virtual bool can_dock_with() const {return 0;}
	virtual bool try_paralyze(u_ship const *source, float intensity, point const &ppos) {return 0;} // do nothing
	virtual bool try_mind_control(u_ship *source, unsigned num_tries) {return 0;} // do nothing
	virtual float get_child_stray_dist() const {return 0.0;}
	virtual float offense()     const {return 0.0;}
	virtual float defense()     const {return 0.0;}
	virtual bool has_weapons()  const {return 0;}
	virtual bool hostile_explode() const {return 0;}
	virtual unsigned get_cost() const {return 0;}
	virtual unsigned get_ai_type() const {return AI_NONE;}
	virtual bool invalid()      const {return invalid_priv();}
	virtual bool not_a_target() const {return invalid_priv();}
	virtual bool is_exploding() const {return 0;}
	virtual bool calc_rvs()     const {return 0;}
	virtual unsigned get_num_draw_passes() const {return 1;}
	virtual bool disabled()     const {return 0;}
	virtual bool powered()      const {return powered_priv();}
	virtual bool has_shields()  const {return 0;}
	virtual bool need_blend()   const {return 0;}
	virtual bool regen_enabled()const {return 0;}
	virtual bool self_shadow()  const {return 0;}
	virtual bool can_move()     const {return 0;}
	virtual bool has_seeking_weapons() const {return 0;}
	virtual float visibility()  const {return 1.0;}
	virtual float damage_done() const {return 0.0;}
	virtual float get_state_val()  const {return 1.0;}
	virtual float get_damage_abs() const {return 0.0;}
	virtual unsigned get_ncrew()   const {return 0;}
	virtual float get_damage()     const {return 0.0;}
	virtual bool has_detailed_coll(free_obj const *const other_obj) const {return 0;}
	virtual bool maybe_has_line_of_sight(upos_point_type const &to_pos) const {return 1;} // only orbiting_ship can return 0
	virtual string get_name()   const = 0;
	virtual string get_info()   const {return "";}
	virtual bool rename(std::string const &name_) {return 0;} // do nothing
	
	// assumes a spherical object, and line/sphere or sphere/sphere intersect has already been tested before these are called
	virtual bool line_int_obj(point const &p1, point const &p2, point *p_int=NULL, float *dscale=NULL) const {return 1;}
	virtual bool sphere_int_obj(point const &c, float r, intersect_params &ip=def_int_params) const {assert(!ip.calc_int); return 1;}
	virtual bool ship_int_obj(u_ship const *const ship,  intersect_params &ip=def_int_params) const;
	virtual bool obj_int_obj (free_obj const *const obj, intersect_params &ip=def_int_params) const;
	virtual bool sphere_intersection(point const &c, float r) const {return sphere_int_obj(c, r);} // inherited from uobject
};


class stationary_obj : public free_obj { // a free_obj that doesn't actually move?

	unsigned type, lifetime;

public:
	stationary_obj(unsigned type_, point const &pos_, float radius_, unsigned lt=0);
	virtual ~stationary_obj() {status = 1;}
	float get_max_t() const {return 1000.0;} // a big number
	virtual float damage(float val, int type, point const &hit_pos, free_obj const *source, int wc);
	virtual void draw_obj(uobj_draw_data &ddata) const;
	void apply_physics();
	void advance_time(float timestep) {velocity = all_zeros;} // should already be all zeros
	int get_gravity(vector3d &vgravity, point const &mpos) const;
	float get_elasticity()  const {return 0.5;}
	virtual bool calc_rvs() const {return 0;}
	string get_name() const {return ((type == SO_BLACK_HOLE) ? "Black Hole" : "Stationary Object");}
};


class uobj_asteroid : public stationary_obj {

protected:
	int tex_id;

public:
	static uobj_asteroid *create(point const &pos, float radius, unsigned model, int tid, unsigned rseed_ix=0, unsigned lt=0);
	uobj_asteroid(point const &pos_, float radius_, int tid, unsigned lt) : stationary_obj(SO_ASTEROID, pos_, radius_, lt), tex_id(tid) {}
	bool calc_rvs() const {return 1;} // so that texture is oriented properly
	string get_name() const {return "Asteroid";}
	void explode(float damage, float bradius, int etype, vector3d const &edir, int exp_time, int wclass,
		int align, unsigned eflags=0, free_obj const *parent_=NULL);
	virtual int get_fragment_tid(point const &hit_pos) const {return tex_id;}
	virtual void draw_with_texture(uobj_draw_data &ddata, int force_tex_id, bool no_reset_texture=0) const {draw_obj(ddata);} // default is to ignore force_tex_id
	virtual bool draw_instanced(unsigned ndiv) {return 0;}
	virtual void final_draw(int xfm_shader_loc, int force_tid_to=-1) {}
	virtual void set_scale(vector3d const &scale) {} // default implementation has no scale so is ignored
};


class rand_spawn_mixin {

protected:
	upos_point_type &obj_pos;
	float obj_radius;
	bool first_pos, pos_valid;
	float max_cdist; // max distance to camera

public:
	rand_spawn_mixin(upos_point_type &pos_, float obj_radius_, float dmax) :
	  obj_pos(pos_), obj_radius(obj_radius_), first_pos(1), pos_valid(0), max_cdist(dmax) {assert(max_cdist > 0.0);}
	void gen_rand_pos();
	bool okay_to_respawn() const; // static?
	bool needs_respawned() const;
};


class uobject_rand_spawn_t : public free_obj, public rand_spawn_mixin {

protected:
	void mark_pos_invalid();
	virtual void gen_pos();

public:
	static uobject_rand_spawn_t *create(unsigned type, float radius_, float dmax, float vmag);
	uobject_rand_spawn_t(float radius_, float dmax, float vmag);

	// virtuals
	//float damage(float val, int type, point const &hit_pos, free_obj const *source, int wc);
	void explode(float damage, float bradius, int etype, vector3d const &edir, int exp_time, int wclass, int align, unsigned eflags, free_obj const *parent_);
	void advance_time(float timestep);
};


class ucomet : public uobject_rand_spawn_t {

	point sun_pos;
	unsigned inst_ids[2];

	virtual void gen_pos();
	void gen_inst_ids();

public:
	ucomet(float radius_, float dmax, float vmag);

	// virtuals
	float get_max_t() const {return 1000.0;} // a big number
	void set_temp(float temp, point const &tcenter, free_obj const *source);
	float damage(float val, int type, point const &hit_pos, free_obj const *source, int wc);
	void draw_obj(uobj_draw_data &ddata) const;
	bool calc_rvs() const {return 1;} // ???
	string get_name() const {return "Comet";}
};


class uparticle : public free_obj {

	unsigned ptype, lifetime;
	int texture_id;
	float angle, rrate, damage_v;
	vector3d axis;
	colorRGBA color1, color2;
	free_obj_block<uparticle> *alloc_block;

public:
	friend class free_obj_allocator<uparticle>;
	static unsigned const max_type = NUM_PTYPES;

	uparticle() : alloc_block(NULL) {}
	uparticle(unsigned ptype_, point const &pos_, vector3d const &vel, vector3d const &d, float radius_, colorRGBA const &c1,
		colorRGBA const &c2, unsigned lt, float damage_, unsigned align, bool coll_, int tid) : alloc_block(NULL) {
		set_params(ptype_, pos_, vel, d, radius_, c1, c2, lt, damage_, align, coll_, tid);
	}
	void reset() {alloc_block = NULL; free_obj::reset();}
	void set_params(unsigned ptype_, point const &pos_, vector3d const &vel, vector3d const &d, float radius_,
		colorRGBA const &c1, colorRGBA const &c2, unsigned lt, float damage_, unsigned align, bool coll_, int tid);
	void set_type(unsigned type) {ptype = type;}
	bool dec_ref();
	vector3d get_tot_vel_at(point const &cpos) const;
	void apply_physics();
	bool collision(point const &copos, vector3d const &vcoll, float obj_mass, float obj_radius, free_obj *source, float elastic);
	float damage(float val, int type, point const &hit_pos, free_obj const *source, int wc);
	float get_max_t()  const {return ((ptype == PTYPE_GLOW) ? 1000.0 : ((ptype == PTYPE_TRIANGLE) ? 40.0 : 200.0));} // arbitrary?
	float offense()    const {return damage_v;}
	bool hostile_explode() const {return 1;}
	bool calc_rvs()    const {return 0;} // was (ptype == PTYPE_TRIANGLE)
	bool need_blend()  const {return (ptype == PTYPE_GLOW);}
	bool can_move()    const {return 1;}
	string get_name()  const {return "Particle";}
	void draw_obj(uobj_draw_data &ddata) const;
};


class uparticle_cloud : public free_obj, public volume_part_cloud {

	unsigned lifetime;
	float rmin, rmax, damage_v, expand_exp, noise_scale, hashval; // or damage_v temperature? unused
	colorRGBA colors[2][2]; // {inner, outer} x {start, end}

public:
	uparticle_cloud() {}
	uparticle_cloud(point const &pos_, float rmin_, float rmax_, colorRGBA const &ci1, colorRGBA const &co1, colorRGBA const &ci2,
		colorRGBA const &co2, unsigned lt, float damage_, float expand_exp_, float noise_scale_);
	float get_lt_scale() const {return CLIP_TO_01((float)time/(float)lifetime);}

	// virtuals
	void apply_physics();
	float get_mass()       const {return 0.001*S_BODY_DENSITY*MASS_SCALE*rmax*rmax*rmax;} // very small
	float get_elasticity() const {return 0.0;} // inelastic
	float get_max_t()  const {return 1.0E6;} // very high
	bool calc_rvs()    const {return 0;}
	bool need_blend()  const {return 1;}
	bool can_move()    const {return 0;} // set to 1 later?
	//float damage_done() const {return damage_v;}
	string get_name()  const {return "Particle Cloud";}
	void draw_obj(uobj_draw_data &ddata) const;

	// no intersections (particle clouds are gasses, not solids)
	bool line_int_obj(point const &p1, point const &p2, point *p_int=NULL, float *dscale=NULL) const {return 0;}
	bool sphere_int_obj(point const &c, float r, intersect_params &ip=def_int_params) const {return 0;}
	bool ship_int_obj(u_ship const *const ship,  intersect_params &ip=def_int_params) const {return 0;}
	bool obj_int_obj (free_obj const *const obj, intersect_params &ip=def_int_params) const {return 0;}
};


class us_projectile : public free_obj { // for a weapon

private:
	unsigned wclass;
	unsigned tup_time;
	float armor;
	free_obj_block<us_projectile> *alloc_block;

public:
	friend class free_obj_allocator<us_projectile>;
	static unsigned const max_type = NUM_UWEAP;

	us_projectile(unsigned type=UWEAP_NONE);
	void reset() {alloc_block = NULL; free_obj::reset();}
	void set_type(unsigned type);
	bool dec_ref();
	us_weapon const &specs() const;
	float get_max_t() const {return specs().max_t;}
	float get_mass()  const {return (specs().mass + extra_mass);} // more mass than a ship to give higher collision impact
	unsigned get_eflags() const;
	void ai_action();
	void apply_physics();
	free_obj const *get_src() const {return ((parent == NULL) ? NULL : parent->get_src());}
	void add_gravity_swp(vector3d const &gravity, vector3d const &swp, float gscale, bool near_bh);
	bool collision(point const &copos, vector3d const &vcoll, float obj_mass, float obj_radius, free_obj *source, float elastic);
	float damage(float val, int type, point const &hit_pos, free_obj const *source, int wc);
	float offense()     const {return specs().offense_rating();}
	float defense()     const {return specs().defense_rating();}
	unsigned get_cost() const {return 0;} // specs().cost?
	unsigned get_ai_type() const {return (specs().seeking ? AI_SEEKING : AI_NONE);}
	float get_damage()  const {return ((specs().armor > 0.0) ? (1.0 - armor/specs().armor) : 0.0);}
	void draw_obj(uobj_draw_data &ddata) const;
	void draw_flares_only() const;
	int auto_orient()   const {return  specs().auto_orient;}
	bool calc_rvs()     const {return !specs().symmetric;}
	bool need_blend()   const {return 0;} // should fix this eventually
	bool has_weapons()  const {return 1;}
	bool hostile_explode() const {return 1;}
	bool can_move()     const {return 1;}
	float damage_done() const {return specs().damage;}
	string get_name()   const {return (align_names[get_align()] + " " + specs().name);}
};



// used for regen (docked fighters and ships themselves)
class u_ship_base { // Note: Can be created on the stack and copied

public:
	unsigned sclass, ncrew, ncredits, kills, tot_kills, deaths;
	bool docked, regened, o_docked;
	float shields, armor, energy, fuel, used_cargo, size_scale;
	point wpt_center;
	set<u_ship *> fighters;
	vector<ship_weapon> weapons;

	u_ship_base(unsigned sclass_);
	void create_from(u_ship const *const ship);
	void free_weapons();
	unsigned get_sclass() const {return sclass;}
	void check_ref_objs(u_ship *cur_ship);
	bool find_weapon_atype(unsigned atype, bool fighter, unsigned &ix) const;
	bool find_weapon_wclass(unsigned wclass, unsigned &ix) const;
	us_class const &specs() const {assert(sclass < NUM_US_CLASS); return sclasses[sclass];}
	bool build_fighter(unsigned fsclass);
	void orbital_ship_regen(u_ship *ship);
	void use_fuel(float val);
	void regen(float rate, u_ship_base *dock);
	void accept_fighters_from(u_ship *ship, u_ship *cur_ship);
	void copy_weapons_from_sclass();
	void add_weapon(ship_weapon const &w) {merge_weapons(weapons, w);}
	void add_fighter(u_ship *ship, u_ship *cur_ship, bool from_fighter_bay);
	void adj_crew(int crew) {assert(-crew <= (int)ncrew); ncrew += crew;} // could check for req_crew
	bool has_ammo_for(unsigned wclass) const;
	bool has_space_for_fighter(unsigned sclass) const;
	void reset_ammo();
	bool can_attack()       const;
	bool can_lead_shot_with(unsigned weap) const;
	bool shields_up()       const {return (shields > min(10.0, 0.005*get_max_shields()));}
	unsigned get_ncrew()    const {return ncrew;}
	float get_shields()     const {return shields;}
	float get_armor()       const {return armor;}
	float get_energy()      const {return energy;}
	float get_max_shields() const {return size_scale*specs().max_shields;}
	float get_max_armor()   const {return size_scale*specs().max_armor;}
	float get_mass()        const {return size_scale*size_scale*size_scale*specs().mass + used_cargo;}
	float get_crew_scale()  const {return float(max(1U, ncrew))/float(max(1U, specs().ncrew));}
	float get_damage()      const {return get_damage_after_time(0.0);}
	float get_damage_after_time(float time_seconds) const;
	float get_weap_ammo_mass(vector<ship_weapon> const &ws) const;
	float get_true_rel_mass_scale() const;
	bool need_ammo_for(unsigned wix) const;
	bool weap_turret(unsigned weapon_id) const;
	void print_ammo() const;

protected:
	bool out_of_ammo_for(unsigned wix, bool current_only) const;
	bool check_fire_delay(unsigned wix) const;
	bool bad_angle(float const angle, float target_dist, unsigned weapon_id) const;
};


class ship_weapon {

public:
	unsigned wclass, init_ammo, ammo, wcount, rtime, nregen, ndamaged, cur_wpt;
	int last_fframe;
	std::shared_ptr<deque<u_ship_base> > docked;
	vector<point> weap_pts;

	ship_weapon(unsigned weapon, unsigned num=1, unsigned ammo0=0, vector<point> const &weap_pts_=vector<point>());
	us_weapon const &get_usw() const;
	void regen_ammo(float rate, u_ship_base *dock);
	void dock_ship(u_ship *ship, u_ship *dock);
	void release_ship(u_ship *ship, u_ship *dock);
	void check_docked() {if (docked == nullptr) {docked.reset(new deque<u_ship_base>);}}
	bool space_for_fighter() const;
	bool can_lead_shot() const {return (!no_ammo() && get_usw().can_lead_shot());}
	float min_damage() const;
	bool no_ammo() const {return (ammo == 0 && (init_ammo > 0 || get_usw().is_fighter));}
};


class sobj_manager : public sphere_t {

	int uobj_id, old_uobj_id, otype, owner;
	string name;

public:
	sobj_manager() {clear();}
	void clear() {uobj_id = old_uobj_id = otype = -1; owner = NO_OWNER; radius = 0.0; pos = all_zeros;}
	void move_by(point const &pos_) {if (is_valid()) pos += pos_;}
	bool is_valid()    const {return (uobj_id != -1);}
	int get_owner()    const {return owner;}
	bool is_cur_obj(int uid) const {return (uobj_id != -1 && uid == uobj_id);}
	void set_object(uobject const *obj);
	void choose_dest(point const &p, unsigned align, float tmax);
	bool update_pos_if_close(uobject const *obj);
	bool update_pos();
	void at_dest() {old_uobj_id = uobj_id; uobj_id = -1;}
	bool claim_object(free_obj *parent, bool homeworld);
	string const &get_name() const {return name;}
};


class u_ship : public free_obj, public u_ship_base {

private:
	unsigned ai_type; // us_class of this ship
	bool lhyper, damaged, target_set, fire_primary, has_obstacle, captured, dest_override, is_flagship;
	float tow_mass, exp_val, cloaked, roll_val, pitch_r, yaw_r, roll_r, cached_rsv, child_stray_dist;
	unsigned curr_weapon, last_hit, target_mode, eflags, init_align;
	unsigned retarg_time, exp_time, tup_time, last_targ_t, disable_t, elapsed_on_t; // times
	point tcent;
	vector3d hit_dir, obs_orient, target_dir;
	string name;
	mesh2d surface_mesh;

	u_ship(u_ship const &); // forbidden
	void operator=(u_ship const &); // forbidden

protected:
	sobj_manager dest_mgr, homeworld;

	bool is_exploding_priv() const {return (exp_time > 0);}
	bool invalid_priv()      const {return (is_exploding_priv() || free_obj::invalid_priv());}
	bool disabled_priv()     const {return (time < disable_t);}
	bool powered_priv()      const {return (free_obj::powered_priv() && !disabled_priv() &&
		(specs().nengines == 0 || eflags < unsigned(1<<specs().nengines)-1));}
	void partial_uncloak(float val) {cloaked = min(val, cloaked);}
	bool can_colonize() const;
	vector3d const &get_last_hit_dir() const {return ((last_hit > 0) ? hit_dir : zero_vector);}

public:
	static unsigned const max_type = NUM_US_CLASS;
	u_ship(unsigned sclass_, point const &pos0, unsigned align, unsigned ai_type_, unsigned target_mode_, bool rand_orient);
	virtual ~u_ship();
	void reset();
	void create_from(u_ship_base const &base);
	void move_by(point const &pos_);
	bool has_detailed_coll(free_obj const *const other_obj) const {return (!get_cobjs().empty());}
	virtual cobj_vector_t const &get_cobjs() const {return specs().cobjs;}
	void draw_shadow_volumes(point const &targ_pos, float cur_radius, point const &sun_pos, int ndiv) const;
	void no_ai_wait();
	virtual void check_size_scale();
	void check_ref_objs();
	float get_engine_status() const;
	float get_real_speed_val() const;
	vector<ship_weapon> const *get_weapons() const {return &weapons;}
	void thrust(int tdir, float speed, bool hyperspeed);
	void turn(vector3d delta);
	int get_move_dir();
	vector3d get_tot_vel_at(point const &cpos) const;
	bool do_multi_target() const;
	free_obj const *find_closest_target(point const &pos0, float min_dist, float max_dist, bool req_shields) const;
	void acquire_target(float min_dist);
	free_obj *get_closest_dock(float max_dist) const;
	int get_line_query_obj_types(float qdist) const {return ((sobj_dist < qdist) ? OBJ_TYPE_LGU : OBJ_TYPE_LARGE);} // only test planets, etc. if close to sobj
	uobject const *setup_int_query(vector3d const &qdir, float qdist, free_obj *&fobj, float &tdist, bool sobjs_only, float line_radius) const;
	void calc_wpt_center();
	uobject const *get_obstacle(float at_time, float max_dist, free_obj *&fobj, float &tdist, bool sobjs_only) const;
	bool obstacle_avoid(vector3d &orient, float target_dist, bool sobjs_only);
	ship_explosion get_explosion() const;
	bool avoid_explosions(vector3d &orient) const;
	void do_turn(vector3d const &orient);
	bool can_return_to_parent()   const;
	bool check_return_to_parent() const;
	bool choose_destination();
	virtual bool claim_world(uobject const *uobj);
	virtual bool is_rand_spawn() const {return 0;} // hack to allow fighters to use the same derived class type
	u_ship const *try_fighter_pickup() const;
	free_obj const *try_orbital_regen(free_obj const *cur_targ, bool last_od, bool &targ_friend, bool &o_dock_close);
	bool roll_to_face_target(float &roll_amt) const;
	float get_fast_target_dist(free_obj const *const target=NULL) const;
	bool has_slow_fighters() const;
	void fire_at_target(free_obj const *const targ_obj, float min_dist);
	virtual void ai_action();
	void fire_point_defenses();
	bool find_coll_enemy_proj(float dmax, point &p_int) const;
	virtual bool has_clear_line_of_fire(us_weapon const &weap, vector3d const &fire_dir, float target_dist) const;
	void ai_fire(vector3d const &targ_dir, float target_dist, float min_dist, int move_dir);
	void get_fighter_target(u_ship const *ship);
	void set_temp(float temp, point const &tcenter, free_obj const *source);
	virtual void apply_physics();
	void set_ship_max_speed(float ms_scale=1.0);
	float get_max_speed() const {return speed_factor*specs().max_speed;}
	void disable(unsigned dtime) {disable_t = max(disable_t, time + dtime);}
	free_obj const *get_src() const {return this;} // a ship is its own source
	bool collision(point const &copos, vector3d const &vcoll, float obj_mass, float obj_radius, free_obj *source, float elastic);
	float damage(float val, int type, point const &hit_pos, free_obj const *source, int wc);
	void destroy_ship(float val);
	bool register_attacker(free_obj const *source);
	void register_destruction(free_obj const *source);
	void do_structure_damage(float val);
	float get_def_explode_damage() const;
	void do_explode();
	void explode(float damage, float bradius, int etype, vector3d const &edir, int exp_time, int wclass, int align,
		unsigned eflags=0, free_obj const *parent_=NULL);
	void fragment(vector3d const &edir, float num, bool outside_cr) const;
	void detonate_weapons();
	void switch_weapon(bool prev);
	void show_weapon_name() const;
	bool check_fire_speed() const;
	void set_fire_primary(bool fp) {fire_primary = fp;}
	bool try_fire_weapon();
	bool custom_wpt() const {return (!weapons.empty() && !weapons[curr_weapon].weap_pts.empty());}
	bool test_self_intersect(point const &fpos, point const &tpos, vector3d const &fdir, float target_dist);
	bool fire_weapon(vector3d const &fire_dir, float target_dist);
	void fire_beam(point const &fpos, vector3d const &fdir, unsigned weapon_id, unsigned num_shots, int intersect_type);
	void fire_projectile(point const &fpos, vector3d const &fire_dir);
	void spawn_fighter(point const &fpos, vector3d const &fire_dir);
	void dec_ammo(unsigned num=1);
	bool source_is_player() const {return is_player_ship();}
	void acknowledge_kill()  {++kills; ++tot_kills;}
	void acknowledge_death() {++deaths;}
	void reset_target();
	bool dock_fighter(u_ship *ship);
	bool orbital_dock(u_ship *ship);
	bool capture_ship(u_ship *ship, bool add_as_fighter);
	void make_flagship(float csd) {is_flagship = 1; child_stray_dist = csd;}
	float get_crew_strength() const;
	bool is_docked()          const {return docked;}
	bool has_homeworld()      const {return (homeworld.is_valid() && homeworld.get_owner() == (int)alignment);}
	float get_child_stray_dist() const;
	float min_time_to_target(point const &targ_pos) const;
	float offense()           const {return specs().offense_rating();}
	float defense()           const {return specs().defense_rating();}
	float get_min_damage()    const {return specs().damage_abs;}
	float get_t_exp()         const {return ((is_exploding_priv() && exp_time >= time) ? float(exp_time - time + 1)/float(specs().death_delay + 1) : 0.0);}
	bool has_weapons()        const {return (!weapons.empty());}
	bool hostile_explode()    const;
	unsigned get_cost()       const {return specs().cost;}
	unsigned get_ai_type()    const {return ai_type;}
	int      get_src_sclass() const {return (int)get_sclass();}
	unsigned get_num_kills()  const {return kills;}
	unsigned get_tot_kills()  const {return tot_kills;}
	unsigned get_num_deaths() const {return deaths;}
	unsigned get_num_draw_passes() const {return specs().draw_passes;}
	unsigned get_on_time()    const {return elapsed_on_t;}
	unsigned get_ncrew()      const {return u_ship_base::get_ncrew();}
	float get_damage()        const {return u_ship_base::get_damage();}
	void seed_on_time(int modval) {assert(modval > 0); elapsed_on_t = rand()%modval;}
	void draw_obj(uobj_draw_data &ddata) const;
	virtual void draw_bounding_volume(unsigned ndiv) const {specs().draw_bounding_volume(ndiv);}

	void clear_damaged() {damaged = 0;}
	void next_frame();
	bool do_boarding(u_ship *ship) {return (board_ship(ship) || ship->board_ship(this));} // try both ways
	bool board_ship(u_ship *ship);
	void near_sobj(s_object &clobj, int coll);
	bool can_board()     const {return specs().can_board;}
	bool can_dock_with() const {return specs().orbiting_dock;}
	bool try_paralyze(u_ship const *source, float intensity, point const &ppos);
	bool try_mind_control(u_ship *source, unsigned num_tries);
	bool was_damaged()  const {return damaged;}
	bool invalid()      const {return invalid_priv();}
	bool invalid_or_disabled() const {return invalid_priv() || disabled_priv();}
	bool not_a_target() const {return (invalid() || captured);}
	bool is_exploding() const {return is_exploding_priv();}
	bool explode_now()  const {return (is_exploding_priv() && time >= exp_time);}
	float get_max_t()   const {return specs().max_t;}
	float get_mass()    const {return (tow_mass + u_ship_base::get_mass() + extra_mass);}
	ship_weapon const &get_weapon()    const {assert(curr_weapon < weapons.size()); return weapons[curr_weapon];}
	unsigned           get_weapon_id() const {return get_weapon().wclass;}
	unsigned get_ammo() const {return get_weapon().ammo;}
	unsigned get_wnum() const {return get_weapon().wcount;}
	bool need_ammo()    const {return need_ammo_for(curr_weapon);}
	bool calc_rvs()     const {return !specs().symmetric;}
	bool disabled()     const {return disabled_priv();}
	bool powered()      const {return powered_priv();}
	bool has_shields()  const {return shields_up();}
	bool show_shields() const {return (last_hit > 0 && shields_up());}
	bool need_blend()   const {return show_shields();}
	bool self_shadow()  const {return specs().self_shadow;}
	bool can_move()     const {return specs().can_move();}
	bool has_seeking_weapons() const;
	float visibility()  const {return (1.0 - cloaked);}
	float get_damage_abs()   const {return specs().damage_abs;}
	bool player_controlled() const;
	string get_name()   const;
	string get_info()   const;
	bool rename(string const &name_) {name = name_; return 1;}
	vector3d predict_target_dir(point const &fpos, free_obj const *targ, unsigned wclass=UWEAP_NONE) const;
	u_ship_base const *get_ship_base() const {return this;}

	bool line_int_obj(point const &p1, point const &p2, point *p_int=NULL, float *dscale=NULL) const;
	bool sphere_int_obj(point const &c, float r, intersect_params &ip=def_int_params) const;
	bool ship_int_obj(u_ship const *const ship,  intersect_params &ip=def_int_params) const;
	bool obj_int_obj (free_obj const *const obj, intersect_params &ip=def_int_params) const;
	bool cobjs_int_obj(cobj_vector_t const &cobjs2, free_obj const *const obj, intersect_params &ip=def_int_params) const;
	bool do_sphere_int(point const &sc, float sr, intersect_params &ip, bool &intersects, point const &p_last,
		cobj_vector_t const &cobjs) const;

	virtual bool regen_enabled() const;

private:
	bool out_of_ammo(bool current_only) const {return out_of_ammo_for(curr_weapon, current_only);}
	bool is_enemy(free_obj const *obj) const;
	float get_min_att_dist() const;
};


class orbiting_ship : public u_ship { // planetary defense, defense sat, antimiss drone

private:
	bool GSO, fixed_pos, has_sobj, sobj_liveable, has_decremented_owner;
	unsigned orbiting_type, last_build_time;
	int system_ix, planet_ix, moon_ix;
	float orbit_r, rot_rate, start_angle, angle, sobj_radius, sun_energy;
	vector3d axis;
	point rel_pos, sobj_pos, sun_pos;

public:
	orbiting_ship(unsigned sclass_, unsigned align, bool guardian, s_object const &world_path,
		vector3d const &axis_, point const &start_pos, float rad, float start_ang, float rate);
	void set_angle(float angle) {start_angle = angle;}
	void update_state();
	void set_pos_from_sobj(urev_body const *const sobj);
	void apply_physics();
	void ai_action();
	void advance_time(float timestep) {} // empty
	bool regen_enabled() const;
	bool maybe_has_line_of_sight(upos_point_type const &to_pos) const;
	bool has_clear_line_of_fire(us_weapon const &weap, vector3d const &fire_dir, float target_dist) const;
};


class multipart_ship : public u_ship {

	float state_val;
	cobj_vector_t cobjs;
	vector<ship_sphere> coll_spheres;
	vector<ship_cylinder> coll_cylinders;

public:
	multipart_ship(unsigned sclass_, point const &pos0, unsigned align, unsigned ai_type_, unsigned target_mode_, bool rand_orient);
	cobj_vector_t const &get_cobjs() const {return cobjs;}
	void apply_physics();
	void ai_action();
	void check_size_scale();
	float get_state_val() const {return state_val;}
	void draw_bounding_volume(unsigned ndiv) const;
};


class rand_spawn_ship : public u_ship, public rand_spawn_mixin {

	bool will_respawn;
	unsigned orig_align;

	void gen_valid_pos();
	void destroy_or_respawn();

public:
	rand_spawn_ship(unsigned sclass_, point const &pos0, unsigned align, unsigned ai_type_, unsigned target_mode_, bool rand_orient, bool will_respawn_);
	void apply_physics();
	void explode(float damage, float bradius, int etype, vector3d const &edir, int exp_time, int wclass,
		int align, unsigned eflags=0, free_obj const *parent_=NULL);
	virtual bool claim_world(uobject const *uobj) {return 0;} // can't claim worlds
	virtual string get_name() const {return u_ship::get_name() + " (randomly spawned)";}
	virtual bool is_rand_spawn() const {return 1;}
};


unsigned get_num_chars(unsigned v);
void write_uint_pad(unsigned v, unsigned min_chars, string const &sep=" ");
void update_maxvals(unsigned *v, unsigned n);

#endif

