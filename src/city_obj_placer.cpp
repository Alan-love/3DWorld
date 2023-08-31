// 3D World - City Object Placement
// by Frank Gennari
// 05/21/23

#include "city_objects.h"
#include "tree_3dw.h" // for tree_placer_t

extern unsigned max_unique_trees;
extern tree_placer_t tree_placer;
extern city_params_t city_params;
extern object_model_loader_t building_obj_model_loader;
extern plot_divider_type_t plot_divider_types[];
extern textured_mat_t pool_deck_mats[];

void add_signs_for_city(unsigned city_id, vector<sign_t> &signs);
void add_flags_for_city(unsigned city_id, vector<city_flag_t> &flags);
city_flag_t create_flag(bool dim, bool dir, point const &base_pt, float height, float length);
void add_house_driveways_for_plot(cube_t const &plot, vect_cube_t &driveways);


bool city_obj_placer_t::gen_parking_lots_for_plot(cube_t plot, vector<car_t> &cars, unsigned city_id, unsigned plot_ix,
	vect_cube_t &bcubes, vect_cube_t &colliders, rand_gen_t &rgen)
{
	vector3d const nom_car_size(city_params.get_nom_car_size()); // {length, width, height}
	float const space_width(PARK_SPACE_WIDTH *nom_car_size.y); // add 50% extra space between cars
	float const space_len  (PARK_SPACE_LENGTH*nom_car_size.x); // space for car + gap for cars to drive through
	float const pad_dist   (max(1.0f*nom_car_size.x, get_min_obj_spacing())); // one car length or min building spacing
	plot.expand_by_xy(-pad_dist);
	if (bcubes.empty()) return 0; // shouldn't happen, unless buildings are disabled; skip to avoid perf problems with an entire plot of parking lot
	unsigned const buildings_end(bcubes.size()), first_corner(rgen.rand()&3); // 0-3
	bool const car_dim(rgen.rand() & 1); // 0=cars face in X; 1=cars face in Y
	bool const car_dir(rgen.rand() & 1);
	float const xsz(car_dim ? space_width : space_len), ysz(car_dim ? space_len : space_width);
	bool has_parking(0);
	vector<hcap_with_dist_t> hcap_cands;
	car_t car;
	car.park();
	car.cur_city = city_id;
	car.cur_road = plot_ix; // store plot_ix in road field
	car.cur_road_type = TYPE_PLOT;

	for (unsigned c = 0; c < 4; ++c) { // generate 0-4 parking lots per plot, starting at the corners, in random order
		unsigned const cix((first_corner + c) & 3), xdir(cix & 1), ydir(cix >> 1), wdir(car_dim ? xdir : ydir), rdir(car_dim ? ydir : xdir);
		float const dx(xdir ? -xsz : xsz), dy(ydir ? -ysz : ysz), dw(car_dim ? dx : dy), dr(car_dim ? dy : dx); // delta-wdith and delta-row
		point const corner_pos(plot.d[0][xdir], plot.d[1][ydir], (plot.z1() + 0.1*ROAD_HEIGHT)); // shift up slightly to avoid z-fighting
		assert(dw != 0.0 && dr != 0.0);
		parking_lot_t cand(cube_t(corner_pos, corner_pos), car_dim, car_dir, city_params.min_park_spaces, city_params.min_park_rows); // start as min size at the corner
		cand.d[!car_dim][!wdir] += cand.row_sz*dw;
		cand.d[ car_dim][!rdir] += cand.num_rows*dr;
		if (!plot.contains_cube_xy(cand)) {continue;} // can't fit a min size parking lot in this plot, so skip it (shouldn't happen)
		if (has_bcube_int_xy(cand, bcubes, pad_dist)) continue; // intersects a building - skip (can't fit min size parking lot)
		cand.z2() += plot.dz(); // probably unnecessary
		parking_lot_t park(cand);

		// try to add more parking spaces in a row
		for (; plot.contains_cube_xy(cand); ++cand.row_sz, cand.d[!car_dim][!wdir] += dw) {
			if (has_bcube_int_xy(cand, bcubes, pad_dist)) break; // intersects a building - done
			park = cand; // success: increase parking lot to this size
		}
		cand = park;
		// try to add more rows of parking spaces
		for (; plot.contains_cube_xy(cand); ++cand.num_rows, cand.d[car_dim][!rdir] += dr) {
			if (has_bcube_int_xy(cand, bcubes, pad_dist)) break; // intersects a building - done
			park = cand; // success: increase parking lot to this size
		}
		assert(park.row_sz >= city_params.min_park_spaces && park.num_rows >= city_params.min_park_rows);
		assert(park.dx() > 0.0 && park.dy() > 0.0);
		car.cur_seg = (unsigned short)parking_lots.size(); // store parking lot index in cur_seg
		parking_lots.push_back(park);
		bcubes.push_back(park); // add to list of blocker bcubes so that no later parking lots overlap this one
		//parking_lots.back().expand_by_xy(0.5*pad_dist); // re-add half the padding for drawing (breaks texture coord alignment)
		unsigned const nspaces(park.row_sz*park.num_rows);
		num_spaces += nspaces;

		// fill the parking lot with cars and assign handicap spaces
		vector<unsigned char> &used_spaces(parking_lots.back().used_spaces);
		used_spaces.resize(nspaces, 0); // start empty
		car.dim = car_dim; car.dir = car_dir;
		point pos(corner_pos.x, corner_pos.y, plot.z2());
		pos[car_dim] += 0.5*dr + (car_dir ? 0.15 : -0.15)*fabs(dr); // offset for centerline, biased toward the front of the parking space
		float const car_density(rgen.rand_uniform(city_params.min_park_density, city_params.max_park_density));

		for (unsigned row = 0; row < park.num_rows; ++row) {
			pos[!car_dim] = corner_pos[!car_dim] + 0.5*dw; // half offset for centerline
			bool prev_was_bad(0);

			for (unsigned col = 0; col < park.row_sz; ++col) { // iterate one past the end
				if (prev_was_bad) {prev_was_bad = 0;} // previous car did a bad parking job, leave this space empty
				else if (rgen.rand_float() < car_density) { // only half the spaces are filled on average
					point cpos(pos);
					cpos[ car_dim] += 0.05*dr*rgen.rand_uniform(-1.0, 1.0); // randomness of front amount
					cpos[!car_dim] += 0.12*dw*rgen.rand_uniform(-1.0, 1.0); // randomness of side  amount

					if (col+1 != park.row_sz && (rgen.rand()&15) == 0) {// occasional bad parking job
						cpos[!car_dim] += dw*rgen.rand_uniform(0.3, 0.35);
						prev_was_bad = 1;
					}
					car.set_bcube(pos, nom_car_size);
					cars.push_back(car);
					if ((rgen.rand()&7) == 0) {cars.back().dir ^= 1;} // pack backwards 1/8 of the time
					used_spaces[row*park.num_rows + col] = 1;
					++filled_spaces;
					has_parking = 1;
				}
				hcap_cands.emplace_back(hcap_space_t(point(pos.x, pos.y, park.z2()), 0.25*space_width, car_dim, car_dir), plot, bcubes, buildings_end);
				pos[!car_dim] += dw;
			} // for col
			pos[car_dim] += dr;
		} // for row
		// generate colliders for each group of used parking space columns
		cube_t cur_cube(park); // set zvals, etc.
		bool inside(0);

		for (unsigned col = 0; col <= park.row_sz; ++col) {
			// mark this space as blocked if any spaces in the row are blocked; this avoids creating diagonally adjacent colliders that cause dead ends and confuse path finding
			bool blocked(0);
			for (unsigned row = 0; col < park.row_sz && row < park.num_rows; ++row) {blocked |= (used_spaces[row*park.num_rows + col] != 0);}

			if (!inside && blocked) { // start a new segment
				cur_cube.d[!car_dim][0] = corner_pos[!car_dim] + col*dw;
				inside = 1;
			}
			else if (inside && !blocked) { // end the current segment
				cur_cube.d[!car_dim][1] = corner_pos[!car_dim] + col*dw;
				cur_cube.normalize();
				//assert(park.contains_cube(cur_cube)); // can fail due to floating-point precision
				colliders.push_back(cur_cube);
				inside = 0;
			}
		} // for col
	} // for c
	// assign handicap spots
	unsigned const num_hcap_spots((hcap_cands.size() + 10)/20); // 5% of total spots, rounded to the center

	if (num_hcap_spots > 0) {
		sort(hcap_cands.begin(), hcap_cands.end());
		for (unsigned n = 0; n < num_hcap_spots; ++n) {hcap_groups.add_obj(hcap_space_t(hcap_cands[n]), hcaps);}
	}
	return has_parking;
}

// non-const because this sets driveway_t::car_ix through add_car()
void city_obj_placer_t::add_cars_to_driveways(vector<car_t> &cars, vector<road_plot_t> const &plots, vector<vect_cube_t> &plot_colliders, unsigned city_id, rand_gen_t &rgen) {
	car_t car;
	car.park();
	car.cur_city = city_id;
	car.cur_road_type = TYPE_DRIVEWAY;
	vector3d const nom_car_size(city_params.get_nom_car_size()); // {length, width, height}

	for (auto i = driveways.begin(); i != driveways.end(); ++i) {
		if (rgen.rand_float() < 0.5) continue; // no car in this driveway 50% of the time
		car.cur_road = (unsigned short)i->plot_ix; // store plot_ix in road field
		car.cur_seg  = (unsigned short)(i - driveways.begin()); // store driveway index in cur_seg
		cube_t const &plot(plots[i->plot_ix]);
		car.dim = (i->y1() == plot.y1() || i->y2() == plot.y2()); // check which edge of the plot the driveway is connected to, which is more accurate than the aspect ratio
		if (i->get_sz_dim(car.dim) < 1.6*nom_car_size.x || i->get_sz_dim(!car.dim) < 1.25*nom_car_size.y) continue; // driveway is too small to fit this car
		car.dir = rgen.rand_bool(); // randomly pulled in vs. backed in, since we don't know the direction to the house anyway
		float const pad_l(0.75*nom_car_size.x), pad_w(0.6*nom_car_size.y); // needs to be a bit larger to fit trucks
		point cpos(0.0, 0.0, i->z2());
		cpos[ car.dim] = rgen.rand_uniform(i->d[ car.dim][0]+pad_l, i->d[ car.dim][1]-pad_l);
		cpos[!car.dim] = rgen.rand_uniform(i->d[!car.dim][0]+pad_w, i->d[!car.dim][1]-pad_w); // not quite centered
		car.set_bcube(cpos, nom_car_size);
		// check if this car intersects another parked car; this can only happen if two driveways intersect, which should be rare
		bool intersects(0);

		for (auto c = cars.rbegin(); c != cars.rend(); ++c) {
			if (c->cur_road != i->plot_ix) break; // prev plot, done
			if (car.bcube.intersects(c->bcube)) {intersects = 1; break;}
		}
		if (intersects) continue; // skip
		i->in_use = 2; // permanently in use
		cars.push_back(car);
		plot_colliders[i->plot_ix].push_back(car.bcube); // prevent pedestrians from walking through this parked car
	} // for i
}

