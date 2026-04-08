/*
 * Continuum — fluid.c
 *
 * Stable Fluids solver. Pure C11.
 */
#include "fluid.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------- index helpers ----------------------------------------------- */
#define IX(i, j) ((i) + (Nx) * (j))

/* ---------- small helpers ----------------------------------------------- */
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* ---------- boundary conditions -----------------------------------------
 * b == 0 : scalar (continuity / Neumann)
 * b == 1 : x-velocity (negate at vertical walls)
 * b == 2 : y-velocity (negate at horizontal walls)
 */
static void set_bnd(int N, int b, float *x) {
    int Nx = N + 2;
    for (int i = 1; i <= N; i++) {
        x[IX(0,     i)] = (b == 1) ? -x[IX(1, i)] : x[IX(1, i)];
        x[IX(N + 1, i)] = (b == 1) ? -x[IX(N, i)] : x[IX(N, i)];
        x[IX(i,     0)] = (b == 2) ? -x[IX(i, 1)] : x[IX(i, 1)];
        x[IX(i, N + 1)] = (b == 2) ? -x[IX(i, N)] : x[IX(i, N)];
    }
    x[IX(0,     0)]     = 0.5f * (x[IX(1, 0)]     + x[IX(0, 1)]);
    x[IX(0,     N + 1)] = 0.5f * (x[IX(1, N + 1)] + x[IX(0, N)]);
    x[IX(N + 1, 0)]     = 0.5f * (x[IX(N, 0)]     + x[IX(N + 1, 1)]);
    x[IX(N + 1, N + 1)] = 0.5f * (x[IX(N, N + 1)] + x[IX(N + 1, N)]);
}

/* Zero a velocity component inside obstacle cells. Scalars are left alone
 * — dye trapped inside a solid is harmless and will not be advected out
 * because the velocity inside the solid is held to zero.
 */
static void zero_in_obstacles(FluidSim *sim, float *x) {
    if (!sim->obstacle) return;
    int N = sim->N, Nx = N + 2;
    for (int j = 0; j <= N + 1; j++) {
        for (int i = 0; i <= N + 1; i++) {
            if (sim->obstacle[IX(i, j)]) x[IX(i, j)] = 0.0f;
        }
    }
}

/* ---------- Gauss–Seidel linear solver -----------------------------------
 * Pure Stam form. Used for diffusion of velocity and dye. Obstacle handling
 * for the pressure Poisson equation lives in pressure_solve below.
 */
static void lin_solve(FluidSim *sim, int b, float *x, float *x0,
                      float a, float c) {
    int   N      = sim->N;
    int   Nx     = N + 2;
    float c_recip = 1.0f / c;
    int   iters  = sim->iters;

    for (int k = 0; k < iters; k++) {
        for (int j = 1; j <= N; j++) {
            for (int i = 1; i <= N; i++) {
                x[IX(i, j)] =
                    (x0[IX(i, j)] +
                     a * (x[IX(i + 1, j)] + x[IX(i - 1, j)] +
                          x[IX(i, j + 1)] + x[IX(i, j - 1)])) *
                    c_recip;
            }
        }
        set_bnd(N, b, x);
    }
}

/* Pressure solve with obstacle-aware (no-flux / Neumann) stencil.
 *
 * Inside solid cells: pressure is undefined; we leave it at zero.
 *
 * For each fluid cell, every neighbour that is solid is treated as having
 * the same pressure as the current cell (∂p/∂n = 0). Substituting that
 * into the discrete Laplacian
 *      4·p = div + Σ p_neighbour
 * gives
 *      (4 - n_solid)·p = div + Σ p_fluid_neighbour
 * which is what we apply below. n_solid is per-cell.
 */
