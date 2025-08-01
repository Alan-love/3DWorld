// 3D World - Building Navigation for AIs
// by Frank Gennari 3/14/20

#include "3DWorld.h"
#include "function_registry.h"
#include "buildings.h"
#include "city.h" // for person_t
#include "profiler.h"
#include "nav_grid.h"
#include "openal_wrap.h"
#include <queue>


bool  const ALLOW_AI_IN_MALLS   = 1;
bool  const USE_MALL_ENT_STAIRS = 0; // TODO: not yet working
float const COLL_RADIUS_SCALE   = 0.75; // somewhat smaller than radius, but larger than PED_WIDTH_SCALE

int player_hiding_frame(0);
building_dest_t cur_player_building_loc, prev_player_building_loc;
room_object_t player_hiding_obj;
vect_cube_t reused_avoid_cubes[2]; // temporary that's reused across frames and people
bool debug_mode(0);

extern bool player_is_hiding, player_on_escalator, player_in_tunnel;
extern int frame_counter, display_mode, animate2, player_in_elevator;
extern float fticks;
extern building_params_t global_building_params;
extern bldg_obj_type_t bldg_obj_types[];

bool ai_follow_player() {return (global_building_params.ai_follow_player || in_building_gameplay_mode());}
bool can_ai_follow_player(person_t const &person, bool allow_diff_building=0, bool allow_outside_room=0);
float get_closest_building_sound(point const &at_pos, point &sound_pos, float floor_spacing);
void maybe_play_zombie_sound(point const &sound_pos_bs, unsigned zombie_ix, bool alert_other_zombies, bool high_priority=0, float gain=1.0, float pitch=1.0);
int register_ai_player_coll(uint8_t &has_key, float height);
unsigned get_stall_cubes(room_object_t const &c, cube_t sides[3]);
unsigned get_shower_cubes(room_object_t const &c, cube_t sides[2]);
void resize_cubes_xy(vect_cube_t &cubes, float val);
void get_sphere_boundary_pts(point const &center, float radius, point *pts, bool skip_z=0);
unsigned get_L_stairs_first_flight_count(stairs_landing_base_t const &s, float landing_width);
void get_L_stairs_entrances(stairs_landing_base_t const &s, float doorway_width, bool for_placement, cube_t entrances[2]);
bool bed_is_wide(room_object_t const &c);

point get_cube_center_zval(cube_t const &c, float zval) {return point(c.xc(), c.yc(), zval);}
float get_ped_coll_radius() {return COLL_RADIUS_SCALE*ped_manager_t::get_ped_radius();}


bool check_line_int_xy(vect_cube_t const &c, point const &p1, point const &p2) {
	for (auto i = c.begin(); i != c.end(); ++i) {
		if (check_line_clip_xy(p1, p2, i->d)) return 1;
	}
	return 0;
}
float get_dist_xy_through_pt(point const &p1, point const &p2, point const &p3) {return (p2p_dist_xy(p1, p2) + p2p_dist_xy(p2, p3));}

unsigned cube_nav_grid::get_node_ix(point p) const {
	float gxy[2]; // grid index, with partial offset
	get_grid_ix_fp(p, gxy);
	return get_node_ix(round_fp(gxy[0]), round_fp(gxy[1]));
}
void cube_nav_grid::get_grid_ix_fp(point p, float gxy[2]) const {
	grid_bcube.clamp_pt_xy(p);
	for (unsigned d = 0; d < 2; ++d) {gxy[d] = (p[d] - grid_bcube.d[d][0])/step[d];}
}
float cube_nav_grid::get_distance(unsigned x1, unsigned y1, unsigned x2, unsigned y2) const {
	float const dx(float(x2) - float(x1)), dy(float(y2) - float(y1));
	return sqrt(dx*dx + dy*dy);
}
bool cube_nav_grid::find_open_node_closest_to(point const &p, point const &dest, unsigned &nx, unsigned &ny) const {
	if (!bcube.contains_pt_xy_inclusive(p)) return 0; // outside the grid valid area; error?
	float gxy[2]; // grid index, with partial offset
	get_grid_ix_fp(p, gxy);
	float dmin_sq(0.0);
	bool found(0);

	for (unsigned y = 0; y < 2; ++y) { // visit points in all directions < SQRT2*radius from p
		for (unsigned x = 0; x < 2; ++x) {
			unsigned const yi(y ? unsigned(ceil(gxy[1])) : unsigned(floor(gxy[1])));
			unsigned const xi(x ? unsigned(ceil(gxy[0])) : unsigned(floor(gxy[0])));
			if (!are_ixs_valid(xi, yi)) continue; // invalid, skip; error/can't happen?
			if ( is_blocked   (xi, yi)) continue; // blocked, skip
			float const dsq(p2p_dist_xy_sq(dest, get_grid_pt(xi, yi))); // choose dir closest to dest to avoid backtracking
			if (!found || dsq < dmin_sq) {found = 1; dmin_sq = dsq; nx = xi; ny = yi;} // keep if closer
		}
	} // for y
	return found;
}
bool cube_nav_grid::check_line_intersect(point const &p1, point const &p2, float radius) const {
	// zvals are ignored - effectively 2D; otherwise could call line_intersect_cubes()
	cube_t const check_cube(p1, p2);

	// check if vertical cylinder with this radius swept from p1 to p2 intersects an unexpanded cube; assumes cube is large relative to radius
	for (cube_t const &c : blockers_exp) {
		if (!c.intersects_xy(check_cube) || !check_line_clip_xy(p1, p2, c.d)) continue; // no intersection
		cube_t c_non_exp(c);
		c_non_exp.expand_by_xy(-radius); // shrink radius back off
		if (check_line_clip_xy(p1, p2, c_non_exp.d)) return 1;
		vector3d v(cross_product((p2 - p1), plus_z)); // tangent vector to the person's cylinder
		v *= radius/v.mag();
		if (check_line_clip_xy(p1+v, p2+v, c_non_exp.d)) return 1;
		if (check_line_clip_xy(p1-v, p2-v, c_non_exp.d)) return 1;
	} // for c
	return 0; // no intersection
}
void cube_nav_grid::get_region_xy_bounds(cube_t const &region, unsigned &x1, unsigned &x2, unsigned &y1, unsigned &y2) const { // bounds are inclusive
	float lo[2], hi[2];
	get_grid_ix_fp(region.get_llc(), lo);
	get_grid_ix_fp(region.get_urc(), hi);
	x1 = round_fp(lo[0]); y1 = round_fp(lo[1]); x2 = round_fp(hi[0]); y2 = round_fp(hi[1]);
}
void cube_nav_grid::make_region_walkable(cube_t const &region) {
	unsigned x1, y1, x2, y2;
	get_region_xy_bounds(region, x1, x2, y1, y2);

	for (unsigned y = y1; y <= y2; ++y) {
		for (unsigned x = x1; x <= x2; ++x) {nodes[get_node_ix(x, y)] = 0;}
	}
}
void cube_nav_grid::build(cube_t const &bcube_, vect_cube_t const &blockers, float radius_, bool add_edge_pad, bool no_blocker_expand) {
	invalid    = 0; // reset, in case build() was called on a nonempty but invalid cube_nav_grid
	radius     = radius_;
	bcube      = bcube_;
	grid_bcube = bcube;
	// determine grid size and allocate vectors
	float const spacing(SQRT2*radius); // can't be further than the test diameter or we'll miss blockers in the gap; use sqrt(2) to make spheres diagonally tangential
	if (add_edge_pad) {grid_bcube.expand_by_xy(-radius);} // entity must fit fully inside the grid if add_edge_pad=1
	vector2d const size(grid_bcube.get_size_xy());
	if (min(size.x, size.y) <= 2.0*spacing) return; // too small (error?)

	for (unsigned d = 0; d < 2; ++d) {
		num [d] = unsigned(ceil(size[d]/spacing));
		step[d] = size[d]/(num[d] - 1);
		assert(num[d] < (1<<15)); // limit to a reasonable value that fits in a 16-bit integer
	}
	nodes.clear(); // in case we're rebuilding, have to zero initialize below
	nodes.resize(num[0]*num[1], 0); // starts unblocked
	//cout << TXT(blockers.size()) << TXT(num[0]) << TXT(num[1]) << TXT(nodes.size()) << endl;
	// determine open nodes; edges are implicitly from adjacent 0 nodes
	blockers_exp = blockers;
	if (!no_blocker_expand) {resize_cubes_xy(blockers_exp, radius);}
	vect_cube_t row_blockers;

	for (unsigned y = 0; y < num[1]; ++y) {
		float const yval(grid_bcube.y1() + y*step[1]);
		row_blockers.clear();

		for (cube_t const &c : blockers_exp) {
			if (c.y1() < yval && c.y2() > yval) {row_blockers.push_back(c);} // include if it spans yval
		}
		for (unsigned x = 0; x < num[0]; ++x) {
			if (check_vect_cube_contains_pt_xy(row_blockers, get_grid_pt(x, y))) {nodes[get_node_ix(x, y)] = 1;} // mark blocked
		}
	} // for y
}
bool cube_nav_grid::find_path(point const &p1, point const &p2, ai_path_t &path) const {
	assert(is_built());
	if (nodes.empty()) return 0; // not built or too small/empty
	//highres_timer_t timer("Find Path"); // ~1.3ms max
	assert(p1.z == p2.z); // must be horizontal
	unsigned nx1(0), ny1(0), nx2(0), ny2(0);
	if (!find_open_node_closest_to(p1, p2, nx1, ny1) || !find_open_node_closest_to(p2, p1, nx2, ny2)) return 0;
	// does it make sense to use A* rather than Dijkstra? maybe not because paths will generally be short compared to the number of total nodes
	unsigned start_ix(get_node_ix(nx1, ny1)), end_ix(get_node_ix(nx2, ny2));

	if (start_ix == end_ix) { // short path case; probably shouldn't be calling this function if there's a simple path from p1 to p2
		path.add(get_grid_pt(nx1, ny1, p1.z)); // add the single point
		return 1;
	}
	if (path.empty()) {path.push_back(p1);} // will assert otherwise
	vector<a_star_node_state_t> state(nodes.size()); // dense vector; unordered_map seems to be slower
	vector<uint8_t> open(nodes.size(), 0), closed(nodes.size(), 0); // tentative/already evaluated nodes
	std::priority_queue<pair<float, ix_pair_t> > open_queue;
	a_star_node_state_t &start(state[start_ix]);
	start.g_score  = 0.0;
	start.f_score  = get_distance(nx1, ny1, nx2, ny2); // estimated total cost from start to end
	open[start_ix] = 1;
	open_queue.push(make_pair(-start.f_score, ix_pair_t(nx1, ny1)));

	while (!open_queue.empty()) {
		ix_pair_t const cur(open_queue.top().second);
		unsigned const cur_ix(get_node_ix(cur.x, cur.y));
		open_queue.pop();
		closed[cur_ix] = 1;
		open  [cur_ix] = 0;

		for (int dy = -1; dy <= 1; ++dy) { // check 3x3 grid except center
			for (int dx = -1; dx <= 1; ++dx) {
				if (dx == 0 && dy == 0) continue; // skip self
				unsigned const new_x(cur.x + dx), new_y(cur.y + dy); // may wrap around to 2^32
				if (!are_ixs_valid(new_x, new_y)) continue; // off the grid
				unsigned const new_ix(get_node_ix(new_x, new_y));
				if (          closed[new_ix] ) continue; // already closed (duplicate)
				if (is_blocked(nodes[new_ix])) continue; // blocked
				a_star_node_state_t &sn(state[new_ix]);
				float const new_g_score(state[cur_ix].g_score + get_distance(cur.x, cur.y, new_x, new_y));
				if (!open[new_ix]) {open[new_ix] = 1;}
				else if (new_g_score >= sn.g_score) continue; // not better
				sn.set(cur.x, cur.y, new_x, new_y);

				if (new_ix == end_ix) { // done, reconstruct path (in reverse)
					assert(!path.empty()); // p1 should have been added by the caller
					unsigned const rev_start_ix(path.size());
					path.add(get_grid_pt(nx2, ny2, p1.z)); // last point, added first
					unsigned path_ix(cur_ix), prev_x(new_x), prev_y(new_y);
					int prev_dx(0), prev_dy(0); // starts at an invalid value so that the first point is always added

					while (path_ix != start_ix) {
						assert(path_ix < state.size());
						a_star_node_state_t &sp(state[path_ix]);
						int const xn(sp.came_from[0]), yn(sp.came_from[1]);
						assert(xn >= 0 && yn >= 0);
						assert(xn != (int)prev_x || yn != (int)prev_y); // must have a delta
						point const path_pt(get_grid_pt(xn, yn, p1.z));
						// smooth path by removing colinear points and merging unblocked segments
						int dx(xn - int(prev_x)), dy(yn - int(prev_y));
						if (dx == prev_dx && dy == prev_dy && !path.empty()) {path.pop_back();} // same angle, extend previous point; remove last point and re-add
						path.add(path_pt); // add new point
						path_ix = get_node_ix(xn, yn);
						prev_x = xn; prev_y = yn; prev_dx = dx; prev_dy = dy;
					} // end while()
					path.add(get_grid_pt(nx1, ny1, p1.z)); // first point, added last
					reverse(path.begin()+rev_start_ix, path.end());
					// run another pass to remove unnecessary points
					path.push_back(p2); // temporary end point; don't call path.add() because we need to remove it later

					while (1) { // iteratively remove points until no more can be removed
						unsigned const orig_sz(path.size());

						for (unsigned i = rev_start_ix; i+1 < path.size(); ++i) { // inefficient, but simple; can this introduce duplicate points?
							if (!check_line_intersect(path[i-1], path[i+1], radius)) {path.erase(path.begin() + i); --i;}
						}
						if (path.size() == orig_sz) break; // no points removed - done
					} // end while
					path.pop_back(); // remove p2
					return 1; // success
				}
				sn.g_score = new_g_score;
				sn.f_score = sn.g_score + get_distance(new_x, new_y, nx2, ny2);
				open_queue.push(make_pair(-sn.f_score, ix_pair_t(new_x, new_y)));
			} // for dx
		} // for dy
	} // end while()
	return 0; // failed - no path from room1 to room2
}

class building_cube_nav_grid : public cube_nav_grid { // for backrooms
public:
	void build_for_building(cube_t const &bcube_, vect_cube_t const &blockers, vect_door_stack_t const &door_stacks,
		vector<stairwell_t> const &stairwells, float stairs_extend, float radius_)
	{
		build(bcube_, blockers, radius_, 1, 0); // add_edge_pad=1, no_blocker_expand=0
		// flag all nodes in doorways as walkable to ensure the graph is connected, even if it means people will clip through the door frame;
		// other solutions involve aligning doors with the grid (too difficult/restrictive) or reducing spacing to doorway_width/(2*radius) (too slow)
		for (door_stack_t const &d : door_stacks) {
			if (!bcube.intersects(d)) continue; // wrong room; includes zval check
			cube_t region(d.get_clearance_bcube()); // include space to both sides
			set_wall_width(region, region.get_center_dim(!d.dim), 0.0, !d.dim); // set width to zero so that exactly one grid is contained in this dim
			make_region_walkable(region);
		}
		// make sure all stairwells connecting to this room are reachable
		for (stairwell_t const &s : stairwells) {
			if (!s.intersects(bcube)) continue;
			cube_t entrance(s);
			bool const is_top(bcube.zc() > s.zc()), dir(s.dir ^ is_top ^ 1);
			entrance.d[s.dim][!dir] = s.d[s.dim][dir] - (dir ? -1.0 : 1.0)*stairs_extend; // shrink to extend length at the entrance to the stairs; will be denormalized
			cube_t region(s);
			set_wall_width(region, entrance.get_center_dim(s.dim), 0.0, s.dim); // shrink to zero area
			make_region_walkable(region);
		} // for s
	}
	void create_debug_objs(vect_room_object_t &objs) const {
		for (unsigned y = 0; y < num[1]; ++y) {
			for (unsigned x = 0; x < num[0]; ++x) {
				if (is_blocked(x, y)) continue; // blocked
				cube_t c; c.set_from_sphere((get_grid_pt(x, y) + vector3d(0.0, 0.0, radius)), radius);
				objs.emplace_back(c, TYPE_DBG_SHAPE, 0, 0, 0, (RO_FLAG_NOCOLL | RO_FLAG_BACKROOM), 1.0, SHAPE_SPHERE, RED); // room_id=0
			}
		}
	}
}; // building_cube_nav_grid

// Note: this should go into building_t/buildings.h at some point, but is temporarily here
class building_nav_graph_t {
	struct conn_room_t { // size=16
		unsigned ix; // node index
		int door_ix; // index of connecting door on first floor, or connecting mall gate if >= doors.size(); -1 for connections with out a door
		vector2d pt[2]; // stairs entry/exit point {up, down}
		conn_room_t(unsigned ix_, int dix, vector2d const &ptu, vector2d const &ptd) : ix(ix_), door_ix(dix) {pt[0] = ptu; pt[1] = ptd;}
	};
	struct node_t { // represents one room or one stairwell
		bool has_exit=0, is_hallway=0, is_stairs=0, is_ramp=0; // has_exit is not yet used
		cube_t bcube;
		vector<conn_room_t> conn_rooms;

		point get_center(float zval) const {return get_cube_center_zval(bcube, zval);}
		bool is_vert_conn() const {return (is_stairs || is_ramp);} // or is_elevator if added later

		void add_conn_room(unsigned room, int door_ix, cube_t const &cu, cube_t const &cd) {
			for (auto i = conn_rooms.begin(); i != conn_rooms.end(); ++i) {if (i->ix == room && i->door_ix == door_ix) return;} // ignore duplicates
			conn_rooms.emplace_back(room, door_ix, vector2d(cu.xc(), cu.yc()), vector2d(cd.xc(), cd.yc()));
		}
	};
	struct a_star_node_state_t {
		int came_from_ix=-1;
		point path_pt;
		float g_score=0.0, f_score=0.0;
	};

	unsigned num_rooms=0, num_stairs=0;
	float stairs_extend=0;
	bool has_pg_ramp=0, has_mall_ent=0;
	vector<node_t> nodes; // {rooms, stairs, mall entrance stairs, parking garage ramp}
	mutable vector<building_cube_nav_grid> nav_grids; // for use with backrooms; cached during path finding
	node_t       &get_node(unsigned room)       {assert(room < nodes.size()); return nodes[room];}
	node_t const &get_node(unsigned room) const {assert(room < nodes.size()); return nodes[room];}
	unsigned get_stairs_end() const {return (num_stairs + num_rooms + has_mall_ent);}

	void remove_connection(unsigned from, unsigned to) {
		auto &conn(get_node(from).conn_rooms);

		for (auto i = conn.begin(); i != conn.end(); ++i) {
			if (i->ix == to) {swap(*i, conn.back()); conn.pop_back(); return;}
		}
		assert(0); // must be found - should not get here
	}
public:
	bool invalid=0;
	void invalidate_nav_grid(unsigned floor_ix) {
		assert(floor_ix < nav_grids.size()); // too strong?
		if (floor_ix < nav_grids.size()) {nav_grids[floor_ix].invalidate();}
	}
	building_nav_graph_t(float stairs_extend_) : stairs_extend(stairs_extend_) {}

	void set_num_rooms(unsigned num_rooms_, unsigned num_stairs_, bool has_pg_ramp_, bool has_mall_ent_) {
		num_rooms    = num_rooms_;
		num_stairs   = num_stairs_;
		has_pg_ramp  = has_pg_ramp_;
		has_mall_ent = has_mall_ent_;
		nodes.resize(get_stairs_end() + has_pg_ramp);
		for (unsigned n = num_rooms; n < get_stairs_end(); ++n) {nodes[n].is_stairs = 1;}
		if (has_pg_ramp) {nodes.back().is_ramp = 1;}
	}
	void set_room_bcube  (unsigned room,   cube_t const &c) {get_node(room).bcube = c;}
	void set_stairs_bcube(unsigned stairs, cube_t const &c) {get_node(stairs + num_rooms).bcube = c;}
	void set_ramp_bcube                   (cube_t const &c) {get_node(get_stairs_end()  ).bcube = c;}
	void mark_hallway(unsigned room) {get_node(room).is_hallway = 1;} // or mall concourse
	void mark_exit   (unsigned room) {get_node(room).has_exit   = 1;}

	void connect_stairs(unsigned room, unsigned stairs, stairwell_t const &s, float doorway_width) {
		assert(room < num_rooms && stairs < (num_stairs + has_mall_ent));
		unsigned const node_ix2(num_rooms + stairs);
		node_t &n2(get_node(node_ix2));
		assert(n2.is_stairs);
		cube_t entry_u(n2.bcube), entry_d(n2.bcube);
		bool const dim(s.dim), dir(s.dir);
		float const extend((dir ? -1.0 : 1.0)*stairs_extend); // extend away from stairs for entrance/exit area; will be denormalized in this dim

		if (s.is_u_shape()) { // U-shaped stairs: entrances are on the same side
			bool const side(dir); // Note: see code in add_stairs_and_elevators()
			entry_u.d[dim][dir] = entry_d.d[dim][dir] = entry_u.d[dim][!dir] + extend; // shrink to extend length at the entrance to the stairs
			float const mid(n2.bcube.get_center_dim(!dim));
			entry_u.d[!dim][ side] = mid; // bottom
			entry_d.d[!dim][!side] = mid; // top
		}
		else if (s.is_l_shape()) { // L-shaped stairs: entrances are at right angles
			cube_t entrances[2]; // {lower, upper}
			get_L_stairs_entrances(s, doorway_width, 0, entrances); // for_placement=0
			entry_u = entrances[0]; entry_d = entrances[1];
		}
		else if (s.shape == SHAPE_FAN) { // mall entrance
			entry_u.d[dim][ dir] = entry_u.d[dim][!dir]; // no extend?
			entry_d.d[dim][!dir] = entry_d.d[dim][ dir];
		}
		else { // straight stairs: entrances are on opposite ends
			entry_u.d[dim][ dir] = entry_u.d[dim][!dir] + extend; // shrink to extend length at the entrance to the stairs when going up
			entry_d.d[dim][!dir] = entry_d.d[dim][ dir] - extend; // shrink to extend length at the entrance to the stairs when going down
		}
		get_node(room).add_conn_room(node_ix2, -1, entry_u, entry_d); // Note: entry_u and entry_d are denormalized here
		n2.add_conn_room(room, -1, entry_u, entry_d);
	}
	void connect_ramp(unsigned room, bool dim, bool dir) {
		assert(room < num_rooms);
		unsigned const node_ix2(get_stairs_end()); // pg_ramp comes after all stairs
		node_t &n2(get_node(node_ix2));
		assert(n2.is_ramp);
		cube_t entry_u(n2.bcube), entry_d(n2.bcube);
		float const extend((dir ? -1.0 : 1.0)*stairs_extend); // extend away from stairs for entrance/exit area; will be denormalized in this dim
		// straight ramp: entrances are on opposite ends
		entry_u.d[dim][ dir] = entry_u.d[dim][!dir] + extend; // shrink to extend length at the entrance to the ramp when going up
		entry_d.d[dim][!dir] = entry_d.d[dim][ dir] - extend; // shrink to extend length at the entrance to the ramp when going down
		get_node(room).add_conn_room(node_ix2, -1, entry_u, entry_d); // Note: entry_u and entry_d are denormalized here
		n2.add_conn_room(room, -1, entry_u, entry_d);
	}
	void connect_rooms(unsigned room1, unsigned room2, int door_ix, cube_t const &conn_bcube) { // graph is bidirectional
		assert(room1 < num_rooms && room2 < num_rooms);
		get_node(room1).add_conn_room(room2, door_ix, conn_bcube, conn_bcube);
		get_node(room2).add_conn_room(room1, door_ix, conn_bcube, conn_bcube);
	}
	void disconnect_room_pair(unsigned room1, unsigned room2) { // remove connections in both directions
		assert(room1 != room2 && room1 < num_rooms && room2 < num_rooms);
		remove_connection(room1, room2);
		remove_connection(room2, room1);
	}
	bool is_room_connected_to(unsigned room1, unsigned room2, building_interior_t const &interior, float zval, unsigned has_key) const {
		// Note: likely faster than running full A* algorithm
		assert(room1 < num_rooms && room2 < num_rooms);
		if (room1 == room2) return 1;
		assert(!invalid);
		vector<uint8_t> seen(nodes.size(), 0);
		vector<unsigned> pend;
		pend.push_back(room1);
		seen[room1] = 1;

		while (!pend.empty()) {
			node_t const &node(get_node(pend.back()));
			pend.pop_back();

			for (auto i = node.conn_rooms.begin(); i != node.conn_rooms.end(); ++i) {
				if (seen[i->ix]) continue;
				if (!can_use_conn(*i, interior, zval, has_key)) continue; // blocked by closed or locked door
				if (i->ix == room2) return 1; // found, done
				pend.push_back(i->ix);
				seen[i->ix] = 1;
			}
		} // end while()
		return 0; // not found
	}
	unsigned count_connected_components() const {
		if (nodes.empty()) return 0;
		vector<uint8_t> seen(nodes.size(), 0);
		vector<unsigned> pend;
		unsigned ncomp(0);
		
		for (unsigned n = 0; n < nodes.size(); ++n) {
			if (seen[n]) continue; // node already processed
			++ncomp; // start a new component
			pend.push_back(n);
			seen[n] = 1;

			while (!pend.empty()) {
				node_t const &node(get_node(pend.back()));
				pend.pop_back();

				for (auto i = node.conn_rooms.begin(); i != node.conn_rooms.end(); ++i) {
					if (!seen[i->ix]) {pend.push_back(i->ix); seen[i->ix] = 1;}
				}
			} // end while()
		} // for n
		return ncomp;
	}
	bool is_fully_connected() const {return (count_connected_components() == 1);}

