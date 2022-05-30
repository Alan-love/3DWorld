// 3D World - Building Basement and Parking Garage Logic
// by Frank Gennari 03/11/2022

#include "3DWorld.h"
#include "function_registry.h"
#include "buildings.h"
#include "city.h" // for car_t

enum {PIPE_TYPE_SEWER=0, PIPE_TYPE_CW, PIPE_TYPE_HW, PIPE_TYPE_GAS, NUM_PIPE_TYPES};

extern city_params_t city_params; // for num_cars

car_t car_from_parking_space(room_object_t const &o);
void subtract_cube_from_floor_ceil(cube_t const &c, vect_cube_t &fs);
bool line_int_cubes_exp(point const &p1, point const &p2, vect_cube_t const &cubes, vector3d const &expand);


bool enable_parked_cars() {return (city_params.num_cars > 0 && !city_params.car_model_files.empty());}

template<typename T> void add_to_and_clear(T &src, T &dest) {
	vector_add_to(src, dest);
	src.clear();
}

void subtract_cubes_from_cube_split_in_dim(cube_t const &c, vect_cube_t const &sub, vect_cube_t &out, vect_cube_t &out2, unsigned dim) {
	out.clear();
	out.push_back(c);

	for (auto s = sub.begin(); s != sub.end(); ++s) {
		if (!c.intersects(c)) continue; // no overlap with orig cube (optimization)
		out2.clear();

		// clip all of out against *s, write results to out2, then swap with out
		for (auto i = out.begin(); i != out.end(); ++i) {
			if (!i->intersects(*s)) {out2.push_back(*i); continue;} // no overlap, keep entire cube

			if (i->d[dim][0] < s->d[dim][0]) { // lo side
				out2.push_back(*i);
				out2.back().d[dim][1] = s->d[dim][0];
			}
			if (i->d[dim][1] > s->d[dim][1]) { // hi side
				out2.push_back(*i);
				out2.back().d[dim][0] = s->d[dim][1];
			}
		} // for i
		out.swap(out2);
	} // for s
}

bool building_t::add_water_heaters(rand_gen_t &rgen, room_t const &room, float zval, unsigned room_id, float tot_light_amt, unsigned objs_start) {
	float const floor_spacing(get_window_vspace()), height(get_floor_ceil_gap());
	float const radius((is_house ? 0.18 : 0.20)*height); // larger radius for office buildings
	cube_t const room_bounds(get_walkable_room_bounds(room));
	cube_t place_area(room_bounds);
	place_area.expand_by(-(1.05*radius + get_trim_thickness())); // account for the pan
	if (!place_area.is_strictly_normalized()) return 0; // too small to place water heater
	vect_room_object_t &objs(interior->room_geom->objs);
	point center(0.0, 0.0, zval);
	bool first_xdir(rgen.rand_bool()), first_ydir(rgen.rand_bool()), first_dim(0);

	if (is_house) { // random corner selection
		first_dim = rgen.rand_bool();
	}
	else { // select corners furthest from the door (back wall)
		first_dim = (room.dy() < room.dx()); // face the shorter dim for office buildings so that we can place longer rows
		vect_door_stack_t const &doorways(get_doorways_for_room(room, zval));

		for (auto i = doorways.begin(); i != doorways.end(); ++i) {
			bool const dir(room.get_center_dim(i->dim) < i->get_center_dim(i->dim));
			(i->dim ? first_ydir : first_xdir) = !dir; // opposite the door
			//first_dim = i->dim; // place against back wall; too restrictive?
		}
	}
	for (unsigned n = 0; n < 5; ++n) { // make 5 attempts to place a water heater - one in each corner and 1 along a random wall for variety
		bool dim(0), dir(0), xdir(0), ydir(0);

		if (n < 4) { // corner
			dim  = (first_dim  ^ (n >= 2));
			xdir = (first_xdir ^ bool(n & 1));
			ydir = (first_ydir ^ bool(n & 1));
			dir  = (dim ? ydir : xdir);
			center.x = place_area.d[0][xdir];
			center.y = place_area.d[1][ydir];
		}
		else { // wall
			dim = rgen.rand_bool();
			dir = rgen.rand_bool(); // choose a random wall
			center[ dim] = place_area.d[dim][dir]; // against this wall
			center[!dim] = rgen.rand_uniform(place_area.d[!dim][0], place_area.d[!dim][1]);
		}
		cube_t c(get_cube_height_radius(center, radius, height));
		if (is_cube_close_to_doorway(c, room, 0.0, 1) || interior->is_blocked_by_stairs_or_elevator(c)) continue;
		cube_t c_exp(c);
		c_exp.expand_by_xy(0.2*radius); // small keepout in XY
		c_exp.d[dim][!dir] += (dir ? -1.0 : 1.0)*0.25*radius; // add more keepout in front where the controls are
		c_exp.intersect_with_cube(room); // don't pick up objects on the other side of the wall
		if (overlaps_other_room_obj(c_exp, objs_start)) continue; // check existing objects, in particular storage room boxes that will have already been placed
		objs.emplace_back(c, TYPE_WHEATER, room_id, dim, !dir, (is_house ? RO_FLAG_IS_HOUSE : 0), tot_light_amt, SHAPE_CYLIN);

		if (!is_house && n < 4) { // office building, placed at corner; try to add additional water heaters along the wall
			vector3d step;
			step[!dim] = 2.2*radius*((dim ? xdir : ydir) ? -1.0 : 1.0); // step in the opposite dim
			// ideally we would iterate over all the plumbing fixtures to determine water usage, but they might not be placed yet, so instead count rooms;
			// but rooms can be of very different sizes, so maybe counting volume is the best?
			float tot_volume(0.0);
			for (auto i = parts.begin(); i != get_real_parts_end(); ++i) {tot_volume += i->get_volume();}
			tot_volume /= floor_spacing*floor_spacing*floor_spacing;
			unsigned const max_wh(max(1, min(5, round_fp(tot_volume/1000.0))));

			for (unsigned n = 1; n < max_wh; ++n) { // one has already been placed
				c.translate(step);
				if (!room_bounds.contains_cube(c)) break; // went outside the room, done
				if (is_cube_close_to_doorway(c, room, 0.0, 1) || interior->is_blocked_by_stairs_or_elevator(c)) continue; // bad placement, skip
				c_exp.translate(step);
				if (overlaps_other_room_obj(c_exp, objs_start)) continue; // check existing objects
				objs.emplace_back(c, TYPE_WHEATER, room_id, dim, !dir, 0, tot_light_amt, SHAPE_CYLIN);
			} // for n
		}
		return 1; // done
	} // for n
	return 0;
}

bool building_t::add_basement_utility_objs(rand_gen_t rgen, room_t const &room, float zval, unsigned room_id, float tot_light_amt, unsigned objs_start) {
	// for now, this is only the water heater; we may want to add a furnace, etc. later
	bool was_added(add_water_heaters(rgen, room, zval, room_id, tot_light_amt, objs_start));
	// TODO: add TYPE_FURNACE
	return was_added;
}

// Note: this function is here rather than in building_rooms.cpp because utility rooms are connected to utilities in the basement, and it's similar to the code above
bool building_t::add_office_utility_objs(rand_gen_t rgen, room_t const &room, float &zval, unsigned room_id, float tot_light_amt, unsigned objs_start) {
	zval       = add_flooring(room, zval, room_id, tot_light_amt, FLOORING_CONCRETE); // add concreate and move the effective floor up
	objs_start = interior->room_geom->objs.size(); // exclude this from collision checks
	// for now, this is only the water heater; we may want to add other objects later
	if (!add_water_heaters(rgen, room, zval, room_id, tot_light_amt, objs_start)) return 0; // and TYPE_FURNACE?
	cube_t place_area(get_walkable_room_bounds(room));
	place_model_along_wall(OBJ_MODEL_SINK, TYPE_SINK, room, 0.45, rgen, zval, room_id, tot_light_amt, place_area, objs_start, 0.6); // place janitorial sink
	add_door_sign("Utility", room, zval, room_id, tot_light_amt);
	return 1;
}

vector3d building_t::get_parked_car_size() const {
	vector3d car_sz(get_nom_car_size());
	if (car_sz.x != 0.0) return car_sz; // valid car size, use this
	return get_window_vspace()*vector3d(1.67, 0.73, 0.45); // no cars, use size relative to building floor spacing
}

