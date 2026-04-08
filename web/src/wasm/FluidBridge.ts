/*
 * Continuum — FluidBridge.ts
 *
 * Thin wrapper around the Emscripten-compiled C core. The same Stam
 * stable-fluids solver runs in the terminal binary and in the browser;
 * here we expose its API as a TypeScript class so the WebGL renderer
 * can step the simulation and read out RGB dye data each frame.
 *
 * Build the WASM artefact with `make wasm` from the project root, which
 * emits web/public/fluid.js + fluid.wasm. The module is loaded lazily.
 */

// The Emscripten ES6 module shape. We don't ship a generated .d.ts so
// we use a hand-rolled minimal interface that covers the bits we touch.
type EmModule = {
  HEAPU8:  Uint8Array;
  HEAPF32: Float32Array;
  HEAPU32: Uint32Array;
  _malloc(size: number): number;
  _free(ptr: number): void;
  _fluid_create(N: number, dt: number, diff: number, visc: number): number;
  _fluid_destroy(sim: number): void;
  _fluid_reset(sim: number): void;
  _fluid_step(sim: number): void;
  _fluid_size(sim: number): number;
  _fluid_set_viscosity(sim: number, v: number): void;
  _fluid_set_diffusion(sim: number, v: number): void;
  _fluid_set_vorticity(sim: number, v: number): void;
  _fluid_set_buoyancy (sim: number, v: number): void;
  _fluid_set_dt       (sim: number, v: number): void;
  _fluid_add_velocity(sim: number, x: number, y: number, fx: number, fy: number): void;
  _fluid_add_dye(sim: number, x: number, y: number, r: number, g: number, b: number, radius: number): void;
  _fluid_set_obstacle(sim: number, x: number, y: number, isSolid: number): void;
  _fluid_clear_obstacles(sim: number): void;
  _fluid_get_dye_r(sim: number, x: number, y: number): number;
  _fluid_get_dye_g(sim: number, x: number, y: number): number;
  _fluid_get_dye_b(sim: number, x: number, y: number): number;
  _fluid_get_velocity_u(sim: number, x: number, y: number): number;
  _fluid_get_velocity_v(sim: number, x: number, y: number): number;
  _fluid_get_velocity_mag(sim: number, x: number, y: number): number;
  _fluid_get_vorticity(sim: number, x: number, y: number): number;
  _fluid_get_obstacle(sim: number, x: number, y: number): number;
};

type EmFactory = (overrides?: Record<string, unknown>) => Promise<EmModule>;

/* Emscripten ES6 output lives at /fluid.js (in /public). Vite will not
 * bundle it; we import it as a side-effect-free dynamic URL so the WASM
 * is fetched once at runtime. */
async function loadEmscripten(): Promise<EmModule> {
  // @ts-expect-error: generated, no .d.ts
  const mod = await import(/* @vite-ignore */ "/fluid.js");
  const factory: EmFactory = mod.default;
  return factory({});
}

export interface FluidConfig {
  N:    number;  // interior grid resolution
  dt:   number;  // timestep
  diff: number;  // dye diffusion
  visc: number;  // kinematic viscosity
}

export class FluidBridge {
  private mod!: EmModule;
  private sim = 0;
  /** Reusable RGB buffer (length = (N+2)² × 3). */
  private rgbBuf!: Float32Array;
  private _N = 0;

  /** Pull dye field directly out of WASM memory by calling the per-cell
   * accessor. Slower than a single memory view but works with `-O3 -flto`
   * regardless of how Emscripten lays out the FluidSim struct. */
  async init(cfg: FluidConfig): Promise<void> {
    this.mod = await loadEmscripten();
    this.sim = this.mod._fluid_create(cfg.N, cfg.dt, cfg.diff, cfg.visc);
    if (this.sim === 0) throw new Error("fluid_create failed");
    this._N = cfg.N;
    this.rgbBuf = new Float32Array((cfg.N + 2) * (cfg.N + 2) * 3);
  }

  get N(): number { return this._N; }

  destroy(): void {
    if (this.sim) {
      this.mod._fluid_destroy(this.sim);
      this.sim = 0;
    }
  }

  reset():           void { this.mod._fluid_reset(this.sim); }
  step():            void { this.mod._fluid_step(this.sim); }

  setViscosity(v: number): void { this.mod._fluid_set_viscosity(this.sim, v); }
  setDiffusion(v: number): void { this.mod._fluid_set_diffusion(this.sim, v); }
  setVorticity(v: number): void { this.mod._fluid_set_vorticity(this.sim, v); }
  setBuoyancy (v: number): void { this.mod._fluid_set_buoyancy (this.sim, v); }
  setDt       (v: number): void { this.mod._fluid_set_dt       (this.sim, v); }

  addVelocity(x: number, y: number, fx: number, fy: number): void {
    this.mod._fluid_add_velocity(this.sim, x | 0, y | 0, fx, fy);
  }

  addDye(x: number, y: number, r: number, g: number, b: number, radius: number): void {
    this.mod._fluid_add_dye(this.sim, x | 0, y | 0, r, g, b, radius);
  }

  setObstacle(x: number, y: number, solid: boolean): void {
    this.mod._fluid_set_obstacle(this.sim, x | 0, y | 0, solid ? 1 : 0);
  }

  clearObstacles(): void { this.mod._fluid_clear_obstacles(this.sim); }

  getObstacle(x: number, y: number): boolean {
    return this.mod._fluid_get_obstacle(this.sim, x | 0, y | 0) !== 0;
  }

  /** Reads the entire RGB dye field into `rgbBuf` and returns a view of
   * it. Indexed (i, j) → ((i + (N+2)*j) * 3 + channel). */
  readDye(): Float32Array {
    const N  = this._N;
    const Nx = N + 2;
    const buf = this.rgbBuf;
    const m = this.mod;
    const sim = this.sim;
    for (let j = 0; j < Nx; j++) {
      for (let i = 0; i < Nx; i++) {
        const idx = (i + Nx * j) * 3;
        buf[idx]     = m._fluid_get_dye_r(sim, i, j);
        buf[idx + 1] = m._fluid_get_dye_g(sim, i, j);
        buf[idx + 2] = m._fluid_get_dye_b(sim, i, j);
      }
    }
    return buf;
  }
}
