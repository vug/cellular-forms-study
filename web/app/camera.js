// The orbit camera of the viewer (as src/viewer/math.hpp) and its mouse, touch and keyboard
// handling. Matrices are column-major Float32Array(16); clip space is WebGPU's (y up, z 0..1).

export const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
export const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
export const scale = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
export const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
export const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
export const length = (a) => Math.hypot(a[0], a[1], a[2]);
export const normalize = (a) => { const l = length(a) || 1; return [a[0] / l, a[1] / l, a[2] / l]; };

export function perspective(fovy, aspect, near, far) {
  const f = 1 / Math.tan(fovy / 2);
  return new Float32Array([f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, far / (near - far), -1, 0, 0, (near * far) / (near - far), 0]);
}

// A box of half-width `half` around the view axis, from `near` to `far` in front of the eye.
export function orthographic(half, near, far) {
  return new Float32Array([1 / half, 0, 0, 0, 0, 1 / half, 0, 0, 0, 0, 1 / (near - far), 0, 0, 0, near / (near - far), 1]);
}

export function lookAt(eye, target, up = [0, 1, 0]) {
  const fw = normalize(sub(target, eye)), s = normalize(cross(fw, up)), u = cross(s, fw);
  return new Float32Array([s[0], u[0], -fw[0], 0, s[1], u[1], -fw[1], 0, s[2], u[2], -fw[2], 0, -dot(s, eye), -dot(u, eye), dot(fw, eye), 1]);
}

export function multiply(a, b) {
  const out = new Float32Array(16);
  for (let c = 0; c < 4; c++)
    for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = s;
    }
  return out;
}

// Inverse of a rotation plus translation.
export function rigidInverse(m) {
  const out = new Float32Array(16);
  for (let c = 0; c < 3; c++) for (let r = 0; r < 3; r++) out[c * 4 + r] = m[r * 4 + c];
  for (let r = 0; r < 3; r++) out[12 + r] = -(out[r] * m[12] + out[4 + r] * m[13] + out[8 + r] * m[14]);
  out[15] = 1;
  return out;
}

// Rotates a direction by the upper 3x3 of m.
export function rotate(m, v) {
  return [m[0] * v[0] + m[4] * v[1] + m[8] * v[2], m[1] * v[0] + m[5] * v[1] + m[9] * v[2], m[2] * v[0] + m[6] * v[1] + m[10] * v[2]];
}

export const CAMERA_DEFAULTS = { yaw: 0.6, pitch: 0.35, fov: 34, zoom: 1, follow: true, spin: 0 };

export class OrbitCamera {
  constructor() {
    this.target = [0, 0, 0];
    this.distance = 30;
    Object.assign(this, CAMERA_DEFAULTS);
    // yaw: radians around +Y; pitch: radians above the horizon; fov: degrees (vertical);
    // zoom: with `follow`, the distance as a multiple of the one that frames the whole form;
    // spin: degrees per second around the vertical.
  }

  get fovy() { return (this.fov * Math.PI) / 180; }

  eye() {
    const cp = Math.cos(this.pitch), sp = Math.sin(this.pitch);
    return add(this.target, scale([cp * Math.sin(this.yaw), sp, cp * Math.cos(this.yaw)], this.distance));
  }

  view() { return lookAt(this.eye(), this.target); }

  // The distance from which a sphere of this radius fills most of the view.
  fitDistance(radius, aspect) {
    const half = this.fovy / 2;
    // On a tall screen the width decides.
    const halfH = aspect < 1 ? Math.atan(Math.tan(half) * aspect) : half;
    return (radius / Math.sin(halfH)) * 1.05;
  }

  orbit(dYaw, dPitch) {
    this.yaw += dYaw;
    this.pitch = Math.max(-1.55, Math.min(1.55, this.pitch + dPitch));
  }

  // dx, dy in units of the distance.
  pan(dx, dy) {
    const f = normalize(sub(this.target, this.eye()));
    const s = normalize(cross(f, [0, 1, 0]));
    const u = cross(s, f);
    this.target = add(this.target, scale(add(scale(s, dx), scale(u, dy)), this.distance));
  }

