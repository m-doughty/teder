#include "pcm.h"

teder_code td_pcm_generation_init(
    td_pcm_generation *g, td_arena *arena, uint64_t generation, uint64_t source, uint64_t track)
{
	if (!g || !arena || !generation || !source)
		return TEDER_INVALID;
	*g = (td_pcm_generation){
	    .arena = arena, .generation = generation, .source = source, .track = track};
	atomic_init(&g->end, TD_END_OPEN);
	atomic_init(&g->first_output, UINT64_MAX);
	return TEDER_OK;
}

teder_code td_pcm_retire(td_pcm_generation **owner)
{
	if (!owner)
		return TEDER_INVALID;
	if (!*owner)
		return TEDER_OK;
	td_pcm_generation *g = *owner;
	g->retiring = true;
	if (g->references)
		return TEDER_BUSY;
	td_arena_destroy(g->arena);
	*owner = NULL;
	return TEDER_OK;
}
