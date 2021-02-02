// 3D World - Building Interior Generation
// by Frank Gennari 11/15/19

#include "3DWorld.h"
#include "function_registry.h"
#include "buildings.h"

cube_t grass_exclude1, grass_exclude2;

extern bool draw_building_interiors;
extern int player_in_closet;
extern float grass_width, CAMERA_RADIUS;
extern double camera_zh;
extern building_params_t global_building_params;
extern bldg_obj_type_t bldg_obj_types[];


float get_railing_height(room_object_t const &c);
cylinder_3dw get_railing_cylinder(room_object_t const &c);

/*static*/ float building_t::get_scaled_player_radius() {return CAMERA_RADIUS*global_building_params.player_coll_radius_scale;}

void building_t::set_z_range(float z1, float z2) {
	bcube.z1() = z1; bcube.z2() = z2;
	adjust_part_zvals_for_floor_spacing(bcube);
	if (!parts.empty()) {parts[0].z1() = z1; parts[0].z2() = z2;}
}
building_mat_t const &building_t::get_material() const {return global_building_params.get_material(mat_ix);}

void building_t::gen_rotation(rand_gen_t &rgen) {

	float const max_rot_angle(get_material().max_rot_angle);
	if (max_rot_angle == 0.0) return;
	float const rot_angle(rgen.rand_uniform(0.0, max_rot_angle));
	rot_sin = sin(rot_angle);
	rot_cos = cos(rot_angle);
	parts.clear();
	parts.push_back(bcube); // this is the actual building base
	cube_t const &bc(parts.back());
	point const center(bc.get_cube_center());

	for (unsigned i = 0; i < 4; ++i) {
		point corner(bc.d[0][i&1], bc.d[1][i>>1], bc.d[2][i&1]);
		do_xy_rotate(center, corner);
		if (i == 0) {bcube.set_from_point(corner);} else {bcube.union_with_pt(corner);} // Note: detail cubes are excluded
	}
}

bool building_t::check_bcube_overlap_xy(building_t const &b, float expand_rel, float expand_abs, vector<point> &points) const {

	if (expand_rel == 0.0 && expand_abs == 0.0 && !bcube.intersects(b.bcube)) return 0;
	if (!is_rotated() && !b.is_rotated()) return 1; // above check is exact, top-level bcube check up to the caller
	if (b.bcube.contains_pt_xy(bcube.get_cube_center()) || bcube.contains_pt_xy(b.bcube.get_cube_center())) return 1; // slightly faster to include this check
	return (check_bcube_overlap_xy_one_dir(b, expand_rel, expand_abs, points) || b.check_bcube_overlap_xy_one_dir(*this, expand_rel, expand_abs, points));
}

// Note: only checks for point (x,y) value contained in one cube/N-gon/cylinder; assumes pt has already been rotated into local coordinate frame
bool building_t::check_part_contains_pt_xy(cube_t const &part, point const &pt, vector<point> &points) const {

	if (!part.contains_pt_xy(pt)) return 0; // check bounding cube
	if (is_simple_cube()) return 1; // that's it
	building_draw_utils::calc_poly_pts(*this, part, points);
	return point_in_polygon_2d(pt.x, pt.y, points.data(), points.size(), 0, 1); // 2D x/y containment
}

bool building_t::check_bcube_overlap_xy_one_dir(building_t const &b, float expand_rel, float expand_abs, vector<point> &points) const { // can be called before levels/splits are created

	// Note: easy cases are handled by check_bcube_overlap_xy() above
	point const center1(b.bcube.get_cube_center()), center2(bcube.get_cube_center());

	for (auto p1 = b.parts.begin(); p1 != b.parts.end(); ++p1) {
		point pts[9]; // {center, 00, 10, 01, 11, x0, x1, y0, y1}

		if (b.parts.size() == 1) {pts[0] = center1;} // single cube: we know we're rotating about its center
		else {
			pts[0] = p1->get_cube_center();
			b.do_xy_rotate(center1, pts[0]); // rotate into global space
		}
		cube_t c_exp(*p1);
		c_exp.expand_by_xy(expand_rel*p1->get_size() + vector3d(expand_abs, expand_abs, expand_abs));

		for (unsigned i = 0; i < 4; ++i) { // {00, 10, 01, 11}
			pts[i+1].assign(c_exp.d[0][i&1], c_exp.d[1][i>>1], 0.0); // XY only
			b.do_xy_rotate(center1, pts[i+1]); // rotate into global space
		}
		for (unsigned i = 0; i < 5; ++i) {do_xy_rotate_inv(center2, pts[i]);} // inverse rotate into local coord space - negate the sine term
		cube_t c_exp_rot(pts+1, 4); // use points 1-4
		pts[5] = 0.5*(pts[1] + pts[3]); // x0 edge center
		pts[6] = 0.5*(pts[2] + pts[4]); // x1 edge center
		pts[7] = 0.5*(pts[1] + pts[2]); // y0 edge center
		pts[8] = 0.5*(pts[3] + pts[4]); // y1 edge center

		for (auto p2 = parts.begin(); p2 != parts.end(); ++p2) {
			if (c_exp_rot.contains_pt_xy(p2->get_cube_center())) return 1; // quick and easy test for heavy overlap

			for (unsigned i = 0; i < 9; ++i) {
				if (check_part_contains_pt_xy(*p2, pts[i], points)) return 1; // Note: building geometry is likely not yet generated, this check should be sufficient
				//if (p2->contains_pt_xy(pts[i])) return 1;
			}
		}
	} // for p1
	return 0;
}

bool building_t::test_coll_with_sides(point &pos, point const &p_last, float radius, cube_t const &part, vector<point> &points, vector3d *cnorm) const {

	building_draw_utils::calc_poly_pts(*this, part, points); // without the expand
	point quad_pts[4]; // quads
	bool updated(0);

	// FIXME: if the player is moving too quickly, the intersection with a side polygon may be missed,
	// which allows the player to travel through the building, but using a line intersection test from p_past2 to pos has other problems
	for (unsigned S = 0; S < num_sides; ++S) { // generate vertex data quads
		for (unsigned d = 0, ix = 0; d < 2; ++d) {
			point const &p(points[(S+d)%num_sides]);
			for (unsigned e = 0; e < 2; ++e) {quad_pts[ix++].assign(p.x, p.y, part.d[2][d^e]);}
		}
		vector3d const normal(get_poly_norm(quad_pts));
		float const rdist(dot_product_ptv(normal, pos, quad_pts[0]));
		if (rdist < 0.0 || rdist >= radius) continue; // too far or wrong side
		if (!sphere_poly_intersect(quad_pts, 4, pos, normal, rdist, radius)) continue;
		pos += normal*(radius - rdist);
		if (cnorm) {*cnorm = normal;}
		updated = 1;
	} // for S
	if (updated) return 1;

	if (max(pos.z, p_last.z) > part.z2() && point_in_polygon_2d(pos.x, pos.y, points.data(), num_sides, 0, 1)) { // test top plane (sphere on top of polygon?)
		pos.z = part.z2() + radius; // make sure it doesn't intersect the roof
		if (cnorm) {*cnorm = plus_z;}
		return 1;
	}
	return 0;
}

bool building_t::check_sphere_coll(point &pos, point const &p_last, vect_cube_t const &ped_bcubes, vector3d const &xlate,
	float radius, bool xy_only, vector<point> &points, vector3d *cnorm_ptr, bool check_interior) const
{
	if (!is_valid()) return 0; // invalid building
	if (radius > 0.0 && !(xy_only ? sphere_cube_intersect_xy((pos - xlate), radius, bcube) : sphere_cube_intersect((pos - xlate), radius, bcube))) return 0;
	point pos2(pos), p_last2(p_last), center;
	bool had_coll(0), is_interior(0);
	float part_z2(bcube.z2());

	if (is_rotated()) {
		center = bcube.get_cube_center() + xlate;
		do_xy_rotate_inv(center, pos2); // inverse rotate - negate the sine term
		do_xy_rotate_inv(center, p_last2);
	}
	if (check_interior && draw_building_interiors && interior != nullptr) { // check for interior case first
		float const zval(max(pos2.z, p_last2.z) - xlate.z); // this is the real zval for use in collsion detection, in building space
		point const pos2_bs(pos2 - xlate);
		cube_t sc; sc.set_from_sphere(pos2_bs, radius); // sphere bounding cube

		if ((zval + radius) > ground_floor_z1 && zval < (ground_floor_z1 + get_door_height())) { // on the ground floor
			for (auto d = doors.begin(); d != doors.end(); ++d) {
				if (d->type == tquad_with_ix_t::TYPE_RDOOR) continue; // doesn't apply to roof door
				cube_t bc(d->get_bcube());
				bool const door_dim(bc.dy() < bc.dx());
				bc.expand_in_dim(door_dim, 1.1*radius); // expand by radius plus some tolerance in door dim
				if (bc.contains_pt(pos2_bs)) return 0; // check if we can use a door - disable collsion detection to allow the player to walk through
			}
		}
		for (auto i = parts.begin(); i != get_real_parts_end_inc_sec(); ++i) { // include garages and sheds
			cube_t c(*i + xlate);
			if (!c.contains_pt(point(pos2.x, pos2.y, zval))) continue; // not interior to this part
			float cont_area(0.0);

			for (auto p = parts.begin(); p != get_real_parts_end(); ++p) {
				if (zval < p->z1() || zval > p->z2()) continue; // wrong floor/part in stack
				if (p->intersects_xy(sc)) {cont_area += (min(p->x2(), sc.x2()) - max(p->x1(), sc.x1()))*(min(p->y2(), sc.y2()) - max(p->y1(), sc.y1()));} // accumulate shared XY area
			}
			if (cont_area < 0.99*sc.get_area_xy()) { // sphere bounding cube not contained in union of parts - sphere is partially outside the building
				c.expand_by_xy(-radius); // shrink part by sphere radius
				c.clamp_pt_xy(pos2); // force pos2 into interior of the cube to prevent the sphere from intersecting the part
				had_coll = 1;
			}
			is_interior = 1;
			break; // flag for interior collision detection
		} // for i
		if (!is_interior) { // not interior to a part - check roof access
			float const floor_thickness(get_floor_thickness());

			for (auto i = interior->stairwells.begin(); i != interior->stairwells.end(); ++i) {
				if (!i->roof_access) continue;
				cube_t test_cube(*i);
				test_cube.expand_by_xy(-0.5f*radius);
				if (!test_cube.contains_pt_xy(pos2 - xlate)) continue; // pos not over stairs
				if (zval < i->z2() + radius + floor_thickness) {is_interior = 1; break;} // Note: don't have to check zval > i->z2() because we know that !is_interior
			}
		}
	}
	if (is_interior) {
		point pos2_bs(pos2 - xlate);
		if (check_sphere_coll_interior(pos2_bs, (p_last - xlate), ped_bcubes, radius, xy_only, cnorm_ptr)) {pos2 = pos2_bs + xlate; had_coll = 1;}
	}
	else {
		for (auto i = parts.begin(); i != parts.end(); ++i) {
			if (xy_only && i->z1() > ground_floor_z1) { // only need to check first level in this mode
				if (has_complex_floorplan) {continue;} else {break;}
			}
			if (!xy_only && ((pos2.z + radius < i->z1() + xlate.z) || (pos2.z - radius > i->z2() + xlate.z))) continue; // test z overlap
			if (radius == 0.0 && !(xy_only ? i->contains_pt_xy(pos2) : i->contains_pt(pos2))) continue; // no intersection; ignores p_last
			cube_t const part_bc(*i + xlate);
			bool part_coll(0);

			if (use_cylinder_coll()) {
				point const cc(part_bc.get_cube_center());
				float const crx(0.5*i->dx()), cry(0.5*i->dy()), r_sum(radius + max(crx, cry));
				if (!dist_xy_less_than(pos2, cc, r_sum)) continue; // no intersection

				if (fabs(crx - cry) < radius) { // close to a circle
					if (p_last2.z > part_bc.z2() && dist_xy_less_than(pos2, cc, max(crx, cry))) {
						pos2.z = part_bc.z2() + radius; // make sure it doesn't intersect the roof
						if (cnorm_ptr) {*cnorm_ptr = plus_z;}
					}
					else { // side coll
						vector2d const d((pos2.x - cc.x), (pos2.y - cc.y));
						float const mult(r_sum/d.mag());
						pos2.x = cc.x + mult*d.x;
						pos2.y = cc.y + mult*d.y;
						if (cnorm_ptr) {*cnorm_ptr = vector3d(d.x, d.y, 0.0).get_norm();} // no z-component
					}
					part_coll = 1;
				}
				else {
					part_coll |= test_coll_with_sides(pos2, p_last2, radius, part_bc, points, cnorm_ptr); // use polygon collision test
				}
			}
			else if (num_sides != 4) { // triangle, hexagon, octagon, etc.
				part_coll |= test_coll_with_sides(pos2, p_last2, radius, part_bc, points, cnorm_ptr);
			}
			else if (!xy_only && part_bc.contains_pt_xy_exp(pos2, radius) && p_last2.z > (i->z2() + xlate.z)) { // on top of building
				pos2.z = i->z2() + xlate.z + radius;
				if (cnorm_ptr) {*cnorm_ptr = plus_z;}
				part_coll = 1;
			}
			else if (sphere_cube_int_update_pos(pos2, radius, part_bc, p_last2, 1, xy_only, cnorm_ptr)) { // cube
				part_coll = 1; // flag as colliding, continue to look for more collisions (inside corners)
			}
			if (part_coll && pos2.z < part_bc.z1()) {pos2.z = part_bc.z2() + radius;} // can't be under a building - make it on top of the building instead
			if (part_coll) {part_z2 = i->z2();}
			had_coll |= part_coll;
		} // for i
		for (auto i = fences.begin(); i != fences.end(); ++i) {
			had_coll |= sphere_cube_int_update_pos(pos2, radius, *i, p_last, 1, xy_only, cnorm_ptr);
		}
		if (!driveway.is_all_zeros()) { // include driveways, though this may not be necessary and not correct since driveway is outside the bcube
			had_coll |= sphere_cube_int_update_pos(pos2, radius, driveway, p_last, 1, xy_only, cnorm_ptr);
		}
		if (!xy_only) { // don't need to check details and roof in xy_only mode because they're contained in the XY footprint of the parts
			for (auto i = details.begin(); i != details.end(); ++i) {
				if (sphere_cube_int_update_pos(pos2, radius, (*i + xlate), p_last2, 1, xy_only, cnorm_ptr)) {had_coll = 1;} // cube, flag as colliding
			}
			for (auto i = roof_tquads.begin(); i != roof_tquads.end(); ++i) {
				point const pos_xlate(pos2 - xlate);

				if (check_interior && had_coll && pos2.z - xlate.z > part_z2) { // player standing on top of a building with a sloped roof or roof access cover
					if (point_in_polygon_2d(pos_xlate.x, pos_xlate.y, i->pts, i->npts, 0, 1)) {
						vector3d const normal(i->get_norm());
						if (normal.z == 0.0) continue; // skip vertical sides as the player can't stand on them
						float const rdist(dot_product_ptv(normal, pos_xlate, i->pts[0]));

						if (draw_building_interiors && i->type == tquad_with_ix_t::TYPE_ROOF_ACC) { // don't allow walking on roof access tquads
							if (rdist < -radius*normal.z) continue; // player is below this tquad
							else {pos2.x = p_last.x; pos2.y = p_last.y; break;} // block the player from walking here (can only walk through raised opening)
						}
						pos2.z += (radius - rdist)/normal.z; // determine the distance we need to move vertically to achieve this diag separation
					}
				}
				else { // normal case for bouncing object, etc.
					vector3d const normal(i->get_norm());
					float const rdist(dot_product_ptv(normal, pos_xlate, i->pts[0]));

					if (fabs(rdist) < radius && sphere_poly_intersect(i->pts, i->npts, pos_xlate, normal, rdist, radius)) {
						pos2 += normal*(radius - rdist); // update current pos
						if (cnorm_ptr) {*cnorm_ptr = ((normal.z < 0.0) ? -1.0 : 1.0)*normal;} // make sure normal points up
						had_coll = 1; // flag as colliding
						break; // only use first colliding tquad
					}
				}
			} // for i
		}
	} // end !is_interior case
	if (!had_coll) return 0; // Note: no collisions with windows or doors, since they're colinear with walls

	if (is_rotated()) {
		do_xy_rotate(center, pos2); // rotate back around center
		if (cnorm_ptr) {do_xy_rotate_normal(*cnorm_ptr);} // rotate normal back
	}
	pos = pos2;
	return 1;
}

