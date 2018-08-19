// 3D World - Collision Object Destruction Code
// by Frank Gennari
// 5/31/11
#include "3DWorld.h"
#include "mesh.h"
#include "csg.h"
#include "physics_objects.h"
#include "openal_wrap.h"


bool const LET_COBJS_FALL    = 0;
bool const REMOVE_UNANCHORED = 1;

int destroy_thresh(0);
vector<unsigned> falling_cobjs;

extern unsigned scene_smap_vbo_invalid;
extern float tstep, zmin, base_gravity;
extern int cobj_counter, coll_id[];
extern obj_type object_types[];
extern obj_group obj_groups[];
extern coll_obj_group coll_objects;
extern vector<portal> portals;
extern vector<obj_draw_group> obj_draw_groups;
extern cobj_groups_t cobj_groups;


unsigned subtract_cube(vector<color_tid_vol> &cts, vector3d &cdir, csg_cube const &cube, int destroy_thresh);


// **************** Cobj Destroy Code ****************


void destroy_coll_objs(point const &pos, float damage, int shooter, int damage_type, float force_radius) {

	//RESET_TIME;
	assert(damage >= 0.0);
	if (damage < 100.0) return;
	float const radius((force_radius > 0.0) ? force_radius : ((damage_type == BLAST_RADIUS) ? 4.0 : 1.0)*sqrt(damage)/650.0);
	vector3d cdir;
	vector<color_tid_vol> cts;
	int const dmin((damage_type == FIRE) ? EXPLODEABLE : ((damage > 800.0) ? DESTROYABLE : ((damage > 200.0) ? SHATTERABLE : EXPLODEABLE)));
	csg_cube cube(pos.x, pos.x, pos.y, pos.y, pos.z, pos.z);
	cube.expand_by(radius);
	unsigned nrem(subtract_cube(cts, cdir, cube, dmin));
	if (nrem == 0 || cts.empty()) return; // nothing removed
	int const xpos(get_xpos(pos.x)), ypos(get_ypos(pos.y));

	if (!point_outside_mesh(xpos, ypos)) {
		// check for waypoints that can be added near this cube (at the center only)
		vector<int> const &cvals(v_collision_matrix[ypos][xpos].cvals);

		for (vector<int>::const_iterator i = cvals.begin(); i != cvals.end(); ++i) {
			if (*i >= 0 && coll_objects.get_cobj(*i).waypt_id < 0) {coll_objects.get_cobj(*i).add_connect_waypoint();} // slow
		}
	}

	// update voxel pflow map for removal
	vector<cube_t> cubes;
	cubes.push_back(cube);

	for (unsigned i = 0; i < cts.size(); ++i) {
		if (cts[i].destroy >= SHATTERABLE || cts[i].unanchored) {cubes.push_back(cts[i]);}
	}
	update_flow_for_voxels(cubes);

	// create fragments
	float const cdir_mag(cdir.mag());
	bool maybe_is_glass(0);

	for (unsigned i = 0; i < cts.size(); ++i) {
		if (cts[i].destroy >= EXPLODEABLE) {
			float const val(float(pow(double(cts[i].volume), 1.0/3.0))), exp_damage(25000.0*val + 0.25*damage + 500.0);
			create_explosion(pos, shooter, 0, exp_damage, 10.0*val, BLAST_RADIUS, 0);
			gen_fire(pos, min(4.0, 12.0*val), shooter);
		}
		if (!cts[i].draw) continue;
		float size_scale(1.0), num_parts(0.0);
		float const thickness(cts[i].thickness);
		bool const shattered(cts[i].destroy >= SHATTERABLE);
		bool const tri_fragments(shattered || cts[i].is_2d); // thin polys shatter to triangle fragments
		float const frag_radius(object_types[FRAGMENT].radius), avg_frag_dia(2.0*frag_radius), max_frag_dia(3.0*frag_radius);
		if (cts[i].volume < (tri_fragments ? MIN_POLY_THICK : frag_radius)*frag_radius*frag_radius) continue;

		if (tri_fragments) {
			float const mfs(cts[i].max_frag_sz);
			if (mfs < 1.2*max_frag_dia) {size_scale *= mfs/max_frag_dia;}
			float const dia(size_scale*avg_frag_dia);
			num_parts = cts[i].volume/(thickness*dia*dia);
		}
		else {
			if (thickness < 1.2*max_frag_dia) {size_scale *= thickness/max_frag_dia;}
			float const dia(size_scale*avg_frag_dia);
			num_parts = cts[i].volume/(dia*dia*dia);
		}
		if (size_scale < 0.1) continue;
		obj_group const &group(obj_groups[coll_id[FRAGMENT]]);
		unsigned const num_avail(group.max_objs - group.end_id);
		unsigned const max_parts(tri_fragments ? min(500U, max(group.max_objs/10, num_avail/4)) : 100);
		unsigned const num(min(max_parts, max(((tri_fragments && !cts[i].is_2d) ? 6U : 1U), unsigned(num_parts)))); // no more than 100-500
		if (tri_fragments && num < num_parts) {size_scale *= sqrt(num_parts/(float)num);} // if need more than the max parts, make them larger to preserve area/volume
		//cout << "shattered: " << shattered << ", tri: " << tri_fragments << ", volume: " << cts[i].volume << ", num_parts: " << num_parts << ", num: " << num << ", ss: " << size_scale << endl;
		bool const non_csg(shattered || cts[i].unanchored);
		csg_cube frag_cube(cts[i]);
		if (!non_csg && !cube.cube_intersection(frag_cube, frag_cube)) {frag_cube = cts[i];} // intersect frag_cube with cube (should pass)
		bool const explodeable(0); //cts[i].destroy >= EXPLODEABLE);
		bool const blacken(tri_fragments && cts[i].color.alpha == 1.0 && ((damage_type != IMPACT && damage_type != PROJECTILE) || explodeable));
		float const blacken_radius(2.0*radius);

		for (unsigned o = 0; o < num; ++o) {
			vector3d velocity(cdir);
			point fpos(global_rand_gen.gen_rand_cube_point(frag_cube)); // only accurate for COLL_CUBE
			float hotness(0.0);

			if (non_csg) {
				vector3d const vadd(fpos - pos); // average cdir and direction from collision point to fragment location
				float const vadd_mag(vadd.mag());

				if (vadd_mag > TOLERANCE) {
					velocity += vadd*(cdir_mag/(vadd_mag*vadd_mag)); // is this double divide by vadd_mag correct?
					velocity *= 0.5;
				}
			}
			if (blacken && dist_less_than(fpos, pos, blacken_radius)) {
				hotness = 1.0 - (explodeable ? 0.0 : p2p_dist(fpos, pos)/blacken_radius);
				cts[i].color *= 1.0 - hotness; // blacken due to heat from explosion
			}
			if (is_underwater(fpos)) {velocity *= 0.1;} // make it slow when underwater
			float const time_mult((hotness > 0.0) ? 0.0 : 0.5*rand_float());
			//vector3d const &orient(tri_fragments ? cts[i].min_thick_dir : zero_vector);
			gen_fragment(fpos, velocity, size_scale, time_mult, cts[i].color, cts[i].tid, cts[i].tscale, shooter, tri_fragments, hotness);
		}
		if (shattered && tri_fragments && cts[i].maybe_is_glass()) {maybe_is_glass = 1;}
	} // for i
	gen_delayed_from_player_sound((maybe_is_glass ? SOUND_GLASS : SOUND_WOOD_CRACK), pos);
	//PRINT_TIME("Destroy Cobjs");
}


