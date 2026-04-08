#ifndef CONTINUUM_PRESETS_H
#define CONTINUUM_PRESETS_H

#include "fluid.h"

typedef enum {
    PRESET_NONE   = 0,
    PRESET_SMOKE  = 1,
    PRESET_INK    = 2,
    PRESET_KARMAN = 3,
    PRESET_KH     = 4
} PresetID;

/* Apply once: clears state, configures obstacles, seeds initial fields. */
void preset_apply(FluidSim *sim, PresetID id);

/* Called every frame inside the main loop, lets the preset keep injecting
 * sources (continuous flow, smoke source, etc.). Pass the current frame
 * counter so the function can phase-modulate. */
void preset_tick(FluidSim *sim, PresetID id, int frame);

#endif
