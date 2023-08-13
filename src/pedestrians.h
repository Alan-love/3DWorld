// 3D World - Buildings Interface
// by Frank Gennari
// 4-5-18
#pragma once

#include "3DWorld.h"

float const PED_WIDTH_SCALE  = 0.5; // ratio of collision radius to model radius (x/y)
float const PED_HEIGHT_SCALE = 2.5; // ratio of collision radius to model height (z)
float const EYE_HEIGHT_RATIO = 0.9; // eye is 90% of the way from the feet to the head

extern double tfticks;

struct waiting_obj_t {
	float waiting_start;
	waiting_obj_t() : waiting_start(0.0) {}
	void reset_waiting() {waiting_start = tfticks;}
	float get_wait_time_ticks() const {return (float(tfticks) - waiting_start);} // Note: only meaningful for cars stopped at lights or peds stopped at roads
	float get_wait_time_secs () const {return get_wait_time_ticks()/TICKS_PER_SECOND;}
};

struct person_base_t : public waiting_obj_t {

	point target_pos;
	vector3d dir, vel;
	point pos;
	float radius=0.0, speed=0.0, anim_time=0.0, idle_time=0.0; // Note: idle_time is currently only used for building people
	unsigned short model_id=0, ssn=0;
	int model_rand_seed=0;
	bool in_building=0, is_stopped=0, is_female=0, is_zombie=0;
	// temp state used for animations/drawing
	mutable float last_anim_state_change_time=0.0;
	mutable bool prev_was_idle=0;

	person_base_t(float radius_) : radius(radius_) {}
	std::string get_name() const;
	unsigned get_unique_id() const {return ssn;} // technically only unique if there are <= 65536 people
	float get_height () const {return PED_HEIGHT_SCALE*radius;}
	float get_width  () const {return PED_WIDTH_SCALE *radius;}
	float get_z1     () const {return (pos.z - radius);}
	float get_z2     () const {return (get_z1() + get_height());}
	cube_t get_bcube () const;
	point get_eye_pos() const {return (pos + vector3d(0.0, 0.0, (EYE_HEIGHT_RATIO*get_height() - radius)));}
	bool target_valid() const {return (target_pos != all_zeros);}
	bool is_waiting_or_stopped() const {return (speed == 0.0 || is_stopped || (in_building && waiting_start > 0));} // city peds normally have waiting_start > 0
	void set_velocity(vector3d const &v) {vel = v*(speed/v.mag());} // normalize to original velocity
	void stop();
	void go();
	void wait_for(float seconds); // for building people
	float get_idle_anim_time() const; // in animation units
	bool is_close_to_player () const;
};

struct ai_path_t : public vector<point> {
	bool uses_nav_grid=0;
	void clear() {vector<point>::clear(); uses_nav_grid = 0;}
	void add(ai_path_t const &path) {vector_add_to(path, *this); uses_nav_grid |= path.uses_nav_grid;}
};

enum {AI_STOP=0, AI_WAITING, AI_NEXT_PT, AI_BEGIN_PATH, AI_AT_DEST, AI_MOVING, AI_TO_REMOVE,
	  AI_WAIT_ELEVATOR, AI_ENTER_ELEVATOR, AI_ACTIVATE_ELEVATOR, AI_RIDE_ELEVATOR, AI_EXIT_ELEVATOR}; // elevator states
enum {GOAL_TYPE_NONE=0, GOAL_TYPE_ROOM, GOAL_TYPE_ELEVATOR, GOAL_TYPE_PLAYER, GOAL_TYPE_PLAYER_LAST_POS, GOAL_TYPE_SOUND};

struct person_t : public person_base_t { // building person
	float retreat_time=0.0;
	int cur_bldg=-1, cur_room=-1, dest_room=-1; // Note: -1 is unassigned
	unsigned short cur_rseed=1;
	uint8_t goal_type=GOAL_TYPE_NONE, cur_elevator=0, dest_elevator_floor=0, ai_state=AI_STOP;
	bool following_player=0, saw_player_hide=0, is_on_stairs=0, has_key=0, is_first_path=1, on_new_path_seg=0, last_used_elevator=0, must_re_call_elevator=0, has_room_geom=0;
	ai_path_t path; // stored backwards, next point on path is path.back()

	person_t(float radius_) : person_base_t(radius_) {in_building = 1;}
	bool on_stairs() const {return is_on_stairs;}
	bool waiting_for_same_elevator_as(person_t const &other, float floor_spacing) const;
	void next_path_pt(bool starting_path);
	void abort_dest() {target_pos = all_zeros; path.clear(); goal_type = GOAL_TYPE_NONE;}
};