void building_t::add_parking_garage_objs(rand_gen_t rgen, room_t const &room, float zval, unsigned room_id, unsigned floor_ix,
	unsigned num_floors, unsigned &nlights_x, unsigned &nlights_y, float &light_delta_z)
{
	assert(has_room_geom());
	rgen.rseed1 += 123*floor_ix; // make it unique per floor
	rgen.rseed2 += room_id;
	// rows are separated by walls and run in dim, with a road and parking spaces on either side of it;
	// spaces are arranged in !dim, with roads along the edges of the building that connect to the roads of each row
	bool const dim(room.dx() < room.dy()); // long/primary dim; cars are lined up along this dim, oriented along the other dim
	vector3d const car_sz(get_parked_car_size()), parking_sz(1.1*car_sz.x, 1.4*car_sz.y, 1.5*car_sz.z); // space is somewhat larger than a car; car length:width = 2.3
	float const window_vspacing(get_window_vspace()), floor_thickness(get_floor_thickness()), wall_thickness(1.2*get_wall_thickness()), wall_hc(0.5*wall_thickness); // thicker
	float const ceiling_z(zval + window_vspacing - floor_thickness); // Note: zval is at floor level, not at the bottom of the room
	float const pillar_width(0.5*car_sz.y), pillar_hwidth(0.5*pillar_width), beam_hwidth(0.5*pillar_hwidth), road_width(2.3*car_sz.y); // road wide enough for two cars
	float const wid_sz(room.get_sz_dim(dim)), len_sz(room.get_sz_dim(!dim)), wid_sz_spaces(wid_sz - 2.0*road_width);
	float const min_strip_sz(2.0*parking_sz.x + road_width + max(wall_thickness, pillar_width)); // road + parking spaces on each side + wall/pillar
	assert(car_sz.z < (window_vspacing - floor_thickness)); // sanity check; may fail for some user parameters, but it's unclear what we do in that case
	unsigned const num_space_wid(wid_sz_spaces/parking_sz.y), num_full_strips(max(1U, unsigned(len_sz/min_strip_sz))); // take the floor
	bool const half_strip((num_full_strips*min_strip_sz + parking_sz.x + road_width + wall_thickness) < len_sz); // no space for a full row, add a half row
	bool const half_row_side(half_strip ? rgen.rand_bool() : 0); // pick a random side
	unsigned const num_rows(2*num_full_strips + half_strip), num_strips(num_full_strips + half_strip), num_walls(num_strips - 1);
	unsigned const capacity(num_rows*num_space_wid); // ignoring space blocked by stairs and elevators
	unsigned &nlights_len(dim ? nlights_x : nlights_y), &nlights_wid(dim ? nlights_y : nlights_x);
	nlights_len = num_rows; // lights over each row of parking spaces
	nlights_wid = round_fp(0.25*wid_sz/parking_sz.y); // 4 parking spaces per light on average, including roads
	//cout << TXT(nlights_len) << TXT(nlights_wid) << TXT(num_space_wid) << TXT(num_rows) << TXT(capacity) << endl; // TESTING
	assert(num_space_wid   >= 4); // must fit at least 4 cars per row
	
	// add walls and pillars between strips
	vect_room_object_t &objs(interior->room_geom->objs);
	unsigned const objs_start(objs.size());
	colorRGBA const wall_color(WHITE);
	cube_t room_floor_cube(room), virt_room_for_wall(room);
	set_cube_zvals(room_floor_cube, zval, ceiling_z);
	cube_t wall(room_floor_cube), pillar(room_floor_cube), beam(room_floor_cube);
	wall.expand_in_dim(dim, -road_width); // wall ends at roads that line the sides of the room; include pillar for better occluder and in case the pillar is skipped
	assert(wall.is_strictly_normalized());
	float wall_spacing(len_sz/(num_walls + 1));
	float const pillar_shift(0.01*pillar_width); // small value to avoid z-fighting
	float const wall_len(wall.get_sz_dim(dim) + 2.0f*pillar_shift), pillar_start(wall.d[dim][0] + pillar_hwidth - pillar_shift);
	float const row_width(wall_spacing - wall_thickness), space_length(0.5f*(row_width - road_width)), beam_spacing(len_sz/num_rows);
	unsigned const num_pillars(max(2U, unsigned(round_fp(0.25*wall_len/parking_sz.y)))); // every 4 spaces, at least 2 at the ends of the wall
	float const pillar_spacing((wall_len - pillar_width)/(num_pillars - 1)), beam_delta_z(0.95*wall.dz()), tot_light_amt(room.light_intensity);
	bool short_sides[2] = {0,0};

	if (half_strip) {
		short_sides[half_row_side] = 1;
		virt_room_for_wall.d[!dim][half_row_side] += (half_row_side ? 1.0 : -1.0)*space_length;
		wall_spacing = virt_room_for_wall.get_sz_dim(!dim)/(num_walls + 1); // recalculate wall spacing
	}
	light_delta_z = beam_delta_z - wall.dz(); // negative
	beam.z1()    += beam_delta_z; // shift the bottom up to the ceiling
	vect_cube_t obstacles, obstacles_exp, obstacles_ps, wall_parts, temp;
	// get obstacles for walls with and without clearance; maybe later add entrance/exit ramps, etc.
	interior->get_stairs_and_elevators_bcubes_intersecting_cube(room_floor_cube, obstacles, 0.0); // without clearance, for beams
	interior->get_stairs_and_elevators_bcubes_intersecting_cube(room_floor_cube, obstacles_exp, 0.9*window_vspacing); // with clearance in front, for walls and pillars
	cube_with_ix_t const &ramp(interior->pg_ramp);
	bool const is_top_floor(floor_ix+1 == num_floors);
	
	// add ramp if one was placed during floorplanning, before adding parking spaces
	// Note: lights can be very close to ramps, but I haven't actually seen them touch; do we need to check for and handle that case?
	if (!ramp.is_all_zeros()) {
		bool const dim(ramp.ix >> 1), dir(ramp.ix & 1), is_blocked(is_top_floor && interior->ignore_ramp_placement);
		cube_t rc(ramp); // ramp clipped to this parking garage floor
		set_cube_zvals(rc, zval, (zval + window_vspacing));
		unsigned const flags(is_blocked ? 0 : RO_FLAG_OPEN); // ramp is open if the top exit is open
		objs.emplace_back(rc, TYPE_RAMP, room_id, dim, dir, flags, tot_light_amt, SHAPE_ANGLED, wall_color);
		obstacles    .push_back(rc); // don't place parking spaces next to the ramp
		obstacles_exp.push_back(rc); // clip beams to ramp
		obstacles_exp.back().expand_in_dim( dim,      road_width); // keep entrance and exit areas clear of parking spaces, even the ones against exterior walls
		obstacles_exp.back().expand_in_dim(!dim, 0.75*road_width); // keep walls and pillars away from the sides of ramps
		obstacles_ps .push_back(obstacles_exp.back()); // also keep parking spaces away from ramp
		// add ramp railings
		bool const side(ramp.get_center_dim(!dim) < room.get_center_dim(!dim)); // which side of the ramp the railing is on (opposite the wall the ramp is against)
		float const railing_thickness(0.4*wall_thickness), ramp_length(rc.get_sz_dim(dim)), dir_sign(dir ? 1.0 : -1.0), side_sign(side ? 1.0 : -1.0), shorten_factor(0.35);
		cube_t railing(rc);
		railing.d[!dim][!side] = railing.d[!dim][side] - side_sign*railing_thickness;
		railing.z1() += 0.5*railing_thickness; // place bottom of bar along ramp/floor
		cube_t ramp_railing(railing);
		ramp_railing.d[dim][dir] -= dir_sign*shorten_factor*ramp_length; // shorten length to only the lower part
		ramp_railing.z2() -= shorten_factor*railing.dz(); // shorten height by the same amount to preserve the slope
		colorRGBA const railing_color(LT_GRAY);
		objs.emplace_back(ramp_railing, TYPE_RAILING, room_id, dim, dir, RO_FLAG_OPEN, tot_light_amt, SHAPE_CUBE, railing_color); // lower railing
		set_cube_zvals(railing, rc.z2(), (rc.z2() + window_vspacing));
		railing.translate_dim(!dim, side_sign*railing_thickness); // shift off the ramp and onto the ajdacent floor

		if (!is_top_floor) { // add side railing for lower level
			railing.d[dim][!dir] += dir_sign*shorten_factor*ramp_length; // shorten length to only the upper part
			objs.emplace_back(railing, TYPE_RAILING, room_id, dim, 0, (RO_FLAG_OPEN | RO_FLAG_TOS), tot_light_amt, SHAPE_CUBE, railing_color);
		}
		else if (!is_blocked) { // add upper railings at the top for the full length
			railing.translate_dim( dim, -0.5*dir_sign*railing_thickness); // shift down the ramp a bit
			objs.emplace_back(railing, TYPE_RAILING, room_id, dim, 0, (RO_FLAG_OPEN | RO_FLAG_TOS), tot_light_amt, SHAPE_CUBE, railing_color);
			cube_t back_railing(rc);
			set_cube_zvals(back_railing, railing.z1(), railing.z2());
			back_railing.translate_dim( dim, -dir_sign*railing_thickness); // shift onto the ajdacent floor
			back_railing.translate_dim(!dim, 0.5*side_sign*railing_thickness); // shift away from the exterior wall
			back_railing.d[dim][dir] = back_railing.d[dim][!dir] + dir_sign*railing_thickness;
			objs.emplace_back(back_railing, TYPE_RAILING, room_id, !dim, 0, (RO_FLAG_OPEN | RO_FLAG_TOS), tot_light_amt, SHAPE_CUBE, railing_color);
		}
	}
	// add walls and pillars
	bool const no_sep_wall(num_walls == 0 || (capacity < 100 && (room_id & 1))); // use room_id rather than rgen so that this agrees between floors
	bool const split_sep_wall(!no_sep_wall && (num_pillars >= 5 || (num_pillars >= 4 && rgen.rand_bool())));
	bool const have_cent_stairs(can_extend_pri_hall_stairs_to_pg()); // assume pri hall stairs were extended down to PG
	float sp_const(0.0);
	if      (no_sep_wall)      {sp_const = 0.25;} // no separator wall, minimal clearance around stairs
	else if (have_cent_stairs) {sp_const = 0.50;} // central stairs should open along the wall, not a tight space, need less clearance
	else if (split_sep_wall)   {sp_const = 0.75;} // gap should provide access, need slightly less clearance
	else                       {sp_const = 1.00;} // stairs may cut through/along full wall, need max clearance
	float const space_clearance(sp_const*max(0.5f*window_vspacing, parking_sz.y));
	// obstacles with clearance all sides, for parking spaces
	interior->get_stairs_and_elevators_bcubes_intersecting_cube(room_floor_cube, obstacles_ps, max(space_clearance, 0.9f*window_vspacing), space_clearance);
	if (interior->room_geom->wall_ps_start == 0) {interior->room_geom->wall_ps_start = objs.size();} // set if not set, on first level
	float center_pos(wall.get_center_dim(dim));
	// if there's an odd number of pillars, move the gap between two pillars on one side or the other
	if (split_sep_wall && (num_pillars & 1)) {center_pos += (rgen.rand_bool() ? -1.0 : 1.0)*0.5*pillar_spacing;}
	vect_cube_t pillars; // added after wall segments

	for (unsigned n = 0; n < num_walls+2; ++n) { // includes room far walls
		if (n < num_walls) { // interior wall
			float const pos(virt_room_for_wall.d[!dim][0] + (n + 1)*wall_spacing); // reference from the room far wall, assuming we can fit a full width double row strip
			set_wall_width(wall,   pos, wall_hc, !dim);
			set_wall_width(pillar, pos, pillar_hwidth, !dim);
			
			if (!no_sep_wall) {
				cube_t walls[2] = {wall, wall};

				if (split_sep_wall) { // add a gap between the walls for people to walk through
					walls[0].d[dim][1] = center_pos - 0.4*window_vspacing;
					walls[1].d[dim][0] = center_pos + 0.4*window_vspacing;
				}
				for (unsigned side = 0; side < (split_sep_wall ? 2U : 1U); ++side) {
					subtract_cubes_from_cube_split_in_dim(walls[side], obstacles_exp, wall_parts, temp, dim);
			
					for (auto const &w : wall_parts) {
						if (w.get_sz_dim(dim) < 2.0*window_vspacing) continue; // too short, skip
						objs.emplace_back(w, TYPE_PG_WALL, room_id, !dim, 0, 0, tot_light_amt, SHAPE_CUBE, wall_color, 0);
					}
				} // for side
			}
		}
		else { // room wall
			bool const side(n == num_walls+1);
			pillar.d[!dim][ side] = room.d[!dim][side];
			pillar.d[!dim][!side] = room.d[!dim][side] + (side ? -1.0 : 1.0)*pillar_hwidth; // half the width of an interior wall pillar
		}
		for (unsigned p = 0; p < num_pillars; ++p) { // add support pillars
			float const ppos(pillar_start + p*pillar_spacing);
			set_wall_width(pillar, ppos, pillar_hwidth, dim);
			if (has_bcube_int_xy(pillar, obstacles_exp)) continue; // skip entire pillar if it intersects stairs or an elevator
			pillars.push_back(pillar);
		} // for p
	} // for n
	for (auto const &p : pillars) {objs.emplace_back(p, TYPE_PG_WALL, room_id, !dim, 0, 0, tot_light_amt, SHAPE_CUBE, wall_color, 1);}

	// add beams in !dim, at and between pillars
	unsigned const beam_flags(RO_FLAG_NOCOLL | RO_FLAG_HANGING);

	for (unsigned p = 0; p < (4*(num_pillars - 1) + 1); ++p) { // add beams, 4 per pillar
		float const ppos(pillar_start + 0.25*p*pillar_spacing);
		set_wall_width(beam, ppos, beam_hwidth, dim);
		subtract_cubes_from_cube_split_in_dim(beam, obstacles, wall_parts, temp, !dim);
		
		for (auto const &w : wall_parts) {
			if (min(w.dx(), w.dy()) > beam_hwidth) {objs.emplace_back(w, TYPE_PG_WALL, room_id, !dim, 0, beam_flags, tot_light_amt, SHAPE_CUBE, wall_color, 2);}
		}
	}
	// add beams in dim for each row of lights
	for (unsigned n = 0; n < num_rows; ++n) {
		float const pos(room.d[!dim][0] + (n + 0.5)*beam_spacing);
		cube_t beam(room_floor_cube);
		beam.z1() += beam_delta_z; // shift the bottom up to the ceiling
		set_wall_width(beam, pos, beam_hwidth, !dim);
		subtract_cubes_from_cube_split_in_dim(beam, obstacles, wall_parts, temp, dim);

		for (auto const &w : wall_parts) {
			if (min(w.dx(), w.dy()) > beam_hwidth) {objs.emplace_back(w, TYPE_PG_WALL, room_id, !dim, 0, beam_flags, tot_light_amt, SHAPE_CUBE, wall_color, 2);}
		}
	}

	// add parking spaces on both sides of each row (one side if half row)
	cube_t row(wall); // same length as the wall; includes the width of the pillars
	row.z2() = row.z1() + 0.001*window_vspacing; // slightly above the floor
	float const space_width(row.get_sz_dim(dim)/num_space_wid), strips_start(virt_room_for_wall.d[!dim][0]), wall_half_gap(2.0*wall_hc), space_shrink(row_width - space_length);
	bool const add_cars(enable_parked_cars() && !is_rotated()); // skip cars for rotated buildings
	unsigned const max_handicap_spots(capacity/20 + 1);
	unsigned num_handicap_spots(0);

	for (unsigned n = 0; n < num_strips; ++n) {
		assert(space_length > 0.0);
		assert(space_width  > 0.0);
		assert(wall_spacing > 2.0*wall_half_gap);
		row.d[!dim][0] = strips_start + (n + 0)*wall_spacing + wall_half_gap;
		row.d[!dim][1] = strips_start + (n + 1)*wall_spacing - wall_half_gap;
		assert(row.is_strictly_normalized());
		if (row.get_sz_dim(!dim) < 1.2*space_shrink) continue; // space too small, likely due to incorrect building scale relative to car size; skip parking space placement

		for (unsigned d = 0; d < 2; ++d) { // for each side of the row
			bool const at_ext_wall[2] = {(n == 0 && d == 0), (n+1 == num_strips && d == 1)}, at_either_ext_wall(at_ext_wall[0] || at_ext_wall[1]);
			if ((short_sides[0] && at_ext_wall[0]) || (short_sides[1] && at_ext_wall[1])) continue; // skip this row
			float row_left_edge(row.d[dim][0]); // spaces start flush with the row, or flush with the room if this is the exterior wall
			unsigned num_spaces_per_row(num_space_wid);

			if (at_either_ext_wall) { // at either room exterior wall - can extend spaces up to the wall
				float row_right_edge(row.d[dim][1]); // opposite end of the row
				while ((row_left_edge  - space_width) > room.d[dim][0]) {row_left_edge  -= space_width; ++num_spaces_per_row;} // add rows to the left
				while ((row_right_edge + space_width) < room.d[dim][1]) {row_right_edge += space_width; ++num_spaces_per_row;} // add rows to the right
			}
			float const d_sign(d ? 1.0 : -1.0);
			cube_t space(row);
			space.d[!dim][!d] += d_sign*space_shrink; // shrink
			space.d[ dim][0]   = row_left_edge;
			bool last_was_space(0);

			for (unsigned s = 0; s < num_spaces_per_row; ++s) {
				space.d[dim][1] = space.d[dim][0] + space_width; // set width
				assert(space.is_strictly_normalized());
				
				if (has_bcube_int_xy(space, obstacles_ps)) { // skip entire space if it intersects stairs or an elevator
					if (last_was_space) {objs.back().flags &= ~RO_FLAG_ADJ_HI;} // no space to the right for the previous space
					last_was_space = 0;
				}
				else {
					unsigned flags(RO_FLAG_NOCOLL);
					if (last_was_space          ) {flags |= RO_FLAG_ADJ_LO;} // adjacent space to the left
					if (s+1 < num_spaces_per_row) {flags |= RO_FLAG_ADJ_HI;} // not the last space - assume there will be a space to the right
					bool const add_car(add_cars && rgen.rand_float() < 0.5); // 50% populated with cars

					// make it a handicap spot if near an elevator and there aren't already too many
					if (num_handicap_spots < max_handicap_spots) {
						cube_t hc_area(space);
						hc_area.expand_by(1.5*space_width);
						if (!no_sep_wall) {hc_area.intersect_with_cube_xy(row);} // keep within the current row if there are walls in between rows

						for (elevator_t const &e : interior->elevators) {
							if (e.z1() > space.z2()) continue; // doesn't extend down to this level
							if (e.intersects_xy(hc_area)) {flags |= RO_FLAG_IS_ACTIVE; ++num_handicap_spots; break;}
						}
					}
					room_object_t pspace(space, TYPE_PARK_SPACE, room_id, !dim, d, flags, tot_light_amt, SHAPE_CUBE, wall_color); // floor_color?

					if (add_car) { // add a collider to block this area from the player, people, and rats; add first so that objs.back() is correct for the next iter
						car_t car(car_from_parking_space(pspace));
						objs.emplace_back(car.bcube, TYPE_COLLIDER, room_id, !dim, d, (RO_FLAG_INVIS | RO_FLAG_FOR_CAR));
						pspace.obj_id = (uint16_t)(objs.size() + rgen.rand()); // will be used for the car model and color
						pspace.flags |= RO_FLAG_USED;
					}
					if (no_sep_wall && !at_either_ext_wall) { // add small yellow curbs to block cars
						float const curb_height(0.04*window_vspacing), curb_width(1.5*curb_height);
						cube_t curb(space);
						curb.z2() += curb_height; // set height
						curb.d[!dim][!d] += d_sign*(space.get_sz_dim(!dim) - curb_width);   // shrink to the correct width
						curb.translate_dim(!dim, -d_sign*(0.5*pillar_hwidth + curb_width)); // move inward to avoid pillars and walls
						curb.expand_in_dim(dim, -0.2*space_width);
						objs.emplace_back(curb, TYPE_CURB, room_id, dim, 0, 0, 1.0, SHAPE_CUBE, colorRGBA(1.0, 0.8, 0.3)); // dir=0
					}
					objs.push_back(pspace);
					last_was_space = 1;
				}
				space.d[dim][0] = space.d[dim][1]; // shift to next space
			} // for s
		} // for d
	} // for n
	if (is_top_floor) {
		float const pipe_light_amt = 1.0; // make pipes and electrical brighter and easier to see
		// avoid intersecting lights, pillars, walls, stairs, elevators, and ramps;
		// note that lights haven't been added yet though, but they're placed on beams, so we can avoid beams instead
		vect_cube_t walls, beams;

		for (auto i = objs.begin()+objs_start; i != objs.end(); ++i) {
			if (i->type == TYPE_PG_WALL) {
				if (i->item_flags == 2) {beams    .push_back(*i);} // beams
				else                    {walls    .push_back(*i);} // walls and pillars
				if (i->item_flags == 1) {obstacles.push_back(*i);} // pillars also count as obstacles
			}
			else if (i->type == TYPE_RAMP) {obstacles.push_back(*i);} // ramps are obstacles for pipes
		}
		add_basement_electrical(obstacles, walls, beams, room_id, pipe_light_amt, rgen);
		// get pipe ends (risers) coming in through the ceiling
		vect_riser_pos_t sewer, cold_water, hot_water;
		get_pipe_basement_water_connections(sewer, cold_water, hot_water, rgen);
		vect_cube_t pipe_cubes;
		// hang sewer pipes under the ceiling beams; hang water pipes from the ceiling, above sewer pipes and through the beams
		float const ceil_zval(beam.z1()), water_ceil_zval(beam.z2());
		add_basement_pipes(obstacles, walls, beams, sewer,      pipe_cubes, room_id, num_floors, pipe_light_amt, ceil_zval,      rgen, PIPE_TYPE_SEWER); // sewer
		add_to_and_clear(pipe_cubes, obstacles); // add sewer pipes to obstacles
		add_basement_pipes(obstacles, walls, beams, cold_water, pipe_cubes, room_id, num_floors, pipe_light_amt, water_ceil_zval, rgen, PIPE_TYPE_CW  ); // cold water
		add_to_and_clear(pipe_cubes, obstacles); // add cold water pipes to obstacles
		add_basement_pipes(obstacles, walls, beams, hot_water,  pipe_cubes, room_id, num_floors, pipe_light_amt, water_ceil_zval, rgen, PIPE_TYPE_HW  ); // hot water
		add_to_and_clear(pipe_cubes, obstacles); // add hot water pipes to obstacles
		add_sprinkler_pipe(obstacles, walls, beams, pipe_cubes, room_id, num_floors, pipe_light_amt, rgen);
	}
}

