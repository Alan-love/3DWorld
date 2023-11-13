// 3D World - Pedestrians for Procedural Cities
// by Frank Gennari
// 12/6/18
#include "city.h"
#include "shaders.h"
#include <fstream>

float const CROSS_SPEED_MULT = 1.8; // extra speed multiplier when crossing the road
float const CROSS_WAIT_TIME  = 60.0; // in seconds
float const LOOKAHEAD_TICKS  = 2.0*TICKS_PER_SECOND; // 2s
bool const FORCE_USE_CROSSWALKS = 0; // more realistic and safe, but causes problems with pedestian collisions

bool some_person_has_idle_animation(0);

extern bool tt_fire_button_down;
extern int display_mode, game_mode, animate2, frame_counter, camera_surf_collide;
extern float fticks, FAR_CLIP;
extern double camera_zh;
extern point pre_smap_player_pos;
extern cube_t smap_light_clip_cube;
extern city_params_t city_params;
extern building_params_t global_building_params;
extern object_model_loader_t building_obj_model_loader; // for umbrella model


string gen_random_name(rand_gen_t &rgen, bool for_universe=0); // from Universe_name.cpp
bool in_building_gameplay_mode(); // from building_gameplay.cpp
void get_dead_players_in_building(vector<dead_person_t> &dead_players, building_t const &building); // from building_gameplay.cpp


class person_name_gen_t {
	bool loaded = 0;
	vector<string> male_names, female_names;

	static void load_from_file(string const &fn, vector<string> &names) {
		std::ifstream in(fn.c_str());
		if (!in.good()) return;
		string line;

		while (std::getline(in, line)) {
			if (!line.empty() && line.front() != '#') {names.push_back(line);}
		}
	}
	void ensure_names_loaded() {
		if (loaded) return;
		load_from_file("text_data/male_names.txt",   male_names  );
		load_from_file("text_data/female_names.txt", female_names);
		loaded = 1; // mark as loaded whether or not reading succeeds
	}
public:
	string gen_name(unsigned id, bool is_female, bool inc_first, bool inc_last) {
		assert(inc_first || inc_last);
		rand_gen_t rgen;
		rgen.set_state(id+456, id+123); // use ssn as name rand gen seed
		rgen.rand_mix();
		string name;

		if (inc_first) {
			ensure_names_loaded();
			vector<string> const &names(is_female ? female_names : male_names);
			if (!names.empty()) {name += names[rgen.rand()%names.size()];}
		}
		if (inc_last) {
			if (!name.empty()) {name += " ";} // add a space between first and last names
			name += gen_random_name(rgen); // borrow the universe name generator to assign silly names
		}
		return name;
	}
	string gen_random_first_name(rand_gen_t &rgen) {
		ensure_names_loaded();
		vector<string> const &names(rgen.rand_bool() ? female_names : male_names); // 50% chance of male, 50% chance of female
		return (names.empty() ? "unnamed" : names[rgen.rand()%names.size()]);
	}
};

person_name_gen_t person_name_gen;

string gen_random_first_name(rand_gen_t &rgen) {return person_name_gen.gen_random_first_name(rgen);} // for use in naming other entities
string gen_random_full_name (rand_gen_t &rgen) {return person_name_gen.gen_name(rgen.rand(), rgen.rand_bool(), 1, 1);} // first and last names, random gender

string person_base_t::get_name() const {
	return person_name_gen.gen_name(ssn, is_female, 1, 1); // use ssn as name rand gen seed; include both first and last name
}
string pedestrian_t::str() const { // Note: no label_str()
	std::ostringstream oss;
	oss << get_name() << ": " << TXTn(ssn) << TXT(speed) << TXTn(radius) << TXT(city) << TXT(plot) << TXT(next_plot) << TXT(dest_plot) << TXTn(dest_bldg)
		<< TXTi(stuck_count) << TXT(collided) << TXTn(in_the_road) << TXT(is_stopped) << TXT(at_dest) << TXTn(has_dest_car) << TXT(target_valid())
		<< "wait_time=" << get_wait_time_secs(); // Note: pos, vel, dir not printed
	return oss.str();
}

float pedestrian_t::get_speed_mult() const { // run across the street if there are cars
	return ((in_the_road && city_params.num_cars > 0) ? CROSS_SPEED_MULT : 1.0);
}

void person_base_t::stop() {
	//dir = vel.get_norm(); // ???
	vel = zero_vector;
	anim_time  = 0.0; // reset animation so that ped is standing normally and not mid-stride - should really transition this gradually somehow
	is_stopped = 1;
}
void person_base_t::go() {
	vel = dir * speed; // assumes dir is correct
	is_stopped = 0;
}
void person_base_t::wait_for(float seconds) {
	anim_time     = 0.0; // reset animation
	waiting_start = seconds*TICKS_PER_SECOND; // stop for N seconds
	target_pos    = all_zeros; // clear any previous target
}
float person_base_t::get_idle_anim_time() const { // in animation units
	return (in_building ? idle_time : get_wait_time_ticks())*speed; // will count up for city pedestrians and count down for building people
}
cube_t person_base_t::get_bcube() const {
	cube_t c;
	c.set_from_sphere(pos, get_width());
	set_cube_zvals(c, get_z1(), get_z2());
	return c;
}

float get_sidewalk_width        () {return SIDEWALK_WIDTH*city_params.road_width;} // approx sidewalk width in the texture
float get_sidewalk_walkable_area() {return 0.65*get_sidewalk_width();} // walkable area of sidewalk on the street side; 65%, to avoid streetlights and traffic lights
float get_inner_sidewalk_width  () {return 1.00*get_sidewalk_width();} // walkable area of sidewalk on the plot side (not a sidewalk texture in residential neighborhoods)

// Note: may update plot_bcube and next_plot_bcube
bool pedestrian_t::check_inside_plot(ped_manager_t &ped_mgr, point const &prev_pos, cube_t &plot_bcube, cube_t &next_plot_bcube) {
	//if (ssn == 2516) {cout << "in_the_road: " << in_the_road << ", pos: " << pos.str() << ", plot_bcube: " << plot_bcube.str() << ", npbc: " << next_plot_bcube.str() << endl;}
	if (plot_bcube.contains_pt_xy(pos)) {return 1;} // inside the plot
	stuck_count = 0; // no longer stuck
	if (next_plot == plot) return 0; // no next plot - clip to this plot
	
	if (next_plot_bcube.contains_pt_xy(pos)) {
		ped_mgr.move_ped_to_next_plot(*this);
		next_plot = ped_mgr.get_next_plot(*this);
		get_plot_bcubes_inc_sidewalks(ped_mgr, plot_bcube, next_plot_bcube); // update plot bcubes
		return 1;
	}
	cube_t union_plot_bcube(plot_bcube);
	union_plot_bcube.union_with_cube(next_plot_bcube);
	if (!union_plot_bcube.contains_pt_xy(pos) && union_plot_bcube.contains_pt_xy(prev_pos)) {return 0;} // went outside the valid area
	float const dx(min(fabs(pos.x - plot_bcube.x1()), fabs(pos.x - plot_bcube.x2()))), dy(min(fabs(pos.y - plot_bcube.y1()), fabs(pos.y - plot_bcube.y2())));

	if (max(dx, dy) < 0.75*city_params.road_width) { // near an intersection - near the road in both dims
		at_crosswalk = 1; // Note: should only be at crosswalks; but if we actually are corssing the road, this is the correct thing to do
	}
	in_the_road = 1;
	return 1; // allow peds to cross the road; don't need to check for building or other object collisions
}

bool pedestrian_t::check_road_coll(ped_manager_t const &ped_mgr, cube_t const &plot_bcube, cube_t const &next_plot_bcube) const {
	// Note: streetlights and stoplights now sit on the edges between sidewalks and roads, so they contribute to collisions in both of these areas;
	// this step can only be skipped if the player is inside a plot, and neither on the sidewalk or in the road
	if (!in_the_road && plot_bcube.contains_pt_xy(pos)) return 0;
	float const expand((get_sidewalk_width() - get_sidewalk_walkable_area()) + radius); // max dist from plot edge where a collision can occur
	cube_t pbce(plot_bcube), npbce(next_plot_bcube);
	pbce .expand_by_xy(expand);
	npbce.expand_by_xy(expand);
	if ((!pbce.contains_pt_xy(pos)) && (!npbce.contains_pt_xy(pos))) return 0; // ped is too far from the edge of the road to collide with streetlights or stoplights
	if (ped_mgr.check_isec_sphere_coll       (*this)) return 1;
	if (ped_mgr.check_streetlight_sphere_coll(*this)) return 1;
	return 0;
}

bool pedestrian_t::is_valid_pos(vect_cube_t const &colliders, bool &ped_at_dest, ped_manager_t const *const ped_mgr) const {
	if (in_the_road) return 1; // not in a plot, no collision detection needed
	unsigned building_id(0);

	if (check_buildings_ped_coll(pos, radius, plot, building_id)) {
		if (!has_dest_bldg || building_id != dest_bldg) return 0; // collided with the wrong building
		float const enter_radius(0.25*radius);
		if (!get_building_bcube(dest_bldg).contains_pt_xy_exp(pos, enter_radius)) return 1; // collided with dest building, but not yet entered
		if (!check_buildings_ped_coll(pos, enter_radius, plot, building_id))      return 1; // test this building at a smaller radius to make sure we've entered
		bool const ret(!at_dest);
		ped_at_dest = 1;
		return ret; // only valid if we just reached our dest
	}
	float const xmin(pos.x - radius), xmax(pos.x + radius);

	for (auto i = colliders.begin(); i != colliders.end(); ++i) {
		if (i->x2() < xmin) continue; // to the left
		if (i->x1() > xmax) break; // to the right - sorted from left to right, so no more colliders can intersect - done
		if (!sphere_cube_intersect(pos, radius, *i)) continue;
		if (!has_dest_car || !ped_mgr || plot != dest_plot) return 0; // not looking for car intersection
		
		if (i->intersects_xy(dest_car_center)) { // check if collider is a parking lot car group that contains the dest car
			// Note: here we consider a collision with any car in this block as at destination, even if it's not the dest car;
			// it's possible that the dest car is walled in and surrounded by cars (which may be poorly parked) such that the ped can't reach it without a collision
			if (!ped_mgr->has_car_at_pt(pos, city, 1)) continue; // no car at this location, continue into parking lot (thread safe and faster version)
			//if (ped_mgr->get_car_manager().get_car_at_pt(pos, 1) == nullptr) continue; // no car at this location, continue into parking lot (slow, but not called very often)
			bool const ret(!at_dest);
			ped_at_dest = 1;
			return ret; // only valid if we just reached our dest
		}
		return 0; // bad collision
	} // for i
	return 1;
}

void register_ped_coll(pedestrian_t &p1, pedestrian_t &p2, unsigned pid1, unsigned pid2) {
	p1.collided = p1.ped_coll = 1; p1.colliding_ped = pid2;
	p2.collided = p2.ped_coll = 1; p2.colliding_ped = pid1;
}

bool check_for_ped_future_coll(point const &p1, point const &p2, vector3d const &v1, vector3d const &v2, float r1, float r2) {
	// determine if these two peds will collide within LOOKAHEAD_TICKS time
	point const p1b(p1 + LOOKAHEAD_TICKS*v1), p2b(p2 + LOOKAHEAD_TICKS*v2);
#if 1 // precise, but complex and slow
	return (line_seg_line_seg_dist_2d(p1, p1b, p2, p2b) < (r1 + r2));
#else // conservative: only look at the X and Y separation values, which at least works if peds are walking in X or Y along sidewalks
	cube_t bc1(p1, p1b), bc2(p2, p2b);
	bc1.expand_by_xy(r1); // XY bounding cube of ped collision volume from now to 2s in the future
	bc2.expand_by_xy(r2);
	return bc1.intersects_xy(bc2); // conservative
#endif
}