static void pressure_solve(FluidSim *sim, float *p, float *div) {
    int N = sim->N, Nx = N + 2;
    int iters = sim->iters;
    unsigned char *o = sim->obstacle;

    for (int k = 0; k < iters; k++) {
        for (int j = 1; j <= N; j++) {
            for (int i = 1; i <= N; i++) {
                if (o && o[IX(i, j)]) { p[IX(i, j)] = 0.0f; continue; }

                float p_l = p[IX(i - 1, j)];
                float p_r = p[IX(i + 1, j)];
                float p_d = p[IX(i, j - 1)];
                float p_u = p[IX(i, j + 1)];

                int n_fluid = 4;
                if (o) {
                    if (o[IX(i - 1, j)]) { p_l = 0.0f; n_fluid--; }
                    if (o[IX(i + 1, j)]) { p_r = 0.0f; n_fluid--; }
                    if (o[IX(i, j - 1)]) { p_d = 0.0f; n_fluid--; }
                    if (o[IX(i, j + 1)]) { p_u = 0.0f; n_fluid--; }
                }
                if (n_fluid == 0) { p[IX(i, j)] = 0.0f; continue; }

                p[IX(i, j)] =
                    (div[IX(i, j)] + p_l + p_r + p_d + p_u) /
                    (float)n_fluid;
            }
        }
        set_bnd(N, 0, p);
    }
}

/* ---------- diffuse ------------------------------------------------------ */
static void diffuse(FluidSim *sim, int b, float *x, float *x0,
                    float diff, float dt) {
    int   N = sim->N;
    float a = dt * diff * (float)N * (float)N;
    lin_solve(sim, b, x, x0, a, 1.0f + 4.0f * a);
}

/* ---------- semi-Lagrangian advection ------------------------------------ */
static void advect(FluidSim *sim, int b, float *d, float *d0,
                   float *velu, float *velv, float dt) {
    int   N   = sim->N;
    int   Nx  = N + 2;
    float dt0 = dt * (float)N;

    for (int j = 1; j <= N; j++) {
        for (int i = 1; i <= N; i++) {
            /* Solid cells have zero velocity, so backtracking lands on
             * (i, j) and the cell is just self-copying — no special case
             * needed and dye is preserved as the comment promises.
             */
            float x = (float)i - dt0 * velu[IX(i, j)];
            float y = (float)j - dt0 * velv[IX(i, j)];

            x = clampf(x, 0.5f, (float)N + 0.5f);
            y = clampf(y, 0.5f, (float)N + 0.5f);

            int   i0 = (int)x;
            int   i1 = i0 + 1;
            int   j0 = (int)y;
            int   j1 = j0 + 1;
            float s1 = x - (float)i0;
            float s0 = 1.0f - s1;
            float t1 = y - (float)j0;
            float t0 = 1.0f - t1;

            d[IX(i, j)] =
                s0 * (t0 * d0[IX(i0, j0)] + t1 * d0[IX(i0, j1)]) +
                s1 * (t0 * d0[IX(i1, j0)] + t1 * d0[IX(i1, j1)]);
        }
    }
    set_bnd(N, b, d);
}

/* ---------- pressure projection ----------------------------------------- */
static void project(FluidSim *sim, float *u, float *v, float *p, float *div) {
    int   N  = sim->N;
    int   Nx = N + 2;
    float h  = 1.0f / (float)N;
    unsigned char *o = sim->obstacle;

    /* First, snap velocities inside solids back to zero so the divergence
     * stencil sees them correctly. */
    if (o) {
        zero_in_obstacles(sim, u);
        zero_in_obstacles(sim, v);
    }

    for (int j = 1; j <= N; j++) {
        for (int i = 1; i <= N; i++) {
            div[IX(i, j)] = -0.5f * h *
                (u[IX(i + 1, j)] - u[IX(i - 1, j)] +
                 v[IX(i, j + 1)] - v[IX(i, j - 1)]);
            p[IX(i, j)] = 0.0f;
        }
    }
    set_bnd(N, 0, div);
    set_bnd(N, 0, p);

    /* Note: Stam's lin_solve uses div with the -0.5*h factor pre-baked.
     * pressure_solve expects the same convention because the algebra is
     * identical at fluid (4 fluid neighbour) cells. */
    pressure_solve(sim, p, div);

    for (int j = 1; j <= N; j++) {
        for (int i = 1; i <= N; i++) {
            if (o && o[IX(i, j)]) continue;

            float p_l = p[IX(i - 1, j)];
            float p_r = p[IX(i + 1, j)];
            float p_d = p[IX(i, j - 1)];
            float p_u = p[IX(i, j + 1)];

            /* Solid neighbours impose dp/dn = 0 → use this cell's pressure
             * so the corresponding gradient component is zero. This keeps
             * the velocity tangential to the wall. */
            if (o) {
                if (o[IX(i - 1, j)]) p_l = p[IX(i, j)];
                if (o[IX(i + 1, j)]) p_r = p[IX(i, j)];
                if (o[IX(i, j - 1)]) p_d = p[IX(i, j)];
                if (o[IX(i, j + 1)]) p_u = p[IX(i, j)];
            }

            u[IX(i, j)] -= 0.5f * (p_r - p_l) / h;
            v[IX(i, j)] -= 0.5f * (p_u - p_d) / h;
        }
    }
    set_bnd(N, 1, u);
    set_bnd(N, 2, v);
    if (o) {
        zero_in_obstacles(sim, u);
        zero_in_obstacles(sim, v);
    }
}