// find the closest wall (including room wall) to this location, avoiding obstacles, and shift outward by radius; routes in X or Y only, for now
point get_closest_wall_pos(point const &pos, float radius, cube_t const &room, vect_cube_t const &walls, vect_cube_t const &obstacles, bool vertical) {
	if (!room.contains_pt_xy_exp(pos, radius)) {return pos;} // error?
	// what about checking pos intersecting walls or obstacles? is that up to the caller to handle?
	vector3d const expand(radius, radius, radius);
	point best(pos);
	float dmin(room.dx() + room.dy()); // use an initial distance larger than what we can return

	for (unsigned dim = 0; dim < 2; ++dim) { // check room exterior walls first
		for (unsigned dir = 0; dir < 2; ++dir) {
			float const val(room.d[dim][dir] + (dir ? -radius : radius)), dist(fabs(val - pos[dim])); // shift val inward
			if (dist >= dmin) continue;
			point cand(pos);
			cand[dim] = val;
			// check walls as well, even though any wall hit should be replaced with a closer point below
			if (line_int_cubes_exp(pos, cand, obstacles, expand) || line_int_cubes_exp(pos, cand, walls, expand)) continue;
			if (vertical && line_int_cubes_exp(cand, point(cand.x, cand.y, room.z1()), obstacles, expand)) continue; // check for vertical obstacles
			best = cand; dmin = dist; // success
		} // for dir
	} // for dim
	for (cube_t const &wall : walls) { // check all interior walls
		for (unsigned dim = 0; dim < 2; ++dim) {
			if (pos[!dim] < wall.d[!dim][0]+radius || pos[!dim] > wall.d[!dim][1]-radius) continue; // doesn't project in this dim
			bool const dir(wall.get_center_dim(dim) < pos[dim]);
			float const val(wall.d[dim][dir] - (dir ? -radius : radius)), dist(fabs(val - pos[dim])); // shift val outward
			if (dist >= dmin) continue;
			point cand(pos);
			cand[dim] = val;
			if (line_int_cubes_exp(pos, cand, obstacles, expand)) continue; // check obstacles only
			if (vertical && line_int_cubes_exp(cand, point(cand.x, cand.y, room.z1()), obstacles, expand)) continue; // check for vertical obstacles
			best = cand; dmin = dist; // success
		} // for dim
	}
	return best;
}

