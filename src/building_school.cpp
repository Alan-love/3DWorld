// 3D World - Hospital Buildings
// by Frank Gennari 2/26/25

#include "function_registry.h"
#include "buildings.h"
#include "city_model.h"

extern object_model_loader_t building_obj_model_loader;


bool building_t::add_classroom_objs(rand_gen_t rgen, room_t const &room, float zval, unsigned room_id, unsigned floor_ix,
	float tot_light_amt, unsigned objs_start, colorRGBA const &chair_color, unsigned &td_orient)
{
	if (room.has_elevator) return 0; // no classroom in an elevator
	float const vspace(get_window_vspace()), wall_thickness(get_wall_thickness());
	vector2d const room_sz(room.get_size_xy());
	if (room_sz.get_max_val() < 3.0*vspace || room_sz.get_min_val() < 1.8*vspace) return 0; // room is too small
	bool const dim(room_sz.x < room_sz.y); // long dim
	// front_dir is the side of the long dim not by the door
	bool valid_dirs[2] = {0,0}, door_sides[2] = {0,0};
	vect_door_stack_t const &doorways(get_doorways_for_room(room, zval)); // get interior doors

	for (unsigned dir = 0; dir < 2; ++dir) {
		cube_t fb_wall(room), side_wall(room);
		set_wall_width(fb_wall,   room.d[ dim][dir], wall_thickness,  dim);
		set_wall_width(side_wall, room.d[!dim][dir], wall_thickness, !dim);
		valid_dirs[dir] = !has_bcube_int(fb_wall,   doorways); // skip if there's a door on this wall
		door_sides[dir] =  has_bcube_int(side_wall, doorways);
	}
	bool dir(0); // front dir
	if (!valid_dirs[0] && !valid_dirs[1]) return 0; // no valid door-free walls
	if ( valid_dirs[0] &&  valid_dirs[1]) { // both ends are valid
		bool const ext[2] = {(classify_room_wall(room, zval, dim, 0, 0) == ROOM_WALL_EXT), (classify_room_wall(room, zval, dim, 1, 0) == ROOM_WALL_EXT)};
		if (ext[0] != ext[1]) {dir = ext[0];} // choose interior wall so that we can place a chalkboard behind the desk
		else {dir = rgen.rand_bool();} // choose a random end
	}
	else {dir = valid_dirs[1];} // only one valid wall

	float const clearance(get_min_front_clearance_inc_people());
	cube_t const room_bounds(get_walkable_room_bounds(room));
	float const td_width(0.8*vspace*rgen.rand_uniform(1.1, 1.2)), td_depth(0.38*vspace*rgen.rand_uniform(1.1, 1.2)), td_height(0.23*vspace*rgen.rand_uniform(1.12, 1.2));
	float const front_wall_pos(room_bounds.d[dim][dir]), room_center(room.get_center_dim(!dim)), dsign(dir ? -1.0 : 1.0);
	float const desk_front_pos(front_wall_pos + dsign*(max(1.2*clearance, 0.25*vspace) + wall_thickness)); // near wall, with space for chair
	float const desk_back_pos(desk_front_pos + dsign*td_depth);
	float const desk_width(0.48*vspace), desk_depth(0.34*vspace), desk_height(0.25*vspace);
	cube_t student_area(room_bounds);
	student_area.d[dim][ dir]  = desk_back_pos + dsign*1.3*clearance; // front side near teacher
	student_area.d[dim][!dir] -= dsign*0.5*clearance; // back wall

	for (unsigned dir = 0; dir < 2; ++dir) {
		student_area.d[!dim][dir] -= (dir ? 1.0 : -1.0)*(door_sides[dir] ? 1.0 : -0.45)*clearance; // leave space for doors but not side walls
	}
	float desk_wspacing(desk_width + clearance), desk_dspacing(desk_depth + clearance);
	float const avail_width(student_area.get_sz_dim(!dim)), avail_len(student_area.get_sz_dim(dim));
	unsigned const ncols(avail_width/desk_wspacing), nrows(avail_len/desk_dspacing);
	if (nrows < 1 || ncols < 1) return 0; // not enough space for student desks; shouldn't happen

	// place teacher desk at front
	cube_t tdesk;
	set_cube_zvals(tdesk, zval, zval+td_height);
	tdesk.d[dim][ dir] = desk_front_pos;
	tdesk.d[dim][!dir] = desk_back_pos ;
	set_wall_width(tdesk, room_center, 0.5*td_width, !dim);
	if (!add_classroom_desk(rgen, room, tdesk, room_id, tot_light_amt, chair_color, dim, dir, 0)) return 0; // desk_ix=0
	td_orient = 2*dim + dir; // place chalkboard behind the teacher desk
	// place rows and columns of student desks
	float const first_row_pos(student_area.d[dim][dir]);
	desk_wspacing = avail_width/ncols;
	desk_dspacing = avail_len  /nrows;

	for (unsigned row = 0; row < nrows; ++row) {
		for (unsigned col = 0; col < ncols; ++col) {
			cube_t desk;
			set_cube_zvals(desk, zval, zval+desk_height);
			desk.d[dim][ dir] = first_row_pos + dsign*desk_dspacing*row;
			desk.d[dim][!dir] = desk.d[dim][dir] + dsign*desk_depth;
			set_wall_width(desk, (student_area.d[!dim][0] + desk_wspacing*(col + 0.5)), 0.5*desk_width, !dim);
			if (!add_classroom_desk(rgen, room, desk, room_id, tot_light_amt, chair_color, dim, !dir, (1 + col + ncols*row))) continue;
		} // for col
	} // for row
	bool const add_bottles(0), add_trash(rgen.rand_float() < 0.4), add_papers(rgen.rand_float() < 0.4), add_glass(0);
	add_floor_clutter_objs(rgen, room, room_bounds, zval, room_id, tot_light_amt, objs_start, add_bottles, add_trash, add_papers, add_glass);
	add_numbered_door_sign("Classroom ", room, zval, room_id, floor_ix);
	return 1;
}