bool pedestrian_t::check_ped_ped_coll_range(vector<pedestrian_t> &peds, unsigned pid, unsigned ped_start, unsigned target_plot, float prox_radius, vector3d &force) {
	float const prox_radius_sq(prox_radius*prox_radius);

	for (auto i = peds.begin()+ped_start; i != peds.end(); ++i) { // check every ped until we exit target_plot
		if (i->plot != target_plot) break; // moved to a new plot, no collision, done; since plots are globally unique across cities, we don't need to check cities
		float const dist_sq(p2p_dist_xy_sq(pos, i->pos));
		if (dist_sq > prox_radius_sq) continue; // proximity test
		if (i->destroyed) continue; // dead
		float const r1(get_coll_radius()), r2(i->get_coll_radius()), r_sum(r1 + r2);
		if (dist_sq < r_sum*r_sum) {register_ped_coll(*this, *i, pid, (i - peds.begin())); return 1;} // collision
		if (speed < TOLERANCE) continue;
		point const p1_xy(pos.x, pos.y, 0.0), p2_xy(i->pos.x, i->pos.y, 0.0); // z=0.0
		vector3d const delta_v(vel - i->vel), delta_p(p1_xy - p2_xy);
		float const dp(-dot_product_xy(delta_v, delta_p));
		if (dp <= 0.0) continue; // diverging, no avoidance needed
		if (!check_for_ped_future_coll(p1_xy, p2_xy, vel, i->vel, r1, r2)) continue;
		float const dv_mag(delta_v.mag());
		if (dv_mag < TOLERANCE) continue;
		float const dist(sqrt(dist_sq)), fmag(dist/(dist - 0.9*r_sum));
		vector3d const rejection(delta_p - (dp/(dv_mag*dv_mag))*delta_v); // component of velocity perpendicular to delta_p (avoid dir)
		float const rmag(rejection.mag()), rel_vel(max(dv_mag/speed, 0.5f)); // higher when peds are converging
		if (rmag < TOLERANCE) continue;
		float const force_mult(dp/(dv_mag*dist)); // stronger with head-on collisions
		force += rejection*(rel_vel*force_mult*fmag/rmag);
		//cout << TXT(r_sum) << TXT(dist) << TXT(fmag) << ", dv: " << delta_v.str() << ", dp: " << delta_p.str() << ", rej: " << rejection.str() << ", force: " << force.str() << endl;
	} // for i
	return 0;
}

bool pedestrian_t::check_ped_ped_coll(ped_manager_t const &ped_mgr, vector<pedestrian_t> &peds, unsigned pid, float delta_dir) {
	assert(pid < peds.size());
	float const lookahead_dist(LOOKAHEAD_TICKS*speed); // how far we can travel in 2s
	float const prox_radius(1.2*radius + lookahead_dist); // assume other ped has a similar radius
	vector3d force(zero_vector);
	if (check_ped_ped_coll_range(peds, pid, pid+1, plot, prox_radius, force)) return 1;

	if (in_the_road && next_plot != plot) {
		// need to check for coll between two peds crossing the street from different sides, since they won't be in the same plot while in the street
		unsigned const ped_ix(ped_mgr.get_first_ped_at_plot(next_plot));
		assert(ped_ix <= peds.size()); // could be at the end
		if (check_ped_ped_coll_range(peds, pid, ped_ix, next_plot, prox_radius, force)) return 1;
	}
	if (force != zero_vector) {set_velocity((0.1*delta_dir)*force + ((1.0 - delta_dir)/speed)*vel);} // apply ped repulsive force
	return 0;
}

bool pedestrian_t::check_ped_ped_coll_stopped(vector<pedestrian_t> &peds, unsigned pid) {
	assert(pid < peds.size());

	// Note: shouldn't have to check peds in the next plot, assuming that if we're stopped, they likely are as well, and won't be walking toward us
	for (auto i = peds.begin()+pid+1; i != peds.end(); ++i) { // check every ped until we exit target_plot
		if (i->plot != plot) break; // moved to a new plot, no collision, done; since plots are globally unique across cities, we don't need to check cities
		if (!dist_xy_less_than(pos, i->pos, (get_coll_radius() + i->get_coll_radius()))) continue; // no collision
		if (i->destroyed) continue; // dead
		i->collided = i->ped_coll = 1; i->colliding_ped = pid;
		return 1; // Note: could omit this return and continue processing peds
	} // for i
	return 0;
}

point rand_xy_pt_on_cube_edge(cube_t const &c, float radius, rand_gen_t &rgen) {
	bool const dim(rgen.rand_bool()), dir(rgen.rand_bool());
	point pt;
	pt[ dim] = c.d[dim][dir] + (dir ? -1.0 : 1.0)*radius;
	pt[!dim] = rgen.rand_uniform(c.d[!dim][0]+radius, c.d[!dim][1]-radius);
	pt.z     = c.z1();
	return pt;
}
bool pedestrian_t::try_place_in_plot(cube_t const &plot_cube, vect_cube_t const &colliders, unsigned plot_id, rand_gen_t &rgen) {
	pos    = rand_xy_pt_on_cube_edge(plot_cube, radius, rgen);
	pos.z += radius; // place on top of the plot
	plot   = next_plot = dest_plot = plot_id; // set next_plot and dest_plot as well so that they're valid for the first frame
	bool temp_at_dest(0); // we don't want to set at_dest from this call
	if (!is_valid_pos(colliders, temp_at_dest, nullptr)) return 0; // plot == next_plot; return if failed
	return 1; // success
}

float path_finder_t::path_t::calc_length_up_to(const_iterator i) const {
	assert(i <= end());
	float len(0.0);
	for (auto p = begin(); p+1 != i; ++p) {len += p2p_dist(*p, *(p+1));}
	return len;
}
cube_t path_finder_t::path_t::calc_bcube() const { // Note: could probably get away with only x/y bounds
	assert(!empty());
	cube_t bcube(front(), front());
	for (auto i = begin()+1; i != end(); ++i) {bcube.union_with_pt(*i);}
	return bcube;
}

// path_finder_t
bool path_finder_t::add_pt_to_path(point const &p, path_t &path) const {
	if (!plot_bcube.contains_pt_xy(p)) return 0; // outside the plot - invalid
	path.push_back(p);
	return 1;
}

bool path_finder_t::add_pts_around_cube_xy(path_t &path, path_t const &cur_path, path_t::const_iterator p, cube_t const &c, bool dir) {
	point const &n(*(p+1));
	if (c.contains_pt_xy(*p) || c.contains_pt_xy(n)) return 0; // point contained in cube - likely two overlapping/adjacent building cubes - fail
	path.clear();
	path.insert(path.end(), cur_path.begin(), p+1); // add the prefix, including p
	cube_t ec(c);
	ec.expand_by_xy(gap); // expand cubes in x and y
	point const corners [4] = {point( c.x1(),  c.y1(), p->z), point( c.x1(),  c.y2(), p->z), point( c.x2(),  c.y2(), p->z), point( c.x2(),  c.y1(), p->z)}; // CW
	point const ecorners[4] = {point(ec.x1(), ec.y1(), p->z), point(ec.x1(), ec.y2(), p->z), point(ec.x2(), ec.y2(), p->z), point(ec.x2(), ec.y1(), p->z)}; // CW; expanded cube
	vector3d const delta(n - *p); // not normalized
	// find the two closest corners to the left and right
	float min_dp1(0.0), min_dp2(0.0);
	unsigned cix1(4), cix2(4); // start at invalid values
	//cout << TXT(p->x) << TXT(p->y) << TXT(n.x) << TXT(n.y) << TXT(delta.x) << TXT(delta.y) << TXT(c.x1()) << TXT(c.x2()) << TXT(c.y1()) << TXT(c.y2()) << endl;

	for (unsigned i = 0; i < 4; ++i) {
		vector3d const delta2(corners[i] - *p); // unexpanded corners
		float const dp(dot_product_xy(delta, delta2)/delta2.mag());
		bool const turn_dir(cross_product_xy(delta, delta2) < 0.0);
		//cout << TXT(delta2.x) << TXT(delta2.y) << TXT(i) << TXT(dp) << TXT(turn_dir) << endl;
		if ((turn_dir ? cix1 : cix2) == 4 || dp < (turn_dir ? min_dp1 : min_dp2)) {(turn_dir ? min_dp1 : min_dp2) = dp; (turn_dir ? cix1 : cix2) = i;}
	}
	//cout << TXT(dir) << TXT(min_dp1) << TXT(min_dp2) << TXT(cix1) << TXT(cix2) << endl;
	if (cix1 == 4 || cix2 == 4) return 0; // something bad happened (floating-point error?), fail
	unsigned dest_cix(dir ? cix2 : cix1);
	bool const move_dir((((dest_cix+1)&3) == (dir ? cix1 : cix2)) ? 0 : 1); // CCW/CW based on which dir moves around the other side of the cube
	if (check_line_clip_xy(*p, ecorners[dest_cix], c.d)) return 0; // something bad happened (floating-point error?), fail
	if (!add_pt_to_path(ecorners[dest_cix], path))       return 0; // expanded corner

	if (check_line_clip_xy(n, ecorners[dest_cix], c.d)) { // no path to dest, add another point
		if (move_dir) {dest_cix = (dest_cix+1)&3;} else {dest_cix = (dest_cix+3)&3;}
		assert(dest_cix != (dir ? cix1 : cix2));
		if (!add_pt_to_path(ecorners[dest_cix], path)) return 0; // expanded corner

		if (check_line_clip_xy(n, ecorners[dest_cix], c.d)) { // no path to dest, add another point
			if (move_dir) {dest_cix = (dest_cix+1)&3;} else {dest_cix = (dest_cix+3)&3;}
			if (!add_pt_to_path(ecorners[dest_cix], path)) return 0; // expanded corner
			//assert(!check_line_clip_xy(n, ecorners[dest_cix], c.d)); // must have a path now
		}
	}
	path.insert(path.end(), p+1, cur_path.end()); // add the suffix, including p+1
	path.calc_length();
	return 1;
}

void path_finder_t::find_best_path_recur(path_t const &cur_path, unsigned depth) {
	if (depth >= MAX_PATH_DEPTH) return; // depth is too high, fail (stack not allocated)
	if (cur_path.length >= best_path.length) return; // bound (best path length is set to an upper bound even when not valid)
	cube_t const bcube(cur_path.calc_bcube());
	path_t::const_iterator first_int_p(cur_path.end());
	float tmin(1.0);
	unsigned cix(0);

	for (unsigned ix = 0; ix < avoid.size(); ++ix) {
		if (used[ix]) continue; // done with this cube
		cube_t const &c(avoid[ix]);
		if (!c.intersects_xy(bcube)) continue;

		for (auto p = cur_path.begin(); p+1 != cur_path.end() && p <= first_int_p; ++p) { // iterate over line segments up to/including first_int_p, skip last point
			float c_tmin, c_tmax;
			if (!get_line_clip_xy(*p, *(p+1), c.d, c_tmin, c_tmax)) continue;
			if (p < first_int_p || c_tmin < tmin) {first_int_p = p; tmin = c_tmin; cix = ix;} // intersection
		}
	} // for ix
	if (first_int_p != cur_path.end()) {
		assert(tmin < 1.0);
		path_t &next_path(path_stack[depth]);
		assert(!used[cix]);
		used[cix] = 1; // mark this cube used so that we don't try to intersect it again (and to avoid floating-point errors with line adjacency)

		for (unsigned d = 0; d < 2; ++d) {
			if (add_pts_around_cube_xy(next_path, cur_path, first_int_p, avoid[cix], d)) {find_best_path_recur(next_path, depth+1);} // recursive call
		}
		assert(used[cix]);
		used[cix] = 0; // mark cube as unused
		if (first_int_p == cur_path.begin() || found_complete_path()) return; // path is no good, terminate
		// calculate the length of the partial path; add twice the distance we're short (to the destination) as a penalty
		float const partial_len(cur_path.calc_length_up_to(first_int_p+1) + 2.0*p2p_dist(*first_int_p, dest));
		if (partial_len >= partial_path.length) return; // not better
		partial_path.clear();
		partial_path.insert(partial_path.end(), cur_path.begin(), first_int_p+1); // record best partial path seen
		partial_path.length = partial_len;
		return;
	}
	if (cur_path.length < best_path.length) { // this test almost always succeeds
		best_path = cur_path; // if we got here without returning above, this is the best path seen so far
		partial_path.clear(); // not using partial_path after this point
		partial_path.length = 0.0;
	}
}

bool path_finder_t::shorten_path(path_t &path) const {
	if (path.size() <= 2) return 0; // nothing to do
	path_t::iterator i(path.begin()+1), o(i); // skip first point, which we must keep

	for (; i != path.end(); ++i) {
		if (i+1 == path.end()) {*(o++) = *i; continue;} // always keep last point
		point const &a(*(i-1)), &b(*(i+1));
		if (line_int_cubes_xy(a, b, avoid)) {*(o++) = *i;} // keep if removing this point generates an intersection
	}
	if (o == path.end()) return 0; // no update
	path.erase(o, path.end());
	path.calc_length();
	return 1; // shortened
}

bool path_finder_t::find_best_path() {
	used.clear();
	used.resize(avoid.size(), 0);
	best_path.clear();
	partial_path.clear();
	cur_path.clear();
	cur_path.init(pos, dest);
	best_path.length = partial_path.length = 5.0*cur_path.length; // add an upper bound of 4x length to avoid too much recursion
	find_best_path_recur(cur_path, 0); // depth=0
	shorten_path(best_path); // see if we can remove any path points; this rarely has a big effect on path length, so it's okay to save time by doing this after the length test
	shorten_path(partial_path);
	//cout << TXT(avoid.size()) << TXT(cur_path.length) << TXT(best_path.length) << found_path() << endl;
	return found_path();
}

