// 3D World - Building/Company Names Signs, and Flags
// by Frank Gennari 01/11/23

#include "function_registry.h"
#include "buildings.h"
#include "city_objects.h" // for sign_t

using std::string;

// names

string gen_random_name(rand_gen_t &rgen); // from Universe_name.cpp

string choose_family_name(rand_gen_t rgen) { // Note: deep copying so as not to update the state of the rgen that was passed in
	return gen_random_name(rgen); // use a generic random name to start with
}

namespace pixel_city {
// borrowed from Pixel City, by Shamus Young
// https://github.com/skeeto/pixelcity/blob/master/Texture.cpp
	static string const prefix[] = {"i", "Green ", "Mega", "Super ", "Omni", "e", "Hyper", "Global ", "Vital", "Next ", "Pacific ", "Metro", "Unity ", "G-",
							        "Trans", "Infinity ", "Superior ", "Monolith ", "Best ", "Atlantic ", "First ", "Union ", "National "};

	static string const name[] = {"Biotic", "Info", "Data", "Solar", "Aerospace", "Motors", "Nano", "Online", "Circuits", "Energy", "Med", "Robotic", "Exports", "Security",
		                          "Systems", "Financial", "Industrial", "Media", "Materials", "Foods", "Networks", "Shipping", "Tools", "Medical", "Publishing", "Enterprises",
		                          "Audio", "Health", "Bank", "Imports", "Apparel", "Petroleum", "Studios"};

	static string const suffix[] = {"Corp", " Inc.", "Co", "World", ".Com", " USA", " Ltd.", "Net", " Tech", " Labs", " Mfg.", " UK", " Unlimited", " One", " LLC"};

	string gen_company_name(rand_gen_t rgen) {
		string const cname(name[rgen.rand() % (sizeof(name) / sizeof(string))]);
		// randomly use a prefix OR suffix, but not both. Too verbose.
		if (rgen.rand_bool()) {return prefix[rgen.rand() % (sizeof(prefix) / sizeof(string))] + cname;}
		else                  {return cname + suffix[rgen.rand() % (sizeof(suffix) / sizeof(string))];}
	}
}

string choose_business_name(rand_gen_t rgen) {
	if (rgen.rand_bool()) {return pixel_city::gen_company_name(rgen);}
	int const v(rgen.rand()%10);

	if (v == 0) { // 3 letter acronym
		string name;
		for (unsigned n = 0; n < 3; ++n) {name.push_back('A' + rand()%26);}
		return name;
	}
	string const base(gen_random_name(rgen));
	switch (v) {
	case 1: return base;
	case 2: return base + (rgen.rand_bool() ? " Co" : " Company");
	case 3: return base + " Inc";
	case 4: return base + (rgen.rand_bool() ? " Ltd" : " Corp");
	case 5: return base + " & " + gen_random_name(rgen);
	case 6: return base + ", " + gen_random_name(rgen) + ", & " + gen_random_name(rgen);
	case 7: return (rgen.rand_bool() ? (rgen.rand_bool() ? "National " : "Global ") : (rgen.rand_bool() ? "United " : "American ")) + base;
	case 8: return base + (rgen.rand_bool() ? (rgen.rand_bool() ? " Bank" : " Trust") : (rgen.rand_bool() ? " Holdings" : " Industries")); // tower, if tall?
	case 9: return base + " " + gen_random_name(rgen);
	default: assert(0);
	}
	return ""; // never gets here
}

void building_t::assign_name(rand_gen_t &rgen) {
	name = (is_house ? choose_family_name(rgen) : choose_business_name(rgen));
}
string building_t::get_name_for_floor(unsigned floor_ix) const {
	if (!multi_family) return name;
	// floor_ix includes basement; adjust to be relative to ground floor
	int const floor_ix_from_ground(floor_ix - get_floor_for_zval(ground_floor_z1 + 0.5*get_window_vspace()));
	if (floor_ix_from_ground <= 0) return name;
	rand_gen_t rgen;
	rgen.set_state(hash<string>{}(name), floor_ix_from_ground); // seed with hash of original name to create a new family name for an upper floor
	return choose_family_name(rgen);
}