/* ---------- vorticity confinement --------------------------------------- */
static void vorticity_confinement(FluidSim *sim) {
    int    N    = sim->N;
    int    Nx   = N + 2;
    float *u    = sim->u, *v = sim->v;
    float *curl = sim->curl;

    /* curl = dv/dx - du/dy  (z-component of vorticity) */
    for (int j = 1; j <= N; j++) {
        for (int i = 1; i <= N; i++) {
            curl[IX(i, j)] =
                0.5f * ((v[IX(i + 1, j)] - v[IX(i - 1, j)]) -
                        (u[IX(i, j + 1)] - u[IX(i, j - 1)]));
        }
    }

    /* Force scaling: derivatives are taken in cell units (h = 1/N), so
     * the magnitude of (curl, ∇|curl|) scales as N. To make the user-
     * facing vort_strength roughly resolution-independent, multiply the
     * applied force by N. */
    float scale = (float)N;
    for (int j = 2; j < N; j++) {
        for (int i = 2; i < N; i++) {
            if (sim->obstacle && sim->obstacle[IX(i, j)]) continue;

            float dw_dx = 0.5f * (fabsf(curl[IX(i + 1, j)]) -
                                  fabsf(curl[IX(i - 1, j)]));
            float dw_dy = 0.5f * (fabsf(curl[IX(i, j + 1)]) -
                                  fabsf(curl[IX(i, j - 1)]));

            float len = sqrtf(dw_dx * dw_dx + dw_dy * dw_dy) + 1e-5f;
            dw_dx /= len;
            dw_dy /= len;

            float w = curl[IX(i, j)];
            u[IX(i, j)] += sim->dt * sim->vort_strength * scale * (dw_dy * w);
            v[IX(i, j)] -= sim->dt * sim->vort_strength * scale * (dw_dx * w);
        }
    }
}

/* ---------- buoyancy ----------------------------------------------------- */
static void apply_buoyancy(FluidSim *sim) {
    int N  = sim->N;
    int Nx = N + 2;
    if (sim->buoyancy == 0.0f) return;
    for (int j = 1; j <= N; j++) {
        for (int i = 1; i <= N; i++) {
            float dye = 0.299f * sim->r[IX(i, j)] +
                        0.587f * sim->g[IX(i, j)] +
                        0.114f * sim->b[IX(i, j)];
            /* Negative v == upward in screen coordinates. */
            sim->v[IX(i, j)] -= sim->dt * sim->buoyancy * dye;
        }
    }
}

/* ---------- step subroutines -------------------------------------------- */
#define SWAP(a, b) do { float *_t = (a); (a) = (b); (b) = _t; } while (0)

static void vel_step(FluidSim *sim) {
    SWAP(sim->u0, sim->u);
    diffuse(sim, 1, sim->u, sim->u0, sim->visc, sim->dt);
    SWAP(sim->v0, sim->v);
    diffuse(sim, 2, sim->v, sim->v0, sim->visc, sim->dt);

    project(sim, sim->u, sim->v, sim->u0, sim->v0);

    SWAP(sim->u0, sim->u);
    SWAP(sim->v0, sim->v);
    advect(sim, 1, sim->u, sim->u0, sim->u0, sim->v0, sim->dt);
    advect(sim, 2, sim->v, sim->v0, sim->u0, sim->v0, sim->dt);

    project(sim, sim->u, sim->v, sim->u0, sim->v0);
}