bool check_pt_and_place_blocker(point const &pos, vect_cube_t &blockers, float radius, float blocker_spacing) {
	cube_t bc(pos);
	if (has_bcube_int_xy(bc, blockers, radius)) return 0; // intersects a building or parking lot - skip
	bc.expand_by_xy(blocker_spacing);
	blockers.push_back(bc); // prevent trees and benches from being too close to each other
	return 1;
}
bool try_place_obj(cube_t const &plot, vect_cube_t &blockers, rand_gen_t &rgen, float radius, float blocker_spacing, unsigned num_tries, point &pos) {
	for (unsigned t = 0; t < num_tries; ++t) {
		pos = rand_xy_pt_in_cube(plot, radius, rgen);
		if (check_pt_and_place_blocker(pos, blockers, radius, blocker_spacing)) return 1; // success
	}
	return 0;
}
void place_tree(point const &pos, float radius, int ttype, vect_cube_t &colliders, vector<point> &tree_pos, bool allow_bush, bool is_sm_tree) {
	tree_placer.add(pos, 0, ttype, allow_bush, is_sm_tree); // use same tree type
	cube_t bcube; bcube.set_from_sphere(pos, 0.15*radius); // use 15% of the placement radius for collision (trunk + planter)
	bcube.z2() += radius; // increase cube height
	colliders.push_back(bcube);
	tree_pos.push_back(pos);
}

void city_obj_placer_t::place_trees_in_plot(road_plot_t const &plot, vect_cube_t &blockers,
	vect_cube_t &colliders, vector<point> &tree_pos, rand_gen_t &rgen, unsigned buildings_end)
{
	if (city_params.max_trees_per_plot == 0) return;
	float const radius(city_params.tree_spacing*city_params.get_nom_car_size().x); // in multiples of car length
	float const spacing(max(radius, get_min_obj_spacing())), radius_exp(2.0*spacing);
	vector3d const plot_sz(plot.get_size());
	if (min(plot_sz.x, plot_sz.y) < 2.0*radius_exp) return; // plot is too small for trees of this size
	unsigned num_trees(city_params.max_trees_per_plot);
	if (plot.is_park) {num_trees += (rgen.rand() % city_params.max_trees_per_plot);} // allow up to twice as many trees in parks
	assert(buildings_end <= blockers.size());
	// shrink non-building blockers (parking lots, driveways, fences, walls, hedges) to allow trees to hang over them; okay if they become denormalized
	unsigned const input_blockers_end(blockers.size());
	float const non_buildings_overlap(0.7*radius);
	for (auto i = blockers.begin()+buildings_end; i != blockers.end(); ++i) {i->expand_by_xy(-non_buildings_overlap);}

	for (unsigned n = 0; n < num_trees; ++n) {
		bool const is_sm_tree((rgen.rand()%3) == 0); // 33% of the time is a pine/palm tree
		int ttype(-1); // Note: okay to leave at -1; also, don't have to set to a valid tree type
		if (is_sm_tree) {ttype = (plot.is_park ? (rgen.rand()&1) : 2);} // pine/short pine in parks, palm in city blocks
		else {ttype = rgen.rand()%100;} // random type
		bool const is_palm(is_sm_tree && ttype == 2);
		bool const allow_bush(plot.is_park && max_unique_trees == 0); // can't place bushes if tree instances are enabled (generally true) because bushes may be instanced in non-parks
		float const bldg_extra_radius(is_palm ? 0.5f*radius : 0.0f); // palm trees are larger and must be kept away from buildings, but can overlap with other trees
		point pos;
		if (!try_place_obj(plot, blockers, rgen, (spacing + bldg_extra_radius), (radius - bldg_extra_radius), 10, pos)) continue; // 10 tries per tree, extra spacing for palm trees
		place_tree(pos, radius, ttype, colliders, tree_pos, allow_bush, is_sm_tree); // size is randomly selected by the tree generator using default values; allow bushes in parks
		if (plot.is_park) continue; // skip row logic and just place trees randomly throughout the park
		// now that we're here, try to place more trees at this same distance from the road in a row
		bool const dim(min((pos.x - plot.x1()), (plot.x2() - pos.x)) < min((pos.y - plot.y1()), (plot.y2() - pos.y)));
		bool const dir((pos[dim] - plot.d[dim][0]) < (plot.d[dim][1] - pos[dim]));
		float const step(1.25*radius_exp*(dir ? 1.0 : -1.0)); // positive or negative (must be > 2x radius spacing)
					
		for (; n < city_params.max_trees_per_plot; ++n) {
			pos[dim] += step;
			if (pos[dim] < plot.d[dim][0]+radius || pos[dim] > plot.d[dim][1]-radius) break; // outside place area
			if (!check_pt_and_place_blocker(pos, blockers, (spacing + bldg_extra_radius), (spacing - bldg_extra_radius))) break; // placement failed
			place_tree(pos, radius, ttype, colliders, tree_pos, plot.is_park, is_sm_tree); // use same tree type
		} // for n
	} // for n
	for (auto i = blockers.begin()+buildings_end; i != blockers.begin()+input_blockers_end; ++i) {i->expand_by_xy(non_buildings_overlap);} // undo initial expand
}

template<typename T> void city_obj_groups_t::add_obj(T const &obj, vector<T> &objs) {
	by_tile[get_tile_id_for_cube(obj.bcube)].push_back(objs.size());
	objs.push_back(obj);
}
template<typename T> void city_obj_groups_t::create_groups(vector<T> &objs, cube_t &all_objs_bcube) {
	vector<T> new_objs;
	new_objs.reserve(objs.size());
	reserve(by_tile.size()); // the number of actual groups

	for (auto g = by_tile.begin(); g != by_tile.end(); ++g) {
		unsigned const group_start(new_objs.size());
		cube_with_ix_t group;

		for (auto i = g->second.begin(); i != g->second.end(); ++i) {
			assert(*i < objs.size());
			group.assign_or_union_with_cube(objs[*i].get_outer_bcube());
			new_objs.push_back(objs[*i]);
		}
		sort(new_objs.begin()+group_start, new_objs.end());
		group.ix = new_objs.size();
		push_back(group);
		all_objs_bcube.assign_or_union_with_cube(group);
	} // for g
	objs.swap(new_objs);
	by_tile.clear(); // no longer needed
}

void add_cube_to_colliders_and_blockers(cube_t const &cube, vect_cube_t &blockers, vect_cube_t &colliders) {
	colliders.push_back(cube);
	blockers .push_back(cube);
}

struct pigeon_place_t {
	point pos;
	vector3d orient;
	bool on_ground;

	pigeon_place_t(point const &pos_, rand_gen_t &rgen  ) : pos(pos_), on_ground(1) {set_rand_orient(rgen);} // ground constructor
	pigeon_place_t(point const &pos_, bool dim, bool dir) : pos(pos_), on_ground(0) {orient[dim] = (dir ? 1.0 : -1.0);} // object constructor
	void set_rand_orient(rand_gen_t &rgen) {orient = rgen.signed_rand_vector_spherical();}
};
void place_pigeon_on_obj(cube_t const &obj, bool dim, bool dir, bool orient_dir, float spacing, rand_gen_t &rgen, vector<pigeon_place_t> &locs) {
	point pos(0.0, 0.0, obj.z2());
	pos[ dim] = obj.d[dim][dir];
	pos[!dim] = rgen.rand_uniform(obj.d[!dim][0]+spacing, obj.d[!dim][1]-spacing); // random position along the edge
	locs.emplace_back(pos, dim, orient_dir);
}

