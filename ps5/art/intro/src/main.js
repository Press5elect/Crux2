// Crux2 - the startup video (ps5/art/intro, README.md there): the scene, the fracture, the timeline and the
// post (three.js 0.160.0, MIT). Made from OGBox's (ps5/art/intro/src/main.js, OGBox by Press5elect): the
// same glass, blows and burst, with Crux2's mark and name (logo.py) and its orange.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
import * as THREE from './node_modules/three/build/three.module.js';

const W = 1280, H = 720;                                                       // shown scaled to the screen
const T = { hb1: 0.45, hb2: 1.0, i1: 1.55, i2: 2.15, i3: 2.7, burst: 3.4, text: 4.3, fade: 6.4, end: 7.0 };   // audio.py has the same
window.T = T;

function mulberry32(a) { return function () { a |= 0; a = a + 0x6D2B79F5 | 0; let t = Math.imul(a ^ a >>> 15, 1 | a); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; }; }
const rnd = mulberry32(20261115);
const rr = (a, b) => a + (b - a) * rnd();

// ---------------------------------------------------------------- layout
const FOV = 35, CAMZ = 6.0;
const LOGO_Z = -0.6, LOGO_W = 3.9, LOGO_H = 2.6;
const XC = new THREE.Vector3(0, (0.5 - 318 / 1024) * LOGO_H, LOGO_Z);           // the mark's middle (logo.py: MARK_Y)
const C = new THREE.Vector2(0, XC.y * CAMZ / (CAMZ - LOGO_Z));                  // where it meets the slab
const TH = 0.07;                                                                // the slab's thickness
const ARMS = [[-1, 1], [1, 1], [-1, -1], [1, -1]].map(([x, y]) => { const l = Math.hypot(x, y); return [x / l, y / l]; });
const SPLIT = 0.40;                                                             // uv.y: the mark above, the name below

// ---------------------------------------------------------------- the fracture
const RX = 4.9, RY = 3.0;
const seeds = [];
for (let k = 0; k < 4; k++) { const a = rnd() * 6.283, r = 0.025; seeds.push([C.x + r * Math.cos(a), C.y + r * Math.sin(a)]); }
const SP0 = 13, spokeOff = 0.31;
const spokeJ = []; for (let k = 0; k < SP0 * 4; k++) spokeJ.push((rnd() - 0.5) * 0.5);   // each spoke keeps its angle: long straight cracks
for (let r = 0.07, ring = 0; r < 7.5; r *= 1.5, ring++) {
  const mul = r < 0.45 ? 1 : r < 1.5 ? 2 : 4, n = SP0 * mul;
  for (let k = 0; k < n; k++) {
    if (rnd() < 0.06 && r > 0.3) continue;                                        // a missing seed: a longer sliver
    const a = spokeOff + (k + 0.5 * (mul > 1 ? 1 : 0) + spokeJ[k * (4 / mul)] / (4 / mul) * 0.9 + (rnd() - 0.5) * 0.10) / n * 6.2831853;
    const q = r * (1 + (rnd() - 0.5) * 0.30);
    seeds.push([C.x + q * Math.cos(a), C.y + q * Math.sin(a)]);
  }
}
function clip(poly, mx, my, nx, ny) {
  const out = [];
  for (let i = 0; i < poly.length; i++) {
    const a = poly[i], b = poly[(i + 1) % poly.length];
    const da = (a[0] - mx) * nx + (a[1] - my) * ny, db = (b[0] - mx) * nx + (b[1] - my) * ny;
    if (da <= 0) out.push(a);
    if ((da < 0 && db > 0) || (da > 0 && db < 0)) { const t = da / (da - db); out.push([a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]); }
  }
  return out;
}
const cells = [];
for (let i = 0; i < seeds.length; i++) {
  let poly = [[-RX, -RY], [RX, -RY], [RX, RY], [-RX, RY]];
  const s = seeds[i];
  for (let j = 0; j < seeds.length && poly.length >= 3; j++) {
    if (j === i) continue;
    const o = seeds[j];
    poly = clip(poly, (s[0] + o[0]) / 2, (s[1] + o[1]) / 2, o[0] - s[0], o[1] - s[1]);
  }
  if (poly.length < 3) continue;
  let A = 0, cx = 0, cy = 0;
  for (let k = 0; k < poly.length; k++) { const a = poly[k], b = poly[(k + 1) % poly.length], w = a[0] * b[1] - b[0] * a[1]; A += w; cx += (a[0] + b[0]) * w; cy += (a[1] + b[1]) * w; }
  if (Math.abs(A) < 2e-5) continue;
  cx /= 3 * A; cy /= 3 * A;
  if (A < 0) poly.reverse();
  cells.push({ poly, cx, cy, area: Math.abs(A) / 2 });
}
// when each cell cracks: the first blow along the X's arms, the second wider, the third everywhere
function xdist(qx, qy, R, w0, w1) {
  let best = 1e9;
  for (const [ux, uy] of ARMS) {
    const al = qx * ux + qy * uy, pe = Math.abs(qx * uy - qy * ux);
    if (al < -0.03) continue;
    best = Math.min(best, Math.max(al / R, pe / (w0 + w1 * Math.max(al, 0))));
  }
  return best;
}
for (const c of cells) {
  const qx = c.cx - C.x, qy = c.cy - C.y, d = Math.hypot(qx, qy);
  c.d = d;
  c.rnd = [rnd(), rnd(), rnd(), rnd()];
  const n1 = 0.8 + rnd() * 0.4, n2 = 0.8 + rnd() * 0.4;
  c.c1 = (xdist(qx, qy, 1.35, 0.06, 0.07) < n1 || d < 0.16) ? T.i1 + d * 0.055 + rnd() * 0.03 : 1e3;
  c.c2 = (xdist(qx, qy, 5.5, 0.2, 0.2) < n2 || d < 0.62) ? T.i2 + d * 0.045 + rnd() * 0.04 : 1e3;
  c.c3 = T.i3 + d * 0.04 + rnd() * 0.06;
  c.rel = T.burst + d * 0.028 + rnd() * 0.05;
}
window.NCELLS = cells.length;
{ const h = {}; for (const c of cells) { const k = Math.floor(Math.log(Math.max(c.d, 0.03) / 0.07) / Math.log(1.5) + 0.5); h[k] = (h[k] || 0) + 1; } console.log('seeds', seeds.length, 'cells by ring', JSON.stringify(h)); }