// Note: pos and p_last are already in rotated coordinate space
// default player is actually too large to fit through doors and too tall to fit between the floor and celing, so player size/height must be reduced in the config file
bool building_t::check_sphere_coll_interior(point &pos, point const &p_last, vect_cube_t const &ped_bcubes, float radius, bool xy_only, vector3d *cnorm) const {
	pos.z = bcube.z1(); // start at building z1 rather than the terrain height in case we're at the foot of a steep hill
	assert(interior);
	float const floor_spacing(get_window_vspace()), floor_thickness(get_floor_thickness());
	bool had_coll(0), on_stairs(0);
	float obj_z(max(pos.z, p_last.z)); // use p_last to get orig zval
	float const wall_test_z(obj_z + radius); // hack to allow player to step over a wall that's below the stairs connecting stacked parts
	float const xy_radius(radius*global_building_params.player_coll_radius_scale); // XY radius can be smaller to allow player to fit between furniture

	// Note: pos.z may be too small here and we should really use obj_z, so skip_z must be set to 1 in cube tests and obj_z tested explicitly instead
	for (unsigned d = 0; d < 2; ++d) { // check XY collision with walls
		for (auto i = interior->walls[d].begin(); i != interior->walls[d].end(); ++i) {
			if (wall_test_z < i->z1() || wall_test_z > i->z2()) continue; // wrong part/floor
			had_coll |= sphere_cube_int_update_pos(pos, xy_radius, *i, p_last, 1, 1, cnorm); // skip_z=1 (handled by zval test above)
		}
	}
	for (auto e = interior->elevators.begin(); e != interior->elevators.end(); ++e) {
		if (obj_z < e->z1() || obj_z > e->z2()) continue; // wrong part/floor

		if (1/*obj_z < e->z1() + floor_spacing*/) { // should players only be allowed in elevators on the ground floor?
			cube_t cubes[5];
			unsigned const num_cubes(e->get_coll_cubes(cubes));
			for (unsigned n = 0; n < num_cubes; ++n) {had_coll |= sphere_cube_int_update_pos(pos, xy_radius, cubes[n], p_last, 1, 1, cnorm);} // skip_z=1
		}
		else {had_coll |= sphere_cube_int_update_pos(pos, xy_radius, *e, p_last, 1, 1, cnorm);} // skip_z=1
	}
	for (auto i = interior->doors.begin(); i != interior->doors.end(); ++i) {
		if (i->open) continue; // doors tend to block the player, don't collide with them unless they're closed
		if (obj_z < i->z1() || obj_z > i->z2()) continue; // wrong part/floor
		had_coll |= sphere_cube_int_update_pos(pos, xy_radius, *i, p_last, 1, 0, cnorm); // skip_z=0
	}
	if (!xy_only && 2.2f*radius < (floor_spacing - floor_thickness)) { // diameter is smaller than space between floor and ceiling
		// check Z collision with floors; no need to check ceilings; this will set pos.z correctly so that we can set skip_z=0 in later tests
		obj_z = max(pos.z, p_last.z);
		float const floor_test_zval(obj_z + floor_thickness); // move up by floor thickness to better handle steep stairs

		for (auto i = interior->floors.begin(); i != interior->floors.end(); ++i) {
			if (!i->contains_pt_xy(pos)) continue; // sphere not in this part/cube
			float const z1(i->z1());
			if (floor_test_zval < z1 || floor_test_zval > z1 + floor_spacing) continue; // this is not the floor the sphere is on
			if (pos.z < z1 + radius) {pos.z = z1 + radius; had_coll = 1;} // move up
			break; // only change zval once
		}
	}
	if (interior->room_geom) { // collision with room geometry
		vector<room_object_t> const &objs(interior->room_geom->objs);
		obj_z = max(pos.z, p_last.z);

		for (auto c = (objs.begin() + interior->room_geom->stairs_start); c != objs.end(); ++c) { // check for and handle stairs first
			if (c->no_coll() || c->type != TYPE_STAIR) continue;
			if (!c->contains_pt_xy(pos))  continue; // sphere not on this stair
			if (obj_z < c->z1())          continue; // below the stair
			if (pos.z - radius > c->z1()) continue; // above the stair
			pos.z = c->z1() + radius; // stand on the stair - this can happen for multiple stairs
			obj_z = max(pos.z, p_last.z);
			bool const is_u(c->shape == SHAPE_STAIRS_U);
			if (!is_u || c->dir == 1) {max_eq(pos[!c->dim], (c->d[!c->dim][0] + xy_radius));} // force the sphere onto the stairs
			if (!is_u || c->dir == 0) {min_eq(pos[!c->dim], (c->d[!c->dim][1] - xy_radius));}
			had_coll = on_stairs = 1;
		} // for c
		for (auto c = objs.begin(); c != objs.end(); ++c) { // check for other objects to collide with (including stairs)
			if (c->no_coll() || !bldg_obj_types[c->type].player_coll) continue;

			if (c->type == TYPE_ELEVATOR) { // special handling for elevators
				if (!c->contains_pt_xy(pos)) continue;
				if      (obj_z >= c->z2()) {max_eq(pos.z, (c->z2() + radius)); had_coll = 1;} // standing on the roof of the elevator
				else if (obj_z >= c->z1()) {max_eq(pos.z, (c->z1() + radius)); had_coll = 1;} // inside the elevator
				continue;
			}
			if ((c->type == TYPE_STAIR || on_stairs) && (obj_z + radius) > c->z2()) continue; // above the stair - allow it to be walked on

			if (c->type == TYPE_RAILING) { // only collide with railing at top of stairs, not when walking under stairs
				if (!sphere_cube_intersect(pos, xy_radius, *c)) continue; // optimization
				cylinder_3dw const railing(get_railing_cylinder(*c));
				float const t((pos[c->dim] - railing.p1[c->dim])/(railing.p2[c->dim] - railing.p1[c->dim]));
				float const railing_zval(railing.p1.z + CLIP_TO_01(t)*(railing.p2.z - railing.p1.z));
				if ((railing_zval - get_railing_height(*c)) > float(pos.z + camera_zh) || railing_zval < (pos.z - radius)) continue; // no Z collision
			}
			if (c->shape == SHAPE_CYLIN) { // vertical cylinder
				float const radius(0.25f*(c->dx() + c->dy())); // average of x/y diameter
				point const center(c->get_cube_center());
				cylinder_3dw const cylin(point(center.x, center.y, c->z1()), point(center.x, center.y, (c->z2() + radius)), radius, radius); // extend upward by radius
				had_coll |= sphere_vert_cylin_intersect(pos, xy_radius, cylin, cnorm);
			}
			else { // assume it's a cube
				cube_t c_extended(*c);
				c_extended.z1() -= camera_zh; // handle the player's head (for stairs), or is pos already at this height?

				if (c->type == TYPE_CLOSET) { // special case to handle closet interiors
					if (!sphere_cube_intersect(pos, xy_radius, c_extended)) continue; // not intersection with closet bounding cube (optimization)
					cube_t cubes[5];
					get_closet_cubes(*c, cubes); // get cubes for walls and door; required to handle collision with closet interior

					for (unsigned n = 0; n < (c->is_open() ? 4U : 5U); ++n) { // only check for door collision if closet door is closed
						if (!cubes[n].is_all_zeros()) {had_coll |= sphere_cube_int_update_pos(pos, xy_radius, cubes[n], p_last, 1, 0, cnorm);} // skip_z=0
					}
					if (c->contains_pt(pos)) {player_in_closet = (c->is_open() ? 1 : 2);}
				}
				else {had_coll |= sphere_cube_int_update_pos(pos, xy_radius, c_extended, p_last, 1, 0, cnorm);} // skip_z=0
			}
		} // for c
	}
	for (auto i = ped_bcubes.begin(); i != ped_bcubes.end(); ++i) {
		float const ped_radius(0.5*max(i->dx(), i->dy())); // determine radius from bcube X/Y
		point const center(i->get_cube_center());
		float const dist(p2p_dist(pos, center)), r_sum(xy_radius + 0.5*ped_radius); // ped_radius is a bit too large
		if (dist >= r_sum) continue; // no intersection
		vector3d const normal(vector3d(pos.x-center.x, pos.y-center.y, 0.0).get_norm()); // XY direction
		if (cnorm) {*cnorm = normal;}
		pos += normal*(r_sum - dist);
		had_coll = 1;
	} // for i
	return had_coll; // will generally always be true due to floors
}

unsigned building_t::check_line_coll(point const &p1, point const &p2, vector3d const &xlate, float &t, vector<point> &points,
	bool occlusion_only, bool ret_any_pt, bool no_coll_pt) const
{
	if (!check_line_clip(p1-xlate, p2-xlate, bcube.d)) return 0; // no intersection
	point p1r(p1), p2r(p2);
	float tmin(0.0), tmax(1.0);
	unsigned coll(0); // 0=none, 1=side, 2=roof, 3=details

	if (is_rotated()) {
		point const center(bcube.get_cube_center() + xlate);
		do_xy_rotate_inv(center, p1r); // inverse rotate - negate the sine term
		do_xy_rotate_inv(center, p2r);
	}
	p1r -= xlate; p2r -= xlate;
	float const pzmin(min(p1r.z, p2r.z)), pzmax(max(p1r.z, p2r.z));
	bool const vert(p1r.x == p2r.x && p1r.y == p2r.y);

	for (auto i = parts.begin(); i != parts.end(); ++i) {
		if (pzmin > i->z2() || pzmax < i->z1()) continue; // no overlap in z
		bool hit(0);

		if (use_cylinder_coll()) { // vertical cylinder
			// Note: we know the line intersects the cylinder's bcube, and there's a good chance it intersects the cylinder, so we don't need any expensive early termination cases here
			point const cc(i->get_cube_center());
			vector3d const csz(i->get_size());

			if (vert) { // vertical line + vertical cylinder optimization + handling of ellipsoids
				if (!point_in_ellipse(p1r, cc, 0.5*csz.x, 0.5*csz.y)) continue; // no intersection (below test should return true as well)
				tmin = (i->z2() - p1r.z)/(p2r.z - p1r.z);
				if (tmin >= 0.0 && tmin < t) {t = tmin; hit = 1;}
			}
			else {
				float const radius(0.5*(occlusion_only ? min(csz.x, csz.y) : max(csz.x, csz.y))); // use conservative radius unless this is an occlusion query
				point const cp1(cc.x, cc.y, i->z1()), cp2(cc.x, cc.y, i->z2());
				if (!line_int_cylinder(p1r, p2r, cp1, cp2, radius, radius, 1, tmin) || tmin > t) continue; // conservative for non-occlusion rays

				if (!occlusion_only && csz.x != csz.y) { // ellipse
					vector3d const delta(p2r - p1r);
					float const rx_inv_sq(1.0/(0.25*csz.x*csz.x)), ry_inv_sq(1.0/(0.25*csz.y*csz.y));
					float t_step(0.1*max(csz.x, csz.y)/delta.mag());

					for (unsigned n = 0; n < 10; ++n) { // use an interative approach
						if (point_in_ellipse_risq((p1r + tmin*delta), cc, rx_inv_sq, ry_inv_sq)) {hit = 1; tmin -= t_step;} else {tmin += t_step;}
						if (hit) {t_step *= 0.5;} // converge on hit point
					}
					if (!hit) continue; // not actually a hit
				} // end ellipse case
				t = tmin; hit = 1;
			}
		}
		else if (num_sides != 4) {
			building_draw_utils::calc_poly_pts(*this, *i, points);
			float const tz((i->z2() - p1r.z)/(p2r.z - p1r.z)); // t value at zval = top of cube

			if (tz >= 0.0 && tz < t) {
				float const xval(p1r.x + tz*(p2r.x - p1r.x)), yval(p1r.y + tz*(p2r.y - p1r.y));
				if (point_in_polygon_2d(xval, yval, points.data(), points.size(), 0, 1)) {t = tz; hit = 1;} // XY plane test for vertical lines and top surface
			}
			if (!vert) { // test building sides
				point quad_pts[4]; // quads

				for (unsigned S = 0; S < num_sides; ++S) { // generate vertex data quads
					for (unsigned d = 0, ix = 0; d < 2; ++d) {
						point const &p(points[(S+d)%num_sides]);
						for (unsigned e = 0; e < 2; ++e) {quad_pts[ix++].assign(p.x, p.y, i->d[2][d^e]);}
					}
					if (line_poly_intersect(p1r, p2r, quad_pts, 4, get_poly_norm(quad_pts), tmin) && tmin < t) {t = tmin; hit = 1;} // Note: untested
				} // for S
			}
		}
		else if (get_line_clip(p1r, p2r, i->d, tmin, tmax) && tmin < t) {t = tmin; hit = 1;} // cube

		if (hit) {
			if (occlusion_only) return 1; // early exit
			if (vert) {coll = 2;} // roof
			else {
				float const zval(p1r.z + t*(p2.z - p1r.z));
				coll = ((fabs(zval - i->z2()) < 0.0001*i->dz()) ? 2 : 1); // test if clipped zval is close to the roof zval
			}
			if (ret_any_pt) return coll;
		}
	} // for i
	if (occlusion_only) return 0;

	for (auto i = details.begin(); i != details.end(); ++i) {
		if (get_line_clip(p1r, p2r, i->d, tmin, tmax) && tmin < t) {t = tmin; coll = 3;} // details cube
	}
	if (!no_coll_pt || !vert) { // vert line already tested building cylins/cubes, and marked coll roof, no need to test again unless we need correct coll_pt t-val
		for (auto i = roof_tquads.begin(); i != roof_tquads.end(); ++i) {
			if (line_poly_intersect(p1r, p2r, i->pts, i->npts, i->get_norm(), tmin) && tmin < t) {t = tmin; coll = 2;} // roof quad
		}
	}
	return coll; // Note: no collisions with windows or doors, since they're colinear with walls; no collision with interior for now
}

