// Crux2 - the startup video (ps5/art/intro, README.md there): the scene and its timeline (three.js
// 0.160.0, MIT: used to render, not shipped). Crux2's mark is put together from its own bars: guides
// show where they go, the two cream bars slot in from opposite corners, the orange one runs in on a
// light trail and locks between them, a light runs along it, then the mark becomes the X of the name
// and C, R, U and 2 come out from behind it one by one. A light crosses the name; it rests; black.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
import * as THREE from './node_modules/three/build/three.module.js';

const W = 1280, H = 720;
// the timeline, in seconds (audio.py has the same): guides, cream bar A, cream bar B, the orange bar,
// the light along it, the mark to its place in the name, the letters (each leaves LET apart and takes
// LDUR), the light across the name, the fade
const T = { guide: 0.2, a0: 0.5, a1: 0.82, b0: 0.92, b1: 1.24, o0: 1.5, o1: 1.96, run: 2.06, move: 2.7, move1: 3.2,
  l0: 3.02, LET: 0.13, LDUR: 0.36, sweep: 4.0, sweep1: 4.6, fade: 5.9, end: 6.6 };
T.land = [0, 1, 2, 3].map(i => +(T.l0 + i * T.LET + T.LDUR).toFixed(3));
window.T = T;

const layout = await (await fetch('letters.json')).json();                     // logo.py
const lin = c => c.map(v => { v /= 255; return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); });
const CREAM = new THREE.Vector3(...lin([243, 236, 228])), ORANGE = new THREE.Vector3(...lin([255, 106, 61]));   // make-title-art.py

function mulberry32(a) { return function () { a |= 0; a = a + 0x6D2B79F5 | 0; let t = Math.imul(a ^ a >>> 15, 1 | a); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; }; }
const rnd = mulberry32(2026111502);

// ---------------------------------------------------------------- layout (world units; the camera sees 4.29 high at z = 0)
const FOV = 30, CAMZ = 8.0;
const BIG = 3.6, TYPE_W = 1.45;            // the mark's square (100 of its units) alone, and the type's size in the name
const MP = 640;                            // the mark's plane, in the mark's units (the bars come from outside the picture)

// ---------------------------------------------------------------- renderer, scene
const canvas = document.getElementById('c');
const renderer = new THREE.WebGLRenderer({ canvas, antialias: false, preserveDrawingBuffer: true });
renderer.setPixelRatio(1); renderer.setSize(W, H, false);
renderer.outputColorSpace = THREE.LinearSRGBColorSpace;
renderer.autoClear = false;
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(FOV, W / H, 0.05, 80);

// the space: a warm dark, lighter behind the mark (the console icon's), with a faint slow haze
const bgMat = new THREE.ShaderMaterial({
  vertexShader: `varying vec2 vUv; void main() { vUv = uv; gl_Position = vec4(position.xy, 0.9999, 1.); }`,
  fragmentShader: `uniform float uI, uGlow, uT; uniform vec2 uC; varying vec2 vUv;
    float h(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
    float n(vec2 p) { vec2 i = floor(p), f = fract(p); f = f * f * (3. - 2. * f);
      return mix(mix(h(i), h(i + vec2(1, 0)), f.x), mix(h(i + vec2(0, 1)), h(i + vec2(1, 1)), f.x), f.y); }
    void main() {
      vec2 q = (vUv - uC) * vec2(1.7778, 1.);
      float r = length(q);
      vec3 edge = vec3(0.0030, 0.0020, 0.0017), mid = vec3(0.020, 0.0083, 0.0052), hot = vec3(0.10, 0.026, 0.010);
      vec3 c = mix(mid, edge, smoothstep(0.0, 1.05, r));
      float haze = n(q * 2.2 + vec2(uT * 0.05, -uT * 0.03)) * 0.6 + n(q * 5.1 - vec2(uT * 0.04, uT * 0.06)) * 0.4;
      c *= 0.85 + 0.3 * haze;
      c += hot * exp(-r * r / 0.07) * uGlow;
      gl_FragColor = vec4(c * uI, 1.);
    }`,
  uniforms: { uI: { value: 0 }, uGlow: { value: 0 }, uT: { value: 0 }, uC: { value: new THREE.Vector2(0.5, 0.5) } },
  depthTest: false, depthWrite: false });
