// 3D World - Heightmap Texture Managment
// by Frank Gennari
// 10/19/13

#include "heightmap.h"
#include "function_registry.h"
#include "inlines.h"
#include "file_utils.h"
#include "sinf.h"

using namespace std;


unsigned const TEX_EDGE_MODE = 2; // 0 = clamp, 1 = cliff/underwater, 2 = mirror

extern unsigned hmap_filter_width, erosion_iters_tt;
extern int display_mode;
extern float mesh_scale, dxdy;
extern string hmap_out_fn;


void adjust_brush_weight(float &delta, float dval, int shape) {

	if      (shape == BSHAPE_LINEAR   ) {delta *= 1.0 - dval;} // linear
	else if (shape == BSHAPE_QUADRATIC) {delta *= 1.0 - dval*dval;} // quadratic
	else if (shape == BSHAPE_COSINE   ) {delta *= COSF(0.5*PI*dval);} // cosine
	else if (shape == BSHAPE_SINE     ) {delta *= 0.5*(1.0 + SINF(PI*dval + 0.5*PI));} // sine
	// else constant
}

//enum {BSHAPE_CONST_SQ=0, BSHAPE_CNST_CIR, BSHAPE_LINEAR, BSHAPE_QUADRATIC, BSHAPE_COSINE, BSHAPE_SINE, BSHAPE_FLAT_SQ, BSHAPE_FLAT_CIR, NUM_BSHAPES};
void tex_mod_map_manager_t::hmap_brush_t::apply(tex_mod_map_manager_t *tmmm, int step_sz, unsigned num_steps) const {

	assert(num_steps > 0);
	float const step_delta(1.0/num_steps), r_inv(1.0/max(1U, radius));
	bool const is_delta(!is_flatten_brush());

	#pragma omp parallel for schedule(dynamic,1) // only ~1.8x faster
	for (int yp = y - (int)radius; yp <= y + (int)radius; yp += step_sz) {
		for (int xp = x - (int)radius; xp <= x + (int)radius; xp += step_sz) {
			for (unsigned sy = 0; sy < num_steps; ++sy) {
				for (unsigned sx = 0; sx < num_steps; ++sx) {
					float const dx(sx*step_delta), dy(sy*step_delta);
					float const dist(sqrt(float(yp + dy - y)*float(yp + dy - y) + float(xp + dx - x)*float(xp + dx - x))), dval(dist*r_inv);
					if (shape != BSHAPE_CONST_SQ && shape != BSHAPE_FLAT_SQ && dval > 1.0) continue; // round (instead of square)
					float mod_delta(delta); // constant
					adjust_brush_weight(mod_delta, dval, shape);
					assert(tmmm);
					tmmm->modify_height_value(xp, yp, round_fp(mod_delta), is_delta, dx, dy);
				}
			}
		}
	}
}


unsigned heightmap_t::get_pixel_ix(unsigned x, unsigned y) const {

	assert(is_allocated());
	assert(ncolors == 1 || ncolors == 2); // one or two byte grayscale
	assert(x < (unsigned)width && y < (unsigned)height);
	return (width*y + x);
}


unsigned heightmap_t::get_pixel_value(unsigned x, unsigned y) const {

	unsigned const ix(get_pixel_ix(x, y));
	if (ncolors == 1) {return data[ix];}
	return *((unsigned short *)(data + (ix<<1)));
}


float heightmap_t::get_heightmap_value(unsigned x, unsigned y) const { // returns values from 0 to 256

	unsigned const ix(get_pixel_ix(x, y));
	if (ncolors == 2) {return (data[ix<<1]/256.0 + data[(ix<<1)+1]);} // already high precision
	assert(ncolors == 1);
	if (hmap_filter_width == 0) {return data[ix];} // return raw low-precision value
	int const N(hmap_filter_width); // 2N+1 x 2N+1 box filter smoothing
	float v(0.0), tot(0.0);

	for (int yy = max(0, int(y)-N); yy <= min(height-1, int(y)+N); ++yy) {
		for (int xx = max(0, int(x)-N); xx <= min(width-1, int(x)+N); ++xx) {
			v   += data[width*yy + xx];
			tot += 1.0;
		}
	}
	assert(tot > 0.0);
	return v/tot;
}