const pos = [], nor = [], cen = [], rndA = [], crk = [], inf = [];
function pushV(c, x, y, z, nx, ny, nz, edge, face) {
  pos.push(x, y, z); nor.push(nx, ny, nz); cen.push(c.cx, c.cy, -TH / 2);
  rndA.push(...c.rnd); crk.push(c.c1, c.c2, c.c3, c.rel); inf.push(c.d, edge, face);
}
for (const c of cells) {
  const P = c.poly, n = P.length;
  for (let k = 0; k < n; k++) {
    const a = P[k], b = P[(k + 1) % n];
    pushV(c, c.cx, c.cy, 0, 0, 0, 1, 0, 0); pushV(c, a[0], a[1], 0, 0, 0, 1, 1, 0); pushV(c, b[0], b[1], 0, 0, 0, 1, 1, 0);          // front
    pushV(c, c.cx, c.cy, -TH, 0, 0, -1, 0, 1); pushV(c, b[0], b[1], -TH, 0, 0, -1, 1, 1); pushV(c, a[0], a[1], -TH, 0, 0, -1, 1, 1);  // back
    const ex = b[0] - a[0], ey = b[1] - a[1], l = Math.hypot(ex, ey) || 1, nx = ey / l, ny = -ex / l;                             // side
    pushV(c, a[0], a[1], 0, nx, ny, 0, 1, 2); pushV(c, a[0], a[1], -TH, nx, ny, 0, 1, 2); pushV(c, b[0], b[1], -TH, nx, ny, 0, 1, 2);
    pushV(c, a[0], a[1], 0, nx, ny, 0, 1, 2); pushV(c, b[0], b[1], -TH, nx, ny, 0, 1, 2); pushV(c, b[0], b[1], 0, nx, ny, 0, 1, 2);
  }
}
const slabGeo = new THREE.BufferGeometry();
slabGeo.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
slabGeo.setAttribute('normal', new THREE.Float32BufferAttribute(nor, 3));
slabGeo.setAttribute('aCen', new THREE.Float32BufferAttribute(cen, 3));
slabGeo.setAttribute('aRnd', new THREE.Float32BufferAttribute(rndA, 4));
slabGeo.setAttribute('aCrack', new THREE.Float32BufferAttribute(crk, 4));
slabGeo.setAttribute('aInfo', new THREE.Float32BufferAttribute(inf, 3));

// ---------------------------------------------------------------- renderer, scene
const canvas = document.getElementById('c');
const renderer = new THREE.WebGLRenderer({ canvas, antialias: false, preserveDrawingBuffer: true, powerPreference: 'high-performance' });
renderer.setPixelRatio(1); renderer.setSize(W, H, false);
renderer.outputColorSpace = THREE.LinearSRGBColorSpace;
renderer.autoClear = false;
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(FOV, W / H, 0.05, 60);

const logoTex = await new THREE.TextureLoader().loadAsync('logo.png');
logoTex.colorSpace = THREE.NoColorSpace; logoTex.generateMipmaps = true; logoTex.minFilter = THREE.LinearMipmapLinearFilter; logoTex.anisotropy = 8;

const GREEN = new THREE.Vector3(1.0, 0.30, 0.08);                               // (the name kept: Crux2's orange, linear)
const U = {                                                                      // shared, set each sub-frame
  uT: { value: 0 }, uStrain: { value: 0 }, uC: { value: C }, uBack: { value: 0 }, uKey: { value: 0 }, uTransmit: { value: 0 },
  uLight: { value: XC.clone() }, uLogo: { value: logoTex }, uLogoRect: { value: new THREE.Vector4(-LOGO_W / 2, -LOGO_H / 2, LOGO_W, LOGO_H) },
  uLogoZ: { value: LOGO_Z }, uG: { value: GREEN }, uSplit: { value: SPLIT }, uRayI: { value: 0 }, uBackI: { value: 0 }, uBurst: { value: T.burst },
};