const bg = new THREE.Mesh(new THREE.PlaneGeometry(2, 2), bgMat); bg.frustumCulled = false; bg.renderOrder = -10; scene.add(bg);

// dust: soft warm motes at every depth, drifting up; the near ones out of focus
const ND = 340, dPos = [], dRnd = [];
for (let i = 0; i < ND; i++) {
  const z = -14 + 20 * Math.pow(rnd(), 0.8);
  const spread = (CAMZ - z) * 0.30;
  dPos.push((rnd() - 0.5) * 2 * spread * 1.9, (rnd() - 0.5) * 2 * spread * 1.15, z);
  dRnd.push(rnd(), rnd(), rnd(), rnd());
}
const dustGeo = new THREE.BufferGeometry();
dustGeo.setAttribute('position', new THREE.Float32BufferAttribute(dPos, 3));
dustGeo.setAttribute('aRnd', new THREE.Float32BufferAttribute(dRnd, 4));
const dustMat = new THREE.ShaderMaterial({
  vertexShader: `attribute vec4 aRnd; uniform float uT, uPush, uI; varying float vA; varying vec3 vCol; varying float vSharp;
    uniform vec3 uCream, uOrange;
    void main() {
      vec3 p = position;
      p.y += uT * (0.05 + 0.10 * aRnd.x); p.x += 0.12 * sin(uT * (0.3 + 0.4 * aRnd.y) + aRnd.z * 6.28);
      vec2 d = p.xy; float l = length(d) + 0.3; p.xy += d / l * uPush * (0.5 + aRnd.w) / (0.6 + 0.25 * l);
      vec4 mv = modelViewMatrix * vec4(p, 1.);
      float dist = -mv.z, blur = abs(dist - 8.0) * 0.11;
      float sz = (0.020 + 0.030 * aRnd.w) + blur * 0.16;
      gl_PointSize = clamp(sz * ${(H / 2 / Math.tan(FOV * Math.PI / 360)).toFixed(1)} / dist, 1.5, 90.);
      vA = uI * (0.10 + 0.25 * aRnd.y) * (0.6 + 0.4 * sin(uT * (1.0 + 2.0 * aRnd.z) + aRnd.x * 9.)) / (1. + blur * 6.);
      vCol = mix(uOrange, uCream, aRnd.z * aRnd.z); vSharp = 1. / (1. + blur * 3.);
      gl_Position = projectionMatrix * mv;
    }`,
  fragmentShader: `varying float vA; varying vec3 vCol; varying float vSharp;
    void main() { vec2 c = gl_PointCoord - 0.5; float r = length(c) * 2.;
      float a = mix(smoothstep(1., 0.6, r), exp(-r * r * 4.), vSharp); gl_FragColor = vec4(vCol * a * vA, 0.); }`,
  uniforms: { uT: { value: 0 }, uPush: { value: 0 }, uI: { value: 0 }, uCream: { value: CREAM }, uOrange: { value: ORANGE } },
  blending: THREE.AdditiveBlending, transparent: true, depthTest: false, depthWrite: false });
const dust = new THREE.Points(dustGeo, dustMat); dust.frustumCulled = false; dust.renderOrder = -5; scene.add(dust);