float get_merged_pipe_radius(float r1, float r2, float exponent) {return pow((pow(r1, exponent) + pow(r2, exponent)), 1/exponent);}

float get_merged_risers_radius(vect_riser_pos_t const &risers, int exclude_flow_dir=2) {
	float radius(0.0);
	
	for (riser_pos_t const &p : risers) {
		if ((int)p.flow_dir == exclude_flow_dir) continue;
		radius = get_merged_pipe_radius(radius, + p.radius, 4.0); // higher exponent to avoid pipes that are too large
	}
	return radius;
}

enum {PIPE_RISER=0, PIPE_CONN, PIPE_MAIN, PIPE_MEC, PIPE_EXIT, PIPE_FITTING};

void expand_cube_except_in_dim(cube_t &c, float expand, unsigned not_dim) {
	c.expand_by(expand);
	c.expand_in_dim(not_dim, -expand); // oops, we shouldn't have expanded in this dim
}

struct pipe_t {
	point p1, p2;
	float radius;
	unsigned dim, type, end_flags; // end_flags: 1 bit is low end, 2 bit is high end
	bool connected, outside_pg;

	pipe_t(point const &p1_, point const &p2_, float radius_, unsigned dim_, unsigned type_, unsigned end_flags_) :
		p1(p1_), p2(p2_), radius(radius_), dim(dim_), type(type_), end_flags(end_flags_), connected(type != PIPE_RISER), outside_pg(0) {}
	float get_length() const {return fabs(p2[dim] - p1[dim]);}

	cube_t get_bcube() const {
		cube_t bcube(p1, p2);
		expand_cube_except_in_dim(bcube, radius, dim);
		return bcube;
	}
};

void add_insul_exclude(pipe_t const &pipe, cube_t const &fitting, vect_cube_t &insul_exclude) {
	cube_t ie(fitting);
	ie.expand_by(2.0*pipe.radius); // expand in all dims; needed for right angle and T junctions because they're in multiple dims
	ie.expand_in_dim(pipe.dim, 1.0*pipe.radius); // expand a bit more in pipe dim
	insul_exclude.push_back(ie);
}