// signs

void gen_text_verts(vector<vert_tc_t> &verts, point const &pos, string const &text, float tsize,
	vector3d const &column_dir, vector3d const &line_dir, bool use_quads=0, bool include_space_chars=0);
void add_city_building_signs(cube_t const &city_bcube, vector<sign_t> &signs);
void add_city_building_flags(cube_t const &city_bcube, vector<city_flag_t> &flags);

class sign_helper_t {
	map<string, unsigned> txt_to_id;
	vector<string> text;
public:
	unsigned register_text(string const &t) {
		auto it(txt_to_id.find(t));
		if (it != txt_to_id.end()) return it->second; // found
		unsigned const id(text.size());
		txt_to_id[t] = id; // new text, insert it
		text.push_back(t);
		assert(text.size() == txt_to_id.size());
		return id;
	}
	string const &get_text(unsigned id) const {
		assert(id < text.size());
		return text[id];
	}
};

sign_helper_t sign_helper;

colorRGBA choose_sign_color(rand_gen_t &rgen, bool emissive=0) {
	// regular sign text colors must be dark for good contrast with the white background; emissive colors must be bright to contrast with the dark sky and buildings
	colorRGBA const sign_green(0.0, 0.7, 0.0), sign_blue(0.05, 0.05, 1.0); // adjust brightness for emissive sign colors
	colorRGBA const sign_colors    [8] = {DK_RED, DK_BLUE, DK_BLUE, DK_BROWN, BLACK, BLACK, BLACK, BLACK};
	colorRGBA const emissive_colors[5] = {RED, sign_green, sign_blue, RED, sign_blue};
	return (emissive ? emissive_colors[rgen.rand() % 5] : sign_colors[rgen.rand() % 8]);
}

unsigned register_sign_text(string const &text) {return sign_helper.register_text(text);}

