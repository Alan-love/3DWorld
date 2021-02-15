// 3D World - Building vs. Player/AI Interaction Logic
// by Frank Gennari 1/14/21

#include "3DWorld.h"
#include "function_registry.h"
#include "buildings.h"
#include "openal_wrap.h"

// physics constants, currently applied to balls
float const KICK_VELOCITY  = 0.0025;
float const THROW_VELOCITY = 0.0035;
float const MIN_VELOCITY   = 0.0001;
float const OBJ_DECELERATE = 0.008;
float const OBJ_GRAVITY    = 0.0003;
float const TERM_VELOCITY  = 1.0;
float const OBJ_ELASTICITY = 0.8;

bool do_room_obj_pickup(0), drop_last_pickup_object(0), show_bldg_pickup_crosshair(0);
int can_pickup_bldg_obj(0);
float office_chair_rot_rate(0.0), cur_building_sound_level(0.0);
bldg_obj_type_t bldg_obj_types[NUM_ROBJ_TYPES];

extern bool toggle_door_open_state;
extern int window_width, window_height, display_framerate, player_in_closet, frame_counter;
extern float fticks, CAMERA_RADIUS;
extern double tfticks, camera_zh;
extern building_params_t global_building_params;


// lights

float get_radius_for_room_light(room_object_t const &obj);
void register_building_sound(point const &pos, float volume);

bool building_t::toggle_room_light(point const &closest_to) { // Note: called by the player; closest_to is in building space, not camera space
	if (!has_room_geom()) return 0; // error?
	vector<room_object_t> &objs(interior->room_geom->objs);
	auto objs_end(objs.begin() + interior->room_geom->stairs_start); // skip stairs and elevators
	point query_pt(closest_to);
	if (is_rotated()) {do_xy_rotate_inv(bcube.get_cube_center(), query_pt);}
	int const room_id(get_room_containing_pt(query_pt));
	if (room_id < 0) return 0; // closest_to is not contained in a room of this building
	room_t const &room(get_room(room_id));
	point light_pos;
	float closest_dist_sq(0.0);
	unsigned closest_light(0);

	for (auto i = objs.begin(); i != objs_end; ++i) {
		if (!i->is_light_type() || i->room_id != room_id) continue; // not a light, or the wrong room
		if (bool(i->flags & RO_FLAG_IN_CLOSET) != bool(player_in_closet)) continue; // while in the closet, player can only toggle closet lights and not room lights
		if (!room.is_sec_bldg && get_floor_for_zval(i->z1()) != get_floor_for_zval(closest_to.z)) continue; // wrong floor (skip garages and sheds)
		point center(i->get_cube_center());
		if (is_rotated()) {do_xy_rotate(bcube.get_cube_center(), center);}
		float const dist_sq(p2p_dist_sq(closest_to, center));
		if (closest_dist_sq == 0.0 || dist_sq < closest_dist_sq) {closest_dist_sq = dist_sq; closest_light = (i - objs.begin()); light_pos = center;}
	} // for i
	room_object_t &light(objs[closest_light]);
	bool updated(0);

	for (auto i = objs.begin(); i != objs_end; ++i) { // toggle all lights on this floor of this room
		if (i->is_light_type() && i->room_id == room_id && i->z1() == light.z1()) { // Note: closet light should have a different z1 that should not match the room lights
			i->toggle_lit_state(); // Note: doesn't update indir lighting
			if (i->type == TYPE_LAMP) continue; // lamps don't affect room object ambient lighting, and don't require regenerating the vertex data, so skip the step below
			set_obj_lit_state_to(room_id, light.z2(), i->is_lit()); // update object lighting flags as well
			updated = 1;
		}
	} // for i
	if (updated) {interior->room_geom->clear_and_recreate_lights();} // recreate light geom with correct emissive properties
	point const sound_pos(get_camera_pos() + (light_pos - closest_to)); // Note: computed relative to closest_to so that this works for either camera or building coord space
	gen_sound(SOUND_CLICK, sound_pos);
	register_building_sound(light_pos, 0.1);
	//interior->room_geom->modified_by_player = 1; // should light state always be preserved?
	return 1;
}

bool building_t::set_room_light_state_to(room_t const &room, float zval, bool make_on) { // called by AI people
	if (!has_room_geom()) return 0; // error?
	if (room.is_hallway)  return 0; // don't toggle lights for hallways, which can have more than one light
	vector<room_object_t> &objs(interior->room_geom->objs);
	auto objs_end(objs.begin() + interior->room_geom->stairs_start); // skip stairs and elevators
	float const window_vspacing(get_window_vspace());
	bool updated(0);

	for (auto i = objs.begin(); i != objs_end; ++i) {
		if (i->type != TYPE_LIGHT) continue; // not a light (excludes lamps)
		if (i->z1() < zval || i->z1() > (zval + window_vspacing) || !room.contains_cube_xy(*i)) continue; // light is on the wrong floor or in the wrong room
		if (i->is_lit() != make_on) {i->toggle_lit_state(); updated = 1;} // Note: doesn't update indir lighting or room light value
	} // for i
	if (updated) {interior->room_geom->clear_and_recreate_lights();} // recreate light geom with correct emissive properties
	return updated;
}

void building_t::set_obj_lit_state_to(unsigned room_id, float light_z2, bool lit_state) {
	assert(has_room_geom());
	float const light_intensity(get_room(room_id).light_intensity);
	vector<room_object_t> &objs(interior->room_geom->objs);
	auto objs_end(objs.begin() + interior->room_geom->stairs_start); // skip stairs and elevators
	float const obj_zmin(light_z2 - get_window_vspace()); // get_floor_thickness()?
	bool was_updated(0);

	for (auto i = objs.begin(); i != objs_end; ++i) {
		if (i->room_id != room_id || i->z1() < obj_zmin || i->z1() > light_z2) continue; // wrong room or floor
		if (i->is_obj_model_type()) continue; // light_amt currently does not apply to 3D models; should it?

		if (i->type == TYPE_STAIR || i->type == TYPE_STAIR_WALL || i->type == TYPE_ELEVATOR || i->type == TYPE_LIGHT || i->type == TYPE_BLOCKER ||
			i->type == TYPE_COLLIDER || i->type == TYPE_SIGN || i->type == TYPE_WALL_TRIM || i->type == TYPE_RAILING || i->type == TYPE_BLINDS)
		{
			continue; // not a type that uses light_amt
		}
		else if (i->type == TYPE_WINDOW) {
			if (lit_state) {i->flags |= RO_FLAG_LIT;} else {i->flags &= ~RO_FLAG_LIT;}
		}
		else {
			if (lit_state) {i->light_amt += light_intensity;} else {i->light_amt = max((i->light_amt - light_intensity), 0.0f);} // shouldn't be negative, but clamp to 0 just in case
		}
		was_updated = 1;
	} // for i
	if (was_updated) {interior->room_geom->clear_materials();} // need to recreate them
}

bool building_room_geom_t::closet_light_is_on(cube_t const &closet) const {
	auto objs_end(objs.begin() + stairs_start); // skip stairs and elevators

	for (auto i = objs.begin(); i != objs_end; ++i) {
		if (i->type == TYPE_LIGHT && closet.contains_cube(*i)) {return i->is_lit();}
	}
	return 0;
}

// doors