bool building_t::add_classroom_desk(rand_gen_t &rgen, room_t const &room, cube_t const &desk, unsigned room_id, float tot_light_amt,
	colorRGBA const &chair_color, bool dim, bool dir, unsigned desk_ix)
{
	vect_room_object_t &objs(interior->room_geom->objs);
	if (is_obj_placement_blocked(desk, room, 1)) return 0; // check proximity to doors, etc.
	bool const teacher_desk(desk_ix == 0);
	unsigned const desk_obj_ix(objs.size()), flags(teacher_desk ? RO_FLAG_HAS_EXTRA : 0); // teacher's desk always has drawers
	objs.emplace_back(desk, TYPE_DESK, room_id, dim, dir, flags, tot_light_amt, SHAPE_CUBE); // no tall desks
	set_obj_id(objs);
	objs.back().obj_id += 123*desk_ix; // more random variation
	// add paper, pens, and pencils
	unsigned const objs_start(objs.size()); // excludes the desk
	add_papers_to_surface      (desk, dim,  dir, 7, rgen, room_id, tot_light_amt); // add 0-7 sheet(s) of paper
	add_pens_pencils_to_surface(desk, dim, !dir, 4, rgen, room_id, tot_light_amt); // 0-4 pens/pencils

	if (teacher_desk && rgen.rand_float() < 0.33) { // add a cup on the desk 33% of the time
		vect_cube_t const avoid(objs.begin()+objs_start, objs.end()); // add all papers, pens, and pencils
		place_cup_on_obj(rgen, desk, room_id, tot_light_amt, avoid);
	}
	if (rgen.rand_float() < 0.33) { // maybe add a book on the desk
		place_book_on_obj(rgen, objs[desk_obj_ix], room_id, tot_light_amt, objs_start, 1, RO_FLAG_USED, 1); // use_dim_dir=1; skip_if_overlaps=1
	}
	// add chair
	point chair_pos;
	chair_pos.z     = desk.z1();
	chair_pos[ dim] = desk.d[dim][dir]; // front of desk
	chair_pos[!dim] = desk.get_center_dim(!dim) + 0.1*rgen.signed_rand_float()*desk.get_sz_dim(dim); // slightly misaligned
	
	if (!add_chair(rgen, room, vect_cube_t(), room_id, chair_pos, chair_color, dim, !dir, tot_light_amt, 0, 0, 0, 0, 2, 1)) { // no blockers, reduced_clearance=1
		objs.resize(desk_obj_ix); // no chair; remove the desk as well, and any objects placed on it, since it may be too close to a door
	}
	return 1;
}