static void dens_step(FluidSim *sim) {
    /* RGB diffusion */
    SWAP(sim->r0, sim->r); diffuse(sim, 0, sim->r, sim->r0, sim->diff, sim->dt);
    SWAP(sim->g0, sim->g); diffuse(sim, 0, sim->g, sim->g0, sim->diff, sim->dt);
    SWAP(sim->b0, sim->b); diffuse(sim, 0, sim->b, sim->b0, sim->diff, sim->dt);

    /* RGB advection */
    SWAP(sim->r0, sim->r);
    advect(sim, 0, sim->r, sim->r0, sim->u, sim->v, sim->dt);
    SWAP(sim->g0, sim->g);
    advect(sim, 0, sim->g, sim->g0, sim->u, sim->v, sim->dt);
    SWAP(sim->b0, sim->b);
    advect(sim, 0, sim->b, sim->b0, sim->u, sim->v, sim->dt);
}

/* ---------- public API --------------------------------------------------- */

FluidSim *fluid_create(int N, float dt, float diff, float visc) {
    if (N < 4) N = 4;
    int   Nx     = N + 2;
    int   size   = Nx * Nx;
    /* 14 float fields + 1 byte mask */
    size_t bytes_f = (size_t)size * sizeof(float) * 14;
    size_t bytes_b = (size_t)size * sizeof(unsigned char);
    size_t total   = sizeof(FluidSim) + bytes_f + bytes_b + 64;

    void *backing = calloc(1, total);
    if (!backing) return NULL;

    FluidSim *sim = (FluidSim *)backing;
    sim->_backing = backing;
    sim->N      = N;
    sim->size   = size;
    sim->dt     = dt;
    sim->diff   = diff;
    sim->visc   = visc;
    sim->vort_strength = 0.05f;
    sim->buoyancy      = 0.0f;
    sim->iters         = 20;

    float *fp = (float *)((char *)backing + sizeof(FluidSim));
    sim->u    = fp + 0  * size;
    sim->v    = fp + 1  * size;
    sim->u0   = fp + 2  * size;
    sim->v0   = fp + 3  * size;
    sim->r    = fp + 4  * size;
    sim->g    = fp + 5  * size;
    sim->b    = fp + 6  * size;
    sim->r0   = fp + 7  * size;
    sim->g0   = fp + 8  * size;
    sim->b0   = fp + 9  * size;
    sim->p    = fp + 10 * size;
    sim->div  = fp + 11 * size;
    sim->curl = fp + 12 * size;
    /* slot 13 reserved scratch (kept zero) */

    sim->obstacle = (unsigned char *)(fp + 14 * size);
    return sim;
}

void fluid_destroy(FluidSim *sim) {
    if (!sim) return;
    free(sim->_backing);
}

void fluid_reset(FluidSim *sim) {
    if (!sim) return;
    int   size = sim->size;
    /* Zero everything except obstacles. */
    memset(sim->u,    0, sizeof(float) * size);
    memset(sim->v,    0, sizeof(float) * size);
    memset(sim->u0,   0, sizeof(float) * size);
    memset(sim->v0,   0, sizeof(float) * size);
    memset(sim->r,    0, sizeof(float) * size);
    memset(sim->g,    0, sizeof(float) * size);
    memset(sim->b,    0, sizeof(float) * size);
    memset(sim->r0,   0, sizeof(float) * size);
    memset(sim->g0,   0, sizeof(float) * size);
    memset(sim->b0,   0, sizeof(float) * size);
    memset(sim->p,    0, sizeof(float) * size);
    memset(sim->div,  0, sizeof(float) * size);
    memset(sim->curl, 0, sizeof(float) * size);
}

void fluid_set_viscosity(FluidSim *s, float v) { if (s) s->visc = v; }
void fluid_set_diffusion(FluidSim *s, float d) { if (s) s->diff = d; }
void fluid_set_vorticity(FluidSim *s, float v) { if (s) s->vort_strength = v; }
void fluid_set_buoyancy (FluidSim *s, float b) { if (s) s->buoyancy = b; }
void fluid_set_dt       (FluidSim *s, float dt){ if (s) s->dt = dt; }

void fluid_add_velocity(FluidSim *sim, int x, int y, float fx, float fy) {
    if (!sim) return;
    int N = sim->N, Nx = N + 2;
    if (x < 1 || x > N || y < 1 || y > N) return;
    sim->u[IX(x, y)] += fx;
    sim->v[IX(x, y)] += fy;
}