void heightmap_t::modify_heightmap_value(unsigned x, unsigned y, int val, bool val_is_delta) {

	assert(is_allocated());
	assert(ncolors == 1 || ncolors == 2); // one or two byte grayscale
	assert(x < (unsigned)width && y < (unsigned)height);
	unsigned const ix(width*y + x);

	if (ncolors == 1) {
		if (val_is_delta) {val += data[ix];}
		data[ix] = max(0, min(255, val)); // clamp
	}
	else { // ncolors == 2
		unsigned short *ptr((unsigned short *)(data + (ix<<1)));
		if (val_is_delta) {val += *ptr;}
		*ptr = max(0, min(65535, val)); // clamp
	}
}


unsigned const NUM_CITIES  = 0;
unsigned const CITY_SIZE   = 384;
unsigned const CITY_BORDER = 1024;

void heightmap_t::postprocess_height() {

	if (erosion_iters_tt == 0 && CITY_SIZE == 0) return; // no erosion or cities
	assert(is_allocated());
	assert(ncolors == 1 || ncolors == 2); // one or two byte grayscale
	vector<float> vals(num_pixels());
	assert(!vals.empty());
	float min_zval(vals.front());

	for (unsigned i = 0; i < vals.size(); ++i) { // convert from pixel to heightmap value; max value is 255.0
		float &v(vals[i]);
		if (ncolors == 2) {v = (data[i<<1]/256.0 + data[(i<<1)+1]);} // 16-bit
		else {v = data[i];} // 8-bit
		v = scale_mh_texture_val(v);
		min_eq(min_zval, v);
	}
	if (erosion_iters_tt > 0) {apply_erosion(&vals.front(), width, height, min_zval, erosion_iters_tt);}

	for (unsigned n = 0; n < NUM_CITIES; ++n) {
		if (CITY_SIZE > 0) {gen_city(&vals.front(), width, height, CITY_SIZE, CITY_BORDER);}
	}
	for (unsigned i = 0; i < vals.size(); ++i) { // convert from heightmap value to pixel
		float v(unscale_mh_texture_val(vals[i]));
		assert(v >= 0.0 && v < 256.0); // must convert to [0,256) range
		if (ncolors == 2) {write_pixel_16_bits(i, v);} // 16-bit
		else {data[i] = unsigned char(v);} // 8-bit
	}
}


void tex_mod_map_manager_t::add_mod(tex_mod_vect_t const &mod) { // vector (could use a template function)
	for (tex_mod_vect_t::const_iterator i = mod.begin(); i != mod.end(); ++i) {add_mod(*i);}
}

void tex_mod_map_manager_t::add_mod(tex_mod_map_t const &mod) { // map (could use a template function)
	for (tex_mod_map_t::const_iterator i = mod.begin(); i != mod.end(); ++i) {add_mod(mod_elem_t(*i));}
}

bool tex_mod_map_manager_t::pop_last_brush(hmap_brush_t &last_brush) {

	if (brush_vect.empty()) return 0;
	last_brush = brush_vect.back();
	brush_vect.pop_back();
	return 1;
}

bool tex_mod_map_manager_t::undo_last_brush() {

	hmap_brush_t brush;
	if (!pop_last_brush(brush)) return 0; // nothing to undo
	brush.delta = -brush.delta; // invert the delta to undo
	apply_brush(brush); // apply inverse brush to cancel/undo the previous operation
	return 1;
}

unsigned const header_sig  = 0xdeadbeef;
unsigned const trailer_sig = 0xbeefdead;