void building_t::add_objects_next_to_classroom_chalkboard(rand_gen_t &rgen, room_object_t const &cb, room_t const &room, float zval, unsigned objs_start) {
	// add US flag; flags are two sided, so lighting doesn't look correct on the unlit side
	bool const dim(cb.dim), dir(cb.dir), side(dir ^ dim); // side of clock; always place on the right because the left side is lit with correct normals
	float const flag_pos(0.5*(cb.d[!dim][side] + room.d[!dim][side])), wall_pos(cb.d[dim][dir]); // halfway between edge of chalkboard and edge of room
	add_wall_us_flag(wall_pos, flag_pos, zval, dim, dir, cb.room_id, cb.light_amt);
	vect_room_object_t &objs(interior->room_geom->objs);
	if (is_obj_placement_blocked(objs.back(), room, 1)) {objs.pop_back();} // remove if invalid placement; inc_open_doors=1
	// add clock to the left side
	float const floor_spacing(get_window_vspace()), clock_sz(0.16*floor_spacing), clock_z1(zval + get_floor_ceil_gap() - 1.4*clock_sz), clock_depth(0.08*clock_sz);
	cube_t clock;
	set_cube_zvals(clock, clock_z1, clock_z1+clock_sz);
	set_wall_width(clock, 0.5*(cb.d[!dim][!side] + room.d[!dim][!side]), 0.5*clock_sz, !dim);
	clock.d[dim][!dir] = wall_pos;
	clock.d[dim][ dir] = wall_pos + (dir ? 1.0 : -1.0)*clock_depth;
	cube_t tc(clock);
	tc.d[dim][dir] += (dir ? 1.0 : -1.0)*0.5*floor_spacing; // add clearance

	if (overlaps_obj_or_placement_blocked(tc, room, objs_start)) { // bad placement, try shifting down (below vent, etc.)
		clock.translate_dim(2, -0.1*floor_spacing);
		set_cube_zvals(tc, clock.z1(), clock.z2());
	}
	if (!overlaps_obj_or_placement_blocked(tc, room, objs_start)) {add_clock(clock, cb.room_id, cb.light_amt, dim, dir, 0);} // digital=0
	// add plants after chalkboard to avoid blocking it
	unsigned const num_plants(rgen.rand() % 3); // 0-2
	add_plants_to_room(rgen, room, zval, cb.room_id, cb.light_amt, objs_start, num_plants);
}