int building_t::find_ext_door_close_to_point(tquad_with_ix_t &door, point const &pos, float dist) const {
	point query_pt(pos);
	if (is_rotated()) {do_xy_rotate_inv(bcube.get_cube_center(), query_pt);}
	int const room_id(get_room_containing_pt(query_pt));
	cube_t room_exp;

	if (room_id >= 0) { // pos is inside a room
		room_exp = get_room(room_id);
		room_exp.expand_by(get_wall_thickness()); // make sure it contains the door
	}
	// Note: returns the first exterior door found, assuming there can be at most one within dist of pos
	for (auto d = doors.begin(); d != doors.end(); ++d) {
		cube_t c(d->get_bcube());
		if (room_id >= 0 && !room_exp.contains_cube(c)) continue; // door not in the same room as pos - there is likely a wall between them
		c.expand_by_xy(dist);
		if (c.contains_pt(query_pt)) {door = *d; return (d - doors.begin());}
	} // for d
	return -1; // not found
}

void building_t::register_open_ext_door_state(int door_ix) {
	bool const is_open(door_ix >= 0), was_open(open_door_ix >= 0);
	if (is_open == was_open) return; // no state change
	unsigned const dix(is_open ? (unsigned)door_ix : (unsigned)open_door_ix);
	assert(dix < doors.size());
	point const door_center(doors[dix].get_bcube().get_cube_center()), sound_pos(door_center + get_camera_coord_space_xlate()); // convert to camera space
	gen_sound((is_open ? (unsigned)SOUND_DOOR_OPEN : (unsigned)SOUND_DOOR_CLOSE), sound_pos);
	register_building_sound(door_center, 0.5);
	open_door_ix = door_ix;
}

bool check_obj_dir_dist(point const &closest_to, vector3d const &in_dir, cube_t const &c, point const &center, float dmax) { // Note: only zvals of door are used, no rotate required
	if (!c.closest_dist_less_than(closest_to, dmax)) return 0; // too far
	if (in_dir == zero_vector) return 1; // no direction filter specified
	point vis_pt(center.x, center.y, closest_to.z); // use query point zval
	max_eq(vis_pt.z, c.z1()); // clamp visibility test point to z-range of door to allow the player to open the door even looking at the top or bottom of it
	min_eq(vis_pt.z, c.z2());
	return (dot_product(in_dir, (vis_pt - closest_to).get_norm()) > 0.5); // door is not in the correct direction, skip
}

bool building_t::toggle_door_state_closest_to(point const &closest_to, vector3d const &in_dir) { // called for the player
	if (!interior) return 0; // error?
	float const dmax(4.0*CAMERA_RADIUS), floor_spacing(get_window_vspace());
	float closest_dist_sq(0.0);
	unsigned door_ix(0), obj_ix(0);
	bool is_obj(0);

	if (!player_in_closet) { // if the player is in the closet, only the closet door can be opened
		for (auto i = interior->doors.begin(); i != interior->doors.end(); ++i) {
			if (i->z1() > closest_to.z || i->z2() < closest_to.z) continue; // wrong floor, skip
			point center(i->get_cube_center());
			if (is_rotated()) {do_xy_rotate(bcube.get_cube_center(), center);}
			float const dist_sq(p2p_dist_sq(closest_to, center));
			if (closest_dist_sq != 0.0 && dist_sq >= closest_dist_sq) continue; // not the closest
			if (!check_obj_dir_dist(closest_to, in_dir, *i, center, dmax)) continue; // door is not in the correct direction or too far away, skip
			closest_dist_sq = dist_sq;
			door_ix = (i - interior->doors.begin());
		} // for i
	}
	if (interior->room_geom) { // check for closet doors in houses and bathroom stalls in office buildings
		vector<room_object_t> &objs(interior->room_geom->objs);
		auto objs_end(objs.begin() + interior->room_geom->stairs_start); // skip stairs and elevators

		for (auto i = objs.begin(); i != objs_end; ++i) {
			// this loop only handles closets with small doors, cube bathroom stalls, and rotated office chairs
			if (i->type != TYPE_CLOSET && !(i->type == TYPE_STALL && i->shape == SHAPE_CUBE) && !(i->type == TYPE_OFF_CHAIR && (i->flags & RO_FLAG_RAND_ROT))) continue;
			if (i->type == TYPE_CLOSET && !i->is_small_closet()) continue; // not a closet with a small door
			point center(i->get_cube_center());
			center[i->dim] = i->d[i->dim][i->dir]; // use center of door, not center of closet
			if (is_rotated()) {do_xy_rotate(bcube.get_cube_center(), center);}
			if (fabs(center.z - closest_to.z) > 0.7*floor_spacing) continue; // wrong floor
			float const dist_sq(p2p_dist_sq(closest_to, center));
			if (closest_dist_sq != 0.0 && dist_sq >= closest_dist_sq) continue; // not the closest
			if (!check_obj_dir_dist(closest_to, in_dir, *i, center, dmax)) continue; // door is not in the correct direction or too far away, skip
			closest_dist_sq = dist_sq; obj_ix = (i - objs.begin());
			is_obj = 1;
		} // for i
	}
	if (closest_dist_sq == 0.0) return 0; // no door found

	if (is_obj) { // closet or bathroom stall
		auto &obj(interior->room_geom->objs[obj_ix]);

		if (obj.type == TYPE_OFF_CHAIR) { // handle rotate of office chair
			office_chair_rot_rate += 0.1;
			obj.flags |= RO_FLAG_ROTATING;
			// play a sound?
			return 0; // done, doesn't count as a door
		}
		if (obj.is_open()) {obj.flags &= ~RO_FLAG_OPEN;} // close
		else               {obj.flags |=  RO_FLAG_OPEN;} // open
		interior->room_geom->clear_static_vbos(); // need to regen object data
		
		if (obj.type == TYPE_CLOSET) {
			interior->room_geom->expand_object(obj); // expand any boxes so that the player can pick them up
			//interior->room_geom->clear_static_small_vbos(); // no longer needed since closet interior is always drawn
		}
		float const pitch((obj.type == TYPE_STALL) ? 2.0 : 1.0); // higher pitch for stalls
		point const door_center(obj.get_cube_center());
		play_door_open_close_sound(door_center, obj.is_open(), pitch);
		register_building_sound(door_center, 0.5);
	}
	else {toggle_door_state(door_ix, 1, 1);} // toggle state if interior door; player_in_this_building=1, by_player=1
	//interior->room_geom->modified_by_player = 1; // should door state always be preserved?
	return 1;
}

void building_t::toggle_door_state(unsigned door_ix, bool player_in_this_building, bool by_player) { // called by the player or AI
	assert(interior && door_ix < interior->doors.size());
	door_t &door(interior->doors[door_ix]);
	door.open ^= 1; // toggle open state
	invalidate_nav_graph(); // we just invalidated the AI navigation graph and must rebuild it; any in-progress paths may have people walking through closed doors
	interior->door_state_updated = 1; // required for AI navigation logic to adjust to this change
	interior->doors_to_update.push_back(door_ix);

	if (player_in_this_building) {
		point const door_center(door.get_cube_center());
		play_door_open_close_sound(door_center, door.open);
		if (by_player) {register_building_sound(door_center, 0.5);}
	}
}