void coll_obj::create_portal() const {

	switch (type) {
	case COLL_POLYGON:
		{
			assert(npoints == 3 || npoints == 4);
			portal p;

			for (int i = 0; i < npoints; ++i) {
				p.pts[i] = points[i]; // ignore thickness - use base polygon only
			}
			if (npoints == 3) p.pts[3] = p.pts[2]; // duplicate the last point
			portals.push_back(p); // Note: p.normal is not set
			break;
		}
	case COLL_CUBE:
		{
			portal p;
			float max_area(0.0);
		
			for (unsigned i = 0; i < 6; ++i) { // choose enabled side with max area
				unsigned const dim(i>>1), dir(i&1), d0((dim+1)%3), d1((dim+2)%3);
				if (cp.surfs & EFLAGS[dim][dir]) continue; // disabled side
				float const area(fabs(d[d0][1] - d[d0][0])*fabs(d[d1][1] - d[d1][0]));

				if (area > max_area) {
					max_area = area;
					point pos;
					pos[dim] = d[dim][dir];

					for (unsigned n = 0; n < 4; ++n) {
						pos[d0] = d[d0][n<2];
						pos[d1] = d[d1][(n&1)^(n<2)];
						p.pts[n] = pos;
					}
				}
			}
			if (max_area > 0.0) {portals.push_back(p);} // Note: p.normal is not set
			break;
		}
	default:
		assert(0); // other types are not supported yet (cylinder, sphere)
	}
}


