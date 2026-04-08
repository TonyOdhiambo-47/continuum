# Continuum

A real-time **2D Navier–Stokes fluid simulator** that runs in your terminal **and** your browser. The same C solver powers both: compiled natively for ANSI-truecolor terminal rendering, and compiled to **WebAssembly** for a GPU-accelerated WebGL2 frontend in React.

The C core has zero external dependencies — `stdio.h`, `stdlib.h`, `math.h`, `termios.h`, that's it.

> Jos Stam's "Stable Fluids" (1999), with vorticity confinement, buoyancy, RGB multi-channel dye, and obstacle boundaries.

---

## Build & run

### Terminal

```bash
make terminal       # builds ./continuum
./continuum         # default 96×96 grid
./continuum 192     # custom resolution
```

You'll need a true-color terminal (iTerm2, Alacritty, kitty, modern macOS Terminal, modern Windows Terminal, etc.). The renderer uses Unicode half-blocks (`▀`) so each character cell shows two vertically-stacked pixels.

**Controls**

| Key             | Action                                      |
|-----------------|---------------------------------------------|
| **Left-drag**   | Inject dye + force in the drag direction    |
| **Right-drag**  | Inject force only (invisible push)          |
| `1`–`4`         | Preset scenarios                            |
| `r`             | Reset current preset                        |
| `v`             | Toggle velocity visualization               |
| `q` / `Esc`     | Quit                                        |

**Presets**

| Key | Scenario              | What you see                                      |
|-----|-----------------------|---------------------------------------------------|
| `1` | Smoke plume           | Rising thermal column with turbulent mixing       |
| `2` | Ink drop              | Expanding dye burst with vortex ring              |
| `3` | Von Kármán            | Alternating vortex shedding behind a cylinder     |
| `4` | Kelvin–Helmholtz      | Rolling instability waves on a shear boundary     |

### Browser

```bash
make wasm           # compile C core to WebAssembly (requires emscripten)
make run-web        # cd web && npm install && npm run dev
```

Then open the URL Vite prints (default `http://localhost:5173`).

The browser version runs the same C solver compiled to WASM, with a WebGL2 renderer that does bloom post-processing on top of the dye field. Sliders for viscosity, vorticity confinement, exposure, and bloom let you mess with the physics live. Resolution can be cranked up to 256×256 if your machine has the headroom.

---

## What's under the hood

```
src/fluid.h         API (one big struct, one big calloc)
src/fluid.c         The solver — Stam stable fluids + extensions
src/presets.c       Four canonical demo scenarios
src/terminal_main.c Raw-mode termios + ANSI truecolor half-block renderer
                    + SGR mouse parsing + main loop

web/                React + TypeScript + Vite + WebGL2 frontend
web/src/wasm/       FluidBridge.ts — type-safe wrapper around the WASM exports
web/src/render/     WebGLRenderer + bloom fragment shader
```

The solver runs the standard Stam timestep:

1. Apply buoyancy + vorticity confinement
2. Diffuse velocity (implicit Gauss–Seidel)
3. Project — solve the pressure Poisson equation, subtract `∇p`
4. Advect velocity with itself (semi-Lagrangian backtrace + bilinear interp)
5. Project again
6. Diffuse + advect dye

**Obstacles** are handled with a masked-stencil pressure solve: solid neighbours impose `∂p/∂n = 0` (no-flux), giving the velocity a tangential boundary condition without modifying the rest of the loop.

**Vorticity confinement** restores the small-scale curl that numerical diffusion would otherwise eat. The strength is grid-resolution-aware (force is multiplied by `N`) so the same slider value behaves the same at 64×64 and 256×256.

**WASM↔JS** uses a single bulk-copy export (`fluid_copy_dye_rgb`) writing into a malloc'd scratch buffer. The TypeScript bridge then exposes that buffer as a `Float32Array.subarray()` view directly over WASM heap — one C call per frame instead of one per cell.

---

## Build matrix

| Target  | Command           | Output                       | Toolchain        |
|---------|-------------------|------------------------------|------------------|
| Native  | `make terminal`   | `./continuum`                | cc (clang/gcc)   |
| Test    | `make test`       | runs solver sanity test      | cc               |
| WASM    | `make wasm`       | `web/public/fluid.{js,wasm}` | Emscripten       |
| Web app | `make web`        | `web/dist/`                  | Node + Vite      |
| All     | `make`            | terminal binary              | cc               |

---

## Tech

C11 · WebAssembly (Emscripten) · WebGL2 / GLSL ES 3.00 · React 18 · TypeScript · Vite

No libraries in the C code. Just math.

---

## Author

Tony Odhiambo · MIT '28
