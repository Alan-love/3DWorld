// 3D World - Building Navigation for AIs
// by Frank Gennari 3/14/20

#include "3DWorld.h"
#include "function_registry.h"
#include "buildings.h"
#include "city.h" // for person_t
#include "profiler.h"
#include <queue>


float const COLL_RADIUS_SCALE = 0.75; // somewhat smaller than radius, but larger than PED_WIDTH_SCALE

int player_hiding_frame(0);
building_dest_t cur_player_building_loc, prev_player_building_loc;
room_object_t player_hiding_obj;

extern bool player_is_hiding;
extern int frame_counter, display_mode, animate2;
extern float fticks;
extern building_params_t global_building_params;
extern bldg_obj_type_t bldg_obj_types[];

bool in_building_gameplay_mode();
bool ai_follow_player() {return (global_building_params.ai_follow_player || in_building_gameplay_mode());}
bool can_ai_follow_player(person_t const &person, bool allow_diff_building=0);
float get_closest_building_sound(point const &at_pos, point &sound_pos, float floor_spacing);
void maybe_play_zombie_sound(point const &sound_pos_bs, unsigned zombie_ix, bool alert_other_zombies, bool high_priority=0, float gain=1.0, float pitch=1.0);
int register_ai_player_coll(bool &has_key, float height);
unsigned get_stall_cubes(room_object_t const &c, cube_t sides[3]);
unsigned get_shower_cubes(room_object_t const &c, cube_t sides[2]);
void resize_cubes_xy(vect_cube_t &cubes, float val);

point get_cube_center_zval(cube_t const &c, float zval) {return point(c.xc(), c.yc(), zval);}
float get_ped_coll_radius() {return COLL_RADIUS_SCALE*ped_manager_t::get_ped_radius();}

class cube_nav_grid {
	float radius=0.0;
	cube_t bcube, grid_bcube;
	unsigned num[2] = {};
	float   step[2] = {};
	vector<uint8_t> nodes; // num[0] x num[1]; 0=blocked, 1=open

	struct ix_pair_t {
		unsigned x, y;
		ix_pair_t(unsigned x_, unsigned y_) : x(x_), y(y_) {}
		bool operator<(ix_pair_t const &p) const {return ((y == p.y) ? (x < p.x) : (y < p.y));} // needed for priority_queue
	};
	struct a_star_node_state_t {
		int came_from[2] = {-1,-1};
		unsigned xy[2] = {0,0};
		float g_score=0.0, f_score=0.0;
		void set(unsigned from_x, unsigned from_y, unsigned x, unsigned y) {came_from[0] = from_x; came_from[1] = from_y; xy[0] = x; xy[1] = y;}
	};
	static bool pt_contained_xy(point const &pt, vect_cube_t const &cubes) {
		for (cube_t const &c : cubes) {
			if (c.contains_pt_xy(pt)) return 1;
		}
		return 0;
	}
	bool    are_ixs_valid(unsigned x, unsigned y) const {return (x < num[0] && y < num[1]);} // negative numbers will wrap around and still fail
	unsigned get_node_ix (unsigned x, unsigned y) const {assert(are_ixs_valid(x, y)); return (x + y* num[0]);}
	point    get_grid_pt (unsigned x, unsigned y, float zval) const {return point((grid_bcube.x1() + x*step[0]), (grid_bcube.y1() + y*step[1]), zval);}
	point    get_grid_pt (unsigned x, unsigned y) const {return get_grid_pt(x, y, grid_bcube.z1());}
	uint8_t  get_node_val(unsigned x, unsigned y) const {return nodes[get_node_ix(x, y)];}

	float get_distance(unsigned x1, unsigned y1, unsigned x2, unsigned y2) const {
		float const dx(float(x2) - float(x1)), dy(float(y2) - float(y1));
		return sqrt(dx*dx + dy*dy);
	}
	bool find_open_node_closest_to(point p, unsigned &nx, unsigned &ny) const {
		if (!bcube.contains_pt_xy(p)) return 0; // outside the grid valid area; error?
		grid_bcube.clamp_pt_xy(p);
		float gxy[2]; // grid index, with partial offset
		for (unsigned d = 0; d < 2; ++d) {gxy[d] = (p[d] - grid_bcube.d[d][0])/step[d];}
		float dmin_sq(0.0);
		bool found(0);

		for (unsigned y = 0; y < 2; ++y) {
			for (unsigned x = 0; x < 2; ++x) {
				unsigned const yi(y ? unsigned(ceil(gxy[1])) : unsigned(floor(gxy[1])));
				unsigned const xi(x ? unsigned(ceil(gxy[0])) : unsigned(floor(gxy[0])));
				if (!are_ixs_valid(xi, yi)) continue; // invalid, skip; error/can't happen?
				if (!get_node_val (xi, yi)) continue; // blocked, skip
				float const dsq(p2p_dist_xy_sq(p, get_grid_pt(xi, yi))); // dist should be < SQRT2*radius
				if (!found || dsq < dmin_sq) {found = 1; dmin_sq = dsq; nx = xi; ny = yi;} // keep if closer
			}
		} // for y
		return found;
	}
public:
	bool is_built() const {return !bcube.is_all_zeros();}

	void build(cube_t const &bcube_, vect_cube_t const &blockers, float radius_) {
		radius     = radius_;
		bcube      = bcube_;
		grid_bcube = bcube;
		// determine grid size and allocate vectors
		float const spacing(SQRT2*radius); // can't be further than the test diameter or we'll miss blockers in the gap; use sqrt(2) to make spheres diagonally tangential
		grid_bcube.expand_by_xy(-radius);
		point const size(grid_bcube.get_size());
		if (min(size.x, size.y) <= 2.0*spacing) return; // too small (error?)

		for (unsigned d = 0; d < 2; ++d) {
			num [d] = unsigned(ceil(size[d]/spacing));
			step[d] = size[d]/(num[d] - 1);
		}
		nodes.resize(num[0]*num[1], 0);
		//cout << TXT(blockers.size()) << TXT(num[0]) << TXT(num[1]) << TXT(nodes.size()) << endl;
		// determine open nodes; edges are implicitly from adjacent 1 nodes
		vect_cube_t blockers_exp(blockers), row_blockers;
		resize_cubes_xy(blockers_exp, radius);

		for (unsigned y = 0; y < num[1]; ++y) {
			float const yval(grid_bcube.y1() + y*step[1]);
			row_blockers.clear();

			for (cube_t const &c : blockers_exp) {
				if (c.y1() < yval && c.y2() > yval) {row_blockers.push_back(c);} // include if it spans yval
			}
			for (unsigned x = 0; x < num[0]; ++x) {
				if (!pt_contained_xy(get_grid_pt(x, y), row_blockers)) {nodes[get_node_ix(x, y)] = 1;}
			}
		}
	}
	bool find_path(point const &p1, point const &p2, vector<point> &path) const {
		assert(is_built());
		if (nodes.empty()) return 0; // not built or too small/empty
		//highres_timer_t timer("Find Path"); // <= 0.5ms
		assert(p1.z == p2.z); // must be horizontal
		unsigned nx1(0), ny1(0), nx2(0), ny2(0);
		if (!find_open_node_closest_to(p1, nx1, ny1) || !find_open_node_closest_to(p2, nx2, ny2)) return 0;
		// does it make sense to use A* rather than Dijkstra? maybe not because paths will generally be short compared to the number of total nodes
		unsigned start_ix(get_node_ix(nx1, ny1)), end_ix(get_node_ix(nx2, ny2));
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
			assert(!closed[cur_ix]);
			closed[cur_ix] = 1;
			open  [cur_ix] = 0;

			for (int dy = -1; dy <= 1; ++dy) { // check 3x3 grid except center
				for (int dx = -1; dx <= 1; ++dx) {
					if (dx == 0 && dy == 0) continue; // skip self
					unsigned const new_x(cur.x + dx), new_y(cur.y + dy); // may wrap around to 2^32
					if (!are_ixs_valid(new_x, new_y)) continue; // off the grid
					unsigned const new_ix(get_node_ix(new_x, new_y));
					if (closed[new_ix]) continue; // already closed (duplicate)
					if (!nodes[new_ix]) continue; // blocked
					a_star_node_state_t &sn(state[new_ix]);
					float const new_g_score(sn.g_score + get_distance(cur.x, cur.y, new_x, new_y));
					if (!open[new_ix]) {open[new_ix] = 1;}
					else if (new_g_score >= sn.g_score) continue; // not better
					sn.set(cur.x, cur.y, new_x, new_y);

					if (new_ix == end_ix) { // done, reconstruct path (in reverse)
						path.push_back(get_grid_pt(nx1, ny1, p1.z)); // first point
						unsigned const rev_start_ix(path.size());
						unsigned path_ix(cur_ix);

						while (path_ix != start_ix) {
							assert(path_ix < state.size());
							a_star_node_state_t &sp(state[path_ix]);
							assert(sp.came_from[0] >= 0 && sp.came_from[1] >= 0);
							// TODO: smooth path by removing colinear segments
							path.push_back(get_grid_pt(sp.came_from[0], sp.came_from[1], p1.z));
							path_ix = get_node_ix(sp.came_from[0], sp.came_from[1]);
						} // end while()
						reverse(path.begin()+rev_start_ix, path.end());
						path.push_back(get_grid_pt(nx2, ny2, p1.z)); // last  point
						cout << "success: " << path.size() << endl;
						return 1; // success
					}
					sn.g_score = new_g_score;
					sn.f_score = sn.g_score + get_distance(new_x, new_y, nx2, ny2);
					open_queue.push(make_pair(-sn.f_score, ix_pair_t(new_x, new_y)));
				} // for dx
			} // for dy
		} // end while()
		//cout << "failed" << endl;
		return 0; // failed - no path from room1 to room2
	}
};

// Note: this should go into building_t/buildings.h at some point, but is temporarily here
class building_nav_graph_t {
	struct conn_room_t { // size=16
		unsigned ix; // node index
		int door_ix; // index of connecting door on first floor; -1 for connections with out a door
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
			for (auto i = conn_rooms.begin(); i != conn_rooms.end(); ++i) {if (i->ix == room) return;} // ignore duplicates
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
	bool has_pg_ramp=0;
	vector<node_t> nodes;
	mutable cube_nav_grid nav_grid; // for use with backrooms; cached during path finding
	node_t       &get_node(unsigned room)       {assert(room < nodes.size()); return nodes[room];}
	node_t const &get_node(unsigned room) const {assert(room < nodes.size()); return nodes[room];}

	void remove_connection(unsigned from, unsigned to) {
		auto &conn(get_node(from).conn_rooms);

		for (auto i = conn.begin(); i != conn.end(); ++i) {
			if (i->ix == to) {swap(*i, conn.back()); conn.pop_back(); return;}
		}
		assert(0); // must be found - should not get here
	}
public:
	bool invalid=0;
	building_nav_graph_t(float stairs_extend_) : stairs_extend(stairs_extend_) {}

	void set_num_rooms(unsigned num_rooms_, unsigned num_stairs_, bool has_pg_ramp_) {
		num_rooms   = num_rooms_;
		num_stairs  = num_stairs_;
		has_pg_ramp = has_pg_ramp_;
		nodes.resize(num_rooms + num_stairs + has_pg_ramp);
		for (unsigned n = num_rooms; n < (num_rooms + num_stairs); ++n) {nodes[n].is_stairs = 1;}
		if (has_pg_ramp) {nodes.back().is_ramp = 1;}
	}
	void set_room_bcube  (unsigned room,   cube_t const &c) {get_node(room).bcube = c;}
	void set_stairs_bcube(unsigned stairs, cube_t const &c) {get_node(stairs + num_rooms).bcube = c;}
	void set_ramp_bcube                   (cube_t const &c) {get_node(num_stairs + num_rooms).bcube = c;}
	void mark_hallway(unsigned room) {get_node(room).is_hallway = 1;}
	void mark_exit   (unsigned room) {get_node(room).has_exit   = 1;}

