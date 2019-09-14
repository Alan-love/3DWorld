// 3D World - Pedestrians for Procedural Cities
// by Frank Gennari
// 12/6/18
#include "city.h"
#include "shaders.h"

float const PED_WIDTH_SCALE  = 0.5; // ratio of collision radius to model radius (x/y)
float const PED_HEIGHT_SCALE = 2.5; // ratio of collision radius to model height (z)
float const CROSS_SPEED_MULT = 1.8; // extra speed multiplier when crossing the road
float const CROSS_WAIT_TIME  = 60.0; // in seconds
bool const FORCE_USE_CROSSWALKS = 0; // more realistic and safe, but causes problems with pedestian collisions

extern bool tt_fire_button_down;
extern int display_mode, game_mode, animate2, frame_counter;
extern float FAR_CLIP;
extern point pre_smap_player_pos;
extern city_params_t city_params;


string gen_random_name(rand_gen_t &rgen); // from Universe_name.cpp

string pedestrian_t::get_name() const {
	rand_gen_t rgen;
	rgen.set_state(ssn, 123); // use ssn as name rand gen seed
	return gen_random_name(rgen); // for now, borrow the universe name generator to assign silly names
}

string pedestrian_t::str() const { // Note: no label_str()
	std::ostringstream oss;
	oss << get_name() << ": " << TXTn(ssn) << TXT(speed) << TXTn(radius) << TXT(city) << TXT(plot) << TXT(next_plot) << TXT(dest_plot) << TXTn(dest_bldg)
		<< TXTi(stuck_count) << TXT(collided) << TXTn(in_the_road) << TXT(is_stopped) << TXT(at_dest) << TXTn(has_dest_car) << TXT(target_valid())
		<< "wait_time=" << get_wait_time_secs(); // Note: pos, vel, dir not printed
	return oss.str();
}

float pedestrian_t::get_speed_mult() const {return (in_the_road ? CROSS_SPEED_MULT : 1.0);}

void pedestrian_t::stop() {
	//dir = vel.get_norm(); // ???
	vel = zero_vector;
	anim_time  = 0.0; // reset animation so that ped is standing normally and not mid-stride - should really transition this gradually somehow
	is_stopped = 1;
}
void pedestrian_t::go() {
	vel = dir * speed; // assumes dir is correct
	is_stopped = 0;
}

float get_sidewalk_width(cube_t const &plot_bcube) { // Note: technically should depend on road_width rather than plot size?
	float const plot_sz(max(plot_bcube.dx(), plot_bcube.dy())); // plot is almost square, so this is close enough
	return -STREETLIGHT_DIST_FROM_PLOT_EDGE*plot_sz + streetlight_ns::get_streetlight_pole_radius();
}

bool pedestrian_t::check_inside_plot(ped_manager_t &ped_mgr, point const &prev_pos, cube_t const &plot_bcube, cube_t const &next_plot_bcube) {
	//if (ssn == 2516) {cout << "in_the_road: " << in_the_road << ", pos: " << pos.str() << ", plot_bcube: " << plot_bcube.str() << ", npbc: " << next_plot_bcube.str() << endl;}
	if (plot_bcube.contains_pt_xy(pos)) {return 1;} // inside the plot
	stuck_count = 0; // no longer stuck
	if (next_plot == plot) return 0; // no next plot - clip to this plot
	
	if (next_plot_bcube.contains_pt_xy(pos)) {
		ped_mgr.move_ped_to_next_plot(*this);
		next_plot = ped_mgr.get_next_plot(*this);
		return 1;
	}
	cube_t union_plot_bcube(plot_bcube);
	union_plot_bcube.union_with_cube(next_plot_bcube);
	if (!union_plot_bcube.contains_pt_xy(pos) && union_plot_bcube.contains_pt_xy(prev_pos)) {return 0;} // went outside the valid area
	float const dx(min(fabs(pos.x - plot_bcube.x1()), fabs(pos.x - plot_bcube.x2()))), dy(min(fabs(pos.y - plot_bcube.y1()), fabs(pos.y - plot_bcube.y2())));

	if (dx < max(city_params.road_width, 0.1f*plot_bcube.dx()) && dy < max(city_params.road_width, 0.1f*plot_bcube.dy())) { // near an intersection
		at_crosswalk = 1; // Note: should only be at crosswalks; but if we actually are corssing the road, this is the correct thing to do
	}
	in_the_road = 1;
	return 1; // allow peds to cross the road; don't need to check for building or other object collisions
}

