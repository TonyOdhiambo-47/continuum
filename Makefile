# Continuum — build system
#
# Targets:
#   make terminal      → native terminal binary  (default if make all)
#   make wasm          → WASM core for the web frontend (requires emcc)
#   make web           → wasm + npm install + npm run build
#   make test          → headless solver sanity test
#   make run-terminal  → build and launch the terminal renderer
#   make run-web       → build wasm and launch the Vite dev server
#   make clean         → remove build artefacts
#
# Native uses cc by default so it works with both clang (macOS) and gcc.

CC      ?= cc
EMCC    ?= emcc
CFLAGS  ?= -O3 -Wall -Wextra -std=c11 -flto
LDLIBS  ?= -lm

CORE_SRC = src/fluid.c
TERM_SRC = src/fluid.c src/presets.c src/terminal_main.c
TEST_SRC = src/fluid.c src/test_main.c

WASM_OUT     = web/public/fluid.js
WASM_EXPORTS = "['_fluid_create','_fluid_destroy','_fluid_reset','_fluid_step',\
'_fluid_add_velocity','_fluid_add_dye','_fluid_set_obstacle','_fluid_clear_obstacles',\
'_fluid_set_viscosity','_fluid_set_diffusion','_fluid_set_vorticity','_fluid_set_buoyancy','_fluid_set_dt',\
'_fluid_get_dye_r','_fluid_get_dye_g','_fluid_get_dye_b','_fluid_get_velocity_u','_fluid_get_velocity_v',\
'_fluid_get_velocity_mag','_fluid_get_vorticity','_fluid_get_obstacle','_fluid_size',\
'_fluid_copy_dye_rgb',\
'_malloc','_free']"
WASM_RUNTIME = "['ccall','cwrap','HEAPU8','HEAPF32','HEAPU32']"
WASM_FLAGS   = -O3 -flto \
               -s WASM=1 \
               -s MODULARIZE=1 \
               -s EXPORT_ES6=1 \
               -s ENVIRONMENT=web \
               -s ALLOW_MEMORY_GROWTH=1 \
               -s INITIAL_MEMORY=33554432 \
               -s EXPORTED_FUNCTIONS=$(WASM_EXPORTS) \
               -s EXPORTED_RUNTIME_METHODS=$(WASM_RUNTIME)

.PHONY: all terminal wasm web test run-terminal run-web clean

all: terminal

terminal:
	$(CC) $(CFLAGS) $(TERM_SRC) -o continuum $(LDLIBS)

test:
	$(CC) $(CFLAGS) $(TEST_SRC) -o continuum_test $(LDLIBS)
	./continuum_test

wasm:
	@command -v $(EMCC) >/dev/null || { echo "emcc not found — install emscripten"; exit 1; }
	@mkdir -p $(dir $(WASM_OUT))
	$(EMCC) $(WASM_FLAGS) $(CORE_SRC) -o $(WASM_OUT)

web: wasm
	cd web && npm install && npm run build

run-terminal: terminal
	./continuum

run-web: wasm
	cd web && npm install && npm run dev

clean:
	rm -f continuum continuum_test
	rm -f web/public/fluid.js web/public/fluid.wasm
	rm -rf web/dist