// Note: if xy_radius == 0.0, this is a point test; otherwise, it's an approximate vertical cylinder test
bool building_t::check_point_or_cylin_contained(point const &pos, float xy_radius, vector<point> &points) const {

	if (xy_radius == 0.0 && !bcube.contains_pt(pos)) return 0; // no intersection
	point pr(pos);
	if (is_rotated()) {do_xy_rotate_inv(bcube.get_cube_center(), pr);} // inverse rotate - negate the sine term

	for (auto i = parts.begin(); i != parts.end(); ++i) {
		if (pr.z > i->z2() || pr.z < i->z1()) continue; // no overlap in z

		if (use_cylinder_coll()) { // vertical cylinder
			point const cc(i->get_cube_center());
			vector3d const csz(i->get_size());
			float const dx(cc.x - pr.x), dy(cc.y - pr.y), rx(0.5*csz.x + xy_radius), ry(0.5*csz.y + xy_radius);
			if (dx*dx/(rx*rx) + dy*dy/(ry*ry) > 1.0f) continue; // no intersection (below test should return true as well)
			return 1;
		}
		else if (num_sides != 4) {
			building_draw_utils::calc_poly_pts(*this, *i, points);

			if (xy_radius > 0.0) { // cylinder case: expand polygon by xy_radius; assumes a convex polygon
				point const center(i->get_cube_center());

				for (auto p = points.begin(); p != points.end(); ++p) {
					vector3d dir(*p - center);
					dir.z = 0.0; // only want XY component
					*p += dir*(xy_radius/dir.mag());
				}
			}
			if (point_in_polygon_2d(pr.x, pr.y, &points.front(), points.size(), 0, 1)) return 1; // XY plane test for top surface
		}
		else { // cube
			if (xy_radius > 0.0) {
				cube_t cube(*i);
				cube.expand_by(xy_radius);
				if (cube.contains_pt(pr)) return 1;
			}
			else if (i->contains_pt(pr)) return 1;
		}
	} // for i
	return 0;
}

void building_t::calc_bcube_from_parts() {
	assert(!parts.empty());
	bcube = parts[0];
	for (auto i = parts.begin()+1; i != parts.end(); ++i) {bcube.union_with_cube(*i);} // update bcube
}

void building_t::move_door_to_other_side_of_wall(tquad_with_ix_t &door, float dist_mult, bool invert_normal) const {
	cube_t const c(door.get_bcube());
	bool const dim(c.dy() < c.dx()), dir(door.get_norm()[dim] > 0.0); // closest cube side dir
	float door_shift(0.0);
	if (invert_normal) {swap(door.pts[0], door.pts[1]); swap(door.pts[2], door.pts[3]);} // swap vertex order to invert normal

	if (door.type == tquad_with_ix_t::TYPE_RDOOR) { // not on a wall, use shift relative to floor/wall thickness
		door_shift = 0.01*(dir ? 1.0 : -1.0)*get_material().get_floor_spacing();
	}
	else {
		door_shift = bcube.dz(); // start with a large value

		for (auto p = parts.begin(); p != get_real_parts_end_inc_sec(); ++p) { // find the part that this door was added to (inc garages and sheds)
			if (is_basement(p)) continue; // skip the basement
			float const dist(door.pts[0][dim] - p->d[dim][dir]); // signed
			if (fabs(dist) < fabs(door_shift)) {door_shift = dist;}
		}
		assert(fabs(door_shift) < bcube.dz());
	}
	door_shift *= -(1.0 + dist_mult); // reflect on other side
	for (unsigned n = 0; n < door.npts; ++n) {door.pts[n][dim] += door_shift;} // move to opposite side of wall
}

void building_t::clip_door_to_interior(tquad_with_ix_t &door, bool clip_to_floor) const {

	cube_t clip_cube(door.get_bcube());
	float const dz(clip_cube.dz());
	float xy_border(0.0), z_border(0.0);
	if      (door.type == tquad_with_ix_t::TYPE_GDOOR) {xy_border = 0.016; z_border = 0.02;} // garage door
	else if (door.type == tquad_with_ix_t::TYPE_BDOOR) {xy_border = 0.04;  z_border = 0.03;} // building door
	else {xy_border = 0.06; z_border = 0.03;} // house door
	// clip off bottom for floor if clip_to_floor==1 and not a roof door; somewhat arbitrary, should we use interior->floors.back().z2() instead?
	if (door.type != tquad_with_ix_t::TYPE_RDOOR) {clip_cube.z1() += (clip_to_floor ? 0.7*get_floor_thickness() : 0.04*dz);}
	clip_cube.z2() -= z_border*dz;
	bool const dim(clip_cube.dx() < clip_cube.dy()); // border dim
	clip_cube.expand_in_dim(dim, -xy_border*clip_cube.get_sz_dim(dim)); // shrink by border
	for (unsigned n = 0; n < door.npts; ++n) {clip_cube.clamp_pt(door.pts[n]);}
}

cube_t building_t::get_part_containing_pt(point const &pt) const {
	for (auto i = parts.begin(); i != (parts.end() - has_chimney); ++i) { // includes garage/shed
		if (i->contains_pt(pt)) {return *i;}
	}
	// we can get here in rare cases due to FP precision problems; find the closest cube to pt, which should be very close
	float dmin_sq(0.0);
	cube_t closest;

	for (auto i = parts.begin(); i != (parts.end() - has_chimney); ++i) {
		float const dist_sq(p2p_dist(pt, i->closest_pt(pt)));
		if (dmin_sq == 0.0 || dist_sq < dmin_sq) {dmin_sq = dist_sq; closest = *i;}
	}
	assert(!closest.is_all_zeros()); // must be found
	return closest;
}

void building_t::adjust_part_zvals_for_floor_spacing(cube_t &c) const {

	if (!EXACT_MULT_FLOOR_HEIGHT) return;
	float const floor_spacing(get_window_vspace()), dz(c.dz());
	assert(dz > 0.0 && floor_spacing > 0.0);
	float const num_floors(dz/floor_spacing);
	int const targ_num_floors(max(1, round_fp(num_floors)));
	c.z2() += floor_spacing*(targ_num_floors - num_floors); // ensure c.dz() is an exact multiple of num_floors
}

// split c against existing cubes and add its non-overlapping pieces
void split_cubes_recur(cube_t c, vect_cube_t &cubes, unsigned search_start, unsigned search_end) {

	for (unsigned i = search_start; i < search_end; ++i) {
		cube_t &sc(cubes[i]);
		// while this generally holds, it can fail in rare cases (or used to?), I assume due to floating-point error or some such thing, so this check has been disabled
		//assert(sc.z2() >= c.z2()); // assumes cubes are ordered descending by ztop
		if (!sc.intersects_no_adj(c)) continue;
		if (sc.contains_cube(c)) return; // contained, done (remove all of c)

		if (sc.z1() == c.z1() && sc.z2() > c.z2() && c.contains_cube_xy(sc)) { // sc covers the base of c and spans c's z-range
			assert(sc.z1() < c.z2());
			sc.z1() = c.z2(); // create a stack instead
			continue;
		}
		// find a split plane
		for (unsigned d = 0; d < 2; ++d) { // dim
			for (unsigned e = 0; e < 2; ++e) { // dir
				float const split_pos(cubes[i].d[d][e]); // Note: can't use sc reference as it may have been invalidated by a push_back()
				
				if (split_pos > c.d[d][0] && split_pos < c.d[d][1]) { // this plane splits c
					cube_t hi_c(c);
					hi_c.d[d][0] = split_pos; // hi part
					c.   d[d][1] = split_pos; // lo part
					// recursively split the hi part starting at this cube if it's not contained; this split plane will no longer be active
					if (!cubes[i].contains_cube(hi_c)) {split_cubes_recur(hi_c, cubes, i, search_end);}
					if ( cubes[i].contains_cube(c)) return; // done (optimization)
				}
			} // for e
		} // for d
	} // for i
	cubes.push_back(c);
}

void building_t::gen_geometry(int rseed1, int rseed2) {

	if (!is_valid()) return; // invalid building
	if (!parts.empty()) {adjust_part_zvals_for_floor_spacing(parts.front());}
	cube_t const base(parts.empty() ? bcube : parts.back());
	assert(base.is_strictly_normalized());
	parts.clear();
	details.clear();
	roof_tquads.clear();
	doors.clear();
	interior.reset();
	building_mat_t const &mat(get_material());
	rand_gen_t rgen;
	rgen.set_state(123+rseed1, 345*rseed2);
	ao_bcz2         = bcube.z2(); // capture z2 before union with roof and detail geometry (which increases building height)
	ground_floor_z1 = bcube.z1(); // record before adding basement
	wall_color      = mat.wall_color; // start with default wall color
	if (is_house) {gen_house(base, rgen); return;}

	// determine building shape (cube, cylinder, other)
	if (rgen.rand_probability(mat.round_prob)) {num_sides = MAX_CYLIN_SIDES;} // max number of sides for drawing rounded (cylinder) buildings
	else if (rgen.rand_probability(mat.cube_prob)) {num_sides = 4;} // cube
	else { // N-gon
		num_sides = mat.min_sides;
		if (mat.min_sides != mat.max_sides) {num_sides += (rgen.rand() % (1 + abs((int)mat.max_sides - (int)mat.min_sides)));}
	}
	bool const was_cube(is_cube()); // before num_sides increase due to ASF

	if (num_sides >= 6 && mat.max_fsa > 0.0) { // at least 6 sides
		flat_side_amt = max(0.0f, min(0.45f, rgen.rand_uniform(mat.min_fsa, mat.max_fsa)));
		if (flat_side_amt > 0.0 && rot_sin == 0.0) {start_angle = rgen.rand_uniform(0.0, TWO_PI);} // flat side, not rotated: add random start angle to break up uniformity
	}
	if ((num_sides == 3 || num_sides == 4 || num_sides == 6) && mat.max_asf > 0.0 && rgen.rand_probability(mat.asf_prob)) { // triangles/cubes/hexagons
		alt_step_factor = max(0.0f, min(0.99f, rgen.rand_uniform(mat.min_asf, mat.max_asf)));
		if (alt_step_factor > 0.0 && !(num_sides&1)) {half_offset = 1;} // chamfered cube/hexagon
		if (alt_step_factor > 0.0) {num_sides *= 2;}
	}

	// determine the number of levels and splits
	unsigned num_levels(mat.min_levels);

	if (mat.min_levels < mat.max_levels) { // have a range of levels
		if (was_cube || rgen.rand_bool()) {num_levels += rgen.rand() % (mat.max_levels - mat.min_levels + 1);} // only half of non-cubes are multilevel (unless min_level > 1)
	}
	if (mat.min_level_height > 0.0) {num_levels = max(mat.min_levels, min(num_levels, unsigned(bcube.get_size().z/mat.min_level_height)));}
	num_levels = max(num_levels, 1U); // min_levels can be zero to apply more weight to 1 level buildings
	bool const do_split(num_levels < 4 && is_cube() && rgen.rand_probability(mat.split_prob)); // don't split buildings with 4 or more levels, or non-cubes

	if (num_levels == 1) { // single level
		if (do_split) { // generate L, T, or U shape
			split_in_xy(base, rgen);

			if (has_city_trees()) { // see if we can place a tree in the courtyard
				point const center(bcube.get_cube_center());
				cube_t place_area(center, center);
				place_area.expand_by_xy(0.05f*(bcube.dx() + bcube.dy()));
				if (!has_bcube_int(place_area, parts)) {tree_pos = place_area.get_cube_center(); tree_pos.z = ground_floor_z1;}
			}
			gen_details(rgen, 0);
		}
		else { // single part, entire cube/cylinder
			parts.push_back(base);
			if ((rgen.rand()&3) != 0) {maybe_add_special_roof(rgen);} // 75% chance
			
			if (0 && interior_enabled()) { // while this works, it doesn't seem to add much value, it only creates odd geometry and connecting stairs/elevators
				// two stacked parts of the same x/y dimensions but different interior floorplans
				float const dz(base.dz()), split_zval(rgen.rand_uniform(0.4, 0.6));
				parts.push_back(base);
				parts[0].z2() = parts[0].z1() + split_zval*dz; // split in Z: parts[0] is the bottom, parts[1] is the top
				adjust_part_zvals_for_floor_spacing(parts[0]);
				parts[1].z1() = parts[0].z2();
			}
			gen_details(rgen, 1);
		}
		end_add_parts();
		gen_interior(rgen, 0);
		gen_building_doors_if_needed(rgen);
		return; // for now the bounding cube
	}
	// generate building levels and splits
	float const height(base.dz()), dz(height/num_levels);
	float const abs_min_edge_move(0.5*mat.get_floor_spacing()); // same as door width
	bool const not_too_small(min(bcube.dx(), bcube.dy()) > 4.0*abs_min_edge_move);
	assert(height > 0.0);

	if (!do_split && not_too_small && (rgen.rand()&3) < (was_cube ? 2 : 3)) {
		// oddly shaped multi-sided overlapping sections (50% chance for cube buildings and 75% chance for others)
		has_complex_floorplan = 1;
		point const sz(base.get_size());
		parts.reserve(num_levels); // at least this many

		for (unsigned i = 0; i < num_levels; ++i) { // generate overlapping cube levels, tallest to shortest
			cube_t bc(base); // copy from base to start, keep z1
			bc.z2() = base.z1() + (num_levels - i)*dz; // z2
			if (i > 0) {bc.z2() += dz*rgen.rand_uniform(-0.45, 0.45); bc.z2() = min(bc.z2(), base.z2());}
			if (i > 0) {assert(bc.z2() <= parts.back().z2());}
			assert(bc.is_strictly_normalized());
			adjust_part_zvals_for_floor_spacing(bc);
			bool valid(0);

			// make 200 attempts to generate a cube that isn't contained in any existing cubes; most of the time should pass the first time, so it should rarely fail
			for (unsigned n = 0; n < 200; ++n) {
				for (unsigned d = 0; d < 2; ++d) { // x,y
					float const mv_lo(rgen.rand_uniform(-0.2, 0.45)), mv_hi(rgen.rand_uniform(-0.2, 0.45));
					if (mv_lo > 0.0) {bc.d[d][0] = base.d[d][0] + max(abs_min_edge_move, mv_lo*sz[d]);}
					if (mv_hi > 0.0) {bc.d[d][1] = base.d[d][1] - max(abs_min_edge_move, mv_hi*sz[d]);}
				}
				if (!bc.is_strictly_normalized()) continue; // can fail when building is very small compared to floor spacing/abs_min_edge_move
				bool contained(0);
				for (auto p = parts.begin(); p != parts.end(); ++p) {contained |= p->contains_cube(bc);}
				if (!contained) {valid = 1; break;} // success
			} // for n
			if (!valid) break; // remove this part and end the building here
			if (i == 0 || !is_simple_cube()) {parts.push_back(bc); continue;} // no splitting
			split_cubes_recur(bc, parts, 0, parts.size()); // split this cube against all previously added cubes and remove overlapping areas
		} // for i
		if (!parts.empty()) { // at least one part was placed; should (almost) always be true?
			parts.shrink_to_fit(); // optional
			std::reverse(parts.begin(), parts.end()); // highest part should be last so that it gets the roof details (remember to update basement_part_ix if needed)
			calc_bcube_from_parts(); // update bcube
			gen_details(rgen, 1);
			end_add_parts();
			gen_interior(rgen, 1);
			gen_building_doors_if_needed(rgen);
			return;
		}
	}
	parts.resize(num_levels);

	for (unsigned i = 0; i < num_levels; ++i) {
		cube_t &bc(parts[i]);
		if (i == 0) {bc = base;} // use full building footprint
		else {
			cube_t const &prev(parts[i-1]);
			float const shift_mult(was_cube ? 1.0 : 0.5); // half the shift for non-cube buildings

			for (unsigned d = 0; d < 2; ++d) {
				float const len(prev.get_sz_dim(d)), min_edge_len((0.2f/shift_mult)*(bcube.get_sz_dim(d)));
				bool const inv(rgen.rand_bool());

				for (unsigned e = 0; e < 2; ++e) {
					float delta(0.0);
					if (rgen.rand()&3) {delta = shift_mult*rgen.rand_uniform(0.1, 0.4);} // 25% chance of no shift, 75% chance of 20-40% shift
					bc.d[d][e] = prev.d[d][e] + (e ? -delta : delta)*len;
				}
				for (unsigned E = 0; E < 2; ++E) {
					bool const e((E != 0) ^ inv); // no dir favoritism for 20% check
					if (bc.get_sz_dim(d) < min_edge_len) {bc.d[d][e] = prev.d[d][e];} // if smaller than 20% base width, revert the change
				}
			}
			bc.z1() = prev.z2(); // z1
		}
		bc.z2() = bc.z1() + dz; // z2
		bc.normalize(); // handle XY inversion due to shift
	} // for i
	for (unsigned i = 1; i < num_levels; ++i) {
		float const ddz(rgen.rand_uniform(-0.35*dz, 0.35*dz)); // random shift in z height
		parts[i-1].z2() += ddz;
		adjust_part_zvals_for_floor_spacing(parts[i-1]);
		parts[i].z1() = parts[i-1].z2(); // make top and bottom parts align
	}
	adjust_part_zvals_for_floor_spacing(parts[num_levels-1]); // last one
	max_eq(bcube.z2(), parts[num_levels-1].z2()); // adjust bcube if needed
	cube_t const split_cube(parts.back());

	if (do_split && split_cube.dx() > 0.4*bcube.dx() && split_cube.dy() > 0.4*bcube.dy()) { // generate L, T, or U shape if not too small
		parts.pop_back();
		split_in_xy(split_cube, rgen);
		if (num_levels <= 3) {gen_details(rgen, 0);}
	}
	else {
		if ((rgen.rand()&3) != 0) {maybe_add_special_roof(rgen);} // 67% chance
		if (num_levels <= 3) {gen_details(rgen, 1);}
	}
	end_add_parts();
	gen_interior(rgen, 0);
	gen_building_doors_if_needed(rgen);
}