// here dim and dir are the direction the text is read
float clip_char_quad(vector<vert_tc_t> &verts, unsigned start_ix, bool dim, bool dir, float lo_val, float hi_val) {
	assert(start_ix + 4 <= verts.size());
	cube_t range(verts[start_ix].v); // only X and Y are used
	float tc_lo(verts[start_ix].t[0]), tc_hi(tc_lo);
	
	for (unsigned n = 1; n < 4; ++n) { // skip first point
		vert_tc_t const &v(verts[start_ix+n]);
		range.union_with_pt(v.v);
		min_eq(tc_lo, v.t[0]);
		max_eq(tc_hi, v.t[0]);
	}
	float const width(range.get_sz_dim(dim)), width_signed((dir ? 1.0 : -1.0)*width), dtc(tc_hi - tc_lo);
	float const clip_vlo(range.d[dim][!dir] + lo_val*width_signed), clip_vhi(range.d[dim][!dir] + hi_val*width_signed);
	float const clip_tlo(tc_lo + lo_val*dtc), clip_thi(tc_lo + hi_val*dtc);
	assert(width > 0.0);
	assert(dtc   > 0.0);
	
	for (unsigned n = 0; n < 4; ++n) {
		vert_tc_t &v(verts[start_ix+n]);
		if      (v.v[dim] == range.d[dim][!dir]) {v.v[dim] = clip_vlo; v.t[0] = clip_tlo;}
		else if (v.v[dim] == range.d[dim][ dir]) {v.v[dim] = clip_vhi; v.t[0] = clip_thi;}
	}
	return clip_vlo;
}
template<typename T> void add_sign_text_verts(string const &text, cube_t const &sign, bool dim, bool dir, colorRGBA const &color,
	vector<T> &verts_out, float first_char_clip_val, float last_char_clip_val, bool include_space_chars)
{
	assert(!text.empty());
	cube_t ct(sign); // text area is slightly smaller than full cube
	ct.expand_in_dim(!dim, -0.1*ct.get_sz_dim(!dim));
	ct.expand_in_dim(2, -0.05*ct.dz());
	vector3d col_dir(zero_vector), normal(zero_vector);
	bool const ldir(dim ^ dir);
	col_dir[!dim] = (ldir  ? 1.0 : -1.0);
	normal [ dim] = (dir ? 1.0 : -1.0);
	static vector<vert_tc_t> verts;
	verts.clear();
	point pos;
	pos[dim] = ct.d[dim][dir] + (dir ? 1.0 : -1.0)*0.2*ct.get_sz_dim(dim); // shift away from the front face to prevent z-fighting
	gen_text_verts(verts, pos, text, 1.0, col_dir, plus_z, 1, include_space_chars); // use_quads=1
	assert(!verts.empty());
	if (last_char_clip_val  > 0.0) {clip_char_quad(verts, verts.size()-4, !dim, ldir, 0.0, 1.0-last_char_clip_val);}

	if (first_char_clip_val > 0.0) {
		float const left_edge(clip_char_quad(verts, 0, !dim, ldir, first_char_clip_val, 1.0)), shift(pos[!dim] - left_edge);
		for (vert_tc_t &v : verts) {v.v[!dim] += shift;} // shift so that left edge of text aligns with left edge of the sign
	}
	cube_t text_bcube(verts[0].v);
	for (auto i = verts.begin()+2; i != verts.end(); i += 2) {text_bcube.union_with_pt(i->v);} // only need to include opposite corners
	float const width_scale(ct.get_sz_dim(!dim)/text_bcube.get_sz_dim(!dim)), height_scale(ct.dz()/text_bcube.dz());
	if (dot_product(normal, cross_product((verts[1].v - verts[0].v), (verts[2].v - verts[1].v))) < 0.0) {std::reverse(verts.begin(), verts.end());} // swap vertex winding order
	color_wrapper const cw(color);
	typename T::normal_type const nc(normal); // vector3d or norm_comp

	for (auto i = verts.begin(); i != verts.end(); ++i) {
		i->v[!dim] = i->v[!dim]*width_scale + ct.d[!dim][!ldir]; // line
		i->v.z     = i->v.z*height_scale + ct.z1(); // column
		verts_out.emplace_back(i->v, nc, i->t[0], i->t[1], cw);
	}
}
template void add_sign_text_verts(string const &text, cube_t const &sign, bool dim, bool dir, colorRGBA const &color, 
	vector<vert_norm_comp_tc_color> &verts_out, float first_char_clip_val, float last_char_clip_val, bool include_space_chars);
template void add_sign_text_verts(string const &text, cube_t const &sign, bool dim, bool dir, colorRGBA const &color,
	vector<vert_norm_tc_color     > &verts_out, float first_char_clip_val, float last_char_clip_val, bool include_space_chars);

void add_sign_text_verts_both_sides(string const &text, cube_t const &sign, bool dim, bool dir, vect_vnctcc_t &verts) {
	assert(!text.empty());
	rand_gen_t rgen;
	rgen.set_state(text.size(), text.front()); // use text string as a random seed for the color
	colorRGBA const color(choose_sign_color(rgen)); // non-emissive
	add_sign_text_verts(text, sign, dim, dir, color, verts, 0.0, 0.0, 0); // no clipping or space chars
}

void add_room_obj_sign_text_verts(room_object_t const &c, colorRGBA const &color, vector<vert_norm_comp_tc_color> &verts_out) {
	add_sign_text_verts(sign_helper.get_text(c.obj_id), c, c.dim, c.dir, color, verts_out);
}

// Note: this is messy because city bcube comes from city_gen.cpp, but city buildings come from gen_buildings.cpp, neither of which has the struct definition of sign_t;
// so we need several layers of function wrappers to gather together all the pieces of data needed to add signs in this file, which includes the various required headers
void add_signs_for_city(unsigned city_id, vector<sign_t> &signs) {
	add_city_building_signs(get_city_bcube(city_id), signs);
}
void add_flags_for_city(unsigned city_id, vector<city_flag_t> &flags) {
	add_city_building_flags(get_city_bcube(city_id), flags);
}

