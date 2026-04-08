/*
 * Continuum — App.tsx
 *
 * Top-level React component. Wires the WASM-compiled fluid solver to a
 * WebGL2 canvas and exposes interactive controls.
 *
 * The simulation runs as fast as the browser will let us; we cap visual
 * updates at the display refresh rate via requestAnimationFrame and step
 * the solver once per frame.
 */
import { useEffect, useMemo, useRef, useState } from "react";
import { FluidBridge } from "./wasm/FluidBridge";
import { WebGLRenderer } from "./render/WebGLRenderer";
import { applyPreset, tickPreset, type PresetID } from "./presets";

type LoadState = "idle" | "loading" | "ready" | "error";

const RESOLUTIONS = [64, 96, 128, 192, 256] as const;

export function App() {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const bridgeRef = useRef<FluidBridge | null>(null);
  const rendererRef = useRef<WebGLRenderer | null>(null);
  const rafRef = useRef<number | null>(null);

  const [loadState, setLoadState] = useState<LoadState>("idle");
  const [error, setError]         = useState<string | null>(null);
  const [fps, setFps]             = useState(0);

  // Live tunables — kept in refs so the animation loop can read them
  // without re-running on every slider tick.
  const [resolution, setResolution] = useState<number>(128);
  const [preset, setPreset]         = useState<PresetID>("smoke");
  const [bloom, setBloom]           = useState(0.9);
  const [exposure, setExposure]     = useState(1.0);
  const [viscosity, setViscosity]   = useState(0.00005);
  const [vorticity, setVorticity]   = useState(0.06);
  const [showVorticity, setShowVorticity] = useState(false);
  const [showPanel, setShowPanel]   = useState(true);

  const stateRef = useRef({
    preset,
    bloom,
    exposure,
    viscosity,
    vorticity,
    showVorticity,
    frame: 0
  });
  stateRef.current.preset        = preset;
  stateRef.current.bloom         = bloom;
  stateRef.current.exposure      = exposure;
  stateRef.current.viscosity     = viscosity;
  stateRef.current.vorticity     = vorticity;
  stateRef.current.showVorticity = showVorticity;

  /* ---------- bring up WASM + renderer once -------------------------- */
  useEffect(() => {
    let cancelled = false;
    setLoadState("loading");

    (async () => {
      try {
        const bridge = new FluidBridge();
        await bridge.init({ N: resolution, dt: 0.1, diff: 0.00005, visc: 0.00005 });
        if (cancelled) { bridge.destroy(); return; }

        const canvas = canvasRef.current!;
        const renderer = new WebGLRenderer(canvas);

        bridgeRef.current   = bridge;
        rendererRef.current = renderer;
        applyPreset(bridge, stateRef.current.preset);
        bridge.setVorticity(stateRef.current.vorticity);
        bridge.setViscosity(stateRef.current.viscosity);
        setLoadState("ready");
      } catch (e) {
        setError(String(e));
        setLoadState("error");
      }
    })();

    return () => {
      cancelled = true;
      if (rafRef.current != null) cancelAnimationFrame(rafRef.current);
      rendererRef.current?.destroy();
      bridgeRef.current?.destroy();
      rendererRef.current = null;
      bridgeRef.current = null;
    };
    // We intentionally only do this on the *initial* mount and on
    // explicit resolution changes; viscosity/vorticity etc. flow through
    // refs.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [resolution]);

  /* ---------- push tunables into the solver -------------------------- */
  useEffect(() => { bridgeRef.current?.setViscosity(viscosity); }, [viscosity]);
  useEffect(() => { bridgeRef.current?.setVorticity(vorticity); }, [vorticity]);
  useEffect(() => {
    const b = bridgeRef.current; if (!b) return;
    stateRef.current.frame = 0;
    applyPreset(b, preset);
  }, [preset]);

  /* ---------- main animation loop ----------------------------------- */
  useEffect(() => {
    if (loadState !== "ready") return;
    const bridge = bridgeRef.current!;
    const renderer = rendererRef.current!;
    const canvas = canvasRef.current!;
    let lastTime = performance.now();
    let acc = 0, frames = 0;

    const loop = () => {
      const s = stateRef.current;
      tickPreset(bridge, s.preset, s.frame++);
      bridge.step();

      const Nx = bridge.N + 2;
      const dye = bridge.readDye();
      renderer.uploadDye(dye, Nx);
      renderer.draw(canvas, {
        bloom: s.bloom,
        exposure: s.exposure,
        showVorticity: s.showVorticity
      });

      const now = performance.now();
      acc += now - lastTime;
      lastTime = now;
      frames++;
      if (acc >= 500) {
        setFps((frames * 1000) / acc);
        acc = 0; frames = 0;
      }
      rafRef.current = requestAnimationFrame(loop);
    };
    rafRef.current = requestAnimationFrame(loop);
    return () => { if (rafRef.current != null) cancelAnimationFrame(rafRef.current); };
  }, [loadState]);

  /* ---------- mouse drag → dye + force ------------------------------ */
  const dragRef = useRef<{ active: boolean; lastX: number; lastY: number; right: boolean }>(
    { active: false, lastX: 0, lastY: 0, right: false }
  );

  const canvasToSim = (clientX: number, clientY: number) => {
    const canvas = canvasRef.current!;
    const rect = canvas.getBoundingClientRect();
    const u = (clientX - rect.left) / rect.width;
    const v = (clientY - rect.top)  / rect.height;
    const N = bridgeRef.current?.N ?? 1;
    let x = Math.floor(u * N) + 1;
    let y = Math.floor(v * N) + 1;
    if (x < 1) x = 1; if (x > N) x = N;
    if (y < 1) y = 1; if (y > N) y = N;
    return { x, y };
  };

  const onPointerDown = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const { x, y } = canvasToSim(e.clientX, e.clientY);
    dragRef.current = { active: true, lastX: x, lastY: y, right: e.button === 2 };
    (e.target as HTMLCanvasElement).setPointerCapture(e.pointerId);
  };
  const onPointerMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (!dragRef.current.active) return;
    const sim = bridgeRef.current; if (!sim) return;
    const { x, y } = canvasToSim(e.clientX, e.clientY);
    const dx = x - dragRef.current.lastX;
    const dy = y - dragRef.current.lastY;
    if (!dragRef.current.right) {
      const hue = ((x + y) % 60) / 60;
      const r = 0.5 + 0.5 * Math.sin(hue * 6.28 + 0.0);
      const g = 0.5 + 0.5 * Math.sin(hue * 6.28 + 2.09);
      const b = 0.5 + 0.5 * Math.sin(hue * 6.28 + 4.18);
      sim.addDye(x, y, r, g, b, 4);
    }
    sim.addVelocity(x, y, dx * 4, dy * 4);
    dragRef.current.lastX = x;
    dragRef.current.lastY = y;
  };
  const onPointerUp = (e: React.PointerEvent<HTMLCanvasElement>) => {
    dragRef.current.active = false;
    (e.target as HTMLCanvasElement).releasePointerCapture(e.pointerId);
  };

  /* ---------- keyboard ---------------------------------------------- */
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.target instanceof HTMLInputElement) return;
      const map: Record<string, PresetID> = {
        "1": "smoke", "2": "ink", "3": "karman", "4": "kelvin-helmholtz", "5": "sandbox"
      };
      if (map[e.key]) setPreset(map[e.key]);
      else if (e.key === "r") {
        const b = bridgeRef.current; if (!b) return;
        stateRef.current.frame = 0;
        applyPreset(b, stateRef.current.preset);
      }
      else if (e.key === "v") setShowVorticity(s => !s);
      else if (e.key === "h") setShowPanel(s => !s);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  /* ---------- UI ----------------------------------------------------- */
  const errorBanner = useMemo(() => {
    if (loadState !== "error") return null;
    return (
      <div style={{
        position: "absolute", top: 12, left: 12, right: 12,
        background: "#3a0d10", color: "#fdd",
        padding: "10px 14px", borderRadius: 6, fontSize: 13
      }}>
        Failed to load WASM core. Did you run <code>make wasm</code>?<br />
        <span style={{ opacity: 0.7 }}>{error}</span>
      </div>
    );
  }, [loadState, error]);

  return (
    <div style={{ position: "fixed", inset: 0, overflow: "hidden", background: "#000" }}>
      <canvas
        ref={canvasRef}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onContextMenu={(e) => e.preventDefault()}
        style={{ position: "absolute", inset: 0, width: "100%", height: "100%", touchAction: "none" }}
      />

      {errorBanner}

      <div style={{
        position: "absolute", left: 16, top: 16,
        fontSize: 13, lineHeight: 1.4,
        color: "#cfd6e6",
        textShadow: "0 0 6px #000, 0 0 6px #000"
      }}>
        <strong style={{ fontSize: 18, letterSpacing: 1 }}>continuum</strong>
        <div style={{ opacity: 0.75 }}>
          {loadState === "loading" && "loading wasm core…"}
          {loadState === "ready"   && `${fps.toFixed(1)} fps · N=${resolution} · preset=${preset}`}
        </div>
      </div>

      {showPanel && loadState === "ready" && (
        <div style={panelStyle}>
          <header style={{ fontWeight: 600, marginBottom: 8 }}>controls (h to hide)</header>

          <Row label="preset">
            <select value={preset} onChange={(e) => setPreset(e.target.value as PresetID)}>
              <option value="smoke">smoke plume</option>
              <option value="ink">ink drop</option>
              <option value="karman">von kármán</option>
              <option value="kelvin-helmholtz">kelvin–helmholtz</option>
              <option value="sandbox">sandbox</option>
            </select>
          </Row>

          <Row label="resolution">
            <select value={resolution} onChange={(e) => setResolution(parseInt(e.target.value))}>
              {RESOLUTIONS.map(r => <option key={r} value={r}>{r}×{r}</option>)}
            </select>
          </Row>

          <Slider label="viscosity"  value={viscosity}  min={0}     max={0.001}  step={0.00001} onChange={setViscosity} />
          <Slider label="vorticity"  value={vorticity}  min={0}     max={0.5}    step={0.005}   onChange={setVorticity} />
          <Slider label="bloom"      value={bloom}      min={0}     max={2}      step={0.05}    onChange={setBloom} />
          <Slider label="exposure"   value={exposure}   min={0.2}   max={2}      step={0.05}    onChange={setExposure} />

          <Row label="vorticity overlay">
            <input type="checkbox" checked={showVorticity} onChange={(e) => setShowVorticity(e.target.checked)} />
          </Row>

          <button style={btnStyle} onClick={() => {
            const b = bridgeRef.current; if (!b) return;
            stateRef.current.frame = 0;
            applyPreset(b, preset);
          }}>reset</button>

          <div style={{ marginTop: 10, fontSize: 11, opacity: 0.65 }}>
            keys: 1–5 presets · r reset · v vorticity · h hide panel
          </div>
        </div>
      )}
    </div>
  );
}