void get_all_connected(unsigned cobj, vector<unsigned> &out) {
	get_intersecting_cobjs_tree(coll_objects.get_cobj(cobj), out, cobj, TOLERANCE, 0, 1, cobj);
}


void check_cobjs_anchored(vector<unsigned> to_check, set<unsigned> anchored[2]) {

	vector<unsigned> out;

	for (vector<unsigned>::const_iterator j = to_check.begin(); j != to_check.end(); ++j) {
		if (anchored[0].find(*j) != anchored[0].end()) continue; // already known to be unanchored
		if (anchored[1].find(*j) != anchored[1].end()) continue; // already known to be anchored

		if (coll_objects[*j].is_anchored()) {
			anchored[1].insert(*j);
			continue;
		}

		// perform a graph search until we find an anchored cobj or we run out of cobjs
		bool is_anchored(0);
		vector<unsigned> open, closed;
		open.push_back(*j);
		++cobj_counter;
		assert(coll_objects[*j].counter != cobj_counter);
		coll_objects[*j].counter = cobj_counter;

		while (!open.empty()) {
			unsigned const cur(open.back());
			open.pop_back();
			closed.push_back(cur);
			//assert(anchored[0].find(cur) == anchored[0].end()); // requires that intersects_cobj() be symmetric
			out.resize(0);
			get_all_connected(cur, out);

			for (vector<unsigned>::const_iterator i = out.begin(); i != out.end(); ++i) {
				assert(*i >= 0 && *i != cur);
				//assert(coll_objects[*i].counter != cobj_counter); // may be too strong - we might want to allow duplicates and just continue here
				if (coll_objects[*i].counter == cobj_counter) continue; // not sure we can actually get here
				open.push_back(*i); // need to do this first

				if (anchored[1].find(*i) != anchored[1].end() || coll_objects[*i].is_anchored()) {
					is_anchored = 1;
					break;
				}
				coll_objects[*i].counter = cobj_counter;
			}
			if (is_anchored) break;
		}
		// everything in the closed set has the same is_anchored state and can be cached
		copy(closed.begin(), closed.end(), inserter(anchored[is_anchored], anchored[is_anchored].begin()));
		
		if (is_anchored) { // all open is anchored as well
			copy(open.begin(), open.end(), inserter(anchored[is_anchored], anchored[is_anchored].begin()));
		}
		else {
			assert(open.empty());
		}
	} // for j
}


void add_to_falling_cobjs(set<unsigned> const &ids) {

	for (set<unsigned>::const_iterator i = ids.begin(); i != ids.end(); ++i) {
		coll_obj &cobj(coll_objects.get_cobj(*i));
		if (cobj.is_movable()) {register_moving_cobj(*i); continue;} // move instead of fall
		cobj.falling = 1;
		falling_cobjs.push_back(*i);
	}
}


void invalidate_static_cobjs() {build_cobj_tree(0, 0);}