// Note: blockers are used for placement of objects within this plot; colliders are used for pedestrian AI
void city_obj_placer_t::place_detail_objects(road_plot_t const &plot, vect_cube_t &blockers, vect_cube_t &colliders,
	vector<point> const &tree_pos, rand_gen_t &rgen, bool is_residential, bool have_streetlights)
{
	float const car_length(city_params.get_nom_car_size().x); // used as a size reference for other objects
	unsigned const benches_start(benches.size()), trashcans_start(trashcans.size()), substations_start(sstations.size());

	// place fire_hydrants if the model has been loaded; don't add fire hydrants in parks
	if (!plot.is_park && building_obj_model_loader.is_model_valid(OBJ_MODEL_FHYDRANT)) {
		// we want the fire hydrant on the edge of the sidewalk next to the road, not next to the plot; this makes it outside the plot itself
		float const radius(0.04*car_length), height(0.18*car_length), dist_from_road(-0.5*radius - get_sidewalk_width());
		point pos(0.0, 0.0, plot.z2()); // XY will be assigned below

		for (unsigned dim = 0; dim < 2; ++dim) {
			pos[!dim] = plot.get_center_dim(!dim);

			for (unsigned dir = 0; dir < 2; ++dir) {
				pos[dim] = plot.d[dim][dir] - (dir ? 1.0 : -1.0)*dist_from_road; // move into the sidewalk along the road
				// Note: will skip placement if too close to a previously placed tree, but that should be okay as it is relatively uncommon
				if (!check_pt_and_place_blocker(pos, blockers, radius, 2.0*radius)) continue; // bad placement, skip
				vector3d orient(zero_vector);
				orient[!dim] = (dir ? 1.0 : -1.0); // oriented perpendicular to the road
				fire_hydrant_t const fire_hydrant(pos, radius, height, orient);
				fhydrant_groups.add_obj(fire_hydrant, fhydrants);
				colliders.push_back(fire_hydrant.bcube);
			} // for dir
		} // for dim
	}
	// place benches in parks and non-residential areas
	if (!is_residential || plot.is_park) {
		bench_t bench;
		bench.radius = 0.3*car_length;
		float const bench_spacing(max(bench.radius, get_min_obj_spacing()));

		for (unsigned n = 0; n < city_params.max_benches_per_plot; ++n) {
			if (!try_place_obj(plot, blockers, rgen, bench_spacing, 0.0, 1, bench.pos)) continue; // 1 try
			float dmin(0.0);

			for (unsigned dim = 0; dim < 2; ++dim) {
				for (unsigned dir = 0; dir < 2; ++dir) {
					float const dist(fabs(bench.pos[dim] - plot.d[dim][dir])); // find closest distance to road (plot edge) and orient bench that way
					if (dmin == 0.0 || dist < dmin) {bench.dim = !dim; bench.dir = !dir; dmin = dist;}
				}
			}
			bench.calc_bcube();
			bench_groups.add_obj(bench, benches);
			colliders.push_back(bench.bcube);
			blockers.back() = bench.bcube; // update blocker since bench is non-square
			blockers.back().expand_in_dim(!bench.dim, 0.25*bench.radius); // add extra padding in front (for seat access) and back (which extends outside bcube)
		} // for n
	}
	// place planters; don't add planters in parks or residential areas
	if (!is_residential && !plot.is_park) {
		float const planter_height(0.05*car_length), planter_radius(0.25*car_length);

		for (auto i = tree_pos.begin(); i != tree_pos.end(); ++i) {
			planter_groups.add_obj(tree_planter_t(*i, planter_radius, planter_height), planters); // no colliders for planters; pedestrians avoid the trees instead
		}
	}
	// place power poles if there are houses or streetlights
	point corner_pole_pos(all_zeros);

	if ((is_residential && have_city_buildings()) || have_streetlights) {
		float const road_width(city_params.road_width), pole_radius(0.015*road_width), height(0.9*road_width);
		float const xspace(plot.dx() + road_width), yspace(plot.dy() + road_width); // == city_params.road_spacing?
		float const xyspace[2] = {0.5f*xspace, 0.5f*yspace};
		// we can move in toward the plot center so that they don't block pedestrians, but then they can block driveways;
		// if we move them into the road, then they block traffic light crosswalks;
		// so we move them toward the road in an assymetic way and allow the pole to be not centered with the wires
		float const offset(0.075*road_width), extra_offset(get_power_pole_offset()); // assymmetric offset to match the aspect ratio of stoplights
		float const pp_x(plot.x2() + offset + extra_offset), pp_y(plot.y2() + offset);
		point pts[3]; // one on the corner and two on each side: {corner, x, y}
		for (unsigned i = 0; i < 3; ++i) {pts[i].assign(pp_x, pp_y, plot.z2());} // start at plot upper corner
		pts[1].x -= 0.5*xspace;
		pts[2].y -= 0.5*yspace;
		unsigned const dims[3] = {3, 1, 2};
		unsigned const pp_start(ppoles.size());

		for (unsigned i = 0; i < 3; ++i) {
			point pos(pts[i]);
			float wires_offset(0.0);

			if (i > 0 && !driveways.empty()) {
				bool const dim(i == 2);
				float const prev_val(pos[dim]);
				move_to_not_intersect_driveway(pos, (pole_radius + get_sidewalk_width()), dim);
				wires_offset = prev_val - pos[dim];
			}
			point base(pos);
			if (i == 1) {base.y += extra_offset;} // shift the pole off the sidewalk and off toward the road to keep it out of the way of pedestrians
			bool const at_line_end[2] = {0, 0};
			bool const at_grid_edge(plot.xpos+1U == num_x_plots || plot.ypos+1U == num_y_plots);
			ppole_groups.add_obj(power_pole_t(base, pos, pole_radius, height, wires_offset, xyspace, dims[i], at_grid_edge, at_line_end, is_residential), ppoles);
			if (i == 0) {corner_pole_pos = base;}
		}
		if (plot.xpos == 0) { // no -x neighbor plot, but need to add the power poles there
			unsigned const pole_ixs[2] = {0, 2};

			for (unsigned i = 0; i < 2; ++i) {
				point pt(pts[pole_ixs[i]]);
				pt.x -= xspace;
				bool const at_line_end[2] = {1, 0};
				ppole_groups.add_obj(power_pole_t(pt, pt, pole_radius, height, 0.0, xyspace, dims[pole_ixs[i]], 1, at_line_end, is_residential), ppoles);
			}
		}
		if (plot.ypos == 0) { // no -y neighbor plot, but need to add the power poles there
			unsigned const pole_ixs[2] = {0, 1};

			for (unsigned i = 0; i < 2; ++i) {
				point pt(pts[pole_ixs[i]]);
				pt.y -= yspace;
				point base(pt);
				if (i == 1) {base.y += extra_offset;}
				bool const at_line_end[2] = {0, 1};
				ppole_groups.add_obj(power_pole_t(base, pt, pole_radius, height, 0.0, xyspace, dims[pole_ixs[i]], 1, at_line_end, is_residential), ppoles);
			}
		}
		if (plot.xpos == 0 && plot.ypos == 0) { // pole at the corner of the grid
			point pt(pts[0]);
			pt.x -= xspace;
			pt.y -= yspace;
			bool const at_line_end[2] = {1, 1};
			ppole_groups.add_obj(power_pole_t(pt, pt, pole_radius, height, 0.0, xyspace, dims[0], 1, at_line_end, is_residential), ppoles);
		}
		for (auto i = (ppoles.begin() + pp_start); i != ppoles.end(); ++i) {colliders.push_back(i->get_ped_occluder());}
	}
	// place substations in commercial cities, near the corner pole that routes power into the ground, if the model has been loaded
	if (!is_residential && corner_pole_pos != all_zeros && building_obj_model_loader.is_model_valid(OBJ_MODEL_SUBSTATION)) {
		bool const dim(rgen.rand_bool()), dir(rgen.rand_bool());
		float const ss_height(0.08*city_params.road_width), dist_from_corner(0.12); // distance from corner relative to plot size
		vector3d const ss_center((1.0 - dist_from_corner)*corner_pole_pos + dist_from_corner*plot.get_cube_center());
		vector3d const model_sz(building_obj_model_loader.get_model_world_space_size(OBJ_MODEL_SUBSTATION));
		vector3d bcube_exp;
		bcube_exp[ dim] = 0.5*ss_height*model_sz.x/model_sz.z;
		bcube_exp[!dim] = 0.5*ss_height*model_sz.y/model_sz.z;
		cube_t ss_bcube(ss_center, ss_center);
		ss_bcube.expand_by_xy(bcube_exp);
		ss_bcube.z2() += ss_height;
		
		if (!has_bcube_int_xy(ss_bcube, blockers, 0.2*ss_height)) { // skip if intersects a building or parking lot
			sstation_groups.add_obj(substation_t(ss_bcube, dim, dir), sstations);
			add_cube_to_colliders_and_blockers(ss_bcube, colliders, blockers);
		}
	}
	// place trashcans next to sidewalks in commercial cities and parks; after substations so that we don't block them
	if (!is_residential || plot.is_park) {
		float const tc_height(0.18*car_length), tc_radius(0.4*tc_height), dist_from_corner(0.06); // dist from corner relative to plot size

		for (unsigned d = 0; d < 4; ++d) { // try all 4 corners
			vector3d const tc_center((1.0 - dist_from_corner)*point(plot.d[0][d&1], plot.d[1][d>>1], plot.z2()) + dist_from_corner*plot.get_cube_center());
			trashcan_t const trashcan(tc_center, tc_radius, tc_height, 0); // is_cylin=0

			if (!has_bcube_int_xy(trashcan.bcube, blockers, 1.5*tc_radius)) { // skip if intersects a building or parking lot, with some padding
				trashcan_groups.add_obj(trashcan, trashcans);
				add_cube_to_colliders_and_blockers(trashcan.bcube, colliders, blockers);
			}
		} // for d
	}
	// place newsracks along non-residential city streets
	if (!is_residential) {
		unsigned const NUM_NR_COLORS = 8;
		colorRGBA const nr_colors[NUM_NR_COLORS] = {WHITE, DK_BLUE, BLUE, ORANGE, RED, DK_GREEN, YELLOW, GRAY_BLACK};
		float const dist_from_corner(-0.03); // dist from corner relative to plot size; negative is outside the plot in the street area
		point pos(0, 0, plot.z2());

		for (unsigned dim = 0; dim < 2; ++dim) {
			for (unsigned dir = 0; dir < 2; ++dir) {
				if (rgen.rand_float() < 0.65) continue; // add to about a third of edges
				unsigned const num_this_side(1 + (rgen.rand() % 5)); // 1-5
				bool const place_together(num_this_side > 1 && rgen.rand_float() < 0.75); // 75% of the time
				unsigned place_region(0);
				pos[dim] = (1.0 - dist_from_corner)*plot.d[dim][dir] + dist_from_corner*plot.get_center_dim(dim); // set distance from the plot edge

				for (unsigned n = 0; n < num_this_side; ++n) {
					float const nr_height(0.28*car_length*rgen.rand_uniform(0.9, 1.11));
					float const nr_width(0.44*nr_height*rgen.rand_uniform(0.8, 1.25)), nr_depth(0.44*nr_height*rgen.rand_uniform(0.8, 1.25));
					float road_pos(0.0);
					if (n == 0 || !place_together) {place_region = (rgen.rand()&3);} // select a new placement region
					// streetlights are at 0.25 and 0.75, and telephone poles are at 0.5, so skip those ranges
					switch (place_region) {
					case 0: road_pos = rgen.rand_uniform(0.10, 0.20); break;
					case 1: road_pos = rgen.rand_uniform(0.30, 0.45); break;
					case 2: road_pos = rgen.rand_uniform(0.55, 0.70); break;
					case 3: road_pos = rgen.rand_uniform(0.80, 0.90); break;
					}
					pos[!dim] = plot.d[!dim][0] + road_pos*plot.get_sz_dim(!dim); // random pos along plot
					newsrack_t const newsrack(pos, nr_height, nr_width, nr_depth, dim, !dir, rgen.rand(), nr_colors[rgen.rand() % NUM_NR_COLORS]); // random style
					cube_t test_cube(newsrack.bcube);
					test_cube.d[dim][!dir] += (dir ? -1.0 : 1.0)*nr_depth; // add front clearance; faces inward from the plot

					if (!has_bcube_int_xy(test_cube, blockers, 0.1*nr_width)) { // skip if intersects a building or parking lot, with a bit of padding
						nrack_groups.add_obj(newsrack, newsracks);
						add_cube_to_colliders_and_blockers(test_cube, colliders, blockers);
					}
				} // for n
			} // for dir
		} // for dim
	}
	// place manholes in adjacent roads, only on +x and +y sides; this will leave one edge road in each dim with no manholes
	if (1) {
		float const radius(0.125*car_length), dist_from_plot_edge(0.35*city_params.road_width); // centered in one lane of the road
		point pos(0.0, 0.0, plot.z2()); // XY will be assigned below
		bool const dir(0); // hard-coded for now

		for (unsigned dim = 0; dim < 2; ++dim) {
			pos[!dim] = plot.d[!dim][0] + 0.35*plot.get_sz_dim(!dim); // not centered
			pos[ dim] = plot.d[ dim][dir] + (dir ? 1.0 : -1.0)*dist_from_plot_edge; // move into the center of the road
			manhole_groups.add_obj(manhole_t(pos, radius), manholes); // Note: colliders not needed
		}
	}
	// maybe place a flag in a city park
	if ((!is_residential && rgen.rand_float() < 0.3) || (plot.is_park && rgen.rand_float() < 0.75)) { // 30% of the time for commerical plots, 75% of the time for parks
		float const length(0.25*city_params.road_width*rgen.rand_uniform(0.8, 1.25)), pradius(0.05*length);
		float const height((plot.is_park ? 0.8 : 1.0)*city_params.road_width*rgen.rand_uniform(0.8, 1.25));
		float const spacing(plot.is_park ? 2.0*pradius : 1.25*length); // parks have low items, so we only need to avoid colliding with the pole; otherwise need to check tall buildings
		cube_t place_area(plot);
		place_area.expand_by_xy(-0.5*city_params.road_width); // shrink slightly to keep flags away from power lines
		point base_pt;

		if (try_place_obj(place_area, blockers, rgen, spacing, 0.0, (plot.is_park ? 5 : 20), base_pt)) { // make up to 5/20 tries
			base_pt.z = plot.z2();
			cube_t pole;
			set_cube_zvals(pole, base_pt.z, (base_pt.z + height));
			for (unsigned d = 0; d < 2; ++d) {set_wall_width(pole, base_pt[d], pradius, d);}
			bool dim(0), dir(0); // facing dir
			get_closest_dim_dir_xy(pole, plot, dim, dir); // face the closest plot edge
			flag_groups.add_obj(create_flag(dim, dir, base_pt, height, length), flags);
			colliders.push_back(pole); // only the pole itself is a collider
		}
	}
	// place pigeons
	if (!is_residential && building_obj_model_loader.is_model_valid(OBJ_MODEL_PIGEON)) {
		float const base_height(0.06*car_length), place_radius(4.0*base_height), obj_edge_spacing(0.25*base_height);

		if (min(plot.dx(), plot.dy()) > 8.0*place_radius) { // plot large enough; should always get here
			vector<pigeon_place_t> pigeon_locs;
			// place some random pigeons; use place_radius because pigeon radius hasn't been calculated yet
			unsigned const count_mod(plot.is_park ? 9 : 5), num_pigeons(rgen.rand() % count_mod); // 0-4, 0-8 for parks
			for (unsigned n = 0; n < num_pigeons; ++n) {pigeon_locs.emplace_back(rand_xy_pt_in_cube(plot, place_radius, rgen), rgen);}

			// maybe place on benches, trashcans, and substations
			for (auto i = benches.begin()+benches_start; i != benches.end(); ++i) {
				if (i->bcube.get_sz_dim(!i->dim) <= 2.0*obj_edge_spacing) continue;
				if (rgen.rand_float() > 0.25) continue; // place 25% of the time
				cube_t top_place(i->bcube);
				top_place.expand_in_dim(!i->dim,  0.1*i->bcube.get_sz_dim(!i->dim)); // expand the back outward a bit
				top_place.expand_in_dim( i->dim, -0.1*i->bcube.get_sz_dim( i->dim)); // shrink a bit to account for the arms extending further to the sides than the back
				place_pigeon_on_obj(top_place, !i->dim, i->dir, rgen.rand_bool(), obj_edge_spacing, rgen, pigeon_locs); // random orient_dir
			}
			for (auto i = trashcans.begin()+trashcans_start; i != trashcans.end(); ++i) {
				if (min(i->bcube.dx(), i->bcube.dy()) <= 2.0*obj_edge_spacing) continue;
				if (rgen.rand_float() > 0.25) continue; // place 25% of the time
				cube_t top_place(i->bcube);
				top_place.expand_by_xy(-1.5*obj_edge_spacing); // small shrink
				bool const dim(rgen.rand_bool()), dir(rgen.rand_bool()); // use a random side of the rim
				place_pigeon_on_obj(top_place, dim, dir, dir, obj_edge_spacing, rgen, pigeon_locs); // facing outward
			}
			for (auto i = sstations.begin()+substations_start; i != sstations.end(); ++i) {
				if (rgen.rand_float() > 0.25) continue; // place 25% of the time
				bool const dim(rgen.rand_bool()), dir(rgen.rand_bool()); // random orient
				pigeon_locs.emplace_back(point(i->bcube.xc(), i->bcube.yc(), i->bcube.z2()), dim, dir); // top center
			}
			for (unsigned i = 0; i < pigeon_locs.size(); ++i) {
				pigeon_place_t p(pigeon_locs[i]);
				float const height(base_height*rgen.rand_uniform(0.8, 1.2));
				// the current model's tail extends below its feet; move down slightly so that feet are on the object, though the tail may clip through the object;
				// the feet gap isn't really visible when placed on the ground since there are no shadows, and it looks better than having the tail clip through the ground
				if (!p.on_ground) {p.pos.z -= 0.15*height;}
				pigeon_t const pigeon(p.pos, height, p.orient);
				if (p.on_ground && has_bcube_int_xy(pigeon.bcube, blockers, 2.0*pigeon.radius)) continue; // placed on the ground - check for collisions
				if (p.on_ground) {blockers.push_back(pigeon.bcube);} // not needed? don't need to add to pedestrian colliders
				pigeon_groups.add_obj(pigeon, pigeons);

				if (i == 0 && p.on_ground) { // place a group of pigeons on the ground for the first pigeon
					float const place_range(0.5*car_length);
					unsigned const group_size(rgen.rand() % count_mod); // 0-4 more, 0-8 for parks
					cube_t valid_range(plot);
					valid_range.expand_by_xy(-place_radius);

					for (unsigned N = 0; N < group_size; ++N) {
						pigeon_place_t p2(p);
						p2.pos += rgen.signed_rand_vector_spherical_xy(place_range);
						if (!valid_range.contains_pt_xy(p2.pos) || pigeon.bcube.contains_pt_xy_exp(p2.pos, base_height)) continue;
						p2.set_rand_orient(rgen);
						pigeon_locs.push_back(p2);
					}
				}
			} // for i
		}
	}
}