// Note: avoid must be non-overlapping and should be non-adjacent; even better if cubes are separated enough that peds can pass between them (> 2*ped radius)
// return values: 0=failed, 1=valid path, 2=init contained, 3=straight path (no collisions)
unsigned path_finder_t::run(point const &pos_, point const &dest_, cube_t const &plot_bcube_, float gap_, point &new_dest) {
	if (!line_int_cubes_xy(pos_, dest_, avoid)) return 3; // no work to be done, leave dest as it is
	pos = pos_; dest = dest_; plot_bcube = plot_bcube_; gap = gap_;
	//if (any_cube_contains_pt_xy(avoid, dest)) return 0; // invalid dest pos - ignore for now and let path finding deal with it when we get to that pos
	unsigned next_pt_ix(1); // default: point after pos

	if (!plot_bcube.contains_pt_xy(pos)) { // keep pos inside the plot
		plot_bcube.clamp_pt_xy(pos); // clamp point to plot bounds
		next_pt_ix = 0; // start at the new pos
	}
	else { // if there are any other cubes containing pos, move away from this cube; could be initial ped positions, ped pushed by a collision, or some other problem
		for (auto i = avoid.begin(); i != avoid.end(); ++i) {
			if (!i->contains_pt_xy(pos)) continue;
			int const building_id(get_building_bcube_contains_pos(pos)); // check if this avoid cube is a building; does it make sense to cache this across iterations?
			point new_pos;
			float dmin(0.0);

			for (unsigned dim = 0; dim < 2; ++dim) {
				for (unsigned dir = 0; dir < 2; ++dir) {
					float const edge(i->d[dim][dir]), dist(abs(pos[dim] - edge));
					if (dmin > 0.0 && dist >= dmin) continue; // not a better direction
					point cand_pos(pos);
					cand_pos[dim] = i->d[dim][dir];
					if (building_id >= 0 && check_line_coll_building(pos, cand_pos, building_id)) continue; // this dir intersects the building
					new_pos = cand_pos;
					dmin    = dist; // valid direction
				} // for dir
			} // for dim
			if (dmin > 0.0) { // new_pos is valid
				pos = new_pos;
				next_pt_ix = 0; // start at the new pos
				break; // at most one cube should contain pos
			}
		} // for i
	}
	if (!find_best_path()) return 0; // if we fail to find a path, leave new_dest unchanged
	vector<point> const &path(get_best_path());
	assert(next_pt_ix < path.size());
	new_dest = path[next_pt_ix]; // set dest to next point on the best path
	return (next_pt_ix ? 1 : 2); // return 2 for the init contained case
}

cube_t get_avoid_area_for_plot(cube_t const &plot_bcube, float radius) {
	cube_t avoid_area(plot_bcube);
	avoid_area.expand_by_xy(0.5f*radius - (get_inner_sidewalk_width() + get_sidewalk_walkable_area())); // shrink to plot interior, and undo the expand applied to the plot
	return avoid_area;
}

// pedestrian_t
point pedestrian_t::get_dest_pos(cube_t const &plot_bcube, cube_t const &next_plot_bcube, ped_manager_t const &ped_mgr, int &debug_state) const {
	if (is_stopped && target_valid()) {debug_state = 0; return target_pos;} // stay the course (this case only needed for debug drawing)

	if (plot == dest_plot) { // this plot contains our dest building/car
		if (!at_dest && has_dest_bldg) { // not there yet
			cube_t const dest_bcube(get_building_bcube(dest_bldg));
			//if (dest_bcube.contains_pt_xy(pos)) {at_dest = 1;} // could set this here, but requiring a collision also works
			point const dest_pos(dest_bcube.get_cube_center()); // target a door nearest pos? this is done in get_avoid_cubes()
			debug_state = 1;
			return point(dest_pos.x, dest_pos.y, pos.z); // same zval
		}
		else if (!at_dest && has_dest_car) {debug_state = 2; return point(dest_car_center.x, dest_car_center.y, pos.z);} // same zval
	}
	else if (next_plot != plot) { // move toward next plot
		if (!next_plot_bcube.contains_pt_xy(pos)) { // not yet crossed into the next plot
			bool const in_cur_plot(plot_bcube.contains_pt_xy(pos));
			point dest_pos(pos);
			// should cross at intersection, not in the middle of the street; find closest crosswalk (corner of plot_bcube) to dest_pos;
			// while the code below is correct, it tends to make peds collide with the stoplight on the corner and each other and never actually reach their destinations,
			// so we allow peds to cross the street wherever they want until at the very least the stoplights can be moved
			if (FORCE_USE_CROSSWALKS) { // closest corner (crosswalk)
				cube_t const &cube(in_cur_plot ? plot_bcube : next_plot_bcube); // target the corner of the current plot, then the corner of the next plot
				float const val((in_cur_plot ? 0.01 : -0.01)*city_params.road_width); // slightly outside the cur plot / inside the next plot, to ensure a proper transition

				for (unsigned d = 0; d < 2; ++d) { // x,y
					dest_pos[d] = (((pos[d] - cube.d[d][0]) < (cube.d[d][1] - pos[d])) ? (cube.d[d][0] - val) : (cube.d[d][1] + val));
				}
			}
			else { // closest point
				point pos_adj(pos);
				road_plot_t const &cur_plot(ped_mgr.get_city_plot_for_peds(city, plot));
				
				if (cur_plot.is_residential && !cur_plot.is_park) { // residential plot
					cube_t const avoid(get_avoid_area_for_plot(plot_bcube, 2.0*radius)); // 2x radius for extra padding

					if (avoid.contains_pt(pos)) { // move point away from plot edge so that the line to the closest point doesn't clip through the avoid cube
						bool dim(0), dir(0);
						get_closest_dim_dir_xy(cube_t(pos, pos), avoid, dim, dir);
						pos_adj[dim] = avoid.d[dim][dir]; // move to the edge of the avoid cube
					}
				}
				dest_pos = next_plot_bcube.closest_pt(pos_adj);
				debug_state = 3;
			}
			if (!in_cur_plot) { // went outside the current plot
				cube_t union_plot_bcube(plot_bcube);
				union_plot_bcube.union_with_cube(next_plot_bcube);
				// if we went outside on the wrong side, go back inside the current plot, or the union of the current and next plots if in the road
				float const exp(in_the_road ? radius : 0.0); // allow a bit of slack when crossing the road
				if (!union_plot_bcube.contains_pt_xy_exp(pos, exp)) {debug_state = 4; dest_pos = (in_the_road ? union_plot_bcube : plot_bcube).closest_pt(pos);}
				else {debug_state = 5;}
			}
			dest_pos.z = pos.z; // same zval
			return dest_pos;
		} else {debug_state = 6;}
	} else {debug_state = 7;}
	return pos; // no dest
}

bool pedestrian_t::choose_alt_next_plot(ped_manager_t const &ped_mgr) {
	reset_waiting(); // reset waiting state regardless of outcome; we don't want to get here every frame if we fail to find another plot
	if (plot == next_plot) return 0; // no next plot (error?)
	//if (next_plot == dest_plot) return 0; // the next plot is our desination, should we still choose another plot?
	unsigned const cand_next_plot(ped_mgr.get_next_plot(*this, next_plot));
	if (cand_next_plot == next_plot || cand_next_plot == plot) return 0; // failed
	next_plot = cand_next_plot;
	return 1; // success
}

void add_and_expand_ped_avoid_cube(cube_t const &c, vect_cube_t &avoid, float expand, float height) {
	avoid.push_back(c);
	avoid.back().expand_by_xy(expand*((c.dz() < 0.67*height) ? 0.5 : 1.0)); // reduce expand value for short objects that will only collide with our legs
}

void pedestrian_t::get_avoid_cubes(ped_manager_t const &ped_mgr, vect_cube_t const &colliders,
	cube_t const &plot_bcube, cube_t const &next_plot_bcube, point &dest_pos, vect_cube_t &avoid) const
{
	avoid.clear();
	float const height(get_height()), expand(1.1*radius); // slightly larger than radius to leave some room for floating-point error
	road_plot_t const &cur_plot(ped_mgr.get_city_plot_for_peds(city, plot));
	bool const is_home_plot(plot == dest_plot); // plot contains our destination
	if (is_home_plot) {assert(plot_bcube == next_plot_bcube);}
	bool keep_cur_dest(0);

	if (cur_plot.is_residential && !cur_plot.is_park) { // apply special restrictions when walking through a residential block
		cube_t const avoid_area(get_avoid_area_for_plot(plot_bcube, radius));
		bool avoid_entire_plot(0);

		if (is_home_plot) {
			if (city_params.assign_house_plots && (has_dest_bldg || has_dest_car)) { // we can only walk through our own sub-plot
				cube_t dest_cube;
				if      (has_dest_bldg) {dest_cube = get_building_bcube(dest_bldg);}
				else if (has_dest_car ) {dest_cube.set_from_sphere(dest_car_center, city_params.get_nom_car_size().x);} // somewhat approximate/conservative
				assert(dest_cube.intersects_xy(plot_bcube)); // or contains, or is that too strong?
				bool dim(0), dir(0);
				get_closest_dim_dir_xy(dest_cube, plot_bcube, dim, dir);
				cube_t approach_area(dest_cube);
				approach_area.d[dim][dir] = plot_bcube.d[dim][dir]; // expand out to the plot
				approach_area.expand_by_xy(radius); // add a small fudge factor
				bool use_proxy_pt(0);

				if (!approach_area.contains_pt(pos)) { // not in approach area
					if (avoid_area.contains_pt_xy(pos)) { // we're already in the avoid area, get out of it/don't avoid this plot
						// find closest edge of plot_bcube, and use that as our dest;
						// hopefully this works better than using our original destination, which may require us to walk through someone else's yard
						get_closest_dim_dir_xy(get_bcube(), plot_bcube, dim, dir);
						dest_pos[dim] = plot_bcube.d[dim][dir];
						keep_cur_dest = 1;
					}
					else {avoid_entire_plot = use_proxy_pt = 1;} // walk around the plot
				}
				else if (has_dest_bldg && !dist_xy_less_than(pos, dest_pos, radius) && ped_mgr.has_parked_car_on_path(pos, dest_pos, city)) {
					use_proxy_pt = 1; // we're in front of the building, but there's a car in the way, and we're not quite across from the door, so keep walking
				}
				if (use_proxy_pt) { // update dest_pos to use the proxy point along the sidewalk across from our destination as the next path point
					dest_pos[ dim] = plot_bcube.d[dim][dir];
					dest_pos[!dim] = dest_cube.get_center_dim(!dim);
					keep_cur_dest  = 1; // don't select a door below
				}
			} // else we can walk through this plot
		}
		else {avoid_entire_plot = 1;} // not our destination plot, we can't walk through any residential properties

		if (!in_the_road) { // include collider bcubes for cars parked in house driveways
			static vect_cube_t car_bcubes; // reused across calls
			car_bcubes.clear();
			ped_mgr.get_parked_car_bcubes_for_plot(plot_bcube, city, car_bcubes);

			for (auto i = car_bcubes.begin(); i != car_bcubes.end(); ++i) {
				i->expand_by_xy(0.75*radius); // use smaller collision radius
				if (avoid_entire_plot && avoid_area.contains_cube_xy(*i)) continue; // contained within the plot cube we're avoiding
				avoid.push_back(*i);
			}
		}
		if (avoid_entire_plot) {
			avoid.push_back(avoid_area); // this is the highest priority

			for (auto i = colliders.begin(); i != colliders.end(); ++i) { // exclude any cubes contained in the plot, since they're redundant
				if (!avoid_area.contains_cube_xy(*i)) {add_and_expand_ped_avoid_cube(*i, avoid, expand, height);}
			}
			return; // done
		}
	} // else we can walk through this plot
	if (is_home_plot && has_dest_bldg && !keep_cur_dest) {
		// target a building door; if it's on the wrong side of the building we'll still collide with the building when walking to it;
		// it would be nice to have the door open when the ped enters, but it's not easy to do this with the way peds and buildings are separate and in different threads
		point door_pos;
		if (get_building_door_pos_closest_to(dest_bldg, pos, door_pos)) {dest_pos.x = door_pos.x; dest_pos.y = door_pos.y;}
	}
	get_building_bcubes(cur_plot, avoid);
	expand_cubes_by_xy(avoid, expand); // expand building cubes in x and y to approximate a cylinder collision (conservative)
	//remove_cube_if_contains_pt_xy(avoid, pos); // init coll cases (for example from previous dest_bldg) are handled by path_finder_t
	if (is_home_plot && has_dest_bldg) {remove_cube_if_contains_pt_xy(avoid, dest_pos);} // exclude our dest building, we do want to collide with it

	for (auto i = colliders.begin(); i != colliders.end(); ++i) { // check colliders for this plot
		if (is_home_plot && has_dest_car && i->contains_pt_xy(dest_pos)) continue; // exclude our dest car, we do want to collide with it
		add_and_expand_ped_avoid_cube(*i, avoid, expand, height);
	}
}