	// Note: assumes zvals are already checked
	static bool is_valid_pos(vect_cube_t const &avoid, building_t const &building, point const &pos, float radius) {
		cube_t c(pos);
		c.expand_by_xy(radius);

		for (auto i = avoid.begin(); i != avoid.end(); ++i) {
			if (i->intersects_xy(c) && sphere_cube_intersect_xy(pos, radius, *i)) return 0;
		}
		if (!building.check_cube_within_part_sides(c)) return 0; // outside non-cube building - will try again with a new pos next time
		return 1;
	}
	point get_stairs_entrance_pt(float zval, unsigned node_ix, bool up_or_down) const {
		// should we always move to the right side on wide stairs such as in malls so that other people can pass by going the other direction?
		node_t const &node(get_node(node_ix));
		assert(node.is_stairs || node.is_ramp);
		assert(!node.bcube.is_all_zeros()); // must have been filled in during graph building
		if (node.conn_rooms.empty()) {cout << TXT(node.is_stairs) << TXT(node.is_ramp) << TXT(node_ix) << TXT(node.bcube.str()) << endl;}
		assert(!node.conn_rooms.empty());
		vector2d const &pt(node.conn_rooms.front().pt[up_or_down]); // Note: all conn_rooms should be the same x/y value
		return point(pt.x, pt.y, zval);
	}
	static bool find_valid_pt_in_room(vect_cube_t const &avoid, building_t const &building, float radius, float zval,
		cube_t const &room, rand_gen_t &rgen, point &pos, bool no_use_init=0)
	{
		if (!no_use_init && avoid.empty()) return 1; // no colliders, any point is valid, choose the initial point (which should be the room center)
		cube_t place_area(room);
		place_area.expand_by_xy(-2.0*radius); // shrink by twice the radius
		if (!place_area.is_strictly_normalized()) return 0; // should generally not be true

		if (no_use_init) { // chose a new initial point
			gen_xy_pos_in_cube(pos, place_area, rgen);
			if (avoid.empty() && building.is_cube()) return 1; // no collision checks needed
		}
		point orig_pos(pos);

		for (unsigned n = 0; n < 100; ++n) { // 100 random tries to find a valid dest_pos
			if (is_valid_pos(avoid, building, pos, radius)) return 1; // success
			gen_xy_pos_in_cube(pos, place_area, rgen); // choose a random new point in the room
		}
		pos = orig_pos; // use orig value as failed point
		return 0;
	}
	point get_room_center(building_t const &building, cube_t const &room, float zval) const {
		point pos(get_cube_center_zval(room, zval));
		if (building.is_cube()) return pos;
		int const part_ix(building.get_part_ix_containing_pt(pos));
		if (part_ix < 0) return pos; // shouldn't get here
		cube_t const &part(building.parts[part_ix]);
		if (room.get_area_xy() > 0.75*part.get_area_xy()) return pos; // part is a single large room
		point const part_center(part.get_cube_center());
		// if room is adjacent to the part center (pie slice), then move the point toward the center and away from the curved exterior wall
		if (!room.contains_pt_xy_exp(part_center, building.get_wall_thickness())) return pos;
		float const blend_val(1.0/SQRT2);
		return blend_val * pos + (1.0 - blend_val)*point(part_center.x, part_center.y, pos.z);
	}
	// up_or_down: 0=up, 1=down
	point find_valid_room_dest(vect_cube_t const &avoid, building_t const &building, float radius, float zval, unsigned node_ix,
		bool up_or_down, bool &not_room_center, rand_gen_t &rgen, bool no_use_init, point const *const custom_dest) const
	{
		node_t const &node(get_node(node_ix));
		assert(!node.is_vert_conn()); // must be room, not stairs or ramp
		point pos;
		if (custom_dest != nullptr) {pos = *custom_dest;}
		else {pos = get_room_center(building, node.bcube, zval);} // first candidate is the center of the room
		
		if (building.is_above_retail_area(pos) && !building.point_over_glass_floor(pos)) {
			point const landing_pos(building.get_retail_upper_stairs_landing_center());
			//zval += (up_or_down ? -1.0 : 1.0)*building.get_window_vspace(); // one floor above or below so that we skip the blocked floor; but this asserts
			return point(landing_pos.x, landing_pos.y, zval);
		}
		if (find_valid_pt_in_room(avoid, building, radius, zval, node.bcube, rgen, pos, no_use_init)) {not_room_center = 1;} // success
		return pos;
	}

	static void choose_pt_xy_in_room_avoid_pool(point &pos, cube_t const &c, building_t const &building, unsigned room_ix, unsigned pt_ix, rand_gen_t &rgen) {
		if (pt_ix < 4 && building.has_pool()) { // check for and handle the pool with a special case, since it can be difficult to navigate around
			indoor_pool_t const &pool(building.interior->pool);

			if ((unsigned)pool.room_ix == room_ix) { // this is the pool room
				room_t const &room(building.get_room(room_ix));
				bool const xd(pt_ix & 1), yd(pt_ix >> 1); // use pt_ix 0-3 to select a corner
				float const xv(0.5*(room.d[0][xd] + pool.d[0][xd])), yv(0.5*(room.d[1][yd] + pool.d[1][yd])); // halfway between the room and pool

				if (xv > c.x1() && xv < c.x2() && yv > c.y1() && yv < c.y2()) { // valid corner
					pos.x = xv; pos.y = yv;
					return;
				}
			}
		}
		gen_xy_pos_in_cube(pos, c, rgen);
	}
	static bool maybe_shorten_path(point const &p1, point const &p2, point &p, vect_cube_t const &keepout) {
		float const t(get_closest_pt_on_line_t(p1, p, p2));
		if (t <= 0.0 || t >= 1.0) return 0; // closest point not on line
		point cand(p + t*(p2 - p));
		if (!check_line_int_xy(keepout, p1, cand) && !check_line_int_xy(keepout, cand, p2)) {p = cand; return 1;} // good point
		cand = p + (0.5*t)*(p2 - p); // try the halfway point
		if (!check_line_int_xy(keepout, p1, cand) && !check_line_int_xy(keepout, cand, p2)) {p = cand; return 1;} // good point
		return 0; // bad point
	}
	static bool maybe_shorten_either_path(point const &p1, point const &p2, point &p, vect_cube_t const &keepout) {
		if (maybe_shorten_path(p1, p2, p, keepout)) return 1;
		if (maybe_shorten_path(p2, p1, p, keepout)) return 1;
		return 0;
	}
	// add any necessary points to <path> that are required to get from <p1> to <p2> inside <walk_area> without intersecting <avoid>
	// return: 0=failed, 1=regular path, 2=nav grid path
	int connect_room_endpoints(vect_cube_t const &avoid, building_t const &building, cube_t const &walk_area, unsigned room_ix, point const &p1, point const &p2,
		float radius, ai_path_t &path, vect_cube_t &keepout, rand_gen_t &rgen, bool ignore_p1_coll=0, bool ignore_p2_coll=0, bool no_grid_path=0) const
	{
		assert(p1.z == p2.z);
		bool is_path_valid(1);
		cube_t walk_area_exp(walk_area);
		walk_area_exp.expand_by_xy(radius); // to capture objects that we could intersect when our center is in walk_area
		keepout.clear();
		building.add_sub_rooms_to_avoid_if_needed(building.get_room(room_ix), keepout); // must avoid sub-rooms

		for (auto i = avoid.begin(); i != avoid.end(); ++i) {
			if (!i->intersects_xy(walk_area_exp)) continue;
			cube_t c(*i);
			c.expand_by_xy(radius);
			bool skip(0), failed(0);

			for (unsigned d = 0; d < 2; ++d) {
				point const &p(d ? p2 : p1);
				if (!c.contains_pt_xy(p)) continue; // okay/keep
				if (!(d ? ignore_p2_coll : ignore_p1_coll)) {failed = 1; continue;} // fail - but we may want to skip if the other point is contained, so we can't return 0

				for (unsigned dim = 0; dim < 2; ++dim) {
					bool const dir(c.get_center_dim(dim) < p[dim]); // direction of point relative to cube center
					cube_t cc(c);
					cc.d[dim][dir] = p[dim] - (dir ? 1.0 : -1.0)*0.1*radius; // clip to exclude p, and a bit more to avoid false intersections
					if (cc.get_sz_dim(dim) <= 0.0) continue;
					if ((d ? ignore_p1_coll : ignore_p2_coll) && c.contains_pt_xy(d ? p1 : p2)) continue; // skip entirely if the partial cube contains the other point
					keepout.push_back(cc); // keep partial cube (expanded by radius)
					if (check_line_clip_xy(p1, p2, cc.d)) {is_path_valid = 0;}
				} // for dim
				skip = 1;
			} // for d
			if (skip  ) continue;
			if (failed) return 0;
			c.expand_by_xy(-0.01*radius); // shrink oh so slightly to avoid false line intersections due to FP error
			if (check_line_clip_xy(p1, p2, c.d)) {is_path_valid = 0;}
			keepout.push_back(c); // expanded by radius
		} // for i
		if (is_path_valid) return 1; // done

		// straight line not valid, need to run path finding
		assert(!keepout.empty());
		cube_t pts_bcube(p1, p2);
		bool const use_pts_bcube(min(pts_bcube.dx(), pts_bcube.dy()) > 2.0*radius && pts_bcube.intersects_xy(walk_area)); // don't use if too small/narrow
		if (use_pts_bcube) {pts_bcube.intersect_with_cube_xy(walk_area);} // must be contained in walkable area of the current room (in case p1 or p2 is in adjacent room)

		// do something simple and brute force:
		// since rooms tend to only have objects in the middle, try to find the best point that creates the shortest non-intersecting 2-part or 3-part path
		for (unsigned npts = 1; npts <= 2; ++npts) { // try to add 1 point; if that fails, try to add 2 points
			unsigned const num_tries((npts == 1) ? 200 : 100);
			float dmin(0.0);
			bool use_pos2(0);
			point best_pt, best_pt2;

			for (unsigned n = 0; n < num_tries; ++n) { // make num_tries attempts
				point pos, pos2;
				pos.z = pos2.z = p1.z;
				cube_t const &pref_bcube((use_pts_bcube && n < num_tries/2) ? pts_bcube : walk_area); // prefer point inside p1/p2 bcube (for backrooms)
				choose_pt_xy_in_room_avoid_pool(pos, pref_bcube, building, room_ix, n, rgen); // choose a rand point in the room
				if (check_vect_cube_contains_pt_xy(keepout, pos)) continue; // bad point
				if (!building.check_pt_within_part_sides(pos))    continue; // outside non-cube building
				bool const seg1_bad(check_line_int_xy(keepout, p1, pos)), seg2_bad(check_line_int_xy(keepout, pos, p2));

				if (npts == 1 || !(seg1_bad || seg2_bad)) { // single point or both segments valid
					if (seg1_bad || seg2_bad) continue; // bad point (either line intersects)
					float const dist(get_dist_xy_through_pt(p1, pos, p2));
					if (dmin == 0.0 || dist < dmin) {best_pt = pos; dmin = dist; use_pos2 = 0;}
				}
				else { // npts == 2
					if (seg1_bad && seg2_bad) continue; // bad point (both lines intersect)
					bool success(0);

					for (unsigned m = 0; m < 20; ++m) { // make 20 random attempts
						choose_pt_xy_in_room_avoid_pool(pos2, pref_bcube, building, room_ix, m, rgen); // choose a rand point in the room
						if (!building.check_pt_within_part_sides(pos2)) continue; // outside non-cube building
						if (!check_vect_cube_contains_pt_xy(keepout, pos2) && !check_line_int_xy(keepout, pos, pos2)) {success = 1; break;} // good point
					}
					if (!success) continue; // no good point

					for (unsigned ordering = 0; ordering < 2; ++ordering) { // try {p1-pos-pos2-p2} and {p1-pos2-pos-p2}
						if (check_line_int_xy(keepout, pos, p1) || check_line_int_xy(keepout, pos2, p2)) {swap(pos, pos2); continue;} // bad point (could optimize this if needed)
						float const dist(get_dist_xy_through_pt(p1, pos, pos2) + p2p_dist_xy(pos2, p2));
						if (dmin == 0.0 || dist < dmin) {best_pt = pos; best_pt2 = pos2; dmin = dist; use_pos2 = 1;}
						break;
					} // for ordering
				}
			} // for n
			if (dmin == 0.0) continue; // no path found above
			maybe_shorten_either_path(p1, (use_pos2 ? best_pt2 : p2), best_pt, keepout);
			path.add(best_pt);
			
			if (use_pos2) {
				maybe_shorten_either_path(best_pt, p2, best_pt2, keepout);
				path.add(best_pt2);
			}
			return 1; // success
		} // for npts
		if (!no_grid_path && building.is_room_backrooms(room_ix)) { // run detailed path finding on backrooms
			float const floor_spacing(building.get_window_vspace());
			unsigned const num_floors(round_fp(walk_area.dz()/floor_spacing)), floor_ix(floor((p1.z - walk_area.z1())/floor_spacing));
			assert(num_floors > 0 && floor_ix < num_floors);
			if (nav_grids.empty()) {nav_grids.resize(num_floors);} else {assert(nav_grids.size() == num_floors);}
			building_cube_nav_grid &nav_grid(nav_grids[floor_ix]);

			if (!nav_grid.is_valid()) { // build once and cache
				//highres_timer_t timer("Build Nav Grid");
				// Note: built once, so must use avoid rather than keepout; this means that our path finding will likely fail even when p1 or p2 coll is disabled
				// since walk_area is room area shrunk by the person radius, it can vary slightly depending on the size of the person to first get here
				cube_t walk_area_this_floor(walk_area);
				float const floor_zval(walk_area.z1() + floor_ix*floor_spacing + building.get_fc_thickness());
				set_cube_zvals(walk_area_this_floor, floor_zval, (floor_zval + building.get_floor_ceil_gap())); // limit to floor-ceil space for this floor
				vect_cube_t blockers;
				blockers.reserve(avoid.size());

				for (cube_t const &c : avoid) {
					if (c.intersects(walk_area_this_floor)) {blockers.push_back(c);}
				}
				// Note: doorway width is 2.38x coll radius
				float const grid_radius(get_ped_coll_radius()); // use conservative person radius so that we can reuse across people
				nav_grid.build_for_building(walk_area_this_floor, blockers, building.interior->door_stacks, building.interior->stairwells, stairs_extend, grid_radius);
				//nav_grid.create_debug_objs(building.interior->room_geom->objs); // for debugging; modifies building, which should be const
				//building.interior->room_geom->invalidate_small_geom();
			}
			if (nav_grid.find_path(p1, p2, path)) {path.uses_nav_grid = 1; return 2;}
		}
		// else, what about parking garages and retail areas?
		return 0; // failed
	}
	static point closest_room_pt(cube_t const &c, point const &pos) {
		return point(max(c.x1(), min(c.x2(), pos.x)), max(c.y1(), min(c.y2(), pos.y)), pos.z); // c.clamp_pt_xy(), but returning a new point
	}
	static cube_t calc_walkable_room_area(node_t const &node, float radius) {
		cube_t walk_area(node.bcube);
		if (!node.is_vert_conn()) {walk_area.expand_by_xy(-radius);} // shrink by radius if not stairs or a ramp

		if (node.is_hallway) {
			bool const min_dim(walk_area.dy() < walk_area.dx());
			float const shrink(min(0.5f*radius, max(0.0f, 0.9f*0.5f*walk_area.get_sz_dim(min_dim)))); // shrink by an extra half radius if hallway is wide enough
			walk_area.expand_in_dim(min_dim, -shrink);
		}
		return walk_area;
	}
	bool reconstruct_path(vector<a_star_node_state_t> const &state, vect_cube_t &avoid, building_t const &building, point const &cur_pt,
		float radius, unsigned start_ix, unsigned end_ix, unsigned ped_ix, bool is_first_path, bool up_or_down, unsigned ped_rseed,
		point const *const custom_dest, bool req_custom_dest, ai_path_t &path) const
	{
		if (req_custom_dest) {assert(custom_dest);}
		unsigned n(start_ix);
		rand_gen_t rgen;
		rgen.set_state((ped_ix + 17*start_ix + 1), (ped_rseed + 1));
		vect_cube_t keepout; // reused across rooms

		while (1) {
			node_t const &node(get_node(n));
			point const &next(state[n].path_pt);
			int const came_from(state[n].came_from_ix);
			bool const is_first_pt(path.empty());
			cube_t const walk_area(calc_walkable_room_area(node, radius));
			
			if (is_first_pt) { // last point in path (first point in reverse path)
				assert(n == start_ix);
				assert(came_from >= 0);

				if (node.is_vert_conn()) { // stairs or ramp (end point of starting floor)
					path.add(get_stairs_entrance_pt(cur_pt.z, n, up_or_down)); // likely == next
				}
				else { // room/doorway/elevator/player
					unsigned const num_tries(req_custom_dest ? 1 : 10); // only try custom_dest if req_custom_dest is set
					bool const is_lg_room(building.is_single_large_room_or_store(n));
					bool success(0);

					for (unsigned N = 0; N < num_tries; ++N) { // keep retrying until we find a point that is reachable from the room or doorway
						// choose a random new point on iterations after the first one, and on first iteration for large rooms unless we have a custom dest
						bool const no_use_init(N > 0 || (is_lg_room && !req_custom_dest));
						bool not_room_center(0);
						unsigned const avoid_size(avoid.size());
						building.add_sub_rooms_to_avoid_if_needed(building.get_room(n), avoid); // must avoid sub-rooms
						point const end_point(find_valid_room_dest(avoid, building, radius, cur_pt.z, n, up_or_down, not_room_center, rgen, no_use_init, custom_dest));
						avoid.resize(avoid_size); // remove added sub-rooms
						path.add(end_point);
						point const room_exit(closest_room_pt(walk_area, next)); // first doorway, or stairs/ramp in this room
						if (connect_room_endpoints(avoid, building, walk_area, n, end_point, room_exit, radius, path, keepout, rgen)) {path.add(room_exit); success = 1; break;}
						path.clear(); // failed, reset for next iteration
						if (!not_room_center) break; // if we did choose the room center, and there is no path to it, we've failed
					} // for n
					if (!success) {assert(path.empty()); return 0;} // failed to connect to a point in dest room
				}
			}
			else if (came_from < 0) { // done (next is not valid here)
				assert(n == end_ix);
				if (node.is_vert_conn()) return 1; // success
				point const final_pt(closest_room_pt(walk_area, path.back())); // walk from room into last doorway
				path.add(final_pt);

				// find path to first doorway; ignore collisions with p2 (cur_pt) in case this person was pushed into an object by another person
				if (!connect_room_endpoints(avoid, building, walk_area, n, final_pt, cur_pt, radius, path, keepout, rgen, 0, 1)) {
					// allow a failure if this is the first path taken by this AI so that it's not stuck behind an object due to bad initial placement,
					// but not if the node is a hallway, to avoid the AI walking through a building's stairs or elevator
					if (!is_first_path || node.is_hallway) {path.clear(); return 0;}
				}
				return 1; // success
			}
			else if (!node.is_vert_conn()) { // adjust the path through a room
				assert(!path.empty());
				point const &prev(path.back());
				assert(prev.z == next.z);
				point const p1(closest_room_pt(walk_area, prev)), p2(closest_room_pt(walk_area, next));
				//assert(p1 != p2); // does this always hold?
				path.add(p1); // walk out of doorway and into room
				
				if (!connect_room_endpoints(avoid, building, walk_area, n, p1, p2, radius, path, keepout, rgen)) { // unreachable
					path.clear();
					// try another path? this case is rare; on failure, the person will wait a second then choose a different destination room
					//disconnect_room_pair(n, came_from); // ???
					return 0;
				}
				path.add(p2); // walk from room into doorway
			}
			path.add(next); // doorway
			n = came_from;
		} // end while()
		return 0; // never gets here
	}
	bool complete_path_within_room(point const &from, point const &to, unsigned room_ix, unsigned ped_ix, float radius, unsigned ped_rseed,
		bool is_first_path, bool following_player, cube_t const &constrain_area, vect_cube_t const &avoid, building_t const &building, ai_path_t &path) const
	{
		// used for reaching a goal such as the player within the same room;
		// assumes the building shape is convex and the goal is inside the building so that the path to the goal never leaves a non-cube building
		cube_t walk_area(calc_walkable_room_area(get_node(room_ix), radius));
		if (!constrain_area.is_all_zeros()) {walk_area.intersect_with_cube_xy(constrain_area);}
		cube_t clamp_area(walk_area);
		clamp_area.expand_by_xy(-0.1*radius); // slightly smaller to allow for some FP inaccuracy
		if (clamp_area.dx() <= 0 || clamp_area.dy() <= 0.0) return 0; // area too small; shouldn't happen
		rand_gen_t rgen;
		rgen.set_state((ped_ix + 13*room_ix + 1), (ped_rseed + 1));
		vect_cube_t keepout;
		unsigned const sub_path_start(path.size());
		path.add(to); // Note: path is constructed backwards, so "to" is added first and connect_room_endpoints takes swapped arguments
		
		// ignore starting collisions, for example collisions with stairwell when exiting stairs?
		// ignore initial coll with "from", and coll with "to" when following the player;
		// this can cause zombies to walk through walls the player is standing next to, but generally only when they have a line of sight;
		// while this is wrong, it's better for gameplay than not being able to reach a player who is hiding in a corner
		if (!connect_room_endpoints(avoid, building, walk_area, room_ix, to, from, radius, path, keepout, rgen, 1, following_player)) { // ignore_p1_coll (to)=1
			if (!is_first_path) { // ignore failure on first path to allow person to get out from an object they spawn in
				bool success(0);

				if (following_player) {
					// try to find a partial path, starting at "to" and working toward "from"; since rooms are rectangular, all points on the line will be contained
					for (unsigned n = 0; n <= 9; ++n) { // {0% ... 90%}
						if (n == 0 && clamp_area.contains_pt_xy(to)) continue; // if <to> is already inside the walk area, there's no need to test it again
						point new_to(to + (float(n)/10)*(from - to));
						//if (!walk_area.contains_pt(new_to)) continue; // can fail for player hiding in a closet
						clamp_area.clamp_pt_xy(new_to);
						if (building.point_in_or_above_pool(new_to)) continue;
						
						if (connect_room_endpoints(avoid, building, walk_area, room_ix, new_to, from, radius, path, keepout, rgen, 0, 1)) { // ignore_p1_coll=0, ignore_p2_coll=1
							assert(sub_path_start < path.size());
							path[sub_path_start] = new_to; // end the path at the new destination
							path.is_shortened = 1;
							success = 1;
							break;
						}
					} // for n
				}
				if (!success) {path.clear(); return 0;}
			}
		}
		// maybe add an extra path point to prevent clipping through walls when walking through a doorway
		path.add(closest_room_pt(walk_area, from)); // add if p1 was clamped
		return 1;
	}

	bool can_use_conn(conn_room_t const &conn, building_interior_t const &interior, float zval, unsigned has_key) const {
		if (conn.door_ix < 0) return 1; // no door
		unsigned door_ix(conn.door_ix);

		if (door_ix >= interior.doors.size()) { // mall gate
			door_ix -= interior.doors.size();
			assert(interior.has_mall());
			assert(door_ix < interior.mall_info->store_doorways.size());
			return !interior.mall_info->store_doorways[door_ix].closed;
		}
		//if (global_building_params.ai_opens_doors && has_key) return 1; // locked door won't stop us; incorrect because door may not cover z-range (for multi-floor backrooms)
		door_t const &first_door(interior.doors[door_ix]);

		for (auto i = (interior.doors.begin() + door_ix); i != interior.doors.end(); ++i) {
			if (!i->is_same_stack(first_door))        break;    // we've reached the end of the vertical stack of doors
			//if (i->mult_floor_room && zval > i->z2()) return 0; // no door at this level - can't pass; but then if the AI gets here they can't get out
			if (zval < i->z1() || zval > i->z2())     continue; // not the correct floor
			return (i->open || (global_building_params.ai_opens_doors && i->check_key_mask_unlocks(has_key)));
		}
		return 1; // Note: we can get here for complex floorplan office buildings with bad interior walls (-4.18, 4.28, -3.46)
	}
	
	// A* algorithm; Note: path is stored backwards
	bool find_path_points(unsigned room1, unsigned room2, unsigned ped_ix, float radius, bool use_stairs, bool is_first_path,
		bool up_or_down, unsigned ped_rseed, vect_cube_t &avoid, building_t const &building, point const &cur_pt,
		unsigned has_key, point const *const custom_dest, bool req_custom_dest, ai_path_t &path) const
	{
		assert(!invalid);
		// Note: opening and closing doors updates the nav graph; an AI encountering a closed door after choosing a path can either open it or stop and wait
		assert(room1 < nodes.size() && room2 < nodes.size());
		assert(room1 != room2); // but one can be the stairs in the same room as the other
		path.clear();
		vector<a_star_node_state_t> state(nodes.size());
		vector<uint8_t> open(nodes.size(), 0), closed(nodes.size(), 0); // tentative/already evaluated nodes
		std::priority_queue<pair<float, unsigned> > open_queue;
		point dest_pos;
		if (custom_dest) {dest_pos = *custom_dest;}
		else {dest_pos = get_node(room2).get_center(cur_pt.z);} // Note: approximate, actual dest may be different
		a_star_node_state_t &start(state[room1]);
		start.g_score = 0.0;
		start.f_score = p2p_dist_xy(cur_pt, dest_pos); // estimated total cost from start to goal through current
		start.path_pt = cur_pt;
		open[room1]   = 1;
		open_queue.push(make_pair(-start.f_score, room1));

		while (!open_queue.empty()) {
			unsigned const cur(open_queue.top().second);
			open_queue.pop();
			node_t const &cur_node(get_node(cur));
			closed[cur] = 1;
			open  [cur] = 0;
			bool reached_goal(0);

			for (auto i = cur_node.conn_rooms.begin(); i != cur_node.conn_rooms.end(); ++i) {
				assert(i->ix < nodes.size());
				if (closed[i->ix]) continue; // already closed (duplicate)
				bool const is_goal(i->ix == room2);
				node_t const &conn_node(get_node(i->ix));
				if (conn_node.is_vert_conn() && !use_stairs && !is_goal)      continue; // skip stairs/ramp in this mode
				if (!can_use_conn(*i, *building.interior, cur_pt.z, has_key)) continue; // blocked by closed or locked door; must do this check before setting open state
				point const node_pt((cur == room1) ? cur_pt   : cur_node .get_center(cur_pt.z));
				point const next_pt(is_goal        ? dest_pos : conn_node.get_center(cur_pt.z));
				a_star_node_state_t &sn(state[i->ix]);
				vector2d const &pt(i->pt[up_or_down]); // point in doorway, etc.
				float const dist_through_cur(state[cur].g_score + p2p_dist_xy(node_pt, pt) + p2p_dist_xy(pt, next_pt));
				float new_g_score(dist_through_cur);
				// in the case of long hallways, the shortest path may pass between doors/adjacencies rather than through the center of the room/node,
				// so we take that into account here by calculating the straight line path between room entrance and exit without going through the center
				point const &prev_edge_pt(state[cur].path_pt); // point in doorway, etc.
				float const dist_without_last_seg(state[cur].g_score - p2p_dist_xy(node_pt, prev_edge_pt));
				float const dist_door_to_door(dist_without_last_seg + p2p_dist_xy(pt, prev_edge_pt));
				min_eq(new_g_score, dist_door_to_door); // dist_door_to_door is always smaller?
				if (!open[i->ix]) {open[i->ix] = 1;}
				else if (new_g_score >= sn.g_score) continue; // not better
				sn.came_from_ix = cur;
				sn.path_pt.assign(pt.x, pt.y, cur_pt.z);
				reached_goal |= is_goal; // Note: can't call reconstruct_path() until the iteration is done in case there's a second door that's closer
				sn.g_score = new_g_score;
				sn.f_score = sn.g_score + p2p_dist_xy(next_pt, dest_pos);
				open_queue.push(make_pair(-sn.f_score, i->ix));
			} // for i
			if (reached_goal) { // done, reconstruct path (in reverse)
				return reconstruct_path(state, avoid, building, cur_pt, radius, room2, room1, ped_ix, is_first_path, up_or_down, ped_rseed, custom_dest, req_custom_dest, path);
			}
		} // end while()
		return 0; // failed - no path from room1 to room2
	}
}; // end building_nav_graph_t