void building_t::split_in_xy(cube_t const &seed_cube, rand_gen_t &rgen) {

	// generate L, T, U, H, +, O shape
	point const llc(seed_cube.get_llc()), sz(seed_cube.get_size());
	bool const allow_courtyard(seed_cube.dx() < 1.6*seed_cube.dy() && seed_cube.dy() < 1.6*seed_cube.dx()); // AR < 1.6:1
	int const shape(rand()%(allow_courtyard ? 10 : 9)); // 0-9
	has_courtyard = (shape == 9);
	bool const is_hpo(shape >= 7);
	bool const dim(rgen.rand_bool()); // {x,y}
	bool const dir(is_hpo ? 1 : rgen.rand_bool()); // {neg,pos} - H/+/O shapes are symmetric and always pos
	float const smin(0.2), smax(has_courtyard ? 0.35 : 0.4); // outer and inner split sizes
	float const div(is_hpo ? rgen.rand_uniform(smin, smax) : rgen.rand_uniform(0.3, 0.7)); // split pos in 0-1 range
	float const s1(rgen.rand_uniform(smin, smax)), s2(rgen.rand_uniform(1.0-smax, 1.0-smin)); // split pos in 0-1 range
	float const dpos(llc[dim] + div*sz[dim]), spos1(llc[!dim] + s1*sz[!dim]), spos2(llc[!dim] + s2*sz[!dim]); // split pos in cube space
	float const dpos2(llc[dim] + (1.0 - div)*sz[dim]); // other end - used for H, +, and O
	unsigned const start(parts.size()), num((shape >= 9) ? 4 : ((shape >= 6) ? 3 : 2));
	parts.resize(start+num, seed_cube);
	parts[start+0].d[dim][ dir] = dpos; // full width part (except +)
	parts[start+1].d[dim][!dir] = dpos; // partial width part (except +)

	switch (shape) {
	case 0: case 1: case 2: case 3: // L
		parts[start+1].d[!dim][shape>>1] = ((shape&1) ? spos2 : spos1);
		break;
	case 4: case 5: // T
		parts[start+1].d[!dim][0] = spos1;
		parts[start+1].d[!dim][1] = spos2;
		break;
	case 6: // U
		parts[start+2].d[ dim][!dir] = dpos; // partial width part
		parts[start+1].d[!dim][1   ] = spos1;
		parts[start+2].d[!dim][0   ] = spos2;
		break;
	case 7: { // H
		parts[start+1].d[ dim][ dir] = dpos2;
		parts[start+1].d[!dim][ 0  ] = spos1; // middle part
		parts[start+1].d[!dim][ 1  ] = spos2;
		parts[start+2].d[ dim][!dir] = dpos2; // full width part
		break;
	}
	case 8: { // +
		parts[start+0].d[!dim][ 0  ] = spos1;
		parts[start+0].d[!dim][ 1  ] = spos2;
		parts[start+2].d[!dim][ 0  ] = spos1;
		parts[start+2].d[!dim][ 1  ] = spos2;
		parts[start+1].d[ dim][ dir] = dpos2; // middle part
		parts[start+2].d[ dim][!dir] = dpos2; // partial width part
		break;
	}
	case 9: { // O (courtyard)
		parts[start+2].d[ dim][!dir] = dpos; // partial width part (same as U)
		parts[start+1].d[ dim][ dir] = dpos2;
		parts[start+2].d[ dim][ dir] = dpos2;
		parts[start+1].d[!dim][1   ] = spos1;
		parts[start+2].d[!dim][0   ] = spos2;
		parts[start+3].d[ dim][!dir] = dpos2; // other full width part
		break;
	}
	default: assert(0);
	}
}

bool get_largest_xy_dim(cube_t const &c) {return (c.dy() > c.dx());}

cube_t building_t::place_door(cube_t const &base, bool dim, bool dir, float door_height, float door_center,
	float door_pos, float door_center_shift, float width_scale, bool can_fail, rand_gen_t &rgen)
{
	float const door_width(width_scale*door_height), door_half_width(0.5*door_width);
	if (can_fail && base.get_sz_dim(!dim) < 2.0*door_width) return cube_t(); // part is too small to place a door
	float const door_shift(0.01*get_material().get_floor_spacing());
	bool const calc_center(door_center == 0.0); // door not yet calculated
	bool const centered(door_center_shift == 0.0 || hallway_dim == (uint8_t)dim); // center doors connected to primary hallways
	cube_t door;
	door.z1() = base.z1(); // same bottom as house
	door.z2() = door.z1() + door_height;

	for (unsigned n = 0; n < 10; ++n) { // make up to 10 tries to place a valid door
		if (calc_center) { // add door to first part of house/building
			float const offset(centered ? 0.5 : rgen.rand_uniform(0.5-door_center_shift, 0.5+door_center_shift));
			door_center = offset*base.d[!dim][0] + (1.0 - offset)*base.d[!dim][1];
			door_pos    = base.d[dim][dir];
		}
		if (interior && !has_pri_hall()) { // not on a hallway
			vect_cube_t const &walls(interior->walls[!dim]); // perpendicular to door
			float const door_lo(door_center - 1.2*door_half_width), door_hi(door_center + 1.2*door_half_width); // pos along wall with a small expand
			float const dpos_lo(door_pos    -     door_half_width), dpos_hi(door_pos    +     door_half_width); // expand width of the door

			for (auto w = walls.begin(); w != walls.end(); ++w) {
				if (w->d[ dim][0] > dpos_hi || w->d[ dim][1] < dpos_lo) continue; // not ending at same wall as door
				if (w->d[!dim][0] > door_hi || w->d[!dim][1] < door_lo) continue; // not intersecting door
				// Note: since we know that all rooms are wider than the door width, we know that we have space for a door on either side of the wall
				float const lo_dist(w->d[!dim][0] - door_lo), hi_dist(door_hi - w->d[!dim][1]);
				if (lo_dist < hi_dist) {door_center += lo_dist;} else {door_center -= hi_dist;} // move the door so that it doesn't open into the end of the wall
				break;
			}
		}
		door.d[ dim][!dir] = door_pos + door_shift*(dir ? 1.0 : -1.0); // move slightly away from the house to prevent z-fighting
		door.d[ dim][ dir] = door.d[dim][!dir]; // make zero size in this dim
		door.d[!dim][0] = door_center - door_half_width; // left
		door.d[!dim][1] = door_center + door_half_width; // right

		// we're free to choose the door pos, and have the interior, so we can check if the door is in a good location;
		// if we get here on the last iteration, just keep the door even if it's an invalid location
		if (calc_center && interior && !centered) {
			if (interior->is_blocked_by_stairs_or_elevator(door, door_width)) {
				if (can_fail && n == 9) return cube_t(); // last iteration, fail
				continue; // try a new location
			}
		}
		break; // done
	} // for n
	return door;
}

void building_t::clip_cube_to_parts(cube_t &c, vect_cube_t &cubes) const { // use for fences
	for (auto p = parts.begin(); p != get_real_parts_end_inc_sec(); ++p) {
		if (!p->intersects(c)) continue;
		cubes.clear();
		subtract_cube_from_cube(c, *p, cubes);
		assert(cubes.size() == 1); // each part should either not intersect the fence, or clip off one side of it
		c = cubes.back();
	}
}