const slabMat = new THREE.ShaderMaterial({
  uniforms: U,
  vertexShader: `
    attribute vec3 aCen; attribute vec4 aRnd; attribute vec4 aCrack; attribute vec3 aInfo;
    uniform float uT, uStrain; uniform vec2 uC;
    varying vec3 vW, vN; varying float vEdge, vFace, vCr, vD, vRel, vHits; varying vec4 vRnd; varying vec2 vP0;
    vec3 rot(vec3 v, vec3 k, float a) { float c = cos(a), s = sin(a); return v * c + cross(k, v) * s + k * dot(k, v) * (1. - c); }
    float spring(float tau, float f, float dmp) { return tau < 0. ? 0. : 1. - exp(-dmp * tau) * cos(f * tau); }
    void main() {
      float d = aInfo.x;
      vec3 local = position - aCen, nrm = normal;
      float h1 = step(aCrack.x, uT), h2 = step(aCrack.y, uT), h3 = step(aCrack.z, uT);
      float cracked = max(h1, max(h2, h3));
      float s1 = spring(uT - aCrack.x, 34., 7.), s2 = spring(uT - aCrack.y, 30., 6.), s3 = spring(uT - aCrack.z, 26., 5.);
      float st = uStrain * h3;
      local.xy *= 1.004 - cracked * 0.004;
      float ll = length(local.xy);
      float inset = (0.0024 * s1 + 0.0010 * s2 + 0.0014 * s3 + 0.0045 * st) * (0.45 + aRnd.w) * (1. + 1.6 * exp(-d / 0.45));
      if (ll > 1e-5) local.xy -= local.xy / ll * min(inset, ll * 0.4);
      float fall = 0.3 + exp(-d / 1.3);
      float ang = (0.014 * s1 + 0.035 * s2 + 0.08 * s3) * fall * (0.25 + aRnd.x * 1.3);
      ang += st * (0.07 * fall * (0.4 + aRnd.w) + 0.010 * sin(uT * 95. + aRnd.y * 40.));
      float pa = aRnd.z * 6.2831853;
      vec3 axP = vec3(cos(pa), sin(pa), 0.);
      float bul = 0.02 * s1 * exp(-d * d / 0.3) + 0.06 * s2 * exp(-d * d / 0.7) + 0.13 * s3 * exp(-d * d / 1.3) + st * 0.26 * exp(-d * d / 1.7);
      vec3 c = aCen + vec3(0., 0., bul);
      vec2 dir = normalize(aCen.xy - uC + vec2(1e-4, 0.));
      c.xy += dir * bul * 0.25 * min(d, 1.);
      // the dome leans each shard away from the centre
      vec3 axD = vec3(-dir.y, dir.x, 0.);
      float lean = bul * 1.6 * min(d * 2.5, 1.);
      local = rot(local, axP, ang); nrm = rot(nrm, axP, ang);
      local = rot(local, axD, lean); nrm = rot(nrm, axD, lean);
      float tau = uT - aCrack.w;
      if (tau > 0.) {
        float near = exp(-d / 1.1);
        vec3 vel = vec3(dir * (0.8 + 3.4 * near) * (0.5 + aRnd.x), (1.5 + 8.0 * near) * (0.45 + aRnd.y * 1.1));
        vel.y += 1.0 * aRnd.w;
        vec3 axT = normalize(vec3(aRnd.y - .5, aRnd.z - .5, aRnd.w - .5) + 1e-3);
        float w = (2. + 8. * aRnd.x) * (0.6 + near);
        local = rot(local, axT, w * tau); nrm = rot(nrm, axT, w * tau);
        c += vel * tau + vec3(0., -4.6, 0.) * 0.5 * tau * tau;
      }
      vec3 wp = c + local;
      vW = wp; vN = nrm; vEdge = aInfo.y; vFace = aInfo.z; vCr = cracked; vRnd = aRnd; vD = d; vRel = step(0., tau);
      vP0 = position.xy - uC; vHits = 0.3 * h1 + 0.3 * h2 + 0.4 * h3 + 0.5 * st;
      gl_Position = projectionMatrix * viewMatrix * vec4(wp, 1.);
    }`,
  fragmentShader: `
    uniform float uT, uBack, uKey, uTransmit, uLogoZ, uSplit, uBurst; uniform vec3 uLight, uG; uniform sampler2D uLogo; uniform vec4 uLogoRect;
    varying vec3 vW, vN; varying float vEdge, vFace, vCr, vD, vRel, vHits; varying vec4 vRnd; varying vec2 vP0;
    vec2 hash2(vec2 p) { p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3))); return fract(sin(p) * 43758.5453); }
    float voroEdge(vec2 x, out float id) {
      vec2 n = floor(x), f = fract(x), mg = vec2(0.), mr = vec2(0.); float md = 8.;
      for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) { vec2 g = vec2(float(i), float(j)); vec2 r = g + hash2(n + g) - f; float d = dot(r, r); if (d < md) { md = d; mr = r; mg = g; } }
      id = hash2(n + mg).x; md = 8.;
      for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) { vec2 g = mg + vec2(float(i), float(j)); vec2 r = g + hash2(n + g) - f; if (dot(mr - r, mr - r) > 1e-5) md = min(md, dot(0.5 * (mr + r), normalize(r - mr))); }
      return md;
    }
    void main() {
      vec3 N = normalize(vN), V = normalize(cameraPosition - vW);
      vec3 col = vec3(0.0012, 0.0018, 0.0016);
      float fres = pow(1. - max(dot(N, V), 0.), 5.);
      vec3 R = reflect(-V, N);
      // a soft box in front, off the slab's mirror direction (as seen from far): only a tilted shard catches it
      vec3 R0 = reflect(vec3(0., 0., -1.), N);
      vec3 D = normalize(vec3(0.10, 0.20, 1.));
      float box = smoothstep(0.984, 0.9995, dot(R0, D)) * step(0., N.z);
      box *= mix(0.2, 1., 0.5 + 0.5 * sin(dot(vP0, vec2(2.3, 3.1)) + vRnd.x * 6.28));
      float top = smoothstep(0.1, 0.9, R.y) * vRel;
      col += vec3(0.92, 0.84, 0.76) * (box * 0.085 + top * 0.16 * (0.15 + 0.85 * fres)) * uKey;
      col += vec3(0.95, 0.7, 0.55) * fres * 0.10 * vRel * uKey;
      // the light behind
      vec3 Lv = uLight - vW; float dl = length(Lv); vec3 L = Lv / dl;
      float att = 1. / (1. + 0.55 * dl * dl);
      float diff = max(dot(N, L), 0.);
      float spec = pow(max(dot(N, normalize(L + V)), 0.), 36.);
      col += uG * (diff * 0.30 + spec * 1.3) * att * uBack;
      // glass edges: the walls carry the light through
      float side = step(1.5, vFace);
      col += vec3(0.42, 0.12, 0.04) * side * (0.05 * uKey + 0.30 * uBack * att);
      // chipped rims of the front face
      float rim = smoothstep(0.965, 1.0, vEdge) * (1. - step(0.5, vFace)) * vCr;
      col += uG * rim * uBack * (att * 0.22 + 0.03);
      // the crazing around the blows: fine cracks in the face, catching the light behind
      if (vFace < 0.5 && vD < 2.0 && vCr > 0.5) {
        float rr = length(vP0);
        vec2 pc = vec2(log(rr + 0.03) * 4.2, atan(vP0.y, vP0.x) * 5.4);
        float id; float e = voroEdge(pc, id);
        float keep = step(id, 0.25 + 0.75 * vHits * exp(-vD / 1.1));
        float line = smoothstep(0.045, 0.0, e) * keep * exp(-vD / 0.75) * vHits;
        col += (uG * 0.5 + vec3(0.25)) * line * (0.05 * uKey + 0.22 * uBack * att);
      }
      // what glows behind the glass
      if (vFace < 0.5) {
        vec3 dirv = vW - cameraPosition;
        vec3 P = cameraPosition + dirv * ((uLogoZ - cameraPosition.z) / dirv.z);
        vec2 uv = (P.xy - uLogoRect.xy) / uLogoRect.zw;
        vec3 g = pow(textureLod(uLogo, uv, 5.6).rgb, vec3(2.2)) * smoothstep(uSplit - 0.03, uSplit + 0.06, uv.y);
        g += pow(textureLod(uLogo, uv, 3.4).rgb, vec3(2.2)) * 0.35 * smoothstep(uSplit, uSplit + 0.04, uv.y);
        col += g * uTransmit * (0.75 + 0.5 * vRnd.x * vCr);
      }
      gl_FragColor = vec4(col, 1.);
    }`,
});
const slab = new THREE.Mesh(slabGeo, slabMat); slab.frustumCulled = false; scene.add(slab);