void building_t::play_door_open_close_sound(point const &pos, bool open, float pitch) const {
	point pos_rot(pos);
	if (is_rotated()) {do_xy_rotate(bcube.get_cube_center(), pos_rot);}
	point const sound_pos(pos_rot + get_camera_coord_space_xlate()); // convert to camera space
	gen_sound((open ? (unsigned)SOUND_DOOR_OPEN : (unsigned)SOUND_DOOR_CLOSE), sound_pos, 1.0, pitch);
}

// dynamic objects: elevators and balls

void apply_object_bounce(vector3d &velocity, vector3d const &cnorm) {
	float const vmag(velocity.mag());
	if (vmag < TOLERANCE) return;
	vector3d v_ref;
	calc_reflection_angle(velocity/vmag, v_ref, cnorm);
	velocity = (OBJ_ELASTICITY*vmag)*v_ref;
}

void building_t::update_player_interact_objects(point const &player_pos) {
	assert(interior);
	interior->update_elevators(player_pos, get_floor_thickness());
	if (!has_room_geom()) return; // nothing else to do
	float const player_radius(get_scaled_player_radius()), player_z1(player_pos.z - camera_zh - player_radius), player_z2(player_pos.z);
	float const fc_thick(0.5*get_floor_thickness());
	static int last_sound_frame(0);
	static point last_sound_pt(all_zeros);

	for (auto c = interior->room_geom->objs.begin(); c != interior->room_geom->objs.end(); ++c) { // check for other objects to collide with (including stairs)
		if (c->no_coll() || !c->has_dstate()) continue; // Note: no test of player_coll flag
		assert(c->type == TYPE_LG_BALL); // currently, only large balls have has_dstate()
		assert(c->obj_id < interior->room_geom->obj_dstate.size());
		room_obj_dstate_t &dstate(interior->room_geom->get_dstate(*c));
		vector3d &velocity(dstate.velocity);
		point const center(c->get_cube_center()), ppos(player_pos.x, player_pos.y, center.z); // use zval of object (Z range was checked above)
		float const radius(c->get_radius()), r_sum(radius + player_radius);
		point new_center(center);
		bool const was_dynamic(c->is_dynamic());
			
		if (c->z2() > player_z1 && c->z1() < player_z2 && dist_xy_less_than(ppos, center, r_sum)) { // collides with the player
			vector3d const dir((center - ppos).get_norm());
			new_center = (center + (r_sum - p2p_dist_xy(ppos, center))*dir); // move so that it no longer collides
			c->flags  |= RO_FLAG_DYNAMIC; // make it dynamic

			if ((frame_counter - last_sound_frame) > 1.0f*TICKS_PER_SECOND && p2p_dist(new_center, last_sound_pt) > radius) { // play at most once per second
				gen_sound(SOUND_KICK_BALL, (get_camera_pos() + (new_center - player_pos)), 0.5);
				last_sound_frame = frame_counter;
				last_sound_pt    = new_center;
			}
			velocity = KICK_VELOCITY*dir;
		}
		else if (was_dynamic) { // not colliding, but is moving
			point const test_pt(center.x, center.y, (center.z - radius - 0.1*fc_thick));
			bool on_floor(0);

			for (auto f = interior->floors.begin(); f != interior->floors.end(); ++f) {
				if (f->contains_pt(test_pt)) {on_floor = 1; break;}
			}
			//cout << TXT(velocity.str()) << TXT(on_floor) << TXT(center.z) << TXT(test_pt.z) << TXT(ground_floor_z1+fc_thick) << endl; // TESTING

			if (on_floor) { // moving on the floor, apply surface friction
				velocity *= (1.0f - OBJ_DECELERATE*fticks);
				if (velocity.mag() < MIN_VELOCITY) {velocity = zero_vector;} // zero velocity if stopped
			}
			else { // in the air - apply gravity
				velocity.z -= OBJ_GRAVITY*fticks; // apply gravitational acceleration
				max_eq(velocity.z, -TERM_VELOCITY);
			}
			if (velocity == zero_vector) { // stopped
				c->flags &= ~RO_FLAG_DYNAMIC; // clear dynamic flag
				interior->update_dynamic_draw_data(); // remove from dynamic objects
				interior->room_geom->clear_static_small_vbos(); // add to static objects
			}
			else { // move based on velocity
				new_center += velocity*fticks;
				// TODO: roll/rotate
			}
		}
		if (new_center != center) { // check for collisions and move to new location
			vector3d cnorm;

			if (interior->check_sphere_coll(new_center, center, radius, c, &cnorm)) {
				if (cnorm == plus_z) { // collision with the floor
					if (fabs(velocity.z) < 0.25*OBJ_GRAVITY*fticks) {velocity.z = 0.0;} // zero velocity z component if near zero to reduce instability
				}
				apply_object_bounce(velocity, cnorm);
			}
			point const prev_new_center(new_center);
			
			if (move_sphere_to_valid_part(new_center, center, radius) && new_center != prev_new_center) { // collision with exterior wall
				apply_object_bounce(velocity, (new_center - prev_new_center).get_norm());
			}
			if (!was_dynamic) {interior->room_geom->clear_static_small_vbos();} // static => dynamic transition, need to remove from static object vertex data
			interior->update_dynamic_draw_data();
			c->translate(new_center - center);
		}
	} // for c
	if (drop_last_pickup_object) {
		maybe_add_last_pickup_room_object(player_pos);
		drop_last_pickup_object = 0;
	}
}

bool building_interior_t::update_elevators(point const &player_pos, float floor_thickness) { // Note: player_pos is in building space
	float const z_space(0.05*floor_thickness); // to prevent z-fighting
	static int prev_move_dir(2); // starts at not-moving

	// Note: the player can only be in one elevator at a time, so we can exit when we find the first containing elevator
	for (auto e = elevators.begin(); e != elevators.end(); ++e) { // find containing elevator (optimization + need to know z-range of elevator shaft)
		if (!e->contains_pt(player_pos)) continue; // player not in this elevator
		unsigned const elevator_id(e - elevators.begin());
		auto objs_start(room_geom->objs.begin() + room_geom->stairs_start); // start with stairs and elevators

		for (auto i = objs_start; i != room_geom->objs.end(); ++i) { // find elevator car and see if player is in it
			if (i->type != TYPE_ELEVATOR || i->room_id != elevator_id || !i->contains_pt(player_pos)) continue;
			bool const move_dir(player_pos[!i->dim] < i->get_center_dim(!i->dim)); // player controls up/down direction based on which side of the elevator they stand on
			float dist(min(0.5f*CAMERA_RADIUS, 0.04f*i->dz()*fticks)*(move_dir ? 1.0 : -1.0)); // clamp to half camera radius to avoid falling through the floor for low framerates
			if (move_dir) {min_eq(dist, (e->z2() - i->z2() - z_space));} // going up
			else          {max_eq(dist, (e->z1() - i->z1() + z_space));} // going down
			if (fabs(dist) < 0.0001*z_space) break; // no movement, at top or bottom of elevator shaft (check with a tolerance)
			i->z1() += dist; i->z2() += dist;
			update_dynamic_draw_data(); // clear dynamic material vertex data (for all elevators) and recreate their VBOs
			
			if ((int)move_dir != prev_move_dir) {
				gen_sound(SOUND_SLIDING, get_camera_pos(), 0.2); // play this sound quietly when the elevator starts moving or changes direction
				register_building_sound(player_pos, 0.4);
			}
			prev_move_dir = move_dir;
			return 1; // done
		} // for i
		break; // player in elevator shaft (on top of elevator?) but not inside elevator car, done
	} // for e
	prev_move_dir = 2;
	return 0;
}