void building_t::gen_house(cube_t const &base, rand_gen_t &rgen) {

	assert(parts.empty());
	int const type(rgen.rand()%3); // 0=single cube, 1=L-shape, 2=two-part
	bool const two_parts(type != 0);
	unsigned force_dim[2] = {2,2}; // force roof dim to this value, per part; 2 = unforced/auto
	bool skip_last_roof(0);
	num_sides = 4;
	parts.reserve(two_parts ? 5 : 2); // two house sections + porch roof + porch support + chimney (upper bound)
	parts.push_back(base);
	// add a door
	bool const gen_door(global_building_params.windows_enabled());
	float const door_width_scale(0.5);
	unsigned const rand_num(rgen.rand()); // some bits will be used for random bools
	float door_height(get_door_height()), floor_spacing(get_window_vspace()), door_center(0.0), door_pos(0.0), dist1(0.0), dist2(0.0);;
	bool door_dim(rand_num & 1), door_dir(0), dim(0), dir(0), dir2(0);
	unsigned door_part(0), detail_type(0);
	real_num_parts = (two_parts ? 2 : 1); // only walkable parts: excludes shed, garage, porch roof, and chimney
	cube_t door_cube;

	if (two_parts) { // multi-part house; parts[1] is the lower height part
		parts.push_back(base); // add second part
		dir = rgen.rand_bool(); // in dim
		float const split(rgen.rand_uniform(0.4, 0.6)*(dir  ? -1.0 : 1.0));
		float delta_height(0.0), shrink[2] = {0.0};

		if (type == 1) { // L-shape
			dir2         = rgen.rand_bool(); // in !dim
			dim          = rgen.rand_bool();
			shrink[dir2] = rgen.rand_uniform(0.4, 0.6)*(dir2 ? -1.0 : 1.0);
			delta_height = max(0.0f, rand_uniform(-0.1, 0.5));
		}
		else if (type == 2) { // two-part
			dim          = get_largest_xy_dim(base); // choose longest dim
			delta_height = rand_uniform(0.1, 0.5);

			for (unsigned d = 0; d < 2; ++d) {
				if (rgen.rand_bool()) {shrink[d] = rgen.rand_uniform(0.2, 0.35)*(d ? -1.0 : 1.0);}
			}
			float const tot_shrink(shrink[0] - shrink[1]), un_shrink_each(0.5*(tot_shrink - 0.6)); // max shrink summed across each side is 0.6 to prevent too-small parts
			if (un_shrink_each > 0.0) {shrink[0] -= un_shrink_each; shrink[1] += un_shrink_each;}
		}
		vector3d const sz(base.get_size());
		parts[0].d[ dim][ dir] += split*sz[dim]; // split in dim
		parts[1].d[ dim][!dir]  = parts[0].d[dim][dir];
		cube_t const pre_shrunk_p1(parts[1]); // save for use in details below
		parts[1].z2() -= delta_height*parts[1].dz(); // lower height
		//ao_bcz2 = parts[1].z2(); // set this lower zval as the base AO so that AO values line up with both parts and the roof triangles
		if (ADD_BUILDING_INTERIORS) {adjust_part_zvals_for_floor_spacing(parts[1]);}

		if (shrink[0] == 0.0 && shrink[1] == 0.0 && parts[0].z2() == parts[1].z2()) { // same width and height - not a valid split
			bool const side(rgen.rand_bool()); // which side do we move
			shrink[side] = rgen.rand_uniform(0.2, 0.35)*(side ? -1.0 : 1.0)*sz[!dim];
		}
		for (unsigned d = 0; d < 2; ++d) {parts[1].d[!dim][d] += shrink[d]*sz[!dim];} // shrink this part in the other dim
		if (type == 1 && rgen.rand_bool()) {force_dim[0] = !dim; force_dim[1] = dim;} // L-shape, half the time
		else if (type == 2) {force_dim[0] = force_dim[1] = dim;} // two-part - force both parts to have roof along split dim
		detail_type = ((type == 1) ? (rgen.rand()%3) : 0); // 0=none, 1=porch, 2=detatched garage/shed
		door_dir = ((door_dim == dim) ? dir : dir2); // if we have a porch/shed/garage, put the door on that side
		if (door_dim == dim && detail_type == 0) {door_dir ^= 1;} // put it on the opposite side so that the second part isn't in the way
		maybe_add_basement(rgen);
		vect_cube_t cubes; // reused for tree and fence

		if (detail_type != 0) { // add details to L-shaped house
			cube_t c(pre_shrunk_p1);
			c.d[!dim][!dir2] = parts[1].d[!dim][dir2]; // other half of the shrunk part1
			dist1 = (c.d[!dim][!dir2] - base.d[!dim][dir2])*rgen.rand_uniform(0.4, 0.6);
			dist2 = (c.d[ dim][!dir ] - base.d[ dim][dir ])*rgen.rand_uniform(0.4, 0.6);
			float const base_dz(parts[1].dz()), height(min(base_dz, max(door_height/0.95f, rgen.rand_uniform(0.55, 0.7)*base_dz)));

			if (gen_door) { // add door in interior of L, centered under porch roof (if it exists, otherwise where it would be)
				door_cube   = c;
				door_center = door_cube.get_center_dim(!door_dim) + 0.5f*((door_dim == dim) ? dist1 : dist2);
				door_pos    = door_cube.d[door_dim][!door_dir];
				door_part   = ((door_dim == dim) ? 0 : 1); // which part the door is connected to
				min_eq(door_height, 0.95f*height);
			}
			if (detail_type == 1) { // porch
				float const width(0.05f*(fabs(dist1) + fabs(dist2))); // width of support pillar
				c.d[!dim][dir2 ] += dist1; // move away from bcube edge
				c.d[ dim][ dir ] += dist2; // move away from bcube edge
				c.d[!dim][!dir2] -= 0.001*dist1; // adjust slightly so it's not exactly adjacent to the house and won't be considered internal face removal logic
				c.d[ dim][ !dir] -= 0.001*dist2;
				c.z1() += height; // move up
				c.z2()  = c.z1() + 0.05*parts[1].dz();
				parts.push_back(c); // porch roof
				c.z2() = c.z1();
				c.z1() = pre_shrunk_p1.z1(); // support pillar
				c.d[!dim][!dir2] = c.d[!dim][dir2] + (dir2 ? -1.0 : 1.0)*width;
				c.d[ dim][!dir ] = c.d[ dim][ dir] + (dir  ? -1.0 : 1.0)*width;
				skip_last_roof = 1;
			}
			else if (detail_type == 2) { // detatched garage/shed
				c.d[!dim][dir2 ]  = base.d[!dim][dir2]; // shove it into the opposite corner of the bcube
				c.d[ dim][dir  ]  = base.d[ dim][dir ]; // shove it into the opposite corner of the bcube
				c.d[!dim][!dir2] -= dist1; // move away from bcube edge
				c.d[ dim][!dir ] -= dist2; // move away from bcube edge
				c.z2() = c.z1() + min(min(c.dx(), c.dy()), height); // no taller than x or y size; Note: z1 same as part1
				vector3d const car_sz(get_nom_car_size());
				bool const is_garage(max(c.dx(), c.dy()) > 1.2f*car_sz.x && min(c.dx(), c.dy()) > 1.2f*car_sz.y && c.dz() > 1.2f*car_sz.z); // must be able to fit a car
				(is_garage ? has_garage : has_shed) = 1;
				// add a door
				bool const long_dim(c.dx() < c.dy());
				bool door_dir(c.d[long_dim][0] == bcube.d[long_dim][0]); // interior face
				if (is_garage) {door_dir ^= 1;} // facing away from the house, not toward it
				float const wscale(is_garage ? 0.9*c.get_sz_dim(!long_dim)/door_height : door_width_scale);
				add_door(place_door(c, long_dim, door_dir, door_height, 0.0, 0.0, 0.0, wscale, 0, rgen), parts.size(), long_dim, door_dir, 0);
				if (is_garage) {doors.back().type = tquad_with_ix_t::TYPE_GDOOR;} // make it a garage door rather than a house door
				//garage_dim = long_dim;

				if (is_garage) { // add driveway TODO: need to disable grass
					bool const ddir((long_dim == dim) ? dir : dir2);
					driveway = c;
					driveway.z2() = driveway.z1() + 0.02*door_height;
					driveway.expand_in_dim(!long_dim, -0.05*c.get_sz_dim(!long_dim));
					driveway.d[long_dim][!ddir] = driveway.d[long_dim][ddir];
					driveway.d[long_dim][ ddir] += (ddir ? 1.0 : -1.0)*0.4*c.get_sz_dim(long_dim); // set length
				}
			}
			parts.push_back(c); // support column or shed/garage
		} // end house details
		else if (type == 1 && has_city_trees() && !is_rotated()) { // place a tree for L-shaped house with no garage or shed
			cube_t empty_space(bcube);

			for (unsigned e = 0; e < 2; ++e) {
				cubes.clear();
				subtract_cube_from_cube(empty_space, parts[e], cubes);
				assert(cubes.size() == 1); // Note: may fail for rotated buildings
				empty_space = cubes[0];
			}
			tree_pos = empty_space.get_cube_center(); // centered on where the garage/shed would have been
			vector3d const dir(tree_pos - bcube.get_cube_center());
			tree_pos  += (0.05f*(bcube.dx() + bcube.dy())/dir.mag())*dir; // shift slightly away from house center so that it's less likely to intersect the house
			tree_pos.z = ground_floor_z1;
		}
		if (type == 1 && (rand_num & 6) != 0 && !is_rotated()) { // L-shaped house, add a fence 75% of the time
			// use the same dim as the shrunk house part for the full fence section; or should we use garage_dim?
			float const fence_thickness(0.08*door_height);
			cube_t fence(bcube), fence2(bcube);
			fence.z2() = fence2.z2() = ground_floor_z1 + 0.65*door_height; // set fence height
			fence.d[!dim][!dir2] = fence.d[!dim][dir2] + (dir2 ? -1.0 : 1.0)*fence_thickness;
			// we can calculate the exact location of the fence, but it depends on detail_type, garage/shed position, etc.,
			// so it's easier to start with the entire edge and clip it by the house parts and optional garage/shed
			clip_cube_to_parts(fence, cubes);
			fence.expand_in_dim(dim, -0.01*door_height); // shrink slightly to avoid clipping through the exterior wall
			fences.push_back(fence);

			if ((rand_num & 6) != 2) { // 67% of the time
				fence2.d[dim][!dir] = fence2.d[dim][dir] + (dir ? -1.0 : 1.0)*fence_thickness;
				clip_cube_to_parts(fence2, cubes);
				float const center_pt(fence2.get_center_dim(!dim)), gate_width(0.8*door_height);

				for (unsigned d = 0; d < 2; ++d) { // split fence, add gap for gate, and add each segment if it's large enough
					cube_t seg(fence2);
					seg.d[!dim][d] = center_pt + (d ? -0.5f : 0.5f)*gate_width;
					if (seg.get_sz_dim(!dim) < gate_width) continue; // too small
					seg.expand_in_dim(!dim, -0.01*door_height); // shrink slightly to avoid clipping through the exterior wall
					fences.push_back(seg);
				}
			}
		}
	} // end type != 0  (multi-part house)
	else if (gen_door) { // single cube house
		maybe_add_basement(rgen);
		door_dir  = rgen.rand_bool(); // select a random dir
		door_part = 0; // only one part
	}
	calc_bcube_from_parts(); // maybe calculate a tighter bounding cube
	gen_interior(rgen, 0); // before adding door

	if (gen_door) {
		cube_t door(place_door(parts[door_part], door_dim, door_dir, door_height, door_center, door_pos, 0.25, door_width_scale, 0, rgen));

		if (interior && interior->is_blocked_by_stairs_or_elevator(door, 0.5*door_height)) { // blocked by stairs - switch door to other side/dim
			door_dim ^= 1;
			door_dir  = (two_parts ? ((door_dim == dim) ? dir : dir2) : (door_dir^1)); // if we have a porch/shed/garage, put the door on that side
			if (door_dim == dim && two_parts && detail_type == 0) {door_dir ^= 1;} // put it on the opposite side so that the second part isn't in the way

			if (door_center != 0.0) { // reclaculate for L-shaped house
				door_center = door_cube.get_center_dim(!door_dim) + 0.5f*((door_dim == dim) ? dist1 : dist2);
				door_pos    = door_cube.d[door_dim][!door_dir];
				door_part   = ((door_dim == dim) ? 0 : 1); // which part the door is connected to
			}
			door = place_door(parts[door_part], door_dim, door_dir, door_height, door_center, door_pos, 0.25, door_width_scale, 0, rgen); // keep door_height
		}
		add_door(door, door_part, door_dim, door_dir, 0);
		if (doors.size() == 2) {swap(doors[0], doors[1]);} // make sure the house door comes before the garage/shed door
		float const tot_area(parts[0].dx()*parts[0].dy() + (two_parts ? parts[1].dx()*parts[1].dy() : 0.0f));

		if (tot_area > 25.0f*floor_spacing*floor_spacing) { // if house is large enough, add a back door
			bool added_door(0);

			for (unsigned p = 0; p < real_num_parts && !added_door; ++p) {
				unsigned door2_part(two_parts ? 1-door_part : 0); // try the other part first
				cube_t const &part(parts[door2_part]);

				for (unsigned d = 0; d < 2 && !added_door; ++d) {
					bool const door2_dim(door_dim ^ bool(d)); // try same dim first

					for (unsigned door2_dir = 0; door2_dir < 2 && !added_door; ++door2_dir) {
						if (door2_dim == door_dim && door_dir == bool(door2_dir) /*&& door2_part == door_part*/) continue; // don't place second door on the same side
						if (part.d[door2_dim][door2_dir] != bcube.d[door2_dim][door2_dir]) continue; // door on building bcube is always exterior/never interior face between two parts
						cube_t door2(place_door(part, door2_dim, door2_dir, door_height, 0.0, 0.0, 0.25, door_width_scale, 1, rgen));
						if (interior && interior->is_blocked_by_stairs_or_elevator(door2, 0.5*door_height)) continue; // bad placement
						added_door |= add_door(door2, door2_part, door2_dim, door2_dir, 0);
					} // for door_dir2
				} // for d
			} // for p
		} // end back door
	} // end gen_door
	// add roof tquads
	float const peak_height(rgen.rand_uniform(0.15, 0.5)); // same for all parts
	float roof_dz[4] = {0.0f};
	bool hipped_roof[4] = {0};

	for (auto i = parts.begin(); (i + skip_last_roof) != parts.end(); ++i) {
		if (is_basement(i)) continue; // skip the basement
		unsigned const ix(i - parts.begin());
		bool const main_part(ix < real_num_parts);
		unsigned const fdim(main_part ? force_dim[ix] : 2);
		cube_t const &other((two_parts && main_part) ? parts[1-ix] : *i); // == self for single part houses
		bool const dim((fdim < 2) ? fdim : get_largest_xy_dim(*i)); // use longest side if not forced
		bool const other_dim(two_parts ? ((main_part && force_dim[1-ix] < 2) ? force_dim[1-ix] : get_largest_xy_dim(other)) : 0);
		float extend_to(0.0), max_dz(i->dz());

		if (type == 1 && ix == 1 && dim != other_dim && parts[0].z2() == parts[1].z2()) { // same z2, opposite dim T-junction
			max_dz    = peak_height*parts[0].get_sz_dim(!other_dim); // clamp roof zval to other roof's peak
			extend_to = parts[0].get_center_dim(!other_dim); // extend lower part roof to center of upper part roof
		}
		bool can_be_hipped(main_part && extend_to == 0.0 && i->get_sz_dim(dim) > i->get_sz_dim(!dim)); // must be longer dim
		
		if (can_be_hipped && two_parts) {
			float const part_roof_z (i->z2()    + min(peak_height*i->get_sz_dim(!dim), i->dz()));
			float const other_roof_z(other.z2() + min(peak_height*other.get_sz_dim(!other_dim), other.dz()));
			can_be_hipped = (part_roof_z >= other_roof_z); // no hipped for lower part
		}
		bool const hipped(can_be_hipped && rgen.rand_bool()); // hipped roof 50% of the time
		if (hipped) {roof_dz[ix] = gen_hipped_roof(*i, peak_height, extend_to);}
		else {
			unsigned skip_side_tri(2); // default = skip neither
			
			if (two_parts && main_part && (dim == other_dim || hipped_roof[1-ix]) && i->d[!dim][0] >= other.d[!dim][0] && i->d[!dim][1] <= other.d[!dim][1] && i->z2() <= other.z2()) {
				// side of i contained in other
				if (ix == 1 && i->z2() == other.z2()) { // same height - check if we need to ajust the slope of this roof to match
					float const roof_slope(roof_dz[0]/other.get_sz_dim(!dim));
					min_eq(max_dz, roof_slope*i->get_sz_dim(!dim)); // peak_height cancels out
				}
				for (unsigned d = 0; d < 2; ++d) {
					if (dim == other_dim && i->d[dim][d] == other.d[dim][!d]) {skip_side_tri = d;} // remove smaller of two opposing/overlapping triangles to prevent z-fighting
				}
			}
			assert(max_dz > 0.0);
			roof_dz[ix] = gen_peaked_roof(*i, peak_height, dim, extend_to, max_dz, skip_side_tri);
		}
		assert(roof_dz[ix] > 0.0);
		hipped_roof[ix] = hipped;
	} // for i
	if ((rgen.rand()%3) != 0) { // add a chimney 67% of the time
		unsigned part_ix(0);

		if (two_parts) { // start by selecting a part (if two parts)
			float const v0(parts[0].get_volume()), v1(parts[1].get_volume());
			if      (v0 > 2.0*v1) {part_ix = 0;} // choose larger part 0
			else if (v1 > 2.0*v0) {part_ix = 1;} // choose larger part 1
			else {part_ix = rgen.rand_bool();} // close in area - choose a random part
		}
		assert(roof_dz[part_ix] > 0.0);
		unsigned const fdim(force_dim[part_ix]);
		cube_t const &part(parts[part_ix]);
		bool const dim((fdim < 2) ? fdim : get_largest_xy_dim(part)); // use longest side if not forced
		bool dir(rgen.rand_bool());
		if (two_parts && part.d[dim][dir] != bcube.d[dim][dir]) {dir ^= 1;} // force dir to be on the edge of the house bcube (not at a point interior to the house)
		cube_t c(part);
		float const sz1(c.get_sz_dim(!dim)), sz2(c.get_sz_dim(dim));
		float shift(0.0);

		if ((rgen.rand()%3) != 0) { // make the chimney non-centered 67% of the time
			shift = sz1*rgen.rand_uniform(0.1, 0.25); // select a shift in +/- (0.1, 0.25) - no small offset from center
			if (rgen.rand_bool()) {shift = -shift;}
		}
		float const center(c.get_center_dim(!dim) + shift);
		float const chimney_dz((hipped_roof[part_ix] ? 0.5 : 1.0)*roof_dz[part_ix]); // lower for hipped roof
		float const chimney_height(rgen.rand_uniform(1.25, 1.5)*chimney_dz - 0.4f*abs(shift));
		c.d[dim][!dir]  = c.d[dim][ dir] + (dir ? -0.03f : 0.03f)*(sz1 + sz2); // chimney depth
		c.d[dim][ dir] += (dir ? -0.01 : 0.01)*sz2; // slight shift from edge of house to avoid z-fighting
		c.d[!dim][0] = center - 0.05*sz1;
		c.d[!dim][1] = center + 0.05*sz1;
		c.z1()  = c.z2();
		c.z2() += max(chimney_height, 0.75f*floor_spacing); // make it at least 3/4 a story in height
		parts.push_back(c);
		// add top quad to cap chimney (will also update bcube to contain chimney)
		tquad_t tquad(4); // quad
		tquad.pts[0].assign(c.x1(), c.y1(), c.z2());
		tquad.pts[1].assign(c.x2(), c.y1(), c.z2());
		tquad.pts[2].assign(c.x2(), c.y2(), c.z2());
		tquad.pts[3].assign(c.x1(), c.y2(), c.z2());
		roof_tquads.emplace_back(tquad, (unsigned)tquad_with_ix_t::TYPE_CCAP); // tag as chimney cap
		has_chimney = 1;
	}
	roof_type = ROOF_TYPE_PEAK; // peaked and hipped roofs are both this type
	add_roof_to_bcube();
	gen_grayscale_detail_color(rgen, 0.4, 0.8); // for roof
	door_color = (rgen.rand_bool() ? LT_BROWN : WHITE);
	// white, white, white, white, pink, peach, lt green, lt blue
	colorRGBA const wall_colors[8] = {WHITE, WHITE, WHITE, WHITE, colorRGBA(1.0, 0.85, 0.85), colorRGBA(1.0, 0.85, 0.75), colorRGBA(0.85, 1.0, 0.85), colorRGBA(0.85, 0.85, 1.0)};
	wall_color = wall_color.modulate_with(wall_colors[rgen.rand()%8]);
	if (rgen.rand_bool()) {add_solar_panels(rgen);} // maybe add solar panels
}