// the light behind everything
const backMat = new THREE.ShaderMaterial({
  uniforms: U,
  vertexShader: `varying vec3 vW; void main() { vW = (modelMatrix * vec4(position, 1.)).xyz; gl_Position = projectionMatrix * viewMatrix * vec4(vW, 1.); }`,
  fragmentShader: `
    uniform float uT, uBackI, uRayI; uniform vec3 uLight, uG; varying vec3 vW;
    float hash(float n) { return fract(sin(n) * 43758.5453); }
    float noise(float x) { float i = floor(x), f = fract(x); f = f * f * (3. - 2. * f); return mix(hash(i), hash(i + 1.), f); }
    void main() {
      vec2 q = vW.xy - uLight.xy; float r = length(q), a = atan(q.y, q.x);
      float core = 2.6 * exp(-r * r / 0.22) + 0.6 * exp(-r * r / 1.6) + 0.2;
      float rays = 0.;
      rays += noise(a * 5.0 + uT * 0.21 + 3.) * noise(a * 11.0 - uT * 0.17 + 9.);
      rays += 0.6 * noise(a * 23.0 + uT * 0.12) * noise(a * 3.0 - uT * 0.3 + 5.);
      rays = pow(rays, 1.6) * exp(-r / 1.9) * smoothstep(0.05, 0.5, r);
      vec3 col = uG * (core * uBackI + rays * uRayI * 1.5);
      col += vec3(1., 0.85, 0.7) * exp(-r * r / 0.035) * uBackI * 1.2;
      gl_FragColor = vec4(col, 1.);
    }`,
});
const back = new THREE.Mesh(new THREE.PlaneGeometry(30, 18), backMat); back.position.z = -1.6; scene.add(back);

// the logo: a stack of planes for its depth, the top one lit
const logoGroup = new THREE.Group(); logoGroup.position.copy(XC); scene.add(logoGroup);
const logoMats = [];
const LAYERS = 14;
for (let i = 0; i < LAYERS; i++) {
  const f = i / (LAYERS - 1);                                                    // 0 back .. 1 front
  const m = new THREE.ShaderMaterial({
    uniforms: { uLogo: U.uLogo, uI: { value: 1 }, uShade: { value: i === LAYERS - 1 ? 1 : 0.05 + 0.2 * f }, uReveal: { value: 0 }, uSplit: U.uSplit, uTop: { value: i === LAYERS - 1 ? 1 : 0 }, uX: { value: 0 }, uG: U.uG, uGl: { value: -1 } },
    transparent: true, depthWrite: false, premultipliedAlpha: true, blending: THREE.CustomBlending, blendSrc: THREE.OneFactor, blendDst: THREE.OneMinusSrcAlphaFactor,
    vertexShader: `varying vec2 vUv; void main() { vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.); }`,
    fragmentShader: `
      uniform sampler2D uLogo; uniform float uI, uShade, uReveal, uSplit, uTop, uX, uGl; uniform vec3 uG; varying vec2 vUv;
      void main() {
        vec3 t = texture2D(uLogo, vUv).rgb;
        float a = smoothstep(0.05, 0.22, max(t.r, max(t.g, t.b)));
        vec3 c = pow(t, vec3(2.2));
        float isText = 1. - step(uSplit, vUv.y);
        float sweep = mix(0.08, 0.95, uReveal);
        float m = mix(uX, smoothstep(sweep + 0.012, sweep - 0.012, vUv.x) * step(0.001, uReveal), isText);
        float lead = isText * exp(-pow((vUv.x - sweep) / 0.018, 2.)) * step(0.001, uReveal) * (1. - step(0.999, uReveal));
        a *= m;
        float gl = exp(-pow((vUv.x + vUv.y * 0.45 - uGl) / 0.03, 2.)) + 0.4 * exp(-pow((vUv.x + vUv.y * 0.45 - uGl + 0.07) / 0.012, 2.));
        vec3 col = c * uShade * (uTop > 0.5 ? uI : 1.) + uG * lead * 3.0 * uTop + vec3(1.) * lead * 1.5 * uTop + (c * 2.2 + vec3(0.25)) * gl * uTop;
        gl_FragColor = vec4(col * a, a);
      }`,
  });
  const mesh = new THREE.Mesh(new THREE.PlaneGeometry(LOGO_W, LOGO_H), m);
  mesh.position.set(0, -XC.y, -0.16 * (1 - f)); mesh.renderOrder = 10 + i;
  logoGroup.add(mesh); logoMats.push(m);
}