// the name: the mark and the letters, on one plane that turns to face the camera
const group = new THREE.Group(); scene.add(group);
const SWEEP = { uSweep: { value: -10 }, uSweepI: { value: 0 } };
const lettersTex = await new THREE.TextureLoader().loadAsync('letters.png');
lettersTex.colorSpace = THREE.NoColorSpace; lettersTex.generateMipmaps = true; lettersTex.minFilter = THREE.LinearMipmapLinearFilter; lettersTex.anisotropy = 8;
const VW = `varying vec2 vUv; varying vec3 vW; void main() { vUv = uv; vec4 w = modelMatrix * vec4(position, 1.); vW = w.xyz; gl_Position = projectionMatrix * viewMatrix * w; }`;
const letters = layout.letters.map((L, i) => {
  const m = new THREE.ShaderMaterial({
    vertexShader: VW,
    fragmentShader: `uniform sampler2D uTex; uniform float uCell, uA, uMarkX, uMaskW, uMask, uSweep, uSweepI; uniform vec3 uGlint;
      varying vec2 vUv; varying vec3 vW;
      vec3 lin(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
      void main() {
        vec4 s = texture2D(uTex, vec2((uCell + vUv.x) / 4., vUv.y));
        vec3 c = lin(s.rgb);
        float a = s.a * uA * mix(1., smoothstep(0.62, 1.0, abs(vW.x - uMarkX) / uMaskW), uMask);   // out from behind the mark
        c += uGlint * exp(-pow((vW.x - uSweep + vW.y * 0.4) / 0.16, 2.)) * uSweepI;
        gl_FragColor = vec4(c * a, a);
      }`,
    uniforms: Object.assign({ uTex: { value: lettersTex }, uCell: { value: L.cell }, uA: { value: 0 }, uMarkX: { value: 0 }, uMaskW: { value: 1 }, uMask: { value: 1 }, uGlint: { value: new THREE.Vector3(1.2, 0.75, 0.5) } }, SWEEP),
    transparent: true, depthTest: false, depthWrite: false,
    blending: THREE.CustomBlending, blendSrc: THREE.OneFactor, blendDst: THREE.OneMinusSrcAlphaFactor });
  const mesh = new THREE.Mesh(new THREE.PlaneGeometry(L.w, L.h), m);
  mesh.renderOrder = 1; group.add(mesh);
  return { L, mesh, m };
});

