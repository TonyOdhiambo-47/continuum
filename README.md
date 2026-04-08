# Continuum

A 2D Navier-Stokes fluid simulator that runs in your terminal and in the browser. The same C solver does both jobs: built natively for ANSI truecolor terminal output, and compiled to WebAssembly for a WebGL2 frontend in React.

The C core has no external dependencies. Just `stdio.h`, `stdlib.h`, `math.h`, and `termios.h`.

Based on Jos Stam's "Stable Fluids" (1999), with vorticity confinement, buoyancy, RGB dye channels, and obstacle boundaries thrown in.

## Build and run

### Terminal

```bash
make terminal       # builds ./continuum
./continuum         # default 96x96 grid
./continuum 192     # custom resolution
```

You'll need a truecolor terminal (iTerm2, Alacritty, kitty, recent macOS Terminal, recent Windows Terminal). The renderer uses Unicode half-blocks (`▀`) so each character cell shows two stacked pixels.

**Controls**

| Key             | Action                                      |
|-----------------|---------------------------------------------|
| **Left-drag**   | Inject dye and force in the drag direction  |
| **Right-drag**  | Inject force only (invisible push)          |
| `1`-`4`         | Preset scenarios                            |
| `r`             | Reset current preset                        |
| `v`             | Toggle velocity visualization               |
| `q` / `Esc`     | Quit                                        |

**Presets**

| Key | Scenario          | What you see                                  |
|-----|-------------------|-----------------------------------------------|
| `1` | Smoke plume       | Rising thermal column with turbulent mixing   |
| `2` | Ink drop          | Expanding dye burst with a vortex ring        |
| `3` | Von Kármán        | Alternating vortex shedding behind a cylinder |
| `4` | Kelvin-Helmholtz  | Rolling instability waves on a shear boundary |

### Browser

```bash
make wasm           # compile C core to WebAssembly (needs emscripten)
make run-web        # cd web && npm install && npm run dev
```

Then open the URL Vite prints (default `http://localhost:5173`).

The browser version runs the same C solver as WASM, with a WebGL2 renderer that adds bloom post-processing on top of the dye field. There are sliders for viscosity, vorticity confinement, exposure, and bloom so you can play with the physics live. Resolution goes up to 256x256 if your machine can handle it.

## What's inside

```
src/fluid.h         API (one big struct, one big calloc)
src/fluid.c         The solver, Stam stable fluids plus extensions
src/presets.c       Four demo scenarios
src/terminal_main.c Raw-mode termios, ANSI truecolor half-block renderer,
                    SGR mouse parsing, main loop

web/                React + TypeScript + Vite + WebGL2 frontend
web/src/wasm/       FluidBridge.ts, type-safe wrapper around the WASM exports
web/src/render/     WebGLRenderer plus the bloom fragment shader
```

The solver runs the standard Stam timestep:

1. Apply buoyancy and vorticity confinement
2. Diffuse velocity (implicit Gauss-Seidel)
3. Project: solve the pressure Poisson equation, subtract `∇p`
4. Advect velocity with itself (semi-Lagrangian backtrace plus bilinear interp)
5. Project again
6. Diffuse and advect dye

**Obstacles** use a masked-stencil pressure solve. Solid neighbours impose `∂p/∂n = 0` (no-flux), which gives the velocity a tangential boundary condition without touching the rest of the loop.

**Vorticity confinement** puts back the small-scale curl that numerical diffusion eats. The strength scales with grid resolution (force is multiplied by `N`) so the same slider value behaves consistently at 64x64 and 256x256.

**WASM to JS** goes through a single bulk-copy export (`fluid_copy_dye_rgb`) that writes into a malloc'd scratch buffer. The TypeScript bridge exposes that buffer as a `Float32Array.subarray()` view directly over the WASM heap, so it's one C call per frame instead of one per cell.

## Build matrix

| Target  | Command           | Output                       | Toolchain        |
|---------|-------------------|------------------------------|------------------|
| Native  | `make terminal`   | `./continuum`                | cc (clang/gcc)   |
| Test    | `make test`       | runs solver sanity test      | cc               |
| WASM    | `make wasm`       | `web/public/fluid.{js,wasm}` | Emscripten       |
| Web app | `make web`        | `web/dist/`                  | Node + Vite      |
| All     | `make`            | terminal binary              | cc               |

## Tech

C11, WebAssembly (Emscripten), WebGL2 / GLSL ES 3.00, React 18, TypeScript, Vite.

## Author

Tony Odhiambo, MIT '28
