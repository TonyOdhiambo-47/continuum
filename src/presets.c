#include "presets.h"

#include <math.h>

/* Helpers ----------------------------------------------------------------- */
static void clear_for_preset(FluidSim *sim) {
    fluid_reset(sim);
    fluid_clear_obstacles(sim);
}

/* 1. Smoke plume — continuous hot dye source at bottom centre */
static void apply_smoke(FluidSim *sim) {
    clear_for_preset(sim);
    fluid_set_buoyancy(sim, 2.5f);
    fluid_set_vorticity(sim, 0.05f);
}

static void tick_smoke(FluidSim *sim, int frame) {
    int N  = fluid_size(sim);
    int cx = N / 2;
    int cy = N - 4;
    /* Subtle horizontal jitter to break the symmetry. */
    float wob = sinf(frame * 0.07f) * 1.5f;
    for (int dx = -3; dx <= 3; dx++) {
        fluid_add_dye(sim, cx + dx, cy, 1.0f, 0.4f, 0.1f, 2.0f);
        fluid_add_velocity(sim, cx + dx, cy, wob * 0.3f, -7.0f);
    }
}

/* 2. Ink drop — single radial burst from centre */
static void apply_ink(FluidSim *sim) {
    clear_for_preset(sim);
    fluid_set_buoyancy(sim, 0.0f);
    fluid_set_vorticity(sim, 0.08f);
    int N  = fluid_size(sim);
    int cx = N / 2;
    int cy = N / 2;
    fluid_add_dye(sim, cx, cy, 0.2f, 0.6f, 1.0f, 8.0f);
    for (int dy = -6; dy <= 6; dy++) {
        for (int dx = -6; dx <= 6; dx++) {
            float r = sqrtf((float)(dx * dx + dy * dy));
            if (r > 0.0f && r < 7.0f) {
                fluid_add_velocity(sim, cx + dx, cy + dy,
                                   dx / r * 6.0f, dy / r * 6.0f);
            }
        }
    }
}

static void tick_ink(FluidSim *sim, int frame) {
    (void)sim; (void)frame;
}

/* 3. Von Kármán vortex street — circular obstacle in uniform flow */
static void apply_karman(FluidSim *sim) {
    clear_for_preset(sim);
    fluid_set_buoyancy(sim, 0.0f);
    fluid_set_vorticity(sim, 0.08f);

    int N   = fluid_size(sim);
    int cx  = N / 4;
    int cy  = N / 2;
    int rad = N / 14;
    if (rad < 2) rad = 2;
    for (int j = cy - rad; j <= cy + rad; j++) {
        for (int i = cx - rad; i <= cx + rad; i++) {
            int dx = i - cx, dy = j - cy;
            if (dx * dx + dy * dy <= rad * rad) {
                fluid_set_obstacle(sim, i, j, 1);
            }
        }
    }
}

static void tick_karman(FluidSim *sim, int frame) {
    int N = fluid_size(sim);
    /* Continuous rightward flow injected at the left edge. */
    for (int j = 1; j <= N; j++) {
        fluid_add_velocity(sim, 2, j, 6.0f, 0.0f);
        /* Stripe the dye to make the shed vortices visible. */
        int band = (j / 4) % 3;
        float r = band == 0 ? 1.0f : 0.05f;
        float g = band == 1 ? 1.0f : 0.10f;
        float b = band == 2 ? 1.0f : 0.20f;
        fluid_add_dye(sim, 2, j, r * 0.6f, g * 0.6f, b * 0.6f, 0.0f);
    }
    /* Tiny perturbation downstream of the cylinder to seed shedding. */
    if (frame < 200) {
        int cx = N / 4 + N / 14 + 2;
        int cy = N / 2;
        float pert = sinf(frame * 0.15f) * 0.6f;
        fluid_add_velocity(sim, cx, cy, 0.0f, pert);
    }
}

/* 4. Kelvin–Helmholtz instability — opposing shear layers */
static void apply_kh(FluidSim *sim) {
    clear_for_preset(sim);
    fluid_set_buoyancy(sim, 0.0f);
    fluid_set_vorticity(sim, 0.05f);

    int N   = fluid_size(sim);
    int mid = N / 2;
    for (int j = 1; j <= N; j++) {
        for (int i = 1; i <= N; i++) {
            if (j < mid) {
                fluid_add_velocity(sim, i, j,  3.0f, 0.0f);
                fluid_add_dye(sim, i, j, 0.9f, 0.25f, 0.1f, 0.0f);
            } else {
                fluid_add_velocity(sim, i, j, -3.0f, 0.0f);
                fluid_add_dye(sim, i, j, 0.1f, 0.35f, 0.95f, 0.0f);
            }
        }
    }
    /* Small perturbation along the interface to trigger the instability. */
    for (int i = 1; i <= N; i++) {
        float pert = 0.6f * sinf(2.0f * 3.14159265f * (float)i / (float)N * 4.0f);
        fluid_add_velocity(sim, i, mid,     0.0f, pert);
        fluid_add_velocity(sim, i, mid + 1, 0.0f, pert);
    }
}

static void tick_kh(FluidSim *sim, int frame) {
    (void)sim; (void)frame;
}

/* Dispatch ---------------------------------------------------------------- */
void preset_apply(FluidSim *sim, PresetID id) {
    switch (id) {
        case PRESET_SMOKE:  apply_smoke(sim);  break;
        case PRESET_INK:    apply_ink(sim);    break;
        case PRESET_KARMAN: apply_karman(sim); break;
        case PRESET_KH:     apply_kh(sim);     break;
        default: clear_for_preset(sim); break;
    }
}

void preset_tick(FluidSim *sim, PresetID id, int frame) {
    switch (id) {
        case PRESET_SMOKE:  tick_smoke(sim, frame);  break;
        case PRESET_INK:    tick_ink(sim, frame);    break;
        case PRESET_KARMAN: tick_karman(sim, frame); break;
        case PRESET_KH:     tick_kh(sim, frame);     break;
        default: break;
    }
}