cube_t building_t::get_walkable_room_bounds(room_t const &room, bool floor_space_only) const {
	if (floor_space_only && room.is_industrial() && interior->ind_info) return interior->ind_info->floor_space; // currently unused
	// regular house rooms start and end at the walls;
	// offices, hallways, and extended basement rooms tile exactly and include half the walls, so we have to subtract those back off
	if (!room.inc_half_walls()) return room;
	cube_t c(room);
	float const half_wall_thick(0.5*get_wall_thickness());

	if ((room.is_hallway && !room.is_ext_basement()) || room.get_office_floorplan()) { // office building room; only shrink interior walls
		cube_t const &part(get_part_for_room(room));
		float min_shrink(0.0);
		if (is_prison() && room.z1() >= ground_floor_z1) {min_shrink = (get_window_trim_thick() - get_trim_thickness());} // prison needs extra padding for window bars

		for (unsigned d = 0; d < 2; ++d) {
			c.d[d][0] += ((room.d[d][0] > part.d[d][0]) ? half_wall_thick : min_shrink);
			c.d[d][1] -= ((room.d[d][1] < part.d[d][1]) ? half_wall_thick : min_shrink);
		}
	}
	else {c.expand_by_xy(-half_wall_thick);} // shrink on all sides; not exact, but we don't know which walls are interior vs. exterior
	return c;
}
cube_t building_t::get_room_bounds_inside_trim(room_t const &room) const {
	cube_t bounds(get_walkable_room_bounds(room));
	bounds.expand_by_xy(-get_trim_thickness());
	return bounds;
}

void building_t::build_nav_graph() const { // Note: does not depend on room geom

	assert(interior);
	if (interior->nav_graph && !interior->nav_graph->invalid) return; // already built
	// Note: reallocating the nav_graph will also rebuild the nested nav_grid
	interior->nav_graph.reset(new building_nav_graph_t(DOOR_WIDTH_SCALE*get_window_vspace())); // set stairs_extend == doorway width
	building_nav_graph_t &ng(*interior->nav_graph);
	float const wall_width(get_wall_thickness()), doorway_width(get_doorway_width());
	unsigned const num_rooms(interior->rooms.size()), num_stairs(interior->stairwells.size());
	bool const has_ramp(has_pg_ramp()), add_mall_ent_stairs(has_mall_ent_stairs());
	ng.set_num_rooms(num_rooms, num_stairs, has_ramp, add_mall_ent_stairs);
	for (unsigned s = 0; s < num_stairs; ++s) {ng.set_stairs_bcube(s, interior->stairwells[s]);}
	if (add_mall_ent_stairs) {ng.set_stairs_bcube(num_stairs, interior->mall_info->ent_stairs);}
	if (has_ramp) {ng.set_ramp_bcube(interior->pg_ramp);}

	for (unsigned r = 0; r < num_rooms; ++r) {
		room_t const &room(interior->rooms[r]);
		cube_t c(get_walkable_room_bounds(room));
		if (room.is_hallway || room.is_mall()) {ng.mark_hallway(r);} // mall concourse works like a hallway
		ng.set_room_bcube(r, c);
		c.expand_by_xy(wall_width); // to include adjacent doors
		if (is_room_adjacent_to_ext_door(c)) {ng.mark_exit(r);}

		for (door_stack_t const &ds : interior->door_stacks) {
			if (ds.not_a_room_separator())   continue; // only using doors between two rooms
			if (!ds.is_connected_to_room(r)) continue; // door not connected to this room
			// we should only add this door if it's on the same floor as our graph, but that doesn't work because the graph is shared across all floors,
			// so instead we'll have to record the door index and check the correct door during path finding; it's not valid to test door open/locked state here
			unsigned const r2(ds.get_conn_room(r));
			if (r2 > r) {ng.connect_rooms(r, r2, ds.first_door_ix, ds);} // check rooms with higher index (since graph is bidirectional)
		} // for d
		for (unsigned s = 0; s < num_stairs; ++s) { // stairs
			stairwell_t const &stairwell(interior->stairwells[s]);

			if (stairwell.stairs_door_ix >= 0 && global_building_params.ai_opens_doors < 2) { // check for open doors; doors on stairs can't be locked
				if (!get_door(stairwell.stairs_door_ix).open) continue; // stairs blocked by closed door, don't connect (even if unlocked)
			}
			// allow adjacency only for mall back hallway stairs, since they don't intersect the room
			if ((stairwell.in_mall == 2) ? room.intersects(stairwell) : room.intersects_no_adj(stairwell)) {ng.connect_stairs(r, s, stairwell, doorway_width);}
		}
		if (has_mall()) {
			bool const is_mall_concourse((int)r == interior->ext_basement_hallway_room_id);

			if (is_mall_concourse) { // bidirectional; only need to check mall => store/parking garage direction
				for (auto i = interior->mall_info->store_doorways.begin(); i != interior->mall_info->store_doorways.end(); ++i) {
					store_doorway_t const &d(*i);
					unsigned const dix(i - interior->mall_info->store_doorways.begin() + interior->doors.size()); // offset by the number of doors
					if (!d.locked_closed()) {ng.connect_rooms(r, d.room_id, dix, d);}
				}
			}
			if (add_mall_ent_stairs && (is_mall_concourse || room.is_parking())) { // handle mall entrance stairs
				ng.connect_stairs(r, num_stairs, interior->mall_info->ent_stairs, doorway_width);
			}
		}
		if (room.open_wall_mask || (has_complex_floorplan && !room.is_ext_basement())) { // check for connected hallways in office buildings, or open wall rooms
			for (unsigned r2 = r+1; r2 < num_rooms; ++r2) { // check rooms with higher index (since graph is bidirectional)
				room_t const &room2(interior->rooms[r2]);
				if (!room2.open_wall_mask || room2.z1() != room.z1() || !room2.intersects(c)) continue; // not a connected hallway
				cube_t conn_cube(c);
				conn_cube.intersect_with_cube(room2);
				ng.connect_rooms(r, r2, -1, conn_cube);
			}
		}
		if ((room.is_parking() || is_parking()) && has_ramp && room.intersects_no_adj(interior->pg_ramp)) { // include parking garage ramp
			bool const dim(interior->pg_ramp.ix >> 1), dir(interior->pg_ramp.ix & 1);
			ng.connect_ramp(r, dim, dir);
		}
		//for (unsigned e = 0; e < interior->elevators.size(); ++e) {} // elevators are ignored here by the AI; should check interior->elevators_disabled
	} // for r
	//if (is_house && !has_sec_bldg() && has_basement() && !ng.is_fully_connected()) {cout << "bcube " << bcube.str() << endl;}
}

void building_t::invalidate_nav_graph() { // Note: this is safe to call in one thread while using in another
	if (interior && interior->nav_graph) {interior->nav_graph->invalid = 1;}
}
void building_t::invalidate_nav_grid(unsigned floor_ix) { // Note: this is safe to call in one thread while using in another
	if (interior && interior->nav_graph) {interior->nav_graph->invalidate_nav_grid(floor_ix);}
}

unsigned building_t::count_connected_room_components() {
	if (!interior) return 0;
	build_nav_graph();
	unsigned const num(interior->nav_graph->count_connected_components());
	interior->nav_graph.reset(); // no longer needed
	return num;
}

#define VISIT_CONN_ROOM(r) \
if (use_bit_mask ? (seen_mask & (1ULL << r)) : seen[r]) continue; \
if ((unsigned)r == room2) return 1; /*found, done*/ \
pend.push_back(r); \
if (use_bit_mask) {seen_mask |= (1ULL << r);} else {seen[r] = 1;}

// Note: this is somewhat slow, should we build and use the nav graph? maybe not since this is only called for room assiggnment on the first floor of houses
// Note: pass in room_exclude=num_rooms for no excluded room
// Note: door_exclude is optional; -1 disables this check; door_ex_zval must be specified when door_exclude >= 0
bool building_t::are_rooms_connected_without_using_room_or_door(unsigned room1, unsigned room2, unsigned room_exclude, int door_exclude, float door_ex_zval) const {
	unsigned const num_rooms(interior->rooms.size());
	assert(room1 < num_rooms && room2 < num_rooms);
	assert(room_exclude != room1 && room_exclude != room2);
	if (room1 == room2) return 1;
	bool const use_bit_mask(num_rooms <= 64); // almost always true, except for buildings with malls
	static vector<unsigned> pend; // reused across calls
	static vector<uint8_t> seen; // reused across calls
	uint64_t seen_mask(0);
	pend.clear();
	pend.push_back(room1);

	if (use_bit_mask) {
		seen_mask |= (1ULL << room1);
		if (room_exclude < num_rooms) {seen_mask |= (1ULL << room_exclude);}
	}
	else { // must use seen vector
		seen.clear();
		seen.resize(num_rooms, 0);
		seen[room1] = 1;
		if (room_exclude < num_rooms) {seen[room_exclude] = 1;} // mark as seen so that we won't visit it
	}
	while (!pend.empty()) { // flood fill - maybe A* is better, but that's a lot of work
		unsigned const cur(pend.back());
		pend.pop_back();

		for (door_stack_t const &ds : interior->door_stacks) {
			if ( ds.not_a_room_separator())    continue; // not a real door between two rooms
			if (!ds.is_connected_to_room(cur)) continue;
			
			if (door_exclude >= 0 && door_exclude >= (int)ds.first_door_ix) { // check if this door is excluded
				bool is_excluded(0);
				assert(ds.first_door_ix < interior->doors.size());

				for (unsigned dix = ds.first_door_ix; dix < interior->doors.size(); ++dix) {
					door_t const &door(interior->doors[dix]);
					if (!ds.is_same_stack(door)) break; // moved to a different stack, done
					if ((int)dix == door_exclude && door.z1() < door_ex_zval && door.z2() > door_ex_zval) {is_excluded = 1; break;}
				}
				if (is_excluded) continue;
			}
			VISIT_CONN_ROOM(ds.get_conn_room(cur));
		} // for d
		if (has_mall() && get_room(cur).is_mall_or_store()) {
			int const mall_room_id(interior->ext_basement_hallway_room_id);
			assert(mall_room_id >= 0);

			for (store_doorway_t const &d : interior->mall_info->store_doorways) {
				if (d.closed) continue;
				if ((int)cur == mall_room_id) {VISIT_CONN_ROOM(d.room_id   );} // mall => store
				else if (d.room_id == cur)    {VISIT_CONN_ROOM(mall_room_id);} // store => mall
			}
		}
	} // end while()
	return 0; // room2 not found
}
// returns true if there is not path between the rooms connected to this room that don't go through this room
bool building_t::is_room_on_critical_path(unsigned room_id, float zval) const {
	vect_door_stack_t const &doorways(get_doorways_for_room(get_room(room_id), zval));
	if (doorways.size() <= 1) return 0;
	vector<unsigned> conn_rooms;
	for (door_stack_t const &ds : doorways) {conn_rooms.push_back(ds.get_conn_room(room_id));}
	unique_cont(conn_rooms); // should already be unique?

	for (auto i = conn_rooms.begin(); i != conn_rooms.end(); ++i) {
		for (auto j = i+1; j != conn_rooms.end(); ++j) {
			if (!are_rooms_connected_without_using_room_or_door(*i, *j, room_id)) return 1;
		}
	}
	return 0;
}

// Warning: this may be called from a different thread from the one that uses it for AI updates
void building_t::register_player_in_building(point const &camera_bs, unsigned building_id) const {
	bool const prev_was_valid(cur_player_building_loc.is_valid());
	unsigned const old_floor_ix(cur_player_building_loc.floor_ix);
	if (animate2) {prev_player_building_loc = cur_player_building_loc;} // only update previous pos when AI is running so that it doesn's miss a floor or room change
	cur_player_building_loc = building_dest_t(get_building_loc_for_pt(get_inv_rot_pos(camera_bs)), camera_bs, building_id);
	unsigned const new_floor_ix(cur_player_building_loc.floor_ix);
	if (prev_was_valid && old_floor_ix != new_floor_ix) {register_player_change_floor(old_floor_ix, new_floor_ix);}
}
void register_player_not_in_building() {
	prev_player_building_loc = cur_player_building_loc = building_dest_t();
}

bool is_ai_coll_obj(room_object_t const &c, bool same_as_player) {
	// these object types are not collided with by people and can be skipped
	if (c.no_coll() || c.is_dynamic() || c.type == TYPE_LG_BALL) return 0; // skip dynamic objects (balls, etc.)
	if (c.type == TYPE_POOL_TILE) return 0; // pools are handled with custom code that works different from the player
	if (c.type == TYPE_RAILING  ) return (!c.is_exterior() && (c.flags & RO_FLAG_TOS)); // railings at top of stairs and escalator are always AI colliders
	if (!(same_as_player ? c.is_player_collidable() : bldg_obj_types[c.type].ai_coll)) return 0;
	return 1;
}

bool move_sphere_to_not_int_cube_xy(point &pos, float radius, cube_t const &c) {
	if (!sphere_cube_intersect_xy(pos, radius, c)) return 0;
	float closest_dist(-1.0);
	point new_pos(pos);

	for (unsigned dim = 0; dim < 2; ++dim) {
		for (unsigned dir = 0; dir < 2; ++dir) {
			point cand(pos);
			cand[dim] = c.d[dim][dir] + (dir ? 1.0 : -1.0)*radius; // move away from this edge
			float const dist(fabs(pos[dim] - cand[dim]));
			if (closest_dist < 0.0 || dist < closest_dist) {closest_dist = dist; new_pos = cand;} // update if closer
		}
	}
	bool const ret(pos != new_pos);
	pos = new_pos; // new_pos is the point not intersecting the cube which requires the least movement
	return ret;
}

bool building_t::choose_dest_goal(person_t &person, rand_gen_t &rgen) const { // used for following the player in gameplay mode

	assert(interior && interior->nav_graph);
	building_loc_t const loc(get_building_loc_for_pt(person.pos));
	if (loc.room_ix < 0) return 0; // not in a room
	float const window_vspace(get_window_vspace());
	building_dest_t goal;
	point sound_pos;

	if ((global_building_params.ai_target_player || cur_player_building_loc.same_room_floor(loc)) && can_target_player(person)) {
		if (courtyard_door_ix >= 0 && interior->room_geom->courtyard.room_ix >= 0 && point_in_courtyard(cur_player_building_loc.pos)) {
			// player in courtyard; currently never gets here because we can't target the player in this case
			select_person_dest_in_room(person, rgen, get_room(interior->room_geom->courtyard.room_ix)); // target room connected to courtyard
		}
		else {
			goal = cur_player_building_loc; // player is in a different room of our building, or we're following the player's position
			person.goal_type = GOAL_TYPE_PLAYER;
		}
	}
	else if (can_ai_follow_player(person) && get_closest_building_sound(person.pos, sound_pos, window_vspace)) { // target the loudest sound
		if (point_in_or_above_pool(sound_pos) && get_pool_room().contains_pt(person.pos)) {
			// ignore pool splash if already in the pool room; maybe should target a nearby location on the side of the pool?
		}
		else {
			// check if the sound is coming from somewhere unreachable such as a closet the player is hiding inside, then move it out into the center of the room;
			// maybe this location is also unreachable, but at least it will get the zombie into the correct room instead of have them wait at the wall in an adj room
			assert(has_room_geom());
			auto objs_end(interior->room_geom->get_placed_objs_end()); // skip buttons/stairs/elevators

			for (auto c = interior->room_geom->objs.begin(); c != objs_end; ++c) {
				if (!is_ai_coll_obj(*c, 1))     continue; // same_as_player=1; affects objects such as chairs
				if (!c->contains_pt(sound_pos)) continue;
				int const room_ix(get_room_containing_pt(sound_pos));
				if (room_ix < 0) continue; // error?
				cube_t const &room(get_room(room_ix));
				sound_pos.x = room.xc();
				sound_pos.y = room.yc();
			} // for c
			goal = building_dest_t(get_building_loc_for_pt(sound_pos), sound_pos, cur_player_building_loc.building_ix); // same building as player (current building)
			person.goal_type = GOAL_TYPE_SOUND;
		}
	}
	if (!goal.is_valid()) return 0; // no player or sound
	unsigned const cand_room(goal.room_ix);
	room_t const &room(get_room(cand_room)); // target room
	if (!interior->nav_graph->is_room_connected_to(loc.room_ix, cand_room, *interior, person.pos.z, person.has_key)) return 0; // unreachable
	person.cur_room     = loc.room_ix;
	person.dest_room    = cand_room; // set but not yet used
	person.target_pos   = (global_building_params.ai_target_player ? goal.pos: get_center_of_room(cand_room));
	person.target_pos.z = person.pos.z; // keep orig zval to stay on the same floor
	person.is_on_stairs = person.path_is_fixed = person.last_used_stairs = person.missed_player_room_change = 0; // non-stairs dest, reset

	if (global_building_params.ai_target_player && point_over_glass_floor(person.pos) && point_over_glass_floor(person.target_pos)) {
		return 1; // person/zombie and player both on glass floor
	}
	// assume current and dest room are both in the mall and we're using mall concourse stairs rather than back hallway stairs
	float const floor_spacing(point_in_mall(person.pos) ? get_mall_floor_spacing() : window_vspace);
	// allow moving to a different floor, currently only one floor at a time
	// note that if we're following a sound this may lead us to the wrong floor of the room if more than one floor above or below;
	// the current system doesn't allow for paths that span more than two floors;
	// however, this will get us on a floor closer to the player, and hopefully there will be new sounds to guide us from there
	if (goal.floor_ix < loc.floor_ix) { // try one floor below
		float const new_z(person.target_pos.z - floor_spacing);
		if (new_z > room.z1()) {person.target_pos.z = new_z;} // change if there is a floor below
	}
	else if (goal.floor_ix > loc.floor_ix) { // try one floor above
		float const new_z(person.target_pos.z + floor_spacing);
		if (new_z < room.z2()) {person.target_pos.z = new_z;} // change if there is a floor above
	}
	// else if floors differ by more than 1, we'll end up visiting the room on the wrong floor
	
	if (global_building_params.ai_target_player) { // ensure target is a valid location in this building; this must be done *after* adjacent floor zval adjustment
		// handle case where player is standing on stairs on same floor by moving zval to a different floor to force this person to use stairs; what about pg_ramp?
		if (person.goal_type == GOAL_TYPE_PLAYER && loc.floor_ix == goal.floor_ix && goal.stairs_ix >= 0) {
			float const person_z1(person.get_z1()), player_z1(cur_player_building_loc.pos.z - get_bldg_player_height()), fc_thick(get_fc_thickness());
			// make destination exactly one floor above or below of where we currently are; some hysteresis is required to handle the case where the player is at the same zval
			if      (player_z1 + fc_thick < person_z1) {person.target_pos.z -= floor_spacing;} // move down one floor
			else if (player_z1 - fc_thick > person_z1) {person.target_pos.z += floor_spacing;} // move up   one floor
		}
		float const z2_add(person.get_height() - person.radius), coll_dist(COLL_RADIUS_SCALE*person.radius);
		cube_t ai_bcube;
		if (has_basement() && person.target_pos.z < ground_floor_z1) {ai_bcube = get_full_basement_bcube();} // in the basement
		else {ai_bcube = bcube;} // above ground
		cube_t legal_area(ai_bcube);
		legal_area.expand_by_xy(-coll_dist);
		legal_area.z1() += person.radius;
		legal_area.z2() -= z2_add;
		assert(legal_area.is_strictly_normalized());
		legal_area.clamp_pt(person.target_pos); // clamp to building interior
		// if target_pos is outside any building parts, target the nearest part
		bool contained(0);
		float dmin_sq(0.0);
		cube_t closest_part;

		for (auto p = parts.begin(); p != get_real_parts_end_inc_sec(); ++p) { // there shouldn't be any people in secondary buildings, but include them anyway
			if (p->contains_pt(person.target_pos)) {contained = 1; closest_part = *p; break;} // done
			float const dsq(p2p_dist_sq(person.target_pos, p->closest_pt(person.target_pos))); // what about basements? should we check zvals?
			if (dmin_sq == 0.0 || dsq < dmin_sq) {dmin_sq = dsq; closest_part = *p;}
		}
		if (!contained && person.target_pos.z < ground_floor_z1 && has_ext_basement()) { // check extended basement
			float const dsq(p2p_dist_sq(person.target_pos, interior->basement_ext_bcube.closest_pt(person.target_pos)));
			if (dsq < dmin_sq) {closest_part = interior->basement_ext_bcube;}
		}
		if (!contained && !closest_part.is_all_zeros()) {closest_part.clamp_pt(person.target_pos);} // clamp to closest part
		vect_cube_t &avoid(reused_avoid_cubes[0]);
		float const z1(person.target_pos.z - person.radius), z2(person.target_pos.z + z2_add), fc_gap(get_floor_ceil_gap());
		interior->get_avoid_cubes(avoid, z1, z2, 0.5*person.radius, get_floor_thickness(), fc_gap, 1, 1); // same_as_player=1, skip_stairs=1
		
		if (has_courtyard && has_room_geom() && z1 >= ground_floor_z1 && z1 < (ground_floor_z1 + fc_gap)) {
			avoid.push_back(interior->room_geom->courtyard); // don't clip through the courtyard door trying to follow the player
		}
		// check for initial collisions at the player's location, but exclude stairs in case the player is standing on them;
		// this may no longer be required since complete_path_within_room() now ignores initial collisions with the dest, but is likely still a good idea
		for (unsigned n = 0; n < 4; ++n) { // iterate a few times in case a collision moves pos into another object
			bool any_updated(0);

			for (auto i = avoid.begin(); i != avoid.end(); ++i) { // move target_pos to avoid room objects
				any_updated |= move_sphere_to_not_int_cube_xy(person.target_pos, 1.2*coll_dist, *i); // add an extra 20% buffer
			}
			if (!any_updated) break; // done
		} // for n
		legal_area.clamp_pt(person.target_pos); // clamp to building interior again
	}
	return 1;
}

bool building_t::select_person_dest_in_room(person_t &person, rand_gen_t &rgen, room_t const &room) const {
	float const height(person.get_height()), radius(COLL_RADIUS_SCALE*person.radius);
	cube_t valid_area(room);

	// if on a glass floor and dest isn't the floor below, clip valid area to bounds of glass floor, assuming it makes a cube; railing coll should handle non-cube case
	if (room.is_retail() && point_over_glass_floor(person.pos)) {
		cube_t const glass_floor_bcube(get_bcubes_union(interior->room_geom->glass_floors));
		
		if (person.target_pos.z > glass_floor_bcube.z2()) {
			valid_area.intersect_with_cube_xy(glass_floor_bcube);
			if (!valid_area.is_strictly_normalized()) return 0; // shouldn't happen
		}
	}
	point dest_pos(valid_area.get_cube_center());
	vect_cube_t &avoid(reused_avoid_cubes[0]);
	get_avoid_cubes(person.target_pos.z, height, radius, avoid, 0); // following_player=0
	add_sub_rooms_to_avoid_if_needed(room, avoid); // must avoid sub-rooms
	bool const no_use_init(room.is_single_large_room_or_store()); // don't use the room center for a parking garage, backrooms, retail area, mall, or store
	if (!interior->nav_graph->find_valid_pt_in_room(avoid, *this, radius, person.target_pos.z, valid_area, rgen, dest_pos, no_use_init)) return 0;
	
	if (!is_cube()) { // non-cube building
		cube_t const person_bcube(person.get_bcube() + (dest_pos - person.pos)); // bcube, but translated to dest_pos
		if (!check_cube_within_part_sides(person_bcube)) return 0; // outside building - will try again with a new pos next time
	}
	person.target_pos.x = dest_pos.x;
	person.target_pos.y = dest_pos.y;
	person.goal_type    = GOAL_TYPE_ROOM;
	return 1;
}

point get_pos_to_stand_for_elevator(elevator_t const &e, float floor_spacing, float zval) {
	point pos;
	pos[!e.dim] = e.get_center_dim(!e.dim); // centered on the elevator
	pos[ e.dim] = e.d[e.dim][e.dir] + (e.dir ? 1.0 : -1.0)*ELEVATOR_STAND_DIST*floor_spacing; // stand in front of the elevator door
	pos.z = zval;
	return pos;
}