// Note: should be named partially_destroy_cube_area() or something like that
unsigned subtract_cube(vector<color_tid_vol> &cts, vector3d &cdir, csg_cube const &cube_in, int min_destroy) {

	if (destroy_thresh >= EXPLODEABLE) return 0;
	if (cube_in.is_zero_area())        return 0;
	//RESET_TIME;
	csg_cube cube(cube_in); // allow cube to be modified
	coll_obj_group &cobjs(coll_objects); // so we don't have to rename everything and can keep the shorter code
	point center(cube.get_cube_center());
	float const clip_cube_volume(cube.get_volume());
	vector<int> just_added, to_remove;
	coll_obj_group new_cobjs;
	cdir = zero_vector;
	vector<cube_t> mod_cubes;
	mod_cubes.push_back(cube);
	vector<unsigned> int_cobjs;
	get_intersecting_cobjs_tree(cube, int_cobjs, -1, 0.0, 0, 0, -1); // can return duplicate cobjs
	set<unsigned> unique_cobjs, cgroups_added;
	copy(int_cobjs.begin(), int_cobjs.end(), inserter(unique_cobjs, unique_cobjs.begin())); // unique the cobjs
	set<unsigned> seen_cobjs(unique_cobjs);

	while (!unique_cobjs.empty()) {
		set<unsigned> next_cobjs;

		// determine affected cobjs
		for (auto k = unique_cobjs.begin(); k != unique_cobjs.end(); ++k) {
			unsigned const i(*k);
			coll_obj &cobj(cobjs.get_cobj(i));
			assert(cobj.status == COLL_STATIC);
			// can't destroy a model3d or voxel terrain cobj because the geometry is also stored in a vbo and won't get updated here
			if (cobj.cp.cobj_type != COBJ_TYPE_STD) continue;
			int const D(cobj.destroy);
			if (D <= max(destroy_thresh, (min_destroy-1))) continue;
			if (!cobj.intersects(cube)) continue; // no intersection
			bool const is_cube(cobj.type == COLL_CUBE), is_polygon(cobj.type == COLL_POLYGON);
			bool const shatter(D >= SHATTERABLE), full_destroy(shatter || cobj.is_movable()); // Note: shatter includes explode here
			csg_cube const cube2(cobj, 1);
			//if (is_cube && !cube2.contains_pt(cube.get_cube_center())) {} // check for non-destroyable cobj between center and cube2?
			float volume(cobj.volume);
			float const min_volume(0.01*min(volume, clip_cube_volume)), int_volume(cube2.get_overlap_volume(cube));

			if (is_cube && !shatter && int_volume < min_volume) { // don't remove tiny bits from cobjs
				cube.unset_intersecting_edge_flags(cobj);
				continue;
			}
			if (full_destroy || cobj.subtract_from_cobj(new_cobjs, cube, 1)) {
				bool no_new_cobjs(full_destroy || volume < TOLERANCE);
				if (no_new_cobjs) {new_cobjs.clear();} // completely destroyed
				if (is_cube)      {cdir += cube2.closest_side_dir(center);} // inexact
				if (D == SHATTER_TO_PORTAL) {cobj.create_portal();}
				
				// Note: cobj reference may be invalidated beyond this point
				for (unsigned j = 0; j < new_cobjs.size(); ++j) { // new cobjs
					new_cobjs[j].set_reflective_flag(0); // the parts are not reflective
					int const index(new_cobjs[j].add_coll_cobj()); // not sorted by alpha
					assert(index >= 0 && (size_t)index < cobjs.size());
					just_added.push_back(index);
					volume -= cobjs[index].volume;
				}
				if (is_polygon) {volume = max(0.0f, volume);} // FIXME: remove this when polygon splitting is correct
				assert(volume >= -TOLERANCE); // usually > 0.0
				int const cgid(cobj.cgroup_id);

				// Note: all cobjs in this group should have the same destroy thresh if any are shatterable or explodeable
				if (cgid >= 0 && full_destroy && cgroups_added.insert(cgid).second) { // newly inserted nonzero group
					cobj_id_set_t const &group(cobj_groups.get_set(cgid));

					for (auto c = group.begin(); c != group.end(); ++c) { // destroy all cobjs in the group
						if (!seen_cobjs.insert(*c).second) continue; // already processed
						next_cobjs.insert(*c); // add to the next wave
					}
				}
				cts.push_back(color_tid_vol(cobjs[i], volume, cobjs[i].calc_min_dim(), 0));
				cobjs[i].clear_internal_data();
				to_remove.push_back(i);
				if (full_destroy) {mod_cubes.push_back(cobjs[i]);}
				int const gid(cobjs[i].group_id);

				if (gid >= 0) { // we only check in the remove case because we can't add without removing
					assert((unsigned)gid < obj_draw_groups.size());
					// free vbo and disable vbos for this group permanently because it's too difficult to keep cobjs sorted by group
					obj_draw_groups[gid].free_vbo();
					obj_draw_groups[gid].set_vbo_enable(0);
				}
			}
			new_cobjs.clear();
		} // for k
		for (auto c = next_cobjs.begin(); c != next_cobjs.end(); ++c) {cube.union_with_cube(cobjs.get_cobj(*c));} // ensure the cube fully contains each new cobj
		unique_cobjs.swap(next_cobjs); // process next wave
	} // end while()

	// remove destroyed cobjs
	for (vector<int>::const_iterator i = to_remove.begin(); i != to_remove.end(); ++i) {
		if (!cobjs[*i].no_shadow_map()) {scene_smap_vbo_invalid = 2;} // full rebuild of shadowers
		cobjs[*i].remove_waypoint();
		remove_coll_object(*i); // remove old collision object
	}
	if (!to_remove.empty()) {invalidate_static_cobjs();} // after destroyed cobj removal

	// add new waypoints (after build_cobj_tree and end_batch)
	for (vector<int>::const_iterator i = just_added.begin(); i != just_added.end(); ++i) {
		cobjs[*i].add_connect_waypoint(); // slow
	}

	// process unanchored cobjs
	if (LET_COBJS_FALL || REMOVE_UNANCHORED) {
		//RESET_TIME;
		set<unsigned> anchored[2]; // {unanchored, anchored}

		for (unsigned i = 0; i < to_remove.size(); ++i) { // cobjs in to_remove are freed but still valid
			vector<unsigned> start;
			++cobj_counter;
			assert(coll_objects[to_remove[i]].counter != cobj_counter);
			coll_objects[to_remove[i]].counter = cobj_counter;
			get_all_connected(to_remove[i], start);
			check_cobjs_anchored(start, anchored);
		}
#if 0
		// additional optional error check that no cobj is both anchored and unanchored - can fail for polygons due to inexact intersection test
		for (auto i = anchored[0].begin(); i != anchored[0].end(); ++i) {
			assert(anchored[1].find(*i) == anchored[1].end());
		}
#endif
		if (REMOVE_UNANCHORED) {
			for (auto i = anchored[0].begin(); i != anchored[0].end(); ++i) {
				coll_obj &cobj(coll_objects.get_cobj(*i));
				if (cobj.is_movable()) {register_moving_cobj(*i); continue;} // move/fall instead of destroy
				if (cobj.destroy <= max(destroy_thresh, (min_destroy-1))) continue; // can't destroy (can't get here?)
				cts.push_back(color_tid_vol(cobj, cobj.volume, cobj.calc_min_dim(), 1));
				cobj.clear_internal_data();
				mod_cubes.push_back(cobj);
				cobj.remove_waypoint();
				remove_coll_object(*i);
				to_remove.push_back(*i);
			}
		}
		else if (LET_COBJS_FALL) {
			add_to_falling_cobjs(anchored[0]);
		}
		//PRINT_TIME("Check Anchored");
	}
	if (!to_remove.empty()) {cdir.normalize();}
	//PRINT_TIME("Subtract Cube");
	return (unsigned)to_remove.size();
}