bool building_t::add_basement_pipes(vect_cube_t const &obstacles, vect_cube_t const &walls, vect_cube_t const &beams, vect_riser_pos_t const &risers,
	vect_cube_t &pipe_cubes, unsigned room_id, unsigned num_floors, float tot_light_amt, float ceil_zval, rand_gen_t &rgen, unsigned pipe_type, bool allow_place_fail)
{
	assert(pipe_type < NUM_PIPE_TYPES);
	if (risers.empty()) return 0; // can this happen?
	float const FITTING_LEN(1.2), FITTING_RADIUS(1.1); // relative to radius
	bool const is_hot_water(pipe_type == PIPE_TYPE_HW), is_water(is_hot_water || pipe_type == PIPE_TYPE_CW), is_closed_loop(is_hot_water), add_insul(is_hot_water);
	vect_room_object_t &objs(interior->room_geom->objs);
	cube_t const &basement(get_basement());
	float const r_main(get_merged_risers_radius(risers, (is_closed_loop ? 0 : 2))); // exclude incoming water from hot water heaters for hot water pipes
	float const insul_thickness(0.4), min_insum_len(4.0); // both relative to pipe radius
	float const window_vspacing(get_window_vspace()), fc_thickness(get_fc_thickness()), wall_thickness(get_wall_thickness());
	float const pipe_zval(ceil_zval   - FITTING_RADIUS*r_main); // includes clearance for fittings vs. beams (and lights - mostly)
	float const pipe_min_z1(pipe_zval - FITTING_RADIUS*r_main);
	float const align_dist(2.0*wall_thickness); // align pipes within this range (in particular sinks and stall toilets)
	float const r_main_spacing((add_insul ? 1.0+insul_thickness : 1.0)*r_main); // include insulation thickness for hot water pipes
	assert(pipe_zval > bcube.z1());
	vector<pipe_t> pipes, fittings;
	cube_t pipe_end_bcube;
	unsigned num_valid(0), num_connected(0);
	// build random shifts table; make consistent per pipe to preserve X/Y alignments
	unsigned const NUM_SHIFTS = 21; // {0,0} + 20 random shifts
	vector3d rshifts[NUM_SHIFTS] = {};
	for (unsigned n = 1; n < NUM_SHIFTS; ++n) {rshifts[n][rgen.rand_bool()] = 0.25*window_vspacing*rgen.signed_rand_float();} // random shift in a random dir
	// determine if we need to add an entrance/exit pipe
	bool add_exit_pipe(1);

	if (is_closed_loop) { // hot water pipes; sewer pipes always have an exit and cold water always has an entrance
		for (riser_pos_t const &p : risers) {
			if (p.flow_dir == 0) {add_exit_pipe = 0; break;} // hot water flowing out of a water heater
		}
	}

	// seed the pipe graph with valid vertical segments and build a graph of X/Y values
	for (riser_pos_t const &p : risers) {
		assert(p.radius > 0.0);
		assert(p.pos.z > pipe_zval);
		point pos(p.pos);
		bool valid(0);

		for (unsigned n = 0; n < NUM_SHIFTS; ++n) { // try zero + random shifts
			cube_t c(pos);
			c.expand_by_xy(p.radius);
			//c.z1() = bcube.z1(); // extend all the way down to the floor of the lowest basement
			c.z1() = pipe_min_z1; // extend down to the lowest pipe z1

			// can't place outside building bcube, or over stairs/elevators/ramps/pillars/walls/beams;
			// here beams are included because lights are attached to the underside of them, so avoiding beams should hopefully also avoid lights
			if (!bcube.contains_cube_xy(c) || has_bcube_int(c, obstacles) || has_bcube_int(c, walls) || has_bcube_int(c, beams)) {
				pos = p.pos + rshifts[n]; // apply shift
				continue;
			}
			valid = 1;
			break;
		} // for n
		if (!valid) continue; // no valid shift, skip this connection
		pipes.emplace_back(point(pos.x, pos.y, pipe_zval), pos, p.radius, 2, PIPE_RISER, 0); // neither end capped
		pipe_end_bcube.assign_or_union_with_cube(pipes.back().get_bcube());
		++num_valid;
	} // for pipe_ends
	if (pipes.empty()) return 0; // no valid pipes

	// calculate unique positions of pipes along the main pipe
	bool const dim(pipe_end_bcube.dx() < pipe_end_bcube.dy()); // main sewer line dim
	map<float, vector<unsigned>> xy_map;

	for (auto p = pipes.begin(); p != pipes.end(); ++p) {
		unsigned const pipe_ix(p - pipes.begin());
		float &v(p->p1[dim]);
		auto it(xy_map.find(v));
		if (it != xy_map.end()) {it->second.push_back(pipe_ix); continue;} // found
		bool found(0);
		// try to find an existing map value that's within align_dist of this value; messy and inefficient, but I'm not sure how else to do this
		for (auto &i : xy_map) {
			if (fabs(i.first - v) > align_dist) continue; // too far
			i.second.push_back(pipe_ix);
			v = p->p2[dim] = i.first;
			found = 1;
			break;
		}
		if (!found) {xy_map[v].push_back(pipe_ix);}
	} // for pipes

	// create main pipe that runs in the longer dim (based on drain pipe XY bounds)
	pipe_end_bcube.expand_in_dim(dim, r_main);
	// use the center of the pipes bcube to minimize run length, but clamp to the interior of the basement;
	// in the case where all risers are outside of the basement perimeter, this will make pipes run against the exterior basement wall (and possibly intersect electical conduits)
	float const pipes_bcube_center(max(basement.d[!dim][0]+r_main, min(basement.d[!dim][1]-r_main, pipe_end_bcube.get_center_dim(!dim))));
	float centerline(pipes_bcube_center), exit_dmin(0.0);
	point mp[2]; // {lo, hi} ends
	bool exit_dir(0);
	point exit_pos;

	for (unsigned d = 0; d < 2; ++d) { // dim
		mp[d][ dim] = pipe_end_bcube.d[dim][d];
		mp[d][!dim] = centerline;
		mp[d].z     = pipe_zval;
	}
	// shift pipe until it clears all obstacles
	float const step_dist(2.0*r_main), step_area(bcube.get_sz_dim(!dim)); // step by pipe radius
	unsigned const max_steps(step_area/step_dist);
	bool success(0);

	for (unsigned n = 0; n < max_steps; ++n) {
		cube_t const c(pipe_t(mp[0], mp[1], r_main_spacing, dim, PIPE_MAIN, 3).get_bcube());
		if (!bcube   .contains_cube_xy(c)) break; // outside valid area
		if (!basement.contains_cube_xy(c)) continue; // outside the basement
		
		if (!has_bcube_int(c, obstacles)) {
			success = 1;

			// check for overlap with beam running parallel to the main pipe, and reject it; mostly there to avoid blocking lights that may be on the beam
			for (cube_t const &beam : beams) {
				if (beam.get_sz_dim(dim) < beam.get_sz_dim(!dim)) continue; // beam not parallel to pipe, ignore
				if (c.intersects_xy(beam)) {success = 0; break;}
			}
			if (success) break; // success/done
		}
		float const xlate(((n>>1)+1)*((n&1) ? -1.0 : 1.0)*step_dist);
		UNROLL_2X(mp[i_][!dim] += xlate;)
	} // for n
	if (success) {
		centerline = mp[0][!dim]; // update centerline based on translate
	}
	else if (allow_place_fail) { // fail case
		// try stripping off some risers from the end and connecting the remaining ones
		vect_riser_pos_t risers_sub;
		float riser_min(FLT_MAX), riser_max(-FLT_MAX);

		for (riser_pos_t const &r : risers) {
			min_eq(riser_min, r.pos[dim]);
			max_eq(riser_max, r.pos[dim]);
		}
		for (unsigned dir = 0; dir < 2; ++dir) {
			risers_sub.clear();
			// try to remove risers at each end recursively until successful
			for (riser_pos_t const &r : risers) {
				if (r.pos[dim] != (dir ? riser_max : riser_min)) {risers_sub.push_back(r);}
			}
			if (risers_sub.empty()) continue;
			assert(risers_sub.size() < risers.size());
			if (add_basement_pipes(obstacles, walls, beams, risers_sub, pipe_cubes, room_id, num_floors, tot_light_amt, ceil_zval, rgen, pipe_type, 1)) return 1;
		} // for dir
		return 0;
	}
	else {
		UNROLL_2X(mp[i_][!dim] = centerline;) // else use the centerline, even though it's invalid; rare, and I don't have a non-house example where it looks wrong
	}
	mp[0][dim] = bcube.d[dim][1]; mp[1][dim] = bcube.d[dim][0]; // make dim range denormalized; will recalculate below with correct range
	bool const d(!dim);
	float const conn_pipe_merge_exp = 3.0; // cubic

	// connect drains to main pipe in !dim
	for (auto const &v : xy_map) { // for each unique position along the main pipe
		float radius(0.0), range_min(centerline), range_max(centerline), unconn_radius(0.0);
		point const &ref_p1(pipes[v.second.front()].p1);
		unsigned num_keep(0);

		for (unsigned ix : v.second) {
			assert(ix < pipes.size());
			pipe_t &pipe(pipes[ix]);
			float const val(pipe.p1[d]);

			if (fabs(val - centerline) < r_main) {pipe.p1[d] = pipe.p2[d] = centerline;} // shift to connect directly to main pipe since it's close enough
			else {
				float lo(val - pipe.radius), hi(val + pipe.radius);
				point p1(ref_p1), p2(p1);
				if      (lo < range_min) {p1[d] = lo; p2[d] = range_min;} // on the lo side
				else if (hi > range_max) {p1[d] = range_max; p2[d] = hi;} // on the hi side
				bool skip(has_bcube_int(pipe_t(p1, p2, radius, d, PIPE_CONN, 3).get_bcube(), obstacles));

				if (skip && (p2[d] - p1[d]) > 8.0*pipe.radius) { // blocked, can't connect, long segment: try extending halfway
					if      (lo < range_min) {p1[d] = lo = 0.5*(lo + range_min); pipe.p1[d] = pipe.p2[d] = lo + pipe.radius;} // on the lo side
					else if (hi > range_max) {p2[d] = hi = 0.5*(hi + range_max); pipe.p1[d] = pipe.p2[d] = hi - pipe.radius;} // on the hi side
					skip = has_bcube_int(pipe_t(p1, p2, radius, d, PIPE_CONN, 3).get_bcube(), obstacles);
				}
				if (skip) {
					unconn_radius = get_merged_pipe_radius(unconn_radius, pipe.radius, conn_pipe_merge_exp); // add this capacity for use in another riser
					continue;
				}
				min_eq(range_min, lo); // update ranges
				max_eq(range_max, hi);
			}
			pipe.connected = 1;
			if (unconn_radius > 0.0) {pipe.radius = get_merged_pipe_radius(pipe.radius, unconn_radius, conn_pipe_merge_exp); unconn_radius = 0.0;} // add extra capacity
			radius = get_merged_pipe_radius(radius, pipe.radius, conn_pipe_merge_exp);
			++num_keep;
		} // for ix
		if (num_keep == 0) continue; // no valid connections for this row

		// we can skip adding a connector if short and under the main pipe
		if (range_max - range_min > r_main) {
			point p1(ref_p1), p2(p1); // copy dims !d and z from a representative pipe
			p1[d] = range_min; p2[d] = range_max;
			pipes.emplace_back(p1, p2, radius, d, PIPE_CONN, 3); // cap both ends

			for (unsigned ix : v.second) { // add fittings
				if (!pipes[ix].connected) continue; // pipe was not connected, don't add the fitting
				float const val(pipes[ix].p1[d]), fitting_len(FITTING_LEN*radius);
				p1[d] = val - fitting_len; p2[d] = val + fitting_len;
				fittings.emplace_back(p1, p2, FITTING_RADIUS*radius, d, PIPE_FITTING, 3);
			}
		} // end connector
		// add fitting to the main pipe
		point p1(mp[0]), p2(p1);
		float const fitting_len(FITTING_LEN*r_main);
		p1[!d] = v.first - fitting_len; p2[!d] = v.first + fitting_len;
		fittings.emplace_back(p1, p2, FITTING_RADIUS*r_main, !d, PIPE_FITTING, 3);
		// update main pipe endpoints to include this connector pipe range
		min_eq(mp[0][dim], v.first-radius);
		max_eq(mp[1][dim], v.first+radius);
		num_connected += num_keep;
	} // for v
	if (mp[0][dim] >= mp[1][dim]) return 0; // no pipes connected to main? I guess there's nothing to do here
	unsigned main_pipe_end_flags(0); // start with both ends unconnected
	bool has_exit(0);

	if (add_exit_pipe && (num_floors > 1 || rgen.rand_bool())) { // exit into the wall of the building
		// Note: if roads are added for secondary buildings, we should have the exit on the side of the building closest to the road
		bool const first_dir((basement.d[dim][1] - mp[1][dim]) < (mp[0][dim] - basement.d[dim][0])); // closer basement exterior wall

		for (unsigned d = 0; d < 2; ++d) { // dir
			bool const dir(bool(d) ^ first_dir);
			point ext[2] = {mp[dir], mp[dir]};
			ext[dir][dim] = basement.d[dim][dir]; // shift this end to the basement wall
			if (has_bcube_int(pipe_t(ext[0], ext[1], r_main_spacing, dim, PIPE_MAIN, 0).get_bcube(), obstacles)) continue; // can't extend to ext wall in this dim
			mp[dir]  = ext[dir];
			has_exit = 1;
			main_pipe_end_flags = (dir ? 2 : 1); // connect the end going to the exit
			break; // success
		} // for d
		if (!has_exit) { // no straight segment? how about a right angle?
			bool first_side(0);
			if (centerline == pipes_bcube_center) {first_side = rgen.rand_bool();} // centered, choose a random side
			else {first_side = ((basement.d[!dim][1] - mp[0][!dim]) < (mp[0][!dim] - basement.d[!dim][0]));} // off-center, choose closer basement exterior wall

			for (unsigned d = 0; d < 2 && !has_exit; ++d) { // dir
				for (unsigned e = 0; e < 2; ++e) { // side
					bool const dir(bool(d) ^ first_dir), side(bool(e) ^ first_side);
					point ext[2] = {mp[dir], mp[dir]};
					ext[side][!dim] = basement.d[!dim][side]; // shift this end to the basement wall
					pipe_t const exit_pipe(ext[0], ext[1], r_main, !dim, PIPE_MEC, (side ? 1 : 2)); // add a bend in the side connecting to the main pipe
					if (has_bcube_int(exit_pipe.get_bcube(), obstacles)) continue; // can't extend to the ext wall in this dim
					pipes.push_back(exit_pipe);
					has_exit = 1;
					main_pipe_end_flags = (dir ? 2 : 1); // connect the end going to the exit connector pipe
					break; // success
				} // for e
			} // for d
		}
	}
	if (add_exit_pipe && !has_exit) { // create exit segment and vertical pipe into the floor
		for (unsigned d = 0; d < 2; ++d) { // dim
			point const cand_exit_pos(get_closest_wall_pos(mp[d], r_main_spacing, basement, walls, obstacles, 1)); // vertical=1
			float const dist(p2p_dist(mp[d], cand_exit_pos));
			if (exit_dmin == 0.0 || dist < exit_dmin) {exit_pos = cand_exit_pos; exit_dir = d; exit_dmin = dist;}
		}
		point &exit_conn(mp[exit_dir]);
		unsigned exit_pipe_end_flags(2); // bend at the top only

		if (exit_pos[!dim] == exit_conn[!dim]) { // exit point is along the main pipe
			if ((exit_conn[dim] < exit_pos[dim]) == exit_dir) { // extend main pipe to exit point
				exit_conn[dim] = exit_pos[dim];
				main_pipe_end_flags = (exit_dir ? 2 : 1); // connect the end going to the exit
			}
			else { // exit is in the middle of the pipe; add fitting to the main pipe
				point p1(exit_pos), p2(p1);
				float const fitting_len(FITTING_LEN*r_main);
				p1[dim] -= fitting_len; p2[dim] += fitting_len;
				fittings.emplace_back(p1, p2, FITTING_RADIUS*r_main, !d, PIPE_FITTING, 3);
				exit_pipe_end_flags = 0; // no bend needed
			}
		}
		else { // create a right angle bend
			// extend by 2*radius to avoid overlapping a connector pipe or it's fittings, but keep it inside the building
			if (exit_dir) {exit_conn[dim] = exit_pos[dim] = min(exit_conn[dim]+2.0f*r_main, bcube.d[dim][1]-r_main);}
			else          {exit_conn[dim] = exit_pos[dim] = max(exit_conn[dim]-2.0f*r_main, bcube.d[dim][0]+r_main);}
			pipes.emplace_back(exit_conn, exit_pos, r_main, !dim, PIPE_MEC, 3); // main exit connector, bends at both ends
			exit_pipe_end_flags = 0; // the above pipe will provide the bend, so it's not needed at the top of the exit pipe
			main_pipe_end_flags = (exit_dir ? 2 : 1); // connect the end going to the exit connector pipe
		}
		point exit_floor_pos(exit_pos);
		exit_floor_pos.z = basement.z1() + fc_thickness; // on the bottom level floor
		pipes.emplace_back(exit_floor_pos, exit_pos, r_main, 2, PIPE_EXIT, exit_pipe_end_flags);
	}
	// add main pipe
	pipe_t main_pipe(mp[0], mp[1], r_main, dim, PIPE_MAIN, main_pipe_end_flags);
	assert(main_pipe.get_bcube().is_strictly_normalized());
	pipes.push_back(main_pipe);

	// add pipe objects: sewer: dark gray pipes / gray-brown fittings; water: copper pipes / brass fittings; hot water: white insulation
	colorRGBA const pcolors[4] = {DK_GRAY, COPPER_C, COPPER_C, GRAY}; // sewer, cw, hw, gas
	colorRGBA const fcolors[4] = {colorRGBA(0.7, 0.6, 0.5, 1.0), BRASS_C, BRASS_C, DK_GRAY}; // sewer, cw, hw, gas
	colorRGBA const &pipes_color(pcolors[pipe_type]), &fittings_color(fcolors[pipe_type]);
	vect_cube_t insul_exclude;
	pipe_cubes.reserve(pipe_cubes.size() + pipes.size());

	for (pipe_t &p : pipes) {
		if (!p.connected) continue; // unconnected drain, skip
		cube_t const pbc(p.get_bcube());
		if (!basement.intersects_xy(pbc)) {p.outside_pg = 1; continue;} // outside the basement, don't need to draw
		pipe_cubes.push_back(pbc);
		pipe_cubes.back().expand_in_dim(p.dim, 0.5*p.radius); // add a bit of extra space for the end cap
		bool const pdim(p.dim & 1), pdir(p.dim >> 1); // encoded as: X:dim=0,dir=0 Y:dim=1,dir=0, Z:dim=x,dir=1
		unsigned flags(0);
		if (p.type != PIPE_EXIT) {flags |= RO_FLAG_NOCOLL;} // only exit pipe has collisions enabled
		if (p.type == PIPE_CONN || p.type == PIPE_MAIN) {flags |= RO_FLAG_HANGING;} // hanging connector/main pipe with flat ends
		room_object_t const pipe(pbc, TYPE_PIPE, room_id, pdim, pdir, flags, tot_light_amt, SHAPE_CYLIN, pipes_color);
		objs.push_back(pipe);
		if (p.type == PIPE_EXIT && p.dim == 2) {objs.back().flags |= RO_FLAG_LIT;} // vertical exit pipes are shadow casting; applies to pipe but not fittings

		// add pipe fittings around ends and joins; only fittings have flat and round ends because raw pipe ends should never be exposed;
		// note that we may not need fittings at T-junctions for hot water pipes, but then we would need to cap the ends
		if (p.type == PIPE_RISER) continue; // not for vertical drain pipes, since they're so short and mostly hidden above the connector pipes
		float const fitting_len(FITTING_LEN*p.radius), fitting_expand((FITTING_RADIUS - 1.0)*p.radius);

		for (unsigned d = 0; d < 2; ++d) {
			if ((p.type == PIPE_CONN || p.type == PIPE_MAIN) && !(p.end_flags & (1<<d))) continue; // already have fittings added from connecting pipes
			room_object_t pf(pipe);
			pf.flags |= RO_FLAG_NOCOLL;
			if (d == 1 || p.type != PIPE_MAIN) {pf.flags |= RO_FLAG_ADJ_LO;} // skip end cap for main pipe exiting through the wall
			if (d == 0 || p.type != PIPE_MAIN) {pf.flags |= RO_FLAG_ADJ_HI;} // skip end cap for main pipe exiting through the wall
			pf.color  = fittings_color;
			expand_cube_except_in_dim(pf, fitting_expand, p.dim); // expand slightly
			pf.d[p.dim][!d] = pf.d[p.dim][d] + (d ? -1.0 : 1.0)*fitting_len;
			if (!basement.intersects_xy(pf)) continue;
			objs.push_back(pf);
			if (add_insul) {add_insul_exclude(p, pf, insul_exclude);}

			if (p.type == PIPE_MEC || p.type == PIPE_EXIT) {
				if (p.end_flags & (1<<d)) { // connector or exit pipe with a round bend needs special handling
					objs.back().flags &= ~(d ? RO_FLAG_ADJ_LO : RO_FLAG_ADJ_HI); // unset end flag on the end that was cut to length, since that's not a bend
					// create a second fitting segment for the flat end; the sides will overlap with the previous fitting
					pf.flags &= ~(d ? RO_FLAG_ADJ_HI : RO_FLAG_ADJ_LO);
					pf.flags |= RO_FLAG_HANGING; // flat ends
					objs.push_back(pf);
					if (add_insul) {add_insul_exclude(p, pf, insul_exclude);}
				}
				else { // connector or exit pipe entering the wall or floor
					objs.back().flags |= RO_FLAG_HANGING; // flat ends
				}
			}
		} // for d
	} // for p
	for (pipe_t const &p : fittings) {
		cube_t const pbc(p.get_bcube());
		if (!basement.intersects_xy(pbc)) continue; // outside the basement, don't need to draw
		bool const pdim(p.dim & 1), pdir(p.dim >> 1);
		unsigned const flags(RO_FLAG_NOCOLL | RO_FLAG_HANGING | RO_FLAG_ADJ_LO | RO_FLAG_ADJ_HI); // non-colliding, flat ends on both sides
		objs.emplace_back(pbc, TYPE_PIPE, room_id, pdim, pdir, flags, tot_light_amt, SHAPE_CYLIN, fittings_color);
		if (add_insul) {add_insul_exclude(p, pbc, insul_exclude);}
	} // for p
	if (add_insul) { // add hot water pipe insulation
		colorRGBA const insul_color(WHITE);
		vect_cube_t insulation, temp;

		for (pipe_t const &p : pipes) {
			if (!p.connected || p.outside_pg || p.type == PIPE_RISER) continue; // unconnected, outside, or drain, skip
			float const min_len(min_insum_len*p.radius), radius_exp(insul_thickness*p.radius);
			if (p.get_length() < min_len) continue; // length is too short
			cube_t const pbc(p.get_bcube());
			insulation.clear();
			subtract_cubes_from_cube_split_in_dim(pbc, insul_exclude, insulation, temp, p.dim);
			unsigned const d1((p.dim+1)%3), d2((p.dim+2)%3);
			bool const pdim(p.dim & 1), pdir(p.dim >> 1); // encoded as: X:dim=0,dir=0 Y:dim=1,dir=0, Z:dim=x,dir=1
			unsigned flags(RO_FLAG_NOCOLL | RO_FLAG_HANGING | RO_FLAG_ADJ_HI | RO_FLAG_ADJ_LO); // not collidable, always flat ends
			if (p.type == PIPE_EXIT && p.dim == 2) {objs.back().flags |= RO_FLAG_LIT;} // vertical exit pipes are shadow casting
			
			for (cube_t &i : insulation) {
				if (i.d[d1][0] != pbc.d[d1][0] || i.d[d1][1] != pbc.d[d1][1] || i.d[d2][0] != pbc.d[d2][0] || i.d[d2][1] != pbc.d[d2][1]) continue; // clipped on any side
				if (i.get_sz_dim(p.dim) < min_len) continue; // too short
				i.expand_in_dim(d1, radius_exp);
				i.expand_in_dim(d2, radius_exp);
				objs.emplace_back(i, TYPE_PIPE, room_id, pdim, pdir, flags, tot_light_amt, SHAPE_CYLIN, insul_color);
			}
		} // for p
	}
	return 1;
}