escalator_t const &building_t::get_escalator(unsigned ix) const {
	assert(interior);
	assert(ix < interior->escalators.size());
	return interior->escalators[ix];
}
void building_t::add_escalator_points(person_t const &person, ai_path_t &path) const {
	escalator_t const& e(get_escalator(person.cur_escalator));
	bool const going_up(e.is_going_up());
	cube_t ends[2];
	e.get_ends_bcube(ends[0], ends[1], 0); // exclude_sides=0
	cube_t const &enter_end(ends[!going_up]), &exit_end(ends[going_up]);
	bool const dim(e.dim), move_dir(enter_end.d[dim][0] < exit_end.d[dim][0]);
	float const floor_thick(e.get_floor_thick()); // small value added to zval when on escalator
	float step_len(e.get_ramp_bcube(0).get_sz_dim(dim)/NUM_STAIRS_PER_FLOOR_ESC);
	if (e.in_mall) {step_len *= get_window_vspace()/get_mall_floor_spacing();} // more steps in mall escalator => smaller step len
	float const move_radius((move_dir ? 1.0 : -1.0)*person.radius); // one radius in the direction of movement
	float const step_lead(1.5*((going_up ^ move_dir) ? 1.0 : -1.0)*step_len); // or 0.5*person.radius?
	point enter_pt, step_beg, step_end, exit_pt;
	enter_pt[!dim] = step_beg[!dim] = step_end[!dim] = exit_pt[!dim] = e.get_center_dim(!dim); // centerline
	enter_pt.z  = step_beg.z = /*enter_end.z1() + person.radius*/person.pos.z; // should be the same zval within FP error
	step_end.z  = exit_pt .z = exit_end.z1() + person.radius;
	step_beg.z += floor_thick; step_end.z += floor_thick; // makes the entrance and exit points sloped so that person.on_stairs=1; is this still needed?
	enter_pt[dim] = enter_end.d[dim][!move_dir] - move_radius; // in front of entrance
	step_beg[dim] = enter_end.d[dim][ move_dir] + step_lead; // just before first step
	step_end[dim] = exit_end .d[dim][!move_dir] + step_lead; // on last step
	exit_pt [dim] = exit_end .d[dim][ move_dir] + move_radius; // at exit
	// add to path backwards
	path.add(exit_pt,  0);
	path.add(step_end, 1);
	path.add(step_beg, 1);
	path.add(enter_pt, 1);
}

int building_t::maybe_use_escalator(person_t &person, building_loc_t const &loc, bool last_used_escalator, rand_gen_t &rgen) const { // for retail area, not malls

	if (!has_tall_retail() || interior->escalators.empty() || !has_glass_floor()) return 0;
	if (loc.room_ix < 0) return 0;
	bool const target_player(global_building_params.ai_target_player && can_ai_follow_player(person) && loc.room_ix == prev_player_building_loc.room_ix);
	if (!target_player && rgen.rand_float() > 0.35) return 0;
	room_t const &room(get_room(loc.room_ix));
	if (!room.is_retail()) return 0;
	bool const on_upper_floor(point_over_glass_floor(person.pos));
	if (!on_upper_floor && person.pos.z > room.z1() + get_window_vspace()) return 0; // on neither upper nor lower floor; on stairs landing or escalator?
	if (!on_upper_floor && last_used_escalator && !target_player) return 0; // don't use escalator on bottom floor if we just came down unless following the player
	point const &ppos(cur_player_building_loc.pos);
	vect_cube_t const &glass_floors(interior->room_geom->glass_floors);

	if (target_player) {
		bool const player_on_upper_floor(point_over_glass_floor(ppos));
		if (!on_upper_floor && !player_on_upper_floor) return 0; // player not on glass/upper floor, so don't go there
		
		if (on_upper_floor && player_on_upper_floor) { // person/zombie and player both on glass floor
			person.dest_room = loc.room_ix; // same as current room
			person.goal_type = GOAL_TYPE_PLAYER;
			person.target_pos.assign(ppos.x, ppos.y, person.pos.z);
			return 2; // not using escalator, but have a goal
		}
	}
	for (auto e = interior->escalators.begin(); e != interior->escalators.end(); ++e) {
		if (e->in_mall)                         continue; // mall escalators don't count
		if (!room.contains_cube(*e))            continue; // escalator in wrong room - shouldn't happen?
		if (e->is_going_up() == on_upper_floor) continue; // wrong direction
		bool const dim(e->dim), dir(e->dir);

		if (on_upper_floor) { // dest is lower floor
			person.target_pos.z = e->z1() + person.radius; // on ground floor of retail
			assert(person.target_pos.z < person.pos.z);
			
			if (target_player) { // the player may not be reachable, so target a point at the bottom of the escalator
				//person.target_pos.x = ppos.x; person.target_pos.y = ppos.y; // player pos
				person.target_pos[!dim] = e->get_center_dim(!dim); // centerline
				person.target_pos[ dim] = e->d[dim][!dir] + (dir ? -1.0 : 1.0)*1.1*person.radius; // slightly in front of exit
			}
			else if (!select_person_dest_in_room(person, rgen, get_room(loc.room_ix))) continue;
		}
		else { // dest is upper floor
			if (target_player) {
				unsigned escalator_floor(0);

				for (unsigned i = 0; i < glass_floors.size(); ++i) { // find floor associated with this escalator; if non, will return the first one
					if (glass_floors[i].intersects(*e)) {escalator_floor = i; break;}
				}
				person.target_pos.z     = glass_floors[escalator_floor].z2() + person.radius;
				person.target_pos[!dim] = e->get_center_dim(!dim); // centerline
				person.target_pos[ dim] = e->d[dim][dir] + (dir ? 1.0 : -1.0)*1.1*person.radius; // slightly in front of exit
			}
			else { // target a random pos on a random glass floor; assumes all glass floors are connected and at the same zval
				cube_t const &dest_floor(glass_floors[rgen.rand() % glass_floors.size()]);
				float const zval(dest_floor.z2() + person.radius), height(person.get_height()), radius(COLL_RADIUS_SCALE*person.radius);
				person.target_pos.assign(dest_floor.xc(), dest_floor.yc(), zval); // init dest is center
				vect_cube_t &avoid(reused_avoid_cubes[0]);
				get_avoid_cubes(person.target_pos.z, height, radius, avoid, 0); // following_player=0
				if (!interior->nav_graph->find_valid_pt_in_room(avoid, *this, radius, zval, dest_floor, rgen, person.target_pos, 1)) continue; // no_use_init=1
			}
			//assert(person.target_pos.z > person.pos.z);
			if (person.target_pos.z <= person.pos.z) {register_debug_event(person.target_pos, "invalid target_pos");}
		}
		person.dest_room = loc.room_ix; // same as current room
		person.goal_type = GOAL_TYPE_ESCALATOR;
		person.cur_escalator    = (e - interior->escalators.begin());
		person.last_used_stairs = 0; // non-stairs dest, reset
		return 1;
	} // for e
	return 0;
}

int building_t::choose_dest_room(person_t &person, rand_gen_t &rgen) const { // used for randomly walking around the building

	assert(interior && interior->nav_graph);
	building_loc_t const loc(get_building_loc_for_pt(person.pos));
	if (loc.room_ix < 0) return 0; // not in a room
	person.dest_room    = person.cur_room = loc.room_ix; // set to the same room and pos just in case
	person.target_pos   = person.pos; // set but not yet used
	person.is_on_stairs = person.path_is_fixed = person.missed_player_room_change = 0;
	if (interior->rooms.size() == 1) return 0; // no other room to move to
	bool const last_used_escalator(person.last_used_escalator);
	person.last_used_escalator = 0;
	
	// maybe use retail area escalator
	if (maybe_use_escalator(person, loc, last_used_escalator, rgen)) return 1;
	float const window_vspace(get_window_vspace());

	// maybe choose a destination elevator (for office buildings) if our prev dest wasn't an elevator
	if (!person.is_first_path && !interior->elevators.empty() && !interior->elevators_disabled && !person.last_changed_floor() &&
		person.goal_type != GOAL_TYPE_ELEVATOR && global_building_params.elevator_capacity > 0)
	{
		if (rgen.rand_probability(global_building_params.use_elevator_prob)) {
			int const nearest_elevator(find_nearest_elevator_this_floor(person.pos));

			if (nearest_elevator >= 0) {
				elevator_t const &e(get_elevator(nearest_elevator));
				point const pos_to_stand(get_pos_to_stand_for_elevator(e, window_vspace, person.pos.z));
				int elevator_room(get_room_containing_pt(point(e.xc(), e.yc(), person.pos.z))); // room containing elevator center at person zval
				// if elevator is outside all rooms, choose the point where this person would stand; needed for mall back hallway elevators; should we always do this?
				if (elevator_room < 0) {elevator_room = get_room_containing_pt(pos_to_stand);}
				assert(elevator_room >= 0); // elevator must be in a valid room

				// Note: to simplify multiple AI logic, we could check e.in_use here and not even go to the elevator if someone else is using it
				if (interior->nav_graph->is_room_connected_to(loc.room_ix, elevator_room, *interior, person.pos.z, person.has_key)) { // check if reachable
					person.target_pos   = pos_to_stand; // check if this is blocked, or leave that up to path finding?
					person.dest_room    = elevator_room;
					person.goal_type    = GOAL_TYPE_ELEVATOR;
					person.cur_elevator = (uint8_t)nearest_elevator;
					person.last_used_elevator = 1;
					person.last_used_stairs   = 0; // non-stairs dest, reset
					return 1;
				}
			}
		}
	}
	room_t const &cur_room(get_room(loc.room_ix));
	bool const single_large_room(cur_room.is_single_large_room());
	bool const must_leave_room(point_in_water_area(person.pos) || (is_above_retail_area(person.pos) && !point_over_glass_floor(person.pos)));

	if (single_large_room && !must_leave_room) {
		bool stay_in_room(person.is_first_path || rgen.rand_float() < 0.75); // stay in this room 75% of the time, always on the first path
		// stay in retail room or mall concourse if the player is there
		stay_in_room |= ((cur_room.is_retail() || cur_room.is_mall()) && cur_room.contains_pt(cur_player_building_loc.pos) && can_ai_follow_player(person));

		if (stay_in_room) {
			person.is_first_path = 0; // respect the walls
			if (select_person_dest_in_room(person, rgen, cur_room)) return 1;
		}
	}
	// sometimes use stairs in backrooms, parking garages, retail areas, and malls
	bool const try_use_stairs(single_large_room && cur_room.has_stairs && (must_leave_room || (!person.last_changed_floor() && rgen.rand_bool())));
	unsigned const num_tries(try_use_stairs ? 0 : 100);
	// handle special rooms that should have more traffic
	int special_rooms[2] = {-1, -1};
	if (has_retail())       {special_rooms[0] = 0;} // retail is the first room
	if (has_parking_garage) {special_rooms[1] = interior->rooms.size()-1;} // parking garage or backrooms

	// try to find a valid room to move to
	for (unsigned n = 0; n < num_tries; ++n) {
		unsigned cand_room(0);
		// select a special room with higher probability
		unsigned const room_sel(rgen.rand() & 7); // 25% chance for each special room type
		if (room_sel < 2 && special_rooms[room_sel] >= 0) {cand_room = special_rooms[room_sel];} else {cand_room = (rgen.rand() % interior->rooms.size());}
		if (cand_room == (unsigned)loc.room_ix) continue;
		room_t const &room(get_room(cand_room)); // dest room
		if (room.is_hallway            ) continue; // don't select a hallway
		if (room.get_has_out_of_order()) continue; // don't select a bathroom that may be out of order (not tracked per-floor)
		if (room.is_sec_bldg || (!ALLOW_AI_IN_MALLS && room.is_mall_or_store())) continue; // not a valid dest
		// use floor spacing of dest room to determine floors above/below; may not work when cur room is normal height, but should be okay (not choose to change floors)
		float const floor_spacing(room.is_mall_or_store() ? get_mall_floor_spacing() : window_vspace);
		// what about rooms were all doors are locked? this isn't easy to check for; it will be a surprise for the player if the person is a zombie
		bool const is_retail(room.is_retail());
		// allow targeting the top floor of a retail room as the path construction will route to the lowest level
		float const bot_floor_z(room.z1()), top_ceil_z((room.is_single_floor && !is_retail) ? (bot_floor_z + floor_spacing) : room.z2()); // approximate
		if ((person.pos.z + floor_spacing) < bot_floor_z || (person.pos.z - floor_spacing) > top_ceil_z) continue; // room more than one floor above/below current pos
		// allow move to a different stacked part 25% of the time; 100% of the time for parking garages and retail, since they're more rare
		if ((person.pos.z < bot_floor_z || person.pos.z > top_ceil_z) && !(has_parking_garage && bot_floor_z < ground_floor_z1) && !is_retail && (rgen.rand()&3) != 0) continue;
		if (!interior->nav_graph->is_room_connected_to(loc.room_ix, cand_room, *interior, person.pos.z, person.has_key)) continue;
		person.target_pos   = get_center_of_room(cand_room);
		person.target_pos.z = person.pos.z; // keep orig zval to stay on the same floor

		if (person.pos.z > top_ceil_z) { // room is below the person
			float const new_z(person.target_pos.z - floor_spacing);
			assert(new_z > bot_floor_z && new_z < top_ceil_z);
			person.target_pos.z = new_z; // target the floor below
		}
		else if (person.pos.z < bot_floor_z) { // room is above the person
			float const new_z(person.target_pos.z + floor_spacing);
			assert(new_z > bot_floor_z && new_z < top_ceil_z);
			person.target_pos.z = new_z; // target the floor above
		}
		//if (person.is_first_path) {} // don't use stairs on the first path?
		else if (interior->ext_basement_hallway_room_id < 0 || cand_room < (unsigned)interior->ext_basement_hallway_room_id) { // skip for ext basement rooms
			// room covers floor this person is on; allow moving to a different floor, currently only one floor at a time
			cube_t const &part(get_part_for_room(room)); // or just use the room?
			unsigned const rand_val(rgen.rand() & 3); // 0-3

			if (rand_val == 0 && !point_in_water_area(person.target_pos - floor_spacing*plus_z)) { // try one floor below if not in the water
				float const new_z(person.target_pos.z - floor_spacing);
				if (new_z > part.z1()) {person.target_pos.z = new_z;} // change if there is a floor below
			}
			else if (rand_val == 1) { // try one floor above
				float const new_z(person.target_pos.z + floor_spacing);
				if (new_z < top_ceil_z) {person.target_pos.z = new_z;} // change if there is a floor above
			}
			assert(person.target_pos.z > bot_floor_z && person.target_pos.z < top_ceil_z);
		}
		if (!in_building_gameplay_mode()) { // handle bathroom genders, unless we're a zombie
			room_type const rtype(room.get_room_type_for_zval(person.target_pos.z, floor_spacing));
			if (rtype == (person.is_female ? RTYPE_MENS : RTYPE_WOMENS)) continue; // wrong gender
		}
		person.dest_room = cand_room; // set but not yet used
		person.goal_type = GOAL_TYPE_ROOM;
		person.last_used_stairs = 0; // non-stairs dest, reset
		return 1;
	} // for n

	// how about a different floor of the same room? only check this 50% of the time for parking garages/backrooms/retail to allow movement within a level
	if (try_use_stairs || (cur_room.has_stairs == 255 && (!single_large_room || rgen.rand_bool()))) {
		// use person.prev_walked_down?
		float const floor_spacing(cur_room.is_mall_or_store() ? get_mall_floor_spacing() : window_vspace);
		float const above_zval(person.target_pos.z + floor_spacing), below_zval(person.target_pos.z - floor_spacing);
		bool const try_below(below_zval > cur_room.z1() && rgen.rand_bool() && !cur_room.is_retail() && !point_in_water_area(person.target_pos - floor_spacing*plus_z));
		float const new_z(try_below ? below_zval : above_zval); // one floor above or below

		if (new_z > cur_room.z1() && new_z < cur_room.z2()) { // valid if this floor is inside the room
			person.target_pos.z = new_z;
			if (select_person_dest_in_room(person, rgen, cur_room)) return 1; // if retail, this may lead to an upper floor stairs path that exits the room
		}
	}
	// how about a different location in the same room? this will at least get the person unstuck from an object and moving inside a parking garage
	if ((person.is_first_path || single_large_room || cur_room.is_store()) && !must_leave_room) {
		if (select_person_dest_in_room(person, rgen, cur_room)) {person.last_used_stairs = 0; return 1;}
	}
	return 2; // failed, but can retry
}

template<typename T> void add_bcube_if_overlaps_zval(vector<T> const &cubes, vect_cube_t &out, float z1, float z2) {
	for (auto const &i : cubes) {
		if (i.z1() < z2 && i.z2() > z1) {out.push_back(i);}
	}
}

// for AI collision detection
// same_as_player allows the AI to follow the player, but can result in it getting stuck when turning gameplay mode off
void building_interior_t::get_avoid_cubes(vect_cube_t &avoid, float z1, float z2, float r_shrink_if_low, float floor_thickness,
	float floor_ceil_gap, bool same_as_player, bool skip_stairs, cube_t const *const fires_select_cube) const
{
	avoid.clear();

	if (!skip_stairs) {
		float const doorway_width(get_doorway_width());

		for (auto const &s : stairwells) { // add extra width for side walls only
			if (s.z1() > z2 || s.z2() < z1) continue;
			cube_t const bc_exp(get_stairs_bcube_expanded(s, 0.0, 0.0, doorway_width));

			if (s.is_l_shape() && z2 < (s.z1() + floor_ceil_gap + floor_thickness)) { // bottom of L-shaped stairs
				float const width(doorway_width + 0.5*s.get_wall_hwidth(floor_ceil_gap + floor_thickness)); // add railing thickness
				cube_t seg1(s), seg2(s);
				seg1.d[!s.dim][ s.bend_dir] = s.d[!s.dim][!s.bend_dir] + (s.bend_dir ? 1.0 : -1.0)*width; // long segment
				seg2.d[ s.dim][!s.     dir] = s.d[ s.dim][ s.     dir] - (s.     dir ? 1.0 : -1.0)*width; // short segment
				avoid.push_back(seg1);
				avoid.push_back(seg2);
				continue;
			}
			if (s.in_mall != 1 || 0.5*(z1 + z2) > s.zc()) {avoid.push_back(bc_exp); continue;} // on top floor, or not mall (single floor tall), use full bcube
			// on bottom floor of mall stairs; this is a single flight of stairs, so we can treat it as a sloped ramp
			cube_t ac(bc_exp);
			float const t(CLIP_TO_01((z2 - s.z1())/(0.5f*s.dz() - floor_thickness))); // value where z2 (head) hits stairs; note that true stairs dz is half the value
			ac.d[s.dim][s.dir] = s.d[s.dim][!s.dir] + (s.dir ? 1.0 : -1.0)*t*s.get_sz_dim(s.dim);
			avoid.push_back(ac);
		} // for s
	}
	if (!(USE_MALL_ENT_STAIRS && skip_stairs) && has_mall_ent_stairs() && mall_info->ent_stairs.z1() < z2 && mall_info->ent_stairs.z2() > z1) {
		avoid.push_back(mall_info->ent_stairs); // only allow walking on mall entrance stairs when enabled, and path includes stairs (so stairs coll is skipped)
	}
	add_bcube_if_overlaps_zval(elevators, avoid, z1, z2); // clearance not required
	
	for (escalator_t const &e : escalators) {
		if (e.z1() > z2 || e.z2() < z1) continue; // no Z overlap
		if (0.5*(z1 + z2) > e.zc()) {avoid.push_back(e); continue;} // on top floor, use full bcube
		// on bottom floor
		cube_t const ramp(e.get_ramp_bcube(0)); // exclude_sides=0
		cube_t ac(e);
		float const t(CLIP_TO_01((z2 - ramp.z1())/ramp.dz())); // value where z2 (head) hits ramp
		ac.d[e.dim][e.dir] = ramp.d[e.dim][!e.dir] + (e.dir ? 1.0 : -1.0)*t*ramp.get_sz_dim(e.dim);
		avoid.push_back(ac);
		if (!e.in_mall) {avoid.push_back(e.get_support_pillar());}
	} // for e
	if (pool.valid && z1 < (pool.z2() + floor_ceil_gap)) { // skip if !same_as_player to allow zombies in pools?
		avoid.push_back(pool);
		avoid.back().z2() += floor_ceil_gap; // increase height so that it overlaps a person standing over it
	}
	if (has_mall()) {
		for (store_doorway_t const &d : mall_info->store_doorways) {
			if (d.closed) {avoid.push_back(d);}
		}
	}
	if (ind_info && z2 > ind_info->floor_space.z1()) { // in industrial area; add interior walls of sub-rooms
		float const wall_shrink(0.25*floor_thickness); // shrink walls slightly at ends since doorways are narrow

		for (unsigned d = 0; d < 2; ++d) {
			unsigned const avoid_start(avoid.size());
			add_bcube_if_overlaps_zval(walls[d], avoid, z1, z2);
			for (auto i = avoid.begin()+avoid_start; i != avoid.end(); ++i) {i->expand_in_dim(!d, -wall_shrink);}
		}
	}
	if (!room_geom) return; // no room objects
	auto objs_end(room_geom->get_placed_objs_end()); // skip buttons/stairs/elevators
	float const z_thresh(z1 + 0.35*(z2 - z1)); // used with r_shrink_if_low

	for (auto c = room_geom->objs.begin(); c != objs_end; ++c) {
		if (!is_ai_coll_obj(*c, same_as_player)) continue;
		// what about TYPE_CURB? should people step on/over these, or generally avoid them?
		// skip_stairs also skips ramps? no, this seems to result in zombies walking in midair as if the ramp was a solid floor when approaching from above;
		// maybe zombies should walk down the ramp instead? but ramps are wider than stairs, if the player stands to the side then the zombie may walk right by;
		// so that means we need navigation to the side while on a ramp? this seems quite difficult for the current system to support
		//if (skip_stairs && c->type == TYPE_RAMP) continue;
		//if (c->type == TYPE_ATTIC_DOOR && (c->in_hallway())) continue; // skip open attic doors in hallways because they block the path too much
		cube_t bc(get_true_room_obj_bcube(*c)); // needed for open attic door
		if (bc.z1() > z2 || bc.z2() < z1) continue;
		if (c->type == TYPE_RAMP && bc.z2() < z1 + floor_thickness) continue; // ramp top is exactly at the floor; add floor_thickness to prevent coll when walking above
		
		if (global_building_params.ai_sees_player_hide >= 2 && c->is_open()) { // open hiding spot that we can enter
			// in the unlikely event the player closes the door on a zombie, I guess they're stuck in here; good job to the player
			if (c->type == TYPE_CLOSET) {
				cube_t cubes[5]; // front left, left side, front right, right side, door
				get_closet_cubes(*c, cubes, 1); // for_collision=1
				bool const small_closet(c->is_small_closet());

				for (unsigned i = 0; i < 4; ++i) { // skip the open door
					if (small_closet && (i == 0 || i == 2)) continue; // skip front sides next to door to allow larger zombies space to enter the closet
					avoid.push_back(cubes[i]);
				}
				// should we skip items inside the closet? I guess they should have RO_FLAG_NOCOLL set anyway, so it may not matter
				continue;
			}
			else if (c->type == TYPE_STALL) {
				// I've seen one person push another into a toilet in a bathroom stall and that person got stuck;
				// maybe the problem is that they're inside two objects at once? in any case, skipping the toilet should be okay; or only if stall is closed?
				if (!avoid.empty() && c->contains_cube(avoid.back())) {avoid.pop_back();}
				cube_t sides[3];
				unsigned const num_cubes(get_stall_cubes(*c, sides));
				avoid.insert(avoid.end(), sides, sides+num_cubes);
				continue;
			}
			else if (c->type == TYPE_SHOWER) {
				cube_t sides[2];
				unsigned const num_cubes(get_shower_cubes(*c, sides));
				avoid.insert(avoid.end(), sides, sides+num_cubes);
				continue;
			}
		}
		if (c->type == TYPE_CUBICLE) {
			cube_t cubes[8];
			get_cubicle_parts(*c, cubes, cubes+3, cubes[2], cubes+5);
			avoid.insert(avoid.end(), cubes, cubes+(same_as_player ? 3 : 8)); // add all cubes, but sides and back only if chasing player
			continue;
		}
		if (r_shrink_if_low > 0.0 && c->z2() < z_thresh && c->shape == SHAPE_CUBE) { // shrink cube if it's low; applies to boxes and crates on the floor
			bc.expand_by_xy(-min(0.95f*0.5f*min(c->dx(), c->dy()), r_shrink_if_low)); // make sure it doesn't shrink to zero area
		}
		// since jail cell doors are narrower than regular doors, shrink the ends of the bars a bit so that the AI can fit
		if (c->type == TYPE_JAIL_BARS) {bc.expand_in_dim(!c->dim, -0.1*get_doorway_width());}
		avoid.push_back(bc);
		
		if (same_as_player && c->type == TYPE_TABLE && c->shape == SHAPE_CYLIN) {
			// special handling of round table so that the player can't hide in the area unreachable from the bounding cube
			float const shrink_val(c->get_radius()*(1.0 - 1.0/SQRT2)); // small shrink to square inscribed in the circle
			avoid.back().expand_by(-shrink_val);

			for (unsigned d = 0; d < 2; ++d) { // create a cross shape
				avoid.push_back(bc);
				avoid.back().expand_in_dim(d, -2.0*shrink_val);
			}
		}
	} // for c
	for (auto c = room_geom->get_stairs_start(); c != room_geom->objs.end(); ++c) { // include stairs walls
		if (c->z1() > z2 || c->z2() < z1) continue;
		if (!c->no_coll() && c->type == TYPE_STAIR_WALL) {avoid.push_back(*c);}
	}
	if (conn_info != nullptr) { // include extended basement connector doors because building people can't pass through these yet
		for (auto const &c : conn_info->conn) {
			for (auto const &room : c.rooms) {
				if (room.door_is_b) continue; // door is for the other building
				door_t const &door(get_door(room.door_ix));
				cube_t const bc(door.get_true_bcube());
				if (bc.z1() > z2 || bc.z2() < z1) continue;
				avoid.push_back(bc);
				avoid.back().expand_in_dim(door.dim, floor_thickness); // make it a bit wider to be sure the person will collide with it
			}
		} // for c
	}
	if (fires_select_cube != nullptr) {
		cube_t sel_cube(*fires_select_cube);
		set_cube_zvals(sel_cube, z1, z2); // clip to the current Z-range
		room_geom->fire_manager.add_fire_bcubes_for_cube(sel_cube, avoid);
	}
}