bool building_t::move_sphere_to_valid_part(point &pos, point const &p_last, float radius) const { // Note: only moves in XY
	point const init_pos(pos);
	float xy_area_contained(0.0);
	cube_t sphere_bcube; sphere_bcube.set_from_sphere(pos, radius);
	cube_t valid_region(bcube);
	valid_region.expand_by(-radius);
	valid_region.clamp_pt(pos); // keep pos within the valid building bcube

	for (auto i = parts.begin(); i != get_real_parts_end(); ++i) {
		if (!i->intersects(sphere_bcube)) continue;
		cube_t overlap(sphere_bcube);
		overlap.intersect_with_cube(*i);
		xy_area_contained += overlap.dx()*overlap.dy();
	}
	if (xy_area_contained > 0.99*sphere_bcube.dx()*sphere_bcube.dy()) return (pos != init_pos); // sphere contained in union of parts (not outside the building)

	// find part containing p_last and clamp to that part
	for (auto i = parts.begin(); i != get_real_parts_end(); ++i) {
		if (!i->contains_pt(p_last)) continue; // not the part containing the previous pos
		cube_t bounds(*i);
		bounds.expand_by_xy(-radius);
		bounds.clamp_pt_xy(pos);
		return 1;
	}
	//assert(0); // should never get here (except for FP error?)
	pos = p_last; // restore original position
	return 1;
}

// ray queries

// center, -x, +x, -y, +y, -z, +z
void get_sphere_boundary_pts(point const &center, float radius, point pts[7]) {
	pts[0] = center;

	for (unsigned dim = 0, ix = 1; dim < 3; ++dim) {
		vector3d dir(zero_vector);
		dir[dim]  = radius;
		pts[ix++] = center - dir;
		pts[ix++] = center + dir;
	}
}

bool building_t::is_pt_visible(point const &p1, point const &p2) const {
	if (!interior) return 1;
	if (is_light_occluded(p1, p2)) return 0; // okay to call for non-light point; checks walls, ceilings, and floors
	float const wall_thickness(get_wall_thickness());
	
	for (auto i = interior->doors.begin(); i != interior->doors.end(); ++i) {
		if (i->open) continue; // check only closed doors
		cube_t door(*i);
		door.expand_in_dim(i->dim, 0.5*wall_thickness); // increase door thickness
		if (door.line_intersects(p1, p2)) return 0;
	}
	return 1;
}
bool building_t::is_sphere_visible(point const &center, float radius, point const &pt) const {
	if (!interior) return 1;
	point pts[7];
	get_sphere_boundary_pts(center, radius, pts);
	for (unsigned n = 0; n < 7; ++n) {if (is_pt_visible(pts[n], pt)) return 1;}
	return 0;
}

bool building_t::is_pt_lit(point const &pt) const {
	if (!has_room_geom()) return 0; // no lights
	int const room_id(get_room_containing_pt(pt)); // call this only once on center in is_sphere_lit()?
	if (room_id < 0) return 0; // outside building?
	room_t const &room(get_room(room_id));
	auto objs_end(interior->room_geom->objs.begin() + interior->room_geom->stairs_start); // skip stairs and elevators

	for (auto i = interior->room_geom->objs.begin(); i != objs_end; ++i) {
		if (!i->is_light_type() || !i->is_lit()) continue; // not a light, or light not on
		bool const same_room((int)i->room_id == room_id);
		//if (!same_room) continue; // different room (optimization); too strong?
		//bool const same_floor(fabs(i->z1() - pt.z) < floor_spacing); // doesn't work with lamps
		bool const same_floor(room.is_sec_bldg || get_floor_for_zval(pt.z) == get_floor_for_zval(i->z1()));
		if (!i->has_stairs() && !same_floor) continue; // different floors, and no stairs (optimization)
		if (same_floor && same_room) return 1; // same floor of same room, should be visible (optimization)
		point const center(i->get_cube_center());
		if (!dist_less_than(center, pt, 0.95*get_radius_for_room_light(*i))) continue;
		if (is_pt_visible(center, pt)) return 1; // likely returns true if same room
	} // for i
	return 0;
}
bool building_t::is_sphere_lit(point const &center, float radius) const {
	if (!has_room_geom()) return 0; // no lights (optimization)
	point pts[7];
	get_sphere_boundary_pts(center, radius, pts);
	for (unsigned n = 0; n < 7; ++n) {if (is_pt_lit(pts[n])) return 1;}
	return 0;
}

// object types/pickup