void building_t::add_sprinkler_pipe(vect_cube_t const &obstacles, vect_cube_t const &walls, vect_cube_t const &beams, vect_cube_t const &pipe_cubes,
		unsigned room_id, unsigned num_floors, float tot_light_amt, rand_gen_t &rgen)
{
	// add red sprinkler system pipe
	float const floor_spacing(get_window_vspace()), fc_thickness(get_fc_thickness());
	float const sp_radius(1.2*get_wall_thickness()), spacing(2.0*sp_radius), flange_expand(0.3*sp_radius);
	float const bolt_dist(sp_radius + 0.5*flange_expand), bolt_radius(0.32*flange_expand), bolt_height(0.1*fc_thickness);
	vect_room_object_t &objs(interior->room_geom->objs);
	cube_t const &basement(get_basement());
	cube_t c;
	set_cube_zvals(c, (basement.z1() + fc_thickness), (basement.z2() - fc_thickness));

	for (unsigned n = 0; n < 100; ++n) { // 100 random tries
		bool const dim(rgen.rand_bool()), dir(rgen.rand_bool());
		set_wall_width(c, rgen.rand_uniform(basement.d[!dim][0]+spacing, basement.d[!dim][1]-spacing), sp_radius, !dim);
		c.d[dim][ dir] = basement.d[dim][dir] + (dir ? -1.0 : 1.0)*flange_expand; // against the wall (with space for the flange)
		c.d[dim][!dir] = c.d[dim][dir] + (dir ? -1.0 : 1.0)*2.0*sp_radius;
		if (has_bcube_int(c, obstacles) || has_bcube_int(c, walls) || has_bcube_int(c, beams) || has_bcube_int(c, pipe_cubes)) continue; // bad position
		objs.emplace_back(c, TYPE_PIPE, room_id, 0, 1, RO_FLAG_LIT, tot_light_amt, SHAPE_CYLIN, RED); // dir=1 for vertical; casts shadows; add to pipe_cubes?
		// add flanges at top and bottom of each floor
		cube_t flange(c);
		flange.expand_by_xy(flange_expand);
		point const center(c.get_cube_center());

		for (unsigned f = 0; f <= num_floors; ++f) { // flanges for each ceiling/floor
			unsigned flags(RO_FLAG_HANGING | (f > 0)*RO_FLAG_ADJ_LO | (f < num_floors)*RO_FLAG_ADJ_HI);
			float const z(basement.z1() + f*floor_spacing);
			flange.z1() = z - ((f ==          0) ? -1.0 : 1.15)*fc_thickness;
			flange.z2() = z + ((f == num_floors) ? -1.0 : 1.15)*fc_thickness;
			objs.emplace_back(flange, TYPE_PIPE, room_id, 0, 1, flags, tot_light_amt, SHAPE_CYLIN, RED);
			unsigned const NUM_BOLTS = 8;
			float const angle_step(TWO_PI/NUM_BOLTS);

			for (unsigned m = 0; m < NUM_BOLTS; ++m) { // add bolts
				float const angle(m*angle_step), dx(bolt_dist*sin(angle)), dy(bolt_dist*cos(angle));
				cube_t bolt;
				bolt.set_from_sphere(point(center.x+dx, center.y+dy, 0.0), bolt_radius);
				set_cube_zvals(bolt, flange.z1()-bolt_height, flange.z2()+bolt_height);
				objs.emplace_back(bolt, TYPE_PIPE, room_id, 0, 1, (flags | RO_FLAG_RAND_ROT), tot_light_amt, SHAPE_CYLIN, RED);
			} // for m
		} // for f
		break; // done
	} // for n
	//cout << TXT(pipe_ends.size()) << TXT(num_valid) << TXT(num_connected) << TXT(pipes.size()) << TXT(xy_map.size()) << endl;
}

