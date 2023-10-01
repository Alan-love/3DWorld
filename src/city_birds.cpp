// 3D World - City Birds Implementation
// by Frank Gennari
// 09/21/23

#include "city_objects.h"

float const BIRD_ACCEL       = 0.00025;
float const BIRD_MAX_VEL     = 0.002;
float const BIRD_ZV_RISE     = 0.4; // Z vs. XY velocity/acceleration for ascent
float const BIRD_ZV_FALL     = 0.8; // Z vs. XY velocity/acceleration for descent
float const BIRD_MAX_ALT_RES = 0.8; // above destination; in multiples of road width; for residential cities
float const BIRD_MAX_ALT_COM = 1.8; // above destination; in multiples of road width; for commercial  cities
float const anim_time_scale(1.0/TICKS_PER_SECOND);

extern int animate2, frame_counter;
extern float fticks;
extern double tfticks;
extern city_params_t city_params;
extern object_model_loader_t building_obj_model_loader;

enum {BIRD_STATE_FLYING=0, BIRD_STATE_GLIDING, BIRD_STATE_LANDING, BIRD_STATE_STANDING, BIRD_STATE_TAKEOFF, NUM_BIRD_STATES};


city_bird_t::city_bird_t(point const &pos_, float height_, vector3d const &init_dir, unsigned loc_ix_, rand_gen_t &rgen) :
	city_bird_base_t(pos_, height_, init_dir, OBJ_MODEL_BIRD_ANIM), state(BIRD_STATE_STANDING), loc_ix(loc_ix_), height(height_), prev_frame_pos(pos)
{
	anim_time = 1.0*TICKS_PER_SECOND*rgen.rand_float(); // 1s random variation so that birds aren't all in sync
}

void city_bird_t::draw(draw_state_t &dstate, city_draw_qbds_t &qbds, float dist_scale, bool shadow_only) const {
	if (dstate.check_cube_visible(bcube, dist_scale)) {
		// animations: 0=flying, 1=gliding, 2=landing, 3=standing, 4=takeoff
		float const model_anim_time(anim_time_scale*anim_time/SKELETAL_ANIM_TIME_CONST); // divide by constant to cancel out multiply in draw_model()
		animation_state_t anim_state(1, ANIM_ID_SKELETAL, model_anim_time, get_model_anim_id()); // enabled=1
		building_obj_model_loader.draw_model(dstate.s, pos, bcube, dir, WHITE, dstate.xlate, OBJ_MODEL_BIRD_ANIM, shadow_only, 0, &anim_state);
	}
	if (0 && dest_valid() && is_close_to_player()) { // debug drawing, even if bcube not visible
		post_draw(dstate, shadow_only); // clear animations
		vector<vert_color> pts;
		pts.emplace_back(pos,  BLUE);
		pts.emplace_back(dest, BLUE);
		select_texture(WHITE_TEX);
		draw_verts(pts, GL_LINES);
		dstate.s.set_cur_color(BLUE);
		draw_sphere_vbo(dest, radius, N_SPHERE_DIV, 0); // draw destination
	}
}
/*static*/ void city_bird_t::post_draw(draw_state_t &dstate, bool shadow_only) {
	animation_state_t anim_state(1); // enabled=1
	anim_state.clear_animation_id(dstate.s); // clear animations
	model3d::bind_default_flat_normal_map();
}

bool city_bird_t::is_close_to_player() const {
	return dist_less_than(pos, (get_camera_pos() - get_tiled_terrain_model_xlate()), 1.0*city_params.road_width);
}
void city_bird_t::set_takeoff_time(rand_gen_t &rgen) {
	takeoff_time = tfticks + rgen.rand_uniform(10.0, 30.0)*TICKS_PER_SECOND; // wait 10-30s
}
void city_bird_t::adjust_new_dest_zval() {
	dest.z += (0.5 + BIRD_ZVAL_ADJ)*height; // place feet at dest, not bird center
}
bool city_bird_t::is_anim_cycle_complete(float new_anim_time) const {
	if (anim_time == 0.0) return 0; // anim_time was just reset
	return building_obj_model_loader.check_anim_wrapped(OBJ_MODEL_BIRD_ANIM, get_model_anim_id(), anim_time_scale*anim_time, anim_time_scale*new_anim_time);
}
bool city_bird_t::in_landing_dist() const {
	assert(dest_valid());
	assert(state == BIRD_STATE_FLYING || state == BIRD_STATE_GLIDING);
	// play landing animation when dest is reached
	float const frame_dist(p2p_dist(pos, prev_frame_pos)), dist_thresh(frame_dist + 0.08*radius); // include previous frame distance to avoid overshoot
	return dist_less_than(pos, dest, dist_thresh);
}