void setup_bldg_obj_types() {
	static bool was_setup(0);
	if (was_setup) return; // nothing to do
	was_setup = 1;
	// player_coll, ai_coll, pickup, attached, is_model, lg_sm, value, weight, name
	//                                                pc ac pu at im ls value  weight  name
	bldg_obj_types[TYPE_TABLE     ] = bldg_obj_type_t(1, 1, 1, 0, 0, 1, 70.0,  40.0,  "table");
	bldg_obj_types[TYPE_CHAIR     ] = bldg_obj_type_t(0, 1, 1, 0, 0, 1, 50.0,  30.0,  "chair"); // skip player collisions because they can be in the way and block the path in some rooms
	bldg_obj_types[TYPE_STAIR     ] = bldg_obj_type_t(1, 0, 0, 1, 0, 1, 0.0,   0.0,   "stair");
	bldg_obj_types[TYPE_STAIR_WALL] = bldg_obj_type_t(1, 1, 0, 1, 0, 1, 0.0,   0.0,   "stairs wall");
	bldg_obj_types[TYPE_ELEVATOR  ] = bldg_obj_type_t(1, 1, 0, 1, 0, 0, 0.0,   0.0,   "elevator");
	bldg_obj_types[TYPE_LIGHT     ] = bldg_obj_type_t(0, 0, 1, 0, 0, 0, 40.0,  5.0,   "light");
	bldg_obj_types[TYPE_RUG       ] = bldg_obj_type_t(0, 0, 1, 0, 0, 1, 50.0,  20.0,  "rug");
	bldg_obj_types[TYPE_PICTURE   ] = bldg_obj_type_t(0, 0, 1, 0, 0, 1, 100.0, 1.0,   "picture"); // should be random value
	bldg_obj_types[TYPE_WBOARD    ] = bldg_obj_type_t(0, 0, 1, 0, 0, 1, 50.0,  25.0,  "whiteboard");
	bldg_obj_types[TYPE_BOOK      ] = bldg_obj_type_t(0, 0, 1, 0, 0, 3, 10.0,  1.0,   "book");
	bldg_obj_types[TYPE_BCASE     ] = bldg_obj_type_t(1, 1, 0, 1, 0, 3, 150.0, 100.0, "bookcase");
	bldg_obj_types[TYPE_TCAN      ] = bldg_obj_type_t(0, 1, 1, 0, 0, 1, 12.0,  2.0,   "trashcan"); // skip player collisions because they can be in the way and block the path in some rooms
	bldg_obj_types[TYPE_DESK      ] = bldg_obj_type_t(1, 1, 0, 0, 0, 1, 100.0, 80.0,  "desk");
	bldg_obj_types[TYPE_BED       ] = bldg_obj_type_t(1, 1, 1, 0, 0, 1, 300.0, 200.0, "bed");
	bldg_obj_types[TYPE_WINDOW    ] = bldg_obj_type_t(0, 0, 0, 1, 0, 1, 0.0,   0.0,   "window");
	bldg_obj_types[TYPE_BLOCKER   ] = bldg_obj_type_t(0, 0, 0, 0, 0, 0, 0.0,   0.0,   "<blocker>"); // not a drawn object; block other objects, but not the player or AI
	bldg_obj_types[TYPE_COLLIDER  ] = bldg_obj_type_t(1, 1, 0, 0, 0, 0, 0.0,   0.0,   "<collider>"); // not a drawn object; block the player and AI
	bldg_obj_types[TYPE_CUBICLE   ] = bldg_obj_type_t(0, 0, 0, 1, 0, 1, 500.0, 250.0, "cubicle"); // skip collisions because they have their own colliders
	bldg_obj_types[TYPE_STALL     ] = bldg_obj_type_t(1, 1, 1, 1, 0, 1, 40.0,  20.0,  "bathroom divider"); // can pick up short sections of bathroom stalls (urinal dividers)
	bldg_obj_types[TYPE_SIGN      ] = bldg_obj_type_t(0, 0, 1, 0, 0, 3, 10.0,  1.0,   "sign");
	bldg_obj_types[TYPE_COUNTER   ] = bldg_obj_type_t(1, 1, 0, 1, 0, 1, 0.0,   0.0,   "kitchen counter");
	bldg_obj_types[TYPE_CABINET   ] = bldg_obj_type_t(0, 0, 0, 0, 0, 1, 0.0,   0.0,   "kitchen cabinet");
	bldg_obj_types[TYPE_KSINK     ] = bldg_obj_type_t(1, 1, 0, 1, 0, 1, 0.0,   0.0,   "kitchen sink");
	bldg_obj_types[TYPE_BRSINK    ] = bldg_obj_type_t(1, 1, 0, 1, 0, 1, 0.0,   0.0,   "bathroom sink");
	bldg_obj_types[TYPE_PLANT     ] = bldg_obj_type_t(0, 0, 1, 0, 0, 3, 18.0,  8.0,   "potted plant");
	bldg_obj_types[TYPE_DRESSER   ] = bldg_obj_type_t(1, 1, 0, 1, 0, 3, 120.0, 120.0, "dresser");
	bldg_obj_types[TYPE_NIGHTSTAND] = bldg_obj_type_t(1, 1, 1, 0, 0, 3, 60.0,  35.0,  "nightstand");
	bldg_obj_types[TYPE_FLOORING  ] = bldg_obj_type_t(0, 0, 0, 1, 0, 1, 0.0,   0.0,   "flooring");
	bldg_obj_types[TYPE_CLOSET    ] = bldg_obj_type_t(1, 1, 1, 1, 0, 3, 0.0,   0.0,   "closet"); // closets can't be picked up, but they can block a pickup
	bldg_obj_types[TYPE_WALL_TRIM ] = bldg_obj_type_t(0, 0, 0, 1, 0, 2, 0.0,   0.0,   "wall trim");
	bldg_obj_types[TYPE_RAILING   ] = bldg_obj_type_t(1, 0, 0, 1, 0, 2, 0.0,   0.0,   "railing");
	bldg_obj_types[TYPE_CRATE     ] = bldg_obj_type_t(1, 1, 1, 0, 0, 2, 10.0,  12.0,  "crate"); // should be random value
	bldg_obj_types[TYPE_BOX       ] = bldg_obj_type_t(1, 1, 1, 0, 0, 2, 5.0,   8.0,   "box");   // should be random value
	bldg_obj_types[TYPE_MIRROR    ] = bldg_obj_type_t(0, 0, 1, 0, 0, 1, 40.0,  15.0,  "mirror");
	bldg_obj_types[TYPE_SHELVES   ] = bldg_obj_type_t(1, 1, 1, 0, 0, 2, 0.0,   0.0,   "shelves");
	bldg_obj_types[TYPE_KEYBOARD  ] = bldg_obj_type_t(0, 0, 1, 0, 0, 2, 15.0,  2.0,   "keyboard");
	bldg_obj_types[TYPE_SHOWER    ] = bldg_obj_type_t(1, 1, 0, 1, 0, 1, 0.0,   0.0,   "shower");
	bldg_obj_types[TYPE_RDESK     ] = bldg_obj_type_t(1, 1, 0, 1, 0, 1, 800.0, 400.0, "reception desk");
	bldg_obj_types[TYPE_BOTTLE    ] = bldg_obj_type_t(0, 0, 1, 0, 0, 2, 1.0,   1.0,   "bottle");
	bldg_obj_types[TYPE_WINE_RACK ] = bldg_obj_type_t(1, 1, 1, 1, 0, 3, 75.0,  40.0,  "wine rack");
	bldg_obj_types[TYPE_COMPUTER  ] = bldg_obj_type_t(0, 0, 1, 0, 0, 2, 500.0, 20.0,  "computer");
	bldg_obj_types[TYPE_MWAVE     ] = bldg_obj_type_t(0, 0, 1, 0, 0, 1, 100.0, 30.0,  "microwave oven");
	bldg_obj_types[TYPE_PAPER     ] = bldg_obj_type_t(0, 0, 1, 0, 0, 2, 0.0,   0.01,  "sheet of paper"); // will have a random value that's often 0
	bldg_obj_types[TYPE_BLINDS    ] = bldg_obj_type_t(0, 0, 0, 0, 0, 1, 0.0,   0.0,   "window blinds");
	bldg_obj_types[TYPE_PEN       ] = bldg_obj_type_t(0, 0, 1, 0, 0, 2, 0.10,  0.02,  "pen");
	bldg_obj_types[TYPE_PENCIL    ] = bldg_obj_type_t(0, 0, 1, 0, 0, 2, 0.10,  0.02,  "pencil");
	bldg_obj_types[TYPE_PAINTCAN  ] = bldg_obj_type_t(0, 0, 1, 0, 0, 2, 12.0,  8.0,   "paint can");
	bldg_obj_types[TYPE_LG_BALL   ] = bldg_obj_type_t(0, 0, 1, 0, 0, 2, 15.0,  1.2,   "ball");
	// 3D models
	bldg_obj_types[TYPE_TOILET    ] = bldg_obj_type_t(1, 1, 1, 1, 1, 0, 120.0, 120.0, "toilet");
	bldg_obj_types[TYPE_SINK      ] = bldg_obj_type_t(1, 1, 1, 1, 1, 0, 80.0,  80.0,  "sink");
	bldg_obj_types[TYPE_TUB       ] = bldg_obj_type_t(1, 1, 0, 1, 1, 1, 250.0, 200.0, "bathtub");
	bldg_obj_types[TYPE_FRIDGE    ] = bldg_obj_type_t(1, 1, 0, 1, 1, 0, 700.0, 300.0, "refrigerator"); // no pickup, too large and may want to keep it for future hunger bar
	bldg_obj_types[TYPE_STOVE     ] = bldg_obj_type_t(1, 1, 1, 1, 1, 0, 400.0, 200.0, "stove");
	bldg_obj_types[TYPE_TV        ] = bldg_obj_type_t(1, 1, 1, 0, 1, 1, 400.0, 70.0,  "TV");
	bldg_obj_types[TYPE_MONITOR   ] = bldg_obj_type_t(1, 1, 1, 0, 1, 1, 250.0, 15.0,  "computer monitor");
	bldg_obj_types[TYPE_COUCH     ] = bldg_obj_type_t(1, 1, 1, 0, 1, 0, 600.0, 300.0, "couch");
	bldg_obj_types[TYPE_OFF_CHAIR ] = bldg_obj_type_t(1, 1, 1, 0, 1, 0, 150.0, 60.0,  "office chair");
	bldg_obj_types[TYPE_URINAL    ] = bldg_obj_type_t(1, 1, 1, 1, 1, 0, 100.0, 80.0,  "urinal");
	bldg_obj_types[TYPE_LAMP      ] = bldg_obj_type_t(0, 0, 1, 0, 1, 0, 25.0,  12.0,  "lamp");
	bldg_obj_types[TYPE_WASHER    ] = bldg_obj_type_t(1, 1, 1, 1, 1, 0, 300.0, 160.0, "washer");
	bldg_obj_types[TYPE_DRYER     ] = bldg_obj_type_t(1, 1, 1, 1, 1, 0, 300.0, 180.0, "dryer");
	//                                                pc ac pu at im ls value  weight  name
}

