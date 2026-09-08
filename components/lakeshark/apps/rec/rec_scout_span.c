#include "rec_scout_span.h"

static const uint32_t SCOUT_SPANS_HZ[REC_SCOUT_SPAN_LEVEL_COUNT] = {
    200000u,
    100000u,
     50000u,
     25000u,
};

static int clamp_level(int level)
{
    if (level < 0) return 0;
    if (level >= REC_SCOUT_SPAN_LEVEL_COUNT)
        return REC_SCOUT_SPAN_LEVEL_COUNT - 1;
    return level;
}

uint32_t rec_scout_span_hz(int level)
{
    return SCOUT_SPANS_HZ[clamp_level(level)];
}

int rec_scout_span_cycle(int level)
{
    level = clamp_level(level);
    return (level + 1) % REC_SCOUT_SPAN_LEVEL_COUNT;
}

int rec_scout_span_zoom_in(int level)
{
    level = clamp_level(level);
    if (level < REC_SCOUT_SPAN_LEVEL_COUNT - 1) level++;
    return level;
}

int rec_scout_span_zoom_out(int level)
{
    level = clamp_level(level);
    if (level > 0) level--;
    return level;
}