bool is_placement_blocked(cube_t const &cube, vect_cube_t const &blockers, cube_t const &exclude, unsigned prev_blockers_end, float expand=0.0, bool exp_dim=0) {
	cube_t query_cube(cube);
	query_cube.expand_in_dim(exp_dim, expand);

	for (auto b = blockers.begin(); b != blockers.begin()+prev_blockers_end; ++b) {
		if (*b != exclude && b->intersects_xy_no_adj(query_cube)) return 1;
	}
	return 0;
}

// dim=narrow dimension of fence; dir=house front/back dir in dimension dim; side=house left/right side in dimension !dim
float extend_fence_to_house(cube_t &fence, cube_t const &house, float fence_hwidth, float fence_height, bool dim, bool dir, bool side) {
	float &fence_end(fence.d[!dim][!side]);
	fence_end = house.d[!dim][side]; // adjacent to the house
	set_wall_width(fence, house.d[dim][dir], fence_hwidth, dim);
	// try to expand to the wall edge of two part houses by doing a line intersection query
	point p1, p2;
	p1.z     = p2.z    = fence.z1() + 0.25*fence_height; // slightly up from the bottom edge of the fence
	p1[ dim] = p2[dim] = fence.d[dim][!dir]; // use the side that overlaps the house bcube
	p1[!dim] = fence_end - (side ? -1.0 : 1.0)*fence_hwidth; // pull back slightly so that the start point isn't exactly at the house edge
	p2[!dim] = house.d[!dim][!side]; // end point is the opposite side of the house
	point p_int;
	if (!check_city_building_line_coll_bs(p1, p2, p_int)) return 0.0; // if this fails, house bcube must be wrong; should this be asserted?
	float const dist(fabs(fence_end - p_int[!dim]));
	fence_end = p_int[!dim];
	assert(fence.is_strictly_normalized());
	return dist;
}