bldg_obj_type_t const &get_room_obj_type(room_object_t const &obj) {
	assert(obj.type < NUM_ROBJ_TYPES);
	return bldg_obj_types[obj.type];
}
bldg_obj_type_t get_taken_obj_type(room_object_t const &obj) {
	if (obj.type == TYPE_PICTURE && (obj.flags & RO_FLAG_TAKEN1)) {return bldg_obj_type_t(0, 0, 1, 0, 0, 1, 20.0, 6.0, "picture frame");} // second item to take from picture

	if (obj.type == TYPE_BED) { // player_coll, ai_coll, pickup, attached, is_model, lg_sm, value, weight, name
		if (obj.flags & RO_FLAG_TAKEN2) {return bldg_obj_type_t(0, 0, 1, 0, 0, 1, 250.0, 80.0, "mattress"  );} // third item to take from bed
		if (obj.flags & RO_FLAG_TAKEN1) {return bldg_obj_type_t(0, 0, 1, 0, 0, 1, 80.0,  4.0,  "bed sheets");} // second item to take from bed
		return bldg_obj_type_t(0, 0, 1, 0, 0, 2, 20.0, 1.0, "pillow"); // first item to take from bed
	}
	if (obj.type == TYPE_PLANT && !(obj.flags & RO_FLAG_ADJ_BOT)) { // plant not on a table/desk
		if (obj.flags & RO_FLAG_TAKEN2) {return bldg_obj_type_t(0, 0, 1, 0, 0, 1, 10.0, 10.0, "plant pot");} // third item to take
		if (obj.flags & RO_FLAG_TAKEN1) {return bldg_obj_type_t(0, 0, 1, 0, 0, 1, 1.0,  10.0, "dirt"     );} // second item to take
		return bldg_obj_type_t(0, 0, 1, 0, 0, 2, 25.0, 5.0, "plant"); // first item to take
	}
	if (obj.type == TYPE_COMPUTER && (obj.flags & RO_FLAG_WAS_EXP)) {return bldg_obj_type_t(0, 0, 1, 0, 0, 2, 100.0, 20.0, "old computer");}
	if (obj.type == TYPE_LG_BALL) {
		bldg_obj_type_t type(get_room_obj_type(obj));
		type.name = ((obj.flags2 & 1) ? "basketball" : "soccer ball"); // use a more specific type name; all other fields are shared across balls
		return type;
	}
	return get_room_obj_type(obj); // default value
}
rand_gen_t rgen_from_obj(room_object_t const &obj) {
	rand_gen_t rgen;
	rgen.set_state(12345*abs(obj.x1()), 67890*abs(obj.y1()));
	return rgen;
}
float get_obj_value(room_object_t const &obj) {
	float value(get_taken_obj_type(obj).value);
	if (obj.type == TYPE_CRATE || obj.type == TYPE_BOX) {value *= (1 + (rgen_from_obj(obj).rand() % 20));}
	else if (obj.type == TYPE_PAPER) {
		rand_gen_t rgen(rgen_from_obj(obj));
		if (rgen.rand_float() < 0.25) { // 25% of papers have some value
			float const val_mult((rgen.rand_float() < 0.25) ? 10 : 1); // 25% of papers have higher value
			value = val_mult*(2 + (rgen.rand()%10))*(1 + (rgen.rand()%10));
		}
	}
	return value;
}
float get_obj_weight(room_object_t const &obj) {
	return get_taken_obj_type(obj).weight; // constant per object type, for now, but really should depend on object size/volume
}
void show_object_info(room_object_t const &obj) {
	float const value(get_obj_value(obj));
	std::ostringstream oss;
	oss << get_taken_obj_type(obj).name << ": value $";
	if (value < 1.0 && value > 0.0) {oss << ((value < 0.1) ? "0.0" : "0.") << round_fp(100.0*value);} // make sure to print the leading/trailing zero for cents
	else {oss << value;}
	oss << " weight " << get_obj_weight(obj) << " lbs";
	print_text_onscreen(oss.str(), GREEN, 1.0, 3*TICKS_PER_SECOND, 0);
}

class player_inventory_t {
	vector<room_object_t> carried; // not sure if we need to track carried inside the house and/or total carried
	float cur_value, cur_weight, tot_value, tot_weight;
public:
	player_inventory_t() {clear();}

	void clear() { // called on player death?
		cur_value = cur_weight = tot_value = tot_weight = 0.0;
		carried.clear();
	}
	bool can_pick_up_item(room_object_t const &obj) const {
		return ((cur_weight + get_obj_weight(obj)) <= global_building_params.player_weight_limit);
	}
	void add_item(room_object_t const &obj) {
		cur_value  += get_obj_value (obj);
		cur_weight += get_obj_weight(obj);
		carried.push_back(obj);
	}
	bool drop_last_item(room_object_t &obj, bool dynamic_only) {
		if (carried.empty()) return 0;
		if (dynamic_only && !carried.back().has_dstate()) return 0;
		obj = carried.back(); // deep copy
		cur_value  -= get_obj_value (obj);
		cur_weight -= get_obj_weight(obj);
		assert(cur_value >= 0.0 && cur_weight >= 0.0); // is this okay if there's FP rounding error?
		carried.pop_back();
		return 1;
	}
	void collect_items() {
		if (carried.empty()) return; // nothing to add
		std::ostringstream oss;
		oss << "Added value $" << cur_value << " Added weight " << cur_weight << " lbs\n";
		tot_value  += cur_value;  cur_value  = 0.0;
		tot_weight += cur_weight; cur_weight = 0.0;
		carried.clear(); // or add to collected?
		oss << "Total value $" << tot_value << " Total weight " << tot_weight << " lbs";
		print_text_onscreen(oss.str(), GREEN, 1.0, 4*TICKS_PER_SECOND, 0);
	}
	void show_stats() const {
		float const aspect_ratio((float)window_width/(float)window_height);

		if (cur_weight > 0.0 || tot_weight > 0.0) { // don't show stats until the player has picked something up
			std::ostringstream oss;
			oss << "Current $" << cur_value << " / " << cur_weight << " lbs  Total $" << tot_value << " / " << tot_weight << " lbs";
			draw_text(GREEN, -0.005*aspect_ratio, -0.011, -0.02, oss.str());
		}
		// display sound meter
		float const lvl(min(cur_building_sound_level, 1.0f));
		unsigned const num_bars(round_fp(20.0*lvl));

		if (num_bars > 0) {
			colorRGBA const color(lvl, (1.0 - lvl), 0.0, 1.0); // green => yellow => orange => red
			draw_text(color, -0.005*aspect_ratio, -0.01, -0.02, std::string(num_bars, '#'));
		}
	}
};