bool pedestrian_t::check_for_safe_road_crossing(ped_manager_t const &ped_mgr, cube_t const &plot_bcube, cube_t const &next_plot_bcube, vect_cube_t *dbg_cubes) const {
	if (!in_the_road || speed < TOLERANCE) return 1;
	float const sw_width(get_sidewalk_width());
	if (!plot_bcube.closest_dist_xy_less_than(pos, sw_width)) return 1; // too far into the road to turn back (should this use get_sidewalk_walkable_area()?)
	cube_t union_plot_bcube(plot_bcube);
	union_plot_bcube.union_with_cube(next_plot_bcube); // this is the area the ped is constrained to (both plots + road in between)
	if (!union_plot_bcube.contains_pt_xy(pos)) return 1; // not crossing between plots - must be in the road, go back to the sidewalk
	// just exited the plot and about the cross the road - check for cars; use speed rather than vel in case we're already stopped and vel==zero_vector
	float const dx(min((pos.x - plot_bcube.x1()), (plot_bcube.x2() - pos.x))), dy(min((pos.y - plot_bcube.y1()), (plot_bcube.y2() - pos.y)));
	bool const road_dim(dx < dy); // if at crosswalk, need to know which direction/road the ped is crossing
	float const time_to_cross((city_params.road_width - 2.0f*sw_width)/(speed*get_speed_mult())); // road area where cars can drive excluding sidewalks on each side
	//cout << "plot_bcube: " << plot_bcube.str() << " " << TXT(dx) << TXT(dy) << TXT(road_dim) << TXT(time_to_cross) << endl;
	return !ped_mgr.has_nearby_car(*this, road_dim, time_to_cross, dbg_cubes);
}

void pedestrian_t::move(ped_manager_t const &ped_mgr, cube_t const &plot_bcube, cube_t const &next_plot_bcube, float &delta_dir) {
	if (in_the_road) {
		if (!check_for_safe_road_crossing(ped_mgr, plot_bcube, next_plot_bcube)) {stop(); return;}
	}
	else if (city_params.cars_use_driveways) { // in a plot; check for cars if about to enter a driveway that's in use
		float const sw_width(get_sidewalk_width());
		dw_query_t const dw(ped_mgr.get_nearby_driveway(city, plot, pos, max(sw_width, radius)));

		if (dw.driveway != nullptr && dw.driveway->in_use == 1) { // crossing into a driveway used/reserved by a non-parked car
			bool const ddim(dw.driveway->dim), ddir(dw.driveway->dir);
			cube_t dw_extend(*dw.driveway);
			dw_extend.d[ddim][ddir] += (ddir ? 1.0 : -1.0)*sw_width; // extend to include the sidewalk
			cube_t dw_wider(dw_extend);
			dw_wider.expand_in_dim(!ddim, 0.25*dw.driveway->get_width());

			// check for crossing the side (not end) of the driveway this frame; use next pos assuming we're not stopped
			if (dw_extend.contains_pt_xy(pos)) {
				// would this deadlock if the car and ped are waiting on each other?
				dw.driveway->mark_ped_this_frame(); // flag the driveway as blocked so that cars don't pull into it
			}
			else if (dw_wider.contains_pt_xy(pos) && dw_extend.contains_pt_xy(pos + 1.5f*fticks*dir*speed)) {
				car_base_t const *const car(ped_mgr.find_car_using_driveway(city, dw));

				if (car != nullptr && !car->is_parked()) { // car using this driveway, not parked (though there shouldn't be any parked cars returned, car should be null if parked)
					cube_t query_cube(dw.driveway->extend_across_road());

					if (car->dest_driveway == (int)dw.dix && car->dim != ddim) { // car turning/entering driveway
						query_cube.d[!ddim][!car->dir] -= 1.0*(car->dir ? 1.0 : -1.0)*city_params.road_width; // extend for car lead distance
					}
					if (query_cube.intersects_xy(car->bcube)) { // car entering or leaving driveway
						//if (city_single_cube_visible_check(pos, car->bcube) {} // we could check this, but it's generally always true
						stop();
						return;
					}
				}
			}
		}
	}
	reset_waiting();
	if (is_stopped) {go();}

	if (target_valid()) { // if facing away from the target, rotate in place rather than moving in a circle
		vector3d const delta(target_pos - pos);
		float const dist(delta.mag());
		if (dist > radius && dot_product_xy(vel, delta) < 0.01*speed*dist) {delta_dir = min(1.0f, 4.0f*delta_dir); return;} // rotate faster
	}
	float const timestep(min(fticks, 4.0f)*get_speed_mult()); // clamp fticks to 100ms
	pos       += timestep*vel;
	anim_time += timestep*speed;
}

void pedestrian_t::run_path_finding(ped_manager_t &ped_mgr, cube_t const &plot_bcube, cube_t const &next_plot_bcube, vect_cube_t const &colliders, vector3d &dest_pos) {
	vect_cube_t &avoid(ped_mgr.path_finder.get_avoid_vector());
	get_avoid_cubes(ped_mgr, colliders, plot_bcube, next_plot_bcube, dest_pos, avoid);
	target_pos = all_zeros;
	cube_t union_plot_bcube(plot_bcube);
	union_plot_bcube.union_with_cube(next_plot_bcube); // this is the area the ped is constrained to (both plots + road in between)
	// run path finding between pos and dest_pos using avoid cubes
	if (ped_mgr.path_finder.run(pos, dest_pos, union_plot_bcube, 0.1*radius, dest_pos)) {target_pos = dest_pos;}
}

void pedestrian_t::get_plot_bcubes_inc_sidewalks(ped_manager_t const &ped_mgr, cube_t &plot_bcube, cube_t &next_plot_bcube) const {
	// this approach is more visually pleasing because pedestrians will actually walk on the edges of the roads on what appears to be the sidewalks;
	// unfortunately, they also run into streetlights, traffic lights, and each other in this narrow area
	plot_bcube      = ped_mgr.get_city_plot_for_peds(city, plot);
	next_plot_bcube = ped_mgr.get_city_plot_for_peds(city, next_plot);
	float const sidewalk_width(get_sidewalk_walkable_area());
	plot_bcube.expand_by_xy(sidewalk_width);
	next_plot_bcube.expand_by_xy(sidewalk_width);
}

void pedestrian_t::next_frame(ped_manager_t &ped_mgr, vector<pedestrian_t> &peds, unsigned pid, rand_gen_t &rgen, float delta_dir) {
	if (destroyed)    return; // destroyed
	if (speed == 0.0) return; // not moving, no update needed

	// navigation with destination
	if (at_dest) {
		register_at_dest();
		ped_mgr.choose_new_ped_plot_pos(*this);
	}
	else if (!has_dest_bldg && !has_dest_car) {ped_mgr.choose_dest_building_or_parked_car(*this);}
	if (at_crosswalk) {ped_mgr.mark_crosswalk_in_use(*this);}
	cube_t plot_bcube, next_plot_bcube;
	get_plot_bcubes_inc_sidewalks(ped_mgr, plot_bcube, next_plot_bcube);
	// movement logic
	point const prev_pos(pos); // assume this ped starts out not colliding
	move(ped_mgr, plot_bcube, next_plot_bcube, delta_dir);

	if (is_stopped) { // ignore any collisions and just stand there, keeping the same target_pos; will go when path is clear
		if (get_wait_time_secs() > CROSS_WAIT_TIME && choose_alt_next_plot(ped_mgr)) { // give up and choose another destination if waiting for too long
			target_pos = all_zeros;
			go(); // back up or turn so that we don't walk forward into the street? move() should attempt to rotate in place
		}
		else {
			check_ped_ped_coll_stopped(peds, pid); // still need to check for other peds colliding with us; this doesn't always work
			collided = ped_coll = 0;
			return;
		}
	}
	// reset state for next frame; these may be set back to 1 below; don't reset if we were collided with, since this will skip the check_inside_plot() call below
	if (!collided) {at_crosswalk = in_the_road = 0;}
	vect_cube_t const &colliders(ped_mgr.get_colliders_for_plot(city, plot));
	bool outside_plot(0);

	if (collided) {} // already collided with a previous ped this frame, handled below
	// this call will set at_crosswalk and in_the_road
	else if (!check_inside_plot(ped_mgr, prev_pos, plot_bcube, next_plot_bcube)) {collided = outside_plot = 1;} // outside the plot, treat as a collision with the plot bounds
	else if (!is_valid_pos(colliders, at_dest, &ped_mgr))           {collided = 1;} // collided with a static collider
	else if (check_road_coll(ped_mgr, plot_bcube, next_plot_bcube)) {collided = 1;} // collided with something in the road (stoplight, streetlight, etc.)
	else if (check_ped_ped_coll(ped_mgr, peds, pid, delta_dir))     {collided = 1;} // collided with another pedestrian
	else { // no collisions
		//cout << TXT(pid) << TXT(plot) << TXT(dest_plot) << TXT(next_plot) << TXT(at_dest) << TXT(delta_dir) << TXT((unsigned)stuck_count) << TXT(collided) << endl;
		int debug_state(0); // unused
		vector3d dest_pos(get_dest_pos(plot_bcube, next_plot_bcube, ped_mgr, debug_state));

		if (dest_pos != pos) {
			bool update_path(0);
			if (dist_less_than(pos, get_camera_pos(), 1000.0*radius)) { // nearby pedestrian - higher update rate
				update_path = (((frame_counter + ssn) & 15) == 0 || (target_valid() && dist_xy_less_than(pos, target_pos, radius)));
			}
			else { // distant pedestrian - lower update rate
				update_path = (((frame_counter + ssn) & 63) == 0);
			}
			// run only every several frames to reduce runtime; also run when at dest and when close to the current target pos or at the destination
			if (at_dest || update_path) {run_path_finding(ped_mgr, plot_bcube, next_plot_bcube, colliders, dest_pos);}
			else if (target_valid()) {dest_pos = target_pos;} // use previous frame's dest if valid
			vector3d dest_dir((dest_pos.x - pos.x), (dest_pos.y - pos.y), 0.0); // zval=0, not normalized
			float const dmag(dest_dir.xy_mag());

			if (speed > TOLERANCE && dmag > TOLERANCE) { // avoid divide-by-zero
				dest_dir /= dmag; // normalize
				// if destination is in exactly the opposite dir, pick an orthogonal direction using our SSN to decide which way deterministically
				if (dot_product_xy(dest_dir, vel) < -0.99*speed) {dest_dir = cross_product(vel, plus_z*((ssn&1) ? 1.0 : -1.0)).get_norm();}
				set_velocity((0.1*delta_dir)*dest_dir + ((1.0 - delta_dir)/speed)*vel); // slowly blend in destination dir (to avoid sharp direction changes)
			}
		}
		stuck_count = 0;
	}
	if (collided) { // collision
		if (!outside_plot) {
			point const cur_pos(pos);
			pos = prev_pos; // restore to previous valid pos unless we're outside the plot
			// if prev pos is also invalid, undo the restore to avoid getting this ped stuck in a collision object
			if (!is_valid_pos(colliders, at_dest, &ped_mgr) || check_road_coll(ped_mgr, plot_bcube, next_plot_bcube)) {pos = cur_pos;}
		}
		vector3d new_dir;

		if (++stuck_count > 8) {
			int debug_state(0); // unused
			if (target_valid()) {pos += (0.1*radius)*(target_pos - pos).get_norm();} // move toward target_pos if it's valid since this should be a good direction
			else if (stuck_count > 100) {pos += (0.1*radius)*(get_dest_pos(plot_bcube, next_plot_bcube, ped_mgr, debug_state) - pos).get_norm();} // move toward dest if stuck count is high
			else {pos += rgen.signed_rand_vector_spherical_xy()*(0.1*radius); } // shift randomly by 10% radius to get unstuck
		}
		if (ped_coll) {
			assert(colliding_ped < peds.size());
			pedestrian_t const &other(peds[colliding_ped]);
			vector3d const coll_dir(other.pos - pos);
			new_dir = cross_product(vel, plus_z); // right angle turn - using the tangent causes peds to get stuck together
			if (dot_product_xy(new_dir, coll_dir) > 0.0) {new_dir.negate();} // orient away from the other ped's position
			if (dot_product_xy(new_dir, other.vel)/(new_dir.mag()*other.vel.mag()) > 0.9) {new_dir.negate();} // velocities too close together (stuck together?)
		}
		else if (outside_plot && !in_the_road && !plot_bcube.contains_pt_xy(pos)) { // attempt to re-enter the plot at the nearest point
			point plot_pt(pos);
			plot_bcube.clamp_pt_xy(plot_pt);
			new_dir = (plot_pt - pos).get_norm();
		}
		else { // static object collision (should be rare if path_finder does a good job), or in_the_road (need this to get around traffic lights, etc.)
			new_dir = rgen.signed_rand_vector_spherical_xy(); // try a random new direction
			if (dot_product_xy(vel, new_dir) > 0.0) {new_dir.negate();} // negate if pointing in the same dir
		}
		if (new_dir != zero_vector) {set_velocity(new_dir);}
		target_pos = all_zeros; // reset and force path finding to re-route from this new direction/pos
	}
	if (vel != zero_vector) { // if stopped, don't update dir
		if (target_valid()) {delta_dir *= 4.0;} // use a tighter turning radius when there's a valid target_pos
		delta_dir = min(1.0f, delta_dir*get_speed_mult()); // tighter turn radius when moving quickly in the road
		dir   = (delta_dir/speed)*vel + (1.0 - delta_dir)*dir; // merge velocity into dir gradually for smooth turning
		dir.z = 0.0; // should be zero, but set just in case
		float const xy_mag_inv(1.0/dir.xy_mag()); // normalize using XY only
		dir.x *= xy_mag_inv; dir.y *= xy_mag_inv;
	}
	collided = ped_coll = 0; // reset for next frame
}