void city_obj_placer_t::place_residential_plot_objects(road_plot_t const &plot, vect_cube_t &blockers, vect_cube_t &colliders,
	vector<road_t> const &roads, unsigned driveways_start, unsigned city_ix, rand_gen_t &rgen)
{
	assert(plot_subdiv_sz > 0.0);
	sub_plots.clear();
	if (plot.is_park) return; // no dividers in parks
	subdivide_plot_for_residential(plot, roads, plot_subdiv_sz, 0, city_ix, sub_plots); // parent_plot_ix=0, not needed
	if (sub_plots.size() <= 1) return; // nothing to divide
	if (rgen.rand_bool()) {std::reverse(sub_plots.begin(), sub_plots.end());} // reverse half the time so that we don't prefer a divider in one side or the other
	unsigned const shrink_dim(rgen.rand_bool()); // mostly arbitrary, could maybe even make this a constant 0
	float const sz_scale(0.06*city_params.road_width);
	unsigned const dividers_start(dividers.size()), prev_blockers_end(blockers.size());
	float const min_pool_spacing_to_plot_edge(0.5*city_params.road_width);
	colorRGBA const pool_side_colors[5] = {WHITE, WHITE, GRAY, LT_BROWN, LT_BLUE};
	vect_cube_with_ix_t bcubes; // we need the building index for the get_building_door_pos_closest_to() call

	for (auto i = sub_plots.begin(); i != sub_plots.end(); ++i) {
		// place plot dividers
		unsigned const type(rgen.rand()%DIV_NUM_TYPES); // use a consistent divider type for all sides of this plot
		if (type == DIV_CHAINLINK) continue; // chain link fence is not a primary divider - no divider for this plot; also, can't place a swimming pool here because it's not enclosed
		// should we remove or move house fences for divided sub-plots? I'm not sure how that would actually be possible at this point; or maybe skip dividers if the house has a fence?
		plot_divider_type_t const &pdt(plot_divider_types[type]);
		float const hwidth(0.5*sz_scale*pdt.wscale), z2(i->z1() + sz_scale*pdt.hscale);
		float const shrink_border(1.5*get_inner_sidewalk_width()); // needed for pedestrians to move along the edge of the plot; slightly larger to prevent collisions
		float translate_dist[2] = {0.0, 0.0};
		unsigned const prev_dividers_end(dividers.size());
		cube_t place_area(plot);
		place_area.expand_by_xy(-shrink_border);

		for (unsigned dim = 0; dim < 2; ++dim) {
			for (unsigned dir = 0; dir < 2; ++dir) {
				float const div_pos(i->d[dim][dir]);
				if (div_pos == plot.d[dim][dir]) continue; // sub-plot is against the plot border, don't need to add a divider
				bool const back_of_plot(i->d[!dim][0] != plot.d[!dim][0] && i->d[!dim][1] != plot.d[!dim][1]); // back of the plot, opposite the street
				unsigned const skip_dims(0); // can't make this (back_of_plot ? (1<<(1-dim)) : 0) because the edge may be showing at borders of different divider types
				cube_t c(*i);
				c.intersect_with_cube_xy(place_area);
				c.z2() = z2;
				set_wall_width(c, div_pos, hwidth, dim); // centered on the edge of the plot

				if (dim == shrink_dim) {
					translate_dist[dir] = (dir ? -1.0 : 1.0)*hwidth;
					c.translate_dim(dim, translate_dist[dir]); // move inside the plot so that edges line up
					// clip to the sides to remove overlap; may not line up with a neighboring divider of a different type/width, but hopefully okay
					for (unsigned d = 0; d < 2; ++d) {
						if (c.d[!dim][d] != plot.d[!dim][d]) {c.d[!dim][d] -= (d ? 1.0 : -1.0)*hwidth;}
					}
				}
				else {
					c.expand_in_dim(!dim, -0.001*hwidth); // fix for z-fighting
				}
				if (!back_of_plot) { // check for overlap of other plot dividers to the left and right
					cube_t test_cube(c);
					test_cube.expand_by_xy(4.0*hwidth); // expand so that adjacency counts as intersection
					bool overlaps(0);

					for (auto d = (dividers.begin()+dividers_start); d != (dividers.begin()+prev_dividers_end) && !overlaps; ++d) {
						overlaps |= (d->dim == bool(dim) && test_cube.contains_pt_xy(d->bcube.get_cube_center()));
					}
					if (overlaps) continue; // overlaps a previous divider, skip this one
				}
				divider_groups.add_obj(divider_t(c, type, dim, dir, skip_dims), dividers);
				add_cube_to_colliders_and_blockers(c, colliders, blockers);
			} // for dir
		} // for dim

		// place yard objects
		if (!i->is_residential || i->is_park || i->street_dir == 0) continue; // not a residential plot along a road
		bcubes.clear();
		if (have_buildings()) {get_building_bcubes(*i, bcubes);}
		if (bcubes.empty()) continue; // no house, skip adding swimming pool
		assert(bcubes.size() == 1); // there should be exactly one building/house in this sub-plot
		cube_with_ix_t const &house(bcubes.front());
		bool const dim((i->street_dir-1)>>1), dir((i->street_dir-1)&1); // direction to the road

		// maybe place a trashcan next to the house
		float const tc_height(0.18*city_params.get_nom_car_size().x), tc_radius(0.3*tc_height);

		for (unsigned n = 0; n < 20; ++n) { // make some attempts to generate a valid trashcan location
			bool tc_dim(0), tc_dir(0);
			unsigned const house_side(rgen.rand() % 3); // any side but the front
			if (house_side == 0) {tc_dim = dim; tc_dir = !dir;} // put it in the back yard
			else {tc_dim = !dim; tc_dir = rgen.rand_bool();} // put it on a random side of the house
			if (tc_radius > 0.25*house.get_sz_dim(!tc_dim)) continue; // house wall is too short - shouldn't happen in the normal case
			point pos, door_pos;
			pos.z = i->z2();
			pos[ tc_dim] = house.d[tc_dim][tc_dir] + (tc_dir ? 1.0 : -1.0)*1.75*tc_radius; // place near this wall of the house
			pos[!tc_dim] = rgen.rand_uniform((house.d[!tc_dim][0] + tc_radius), (house.d[!tc_dim][1] - tc_radius));
			trashcan_t const trashcan(pos, tc_radius, tc_height, 1); // is_cylin=1
			if (is_placement_blocked(trashcan.bcube, blockers, house, prev_blockers_end, 0.0, 0))          continue; // no expand
			if (check_sphere_coll_building((pos + vector3d(0.0, 0.0, tc_radius)), tc_radius, 0, house.ix)) continue; // xy_only=0

			if (!get_building_door_pos_closest_to(house.ix, pos, door_pos) || !dist_xy_less_than(pos, door_pos, 2.0*tc_radius)) { // not too close to doors
				trashcan_groups.add_obj(trashcan, trashcans);
				add_cube_to_colliders_and_blockers(trashcan.bcube, colliders, blockers);
				break; // success
			}
		} // for n

		// place swimming pools; must be placed last due to the continues
		if (rgen.rand_float() < 0.1) continue; // add pools 90% of the time (since placing them often fails anyway)
		cube_t back_yard(*i);
		back_yard.d[dim][dir] = house.d[dim][!dir];
		cube_t pool_area(back_yard); // limit the pool to the back yard

		for (unsigned d = 0; d < 2; ++d) {
			if (i->d[!dim][d] == plot.d[!dim][d]) {pool_area.d[!dim][d] = house.d[!dim][d];} // adjacent to road - constrain to house projection so that side fence can be placed
		}
		float const dmin(min(pool_area.dx(), pool_area.dy())); // or should this be based on city_params.road_width?
		if (dmin < 0.75f*city_params.road_width) continue; // back yard is too small to add a pool
		bool const above_ground(rgen.rand_bool());

		for (unsigned d = 0; d < 2; ++d) { // keep pools away from the edges of plots; applies to sub-plots on the corners
			max_eq(pool_area.d[d][0], plot.d[d][0]+min_pool_spacing_to_plot_edge);
			min_eq(pool_area.d[d][1], plot.d[d][1]-min_pool_spacing_to_plot_edge);
		}
		pool_area.expand_by_xy(-0.05*dmin); // small shrink to keep away from walls, fences, and hedges
		vector3d pool_sz;
		pool_sz.z = (above_ground ? rgen.rand_uniform(0.08, 0.12)*city_params.road_width : 0.01f*dmin);

		for (unsigned d = 0; d < 2; ++d) {
			pool_sz[d] = ((above_ground && d == 1) ? pool_sz[0] : rgen.rand_uniform(0.5, 0.7)*dmin); // above_ground_cylin pools have square bcubes
			pool_area.d[d][1] -= pool_sz[d]; // shrink so that pool_area is where (x1, x2) can be placed
		}
		if (!pool_area.is_normalized()) continue; // pool area is too small; this can only happen due to shrink at plot edges
		point pool_llc;
		pool_llc.z = i->z2();

		for (unsigned n = 0; n < 20; ++n) { // make some attempts to generate a valid pool location
			for (unsigned d = 0; d < 2; ++d) {pool_llc[d] = rgen.rand_uniform(pool_area.d[d][0], pool_area.d[d][1]);}
			cube_t pool(pool_llc, (pool_llc + pool_sz));
			cube_t tc(pool);
			tc.expand_by_xy(0.08*dmin);
			if (is_placement_blocked(tc, blockers, cube_t(), prev_blockers_end)) continue; // intersects some other object
			float const grayscale(rgen.rand_uniform(0.7, 1.0));
			float const water_white_comp(rgen.rand_uniform(0.1, 0.3)), extra_green(rgen.rand_uniform(0.2, 0.5)), lightness(rgen.rand_uniform(0.5, 0.8));
			colorRGBA const color(above_ground ? pool_side_colors[rgen.rand()%5]: colorRGBA(grayscale, grayscale, grayscale));
			colorRGBA const wcolor(lightness*water_white_comp, lightness*(water_white_comp + extra_green), lightness);
			
			// add fences along the sides of the house to separate the back yard from the front yard; if fences can't be added, then don't add the pool either
			plot_divider_type_t const &fence_pdt(plot_divider_types[DIV_CHAINLINK]);
			float const fence_hwidth(0.5*sz_scale*fence_pdt.wscale), fence_height(sz_scale*fence_pdt.hscale), fence_z2(i->z1() + fence_height);
			float const expand(-1.5*sz_scale*plot_divider_types[DIV_HEDGE].wscale); // shrink by widest divider to avoid false intersection with orthogonal dividers
			cube_t subplot_shrunk(*i);
			// translate so that fences line up with dividers; inexact if different width dividers are on each side
			for (unsigned d = 0; d < 2; ++d) {subplot_shrunk.d[shrink_dim][d] += translate_dist[d];}
			subplot_shrunk.expand_by_xy(-hwidth); // shrink by half width of surrounding dividers
			divider_t fences[2];
			bool bad_fence_place(0);

			for (unsigned side = 0; side < 2; ++side) { // left/right
				bool fence_dim(dim);
				cube_t fence(subplot_shrunk);
				fence.z2() = fence_z2;

				if (i->d[!dim][side] == plot.d[!dim][side]) { // at the edge of the plot, wrap the fence around in the back yard instead
					fence_dim ^= 1;
					extend_fence_to_house(fence, house, fence_hwidth, fence_height, !dim, side, !dir);

					if (is_placement_blocked(fence, blockers, house, prev_blockers_end, expand, !fence_dim)) {
						// Note: can't safely move the fence to the middle of the house if it intersects the pooly because it may intersect or block a door
						if ((house.d[!dim][!side] > pool.d[!dim][side]) ^ side) {bad_fence_place = 1; break;} // fence at back of house does not contain the pool
						extend_fence_to_house(fence, house, fence_hwidth, fence_height, !dim, !side, !dir); // try the back edge of the house
						if (is_placement_blocked(fence, blockers, house, prev_blockers_end, expand, !fence_dim)) {bad_fence_place = 1; break;} // blocked by a driveway, etc.
					}
				}
				else { // at the front of the house
					float const ext_dist(extend_fence_to_house(fence, house, fence_hwidth, fence_height, dim, dir, side));

					// check if front fence position is bad, or fence extension is too long (may block off the porch)
					if (ext_dist > 0.33*house.get_sz_dim(!dim) || is_placement_blocked(fence, blockers, house, prev_blockers_end, expand, !fence_dim)) {
						extend_fence_to_house(fence, house, fence_hwidth, fence_height, dim, !dir, side); // try the back edge of the house
						if (is_placement_blocked(fence, blockers, house, prev_blockers_end, expand, !fence_dim)) {bad_fence_place = 1; break;} // blocked by a driveway, etc.
					}
				}
				fences[side] = divider_t(fence, DIV_CHAINLINK, fence_dim, dir, 0); // Note: dir is unused in divider_t so doesn't have to be set correctly
			} // for side
			if (bad_fence_place) continue; // failed to fence off the pool, don't place it here
			pool_groups.add_obj(swimming_pool_t(pool, color, wcolor, above_ground, dim, dir), pools);
			unsigned const pre_pool_blockers_end(blockers.size());
			cube_t pool_collider(pool);
			pool_collider.z2() += 0.1*city_params.road_width; // extend upward to make a better collider
			add_cube_to_colliders_and_blockers(pool_collider, colliders, blockers);

			for (unsigned side = 0; side < 2; ++side) {
				divider_t const &fence(fences[side]);
				assert(fence.bcube.is_strictly_normalized());
				divider_groups.add_obj(fence, dividers);
				add_cube_to_colliders_and_blockers(fence.bcube, colliders, blockers);
			}
			if (!above_ground && rgen.rand_float() < 0.8) { // in-ground pool, add pool deck 80% of the time
				bool had_cand(0);

				for (unsigned d = 0; d < 2 && !had_cand; ++d) { // check for projections in both dims
					float const deck_height(0.4*pool.dz()), deck_shrink(0.8*deck_height);
					float const plo(pool.d[d][0]), phi(pool.d[d][1]), hlo(house.d[d][0]), hhi(house.d[d][1]);
					if (max(plo, hlo) > min(phi, hhi)) continue; // no shared projection in this dim
					// randomly choose to extend deck to cover both objects or restrict to the shared length
					float const lo((rgen.rand_bool() ? min(plo, hlo) : max(plo, hlo)) + deck_shrink), hi((rgen.rand_bool() ? max(phi, hhi) : min(phi, hhi)) - deck_shrink);
					if (hi - lo < 0.25*pool.get_sz_dim(d)) continue; // not enough shared area
					bool const dir(house.get_center_dim(!d) < pool.get_center_dim(!d)); // dir from house to pool
					cube_t deck;
					set_cube_zvals(deck, pool.z1(), (pool.z1() + deck_height)); // lower height than the pool
					deck.d[ d][0   ] = lo;
					deck.d[ d][1   ] = hi;
					deck.d[!d][ dir] = pool .d[!d][!dir];
					deck.d[!d][!dir] = house.d[!d][ dir];
					assert(deck.is_strictly_normalized());
					had_cand = 1; // we have a candidate, even if it was blocked, so there are no more cands to check
					// check for partial intersections of objects such as trashcans, etc. and skip the deck in that case; allow contained objects to rest on the deck
					bool valid(1);

					for (auto b = blockers.begin()+prev_blockers_end; b != blockers.begin()+pre_pool_blockers_end; ++b) { // skip pool, pool fence, and house
						if (*b != house && deck.intersects_xy_no_adj(*b) && !deck.contains_cube_xy(*b)) {valid = 0; break;}
					}
					if (!valid) continue;
					pdeck_groups.add_obj(pool_deck_t(deck, rgen.rand(), d, dir), pdecks);// choose a random material
					blockers.push_back(deck); // blocker for other objects, but not a collider for people or the player
				} // for d
			}
			break; // success - done with pool
		} // for n
	} // for i (sub_plots)
	if (building_obj_model_loader.is_model_valid(OBJ_MODEL_MAILBOX)) {
		// place mailboxes on residential streets
		assert(driveways_start <= driveways.size());
		float const mbox_height(1.1*sz_scale);

		for (auto dw = driveways.begin()+driveways_start; dw != driveways.end(); ++dw) {
			if (rgen.rand_bool()) continue; // only 50% of houses have mailboxes along the road
			bool const dim(dw->dim), dir(dw->dir);
			unsigned pref_side(2); // starts invalid
			point pos(dw->get_cube_center()); // start at the driveway center

			for (auto i = sub_plots.begin(); i != sub_plots.end(); ++i) { // find subplot for this driveway
				if (!i->contains_pt_xy(pos)) continue; // wrong subplot
				pref_side = (pos[!dim] < i->get_center_dim(!dim)); // place mailbox on the side of the driveway closer to the center of the plot
				break; // done
			}
			if (pref_side == 2) continue; // no subplot found? error, or just skip the mailbox?
			pos[dim] = dw->d[dim][dir] - (dir ? 1.0 : -1.0)*1.5*mbox_height; // at end of driveway at the road, but far enough back to leave space for peds

			for (unsigned n = 0; n < 2; ++n) {
				unsigned const side(pref_side ^ n);
				pos[!dim] = dw->d[!dim][side] + (side ? 1.0 : -1.0)*0.5*mbox_height; // off to the side of the driveway
				mailbox_t const mbox(pos, mbox_height, dim, dir);
				if (is_placement_blocked(mbox.bcube, blockers, *dw, prev_blockers_end, 0.0, 0)) continue; // try the other side
				mbox_groups.add_obj(mbox, mboxes);
				add_cube_to_colliders_and_blockers(mbox.bcube, colliders, blockers);
				break; // done
			} // for n
		} // for dw
	}
}