void building_t::add_hallway_lockers(rand_gen_t &rgen, room_t const &room, float zval, unsigned room_id, unsigned floor_ix, float tot_light_amt, unsigned objs_start) {
	bool const dim(room.dx() < room.dy()); // hallway dim
	float const floor_spacing(get_window_vspace()), locker_height(0.75*floor_spacing), locker_depth(0.25*locker_height), locker_width(0.22*locker_height);
	cube_t room_bounds(get_walkable_room_bounds(room));
	cube_t place_area(room_bounds);
	place_area.expand_in_dim(dim, -4.0*locker_width); // leave 4 locker's worth of space at the ends for windows, etc.
	vect_room_object_t &objs(interior->room_geom->objs);
	float const hall_len(place_area.get_sz_dim(dim));
	unsigned const lockers_start(objs.size()), num_lockers(hall_len/locker_width); // floor
	// add expanded blockers for stairs and elevators in this hallway to ensure there's space for the player and people to walk on the sides
	float const se_clearance(2.0*get_min_front_clearance_inc_people());
	bool const add_padlocks(floor_ix == 0 && building_obj_model_loader.is_model_valid(OBJ_MODEL_PADLOCK));
	vector3d const sz(add_padlocks ? building_obj_model_loader.get_model_world_space_size(OBJ_MODEL_PADLOCK) : zero_vector); // D, W, H
	colorRGBA const lock_color(0.4, 0.4, 0.4);
	unsigned const num_locker_colors = 6;
	colorRGBA const locker_colors[num_locker_colors] =
	{colorRGBA(0.4, 0.6, 0.7), colorRGBA(0.4, 0.7, 0.6), colorRGBA(0.2, 0.5, 0.8), colorRGBA(0.7, 0.05, 0.05), colorRGBA(0.6, 0.45, 0.25), GRAY};
	colorRGBA const locker_color(locker_colors[(7*mat_ix + 11*room_id + 13*interior->rooms.size())%num_locker_colors]); // random per part/room
	vect_cube_t blockers;
	
	for (stairwell_t const &s : interior->stairwells) {
		if (room.contains_cube_xy_overlaps_z(s)) {blockers.push_back(s);}
	}
	for (elevator_t const &e : interior->elevators) {
		if (room.contains_cube_xy_overlaps_z(e)) {blockers.push_back(e);}
	}
	for (cube_t &c : blockers) {c.expand_by_xy(se_clearance);}
	cube_t locker;
	set_cube_zvals(locker, zval, zval+locker_height);
	unsigned lix(0);

	for (unsigned d = 0; d < 2; ++d) { // for each side of the hallway
		float const wall_edge(place_area.d[!dim][d]);
		locker.d[!dim][ d] = wall_edge;
		locker.d[!dim][!d] = wall_edge + (d ? -1.0 : 1.0)*locker_depth;

		for (unsigned n = 0; n < num_lockers; ++n) {
			float const pos(place_area.d[dim][0] + n*locker_width);
			locker.d[dim][0] = pos;
			locker.d[dim][1] = pos + locker_width;
			if (has_bcube_int(locker, blockers)) continue;
			cube_t test_cube(locker);
			test_cube.expand_in_dim(dim, 2.0*locker_width); // add some padding to the sides
			test_cube.d[!dim][!d] += (d ? -1.0 : 1.0)*locker_width; // add space in front for the door to open
			bool invalid(0);

			for (auto i = objs.begin()+objs_start; i != objs.begin()+lockers_start; ++i) { // can skip other lockers
				if (i->intersects(test_cube)) {invalid = 1; break;}
			}
			if (invalid || is_obj_placement_blocked(test_cube, room, 1,    0)) continue;
			if (!check_if_placed_on_interior_wall  (test_cube, room, !dim, d)) continue; // ensure the vent is on a wall
			unsigned flags(0);

			// add padlocks to some lockers; first floor only, to avoid having too many model objects (but may be added to stacked parts)
			if (add_padlocks && rgen.rand_float() < 0.25) {
				float const height(0.04*floor_spacing), hwidth(0.5*height*sz.y/sz.z), depth(height*sz.x/sz.z);
				cube_t lock;
				lock.z1() = zval + 0.365*floor_spacing;
				lock.z2() = lock.z1() + height;
				set_wall_width(lock, (locker.d[dim][0] + ((dim ^ bool(d)) ? 0.175 : 0.825)*locker_width), hwidth, dim);
				float const pos(locker.d[!dim][!d]);
				lock.d[!dim][ d] = pos;
				lock.d[!dim][!d] = pos + (d ? -1.0 : 1.0)*depth;
				objs.emplace_back(lock, TYPE_PADLOCK, room_id, !dim, d, (RO_FLAG_NOCOLL | RO_FLAG_IS_ACTIVE), 1.0, SHAPE_CUBE, lock_color); // attached
				flags |= RO_FLAG_NONEMPTY; // flag as locked
			}
			objs.emplace_back(locker, TYPE_LOCKER, room_id, !dim, !d, flags, tot_light_amt, SHAPE_CUBE, locker_color, lix++);
			set_obj_id(objs); // for random contents
		} // for n
	} // for d
	bool const add_bottles(0), add_trash(rgen.rand_float() < 0.75), add_papers(rgen.rand_float() < 0.5), add_glass(0);
	add_floor_clutter_objs(rgen, room, room_bounds, zval, room_id, tot_light_amt, objs_start, add_bottles, add_trash, add_papers, add_glass);
}