void pedestrian_t::register_at_dest() {
	assert(plot == dest_plot);
	//cout << get_name() << " at destination " << (has_dest_car ? "car " : (has_dest_bldg ? "building " : "")) << dest_bldg << " in plot " << dest_plot << endl; // placeholder
}

bool person_base_t::is_close_to_player() const { // for debug printouts, etc.
	return dist_less_than((pos + get_tiled_terrain_model_xlate()), get_camera_pos(), 4.0*radius);
}


// somewhat of a hack, but works with current set of models because Katie kid model is the only female with a scale of 0.7, men have a scale of 1.0, and women have a scale of 0.9
bool is_female_model(city_model_t const &model) {return (model.scale <= 0.95);}

unsigned ped_model_loader_t::num_models() const {return city_params.ped_model_files.size();}

city_model_t const &ped_model_loader_t::get_model(unsigned id) const {
	assert(id < num_models());
	return city_params.ped_model_files[id];
}
city_model_t &ped_model_loader_t::get_model(unsigned id) {
	assert(id < num_models());
	return city_params.ped_model_files[id];
}
// pref_gender: 0=male, 1=female, 2=no preference
int ped_model_loader_t::select_random_model(int rand_val, bool choose_zombie, unsigned pref_gender) {
	if (num_models() > 0 && zombie_models.empty() && people_models.empty()) { // first call - not setup
		for (unsigned i = 0; i < num_models(); ++i) {(city_params.ped_model_files[i].is_zombie ? zombie_models : people_models).push_back(i);}
	}
	vector<unsigned> const &pool(choose_zombie ? zombie_models : people_models);
	unsigned ret(0);

	if (pool.empty()) { // no candidate found, try selecting from the other set
		vector<unsigned> const &alt(choose_zombie ? people_models : zombie_models);
		if (alt.empty()) return 0; // no models found
		ret = alt[rand_val % alt.size()];
	}
	else {
		if (choose_zombie) {rand_val ^= 0xdeadbeef;} // mix up the random value to decorrelate models when the people and zombie sets are the same size
		// start with our randomly selected model and try to find a model of the correct gender;
		// will loop around to the first model again and select the original model if no matching gender is found
		for (unsigned n = 0; n < pool.size(); ++n) {
			ret = pool[(rand_val + n) % pool.size()];
			if (pref_gender >= 2 || (unsigned)is_female_model(get_model(ret)) == pref_gender) break;
		}
	}
	assert(ret < num_models());
	return ret;
}

void ped_city_vect_t::add_ped(pedestrian_t const &ped, unsigned road_ix) {
	if (ped.city >= peds.size()) {peds.resize(ped.city+1);} // allocate city if needed
	auto &city(peds[ped.city]);
	if (road_ix  >= city.size()) {city.resize(road_ix+1 );} // allocate road if needed
	city[road_ix].emplace_back(ped.pos, ped.radius);
}
void ped_city_vect_t::clear() {
	for (auto i = peds.begin(); i != peds.end(); ++i) {
		for (auto j = i->begin(); j != i->end(); ++j) {j->clear();}
	}
}


// ped_manager_t
/*static*/ float ped_manager_t::get_ped_radius() {return 0.05*city_params.road_width;} // or should this be relative to player/camera radius?

void ped_manager_t::expand_cube_for_ped(cube_t &cube) const {
	float const radius(get_ped_radius());
	cube.expand_by_xy(radius); // PED_WIDTH_SCALE*radius for models?
	cube.z2() += PED_HEIGHT_SCALE*radius;
}

void ped_manager_t::init(unsigned num_city) {
	if (num_city == 0) return;
	timer_t timer("Gen Peds");
	peds.reserve(num_city);
	float const radius(get_ped_radius()); // base radius

	// place pedestrians in cities
	for (unsigned n = 0; n < num_city; ++n) {
		pedestrian_t ped(radius); // start with a constant radius
		assign_ped_model(ped);

		if (gen_ped_pos(ped)) {
			if (city_params.ped_speed > 0.0) {
				ped.speed = city_params.ped_speed*rgen.rand_uniform(0.5, 1.0);
				ped.vel   = rgen.signed_rand_vector_spherical_xy_norm()*ped.speed;
			}
			ped.ssn = (unsigned short)peds.size(); // assign init peds index so that all are unique; won't change if peds are reordered
			peds.push_back(ped);
		}
	} // for n
	cout << "City Pedestrians: " << peds.size() << endl; // testing
	sort_by_city_and_plot();
}

void ped_manager_t::assign_ped_model(person_base_t &ped) { // Note: non-const, modifies rgen
	if (ped_model_loader.num_models() == 0) {ped.model_id = 0; return;} // will be unused
	bool const choose_zombie(in_building_gameplay_mode());
	bool const is_new(ped.model_rand_seed == 0);
	unsigned const pref_gender(is_new ? 2 : ped.is_female); // 0=male, 1=female, 2=no preference
	if (is_new) {ped.model_rand_seed = rgen.rand();} // choose once and be consistent when switching between people and zombies
	ped.model_id  = ped_model_loader.select_random_model(ped.model_rand_seed, choose_zombie, pref_gender);
	city_model_t const &model(ped_model_loader.get_model(ped.model_id));
	ped.radius   *= model.scale;
	ped.is_female = is_female_model(model);
	ped.is_zombie = model.is_zombie;
	assert(ped.radius > 0.0); // no zero/negative model scales
}
void ped_manager_t::maybe_reassign_ped_model(person_base_t &ped) {
	bool const choose_zombie(in_building_gameplay_mode());
	city_model_t const &cur_model(ped_model_loader.get_model(ped.model_id));
	
	if (cur_model.is_zombie != choose_zombie) {
		float const old_radius(ped.radius);
		ped.radius /= cur_model.scale; // undo the previous model's scale
		assign_ped_model(ped);
		ped.pos.z += (ped.radius - old_radius); // adjust height based on new radius
	}
}
void ped_manager_t::maybe_reassign_models() { // called when switching between normal/people and gameplay/zombies modes
	if (!ped_model_loader.has_mix_of_model_types()) return;
	bool const choose_zombie(in_building_gameplay_mode());
	if (prev_choose_zombie == choose_zombie) return; // no state change (optimization)
	prev_choose_zombie = choose_zombie;
	for (pedestrian_t &ped : peds) {maybe_reassign_ped_model(ped);}
}
void ped_manager_t::maybe_reassign_building_models(building_t &building) {
	if (!ped_model_loader.has_mix_of_model_types()) return;
	for (person_t &person : building.interior->people) {maybe_reassign_ped_model(person);}
}

struct ped_by_plot {
	bool operator()(pedestrian_t const &a, pedestrian_t const &b) const {return (a.plot < b.plot);}
};

void ped_manager_t::sort_by_city_and_plot() {
	//timer_t timer("Ped Sort"); // 0.12ms
	if (peds.empty()) return;
	bool const first_sort(by_city.empty()); // since peds can't yet move between cities, we only need to sorty by city the first time

	if (first_sort) { // construct by_city
		sort(peds.begin(), peds.end());
		unsigned const max_city(peds.back().city), max_plot(peds.back().plot);
		by_city.resize(max_city + 2); // one per city + terminator
		need_to_sort_city.resize(max_city+1, 0);

		for (unsigned city = 0, pix = 0; city <= max_city; ++city) {
			while (pix < peds.size() && peds[pix].city == city) {++pix;}
			unsigned const cur_plot((pix < peds.size()) ? peds[pix].plot : max_plot+1);
			by_city[city+1].assign(pix, cur_plot); // next city begins here
		}
	}
	else { // sort by plot within each city
		for (unsigned city = 0; city+1 < by_city.size(); ++city) {
			if (!need_to_sort_city[city]) continue;
			need_to_sort_city[city] = 0;
			sort((peds.begin() + by_plot[by_city[city].plot_ix]), (peds.begin() + by_plot[by_city[city+1].plot_ix]), ped_by_plot());
		}
	}
	// construct by_plot
	unsigned const max_plot(peds.back().plot);
	by_plot.resize((max_plot + 2), 0); // one per by_plot + terminator

	for (unsigned plot = 0, pix = 0; plot <= max_plot; ++plot) {
		while (pix < peds.size() && peds[pix].plot == plot) {++pix;}
		by_plot[plot+1] = pix; // next plot begins here
	}
	need_to_sort_peds = 0; // peds are now sorted
}

bool ped_manager_t::proc_sphere_coll(point &pos, float radius, vector3d *cnorm) const { // Note: no p_last; for potential use with ped/ped collisions
	float const rsum(get_ped_radius() + radius);

	for (unsigned city = 0; city+1 < by_city.size(); ++city) {
		cube_t const city_bcube(get_expanded_city_bcube_for_peds(city));
		if (pos.z > city_bcube.z2() + rsum) continue; // above the peds
		if (!sphere_cube_intersect_xy(pos, radius, city_bcube)) continue;

		for (unsigned plot = by_city[city].plot_ix; plot < by_city[city+1].plot_ix; ++plot) {
			cube_t const plot_bcube(get_expanded_city_plot_bcube_for_peds(city, plot));
			if (!sphere_cube_intersect_xy(pos, radius, plot_bcube)) continue;
			unsigned const ped_start(by_plot[plot]), ped_end(by_plot[plot+1]);

			for (unsigned i = ped_start; i < ped_end; ++i) { // peds iteration
				assert(i < peds.size());
				if (!dist_less_than(pos, peds[i].pos, rsum)) continue;
				if (cnorm) {*cnorm = (pos - peds[i].pos).get_norm();}
				return 1; // return on first coll
			}
		} // for plot
	} // for city
	return 0;
}

bool ped_manager_t::line_intersect_peds(point const &p1, point const &p2, float &t) const {
	bool ret(0);

	for (unsigned city = 0; city+1 < by_city.size(); ++city) {
		if (!get_expanded_city_bcube_for_peds(city).line_intersects(p1, p2)) continue;

		for (unsigned plot = by_city[city].plot_ix; plot < by_city[city+1].plot_ix; ++plot) {
			if (!get_expanded_city_plot_bcube_for_peds(city, plot).line_intersects(p1, p2)) continue;
			unsigned const ped_start(by_plot[plot]), ped_end(by_plot[plot+1]);

			for (unsigned i = ped_start; i < ped_end; ++i) { // peds iteration
				assert(i < peds.size());
				float tmin(0.0);
				if (line_sphere_int_closest_pt_t(p1, p2, peds[i].pos, peds[i].radius, tmin) && tmin < t) {t = tmin; ret = 1;}
			}
		} // for plot
	} // for city
	return ret;
}

void ped_manager_t::destroy_peds_in_radius(point const &pos_in, float radius) {
	point const pos(pos_in - get_camera_coord_space_xlate());
	bool const is_pt(radius == 0.0);
	float const rsum(get_ped_radius() + radius);

	for (unsigned city = 0; city+1 < by_city.size(); ++city) {
		cube_t const city_bcube(get_expanded_city_bcube_for_peds(city));
		if (pos.z > city_bcube.z2() + rsum) continue; // above the peds
		if (is_pt ? !city_bcube.contains_pt_xy(pos) : !sphere_cube_intersect_xy(pos, radius, city_bcube)) continue;

		for (unsigned plot = by_city[city].plot_ix; plot < by_city[city+1].plot_ix; ++plot) {
			cube_t const plot_bcube(get_expanded_city_plot_bcube_for_peds(city, plot));
			if (is_pt ? !plot_bcube.contains_pt_xy(pos) : !sphere_cube_intersect_xy(pos, radius, plot_bcube)) continue;
			unsigned const ped_start(by_plot[plot]), ped_end(by_plot[plot+1]);

			for (unsigned i = ped_start; i < ped_end; ++i) { // peds iteration
				assert(i < peds.size());
				if (!dist_less_than(pos, peds[i].pos, rsum)) continue;
				peds[i].destroy();
				ped_destroyed = 1;
			}
		} // for plot
	} // for city
}

void ped_manager_t::remove_destroyed_peds() {
	//remove_destroyed(peds); // invalidates indexing, can't do this yet
	ped_destroyed = 0;
}

