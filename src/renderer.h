#ifndef TEDER_RENDERER_H
#define TEDER_RENDERER_H
#include "teder_notcurses.h"
#include "memory.h"
/* Private allocator seam. Its context outlives the renderer arena. */
teder_code td_renderer_create_with(const teder_renderer_config *, td_allocator, teder_renderer **);
#endif