bool pedestrian_t::check_road_coll(ped_manager_t const &ped_mgr, cube_t const &plot_bcube, cube_t const &next_plot_bcube) const {
	if (!in_the_road) return 0;
	float const expand(get_sidewalk_width(plot_bcube) + radius); // max dist from plot edge where a collision can occur
	cube_t pbce(plot_bcube), npbce(next_plot_bcube);
	pbce.expand_by_xy(expand);
	npbce.expand_by_xy(expand);
	if ((!pbce.contains_pt_xy(pos)) && (!npbce.contains_pt_xy(pos))) return 0; // ped is too far from the edge of the road to collide with streetlights or stoplights
	if (ped_mgr.check_isec_sphere_coll(*this)) return 1;
	if (ped_mgr.check_streetlight_sphere_coll(*this)) return 1;
	return 0;
}

bool pedestrian_t::is_valid_pos(vect_cube_t const &colliders, bool &ped_at_dest, car_manager_t const *const car_manager) const {
	if (in_the_road) return 1; // not in a plot, no collision detection needed
	unsigned building_id(0);

	if (check_buildings_ped_coll(pos, radius, plot, building_id)) {
		if (!has_dest_bldg || building_id != dest_bldg) return 0;
		bool const ret(!at_dest);
		ped_at_dest = 1;
		return ret; // only valid if we just reached our dest
	}
	float const xmin(pos.x - radius), xmax(pos.x + radius);

	for (auto i = colliders.begin(); i != colliders.end(); ++i) {
		if (i->x2() < xmin) continue; // to the left
		if (i->x1() > xmax) break; // to the right - sorted from left to right, so no more colliders can intersect - done
		if (!sphere_cube_intersect(pos, radius, *i)) continue;
		if (!has_dest_car) return 0;
		// check if collider is a parking lot car group that contains the dest car; if so, mark as at_dest, even if we haven't quite hit the correct car
		if (!(car_manager && i->intersects_xy(car_manager->get_car_bcube(dest_bldg)))) return 0; // car_id == dest_bldg
		bool const ret(!at_dest);
		ped_at_dest = 1;
		return ret; // only valid if we just reached our dest
	}
	return 1;
}

void register_ped_coll(pedestrian_t &p1, pedestrian_t &p2, unsigned pid1, unsigned pid2) {
	p1.collided = p1.ped_coll = 1; p1.colliding_ped = pid2;
	p2.collided = p2.ped_coll = 1; p2.colliding_ped = pid1;
}

bool pedestrian_t::check_ped_ped_coll_range(vector<pedestrian_t> &peds, unsigned pid, unsigned ped_start, unsigned target_plot, float prox_radius, vector3d &force) {
	float const prox_radius_sq(prox_radius*prox_radius);

	for (auto i = peds.begin()+ped_start; i != peds.end(); ++i) { // check every ped until we exit target_plot
		if (i->plot != target_plot) break; // moved to a new plot, no collision, done; since plots are globally unique across cities, we don't need to check cities
		float const dist_sq(p2p_dist_xy_sq(pos, i->pos));
		if (dist_sq > prox_radius_sq) continue; // proximity test
		float const r_sum(0.6f*(radius + i->radius)); // using a smaller radius to allow peds to get close to each other
		if (dist_sq < r_sum*r_sum) {register_ped_coll(*this, *i, pid, (i - peds.begin())); return 1;} // collision
		if (speed < TOLERANCE) continue;
		vector3d const delta_v(vel - i->vel), delta_p((pos.x - i->pos.x), (pos.y - i->pos.y), 0.0);
		float const dp(-dot_product_xy(delta_v, delta_p));
		if (dp <= 0.0) continue; // diverging, no avoidance needed
		float const dv_mag(delta_v.mag()), dist(sqrt(dist_sq)), fmag(dist/(dist - 0.9*r_sum));
		if (dv_mag < TOLERANCE) continue;
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
	float const timestep(2.0*TICKS_PER_SECOND), lookahead_dist(timestep*speed); // how far we can travel in 2s
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
		if (!dist_xy_less_than(pos, i->pos, 0.6f*(radius + i->radius))) continue; // no collision
		i->collided = i->ped_coll = 1; i->colliding_ped = pid;
		return 1; // Note: could omit this return and continue processing peds
	} // for i
	return 0;
}