void building_t::set_rgen_state_for_building(rand_gen_t &rgen) const {
	rgen.set_state((mat_ix + parts.size()*12345 + 1), (name[0] - 'A' + 1));
	rgen.rand_mix();
}

unsigned building_t::get_street_house_number() const { // for signs, etc.
	if (address.empty()) return 0; // error?
	return stoi(address); // street number should be first in the address
}

void building_t::add_exterior_door_items(rand_gen_t &rgen) { // mostly signs; added as interior objects
	// Note: these are interior items drawn on the exterior and expect interior lights, so they won't get sunlight;
	// but we want to generate these dynamically rather than statically because each sign text character is a separate quad
	if (is_house) { // maybe add welcome sign and add doorbell
		assert(!doors.empty());
		tquad_with_ix_t const &front_door(doors.front());
		add_doorbell_and_lamp(front_door, rgen);

		if ((rgen.rand() & 3) == 0) { // add a welcome sign to 25% of houses
			add_sign_by_door(front_door, 1, "Welcome", choose_sign_color(rgen), 0); // front door only, outside
		}
		else if (!name.empty() && name.back() != 's' && (rgen.rand() & 3) == 0) { // add a name sign to 25% of remaining houses
			add_sign_by_door(front_door, 1, ("The " + name + "s"), choose_sign_color(rgen), 0); // front door only, outside
		}
		else if (!address.empty() && rgen.rand_bool()) { // add a house number sign
			add_sign_by_door(front_door, 1, to_string(get_street_house_number()), choose_sign_color(rgen), 0); // front door only, outside
		}
	}
	else { // office building; add signs
		if (!name.empty()) {
			// Note: these will only appear when the player is very close to city office buildings, and about to walk through a door
			colorRGBA const &sign_color(choose_sign_color(rgen));

			for (auto d = doors.begin(); d != doors.end(); ++d) {
				if (has_courtyard && (d+1) == doors.end()) break; // courtyard door is not an exit
				if (d->type != tquad_with_ix_t::TYPE_BDOOR) continue; // office front doors only (not back door, roof, etc.)
				add_sign_by_door(*d, 1, name, sign_color, 0); // outside name plate sign, not emissive
			}
		}
		if (pri_hall.is_all_zeros() && rgen.rand_bool()) return; // place exit signs on buildings with primary hallways and 50% of other buildings
		colorRGBA const exit_color(rgen.rand_bool() ? RED : GREEN);

		for (auto d = doors.begin(); d != doors.end(); ++d) {
			if (has_courtyard && (d+1) == doors.end()) break; // courtyard door is not an exit
			if (!d->is_building_door()) continue; // roof door, etc.
			add_sign_by_door(*d, 0, "Exit", exit_color, 1); // inside exit sign, emissive
		}
	}
}