// here each sphere represents the entry point of a pipe with this radius into the basement ceiling
// find all plumbing fixtures such as toilets, urinals, sinks, and showers; these should have all been placed in rooms by now
void building_t::get_pipe_basement_water_connections(vect_riser_pos_t &sewer, vect_riser_pos_t &cold_water, vect_riser_pos_t &hot_water, rand_gen_t &rgen) const {
	cube_t const &basement(get_basement());
	float const merge_dist = 4.0; // merge two pipes if their combined radius is within this distance
	float const floor_spacing(get_window_vspace()), base_pipe_radius(0.01*floor_spacing), base_pipe_area(base_pipe_radius*base_pipe_radius);
	float const merge_dist_sq(merge_dist*merge_dist), max_radius(0.4*get_wall_thickness()), ceil_zval(basement.z2() - get_fc_thickness());
	vector<room_object_t> water_heaters;
	bool const inc_basement_wheaters = 1;

	// start with sewer pipes and water heaters
	for (room_object_t const &i : interior->room_geom->objs) { // check all objects placed so far
		if (i.type == TYPE_WHEATER) { // water heaters are special because they take cold water and return hot water
			// maybe skip if in the basement, since this must connect directly to pipes rather than through a riser;
			// this can happen for houses (which don't have parking garages or pipes), but currently not for office buildings
			if (!inc_basement_wheaters && i.z1() < ground_floor_z1) continue;
			water_heaters.push_back(i);
			continue;
		}
		if (i.z1() < ground_floor_z1) continue; // object in the house basement; unclear how to handle it here
		bool const hot_cold_obj (i.type == TYPE_SINK || i.type == TYPE_TUB || i.type == TYPE_SHOWER || i.type == TYPE_BRSINK || i.type == TYPE_KSINK || i.type == TYPE_WASHER);
		bool const cold_only_obj(i.type == TYPE_TOILET || i.type == TYPE_URINAL || i.type == TYPE_DRAIN);
		if (!hot_cold_obj && !cold_only_obj) continue;
		point pos(i.xc(), i.yc(), ceil_zval);
		//if (!basement.contains_pt_xy(pos)) continue; // riser/pipe doesn't pass through the basement, but this is now allowed
		bool merged(0);

		// see if we can merge this sewer riser into an existing nearby riser
		for (auto &r : sewer) {
			float const p_area(r.radius*r.radius), sum_area(p_area + base_pipe_area);
			if (!dist_xy_less_than(r.pos, pos, merge_dist_sq*sum_area)) continue;
			r.pos      = (p_area*r.pos + base_pipe_area*pos)/sum_area; // merged position is weighted average area
			r.radius   = get_merged_pipe_radius(r.radius, base_pipe_radius, 3.0); // cubic
			r.has_hot |= hot_cold_obj;
			merged     = 1;
			break;
		} // for p
		if (!merged) {sewer.emplace_back(pos, base_pipe_radius, hot_cold_obj, 0);} // create a new riser; flow_dir=0/out
	} // for i
	for (riser_pos_t &s : sewer) {min_eq(s.radius, max_radius);} // clamp radius to a reasonable value after all merges

	// generate hot and cold water pipes
	// choose a shift direction 45 degrees diagonally in the XY plane to avoid collisions with sewer pipes placed in either dim
	vector3d const shift_dir(vector3d((rgen.rand_bool() ? -1.0 : 1.0), (rgen.rand_bool() ? -1.0 : 1.0), 0.0).get_norm());
	cold_water.reserve(sewer.size()); // one cold water pipe per sewer pipe

	for (riser_pos_t &s : sewer) {
		float const wp_radius(0.5*s.radius), pipe_spacing(2.0*(s.radius + wp_radius));
		cube_t place_area(basement.contains_pt_xy(s.pos) ? basement : bcube); // force the water riser to be in the basement if the sewer riser is there
		place_area.expand_by_xy(-wp_radius);
		vector3d delta(shift_dir*pipe_spacing);
		if (!place_area.contains_pt_xy(s.pos + delta)) {delta.negate();} // if shift takes pos outside placement area, shift in the other direction
		cold_water.emplace_back((s.pos + delta), wp_radius, 0, 1); // has_hot=0, flow_dir=1/in
		if (!s.has_hot) continue; // no hot water, done
		float const hot_radius(0.75*wp_radius); // smaller radius than cold water pipe
		place_area.expand_by_xy(wp_radius - hot_radius); // resize for new radius
		if (!place_area.contains_pt_xy(s.pos - delta)) {delta *= 2.0;} // if shift takes pos outside placement area, shift further from the drain
		hot_water.emplace_back((s.pos - delta), hot_radius, 1, 1); // shift in opposite dir; has_hot=1, flow_dir=1/in
	} // for sewer
	if (!water_heaters.empty() && !hot_water.empty()) { // add connections for water heaters if there are hot water pipes
		float const radius_hot(get_merged_risers_radius(hot_water)); // this is the radius of the main hot water supply
		float const per_wh_radius(radius_hot*pow(1.0/water_heaters.size(), 1/4.0)); // distribute evenly among the water heaters using the same merge exponent

		for (room_object_t const &wh : water_heaters) {
			if (wh.z1() < ground_floor_z1) { // house basement water heater
				// should this connect directly? what if vent or other pipe is in the way? how to match pipe radius?
			}
			float const shift_val(0.75*wh.get_radius());
			point const center(wh.xc(), wh.yc(), ceil_zval);
			cold_water.emplace_back((center + shift_val*shift_dir), per_wh_radius, 0, 1); // has_hot=0, flow_dir=1/in
			hot_water .emplace_back((center - shift_val*shift_dir), per_wh_radius, 1, 0); // has_hot=1, flow_dir=0/out
		} // for wh
	}
}

void building_t::get_pipe_basement_gas_connections(vect_riser_pos_t &pipes) const {
	float const pipe_radius(0.02*get_window_vspace()), ceil_zval(get_basement().z2() - get_fc_thickness());

	for (room_object_t const &i : interior->room_geom->objs) { // check all objects placed so far
		if (i.z1() < ground_floor_z1) continue; // object in the house basement; unclear how to handle it here
		if (i.type != TYPE_WHEATER && i.type != TYPE_FURNACE && i.type != TYPE_STOVE && i.type != TYPE_DRYER && i.type != TYPE_FPLACE) continue;
		pipes.emplace_back(point(i.xc(), i.yc(), ceil_zval), pipe_radius, 0, 1); // flows in
	}
}

void building_t::add_basement_electrical(vect_cube_t &obstacles, vect_cube_t const &walls, vect_cube_t const &beams, int room_id, float tot_light_amt, rand_gen_t &rgen) {
	cube_t const &basement(get_basement());
	float const floor_spacing(get_window_vspace()), fc_thickness(get_fc_thickness()), floor_height(floor_spacing - 2.0*fc_thickness), ceil_zval(basement.z2() - fc_thickness);
	unsigned const num_panels(is_house ? 1 : (1 + (rgen.rand()&3))); // 1 for houses, 1-3 for office buildings
	colorRGBA const color(0.5, 0.6, 0.7);
	auto &objs(interior->room_geom->objs);

	for (unsigned n = 0; n < num_panels; ++n) {
		float const bp_hwidth(rgen.rand_uniform(0.15, 0.25)*(is_house ? 0.7 : 1.0)*floor_height), bp_depth(rgen.rand_uniform(0.05, 0.07)*(is_house ? 0.5 : 1.0)*floor_height);
		if (bp_hwidth > 0.25*min(basement.dx(), basement.dy())) continue; // basement too small
		cube_t c;
		set_cube_zvals(c, (ceil_zval - 0.75*floor_height), (ceil_zval - rgen.rand_uniform(0.2, 0.35)*floor_height));

		for (unsigned t = 0; t < ((n == 0) ? 100U : 1U); ++t) { // 100 tries for the first panel, one try after that
			bool const dim(rgen.rand_bool()), dir(rgen.rand_bool());
			set_wall_width(c, rgen.rand_uniform(basement.d[!dim][0]+bp_hwidth, basement.d[!dim][1]-bp_hwidth), bp_hwidth, !dim);
			c.d[dim][ dir] = basement.d[dim][dir];
			c.d[dim][!dir] = c.d[dim][dir] + (dir ? -1.0 : 1.0)*2.0*bp_depth;
			assert(c.is_strictly_normalized());
			cube_t test_cube(c);
			test_cube.d[dim][!dir] += (dir ? -1.0 : 1.0)*2.0*bp_hwidth; // add a width worth of clearance in the front so that the door can be opened
			if (has_bcube_int(test_cube, obstacles) || has_bcube_int(test_cube, walls)) continue; // bad breaker box position
			point top_center(c.xc(), c.yc(), c.z2());
			cube_t conduit(top_center);
			conduit.z2() = ceil_zval;
			conduit.expand_by_xy(rgen.rand_uniform(0.38, 0.46)*bp_depth);
			if (has_bcube_int(conduit, beams)) continue; // bad conduit position
			unsigned cur_room_id((room_id < 0) ? get_room_containing_pt(c.get_cube_center()) : (unsigned)room_id); // calculate room_id if needed
			objs.emplace_back(c, TYPE_BRK_PANEL, cur_room_id, dim, dir, RO_FLAG_INTERIOR, tot_light_amt, SHAPE_CUBE, color);
			objs.emplace_back(conduit, TYPE_PIPE, cur_room_id, 0, 1, (RO_FLAG_NOCOLL | RO_FLAG_INTERIOR), tot_light_amt, SHAPE_CYLIN, LT_GRAY); // vertical pipe
			set_obj_id(objs);
			set_cube_zvals(c, ceil_zval-floor_height, ceil_zval); // expand to floor-to-ceiling
			obstacles.push_back(c); // block off from pipes
			break; // done
		} // for t
	} // for n
}

void building_t::add_basement_electrical_house(rand_gen_t &rgen) {
	assert(has_room_geom());
	float const tot_light_amt = 1.0; // ???
	float const obj_expand(0.5*get_wall_thickness());
	vect_cube_t obstacles, walls;

	for (unsigned d = 0; d < 2; ++d) { // add basement walls
		for (cube_t const &wall : interior->walls[d]) {
			if (wall.zc() < ground_floor_z1) {walls.push_back(wall);}
		}
	}
	// add basement objects; include them all, since it's not perf critical; we haven't added objects such as trim yet;
	// what about blocking the breaker box with something, is that possible?
	for (room_object_t const &c : interior->room_geom->objs) {
		if (c.z1() < ground_floor_z1) {obstacles.push_back(c); obstacles.back().expand_by(obj_expand);} // with some clearance
	}
	vector_add_to(interior->stairwells, obstacles); // add stairs; what about open doors?
	vector_add_to(interior->elevators,  obstacles); // there probably are none, but it doesn't hurt to add them
	add_basement_electrical(obstacles, walls, vect_cube_t(), -1, tot_light_amt, rgen); // no beams, room_id=-1 (to be calculated)
}