bool building_t::add_cafeteria_objs(rand_gen_t rgen, room_t const &room, float &zval, unsigned room_id, unsigned floor_ix, float tot_light_amt, unsigned objs_start) {
	if (room_has_stairs_or_elevator(room, zval, floor_ix)) return 0;
	float const floor_spacing(get_window_vspace());
	cube_t const room_bounds(get_walkable_room_bounds(room));
	vector2d const room_sz(room_bounds.get_size_xy());
	if (min(room_sz.x, room_sz.y) < 3.0*floor_spacing || max(room_sz.x, room_sz.y) < 3.5*floor_spacing) return 0; // too small to be a cafeteria
	if (!has_tile_floor()) {zval = add_flooring(room, zval, room_id, tot_light_amt, FLOORING_LGTILE);}
	bool const dim(room_sz.y < room_sz.x); // short dim
	unsigned const num_cols((room_sz[dim] > 3.5*floor_spacing) ? 2 : 1);
	float const clearance(get_min_front_clearance_inc_people()), col_space(1.25*clearance), trim_thick(get_trim_thickness());
	cube_t place_area(room_bounds);
	place_area.expand_by_xy(-1.1*clearance);
	float const row_span(place_area.get_sz_dim(!dim)), col_span(place_area.get_sz_dim(dim)), col_start(place_area.d[dim][0]);
	float const table_height(0.3*floor_spacing), table_width(0.4*floor_spacing), table_len((col_span + col_space)/num_cols - col_space);
	float row_spacing(2.5*table_width);
	unsigned const num_rows(max(1U, unsigned(row_span/row_spacing))); // floor
	row_spacing = row_span/num_rows;
	colorRGBA const &chair_color(mall_chair_colors[rgen.rand() % NUM_MALL_CHAIR_COLORS]);
	unsigned const tid_tag(rgen.rand() + 1); // sets table texture; make nonzero to flag as a textured surface table
	vect_door_stack_t const &doorways(get_doorways_for_room(room, zval));
	cube_t table;
	set_cube_zvals(table, zval, zval+table_height);
	vect_cube_t blockers;
	blockers.reserve(doorways.size());

	for (door_stack_t const &d : doorways) {
		float const width(d.get_width());
		blockers.push_back(d.get_true_bcube());
		blockers.back().expand_in_dim( d.dim, 1.5*width); // more space in front of door
		blockers.back().expand_in_dim(!d.dim, 1.0*width); // less space to the side
	}
	for (unsigned r = 0; r < num_rows; ++r) {
		float const row_pos(place_area.d[!dim][0] + (r + 0.5)*row_spacing);
		set_wall_width(table, row_pos, 0.5*table_width, !dim);
		float pos(col_start); // starting point

		for (unsigned c = 0; c < num_cols; ++c) {
			table.d[dim][0] = pos;
			table.d[dim][1] = pos + table_len;
			
			for (cube_t const &b : blockers) {
				if (!b.intersects(table)) continue;
				if      (b.d[dim][0] <= table.d[dim][0]) {table.d[dim][0] = b.d[dim][1] + trim_thick;} // clip lo
				else if (b.d[dim][1] >= table.d[dim][1]) {table.d[dim][1] = b.d[dim][0] - trim_thick;} // clip hi
			}
			if (table.get_sz_dim(dim) > table_width) {add_mall_table_with_chairs(rgen, table, place_area, chair_color, room_id, tot_light_amt, dim, tid_tag, blockers);}
			pos += table_len + col_space;
		} // for c
	} // for r
	add_clock_to_room_wall(rgen, room, zval, room_id, tot_light_amt, objs_start);
	add_door_sign("Cafeteria", room, zval, room_id);
	return 1;
}

