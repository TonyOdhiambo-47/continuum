/*
 * Continuum — fluid.h
 *
 * 2D incompressible Navier–Stokes solver based on Jos Stam's
 * "Stable Fluids" (1999), with extensions:
 *   - Vorticity confinement
 *   - Buoyancy from dye concentration
 *   - RGB multi-channel dye
 *   - Static obstacle mask
 *
 * Pure C11. No dependencies beyond libc + libm.
 */
#ifndef CONTINUUM_FLUID_H
#define CONTINUUM_FLUID_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FluidSim {
    int   N;             /* interior grid resolution (N x N) */
    int   size;          /* (N+2) * (N+2) */
    float dt;            /* timestep */
    float diff;          /* dye diffusion */
    float visc;          /* kinematic viscosity */
    float vort_strength; /* vorticity confinement strength */
    float buoyancy;      /* upward force per unit dye */
    int   iters;         /* Gauss–Seidel iteration count */

    /* Velocity (current + scratch) */
    float *u,  *v;
    float *u0, *v0;

    /* Dye RGB (current + scratch) */
    float *r,  *g,  *b;
    float *r0, *g0, *b0;

    /* Pressure / divergence / vorticity scratch */
    float *p, *div;
    float *curl;

    /* Solid cell mask: 1 = solid, 0 = fluid */
    unsigned char *obstacle;

    /* Single backing allocation pointer (so destroy is one free) */
    void *_backing;
} FluidSim;

/* Lifecycle ----------------------------------------------------------------*/
FluidSim *fluid_create(int N, float dt, float diff, float visc);
void      fluid_destroy(FluidSim *sim);
void      fluid_reset(FluidSim *sim);

/* Tunables -----------------------------------------------------------------*/
void fluid_set_viscosity(FluidSim *sim, float visc);
void fluid_set_diffusion(FluidSim *sim, float diff);
void fluid_set_vorticity(FluidSim *sim, float strength);
void fluid_set_buoyancy(FluidSim *sim, float buoy);
void fluid_set_dt(FluidSim *sim, float dt);

/* Interaction --------------------------------------------------------------*/
void fluid_add_velocity(FluidSim *sim, int x, int y, float fx, float fy);
void fluid_add_dye(FluidSim *sim, int x, int y,
                   float r, float g, float b, float radius);
void fluid_set_obstacle(FluidSim *sim, int x, int y, int is_solid);
void fluid_clear_obstacles(FluidSim *sim);

/* Step ---------------------------------------------------------------------*/
void fluid_step(FluidSim *sim);

/* Accessors (renderers) ----------------------------------------------------*/
int   fluid_size(FluidSim *sim);   /* returns N */
float fluid_get_dye_r(FluidSim *sim, int x, int y);
float fluid_get_dye_g(FluidSim *sim, int x, int y);
float fluid_get_dye_b(FluidSim *sim, int x, int y);
float fluid_get_velocity_u(FluidSim *sim, int x, int y);
float fluid_get_velocity_v(FluidSim *sim, int x, int y);
float fluid_get_velocity_mag(FluidSim *sim, int x, int y);
float fluid_get_vorticity(FluidSim *sim, int x, int y);
int   fluid_get_obstacle(FluidSim *sim, int x, int y);

/* Bulk copy. Writes the entire (N+2)² dye field into `dst` interleaved
 * as RGB float triples. `dst` must point to at least (N+2)² * 3 floats.
 * Used by the WebGL renderer to avoid per-cell JS↔WASM round-trips. */
void fluid_copy_dye_rgb(FluidSim *sim, float *dst);

#ifdef __cplusplus
}
#endif

#endif /* CONTINUUM_FLUID_H */
