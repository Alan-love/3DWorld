// 3D World
// by Frank Gennari
// 3/19/05
#pragma once

#include "3DWorld.h"

unsigned const TBITS(15), TSIZE(1 << TBITS);
float const sscale(TSIZE/TWO_PI);

extern vector<float> sin_table;

#define ST_SCALE(val) ((int(sscale*(val)))&(TSIZE-1))
#define sinf_approx(val) (((val) < 0) ? -sin_table[ST_SCALE(-(val))] : sin_table[ST_SCALE(val)])
#define cosf_approx(val) (sin_table[TSIZE+ST_SCALE(fabs(val))])

//#define SINF(val) sinf(val)
//#define COSF(val) cosf(val)
#define SINF(val) sinf_approx(val)
#define COSF(val) cosf_approx(val)