bool tex_mod_map_manager_t::read_mod(string const &fn) {

	//assert(mod_map.empty()); // ???
	mod_map.clear(); // allow merging ???
	FILE *fp(fopen(fn.c_str(), "rb"));

	if (fp == NULL) {
		cerr << "Error opening terrain height mod map " << fn << " for read" << endl;
		return 0;
	}
	if (read_binary_uint(fp) != header_sig) {
		cerr << "Error: incorrect header found in terrain height mod map " << fn << "." << endl;
		return 0;
	}
	unsigned const sz(read_binary_uint(fp));

	for (unsigned i = 0; i < sz; ++i) {
		mod_elem_t elem;
		unsigned const elem_read(fread(&elem, sizeof(mod_elem_t), 1, fp)); // use a larger block?
		assert(elem_read == 1); // add error checking?
		mod_map.add(elem);
	}
	unsigned const bsz(read_binary_uint(fp));
	brush_vect.resize(bsz);

	if (!brush_vect.empty()) { // read brushes
		unsigned const elem_read(fread(&brush_vect.front(), sizeof(brush_vect_t::value_type), brush_vect.size(), fp));
		assert(elem_read == brush_vect.size()); // add error checking?
	}
	if (read_binary_uint(fp) != trailer_sig) {
		cerr << "Error: incorrect trailer found in terrain height mod map " << fn << "." << endl;
		return 0;
	}
	fclose(fp);
	return 1;
}

bool tex_mod_map_manager_t::write_mod(string const &fn) const {

	FILE *fp(fopen(fn.c_str(), "wb"));

	if (fp == NULL) {
		cerr << "Error opening terrain height mod map " << fn << " for write" << endl;
		return 0;
	}
	write_binary_uint(fp, header_sig);
	write_binary_uint(fp, mod_map.size());

	for (tex_mod_map_t::const_iterator i = mod_map.begin(); i != mod_map.end(); ++i) {
		mod_elem_t const elem(*i); // could use *i directly?
		unsigned const elem_write(fwrite(&elem, sizeof(mod_elem_t), 1, fp)); // use a larger block?
		assert(elem_write == 1); // add error checking?
	}
	write_binary_uint(fp, brush_vect.size());

	if (!brush_vect.empty()) { // write brushes
		unsigned const elem_write(fwrite(&brush_vect.front(), sizeof(brush_vect_t::value_type), brush_vect.size(), fp));
		assert(elem_write == brush_vect.size()); // add error checking?
	}
	write_binary_uint(fp, trailer_sig);
	fclose(fp);
	return 1;
}

bool terrain_hmap_manager_t::clamp_xy(int &x, int &y, float fract_x, float fract_y, bool allow_wrap) const {

	x = round_fp(mesh_scale*(x + fract_x));
	y = round_fp(mesh_scale*(y + fract_y));
	return clamp_no_scale(x, y, allow_wrap);
}

bool terrain_hmap_manager_t::clamp_no_scale(int &x, int &y, bool allow_wrap) const {

	assert(hmap.width > 0 && hmap.height > 0);
	x += hmap.width /2; // scale and offset (0,0) to texture center
	y += hmap.height/2;
	if (x >= 0 && y >= 0 && x < hmap.width && y < hmap.height) return 1; // nothing to do (optimization)
	unsigned tex_edge_mode(TEX_EDGE_MODE);
	if (!allow_wrap && tex_edge_mode == 2) {tex_edge_mode = 0;} // replace mirror with clamp

	switch (tex_edge_mode) {
	case 0: // clamp
		x = max(0, min(hmap.width -1, x));
		y = max(0, min(hmap.height-1, y));
		break;
	case 1: // cliff/underwater
		return 0; // off the texture
	case 2: // mirror
		{
			int const xmod(abs(x)%hmap.width), ymod(abs(y)%hmap.height), xdiv(x/hmap.width), ydiv(y/hmap.height);
			x = ((xdiv & 1) ? (hmap.width  - xmod - 1) : xmod);
			y = ((ydiv & 1) ? (hmap.height - ymod - 1) : ymod);
		}
		break;
	}
	return 1;
}

void terrain_hmap_manager_t::load(char const *const fn, bool invert_y) {

	assert(fn != NULL);
	cout << "Loading terrain heightmap file " << fn << endl;
	RESET_TIME;
	assert(!hmap.is_allocated()); // can only call once
	hmap = heightmap_t(0, 7, 0, 0, fn, invert_y);
	hmap.load(-1, 0, 1, 1);
	PRINT_TIME("Heightmap Load");
	hmap.postprocess_height(); // apply erosion, etc. directly after loading, before applying mod brushes
	if (!hmap_out_fn.empty()) {write_png(hmap_out_fn);}
}