// timestep is in ticks
void city_bird_t::next_frame(float timestep, float delta_dir, bool &tile_changed, bool &bird_moved, city_obj_placer_t &placer, rand_gen_t &rgen) {
	// update state
	uint8_t const prev_state(state);
	float const new_anim_time(anim_time + timestep);

	switch (state) {
		// Note: if flying level (velocity.z == 0.0), maintain the current flying vs. gliding state
	case BIRD_STATE_FLYING:
		if (velocity.z < 0.0 && is_anim_cycle_complete(new_anim_time)) {state = BIRD_STATE_GLIDING;} // maybe switch to gliding
		// fall through
	case BIRD_STATE_GLIDING:
		if (velocity.z > 0.0) {state = BIRD_STATE_FLYING;} // should we call is_anim_cycle_complete()?
		
		if (in_landing_dist()) { // check if close enough to dest, then switch to landing
			// reorient into dest_dir? otherwise remove dest_dir?
			state = BIRD_STATE_LANDING;
			pos   = dest; // snap to dest; is this a good idea? move more slowly?
			dest  = all_zeros; // clear for next cycle
		}
		break;
	case BIRD_STATE_LANDING:
		if (is_anim_cycle_complete(new_anim_time)) { // wait for animation to complete
			state       = BIRD_STATE_STANDING;
			velocity    = zero_vector; // make sure to stop
			hit_min_alt = 0; // reset for next cycle
			set_takeoff_time(rgen);
		}
		break;
	case BIRD_STATE_STANDING:
		if (takeoff_time == 0.0) { // set initial takeoff time
			set_takeoff_time(rgen);
		}
		else if (tfticks > takeoff_time && is_anim_cycle_complete(new_anim_time)) {
			if (placer.choose_bird_dest(pos, radius, loc_ix, dest, dest_dir)) {
				adjust_new_dest_zval();
				start_end_zmax = max(dest.z, pos.z);
				state          = BIRD_STATE_TAKEOFF;
			}
		}
		break;
	case BIRD_STATE_TAKEOFF:
		if (is_anim_cycle_complete(new_anim_time)) {state = BIRD_STATE_FLYING;} // wait for animation to complete
		break;
	default: assert(0);
	}
	if (state != prev_state) {anim_time = 0.0;} // reset animation time on state change
	else {anim_time = new_anim_time;}

	if (state == BIRD_STATE_STANDING || state == BIRD_STATE_LANDING || pos == dest) {
		velocity = zero_vector; // stopped
	}
	else { // update direction and velocity
		vector3d dest_dir_xy(dest.x-pos.x, dest.y-pos.y, 0.0); // XY only
		float const dist_xy(dest_dir_xy.mag());
		if (dist_xy > TOLERANCE) {dest_dir_xy /= dist_xy;} else {dest_dir_xy = dir;} // normalize if nonzero, otherwise use cur dir
		float const accel(((state == BIRD_STATE_TAKEOFF) ? 0.1 : 1.0)*BIRD_ACCEL); // slower during takeoff
		float const dir_dp(dot_product(dir, dest_dir_xy));

		// update direction
		if (dir_dp < 0.999) { // not oriented in dir
			delta_dir *= max(1.0f, 4.0f*radius/dist_xy); // faster correction when close to destination to prevent circling
			// if directions are nearly opposite, pick a side to turn using the cross product to get an orthogonal vector
			if (dir_dp < -0.9) {dest_dir_xy = cross_product(dir, plus_z)*((loc_ix & 1) ? -1.0 : 1.0);} // random turn direction (CW/CCW)
			dir = delta_dir*dest_dir_xy + (1.0 - delta_dir)*dir; // merge new_dir into dir gradually for smooth turning
			dir.normalize();
		}
		// handle vertical velocity component; this assumes the start and end points are similar zvals
		// check dir is aligned and approach angle is steep enough to glide down;
		// the extra radius factor allows for smooth landings when close; closer to radius is more smooth, while omitting this results in flappy landings
		bool const can_descend(hit_min_alt && dir_dp > 0.5 && BIRD_ZV_FALL*dist_xy < (pos.z - dest.z - 0.1*radius));

		if (!can_descend) { // ascent stage
			float const max_alt_mult(placer.has_residential() ? BIRD_MAX_ALT_RES : BIRD_MAX_ALT_COM);
			float const max_alt(max_alt_mult*(city_params.road_width + 4.0*radius)); // larger birds can fly a bit higher
			hit_min_alt |= (pos.z > start_end_zmax + 4.0*radius); // lift off at least 4x radius to clear the starting object
			if (pos.z > max_alt + start_end_zmax) {velocity.z = 0.0;} // too high; level off (should we decelerate smoothly?)
			else {velocity.z = min(BIRD_ZV_RISE*BIRD_MAX_VEL, (velocity.z + BIRD_ZV_RISE*accel));} // rise in the air
		}
		else { // descent stage
			float const z_clearance(min(1.0f*radius, 0.5f*dist_xy)); // approach angle not too shallow
			
			if (pos.z <= dest.z + z_clearance) { // don't fall below the destination zval
				velocity.z = 0.0;
				max_eq(pos.z, dest.z); // can't go below dest
			}
			else {velocity.z = max(-BIRD_ZV_FALL*BIRD_MAX_VEL, (velocity.z - BIRD_ZV_FALL*accel));} // glide down
		}
		// handle horizontal velocity component
		float const frame_dist_xy(p2p_dist_xy(pos, prev_frame_pos)), dist_thresh(frame_dist_xy + 0.05*radius);
		
		if (dist_xy_less_than(pos, dest, dist_thresh)) { // close enough in XY, but may need to descend in Z
			velocity.x = velocity.y = 0.0;
		}
		else {
			velocity += accel*dir; // accelerate in XY; acceleration always follows direction
			float const max_vel(BIRD_MAX_VEL*max(0.2f, min(1.0f, 0.05f*dist_xy/radius))); // prevent overshoot and circling by slowing when close
			float const xy_mag(velocity.xy_mag());
		
			if (xy_mag > max_vel) { // apply velocity cap
				float const xy_scale(max_vel/xy_mag);
				velocity.x *= xy_scale;
				velocity.y *= xy_scale;
			}
			// check for pending collisions every 16 frames, and reroute if needed
			if (((loc_ix + frame_counter) & 15) == 0 && placer.check_path_segment_coll(pos, dest, radius)) {
				if (placer.choose_bird_dest(pos, radius, loc_ix, dest, dest_dir)) {
					adjust_new_dest_zval();
					max_eq(start_end_zmax, dest.z);
					// no state update since we're already flying
				} // else continue to original dest
			}
		}
		if (dist_xy < radius) { // special case to pull bird in when close
			vector3d const delta(dest - pos);
			float const dist(delta.mag()), vmag(max(min(dist/timestep, min(velocity.mag(), 0.2f*BIRD_MAX_VEL)), 0.01f*BIRD_MAX_VEL)); // limit vmag to not overshoot
			velocity = delta*(vmag/dist);
		}
	}
	if (velocity != zero_vector) { // apply movement
		prev_frame_pos = pos;
		pos           += velocity*timestep;
		bcube         += (pos - bcube.get_cube_center()); // translate bcube to match - keep it centered on pos
		tile_changed  |= (get_tile_id_containing_point_no_xyoff(pos) != get_tile_id_containing_point_no_xyoff(prev_frame_pos));
		bird_moved     = 1;
	}
}