void building_t::add_signs(vector<sign_t> &signs) const { // added as exterior city objects
	// house welcome and other door signs are currently part of the interior - should they be? I guess at least for secondary buildings, which aren't in a city
	if (is_house) { // add address sign above porch; should be 3-4 digits
		if (address.empty()) return; // address not set - not in a city
		if (street_dir == 0) return; // street_dir not set - shouldn't happen if address is set
		if (!has_porch())    return;
		cube_t porch_roof, sign;

		for (auto i = get_real_parts_end_inc_sec(); i != parts.end(); ++i) {
			if (i->contains_cube_xy(porch)) {porch_roof = *i; break;} // find the porch roof; should be the first part visited
		}
		assert(!porch_roof.is_all_zeros()); // must be found
		bool const dim((street_dir-1)>>1), dir((street_dir-1)&1); // direction to the road; should be on the exterior side of the porch
		float const sign_hwidth(0.08*porch.get_sz_dim(!dim)), sign_height(0.75*sign_hwidth), sign_depth(0.025*sign_height);
		if (porch_roof.dz() < sign_height) {set_cube_zvals(sign, (porch_roof.z2() - sign_height), porch_roof.z2());} // hang from the top edge of the porch roof
		else {set_wall_width(sign, porch_roof.zc(), 0.5*sign_height, 2);} // center on the porch roof
		set_wall_width(sign, porch.get_center_dim(!dim), sign_hwidth, !dim);
		sign.d[dim][!dir] = porch.d[dim][dir]; // back face
		sign.d[dim][ dir] = sign.d[dim][!dir] + (dir ? 1.0 : -1.0)*sign_depth; // front face
		assert(sign.is_strictly_normalized());
		signs.emplace_back(sign, dim, dir, to_string(get_street_house_number()), WHITE, BLACK, 0, 0, 1); // twp_sided=0, emissive=0, small=1
		return;
	}
	if (name.empty())  return; // no company name; shouldn't get here
	if (num_sides & 1) return; // odd number of sides, may not be able to place a sign correctly (but maybe we can check this with a collision test with conn?)
	if (half_offset || flat_side_amt != 0.0 || alt_step_factor != 0.0 || start_angle != 0.0) return; // not a shape that's guanrateed to reach the bcube edge
	rand_gen_t rgen;
	set_rgen_state_for_building(rgen);
	// find the highest part, widest if tied, and place the sign on top of it
	assert(!parts.empty());
	float part_zmax(bcube.z1()), best_width(0.0), wall_pos[2] = {}, center_pos(0.0);
	bool dim(0), pri_dir(rgen.rand_bool());
	auto parts_end((real_num_parts > 0) ? get_real_parts_end() : parts.end()); // if real_num_parts hasn't been set yet, assume it includes all parts (for non-city office buildings)

	for (auto i = parts.begin(); i != parts_end; ++i) {
		// choose max dim; or should we use dim from street_dir if set? but that would limit us for corner buildings
		bool const dmax(i->dx() < i->dy());
		float const width(i->get_sz_dim(dmax));
		
		if (i->z2() > part_zmax || (i->z2() == part_zmax && width > best_width)) {
			dim        = !dmax; // points in smaller dim
			part_zmax  = i->z2();
			best_width = width;
			center_pos = i->get_center_dim(!dim);
			UNROLL_2X(wall_pos[i_] = i->d[dim][i_];);
		}
	} // for i
	assert(best_width > 0.0);

	if (street_dir > 0) { // encoded as 2*dim + dir + 1; 0 is unassigned
		bool const sdim((street_dir - 1) >> 1), sdir((street_dir - 1) & 1);
		if (dim == sdim) {pri_dir = sdir;} // face the street if possible
	}
	bool sign_both_sides(rgen.rand_bool());
	bool const two_sided(!sign_both_sides && 0);
	bool const emissive(is_in_city && rgen.rand_float() < 0.65);
	bool const scrolling(emissive && name.size() >= 8 && rgen.rand_float() < 0.75);
	// non-cube buildings can have signs tangent to a point or curve and need proper connectors; also, only cube buildings have roof walls that connectors may clip through
	bool const add_connector(!is_cube());
	float const width(bcube.get_sz_dim(!dim)*(is_in_city ? 1.0 : 0.67)), sign_hwidth(0.5*min(0.8*best_width, 0.5*width)); // city office building signs are larger
	float const sign_height(4.0*sign_hwidth/(name.size() + 2)), sign_depth(0.025*sign_height);
	colorRGBA const color(choose_sign_color(rgen, emissive));
	float sign_z1(part_zmax);
	if (!add_connector) {sign_z1 -= 1.2*sign_depth;} // shift down slightly to ensure some ext wall adjacency in case there's no roof wall
	cube_t sign;
	set_cube_zvals(sign, sign_z1, (sign_z1 + sign_height));
	set_wall_width(sign, center_pos, sign_hwidth, !dim);

	for (unsigned d = 0; d < 2; ++d) {
		bool const dir(pri_dir ^ bool(d));
		float const wpos(wall_pos[dir]);
		if (has_courtyard && wpos != bcube.d[dim][dir]) continue; // no signs in the courtyard
		sign.d[dim][!dir] = wpos;
		sign.d[dim][ dir] = wpos + (dir ? 1.0 : -1.0)*sign_depth;
		assert(sign.is_strictly_normalized());
		bool bad_place(0);

		for (auto i = parts.begin(); i != parts_end; ++i) { // check for interior split edges
			if (i->z2() == part_zmax && i->intersects_xy_no_adj(sign)) {bad_place = 1; break;}
		}
		if (bad_place) continue; // Note: intentionally skips the break below
		signs.emplace_back(sign, dim, dir, name, WHITE, color, two_sided, emissive, 0, scrolling); // small=0

		if (add_connector) {
			cube_t conn(sign);
			conn.z2() = sign.z1() + 0.01*sign_height;
			conn.expand_in_dim(!dim, -0.94*sign_hwidth);
			conn.d[dim][!dir] = wpos - (dir ? 1.0 : -1.0)*(4.0*sign_depth + get_wall_thickness());
			conn.d[dim][ dir] = wpos;
			assert(conn.is_strictly_normalized());
			signs.back().connector = conn;
		}
		if (sign_both_sides) break; // one side only - done
	} // for d
}