unsigned get_elevator_floor(float zval, elevator_t const &e, float floor_spacing) { // floor index relative to this elevator
	return max(0.0f, (zval - e.z1()))/floor_spacing;
}

bool building_t::stairs_contained_in_part(stairwell_t const &s, cube_t const &p) const {
	cube_t sc(s);
	if (s.roof_access) {sc.z2() -= get_window_vspace();} // clip off top floor roof access
	return p.contains_cube(sc);
}
bool building_t::no_stairs_exit_on_floor(stairwell_t const &stairs, float zval) const {
	if (stairs.not_an_exit_mask == 0) return 0;
	int const to_floor(max(0, int((zval - stairs.z1())/get_stairs_floor_spacing(stairs))));
	return (stairs.not_an_exit_mask & (1 << to_floor));
}
void building_t::find_nearest_stairs_ramp_esc(point const &p1, point const &p2, vector<unsigned> &nearest_stairs, int part_ix) const { // p1=from, p2=to
	nearest_stairs.clear();
	assert(interior);
	assert(part_ix < 0 || (unsigned)part_ix < parts.size());
	bool const maybe_in_basement(part_ix < 0 || part_ix == basement_part_ix), inc_mall_ent_stairs(has_mall_ent_stairs());
	float const zmin(min(p1.z, p2.z)), zmax(max(p1.z, p2.z));
	unsigned const num_stairwells(interior->stairwells.size()), stairs_end(num_stairwells + inc_mall_ent_stairs);
	vector<pair<float, unsigned>> sorted;

	for (unsigned s = 0; s < num_stairwells; ++s) {
		stairwell_t const &stairs(interior->stairwells[s]);
		if (zmin < stairs.z1() || zmax > stairs.z2()) continue; // stairs don't span the correct floors
		if (part_ix >= 0 && !stairs_contained_in_part(stairs, parts[part_ix])) continue; // stairs don't belong to this part (Note: this option is currently unused)
		//if (no_stairs_exit_on_floor(stairs, p2.z))    continue; // not an exit; now handled in path reconstruction by pathing to the floor above or below through this point
		sorted.emplace_back(get_dist_xy_through_pt(p1, stairs.get_cube_center(), p2), s);
	}
	if (has_pg_ramp() && ((maybe_in_basement && zmax < ground_floor_z1) || is_parking())) { // parking garage ramp
		if (zmin > interior->pg_ramp.z1() && zmax < interior->pg_ramp.z2()) { // ramp is within vertical range
			sorted.emplace_back(get_dist_xy_through_pt(p1, interior->pg_ramp.get_cube_center(), p2), stairs_end); // ix=stairs_end
		}
	}
	if (maybe_in_basement && has_mall()) {
		if (point_in_extended_basement_not_basement(p1) && point_in_extended_basement_not_basement(p2)) { // both points in mall area
			for (unsigned i = 0; i < interior->escalators.size(); ++i) {
				escalator_t const &e(interior->escalators[i]);
				if (!e.in_mall) continue; // only consider mall escalators
				if (zmin < e.z1() || zmax > e.z2())   continue; // escalator doesn't span the correct floors
				if (e.is_going_up() != (p2.z > p1.z)) continue; // wrong direction
				float const dist((e.is_powered ? 1.0 : 1.5)*get_dist_xy_through_pt(p1, e.get_cube_center(), p2)); // increase distance/reduce priority if unpowered
				sorted.emplace_back(dist, (i + stairs_end + 1));
			}
		}
		if (inc_mall_ent_stairs && zmax < ground_floor_z1) { // consider mall entrance stairs
			cube_t const pg(get_basement());
			float const mall_top_floor_zval(get_mall_concourse().z2() - get_mall_floor_spacing()), pg_bot_ceil_zval(pg.z1() + get_window_vspace());

			for (unsigned d = 0; d < 2; ++d) {
				point const &A(d ? p2 : p1), &B(d ? p1 : p2);

				// check if one point is in lowest level of the parking garage and the other is in the upper level of the mall
				if (pg.contains_pt(A) && A.z < pg_bot_ceil_zval && point_in_mall(B) && B.z > mall_top_floor_zval) {
					sorted.emplace_back(get_dist_xy_through_pt(p1, interior->mall_info->ent_stairs.get_cube_center(), p2), num_stairwells); // ix=num_stairwells
				}
			} // for d
		}
	}
	sort(sorted.begin(), sorted.end()); // sort by distance, min first
	for (auto s = sorted.begin(); s != sorted.end(); ++s) {nearest_stairs.push_back(s->second);}
}
int building_t::find_nearest_elevator_this_floor(point const &pos) const {
	int nearest(-1); // -1 => none found
	float dmin_sq(0.0);

	for (auto e = interior->elevators.begin(); e != interior->elevators.end(); ++e) {
		if (e->z1() > pos.z || e->z2() < pos.z) continue; // doesn't span the correct floor
		if (e->in_mall && !point_in_mall(pos))  continue; // mall elevator only reaches the mall
		float const floor_spacing(get_elevator_floor_spacing(*e));
		
		if (e->in_backrooms) {
			if (!(interior->has_backrooms && point_in_extended_basement_not_basement(pos))) continue; // backrooms elevator only reaches the backrooms
			
			if (has_water()) { // skip if on first floor and second floor is underwater
				float const fc_thick(get_fc_thickness()), top_floor_zval(e->z2() - floor_spacing + fc_thick), sec_floor_zval(e->z2() - 2.0*floor_spacing + fc_thick);
				if (pos.z > top_floor_zval && interior->water_zval > sec_floor_zval) continue;
			}
		}
		if (e->skip_floor_ix(get_elevator_floor(pos.z, *e, floor_spacing))) continue; // floor not reachable from this elevator; unclear if we can get here
		// skip if elevator is currently in use?; skip elevator if wait pos is blocked?
		float const dsq(p2p_dist_xy_sq(pos, e->get_cube_center()));
		if (nearest < 0 || dsq < dmin_sq) {nearest = (e - interior->elevators.begin()); dmin_sq = dsq;}
	} // for e
	return nearest;
}

cube_t get_stairs_plus_step_up(stairwell_t const &stairs, float max_ext) {
	float step_len((stairs.get_length()/stairs.get_num_stairs())*(stairs.is_straight() ? 1.0 : 2.0)); // U and L shaped stairs are longer due to bends
	min_eq(step_len, max_ext); // limit to a reasonable value for long stairs such as L-shaped
	cube_t stairs_ext(stairs);
	stairs_ext.d[stairs.dim][!stairs.dir] += (stairs.dir ? -1.0 : 1.0)*step_len; // location of step up
	return stairs_ext;
}

void ai_path_t::add(path_pt_t const &p) {
	if (empty() || p != back()) {push_back(p);} // don't add duplicates
	else {back().fixed |= p.fixed;} // if it was a duplicate, update the fixed flag
}
void ai_path_t::add(ai_path_t const &path) {
	for (path_pt_t const &p : path) {add(p);}
	uses_nav_grid |= path.uses_nav_grid;
}

void remove_escalator_from_avoid_cubes(person_t const &person, float radius, vector<escalator_t> const &escalators, vect_cube_t &avoid) {
	escalator_t const &e(escalators[person.cur_escalator]);
	cube_t person_bc;
	person_bc.set_from_sphere(person.pos, radius); // tighter than person.get_bcube()

	if (person_bc.intersects(e)) {
		for (auto i = avoid.begin(); i != avoid.end(); ++i) {
			if (*i == e) {avoid.erase(i); break;} // only applies to top floor of escalator
		}
	}
}

void building_t::get_avoid_cubes(float zval, float height, float radius, vect_cube_t &avoid, bool following_player, cube_t const *const fires_select_cube) const {
	assert(interior);
	float const z1(zval - radius);
	interior->get_avoid_cubes(avoid, z1, (z1 + height), 0.5*radius, get_floor_thickness(), get_floor_ceil_gap(), following_player, 0, fires_select_cube); // skip_stairs=0
}
void building_t::add_sub_rooms_to_avoid_if_needed(room_t const &room, vect_cube_t &avoid) const {
	if (room.is_industrial() && interior->ind_info) {vector_add_to(interior->ind_info->sub_rooms, avoid);}
}

bool building_t::find_route_to_point(person_t &person, float radius, bool is_first_path, bool following_player, ai_path_t &path) const {

	assert(interior && interior->nav_graph);
	path.clear();
	point const &from(person.pos), &to(person.target_pos);
	building_loc_t const loc1(get_building_loc_for_pt(from)), loc2(get_building_loc_for_pt(to));
	if (loc1.part_ix < 0 || loc2.part_ix < 0 || loc1.room_ix < 0 || loc2.room_ix < 0) return 0; // not in a room
	unsigned const num_rooms(interior->rooms.size());
	assert((unsigned)loc1.part_ix < parts.size() && (unsigned)loc2.part_ix < parts.size());
	assert((unsigned)loc1.room_ix < num_rooms && (unsigned)loc2.room_ix < num_rooms);
	float const height(person.get_height());
	vect_cube_t &avoid(reused_avoid_cubes[0]);

	if (person.goal_type == GOAL_TYPE_ESCALATOR) {
		ai_path_t epath;
		add_escalator_points(person, epath);
		assert(epath.size() == 4);
		point const &eenter(epath.back()), &eexit(epath.front()); // entrance and exit point of escalator
		get_avoid_cubes(to.z, height, radius, avoid, following_player);
		// upper path should be constrained to glass floor rather than room; assumes glass floors create a full rectangle
		cube_t const clamp_area(get_bcubes_union(interior->room_geom->glass_floors));
		assert(clamp_area.is_strictly_normalized()); // glass floors must be nonempty
		bool const going_up(to.z > from.z);
		if (!interior->nav_graph->complete_path_within_room(eexit, to, loc1.room_ix, person.ssn, radius, person.cur_rseed,
			is_first_path, following_player, (going_up ? clamp_area : cube_t()), avoid, *this, path)) return 0; // escalator to target
		path.add(epath); // add escalator segment
		get_avoid_cubes(from.z, height, radius, avoid, following_player);
		remove_escalator_from_avoid_cubes(person, radius, interior->escalators, avoid); // check if already intersecting the escalator; if so, remove it from avoid
		return interior->nav_graph->complete_path_within_room(from, eenter, loc1.room_ix, person.ssn, radius, person.cur_rseed,
			is_first_path, following_player, (going_up ? cube_t() : clamp_area), avoid, *this, path); // person to escalator
	}
	room_t const &start_room(get_room(loc1.room_ix));
	get_avoid_cubes(from.z, height, radius, avoid, following_player, &start_room); // include fires in the current room

	if (loc1.same_room_floor(loc2)) { // same room/floor (not checking stairs_ix)
		assert(from.z == to.z);
		point dest(to);

		// if both the person and target (player) are in an extended basement room, clamp dest to walkable room bounds;
		// since these rooms aren't packed together, it's possible for the player to be unreachable, for example when crossing through a connector hallway
		if (to.z < ground_floor_z1) { // in the basement
			room_t const &room(get_room(loc1.room_ix));

			if (room.is_ext_basement()) { // extended basement room; room.is_ext_basement_conn()? or does it need to apply to the other adj room as well?
				cube_t valid_area(room);
				valid_area.expand_by_xy(-radius);
				valid_area.clamp_pt_xy(dest);
			}
		}
		add_sub_rooms_to_avoid_if_needed(start_room, avoid); // must avoid sub-rooms
		cube_t const constrain_area(point_over_glass_floor(from) ? get_bcubes_union(interior->room_geom->glass_floors) : cube_t());
		if (!interior->nav_graph->complete_path_within_room(from, dest, loc1.room_ix, person.ssn, radius,
			person.cur_rseed, is_first_path, following_player, constrain_area, avoid, *this, path)) return 0;
		return 1;
	}
	bool have_goal_pos(0), req_custom_dest(0);
	// if the target is an elevator, use that as the required destination rather than the center of the room
	if (person.goal_type == GOAL_TYPE_ELEVATOR) {have_goal_pos = req_custom_dest = 1;}
	// if the target is the player and they're in a hallway or industrial area, use the correct dest along the hallway
	else if (person.goal_type == GOAL_TYPE_PLAYER || person.goal_type == GOAL_TYPE_PLAYER_LAST_POS) {
		//have_goal_pos = is_valid_ai_placement(person.target_pos, person.radius, 0, 1); // skip_nocoll=0, no_check_objs=1
		room_t const &dest_room(get_room(loc2.room_ix));
		have_goal_pos = (dest_room.is_hallway || dest_room.is_industrial());
	}
	point const *const custom_dest(have_goal_pos ? &person.target_pos : nullptr);

	if (loc1.floor_ix != loc2.floor_ix) { // different floors: find path from <from> to nearest stairs, then find path from stairs to <to>
		vector<unsigned> cand_stairs_ramp_esc;
		find_nearest_stairs_ramp_esc(from, to, cand_stairs_ramp_esc); // pass in loc1.part_ix if both loc part_ix values are equal?
		bool const up_or_down(loc1.floor_ix > loc2.floor_ix); // 0=up, 1=down; Note: floor_ix is relative to bcube.z1() (or ext_basement z1), so is consistent across parts
		unsigned const num_stairwells(interior->stairwells.size()), stairs_end(num_stairwells + has_mall_ent_stairs());

		for (unsigned s : cand_stairs_ramp_esc) { // try using stairs, ramp, or escalator, closest to furthest: {stairs, mall stairs, ramp, escalators}
			bool const is_ramp(s == stairs_end), is_escalator(s > stairs_end);
			if (is_ramp     ) {assert(has_pg_ramp());}
			if (is_escalator) {assert(!interior->escalators.empty());}
			// map to graph space; should work for stairs or ramp, and escalator is always in the mall
			unsigned const sre_room_ix(is_escalator ? (unsigned)interior->ext_basement_hallway_room_id : (s + num_rooms)); // escalators are not assigned nav graph nodes
			point seg2_start, enter_pt;
			ai_path_t from_path, escalator_path;
			path.clear();

			if (is_escalator) {
				person.cur_escalator = (s - stairs_end - 1);
				add_escalator_points(person, escalator_path);
				assert(escalator_path.size() == 4);
				seg2_start   = escalator_path[0]; // escalator exit point
				enter_pt     = escalator_path[3]; // escalator entrance point
				seg2_start.z = to.z; // required to avoid assert; should be close, but may not be exactly to.z
				assert(enter_pt.z == from.z);
			}
			else { // stairs or ramp
				seg2_start = interior->nav_graph->get_stairs_entrance_pt(to.z, sre_room_ix, !up_or_down); // other/exit end
			}
			if (!point_in_building_or_basement_bcube(seg2_start)) {
				cerr << TXT(s) << TXT(num_stairwells) TXT(stairs_end) << TXT(sre_room_ix) << TXT(is_ramp) << TXT(is_escalator) << endl;
				cerr << TXT(seg2_start.str()) << TXT(bcube.str());
				if (has_basement()) {cerr << TXT(get_basement().str());}
				cerr << TXT(interior->basement_ext_bcube.str()) << TXT(from.str()) << TXT(to.str()) << endl;
			}
			assert(point_in_building_or_basement_bcube(seg2_start));
			
			// from => stairs/ramp/escalator
			if (is_escalator && (int)sre_room_ix == loc1.room_ix) {
				remove_escalator_from_avoid_cubes(person, radius, interior->escalators, avoid); // check if already intersecting the escalator
				if (!interior->nav_graph->complete_path_within_room(from, enter_pt, loc1.room_ix, person.ssn, radius, person.cur_rseed,
					is_first_path, following_player, cube_t(), avoid, *this, from_path)) continue; // person to escalator
			}
			else {
				point const *const seg1_dest(is_escalator ? &enter_pt : nullptr); // must enter the escalator exactly at this point
				// Note: passing use_stairs=0 here because it's unclear if we want to go through stairs nodes in our A* algorithm
				if (!interior->nav_graph->find_path_points(loc1.room_ix, sre_room_ix, person.ssn, radius, 0, is_first_path,
					up_or_down, person.cur_rseed, avoid, *this, from, person.has_key, seg1_dest, is_escalator, from_path)) continue; // custom_dest for escalator
			}
			// new floor, new zval, new avoid cubes
			vect_cube_t &avoid2(reused_avoid_cubes[1]);
			get_avoid_cubes(seg2_start.z, height, radius, avoid2, following_player); // no fires_select_cube
			
			// stairs/ramp/escalator => to
			if (is_escalator && (int)sre_room_ix == loc2.room_ix) {
				if (!interior->nav_graph->complete_path_within_room(seg2_start, to, loc2.room_ix, person.ssn, radius, person.cur_rseed,
					is_first_path, following_player, cube_t(), avoid2, *this, path)) continue; // escalator to target
			}
			else {
				if (!interior->nav_graph->find_path_points(sre_room_ix, loc2.room_ix, person.ssn, radius, 0, is_first_path,
					!up_or_down, person.cur_rseed, avoid2, *this, seg2_start, person.has_key, custom_dest, req_custom_dest, path)) continue;
			}
			assert(!path.empty() && !from_path.empty());
			path.add(seg2_start, 1); // exit end of the stairs/ramp/escalator, extended with clearance

			// add two or more more points to straighten the entrance and exit paths and wrap around stairs; this segment doesn't check for intersection with stairs
			if (is_ramp) { // ramp
				cube_t const &walk_area(interior->pg_ramp);
				assert(!walk_area.is_all_zeros());
				enter_pt = walk_area.closest_pt(from_path.front()); // entrance point of ramp
				path.add(walk_area.closest_pt(path.back()), 1); // exit point of ramp
			}
			else if (is_escalator) { // escalator
				assert(escalator_path.size() == 4);
				for (unsigned i = 1; i < 3; ++i) {path.add(escalator_path[i]);} // add the two middle points; the exit point was added above
			}
			else { // stairs
				assert(s < stairs_end); // last set is mall stairwell
				stairwell_t const &stairs((s == num_stairwells) ? interior->mall_info->ent_stairs : interior->stairwells[s]);
				assert(!stairs.is_all_zeros());
				cube_t const stairs_ext(get_stairs_plus_step_up(stairs, radius));
				enter_pt = stairs_ext.closest_pt(from_path.front());
				point const exit_pt(stairs_ext.closest_pt(path.back()));
				path.add(exit_pt, 1);

				if (stairs.is_u_shape()) {
					// add 2 extra points on mid-level landing and possibly more for non-exit landings; entrance and exit will be on the same side
					bool const dim(stairs.dim), dir(stairs.dir); // Note: see code in add_stairs_and_elevators()
					float const window_vspace(get_window_vspace());
					// make sure we can reach the turning point; should this be 1.0/NUM_STAIRS_PER_FLOOR rather than 0.1?
					float const turn_pt(stairs.d[dim][dir] - (dir ? 1.0 : -1.0)*max(radius, 0.1f*stairs.get_length()));
					float const seg_delta_z(0.45f*SIGN(to.z - from.z)*window_vspace);
					bool const going_up(exit_pt.z > enter_pt.z);
					point const next_floor_delta(0.0, 0.0, (going_up ? 1.0 : -1.0)*window_vspace); // dz
					point exit_turn (exit_pt .x, exit_pt .y, (to  .z - seg_delta_z));
					point enter_turn(enter_pt.x, enter_pt.y, (from.z + seg_delta_z));
					exit_turn [dim] = turn_pt;
					enter_turn[dim] = turn_pt;

					if (stairs.in_mall != 2 && no_stairs_exit_on_floor(stairs, exit_pt.z)) {
						// two level retail stairs where exit point is at the middle level that can't be exited;
						// end our path here, since it may be invalid, then extend a new path away from the stairs
						path.clear();
						vector3d path_extend;
						path_extend[dim] -= (dir ? 1.0 : -1.0)*0.5*get_doorway_width(); // move out of the extended stairs area
						path.add((exit_pt    + next_floor_delta + path_extend), 1); // exit point from stairwell area (clear of walls)
						// create another loop around the stairs to the floor above or below
						path.add((exit_pt    + next_floor_delta), 1); // move the exit to the other floor
						path.add((exit_turn  + next_floor_delta), 1); // turning point for exit side of stairs on other floor (slightly sloped)
						path.add((enter_turn + next_floor_delta), 1); // turning point for entrance side of stairs on other floor
						path.add((enter_pt   + next_floor_delta), 1); // entrance on prev exit floor
						path.add(exit_pt, 1); // original exit point
						person.no_wait_at_dest  = 1; // don't wait at (blocking) the stairs exit, since this isn't our real destination anyway
						person.last_used_stairs = 1; // don't immediately go back up/down the stairs
						// Note: person.dest_room can be wrong when exiting in the hallway above the retail room, but it should be unused
					}
					path.add(exit_turn, 1); // turning point for exit side of stairs

					if (stairs.in_mall == 2) { // two floor tall mall back hallway stairs
						// two floors of separation; create another loop around the stairs to connect the upper and lower segments
						path.add((enter_turn + next_floor_delta), 1); // turning point for entrance side of stairs on other floor
						path.add((enter_pt   + next_floor_delta), 1); // entrance on prev exit floor
						path.add((exit_pt    - next_floor_delta), 1); // move the exit to the other floor
						path.add((exit_turn  - next_floor_delta), 1); // turning point for exit side of stairs on other floor (slightly sloped)
					}
					path.add(enter_turn, 1); // turning point for entrance side of stairs
				}
				else if (stairs.is_l_shape()) {
					float const landing_width(get_landing_width()), landing_offset(0.25*landing_width), stair_dz(get_window_vspace()/(stairs.get_num_stairs()+1));
					unsigned const num_stairs1(get_L_stairs_first_flight_count(stairs, landing_width)); // number of stairs in first/lower flight
					point landing_center;
					landing_center.z = min(from.z, to.z) + (num_stairs1 + 1)*stair_dz; // landing is up num_stairs1 + 1 (for the landing itself) in height
					landing_center[ stairs.dim] = stairs.d[ stairs.dim][ stairs.dir     ] - (stairs.dir      ?  1.0 : -1.0)*0.5*landing_width;
					landing_center[!stairs.dim] = stairs.d[!stairs.dim][!stairs.bend_dir] - (stairs.bend_dir ? -1.0 :  1.0)*0.5*landing_width;
					//path.add(landing_center, 1); // turn at the landing center
					// cut the corner slightly rather than turning at the landing center so that our feet are closer to the landing z2;
					// use a very small Z offset so that the path segment is sloped and we correctly detect that the person is on the stairs
					vector3d const delta_exit(exit_pt - landing_center), delta_enter(enter_pt - landing_center);
					vector3d const exit_dir_xy (vector3d(delta_exit .x, delta_exit .y, 0.01*delta_exit .z).get_norm());
					vector3d const enter_dir_xy(vector3d(delta_enter.x, delta_enter.y, 0.01*delta_enter.z).get_norm());
					path.add((landing_center + exit_dir_xy *landing_offset), 1);
					path.add((landing_center + enter_dir_xy*landing_offset), 1);
				}
				else if (stairs.shape == SHAPE_FAN) { // mall stairs
					// TODO: same as straight stairs?
				}
			} // end stairs case
			if (from_path.front() != enter_pt) {path.add(enter_pt, 1);} // don't add a duplicate
			path.add(from_path); // concatenate the two path segments in reverse order: to + from
			return 1; // done/success
		} // for s
		path.clear(); // not necessary?
		return 0; // failed
	}
	assert(loc1.room_ix != loc2.room_ix);
	if (!interior->nav_graph->find_path_points(loc1.room_ix, loc2.room_ix, person.ssn, radius, 0, is_first_path,
		0, person.cur_rseed, avoid, *this, from, person.has_key, custom_dest, req_custom_dest, path)) return 0;
	assert(!path.empty());
	return 1;
}

bool person_t::waiting_for_same_elevator_as(person_t const &other, float floor_spacing) const {
	if (&other == this) return 0; // skip ourself
	if (other.ai_state != AI_WAIT_ELEVATOR) return 0; // other person is not also waiting for the elevator
	return (other.cur_elevator == cur_elevator && fabs(other.pos.z - pos.z) < 0.5f*floor_spacing); // check for same elevator and floor
}
void person_t::next_path_pt(bool starting_path) {
	assert(!path.empty());
	is_on_stairs  = (!starting_path && target_pos.z != path.back().z); // or ramp, or escalator
	path_is_fixed = path.back().fixed;
	target_pos    = path.back();
	path.pop_back();
}

bool building_t::is_above_retail_area(point const &pos) const {
	if (!has_tall_retail()) return 0;
	cube_t const &retail_room(get_retail_part());
	return (retail_room.contains_pt(pos) && pos.z > (retail_room.z1() + get_window_vspace()));
}
bool building_t::is_valid_ai_placement(point const &pos, float radius, bool skip_nocoll, bool no_check_objs) const { // for people and animals
	if (!is_pos_inside_building(pos, radius, radius, 1))       return 0; // required for attic; for_attic=1
	if (point_in_water_area(pos) || is_above_retail_area(pos)) return 0; // excludes glass floors, in case they're not connected to the lower floor by escalators
	cube_t ai_bcube(pos);
	ai_bcube.expand_by(radius); // expand more in Z?
	if (!is_valid_stairs_elevator_placement(ai_bcube, ai_bcube, radius)) return 0; // Note: will double pad by radius

	// Note: people are placed before room geom is generated for all buildings, so this may not work and will have to be handled during room geom placement
	if (!no_check_objs && interior->room_geom) { // check placement against room geom objects
		auto objs_end(interior->room_geom->get_placed_objs_end()); // skip buttons/stairs/elevators

		for (auto i = interior->room_geom->objs.begin(); i != objs_end; ++i) {
			if (skip_nocoll && i->no_coll()) continue;
			if (i->type == TYPE_FLOORING || i->type == TYPE_BLOCKER || i->type == TYPE_POOL_TILE) continue; // okay to place on flooring; ignore blockers (used for placement clearance)
			if (i->intersects(ai_bcube)) return 0;
		}
	}
	return 1;
}