bool pedestrian_t::try_place_in_plot(cube_t const &plot_cube, vect_cube_t const &colliders, unsigned plot_id, rand_gen_t &rgen) {
	pos    = rand_xy_pt_in_cube(plot_cube, radius, rgen);
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
	if (!add_pt_to_path(ecorners[dest_cix], path)) return 0; // expanded corner

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
			int const building_id(get_building_bcube_contains_pos(pos));
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

// pedestrian_t
point pedestrian_t::get_dest_pos(cube_t const &plot_bcube, cube_t const &next_plot_bcube, ped_manager_t const &ped_mgr) const {
	if (is_stopped && target_valid()) {return target_pos;} // stay the course (this case only needed for debug drawing)

	if (plot == dest_plot) { // this plot contains our dest building
		if (!at_dest && has_dest_bldg) { // not there yet
			cube_t const dest_bcube(get_building_bcube(dest_bldg));
			//if (dest_bcube.contains_pt_xy(pos)) {at_dest = 1;} // could set this here, but requiring a collision also works
			point const dest_pos(dest_bcube.get_cube_center()); // slowly adjust dir to move toward dest_bldg
			return point(dest_pos.x, dest_pos.y, pos.z); // same zval
		}
		else if (!at_dest && has_dest_car) {
			point const dest_pos(ped_mgr.get_car_manager().get_car_bcube(dest_bldg).get_cube_center());
			return point(dest_pos.x, dest_pos.y, pos.z); // same zval
		}
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
				dest_pos = next_plot_bcube.closest_pt(pos);
			}
			if (!in_cur_plot) { // went outside the current plot
				cube_t union_plot_bcube(plot_bcube);
				union_plot_bcube.union_with_cube(next_plot_bcube);
				if (!union_plot_bcube.contains_pt_xy(pos)) {dest_pos = plot_bcube.closest_pt(pos);} // went outside on the wrong side, go back inside the current plot
			}
			dest_pos.z = pos.z; // same zval
			return dest_pos;
			//if (???) {at_crosswalk = 1;}
		}
	}
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

void pedestrian_t::get_avoid_cubes(ped_manager_t const &ped_mgr, vect_cube_t const &colliders, point const &dest_pos, vect_cube_t &avoid) const {
	avoid.clear();
	get_building_bcubes(ped_mgr.get_city_plot_bcube_for_peds(city, plot), avoid);
	float const expand(1.1*radius); // slightly larger than radius to leave some room for floating-point error
	expand_cubes_by_xy(avoid, expand); // expand building cubes in x and y to approximate a cylinder collision (conservative)
	//remove_cube_if_contains_pt_xy(avoid, pos); // init coll cases (for example from previous dest_bldg) are handled by path_finder_t
	// exclude our dest building, since we do want to collide with it
	if (plot == dest_plot) {remove_cube_if_contains_pt_xy(avoid, dest_pos);}
	size_t const num_building_cubes(avoid.size());
	vector_add_to(colliders, avoid);
	for (auto i = avoid.begin()+num_building_cubes; i != avoid.end(); ++i) {i->expand_by_xy(expand);} // expand colliders as well
}

bool pedestrian_t::check_for_safe_road_crossing(ped_manager_t const &ped_mgr, cube_t const &plot_bcube, cube_t const &next_plot_bcube, vect_cube_t *dbg_cubes) const {
	if (!in_the_road || speed < TOLERANCE) return 1;
	float const sw_width(get_sidewalk_width(plot_bcube));
	if (!plot_bcube.closest_dist_xy_less_than(pos, sw_width)) return 1; // too far into the road to turn back
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
	if (!check_for_safe_road_crossing(ped_mgr, plot_bcube, next_plot_bcube)) {stop(); return;}
	reset_waiting();
	if (is_stopped) {go();}

	if (target_valid()) { // if facing away from the target, rotate in place rather than moving in a circle
		vector3d const delta(target_pos - pos);
		float const dist(delta.mag());
		if (dist > radius && dot_product_xy(vel, delta) < 0.01*speed*dist) {delta_dir = min(1.0f, 4.0f*delta_dir); return;} // rotate faster
	}
	float const timestep(fticks*get_speed_mult());
	pos       += vel*timestep;
	anim_time += timestep*speed;
}