void building_t::add_company_sign(rand_gen_t &rgen) {
	if (is_house || name.empty()) return; // shouldn't be called?
	if (is_in_city)               return; // already has a sign added as a city object
	if (rgen.rand_bool())         return; // only add sign 50% of the time
	vector<sign_t> signs;
	add_signs(signs); // should add office building rooftop signs only

	for (sign_t const &sign : signs) {
		assert(!sign.emissive && !sign.scrolling); // not supported
		assert(sign.text == name);
		details.emplace_back(sign.bcube, ROOF_OBJ_SIGN);
		if (!sign.connector.is_all_zeros()) {details.emplace_back(sign.connector, ROOF_OBJ_SIGN_CONN);} // never added for this type of building?
	}
}

void building_t::add_sign_by_door(tquad_with_ix_t const &door, bool outside, std::string const &text, colorRGBA const &color, bool emissive) { // interior signs
	assert(!text.empty());
	cube_t const door_bcube(door.get_bcube());
	bool const dim(door_bcube.dy() < door_bcube.dx());
	int const dir_ret(get_ext_door_dir(door_bcube, dim));
	if (dir_ret > 1) return; // not found, skip sign
	bool dir(dir_ret != 0);
	float const width(door_bcube.get_sz_dim(!dim)), height(door_bcube.dz());
	cube_t c(door_bcube);

	if (outside) { // outside, place above the door
		c.z2() = door_bcube.z2() + 0.1*height;
	}
	else { // inside, place hanging near the top of the door
		c.z2() = door_bcube.z1() + get_window_vspace() - get_fc_thickness(); // right against the ceiling
	}
	c.z1() = c.z2() - 0.05*height;
	float const sign_width(0.8*text.size()*c.dz()), shrink(0.5f*(width - sign_width));
	c.expand_in_dim(!dim, -shrink);
	if (!outside) {dir ^= 1; c.translate_dim(dim, (dir ? 1.0 : -1.0)*0.1*height);} // move inside the building
	c.d[dim][dir] += (dir ? 1.0 : -1.0)*0.01*height;

	if (outside) {
		for (auto p2 = get_real_parts_end_inc_sec(); p2 != parts.end(); ++p2) {
			if (p2->intersects(c)) return; // sign intersects porch roof, skip this building
		}
	}
	unsigned const flags(RO_FLAG_LIT | RO_FLAG_NOCOLL | (emissive ? RO_FLAG_EMISSIVE : 0) | (outside ? RO_FLAG_EXTERIOR : RO_FLAG_HANGING));
	vect_room_object_t &objs(interior->room_geom->objs);
	objs.emplace_back(c, TYPE_SIGN, 0, dim, dir, flags, 1.0, SHAPE_CUBE, color); // always lit; room_id is not valid
	objs.back().obj_id = register_sign_text(text);
}

