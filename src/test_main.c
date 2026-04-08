/*
 * Continuum sanity test.
 * Creates a small simulation, injects dye and velocity, runs 100 steps,
 * and prints aggregate statistics. Used during Phase 1 to verify the
 * solver compiles, doesn't crash, and produces finite, non-trivial output.
 */
#include "fluid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const int N = 64;
    FluidSim *sim = fluid_create(N, 0.1f, 0.0001f, 0.0001f);
    if (!sim) {
        fprintf(stderr, "fluid_create failed\n");
        return 1;
    }

    fluid_set_vorticity(sim, 0.1f);
    fluid_set_buoyancy(sim, 1.5f);

    /* Inject a hot red blob at the bottom centre with upward velocity. */
    for (int s = 0; s < 100; s++) {
        fluid_add_dye(sim, N / 2, N - 4, 1.0f, 0.4f, 0.1f, 3.0f);
        fluid_add_velocity(sim, N / 2, N - 4, 0.0f, -5.0f);
        fluid_step(sim);
    }

    /* Walk the field, gather stats. */
    double sum_r = 0, sum_g = 0, sum_b = 0;
    double sum_speed = 0;
    float  max_r = 0, max_speed = 0;
    int    nan_count = 0;
    int    inside = 0;

    for (int j = 1; j <= N; j++) {
        for (int i = 1; i <= N; i++) {
            float rr = fluid_get_dye_r(sim, i, j);
            float gg = fluid_get_dye_g(sim, i, j);
            float bb = fluid_get_dye_b(sim, i, j);
            float spd = fluid_get_velocity_mag(sim, i, j);
            if (isnan(rr) || isnan(gg) || isnan(bb) || isnan(spd) ||
                isinf(rr) || isinf(gg) || isinf(bb) || isinf(spd)) {
                nan_count++;
                continue;
            }
            sum_r += rr; sum_g += gg; sum_b += bb;
            sum_speed += spd;
            if (rr > max_r) max_r = rr;
            if (spd > max_speed) max_speed = spd;
            inside++;
        }
    }

    printf("Continuum sanity test\n");
    printf("  N=%d, steps=100\n", N);
    printf("  cells inspected: %d\n", inside);
    printf("  NaN/Inf cells:   %d\n", nan_count);
    printf("  total dye R:     %.4f\n", sum_r);
    printf("  total dye G:     %.4f\n", sum_g);
    printf("  total dye B:     %.4f\n", sum_b);
    printf("  max dye R:       %.4f\n", max_r);
    printf("  total |v|:       %.4f\n", sum_speed);
    printf("  max  |v|:        %.4f\n", max_speed);

    if (nan_count > 0) {
        fprintf(stderr, "FAIL: NaN/Inf detected\n");
        return 2;
    }
    if (sum_r < 1.0f) {
        fprintf(stderr, "FAIL: dye did not propagate\n");
        return 3;
    }
    if (max_speed < 0.1f) {
        fprintf(stderr, "FAIL: velocity field not advancing\n");
        return 4;
    }

    /* --- obstacle smoke test ---------------------------------------- */
    fluid_reset(sim);
    int cx = N / 3, cy = N / 2, rad = N / 12;
    for (int j = cy - rad; j <= cy + rad; j++) {
        for (int i = cx - rad; i <= cx + rad; i++) {
            if ((i - cx) * (i - cx) + (j - cy) * (j - cy) <= rad * rad) {
                fluid_set_obstacle(sim, i, j, 1);
            }
        }
    }
    for (int s = 0; s < 60; s++) {
        for (int j = 1; j <= N; j++) {
            fluid_add_velocity(sim, 2, j, 6.0f, 0.0f);
            fluid_add_dye(sim, 2, j, 0.2f, 0.7f, 1.0f, 0.0f);
        }
        fluid_step(sim);
    }
    /* No NaN, and obstacle interior must remain dye-free since velocity
     * inside the solid is held at zero. */
    int obstacle_dirty = 0;
    int post_nan = 0;
    for (int j = 1; j <= N; j++) {
        for (int i = 1; i <= N; i++) {
            float rr = fluid_get_dye_r(sim, i, j);
            float gg = fluid_get_dye_g(sim, i, j);
            float bb = fluid_get_dye_b(sim, i, j);
            if (isnan(rr) || isnan(gg) || isnan(bb)) post_nan++;
            if (fluid_get_obstacle(sim, i, j) && (rr + gg + bb) > 1e-3f) {
                obstacle_dirty++;
            }
        }
    }
    printf("  obstacle test: nan=%d dirty=%d\n", post_nan, obstacle_dirty);
    if (post_nan > 0) {
        fprintf(stderr, "FAIL: obstacle run produced NaN\n");
        return 5;
    }
    if (obstacle_dirty > 0) {
        fprintf(stderr, "FAIL: dye leaked into solid cells\n");
        return 6;
    }

    fluid_destroy(sim);
    printf("PASS\n");
    return 0;
}