const panelStyle: React.CSSProperties = {
  position: "absolute", right: 16, top: 16, width: 240,
  background: "rgba(15,18,28,0.78)",
  backdropFilter: "blur(8px)",
  border: "1px solid rgba(255,255,255,0.08)",
  borderRadius: 8, padding: 14, fontSize: 12,
  color: "#cfd6e6"
};

const btnStyle: React.CSSProperties = {
  width: "100%", padding: "6px 10px", marginTop: 8,
  background: "#1a2030", color: "#cfd6e6",
  border: "1px solid rgba(255,255,255,0.12)",
  borderRadius: 4, cursor: "pointer"
};

function Row(props: { label: string; children: React.ReactNode }) {
  return (
    <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", margin: "6px 0" }}>
      <span style={{ opacity: 0.75 }}>{props.label}</span>
      {props.children}
    </div>
  );
}

function Slider(props: {
  label: string; value: number; min: number; max: number; step: number;
  onChange: (v: number) => void;
}) {
  return (
    <div style={{ margin: "6px 0" }}>
      <div style={{ display: "flex", justifyContent: "space-between", fontSize: 11, opacity: 0.75 }}>
        <span>{props.label}</span>
        <span>{props.value.toFixed(5)}</span>
      </div>
      <input
        type="range"
        min={props.min} max={props.max} step={props.step} value={props.value}
        onChange={(e) => props.onChange(parseFloat(e.target.value))}
        style={{ width: "100%" }}
      />
    </div>
  );
}