void building_t::maybe_add_basement(rand_gen_t &rgen) { // currently for houses only
	if (global_building_params.basement_prob <= 0.0) return; // no basement

	if (global_building_params.basement_prob < 1.0) {
		rand_gen_t rgen_copy(rgen); // make a copy so that we don't modify the incoming rgen
		if (rgen_copy.rand_float() > global_building_params.basement_prob) return;
	}
	basement_part_ix = (int8_t)parts.size();
	cube_t basement(parts[0]);
	
	if (real_num_parts == 2) {
		unsigned const ix(parts[1].get_area_xy() > parts[0].get_area_xy());
		basement = parts[ix]; // start with the larger part

		// attempt to expand into the smaller part as long as it fits within the footprint of the upper floors
		for (unsigned dim = 0; dim < 2; ++dim) {
			for (unsigned dir = 0; dir < 2; ++dir) {
				if (parts[ix].d[dim][dir] != parts[!ix].d[dim][!dir]) continue; // not adjacent in this dim
				if (parts[ix].d[!dim][0] < parts[!ix].d[!dim][0] || parts[ix].d[!dim][1] > parts[!ix].d[!dim][1]) continue; // smaller part does not contain larger part in this dim
				basement.d[dim][dir] = parts[!ix].d[dim][dir]; // extend into the other part
			}
		}
	}
	set_cube_zvals(basement, (basement.z1() - get_window_vspace()), basement.z1());
	parts.push_back(basement);
	min_eq(bcube.z1(), basement.z1()); // not really necessary, will be updated later anyway, but good to have here for reference; orig bcube.z1() is saved in ground_floor_z1
	++real_num_parts;
}

void building_t::add_solar_panels(rand_gen_t &rgen) { // for houses
	if (roof_tquads.empty()) return; // not a house?
	unsigned best_tquad(0);
	float best_zmax(0.0), best_area(0.0);

	for (auto r = roof_tquads.begin(); r != roof_tquads.end(); ++r) { // find highest roof, otherwise largest area if tied
		if (r->type != tquad_with_ix_t::TYPE_ROOF) continue; // only include roofs
		if (r->npts != 4) continue; // only quads, skip triangles
		float const zmax(max(max(r->pts[0].z, r->pts[1].z), max(r->pts[2].z, r->pts[3].z))), area(polygon_area(r->pts, r->npts));
		
		// determine the best candidate so far; use a random value to break ties
		if (best_area == 0.0 || zmax > best_zmax || (zmax == best_zmax && area > best_area) || (zmax == best_zmax && area == best_area && rgen.rand_bool())) {
			cube_t bcube(r->get_bcube());
			bcube.expand_by(-0.1*bcube.get_size()); // shrink by 10% (upper bound of approx solar panel area)
			bool valid(1);

			for (auto r2 = roof_tquads.begin(); r2 != roof_tquads.end(); ++r2) { // check other objects, including anything on the roof that's not part of the roof
				if (r2 == r) continue; // skip self
				if (r2->type == tquad_with_ix_t::TYPE_ROOF && r2->npts == 3) continue; // skip roof triangles as they won't intersect the adjacent quad
				if (r2->get_bcube().intersects(bcube)) {valid = 0; break;}
			}
			if (!valid) continue; // intersects another roof section, skip
			best_zmax  = zmax;
			best_area  = area;
			best_tquad = (r - roof_tquads.begin());
		}
	}
	assert(best_tquad < roof_tquads.size());
	tquad_with_ix_t const &roof(roof_tquads[best_tquad]);
	cube_t const roof_bcube(roof.get_bcube());
	float const thickness(0.075*get_window_vspace());
	vector3d const normal(roof.get_norm()), bias(thickness*normal); // slightly above the roof
	tquad_with_ix_t panel(4, tquad_with_ix_t::TYPE_SOLAR);
	point const *const pts(roof.pts);
	// shrink and make rectangular
	point const center(0.25*(pts[0] + pts[1] + pts[2] + pts[3]) + bias);
	vector3d const v10(pts[1] - pts[0]), v32(pts[2] - pts[3]), v30(pts[3] - pts[0]), v21(pts[2] - pts[1]); // 4 sides of roof quad
	float const mu1(v10.mag()), mu2(v32.mag()), mv1(v30.mag()), mv2(v21.mag()); // 4 side lengths
	// min side lengths shrunk, then halved; include vector sum because we want the orthogonal projection
	float const mu(rgen.rand_uniform(0.75, 0.85)*0.5*min(min(mu1, mu2), 0.5f*(v10 + v32).mag())), mv(rgen.rand_uniform(0.75, 0.85)*0.5*min(min(mv1, mv2), 0.5f*(v30 + v21).mag()));
	if (4.0f*mu*mv < 0.25f*roof_bcube.dx()*roof_bcube.dy()) return; // skip if less than 25% of rectangularized roof area (or try another root quad?)
	vector3d const u((v10/mu1 + v32/mu2).get_norm()), v((v30/mv1 + v21/mv2).get_norm());
	panel.pts[0] = center - mu*u - mv*v;
	panel.pts[1] = center + mu*u - mv*v;
	panel.pts[2] = center + mu*u + mv*v;
	panel.pts[3] = center - mu*u + mv*v;
	roof_tquads.push_back(panel);
	// add sides, similar to thick_poly_to_sides()
	tquad_t bottom(panel);
	for (unsigned n = 0; n < 4; ++n) {bottom[n] -= bias;}
	tquad_with_ix_t side(4, tquad_with_ix_t::TYPE_TRIM);

	for (unsigned i = 0; i < 4; ++i) {
		unsigned const inext((i+1)&3);
		side[0] = panel [i];
		side[1] = bottom[i];
		side[2] = bottom[inext];
		side[3] = panel [inext];
		roof_tquads.push_back(side);
	}
}

void rotate_xy(point &pt, point const &origin, float angle) {
	float const xv(pt.x - origin.x), yv(pt.y - origin.y), sin_term(sin(angle)), cos_term(cos(angle));
	pt.x = cos_term*xv - sin_term*yv + origin.x;
	pt.y = cos_term*yv + sin_term*xv + origin.y;
}

bool building_t::check_cube_contained_in_part(cube_t const &c) const {
	for (auto p = parts.begin(); p != get_real_parts_end(); ++p) {
		if (p->contains_cube(c)) return 1;
	}
	return 0;
}

tquad_with_ix_t building_t::set_door_from_cube(cube_t const &c, bool dim, bool dir, unsigned type, float pos_adj,
	bool exterior, bool opened, bool opens_out, bool opens_up, bool swap_sides) const
{
	tquad_with_ix_t door(4, type); // quad
	float const wall_thickness(get_wall_thickness()), floor_thickness(get_floor_thickness());
	float const pos(c.d[dim][0] + (opened ? 0.0 : pos_adj*(dir ? 1.0 : -1.0))); // move away from wall slightly (not needed if opened)
	door.pts[0].z = door.pts[1].z = c.z1(); // bottom
	door.pts[2].z = door.pts[3].z = c.z2(); // top
	door.pts[0][!dim] = door.pts[3][!dim] = c.d[!dim][ dir]; //  dir side
	door.pts[1][!dim] = door.pts[2][!dim] = c.d[!dim][!dir]; // !dir side
	door.pts[0][ dim] = door.pts[1][ dim] = door.pts[2][dim] = door.pts[3][dim] = pos;
	if (dim == 0) {swap(door.pts[0], door.pts[1]); swap(door.pts[2], door.pts[3]);} // swap two corner points to flip windowing dir and invert normal for doors oriented in X

	if (opened) { // rotate 90 degrees about pts[0]/pts[3] - change pts[1]/pts[2]
		if (opens_up) { // rotates inward and upward
			door.pts[0].z = door.pts[1].z = c.z2();
			door.pts[0][dim] = door.pts[1][dim] = pos + c.dz()*((dir ^ opens_out) ? 1.0 : -1.0);
		}
		else { // rotates to the side
			float const width(c.get_sz_dim(!dim)), signed_width(width*((dir ^ opens_out) ? 1.0 : -1.0));
			float const offset(0.01*width*((dir ^ dim) ? 1.0 : -1.0)); // move slightly away from the wall to prevent z-fighting
			door.pts[!swap_sides][!dim] = door.pts[2+swap_sides][!dim] = door.pts[swap_sides][!dim] + offset;
			door.pts[1][dim] = door.pts[2][dim] = pos + signed_width;

			if (!exterior) { // try to open the door all the way
				for (unsigned angle = 75; angle > 0; angle -= 15) { // try to open door as much as 75 degrees in steps of 15 degrees
					tquad_with_ix_t orig_door(door); // cache orig 90 degree open door in case we need to revert it
					float const shift(0.07*signed_width), rot_angle(-float(angle)*TO_RADIANS);
					for (unsigned i = 1; i < 3; ++i) {rotate_xy(door.pts[i], door.pts[swap_sides], rot_angle);}
					for (unsigned i = 0; i < 4; ++i) {door.pts[i][dim] += shift;}
					cube_t test_bcube(door.get_bcube());
					test_bcube.expand_in_dim(!dim,  wall_thickness); // expand slightly to leave a bit of a gap between walls, and space for whiteboards
					test_bcube.expand_in_dim( dim, -wall_thickness); // shrink in other dim to avoid intersecting with other part/walls when this door separates two parts
					test_bcube.expand_in_dim(2, -floor_thickness);   // shrink a bit in z to avoid picking up objects from stacks above or below
					
					if (!check_cube_contained_in_part(test_bcube) || // bad placement (extends outside part)
					    has_bcube_int(test_bcube, interior->walls[!dim]) || // bad placement (hits perp wall)
					    interior->is_blocked_by_stairs_or_elevator(test_bcube)) // bad placement (hits stairs or elevator)
					{
						door = orig_door; // revert
						continue;
					}
					break; // done
				} // for angle
			}
		}
	}
	return door;
}

bool building_t::add_door(cube_t const &c, unsigned part_ix, bool dim, bool dir, bool for_building, bool roof_access) { // exterior doors

	if (c.is_all_zeros()) return 0;
	vector3d const sz(c.get_size());
	assert(sz[dim] == 0.0 && sz[!dim] > 0.0 && sz.z > 0.0);
	unsigned const type(for_building ? (unsigned)tquad_with_ix_t::TYPE_BDOOR : (unsigned)tquad_with_ix_t::TYPE_HDOOR);
	float const pos_adj(0.015*get_window_vspace()); // distance to move away from the building wall
	doors.push_back(set_door_from_cube(c, dim, dir, type, pos_adj, 1, 0, 0, 0, 0)); // exterior=1, opened=0, opens_out=0, opens_up=0, swap_sides=0
	if (!roof_access && part_ix < 4) {door_sides[part_ix] |= 1 << (2*dim + dir);}
	if (roof_access) {doors.back().type = tquad_with_ix_t::TYPE_RDOOR;}
	return 1;
}

unsigned extend_roof(cube_t &top, float extend_to, bool dim) {
	if (extend_to == 0.0) return 2; // extend in neither dim
	if (extend_to < top.d[dim][0]) {top.d[dim][0] = extend_to; return 0;} // lo side extend
	if (extend_to > top.d[dim][1]) {top.d[dim][1] = extend_to; return 1;} // hi side extend
	return 0; // extend in neither dim
}

// roof made from two sloped quads and two triangles
float building_t::gen_peaked_roof(cube_t const &top_, float peak_height, bool dim, float extend_to, float max_dz, unsigned skip_side_tri) {

	cube_t top(top_); // deep copy
	unsigned const extend_dir(extend_roof(top, extend_to, dim));
	float const width(top.get_sz_dim(!dim)), roof_dz(min(max_dz, min(peak_height*width, top.dz())));
	float const z1(top.z2()), z2(z1 + roof_dz), x1(top.x1()), y1(top.y1()), x2(top.x2()), y2(top.y2());
	assert(roof_dz > 0.0);
	point pts[6] = {point(x1, y1, z1), point(x1, y2, z1), point(x2, y2, z1), point(x2, y1, z1), point(x1, y1, z2), point(x2, y2, z2)};
	if (dim == 0) {pts[4].y = pts[5].y = 0.5f*(y1 + y2);} // yc
	else          {pts[4].x = pts[5].x = 0.5f*(x1 + x2);} // xc
	unsigned const qixs[2][2][4] = {{{0,3,5,4}, {4,5,2,1}}, {{0,4,5,1}, {4,3,2,5}}}; // 2 quads
	roof_tquads.reserve(roof_tquads.size() + (3 + (extend_dir == 2))); // 2 roof quads + 1-2 side triangles
	unsigned const tixs[2][2][3] = {{{1,0,4}, {3,2,5}}, {{0,3,4}, {2,1,5}}}; // 2 triangles

	for (unsigned n = 0; n < 2; ++n) { // triangle section/wall from z1 up to roof
		if (n == extend_dir || n == skip_side_tri) continue; // skip this side
		bool skip(0);

		// exclude tquads contained in/adjacent to other parts, considering only the cube parts;
		// yes, a triangle side can be occluded by a cube + another opposing triangle side from a higher wall of the house, but it's uncommon, complex, and currently ignored
		for (auto p = parts.begin(); p != get_real_parts_end(); ++p) {
			if (is_basement(p)) continue; // skip the basement
			if (p->d[dim][!n] != top.d[dim][n]) continue; // not the opposing face
			if (p->z1() <= z1 && p->z2() >= z2 && p->d[!dim][0] <= top.d[!dim][0] && p->d[!dim][1] >= top.d[!dim][1]) {skip = 1; break;}
		}
		if (skip) continue;
		tquad_t tquad(3); // triangle
		UNROLL_3X(tquad.pts[i_] = pts[tixs[dim][n][i_]];);
		if (z1 < bcube.z2() && tquad.pts[2][dim] != bcube.d[dim][n]) {tquad.pts[2][dim] += (n ? -1.0 : 1.0)*0.001*width;} // shift peak in slightly to prevent z-fighting with int walls
		roof_tquads.emplace_back(tquad, (unsigned)tquad_with_ix_t::TYPE_WALL); // tag as wall
	} // for n
	bool const extend_roof = 0; // disabled for now, but somewhat works

	if (extend_roof) { // extend the roof outside the wall a small amount
		// may require updating bcube for drawing; causes problems with L/T shaped houses with roof intersecting other walls and interiors; requires two sided drawing
		float const extend(0.05*width), extend_dz(2.0*extend*roof_dz/width);
		for (unsigned n = 0; n < 4; ++n) {pts[n].z -= extend_dz;} // so that slope is preserved
		if (dim == 0) {pts[0].y -= extend; pts[1].y += extend; pts[2].y += extend; pts[3].y -= extend;}
		else          {pts[0].x -= extend; pts[1].x -= extend; pts[2].x += extend; pts[3].x += extend;}
	}
	for (unsigned n = 0; n < 2; ++n) { // roof
		tquad_t tquad(4); // quad
		UNROLL_4X(tquad.pts[i_] = pts[qixs[dim][n][i_]];);
		roof_tquads.emplace_back(tquad, (unsigned)tquad_with_ix_t::TYPE_ROOF); // tag as roof
	}
	return roof_dz;
}

