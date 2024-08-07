// 3D World - OpenGL CS184 Computer Graphics Project
// by Frank Gennari
// 5/29/05

#include "3DWorld.h"
#include "shape_line3d.h"
#include "collision_detect.h"
#include "draw_utils.h"
#include <fstream>


extern int display_mode;
extern vector3d up_norm;


// ************** SHAPE3D *************


bool shape3d::alloc_shape(unsigned npoints, unsigned nfaces, unsigned ncolors) {

	if (npoints < 3 || nfaces < 1) return 0;
	points.resize(npoints);
	faces.resize(nfaces);
	colors.resize(ncolors);
	return 1;
}


// similar to the object file reader
bool shape3d::read_from_file(const char *filename) {

	float x, y, z;
	char  letter;
	unsigned ix, iy, iz, nfaces(0), nverts(0), ncolors(0), color_id;
	colorRGBA color;
	assert(filename);
	FILE *fp;
	if (!open_file(fp, filename, "shape3d")) return 0;

	if (fscanf(fp, "%c", &letter) == 1 && letter == 'n') {	
		if (fscanf(fp, "%u%u%u\n", &ncolors, &nverts, &nfaces) != 3) {
			cout << "Error reading number of vertices and faces from file '" << filename << "'." << endl;
			checked_fclose(fp);
			return 0;
		}
	}
	else { // Count the number of vertices, faces, and colors
		ungetc((int)letter, fp);
		
		while(!feof(fp)) {
			if (fscanf(fp, "%c%f%f%f", &letter, &x, &y, &z) != 4) {
				cout << "Error reading entry from file '" << filename << "'." << endl;
				checked_fclose(fp);
				return 0;
			}
			switch (letter) {
			case 'v': nverts++;  break;
			case 'f': nfaces++;  break;
			case 'c': ncolors++; break;
			default:
				cout << "Error: Invalid letter in input file: " << letter << endl;
				checked_fclose(fp);
				return 0;
			}
		}
		checked_fclose(fp);
		fp = fopen(filename, "r");
	}
	assert(fp);
	
	if (nverts < 3 || nfaces < 1) {
		cout << "Error: Mesh in file '" << filename << "' must have at least three vertices and one face: " << nverts << ", " << nfaces << "." << endl;
		checked_fclose(fp);
		return 0;
	}
	assert(alloc_shape(nverts, nfaces, ncolors));

	// Read the colors
	for(unsigned i = 0; i < ncolors; i++) {
		if (fscanf(fp, "%c%f%f%f%f%f%f%i\n", &letter, &color.R, &color.G, &color.B, &color.A, &colors[i].spec1, &colors[i].spec2, &colors[i].tid) != 8) {
			cout << "Error reading color from file '" << filename << "'." << endl;
			checked_fclose(fp);
			return 0;
		}
		if (letter != 'c') {
			cout << "Error: Expecting color " << i << " in input file but got character " << letter << " <" << int((unsigned char)letter) << ">" << endl;
			checked_fclose(fp);
			return 0;
		}
		colors[i].c = color;
	}

	// Read the vertices
	for(unsigned i = 0; i < nverts; i++) {
		if (fscanf(fp, "%c%f%f%f\n", &letter, &points[i].x, &points[i].y, &points[i].z) != 4) {
			cout << "Error reading vertex from file '" << filename << "'." << endl;
			checked_fclose(fp);
			return 0;
		}
		if (letter != 'v') { // in case vertices and faces are mixed together
			cout << "Error: Expecting vertex " << i << " in input file but got character " << letter << " <" << int((unsigned char)letter) << ">" << endl;
			checked_fclose(fp);
			return 0;
		}
	}

	// Read the faces
	for(unsigned i = 0; i < nfaces; i++) {
		if (fscanf(fp, "%c%u%u%u%u\n", &letter, &ix, &iy, &iz, &color_id) != 5) {
			cout << "Error reading face from file '" << filename << "'." << endl;
			checked_fclose(fp);
			return 0;
		}
		if (letter != 'f') { // in case vertices and faces are mixed together
			cout << "Error: Expecting face " << i << " in input file." << endl;
			checked_fclose(fp);
			return 0;
		}
		if (color_id >= max(1U, ncolors)) {
			cout << "Illegal type: " << color_id << "." << endl;
			checked_fclose(fp);
			return 0;
		}
		if (ix < 1 || iy < 1 || iz < 1 || ix > nverts || iy > nverts || iz > nverts) {
			cout << "Error: Face " << i << " references nonexistant vertex (" << ix << ", " << iy << ", " << iz << ")." << endl;
			checked_fclose(fp);
			return 0;
		}
		faces[i].v[2]     = ix-1;
		faces[i].v[1]     = iy-1;
		faces[i].v[0]     = iz-1;
		faces[i].color_id = color_id;
	}
	checked_fclose(fp);
	return 1;
}