void fluid_add_dye(FluidSim *sim, int x, int y,
                   float r, float g, float b, float radius) {
    if (!sim) return;
    int N = sim->N, Nx = N + 2;
    int rad = (int)radius;
    if (rad < 0) rad = 0;
    float r2 = radius * radius;
    if (r2 < 1.0f) r2 = 1.0f;
    for (int dy = -rad; dy <= rad; dy++) {
        for (int dx = -rad; dx <= rad; dx++) {
            int xi = x + dx, yi = y + dy;
            if (xi < 1 || xi > N || yi < 1 || yi > N) continue;
            float d2 = (float)(dx * dx + dy * dy);
            float falloff = expf(-d2 / r2);
            sim->r[IX(xi, yi)] += r * falloff;
            sim->g[IX(xi, yi)] += g * falloff;
            sim->b[IX(xi, yi)] += b * falloff;
        }
    }
}

void fluid_set_obstacle(FluidSim *sim, int x, int y, int is_solid) {
    if (!sim) return;
    int N = sim->N, Nx = N + 2;
    if (x < 0 || x > N + 1 || y < 0 || y > N + 1) return;
    sim->obstacle[IX(x, y)] = is_solid ? 1 : 0;
    if (is_solid) {
        sim->u[IX(x, y)] = 0.0f;
        sim->v[IX(x, y)] = 0.0f;
        sim->r[IX(x, y)] = 0.0f;
        sim->g[IX(x, y)] = 0.0f;
        sim->b[IX(x, y)] = 0.0f;
    }
}

void fluid_clear_obstacles(FluidSim *sim) {
    if (!sim) return;
    memset(sim->obstacle, 0, (size_t)sim->size);
}

/* The main step ----------------------------------------------------------- */
void fluid_step(FluidSim *sim) {
    if (!sim) return;
    apply_buoyancy(sim);
    vorticity_confinement(sim);
    vel_step(sim);
    dens_step(sim);
    /* Diffusion is unaware of obstacles and will leak dye into solid cells.
     * Clear it; the obstacle is, by definition, opaque. */
    if (sim->obstacle) {
        zero_in_obstacles(sim, sim->r);
        zero_in_obstacles(sim, sim->g);
        zero_in_obstacles(sim, sim->b);
    }
}

/* Accessors --------------------------------------------------------------- */
int fluid_size(FluidSim *sim) { return sim ? sim->N : 0; }

#define ACC(field)                                                  \
    if (!sim) return 0.0f;                                          \
    int N = sim->N, Nx = N + 2;                                     \
    if (x < 0 || x > N + 1 || y < 0 || y > N + 1) return 0.0f;     \
    return sim->field[IX(x, y)]

float fluid_get_dye_r(FluidSim *sim, int x, int y) { ACC(r); }
float fluid_get_dye_g(FluidSim *sim, int x, int y) { ACC(g); }
float fluid_get_dye_b(FluidSim *sim, int x, int y) { ACC(b); }
float fluid_get_velocity_u(FluidSim *sim, int x, int y) { ACC(u); }
float fluid_get_velocity_v(FluidSim *sim, int x, int y) { ACC(v); }

float fluid_get_velocity_mag(FluidSim *sim, int x, int y) {
    if (!sim) return 0.0f;
    int N = sim->N, Nx = N + 2;
    if (x < 0 || x > N + 1 || y < 0 || y > N + 1) return 0.0f;
    float ux = sim->u[IX(x, y)];
    float vy = sim->v[IX(x, y)];
    return sqrtf(ux * ux + vy * vy);
}

float fluid_get_vorticity(FluidSim *sim, int x, int y) { ACC(curl); }

int fluid_get_obstacle(FluidSim *sim, int x, int y) {
    if (!sim) return 0;
    int N = sim->N, Nx = N + 2;
    if (x < 0 || x > N + 1 || y < 0 || y > N + 1) return 0;
    return sim->obstacle[IX(x, y)] ? 1 : 0;
}

void fluid_copy_dye_rgb(FluidSim *sim, float *dst) {
    if (!sim || !dst) return;
    int size = sim->size;
    const float *r = sim->r;
    const float *g = sim->g;
    const float *b = sim->b;
    for (int i = 0; i < size; i++) {
        dst[i * 3 + 0] = r[i];
        dst[i * 3 + 1] = g[i];
        dst[i * 3 + 2] = b[i];
    }
}