city_flag_t create_flag(bool dim, bool dir, point const &base_pt, float height, float length) {
	float const width(0.5*length), pradius(0.05*length), thickness(0.1*pradius), pole_top(base_pt.z + height);
	cube_t flag;
	set_cube_zvals(flag, (pole_top - width), pole_top);
	set_wall_width(flag, base_pt[dim], thickness, dim); // flag thickness
	flag.d[!dim][!dir] = base_pt[!dim] + (dir ? 1.0 : -1.0)*0.5*pradius; // starts flush with the pole inside edge
	flag.d[!dim][ dir] = base_pt[!dim] + (dir ? 1.0 : -1.0)*length; // end extends in dir
	return city_flag_t(flag, dim, dir, base_pt, pradius);
}
void building_t::add_flags(vector<city_flag_t> &flags) const {
	rand_gen_t rgen;
	set_rgen_state_for_building(rgen);

	if (is_house) {
		if (street_dir == 0) return; // street_dir not set
		if (rgen.rand_float() > 0.15) return; // add a flag 15% of the time
		bool const dim((street_dir-1)>>1), dir((street_dir-1)&1);
		cube_t part;

		for (auto i = parts.begin(); i != get_real_parts_end(); ++i) {
			if (i->d[dim][dir] == bcube.d[dim][dir]) {part = *i; break;} // find first part on front side of house
		}
		assert(!part.is_all_zeros());
		float const floor_spacing(get_window_vspace());
		if (part.dz() < 1.5*floor_spacing) return; // single floor; skip flag so that it doesn't block people, the door, etc.
		float const length(0.65*floor_spacing), width(0.5*length), pradius(0.03*length), thickness(0.1*pradius), pole_len(2.5*width);
		point base_pt;
		base_pt[ dim] = part.d[dim][dir];
		base_pt[!dim] = part.get_center_dim(!dim);
		base_pt.z     = part.z2() - 0.15*length;
		cube_t flag; // sticks out and hangs vertically
		set_cube_zvals(flag, (base_pt.z - length), base_pt.z);
		set_wall_width(flag, base_pt[!dim], thickness, !dim); // flag thickness
		flag.d[dim][!dir] = base_pt[dim] + (dir ? 1.0 : -1.0)*(pole_len - width); // end near wall
		flag.d[dim][ dir] = base_pt[dim] + (dir ? 1.0 : -1.0)*pole_len; // far end
		flags.emplace_back(flag, !dim, dir, base_pt, pradius);
		return;
	}
	if (has_helipad || !skylights.empty()) return; // flag may block the helipad or skylight
	if (roof_type != ROOF_TYPE_SLOPE) return; // only sloped roofs, since the flag is more visible
	if (rgen.rand_bool()) return; // place a flag 50% of the time
	// we can place signs on roof of the tallest part, or on the ground next to the building, on on the building wall; here we add them to the roof
	// find the highest part, largest area if tied, and place the sign on top of it
	assert(!parts.empty());
	float best_area(0.0);
	bool dim(0);
	point base_pt(0.0, 0.0, bcube.z1());

	for (auto i = parts.begin(); i != get_real_parts_end(); ++i) {
		float const area(i->get_area_xy());
		if (i->z2() <= base_pt.z && !(i->z2() == base_pt.z && area > best_area)) continue; // not better
		dim       = (i->dy() < i->dx()); // points in smaller dim
		best_area = area;
		base_pt.assign(i->xc(), i->yc(), i->z2());
	} // for i
	if (best_area == 0.0) return; // no valid placement

	for (roof_obj_t const &ro : details) {
		if (ro.contains_pt_xy(base_pt)) return;
	}
	bool const dir(rgen.rand_bool());
	float const window_spacing(get_window_vspace()), length(1.0*window_spacing*rgen.rand_uniform(0.8, 1.25));
	// assume the tallest part has the roof that sets the bcube and place the flag with that height_add
	float const height_add(bcube.z2() - base_pt.z), height( 2.0*window_spacing*rgen.rand_uniform(0.8, 1.25) + height_add);
	flags.push_back(create_flag(dim, dir, base_pt, height, length));
}