void pedestrian_t::next_frame(ped_manager_t &ped_mgr, vector<pedestrian_t> &peds, unsigned pid, rand_gen_t &rgen, float delta_dir) {
	if (destroyed) return; // destroyed
	if (speed == 0.0) return; // not moving, no update needed
	//assert(!is_nan(pos));

	// navigation with destination
	if (at_dest) {
		register_at_dest();
		ped_mgr.choose_new_ped_plot_pos(*this);
	}
	if (at_crosswalk) {ped_mgr.mark_crosswalk_in_use(*this);}
	// movement logic
	cube_t const &plot_bcube(ped_mgr.get_city_plot_bcube_for_peds(city, plot));
	cube_t const &next_plot_bcube(ped_mgr.get_city_plot_bcube_for_peds(city, next_plot));
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
	at_crosswalk = in_the_road = 0; // reset state for next frame; these may be set back to 1 below
	vect_cube_t const &colliders(ped_mgr.get_colliders_for_plot(city, plot));
	bool outside_plot(0);

	if (collided) {} // already collided with a previous ped this frame, handled below
	else if (!check_inside_plot(ped_mgr, prev_pos, plot_bcube, next_plot_bcube)) {collided = outside_plot = 1;} // outside the plot, treat as a collision with the plot bounds
	else if (!is_valid_pos(colliders, at_dest, &ped_mgr.get_car_manager())) {collided = 1;} // collided with a static collider
	else if (check_road_coll(ped_mgr, plot_bcube, next_plot_bcube)) {collided = 1;} // collided with something in the road (stoplight, streetlight, etc.)
	else if (check_ped_ped_coll(ped_mgr, peds, pid, delta_dir)) {collided = 1;} // collided with another pedestrian
	else { // no collisions
		//cout << TXT(pid) << TXT(plot) << TXT(dest_plot) << TXT(next_plot) << TXT(at_dest) << TXT(delta_dir) << TXT((unsigned)stuck_count) << TXT(collided) << endl;
		vector3d dest_pos(get_dest_pos(plot_bcube, next_plot_bcube, ped_mgr));

		if (dest_pos != pos) {
			bool update_path(0);
			if (dist_less_than(pos, get_camera_pos(), 1000.0*radius)) { // nearby pedestrian - higher update rate
				update_path = (((frame_counter + ssn) & 15) == 0 || (target_valid() && dist_xy_less_than(pos, target_pos, radius)));
			}
			else { // distant pedestrian - lower update rate
				update_path = (((frame_counter + ssn) & 63) == 0);
			}
			// run only every several frames to reduce runtime; also run when at dest and when close to the current target pos or at the destination
			if (at_dest || update_path) {
				get_avoid_cubes(ped_mgr, colliders, dest_pos, ped_mgr.path_finder.get_avoid_vector());
				target_pos = all_zeros;
				cube_t union_plot_bcube(plot_bcube);
				union_plot_bcube.union_with_cube(next_plot_bcube); // this is the area the ped is constrained to (both plots + road in between)
				// run path finding between pos and dest_pos using avoid cubes
				if (ped_mgr.path_finder.run(pos, dest_pos, union_plot_bcube, 0.1*radius, dest_pos)) {target_pos = dest_pos;}
			}
			else if (target_valid()) {dest_pos = target_pos;} // use previous frame's dest if valid
			vector3d dest_dir((dest_pos.x - pos.x), (dest_pos.y - pos.y), 0.0); // zval=0, not normalized
			float const dmag(dest_dir.mag());

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
			if (!is_valid_pos(colliders, at_dest, &ped_mgr.get_car_manager()) || check_road_coll(ped_mgr, plot_bcube, next_plot_bcube)) {pos = cur_pos;}
		}
		vector3d new_dir;

		if (++stuck_count > 8) {
			if (target_valid()) {pos += (0.1*radius)*(target_pos - pos).get_norm();} // move toward target_pos if it's valid since this should be a good direction
			else if (stuck_count > 100) {pos += (0.1*radius)*(get_dest_pos(plot_bcube, next_plot_bcube, ped_mgr) - pos).get_norm();} // move toward dest if stuck count is high
			else {pos += rgen.signed_rand_vector_spherical_xy()*(0.1*radius); } // shift randomly by 10% radius to get unstuck
		}
		if (ped_coll) {
			assert(colliding_ped < peds.size());
			vector3d const coll_dir(peds[colliding_ped].pos - pos);
			new_dir = cross_product(vel, plus_z);
			if (dot_product_xy(new_dir, coll_dir) > 0.0) {new_dir = -new_dir;} // orient away from the other ped
		}
		else { // static object collision (should be rare if path_finder does a good job)
			new_dir = rgen.signed_rand_vector_spherical_xy(); // try a random new direction
			if (dot_product_xy(vel, new_dir) > 0.0) {new_dir *= -1.0;} // negate if pointing in the same dir
		}
		set_velocity(new_dir);
		target_pos = all_zeros; // reset and force path finding to re-route from this new direction/pos
	}
	if (vel != zero_vector) { // if stopped, don't update dir
		if (!collided && target_valid()) {delta_dir = min(1.0f, 4.0f*delta_dir);} // use a tighter turning radius when there's an unobstructed target_pos
		dir = (delta_dir/speed)*vel + (1.0 - delta_dir)*dir; // merge velocity into dir gradually for smooth turning
		dir.z = 0.0; // should be zero, but set just in case
		dir.normalize();
	}
	collided = ped_coll = 0; // reset for next frame
}