struct room_cand_t {
	unsigned room_ix, floor_ix;
	room_cand_t(unsigned r, unsigned f) : room_ix(r), floor_ix(f) {}
};

bool building_t::place_people_if_needed(unsigned building_ix, float radius) const {
	if (!interior || interior->rooms.empty() || is_rotated()) return 0; // no people in these cases
	if (interior->placed_people) return 0; // already placed
	interior->placed_people = 1; // set, even if no people are placed below
	if (!global_building_params.gen_building_interiors) return 0; // no interiors, no people
	unsigned num_min(is_house ? global_building_params.people_per_house_min : global_building_params.people_per_office_min);
	unsigned num_max(is_house ? global_building_params.people_per_house_max : global_building_params.people_per_office_max);
	if (num_max < num_min) {swap(num_min, num_max);} // or error? if so, it should be checked during option processing
	if (num_max == 0) return 0;
	rand_gen_t rgen;
	rgen.set_state(building_ix+1, mat_ix); // should be canonical per building
	rgen.rand_mix();
	unsigned num_people(num_min + (rgen.rand()%(num_max - num_min + 1)));
	if (num_people == 0) return 0;
	float const floor_thickness(get_floor_thickness()), fc_thick(0.5*floor_thickness);
	// weight rooms by number of floors to avoid placing too many people in basements
	static vector<room_cand_t> room_cands;
	room_cands.clear();
	unsigned first_basement_room(0);

	for (auto r = interior->rooms.begin(); r != interior->rooms.end(); ++r) { // add room_cands
		if (r->is_sec_bldg)                              continue; // don't place people in garages and sheds
		if (!ALLOW_AI_IN_MALLS && r->is_mall_or_store()) continue; // don't place people in malls or stores
		//if (has_mall() && !r->is_mall_or_store())        continue; // TESTING - only place in malls
		if (min(r->dx(), r->dy()) < 3.0*radius)          continue; // room to small to place a person
		unsigned const room_ix(r - interior->rooms.begin());
		if (interior->pool.valid && (int)room_ix == interior->pool.room_ix) continue; // don't place in pool room so that we don't have to check for pool collisions
		float const floor_spacing(get_room_floor_spacing(*r));
		unsigned const num_floors(calc_num_floors_room(*r, floor_spacing, floor_thickness));
		assert(num_floors > 0);
		if (first_basement_room == 0 && r->z1() < ground_floor_z1) {first_basement_room = room_cands.size();}
		vect_door_stack_t const &doorways(get_doorways_for_room(*r, 0.0, 1)); // get interior doors; all_floors=1

		for (unsigned f = 0; f < num_floors; ++f) {
			if (r->lit_by_floor && !r->is_lit_on_floor(f)) continue; // don't place person in an unlit room; only applies if room geom has been generated
			float const zval(r->z1() + f*floor_spacing);
			if (has_water() && r->intersects(get_water_cube()) && zval < interior->water_zval) continue; // don't place in a room with water on the floor
			
			if (!r->is_hallway && !r->has_stairs_on_floor(f) && !r->is_single_large_room()) { // check if this room has all closed/locked doors
				float const z_test(zval + 0.5*floor_spacing); // mid-floor height
				bool any_usable_doors(0);

				for (door_stack_t const &ds : doorways) {
					assert(ds.first_door_ix < interior->doors.size());

					for (unsigned dix = ds.first_door_ix; dix < interior->doors.size(); ++dix) {
						door_t const &door(interior->doors[dix]);
						if (!ds.is_same_stack(door)) break; // moved to a different stack, done
						if (door.z1() > z_test || door.z2() < z_test) continue; // wrong floor
						if ((global_building_params.ai_opens_doors >= 2) ? !door.is_closed_and_locked() : door.open) {any_usable_doors = 1; break;}
					}
					if (any_usable_doors) break;
				} // for i
				if (!any_usable_doors && has_mall() && r->is_store()) { // check mall store doorways; uses initial closed state
					for (store_doorway_t const &d : interior->mall_info->store_doorways) {
						if (!d.closed && d.room_id == room_ix) {any_usable_doors = 1; break;}
					}
				}
				if (!any_usable_doors) continue; // skip
			}
			unsigned const num_cands(r->is_backrooms() ? 4 : 1); // add 4x for backrooms since this is one room with many sub-rooms (even if multi-level?)
			for (unsigned n = 0; n < num_cands; ++n) {room_cands.emplace_back(room_ix, f);}
		} // for f
	} // for r
	min_eq(num_people, (unsigned)room_cands.size()); // don't place more people than we have rooms (notably for factories with 3 single floor rooms total)
	person_t person(radius);

	for (unsigned N = 0; N < num_people; ++N) {
		unsigned const max_cand_ix((N == 0 && first_basement_room > 0) ? first_basement_room : room_cands.size()); // place the first person in a non-basement room

		for (unsigned n = 0; n < 10; ++n) { // make 10 attempts at choosing a room
			unsigned const cand_ix(rgen.rand() % max_cand_ix);
			room_cand_t const &cand(room_cands[cand_ix]);
			room_t const &room(get_room(cand.room_ix));
			float const zval(room.z1() + fc_thick + get_room_floor_spacing(room)*cand.floor_ix);
			bool success(0);

			for (unsigned m = 0; m < 100; ++m) { // make 100 attempts at choosing a position in this room
				person.pos = gen_xy_pos_in_area(room, radius, rgen, zval); // random XY point inside this room
				if (!is_valid_ai_placement(person.pos, radius, 1)) continue; // skip_nocoll=1
				person.ssn = interior->people.size();
				interior->people.push_back(person); // other params will be filled in later
				success = 1;
				break; // done/success
			} // for n
			if (success) {
				// don't place another person on this floor of this room unless there aren't enough rooms for everyone
				if (room_cands.size() >= (num_people - N)) {room_cands.erase(room_cands.begin() + cand_ix);}
				break;
			}
		} // for n
	} // for N
	// add people to beds; has_room_geom() is usually true; this adds people to jail cell beds as well, but only for houses, apartments, and hotels
	if (has_room_geom() && (is_residential() || is_hospital() /*|| interior->has_jail*/)) {
		auto objs_end(interior->room_geom->get_placed_objs_end()); // skip buttons/stairs/elevators

		for (auto i = interior->room_geom->objs.begin(); i != objs_end; ++i) {
			if (i->type == TYPE_BED) {
				bool const in_jail(i->flags & RO_FLAG_IN_JAIL);
				if (rgen.rand_float() > (in_jail ? 0.15 : 0.1)) continue; // 10% chance for residential beds, 15% for jail beds
				cube_t cubes[6]; // frame, head, foot, mattress, pillow, legs_bcube
				get_bed_cubes(*i, cubes);
				person.pos = cube_top_center(cubes[3]); // center of the mattress
				// if there are two pillows, move to a random side
				if (bed_is_wide(*i)) {person.pos[!i->dim] += (rgen.rand_bool() ? -1.0 : 1.0)*0.265*cubes[4].get_sz_dim(!i->dim);}
			}
			else if (i->type == TYPE_HOSP_BED) {
				if (rgen.rand_float() > 0.2) continue; // 20% chance
				person.pos = cube_top_center(*i);
				switch (i->item_flags % 3) {
				case 0: person.pos.z -= 0.16*i->dz(); break; // flat bed used in patient rooms and exam rooms
				case 1: person.pos.z -= 0.10*i->dz(); break; // flat bed used in patient rooms
				case 2: continue; // electric bed; skip since it's inclined rather than flat
				}
			}
			else if (i->type == TYPE_OP_TABLE) {
				if (rgen.rand_float() > 0.1) continue; // 25% chance
				person.pos.assign(i->xc(), i->yc(), (i->z2() - 0.03*i->dz()));
				person.pos[i->dim] += (i->dir ? 1.0 : -1.0)*0.05*i->get_length(); // move up a bit
			}
			else {continue;}
			person.dir = vector_from_dim_dir(i->dim, (i->dir ^ (i->type != TYPE_HOSP_BED))); // hospital bed dir is backwards from bed/OR table
			person.ssn = interior->people.size();
			person.lying_down = 1;
			interior->people.push_back(person);
			i->flags |= RO_FLAG_USED; // mark bed as occupied
		} // for i
	}
	return 1;
}

bool can_ai_follow_player(person_t const &person, bool allow_diff_building, bool allow_outside_room) {
	if (!ai_follow_player()) return 0; // disabled
	if (!cur_player_building_loc.is_valid() && !allow_outside_room) return 0; // no target
	if (!allow_diff_building && cur_player_building_loc.building_ix != person.cur_bldg) return 0; // wrong building
	if (player_is_hiding && !person.saw_player_hide) return 0; // ignore player in closet, bathroom stall, or shower with door closed, and we didn't see them hide
	if (person.retreat_time > 0.0) return 0; // ignore the player if retreating
	// handle elevator case; this isn't perfect because it doesn't handle the case of player and zombie in adjacent elevators, but I've never seen that failure mode
	if (player_in_elevator != (person.ai_state >= AI_ENTER_ELEVATOR)) return 0; // {player, zombie) in elevator, and {zombie, player} is not
	return 1;
}
bool has_nearby_sound(person_t const &person, float floor_spacing) {
	if (!can_ai_follow_player(person)) return 0; // no need to track sounds
	point sound_pos; // unused
	return get_closest_building_sound(person.pos, sound_pos, floor_spacing);
}

bool building_t::point_in_or_above_pool(point const &pos) const {
	return (has_pool() && interior->pool.contains_pt_xy(pos) && pos.z < get_pool_room().z2());
}
bool building_t::same_room_and_floor_as_player(person_t const &person) const {
	return (cur_player_building_loc.room_ix == person.cur_room && cur_player_building_loc.floor_ix == get_floor_for_zval(person.pos.z) && cur_player_building_loc.stairs_ix < 0);
}
bool building_t::is_player_visible(person_t const &person, unsigned vis_test) const {
	building_dest_t const &target(cur_player_building_loc);
	if (point_in_or_above_pool(target.pos)) return 0; // don't follow the player into the pool (including player above the pool)
	if (vis_test == 0) return 1; // no visibility test
	float const player_radius(get_scaled_player_radius());
	point const pp2(target.pos - vector3d(0.0, 0.0, get_player_height())); // player's bottom sphere
	bool const same_room(person.cur_room >= 0 && cur_player_building_loc.room_ix == person.cur_room);
	unsigned const person_floor_ix(get_floor_for_zval(person.pos.z));
	unsigned const floor_delta(abs((int)cur_player_building_loc.floor_ix - (int)person_floor_ix));
	bool const same_room_and_floor(same_room && floor_delta == 0); // Note: doesn't check cur_player_building_loc.stairs_ix
	bool has_los(0);

	if (same_room_and_floor) {
		room_t const &room(get_room(person.cur_room));

		if (room.is_single_large_room() && has_room_geom() && !room.is_mall()) { // rooms with additional occluders
			point const viewer(person.get_eye_pos());
			cube_t occ_area(target.pos);
			occ_area.expand_by(player_radius);
			occ_area.union_with_pt(viewer); // any occluder must intersect this cube
			point pts[5]; // center, -x, +x, -y, +y; no need to check -z/+z
			get_sphere_boundary_pts(target.pos, player_radius, pts, 1); // skip_z=1

			if (room.is_retail()) { // retail room
				if (check_shelfrack_occlusion(viewer, pts, 5, occ_area)) return 0; // blocked by a shelfrack
			}
			else if (is_warehouse()) {
				if (check_warehouse_shelf_occlusion(viewer, pts, 5, occ_area)) return 0; // blocked by a shelf
			}
			else { // parking garage or backrooms
				if (check_pg_br_wall_occlusion(viewer, pts, 5, occ_area, (viewer - target.pos))) return 0; // blocked by a wall
			}
		}
		else {has_los = 1;} // assume that we have a line of sight if we're in the same room and floor as the player (optimization)
	}
	if (!has_los && same_room && floor_delta == 1 && person.pos.z > ground_floor_z1) {
		// if the person and the player are on adjacent floors of the same room connected by stairs (and not in a parking garage), cheat and say they have a line of sight;
		// what about U-shaped stairs in office building hallways?
		room_t const &room(get_room(person.cur_room));
		unsigned const room_floor_start(get_floor_for_zval(room.z1()));
		assert(room_floor_start <= cur_player_building_loc.floor_ix && room_floor_start <= person_floor_ix);
		unsigned const f1(cur_player_building_loc.floor_ix - room_floor_start), f2(person_floor_ix - room_floor_start);
		has_los = (room.has_stairs_on_floor(f1) && room.has_stairs_on_floor(f2));
	}
	if (!has_los) { // check visibility; assume LOS if in the same room
		point const eye_pos(person.get_eye_pos());
		if (!is_sphere_visible(target.pos, player_radius, eye_pos) && !is_sphere_visible(pp2, player_radius, eye_pos)) return 0; // check both the bottom and top of player
	}
	if (vis_test >= 2) { // check person FOV
		//if (dot_product((pp2 - eye_pos).get_norm(), person.dir) < 0.5) return 0; // 60 degree FOV => dp < 0.5
		float const view_dist(10.0*get_window_vspace()); // 10 stories (~100 feet) should be good enough
		pos_dir_up pdu(person.pos, person.dir, plus_z, 0.0, 0.1*person.radius, view_dist, 0.0, 1); // auto perspective angle, no_zoom=1
		if (!pdu.sphere_visible_test(target.pos, player_radius) && !pdu.sphere_visible_test(pp2, player_radius)) return 0;
	}
	if (vis_test >= 3 && !same_room_and_floor) { // check lit state if not in the same room and floor
		if (!is_sphere_lit(target.pos, player_radius) && !is_sphere_lit(pp2, player_radius)) return 0; // check both the bottom and top of player
	}
	return 1;
}
bool building_t::can_target_player(person_t const &person) const {
	if (!can_ai_follow_player(person))    return 0;
	if (point_in_escalator  (person.pos)) return 0; // don't update target when entering or exiting an escalator
	if (point_in_U_stairwell(person.pos)) return 0; // don't update target when on U-shaped stairs or stairs landing
	if (player_on_escalator)              return 0; // don't follow player on escalator; wait until player reaches the floor above or below
	// if we're on an upper retail glass floor, we follow the player if and only if they are also on the floor
	if (point_over_glass_floor(person.pos)) {return point_over_glass_floor(cur_player_building_loc.pos);}
	assert(cur_player_building_loc.room_ix >= 0);
	room_t const &player_room(get_room(cur_player_building_loc.room_ix));
	float const first_floor_zceil(player_room.z1() + get_window_vspace());
	if (player_room.is_single_floor && !player_room.is_mall() && cur_player_building_loc.pos.z > first_floor_zceil) return 0; // check for player on unreachable floor
	// if player is on the upper floor, don't update target (in case person is on stairs landing)
	if (person.pos.z > first_floor_zceil && point_over_glass_floor(cur_player_building_loc.pos)) return 0;
	return is_player_visible(person, global_building_params.ai_player_vis_test); // 0=no test, 1=LOS, 2=LOS+FOV, 3=LOS+FOV+lit
}

bool building_t::need_to_update_ai_path(person_t &person) const {
	if (!global_building_params.ai_target_player || !can_ai_follow_player(person) || !interior) return 0; // disabled
	if (point_in_or_above_pool(cur_player_building_loc.pos)) return 0; // don't follow the player into the pool (including player above the pool)
	building_dest_t const &target(cur_player_building_loc);
	bool const same_room(same_room_and_floor_as_player(person)); // check room and floor

	if (global_building_params.ai_player_vis_test == 0) { // the below logic only applies if there's no line of sight test and is an optimization
		// if the player's room has not changed, and the person is not yet in this room, continue on the same path to the dest room;
		// however, if the path is empty, continue to choose a new path (needed for AI more than one floor away from target to take multiple flights of stairs)
		if (target.same_room_floor(prev_player_building_loc) && !same_room && !person.on_new_path_seg && !person.path.empty()) return 0;
	}
	//if (same_room && person.path.size() > 1) return 0; // same room but path has a jog, continue on existing path (faster, but slower to adapt to player position change)
	if (dist_less_than(person.pos, target.pos, person.radius)) {person.missed_player_room_change = 0; return 0;} // already close enough
	if (person.on_fixed_path()) return 0; // don't change paths when on the stairs/ramp/escalator
	float const floor_spacing(get_window_vspace());

	if (get_floor_for_zval(target.pos.z) != get_floor_for_zval(prev_player_building_loc.pos.z)) { // if player did not change floors
		if (fabs(person.pos.z - target.pos.z) > 2.0f*floor_spacing) return 0; // person/player are > 2 floors apart, continue toward stairs (should it be one floor apart?) (optimization)
	}
	bool const goal_player(person.goal_type == GOAL_TYPE_PLAYER), player_room_changed(!target.same_room_floor(prev_player_building_loc));

	if (can_target_player(person)) { // have player visibility
		if (goal_player && !player_room_changed && !person.missed_player_room_change && !person.on_new_path_seg && !person.path.empty()) {
			if (!same_room) return 0;
			if (is_pos_in_pg_or_backrooms(person.pos) && ((person.ssn + frame_counter) & 3) != 0) return 0; // path finding is expensive, update every 4th frame
		}
		person.missed_player_room_change = 0;
		return 1;
	}
	else if (goal_player && player_room_changed) {person.missed_player_room_change = 1;}
	// if we were following the player, don't get distracted by a sound such as another zombie moaning
	if (goal_player && person.target_valid() && !person.path.empty()) return 0;
	if (has_nearby_sound(person, floor_spacing)) {person.missed_player_room_change = 0; return 1;} // new sound source; ignore player as they're not visible
	return 0; // continue on the previously chosen path
}

// technically, this isn't const because it modifies the saw_player_hide of people, but it doesn't actually modify the building itself
void building_t::register_player_hiding(room_object_t const &hiding_obj) const {
	player_is_hiding  = 1;
	player_hiding_obj = hiding_obj; // cache this object for later use, for example so that we can open a closet door to find the player
	if (!global_building_params.ai_sees_player_hide) return; // no setting of saw_player_hide
	bool const repeat(frame_counter <= player_hiding_frame+1); // player was hiding in prev frame - not a new hiding spot
	player_hiding_frame = frame_counter;
	if (repeat)    return;
	if (!interior) return; // error?
	if (!ai_follow_player()) return; // no AI update
	assert(cur_player_building_loc.is_valid());
	if (!cur_player_building_loc.is_valid()) return; // no target

	for (person_t &person : interior->people) {
		assert(cur_player_building_loc.building_ix == person.cur_bldg);
		if (person.retreat_time > 0.0) continue; // ignore the player if retreating
		person.saw_player_hide = is_player_visible(person, max(global_building_params.ai_player_vis_test, 1U)); // at least LOS test
	}
}

void building_t::all_ai_room_update(rand_gen_t &rgen, float delta_dir) {
	assert(interior);

	for (unsigned i = 0; i < interior->people.size(); ) { // Note: no increment
		person_t &person(interior->people[i]);
		person.ai_state = ai_room_update(person, delta_dir, i, rgen);
		if (person.ai_state == AI_TO_REMOVE) {interior->people.erase(interior->people.begin() + i);} // remove this person
		else {++i;}
	}
}

float get_person_max_move_dist(person_t const &person, float speed_mult=1.0) {
	return person.speed*speed_mult*min(fticks, 4.0f); // clamp fticks to 100ms
}
float move_person_forward_to_target(person_t &person) { // Note: expected that person.target_pos.z == person.pos.z
	float const move_dist(get_person_max_move_dist(person));
	vector3d const delta(person.target_pos - person.pos);
	person.pos       += (move_dist/delta.mag())*delta;
	person.anim_time += move_dist;
	return move_dist;
}
bool person_slow_turn(person_t &person, point const &target, float delta_dir) {
	vector3d dir_to_target((target.x - person.pos.x), (target.y - person.pos.y), 0.0); // zval is always 0
	dir_to_target.normalize();
	if (dot_product(person.dir, dir_to_target) > 0.99) return 0; // turn is complete
	// if directions are nearly opposite, pick a side to turn using the cross product to get an orthogonal vector
	if (dot_product(dir_to_target, person.dir) < -0.9) {
		dir_to_target = cross_product(person.dir, plus_z)*((person.ssn & 1) ? -1.0 : 1.0); // use SSN to select a turn direction (CW/CCW)
	}
	person.dir = delta_dir*dir_to_target + (1.0 - delta_dir)*person.dir; // merge new_dir into dir gradually for smooth turning
	person.dir.normalize();
	return 1;
}

void building_t::call_elevator_to_floor_and_light_nearest_button(elevator_t &elevator, unsigned floor_ix, bool is_inside_elevator, bool is_up) {
	call_elevator_to_floor(elevator, floor_ix, is_inside_elevator, is_up);
	// light the button that was pressed
	if (!has_room_geom()) return; // can't light the button
	assert(elevator.button_id_start <= elevator.button_id_end && elevator.button_id_end <= interior->room_geom->objs.size());

	for (auto i = interior->room_geom->objs.begin() + elevator.button_id_start; i != interior->room_geom->objs.begin() + elevator.button_id_end; ++i) {
		if (i->type == TYPE_BLOCKER || i->type == TYPE_ELEC_WIRE) continue; // button was removed?
		assert(i->type == TYPE_BUTTON);
		if (i->obj_id != floor_ix) continue; // wrong floor
		if (i->in_elevator() != is_inside_elevator) continue; // wrong inside/outside
		if (!is_inside_elevator && (i->flags & (is_up ? RO_FLAG_ADJ_BOT : RO_FLAG_ADJ_TOP))) continue; // wrong up/down button
		i->flags |= RO_FLAG_IS_ACTIVE; // set active/lit state
		interior->room_geom->invalidate_small_geom(); // need to regen object data due to lit state change; should be thread safe
		break; // only one button
	} // for i
}

bool building_t::run_ai_pool_logic(person_t &person, float &speed_mult) const {
	float zval(0.0);
	if (!get_zval_for_pool_bottom(person.pos, zval)) return 0; // not in the pool
	float const z_feet(person.get_z1()), z_head(person.get_z2());
	indoor_pool_t const &pool(interior->pool);
	room_t const &pool_room(get_room(pool.room_ix));
	// calculate new height and move pos.z
	zval += (person.pos.z - z_feet); // adjust to correct person height
	float const dz(zval - person.pos.z), max_dz(0.1*fticks*CAMERA_RADIUS);
	if (dz > 0.0) {person.pos.z += min( dz, max_dz);} // climbing stairs or ramp
	else          {person.pos.z -= min(-dz, max_dz);} // falling
	float const radius(person.radius); // Note: not using COLL_RADIUS_SCALE here
	
	if (dz > -radius && !pool.bottomless) { // move toward the stairs once we're near the bottom, unless it's bottomless
		cube_t pool_exp(pool);
		pool_exp.expand_by_xy(radius);
		vector3d move_dir;
		move_dir[pool.dim] = (pool.dir ? 1.0 : -1.0);
		person.target_pos  = person.pos + move_dir*(pool_exp.dx() + pool_exp.dy()); // move toward the pool exit in X or Y
		person.target_pos[!pool.dim] = max((pool.d[!pool.dim][0] + 2.0f*radius), min((pool.d[!pool.dim][1] - 2.0f*radius), person.target_pos[!pool.dim])); // avoid railings
		pool_exp.clamp_pt_xy(person.target_pos); // slightly outside the pool
	}
	// force person inside the pool walls
	float const fall_amt(min(1.0f, ((pool.z2() - z_feet) / (z_head - z_feet))));
	cube_t pool_clip(pool);
	pool_clip.expand_by_xy(-fall_amt*radius); // increase spacing as they fall
	pool_clip.d[pool.dim][pool.dir] = pool_room.d[pool.dim][pool.dir]; // no clipping at the pool exit end
	pool_clip.clamp_pt_xy(person.pos);
	speed_mult *= (1.0 - 0.4*fall_amt); // slower when in the water
	person.in_pool = 1;
	return 1; // in the pool
}