// ---------------------------------------------------------------- particles (instanced billboards)
function particles(list, kind, extra) {
  const g = new THREE.InstancedBufferGeometry();
  g.setAttribute('position', new THREE.Float32BufferAttribute([-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, 1, 1, 0, -1, 1, 0], 3));
  const o = [], v = [], b = [], s = [];
  for (const p of list) { o.push(...p.o); v.push(...p.v); b.push(p.birth, p.life, p.s0, p.s1); s.push(p.seed, p.bright, p.warm || 0, 0); }
  g.setAttribute('aO', new THREE.InstancedBufferAttribute(new Float32Array(o), 3));
  g.setAttribute('aV', new THREE.InstancedBufferAttribute(new Float32Array(v), 3));
  g.setAttribute('aB', new THREE.InstancedBufferAttribute(new Float32Array(b), 4));
  g.setAttribute('aS', new THREE.InstancedBufferAttribute(new Float32Array(s), 4));
  g.instanceCount = list.length;
  const m = new THREE.ShaderMaterial({
    uniforms: { uT: U.uT, uG: U.uG, uDrag: { value: extra.drag }, uGrav: { value: extra.grav }, uWob: { value: extra.wob || 0 }, uFade: { value: 1 } },
    defines: { KIND: kind }, transparent: true, depthWrite: false, depthTest: extra.depthTest !== false, blending: THREE.AdditiveBlending,
    vertexShader: `
      attribute vec3 aO, aV; attribute vec4 aB, aS; uniform float uT, uDrag, uGrav, uWob;
      varying vec2 vQ; varying float vLife, vSeed, vBright, vWarm, vSize;
      void main() {
        float tau = uT - aB.x, life = aB.y, k = clamp(tau / life, 0., 1.);
        vec3 p = aO + aV * (1. - exp(-uDrag * tau)) / uDrag + vec3(0., -uGrav, 0.) * 0.5 * tau * tau;
        p += uWob * vec3(sin(tau * 0.9 + aS.x * 50.), cos(tau * 0.7 + aS.x * 31.), sin(tau * 0.5 + aS.x * 17.)) * 0.12;
        float size = mix(aB.z, aB.w, KIND == 1 ? sqrt(k) : k);
        vec4 mv = viewMatrix * vec4(p, 1.);
      #if KIND == 0
        float t0 = max(tau - 0.03, 0.);
        vec3 p0 = aO + aV * (1. - exp(-uDrag * t0)) / uDrag + vec3(0., -uGrav, 0.) * 0.5 * t0 * t0;
        vec4 mv0 = viewMatrix * vec4(p0, 1.);
        vec2 dv = mv.xy - mv0.xy * (mv.z / mv0.z); float len = length(dv); vec2 dn = len > 1e-5 ? dv / len : vec2(1., 0.);
        mv.xy += dn * (position.x * (size + len * 0.5) - len * 0.5) + vec2(-dn.y, dn.x) * position.y * size;
      #else
        mv.xy += position.xy * size;
      #endif
        gl_Position = projectionMatrix * mv;
        if (tau < 0. || tau > life) gl_Position = vec4(2., 2., 2., 1.);
        vQ = position.xy; vLife = k; vSeed = aS.x; vBright = aS.y; vWarm = aS.z; vSize = size;
      }`,
    fragmentShader: `
      uniform float uT, uFade; uniform vec3 uG; varying vec2 vQ; varying float vLife, vSeed, vBright, vWarm, vSize;
      void main() {
        float r = length(vQ); if (r > 1.) discard;
        vec3 col;
      #if KIND == 0
        float core = exp(-r * r * 7.) ;
        float fl = 0.7 + 0.3 * sin(uT * 60. + vSeed * 100.);
        col = mix(vec3(1., 0.95, 0.8), uG, smoothstep(0., 0.3, vLife) * 0.9 + 0.1) * core * fl * vBright * (1. - vLife * vLife);
      #elif KIND == 1
        float soft = exp(-r * r * 3.2) * (1. - r);
        col = mix(uG, vec3(0.6, 0.55, 0.5), 0.35) * soft * vBright * sin(3.14159 * pow(vLife, 0.6));
      #else
        float disc = smoothstep(1., 0.82, r) * (0.55 + 0.45 * smoothstep(0.55, 0.95, r));
        float tw = 0.6 + 0.4 * sin(uT * (1.5 + vSeed * 3.) + vSeed * 40.);
        vec3 tint = mix(uG, vec3(1., 0.95, 0.7), vWarm);
        col = tint * disc * tw * vBright * smoothstep(0., 0.12, vLife) * smoothstep(1., 0.8, vLife);
      #endif
        gl_FragColor = vec4(col * uFade, 1.);
      }`,
  });
  const mesh = new THREE.Mesh(g, m); mesh.frustumCulled = false; mesh.renderOrder = extra.order || 50; scene.add(mesh);
  return m;
}
function onRim(c) { const P = c.poly, k = Math.floor(rnd() * P.length), a = P[k], b = P[(k + 1) % P.length], t = rnd(); return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]; }
const sparks = [], dust = [], embers = [];
for (const [key, n, sp] of [['c1', 500, 1.0], ['c2', 900, 1.3], ['c3', 1400, 1.6]]) {
  const hit = cells.filter(c => c[key] < 100);
  for (let i = 0; i < n; i++) {
    const c = hit[Math.floor(rnd() * hit.length)], p = onRim(c), d = Math.hypot(p[0] - C.x, p[1] - C.y) + 1e-3;
    if (d > 2.6 && rnd() < 0.7) continue;
    const dx = (p[0] - C.x) / d, dy = (p[1] - C.y) / d, a = rnd() * 6.283, sv = rr(0.2, 1.5) * sp;
    sparks.push({ o: [p[0], p[1], 0.01], v: [dx * sv * 0.6 + Math.cos(a) * sv * 0.5, dy * sv * 0.6 + Math.sin(a) * sv * 0.5 + 0.3, rr(0.6, 4.2) * sp], birth: c[key] + rnd() * 0.05, life: rr(0.25, 0.9), s0: rr(0.005, 0.014), s1: 0.002, seed: rnd(), bright: rr(2, 7) * rnd() + 0.8 });
  }
}
for (let i = 0; i < 1600; i++) {                                                 // the burst
  const a = rnd() * 6.283, r = Math.pow(rnd(), 1.7) * 2.6, sv = rr(0.6, 4.5), a2 = a + rr(-0.5, 0.5);
  sparks.push({ o: [C.x + r * Math.cos(a), C.y + r * Math.sin(a), rr(-0.3, 0.05)], v: [Math.cos(a2) * sv, Math.sin(a2) * sv + 0.6, rr(1, 11) * Math.exp(-r / 1.6)], birth: T.burst + r * 0.03 + rnd() * 0.12, life: rr(0.35, 1.6), s0: rr(0.005, 0.017), s1: 0.002, seed: rnd(), bright: rr(1.5, 8) * rnd() + 0.8 });
}
for (const [t0, n, amp] of [[T.i1, 26, 0.6], [T.i2, 40, 0.8], [T.i3, 60, 1.0], [T.burst, 150, 1.6]]) {
  for (let i = 0; i < n; i++) {
    const arm = ARMS[Math.floor(rnd() * 4)], al = rnd() * rnd() * 1.6 * amp, a = rnd() * 6.283, off = rr(0, 0.12);
    const x = C.x + arm[0] * al + Math.cos(a) * off, y = C.y + arm[1] * al + Math.sin(a) * off, sv = rr(0.05, 0.5) * amp;
    dust.push({ o: [x, y, 0.05], v: [Math.cos(a) * sv, Math.sin(a) * sv + 0.1, rr(0.3, 1.8) * amp], birth: t0 + al * 0.05 + rnd() * 0.08, life: rr(0.9, 2.2) * (t0 === T.burst ? 1.5 : 1), s0: rr(0.04, 0.1), s1: rr(0.3, 0.75) * amp, seed: rnd(), bright: rr(0.03, 0.075) * (t0 === T.burst ? 0.45 : 1) });
  }
}
for (let i = 0; i < 300; i++) {
  const z = rr(-1.0, 3.9), foc = Math.abs(z - 0.2), size = 0.006 + foc * foc * 0.006 + rnd() * 0.004;
  embers.push({ o: [rr(-4.2, 4.2) * (1 - z * 0.12), rr(-2.6, 2.4) * (1 - z * 0.12), z], v: [rr(-0.06, 0.06), rr(0.03, 0.2), rr(-0.03, 0.08)], birth: T.burst + 0.15 + rnd() * 1.6, life: rr(2.5, 6), s0: size, s1: size * 1.15, seed: rnd(), bright: Math.min(1.3, 0.00008 / (size * size)) * rr(0.4, 1.3) + 0.035, warm: rnd() < 0.25 ? rr(0.3, 0.9) : 0 });
}
const sparkMat = particles(sparks, 0, { drag: 1.6, grav: 3.2, order: 60 });
const dustMat = particles(dust, 1, { drag: 2.2, grav: -0.05, order: 55 });
const emberMat = particles(embers, 2, { drag: 0.05, grav: 0, wob: 1, order: 58 });