void city_obj_placer_t::add_house_driveways(road_plot_t const &plot, vect_cube_t &temp_cubes, rand_gen_t &rgen, unsigned plot_ix) {
	cube_t plot_z(plot);
	plot_z.z1() = plot_z.z2() = plot.z2() + 0.0002*city_params.road_width; // shift slightly up to avoid Z-fighting
	temp_cubes.clear();
	add_house_driveways_for_plot(plot_z, temp_cubes);

	for (auto i = temp_cubes.begin(); i != temp_cubes.end(); ++i) {
		bool dim(0), dir(0);
		get_closest_dim_dir_xy(*i, plot, dim, dir);
		driveways.emplace_back(*i, dim, dir, plot_ix);
	}
}

void city_obj_placer_t::place_signs_in_isec(road_isec_t &isec) {
	if (isec.has_stoplight) return; // can't have both a stoplight and a stopsign
	if (isec.num_conn == 2) return; // skip for 2-way intersections (bends)
	float const height(isec.get_stop_sign_height()), width(0.3*height);

	for (unsigned n = 0; n < 4; ++n) { // place stop signs on each connector
		if (!(isec.conn & (1 << n))) continue; // no road in this dir
		bool const dim((n>>1) != 0), dir((n&1) == 0); // Note: dir is inverted here to represent car dir
		stopsign_t const ssign(isec.get_stop_sign_pos(n), height, width, dim, !dir, isec.num_conn);
		stopsign_groups.add_obj(ssign, stopsigns);
		isec.has_stopsign = 1;
	} // for n
}

void city_obj_placer_t::add_stop_sign_plot_colliders(vector<road_plot_t> const &plots, vector<vect_cube_t> &plot_colliders) const {
	assert(plots.size() == plot_colliders.size());
	float const bcube_expand(2.0*get_sidewalk_width()); // include sidewalk stop signs in their associated plots

	for (stopsign_t const &s : stopsigns) {
		cube_t bcube_ext(s.bcube);
		bcube_ext.expand_by_xy(bcube_expand);

		for (unsigned i = 0; i < plots.size(); ++i) { // linear iteration; seems to be only 0.05ms per call
			if (plots[i].intersects_xy(bcube_ext)) {plot_colliders[i].push_back(s.bcube); break;}
		}
	}
}

template<typename T> void city_obj_placer_t::draw_objects(vector<T> const &objs, city_obj_groups_t const &groups, draw_state_t &dstate,
	float dist_scale, bool shadow_only, bool has_immediate_draw, bool draw_qbd_as_quads, float specular, float shininess)
{
	if (objs.empty()) return;
	T::pre_draw(dstate, shadow_only);
	unsigned start_ix(0);
	assert(city_draw_qbds_t::empty());

	for (auto g = groups.begin(); g != groups.end(); start_ix = g->ix, ++g) {
		if (!dstate.check_cube_visible(*g, dist_scale)) continue; // VFC/distance culling for group
		if (has_immediate_draw) {dstate.begin_tile(g->get_cube_center(), 1, 1);} // must setup shader and tile shadow map before drawing
		assert(start_ix <= g->ix && g->ix <= objs.size());

		for (unsigned i = start_ix; i < g->ix; ++i) {
			T const &obj(objs[i]);
			if (dstate.check_sphere_visible(obj.pos, obj.get_bsphere_radius(shadow_only))) {obj.draw(dstate, *this, dist_scale, shadow_only);}
		}
		if (!city_draw_qbds_t::empty() || !dstate.hedge_draw.empty()) { // we have something to draw
			if (!has_immediate_draw) {dstate.begin_tile(g->get_cube_center(), 1, 1);} // will_emit_now=1, ensure_active=1
			bool must_restore_state(!dstate.hedge_draw.empty());
			dstate.hedge_draw.draw_and_clear(dstate.s);

			if (has_untex_verts()) { // draw untextured verts before qbd so that textures such as text can alpha blend on top
				dstate.set_untextured_material();
				untex_qbd.draw_and_clear(); // matte

				if (!untex_spec_qbd.empty()) { // specular
					dstate.s.set_specular(specular, shininess); // shiny; values are per-object type
					untex_spec_qbd.draw_and_clear();
					dstate.s.clear_specular();
				}
				dstate.unset_untextured_material();
				must_restore_state = 1;
			}
			if (!shadow_only && must_restore_state) {T::pre_draw(dstate, shadow_only);} // re-setup for below draw call and/or next tile
			if (draw_qbd_as_quads) {qbd.draw_and_clear_quads();} else {qbd.draw_and_clear();} // draw this group with current smap

			if (!emissive_qbd.empty()) { // draw emissive materials
				dstate.s.add_uniform_float("emissive_scale", 1.0); // 100% emissive
				if (draw_qbd_as_quads) {emissive_qbd.draw_and_clear_quads();} else {emissive_qbd.draw_and_clear();}
				dstate.s.add_uniform_float("emissive_scale", 0.0); // reset
			}
		}
	} // for g
	T::post_draw(dstate, shadow_only);
}

void city_obj_placer_t::clear() {
	parking_lots.clear(); benches.clear(); planters.clear(); trashcans.clear(); fhydrants.clear(); sstations.clear(); driveways.clear(); dividers.clear();
	pools.clear(); pdecks.clear(); ppoles.clear(); hcaps.clear(); manholes.clear(); mboxes.clear(); pigeons.clear(); signs.clear(); stopsigns.clear(); flags.clear();
	bench_groups.clear(); planter_groups.clear(); trashcan_groups.clear(); fhydrant_groups.clear(); sstation_groups.clear(); divider_groups.clear(); pool_groups.clear();
	ppole_groups.clear(); hcap_groups.clear(); manhole_groups.clear(); mbox_groups.clear(); pigeon_groups.clear(); sign_groups.clear(); stopsign_groups.clear();
	flag_groups.clear(); nrack_groups.clear();
	all_objs_bcube.set_to_zeros();
	num_spaces = filled_spaces = 0;
}

void city_obj_placer_t::gen_parking_and_place_objects(vector<road_plot_t> &plots, vector<vect_cube_t> &plot_colliders, vector<car_t> &cars,
	vector<road_t> const &roads, vector<road_isec_t> isecs[3], unsigned city_id, bool have_cars, bool is_residential, bool have_streetlights)
{
	// Note: fills in plots.has_parking
	//timer_t timer("Gen Parking Lots and Place Objects");
	vect_cube_t bcubes, temp_cubes; // blockers, driveways
	vector<point> tree_pos;
	rand_gen_t rgen, detail_rgen;
	rgen.set_state(city_id, 123);
	detail_rgen.set_state(3145739*(city_id+1), 1572869*(city_id+1));
	if (city_params.max_trees_per_plot > 0) {tree_placer.begin_block(0); tree_placer.begin_block(1);} // both small and large trees
	bool const add_parking_lots(have_cars && !is_residential && city_params.min_park_spaces > 0 && city_params.min_park_rows > 0);
	float const sidewalk_width(get_sidewalk_width());

	for (auto i = plots.begin(); i != plots.end(); ++i) { // calculate num_x_plots and num_y_plots; these are used for determining edge power poles
		max_eq(num_x_plots, i->xpos+1U);
		max_eq(num_y_plots, i->ypos+1U);
	}
	for (auto i = plots.begin(); i != plots.end(); ++i) {
		tree_pos.clear();
		bcubes.clear();
		get_building_bcubes(*i, bcubes);
		size_t const plot_id(i - plots.begin()), buildings_end(bcubes.size());
		assert(plot_id < plot_colliders.size());
		vect_cube_t &colliders(plot_colliders[plot_id]); // used for pedestrians
		if (add_parking_lots && !i->is_park) {i->has_parking = gen_parking_lots_for_plot(*i, cars, city_id, plot_id, bcubes, colliders, rgen);}
		unsigned const driveways_start(driveways.size());
		if (is_residential) {add_house_driveways(*i, temp_cubes, detail_rgen, plot_id);}

		// driveways become blockers for other placed objects; make sure they extend into the road so that they intersect any placed streetlights or fire hydrants
		for (auto j = driveways.begin()+driveways_start; j != driveways.end(); ++j) {
			cube_t dw(*j);

			for (unsigned d = 0; d < 2; ++d) {
				if      (dw.d[d][0] == i->d[d][0]) {dw.d[d][0] -= sidewalk_width;}
				else if (dw.d[d][1] == i->d[d][1]) {dw.d[d][1] += sidewalk_width;}
			}
			bcubes.push_back(dw);
		} // for j
		if (city_params.assign_house_plots && plot_subdiv_sz > 0.0) {
			place_residential_plot_objects(*i, bcubes, colliders, roads, driveways_start, city_id, detail_rgen); // before placing trees
		}
		place_trees_in_plot (*i, bcubes, colliders, tree_pos, detail_rgen, buildings_end);
		place_detail_objects(*i, bcubes, colliders, tree_pos, detail_rgen, is_residential, have_streetlights);
	} // for i
	for (unsigned n = 0; n < 3; ++n) {
		for (road_isec_t &isec : isecs[n]) {place_signs_in_isec(isec);} // Note: not a plot, can't use plot colliders
	}
	add_stop_sign_plot_colliders(plots, plot_colliders);
	connect_power_to_buildings(plots);
	if (have_cars) {add_cars_to_driveways(cars, plots, plot_colliders, city_id, rgen);}
	add_objs_on_buildings(city_id);
	for (auto i = plot_colliders.begin(); i != plot_colliders.end(); ++i) {sort(i->begin(), i->end(), cube_by_x1());}
	bench_groups   .create_groups(benches,   all_objs_bcube);
	planter_groups .create_groups(planters,  all_objs_bcube);
	trashcan_groups.create_groups(trashcans, all_objs_bcube);
	fhydrant_groups.create_groups(fhydrants, all_objs_bcube);
	sstation_groups.create_groups(sstations, all_objs_bcube);
	divider_groups .create_groups(dividers,  all_objs_bcube);
	pool_groups    .create_groups(pools,     all_objs_bcube);
	pdeck_groups   .create_groups(pdecks,    all_objs_bcube);
	ppole_groups   .create_groups(ppoles,    all_objs_bcube);
	hcap_groups    .create_groups(hcaps,     all_objs_bcube);
	manhole_groups .create_groups(manholes,  all_objs_bcube);
	mbox_groups    .create_groups(mboxes,    all_objs_bcube);
	pigeon_groups  .create_groups(pigeons,   all_objs_bcube);
	sign_groups    .create_groups(signs,     all_objs_bcube);
	stopsign_groups.create_groups(stopsigns, all_objs_bcube);
	flag_groups    .create_groups(flags,     all_objs_bcube);
	nrack_groups   .create_groups(newsracks, all_objs_bcube);

	if (0) { // debug info printing
		cout << TXT(benches.size()) << TXT(planters.size()) << TXT(trashcans.size()) << TXT(fhydrants.size()) << TXT(sstations.size()) << TXT(dividers.size())
			 << TXT(pools.size()) << TXT(pdecks.size()) << TXT(ppoles.size()) << TXT(hcaps.size()) << TXT(manholes.size()) << TXT(mboxes.size()) << TXT(pigeons.size())
			 << TXT(signs.size()) << TXT(stopsigns.size()) << TXT(flags.size()) << TXT(newsracks.size()) << endl;
	}
	if (add_parking_lots) {
		cout << "parking lots: " << parking_lots.size() << ", spaces: " << num_spaces << ", filled: " << filled_spaces << ", benches: " << benches.size() << endl;
	}
}

