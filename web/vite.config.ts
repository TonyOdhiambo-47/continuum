import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Continuum web — Vite config.
//
// The Emscripten artefacts (fluid.js / fluid.wasm) are emitted into
// /public by `make wasm`, so they ship as static assets at the site root.
export default defineConfig({
  plugins: [react()],
  server: { port: 5173 },
  build: {
    target: "es2020",
    rollupOptions: {
      // /fluid.js is the Emscripten ES6 module emitted into /public by
      // `make wasm`. It is loaded at runtime via dynamic import; treat it
      // as external so Rollup doesn't try to bundle it.
      external: ["/fluid.js"]
    }
  }
});