// the mark, drawn exactly (crux_mark): a square of 100; thin bars 7 wide, 7 to either side of the
// middle, broad one 16 wide, all 84 long with round ends, the thin ones cut 2.5 clear of the broad one
const markMat = new THREE.ShaderMaterial({
  vertexShader: VW,
  fragmentShader: `
    uniform float uA, uB, uO, uRA, uRB, uRO, uGuide, uGuideR, uGA, uGB, uGO, uTrail, uRun, uRunI, uFlash, uSweep, uSweepI, uOn;
    uniform vec3 uCream, uOrange, uGlint;
    varying vec2 vUv; varying vec3 vW;
    const vec2 AX = vec2(0.70710678, 0.70710678), AT = vec2(0.70710678, -0.70710678);   // the broad bar's way, the thin ones'
    float cap(vec2 p, vec2 c, vec2 ax, float hl, float r) { vec2 q = p - c; float t = clamp(dot(q, ax), -hl, hl); return length(q - ax * t) - r; }
    void main() {
      vec2 p = (vUv - 0.5) * ${MP.toFixed(1)};                       // the mark's units, y up
      float aa = max(fwidth(p.x), fwidth(p.y)) * 0.75;
      // the slot the broad bar leaves in the thin ones (where it rests), and the three bars where they are now
      float gap = cap(p, vec2(0.), AX, 34., 10.5);
      float dA = max(cap(p, AX * 7. - AT * uA, AT, 38.5, 3.5), -gap);
      float dB = max(cap(p, -AX * 7. + AT * uB, AT, 38.5, 3.5), -gap);
      float dO = cap(p, -AX * uO, AX, 34., 8.);
      // where they will rest: thin guide lines, drawn out from the middle
      float gA = max(cap(p, AX * 7., AT, 38.5, 3.5), -gap), gB = max(cap(p, -AX * 7., AT, 38.5, 3.5), -gap), gO = cap(p, vec2(0.), AX, 34., 8.);
      float reach = smoothstep(uGuideR, uGuideR - 8., length(p));
      float line = 0.35;
      float guide = (smoothstep(line + aa, line - aa, abs(gA)) * uGA + smoothstep(line + aa, line - aa, abs(gB)) * uGB + smoothstep(line + aa, line - aa, abs(gO)) * uGO) * reach * uGuide;
      float cA = smoothstep(aa, -aa, dA), cB = smoothstep(aa, -aa, dB), cO = smoothstep(aa, -aa, dO);
      // a little relief: the top-left edges catch the light, the far ones fall off
      vec2 L = normalize(vec2(-0.6, 0.8));
      float relA = 1. + 0.08 * smoothstep(-2.2, 0., dA) * dot(normalize(p - AX * 7. + AT * uA - AT * clamp(dot(p - AX * 7. + AT * uA, AT), -38.5, 38.5) + 1e-4), L);
      float relB = 1. + 0.08 * smoothstep(-2.2, 0., dB) * dot(normalize(p + AX * 7. - AT * uB - AT * clamp(dot(p + AX * 7. - AT * uB, AT), -38.5, 38.5) + 1e-4), L);
      vec2 qo = p + AX * uO; vec2 no = qo - AX * clamp(dot(qo, AX), -34., 34.);
      float relO = 1. + 0.10 * smoothstep(-4., 0., dO) * dot(normalize(no + 1e-4), L);
      vec3 cream = uCream, orange = mix(uOrange, vec3(1., 0.36, 0.17), 0.35 * uFlash);
      // the light that runs along the bars (along the broad one's way), and the one across the name
      float run = exp(-pow((dot(p, AX) - uRun) / 9., 2.)) * uRunI;
      float sw = exp(-pow((vW.x - uSweep + vW.y * 0.4) / 0.16, 2.)) * uSweepI;
      vec3 glint = uGlint * (run + sw);
      // premultiplied, the orange over the cream
      vec3 col = vec3(0.); float a = 0.;
      vec3 c1 = (cream * relA + glint) * cA; col = c1 + col * (1. - cA); a = cA + a * (1. - cA);
      vec3 c2 = (cream * relB + glint) * cB; col = c2 + col * (1. - cB); a = cB + a * (1. - cB);
      vec3 c3 = (orange * relO + glint * 1.2) * cO; col = c3 + col * (1. - cO); a = cO + a * (1. - cO);
      // the orange bar's trail (behind it, along its way) and its glow; the thin bars' faint ones
      float along = dot(qo, AX), across = dot(qo, vec2(-AX.y, AX.x));
      float behind = max(-34. - along, 0.);
      float trail = exp(-behind / 150.) * exp(-pow(across / (5.5 + behind * 0.02), 2.)) * step(along, -20.) * smoothstep(-20., -34., along) * uTrail;
      trail += exp(-max(dO, 0.) / 7.) * (0.10 * uTrail + 0.22 * uFlash) * (1. - cO);
      vec2 qa = p - AX * 7. + AT * uA, qb = p + AX * 7. - AT * uB;
      float ta = exp(-max(-38.5 - dot(qa, AT), 0.) / 60.) * exp(-pow(dot(qa, AX) / 3.2, 2.)) * step(dot(qa, AT), -30.) * uRA;
      float tb = exp(-max(dot(qb, AT) - 38.5, 0.) / 60.) * exp(-pow(dot(qb, AX) / 3.2, 2.)) * step(30., dot(qb, AT)) * uRB;
      col += (uOrange * vec3(1., 1.25, 1.6) * trail * 1.6 + uCream * (ta + tb) * 0.30) * (1. - a);
      col += uOrange * 0.55 * guide * (1. - a);
      gl_FragColor = vec4(col * uOn, a * uOn);
    }`,
  uniforms: Object.assign({ uA: { value: 300 }, uB: { value: 300 }, uO: { value: 300 }, uRA: { value: 0 }, uRB: { value: 0 }, uRO: { value: 0 },
    uGuide: { value: 0 }, uGuideR: { value: 0 }, uGA: { value: 1 }, uGB: { value: 1 }, uGO: { value: 1 }, uTrail: { value: 0 }, uRun: { value: -100 }, uRunI: { value: 0 },
    uFlash: { value: 0 }, uOn: { value: 1 }, uCream: { value: CREAM }, uOrange: { value: ORANGE }, uGlint: { value: new THREE.Vector3(1.2, 0.75, 0.5) } }, SWEEP),
  transparent: true, depthTest: false, depthWrite: false,
  blending: THREE.CustomBlending, blendSrc: THREE.OneFactor, blendDst: THREE.OneMinusSrcAlphaFactor });
const mark = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), markMat); mark.renderOrder = 2; group.add(mark);