void city_obj_placer_t::add_objs_on_buildings(unsigned city_id) {
	// add signs and flags attached to buildings; note that some signs and flags may have already been added at this point
	vector<sign_t> signs_to_add;
	vector<city_flag_t> flags_to_add;
	add_signs_for_city(city_id, signs_to_add);
	add_flags_for_city(city_id, flags_to_add);
	signs.reserve(signs.size() + signs_to_add.size());
	flags.reserve(flags.size() + flags_to_add.size());
	for (sign_t      const &sign : signs_to_add) {sign_groups.add_obj(sign, signs);}
	for (city_flag_t const &flag : flags_to_add) {flag_groups.add_obj(flag, flags);}
}

/*static*/ bool city_obj_placer_t::subdivide_plot_for_residential(cube_t const &plot, vector<road_t> const &roads,
	float plot_subdiv_sz, unsigned parent_plot_ix, unsigned city_ix, vect_city_zone_t &sub_plots)
{
	if (min(plot.dx(), plot.dy()) < city_params.road_width) return 0; // plot is too small to divide
	assert(plot_subdiv_sz > 0.0);
	unsigned ndiv[2] = {0,0};
	float spacing[2] = {0,0};

	for (unsigned d = 0; d < 2; ++d) {
		float const plot_sz(plot.get_sz_dim(d));
		ndiv   [d] = max(1U, unsigned(round_fp(plot_sz/plot_subdiv_sz)));
		spacing[d] = plot_sz/ndiv[d];
	}
	if (ndiv[0] >= 100 || ndiv[1] >= 100) return 0; // too many plots? this shouldn't happen, but failing here is better than asserting or generating too many buildings
	unsigned const max_floors(0); // 0 is unlimited
	if (sub_plots.empty()) {sub_plots.reserve(2*(ndiv[0] + ndiv[1]) - 4);}

	for (unsigned y = 0; y < ndiv[1]; ++y) {
		float const y1(plot.y1() + spacing[1]*y), y2((y+1 == ndiv[1]) ? plot.y2() : (y1 + spacing[1])); // last sub-plot must end exactly at plot y2

		for (unsigned x = 0; x < ndiv[0]; ++x) {
			if (x > 0 && y > 0 && x+1 < ndiv[0] && y+1 < ndiv[1]) continue; // interior plot, no road access, skip
			float const x1(plot.x1() + spacing[0]*x), x2((x+1 == ndiv[0]) ? plot.x2() : (x1 + spacing[0])); // last sub-plot must end exactly at plot x2
			cube_t const c(x1, x2, y1, y2, plot.z1(), plot.z2());
			unsigned const street_dir(get_street_dir(c, plot)); // will favor x-dim for corner plots
			sub_plots.emplace_back(c, 0.0, 0, 1, street_dir, 1, parent_plot_ix, city_ix, max_floors); // cube, zval, park, res, sdir, capacity, ppix, cix, nf

			if (!roads.empty()) { // find the road name and determine the street address
				bool const sdim((street_dir - 1) >> 1), sdir((street_dir - 1) & 1);
				unsigned road_ix(0);
				float dmin(FLT_MAX), road_pos(0.0);

				for (auto r = roads.begin(); r != roads.end(); ++r) {
					if (r->dim == sdim) continue;
					float const dist(fabs(r->d[sdim][!sdir] - c.d[sdim][sdir]));
					if (dist < dmin) {dmin = dist; road_ix = (r - roads.begin()); road_pos = (c.get_center_dim(!sdim) - r->d[!sdim][0]);}
				}
				string const &road_name(roads[road_ix].get_name(city_ix));
				unsigned street_number(100 + 100*((7*road_ix + 13*road_name.size())%20)); // start at 100-2000 randomly based on road name and index
				street_number += 2*round_fp(25.0*road_pos/plot.get_sz_dim(!sdim)); // generate an even street number; should it differ by more than 1?
				if (sdir) {++street_number;} // make it an odd number if on this side of the road
				sub_plots.back().address    = std::to_string(street_number) + " " + road_name;
				sub_plots.back().street_num = street_number;
			}
		} // for x
	} // for y
	return 1;
}

bool city_obj_placer_t::connect_power_to_point(point const &at_pos, bool near_power_pole) {
	float dmax_sq(0.0);

	for (unsigned n = 0; n < 4; ++n) { // make up to 4 attempts to connect a to a power pole without intersecting a building
		float dmin_sq(0.0);
		unsigned best_pole(0);
		point best_pos;

		for (auto p = ppoles.begin(); p != ppoles.end(); ++p) {
			point const cur_pos(p->get_nearest_connection_point(at_pos, near_power_pole));
			if (cur_pos == at_pos) continue; // bad point
			float const dsq(p2p_dist_sq(at_pos, cur_pos));
			if (dsq <= dmax_sq) continue; // this pole was previously flagged as bad
			if (dmin_sq == 0.0 || dsq < dmin_sq) {best_pos = cur_pos; dmin_sq = dsq; best_pole = (p - ppoles.begin());}
		} // for p
		if (dmin_sq == 0.0) return 0; // failed (no power poles?)
		if (ppoles[best_pole].add_wire(at_pos, best_pos, near_power_pole)) return 1; // add a wire pole for houses
		dmax_sq = dmin_sq; // prevent this pole from being used in the next iteration
	} // for n
	return 0; // failed
}
void city_obj_placer_t::connect_power_to_buildings(vector<road_plot_t> const &plots) {
	if (plots.empty() || ppoles.empty() || !have_buildings()) return;
	cube_t all_plots_bcube(plots.front());
	for (auto p = plots.begin()+1; p != plots.end(); ++p) {all_plots_bcube.union_with_cube(*p);} // query all buildings in the entire city rather than per-plot
	vector<point> ppts;
	get_building_power_points(all_plots_bcube, ppts);
	for (auto p = ppts.begin(); p != ppts.end(); ++p) {connect_power_to_point(*p, 1);} // near_power_pole=1
}

void city_obj_placer_t::move_to_not_intersect_driveway(point &pos, float radius, bool dim) const {
	cube_t test_cube;
	test_cube.set_from_sphere(pos, radius);

	// Note: this could be accelerated by iterating by plot, but this seems to already be fast enough (< 1ms)
	for (auto d = driveways.begin(); d != driveways.end(); ++d) {
		if (!d->intersects_xy(test_cube)) continue;
		bool const dir((d->d[dim][1] - pos[dim]) < (pos[dim] - d->d[dim][0]));
		pos[dim] = d->d[dim][dir] + (dir ? 1.0 : -1.0)*0.1*city_params.road_width;
		break; // maybe we should check for an adjacent driveway, but that would be rare and moving could result in oscillation
	}
}
void city_obj_placer_t::move_and_connect_streetlights(streetlights_t &sl) {
	for (auto s = sl.streetlights.begin(); s != sl.streetlights.end(); ++s) {
		if (!driveways.empty()) { // move to avoid driveways
			bool const dim(s->dir.y == 0.0); // direction to move in
			move_to_not_intersect_driveway(s->pos, 0.25*city_params.road_width, dim);
		}
		if (!ppoles.empty()) { // connect power
			point top(s->get_lpos());
			top.z += 1.05f*streetlight_ns::light_radius*city_params.road_width; // top of light
			connect_power_to_point(top, 0); // near_power_pole=0 because it may be too far away
		}
	} // for s
}

void city_obj_placer_t::draw_detail_objects(draw_state_t &dstate, bool shadow_only) {
	if (!dstate.check_cube_visible(all_objs_bcube, 1.0)) return; // check bcube, dist_scale=1.0
	draw_objects(benches,   bench_groups,    dstate, 0.16, shadow_only, 0); // dist_scale=0.16
	draw_objects(fhydrants, fhydrant_groups, dstate, 0.06, shadow_only, 1); // dist_scale=0.07, has_immediate_draw=1
	draw_objects(sstations, sstation_groups, dstate, 0.15, shadow_only, 1); // dist_scale=0.15, has_immediate_draw=1
	draw_objects(mboxes,    mbox_groups,     dstate, 0.04, shadow_only, 1); // dist_scale=0.10, has_immediate_draw=1
	draw_objects(ppoles,    ppole_groups,    dstate, 0.20, shadow_only, 0); // dist_scale=0.20
	draw_objects(signs,     sign_groups,     dstate, 0.25, shadow_only, 0, 1); // dist_scale=0.25, draw_qbd_as_quads=1
	draw_objects(flags,     flag_groups,     dstate, 0.18, shadow_only, 0); // dist_scale=0.16
	draw_objects(newsracks, nrack_groups,    dstate, 0.10, shadow_only, 0); // dist_scale=0.14
	if (!shadow_only) {draw_objects(hcaps,    hcap_groups,    dstate, 0.12, shadow_only, 0);} // dist_scale=0.12, no shadows
	if (!shadow_only) {draw_objects(manholes, manhole_groups, dstate, 0.07, shadow_only, 1);} // dist_scale=0.07, no shadows, has_immediate_draw=1
	if (!shadow_only) {draw_objects(pigeons,  pigeon_groups,  dstate, 0.03, shadow_only, 1);} // dist_scale=0.10, no shadows, has_immediate_draw=1
	dstate.s.add_uniform_float("min_alpha", DEF_CITY_MIN_ALPHA); // reset back to default after drawing fire hydrant and substation models
	
	for (dstate.pass_ix = 0; dstate.pass_ix < 2; ++dstate.pass_ix) { // {cube/city, cylinder/residential}
		bool const is_cylin(dstate.pass_ix > 0);
		draw_objects(trashcans, trashcan_groups, dstate, (is_cylin ? 0.08 : 0.10), shadow_only, is_cylin); // has_immediate_draw=cylinder
	}
	if (!shadow_only) { // low profile, not drawn in shadow pass
		for (dstate.pass_ix = 0; dstate.pass_ix < 2; ++dstate.pass_ix) { // {dirt, stone}
			draw_objects(planters, planter_groups, dstate, 0.1, shadow_only, 0); // dist_scale=0.1
		}
	}
	for (dstate.pass_ix = 0; dstate.pass_ix < 4; ++dstate.pass_ix) { // {in-ground walls, in-ground water, above ground sides, above ground water}
		if (shadow_only && dstate.pass_ix <= 1) continue; // only above ground pools are drawn in the shadow pass; water surface is drawn to prevent light leaks, but maybe should extend z1 lower
		float const dist_scales[4] = {0.1, 0.5, 0.3, 0.5};
		draw_objects(pools, pool_groups, dstate, dist_scales[dstate.pass_ix], shadow_only, (dstate.pass_ix > 1)); // final 2 passes use immediate draw rather than qbd
	}
	// Note: not the most efficient solution, as it required processing blocks and binding shadow maps multiple times
	for (dstate.pass_ix = 0; dstate.pass_ix <= DIV_NUM_TYPES; ++dstate.pass_ix) { // {wall, fence, hedge, chainlink fence, chainlink fence posts}
		if (dstate.pass_ix == DIV_CHAINLINK && shadow_only) continue; // chainlink fence not drawn in the shadow pass
		draw_objects(dividers, divider_groups, dstate, 0.2, shadow_only, 0); // dist_scale=0.2
	}
	if (!shadow_only) {
		for (dstate.pass_ix = 0; dstate.pass_ix < NUM_POOL_DECK_TYPES; ++dstate.pass_ix) { // {wood, concrete}
			draw_objects(pdecks, pdeck_groups, dstate, 0.26, shadow_only, 0); // dist_scale=0.3
		}
	}
	for (dstate.pass_ix = 0; dstate.pass_ix < 3; ++dstate.pass_ix) { // {sign front, sign back + pole, 4-way sign}
		draw_objects(stopsigns, stopsign_groups, dstate, 0.1, shadow_only, 0); // dist_scale=0.1
	}
	dstate.pass_ix = 0; // reset back to 0
}