bool terrain_hmap_manager_t::maybe_load(char const *const fn, bool invert_y) {

	if (fn == NULL || enabled()) return 0;
	load(fn, invert_y);
	return 1;
}

void terrain_hmap_manager_t::write_png(std::string const &fn) const {
	timer_t timer("Heightmap PNG Write");
	hmap.write_to_png(fn);
}

tex_mod_map_manager_t::hmap_val_t terrain_hmap_manager_t::get_clamped_pixel_value(int x, int y, bool allow_wrap) const {
	if (!clamp_xy(x, y, allow_wrap)) return 0; // not sure what to do in this case - can we ever get here?
	return hmap.get_pixel_value(x, y);
}

float terrain_hmap_manager_t::get_clamped_height(int x, int y) const { // translate so that (0,0) is in the center of the heightmap texture

	assert(enabled());
	if (mesh_scale < 1.0) {return interpolate_height(float(x), float(y));}
	if (mesh_scale > 1.0) {} // sample multiple pixels as a mipmap?
	if (!clamp_xy(x, y)) {return scale_mh_texture_val(0.0);} // off the texture, use min value
	return get_raw_height(x, y);
}

float terrain_hmap_manager_t::interpolate_height(float x, float y) const { // bilinear interpolation

	float const sx(mesh_scale*x), sy(mesh_scale*y);
	int xlo(floor(sx)), ylo(floor(sy)), xhi(ceil(sx)), yhi(ceil(sy));
	float const xv((xlo == xhi) ? 0.0 : (sx - xlo)/float(xhi - xlo)), yv((ylo == yhi) ? 0.0 : (sy - ylo)/float(yhi - ylo)); // avoid div-by-zero (use cubic_interpolate()?)
	if (!clamp_no_scale(xlo, ylo) || !clamp_no_scale(xhi, yhi)) {return scale_mh_texture_val(0.0);}
	return   yv *(xv*get_raw_height(xhi, yhi) + (1.0-xv)*get_raw_height(xlo, yhi)) +
		(1.0-yv)*(xv*get_raw_height(xhi, ylo) + (1.0-xv)*get_raw_height(xlo, ylo));
}

vector3d terrain_hmap_manager_t::get_norm(int x, int y) const {

	return vector3d(DY_VAL*(get_clamped_height(x, y) - get_clamped_height(x+1, y)),
			        DX_VAL*(get_clamped_height(x, y) - get_clamped_height(x, y+1)), dxdy).get_norm();
}

void terrain_hmap_manager_t::modify_height(mod_elem_t const &elem, bool is_delta) {

	assert((unsigned)max(hmap.width, hmap.height) <= max_tex_ix());
	hmap.modify_heightmap_value(elem.x, elem.y, elem.delta, is_delta);
}

tex_mod_map_manager_t::hmap_val_t terrain_hmap_manager_t::scale_delta(float delta) const {

	int const scale_factor(1 << (hmap.bytes_per_channel() << 3));
	return scale_factor*CLIP_TO_pm1(delta);
}

bool terrain_hmap_manager_t::read_and_apply_mod(string const &fn) {
	if (!tex_mod_map_manager_t::read_mod(fn)) return 0;
	apply_cur_mod_map();
	apply_cur_brushes();
	return 1;
}

void terrain_hmap_manager_t::apply_cur_mod_map() {
	for (tex_mod_map_t::const_iterator i = mod_map.begin(); i != mod_map.end(); ++i) { // apply the mod to the current texture
		assert(i->first.x < hmap.width && i->first.y < hmap.height); // ensure the mod values fit within the texture
		hmap.modify_heightmap_value(i->first.x, i->first.y, i->second.val, 1); // no clamping
	}
}

void terrain_hmap_manager_t::apply_cur_brushes() { // apply the brushes to the current texture
	for (brush_vect_t::const_iterator i = brush_vect.begin(); i != brush_vect.end(); ++i) {apply_brush(*i);}
}