void ped_manager_t::register_ped_new_plot(pedestrian_t const &ped) {
	if (!need_to_sort_city.empty()) {need_to_sort_city[ped.city] = 1;}
	need_to_sort_peds = 1;
}
void ped_manager_t::move_ped_to_next_plot(pedestrian_t &ped) {
	if (ped.next_plot == ped.plot) return; // already there (error?)
	ped.plot = ped.next_plot; // assumes plot is adjacent; doesn't actually do any moving, only registers the move
	register_ped_new_plot(ped);
}

void ped_manager_t::next_frame() {
	if (!animate2) return; // nothing to do (only applies to moving peds)
	float const delta_dir(1.2*(1.0 - pow(0.7f, fticks))); // controls pedestrian turning rate
	// Note: peds and peds_b can be processed in parallel, but that doesn't seem to make a significant difference in framerate
	// update people in buildings first, so that it can overlap with car sort and spend less time in the modify_car_data critical section
	update_building_ai_state(delta_dir);

	if (!peds.empty()) {
		//timer_t timer("Ped Update"); // ~4.2ms for 10K peds; 1ms for sparse per-city update
		// Note: should make sure this is after sorting cars, so that road_ix values are actually in order; however, that makes things slower, and is unlikely to make a difference
#pragma omp critical(modify_car_data)
		car_manager.extract_car_data(cars_by_city);

		if (ped_destroyed) {remove_destroyed_peds();} // at least one ped was destroyed in the previous frame - remove it/them
		maybe_reassign_models();
		static bool first_frame(1);
		float const enable_ai_dist(1.0f*(X_SCENE_SIZE + Y_SCENE_SIZE));
		point const camera_bs(get_camera_building_space());

		if (first_frame) { // choose initial ped destinations (must be after building setup, etc.)
			for (auto i = peds.begin(); i != peds.end(); ++i) {choose_dest_building_or_parked_car(*i);}
		}
		for (unsigned city = 0; city+1 < by_city.size(); ++city) {
			if (!get_expanded_city_bcube_for_peds(city).closest_dist_less_than(camera_bs, enable_ai_dist)) continue; // too far from the player
			unsigned const ped_start(by_city[city].ped_ix), ped_end(by_city[city+1].ped_ix);
			assert(ped_start <= ped_end && ped_end <= peds.size());
				
			for (auto i = peds.begin()+ped_start; i != peds.begin()+ped_end; ++i) {
				i->next_frame(*this, peds, (i - peds.begin()), rgen, delta_dir);
			}
		} // for city
		if (need_to_sort_peds) {sort_by_city_and_plot();}
		first_frame = 0;
	}
}

pedestrian_t const *ped_manager_t::get_ped_at(point const &p1, point const &p2) const { // Note: p1/p2 in local TT space
	for (unsigned city = 0; city+1 < by_city.size(); ++city) {
		if (!get_expanded_city_bcube_for_peds(city).line_intersects(p1, p2)) continue; // skip

		for (unsigned plot = by_city[city].plot_ix; plot < by_city[city+1].plot_ix; ++plot) {
			if (!get_expanded_city_plot_bcube_for_peds(city, plot).line_intersects(p1, p2)) continue; // skip
			unsigned const ped_start(by_plot[plot]), ped_end(by_plot[plot+1]);

			for (unsigned i = ped_start; i < ped_end; ++i) { // peds iteration
				assert(i < peds.size());
				if (line_sphere_intersect(p1, p2, peds[i].pos, peds[i].radius)) {return &peds[i];}
			}
		} // for plot
	} // for city
	return nullptr; // no ped found
}

void ped_manager_t::get_peds_crossing_roads(ped_city_vect_t &pcv) const {
	//timer_t timer("Get Peds Corssing Roads");
	pcv.clear();

	for (auto i = peds.begin(); i != peds.end(); ++i) {
		if (!i->in_the_road || i->is_stopped || i->destroyed) continue; // not actively crossing the road
		bool const road_dim(fabs(i->vel.y) < fabs(i->vel.x)); // ped should be moving across the road, so velocity should give us the road dim (opposite of velocity dim)
		int const road_ix(get_road_ix_for_ped_crossing(*i, road_dim));
		if (road_ix >= 0) {pcv.add_ped(*i, road_ix);}
	} // for i
}

bool ped_manager_t::has_nearby_car(pedestrian_t const &ped, bool road_dim, float delta_time, vect_cube_t *dbg_cubes) const {
	int const road_ix(get_road_ix_for_ped_crossing(ped, road_dim));
	if (road_ix < 0) return 0; // failed for some reason, assume the answer is no
	// Note: we only use road_ix, not seg_ix, because we need to find cars that are in adjacent segments to the ped (and it's difficult to get seg_ix)
	return has_nearby_car_on_road(ped, road_dim, (unsigned)road_ix, delta_time, dbg_cubes);
}
car_base_t const *ped_manager_t::find_car_using_driveway(unsigned city_ix, dw_query_t const &dw) const {
	if (city_ix >= cars_by_city.size()) return nullptr; // no cars in this city?
	assert(dw.driveway != nullptr);
	car_city_vect_t const &cv(cars_by_city[city_ix]);
	cube_t query_cube(dw.driveway->extend_across_road());

	// this isn't very efficient because the driveway doesn't give us the car, the road, the dim, or the dir
	for (unsigned dim = 0; dim < 2; ++dim) { // since we don't know if the car is pulling into, pulling out of, or backing out of the driveway, we must check both dims
		for (unsigned dir = 0; dir < 2; ++dir) { // look both ways
			auto const &cars(cv.cars[dim][dir]); // cars for this city, in this dim and dir
			
			for (auto c = cars.begin(); c != cars.end(); ++c) { // there should be no parked cars in this vector
				if (c->dest_driveway == (int)dw.dix) return &(*c); // entering driveway (eventually)
				if (c->cur_road_type == TYPE_DRIVEWAY && query_cube.intersects_xy(c->bcube)) return &(*c); // leaving driveway
			}
		} // for dir
	} // for dim
	return nullptr; // not found
}

bool ped_manager_t::has_nearby_car_on_road(pedestrian_t const &ped, bool dim, unsigned road_ix, float delta_time, vect_cube_t *dbg_cubes) const {
	if (ped.city >= cars_by_city.size()) return 0; // no cars in this city? should be rare, unless cars aren't enabled
	car_city_vect_t const &cv(cars_by_city[ped.city]);
	point const &pos(ped.pos);

	for (unsigned dir = 0; dir < 2; ++dir) { // look both ways before crossing
		auto const &cars(cv.cars[dim][dir]); // cars for this city, in this dim and dir
		// Note: this won't check for cars entering the city from a connector road, so we have to rely on the cars checking for peds in this case
		car_base_t ref_car; ref_car.cur_city = ped.city; ref_car.cur_road = road_ix;
		auto range_start(std::lower_bound(cars.begin(), cars.end(), ref_car, comp_car_road())); // binary search acceleration
		float const speed_mult(CAR_SPEED_SCALE*city_params.car_speed), pos_min(pos[dim] - ped.radius), pos_max(pos[dim] + ped.radius);
		auto closest_car(cars.end());

		for (auto it = range_start; it != cars.end(); ++it) {
			car_base_t const &c(*it);
			assert(c.cur_city == ped.city && c.dim == dim && c.dir == (dir != 0));
			if (c.cur_road != road_ix) break; // different road, done
			float const val(c.bcube.d[dim][!dir]); // back end of the car
			if (dir) {if (val > pos_max) break;   } // already passed the ped, not a threat - done (cars are sorted in this dim)
			else     {if (val < pos_min) continue;} // already passed the ped, not a threat - skip to next car
			if (closest_car == cars.end()) {closest_car = it;} // first threatening car
			else {
				float const val2(closest_car->bcube.d[dim][!dir]); // back end of the other car
				if (dir ? (val > val2) : (val < val2)) {closest_car = it;} // this car is closer
			}
			if (!dir) break; // no cars can be closer than this (cars are sorted in this dim)
		} // for it
		if (closest_car == cars.end()) continue; // no car found
		car_base_t const &c(*closest_car);
		float lo(c.bcube.d[dim][0]), hi(c.bcube.d[dim][1]), travel_dist(0.0);

		if (lo >= pos_max || hi <= pos_min) { // current car doesn't already overlap, do more work to determine if it will overlap pos sometime in the near future
			if (c.turn_dir != TURN_NONE) {} // car is turning; since it's already on this road, it must be turning off this road; don't update its future position
			else if (c.stopped_at_light) { // check if the light will change in time for this car to reach pos; would it be easier to check the crosswalk signal?
				float const dist_gap(dir ? (pos_min - hi) : (lo - pos_max)), time_to_close(dist_gap/(speed_mult*c.max_speed));
				if (dist_gap > 0.0 && get_car_isec(c).will_be_green_light_in(c, time_to_close/TICKS_PER_SECOND)) {travel_dist = 1.01*dist_gap;} // move it just enough to cover the gap
			}
			else if (!c.is_stopped()) { // moving and not turning; assume it may be accelerating, and could reach max_speed by the time it passes pos
				//travel_dist = delta_time*speed_mult*c.cur_speed; // Note: inaccurate if car is accelerating
				travel_dist = delta_time*speed_mult*c.max_speed; // conservative - safer
			}
			//max_eq(travel_dist, 0.5f*c.get_length()); // extend by half a car length to avoid letting pedestrians cross in between cars stopped at a light (no longer needed?)
			if (dir) {hi += travel_dist;} else {lo -= travel_dist;}
			assert(lo < hi);
		}
		if (dbg_cubes) {
			cube_t cube(c.bcube);
			cube.d[dim][0] = lo; cube.d[dim][1] = hi;
			dbg_cubes->push_back(cube);
		}
		if (lo < pos_max && hi > pos_min) return 1; // overlaps current or future car in dim
	} // for dir
	return 0;
}

bool ped_manager_t::has_car_at_pt(point const &pos, unsigned city, bool is_parked) const { // intended for use with parking lots
	car_city_vect_t const &cv(get_cars_for_city(city));

	if (is_parked) { // handle parked cars case
		for (auto c = cv.parked_car_bcubes.begin(); c != cv.parked_car_bcubes.end(); ++c) {
			if (c->contains_pt_xy(pos)) return 1;
		}
	}
	else { // check all dims/dirs of non-parked cars
		for (unsigned d = 0; d < 4; ++d) {
			auto const &cars(cv.cars[d>>1][d&1]);

			for (auto c = cars.begin(); c != cars.end(); ++c) {
				if (c->bcube.contains_pt_xy(pos)) return 1;
			}
		}
	}
	return 0;
}

// these two functions are intended for use with cars in driveways
bool ped_manager_t::has_parked_car_on_path(point const &p1, point const &p2, unsigned city) const {
	car_city_vect_t const &cv(get_cars_for_city(city));

	for (unsigned v = 0; v < 2; ++v) { // check both parked and sleeping cars
		vect_cube_with_ix_t const &cars(v ? cv.sleeping_car_bcubes : cv.parked_car_bcubes);

		for (auto c = cars.begin(); c != cars.end(); ++c) {
			if (check_line_clip(p1, p2, c->d)) return 1;
		}
	}
	return 0;
}
void ped_manager_t::get_parked_car_bcubes_for_plot(cube_t const &plot, unsigned city, vect_cube_t &car_bcubes) const {
	car_city_vect_t const &cv(get_cars_for_city(city));

	for (unsigned v = 0; v < 2; ++v) { // check both parked and sleeping cars
		vect_cube_with_ix_t const &cars(v ? cv.sleeping_car_bcubes : cv.parked_car_bcubes);

		for (auto c = cars.begin(); c != cars.end(); ++c) {
			if (c->intersects_xy(plot)) {car_bcubes.push_back(*c);}
		}
	}
}

bool ped_manager_t::choose_dest_parked_car(unsigned city_id, unsigned &plot_id, unsigned &car_ix, point &car_center) {
	car_city_vect_t const &cv(get_cars_for_city(city_id));
	if (cv.parked_car_bcubes.empty()) return 0; // no parked cars; excludes sleeping cars in driveways
	car_ix     = rgen.rand() % cv.parked_car_bcubes.size(); // Note: car_ix is stored in ped dest_bldg and doesn't get used after that
	plot_id    = cv.parked_car_bcubes[car_ix].ix;
	car_center = cv.parked_car_bcubes[car_ix].get_cube_center();
	return 1;
}

// drawing
void begin_ped_sphere_draw(shader_t &s, colorRGBA const &color, bool &in_sphere_draw, bool textured) {
	if (in_sphere_draw) return;
	if (!textured) {select_texture(WHITE_TEX);} // currently not textured
	s.set_cur_color(color);
	begin_sphere_draw(textured);
	in_sphere_draw = 1;
}
void end_sphere_draw(bool &in_sphere_draw) {
	if (!in_sphere_draw) return;
	end_sphere_draw();
	in_sphere_draw = 0;
}

void draw_colored_cube(cube_t const &c, colorRGBA const &color, shader_t &s) {
	s.set_cur_color(color);
	draw_simple_cube(c);
}