void pedestrian_t::register_at_dest() {
	assert(plot == dest_plot);
	//cout << get_name() << " at destination " << (has_dest_car ? "car " : (has_dest_bldg ? "building " : "")) << dest_bldg << " in plot " << dest_plot << endl; // placeholder
}


unsigned ped_model_loader_t::num_models() const {return city_params.ped_model_files.size();}

city_model_t const &ped_model_loader_t::get_model(unsigned id) const {
	assert(id < num_models());
	return city_params.ped_model_files[id];
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

void ped_manager_t::init(unsigned num) {
	if (num == 0) return;
	timer_t timer("Gen Peds");
	peds.reserve(num);
	float const radius(get_ped_radius()); // base radius
	unsigned const num_models(ped_model_loader.num_models());

	for (unsigned n = 0; n < num; ++n) {
		pedestrian_t ped(radius); // start with a constant radius

		if (num_models > 0) {
			ped.model_id = rgen.rand()%num_models;
			ped.radius  *= ped_model_loader.get_model(ped.model_id).scale;
			assert(ped.radius > 0.0); // no zero/negative model scales
		}
		else {ped.model_id = 0;} // will be unused
		if (gen_ped_pos(ped)) {
			if (city_params.ped_speed > 0.0) {
				ped.speed = city_params.ped_speed*rgen.rand_uniform(0.5, 1.0);
				ped.vel   = rgen.signed_rand_vector_spherical_xy().get_norm()*ped.speed;
			}
			ped.ssn = (unsigned short)peds.size(); // assign init peds index so that all are unique; won't change if peds are reordered
			peds.push_back(ped);
		}
	}
	cout << "Pedestrians: " << peds.size() << endl; // testing
	sort_by_city_and_plot();
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
	if (!animate2 || peds.empty()) return; // nothing to do
	//timer_t timer("Ped Update"); // ~3.9ms for 10K peds

	// Note: should make sure this is after sorting cars, so that road_ix values are actually in order; however, that makes things slower, and is unlikely to make a difference
#pragma omp critical(modify_car_data)
	{car_manager.extract_car_data(cars_by_city);}

	if (ped_destroyed) {remove_destroyed_peds();} // at least one ped was destroyed in the previous frame - remove it/them
	float const delta_dir(1.2*(1.0 - pow(0.7f, fticks))); // controls pedestrian turning rate
	static bool first_frame(1);

	if (first_frame) { // choose initial ped destinations (must be after building setup, etc.)
		for (auto i = peds.begin(); i != peds.end(); ++i) {choose_dest_building_or_parked_car(*i);}
	}
	for (auto i = peds.begin(); i != peds.end(); ++i) {i->next_frame(*this, peds, (i - peds.begin()), rgen, delta_dir);}
	if (need_to_sort_peds) {sort_by_city_and_plot();}
	first_frame = 0;
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
		if (!i->in_the_road || i->is_stopped) continue; // not actively crossing the road
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

bool ped_manager_t::has_nearby_car_on_road(pedestrian_t const &ped, bool dim, unsigned road_ix, float delta_time, vect_cube_t *dbg_cubes) const {
	if (ped.city >= cars_by_city.size()) return 0; // no cars in this city? should be rare, unless cars aren't enabled
	car_city_vect_t const &cv(cars_by_city[ped.city]);
	point const &pos(ped.pos);

	for (unsigned dir = 0; dir < 2; ++dir) { // look both ways before crossing
		auto const &cars(cv.cars[dim][dir]);
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
				float const val2(closest_car->bcube.d[dim][!dir]);
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

void pedestrian_t::debug_draw(ped_manager_t &ped_mgr) const {
	cube_t const &plot_bcube(ped_mgr.get_city_plot_bcube_for_peds(city, plot));
	cube_t const &next_plot_bcube(ped_mgr.get_city_plot_bcube_for_peds(city, next_plot));
	point const orig_dest_pos(get_dest_pos(plot_bcube, next_plot_bcube, ped_mgr));
	point dest_pos(orig_dest_pos);
	if (dest_pos == pos) return; // no path, nothing to draw
	vect_cube_t dbg_cubes;
	bool const safe_to_cross(check_for_safe_road_crossing(ped_mgr, plot_bcube, next_plot_bcube, &dbg_cubes));
	if (!safe_to_cross) {assert(!dbg_cubes.empty());} // must find a blocking car
	path_finder_t path_finder(1); // debug=1
	get_avoid_cubes(ped_mgr, ped_mgr.get_colliders_for_plot(city, plot), dest_pos, path_finder.get_avoid_vector());
	cube_t union_plot_bcube(plot_bcube);
	union_plot_bcube.union_with_cube(next_plot_bcube);
	vector<point> path;
	unsigned const ret(path_finder.run(pos, dest_pos, union_plot_bcube, 0.05*radius, dest_pos)); // 0=no path, 1=standard path, 2=init intersection path
	if (ret == 0) return; // no path found
	bool const at_dest_plot(plot == dest_plot), complete(path_finder.found_complete_path());
	colorRGBA line_color(at_dest_plot ? RED : YELLOW); // paths
	colorRGBA node_color(complete ? YELLOW : ORANGE);

	if (ret == 3) { // straight line
		path.push_back(pos);
		path.push_back(dest_pos);
		if (!at_dest_plot) {line_color = ORANGE; node_color = (safe_to_cross ? GREEN : RED);} // straight line
	}
	else {path = path_finder.get_best_path();} // found a path
	vector<vert_color> line_pts;
	shader_t s;
	s.begin_color_only_shader();
	bool in_sphere_draw(0);
	begin_ped_sphere_draw(s, node_color, in_sphere_draw, 0);

	if (ret == 2) { // show segment from current pos to edge of building/car
		assert(!path.empty());
		draw_sphere_vbo(path[0], radius, 16, 0);
		line_pts.emplace_back(pos, BLUE);
		line_pts.emplace_back(path[0], BLUE);
	}
	for (auto p = path.begin(); p+1 != path.end(); ++p) { // iterate over line segments, skip last point
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
		draw_simple_cube(*i, 0);
	}
	draw_verts(line_pts, GL_LINES);
	s.end_shader();
}

void ped_manager_t::next_animation() {
	unsigned const NUM_ANIMATIONS = 7; // including null animation
	string const animation_names[NUM_ANIMATIONS] = {"The Slide", "Walking", "The Bunny Hop", "The Flip", "The Twirl", "Marching", "Walk Like an Alien"};
	animation_id = (animation_id + 1) % NUM_ANIMATIONS;
	print_text_onscreen(animation_names[animation_id], WHITE, 1.5, 2*TICKS_PER_SECOND, 1);
}

void ped_manager_t::draw(vector3d const &xlate, bool use_dlights, bool shadow_only, bool is_dlight_shadows) {
	if (empty()) return;
	if (is_dlight_shadows && !city_params.car_shadows) return; // use car_shadows as ped_shadows
	if (shadow_only && !is_dlight_shadows) return; // don't add to precomputed shadow map
	//timer_t timer("Ped Draw"); // ~1ms
	bool const use_models(ped_model_loader.num_models() > 0), enable_animations(use_models);
	float const def_draw_dist((use_models ? 500.0 : 2000.0)*get_ped_radius());
	float const draw_dist(is_dlight_shadows ? 0.8*camera_pdu.far_ : def_draw_dist), draw_dist_sq(draw_dist*draw_dist); // smaller view dist for models
	pos_dir_up pdu(camera_pdu); // decrease the far clipping plane for pedestrians
	pdu.far_ = draw_dist;
	pdu.pos -= xlate; // adjust for local translate
	dstate.xlate = xlate;
	dstate.set_enable_normal_map(use_models && use_model3d_bump_maps());
	fgPushMatrix();
	translate_to(xlate);
	if (enable_animations) {dstate.s.add_property("animation_shader", "pedestrian_animation.part+");}
	dstate.pre_draw(xlate, use_dlights, shadow_only);
	bool const textured(shadow_only && 0); // disabled for now
	bool in_sphere_draw(0);
	if (enable_animations) {dstate.s.add_uniform_int("animation_id", animation_id);}

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
				if (ped.destroyed) continue; // skip
				float const dist_sq(p2p_dist_sq(pdu.pos, ped.pos));
				if (dist_sq > draw_dist_sq) continue; // too far - skip
				if (is_dlight_shadows && !dist_less_than(pre_smap_player_pos, ped.pos, 0.4*def_draw_dist))  continue; // too far from the player
				if (is_dlight_shadows && !sphere_in_light_cone_approx(pdu, ped.pos, 0.5*PED_HEIGHT_SCALE*ped.radius)) continue;
				
				if (!use_models || !ped_model_loader.is_model_valid(ped.model_id)) {
					if (!pdu.sphere_visible_test(ped.pos, ped.radius)) continue; // not visible - skip
					if (enable_animations) {dstate.s.add_uniform_float("animation_time", 0.0);}
					begin_ped_sphere_draw(dstate.s, YELLOW, in_sphere_draw, textured);
					int const ndiv = 16; // currently hard-coded
					draw_sphere_vbo(ped.pos, ped.radius, ndiv, textured);
				}
				else {
					float const width(PED_WIDTH_SCALE*ped.radius), height(PED_HEIGHT_SCALE*ped.radius);
					cube_t bcube;
					bcube.set_from_sphere(ped.pos, width);
					bcube.z1() = ped.pos.z - ped.radius;
					bcube.z2() = bcube.z1() + height;
					if (!pdu.sphere_visible_test(bcube.get_cube_center(), 0.5*height)) continue; // not visible - skip
					if (dstate.is_occluded(bcube)) continue; // only check occlusion for expensive ped models
					end_sphere_draw(in_sphere_draw);
					bool const low_detail(!shadow_only && dist_sq > 0.25*draw_dist_sq); // low detail for non-shadow pass at half draw dist
					if (enable_animations) {dstate.s.add_uniform_float("animation_time", ped.anim_time);}
					ped_model_loader.draw_model(dstate.s, ped.pos, bcube, ped.dir, ALPHA0, xlate, ped.model_id, shadow_only, low_detail, enable_animations);
				}
				if (dist_sq < 0.25*draw_dist_sq) { // fake AO shadow at below half draw distance
					float const ao_radius(0.6*ped.radius);
					float const zval(get_city_plot_bcube_for_peds(ped.city, ped.plot).z2() + 0.02*ped.radius); // at the feet
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
	pedestrian_t const *selected_ped(nullptr);

	if (tt_fire_button_down && !game_mode) {
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

