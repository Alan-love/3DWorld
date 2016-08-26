// 3D World - Drawing Code
// by Frank Gennari
// 3/10/02
#include "3DWorld.h"
#include "function_registry.h"
#include "shaders.h"


string const default_font_texture_atlas_fn = "textures/atlas/text_atlas.png";
string font_texture_atlas_fn(default_font_texture_atlas_fn);

extern int window_height, display_mode, draw_model;


struct per_char_data_t {
	float u1, u2, v1, v2; // texture coords
	float width; // horizontal size/step size

	per_char_data_t(float u1_=0, float u2_=0, float v1_=0, float v2_=0, float width_=1) : u1(u1_), u2(u2_), v1(v1_), v2(v2_), width(width_) {}
};


class font_texture_manager_t {

	texture_t texture;
	per_char_data_t pcd[256]; // one per ASCII value

public:
	font_texture_manager_t() : texture(0, 7, 0, 0, 0, 4, 3, "", 0, 0) {} // custom alpha mipmaps, uncompressed

	bool check_nonempty_tile_column(unsigned tx, unsigned ty, unsigned x, unsigned tsize) const {
		unsigned char const *data(texture.get_data());

		for (unsigned y = 0; y < tsize; ++y) { // iterate over one character tile
			unsigned const pixel((ty*tsize + y)*texture.width + tx*tsize + x);
			if (data[(pixel<<2)+3] != 0) return 1; // check alpha component
		}
		return 0;
	}

	// Note: expects to find a square texture with 16x16 tiles, one per ASCII value, starting from the ULC
	void load(string const &fn) {
		texture.free_data();
		texture.name = (fn.empty() ? font_texture_atlas_fn : fn);
		texture.no_avg_color_alpha_fill = 1; // since we set alpha equal to luminance
		texture.load(-1);
		assert(texture.ncolors == 4); // RGBA (really only need RA though)
		assert(texture.width == texture.height);
		assert((texture.width & 15) == 0); // multiple of 16
		float const duv(1.0/16.0), pw(1.0/texture.width);
		unsigned const tsize(texture.width >> 4); // tile size
		unsigned char *data(texture.get_data());

		for (unsigned i = 0; i < texture.num_pixels(); ++i) {
			unsigned weight(0);
			UNROLL_3X(weight += data[4*i+i_];)
			data[4*i+3] = (unsigned char)(weight/3.0); // set alpha equal to luminance
			UNROLL_3X(data[4*i+i_] = 255;) // set luminance/RGB to 1
		}
		for (unsigned ty = 0; ty < 16; ++ty) {
			for (unsigned tx = 0; tx < 16; ++tx) {
				// calculate kerning by looking for all black/transparent columns
				unsigned col_start(0), col_end(tsize/2); // non-printable chars are half width
				unsigned const ty_inv(15-ty), dix((ty_inv<<4) + tx); // index into pcd

				for (unsigned x = 0; x < tsize; ++x) { // forward iterate over one character tile
					if (check_nonempty_tile_column(tx, ty, x, tsize)) {col_start = x; break;}
				}
				for (unsigned x = tsize; x > col_start; --x) { // backward iterate over one character tile
					if (check_nonempty_tile_column(tx, ty, x-1, tsize)) {col_end = x; break;}
				}
				float const width(float(col_end - col_start)/float(tsize));
				pcd[dix] = per_char_data_t(tx*duv+pw*col_start, (tx+1)*duv-pw*(tsize - col_end), ty*duv, (ty+1)*duv-pw, width);
			} // for tx
		} // for ty
	}
	void bind_gl() {
		texture.check_init();
		texture.bind_gl();
	}
	per_char_data_t const &lookup_ascii(unsigned char val) const {assert(val < 256); return pcd[val];}
	void free_gl_state() {texture.gl_delete();}
};

font_texture_manager_t font_texture_manager; // singleton

void load_font_texture_atlas(string const &fn) {font_texture_manager.load(fn);}
void free_font_texture_atlas() {font_texture_manager.free_gl_state();}


void gen_text_verts(vector<vert_tc_t> &verts, point const &pos, string const &text, float tsize, vector3d const &column_dir, vector3d const &line_dir) {

	float const line_spacing = 1.25;
	float const char_spacing = 0.06;
	float const char_sz(0.001*tsize);
	vector3d const line_delta(-line_dir*line_spacing*char_sz);
	point cursor(pos), line_start(cursor);

	for (string::const_iterator i = text.begin(); i != text.end(); ++i) {
		if (*i == '\n') { // newline (CR/LF)
			line_start += line_delta; // LF
			cursor      = line_start; // CR
		}
		else {
			per_char_data_t const &pcd(font_texture_manager.lookup_ascii(*i));
			if (pcd.width == 0.0) continue; // non-printable character, skip it (currently never get here, but left for future use)
			float const char_width(char_sz*pcd.width);
			float const t[4][2] = {{pcd.u1,pcd.v1}, {pcd.u2,pcd.v1}, {pcd.u2,pcd.v2}, {pcd.u1,pcd.v2}};
			float const dx[4] = {0.0, char_width, char_width, 0.0};
			float const dy[4] = {0.0, 0.0, char_sz, char_sz};

			for (unsigned i = 0; i < 6; ++i) {
				unsigned const ix(quad_to_tris_ixs[i]);
				verts.emplace_back((cursor + column_dir*dx[ix] + line_dir*dy[ix]), t[ix][0], t[ix][1]);
			}
			cursor += column_dir*(char_width + char_sz*char_spacing);
		}
	}
}


void begin_text_draw(shader_t &s, colorRGBA const *const color=nullptr) {

	ensure_filled_polygons();
	glDisable(GL_DEPTH_TEST);
	enable_blend();
	s.begin_simple_textured_shader(0.1, 0, 0, color);
	font_texture_manager.bind_gl();
}

void end_text_draw(shader_t &s) {

	s.end_shader();
	disable_blend();
	glEnable(GL_DEPTH_TEST);
	reset_fill_mode();
}


void draw_bitmap_text(colorRGBA const &color, point const &pos, string const &text, float tsize) {

	if (text.empty()) return; // error?
	shader_t s;
	begin_text_draw(s, &color);
	vector<vert_tc_t> verts;
	gen_text_verts(verts, pos, text, tsize, plus_x, plus_y);
	draw_verts(verts, GL_TRIANGLES);
	end_text_draw(s);
}


void draw_text(colorRGBA const &color, float x, float y, float z, char const *text, float tsize) {
	draw_bitmap_text(color, point(x, y, z), text, 0.8*tsize);
}


void text_drawer_t::draw() const {

	if (strs.empty()) return;
	shader_t s;
	begin_text_draw(s);
	vector<vert_tc_t> verts;
	vector3d const tdir(cross_product(get_vdir_all(), get_upv_all())); // screen space x
	colorRGBA last_color(ALPHA0);

	for (auto i = strs.begin(); i != strs.end(); ++i) {
		if (i->color != last_color) {
			draw_and_clear_verts(verts, GL_TRIANGLES);
			s.set_cur_color(i->color);
			last_color = i->color;
		}
		gen_text_verts(verts, i->pos, i->str, i->size, tdir, up_vector);
	}
	draw_and_clear_verts(verts, GL_TRIANGLES);
	end_text_draw(s);
}