// ---------------------------------------------------------------- post
const RT = (w, h, depth) => new THREE.WebGLRenderTarget(w, h, { type: THREE.HalfFloatType, depthBuffer: !!depth, minFilter: THREE.LinearFilter, magFilter: THREE.LinearFilter, wrapS: THREE.ClampToEdgeWrapping, wrapT: THREE.ClampToEdgeWrapping });
const rtScene = RT(W, H, true), rtAccum = RT(W, H);
const NM = 7, down = [], up = [];
for (let i = 0, w = W >> 1, h = H >> 1; i < NM; i++, w = Math.max(1, w >> 1), h = Math.max(1, h >> 1)) { down.push(RT(w, h)); up.push(RT(w, h)); }
const rtRays = RT(W >> 1, H >> 1), rtStreakA = RT(W >> 2, H >> 2), rtStreakB = RT(W >> 2, H >> 2);
const quadScene = new THREE.Scene(), quadCam = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);
const quad = new THREE.Mesh(new THREE.PlaneGeometry(2, 2)); quad.frustumCulled = false; quadScene.add(quad);
const VS = `varying vec2 vUv; void main() { vUv = uv; gl_Position = vec4(position.xy, 0., 1.); }`;
const mk = (fs, uniforms, extra) => new THREE.ShaderMaterial(Object.assign({ vertexShader: VS, fragmentShader: fs, uniforms, depthTest: false, depthWrite: false }, extra || {}));
function pass(mat, target, clear) { quad.material = mat; renderer.setRenderTarget(target); if (clear) renderer.clear(); renderer.render(quadScene, quadCam); }
const mAcc = mk(`uniform sampler2D t; uniform float w; varying vec2 vUv; void main() { gl_FragColor = vec4(min(texture2D(t, vUv).rgb, vec3(60.)) * w, 1.); }`, { t: { value: null }, w: { value: 1 } }, { blending: THREE.AdditiveBlending, transparent: true });
const mDown = mk(`uniform sampler2D t; uniform vec2 px; uniform float thr; varying vec2 vUv;
  vec3 s(vec2 o) { vec3 c = texture2D(t, vUv + o * px).rgb; float l = max(c.r, max(c.g, c.b)); return c * max(l - thr, 0.) / max(l, 1e-4); }
  void main() { vec3 c = s(vec2(0.)) * 4. + s(vec2(1., 1.)) + s(vec2(-1., 1.)) + s(vec2(1., -1.)) + s(vec2(-1., -1.)); gl_FragColor = vec4(c / 8., 1.); }`, { t: { value: null }, px: { value: new THREE.Vector2() }, thr: { value: 0 } });
const mUp = mk(`uniform sampler2D t, b; uniform vec2 px; varying vec2 vUv;
  void main() { vec3 c = vec3(0.);
    c += texture2D(t, vUv + vec2(-2., 0.) * px).rgb + texture2D(t, vUv + vec2(2., 0.) * px).rgb + texture2D(t, vUv + vec2(0., 2.) * px).rgb + texture2D(t, vUv + vec2(0., -2.) * px).rgb;
    c += 2. * (texture2D(t, vUv + vec2(1., 1.) * px).rgb + texture2D(t, vUv + vec2(-1., 1.) * px).rgb + texture2D(t, vUv + vec2(1., -1.) * px).rgb + texture2D(t, vUv + vec2(-1., -1.) * px).rgb);
    gl_FragColor = vec4(texture2D(b, vUv).rgb + c / 12., 1.); }`, { t: { value: null }, b: { value: null }, px: { value: new THREE.Vector2() } });