void building_t::add_house_basement_pipes(rand_gen_t &rgen) {
	if (!has_room_geom()) return; // error?
	float const fc_thick(get_fc_thickness());
	cube_t const &basement(get_basement());
	// hang sewer pipes under the ceiling beams; hang water pipes from the ceiling, above sewer pipes and through the beams
	unsigned const num_floors(1); // basement is always a single floor
	float const pipe_light_amt = 1.0; // make pipes brighter and easier to see
	// houses have smaller radius pipes, so we should have enough space to stack sewer below hot water below cold water
	float const sewer_zval(basement.z2() - 1.8*fc_thick), cw_zval(basement.z2() - 1.0*fc_thick), hw_zval(basement.z2() - 1.4*fc_thick);
	float const trim_thickness(get_trim_thickness()), wall_thickness(get_wall_thickness());
	vect_cube_t pipe_cubes, obstacles, walls, beams; // beams remains empty
	unsigned room_id(0);

	// we can't pass in a single valid room_id because house basements/pipes span multiple rooms, but we can at least use the ID of a room in the basement
	for (auto r = interior->rooms.begin(); r != interior->rooms.end(); ++r) {
		if (basement.contains_cube(*r)) {room_id = (r - interior->rooms.begin()); break;}
	}
	for (unsigned d = 0; d < 2; ++d) { // add all basement walls
		for (cube_t &wall : interior->walls[d]) {
			if (wall.z1() >= ground_floor_z1) continue; // not in the basement
			walls.push_back(wall);
			walls.back().expand_by_xy(trim_thickness); // include the trim
		}
	}
	// Note: elevators/buttons/stairs haven't been placed at this point, so iterate over all objects
	for (room_object_t const &i : interior->room_geom->objs) {
		bool no_blocking(i.type == TYPE_PICTURE || i.type == TYPE_WBOARD);
		// Note: we exclude TYPE_PIPE (vertical electrical conduits from outlets) here because they may block pipes from running horizontally along walls
		if (i.no_coll() && !no_blocking && i.type != TYPE_LIGHT /*&& i.type != TYPE_PIPE*/) continue; // no collisions
		if (i.z1() >= ground_floor_z1) continue; // not in the basement
		cube_t obstacle(i);
		// Note: we could maybe skip if i.z2() < sewer_zval-pipe_radius, but we still need to handle collisions with vertical exit pipe segments

		if (i.type == TYPE_WHEATER) {
			// shrink to include the pipes since routing should be above the body
			float const radius(i.get_radius());
			cube_t c(i);
			set_wall_width(c, i.get_center_dim( i.dim), 0.2*radius,  i.dim); // width of vent
			set_wall_width(c, i.get_center_dim(!i.dim), 0.7*radius, !i.dim); // width of top pipes extent
			obstacles.push_back(c);
			obstacle.z2() -= 0.2*i.dz(); // shorten the height for the tank; needed for vertical exit pipes
		}
		else if (no_blocking) {
			obstacle.d[i.dim][!i.dir] += (i.dir ? 1.0 : -1.0)*wall_thickness; // add a wall thickness of clearance
		}
		obstacles.push_back(obstacle);
	} // for i
	// TODO: maybe should move the ceiling up or move th tops of the doors down to avoid door collisions
	float const door_trim_exp(2.0*trim_thickness + 0.5*wall_thickness);

	for (door_t const &d : interior->doors) {
		if (d.z1() >= ground_floor_z1) continue; // not in the basement
		door_t door(d);
		door.open = 0; // start closed
		cube_t door_bcube(get_door_bounding_cube(door));
		door_bcube.d[d.dim][ d.open_dir] += (d.open_dir ? 1.0 : -1.0)*d.get_width(); // include space for door to swing open
		door_bcube.d[d.dim][!d.open_dir] -= (d.open_dir ? 1.0 : -1.0)*door_trim_exp; // include door trim width on the other side

		if (!d.on_stairs) { // [basement] stairs doors don't really open, so we only need clearance in front
			door.open = 1; // now try the open door to avoid blocking it when open
			door_bcube.union_with_cube(get_door_bounding_cube(door));
		}
		obstacles.push_back(door_bcube);
	} // for doors
	for (stairwell_t const &s : interior->stairwells) { // add stairwells (basement stairs); there should be no elevators
		if (s.z1() >= ground_floor_z1) continue; // not in the basement
		obstacles.push_back(s);
	}
	vect_riser_pos_t sewer, cold_water, hot_water;
	get_pipe_basement_water_connections(sewer, cold_water, hot_water, rgen);
	add_basement_pipes(obstacles, walls, beams, sewer, pipe_cubes, room_id, num_floors, pipe_light_amt, sewer_zval,   rgen, PIPE_TYPE_SEWER, 1); // sewer
	add_to_and_clear(pipe_cubes, obstacles); // add sewer pipes to obstacles
	add_basement_pipes(obstacles, walls, beams, cold_water, pipe_cubes, room_id, num_floors, pipe_light_amt, cw_zval, rgen, PIPE_TYPE_CW,    1); // cold water
	add_to_and_clear(pipe_cubes, obstacles); // add cold water pipes to obstacles
	add_basement_pipes(obstacles, walls, beams, hot_water,  pipe_cubes, room_id, num_floors, pipe_light_amt, hw_zval, rgen, PIPE_TYPE_HW,    1); // hot water
#if 0 // TODO: finish/enable
	float const gas_zval(basement.z2() - 2.2*fc_thick);
	add_to_and_clear(pipe_cubes, obstacles); // add hot water pipes to obstacles
	vect_riser_pos_t gas_pipes;
	get_pipe_basement_gas_connections(gas_pipes);
	add_basement_pipes(obstacles, walls, beams, gas_pipes, pipe_cubes, room_id, num_floors, pipe_light_amt, gas_zval, rgen, PIPE_TYPE_GAS);
#endif
}

void building_t::add_parking_garage_ramp(rand_gen_t &rgen) {
	assert(interior && !is_house && has_parking_garage);
	cube_with_ix_t &ramp(interior->pg_ramp);
	assert(ramp.is_all_zeros()); // must not have been set
	cube_t const &basement(get_basement());
	bool const dim(basement.dx() < basement.dy()); // long/primary dim
	// see building_t::add_parking_garage_objs(); make sure there's space for a ramp plus both exit dirs within the building width
	float const width(basement.get_sz_dim(!dim)), road_width(min(0.25f*width, 2.3f*get_parked_car_size().y));
	float const window_vspacing(get_window_vspace()), floor_thickness(get_floor_thickness()), fc_thick(0.5*floor_thickness);
	float const z1(basement.z1() + fc_thick), z2(basement.z2() + fc_thick); // bottom level basement floor to first floor floor
	bool const ramp_pref_xdir(rgen.rand_bool()), ramp_pref_ydir(rgen.rand_bool());
	bool added_ramp(0), dir(0);

	for (unsigned pass = 0; pass < 2 && !added_ramp; ++pass) {
		for (unsigned xd = 0; xd < 2 && !added_ramp; ++xd) {
			for (unsigned yd = 0; yd < 2; ++yd) {
				bool const xdir(bool(xd) ^ ramp_pref_xdir), ydir(bool(yd) ^ ramp_pref_ydir);
				float const xsz((dim ? 2.0 : 1.0)*road_width), ysz((dim ? 1.0 : 2.0)*road_width); // longer in !dim
				unsigned const num_ext(unsigned(basement.d[0][xdir] == bcube.d[0][xdir]) + unsigned(basement.d[1][ydir] == bcube.d[1][ydir]));
				if (num_ext < 2-pass) continue; // must be on the exterior edge of the building in both dims for pass 0, and one dim for pass 1
				dir = (dim ? xdir : ydir);
				point corner(basement.d[0][xdir], basement.d[1][ydir], z1);
				corner[!dim] += (dir ? -1.0 : 1.0)*road_width; // shift away from the wall so that cars have space to turn onto the level floor
				point const c1((corner.x - 0.001*(xdir ? 1.0 : -1.0)*xsz), (corner.y - 0.001*(ydir ? 1.0 : -1.0)*ysz), z1); // slight inward shift to prevent z-fighting
				point const c2((corner.x + (xdir ? -1.0 : 1.0)*xsz), (corner.y + (ydir ? -1.0 : 1.0)*ysz), z2);
				cube_t const ramp_cand(c1, c2);
				assert(ramp_cand.is_strictly_normalized());
				cube_t test_cube(ramp_cand);
				test_cube.expand_in_dim(!dim, road_width); // extend outward for clearance to enter/exit the ramp (ramp dim is actually !dim)
				if (interior->is_blocked_by_stairs_or_elevator(test_cube)) continue;
				ramp = cube_with_ix_t(ramp_cand, (((!dim)<<1) + dir)); // encode dim and dir in ramp index field
				added_ramp = 1;
				break; // done
			} // for yd
		} // for xd
	} // for pass
	if (!added_ramp) return; // what if none of the 4 corners work for a ramp?
	// add landings, which are used to draw the vertical edges of the cutout
	unsigned num_floors(calc_num_floors(basement, window_vspacing, floor_thickness));
	float z(basement.z1() + window_vspacing); // start at upper floor rather than lower floor

	if (1) { // FIXME: rooms on the ground floor above ramps aren't yet handled, so clip ramps to avoid disrupting their floors until this is fixed
		ramp.z2() -= 2.0*floor_thickness;
		--num_floors;
		interior->ignore_ramp_placement = 1; // okay to place room objects over ramps because the floor has not been removed
	}
	for (unsigned f = 0; f < num_floors; ++f, z += window_vspacing) { // skip first floor - draw pairs of floors and ceilings
		landing_t landing(ramp, 0, f, !dim, dir, 0, SHAPE_RAMP, 0, (f+1 == num_floors), 0, 1); // for_ramp=1
		set_cube_zvals(landing, (z - fc_thick), (z + fc_thick));
		interior->landings.push_back(landing);
	}
	// cut out spaces from floors and ceilings
	subtract_cube_from_floor_ceil(ramp, interior->floors  );
	subtract_cube_from_floor_ceil(ramp, interior->ceilings);
	// make rooms over the ramp of type RTYPE_RAMP_EXIT
}

