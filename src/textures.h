// 3D World - OpenGL CS184 Computer Graphics Project
// by Frank Gennari
// 8/27/02
#pragma once

#include "3DWorld.h"

int const NTEX_DIRT = 5;

struct ttex {
	int id;
	float zval;
};

extern ttex lttex_dirt[];

#define RGB_BLOCK_COPY(dst, src)           {dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];            }
#define RGB_BLOCK_ASSIGN(dst, R, G, B)     {dst[0] = R;      dst[1] = G;      dst[2] = B;                 }
#define RGBA_BLOCK_ASSIGN(dst, R, G, B, A) {dst[0] = R;      dst[1] = G;      dst[2] = B;      dst[3] = A;}

// fixed-point math
#define BLEND_COLOR(dst, s1, s2, bv) \
       unsigned const bvi(unsigned(256.0f*bv)), om_bvi(256 - bvi); \
       UNROLL_3X(dst[i_] = (unsigned char)((bvi*s1[i_] + om_bvi*s2[i_]) >> 8);)

inline void unpack_color(unsigned char dst[3], colorRGBA const& src) {
	UNROLL_3X(dst[i_] = (unsigned char)(255.0f * src[i_]);)
}