const mRays = mk(`uniform sampler2D t; uniform vec2 light; varying vec2 vUv;
  float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
  void main() { const int N = 80; vec2 stp = (light - vUv) / float(N) * 0.94; vec2 p = vUv + stp * hash(gl_FragCoord.xy); vec3 acc = vec3(0.); float w = 1.;
    for (int i = 0; i < N; i++) { acc += texture2D(t, p).rgb * w; w *= 0.975; p += stp; }
    gl_FragColor = vec4(acc / float(N) * 5., 1.); }`, { t: { value: null }, light: { value: new THREE.Vector2(0.5, 0.5) } });
const mStreak = mk(`uniform sampler2D t; uniform vec2 stp; varying vec2 vUv;
  void main() { vec3 c = vec3(0.); float tw = 0.; for (int i = -9; i <= 9; i++) { float w = exp(-float(i * i) / 34.); c += texture2D(t, vUv + stp * float(i)).rgb * w; tw += w; } gl_FragColor = vec4(c / tw, 1.); }`, { t: { value: null }, stp: { value: new THREE.Vector2() } });
const mFinal = mk(`
  uniform sampler2D tS, tB, tR, tK; uniform vec2 light; uniform float uBloom, uRays, uStreak, uCA, uFade, uSeed, uShock, uShockA, uExp;
  varying vec2 vUv;
  float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
  vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0., 1.); }
  void main() {
    vec2 uv = vUv, asp = vec2(1.7777778, 1.);
    vec2 q = (uv - light) * asp; float r = length(q); vec2 dir = q / max(r, 1e-4);
    float ring = exp(-pow((r - uShock) / 0.07, 2.)) * uShockA;
    uv -= dir / asp * ring * 0.035;
    vec2 c = uv - 0.5; float ca = uCA * (0.4 + dot(c, c) * 3.);
    vec3 col = vec3(texture2D(tS, uv + c * ca).r, texture2D(tS, uv).g, texture2D(tS, uv - c * ca).b);
    vec3 bloom = vec3(texture2D(tB, uv + c * ca * 1.5).r, texture2D(tB, uv).g, texture2D(tB, uv - c * ca * 1.5).b);
    col += bloom * uBloom;
    col += texture2D(tR, uv).rgb * uRays;
    col += texture2D(tK, uv).rgb * vec3(1., 0.8, 0.7) * uStreak;
    col += vec3(1., 0.55, 0.3) * ring * 0.35;
    col *= uExp;
    float vig = smoothstep(1.25, 0.25, length(c * vec2(1.25, 1.)) * 1.35);
    col *= mix(0.55, 1., vig);
    col = aces(col);
    col = pow(col, vec3(1. / 2.2));
    col += (hash(gl_FragCoord.xy + uSeed) - 0.5) * 0.006;
    gl_FragColor = vec4(col * uFade, 1.);
  }`, { tS: { value: null }, tB: { value: null }, tR: { value: null }, tK: { value: null }, light: { value: new THREE.Vector2(.5, .5) }, uBloom: { value: 1 }, uRays: { value: 1 }, uStreak: { value: .3 }, uCA: { value: 0 }, uFade: { value: 1 }, uSeed: { value: 0 }, uShock: { value: 0 }, uShockA: { value: 0 }, uExp: { value: 1 } });