  zoomBy(factor) {
    if (this.follow) this.zoom = Math.max(0.02, Math.min(20, this.zoom * factor));
    else this.distance = Math.max(1e-3, this.distance * factor);
  }

  // Moves towards framing a sphere (while `follow` is on). `blend` 1 jumps there.
  track(centre, radius, aspect, blend) {
    if (!this.follow) return false;
    const goal = this.fitDistance(radius, aspect) * this.zoom;
    const before = [this.target[0], this.target[1], this.target[2], this.distance];
    for (let i = 0; i < 3; i++) this.target[i] += (centre[i] - this.target[i]) * blend;
    this.distance += (goal - this.distance) * blend;
    return Math.abs(this.distance - before[3]) > 1e-4 * this.distance || length(sub(this.target, before)) > 1e-4 * this.distance;
  }
}

// Mouse: drag to orbit, right or middle button (or shift) to pan, wheel to zoom. Touch: one
// finger orbits, two fingers zoom and pan. Keys on the focused view: arrows, + and -.
export function attachCameraInput(canvas, camera, onChange) {
  const pointers = new Map();
  let pinch = null;

  const panBy = (dxPixels, dyPixels) => {
    const k = (2 * Math.tan(camera.fovy / 2)) / Math.max(canvas.clientHeight, 1);
    if (camera.follow) {
      camera.follow = false;
      onChange('follow');
    }
    camera.pan(-dxPixels * k, dyPixels * k);
  };

  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  canvas.addEventListener('pointerdown', (e) => {
    canvas.setPointerCapture(e.pointerId);
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY, button: e.button, shift: e.shiftKey });
    pinch = null;
  });
  const release = (e) => {
    pointers.delete(e.pointerId);
    pinch = null;
  };
  canvas.addEventListener('pointerup', release);
  canvas.addEventListener('pointercancel', release);
  canvas.addEventListener('pointermove', (e) => {
    const p = pointers.get(e.pointerId);
    if (!p) return;
    const dx = e.clientX - p.x, dy = e.clientY - p.y;
    p.x = e.clientX;
    p.y = e.clientY;
    if (pointers.size === 1) {
      if (p.button === 1 || p.button === 2 || p.shift) panBy(dx, dy);
      else camera.orbit(-dx * 0.006, dy * 0.006);
      onChange('move');
    } else if (pointers.size === 2) {
      const [a, b] = [...pointers.values()];
      const span = Math.hypot(a.x - b.x, a.y - b.y), mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
      if (pinch) {
        if (span > 1 && pinch.span > 1) camera.zoomBy(pinch.span / span);
        // Two fingers never stay quite in place while they pinch: the view only moves
        // sideways (and stops following the form) once they have clearly travelled.
        if (!pinch.panning && Math.hypot(mx - pinch.startX, my - pinch.startY) > 14) pinch.panning = true;
        if (pinch.panning) panBy(mx - pinch.mx, my - pinch.my);
        onChange('move');
      }
      pinch = { span, mx, my, startX: pinch ? pinch.startX : mx, startY: pinch ? pinch.startY : my, panning: pinch ? pinch.panning : false };
    }
  });
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    const lines = e.deltaMode === 1 ? e.deltaY : e.deltaY / 100;
    camera.zoomBy(Math.pow(1.12, Math.max(-6, Math.min(6, lines))));
    onChange('move');
  }, { passive: false });
  canvas.addEventListener('keydown', (e) => {
    const step = 0.08;
    if (e.key === 'ArrowLeft') camera.orbit(-step, 0);
    else if (e.key === 'ArrowRight') camera.orbit(step, 0);
    else if (e.key === 'ArrowUp') camera.orbit(0, step);
    else if (e.key === 'ArrowDown') camera.orbit(0, -step);
    else if (e.key === '+' || e.key === '=') camera.zoomBy(1 / 1.12);
    else if (e.key === '-' || e.key === '_') camera.zoomBy(1.12);
    else return;
    e.preventDefault();
    onChange('move');
  });
}