// ---------------------------------------------------------------- post: soft bloom, the sRGB curve, a dither
const RT = (w, h) => new THREE.WebGLRenderTarget(w, h, { type: THREE.HalfFloatType, depthBuffer: false, minFilter: THREE.LinearFilter, magFilter: THREE.LinearFilter, wrapS: THREE.ClampToEdgeWrapping, wrapT: THREE.ClampToEdgeWrapping });
const rtScene = RT(W, H), rtAccum = RT(W, H);
const NM = 6, down = [], up = [];
for (let i = 0, w = W >> 1, h = H >> 1; i < NM; i++, w = Math.max(1, w >> 1), h = Math.max(1, h >> 1)) { down.push(RT(w, h)); up.push(RT(w, h)); }
const quadScene = new THREE.Scene(), quadCam = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);
const quad = new THREE.Mesh(new THREE.PlaneGeometry(2, 2)); quad.frustumCulled = false; quadScene.add(quad);
const VS = `varying vec2 vUv; void main() { vUv = uv; gl_Position = vec4(position.xy, 0., 1.); }`;
const mk = (fs, uniforms, extra) => new THREE.ShaderMaterial(Object.assign({ vertexShader: VS, fragmentShader: fs, uniforms, depthTest: false, depthWrite: false }, extra || {}));
function pass(mat, target, clear) { quad.material = mat; renderer.setRenderTarget(target); if (clear) renderer.clear(); renderer.render(quadScene, quadCam); }
const mAcc = mk(`uniform sampler2D t; uniform float w; varying vec2 vUv; void main() { gl_FragColor = vec4(min(texture2D(t, vUv).rgb, vec3(40.)) * w, 1.); }`,
  { t: { value: null }, w: { value: 1 } }, { blending: THREE.AdditiveBlending, transparent: true });
const mDown = mk(`uniform sampler2D t; uniform vec2 px; uniform float thr; varying vec2 vUv;
  vec3 s(vec2 o) { vec3 c = texture2D(t, vUv + o * px).rgb; float l = max(c.r, max(c.g, c.b)); float k = clamp((l - thr + 0.25) / 0.5, 0., 1.); return c * max(l - thr, k * k * 0.125) / max(l, 1e-4); }
  void main() { vec3 c = s(vec2(0.)) * 4. + s(vec2(1., 1.)) + s(vec2(-1., 1.)) + s(vec2(1., -1.)) + s(vec2(-1., -1.)); gl_FragColor = vec4(c / 8., 1.); }`,
  { t: { value: null }, px: { value: new THREE.Vector2() }, thr: { value: 0 } });
const mUp = mk(`uniform sampler2D t, b; uniform vec2 px; varying vec2 vUv;
  void main() { vec3 c = 2. * (texture2D(t, vUv + vec2(1., 1.) * px).rgb + texture2D(t, vUv + vec2(-1., 1.) * px).rgb + texture2D(t, vUv + vec2(1., -1.) * px).rgb + texture2D(t, vUv + vec2(-1., -1.) * px).rgb)
      + texture2D(t, vUv + vec2(-2., 0.) * px).rgb + texture2D(t, vUv + vec2(2., 0.) * px).rgb + texture2D(t, vUv + vec2(0., 2.) * px).rgb + texture2D(t, vUv + vec2(0., -2.) * px).rgb;
    gl_FragColor = vec4(texture2D(b, vUv).rgb + c / 12., 1.); }`,
  { t: { value: null }, b: { value: null }, px: { value: new THREE.Vector2() } });
const mFinal = mk(`uniform sampler2D tS, tB; uniform float uBloom, uFade, uSeed; varying vec2 vUv;
  float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
  vec3 srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1. / 2.4)) - 0.055, step(0.0031308, c)); }
  void main() {
    vec3 col = texture2D(tS, vUv).rgb + texture2D(tB, vUv).rgb * uBloom;
    col = mix(col, 1. + 0.06 * (1. - exp(-(col - 1.) / 0.06)), step(1., col));     // a soft shoulder over the brand's colours
    vec2 c = vUv - 0.5; col *= 1. - 0.18 * smoothstep(0.35, 0.85, length(c * vec2(1.4, 1.)));
    col = srgb(clamp(col, 0., 1.)) * uFade;
    col += (hash(gl_FragCoord.xy + uSeed) + hash(gl_FragCoord.yx * 1.37 + uSeed) - 1.) / 255. * step(0.002, uFade);
    gl_FragColor = vec4(col, 1.);
  }`, { tS: { value: null }, tB: { value: null }, uBloom: { value: 0.2 }, uFade: { value: 1 }, uSeed: { value: 0 } });