// this is here because only birds are updated each frame
void city_obj_placer_t::next_frame() {
	if (!animate2 || birds.empty()) return;
	float const enable_birds_dist(0.5f*(X_SCENE_SIZE + Y_SCENE_SIZE)); // half the pedestrian AI distance
	point const camera_bs(get_camera_pos() - get_tiled_terrain_model_xlate());
	if (!all_objs_bcube.closest_dist_less_than(camera_bs, enable_birds_dist)) return; // too far from the player
	float const timestep(min(fticks, 4.0f)); // clamp fticks to 100ms
	float const delta_dir(0.1*(1.0 - pow(0.7f, fticks))); // controls bird turning rate
	bool tile_changed(0), bird_moved(0);
	for (city_bird_t &bird : birds) {bird.next_frame(timestep, delta_dir, tile_changed, bird_moved, *this, bird_rgen);}
	
	if (tile_changed) { // update bird_groups; is there a more efficient way than rebuilding bird_groups each frame?
		bird_groups.clear();
		for (unsigned i = 0; i < birds.size(); ++i) {bird_groups.insert_obj_ix(birds[i].bcube, i);}
		bird_groups.create_groups(birds, all_objs_bcube);
	}
	else if (bird_moved) { // incrementally update group bcubes and all_objs_bcube
		unsigned start_ix(0);

		for (auto &g : bird_groups) {
			cube_t const prev(g);
			g.set_to_zeros(); // clear bcube for this pass
			assert(start_ix <= g.ix && g.ix <= birds.size());
			for (auto i = birds.begin()+start_ix; i != birds.begin()+g.ix; ++i) {g.assign_or_union_with_cube(i->bcube);}
			all_objs_bcube.union_with_cube(g);
			start_ix = g.ix;
		} // for g
	}
}