player_inventory_t player_inventory;

void register_building_sound_for_obj(room_object_t const &obj, point const &pos) {
	float const weight(get_obj_weight(obj)), volume((weight <= 1.0) ? 0.0 : min(1.0f, 0.01f*weight)); // heavier objects make more sound
	register_building_sound(pos, volume);
}

bool building_t::player_pickup_object(point const &at_pos, vector3d const &in_dir) {
	if (!has_room_geom()) return 0;
	return interior->room_geom->player_pickup_object(*this, at_pos, in_dir);
}
bool building_room_geom_t::player_pickup_object(building_t &building, point const &at_pos, vector3d const &in_dir) {
	int const obj_id(find_nearest_pickup_object(building, at_pos, in_dir, 3.0*CAMERA_RADIUS));
	if (obj_id < 0) return 0;
	room_object_t &obj(get_room_object_by_index(obj_id));
	bool const can_pick_up(player_inventory.can_pick_up_item(obj));

	if (!do_room_obj_pickup) { // player has not used the pickup key, but we can still use this to notify the player that an object can be picked up
		can_pickup_bldg_obj = (can_pick_up ? 1 : 2);
		return 0;
	}
	if (obj.type == TYPE_SHELVES) {
		assert(!(obj.flags & RO_FLAG_EXPANDED)); // should not have been expanded
		expand_object(obj);
		bool const picked_up(player_pickup_object(building, at_pos, in_dir)); // call recursively on shelves contents
		// if we picked up an object, assume the VBOs have already been updated; otherwise we need to update them to expand this object
		if (!picked_up) {create_small_static_vbos(building);} // assumes expanded objects are all "small"
		return picked_up;
	}
	if (!can_pick_up) {
		std::ostringstream oss;
		oss << "Over weight limit of " << global_building_params.player_weight_limit << " lbs";
		print_text_onscreen(oss.str(), RED, 1.0, 1.5*TICKS_PER_SECOND, 0);
		return 0;
	}
	show_object_info(obj);
	gen_sound(SOUND_ITEM, get_camera_pos(), 0.25);
	register_building_sound_for_obj(obj, at_pos);
	remove_object(obj_id, building);
	return 1;
}

void building_t::register_player_enter_building() const {
	// nothing to do yet
}
void building_t::register_player_exit_building() const {
	player_inventory.collect_items();
}

bool has_cube_line_coll(point const &p1, point const &p2, vect_cube_t const &cubes) {
	for (auto i = cubes.begin(); i != cubes.end(); ++i) {
		if (i->line_intersects(p1, p2)) return 1;
	}
	return 0;
}
bool building_t::check_for_wall_ceil_floor_int(point const &p1, point const &p2) const {
	if (!interior) return 0;
	for (unsigned d = 0; d < 2; ++d) {if (has_cube_line_coll(p1, p2, interior->walls[d])) return 1;}
	return (has_cube_line_coll(p1, p2, interior->ceilings) || has_cube_line_coll(p1, p2, interior->floors)); // or is only checking one good enough?
}

bool object_has_something_on_it(room_object_t const &obj, vector<room_object_t> const &objs) {
	// only these types can have objects on them (what about TYPE_SHELF?)
	if (obj.type != TYPE_TABLE && obj.type != TYPE_DESK && obj.type != TYPE_COUNTER && obj.type != TYPE_DRESSER &&
		obj.type != TYPE_NIGHTSTAND && obj.type != TYPE_BOX && obj.type != TYPE_CRATE) return 0;

	for (auto i = objs.begin(); i != objs.end(); ++i) {
		if (i->type == TYPE_BLOCKER) continue; // ignore blockers (from removed objects)
		if (i->z1() == obj.z2() && i->intersects_xy(obj)) return 1; // zval has to match exactly
	}
	return 0;
}

int building_room_geom_t::find_nearest_pickup_object(building_t const &building, point const &at_pos, vector3d const &in_dir, float range) const {
	int closest_obj_id(-1);
	float dmin_sq(0.0);
	point const p2(at_pos + in_dir*range);

	for (unsigned vect_id = 0; vect_id < 2; ++vect_id) {
		auto const &obj_vect((vect_id == 1) ? expanded_objs : objs);
		unsigned const obj_id_offset((vect_id == 1) ? objs.size() : 0); // treat {objs + expanded_objs} as a single contiguous range

		for (auto i = obj_vect.begin(); i != obj_vect.end(); ++i) {
			assert(i->type < NUM_ROBJ_TYPES);
			if (!bldg_obj_types[i->type].pickup) continue; // this object type can't be picked up
			cube_t obj_bcube(*i);
			if (i->type == TYPE_PEN || i->type == TYPE_PENCIL) {obj_bcube.expand_in_dim(!i->dim, i->get_sz_dim(!i->dim));}
			point p1c(at_pos), p2c(p2);
			if (!do_line_clip(p1c, p2c, obj_bcube.d)) continue; // test ray intersection vs. bcube
			float const dsq(p2p_dist(at_pos, p1c)); // use closest intersection point
			if (dmin_sq > 0.0 && dsq > dmin_sq)       continue; // not the closest
		
			if (i->type == TYPE_CLOSET || (i->type == TYPE_STALL && i->shape != SHAPE_SHORT)) { // can only take short stalls (separating urinals)
				if (!i->is_open() && !i->contains_pt(at_pos)) { // stalls/closets block the player from taking toilets/boxes unless open, or the player is inside
					closest_obj_id = -1;
					dmin_sq = dsq;
				}
				continue;
			}
			if (i->type == TYPE_MIRROR && !i->is_house())                 continue; // can only pick up mirrors from houses, not office buildings
			if (i->type == TYPE_TABLE && i->shape == SHAPE_CUBE)          continue; // can only pick up short (TV) tables and cylindrical tables
			if (i->type == TYPE_BED   && (i->flags & RO_FLAG_TAKEN3))     continue; // can only take pillow, sheets, and mattress - not the frame
			if (i->type == TYPE_SHELVES && (i->flags & RO_FLAG_EXPANDED)) continue; // shelves are already expanded, can no longer select this object
			if (object_has_something_on_it(*i, obj_vect))                 continue; // can't remove a table, etc. that has something on it
			if (building.check_for_wall_ceil_floor_int(at_pos, p1c))      continue; // skip if it's on the other side of a wall, ceiling, or floor
			closest_obj_id = (i - obj_vect.begin()) + obj_id_offset; // valid pickup object
			dmin_sq = dsq; // this object is the closest, even if it can't be picked up
		} // for i
	} // for vect_id
	return closest_obj_id;
}

room_object_t &building_room_geom_t::get_room_object_by_index(unsigned obj_id) {
	if (obj_id < objs.size()) {return objs[obj_id];}
	unsigned const exp_obj_id(obj_id - objs.size());
	assert(exp_obj_id < expanded_objs.size());
	return expanded_objs[exp_obj_id];
}