void pedestrian_t::debug_draw(ped_manager_t &ped_mgr) const {
	int debug_state(0);
	cube_t plot_bcube, next_plot_bcube;
	get_plot_bcubes_inc_sidewalks(ped_mgr, plot_bcube, next_plot_bcube);
	point const orig_dest_pos(get_dest_pos(plot_bcube, next_plot_bcube, ped_mgr, debug_state));
	point dest_pos(orig_dest_pos);
	if (dest_pos == pos) return; // no path, nothing to draw
	vect_cube_t dbg_cubes;
	bool const safe_to_cross(check_for_safe_road_crossing(ped_mgr, plot_bcube, next_plot_bcube, &dbg_cubes));
	if (!safe_to_cross) {assert(!dbg_cubes.empty());} // must find a blocking car
	path_finder_t path_finder(1); // debug=1
	vect_cube_t const &colliders(ped_mgr.get_colliders_for_plot(city, plot));
	vect_cube_t &avoid(path_finder.get_avoid_vector());
	get_avoid_cubes(ped_mgr, colliders, plot_bcube, next_plot_bcube, dest_pos, avoid);
	cube_t union_plot_bcube(plot_bcube);
	union_plot_bcube.union_with_cube(next_plot_bcube);
	vector<point> path;
	unsigned const ret(path_finder.run(pos, dest_pos, union_plot_bcube, 0.05*radius, dest_pos)); // 0=failed, 1=valid path, 2=init contained, 3=straight path (no collisions)
	bool const at_dest_plot(plot == dest_plot), complete(path_finder.found_complete_path());
	colorRGBA line_color(at_dest_plot ? RED : YELLOW); // paths
	colorRGBA node_color(complete ? YELLOW : ORANGE);

	if (ret == 3) { // straight line
		path.push_back(pos);
		path.push_back(dest_pos);
		if (!at_dest_plot) {line_color = ORANGE; node_color = (safe_to_cross ? GREEN : RED);} // straight line
	}
	else if (ret != 0) {path = path_finder.get_best_path();} // found a path
	vector<vert_color> line_pts;
	shader_t s;
	s.begin_color_only_shader();
	bool in_sphere_draw(0);
	begin_ped_sphere_draw(s, node_color, in_sphere_draw, 0);

	if (ret == 0) { // no path found, show line to dest
		s.set_cur_color(RED);
		draw_simple_cube(union_plot_bcube);
		line_pts.emplace_back(pos,      BROWN);
		line_pts.emplace_back(dest_pos, BROWN);
	}
	else if (ret == 2) { // show segment from current pos to edge of building/car
		assert(!path.empty());
		draw_sphere_vbo(path[0], radius, 16, 0);
		line_pts.emplace_back(pos,     BLUE);
		line_pts.emplace_back(path[0], BLUE);
	}
	for (auto p = path.begin(); p+1 < path.end(); ++p) { // iterate over line segments, skip last point
		point const &n(*(p+1));
		draw_sphere_vbo(n, radius, 16, 0);
		line_pts.emplace_back(*p, line_color);
		line_pts.emplace_back(n,  line_color);
	}
	if (at_dest_plot && (has_dest_car || !complete)) { // show destination when in dest plot with incomplete path or car
		s.set_cur_color(PURPLE);
		draw_sphere_vbo(orig_dest_pos, 1.5*radius, 16, 0);
	}
	end_sphere_draw(in_sphere_draw);

	for (auto i = dbg_cubes.begin(); i != dbg_cubes.end(); ++i) {
		s.set_cur_color((!safe_to_cross && i+1 == dbg_cubes.end()) ? RED : GREEN);
		draw_simple_cube(*i);
	}
	ensure_outlined_polygons();
	s.set_cur_color(CYAN);

	for (auto i = avoid.begin(); i != avoid.end(); ++i) { // draw avoid cubes
		cube_t c(*i);
		max_eq(c.z2(), (c.z1() + radius)); // make sure it's nonzero area
		draw_simple_cube(c);
	}
	if (has_dest_bldg   ) {draw_colored_cube(get_building_bcube(dest_bldg), PURPLE, s);} // draw dest building bcube
	if (collided        ) {draw_colored_cube(get_bcube(), RED, s);} // show marker if collided this frame
	else if (in_the_road) {draw_colored_cube(get_bcube(), GREEN, s);}

	if (1) { // show debug state cube
		colorRGBA const debug_colors[8] = {BLACK, WHITE, RED, GREEN, BLUE, YELLOW, ORANGE, PURPLE};
		cube_t c(get_bcube());
		c.z1() = c.z2(); c.z2() += 0.5*get_height();
		draw_colored_cube(c, debug_colors[debug_state], s);
	}
	if (0) { // show lookahead cube
		point const lookahead_pos(pos + LOOKAHEAD_TICKS*vel);
		cube_t lookahead(pos, (pos + LOOKAHEAD_TICKS*vel));
		lookahead.expand_by_xy(get_coll_radius());
		set_cube_zvals(lookahead, get_z1(), get_z2());
		draw_colored_cube(lookahead, BROWN, s);
	}
	set_fill_mode(); // reset
	draw_verts(line_pts, GL_LINES);
	s.end_shader();
}

void ped_manager_t::next_animation() {
	unsigned const NUM_ANIMATIONS = 7; // procedural animations for people, including null animation
	string const animation_names[NUM_ANIMATIONS] = {"The Slide", "Walking", "The Bunny Hop", "The Flip", "The Twirl", "Marching", "Walk Like an Alien"};
	animation_id = (animation_id + 1) % NUM_ANIMATIONS;
	print_text_onscreen(animation_names[animation_id], WHITE, 1.5, 2*TICKS_PER_SECOND, 1);
}

void ped_manager_t::draw(vector3d const &xlate, bool use_dlights, bool shadow_only, bool is_dlight_shadows) {
	if (peds.empty()) return;
	if (is_dlight_shadows && !city_params.car_shadows) return; // use car_shadows as ped_shadows
	if (shadow_only && !is_dlight_shadows) return; // don't add to precomputed shadow map
	//timer_t timer("Ped Draw"); // ~1ms
	bool const use_models(ped_model_loader.num_models() > 0), enable_animations(use_models);
	float const def_draw_dist((use_models ? 500.0 : 2000.0)*get_ped_radius());
	float const draw_dist(is_dlight_shadows ? 0.8*camera_pdu.far_ : def_draw_dist), draw_dist_sq(draw_dist*draw_dist); // smaller view dist for models
	pos_dir_up pdu(camera_pdu); // decrease the far clipping plane for pedestrians
	pdu.far_     = draw_dist;
	pdu.pos     -= xlate; // adjust for local translate
	dstate.xlate = xlate;
	dstate.set_enable_normal_map(use_models && use_model3d_bump_maps());
	fgPushMatrix();
	translate_to(xlate);
	if (enable_animations) {enable_animations_for_shader(dstate.s);}
	dstate.pre_draw(xlate, use_dlights, shadow_only);
	dstate.s.add_uniform_float("min_alpha", global_building_params.people_min_alpha); // set in case alpha test is enabled
	animation_state_t anim_state(enable_animations, animation_id);
	if (!shadow_only) {dstate.s.add_uniform_float("hemi_lighting_normal_scale", 0.0);} // disable hemispherical lighting normal because the transforms make it incorrect
	bool in_sphere_draw(0);

	for (unsigned city = 0; city+1 < by_city.size(); ++city) {
		if (!pdu.cube_visible(get_expanded_city_bcube_for_peds(city))) continue; // city not visible - skip
		unsigned const plot_start(by_city[city].plot_ix), plot_end(by_city[city+1].plot_ix);
		assert(plot_start <= plot_end);

		for (unsigned plot = plot_start; plot < plot_end; ++plot) {
			assert(plot < by_plot.size());
			cube_t const plot_bcube(get_expanded_city_plot_bcube_for_peds(city, plot));
			if (is_dlight_shadows && !plot_bcube.closest_dist_less_than(pdu.pos, draw_dist)) continue; // plot is too far away
			if (!pdu.cube_visible(plot_bcube)) continue; // plot not visible - skip
			unsigned const ped_start(by_plot[plot]), ped_end(by_plot[plot+1]);
			assert(ped_start <= ped_end);
			if (ped_start == ped_end) continue; // no peds on this plot
			dstate.ensure_shader_active(); // needed for use_smap=0 case
			if (!shadow_only) {dstate.begin_tile(plot_bcube.get_cube_center(), 1);} // use the plot's tile's shadow map

			for (unsigned i = ped_start; i < ped_end; ++i) { // peds iteration
				assert(i < peds.size());
				pedestrian_t const &ped(peds[i]);
				assert(ped.city == city && ped.plot == plot);
				if (ped.destroyed || skip_ped_draw(ped)) continue;
				if (!draw_ped(ped, dstate.s, pdu, xlate, def_draw_dist, draw_dist_sq, in_sphere_draw, shadow_only, is_dlight_shadows, &anim_state, 0)) continue;

				if (dist_less_than(pdu.pos, ped.pos, 0.5*draw_dist)) { // fake AO shadow at below half draw distance
					float const ao_radius(0.6*ped.radius);
					float const zval(get_city_plot_for_peds(ped.city, ped.plot).z2() + 0.04*ped.radius); // at the feet
					point pao[4];
					
					for (unsigned n = 0; n < 4; ++n) {
						point &v(pao[n]);
						v.x = ped.pos.x + (((n&1)^(n>>1)) ? -ao_radius : ao_radius);
						v.y = ped.pos.y + ((n>>1)         ? -ao_radius : ao_radius);
						v.z = zval;
					}
					dstate.ao_qbd.add_quad_pts(pao, colorRGBA(0, 0, 0, 0.4), plus_z);
				}
			} // for i
		} // for plot
	} // for city
	end_sphere_draw(in_sphere_draw);
	anim_state.clear_animation_id(dstate.s);
	if (!shadow_only) {dstate.s.add_uniform_float("hemi_lighting_normal_scale", 1.0);} // restore
	pedestrian_t const *selected_ped(nullptr);

	if (tt_fire_button_down && game_mode != GAME_MODE_FPS) {
		point const p1(get_camera_pos() - xlate), p2(p1 + cview_dir*FAR_CLIP);
		pedestrian_t const *ped(get_ped_at(p1, p2));
		selected_ped_ssn = (ped ? ped->ssn : -1); // update and cache for use in later frames
		if (ped) {dstate.set_label_text(ped->str(), (ped->pos + xlate));} // found
	}
	else if (selected_ped_ssn >= 0) { // a ped was selected, iterate and find it by SSN
		for (auto i = peds.begin(); i != peds.end(); ++i) {
			if (i->ssn == selected_ped_ssn) {selected_ped = &(*i); break;}
		}
		assert(selected_ped); // must be found
	}
	dstate.post_draw();
	if (selected_ped) {selected_ped->debug_draw(*this);}
	fgPopMatrix();
	dstate.show_label_text();
}

person_t ped_manager_t::add_person_to_building(point const &pos, unsigned bix, unsigned ssn) {
	person_t per(get_ped_radius());
	assign_ped_model(per);
	float const angle(rgen.rand_uniform(0.0, TWO_PI));
	per.pos   = pos + vector3d(0.0, 0.0, per.radius);
	per.dir   = vector3d(cosf(angle), sinf(angle), 0.0);
	per.speed = (enable_building_people_ai() ? city_params.ped_speed*rgen.rand_uniform(0.5, 0.75) : 0.0f); // small range, slower than outdoor city pedestrians
	per.ssn   = ssn; // may wrap
	per.cur_bldg = bix; // store building index in dest_bldg field
	return per;
}

void ped_manager_t::gen_and_draw_people_in_building(ped_draw_vars_t const &pdv) {
	if (!pdv.building.interior) return;
	auto &people(pdv.building.interior->people);
	vector<point> locs;
	pdv.building.place_people_if_needed(pdv.bix, get_ped_radius(), locs);
	maybe_reassign_building_models(pdv.building);
	for (point const &p : locs) {people.push_back(add_person_to_building(p, pdv.bix, people.size()));}
	draw_people_in_building(people, pdv);
}

struct cmp_ped_dist_to_pos {
	point const &pos;
	cmp_ped_dist_to_pos(point const &pos_) : pos(pos_) {}
	bool operator()(person_t const *a, person_t const *b) const {return (p2p_dist_sq(pos, a->pos) > p2p_dist_sq(pos, b->pos));} // sorts furthest to nearest
};

