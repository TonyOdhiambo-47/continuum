/*
 * WebGLRenderer — uploads the (N+2)² RGB dye field as a floating-point
 * texture each frame and draws it fullscreen with the bloom shader.
 *
 * Uses Vite's `?raw` import suffix to inline the GLSL source at build
 * time so the renderer ships as a single JS bundle (no shader fetches).
 */

// @ts-expect-error: Vite ?raw asset import
import vertSrc from "./shaders/dye.vert?raw";
// @ts-expect-error: Vite ?raw asset import
import fragSrc from "./shaders/dye.frag?raw";

export interface RendererOptions {
  bloom:        number;
  exposure:     number;
  showVorticity: boolean;
}

export class WebGLRenderer {
  private gl:    WebGL2RenderingContext;
  private prog:  WebGLProgram;
  private vao:   WebGLVertexArrayObject;
  private tex:   WebGLTexture;
  private uRes:  WebGLUniformLocation;
  private uBloom:    WebGLUniformLocation;
  private uExposure: WebGLUniformLocation;
  private uShowVort: WebGLUniformLocation;
  private uDye:      WebGLUniformLocation;
  private texW = 0;
  private texH = 0;
  private canFilterFloat = false;

  constructor(canvas: HTMLCanvasElement) {
    const gl = canvas.getContext("webgl2", {
      alpha: false,
      antialias: false,
      preserveDrawingBuffer: false
    });
    if (!gl) throw new Error("WebGL2 not supported in this browser");
    this.gl = gl;

    // RGB32F is sample-able from a fragment shader on all WebGL2
    // implementations, but LINEAR filtering of float textures requires
    // OES_texture_float_linear. If the browser doesn't expose it we
    // silently downgrade to NEAREST so the texture stays complete.
    this.canFilterFloat = gl.getExtension("OES_texture_float_linear") != null;

    this.prog = this.compile(vertSrc, fragSrc);
    this.vao  = gl.createVertexArray()!;

    this.uRes      = gl.getUniformLocation(this.prog, "u_resolution")!;
    this.uBloom    = gl.getUniformLocation(this.prog, "u_bloom")!;
    this.uExposure = gl.getUniformLocation(this.prog, "u_exposure")!;
    this.uShowVort = gl.getUniformLocation(this.prog, "u_show_vorticity")!;
    this.uDye      = gl.getUniformLocation(this.prog, "u_dye")!;

    this.tex = gl.createTexture()!;
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    const filter = this.canFilterFloat ? gl.LINEAR : gl.NEAREST;
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  }

  private compile(vs: string, fs: string): WebGLProgram {
    const gl = this.gl;
    const compileOne = (type: number, src: string) => {
      const sh = gl.createShader(type)!;
      gl.shaderSource(sh, src);
      gl.compileShader(sh);
      if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
        const log = gl.getShaderInfoLog(sh);
        gl.deleteShader(sh);
        throw new Error(`shader compile failed: ${log}`);
      }
      return sh;
    };
    const v = compileOne(gl.VERTEX_SHADER,   vs);
    const f = compileOne(gl.FRAGMENT_SHADER, fs);
    const p = gl.createProgram()!;
    gl.attachShader(p, v); gl.attachShader(p, f);
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
      const log = gl.getProgramInfoLog(p);
      gl.deleteProgram(p);
      throw new Error(`program link failed: ${log}`);
    }
    gl.deleteShader(v);
    gl.deleteShader(f);
    return p;
  }

  /** Upload a (Nx × Nx) RGB float field as a texture. */
  uploadDye(rgb: Float32Array, Nx: number) {
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    if (Nx !== this.texW || Nx !== this.texH) {
      // (Re)allocate storage at the new resolution.
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB32F, Nx, Nx, 0,
                    gl.RGB, gl.FLOAT, rgb);
      this.texW = this.texH = Nx;
    } else {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, Nx, Nx,
                       gl.RGB, gl.FLOAT, rgb);
    }
  }

  draw(canvas: HTMLCanvasElement, opts: RendererOptions) {
    const gl = this.gl;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = Math.floor(canvas.clientWidth  * dpr);
    const h = Math.floor(canvas.clientHeight * dpr);
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
    gl.viewport(0, 0, w, h);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);

    gl.useProgram(this.prog);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.uniform1i(this.uDye, 0);
    gl.uniform2f(this.uRes, this.texW, this.texH);
    gl.uniform1f(this.uBloom,    opts.bloom);
    gl.uniform1f(this.uExposure, opts.exposure);
    gl.uniform1i(this.uShowVort, opts.showVorticity ? 1 : 0);

    gl.bindVertexArray(this.vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }

  destroy() {
    const gl = this.gl;
    gl.deleteTexture(this.tex);
    gl.deleteProgram(this.prog);
    gl.deleteVertexArray(this.vao);
  }
}
