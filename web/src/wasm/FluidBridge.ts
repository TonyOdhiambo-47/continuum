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
  _fluid_copy_dye_rgb(sim: number, dstPtr: number): void;
};

type EmFactory = (overrides?: Record<string, unknown>) => Promise<EmModule>;

/* Emscripten ES6 output lives at <BASE_URL>/fluid.js (i.e. /public).
 * Vite will not bundle it; we import it as a side-effect-free dynamic
 * URL so the WASM is fetched once at runtime. We resolve through
 * import.meta.env.BASE_URL so the bundle works whether the site is
 * hosted at "/" or "/some/subpath/". */
async function loadEmscripten(): Promise<EmModule> {
  const base = (import.meta as { env?: { BASE_URL?: string } }).env?.BASE_URL ?? "/";
  const url  = base.replace(/\/$/, "") + "/fluid.js";
  const mod = await import(/* @vite-ignore */ url);
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
  /** Pointer (byte offset into HEAPU8) of an RGB scratch buffer in WASM
   * memory that fluid_copy_dye_rgb writes into each frame. */
  private rgbPtr = 0;
  /** Number of float entries in the RGB scratch buffer. */
  private rgbLen = 0;
  private _N = 0;

  async init(cfg: FluidConfig): Promise<void> {
    this.mod = await loadEmscripten();
    this.sim = this.mod._fluid_create(cfg.N, cfg.dt, cfg.diff, cfg.visc);
    if (this.sim === 0) throw new Error("fluid_create failed");
    this._N = cfg.N;
    /* Allocate the RGB scratch buffer once, inside WASM memory. */
    this.rgbLen = (cfg.N + 2) * (cfg.N + 2) * 3;
    this.rgbPtr = this.mod._malloc(this.rgbLen * 4 /* sizeof(float) */);
    if (this.rgbPtr === 0) throw new Error("malloc for RGB scratch failed");
  }

  get N(): number { return this._N; }

  destroy(): void {
    if (this.rgbPtr) {
      this.mod._free(this.rgbPtr);
      this.rgbPtr = 0;
    }
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

  /** Bulk-copies the entire RGB dye field into a WASM scratch buffer
   * with one C call, then returns a Float32Array VIEW directly over the
   * WASM heap (no JS-side copy). The view is invalidated whenever the
   * WASM heap is resized — callers must not retain it across `step()`.
   *
   * Note we recompute the view every frame because ALLOW_MEMORY_GROWTH
   * may have moved HEAPF32's underlying ArrayBuffer. The cost is a few
   * Float32Array constructor calls; negligible vs. the per-cell ccall
   * loop the bridge replaced.
   */
  readDye(): Float32Array {
    this.mod._fluid_copy_dye_rgb(this.sim, this.rgbPtr);
    const f32 = this.mod.HEAPF32;
    const offsetF32 = this.rgbPtr >>> 2; // byte offset → float32 index
    return f32.subarray(offsetF32, offsetF32 + this.rgbLen);
  }
}
