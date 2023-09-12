#pragma once
#include "valve.hpp"

#include <deque>
#include <unordered_map>

// enum for mapping scaling variables to their data
enum scaling_variable : int {
  sensitivity = 0,
  pitch,
  yaw,
  zoom_ratio,
};

// snapshot contains all the data required to process the tick
struct snapshot_t {
  // state of usercmd
  valve::user_cmd cmd;

  // state of fov
  float fov;

  // state of zoomed
  bool zoomed;

  // state of resume_zoom
  bool resume_zoom;
};

// struct containing player data
struct player_entry_t {
  // container storing the player's last X snapshots
  std::deque< snapshot_t > snapshots;

  // map of the player's scaling_variables
  std::unordered_map< scaling_variable, float > scaling;

  // last tick a violation was registered
  int last_violation;
};

// map of edicts to player data entries
using player_store_t = std::unordered_map< void *, player_entry_t >;
