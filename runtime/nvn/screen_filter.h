// No screen filter (Link's Awakening's Game Boy looks were removed): the present shaders' hook stays, always off.
#pragma once

static inline void screen_filter_params(float pf[4]) { pf[0] = pf[1] = pf[2] = pf[3] = 0; }
#define SCREEN_FILTER_SRC(V3) ""
