/*
 * Browser-side mirrors of the C presets in src/presets.c.
 * The simulation core is the same — only the driver is reimplemented
 * here so the React app can manipulate it via FluidBridge without
 * exporting the C presets through WASM.
 */
import type { FluidBridge } from "./wasm/FluidBridge";

export type PresetID = "smoke" | "ink" | "karman" | "kelvin-helmholtz" | "sandbox";

export const PRESETS: PresetID[] = ["smoke", "ink", "karman", "kelvin-helmholtz", "sandbox"];

export function applyPreset(sim: FluidBridge, id: PresetID) {
  const N = sim.N;
  sim.reset();
  sim.clearObstacles();

  switch (id) {
    case "smoke":
      sim.setBuoyancy(2.5);
      sim.setVorticity(0.05);
      break;

    case "ink": {
      sim.setBuoyancy(0);
      sim.setVorticity(0.08);
      const cx = (N >> 1), cy = (N >> 1);
      sim.addDye(cx, cy, 0.2, 0.6, 1.0, 8);
      for (let dy = -6; dy <= 6; dy++) {
        for (let dx = -6; dx <= 6; dx++) {
          const r = Math.hypot(dx, dy);
          if (r > 0 && r < 7) {
            sim.addVelocity(cx + dx, cy + dy, (dx / r) * 6, (dy / r) * 6);
          }
        }
      }
      break;
    }

    case "karman": {
      sim.setBuoyancy(0);
      sim.setVorticity(0.08);
      const cx = (N >> 2), cy = (N >> 1);
      const rad = Math.max(2, Math.floor(N / 14));
      for (let j = cy - rad; j <= cy + rad; j++) {
        for (let i = cx - rad; i <= cx + rad; i++) {
          const dx = i - cx, dy = j - cy;
          if (dx * dx + dy * dy <= rad * rad) {
            sim.setObstacle(i, j, true);
          }
        }
      }
      break;
    }

    case "kelvin-helmholtz": {
      sim.setBuoyancy(0);
      sim.setVorticity(0.05);
      const mid = (N >> 1);
      for (let j = 1; j <= N; j++) {
        for (let i = 1; i <= N; i++) {
          if (j < mid) {
            sim.addVelocity(i, j,  3, 0);
            sim.addDye(i, j, 0.9, 0.25, 0.1, 0);
          } else {
            sim.addVelocity(i, j, -3, 0);
            sim.addDye(i, j, 0.1, 0.35, 0.95, 0);
          }
        }
      }
      for (let i = 1; i <= N; i++) {
        const pert = 0.6 * Math.sin((2 * Math.PI * i / N) * 4);
        sim.addVelocity(i, mid,     0, pert);
        sim.addVelocity(i, mid + 1, 0, pert);
      }
      break;
    }

    case "sandbox":
      sim.setBuoyancy(0);
      sim.setVorticity(0.05);
      break;
  }
}

export function tickPreset(sim: FluidBridge, id: PresetID, frame: number) {
  const N = sim.N;
  switch (id) {
    case "smoke": {
      const cx = (N >> 1), cy = N - 4;
      const wob = Math.sin(frame * 0.07) * 1.5;
      for (let dx = -3; dx <= 3; dx++) {
        sim.addDye(cx + dx, cy, 1.0, 0.4, 0.1, 2);
        sim.addVelocity(cx + dx, cy, wob * 0.3, -7);
      }
      break;
    }

    case "karman": {
      for (let j = 1; j <= N; j++) {
        sim.addVelocity(2, j, 6, 0);
        const band = (j >> 2) % 3;
        const r = band === 0 ? 1 : 0.05;
        const g = band === 1 ? 1 : 0.10;
        const b = band === 2 ? 1 : 0.20;
        sim.addDye(2, j, r * 0.6, g * 0.6, b * 0.6, 0);
      }
      if (frame < 200) {
        const cx = (N >> 2) + Math.max(2, Math.floor(N / 14)) + 2;
        const cy = (N >> 1);
        const pert = Math.sin(frame * 0.15) * 0.6;
        sim.addVelocity(cx, cy, 0, pert);
      }
      break;
    }

    default:
      break;
  }
}