void check_falling_cobjs() {

	if (falling_cobjs.empty()) return; // nothing to do
	//RESET_TIME;
	float const accel(-0.5*base_gravity*GRAVITY*tstep); // half gravity
	set<unsigned> anchored[2]; // {unanchored, anchored}

	for (unsigned i = 0; i < falling_cobjs.size(); ++i) {
		unsigned const ix(falling_cobjs[i]);
	
		if (coll_objects.get_cobj(ix).status != COLL_STATIC) { // disable
			falling_cobjs[i] = falling_cobjs.back();
			falling_cobjs.pop_back();
			--i; // wraparound is ok
			continue;
		}
		// translate, add the new, then remove the old
		coll_objects.get_cobj(ix).clear_internal_data();
		coll_obj cobj(coll_objects[ix]); // make a copy
		cobj.v_fall += accel; // terminal velocity?
		cobj.shift_by(point(0.0, 0.0, tstep*cobj.v_fall), 1); // translate down
		int const index(cobj.add_coll_cobj());
		remove_coll_object(ix);
		assert((int)ix != index);
		falling_cobjs[i] = index;
	}
	vector<unsigned> last_falling(falling_cobjs);
	sort(last_falling.begin(), last_falling.end());
	check_cobjs_anchored(falling_cobjs, anchored);
	falling_cobjs.resize(0);
	add_to_falling_cobjs(anchored[0]);
	
	if (falling_cobjs != last_falling) {
		invalidate_static_cobjs();
		scene_smap_vbo_invalid = 2; // full rebuild of shadowers
	}
	//PRINT_TIME("Check Falling Cobjs");
}