void building_room_geom_t::remove_object(unsigned obj_id, building_t &building) {
	room_object_t &obj(get_room_object_by_index(obj_id));
	room_object_t const old_obj(obj); // deep copy
	assert(obj.type != TYPE_ELEVATOR); // elevators require special updates for drawing logic and cannot be removed at this time
	player_inventory.add_item(obj);
	bldg_obj_type_t const type(get_taken_obj_type(obj)); // capture type before updating obj
	bool const is_light(obj.type == TYPE_LIGHT);

	if (obj.type == TYPE_PICTURE && !(obj.flags & RO_FLAG_TAKEN1)) {obj.flags |= RO_FLAG_TAKEN1;} // take picture, leave frame
	else if (obj.type == TYPE_BED) {
		if      (obj.flags & RO_FLAG_TAKEN2) {obj.flags |= RO_FLAG_TAKEN3;} // take mattress
		else if (obj.flags & RO_FLAG_TAKEN1) {obj.flags |= RO_FLAG_TAKEN2;} // take sheets
		else {obj.flags |= RO_FLAG_TAKEN1;} // take pillow(s)
	}
	else if (obj.type == TYPE_PLANT && !(obj.flags & RO_FLAG_ADJ_BOT)) { // plant not on a table/desk
		if      (obj.flags & RO_FLAG_TAKEN2) {obj.type = TYPE_BLOCKER;} // take pot - gone
		else if (obj.flags & RO_FLAG_TAKEN1) {obj.flags |= RO_FLAG_TAKEN2;} // take dirt
		else {obj.flags |= RO_FLAG_TAKEN1;} // take plant
	}
	else {obj.type = TYPE_BLOCKER; obj.flags = (RO_FLAG_NOCOLL | RO_FLAG_INVIS);} // replace it with an invisible blocker that won't collide with anything
	if (is_light) {clear_and_recreate_lights();}
	update_draw_state_for_room_object(old_obj, building);
}
void building_room_geom_t::update_draw_state_for_room_object(room_object_t const &obj, building_t &building) { // Note: called when adding or removing objects
	// reuild necessary VBOs and other data structures
	if (obj.is_dynamic()) {mats_dynamic.clear();} // dynamic object
	else { // static object
		bldg_obj_type_t const type(get_taken_obj_type(obj));
		if (type.lg_sm & 2) {create_small_static_vbos(building);} // small object
		if (type.lg_sm & 1) {create_static_vbos      (building);} // large object
		if (type.is_model ) {create_obj_model_insts  (building);} // 3D model
		if (type.ai_coll  ) {building.invalidate_nav_graph();} // removing this object may affect the AI navigation graph
	}
	modified_by_player = 1; // flag so that we avoid re-generating room geom if the player leaves and comes back
}

int building_room_geom_t::find_avail_obj_slot() const {
	auto objs_end(objs.begin() + stairs_start); // skip stairs and elevators

	for (auto i = objs.begin(); i != objs_end; ++i) {
		if (i->type == TYPE_BLOCKER) {return int(i - objs.begin());} // blockers are used as temporaries for room object placement and to replace removed objects
	}
	return -1; // no slot found
}

bool building_room_geom_t::add_room_object(room_object_t const &obj, building_t &building, bool set_obj_id, vector3d const &velocity) {
	assert(obj.type != TYPE_LIGHT && get_room_obj_type(obj).pickup); // currently must be a pickup object, and not a light
	int const obj_id(find_avail_obj_slot());
	if (obj_id < 0) return 0; // no slot found
	room_object_t &added_obj(get_room_object_by_index(obj_id));
	added_obj = obj; // overwrite with new object
	if (set_obj_id) {added_obj.obj_id = (uint16_t)(obj.has_dstate() ? allocate_dynamic_state() : obj_id);}
	if (velocity != zero_vector) {get_dstate(added_obj).velocity = velocity;}
	update_draw_state_for_room_object(obj, building);
	return 1;
}
bool building_t::maybe_add_last_pickup_room_object(point const &player_pos) {
	assert(has_room_geom());
	bool const dynamic_only = 1;
	room_object_t obj;
	if (!player_inventory.drop_last_item(obj, dynamic_only)) return 0;
	float const cradius(get_scaled_player_radius());
	point const obj_bot_center(obj.xc(), obj.yc(), obj.z1());
	point dest(player_pos + (1.2f*(cradius + obj.get_radius()))*cview_dir);
	dest.z -= 0.5*cradius; // slightly below the player's face
	obj.translate(dest - obj_bot_center);
	if (obj.has_dstate()) {obj.flags |= RO_FLAG_DYNAMIC;} // make it dynamic, assuming it will be dropped/thrown
	if (!interior->room_geom->add_room_object(obj, *this, 1, THROW_VELOCITY*cview_dir)) return 0;
	gen_sound(SOUND_OBJ_FALL, (get_camera_pos() + (dest - player_pos)));
	register_building_sound_for_obj(obj, player_pos);
	return 1;
}

unsigned building_room_geom_t::allocate_dynamic_state() {
	unsigned const ix(obj_dstate.size());
	obj_dstate.push_back(room_obj_dstate_t());
	return ix;
}
room_obj_dstate_t &building_room_geom_t::get_dstate(room_object_t const &obj) {
	assert(obj.has_dstate());
	assert(obj.obj_id < obj_dstate.size());
	return obj_dstate[obj.obj_id];
}

// sound/audio tracking

void register_building_sound(point const &pos, float volume) {
	if (volume == 0.0 || !show_bldg_pickup_crosshair) return; // only when in gameplay/item pickup mode
	assert(volume > 0.0); // can't be negative
	// TODO: alert AT at this position if (volume > ALERT_THRESH)
	cur_building_sound_level += volume;
}

// gameplay logic

void register_ai_player_coll(pedestrian_t const &person) {
	static double last_coll_time(0.0);
	
	if (tfticks - last_coll_time > 2.0*TICKS_PER_SECOND) {
		gen_sound(SOUND_SCREAM1, get_camera_pos());
		last_coll_time = tfticks;
	}
	add_camera_filter(colorRGBA(RED, 0.25), 1, -1, CAM_FILT_DAMAGE); // 4 ticks of red damage
}

void building_gameplay_action_key(int mode) {
	// show crosshair on first pickup because it's too difficult to pick up objects without it
	if      (mode == 1) {do_room_obj_pickup = show_bldg_pickup_crosshair = 1;} // 'e'
	else if (mode == 2) {drop_last_pickup_object = 1;} // 'E'
	else                {toggle_door_open_state  = 1;} // 'q'
}

void building_gameplay_next_frame() {
	if (display_framerate) {player_inventory.show_stats();} // controlled by framerate toggle
	
	if (office_chair_rot_rate != 0.0) { // update office chair rotation
		office_chair_rot_rate *= exp(-0.05*fticks); // exponential slowdown
		if (office_chair_rot_rate < 0.001) {office_chair_rot_rate = 0.0;} // stop rotating
	}
	// reset state for next frame
	cur_building_sound_level = min(1.2f, max(0.0f, (cur_building_sound_level - 0.01f*fticks))); // gradual decrease
	can_pickup_bldg_obj = 0;
	do_room_obj_pickup  = 0;
}