template<typename T> bool proc_vector_sphere_coll(vector<T> const &objs, city_obj_groups_t const &groups, point &pos,
	point const &p_last, float radius, vector3d const &xlate, vector3d *cnorm)
{
	point const pos_bs(pos - xlate);
	unsigned start_ix(0);

	for (auto g = groups.begin(); g != groups.end(); start_ix = g->ix, ++g) {
		if (!sphere_cube_intersect(pos_bs, radius, *g)) continue;
		assert(start_ix <= g->ix && g->ix <= objs.size());

		for (auto i = objs.begin()+start_ix; i != objs.begin()+g->ix; ++i) {
			if (i->proc_sphere_coll(pos, p_last, radius, xlate, cnorm)) return 1;
		}
	} // for g
	return 0;
}
bool city_obj_placer_t::proc_sphere_coll(point &pos, point const &p_last, vector3d const &xlate, float radius, vector3d *cnorm) const { // pos in in camera space
	if (!sphere_cube_intersect(pos, (radius + p2p_dist(pos,p_last)), (all_objs_bcube + xlate))) return 0;
	if (proc_vector_sphere_coll(benches,   bench_groups,    pos, p_last, radius, xlate, cnorm)) return 1;
	if (proc_vector_sphere_coll(trashcans, trashcan_groups, pos, p_last, radius, xlate, cnorm)) return 1;
	if (proc_vector_sphere_coll(fhydrants, fhydrant_groups, pos, p_last, radius, xlate, cnorm)) return 1;
	if (proc_vector_sphere_coll(sstations, sstation_groups, pos, p_last, radius, xlate, cnorm)) return 1;
	if (proc_vector_sphere_coll(dividers,  divider_groups,  pos, p_last, radius, xlate, cnorm)) return 1;
	if (proc_vector_sphere_coll(pools,     pool_groups,     pos, p_last, radius, xlate, cnorm)) return 1;
	if (proc_vector_sphere_coll(ppoles,    ppole_groups,    pos, p_last, radius, xlate, cnorm)) return 1;
	if (proc_vector_sphere_coll(mboxes,    mbox_groups,     pos, p_last, radius, xlate, cnorm)) return 1;
	if (proc_vector_sphere_coll(signs,     sign_groups,     pos, p_last, radius, xlate, cnorm)) return 1;
	if (proc_vector_sphere_coll(stopsigns, stopsign_groups, pos, p_last, radius, xlate, cnorm)) return 1;
	if (proc_vector_sphere_coll(flags,     flag_groups,     pos, p_last, radius, xlate, cnorm)) return 1;
	if (proc_vector_sphere_coll(newsracks, nrack_groups,    pos, p_last, radius, xlate, cnorm)) return 1;
	// Note: no coll with tree_planters because the tree coll should take care of it; no coll with hcaps, manholes, pool decks, or pigeons
	return 0;
}

template<typename T> void check_vector_line_intersect(vector<T> const &objs, city_obj_groups_t const &groups, point const &p1, point const &p2, float &t, bool &ret) {
	unsigned start_ix(0);

	for (auto g = groups.begin(); g != groups.end(); start_ix = g->ix, ++g) {
		if (!check_line_clip(p1, p2, g->d)) continue;
		assert(start_ix <= g->ix && g->ix <= objs.size());
		for (auto i = objs.begin()+start_ix; i != objs.begin()+g->ix; ++i) {ret |= check_line_clip_update_t(p1, p2, t, i->bcube);}
	}
}
bool city_obj_placer_t::line_intersect(point const &p1, point const &p2, float &t) const { // p1/p2 in world space; uses object bcubes
	if (!all_objs_bcube.line_intersects(p1, p2)) return 0;
	bool ret(0);
	check_vector_line_intersect(benches,   bench_groups,    p1, p2, t, ret);
	check_vector_line_intersect(trashcans, trashcan_groups, p1, p2, t, ret);
	check_vector_line_intersect(fhydrants, fhydrant_groups, p1, p2, t, ret); // check bounding cube; cylinder intersection may be more accurate, but likely doesn't matter much
	check_vector_line_intersect(sstations, sstation_groups, p1, p2, t, ret);
	check_vector_line_intersect(dividers,  divider_groups,  p1, p2, t, ret);
	check_vector_line_intersect(pools,     pool_groups,     p1, p2, t, ret);
	check_vector_line_intersect(ppoles,    ppole_groups,    p1, p2, t, ret); // inaccurate, could be customized if needed
	check_vector_line_intersect(signs,     sign_groups,     p1, p2, t, ret);
	check_vector_line_intersect(stopsigns, stopsign_groups, p1, p2, t, ret);
	check_vector_line_intersect(flags,     flag_groups,     p1, p2, t, ret);
	check_vector_line_intersect(newsracks, nrack_groups,    p1, p2, t, ret);
	// Note: nothing to do for parking lots, tree_planters, hcaps, manholes, pool decks, or pigeons; mboxes are ignored because they're not simple shapes
	return ret;
}

template<typename T> bool check_city_obj_bcube_pt_xy_contain(city_obj_groups_t const &groups, vector<T> const &objs, point const &pos, unsigned &obj_ix) {
	unsigned start_ix(0);

	for (auto i = groups.begin(); i != groups.end(); start_ix = i->ix, ++i) {
		if (!i->contains_pt_xy(pos)) continue;
		assert(start_ix <= i->ix && i->ix <= objs.size());

		for (auto b = objs.begin()+start_ix; b != objs.begin()+i->ix; ++b) {
			if (pos.x < b->bcube.x1()) break; // objects are sorted by x1, none after this can match
			if (b->bcube.contains_pt_xy(pos)) {obj_ix = (b - objs.begin()); return 1;}
		}
	} // for i
	return 0;
}
bool city_obj_placer_t::get_color_at_xy(point const &pos, colorRGBA &color, bool skip_in_road) const {
	unsigned start_ix(0), obj_ix(0);
	if (check_city_obj_bcube_pt_xy_contain(bench_groups, benches, pos, obj_ix)) {color = texture_color(FENCE_TEX); return 1;}
	float const expand(0.15*city_params.road_width), x_test(pos.x + expand); // expand to approx tree diameter

	for (auto i = planter_groups.begin(); i != planter_groups.end(); start_ix = i->ix, ++i) {
		if (!i->contains_pt_xy_exp(pos, expand)) continue;
		assert(start_ix <= i->ix && i->ix <= planters.size());

		for (auto p = planters.begin()+start_ix; p != planters.begin()+i->ix; ++p) {
			if (x_test < p->bcube.x1()) break; // planters are sorted by x1, none after this can match
			if (!p->bcube.contains_pt_xy_exp(pos, expand)) continue;
			// treat this as a tree rather than a planter by testing against a circle, since trees aren't otherwise included
			if (dist_xy_less_than(pos, p->pos, (p->radius + expand))) {color = DK_GREEN; return 1;}
		}
	} // for i
	start_ix = 0;

	if (!skip_in_road) { // fire hydrants are now placed on the edges of the road, so they're not inside plots and are skipped here
		for (auto i = fhydrant_groups.begin(); i != fhydrant_groups.end(); start_ix = i->ix, ++i) {
			if (!i->contains_pt_xy(pos)) continue;
			assert(start_ix <= i->ix && i->ix <= fhydrants.size());

			for (auto b = fhydrants.begin()+start_ix; b != fhydrants.begin()+i->ix; ++b) {
				if (pos.x < b->bcube.x1()) break; // fire_hydrants are sorted by x1, none after this can match
				if (dist_xy_less_than(pos, b->pos, b->radius)) {color = colorRGBA(1.0, 0.75, 0.0); return 1;} // orange/yellow color
			}
		} // for i
		start_ix = 0;
	}
	if (check_city_obj_bcube_pt_xy_contain(divider_groups, dividers, pos, obj_ix)) {
		assert(obj_ix < dividers.size());
		color = plot_divider_types[dividers[obj_ix].type].get_avg_color();
		return 1;
	}
	for (auto i = pool_groups.begin(); i != pool_groups.end(); start_ix = i->ix, ++i) {
		if (!i->contains_pt_xy(pos)) continue;
		assert(start_ix <= i->ix && i->ix <= pools.size());

		for (auto b = pools.begin()+start_ix; b != pools.begin()+i->ix; ++b) {
			if (pos.x < b->bcube.x1()) break; // pools are sorted by x1, none after this can match
			if (!b->bcube.contains_pt_xy(pos)) continue;
			if (b->above_ground && !dist_xy_less_than(pos, point(b->bcube.xc(), b->bcube.yc(), b->bcube.z1()), b->get_radius())) continue; // circular in-ground pool
			color = b->wcolor; // return water color
			return 1;
		}
	} // for i
	start_ix = 0;

	if (check_city_obj_bcube_pt_xy_contain(pdeck_groups, pdecks, pos, obj_ix)) {
		assert(obj_ix < pdecks.size());
		color = pool_deck_mats[pdecks[obj_ix].mat_id].get_avg_color();
		return 1;
	}
	if (!skip_in_road && check_city_obj_bcube_pt_xy_contain(nrack_groups, newsracks, pos, obj_ix)) { // now placed in roads
		assert(obj_ix < newsracks.size());
		color = newsracks[obj_ix].color;
		return 1;
	}
	if (check_city_obj_bcube_pt_xy_contain(sstation_groups, sstations, pos, obj_ix)) {color = colorRGBA(0.6, 0.8, 0.4, 1.0); return 1;} // light olive
	if (check_city_obj_bcube_pt_xy_contain(trashcan_groups, trashcans, pos, obj_ix)) {color = colorRGBA(0.8, 0.6, 0.3, 1.0); return 1;} // tan
	// Note: ppoles, hcaps, manholes, mboxes, signs, stopsigns, flags, and pigeons are skipped for now
	return 0;
}

void city_obj_placer_t::get_occluders(pos_dir_up const &pdu, vect_cube_t &occluders) const {
	if (dividers.empty()) return; // dividers are currently the only occluders
	float const dmax(0.25f*(X_SCENE_SIZE + Y_SCENE_SIZE)); // set far clipping plane to 1/4 a tile (currently 2.0)
	unsigned start_ix(0);

	for (auto i = divider_groups.begin(); i != divider_groups.end(); start_ix = i->ix, ++i) {
		if (!dist_less_than(pdu.pos, i->closest_pt(pdu.pos), dmax) || !pdu.cube_visible(*i)) continue;
		assert(start_ix <= i->ix && i->ix <= dividers.size());

		for (auto d = dividers.begin()+start_ix; d != dividers.begin()+i->ix; ++d) {
			assert(d->type < DIV_NUM_TYPES);
			if (!plot_divider_types[d->type].is_occluder) continue; // skip
			if (d->bcube.z1() > pdu.pos.z || d->bcube.z2() < pdu.pos.z) continue; // z-range does not include the camera
			if (dist_less_than(pdu.pos, d->bcube.closest_pt(pdu.pos), dmax) && pdu.cube_visible(d->bcube)) {occluders.push_back(d->bcube);}
		}
	} // for i
}