// **************** Cobj Connectivity Code ****************


bool is_pt_under_mesh(point const &p) {

	//return is_under_mesh(p); // too slow?
	int const xpos(get_xpos(p.x)), ypos(get_ypos(p.y));
	if (point_outside_mesh(xpos, ypos)) return 0;
	return (p.z < mesh_height[ypos][xpos]);
}


int coll_obj::is_anchored() const {

	if (platform_id >= 0 || status != COLL_STATIC) return 0; // platforms and dynamic objects are never connecting
	if (is_movable())                              return 0; // movable cobjs aren't anchored
	if (destroy <= destroy_thresh)                 return 2; // can't be destroyed, so it never moves
	if (d[2][0] <= min(zbottom, czmin))            return 1; // below the scene
	if (d[2][0] > ztop)                            return 0; // above the mesh

	switch (type) {
	case COLL_CUBE: {
		int const x1(get_xpos(d[0][0])), x2(get_xpos(d[0][1]));
		int const y1(get_ypos(d[1][0])), y2(get_ypos(d[1][1]));
			
		for (int y = max(0, y1); y <= min(MESH_Y_SIZE-1, y2); ++y) {
			for (int x = max(0, x1); x <= min(MESH_X_SIZE-1, x2); ++x) {
				if (d[2][0] < mesh_height[y][x]) return 1;
			}
		}
		return 0;
	}
	case COLL_SPHERE:
		return is_pt_under_mesh((points[0] - vector3d(0.0, 0.0, radius)));
	case COLL_CYLINDER: // should really test the entire top/bottom surface
	case COLL_CYLINDER_ROT:
	case COLL_TORUS:
	case COLL_CAPSULE:
		return is_pt_under_mesh(points[0]) || is_pt_under_mesh(points[1]);
	case COLL_POLYGON: // should really test the entire surface(s)
		for (int i = 0; i < npoints; ++i) {
			if (is_pt_under_mesh(points[i])) return 1;

			if (thickness > MIN_POLY_THICK) {
				if (is_pt_under_mesh(points[i] + norm*(0.5*thickness))) return 1;
				if (is_pt_under_mesh(points[i] - norm*(0.5*thickness))) return 1;
			}
		}
		return 0;
	default: assert(0);
	}
	return 0;
}


void fire_damage_cobjs(int xpos, int ypos) {

	if (point_outside_mesh(xpos, ypos)) return;
	vector<int> const &cvals(v_collision_matrix[ypos][xpos].cvals);
	if (cvals.empty()) return;
	point const pos(get_xval(xpos), get_yval(ypos), mesh_height[ypos][xpos]);

	for (vector<int>::const_iterator i = cvals.begin(); i != cvals.end(); ++i) {
		if (*i < 0) continue;
		coll_obj &cobj(coll_objects.get_cobj(*i));
		if (cobj.destroy < EXPLODEABLE) continue;
		if (!cobj.sphere_intersects(pos, HALF_DXY)) continue;
		destroy_coll_objs(pos, 1000.0, NO_SOURCE, FIRE, HALF_DXY);
		break; // done
	} // for i
}