bool city_obj_placer_t::choose_bird_dest(point const &pos, float radius, unsigned &loc_ix, point &dest_pos, vector3d &dest_dir) {
	if (bird_locs.size() == birds.size()) return 0; // all locs in use
	assert(loc_ix < bird_locs.size());
	bird_place_t &old_loc(bird_locs[loc_ix]);
	assert(old_loc.in_use);
	float const xlate_dist(2.0*radius); // move away from the object to avoid intersecting it
	bool const find_closest = 0; // makes for easier debugging
	vector<pair<float, unsigned>> pref_locs;

	if (find_closest) {
		for (unsigned i = 0; i < bird_locs.size(); ++i) {
			if (i == loc_ix || bird_locs[i].in_use) continue;
			pref_locs.emplace_back(p2p_dist_xy(pos, bird_locs[i].pos), i);
		}
		sort(pref_locs.begin(), pref_locs.end()); // sort closes to furthest
	}
	// else find a random visible/reachable dest
	for (unsigned n = 0; n < 10; ++n) { // make up to 10 attempts to avoid long runtime
		unsigned const new_loc_ix((n < pref_locs.size()) ? pref_locs[n].second : (bird_rgen.rand() % bird_locs.size()));
		if (new_loc_ix == loc_ix) continue; // same
		bird_place_t &new_loc(bird_locs[new_loc_ix]);
		if (new_loc.in_use) continue;
		assert(new_loc.pos != pos);
		if (new_loc.pos.z > pos.z + BIRD_ZV_RISE*p2p_dist_xy(new_loc.pos, pos)) continue; // too steep a rise
		if (new_loc.pos.z < pos.z - BIRD_ZV_FALL*p2p_dist_xy(new_loc.pos, pos)) continue; // too steep a drop
		vector3d const dir((new_loc.pos - pos).get_norm());
		point const start_pos(pos + xlate_dist*dir), end_pos(new_loc.pos - xlate_dist*dir);
		if (check_path_segment_coll(start_pos, end_pos, radius)) continue;
		loc_ix   = new_loc_ix;
		dest_pos = new_loc.pos;
		dest_dir = new_loc.orient;
		old_loc.in_use = 0;
		new_loc.in_use = 1;
		return 1; // success
	} // for n
	return 0; // failed, try again next frame or next animation cycle
}
// returns: 0=no coll, 1=city object coll, 2=building coll
int city_obj_placer_t::check_path_segment_coll(point const &p1, point const &p2, float radius) const {
	float t(0.0); // unused
	if (line_intersect(p1, p2, t)) return 1;
	if (check_city_building_line_coll_bs_any(p1, p2)) return 2;

	if (radius > 0.0) { // cylinder case: check 4 points a distance radius from the center
		vector3d const dir((p2 - p1).get_norm()), v1(cross_product(dir, plus_z).get_norm()), v2(cross_product(v1, dir).get_norm()); // orthogonalize_dir?
		vector3d const z_off(0.0, 0.0, 2.0*radius); // move upward to clear any low-lying obstacles such as the source and dest objects, since we'll be flying upward anyway
		vector3d const offs[4] = {v1, -v1, v2, -v2};

		for (unsigned n = 0; n < 4; ++n) {
			vector3d const off(radius*offs[n] + z_off);
			point const p1o(p1 + off), p2o(p2 + off);
			if (line_intersect(p1o, p2o, t)) return 1; // doesn't include objects such as power lines
			if (check_city_building_line_coll_bs_any(p1o, p2o)) return 2; // doesn't include objects such as building rooftop signs?
		}
	}
	return 0;
}

void vect_bird_place_t::add_placement(cube_t const &obj, bool dim, bool dir, bool orient_dir, float spacing, rand_gen_t &rgen) {
	point pos(0.0, 0.0, obj.z2());
	pos[ dim] = obj.d[dim][dir];
	pos[!dim] = rgen.rand_uniform(obj.d[!dim][0]+spacing, obj.d[!dim][1]-spacing); // random position along the edge
	emplace_back(pos, dim, orient_dir);
}
void vect_bird_place_t::add_placement_rand_dim_dir(cube_t const &obj, float spacing, rand_gen_t &rgen) {
	bool const dir(rgen.rand_bool());
	add_placement(obj, rgen.rand_bool(), dir, dir, spacing, rgen);
}
void vect_bird_place_t::add_placement_top_center(cube_t const &obj, rand_gen_t &rgen) {
	emplace_back(cube_top_center(obj), rgen.rand_bool(), rgen.rand_bool());
}