// roof made from two sloped quads + two sloped triangles
float building_t::gen_hipped_roof(cube_t const &top_, float peak_height, float extend_to) {

	bool const dim(get_largest_xy_dim(top_)); // always the largest dim
	cube_t top(top_); // deep copy
	extend_roof(top, extend_to, dim); // Note: return value is unused
	float const width(top.get_sz_dim(!dim)), length(top.get_sz_dim(dim)), offset(0.5f*(length - width)), roof_dz(min(peak_height*width, top.dz()));
	float const z1(top.z2()), z2(z1 + roof_dz), x1(top.x1()), y1(top.y1()), x2(top.x2()), y2(top.y2());
	assert(roof_dz > 0.0);
	point const center(0.5f*(x1 + x2), 0.5f*(y1 + y2), z2);
	point pts[6] = {point(x1, y1, z1), point(x1, y2, z1), point(x2, y2, z1), point(x2, y1, z1), center, center};
	if (dim) {UNROLL_3X(swap(pts[0], pts[i_+1]);)} // rotate 1 vertex CCW
	pts[4][dim] -= offset; pts[5][dim] += offset; // move points away from center to create ridgeline
	unsigned const qixs[2][4] = {{0,3,5,4}, {2,1,4,5}};
	unsigned const tixs[2][3] = {{1,0,4}, {3,2,5}};
	unsigned const start_ix(roof_tquads.size());
	roof_tquads.resize(start_ix + 4); // defaults to TYPE_ROOF

	for (unsigned n = 0; n < 2; ++n) {
		unsigned const ix(start_ix + n);
		roof_tquads[ix+0].npts = 4; // quads
		UNROLL_4X(roof_tquads[ix+0].pts[i_] = pts[qixs[n][i_]];)
		roof_tquads[ix+2].npts = 3; // triangles
		UNROLL_3X(roof_tquads[ix+2].pts[i_] = pts[tixs[n][i_]];)
	}
	return roof_dz;
}

void building_t::gen_building_doors_if_needed(rand_gen_t &rgen) { // for office buildings

	if (!is_cube()) return; // for now, only cube buildings can have doors; doors can be added to N-gon (non cylinder) buildings later
	assert(!parts.empty());
	float const door_height(1.1*get_door_height()), wscale(0.7); // a bit taller and a lot wider than house doors

	if (has_pri_hall()) { // building has primary hallway, place doors at both ends of first part
		for (unsigned d = 0; d < 2; ++d) {
			add_door(place_door(parts.front(), bool(hallway_dim), d, door_height, 0.0, 0.0, 0.0, wscale, 0, rgen), 0, bool(hallway_dim), d, 1);
		}
		return;
	}
	bool const pref_dim(rgen.rand_bool()), pref_dir(rgen.rand_bool());
	bool const has_windows(get_material().add_windows);
	bool used[4] = {0,0,0,0}; // per-side, not per-base cube
	unsigned const min_doors((parts.size() > 1) ? 2 : 1); // at least 2 doors unless it's a small rectangle (large rectangle will have a central hallway with doors at each end)
	unsigned const max_doors(has_windows ? 3 : 4); // buildings with windows have at most 3 doors since they're smaller
	unsigned const num_doors(min_doors + (rgen.rand() % (max_doors - min_doors + 1)));

	for (unsigned num = 0; num < num_doors; ++num) {
		bool placed(0);

		for (auto b = parts.begin(); b != get_real_parts_end() && !placed; ++b) { // try all different ground floor parts
			if (is_basement(b)) continue; // skip the basement
			unsigned const part_ix(b - parts.begin());
			if (has_windows && part_ix >= 4) break; // only first 4 parts can have doors - must match first floor window removal logic
			if (b->z1() > ground_floor_z1)   break; // moved off the ground floor

			for (unsigned n = 0; n < 4; ++n) {
				bool const dim(pref_dim ^ bool(n>>1)), dir(pref_dir ^ bool(n&1));
				if (b->d[dim][dir] != bcube.d[dim][dir]) continue; // find a side on the exterior to ensure door isn't obstructed by a building cube
				if (used[2*dim + dir]) continue; // door already placed on this side
				used[2*dim + dir] = 1; // mark used
				bool const allow_fail(!doors.empty() || b+1 != get_real_parts_end()); // allow door placement to fail if we've already placed at least one door of not last part
				placed = add_door(place_door(*b, dim, dir, door_height, 0.0, 0.0, 0.1, wscale, allow_fail, rgen), part_ix, dim, dir, 1);
				break;
			} // for n
		} // for b
	} // for num
	if (has_courtyard) { // add a door opening into the courtyard
		assert(parts.size() >= 4);
		unsigned const pref_side(rgen.rand()&3);
		bool placed(0);

		for (unsigned p = 0; p < 4 && !placed; ++p) {
			unsigned const part_ix((p + pref_side) & 3);
			cube_t const &part(parts[part_ix]);
			if (part.z1() != ground_floor_z1) continue; // not on the ground floor

			for (unsigned n = 0; n < 4; ++n) {
				bool const dim(n>>1), dir(n&1);
				if (part.d[dim][dir] == bcube.d[dim][dir] || part.d[dim][!dir] != bcube.d[dim][!dir]) continue; // find a side on the interior
				placed = add_door(place_door(part, dim, dir, door_height, 0.0, 0.0, 0.0, wscale, 1, rgen), part_ix, dim, dir, 1); // centered
			}
		} // for b
	}
}

void building_t::place_roof_ac_units(unsigned num, float sz_scale, cube_t const &bounds, vect_cube_t const &avoid, bool avoid_center, rand_gen_t &rgen) {

	if (num == 0) return;
	vector3d cube_sz(sz_scale, 1.5*sz_scale, 1.2*sz_scale); // consistent across all AC units (note that x and y will be doubled)
	if (rgen.rand_bool()) {swap(cube_sz.x, cube_sz.y);} // other orientation

	for (unsigned i = 0; i < num; ++i) {
		roof_obj_t c(ROOF_OBJ_AC);
		bool placed(0);

		for (unsigned n = 0; n < 100 && !placed; ++n) { // limited to 100 attempts to prevent infinite loop
			c.set_from_point(point(rgen.rand_uniform(bounds.x1(), bounds.x2()), rgen.rand_uniform(bounds.y1(), bounds.y2()), bounds.z2()));
			c.expand_by_xy(cube_sz);
			if (!bounds.contains_cube_xy(c)) continue; // not contained
			c.z2() += cube_sz.z; // z2
			if (has_bcube_int_no_adj(c, avoid) || has_bcube_int_no_adj(c, details)) continue; // intersects avoid cubes or other detail objects (inc_adj=0)
			if (avoid_center && c.contains_pt_xy(bounds.get_cube_center())) continue; // intersects antenna
			placed = 1;
		} // for n
		if (!placed) break; // failed, exit loop
		if ((rgen.rand() & 7) == 0) {swap(cube_sz.x, cube_sz.y);} // swap occasionally
		details.push_back(c);
	} // for n
}

void building_t::add_roof_walls(cube_t const &c, float wall_width, bool overlap_corners, cube_t out[4]) {
	// add walls around the roof; we don't need to draw all sides of all cubes, but it's probably not worth the trouble to sort it out
	float const height(6.0f*wall_width), width(wall_width), shorten(overlap_corners ? 0.0f : wall_width), z1(c.z2()), z2(c.z2()+height);
	out[0] = cube_t(c.x1(), c.x2(), c.y1(), c.y1()+width, z1, z2);
	out[1] = cube_t(c.x1(), c.x2(), c.y2()-width, c.y2(), z1, z2);
	out[2] = cube_t(c.x1(), c.x1()+width, c.y1()+shorten, c.y2()-shorten, z1, z2);
	out[3] = cube_t(c.x2()-width, c.x2(), c.y1()+shorten, c.y2()-shorten, z1, z2);
}

void subtract_part_from_walls(cube_t const &s, vect_cube_t &cubes, float wall_width) {
	unsigned iter_end(cubes.size()); // capture size before splitting

	for (unsigned i = 0; i < iter_end; ++i) {
		cube_t const &c(cubes[i]);
		if (!c.intersects_no_adj(s)) continue; // keep it
		cube_t shared(c);
		shared.intersect_with_cube(s);
		if (shared.dx() < 1.1*wall_width && shared.dy() < 1.1*wall_width) continue; // clipped off only the end of the wall (inside corner), keep entire wall
		subtract_cube_from_cube_inplace(s, cubes, i, iter_end); // Note: invalidates c reference
	} // for i
}

void merge_cubes_xy(vect_cube_t &cubes) {
	for (unsigned i = 0; i < cubes.size(); ++i) {
		for (unsigned j = 0; j < cubes.size(); ++j) {
			if (i == j) continue; // skip self
			cube_t &a(cubes[i]), &b(cubes[j]);
			if (a.is_all_zeros() || b.is_all_zeros()) continue; // one of the cubes has already been removed
			if (a.z1() != b.z1() || a.z2() != b.z2()) continue; // zvals not shared (doesn't actually happen for building walls)

			for (unsigned d = 0; d < 2; ++d) { // try to merge b into a
				if (a.d[!d][0] != b.d[!d][0] || a.d[!d][1] != b.d[!d][1]) continue; // range not shared in this dim
				if (a.d[d][1] >= b.d[d][0] && a.d[d][0] <= b.d[d][1]) {min_eq(a.d[d][0], b.d[d][0]); max_eq(a.d[d][1], b.d[d][1]); b = cube_t(); break;}
			}
		} // for j
	} // for i
}

void building_t::gen_details(rand_gen_t &rgen, bool is_rectangle) { // for the roof

	bool const flat_roof(roof_type == ROOF_TYPE_FLAT), add_walls(is_simple_cube() && flat_roof); // simple cube buildings with flat roofs
	unsigned const num_ac_units((flat_roof && is_cube() && !is_rotated()) ? (rgen.rand() % 7) : 0); // cube buildings only for now
	float const window_vspacing(get_material().get_floor_spacing()), wall_width(0.05*window_vspacing);
	assert(!parts.empty());

	if (!is_rectangle) { // polygon roof, can only add AC units
		if (add_walls && rgen.rand_bool()) { // 50% of the time
			cube_t temp[4];
			vect_cube_t cubes, to_add;

			for (auto p = parts.begin(); p != parts.end(); ++p) {
				if (p->z2() < bcube.z2()) continue; // not the top floor
				add_roof_walls(*p, wall_width, 1, temp); // overlap_corners=1 so that clipping works correctly
				cubes.resize(4);
				for (unsigned i = 0; i < 4; ++i) {cubes[i] = temp[i];}
				float const z2(cubes[0].z2());

				for (auto p2 = parts.begin(); p2 != parts.end(); ++p2) { // remove any interior wall sections that overlap another top part
					if (p2 == p || p2->z2() < p->z2()) continue; // skip self and non-top floors
					
					for (unsigned d = 0; d < 2; ++d) {
						cube_t clip_cube(*p2);
						clip_cube.z2() = z2;
						float const exp(d ? wall_width : -wall_width);
						clip_cube.expand_by(exp, -exp, 0.0);
						subtract_part_from_walls(clip_cube, cubes, wall_width);
					}
				} // for p2
				vector_add_to(cubes, to_add);
			} // for p
			merge_cubes_xy(to_add); // optimization

			for (auto i = to_add.begin(); i != to_add.end(); ++i) {
				if (!i->is_all_zeros()) {details.emplace_back(*i, (uint8_t)ROOF_OBJ_WALL);}
			}
		}
		if (num_ac_units > 0) {
			float const xy_sz(0.75*bcube.get_size().xy_mag()*rgen.rand_uniform(0.012, 0.02)); // size based on bcube

			for (auto p = parts.begin(); p != parts.end(); ++p) {
				unsigned const num_this_part(min(num_ac_units, unsigned(num_ac_units*p->dx()*p->dy()/(bcube.dx()*bcube.dy()) + 1))); // distribute based on area
				place_roof_ac_units(num_this_part, xy_sz, *p, parts, 0, rgen);
			}
		}
		details.shrink_to_fit();
		return;
	}
	cube_t const &top(parts.back()); // top/last part
	float const helipad_radius(2.0*window_vspacing);
	has_helipad = (flat_roof && num_sides >= 4 && flat_side_amt == 0.0 && !is_house && min(top.dx(), top.dy()) > (is_simple_cube() ? 3.2 : 4.0)*helipad_radius &&
		bcube.dz() > 8.0*window_vspacing && (rgen.rand() % 12) == 0);
	cube_t helipad_bcube;

	if (has_helipad) { // add helipad
		tquad_t helipad(4); // quad
		point const center(top.get_cube_center());
		float const z(top.z2() + 0.01*window_vspacing); // slightly above the roof to avoid Z-fighting
		float const x1(center.x - helipad_radius), x2(center.x + helipad_radius), y1(center.y - helipad_radius), y2(center.y + helipad_radius);
		bool const dir(rgen.rand_bool()); // R90 50% of the time
		helipad.pts[ dir+0   ].assign(x1, y1, z);
		helipad.pts[ dir+1   ].assign(x2, y1, z);
		helipad.pts[ dir+2   ].assign(x2, y2, z);
		helipad.pts[(dir+3)&3].assign(x1, y2, z);
		roof_tquads.emplace_back(helipad, (uint8_t)tquad_with_ix_t::TYPE_HELIPAD);
		helipad_bcube = helipad.get_bcube();
	}
	unsigned const num_blocks(flat_roof ? (rgen.rand() % 9) : 0); // 0-8; 0 if there are roof quads (houses, etc.)
	bool const add_antenna((flat_roof || roof_type == ROOF_TYPE_SLOPE) && !has_helipad && (rgen.rand() & 1));
	unsigned const num_details(num_blocks + num_ac_units + 4*add_walls + add_antenna);
	if (num_details == 0) return; // nothing to do
	if (add_walls && min(top.dx(), top.dy()) < 4.0*wall_width) return; // too small
	float const xy_sz(top.get_size().xy_mag()); // better to use bcube for size?
	cube_t bounds(top);
	if (add_walls) {bounds.expand_by_xy(-wall_width);}
	details.reserve(details.size() + num_details);
	vector<point> points; // reused across calls

	for (unsigned i = 0; i < num_blocks; ++i) {
		roof_obj_t c(ROOF_OBJ_BLOCK); // generic block
		float const height_scale(0.0035f*(top.dz() + bcube.dz())); // based on avg height of current section and entire building
		float const height(height_scale*rgen.rand_uniform(1.0, 4.0));
		bool placed(0);

		for (unsigned n = 0; n < 100 && !placed; ++n) { // limited to 100 attempts to prevent infinite loop
			c.set_from_point(point(rgen.rand_uniform(bounds.x1(), bounds.x2()), rgen.rand_uniform(bounds.y1(), bounds.y2()), top.z2()));
			c.expand_by_xy(vector3d(xy_sz*rgen.rand_uniform(0.01, 0.07), xy_sz*rgen.rand_uniform(0.01, 0.07), 0.0));
			if (!bounds.contains_cube_xy(c)) continue; // not contained
			if (has_helipad && c.intersects_xy(helipad_bcube)) continue; // bad placement
			placed = 1;
			if (is_simple_cube()) break; // success/done

			for (unsigned j = 0; j < 4; ++j) { // check cylinder/ellipse
				point const pt(c.d[0][j&1], c.d[1][j>>1], 0.0); // XY only
				if (!check_part_contains_pt_xy(top, pt, points)) {placed = 0; break;}
			}
		} // for n
		if (!placed) break; // failed, exit loop
		c.z2() += height; // z2
		details.push_back(c);
	} // for i
	if (num_ac_units > 0) {
		vect_cube_t avoid;
		if (has_helipad) {avoid.push_back(helipad_bcube);}
		place_roof_ac_units(num_ac_units, xy_sz*rgen.rand_uniform(0.012, 0.02), bounds, avoid, add_antenna, rgen);
	}
	if (add_walls) {
		cube_t cubes[4];
		add_roof_walls(top, wall_width, 0, cubes); // overlap_corners=0
		for (unsigned i = 0; i < 4; ++i) {details.emplace_back(cubes[i], (uint8_t)ROOF_OBJ_WALL);}
	}
	if (add_antenna) { // add antenna
		float const radius(0.003f*rgen.rand_uniform(1.0, 2.0)*(top.dx() + top.dy()));
		float const height(rgen.rand_uniform(0.25, 0.5)*top.dz());
		roof_obj_t antenna(ROOF_OBJ_ANT);
		antenna.set_from_point(top.get_cube_center());
		antenna.expand_by_xy(radius);
		antenna.z1() = top.z2(); // z1
		antenna.z2() = bcube.z2() + height; // z2 (use bcube to include sloped roof)
		details.push_back(antenna);
	}
	for (auto i = details.begin(); i != details.end(); ++i) {assert(i->is_strictly_normalized()); max_eq(bcube.z2(), i->z2());} // extend bcube z2 to contain details
	if (roof_type == ROOF_TYPE_FLAT) {gen_grayscale_detail_color(rgen, 0.2, 0.6);} // for antenna and roof
}