void ped_manager_t::draw_people_in_building(vector<person_t> const &people, ped_draw_vars_t const &pdv) {
	vector<dead_person_t> dead_players;
	if (ped_model_loader.num_models() > 0) {get_dead_players_in_building(dead_players, pdv.building);} // get dead players if there's a model to draw
	if (people.empty() && dead_players.empty()) return;
	float const def_draw_dist(120.0*get_ped_radius()); // smaller than city peds
	float const draw_dist(pdv.shadow_only ? camera_pdu.far_ : def_draw_dist), draw_dist_sq(draw_dist*draw_dist);
	bool const enable_occ_cull((display_mode & 0x08) && !city_params.ped_model_files.empty()); // occlusion culling, if using models
	pos_dir_up pdu(camera_pdu); // decrease the far clipping plane for pedestrians
	pdu.pos -= pdv.xlate; // adjust for local translate
	to_draw.clear();

	// Note: no far clip adjustment or draw dist scale
	for (person_t const &p : people) {
		if (skip_bai_draw(p)) continue;
		if (!dist_less_than(pdu.pos, p.pos, draw_dist)) continue; // early exit test
		
		if (pdv.shadow_only) {
			if (p.pos.z > pdu.pos.z) continue; // above the light
			if (p.pos.z < pdu.pos.z - 2.0*pdv.building.get_window_vspace()) continue; // more than two floors below the light
			if (!smap_light_clip_cube.is_all_zeros() && !smap_light_clip_cube.intersects(p.get_bcube() + pdv.xlate)) continue; // shadow map clip cube test
		}
		if (enable_occ_cull && pdv.building.check_obj_occluded(p.get_bcube(), pdu.pos, pdv.oc, pdv.reflection_pass)) continue;
		to_draw.push_back(&p);
	} // for p
	if (to_draw.empty() && dead_players.empty()) return;
	// sort back to front for proper alpha blending when player is in the building bcube, including the extended basement
	if (!pdv.shadow_only && pdv.building.get_bcube_inc_extensions().contains_pt(pdu.pos)) {sort(to_draw.begin(), to_draw.end(), cmp_ped_dist_to_pos(pdu.pos));}
	animation_state_t anim_state(enable_building_people_ai(), animation_id);
	bool in_sphere_draw(0);
	for (person_t const *p : to_draw) {draw_ped(*p, pdv.s, pdu, pdv.xlate, def_draw_dist, draw_dist_sq, in_sphere_draw, pdv.shadow_only, pdv.shadow_only, &anim_state, 1);}
	end_sphere_draw(in_sphere_draw);
	anim_state.clear_animation_id(pdv.s); // make sure to leave animations disabled so that they don't apply to buildings and dead players

	if (!dead_players.empty()) { // have dead player(s)
		unsigned const model_id(get_player_model_id());
		city_model_t const &model(ped_model_loader.get_model(model_id));

		if (model.is_loaded()) {
			for (dead_person_t const &p : dead_players) {
				float const player_eye_height(CAMERA_RADIUS + camera_zh), player_height(player_eye_height/EYE_HEIGHT_RATIO), player_radius(player_height/PED_HEIGHT_SCALE);
				cube_t bcube;
				bcube.set_from_point(p.pos);
				// always facing with head in +X, face down in -Z, and arms out to the sides in a Y T-pose; scale to account for different bcube
				bcube.expand_by(3.0*vector3d(0.5*player_height*model.scale, PED_WIDTH_SCALE*player_radius, PED_WIDTH_SCALE*player_radius));
				ped_model_loader.draw_model(pdv.s, p.pos, bcube, -plus_z, ALPHA0, pdv.xlate, model_id, pdv.shadow_only); // looking down at the ground
			} // for p
		}
	}
	pdv.s.upload_mvm(); // needed after applying model or sphere draw transforms
}

bool ped_manager_t::draw_ped(person_base_t const &ped, shader_t &s, pos_dir_up const &pdu, vector3d const &xlate, float def_draw_dist, float draw_dist_sq,
	bool &in_sphere_draw, bool shadow_only, bool is_dlight_shadows, animation_state_t *anim_state, bool is_in_building)
{
	float const dist_sq(p2p_dist_sq(pdu.pos, ped.pos));
	if (dist_sq > draw_dist_sq) return 0; // too far - skip
	if (is_dlight_shadows && !dist_less_than(pre_smap_player_pos, ped.pos, 0.4*def_draw_dist)) return 0; // too far from the player
	if (is_dlight_shadows && !sphere_in_light_cone_approx(pdu, ped.pos, 0.5*ped.get_height())) return 0;

	if (ped_model_loader.num_models() == 0 || !ped_model_loader.is_model_valid(ped.model_id)) { // no model - draw as sphere
		if (!pdu.sphere_visible_test(ped.pos, ped.radius)) return 0; // not visible - skip
		if (anim_state) {anim_state->clear_animation_id(s);} // no animations for a sphere
		begin_ped_sphere_draw(s, YELLOW, in_sphere_draw, 0);
		int const ndiv = 16; // currently hard-coded
		draw_sphere_vbo(ped.pos, ped.radius, ndiv, 0);
	}
	else { // draw as 3D model
		cube_t const bcube(ped.get_bcube());
		// Note: the below test uses the bsphere, not the bcube directly, so it will be more accurate even if the model bcube doesn't include the animations;
		// however, it doesn't use the model bcube itself but rather the height/radius-based person bcube, so it may result in people clipping through objects
		if (!pdu.sphere_visible_test(bcube.get_cube_center(), 0.5*ped.get_height())) return 0; // not visible - skip
		if (!ped.in_building && dstate.is_occluded(bcube)) return 0; // only check occlusion for expensive ped models, and for peds outside buildings
		end_sphere_draw(in_sphere_draw);
		bool const low_detail(!shadow_only && !is_in_building && dist_sq > 0.25*draw_dist_sq); // low detail for non-shadow pass at half draw dist, if not in building
		
		if (anim_state) { // calculate/update animation data
			// only consider the person as idle if there's an idle animation;
			// otherwise will always use walk animation, which is assumed to exist, but anim_time won't be updated while idle
			bool const is_idle(ped.is_waiting_or_stopped() && ped_model_loader.get_model(ped.model_id).has_animation("idle"));
			// we need to know if there are idle animations for light/shadow updates in building_t::add_room_lights(),
			// where we don't have access to ped_model_loader, so the only solution I can come up with is using a global variable
			some_person_has_idle_animation |= is_idle;
			float state_change_elapsed(0.0), blend_factor(0.0); // [0.0, 1.0] where 0.0 => anim1 and 1.0 => anim2

			if (ped.last_anim_state_change_time > 0.0) { // if there was an animation state change
				float const blend_time_ticks(0.25*TICKS_PER_SECOND);
				state_change_elapsed = tfticks - ped.last_anim_state_change_time;
				// just after a state change we have state_change_elapsed == 0 and want blend_factor = 1.0 to select the previous animation
				if (state_change_elapsed < blend_time_ticks) {blend_factor = 1.0 - state_change_elapsed/blend_time_ticks;}
			}
			anim_state->anim_time     = (is_idle ? ped.get_idle_anim_time() : ped.anim_time); // if is_idle, we still need to advance the animation time
			anim_state->model_anim_id = (is_idle ? MODEL_ANIM_IDLE : MODEL_ANIM_WALK);
			anim_state->blend_factor  = blend_factor;
			anim_state->fixed_anim_speed = is_idle; // idle anim plays at normal speed, not zombie walking speed
			
			if (blend_factor > 0.0) {
				// blend animations between walking and idle states using opposite is_idle logic;
				// since anim_time won't increase in this state, add the elapsed time to it
				anim_state->anim_time2     = ((!is_idle) ? ped.get_idle_anim_time() : ped.anim_time) + state_change_elapsed*ped.speed;
				anim_state->model_anim_id2 = ((!is_idle) ? MODEL_ANIM_IDLE : MODEL_ANIM_WALK);
			}
			if (is_idle != ped.prev_was_idle) { // update mutable temp state for animations
				ped.prev_was_idle = is_idle;
				ped.last_anim_state_change_time = tfticks;
			}
		}
		vector3d dir_horiz(ped.dir);
		dir_horiz.z = 0.0; // always face a horizontal direction, even if walking on a slope
		dir_horiz.normalize();
		// A=0.0, leave unchanged
		//colorRGBA const &color(ped.following_player ? RED : WHITE); // force red when following player, for debugging purposes
		//colorRGBA const &color(ped.on_stairs() ? RED : ALPHA0);
		//colorRGBA const &color((ped.retreat_time > 0.0) ? RED : ALPHA0);
		// color applies to the skin of the Katie model (ID 3), but all of the other models since they're only one material; disable this since we now have separate zombie models
		//colorRGBA const &color(((!ped.is_zombie && ped.model_id == 3) && is_in_building && in_building_gameplay_mode()) ? OLIVE : ALPHA0);
		colorRGBA const &color(ALPHA0);
		ped_model_loader.draw_model(s, ped.pos, bcube, dir_horiz, color, xlate, ped.model_id, shadow_only, low_detail, anim_state);

		// draw umbrella 75% of the time if pedestrian is outside and in the rain
		if (!ped.in_building && !ped.is_zombie && is_rain_enabled() && !shadow_only && (ped.ssn & 3) != 0 && building_obj_model_loader.is_model_valid(OBJ_MODEL_UMBRELLA)) {
			vector3d const sz(building_obj_model_loader.get_model_world_space_size(OBJ_MODEL_UMBRELLA));
			float const ped_sz_scale(ped_model_loader.get_model(ped.model_id).scale), radius(0.5*bcube.dz()/ped_sz_scale);
			point const center(bcube.get_cube_center() + 0.25*radius*dir_horiz);
			cube_t u_bcube(center, center);
			u_bcube.expand_by_xy(radius);
			u_bcube.z1() -= 0.35*radius;
			u_bcube.z2() += 0.85*radius;
			if (anim_state) {anim_state->clear_animation_id(s);} // not animated
			// the handle direction is always in -x and doesn't rotate with the ped because there's no option to do this transform
			building_obj_model_loader.draw_model(s, u_bcube.get_cube_center(), u_bcube, plus_z, WHITE, xlate, OBJ_MODEL_UMBRELLA, shadow_only);
		}
	}
	return 1;
}

unsigned ped_manager_t::get_player_model_id() {
	unsigned const num_ped_models(ped_model_loader.num_models());
	assert(num_ped_models > 0);
	unsigned const model_id(global_building_params.player_model_ix % num_ped_models); // wrap around if set too large
	if (city_params.num_peds == 0 && !global_building_params.building_people_enabled()) {ped_model_loader.load_model_id(model_id);} // only need to load this particular model
	return model_id;
}
bool ped_manager_t::is_player_model_female() {
	if (ped_model_loader.num_models() == 0) return 0; // unknown
	return is_female_model(ped_model_loader.get_model(get_player_model_id()));
}

void draw_player_as_sphere() {
	point const player_pos(pre_smap_player_pos - vector3d(0.0, 0.0, 0.5f*camera_zh)); // shift to center of player height; ignore crouching for now
	draw_sphere_vbo(player_pos, 0.5f*CAMERA_RADIUS, N_SPHERE_DIV, 0); // use a smaller radius
}
void ped_manager_t::draw_player_model(shader_t &s, vector3d const &xlate, bool shadow_only) {
	if (ped_model_loader.num_models() == 0) { // no model - draw as sphere
		if (shadow_only) {draw_player_as_sphere();} // sphere is only used for shadows
		return;
	}
	unsigned const model_id(get_player_model_id());
	city_model_t const &model(ped_model_loader.get_model(model_id));
	
	if (!model.is_loaded()) {
		if (shadow_only) {draw_player_as_sphere();} // sphere is only used for shadows
		return;
	}
	bool const enable_animations(camera_surf_collide); // animate when walking but not when flying
	static float player_anim_time(0.0);
	static point prev_player_pos;
	
	if (enable_animations && p2p_dist_xy(pre_smap_player_pos, prev_player_pos) > 0.01*CAMERA_RADIUS) { // don't include minor differences related to turning in place
		prev_player_pos   = pre_smap_player_pos;
		player_anim_time += fticks*city_params.ped_speed;
	}
	animation_state_t anim_state(enable_animations, animation_id);
	float const player_eye_height(CAMERA_RADIUS + camera_zh), player_height(player_eye_height/EYE_HEIGHT_RATIO), player_radius(player_height/PED_HEIGHT_SCALE);
	point const pos(pre_smap_player_pos + vector3d(0.0, 0.0, (player_radius - player_eye_height)));
	vector3d const dir_horiz(vector3d(cview_dir.x, cview_dir.y, 0.0).get_norm()); // always face a horizontal direction, even if walking on a slope
	cube_t bcube;
	bcube.set_from_sphere(pos, PED_WIDTH_SCALE*player_radius);
	bcube.z1() = pos.z - player_radius;
	bcube.z2() = bcube.z1() + player_height*model.scale; // respect the model's scale; however, the player does seem a bit shorter than other people with the same model
	if (shadow_only && !smap_light_clip_cube.is_all_zeros() && !smap_light_clip_cube.intersects(bcube + xlate)) return; // shadow map clip cube test
	anim_state.anim_time = player_anim_time;
	ped_model_loader.draw_model(s, pos, bcube, dir_horiz, ALPHA0, xlate, model_id, shadow_only, 0, &anim_state);
	s.upload_mvm(); // not sure if this is needed
	anim_state.clear_animation_id(s); // make sure to leave animations disabled so that they don't apply to buildings
}