void shape3d::gen_face_normals() {

	for (unsigned i = 0; i < faces.size(); ++i) {
		get_face_normal(i);
	}
}


void shape3d::get_face_normal(unsigned face_id) {

	assert(face_id < faces.size());
	unsigned const *const verts(faces[face_id].v);
	assert(verts != NULL);
	assert(verts[0] < points.size() && verts[1] < points.size() && verts[2] < points.size());
	get_normal(points[verts[0]], points[verts[1]], points[verts[2]], faces[face_id].norm, 1);
}


void shape3d::get_triangle_center(point &center, unsigned face_id) {

	assert(face_id < faces.size());
	unsigned const *const verts(faces[face_id].v);
	assert(verts != NULL);
	assert(verts[0] < points.size() && verts[1] < points.size() && verts[2] < points.size());
	point const p1(points[verts[0]]), p2(points[verts[1]]), p3(points[verts[2]]);
	point m1, m2;

	for (unsigned i = 0; i < 3; ++i) {
		m1[i] = 0.5f*(p1[i] + p2[i]);
		m2[i] = 0.5f*(p1[i] + p3[i]);
	}
	vector3d const v1(vector3d(m1, p3).get_norm()), v2(vector3d(m2, p2).get_norm());
	vector3d const v12(cross_product(v1, v2).get_norm()), vp2p3(p2, p3);
	float const s1(vector_determinant(vp2p3, v2, v12));///magsq12;
	float const s2(vector_determinant(vp2p3, v1, v12));///magsq12;

	for (unsigned i = 0; i < 3; ++i) {
		center[i] = 0.5f*(p3[i] + v1[i]*s1 + p2[i] + v2[i]*s2);
	}
}


void shape3d::add_vertex(unsigned vertex, unsigned face_id, unsigned &face_counter) {

	assert(face_id < faces.size());
	unsigned *verts(faces[face_id].v);
	assert(verts != NULL);
	unsigned const p0(verts[0]), p1(verts[1]), p2(verts[2]);
	assert(p0 < points.size() && p1 < points.size() && p2 < points.size());
	verts[2] = vertex;
	verts    = faces[face_counter++].v;
	verts[0] = p2;
	verts[1] = p0;
	verts[2] = vertex;
	verts    = faces[face_counter++].v;
	verts[0] = p1;
	verts[1] = p2;
	verts[2] = vertex;
}


void shape3d::get_triangle_verts(vector<vert_norm_tc> &verts) const {

	if (points.empty()) return;
	assert(points.size() >= 3 && !faces.empty());
	assert(colors.empty()); // not supported

	for (unsigned i = 0; i < faces.size(); ++i) {
		int const max_dim(get_max_dim(faces[i].norm));

		for (unsigned j = 0; j < 3; ++j) {
			unsigned const index(faces[i].v[j]);
			assert(index < points.size());
			point const p(points[index]);
			int const d1[3] = {1,0,0}, d2[3] = {2,2,1};
			verts.emplace_back((p*scale + pos), faces[i].norm, tex_scale*p[d1[max_dim]], tex_scale*p[d2[max_dim]]);
		}
	}
}


void shape3d::add_cobjs(vector<int> &cids, bool draw) {

	point points2[3];

	for (unsigned i = 0; i < faces.size(); ++i) {
		for (unsigned j = 0; j < 3; ++j) {
			points2[j] = points[faces[i].v[j]]*scale + pos;
		}
		scolor const &sc(colors[faces[i].color_id]);
		cids.push_back(add_coll_polygon(points2, 3, cobj_params(0.7, sc.c, draw, 0, NULL, 0, sc.tid), 0.0));
	}
}


void shape3d::destroy() {

	points.clear();
	faces.clear();
	colors.clear();
	color = BLACK; // reset in case it is to be used again
	pos   = all_zeros;
	remove_cobjs();
}


// ************** LINE3D *************


void line3d::draw_lines(bool fade_ends, bool no_end_draw) const {

	assert(points.size() != 1); // empty or at least one line
	assert(width > 0.0);
	static line_tquad_draw_t drawer;
	float const w(0.01*width);
	colorRGBA const end_color(color, 0.0); // fade to alpha of 0

	for (unsigned i = 1; i < points.size(); ++i) {
		bool const first(i == 1), last(i+1 == points.size());
		drawer.add_line_as_tris(points[i-1], points[i], w, w, ((fade_ends && first) ? end_color : color), ((fade_ends && last) ? end_color : color),
			(first ? nullptr : &points[i-2]), (last ? nullptr : &points[i+1]));
	}
	if (!no_end_draw) {drawer.draw_and_clear();} // uses a custom shader; if no_end_draw=1, will append but not draw
}