bool building_t::run_ai_tunnel_logic(person_t &person, float &speed_mult) const {
	if (person.in_pool || person.retreat_time > 0.0) return 0;
	if (interior->tunnels.empty() || !point_in_extended_basement(person.pos)) return 0;
	int const tunnel_ix(interior->get_tunnel_ix_for_point(person.pos));
	bool const in_tunnel(tunnel_ix >= 0), target_in_tunnel(player_in_tunnel && cur_player_building_loc.building_ix == person.cur_bldg && ai_follow_player());
	float const radius(COLL_RADIUS_SCALE*person.radius);
	point const dest(cur_player_building_loc.pos.x, cur_player_building_loc.pos.y, person.pos.z);

	if (person.in_tunnel) {
		if (!in_tunnel && get_room_containing_pt(person.pos) >= 0) { // must check both conditions if case person was pushed
			person.in_tunnel = 0;
			return 0;
		}
		if (target_in_tunnel) {
			if (dest == person.pos) return 0; // already at dest
			if (!interior->get_tunnel_path_two_pts(person.pos, dest, radius, person.path)) return 0; // tunnel => tunnel
			assert(!person.path.empty());
			speed_mult      *= 2.0; // faster
			person.goal_type = GOAL_TYPE_PLAYER;
		}
		else { // target not in tunnel
			unsigned room_ix(0);
			if (!interior->get_tunnel_path_to_room(person.pos, room_ix, person.path)) return 0; // tunnel => room
			room_t const &room(get_room(room_ix));
			cube_t room_inner(get_walkable_room_bounds(room));
			room_inner.expand_by_xy(-radius);
			assert(!person.path.empty());
			point pt_in_room(person.path.back());
			room_inner.clamp_pt_xy(pt_in_room);
			person.path.add(pt_in_room);
			person.goal_type = GOAL_TYPE_ROOM;
		}
		person.path.erase(person.path.begin()); // remove person.pos
		reverse(person.path.begin(), person.path.end());

		if (in_tunnel) { // check current tunnel segment and make sure we're in the walkable area
			point pos_clamped(person.pos);
			interior->tunnels[tunnel_ix].get_walk_area(person.pos, radius).clamp_pt_xy(pos_clamped);
			if (pos_clamped != person.pos) {person.path.add(pos_clamped);}
		}
		if (!person.path.empty()) {person.next_path_pt(1);}
		return 1;
	}
	else { // not currently in the tunnel
		if (in_tunnel) { // newly entering tunnel
			person.in_tunnel = 1;
			return 0;
		}
		if (target_in_tunnel) { // not in tunnel, but want to be in tunnel
			if (dest == person.pos) return 0; // already at dest
			int const room_ix(get_room_containing_pt(person.pos));
			if (room_ix < 0) return 0;

			// Note: doesn't check for obstacles in the room; this can lead to zombies walking through boxes and crates in storage rooms, and chairs;
			// can we call interior->get_avoid_cubes() and then nav_graph->connect_room_endpoints() here? wait until AI is past the center of the room?
			if (interior->get_tunnel_path_from_room(dest, room_ix, radius, person.path)) { // room => tunnel
				reverse(person.path.begin(), person.path.end());
				if (!person.path.empty()) {person.next_path_pt(1);}
				person.goal_type = GOAL_TYPE_PLAYER;
				return 1;
			}
		}
	}
	return 0;
}

int building_t::run_ai_elevator_logic(person_t &person, float delta_dir, rand_gen_t &rgen) {
	assert(person.goal_type == GOAL_TYPE_ELEVATOR);
	if (!has_room_geom()) return person.ai_state; // if player has moved away and room_geom was deleted, remain in the current state
	elevator_t &e(get_elevator(person.cur_elevator));
	float const window_vspace(get_window_vspace()), floor_spacing(get_elevator_floor_spacing(e));
	room_object_t const &ecar(interior->get_elevator_car(e));

	// Note: when AI_LOCKS_ELEVATOR==1, only one person can be in the elevator at once, including walking into and out of the elevator
	// Note: if interior->elevators_disabled==1, the AI will either give up waiting or get stuck in the elevator if already inside
	if (person.ai_state == AI_WAIT_ELEVATOR) {
		// waiting for the elevator to arrive; we allow the AI to be pushed and to give up if waiting too long;
		// when the elevator opens, we could check e.going_up to see if it's headed in the correct direction;
		// however, since we haven't decided on a dest floor yet, it makes sense to just go with it and select a floor in the direction the elevator is headed
		point const elevator_center(e.xc(), e.yc(), person.pos.z);
		unsigned const cur_person_floor(get_elevator_floor(person.pos.z, e, floor_spacing));

		if (e.open_amt == 1.0) { // doors are fully open
			if (get_elevator_floor(ecar.zc(), e, floor_spacing) == cur_person_floor) { // wait for elevator to reach our current floor
				if (e.num_occupants < global_building_params.elevator_capacity) { // we can fit
					person.dir = (elevator_center - person.pos).get_norm(); // snap our direction to forward, in the rare case the elevator arrives before we've completed our turn
					person.waiting_start = 0.0; // no longer waiting for elevator
					++e.num_occupants; // make space for ourselves in the elevator
					return AI_ENTER_ELEVATOR;
				}
				// check if someone is exiting the elevator on this floor
				bool has_space(0);

				for (person_t const &other : interior->people) {
					if (&other == &person) continue; // skip ourself
					if (other.cur_elevator != person.cur_elevator || other.dest_elevator_floor != cur_person_floor) continue; // different elevator or floor
					if (other.ai_state != AI_RIDE_ELEVATOR && other.ai_state != AI_EXIT_ELEVATOR) continue; // not ready to exit
					has_space = 1;
					break;
				}
				if (!has_space) { // we can't fit
					if (person.must_re_call_elevator) {
						// waiting has already been decided
					}
					else if (rgen.rand_probability(global_building_params.elevator_wait_recall_prob)) { // wait and press the button again some of the time
						person.must_re_call_elevator = 1; // schedule this for later, to avoid causing the doors to re-open
					}
					else { // give up and walk away 50% of the time
						person.waiting_start = 0;
						person.abort_dest();
						return AI_WAITING;
					}
				}
				else {
					// in this case we currently walk through the other person who is exiting the elevator
				}
			}
		}
		if (!global_building_params.allow_elevator_line /*|| global_building_params.elevator_capacity = 1*/) {
			// check for other people who were waiting for the elevator first, and move on if the spot is taken to avoid forming a line of people that get in each other's ways;
			// note that it would be better to do this check before reaching the elevator to avoid bumping another person or getting stuck on them
			for (person_t const &other : interior->people) {
				if (other.waiting_start > person.waiting_start) continue; // we were here first
				if (!person.waiting_for_same_elevator_as(other, floor_spacing)) continue;
				// other person was there first, stop waiting and do something else
				person.waiting_start = 0;
				person.abort_dest();
				return AI_WAITING;
			} // for person
		}
		if (person.get_bcube().intersects(e)) { // check if we've been pushed to intersect the elevator
			// try to get back to our waiting pos by moving back to the non-elevator waiting state and clearing our last_used_elevator flag;
			// this will make the person either choose the same elevator again, or give up and choose a non-elevator dest
			person.last_used_elevator = 0;
			person.waiting_start      = 0;
			return AI_WAITING;
		}
		// re-press the call button when the elevator has moved from this floor
		if (person.must_re_call_elevator && get_elevator_floor(ecar.zc(), e, floor_spacing) != cur_person_floor) {
			call_elevator_to_floor_and_light_nearest_button(e, cur_person_floor, 0, (person.dest_elevator_floor > cur_person_floor));
			person.must_re_call_elevator = 0;
		}
		person_slow_turn(person, elevator_center, 0.5*delta_dir); // slow turn to face the elevator
	}
	else if (person.ai_state == AI_ENTER_ELEVATOR) {
		// move to the elevator center; maybe if capacity > 1 and someone else is in the elevator, we should pick a side and have them move to the other side?
		// this seems difficult to coordinate without some more detailed collision checks
		person.target_pos.assign(e.xc(), e.yc(), person.pos.z);
		float const move_dist(move_person_forward_to_target(person)); // walk into elevator

		if (dist_xy_less_than(person.pos, person.target_pos, move_dist)) {
			person.anim_time = 0.0; // stop and turn
			return AI_ACTIVATE_ELEVATOR;
		}
		e.hold_doors = e.hold_movement = 1; // keep the doors from closing on this person as they enter
	}
	else if (person.ai_state == AI_ACTIVATE_ELEVATOR) {
		vector3d const prev_dir(person.dir);
		bool const is_turning(person_slow_turn(person, get_pos_to_stand_for_elevator(e, window_vspace, person.pos.z), 0.5*delta_dir)); // slow turn to face the elevator door

		if (!is_turning) { // turn completed
			call_elevator_to_floor_and_light_nearest_button(e, person.dest_elevator_floor, 1, 0); // is_inside_elevator=1, is_up=0
			person.anim_time = 0.0; // stop and wait
			return AI_RIDE_ELEVATOR;
		}
		e.hold_movement = 1; // keep the elevator from moving until we press the button and are ready to ride it, otherwise it may reverse direction due to press
	}
	else if (person.ai_state == AI_RIDE_ELEVATOR) {
		person.pos.z = ecar.z1() + person.radius + get_fc_thickness(); // move with the elevator

		if (e.open_amt == 1.0) { // doors are fully open
			// Note: we could probably query e.get_target_floor() for this
			if (get_elevator_floor(ecar.zc(), e, floor_spacing) == person.dest_elevator_floor) {return AI_EXIT_ELEVATOR;} // at destination floor
		}
		person.is_stopped = 1; // enable idle animation
		person.idle_time += fticks;
	}
	else if (person.ai_state == AI_EXIT_ELEVATOR) {
		point const target_dest(get_pos_to_stand_for_elevator(e, window_vspace, person.pos.z));
		bool const is_inside_elevator(e.contains_pt_xy(person.pos));

		// as long as we're inside the elevator, plan to exit to the designated point in front of the elevator
		if (is_inside_elevator) {person.target_pos = target_dest;}
		// once we no longer intersect the elevator, we're free to turn to avoid people
		else {
			if (person.target_pos == target_dest) { // if we already chose a new target_pos, stick with that and don't udpate it
				for (person_t const &other : interior->people) {
					if (!person.waiting_for_same_elevator_as(other, floor_spacing)) continue;
					float const r_sum(COLL_RADIUS_SCALE*(person.radius + other.radius));
					// check if someone else is waiting here and choose another spot nearby that's free; note that this doesn't check for intersecting paths
					if (!dist_xy_less_than(person.target_pos, other.pos, r_sum)) continue; // target pos is not intersecting
					vector3d const delta(person.target_pos - person.pos), tangent(cross_product(delta, plus_z).get_norm());
					point new_cands[2]; // candidate points to the left and right of the other person
					for (unsigned d = 0; d < 2; ++d) {new_cands[d] = other.pos + ((d ? -1.0 : 1.0)*1.1*r_sum)*tangent;} // move a bit further than radius away
					bool const best_ix(p2p_dist_xy_sq(person.target_pos, new_cands[1]) < p2p_dist_xy_sq(person.target_pos, new_cands[0]));
					person.target_pos.x = new_cands[best_ix].x; person.target_pos.y = new_cands[best_ix].y; // choose the candidate point closer to our target pos
					break; // only handle one person, since iterating may cause us to intersect an earlier person
				} // for person
			}
			if (person.target_pos != target_dest) {person_slow_turn(person, person.target_pos, 0.5*delta_dir);} // update direction; should already be facing close to this way
		}
		float const move_dist(move_person_forward_to_target(person)); // exit elevator to a point in front, then select a new non-elevator destination

		if (dist_xy_less_than(person.pos, person.target_pos, move_dist)) { // out of the elevator - done
			person.abort_dest();
			//assert(e.num_occupants > 0); // invalid if people are despawned/respawned when the player is far away?
			if (e.num_occupants > 0) {--e.num_occupants;} // we're no longer in this elevator
			return AI_AT_DEST;
		}
		e.hold_doors = e.hold_movement = 1; // keep the doors from closing on this person as they exit
	}
	else {assert(0);} // invalid state
	return person.ai_state; // fallthrough case, no change to ai_state
}

bool zombie_in_attack_range(person_t &person) {
	// use zval of the feet to handle cases where the person and the player are different heights
	float const player_height(get_bldg_player_height());
	point const feet_pos(person.pos.x, person.pos.y, person.get_z1()), player_feet_pos(cur_player_building_loc.pos - vector3d(0.0, 0.0, player_height));
	return (fabs(feet_pos.z - player_feet_pos.z) < 0.5*player_height && dist_less_than(feet_pos, player_feet_pos, 1.2f*(person.radius + building_t::get_scaled_player_radius())));
}
void clamp_person_to_building_bcube(point &pos, cube_t bcube, float radius, float fc_thick) {
	bcube.z1() += fc_thick + radius; // make sure feet are above the floor
	bcube.clamp_pt(pos);
}