// ---------------------------------------------------------------- the timeline
const clamp01 = x => Math.min(1, Math.max(0, x));
const sm = (a, b, x) => { const t = clamp01((x - a) / (b - a)); return t * t * (3 - 2 * t); };
const smoother = (a, b, x) => { const t = clamp01((x - a) / (b - a)); return t * t * t * (t * (t * 6 - 15) + 10); };
const dec = (t, t0, k) => t < t0 ? 0 : Math.exp(-k * (t - t0));
// a part that slides into its slot: fast, braking, still moving when it hits the stop, a small recoil
const recoil = (t, t1, r) => { if (t < t1) return 0; const d = (t - t1) * 30; return r * d * Math.exp(1 - d); };
function slot(t, t0, t1, dist, r) {
  if (t < t0) return dist;
  if (t < t1) { const u = (t - t0) / (t1 - t0); return dist * (1 - u) * (1 - 0.8 * u); }
  return recoil(t, t1, r);
}
const speed = (t, t0, t1) => t > t0 - 0.02 && t < t1 + 0.08;
function apply(t) {
  // the camera: a slow push in; the plane of the mark turns to face it
  camera.position.set(0, 0, CAMZ + 0.7 * (1 - smoother(0, 4.2, t)) - 0.12 * sm(4.2, T.end, t));
  camera.lookAt(0, 0, 0);
  const turn = 1 - smoother(0.0, 2.6, t);
  group.rotation.set(0.28 * turn, -0.62 * turn, -0.06 * turn);
  const jolt = 0.012 * dec(t, T.o1, 9) * Math.sin(Math.max(0, t - T.o1) * 60);
  group.position.set(0, jolt, 0);
  // the mark: alone and large, then the X of the name
  const mv = smoother(T.move, T.move1, t);
  const unit = (BIG + (TYPE_W - BIG) * mv) / 100;
  const markX = layout.mark * TYPE_W * mv;
  const thump = 1 + 0.025 * dec(t, T.o1, 10) * Math.sin(Math.min(Math.PI, (t - T.o1) * 25));
  mark.scale.set(MP * unit * thump, MP * unit * thump, 1);
  mark.position.set(markX, 0, 0);
  const u = markMat.uniforms;
  u.uA.value = slot(t, T.a0, T.a1, 260, 1.6);
  u.uB.value = slot(t, T.b0, T.b1, 260, 1.6);
  u.uO.value = slot(t, T.o0, T.o1, 330, 2.4);
  u.uRA.value = speed(t, T.a0, T.a1) ? 1 - sm(T.a1 - 0.05, T.a1 + 0.06, t) : 0;
  u.uRB.value = speed(t, T.b0, T.b1) ? 1 - sm(T.b1 - 0.05, T.b1 + 0.06, t) : 0;
  u.uTrail.value = t < T.o0 ? 0 : t < T.o1 ? sm(T.o0, T.o0 + 0.1, t) : Math.exp(-7 * (t - T.o1));
  u.uFlash.value = dec(t, T.o1, 7);
  u.uGuide.value = sm(T.guide, T.guide + 0.15, t);
  u.uGuideR.value = 8 + 70 * smoother(T.guide, T.guide + 0.45, t);
  u.uGA.value = 1 - sm(T.a1 - 0.02, T.a1 + 0.15, t);
  u.uGB.value = 1 - sm(T.b1 - 0.02, T.b1 + 0.15, t);
  u.uGO.value = 1 - sm(T.o1 - 0.02, T.o1 + 0.15, t);
  u.uRun.value = -62 + 124 * sm(T.run, T.run + 0.5, t);
  u.uRunI.value = sm(T.run, T.run + 0.08, t) * (1 - sm(T.run + 0.4, T.run + 0.55, t)) * 0.9;
  // the letters come out from behind the mark, one after another, to their places
  layout.letters.forEach((L, i) => {
    const o = letters[i], d0 = T.l0 + i * T.LET, d1 = d0 + T.LDUR;
    const fx = L.x * TYPE_W, from = markX;
    const x = fx + (from - fx) * slot(t, d0, d1, 1, -0.012 / Math.max(1e-3, Math.abs(from - fx)));
    o.mesh.scale.set(TYPE_W, TYPE_W, 1);
    o.mesh.position.set(x, 0, -0.001);
    o.m.uniforms.uA.value = t > d0 ? 1 : 0;
    o.m.uniforms.uMarkX.value = markX; o.m.uniforms.uMaskW.value = TYPE_W * 0.42;
    o.m.uniforms.uMask.value = 1 - sm(d1, d1 + 0.05, t);
  });
  SWEEP.uSweep.value = -3.2 + 6.4 * sm(T.sweep, T.sweep1, t);
  SWEEP.uSweepI.value = sm(T.sweep, T.sweep + 0.1, t) * (1 - sm(T.sweep1 - 0.12, T.sweep1, t)) * 0.75;
  // the space and the dust
  bgMat.uniforms.uI.value = sm(0.0, 0.7, t);
  bgMat.uniforms.uGlow.value = 0.25 * sm(T.o0, T.o1, t) + 0.9 * dec(t, T.o1, 4) + 0.25 * sm(T.sweep, T.sweep + 0.3, t) * (1 - sm(T.sweep + 0.3, T.sweep + 1.6, t));
  bgMat.uniforms.uT.value = t;
  dustMat.uniforms.uT.value = t;
  dustMat.uniforms.uI.value = sm(0.1, 1.0, t);
  dustMat.uniforms.uPush.value = t > T.o1 ? 0.35 * (1 - Math.exp(-5 * (t - T.o1))) : 0;
}
const halton = (i, b) => { let f = 1, r = 0; while (i > 0) { f /= b; r += f * (i % b); i = Math.floor(i / b); } return r; };
const moving = t => (t > T.a0 - 0.05 && t < T.o1 + 0.12) || (t > T.move - 0.05 && t < T.land[3] + 0.1);
const proj = new THREE.Vector3();
window.renderFrame = function (t, fps, S) {
  S = S || (moving(t) ? 10 : 4);
  renderer.setRenderTarget(rtAccum); renderer.setClearColor(0x000000, 1); renderer.clear();
  for (let s = 0; s < S; s++) {
    const ts = t + ((s + 0.5) / S - 0.5) * 0.5 / fps;                // a half-frame shutter
    apply(ts);
        camera.setViewOffset(W, H, halton(s + 1, 2) - 0.5, halton(s + 1, 3) - 0.5, W, H);
    camera.updateProjectionMatrix(); camera.updateMatrixWorld(); scene.updateMatrixWorld();
    proj.set(0, 0, 0).applyMatrix4(group.matrixWorld).project(camera);
    bgMat.uniforms.uC.value.set(proj.x * 0.5 + 0.5, proj.y * 0.5 + 0.5);
    renderer.setRenderTarget(rtScene); renderer.clear(); renderer.render(scene, camera);
    mAcc.uniforms.t.value = rtScene.texture; mAcc.uniforms.w.value = 1 / S; pass(mAcc, rtAccum, false);
  }
  apply(t); camera.clearViewOffset(); camera.updateProjectionMatrix();
  let src = rtAccum;
  for (let i = 0; i < NM; i++) { mDown.uniforms.t.value = src.texture; mDown.uniforms.px.value.set(1 / src.width, 1 / src.height); mDown.uniforms.thr.value = i === 0 ? 0.85 : 0; pass(mDown, down[i], true); src = down[i]; }
  let prev = down[NM - 1];
  for (let i = NM - 2; i >= 0; i--) { mUp.uniforms.t.value = prev.texture; mUp.uniforms.b.value = down[i].texture; mUp.uniforms.px.value.set(1 / prev.width, 1 / prev.height); pass(mUp, up[i], true); prev = up[i]; }
  const f = mFinal.uniforms;
  f.tS.value = rtAccum.texture; f.tB.value = up[0].texture;
  f.uBloom.value = 0.12 + 0.15 * dec(t, T.o1, 5) + 0.1 * markMat.uniforms.uRunI.value + 0.12 * SWEEP.uSweepI.value;
  f.uFade.value = sm(0, 0.3, t) * (1 - smoother(T.fade, T.end - 0.08, t));
  f.uSeed.value = 17.0;                                             // a still dither: the encoder keeps it, the gradient keeps no steps
  pass(mFinal, null, true);
};
window.grab = () => canvas.toDataURL('image/png');
window.ready = true;