	void connect_stairs(unsigned room, unsigned stairs, bool dim, bool dir, bool is_u, bool is_ramp) {
		assert(room < num_rooms && (is_ramp || stairs < num_stairs));
		unsigned const node_ix2(num_rooms + (is_ramp ? num_stairs : stairs)); // pg_ramp comes after all stairs
		node_t &n2(get_node(node_ix2));
		cube_t entry_u(n2.bcube), entry_d(n2.bcube);
		float const extend((dir ? -1.0 : 1.0)*stairs_extend); // extend away from stairs for entrance/exit area; will be denormalized in this dim

		if (is_u) { // U-shaped stairs: entrances are on the same side
			bool const side(dir); // Note: see code in add_stairs_and_elevators()
			entry_u.d[dim][dir] = entry_d.d[dim][dir] = entry_u.d[dim][!dir] + extend; // shrink to extend length at the entrance to the stairs
			float const mid(n2.bcube.get_center_dim(!dim));
			entry_u.d[!dim][ side] = mid; // bottom
			entry_d.d[!dim][!side] = mid; // top
		}
		else { // straight stairs: entrances are on opposite ends
			entry_u.d[dim][ dir] = entry_u.d[dim][!dir] + extend; // shrink to extend length at the entrance to the stairs when going up
			entry_d.d[dim][!dir] = entry_d.d[dim][ dir] - extend; // shrink to extend length at the entrance to the stairs when going down
		}
		get_node(room).add_conn_room(node_ix2, -1, entry_u, entry_d);
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
	bool is_room_connected_to(unsigned room1, unsigned room2, vect_door_t const &doors, float zval, bool has_key) const {
		// Note: likely faster than running full A* algorithm
		assert(room1 < num_rooms && room2 < num_rooms);
		if (room1 == room2) return 1;
		vector<uint8_t> seen(nodes.size(), 0);
		vector<unsigned> pend;
		pend.push_back(room1);
		seen[room1] = 1;

		while (!pend.empty()) {
			node_t const &node(get_node(pend.back()));
			pend.pop_back();

			for (auto i = node.conn_rooms.begin(); i != node.conn_rooms.end(); ++i) {
				if (seen[i->ix]) continue;
				if (!can_use_conn(*i, doors, zval, has_key)) continue; // blocked by closed or locked door
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
	static bool is_valid_pos(vect_cube_t const &avoid, building_t const &building, point const &pos, float radius, float height) {
		cube_t c(pos, pos);
		c.expand_by_xy(radius);

		for (auto i = avoid.begin(); i != avoid.end(); ++i) {
			if (i->intersects_xy(c) && sphere_cube_intersect_xy(pos, radius, *i)) return 0;
		}
		if (!building.check_cube_within_part_sides(c)) return 0; // outside non-cube building - will try again with a new pos next time
		return 1;
	}
	static void choose_pt_xy_in_room(point &pos, cube_t const &c, rand_gen_t &rgen) {
		for (unsigned d = 0; d < 2; ++d) {pos[d] = rgen.rand_uniform(c.d[d][0], c.d[d][1]);}
	}
	point get_stairs_entrance_pt(float zval, unsigned node_ix, bool up_or_down) const {
		node_t const &node(get_node(node_ix));
		assert(!node.conn_rooms.empty());
		vector2d const &pt(node.conn_rooms.front().pt[up_or_down]); // Note: all conn_rooms should be the same value
		return point(pt.x, pt.y, zval);
	}
	static bool find_valid_pt_in_room(vect_cube_t const &avoid, building_t const &building, float radius, float height, float zval,
		cube_t const &room, rand_gen_t &rgen, point &pos, bool no_use_init=0)
	{
		if (!no_use_init && avoid.empty()) return 1; // no colliders, any point is valid, choose the initial point (which should be the room center)
		cube_t place_area(room);
		place_area.expand_by_xy(-2.0*radius); // shrink by twice the radius
		if (!place_area.is_strictly_normalized()) return 0; // should generally not be true

		if (no_use_init) { // chose a new initial point
			choose_pt_xy_in_room(pos, place_area, rgen);
			if (avoid.empty() && building.is_cube()) return 1; // no collision checks needed
		}
		point orig_pos(pos);

		for (unsigned n = 0; n < 100; ++n) { // 100 random tries to find a valid dest_pos
			if (is_valid_pos(avoid, building, pos, radius, height)) return 1; // success
			choose_pt_xy_in_room(pos, place_area, rgen); // choose a random new point in the room
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
	point find_valid_room_dest(vect_cube_t const &avoid, building_t const &building, float radius, float height, float zval, unsigned node_ix,
		bool up_or_down, bool &not_room_center, rand_gen_t &rgen, bool no_use_init, point const *const custom_dest) const
	{
		node_t const &node(get_node(node_ix));
		if (node.is_vert_conn()) {return get_stairs_entrance_pt(zval, node_ix, up_or_down);}
		point pos;
		if (custom_dest != nullptr) {pos = *custom_dest;}
		else {pos = get_room_center(building, node.bcube, zval);} // first candidate is the center of the room
		if (find_valid_pt_in_room(avoid, building, radius, height, zval, node.bcube, rgen, pos, no_use_init)) {not_room_center = 1;} // success
		return pos;
	}

	static bool check_line_int_xy(vect_cube_t const &c, point const &p1, point const &p2) {
		for (auto i = c.begin(); i != c.end(); ++i) {
			if (check_line_clip_xy(p1, p2, i->d)) return 1;
		}
		return 0;
	}
	static bool check_pt_contained_xy(vect_cube_t const &c, point const &p) {
		for (auto i = c.begin(); i != c.end(); ++i) {
			if (i->contains_pt_xy(p)) return 1;
		}
		return 0;
	}
	// add any necessary points to <path> that are required to get from <p1> to <p2> inside <walk_area> without intersecting <avoid>
	bool connect_room_endpoints(vect_cube_t const &avoid, building_t const &building, cube_t const &walk_area, unsigned room_ix, point const &p1, point const &p2,
		float radius, vector<point> &path, vect_cube_t &keepout, rand_gen_t &rgen, bool ignore_p1_coll=0, bool ignore_p2_coll=0) const
	{
		assert(p1.z == p2.z);
		bool is_path_valid(1);
		cube_t walk_area_exp(walk_area);
		walk_area_exp.expand_by_xy(radius); // to capture objects that we could intersect when our center is in walk_area
		keepout.clear();

		for (auto i = avoid.begin(); i != avoid.end(); ++i) {
			if (!i->intersects_xy(walk_area_exp)) continue;
			cube_t c(*i);
			c.expand_by_xy(radius);

			if (c.contains_pt_xy(p1)) {
				if (ignore_p1_coll) {
					if (i->contains_pt_xy(p1)) continue; // even the unexpanded cube intersects, skip it
					c = *i; // use unexpanded cube instead
				}
				else {return 0;} // fail
			}
			if (c.contains_pt_xy(p2)) {
				if (ignore_p2_coll) {
					if (i->contains_pt_xy(p2)) continue; // even the unexpanded cube intersects, skip it
					c = *i; // use unexpanded cube instead
				}
				else {return 0;} // fail
			}
			if (check_line_clip_xy(p1, p2, c.d)) {is_path_valid = 0;}
			keepout.push_back(c);
		} // for i
		if (is_path_valid) return 1; // done

		// straight line not valid, need to run path finding
		assert(!keepout.empty());
		// do something simple and brute force:
		// since rooms tend to only have objects in the middle, try to find the best point that creates the shortest non-intersecting 2-part or 3-part path
		for (unsigned npts = 1; npts <= 2; ++npts) { // try to add 1 point; if that fails, try to add 2 points
			unsigned const num_tries((npts == 1) ? 200 : 100);
			float dmin(0.0);
			bool use_pos2(0);
			point best_pt, best_pt2, pos, pos2;
			pos.z = pos2.z = p1.z;

			for (unsigned n = 0; n < num_tries; ++n) { // make num_tries attempts
				choose_pt_xy_in_room(pos, walk_area, rgen); // choose a rand point in the room
				if (check_pt_contained_xy(keepout, pos))       continue; // bad point
				if (!building.check_pt_within_part_sides(pos)) continue; // outside non-cube building
				bool const seg1_bad(check_line_int_xy(keepout, p1, pos)), seg2_bad(check_line_int_xy(keepout, pos, p2));

				if (npts == 1 || !(seg1_bad || seg2_bad)) { // single point or both segments valid
					if (seg1_bad || seg2_bad) continue; // bad point (either line intersects)
					float const dist(p2p_dist_xy(p1, pos) + p2p_dist_xy(pos, p2));
					if (dmin == 0.0 || dist < dmin) {best_pt = pos; dmin = dist; use_pos2 = 0;}
				}
				else { // npts == 2
					if (seg1_bad && seg2_bad) continue; // bad point (both lines intersect)
					bool success(0);

					for (unsigned m = 0; m < 20; ++m) { // make 20 random attempts
						choose_pt_xy_in_room(pos2, walk_area, rgen); // choose a rand point in the room
						if (!building.check_pt_within_part_sides(pos2)) continue; // outside non-cube building
						if (!check_pt_contained_xy(keepout, pos2) && !check_line_int_xy(keepout, pos, pos2)) {success = 1; break;} // good point
					}
					if (!success) continue; // no good point

					for (unsigned ordering = 0; ordering < 2; ++ordering) { // try {p1-pos-pos2-p2} and {p1-pos2-pos-p2}
						if (check_line_int_xy(keepout, pos, p1) || check_line_int_xy(keepout, pos2, p2)) {swap(pos, pos2); continue;} // bad point (could optimize this if needed)
						float const dist(p2p_dist_xy(p1, pos) + p2p_dist_xy(pos, pos2) + p2p_dist_xy(pos2, p2));
						if (dmin == 0.0 || dist < dmin) {best_pt = pos; best_pt2 = pos2; dmin = dist; use_pos2 = 1;}
						break;
					} // for ordering
				}
			} // for n
			if (dmin == 0.0) continue;
			path.push_back(best_pt);
			if (use_pos2) {path.push_back(best_pt2);}
			return 1; // success
		} // for npts
		if (0 && building.is_room_backrooms(room_ix)) { // run detailed path finding on backrooms
			if (!nav_grid.is_built()) { // build once and cache
				//highres_timer_t timer("Build Nav Grid");
				vect_cube_t blockers;
				blockers.reserve(avoid.size());

				for (cube_t const &c : avoid) {
					if (c.intersects(walk_area)) {blockers.push_back(c);}
				}
				float const radius(get_ped_coll_radius());
				nav_grid.build(walk_area, blockers, radius);
			}
			if (nav_grid.find_path(p1, p2, path)) return 1;
		}
		// else, what about parking garages?
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
	bool reconstruct_path(vector<a_star_node_state_t> const &state, vect_cube_t const &avoid, building_t const &building, point const &cur_pt,
		float radius, float height, unsigned start_ix, unsigned end_ix, unsigned ped_ix, bool is_first_path, bool up_or_down, unsigned ped_rseed,
		point const *const custom_dest, vector<point> &path) const
	{
		unsigned n(start_ix);
		rand_gen_t rgen;
		rgen.set_state((ped_ix + 17*start_ix + 1), (ped_rseed + 1));
		vect_cube_t keepout;

		while (1) {
			node_t const &node(get_node(n));
			point const &next(state[n].path_pt);
			int const came_from(state[n].came_from_ix);
			bool const is_first_pt(path.empty());
			cube_t const walk_area(calc_walkable_room_area(node, radius));
			
			if (is_first_pt) { // last point in path (first point in reverse path)
				assert(came_from >= 0);
				bool success(0);

				for (unsigned N = 0; N < 10; ++N) { // keep retrying until we find a point that is reachable from the doorway
					bool const no_use_init(N > 0); // choose a random new point on iterations after the first one
					bool not_room_center(0);
					point const end_point(find_valid_room_dest(avoid, building, radius, height, cur_pt.z, start_ix, up_or_down, not_room_center, rgen, no_use_init, custom_dest));
					path.push_back(end_point);
					if (node.is_vert_conn()) {success = 1; break;} // done, don't need to run code below
					point const room_exit(closest_room_pt(walk_area, next)); // first doorway
					if (connect_room_endpoints(avoid, building, walk_area, n, end_point, room_exit, radius, path, keepout, rgen)) {path.push_back(room_exit); success = 1; break;}
					path.clear(); // failed, reset for next iteration
					if (!not_room_center) break; // if we did choose the room center, and there is no path to it, we've failed
				} // for n
				if (!success) {assert(path.empty()); return 0;} // failed to connect to a point in dest room
			}
			else if (came_from < 0) { // done (next is not valid here)
				assert(n == end_ix);
				if (node.is_vert_conn()) return 1; // success
				point const final_pt(closest_room_pt(walk_area, path.back())); // walk from room into last doorway
				path.push_back(final_pt);

				// find path to first doorway; ignore collisions with p2 (cur_pt) in case this person was pushed into an object by another person
				if (!connect_room_endpoints(avoid, building, walk_area, n, final_pt, cur_pt, radius, path, keepout, rgen, 0, 1)) {
					// allow a failure if this is the first path taken by this AI so that it's not stuck behind an object due to bad initial placement
					if (!is_first_path) {path.clear(); return 0;}
				}
				return 1; // success
			}
			else if (!node.is_vert_conn()) { // adjust the path through a room
				assert(!path.empty());
				point const &prev(path.back());
				assert(prev.z == next.z);
				point const p1(closest_room_pt(walk_area, prev)), p2(closest_room_pt(walk_area, next));
				path.push_back(p1); // walk out of doorway and into room
				
				if (!connect_room_endpoints(avoid, building, walk_area, n, p1, p2, radius, path, keepout, rgen)) { // unreachable
					path.clear();
					// try another path? this case is rare; on failure, the person will wait a second then choose a different destination room
					//disconnect_room_pair(n, came_from); // ???
					return 0;
				}
				path.push_back(p2); // walk from room into doorway
			}
			path.push_back(next); // doorway
			n = came_from;
		} // end while()
		return 0; // never gets here
	}
	bool complete_path_within_room(point const &from, point const &to, unsigned room_ix, unsigned ped_ix, float radius,
		unsigned ped_rseed, bool is_first_path, bool following_player, vect_cube_t const &avoid, building_t const &building, vector<point> &path) const
	{
		// used for reaching a goal such as the player within the same room;
		// assumes the building shape is convex and the goal is inside the building so that the path to the goal never leaves a non-cube building
		cube_t const walk_area(calc_walkable_room_area(get_node(room_ix), radius));
		rand_gen_t rgen;
		rgen.set_state((ped_ix + 13*room_ix + 1), (ped_rseed + 1));
		vect_cube_t keepout;
		path.push_back(to); // Note: path is constructed backwards, so "to" is added first and connect_room_endpoints takes swapped arguments
		
		// ignore starting collisions, for example collisions with stairwell when exiting stairs?
		// ignore initial coll with "from", and coll with "to" when following the player
		if (!connect_room_endpoints(avoid, building, walk_area, room_ix, to, from, radius, path, keepout, rgen, 1, following_player)) { // ignore_p1_coll (to) = 1
			if (!is_first_path) { // ignore failure on first path to allow person to get out from an object they spawn in
				bool success(0);

				if (following_player) {
					// try to find a partial path, starting at "to" and working toward "from"; since rooms are rectangular, all points on the line will be contained
					for (unsigned n = 1; n <= 9; ++n) { // {10% ... 90%}
						point new_to(to + (float(n)/10)*(from - to));
						//if (!walk_area.contains_pt(new_to)) continue; // can fail for player hiding in a closet
						walk_area.clamp_pt_xy(new_to);
						// ignore_p1_coll = 0
						if (connect_room_endpoints(avoid, building, walk_area, room_ix, new_to, from, radius, path, keepout, rgen, 0, following_player)) {success = 1; break;}
					} // for n
				}
				if (!success) {path.clear(); return 0;}
			}
		}
		// maybe add an extra path point to prevent clipping through walls when walking through a doorway
		point const from_extend_pt(closest_room_pt(walk_area, from));
		if (from_extend_pt != from) {path.push_back(from_extend_pt);} // add if p1 was clamped
		return 1;
	}

	bool can_use_conn(conn_room_t const &conn, vect_door_t const &doors, float zval, bool has_key) const {
		if (conn.door_ix < 0) return 1; // no door
		if (global_building_params.ai_opens_doors && has_key) return 1; // locked door won't stop us
		unsigned const door_ix(conn.door_ix);
		assert(door_ix < doors.size());
		door_t const &first_door(doors[door_ix]);

		for (auto i = (doors.begin() + door_ix); i != doors.end(); ++i) {
			if (!i->is_same_stack(first_door)) break; // we've reached the end of the vertical stack of doors
			if (zval < i->z1() || zval > i->z2()) continue; // not the correct floor
			return (i->open || (global_building_params.ai_opens_doors && (!i->locked || has_key)));
		}
		return 1; // Note: we can get here for complex floorplan office buildings with bad interior walls (-4.18, 4.28, -3.46)
	}
	
	// A* algorithm; Note: path is stored backwards
	bool find_path_points(unsigned room1, unsigned room2, unsigned ped_ix, float radius, float height, bool use_stairs, bool is_first_path,
		bool up_or_down, unsigned ped_rseed, vect_cube_t const &avoid, building_t const &building, point const &cur_pt,
		vect_door_t const &doors, bool has_key, point const *const custom_dest, vector<point> &path) const
	{
		// Note: opening and closing doors updates the nav graph; an AI encountering a closed door after choosing a path can either open it or stop and wait
		assert(room1 < nodes.size() && room2 < nodes.size());
		assert(room1 != room2);
		path.clear();
		vector<a_star_node_state_t> state(nodes.size());
		vector<uint8_t> open(nodes.size(), 0), closed(nodes.size(), 0); // tentative/already evaluated nodes
		std::priority_queue<pair<float, unsigned> > open_queue;
		point const dest_pos(get_node(room2).get_center(cur_pt.z)); // Note: approximate, actual dest may be different
		a_star_node_state_t &start(state[room1]);
		start.g_score = 0.0;
		start.f_score = p2p_dist_xy(get_node(room1).get_center(cur_pt.z), dest_pos); // estimated total cost from start to goal through current
		open[room1]   = 1;
		open_queue.push(make_pair(-start.f_score, room1));

		while (!open_queue.empty()) {
			unsigned const cur(open_queue.top().second);
			open_queue.pop();
			node_t const &cur_node(get_node(cur));
			point const center(cur_node.get_center(cur_pt.z));
			assert(!closed[cur]);
			closed[cur] = 1;
			open  [cur] = 0;

			for (auto i = cur_node.conn_rooms.begin(); i != cur_node.conn_rooms.end(); ++i) {
				assert(i->ix < nodes.size());
				if (closed[i->ix]) continue; // already closed (duplicate)
				node_t const &conn_node(get_node(i->ix));
				if (conn_node.is_vert_conn() && !use_stairs && i->ix != room2) continue; // skip stairs/ramp in this mode
				if (!can_use_conn(*i, doors, cur_pt.z, has_key)) continue; // blocked by closed or locked door; must do this check before setting open state
				point const conn_center(conn_node.get_center(cur_pt.z));
				a_star_node_state_t &sn(state[i->ix]);
				vector2d const &pt(i->pt[up_or_down]);
				float const new_g_score(sn.g_score + p2p_dist_xy(center, pt) + p2p_dist_xy(pt, conn_center));
				if (!open[i->ix]) {open[i->ix] = 1;}
				else if (new_g_score >= sn.g_score) continue; // not better
				sn.came_from_ix = cur;
				sn.path_pt.assign(pt.x, pt.y, cur_pt.z);
				
				if (i->ix == room2) { // done, reconstruct path (in reverse)
					return reconstruct_path(state, avoid, building, cur_pt, radius, height, i->ix, room1, ped_ix, is_first_path, up_or_down, ped_rseed, custom_dest, path);
				}
				sn.g_score = new_g_score;
				sn.f_score = sn.g_score + p2p_dist_xy(conn_center, dest_pos);
				open_queue.push(make_pair(-sn.f_score, i->ix));
			} // for i
		} // end while()
		return 0; // failed - no path from room1 to room2
	}
}; // end building_nav_graph_t

cube_t building_t::get_walkable_room_bounds(room_t const &room) const {
	cube_t c(room);
	// Note: regular house rooms start and end at the walls;
	// offices, hallways, and extended basement rooms tile exactly and include half the walls, so we have to subtract those back off
	if (room.inc_half_walls()) {c.expand_by_xy(-0.5*get_wall_thickness());}
	return c;
}

void building_t::build_nav_graph() const { // Note: does not depend on room geom

	assert(interior);
	if (interior->nav_graph && !interior->nav_graph->invalid) return; // already built
	interior->nav_graph.reset(new building_nav_graph_t(0.5*get_window_vspace())); // set stairs_extend == doorway width
	building_nav_graph_t &ng(*interior->nav_graph);
	float const wall_width(get_wall_thickness());
	unsigned const num_rooms(interior->rooms.size()), num_stairs(interior->stairwells.size());
	bool const has_ramp(has_pg_ramp());
	ng.set_num_rooms(num_rooms, num_stairs, has_ramp);
	for (unsigned s = 0; s < num_stairs; ++s) {ng.set_stairs_bcube(s, interior->stairwells[s]);}
	if (has_ramp) {ng.set_ramp_bcube(interior->pg_ramp);}

	for (unsigned r = 0; r < num_rooms; ++r) {
		room_t const &room(interior->rooms[r]);
		cube_t c(get_walkable_room_bounds(room));
		if (room.is_hallway) {ng.mark_hallway(r);}
		ng.set_room_bcube(r, c);
		c.expand_by_xy(wall_width); // to include adjacent doors
		if (is_room_adjacent_to_ext_door(c)) {ng.mark_exit(r);}

		for (auto d = interior->door_stacks.begin(); d != interior->door_stacks.end(); ++d) {
			// if SPLIT_DOOR_PER_FLOOR, we should only add this door if it's on the same floor as our graph but that doesn't work because the graph is shared across all floors,
			// so instead we'll have to record the door index and check the correct door during path finding; it's not valid to test door open/locked state here
			if (!c.intersects_no_adj(*d)) continue; // door not adjacent to this room
			cube_t dc(*d);
			dc.expand_by_xy(wall_width); // to include adjacent rooms
			assert(d->first_door_ix < interior->doors.size());

			for (unsigned r2 = r+1; r2 < num_rooms; ++r2) { // check rooms with higher index (since graph is bidirectional)
				if (dc.intersects_no_adj(interior->rooms[r2])) {ng.connect_rooms(r, r2, d->first_door_ix, *d); break;}
			}
		} // for d
		for (unsigned s = 0; s < num_stairs; ++s) { // stairs
			stairwell_t const &stairwell(interior->stairwells[s]);

			if (stairwell.stairs_door_ix >= 0 && global_building_params.ai_opens_doors < 2) { // check for open doors; doors on stairs can't be locked
				if (!get_door(stairwell.stairs_door_ix).open) continue; // stairs blocked by closed door, don't connect (even if unlocked)
			}
			if (room.intersects_no_adj(stairwell)) {ng.connect_stairs(r, s, stairwell.dim, stairwell.dir, (stairwell.shape == SHAPE_U), 0);} // is_ramp=0
		}
		if (room.is_hallway) { // check for connected hallways
			for (unsigned r2 = r+1; r2 < num_rooms; ++r2) { // check rooms with higher index (since graph is bidirectional)
				room_t const &room2(interior->rooms[r2]);
				if (!room2.is_hallway || room2.z1() != room.z1() || !room2.intersects(c)) continue; // not a connected hallway
				cube_t conn_cube(c);
				conn_cube.intersect_with_cube(room2);
				ng.connect_rooms(r, r2, -1, conn_cube);
			} // for r2
		}
		if (room.get_room_type(0) == RTYPE_PARKING && has_ramp && room.intersects_no_adj(interior->pg_ramp)) { // include parking garage ramp
			bool const dim(interior->pg_ramp.ix >> 1), dir(interior->pg_ramp.ix & 1);
			ng.connect_stairs(r, 0, dim, dir, 0, 1); // stairs_ix=0, is_u=0, is_ramp=1
		}
		//for (unsigned e = 0; e < interior->elevators.size(); ++e) {} // elevators are ignored here by the AI; should check interior->elevators_disabled
	} // for r
	//if (is_house && !has_sec_bldg() && has_basement() && !ng.is_fully_connected()) {cout << "bcube " << bcube.str() << endl;}
}

void building_t::invalidate_nav_graph() { // Note: this is safe to call in one thread while using in another
	if (interior && interior->nav_graph) {interior->nav_graph->invalid = 1;}
}

unsigned building_t::count_connected_room_components() {
	if (!interior) return 0;
	build_nav_graph();
	unsigned const num(interior->nav_graph->count_connected_components());
	interior->nav_graph.reset(); // no longer needed
	return num;
}

bool building_t::is_room_adjacent_to_ext_door(cube_t const &room, bool front_door_only) const {
	cube_t room_exp(room);
	room_exp.expand_by_xy(get_wall_thickness());

	for (auto d = doors.begin(); d != doors.end(); ++d) { // exterior doors
		if (!d->is_exterior_door() || d->type == tquad_with_ix_t::TYPE_RDOOR) continue;
		if (room_exp.contains_pt(d->get_bcube().get_cube_center())) return 1;
		if (front_door_only) return 0; // assumes the first door is the front door
	}
	return 0;
}

// Note: this is somewhat slow, should we build and use the nav graph? maybe not since this is only called for room assiggnment on the first floor of houses
bool building_t::are_rooms_connected_without_using_room(unsigned room1, unsigned room2, unsigned room_exclude) const {
	unsigned const num_rooms(interior->rooms.size());
	assert(room1 < num_rooms && room2 < num_rooms);
	assert(room_exclude != room1 && room_exclude != room2 && room_exclude < num_rooms);
	if (room1 == room2) return 1;
	float const wall_width(get_wall_thickness());
	bool const use_bit_mask(num_rooms <= 64); // almost always true
	static vector<unsigned> pend; // reused across calls
	pend.clear();
	pend.push_back(room1);
	static vector<uint8_t> seen; // reused across calls
	uint64_t seen_mask(0);

	if (use_bit_mask) {seen_mask |= ((1ULL << room1) | (1ULL << room_exclude));}
	else { // must use seen vector
		seen.clear();
		seen.resize(num_rooms, 0);
		seen[room1       ] = 1;
		seen[room_exclude] = 1; // mark as seen so that we won't visit it
	}
	while (!pend.empty()) { // flood fill - maybe A* is better, but that's a lot of work
		unsigned const cur(pend.back());
		pend.pop_back();
		cube_t room(interior->rooms[cur]);
		room.expand_by_xy(wall_width); // to include adjacent doors

		for (auto d = interior->door_stacks.begin(); d != interior->door_stacks.end(); ++d) {
			if (!room.intersects_no_adj(*d)) continue; // door not adjacent to this room
			cube_t dc(*d);
			dc.expand_by_xy(wall_width); // to include adjacent rooms

			for (unsigned r = 0; r < num_rooms; ++r) {
				if ((use_bit_mask ? (seen_mask & (1ULL << r)) : seen[r]) || !dc.intersects_no_adj(interior->rooms[r])) continue;
				if (r == room2) return 1; // found, done
				pend.push_back(r);
				if (use_bit_mask) {seen_mask |= (1ULL << r);} else {seen[r] = 1;}
			}
		} // for d
	} // end while()
	return 0; // room2 not found
}

// Warning: this may be called from a different thread from the one that uses it for AI updates
void building_t::register_player_in_building(point const &camera_bs, unsigned building_id) const {
	bool const prev_was_valid(cur_player_building_loc.is_valid());
	unsigned const old_floor_ix(cur_player_building_loc.floor_ix);
	if (animate2) {prev_player_building_loc = cur_player_building_loc;} // only update previous pos when AI is running so that it doesn's miss a floor or room change
	cur_player_building_loc = building_dest_t(get_building_loc_for_pt(camera_bs), camera_bs, building_id);
	unsigned const new_floor_ix(cur_player_building_loc.floor_ix);
	if (prev_was_valid && old_floor_ix != new_floor_ix) {register_player_change_floor(old_floor_ix, new_floor_ix);}
}
void register_player_not_in_building() {
	prev_player_building_loc = cur_player_building_loc = building_dest_t();
}

bool is_ai_coll_obj(room_object_t const &c, bool same_as_player) {
	// these object types are not collided with by people and can be skipped
	if (c.no_coll() || c.is_dynamic() || c.type == TYPE_LG_BALL) return 0; // skip dynamic objects (balls, etc.)
	if (!(same_as_player ? c.is_player_collidable() : bldg_obj_types[c.type].ai_coll)) return 0;
	return 1;
}

bool building_t::choose_dest_goal(person_t &person, rand_gen_t &rgen) const { // used for following the player in gameplay mode

	assert(interior && interior->nav_graph);
	building_loc_t const loc(get_building_loc_for_pt(person.pos));
	if (loc.room_ix < 0) return 0; // not in a room
	float const floor_spacing(get_window_vspace());
	building_dest_t goal;
	point sound_pos;

	if ((global_building_params.ai_target_player || cur_player_building_loc.same_room_floor(loc)) && can_target_player(person)) {
		goal = cur_player_building_loc; // player is in a different room of our building, or we're following the player's position
		person.goal_type = GOAL_TYPE_PLAYER;
	}
	else if (can_ai_follow_player(person) && get_closest_building_sound(person.pos, sound_pos, floor_spacing)) { // target the loudest sound
		// check if the sound is coming from somewhere unreachable such as a closet the player is hiding inside, then move it out into the center of the room;
		// maybe this location is also unreachable, but at least it will get the zombie into the correct room instead of have them wait at the wall in an adjacent room
		assert(has_room_geom());
		auto objs_end(interior->room_geom->get_placed_objs_end()); // skip buttons/stairs/elevators

		for (auto c = interior->room_geom->objs.begin(); c != objs_end; ++c) {
			if (!is_ai_coll_obj(*c, 1))     continue; // same_as_player=1
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
	if (!goal.is_valid()) return 0; // player or sound
	unsigned const cand_room(goal.room_ix);
	room_t const &room(get_room(cand_room)); // target room
	if (!interior->nav_graph->is_room_connected_to(loc.room_ix, cand_room, interior->doors, person.pos.z, person.has_key)) return 0; // unreachable
	person.cur_room     = loc.room_ix;
	person.dest_room    = cand_room; // set but not yet used
	person.target_pos   = (global_building_params.ai_target_player ? goal.pos: get_center_of_room(cand_room));
	person.target_pos.z = person.pos.z; // keep orig zval to stay on the same floor
	person.is_on_stairs = 0;

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
		// handle the case where the player is standing on the stairs on the same floor by moving zval to a different floor to force this person to use the stairs; what about pg_ramp?
		if (person.goal_type == GOAL_TYPE_PLAYER && loc.floor_ix == goal.floor_ix && goal.stairs_ix >= 0) {
			float const person_z1(person.get_z1()), player_z1(cur_player_building_loc.pos.z - CAMERA_RADIUS - get_player_height()), fc_thick(get_fc_thickness());
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
		static vect_cube_t avoid; // reuse across frames/people
		// same_as_player=1, skip_stairs=1
		interior->get_avoid_cubes(avoid, (person.target_pos.z - person.radius), (person.target_pos.z + z2_add), 0.5*person.radius, get_floor_thickness(), 1, 1);

		// check for initial collisions at the player's location, but exclude stairs in case the player is standing on them;
		// this may no longer be required since complete_path_within_room() now ignores initial collisions with the dest, but is likely still a good idea
		for (unsigned n = 0; n < 4; ++n) { // iterate a few times in case a collision moves pos into another object
			bool any_updated(0);

			for (auto i = avoid.begin(); i != avoid.end(); ++i) { // move target_pos to avoid room objects
				cube_t c(*i);
				c.expand_by_xy(coll_dist);
				any_updated |= sphere_cube_int_update_pos(person.target_pos, 1.01*coll_dist, c, person.pos, 1); // skip_z=1, ignore return value
			}
			if (!any_updated) break; // done
		} // for n
	}
	return 1;
}

bool building_t::select_person_dest_in_room(person_t &person, rand_gen_t &rgen, room_t const &room) const {
	float const height(0.7*get_window_vspace()), radius(COLL_RADIUS_SCALE*person.radius);
	point dest_pos(room.get_cube_center());
	static vect_cube_t avoid; // reuse across frames/people
	get_avoid_cubes(person.target_pos.z, height, radius, avoid, 0); // following_player=0
	bool const no_use_init(is_room_pg_or_backrooms(room)); // don't use the room center for a parking garage or backrooms
	if (!interior->nav_graph->find_valid_pt_in_room(avoid, *this, radius, height, person.target_pos.z, room, rgen, dest_pos, no_use_init)) return 0;
	
	if (!is_cube()) { // non-cube building
		cube_t const person_bcube(person.get_bcube() + (dest_pos - person.pos)); // bcube, but translated to dest_pos
		if (!check_cube_within_part_sides(person_bcube)) return 0; // outside building - will try again with a new pos next time
	}
	person.target_pos.x = dest_pos.x;
	person.target_pos.y = dest_pos.y;
	person.goal_type    = GOAL_TYPE_ROOM;
	return 1;
}

point get_pos_to_stand_for_elevator(person_t const &person, elevator_t const &e, float floor_spacing) {
	point pos(person.pos);
	pos[!e.dim] = e.get_center_dim(!e.dim); // centered on the elevator
	pos[ e.dim] = e.d[e.dim][e.dir] + (e.dir ? 1.0 : -1.0)*0.4*floor_spacing; // stand in front of the elevator door
	return pos;
}

int building_t::choose_dest_room(person_t &person, rand_gen_t &rgen) const { // used for randomly walking around the building

	assert(interior && interior->nav_graph);
	building_loc_t const loc(get_building_loc_for_pt(person.pos));
	if (loc.room_ix < 0) return 0; // not in a room
	person.dest_room    = person.cur_room = loc.room_ix; // set to the same room and pos just in case
	person.target_pos   = person.pos; // set but not yet used
	person.is_on_stairs = 0;
	if (interior->rooms.size() == 1) return 0; // no other room to move to
	float const floor_spacing(get_window_vspace());

	// maybe choose a destination elevator (for office buildings) if our prev dest wasn't an elevator
	if (!is_house && !person.is_first_path && !interior->elevators_disabled && !person.last_used_elevator &&
		person.goal_type != GOAL_TYPE_ELEVATOR && global_building_params.elevator_capacity > 0)
	{
		if (global_building_params.use_elevator_prob >= 1.0 || (global_building_params.use_elevator_prob > 0.0 && rgen.rand_float() < global_building_params.use_elevator_prob)) {
			int const nearest_elevator(find_nearest_elevator_this_floor(person.pos));

			if (nearest_elevator >= 0) {
				elevator_t const &e(get_elevator(nearest_elevator));
				int const elevator_room(get_room_containing_pt(point(e.xc(), e.yc(), person.pos.z))); // room containing elevator center at person zval
				assert(elevator_room >= 0); // elevator must be in a valid room

				// Note: to simplify multiple AI logic, we could check e.in_use here and not even go to the elevator if someone else is using it
				if (interior->nav_graph->is_room_connected_to(loc.room_ix, elevator_room, interior->doors, person.pos.z, person.has_key)) { // check if reachable
					person.target_pos   = get_pos_to_stand_for_elevator(person, e, floor_spacing);
					person.dest_room    = elevator_room;
					person.goal_type    = GOAL_TYPE_ELEVATOR;
					person.cur_elevator = (uint8_t)nearest_elevator;
					person.last_used_elevator = 1;
					return 1;
				}
			}
		}
	}
	if (is_pos_in_pg_or_backrooms(person.pos)) {
		if (person.cur_room >= 0 && (person.is_first_path || rgen.rand_float() < 0.75)) { // stay in this room 75% of the time, always on the first path
			person.is_first_path = 0; // respect the walls
			if (select_person_dest_in_room(person, rgen, get_room(person.cur_room))) return 1;
			// if we failed above (for example, stuck inside a small sub-room), we'll walk through the walls to get out of the room
		}
	}

	// make 100 attempts at finding a valid room
	for (unsigned n = 0; n < 100; ++n) {
		unsigned const cand_room(rgen.rand() % interior->rooms.size());
		if (cand_room == (unsigned)loc.room_ix) continue;
		room_t const &room(interior->rooms[cand_room]);
		if (room.is_hallway) continue; // don't select a hallway
		if ((person.pos.z + floor_spacing) < room.z1() || (person.pos.z - floor_spacing) > room.z2()) continue; // room more than one floor above/below current pos
		// allow move to a different stacked part 25% of the time; 100% of the time for parking garages, since they're more rare
		if ((person.pos.z < room.z1() || person.pos.z > room.z2()) && !(has_parking_garage && room.z1() < ground_floor_z1) && (rgen.rand()&3) != 0) continue;
		if (!interior->nav_graph->is_room_connected_to(loc.room_ix, cand_room, interior->doors, person.pos.z, person.has_key)) continue;
		person.dest_room    = cand_room; // set but not yet used
		person.target_pos   = get_center_of_room(cand_room);
		person.target_pos.z = person.pos.z; // keep orig zval to stay on the same floor

		if (person.pos.z > room.z2()) { // room is below the person
			float const new_z(person.target_pos.z - floor_spacing);
			assert(new_z > room.z1() && new_z < room.z2());
			person.target_pos.z = new_z; // target the floor below
		}
		else if (person.pos.z < room.z1()) { // room is above the person
			float const new_z(person.target_pos.z + floor_spacing);
			assert(new_z > room.z1() && new_z < room.z2());
			person.target_pos.z = new_z; // target the floor above
		}
		else if (interior->ext_basement_hallway_room_id < 0 || cand_room < (unsigned)interior->ext_basement_hallway_room_id) { // skip for ext basement rooms
			// room covers floor this person is on; allow moving to a different floor, currently only one floor at a time
			cube_t const &part(get_part_for_room(room)); // or just use the room?
			unsigned const rand_val(rgen.rand() & 3); // 0-3

			if (rand_val == 0) { // try one floor below
				float const new_z(person.target_pos.z - floor_spacing);
				if (new_z > part.z1()) {person.target_pos.z = new_z;} // change if there is a floor below
			}
			else if (rand_val == 1) { // try one floor above
				float const new_z(person.target_pos.z + floor_spacing);
				if (new_z < part.z2()) {person.target_pos.z = new_z;} // change if there is a floor above
			}
			assert(person.target_pos.z > room.z1() && person.target_pos.z < room.z2());
		}
		person.goal_type = GOAL_TYPE_ROOM;
		return 1;
	} // for n
	if (loc.room_ix < 0) return 2; // failed - room not valid (error?)
	room_t const &room(get_room(loc.room_ix));
	bool const is_single_large_room(is_room_pg_or_backrooms(room));

	// how about a different floor of the same room? only check this 50% of the time for parking garages to allow movement within a level
	if (room.has_stairs == 255 && (!is_single_large_room || rgen.rand_bool())) {
		float const new_z(person.target_pos.z + (rgen.rand_bool() ? -1.0 : 1.0)*floor_spacing); // one floor above or below

		if (new_z > room.z1() && new_z < room.z2()) { // valid if this floor is inside the room
			person.target_pos.z = new_z;
			if (select_person_dest_in_room(person, rgen, room)) return 1;
		}
	}
	// how about a different location in the same room? this will at least get the person unstuck from an object and moving inside a parking garage
	if (person.is_first_path || is_single_large_room) {
		if (select_person_dest_in_room(person, rgen, room)) return 1;
	}
	return 2; // failed, but can retry
}

template<typename T> void add_bcube_if_overlaps_zval(vector<T> const &cubes, vect_cube_t &out, float z1, float z2) {
	for (auto i = cubes.begin(); i != cubes.end(); ++i) {
		if (i->z1() < z2 && i->z2() > z1) {out.push_back(*i);}
	}
}

// for AI collision detection
void building_interior_t::get_avoid_cubes(vect_cube_t &avoid, float z1, float z2, float r_shrink_if_low, float floor_thickness,
	bool same_as_player, bool skip_stairs, cube_t const *const fires_select_cube) const
{
	avoid.clear();
	if (!skip_stairs) {add_bcube_if_overlaps_zval(stairwells, avoid, z1, z2);} // clearance not required
	add_bcube_if_overlaps_zval(elevators, avoid, z1, z2); // clearance not required
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
		//if (c->type == TYPE_ATTIC_DOOR && (c->flags & RO_FLAG_IN_HALLWAY)) continue; // skip open attic doors in hallways because they block the path too much
		
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
				for (unsigned i = 0; i < num_cubes; ++i) {avoid.push_back(sides[i]);}
				continue;
			}
			else if (c->type == TYPE_SHOWER) {
				cube_t sides[2];
				unsigned const num_cubes(get_shower_cubes(*c, sides));
				for (unsigned i = 0; i < num_cubes; ++i) {avoid.push_back(sides[i]);}
				continue;
			}
		}
		cube_t bc(get_true_room_obj_bcube(*c)); // needed for open attic door
		if (bc.z1() > z2 || bc.z2() < z1) continue;

		if (r_shrink_if_low > 0.0 && c->z2() < z_thresh && c->shape == SHAPE_CUBE) { // shrink cube if it's low; applies to boxes and crates on the floor
			bc.expand_by_xy(-min(0.95f*0.5f*min(c->dx(), c->dy()), r_shrink_if_low)); // make sure it doesn't shrink to zero area
		}
		avoid.push_back(bc);
		
		if (same_as_player && c->type == TYPE_TABLE && c->shape == SHAPE_CYLIN) {
			// special handling of round table so that the player can't hide in the area unreachable from the bounding cube
			float const shrink_val(c->get_radius()/SQRT2); // small shrink
			avoid.back().expand_by(-shrink_val);

			for (unsigned d = 0; d < 2; ++d) { // create a cross shape
				avoid.push_back(bc);
				avoid.back().expand_in_dim(d, -2.0*shrink_val);
			}
		}
	} // for c
	if (conn_info != nullptr) { // include extended basement connector doors because building people can't pass through these yet
		for (auto const &c : conn_info->conn) {
			for (auto const &room : c.rooms) {
				if (room.door_is_b) continue; // door is for the other building
				door_t const &door(get_door(room.door_ix));
				avoid.push_back(door.get_true_bcube());
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

bool building_t::stairs_contained_in_part(stairwell_t const &s, cube_t const &p) const {
	cube_t sc(s);
	if (s.roof_access) {sc.z2() -= get_window_vspace();} // clip off top floor roof access
	return p.contains_cube(sc);
}
void building_t::find_nearest_stairs_or_ramp(point const &p1, point const &p2, vector<unsigned> &nearest_stairs, int part_ix) const {
	nearest_stairs.clear();
	assert(interior);
	assert(part_ix < 0 || (unsigned)part_ix < parts.size());
	float const zmin(min(p1.z, p2.z)), zmax(max(p1.z, p2.z));
	vector<pair<float, unsigned>> sorted;

	for (unsigned s = 0; s < interior->stairwells.size(); ++s) {
		stairwell_t const &stairs(interior->stairwells[s]);
		if (zmin < stairs.z1() || zmax > stairs.z2()) continue; // stairs don't span the correct floors
		if (part_ix >= 0 && !stairs_contained_in_part(stairs, parts[part_ix])) continue; // stairs don't belong to this part (Note: this option is currently unused)
		point const center(stairs.get_cube_center());
		sorted.emplace_back((p2p_dist(p1, center) + p2p_dist(center, p2)), s);
	} // for s
	if ((part_ix < 0 || part_ix == basement_part_ix) && has_pg_ramp() && zmax < ground_floor_z1) { // parking garage ramp
		point const center(interior->pg_ramp.get_cube_center());
		sorted.emplace_back((p2p_dist(p1, center) + p2p_dist(center, p2)), interior->stairwells.size());
	}
	sort(sorted.begin(), sorted.end()); // sort by distance, min first
	for (auto s = sorted.begin(); s != sorted.end(); ++s) {nearest_stairs.push_back(s->second);}
}
int building_t::find_nearest_elevator_this_floor(point const &pos) const {
	int nearest(-1); // -1 => none found
	float dmin_sq(0.0);

	for (auto e = interior->elevators.begin(); e != interior->elevators.end(); ++e) {
		if (e->z1() > pos.z || e->z2() < pos.z) continue; // doesn't span the correct floor
		// skip if elevator is currently in use?
		float const dsq(p2p_dist_xy_sq(pos, e->get_cube_center()));
		if (nearest < 0 || dsq < dmin_sq) {nearest = (e - interior->elevators.begin()); dmin_sq = dsq;}
	}
	return nearest;
}

cube_t get_stairs_plus_step_up(stairwell_t const &stairs) {
	cube_t stairs_ext(stairs);
	stairs_ext.d[stairs.dim][!stairs.dir] += (stairs.dir ? -1.0 : 1.0)*stairs.get_sz_dim(stairs.dim)/NUM_STAIRS_PER_FLOOR; // location of step up
	return stairs_ext;
}

void building_t::get_avoid_cubes(float zval, float height, float radius, vect_cube_t &avoid, bool following_player, cube_t const *const fires_select_cube) const {
	assert(interior);
	interior->get_avoid_cubes(avoid, (zval - radius), (zval + (height - radius)), 0.5*radius, get_floor_thickness(), following_player, 0, fires_select_cube); // skip_stairs=0
}
bool building_t::find_route_to_point(person_t const &person, float radius, bool is_first_path, bool following_player, vector<point> &path) const {

	assert(interior && interior->nav_graph);
	point const &from(person.pos), &to(person.target_pos);
	path.clear();
	building_loc_t const loc1(get_building_loc_for_pt(from)), loc2(get_building_loc_for_pt(to));
	if (loc1.part_ix < 0 || loc2.part_ix < 0 || loc1.room_ix < 0 || loc2.room_ix < 0) return 0; // not in a room
	assert((unsigned)loc1.part_ix < parts.size() && (unsigned)loc2.part_ix < parts.size());
	assert((unsigned)loc1.room_ix < interior->rooms.size() && (unsigned)loc2.room_ix < interior->rooms.size());
	float const floor_spacing(get_window_vspace()), height(0.7*floor_spacing), z2_add(height - radius); // approximate, since we're not tracking actual heights
	static vect_cube_t avoid; // reuse across frames/people
	get_avoid_cubes(from.z, height, radius, avoid, following_player, &get_room(loc1.room_ix)); // include fires in the current room

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
		return interior->nav_graph->complete_path_within_room(from, dest, loc1.room_ix, person.ssn, radius, person.cur_rseed, is_first_path, following_player, avoid, *this, path);
	}
	if (loc1.floor_ix != loc2.floor_ix) { // different floors: find path from <from> to nearest stairs, then find path from stairs to <to>
		vector<unsigned> nearest_stairs_or_ramp;
		find_nearest_stairs_or_ramp(from, to, nearest_stairs_or_ramp); // pass in loc1.part_ix if both loc part_ix values are equal?
		bool const up_or_down(loc1.floor_ix > loc2.floor_ix); // 0=up, 1=down; Note: floor_ix is relative to bcube.z1() (or ext_basement z1), so is consistent across parts

		for (unsigned s : nearest_stairs_or_ramp) { // try using stairs or ramp, closest to furthest
			bool const is_ramp(s == interior->stairwells.size());
			assert(is_ramp || s < interior->stairwells.size());
			unsigned const stairs_room_ix(s + interior->rooms.size()); // map to graph space (should work for stairs or ramp)
			path.clear();
			vector<point> from_path;
			// Note: passing use_stairs=0 here because it's unclear if we want to go through stairs nodes in our A* algorithm
			// from => stairs/ramp
			if (!interior->nav_graph->find_path_points(loc1.room_ix, stairs_room_ix, person.ssn, radius, height, 0, is_first_path,
				up_or_down, person.cur_rseed, avoid, *this, from, interior->doors, person.has_key, nullptr, from_path)) continue; // no custom_dest
			point const seg2_start(interior->nav_graph->get_stairs_entrance_pt(to.z, stairs_room_ix, !up_or_down)); // other end
			// new floor, new zval, new avoid cubes
			interior->get_avoid_cubes(avoid, (seg2_start.z - radius), (seg2_start.z + z2_add), 0.5*radius, get_floor_thickness(), following_player);
			// stairs/ramp => to
			if (!interior->nav_graph->find_path_points(stairs_room_ix, loc2.room_ix, person.ssn, radius, height, 0, is_first_path,
				!up_or_down, person.cur_rseed, avoid, *this, seg2_start, interior->doors, person.has_key, nullptr, path)) continue; // no custom_dest
			assert(!path.empty() && !from_path.empty());
			path.push_back(seg2_start); // other end of the stairs
			// add two more points to straighten the entrance and exit paths; this segment doesn't check for intersection with stairs
			point enter_pt;

			if (is_ramp) {
				cube_t const walk_area(interior->pg_ramp);
				enter_pt = walk_area.closest_pt(from_path.front());
				path.push_back(walk_area.closest_pt(path.back())); // exit point
			}
			else { // stairs
				stairwell_t const &stairs(interior->stairwells[s]);
				cube_t const stairs_ext(get_stairs_plus_step_up(stairs));
				enter_pt = stairs_ext.closest_pt(from_path.front());
				point const exit_pt(stairs_ext.closest_pt(path.back()));
				path.push_back(exit_pt);

				if (stairs.shape == SHAPE_U) { // add 2 extra points on mid-level landing; entrance and exit will be on the same side
					bool const dim(stairs.dim), dir(stairs.dir); // Note: see code in add_stairs_and_elevators()
					float const turn_pt(stairs.d[dim][dir] - 0.1*(dir ? 1.0 : -1.0)*stairs.get_sz_dim(dim)), seg_delta_z(0.45f*(to.z - from.z));
					point exit_turn(exit_pt.x, exit_pt.y, (to.z - seg_delta_z));
					exit_turn[dim] = turn_pt;
					path.push_back(exit_turn); // turning point for exit side of stairs
					point enter_turn(enter_pt.x, enter_pt.y, (from.z + seg_delta_z));
					enter_turn[dim] = turn_pt;
					path.push_back(enter_turn); // turning point for entrance side of stairs
				}
			}
			path.push_back(enter_pt);
			vector_add_to(from_path, path); // concatenate the two path segments in reverse order
			assert(!path.empty());
			return 1; // done/success
		} // for s
		path.clear(); // not necessary?
		return 0; // failed
	}
	assert(loc1.room_ix != loc2.room_ix);
	// if the target is an elevator, use that as the preferred destination rather than the center of the room
	point const *const custom_dest((person.goal_type == GOAL_TYPE_ELEVATOR) ? &person.target_pos : nullptr);
	if (!interior->nav_graph->find_path_points(loc1.room_ix, loc2.room_ix, person.ssn, radius, height, 0, is_first_path,
		0, person.cur_rseed, avoid, *this, from, interior->doors, person.has_key, custom_dest, path)) return 0;
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
	is_on_stairs = (!starting_path && target_pos.z != path.back().z); // or ramp
	target_pos   = path.back();
	path.pop_back();
}

bool building_t::is_valid_ai_placement(point const &pos, float radius, bool skip_nocoll) const { // for people and animals
	if (!is_pos_inside_building(pos, radius, radius, 1)) return 0; // required for attic; for_attic=1
	cube_t ai_bcube(pos);
	ai_bcube.expand_by(radius); // expand more in Z?
	if (!is_valid_stairs_elevator_placement(ai_bcube, radius)) return 0;

	// Note: people are placed before room geom is generated for all buildings, so this may not work and will have to be handled during room geom placement
	if (interior->room_geom) { // check placement against room geom objects
		auto objs_end(interior->room_geom->get_placed_objs_end()); // skip buttons/stairs/elevators

		for (auto i = interior->room_geom->objs.begin(); i != objs_end; ++i) {
			if (skip_nocoll && i->no_coll()) continue;
			if (i->type == TYPE_FLOORING || i->type == TYPE_BLOCKER) continue; // okay to place on flooring; ignore blockers (used for placement clearance)
			if (i->intersects(ai_bcube)) return 0;
		}
	}
	return 1;
}

struct room_cand_t {
	unsigned room_ix, floor_ix;
	room_cand_t(unsigned r, unsigned f) : room_ix(r), floor_ix(f) {}
};

bool building_t::place_people_if_needed(unsigned building_ix, float radius, vector<point> &locs) const {
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
	unsigned const num_people(num_min + (rgen.rand()%(num_max - num_min + 1)));
	if (num_people == 0) return 0;
	float const window_vspacing(get_window_vspace()), floor_thickness(get_floor_thickness()), fc_thick(0.5*floor_thickness);
	// weight rooms by number of floors to avoid placing too many people in basements
	static vector<room_cand_t> room_cands;
	room_cands.clear();
	unsigned first_basement_room(0);

	for (auto r = interior->rooms.begin(); r != interior->rooms.end(); ++r) { // add room_cands
		if (r->is_sec_bldg) continue; // don't place people in garages and sheds
		if (min(r->dx(), r->dy()) < 3.0*radius) continue; // room to small to place a person
		unsigned const num_floors(calc_num_floors(*r, window_vspacing, floor_thickness));
		assert(num_floors > 0);
		if (first_basement_room == 0 && r->z1() < ground_floor_z1) {first_basement_room = room_cands.size();}
		unsigned const room_ix(r - interior->rooms.begin());

		for (unsigned f = 0; f < num_floors; ++f) {
			if (r->lit_by_floor && !r->is_lit_on_floor(f)) continue; // don't place person in an unlit room; only applies if room lighting has been calculated
			unsigned const num_cands(is_room_backrooms(*r) ? 4 : 1); // add 4x for backrooms since this is one room with many sub-rooms
			for (unsigned n = 0; n < num_cands; ++n) {room_cands.emplace_back(room_ix, f);}
		}
	} // for r
	for (unsigned N = 0; N < num_people && !room_cands.empty(); ++N) {
		unsigned const max_cand_ix((N == 0 && first_basement_room > 0) ? first_basement_room : room_cands.size()); // place the first person in a non-basement room

		for (unsigned n = 0; n < 10; ++n) { // make 10 attempts at choosing a room
			unsigned const cand_ix(rgen.rand() % max_cand_ix);
			room_cand_t const &cand(room_cands[cand_ix]);
			room_t const &room(get_room(cand.room_ix));
			float const zval(room.z1() + fc_thick + window_vspacing*cand.floor_ix);
			bool success(0);

			for (unsigned m = 0; m < 100; ++m) { // make 100 attempts at choosing a position in this room
				point const pos(gen_xy_pos_in_area(room, radius, rgen, zval)); // random XY point inside this room
				if (!is_valid_ai_placement(pos, radius, 1)) continue; // skip_nocoll=1
				locs.push_back(pos);
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
	return 1;
}

bool can_ai_follow_player(person_t const &person, bool allow_diff_building) {
	if (!ai_follow_player()) return 0; // disabled
	if (!cur_player_building_loc.is_valid()) return 0; // no target
	if (!allow_diff_building && cur_player_building_loc.building_ix != person.cur_bldg) return 0; // wrong building
	if (player_is_hiding && !person.saw_player_hide) return 0; // ignore player in closet, bathroom stall, or shower with door closed, and we didn't see them hide
	if (person.retreat_time > 0.0) return 0; // ignore the player if retreating
	return 1;
}
bool has_nearby_sound(person_t const &person, float floor_spacing) {
	if (!can_ai_follow_player(person)) return 0; // no need to track sounds
	point sound_pos; // unused
	return get_closest_building_sound(person.pos, sound_pos, floor_spacing);
}

bool building_t::same_room_and_floor_as_player(person_t const &person) const {
	return (cur_player_building_loc.room_ix == person.cur_room && cur_player_building_loc.floor_ix == get_floor_for_zval(person.pos.z) && cur_player_building_loc.stairs_ix < 0);
}
bool building_t::is_player_visible(person_t const &person, unsigned vis_test) const {
	if (vis_test == 0) return 1; // no visibility test
	building_dest_t const &target(cur_player_building_loc);
	float const player_radius(get_scaled_player_radius());
	point const pp2(target.pos - vector3d(0.0, 0.0, get_player_height())); // player's bottom sphere
	bool const same_room(person.cur_room >= 0 && cur_player_building_loc.room_ix == person.cur_room);
	unsigned const person_floor_ix(get_floor_for_zval(person.pos.z));
	unsigned const floor_delta(abs((int)cur_player_building_loc.floor_ix - (int)person_floor_ix));
	bool const same_room_and_floor(same_room && floor_delta == 0); // Note: doesn't check cur_player_building_loc.stairs_ix
	bool has_los(same_room_and_floor); // assume that we have a line of sight if we're in the same room and floor as the player (optimization)

	if (!has_los && same_room && floor_delta == 1 && person.pos.z > ground_floor_z1) {
		// if the person and the player are on adjacent floors of the same room connected by stairs (and not in a parking garage), cheat and say they have a line of sight
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
	if (!can_ai_follow_player(person)) return 0;
	return is_player_visible(person, global_building_params.ai_player_vis_test); // 0=no test, 1=LOS, 2=LOS+FOV, 3=LOS+FOV+lit
}

bool building_t::need_to_update_ai_path(person_t const &person) const {
	if (!global_building_params.ai_target_player || !can_ai_follow_player(person) || !interior) return 0; // disabled
	building_dest_t const &target(cur_player_building_loc);
	bool const same_room(same_room_and_floor_as_player(person)); // check room and floor

	if (global_building_params.ai_player_vis_test == 0) { // the below logic only applies if there's no line of sight test and is an optimization
		// if the player's room has not changed, and the person is not yet in this room, continue on the same path to the dest room;
		// however, if the path is empty, continue to choose a new path (needed for AI more than one floor away from target to take multiple flights of stairs)
		if (target.same_room_floor(prev_player_building_loc) && !same_room && !person.on_new_path_seg && !person.path.empty()) return 0;
	}
	//if (same_room && person.path.size() > 1) return 0; // same room but path has a jog, continue on existing path (faster, but slower to adapt to player position change)
	if (dist_less_than(person.pos, target.pos, person.radius)) return 0; // already close enough
	if (person.on_stairs()) return 0; // don't change paths when on the stairs
	float const floor_spacing(get_window_vspace());

	if (int(target.pos.z/floor_spacing) != int(prev_player_building_loc.pos.z/floor_spacing)) { // if player did not change floors
		if (fabs(person.pos.z - target.pos.z) > 2.0f*floor_spacing) return 0; // person and player are > 2 floors apart, continue toward stairs (or should it be one floor apart?) (optimization)
	}
	if (can_target_player(person)) { // have player visibility
		if (person.goal_type == GOAL_TYPE_PLAYER && target.same_room_floor(prev_player_building_loc) && !same_room && !person.on_new_path_seg && !person.path.empty()) return 0;
		return 1;
	}
	if (has_nearby_sound(person, floor_spacing)) return 1; // new sound source
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

unsigned get_elevator_floor(float zval, elevator_t const &e, float floor_spacing) { // floor index relative to this elevator
	return max(0.0f, (zval - e.z1()))/floor_spacing;
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
		if (i->type == TYPE_BLOCKER) continue; // button was removed?
		assert(i->type == TYPE_BUTTON);
		if (i->obj_id != floor_ix) continue; // wrong floor
		if (i->in_elevator() != is_inside_elevator) continue; // wrong inside/outside
		if (!is_inside_elevator && (i->flags & (is_up ? RO_FLAG_ADJ_BOT : RO_FLAG_ADJ_TOP))) continue; // wrong up/down button
		i->flags |= RO_FLAG_IS_ACTIVE; // set active/lit state
		interior->room_geom->invalidate_small_geom(); // need to regen object data due to lit state change; should be thread safe
		break; // only one button
	} // for i
}

int building_t::run_ai_elevator_logic(person_t &person, float delta_dir, rand_gen_t &rgen) {
	assert(person.goal_type == GOAL_TYPE_ELEVATOR);
	if (!has_room_geom()) return person.ai_state; // if player has moved away and room_geom was deleted, remain in the current state
	float const floor_spacing(get_window_vspace());
	elevator_t &e(get_elevator(person.cur_elevator));
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
					else if (rgen.rand_float() < global_building_params.elevator_wait_recall_prob) { // wait and press the button again some of the time
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
		bool const is_turning(person_slow_turn(person, get_pos_to_stand_for_elevator(person, e, floor_spacing), 0.5*delta_dir)); // slow turn to face the elevator door

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
		point const target_dest(get_pos_to_stand_for_elevator(person, e, floor_spacing));
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

// Note: non-const because this updates room light and door state
int building_t::ai_room_update(person_t &person, float delta_dir, unsigned person_ix, rand_gen_t &rgen) {

	if (person.speed == 0.0) {person.anim_time = 0.0; return AI_STOP;} // stopped
	assert(interior);
	if (!interior->room_geom && frame_counter < 60) {person.anim_time = 0.0; return AI_WAITING;} // wait until room geom is generated for this building
	float const coll_dist(COLL_RADIUS_SCALE*person.radius);
	float &wait_time(person.waiting_start); // reuse this field
	float speed_mult(1.0);
	person.following_player = person.is_stopped = 0; // reset for this frame
	// skip the same building check for coll if both this person and the player may be in different but connected buildings
	bool allow_diff_building(interior->conn_info && person.pos.z < ground_floor_z1 && cur_player_building_loc.pos.z < ground_floor_z1 &&
		dist_xy_less_than(person.pos, cur_player_building_loc.pos, 2.0*get_window_vspace()));

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
			// check for other people colliding with this person and handle it
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
	if (person.ai_state >= AI_WAIT_ELEVATOR) {return run_ai_elevator_logic(person, delta_dir, rgen);} // handle elevator case
	person.must_re_call_elevator = 0; // reset if we got out of the elevator logic with this set
	build_nav_graph();

	if (can_ai_follow_player(person, allow_diff_building)) {
		// use zval of the feet to handle cases where the person and the player are different heights
		point const feet_pos(person.pos.x, person.pos.y, person.get_z1()), player_feet_pos(cur_player_building_loc.pos - vector3d(0.0, 0.0, CAMERA_RADIUS+get_player_height()));

		if (dist_less_than(feet_pos, player_feet_pos, 1.2f*(person.radius + get_scaled_player_radius()))) { // intersecting the player
			if (!check_for_wall_ceil_floor_int(person.pos, cur_player_building_loc.pos, 1)) { // inc_pg_br_walls=1
				int const ret_status(register_ai_player_coll(person.has_key, person.get_height())); // return value: 0=no effect, 1=player is killed, 2=this person is killed
				// player is killed, we could track kills here
				if (ret_status == 1) {add_blood_decal(cur_player_building_loc.pos, get_scaled_player_radius());}
				else if (ret_status == 2) {return AI_TO_REMOVE;} // player defeats zombie, remove it
			}
		}
		if (player_is_hiding && person.saw_player_hide && global_building_params.ai_opens_doors && global_building_params.ai_sees_player_hide >= 2 &&
			player_hiding_obj.type != TYPE_NONE && same_room_and_floor_as_player(person) && cur_player_building_loc.building_ix == person.cur_bldg)
		{
			// open the closet, stall, or shower door
			// it may not be safe to use an object iterator or even an index since we're in a different thread, so do a linear search for the target object
			auto objs_end(interior->room_geom->get_placed_objs_end()); // skip buttons/stairs/elevators
			cube_t coll_cube(person.get_bcube());
			coll_cube.expand_by_xy(1.5*person.radius);

			for (auto i = interior->room_geom->objs.begin(); i != objs_end; ++i) {
				if (*i != player_hiding_obj) continue;
				if (i->is_open()) break; // already open
				if (!i->intersects(coll_cube)) break; // too far away (uses full object, not door, but should be close enough)
				i->flags |= RO_FLAG_OPEN; // open the door
				play_open_close_sound(*i, person.pos);
				interior->room_geom->update_draw_state_for_room_object(*i, *this, 0);
				break; // done
			} // for i
		}
	}
	bool choose_dest(!person.target_valid());
	bool const update_path(need_to_update_ai_path(person)), has_rgeom(has_room_geom());
	// if room objects spawn in, select a new dest to avoid walking through objects based on our previous, possibly invalid path
	if (has_rgeom && !person.has_room_geom) {person.abort_dest();}
	person.has_room_geom = has_rgeom;

	if (update_path) { // need to update based on player movement; higher priority than choose_dest
		if (choose_dest_goal(person, rgen) != 1 || // check if person can reach the target
			!find_route_to_point(person, coll_dist, 0, 1, person.path)) // is_first_path=0, following_player=1
		{
			choose_dest = 1; // or increment person.cur_rseed and return AI_WAITING? or restore person to prev value?
		}
		else { // success
			person.next_path_pt(1);
			person.following_player = 1;
			choose_dest = 0;
			speed_mult  = 1.5; // faster when the player is in the same room
			// run logic to play zombie sounds
			bool const same_room_and_floor(same_room_and_floor_as_player(person));
			bool play_sound(same_room_and_floor); // always play sound if in the same room and floor
			if (!play_sound && (person_ix & 1)) {play_sound |= is_player_visible(person, 1);} // 50% of zombies use line of sight test
			if (!play_sound && (person_ix & 2)) {play_sound |= has_nearby_sound(person, get_window_vspace());} // 50% of zombies use sound test
			if (play_sound) {maybe_play_zombie_sound(person.pos, person_ix, same_room_and_floor);} // alert other zombies if in the same room and floor as the player
		}
	}
	if (choose_dest) { // no current destination, choose a new destination
		++person.cur_rseed; // update rseed so that this person will choose a different path if the previous path finding call failed
		if (!update_path) {person.anim_time = 0.0;} // reset animation if not a failed update path frame
		int const ret(choose_dest_room(person, rgen)); // 0=failed, 1=success, 2=failed but can retry

		if (ret == 2 && interior->door_state_updated) { // wait rather than stopping in case the player trapped this person in a room by closing the door
			person.wait_for(2.0); // stop for 2 seconds, then try again
			return AI_WAITING;
		}
		else if (ret != 1) { // if there's no valid room or valid path, set the speed to 0 so that we don't check this every frame
			if (!ai_follow_player()) { // if not following the player
				//person.speed = 0.0; // movement will be stopped from now on - not valid if we get into this state before gameplay is enabled
				person.wait_for(2.0); // stop for 2 seconds, then try again
			}
			return AI_STOP;
		}
		if (!find_route_to_point(person, coll_dist, person.is_first_path, 0, person.path)) { // following_player=0
			float const wait_time(is_pos_in_pg_or_backrooms(person.pos) ? 0.05 : 1.0);
			person.wait_for(wait_time); // stop for 1 second (0.1s for parking garage or backrooms), then try again
			return AI_WAITING;
		}
		if (has_rgeom) {person.is_first_path = 0;} // treat the path as the first path until room geom is generated
		person.next_path_pt(1);
		return AI_BEGIN_PATH;
	}
	float const max_dist(get_person_max_move_dist(person, speed_mult));
	person.on_new_path_seg = 0; // clear flag for this frame
	//person.following_player = can_target_player(person); // for debugging visualization

	// check if at dest; note that we use a different distance check for Z to account for model height changes when switching between people and zombie models
	if (dist_xy_less_than(person.pos, person.target_pos, 1.1f*max_dist) && fabs(person.pos.z - person.target_pos.z) < 0.5*person.get_height()) {
		assert(point_in_building_or_basement_bcube(person.target_pos));
		person.pos = person.target_pos;
		if (!person.path.empty()) {person.next_path_pt(0); return AI_NEXT_PT;} // move to next path point
		float const floor_spacing(get_window_vspace());

		if (person.goal_type == GOAL_TYPE_ELEVATOR) {
			elevator_t &e(get_elevator(person.cur_elevator));
			// floor index relative to this elevator, not the room or building
			unsigned const cur_floor(get_elevator_floor(person.pos.z, e, floor_spacing)), num_floors(round_fp(e.dz()/floor_spacing));
			assert(num_floors > 1);
			assert(cur_floor  < num_floors);

			// select the destination floor, different from the current floor; this must be done before calling the elevator so that we know if we're going up or down
			while (1) {
				person.dest_elevator_floor = rgen.rand() % num_floors;
				if (person.dest_elevator_floor != cur_floor) break; // floor is valid
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
		// don't wait if we can follow the player
		bool const no_wait(global_building_params.ai_target_player && (can_target_player(person) || has_nearby_sound(person, floor_spacing)));
		if (!no_wait) {person.wait_for(rgen.rand_uniform(1.0, (can_ai_follow_player(person) ? 2.0 : 10.0)));} // stop for 1-10s, 1-2s if player is in this building in gameplay mode
		person.on_new_path_seg    = 1; // allow player following AI update logic to rerun this frame
		person.last_used_elevator = 0;
		return AI_AT_DEST;
	}
	// this person is walking to a destination point
	vector3d const new_dir(person.target_pos - person.pos);
	float const new_dir_mag(new_dir.mag());
	point new_pos;

	if (dot_product(new_dir, person.dir) < 0.999*new_dir_mag) { // dir not perfectly aligned
		//if (person.is_close_to_player()) {cout << TXT(new_dir.str()) << TXT(person.dir.str()) << TXT(new_dir_mag) << TXT(delta_dir) << TXT(max_dist) << TXT(person.radius) << endl;}
		assert(new_dir_mag > TOLERANCE); // should be guaranteed by dist_less_than() test, assuming zvals are equal (which they should be)
		float const step_scale(max(0.1f, dot_product(person.dir, new_dir)/new_dir_mag)); // move more slowly when direction misaligns to avoid overshooting target_pos
		person.dir = (delta_dir/new_dir_mag)*new_dir + (1.0 - delta_dir)*person.dir; // merge new_dir into dir gradually for smooth turning
		if (person.on_stairs()) {person.dir.z = new_dir.z/new_dir_mag;} // dir.z tracks exactly
		person.dir.normalize();
		new_pos = person.pos + (max_dist*step_scale)*person.dir;
	}
	else { // optimization for aligned dir
		new_pos = person.pos + max_dist*person.dir;
	}
	// make sure the person is inside the building, in case they were pushed by another person
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

				for (auto r = interior->ext_basement_rooms_start(); r != interior->rooms.end(); ++r) {
					if (new_pos.z < r->z1() || new_pos.z > r->z2()) continue; // wrong level
					if (r->contains_pt(new_pos)) {cur_room_bcube = *r;} // this is the room we'll be pushed into if needed
					accumulate_shared_xy_area(*r, sc, cont_area);
				}
				if (cont_area < 0.99*sc.get_area_xy()) {clip_cube = cur_room_bcube;} // not contained - force into containing room
			}
		}
		else {clip_cube = basement;} // basement only
	}
	else {clip_cube = bcube;} // above ground
	clip_cube.expand_by_xy(-coll_dist); // shrink
	clip_cube.clamp_pt_xy(new_pos); // make sure person stays within building bcube; can't clip to room because person may be exiting it
	
	if (!is_cube() && !person.on_stairs() && !check_cube_within_part_sides(person.get_bcube() + (new_pos - person.pos))) { // outside the building
		int const part_ix(get_part_ix_containing_pt(new_pos));

		if (part_ix >= 0) { // center is at least in a valid part
			// we don't know exactly how far outside the building this person is,
			// so move them 10% of their radius toward the center of their current part and hope they make it back into the building after a few frames;
			// seems to work better than calling do_sphere_coll_polygon_sides() here
			point const part_center(parts[part_ix].get_cube_center());
			vector3d const move_dir((part_center.x - new_pos.x), (part_center.y - new_pos.y), 0.0);
			new_pos += move_dir*(0.1*person.radius/move_dir.mag());
			person.abort_dest();
		}
	}
	if (!point_in_building_or_basement_bcube(new_pos)) { // person must be inside the building
		cout << TXT(new_pos.str()) << TXT(bcube.str()) << TXT(interior->basement_ext_bcube.str()) << endl;
		assert(0);
	}
	// don't do collision detection while on stairs because it doesn't work properly; just let people walk through each other
	if (!person.on_stairs()) {
		if (person.goal_type == GOAL_TYPE_ELEVATOR) { // heading for the elevator
			// check for collisions with someone else waiting for the elevator
			for (auto p = interior->people.begin(); p < interior->people.end(); ++p) {
				if (!person.waiting_for_same_elevator_as(*p, get_window_vspace())) continue;
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

	if (interior->door_state_updated || (global_building_params.ai_opens_doors == 2 && might_have_closed_door)) {
		cube_t sc; sc.set_from_sphere(new_pos, person.radius); // sphere bounding cube

		for (auto i = interior->door_stacks.begin(); i != interior->door_stacks.end(); ++i) { // can be slow, but not as slow as iterating over doors
			if (new_pos.z < i->z1() || new_pos.z > i->z2())         continue; // wrong part/floor
			if (!i->intersects(sc)) continue; // no intersection with door
			if (!i->get_true_bcube().line_intersects(person.pos, person.target_pos)) continue; // check if path goes through door, to allow for "glancing blows" when pushed or turning
			assert(i->first_door_ix < interior->doors.size());

			for (unsigned dix = i->first_door_ix; dix < interior->doors.size(); ++dix) {
				door_t const &door(interior->doors[dix]);
				if (!i->is_same_stack(door)) break; // moved to a different stack, done
				if (door.z1() > person.pos.z || door.z2() < person.pos.z) continue; // wrong floor
				if (door.open) continue; // doors tend to block the AI, don't collide with them unless they're closed

				if (global_building_params.ai_opens_doors && !door.is_locked_or_blocked(person.has_key)) { // can open the door
					toggle_door_state(dix, player_in_this_building, 0, person.pos); // by_player=0
				}
				else { // can't open the door
					person.wait_for(5.0); // wait for 5s and then choose a new desination
					return AI_WAITING; // cut the path short at this closed door
				}
			} // for dix
		} // for i
	}
	handle_vert_cylin_tape_collision(new_pos, person.pos, person.get_z1(), person.get_z2(), person.radius, 0); // should be okay to use zvals from old pos; is_player=0
	// logic to clip this person to correct room Z-bounds in case something went wrong; remove if/when this is fixed
	// Note: we probably can't use the room Z bounds here becase the person may be on the stairs connecting two stacked parts
	float const true_z1(point_in_extended_basement_not_basement(new_pos) ? get_bcube_z1_inc_ext_basement() : bcube.z1());
	float const min_valid_zval(true_z1 + get_fc_thickness() + person.radius), max_valid_zval(bcube.z2() - person.radius); // Note: max should include the attic

	if (/*player_in_this_building &&*/ !person.on_stairs() && person.cur_room >= 0) { // movement in XY, not on stairs, room is valid: snap to nearest floor
		// this is optional and is done just in case something went wrong
		room_t room(get_room(person.cur_room));

		if (!room.contains_pt(new_pos)) { // maybe we just exited the stairs into a different part and cur_room hasn't been updated yet
			int const new_room_ix(get_room_containing_pt(new_pos));
			if (new_room_ix >= 0) {room = get_room(new_room_ix);}
		}
		float const floor_spacing(get_window_vspace());
		int cur_floor(max(0, round_fp((new_pos.z - min_valid_zval)/floor_spacing)));
		int const max_floor(round_fp((room.z2() - true_z1)/floor_spacing) - 1);
		min_eq(cur_floor, max_floor); // clip to the valid floors for this room relative to lowest building floor
		float const adj_zval(cur_floor*floor_spacing + min_valid_zval);
		if (fabs(adj_zval - new_pos.z) > 0.1*person.radius) {person.abort_dest();} // if we snap to the floor, reset the target and path
		new_pos.z = adj_zval;
	}
	max_eq(new_pos.z, min_valid_zval); // don't let the person go below the ground floor
	min_eq(new_pos.z, max_valid_zval); // don't let the person go above the room ceiling
	// update state
	person.pos        = new_pos; // Note: new_pos.z should equal person.poz.z unless on stairs, which is difficult to accurately check for in this function
	person.anim_time += max_dist;
	bool enable_bl_update(display_mode & 0x20); // disabled by default, enable with key '6'
	if (ai_follow_player() && global_building_params.ai_player_vis_test >= 3) {enable_bl_update = 0;} // if AI tests that player is lit, don't turn on/off lights
	if (enable_bl_update) {ai_room_lights_update(person);} // non-const part
	// update cur_room after moving and lights update; needed if player is in the room and for ai_room_lights_update() same room optimization
	if (player_in_this_building || enable_bl_update) {person.cur_room = get_room_containing_pt(person.pos);}
	person.idle_time = 0.0; // reset idle time if we actually move
	return AI_MOVING;
}

void building_t::ai_room_lights_update(person_t const &person) {
	int const room_ix(get_room_containing_pt(person.pos));
	if (room_ix == person.cur_room) return; // same room as last time - done
	if (room_ix >= 0) {set_room_light_state_to(get_room(room_ix), person.pos.z, 1);} // make sure current room light is on when entering
	if (person.cur_room < 0) return;
	float const floor_spacing(get_window_vspace());
	bool other_person_in_room(0);

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

	if (!point_in_building_or_basement_bcube(person.pos)) { // this can happen on rare occasions, due to fp inaccuracy or multiple collisions
		//cout << TXT(rsum) << TXT(sep_dist) << TXT(move_dist) << TXT(room_ix) << TXT(other.pos.str()) << TXT(person.pos.str()) << TXT(bcube.str()) << endl;
		bcube.clamp_pt_xy(person.pos); // just clamp pos so that it doesn't assert later
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
		if (r->contains_pt_exp_xy_only(pt, wall_thickness)) {return (r - interior->rooms.begin());} // expand to include point in doorway
	}
	return -1; // room not found
}
building_loc_t building_t::get_building_loc_for_pt(point const &pt) const {
	building_loc_t loc;

	for (auto p = parts.begin(); p != get_real_parts_end_inc_sec(); ++p) {
		if (p->contains_pt(pt)) {loc.part_ix = (p - parts.begin()); break;}
	}
	if (loc.part_ix < 0 && point_in_extended_basement(pt)) {loc.part_ix = basement_part_ix;} // use basement part if in extended basement, even though point is outside the cube

	if (interior) { // rooms and stairwells, no elevators yet
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
int building_t::room_or_adj_room_has_stairs(int room_ix, float zval, bool inc_adj_rooms) const {
	if (room_ix < 0) return 0; // no room contains this point
	room_t const &room(get_room(room_ix));
	unsigned floor_ix(room.get_floor_containing_zval(max(zval, room.z1()), get_window_vspace())); // clamp zval to the room
	if (room.has_stairs_on_floor(floor_ix)) return 1;
	if (!inc_adj_rooms) return 0;
	cube_t cr(room);
	cr.expand_by_xy(2.0*get_wall_thickness());

	for (auto r = interior->rooms.begin(); r != interior->rooms.end(); ++r) {
		if (r->has_stairs_on_floor(floor_ix) && r->intersects_no_adj(cr)) return 2;
	}
	return 0;
}

bool building_t::maybe_zombie_retreat(unsigned person_ix, point const &hit_pos) {
	if (!ai_follow_player()) return 0; // not in gameplay mode, ignore it
	assert(interior && person_ix < interior->people.size());
	person_t &person(interior->people[person_ix]);
	if (person.is_on_stairs) return 0; // ignore when on stairs as this doesn't work correctly
	if (hit_pos.z < (person.get_z1() + 0.25*person.get_height())) return 0; // less than 25% up, coll with legs, assume this is kicking a ball that's on the floor (if a ball)
	// play sound on first retreat: alert_other_zombies=1, high_priority=1, gain=1.0, pitch=1.25
	if (person.retreat_time == 0.0) {maybe_play_zombie_sound(person.pos, person_ix, 1, 1, 1.0, 1.25);}
	// Note: this isn't really thread safe, but it should be okay to modify this state while the AI thread is running
	person.retreat_time = global_building_params.ai_retreat_time*TICKS_PER_SECOND; // retreat
	return 1;
}
void building_t::register_person_hit(unsigned person_ix, room_object_t const &obj, vector3d const &velocity) {
	if (velocity == zero_vector)  return; // stationary object, ignore it
	if (obj.type != TYPE_LG_BALL) return; // currently this is the only throwable/dynamic object
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