// Note: non-const because this updates room light and door state
int building_t::ai_room_update(person_t &person, float delta_dir, unsigned person_ix, rand_gen_t &rgen) {

	if (person.speed == 0.0 || person.lying_down) {person.anim_time = 0.0; return AI_STOP;} // stopped
	assert(interior);
	if (!interior->room_geom && frame_counter < 60) {person.anim_time = 0.0; return AI_WAITING;} // wait until room geom is generated for this building
	float const radius(person.radius), coll_dist(COLL_RADIUS_SCALE*radius), floor_spacing(get_window_vspace()), fc_thick(get_fc_thickness());
	float &wait_time(person.waiting_start); // reuse this field
	float const base_speed_mult(is_house ? 1.0 : 1.2); // 20% faster in office buildings, if targeting the player, player last pos, or sound
	float speed_mult((person.goal_type >= GOAL_TYPE_PLAYER) ? base_speed_mult : 1.0);
	person.following_player = person.is_stopped = 0; // reset for this frame
	// skip the same building check for coll if both this person and the player may be in different but connected buildings
	bool allow_diff_building(interior->conn_info && person.pos.z < ground_floor_z1 && cur_player_building_loc.pos.z < ground_floor_z1 &&
		dist_xy_less_than(person.pos, cur_player_building_loc.pos, 2.0*floor_spacing));

	if (person.retreat_time > 0.0) {
		if (person.retreat_time == global_building_params.ai_retreat_time*TICKS_PER_SECOND) { // first retreating frame - clear path
			person.abort_dest();
			//person.is_first_path = 1; // probably not needed
		}
		wait_time = 0.0; // no waiting while retreating
		person.retreat_time -= fticks;
		max_eq(person.retreat_time, 0.0f);
	}
	if (wait_time > 0) { // waiting, possibly for an elevator
		person.idle_time += fticks;

		if (wait_time > fticks && !can_ai_follow_player(person, allow_diff_building)) { // waiting; don't wait if there's a player to follow
			if (!person.in_tunnel) { // check for other people colliding with this person and handle it; skip for tunnels
				bool was_bumped(0);

				for (auto p = interior->people.begin()+person_ix+1; p < interior->people.end(); ++p) {
					if (fabs(person.pos.z - p->pos.z) > coll_dist) continue; // different floors
					if (global_building_params.no_coll_enter_exit_elevator && person.ai_state == AI_WAIT_ELEVATOR && p->ai_state == AI_EXIT_ELEVATOR) continue;
					float const rsum(coll_dist + COLL_RADIUS_SCALE*p->radius);
					if (!dist_xy_less_than(person.pos, p->pos, rsum)) continue; // not intersecting
					move_person_to_not_collide(person, *p, person.pos, rsum, coll_dist); // if we get here, we have to actively move out of the way
					was_bumped = 1;
				} // for p
				if (was_bumped) {wait_time  = 0.0;} // stop waiting so that we can react
				else            {wait_time -= fticks;}
			}
			person.anim_time = 0.0; // reset just in case (though should already be at 0.0)

			if (person.ai_state != AI_WAIT_ELEVATOR) { // don't reset goal and return here if waiting at an elevator
				person.goal_type = GOAL_TYPE_NONE; // our orig goal is invalid
				return AI_WAITING;
			}
		}
		else { // wait is up
			wait_time = 0.0;
			person.abort_dest(); // force choose_dest=1 below
			person.ai_state = AI_WAITING; // waiting, but no longer for an elevator
		}
	}
	if (!point_in_building_or_basement_bcube(person.pos)) { // person must be inside the building
		cout << TXT(person.pos.str()) << TXT(bcube.str()) << TXT(interior->basement_ext_bcube.str()) << endl;
		assert(0);
	}
	debug_mode = (cur_player_building_loc.building_ix == person.cur_bldg); // used for debugging printouts
	//if (zombie_in_attack_range(person)) {cout << TXTi(person.goal_type) << TXTi(person.ai_state) << TXT(person.path.size()) << endl;} // TESTING
	bool const allow_outside_room(person.in_tunnel && player_in_tunnel); // zombies can attack the player in tunnels
	bool const zombie_attack_mode(can_ai_follow_player(person, allow_diff_building, allow_outside_room));
	
	if (zombie_attack_mode && zombie_in_attack_range(person)) { // check for player intersection/damage (even if zombie is in the elevator)
		if (!check_for_wall_ceil_floor_int(person.pos, cur_player_building_loc.pos, 1)) { // inc_pg_br_walls=1
			int const ret_status(register_ai_player_coll(person.has_key, person.get_height())); // return value: 0=no effect, 1=player is killed, 2=this person is killed
			// player is killed, we could track kills here
			if (ret_status == 1) {register_player_death(cur_player_building_loc.pos);}
			else if (ret_status == 2) {return AI_TO_REMOVE;} // player defeats zombie, remove it
		}
	}
	if (person.ai_state >= AI_WAIT_ELEVATOR) { // handle elevator case
		return run_ai_elevator_logic(person, delta_dir, rgen);
	}
	person.must_re_call_elevator = 0; // reset if we got out of the elevator logic with this set
	bool const has_rgeom(has_room_geom()), prev_in_pool(person.in_pool);
	person.in_pool = 0; // reset for this frame
	run_ai_pool_logic(person, speed_mult); // handle AI inside the pool; this can happen for zombies
	
	if (prev_in_pool && !person.in_pool) { // stepped out of the pool
		assert(has_pool());
		person.retreat_time = 0.5*TICKS_PER_SECOND; // retreat for 0.5s to avoid falling back into the pool chasing the player
		person.abort_dest(); // reset target to avoid walking back toward the pool
		max_eq(person.pos.z, (get_room(interior->pool.room_ix).z1() + fc_thick + radius)); // make sure completely out of pool
	}
	if (run_ai_tunnel_logic(person, speed_mult)) {person.no_wait_at_dest = 1;}
	build_nav_graph();

	if (zombie_attack_mode && player_is_hiding && person.saw_player_hide && global_building_params.ai_opens_doors && global_building_params.ai_sees_player_hide >= 2 &&
		player_hiding_obj.type != TYPE_NONE && same_room_and_floor_as_player(person) && cur_player_building_loc.building_ix == person.cur_bldg && has_rgeom)
	{
		// open the closet, stall, or shower door (closet door should already be open)
		// it may not be safe to use an object iterator or even an index since we're in a different thread, so do a linear search for the target object
		auto objs_end(interior->room_geom->get_placed_objs_end()); // skip buttons/stairs/elevators
		cube_t coll_cube(person.get_bcube());
		coll_cube.expand_by_xy(1.5*radius);

		for (auto i = interior->room_geom->objs.begin(); i != objs_end; ++i) {
			if (*i != player_hiding_obj)   continue;
			if (i->is_open())              break; // already open
			if (!i->intersects(coll_cube)) break; // too far away (uses full object, not door, but should be close enough)
			i->flags |= RO_FLAG_OPEN; // open the door
			play_open_close_sound(*i, person.pos);
			interior->room_geom->update_draw_state_for_room_object(*i, *this, 0);
			break; // done
		} // for i
	}
	bool choose_dest(!person.target_valid());
	bool const update_path(!person.in_pool && !person.in_tunnel && need_to_update_ai_path(person));
	// if room objects spawn in, select a new dest to avoid walking through objects based on our previous, possibly invalid path;
	// but not if this person is on stairs/ramp/escalator or an elevator, or they may end at an invalid zval between floors
	if (has_rgeom && !person.has_room_geom && !person.on_fixed_path() && person.ai_state < AI_ENTER_ELEVATOR) {person.abort_dest();}
	person.has_room_geom = has_rgeom;

	if (update_path) { // need to update based on player movement; higher priority than choose_dest
		bool const goal_ret(choose_dest_goal(person, rgen)); // check if person can reach the target

		if (!goal_ret || !find_route_to_point(person, coll_dist, 0, 1, person.path)) { // is_first_path=0, following_player=1
			choose_dest = 1; // or increment person.cur_rseed and return AI_WAITING? or restore person to prev value?
		}
		else { // success
			assert(!person.path.empty());
			person.next_path_pt(1);
			person.following_player = 1;
			choose_dest = 0;
			speed_mult  = 1.5*base_speed_mult; // faster when the player is in the same room
			// run logic to play zombie sounds
			bool const same_room_and_floor(same_room_and_floor_as_player(person));
			bool play_sound(same_room_and_floor); // always play sound if in the same room and floor; even if in backrooms?
			if (!play_sound && (person_ix & 1)) {play_sound |= is_player_visible(person, 1);} // 50% of zombies use line of sight test
			if (!play_sound && (person_ix & 2)) {play_sound |= has_nearby_sound (person, floor_spacing);} // 50% of zombies use sound test
			
			if (play_sound) { // alert other zombies if in the same room and floor as the player, except in backrooms/parking garage/retail/mall, unless the player is visible
				bool const alert_other_zombies(same_room_and_floor && (!is_single_large_room(person.cur_room) || is_player_visible(person, 1)));
				maybe_play_zombie_sound(person.pos, person_ix, alert_other_zombies);
			}
		}
	}
	if (choose_dest) { // no current destination, choose a new destination
		++person.cur_rseed; // update rseed so that this person will choose a different path if the previous path finding call failed
		if (!update_path) {person.anim_time = 0.0;} // reset animation if not a failed update path frame
		int const ret(choose_dest_room(person, rgen)); // 0=failed, 1=success, 2=failed but can retry

		if (ret == 2 && interior->door_state_updated) { // wait rather than stopping in case the player trapped this person in a room by closing the door
			person.wait_for(1.0); // stop for a second, then try again
			return AI_WAITING;
		}
		else if (ret != 1) { // if there's no valid room or valid path, set the speed to 0 so that we don't check this every frame
			if (!ai_follow_player()) { // if not following the player
				//person.speed = 0.0; // movement will be stopped from now on - not valid if we get into this state before gameplay is enabled
				person.wait_for(1.0); // stop for a second, then try again
			}
			return AI_STOP;
		}
		if (!find_route_to_point(person, coll_dist, person.is_first_path, 0, person.path)) { // following_player=0
			if (zombie_attack_mode && same_room_and_floor_as_player(person)) {
				// don't wait if in the room with the player because this creates animation transition jitter
			}
			else {
				float const wait_time(is_single_large_room(person.cur_room) ? 0.1 : 0.5);
				person.wait_for(wait_time); // stop for 0.5 second (0.1s for parking garage/backrooms/retail/mall), then try again
				person.goal_type = GOAL_TYPE_NONE; // reset just to be safe
			}
			return AI_WAITING;
		}
		if (has_rgeom) {person.is_first_path = 0;} // treat the path as the first path until room geom is generated
		if      (person.target_pos.z < person.pos.z) {person.prev_walked_down = 1;}
		else if (person.target_pos.z > person.pos.z) {person.prev_walked_down = 0;}
		assert(!person.path.empty());
		person.next_path_pt(1);
		return AI_BEGIN_PATH;
	}
	if (person.on_stairs()) {
		if (person.on_escalator()) {speed_mult *= (get_escalator(person.cur_escalator).is_powered ? 0.8 : 0.65);} // slower if escalator is unpowered
		else {speed_mult *= 0.9;} // stairs
	}
	float const max_dist(get_person_max_move_dist(person, speed_mult));
	float goal_dist(1.1f*max_dist);
	if (dot_product((person.target_pos - person.pos), person.dir) < 0.0) {max_eq(goal_dist, coll_dist);} // don't turn in place if dest is behind us and we're close
	person.on_new_path_seg = 0; // clear flag for this frame
	//person.following_player = can_target_player(person); // for debugging visualization

	// check if at dest; note that we use a different distance check for Z to account for model height changes when switching between people and zombie models
	if (dist_xy_less_than(person.pos, person.target_pos, goal_dist) && fabs(person.pos.z - person.target_pos.z) < 0.5*person.get_height()) {
		assert(point_in_building_or_basement_bcube(person.target_pos));
		person.pos = person.target_pos;
		
		if (!person.path.empty()) { // move to next path point
			person.next_path_pt(0);

			if (person.target_pos == person.pos) { // this is rare and maybe should be an error, but I've seen it fail once, and it can fail for tunnels
				cout << "Invalid building AI path point: " << person.pos.str() << " " << TXT(person.in_tunnel) << endl;
				//register_debug_event(person.target_pos, "invalid building AI path point");
				if (person.dir.x != 0.0 || person.dir.y != 0.0) {person.target_pos += 0.01*radius*person.dir;} // move forward slightly
				else {person.target_pos += 0.01*radius*rgen.signed_rand_vector_xy();} // add a small random offset so that dir isn't a zero vector
			}
			//return AI_NEXT_PT; // returning here and recalculating the path on the next frame can get us stuck at this point when chasing the player
		}
		else {
			if (person.goal_type == GOAL_TYPE_ELEVATOR) {
				elevator_t &e(get_elevator(person.cur_elevator));
				float const e_floor_spacing(get_elevator_floor_spacing(e));
				// floor index relative to this elevator, not the room or building
				float top_floor_z2(e.z2());
				if (e.in_mall == 1) {min_eq(top_floor_z2, ground_floor_z1);} // prevent selection of ground floor of underground elevator
				assert(e.z1() < top_floor_z2);
				unsigned const cur_floor(get_elevator_floor(person.pos.z, e, e_floor_spacing)), num_floors(round_fp((top_floor_z2 - e.z1())/e_floor_spacing));
				assert(num_floors > 1);
				assert(cur_floor  < num_floors);

				// select the destination floor, different from the current floor; this must be done before calling the elevator so that we know if we're going up or down
				for (unsigned n = 0; n < 1000; ++n) { // should exit after a few iterations, but add a cap of 1000 just in case
					person.dest_elevator_floor = (rgen.rand() % num_floors);
					// check for backrooms floors that are underwater
					if (e.in_backrooms && has_water() && (e.z1() + e_floor_spacing*person.dest_elevator_floor + fc_thick) < interior->water_zval) continue;
					if (person.dest_elevator_floor != cur_floor && !e.skip_floor_ix(person.dest_elevator_floor)) break; // floor is valid
				}
				bool const is_up(person.dest_elevator_floor > cur_floor);
				call_elevator_to_floor_and_light_nearest_button(e, cur_floor, 0, is_up); // is_inside_elevator=0
				vector3d const dir_to_elevator(e.get_cube_center() - person.pos);
				// snap to face the elevator; would be better to gradually turn, but it's not clear how to do that; at least we should be facing in that general direction
				person.dir.assign(dir_to_elevator.x, dir_to_elevator.y, 0.0);
				person.dir.normalize();
				person.wait_for(global_building_params.elevator_wait_time); // wait there for the elevator to arrive; if we're waiting too long, give up and choose another dest
				return AI_WAIT_ELEVATOR;
			}
			if (person.no_wait_at_dest) {
				person.target_pos      = all_zeros; // reset target without waiting
				person.no_wait_at_dest = 0; // reset for next goal
			}
			// don't wait if we can follow the player
			else if (!(global_building_params.ai_target_player && (can_target_player(person) || has_nearby_sound(person, floor_spacing))) && !person.in_tunnel) {
				person.wait_for(rgen.rand_uniform(1.0, (can_ai_follow_player(person) ? 2.0 : 10.0))); // stop for 1-10s, 1-2s if player is in this building in gameplay mode
			}
			person.on_new_path_seg    = 1; // allow player following AI update logic to rerun this frame
			person.last_used_elevator = 0;
			return AI_AT_DEST;
		}
	}
	// this person is walking to a destination point
	vector3d new_dir((person.target_pos - person.pos).get_norm());
	float const dir_dp(dot_product(new_dir, person.dir));
	point new_pos;

	if (dir_dp < 0.9999) { // dir not perfectly aligned
		//if (person.is_close_to_player()) {cout << TXT(new_dir.str()) << TXT(person.dir.str()) << TXT(new_dir_mag) << TXT(delta_dir) << TXT(max_dist) << TXT(radius) << endl;}
		assert(new_dir != zero_vector); // should be guaranteed by dist_less_than() test, assuming zvals are equal (which they should be)
		float const step_scale(max(0.1f, dot_product(person.dir, new_dir))); // move more slowly when direction misaligns to avoid overshooting target_pos
		
		if (dir_dp < -0.9999) { // direction nearly opposite
			vector3d const orig_dir(person.dir);
			bool const pri_dir(fabs(person.dir.x) < fabs(person.dir.y));
			person.dir[!pri_dir] += 0.1; // adjust directly to avoid getting stuck and not being able to turn
		}
		person.dir = delta_dir*new_dir + (1.0 - delta_dir)*person.dir; // merge new_dir into dir gradually for smooth turning
		if (person.on_stairs()) {person.dir.z = new_dir.z;} // dir.z tracks exactly
		person.dir.normalize();
		new_pos = person.pos + (max_dist*step_scale)*person.dir;
	}
	else { // optimization for aligned dir
		new_pos = person.pos + max_dist*person.dir;
	}
	if (!person.in_pool && !person.in_tunnel) { // make sure the person is inside the building, in case they were pushed by another person
		cube_t clip_cube;
		
		if (has_basement() && new_pos.z < ground_floor_z1) { // in the basement
			cube_t const &basement(get_basement());

			if (has_ext_basement()) {
				cube_t sc; sc.set_from_sphere(new_pos, coll_dist); // sphere bounding cube
				float cont_area(0.0);
				accumulate_shared_xy_area(basement, sc, cont_area);
				clip_cube = get_full_basement_bcube(); // start with full union bcube for basement

				if (basement.contains_pt(new_pos)) { // primarily in the basement
					accumulate_shared_xy_area(interior->basement_ext_bcube, sc, cont_area);
					if (cont_area < 0.99*sc.get_area_xy()) {clip_cube = basement;} // not contained - force into the basement
				}
				else { // primarily in the extended basement
					cube_t cur_room_bcube(basement); // start at the basement in case we're not even in a room, so we can at least snap here as a fallback

					for (auto r = interior->ext_basement_rooms_start(); r != interior->rooms.end(); ++r) { // include extended basement rooms
						if (new_pos.z < r->z1() || new_pos.z > r->z2()) continue; // wrong level
						if (r->contains_pt(new_pos)) {cur_room_bcube = *r;} // this is the room we'll be pushed into if needed
						accumulate_shared_xy_area(*r, sc, cont_area);
					}
					for (tunnel_seg_t const &t : interior->tunnels) { // include tunnels
						if (new_pos.z < t.bcube_ext.z1() || new_pos.z > t.bcube_ext.z2()) continue; // wrong zval
						if (t.room_conn && t.bcube_ext.contains_pt(new_pos)) {cur_room_bcube = t.bcube_ext;}
						accumulate_shared_xy_area(t.bcube_ext, sc, cont_area);
					}
					if (has_mall()) { // include mall back hallway stairs, which are outside any rooms
						for (stairwell_t const &s : interior->stairwells) {
							if (s.in_mall != 2) continue; // not mall back hallway stairs
							if (new_pos.z < s.z1() || new_pos.z > s.z2()) continue; // wrong level
							cube_t stairs_ext(s);
							stairs_ext.expand_in_dim(s.dim, (0.2*s.get_length() + coll_dist)); // expand to allow room for people to turn around at corners
							if (stairs_ext.contains_pt(new_pos)) {cur_room_bcube = stairs_ext;}
							accumulate_shared_xy_area(stairs_ext, sc, cont_area);
							clip_cube.union_with_cube(stairs_ext);
						}
					}
					if (cont_area < 0.99*sc.get_area_xy()) {clip_cube = cur_room_bcube;} // not contained - force into containing room
				}
			}
			else {clip_cube = basement;} // basement only
		}
		else {clip_cube = bcube;} // above ground
		// make sure person stays within building bcube; can't clip to room because person may be exiting it
		clip_cube.expand_by_xy(-coll_dist); // shrink
		clamp_person_to_building_bcube(new_pos, clip_cube, radius, fc_thick);
	}
	if (!is_cube() && !person.on_fixed_path() && !check_cube_within_part_sides(person.get_bcube() + (new_pos - person.pos))) { // outside the building
		int const part_ix(get_part_ix_containing_pt(new_pos));

		if (part_ix >= 0) { // center is at least in a valid part
			// we don't know exactly how far outside the building this person is,
			// so move them 10% of their radius toward the center of their current part and hope they make it back into the building after a few frames;
			// seems to work better than calling do_sphere_coll_polygon_sides() here
			point const part_center(parts[part_ix].get_cube_center());
			vector3d const move_dir((part_center.x - new_pos.x), (part_center.y - new_pos.y), 0.0);
			new_pos += move_dir*(0.1*radius/move_dir.mag());
			person.abort_dest();
		}
	}
	if (!point_in_building_or_basement_bcube(new_pos)) { // person must be inside the building
		cout << TXT(new_pos.str()) << TXT(person.pos.str()) << TXT(bcube.str()) << TXT(interior->basement_ext_bcube.str())
			 << TXT(person.on_stairs()) << TXT(max_dist) << TXT(person.dir.str()) << TXT(prev_in_pool) << TXT(person.in_pool) << endl;
		assert(0);
	}
	// don't do collision detection while on stairs/ramp/escalator because it doesn't work properly; just let people walk through each other
	if (!person.on_fixed_path() && !person.in_tunnel) {
		if (person.goal_type == GOAL_TYPE_ELEVATOR) { // heading for the elevator
			// check for collisions with someone else waiting for the elevator
			for (auto p = interior->people.begin(); p < interior->people.end(); ++p) {
				if (!person.waiting_for_same_elevator_as(*p, floor_spacing)) continue;
				float const rsum(coll_dist + COLL_RADIUS_SCALE*p->radius);
				if (!dist_xy_less_than(new_pos, p->pos, rsum)) continue; // new pos not close

				if (global_building_params.allow_elevator_line /*&& global_building_params.elevator_capacity > 1*/) {
					person.anim_time  = 0.0; // pause animation in case this person is mid-step
					person.target_pos = person.pos; // stop here and wait on the next frame; will form a line behind/beside this person
				}
				else {person.abort_dest();} // choose another dest
				return AI_MOVING; // return here with current person.pos
			} // for p
		}
		// check all other people in the same building after this one and attempt to avoid them
		for (auto p = interior->people.begin()+person_ix+1; p < interior->people.end(); ++p) {
			if (fabs(person.pos.z - p->pos.z) > coll_dist) continue; // different floors
			// don't let us be pushed by someone entering or exiting the elevator as that causes problems; instead, let these people walk through each other
			if (global_building_params.no_coll_enter_exit_elevator && p->ai_state >= AI_ENTER_ELEVATOR) continue;
			float const rsum(coll_dist + COLL_RADIUS_SCALE*p->radius);
			if (!dist_xy_less_than(new_pos, p->pos, rsum)) continue; // new pos not close
			if (!dist_xy_less_than(person.pos, p->pos, rsum)) return AI_STOP; // old pos not intersecting, stop
			person.anim_time = 0.0; // pause animation in case this person is mid-step
			move_person_to_not_collide(person, *p, new_pos, rsum, coll_dist); // if we get here, we have to actively move out of the way
			return AI_MOVING; // return here, but don't update animation or dir; only handles a single collision
		} // for p
	}
	bool const player_in_this_building(cur_player_building_loc.building_ix == person.cur_bldg); // basement door only counts if the player is in this building
	bool const might_have_closed_door(global_building_params.open_door_prob < 1.0 || (player_in_this_building && is_house && has_basement()));
	bool const check_for_closed_doors(global_building_params.ai_opens_doors == 2 && might_have_closed_door);

	if (interior->door_state_updated || check_for_closed_doors) {
		cube_t sc; sc.set_from_sphere(new_pos, coll_dist); // sphere bounding cube

		for (auto i = interior->door_stacks.begin(); i != interior->door_stacks.end(); ++i) { // can be slow, but not as slow as iterating over doors
			if (new_pos.z < i->z1() || new_pos.z > i->z2())    continue; // wrong part/floor
			if (!i->get_open_door_path_bcube().intersects(sc)) continue; // no intersection with door
			cube_t const dbc(i->get_true_bcube());

			if (!dbc.line_intersects(person.pos, person.target_pos)) { // check if path goes through door, to allow for "glancing blows" when pushed or turning
				if (person.path.empty() || !dbc.line_intersects(person.target_pos, person.path.back())) continue; // check next path point as well
			}
			assert(i->first_door_ix < interior->doors.size());

			for (unsigned dix = i->first_door_ix; dix < interior->doors.size(); ++dix) {
				door_t const &door(interior->doors[dix]);
				if (!i->is_same_stack(door)) break; // moved to a different stack, done
				if (door.z1() > person.pos.z || door.z2() < person.pos.z) continue; // wrong floor
				
				if (door.open) { // doors tend to block the AI, don't collide with them unless they're closed
					if (door.open_amt < 1.0) {new_pos = person.pos;} // wait here until the door is fully open
					continue;
				}
				if (global_building_params.ai_opens_doors && !door.is_locked_or_blocked(person.has_key)) { // can open the door
					toggle_door_state(dix, player_in_this_building, 0, person.pos); // by_player=0
				}
				else { // can't open the door
					person.wait_for(5.0); // wait for 5s and then choose a new desination
					return AI_WAITING; // cut the path short at this closed door
				}
			} // for dix
		} // for i
		if (has_mall()) { // handle mall store gates that the player may have closed
			for (store_doorway_t const &d : interior->mall_info->store_doorways) {
				if (!d.closed) continue; // open gate
				if (new_pos.z < d.z1() || new_pos.z > d.z2() || !d.intersects(sc)) continue; // wrong part/floor or no intersection with door

				if (!d.line_intersects(person.pos, person.target_pos)) { // check if path goes through door, to allow for "glancing blows" when pushed or turning
					if (person.path.empty() || !d.line_intersects(person.target_pos, person.path.back())) continue; // check next path point as well
				}
				person.wait_for(5.0); // wait for 5s and then choose a new desination
				return AI_WAITING; // cut the path short at this closed door
			} // for d
		}
	}
	handle_vert_cylin_tape_collision(new_pos, person.pos, person.get_z1(), person.get_z2(), radius, 0); // should be okay to use zvals from old pos; is_player=0
	// logic to clip this person to correct room Z-bounds in case something went wrong; remove if/when this is fixed
	// Note: we probably can't use the room Z bounds here becase the person may be on the stairs connecting two stacked parts
	float true_z1(bcube.z1());

	if (point_in_extended_basement_not_basement(new_pos)) {
		// round to an exact floor; needed for shallow swimming pools less than a floor in depth
		float const extb_depth(bcube.z1() - interior->basement_ext_bcube.z1());
		int const extb_num_floors(round_fp(extb_depth/floor_spacing + 0.25)); // bias larger to capture pool depth slightly less than half a floor
		min_eq(true_z1, (bcube.z1() - extb_num_floors*floor_spacing));
	}
	float const min_valid_zval(true_z1 + fc_thick + radius), max_valid_zval(bcube.z2() - radius); // Note: max should include the attic

	if (/*player_in_this_building &&*/ !person.in_pool && !person.in_tunnel && !person.on_stairs() && person.cur_room >= 0) {
		// movement in XY, not on stairs, room is valid: snap to nearest floor; this is optional and is done just in case something went wrong
		room_t room(get_room(person.cur_room));

		if (!room.contains_pt(new_pos)) { // maybe we just exited the stairs into a different part and cur_room hasn't been updated yet
			int const new_room_ix(get_room_containing_pt(new_pos));
			if (new_room_ix >= 0) {room = get_room(new_room_ix);}
		}
		float const room_floor_spacing(get_room_floor_spacing(room));
		int cur_floor(max(0, round_fp((new_pos.z - min_valid_zval)/room_floor_spacing)));
		int const max_floor(round_fp((room.z2() - true_z1)/room_floor_spacing) - 1);
		min_eq(cur_floor, max_floor); // clip to the valid floors for this room relative to lowest building floor
		float const adj_zval(cur_floor*room_floor_spacing + min_valid_zval);
		if (fabs(adj_zval - new_pos.z) > 0.1*radius) {person.abort_dest();} // if we snap to the floor, reset the target and path
		new_pos.z = adj_zval;
	}
	if (USE_MALL_ENT_STAIRS) { // handle mall entrance stairs
		float feet_zval(new_pos.z - radius);

		if (!person.in_pool && !person.in_tunnel && adjust_zval_for_mall_stairs(new_pos, feet_zval)) {
			new_pos.z = feet_zval + radius; // move up onto stairs
			//person.is_on_stairs = 1; // ???
		}
	}
	max_eq(new_pos.z, min_valid_zval); // don't let the person go below the ground floor
	min_eq(new_pos.z, max_valid_zval); // don't let the person go above the room ceiling
	// update state
	float const old_anim_time(person.anim_time);
	person.pos        = new_pos; // Note: new_pos.z should equal person.poz.z unless on stairs, which is difficult to accurately check for in this function
	person.anim_time += max_dist; // * (person.on_escalator() ? 0.75 : 1.0); // slower walking on escalator; TODO: should we reduce speed and use the idle animation?
	person.idle_time  = 0.0; // reset idle time if we actually move
	bool enable_bl_update(display_mode & 0x20); // disabled by default, enable with key '6'
	if (in_building_gameplay_mode()) {enable_bl_update = 0;} // zombies don't turn on and off lights
	if (ai_follow_player() && global_building_params.ai_player_vis_test >= 3) {enable_bl_update = 0;} // if AI tests that player is lit, don't turn on/off lights
	if (enable_bl_update) {ai_room_lights_update(person);} // non-const part
	// update cur_room after moving and lights update; needed if player is in the room and for ai_room_lights_update() same room optimization
	if (player_in_this_building || enable_bl_update) {person.cur_room = get_room_containing_pt(person.pos);}

	// create splashes if just fell in the pool, or if head is above the water
	if (!prev_in_pool && person.in_pool) { // fell in
		check_for_water_splash(person.pos, 1.5, 1, 1, 1); // full_room_height=1, draw_splash=1, alert_zombies=1
	}
	else if (point_in_water_area(point(person.pos.x, person.pos.y, person.get_z1()), 0) && person.get_z2() > interior->water_zval) { // full_room_height=0
		float const splash_dist(2.0*radius);

		if (round_fp(old_anim_time/splash_dist) != round_fp(person.anim_time/splash_dist)) { // update every splash_dist
			check_for_water_splash(person.pos, 1.0, 1, 0, 0); // full_room_height=1, draw_splash=0, alert_zombies=0
		}
	}
	// maybe play footstep sounds
	if (!person.in_pool && cur_player_building_loc.building_ix == person.cur_bldg && same_room_and_floor_as_player(person)) {
		float const anim_scale(1.0/person.radius);

		if (unsigned(anim_scale*person.anim_time) != unsigned(anim_scale*old_anim_time)) {
			gen_sound_thread_safe(SOUND_FOOTSTEP, local_to_camera_space(person.pos), 0.5, 1.2, 0.1);
		}
	}
	return (person.in_pool ? AI_IN_POOL : AI_MOVING);
}

void building_t::ai_room_lights_update(person_t const &person) {
	int const room_ix(get_room_containing_pt(person.pos));
	if (room_ix == person.cur_room) return; // same room as last time - done
	if (room_ix >= 0) {set_room_light_state_to(get_room(room_ix), person.pos.z, 1);} // make sure current room light is on when entering
	if (person.cur_room < 0)        return; // no old room (error?)
	room_t const &room(get_room(person.cur_room));
	if (room.is_single_large_room_or_store()) return; // don't turn off parking garage/backrooms/retail/mall/store lights since they affect a large area
	float const floor_spacing(get_window_vspace());
	bool other_person_in_room(cur_player_building_loc.building_ix == person.cur_bldg && same_room_and_floor_as_player(person)); // player counts

	// check for other people in the room before turning the lights off on them
	for (person_t const &p : interior->people) {
		if (p.pos == person.pos) continue; // skip ourself
		if (fabs(person.pos.z - p.pos.z) < floor_spacing && get_room_containing_pt(p.pos) == person.cur_room) {other_person_in_room = 1; break;}
	}
	if (!other_person_in_room) {set_room_light_state_to(get_room(person.cur_room), person.pos.z, 0);} // make sure old room light is off
}

void building_t::move_person_to_not_collide(person_t &person, person_t const &other, point const &new_pos, float rsum, float coll_dist) const {
	point const other_pos(other.pos.x, other.pos.y, person.pos.z); // use same zval to ignore height differences
	float const sep_dist(p2p_dist_xy(person.pos, other_pos)), move_dist(rsum - sep_dist); // distance we have to move
	// move away from the other person, hopefully not through a wall
	point const orig_pos(person.pos);
	if (sep_dist > 0.01*coll_dist) {person.pos += (move_dist/sep_dist)*(person.pos - other_pos);}
	else {person.pos.x += rsum;} // avoid divide-by-zero, choose +X direction arbitrarily
	int const room_ix(get_building_loc_for_pt(orig_pos).room_ix);
	cube_t clip_bounds((room_ix >= 0 && (unsigned)room_ix < interior->rooms.size()) ? get_room(room_ix) : bcube);
	clip_bounds.expand_by_xy(-coll_dist); // shrink
	clip_bounds.union_with_pt(orig_pos); // we know this point was valid
	clip_bounds.union_with_pt(new_pos);  // we know this point is valid
	clip_bounds.clamp_pt_xy(person.pos); // force player into the room

	// check for person getting pushed off the upper glass floor if not on an escalator; check for glass floor in case interior->room_geom was deleted
	if (person.goal_type == GOAL_TYPE_ESCALATOR && !person.on_stairs() && has_glass_floor()) {
		if (person.pos.z > interior->room_geom->glass_floors.front().z2()) { // on upper floor
			if (!point_over_glass_floor(person.pos) && point_over_glass_floor(orig_pos)) {person.pos = orig_pos;} // restore original pos if not over glass floor
		}
	}
	if (!point_in_building_or_basement_bcube(person.pos)) { // this can happen on rare occasions, due to fp inaccuracy or multiple collisions
		//cout << TXT(rsum) << TXT(sep_dist) << TXT(move_dist) << TXT(room_ix) << TXT(other.pos.str()) << TXT(person.pos.str()) << TXT(bcube.str()) << endl;
		clamp_person_to_building_bcube(person.pos, bcube, person.radius, get_fc_thickness()); // just clamp pos so that it doesn't assert later
	}
}

// Note: non-const because this updates room lights
void vect_building_t::ai_room_update(float delta_dir, float dmax, point const &camera_bs, rand_gen_t &rgen) {
	//timer_t timer("Building People Update"); // 0.25ms, mostly iteration overhead, for sparse update with 2-6 people per building (avg for 2 calls city + secondary)

	for (iterator b = begin(); b != end(); ++b) {
		if (!b->has_people() || !b->bcube.closest_dist_less_than(camera_bs, dmax)) continue; // no people or too far away, no updates
		b->all_ai_room_update(rgen, delta_dir);
	}
}

int building_t::get_room_containing_pt(point const &pt) const {
	assert(interior);
	float const wall_thickness(get_wall_thickness());
	bool const in_ext_basement(point_in_extended_basement_not_basement(pt));
	auto rooms_start(in_ext_basement ? interior->ext_basement_rooms_start() : interior->rooms.begin());
	auto rooms_end  (in_ext_basement ? interior->rooms.end() : interior->ext_basement_rooms_start());

	for (auto r = rooms_start; r != rooms_end; ++r) {
		if (r->contains_pt_exp_xy_only(pt, wall_thickness)) {return (r - interior->rooms.begin());} // expand in XY only to include point in doorway
	}
	if (has_pool() && interior->pool.contains_pt(pt)) {return interior->pool.room_ix;}
	return -1; // room not found
}
unsigned building_t::get_sub_room_containing_pt(point const &pt, unsigned parent_room_id) const {
	room_t const &parent(get_room(parent_room_id));

	if (parent.has_subroom()) {
		for (int r = (int)parent_room_id-1; r >= 0; --r) { // nested rooms are added before the parent room, so iterate backwards
			room_t const &room(get_room(r));
			if (!room.is_nested() || !parent.contains_cube(room)) break; // no more nested rooms for this parent
			if (room.contains_pt(pt)) return r;
		}
	}
	return parent_room_id; // no containing sub-room found; return the paren's room
}
int building_t::get_room_containing_camera(point const &camera_rot) const {
	if (player_in_elevator) { // use room assigned to elevator the player is in; more correct when elevator overlaps rooms other than the one it opens to
		for (elevator_t const &e : interior->elevators) {
			if (!e.get_bcube_padded(get_wall_thickness()).contains_pt(camera_rot)) continue;
			
			if (e.all_room_ids.size() > 1) { // elevator spans multiple rooms vertically (mall back hallway elevator); find the correct room
				for (unsigned room_id : e.all_room_ids) {
					room_t const &room(get_room(room_id));
					if (camera_rot.z > room.z1() && camera_rot.z < room.z2()) return room_id; // only need to check zvals to get the correct floor
				}
			}
			return e.room_id;
		} // for e
	}
	return get_room_containing_pt(camera_rot);
}
building_loc_t building_t::get_building_loc_for_pt(point const &pt) const {
	building_loc_t loc;

	for (auto p = parts.begin(); p != get_real_parts_end_inc_sec(); ++p) {
		if (p->contains_pt(pt)) {loc.part_ix = (p - parts.begin()); break;}
	}
	if (loc.part_ix < 0 && point_in_extended_basement(pt)) {loc.part_ix = basement_part_ix;} // use basement part if in extended basement, even though point is outside the cube

	if (interior) { // rooms and stairwells, no elevators or escalators
		loc.room_ix = get_room_containing_pt(pt);
		if (loc.room_ix >= 0) {loc.floor_ix = get_floor_for_zval(pt.z);}

		for (auto s = interior->stairwells.begin(); s != interior->stairwells.end(); ++s) {
			if (s->contains_pt(pt)) {loc.stairs_ix = (s - interior->stairwells.begin()); break;} // Note: stairs_ix is not currently used, except for >= 0 test
		}
		// what about interior->pg_ramp? should that set stairs_ix to stairwells.size()-1?
	}
	return loc;
}
// returns: 0=no room, 1=cur room, 2=adj room
int building_t::room_or_adj_room_has_stairs(int room_ix, float zval, bool inc_adj_rooms, bool check_door_open) const {
	if (room_ix < 0) return 0; // no room contains this point
	room_t const &room(get_room(room_ix));
	unsigned floor_ix(room.get_floor_containing_zval(max(zval, room.z1()), get_window_vspace())); // clamp zval to the room
	if (room.has_stairs_on_floor(floor_ix)) return 1;
	if (!inc_adj_rooms) return 0;

	for (auto r = interior->rooms.begin(); r != interior->rooms.end(); ++r) {
		if (r->has_stairs_on_floor(floor_ix) && are_rooms_connected(room, *r, zval, check_door_open)) return 2;
	}
	return 0;
}

bool building_t::maybe_zombie_retreat(unsigned person_ix, point const &hit_pos) {
	if (!ai_follow_player()) return 0; // not in gameplay mode, ignore it
	assert(interior && person_ix < interior->people.size());
	person_t &person(interior->people[person_ix]);
	if (person.on_fixed_path()) return 0; // ignore when on stairs/ramp/escalator as this doesn't work correctly
	if (hit_pos.z < (person.get_z1() + 0.25*person.get_height())) return 0; // less than 25% up, coll with legs, assume this is kicking a ball that's on the floor (if a ball)
	// play sound on first retreat: alert_other_zombies=1, high_priority=1, gain=1.0, pitch=1.25
	if (person.retreat_time == 0.0) {maybe_play_zombie_sound(person.pos, person_ix, 1, 1, 1.0, 1.25);}
	// Note: this isn't really thread safe, but it should be okay to modify this state while the AI thread is running
	person.retreat_time = global_building_params.ai_retreat_time*TICKS_PER_SECOND; // retreat
	return 1;
}
void building_t::register_person_hit(unsigned person_ix, room_object_t const &obj, vector3d const &velocity) {
	if (velocity == zero_vector)           return; // stationary object, ignore it
	if (!is_ball_type(obj.type))           return; // currently balls are the only throwable/dynamic object
	if (!obj.get_ball_type().hurts_zombie) return;
	if (maybe_zombie_retreat(person_ix, obj.get_cube_center())) {register_achievement("Zombie Bashing");}
}

/*static*/ float building_t::get_min_front_clearance_inc_people() {
	float clearance(get_min_front_clearance());
	// include people models; note that this means building interiors may be generated differently when building AIs are enabled
	// should we use get_ped_coll_radius() instead?
	if (enable_building_people_ai() && global_building_params.building_people_enabled()) {max_eq(clearance, 2.0f*ped_manager_t::get_ped_radius());}
	return clearance;
}

// these must be here to handle deletion of building_nav_graph_t, which is only defined in this file
building_interior_t:: building_interior_t() {}
building_interior_t::~building_interior_t() {}