// ---------------------------------------------------------------- the timeline
const sm = (a, b, x) => { const t = Math.min(1, Math.max(0, (x - a) / (b - a))); return t * t * (3 - 2 * t); };
const dec = (t, t0, k) => t < t0 ? 0 : Math.exp(-k * (t - t0));
const beat = (t, t0) => Math.exp(-Math.pow((t - t0 - 0.07) / 0.075, 2)) + 0.8 * Math.exp(-Math.pow((t - t0 - 0.30) / 0.10, 2));
function shake(t) {
  const A = 0.014 * dec(t, T.i1, 9) + 0.024 * dec(t, T.i2, 8) + 0.04 * dec(t, T.i3, 7) + 0.10 * dec(t, T.burst, 4.2) + 0.006 * sm(T.i3 + 0.2, T.burst, t) * (t < T.burst ? 1 : 0);
  return [A * (Math.sin(t * 83.1 + 1.3) + Math.sin(t * 127.7 + 0.4)) * 0.5, A * (Math.sin(t * 91.7 + 2.1) + Math.sin(t * 143.3 + 4.0)) * 0.5, A * 0.25 * Math.sin(t * 71.3)];
}
function apply(t) {
  const tb = t - T.burst, post = tb > 0;
  U.uT.value = t;
  U.uStrain.value = Math.pow(sm(T.i3 + 0.12, T.burst, t), 1.6);
  const strain = U.uStrain.value;
  U.uTransmit.value = sm(0.25, 1.6, t) * 0.010 + 0.11 * beat(t, T.hb1) + 0.2 * beat(t, T.hb2) + (t > T.i1 ? 0.05 : 0) + 0.14 * dec(t, T.i1, 5) + (t > T.i2 ? 0.035 : 0) + 0.16 * dec(t, T.i2, 5) + (t > T.i3 ? 0.045 : 0) + 0.2 * dec(t, T.i3, 4) + strain * 0.16;
  const flick = 1 + 0.10 * Math.sin(t * 23.0) * Math.sin(t * 7.3 + 1.0) + 0.06 * Math.sin(t * 61.0);
  const backPre = ((t > T.i1 ? 1.5 : 0) + 1.8 * dec(t, T.i1, 6) + (t > T.i2 ? 0.5 : 0) + 2.2 * dec(t, T.i2, 5) + (t > T.i3 ? 0.6 : 0) + 3.0 * dec(t, T.i3, 4.5) + strain * 3.2) * flick;
  const pulse = 1 + 0.06 * Math.sin((t - T.burst) * 2.1);
  U.uBackI.value = post ? 4.5 * Math.exp(-8 * tb) + 0.05 * pulse : backPre;
  U.uBack.value = post ? 2.4 * Math.exp(-2.4 * tb) + 0.6 : backPre * 0.3;
  U.uKey.value = post ? 1.15 : (t > T.i1 ? 0.55 + 0.45 * sm(T.i1, T.burst, t) : 0);
  U.uRayI.value = post ? 0.13 + 1.3 * Math.exp(-4 * tb) : 0.12 * backPre;
  const logoI = post ? 1.1 * pulse + 3.2 * Math.exp(-6 * tb) : 0.75 + 0.25 * backPre;
  const rev = sm(T.text, T.text + 0.55, t);
  const gl = -0.2 + 1.9 * sm(T.text + 0.95, T.text + 1.75, t);
  for (const m of logoMats) { m.uniforms.uI.value = logoI; m.uniforms.uReveal.value = rev; m.uniforms.uX.value = t > T.i1 - 0.01 ? 1 : 0; m.uniforms.uGl.value = gl; }
  const sc = post ? 1 + 0.10 * Math.exp(-5 * tb) * Math.cos(11 * tb) : 0.93 + 0.02 * strain;
  logoGroup.scale.set(sc, sc, 1);
  emberMat.uniforms.uFade.value = 1;
  // camera
  const [sx, sy, sr] = shake(t);
  let z = CAMZ - 0.42 * sm(0.2, T.burst, t);
  if (post) z = CAMZ - 0.42 + 0.42 * (1 - Math.exp(-5.5 * tb) * Math.cos(7.5 * tb)) - 0.14 * sm(0.8, T.end - T.burst, tb);
  const drift = post ? 0.16 * Math.sin(0.55 * tb) * sm(0.2, 1.5, tb) : 0;
  camera.position.set(sx + drift, sy + (post ? 0.05 * Math.sin(0.4 * tb) : 0), z);
  camera.up.set(Math.sin(sr), Math.cos(sr), 0);
  camera.lookAt(0, 0, LOGO_Z);
}
const halton = (i, b) => { let f = 1, r = 0; while (i > 0) { f /= b; r += f * (i % b); i = Math.floor(i / b); } return r; };
const lightNDC = new THREE.Vector3();
window.renderFrame = function (t, fps, S) {
  const tb = t - T.burst;
  S = S || ((tb > -0.05 && tb < 1.9) ? 10 : (Math.abs(t - T.i1) < 0.3 || Math.abs(t - T.i2) < 0.3 || (t > T.i3 - 0.05)) && tb < 0 ? 6 : 4);
  renderer.setRenderTarget(rtAccum); renderer.setClearColor(0x000000, 1); renderer.clear();
  for (let s = 0; s < S; s++) {
    const ts = t + ((s + 0.5) / S - 0.5) * 0.55 / fps;
    apply(ts);
    camera.setViewOffset(W, H, halton(s + 1, 2) - 0.5, halton(s + 1, 3) - 0.5, W, H);
    camera.updateProjectionMatrix(); camera.updateMatrixWorld();
    renderer.setRenderTarget(rtScene); renderer.clear(); renderer.render(scene, camera);
    mAcc.uniforms.t.value = rtScene.texture; mAcc.uniforms.w.value = 1 / S; pass(mAcc, rtAccum, false);
  }
  apply(t); camera.clearViewOffset(); camera.updateProjectionMatrix(); camera.updateMatrixWorld();
  lightNDC.copy(XC).project(camera);
  const lx = lightNDC.x * 0.5 + 0.5, ly = lightNDC.y * 0.5 + 0.5;
  // bloom
  let src = rtAccum;
  for (let i = 0; i < NM; i++) { mDown.uniforms.t.value = src.texture; mDown.uniforms.px.value.set(1 / src.width, 1 / src.height); mDown.uniforms.thr.value = i === 0 ? 0.85 : 0; pass(mDown, down[i], true); src = down[i]; }
  let prev = down[NM - 1];
  for (let i = NM - 2; i >= 0; i--) { mUp.uniforms.t.value = prev.texture; mUp.uniforms.b.value = down[i].texture; mUp.uniforms.px.value.set(1 / prev.width, 1 / prev.height); pass(mUp, up[i], true); prev = up[i]; }
  mRays.uniforms.t.value = down[0].texture; mRays.uniforms.light.value.set(lx, ly); pass(mRays, rtRays, true);
  mStreak.uniforms.t.value = down[1].texture; mStreak.uniforms.stp.value.set(2.2 / rtStreakA.width, 0); pass(mStreak, rtStreakA, true);
  mStreak.uniforms.t.value = rtStreakA.texture; mStreak.uniforms.stp.value.set(7 / rtStreakA.width, 0); pass(mStreak, rtStreakB, true);
  const post = tb > 0, f = mFinal.uniforms;
  f.tS.value = rtAccum.texture; f.tB.value = up[0].texture; f.tR.value = rtRays.texture; f.tK.value = rtStreakB.texture;
  f.light.value.set(lx, ly);
  f.uBloom.value = (post ? 0.10 + 0.08 * Math.exp(-3 * tb) : 0.13);
  f.uRays.value = post ? 0.05 + 0.3 * Math.exp(-3 * tb) : 0.15;
  f.uStreak.value = post ? 0.07 + 0.3 * Math.exp(-3 * tb) : 0.12;
  f.uCA.value = 0.0012 + 0.006 * dec(t, T.i1, 9) + 0.008 * dec(t, T.i2, 8) + 0.012 * dec(t, T.i3, 7) + 0.012 * dec(t, T.burst, 5);
  f.uShock.value = post ? 2.6 * Math.pow(tb, 0.62) : 0; f.uShockA.value = post ? Math.max(0, 1 - tb / 0.75) : 0;
  f.uExp.value = 1;
  f.uFade.value = sm(0, 0.15, t) * (1 - sm(T.fade, T.end - 0.05, t));
  f.uSeed.value = (t * 977.13) % 100;
  pass(mFinal, null, true);
};
window.grab = () => canvas.toDataURL('image/png');
window.ready = true;