cube_t building_t::get_helipad_bcube() const {
	assert(has_helipad);

	for (auto i = roof_tquads.begin(); i != roof_tquads.end(); ++i) {
		if (i->type == tquad_with_ix_t::TYPE_HELIPAD) return i->get_bcube();
	}
	assert(0); // if has_helipad was set, a helipad must be found
	return cube_t(); // never gets here
}

void building_t::maybe_add_special_roof(rand_gen_t &rgen) {
	assert(!parts.empty());
	cube_t const &top(parts.back());
	vector3d const sz(top.get_size()); // top/last part

	if (global_building_params.onion_roof && num_sides >= 16 && flat_side_amt == 0.0) { // cylinder building
		if (sz.x < 1.2*sz.y && sz.y < 1.2*sz.x && sz.z > max(sz.x, sz.y)) {roof_type = ROOF_TYPE_ONION;}
	}
	else if (is_simple_cube()) { // only simple cubes are handled
		if (global_building_params.dome_roof && sz.x < 1.2*sz.y && sz.y < 1.2*sz.x && sz.z > max(sz.x, sz.y)) {roof_type = ROOF_TYPE_DOME;} // roughly square, not too short
		else {gen_sloped_roof(rgen, top);} // sloped roof
	}
	if      (roof_type == ROOF_TYPE_DOME ) {max_eq(bcube.z2(), (top.z2() + 0.5f*max(sz.x, sz.y)));}
	else if (roof_type == ROOF_TYPE_ONION) {max_eq(bcube.z2(), (top.z2() + 1.0f*max(sz.x, sz.y)));}
}
void building_t::gen_sloped_roof(rand_gen_t &rgen, cube_t const &top) { // Note: currently not supported for rotated buildings

	float const peak_height(rgen.rand_uniform(0.2, 0.5));
	float const wmin(min(top.dx(), top.dy())), z1(top.z2()), z2(z1 + peak_height*wmin), x1(top.x1()), y1(top.y1()), x2(top.x2()), y2(top.y2());
	point const pts[5] = {point(x1, y1, z1), point(x1, y2, z1), point(x2, y2, z1), point(x2, y1, z1), point(0.5f*(x1 + x2), 0.5f*(y1 + y2), z2)};
	float const d1(rgen.rand_uniform(0.0, 0.8));

	if (d1 < 0.2) { // pointed roof with 4 sloped triangles
		unsigned const ixs[4][3] = {{1,0,4}, {3,2,4}, {0,3,4}, {2,1,4}};
		roof_tquads.resize(4); // defaults to TYPE_ROOF

		for (unsigned n = 0; n < 4; ++n) {
			roof_tquads[n].npts = 3; // triangles
			UNROLL_3X(roof_tquads[n].pts[i_] = pts[ixs[n][i_]];)
		}
	}
	else { // flat roof with center quad and 4 surrounding sloped quads
		point const center((1.0 - d1)*pts[4]);
		point pts2[8];
		for (unsigned n = 0; n < 4; ++n) {pts2[n] = pts[n]; pts2[n+4] = d1*pts[n] + center;}
		unsigned const ixs[5][4] = {{4,7,6,5}, {0,4,5,1}, {3,2,6,7}, {0,3,7,4}, {2,1,5,6}}; // add the flat quad first, which works better for sphere intersections
		roof_tquads.resize(5); // defaults to TYPE_ROOF

		for (unsigned n = 0; n < 5; ++n) {
			roof_tquads[n].npts = 4; // quads
			UNROLL_4X(roof_tquads[n].pts[i_] = pts2[ixs[n][i_]];)
		}
	}
	roof_type = ROOF_TYPE_SLOPE;
	add_roof_to_bcube(); // is max_eq(bcube.z2(), z2) good enough?
	gen_grayscale_detail_color(rgen, 0.4, 0.8); // for antenna and roof
}

void building_t::add_roof_to_bcube() {
	for (auto i = roof_tquads.begin(); i != roof_tquads.end(); ++i) {i->update_bcube(bcube);} // technically should only need to update z2
}
void building_t::gen_grayscale_detail_color(rand_gen_t &rgen, float imin, float imax) {
	float const cscale(rgen.rand_uniform(imin, imax));
	detail_color = colorRGBA(cscale, cscale, cscale, 1.0);
}


// *** Interiors ***

void building_t::get_exclude_cube(point const &pos, cube_t const &skip, cube_t &exclude, bool camera_in_building) const {
	float const cube_pad(4.0*grass_width*(camera_in_building ? 2.0 : 1.0)), extent(bcube.get_max_extent());
	float dmin_sq(extent*extent); // start with a large value, squared

	for (auto p = parts.begin(); p != get_real_parts_end_inc_sec(); ++p) { // find closest part, including garages/sheds
		if (is_basement(p)) continue; // skip the basement
		if (skip.contains_cube_xy(*p)) continue; // already contained, skip
		float const dist_sq(p2p_dist_sq(pos, p->closest_pt(pos)));
		if (dist_sq < dmin_sq) {exclude = *p; dmin_sq = dist_sq;} // keep if closest part to pos
	}
	for (auto p = parts.begin(); p != get_real_parts_end(); ++p) { // try to expand the cube to cover more parts (pri parts only)
		if (is_basement(p)) continue; // skip the basement
		if (skip.contains_cube_xy(*p)) continue; // already contained, skip
		cube_t cand_ge(exclude);
		cand_ge.union_with_cube(*p);
		if (cand_ge.get_area_xy() < 1.05f*(exclude.get_area_xy() + p->get_area_xy())) {exclude = cand_ge;} // union mostly includes the two parts
	}
	if (!exclude.is_all_zeros()) {exclude.expand_by_xy(cube_pad);} // exclude grass blades that partially intersect the building interior
}

void building_t::update_grass_exclude_at_pos(point const &pos, vector3d const &xlate, bool camera_in_building) const {
	get_exclude_cube(pos, cube_t(),       grass_exclude1, camera_in_building); // first/closest cube
	get_exclude_cube(pos, grass_exclude1, grass_exclude2, camera_in_building); // second closest cube
	
	if (!driveway.is_all_zeros()) { // make driveway a grass exclude cube if one is available; this generally fails, but is a good idea if we can extend grass_exclude to more cubes
		if      (grass_exclude1.is_all_zeros()) {grass_exclude1 = driveway;}
		else if (grass_exclude2.is_all_zeros()) {grass_exclude2 = driveway;}
	}
	if (!grass_exclude1.is_all_zeros()) {grass_exclude1 = get_rotated_bcube(grass_exclude1) + xlate;}
	if (!grass_exclude2.is_all_zeros()) {grass_exclude2 = get_rotated_bcube(grass_exclude2) + xlate;}
}

void building_t::update_stats(building_stats_t &s) const { // calculate all of the counts that are easy to get
	++s.nbuildings;
	s.nparts   += parts.size();
	s.ndetails += details.size();
	s.ntquads  += roof_tquads.size();
	s.ndoors   += doors.size();
	if (!interior) return;
	++s.ninterior;
	s.nrooms  += interior->rooms.size();
	s.nceils  += interior->ceilings.size();
	s.nfloors += interior->floors.size();
	s.nwalls  += interior->walls[0].size() + interior->walls[1].size();
	s.ndoors  += interior->doors.size(); // I guess these also count as doors?
	if (!interior->room_geom) return;
	++s.nrgeom;
	s.nobjs  += interior->room_geom->objs.size();
	s.nverts += interior->room_geom->get_num_verts();
}

bool door_opens_inward(door_t const &door, cube_t const &room) {
	return (room.is_all_zeros() || (door.d[door.dim][0] < room.get_center_dim(door.dim)) == door.open_dir); // null room always returns 1 (conservative)
}
bool is_cube_close_to_door(cube_t const &c, float dmin, bool inc_open, cube_t const &door, unsigned check_dirs, bool check_zval) {
	if (check_zval && (c.z2() < door.z1() || c.z1() > door.z2())) return 0;
	bool const dim(door.dy() < door.dx());
	// we don't know how much the door is open and in which direction, so expand by door width in both dirs to be conservative
	float const width(door.get_sz_dim(!dim)), keepout(inc_open ? width : 0.0f);
	float const lo_edge(door.d[!dim][0] - ((check_dirs == 1) ? 0 : keepout)), hi_edge(door.d[!dim][1] + ((check_dirs == 0) ? 0 : keepout));
	if (c.d[!dim][0] > hi_edge || c.d[!dim][1] < lo_edge) return 0; // no overlap in !dim
	float const min_dist((dmin == 0.0f) ? width : max(width, dmin)); // if dmin==0, use door width (so that door has space to open)
	return (c.d[dim][0] < door.d[dim][1]+min_dist && c.d[dim][1] > door.d[dim][0]-min_dist); // within min_dist
}
bool building_t::is_cube_close_to_doorway(cube_t const &c, cube_t const &room, float dmin, bool inc_open) const { // Note: inc_open only applies to interior doors
	// Note: we want to test this for things like stairs, but exterior doors likely haven't been allocated at this point, so we have to check for that during door placement
	for (auto i = doors.begin(); i != doors.end(); ++i) { // test exterior doors
		if (is_cube_close_to_door(c, dmin, inc_open, i->get_bcube(), 2, 1)) return 1; // check both dirs, check_zval=1
	}
	return (interior ? interior->is_cube_close_to_doorway(c, room, dmin, inc_open, 1) : 0); // test interior doors, check_zval=1
}
bool building_interior_t::is_cube_close_to_doorway(cube_t const &c, cube_t const &room, float dmin, bool inc_open, bool check_zval) const { // ignores zvals
	for (auto i = doors.begin(); i != doors.end(); ++i) { // interior doors
		if (is_cube_close_to_door(c, dmin, (inc_open && door_opens_inward(*i, room)), *i, (i->dim ^ i->open_dir ^ 1), check_zval)) return 1;
	}
	return 0;
}

bool has_bcube_int(cube_t const &bcube, vect_stairwell_t const &stairs, float doorway_width) {
	cube_t pre_test(bcube);
	pre_test.expand_by_xy(doorway_width);

	for (auto s = stairs.begin(); s != stairs.end(); ++s) {
		if (!s->intersects(pre_test)) continue; // early termination test optimization
		cube_t tc(*s);
		tc.expand_in_dim(s->dim, doorway_width); // add extra space at both ends of stairs
		float const wall_hw(0.15*s->get_sz_dim(s->dim)/NUM_STAIRS_PER_FLOOR); // see step_len_pos logic in building_t::add_stairs_and_elevators()
		tc.expand_in_dim(!s->dim, wall_hw); // add extra space to account for walls and railings on stairs
		if (tc.intersects(bcube)) return 1;
		// extra check for objects blocking the entrance/exit to the side; this is really only needed for open ends, but helps to avoid squeezing objects behind stairs as well
		if (s->shape == SHAPE_U) continue; // U-shaped stairs are only open on one side and generally placed in hallways, so ignore

		for (unsigned e = 0; e < 2; ++e) { // for each end (entrance/exit)
			cube_t end(tc);
			end.d[s->dim][!e] = s->d[s->dim][e]; // shrink to the gap between *s and tc
			if (!s->against_wall[0]) {end.d[!s->dim][0] -= 0.75*doorway_width;}
			if (!s->against_wall[1]) {end.d[!s->dim][1] += 0.75*doorway_width;}
			if (end.intersects(bcube)) return 1;
		}
	} // for s
	return 0;
}
bool has_bcube_int(cube_t const &bcube, vector<elevator_t> const &elevators, float doorway_width) {
	for (auto e = elevators.begin(); e != elevators.end(); ++e) {
		cube_t tc(*e);
		tc.d[e->dim][e->dir] += doorway_width*(e->dir ? 1.0 : -1.0); // add extra space in front of the elevator
		if (tc.intersects(bcube)) return 1;
	}
	return 0;
}
float building_interior_t::get_doorway_width() const {
	return (doors.empty() ? 0.0 : max(doors.front().dx(), doors.front().dy())); // calculate doorway width from first door
}
bool building_interior_t::is_blocked_by_stairs_or_elevator(cube_t const &c, float dmin, bool elevators_only) const {
	cube_t tc(c);
	tc.expand_by_xy(dmin); // no pad in z
	float const doorway_width(get_doorway_width());
	if (has_bcube_int(tc, elevators, doorway_width)) return 1;
	if (elevators_only || stairwells.empty()) return 0;
	tc.z1() -= 0.001*tc.dz(); // expand slightly to avoid placing an object exactly at the top of the stairs
	return has_bcube_int(tc, stairwells, doorway_width); // must check zval to exclude stairs and elevators in parts with other z-ranges
}
// similar to above, but no expand for stairs/elevator, uses raw bcubes
bool building_interior_t::is_blocked_by_stairs_or_elevator_no_expand(cube_t const &c, float dmin) const {
	cube_t tc(c);
	tc.expand_by_xy(dmin); // no pad in z
	if (has_bcube_int(c, elevators)) return 1;
	tc.z1() -= 0.001*tc.dz(); // expand slightly to avoid placing an object exactly at the top of the stairs
	return has_bcube_int(tc, stairwells); // must check zval to exclude stairs and elevators in parts with other z-ranges
}

void building_interior_t::finalize() {
	remove_excess_cap(floors);
	remove_excess_cap(ceilings);
	remove_excess_cap(rooms);
	remove_excess_cap(doors);
	remove_excess_cap(landings);
	remove_excess_cap(stairwells);
	remove_excess_cap(elevators);
	for (unsigned d = 0; d < 2; ++d) {remove_excess_cap(walls[d]);}
}

