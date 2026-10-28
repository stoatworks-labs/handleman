/**
 * The browser demo against the plugin, pixel for pixel, from the same frames.
 *
 *     cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 && cmake --build build
 *     python3 -m http.server 8717 --bind 127.0.0.1 --directory demo &
 *     node demo/tools/crosscheck/crosscheck.mjs [--url http://127.0.0.1:8717/] [--hmtest build/hmtest]
 *                                              [--size 320x180] [--work DIR] [--gl metal|swiftshader]
 *
 * Exit 0 when every scenario agrees and both negative controls disagree.
 *
 * ------------------------------------------------------------------- what it does
 *
 * The page's CPU half -- the two hands, the shutter's weights, every
 * pull-down, the camera's exposures and ring layers, the weave, the marks, the
 * reel -- is a hand port of Model.cpp and Handleman::ProcessOpenGL, and
 * check_shaders.py cannot see it. This drives the page in headless Chrome
 * (cdp.mjs, over a pipe: no debugging port) one frame at a time from a fresh
 * instance, the clock at n / 60 as `hmtest --pipe --fps 60` clocks it, reads
 * back both the clip the page fed the plugin and what the plugin's shaders
 * drew, then pipes those same input frames through `hmtest --pipe` with the
 * same values `--set` (and Run Out pressed by `--script` on the same frames)
 * and compares the two outputs channel by channel. A wrong hand, a pull-down a
 * frame early, a weave off by a hundredth or a reel a cell out shows as
 * differences of tens of levels; the two GL stacks' last-bit rounding shows as
 * ones. Tolerance: 1/255 per channel, every frame.
 *
 * First it holds the page's controls to `hmtest --list`: the same names, in
 * the same order, of the same kinds, with the same defaults (FF_TYPE_EVENT is a
 * boolean toggle on the page; the About block is absent).
 *
 * Two negative controls prove it can fail: the page at Weave 0.36 against the
 * plugin at 0.35, and at Camera Hand 0.31 against 0.3. Each must differ by
 * more than the tolerance somewhere.
 *
 * ------------------------------------------------------------------- what it cannot
 *
 * A resize mid-run (the ring's resample) is not exercised: --pipe has one
 * raster. Nor is a browser's own clock: the page is driven at exactly n / 60,
 * as hmtest is, so a real display's jitter is not in it. Scratch output goes
 * to --work (default: a new folder under $TMPDIR).
 */
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { launch } from './cdp.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..', '..', '..');

const args = Object.fromEntries(process.argv.slice(2).reduce((pairs, a, i, all) => {
  if (a.startsWith('--')) pairs.push([a.slice(2), all[i + 1]]);
  return pairs;
}, []));
const URL_ = args.url ?? 'http://127.0.0.1:8717/';
const HMTEST = resolve(args.hmtest ?? join(REPO, 'build', 'hmtest'));
const [W, H] = (args.size ?? '320x180').split('x').map(Number);
const WORK = args.work ? resolve(args.work) : mkdtempSync(join(tmpdir(), 'hm-crosscheck-'));
mkdirSync(WORK, { recursive: true });
const TOLERANCE = 1;

if (!existsSync(HMTEST)) {
  console.error(`crosscheck: no hmtest at ${HMTEST} -- build it first (see the top of this file)`);
  process.exit(2);
}

/// What each scenario sets, by the page's parameter ids; the plugin gets the
/// same values by name. `presses` are display frames on which Run Out goes 1.
const SCENARIOS = [
  { name: 'defaults', clip: 'scene', frames: 120, values: {} },
  {
    name: 'both hands at full, 24 fps, 2 blades at 180, panchromatic B&W, worn, framed off',
    clip: 'scene', frames: 120,
    values: {
      crank: '1', cameraHand: '1', projectorHand: '1', blades: '1', shutter: '0.5', stock: '1', tone: '1',
      contrast: '0.8', grain: '0.7', weave: '0.9', scratches: '1', dust: '1', hair: '1', framing: '0.4', vignette: '1',
    },
  },
  {
    name: 'run out at 0.25 s, Off the End, a new roll at frame 200',
    clip: 'scene', frames: 260, presses: [10, 200],
    values: { runOutTime: '0', tail: '2', hair: '1' },
  },
  {
    name: 'run out, Framed, amber tint, low contrast, on the bars',
    clip: 'bars', frames: 240, presses: [20],
    values: { runOutTime: '0.1', tail: '1', tone: '3', contrast: '0.2', projectorHand: '1' },
  },
  {
    name: 'Colour at Mix 0.7 on transparency, 8 fps, camera hand only, 1 blade at 126',
    clip: 'alpha', frames: 120,
    values: { tone: '0', mix: '0.7', crank: '0', cameraHand: '1', projectorHand: '0', blades: '0', shutter: '0.3', vignette: '0' },
  },
  {
    name: 'NEGATIVE: the page at Weave 0.36, the plugin at 0.35',
    clip: 'scene', frames: 60, negative: true,
    values: { weave: '0.36' }, pluginValues: { weave: '0.35' },
  },
  {
    name: 'NEGATIVE: the page at Camera Hand 0.31, the plugin at 0.3',
    clip: 'scene', frames: 120, negative: true,
    values: { cameraHand: '0.31' }, pluginValues: { cameraHand: '0.3' },
  },
];

const PAGE_SIDE = `
window.__cc = {
  b64(u8) {
    let s = '';
    for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
    return btoa(s);
  },
  tick() { return new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))); },
  async setup(clip, values, w, h) {
    const d = window.__handlemanDemo;
    const st = d.demo.state;
    st.playing = false;
    st.sourceId = clip;
    st.resolution = { id: 'crosscheck', name: 'crosscheck', width: w, height: h };
    d.demo.params.apply(values);
    d.demo.redraw();
    await this.tick();
    d.hooks.fresh();
    st.time = 0;
    st.frameIndex = 0;
    return d.demo.params.toQuery().toString();
  },
  step(n, press) {
    return new Promise((resolve) => {
      const d = window.__handlemanDemo;
      if (press) d.demo.params.set('runOut', 1, { silent: true });
      d.demo.state.time = n / 60;
      d.hooks.afterRender = (gl, input) => {
        d.hooks.afterRender = null;
        const w = gl.drawingBufferWidth, h = gl.drawingBufferHeight;
        const out = new Uint8Array(w * h * 4);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, out);
        const fbo = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, input.texture, 0);
        const inp = new Uint8Array(input.width * input.height * 4);
        gl.readPixels(0, 0, input.width, input.height, gl.RGBA, gl.UNSIGNED_BYTE, inp);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.deleteFramebuffer(fbo);
        const t = d.telemetry;
        resolve({ w, h, out: this.b64(out), inp: this.b64(inp), ring: t.ringDepth, fog: t.fog, noFilm: t.noFilm });
      };
      d.demo.redraw();
    });
  },
};
true`;

/// readPixels is bottom-up; --pipe is top-down.
function flip(buf, w, h) {
  const out = Buffer.alloc(buf.length);
  const row = w * 4;
  for (let y = 0; y < h; y += 1) buf.copy(out, (h - 1 - y) * row, y * row, (y + 1) * row);
  return out;
}

function hmtestList() {
  const r = spawnSync(HMTEST, ['--list'], { encoding: 'utf8' });
  const rows = [];
  for (const line of r.stdout.split('\n')) {
    const m = /^\s*(\d+)\s+(.+?)\s{2,}(standard|option|event|about|boolean|integer|text)\s+([-0-9.]+)\s+\[([-0-9.]+)\.\.([-0-9.]+)\]/.exec(line);
    if (m) rows.push({ index: Number(m[1]), name: m[2].trim(), kind: m[3], def: Number(m[4]), hi: Number(m[6]) });
  }
  return rows;
}

function compare(page, plugin, frames, frameBytes) {
  let worst = 0;
  let differing = 0;
  let over = 0;
  const badFrames = [];
  for (let n = 0; n < frames; n += 1) {
    let frameWorst = 0;
    for (let i = n * frameBytes; i < (n + 1) * frameBytes; i += 1) {
      const d = Math.abs(page[i] - plugin[i]);
      if (d === 0) continue;
      differing += 1;
      if (d > TOLERANCE) over += 1;
      if (d > frameWorst) frameWorst = d;
    }
    if (frameWorst > TOLERANCE) badFrames.push(`${n}:${frameWorst}`);
    worst = Math.max(worst, frameWorst);
  }
  return { worst, differing, over, badFrames };
}

const chrome = await launch({ width: 1400, height: 1000, gl: args.gl ?? 'metal' });
let failures = 0;
try {
  const page = await chrome.open(URL_);
  await page.waitFor('window.__handlemanDemo && window.__handlemanDemo.telemetry.ticked === true', 20000);
  await page.evaluate(PAGE_SIDE);
  const renderer = await page.evaluate(`(() => { const gl = document.querySelector('canvas').getContext('webgl2');
    const e = gl.getExtension('WEBGL_debug_renderer_info'); return e ? gl.getParameter(e.UNMASKED_RENDERER_WEBGL) : 'unknown'; })()`);
  console.log(`page renderer: ${renderer}`);

  //--- the controls ----------------------------------------------------------
  const spec = await page.evaluate(`window.__handlemanDemo.demo.params.spec.map((p) => ({ id: p.id, name: p.name, type: p.type, def: p.default, count: p.elements ? p.elements.length : 0 }))`);
  const list = hmtestList().filter((r) => r.kind !== 'about');
  const problems = [];
  if (list.length !== spec.length) problems.push(`hmtest lists ${list.length} controls, the page has ${spec.length}`);
  for (let i = 0; i < Math.min(list.length, spec.length); i += 1) {
    const a = list[i];
    const b = spec[i];
    const kind = { standard: 'standard', option: 'option', event: 'boolean' }[a.kind];
    if (a.name !== b.name) problems.push(`#${i}: plugin '${a.name}', page '${b.name}'`);
    if (kind !== b.type) problems.push(`${a.name}: plugin ${a.kind}, page ${b.type}`);
    if (Math.abs(a.def - b.def) > 5e-5) problems.push(`${a.name}: default ${a.def} in the plugin, ${b.def} on the page`);
    if (a.kind === 'option' && a.hi + 1 !== b.count) problems.push(`${a.name}: ${a.hi + 1} elements in the plugin, ${b.count} on the page`);
  }
  if (problems.length) {
    failures += 1;
    console.log(`FAIL  the controls differ from hmtest --list:\n      ${problems.join('\n      ')}`);
  } else {
    console.log(`ok    the page's ${spec.length} controls are hmtest --list's: names, order, kinds, defaults, element counts`);
  }
  const nameOf = Object.fromEntries(spec.map((p) => [p.id, p.name]));

  //--- the frames ------------------------------------------------------------
  for (const s of SCENARIOS) {
    const query = await page.evaluate(`window.__cc.setup(${JSON.stringify(s.clip)}, ${JSON.stringify(s.values)}, ${W}, ${H})`);
    const presses = new Set(s.presses ?? []);
    const inputs = [];
    const outputs = [];
    const states = [];
    for (let n = 0; n < s.frames; n += 1) {
      const r = await page.evaluate(`window.__cc.step(${n}, ${presses.has(n)})`);
      if (r.w !== W || r.h !== H) throw new Error(`the page drew ${r.w}x${r.h}, not ${W}x${H}`);
      inputs.push(flip(Buffer.from(r.inp, 'base64'), W, H));
      outputs.push(flip(Buffer.from(r.out, 'base64'), W, H));
      states.push(r);
    }
    const slug = s.name.replace(/[^a-z0-9]+/gi, '-').toLowerCase().slice(0, 40);
    const inputPath = join(WORK, `${slug}.in.rgba`);
    writeFileSync(inputPath, Buffer.concat(inputs));
    writeFileSync(join(WORK, `${slug}.page.rgba`), Buffer.concat(outputs));

    const pluginValues = { ...s.values, ...(s.pluginValues ?? {}) };
    const sets = Object.entries(pluginValues).flatMap(([id, v]) => ['--set', `${nameOf[id]}=${v}`]);
    const cmd = ['--pipe', '--size', `${W}x${H}`, '--fps', '60', ...sets];
    if (presses.size) {
      const script = join(WORK, `${slug}.cues`);
      writeFileSync(script, [...presses].map((n) => `${n} Run Out 1\n`).join(''));
      cmd.push('--script', script);
    }
    const run = spawnSync(HMTEST, cmd, { input: readFileSync(inputPath), maxBuffer: 1 << 30 });
    if (run.status !== 0) throw new Error(`hmtest ${cmd.join(' ')} exited ${run.status}: ${run.stderr}`);
    const plugin = run.stdout;
    writeFileSync(join(WORK, `${slug}.plugin.rgba`), plugin);
    const pageOut = Buffer.concat(outputs);
    if (plugin.length !== pageOut.length) throw new Error(`hmtest wrote ${plugin.length} bytes, the page ${pageOut.length}`);

    const c = compare(pageOut, plugin, s.frames, W * H * 4);
    const total = s.frames * W * H * 4;
    const deepest = Math.max(...states.map((r) => r.ring));
    const blank = states.filter((r) => r.fog >= 1).length;
    const empty = states.filter((r) => r.noFilm).length;
    const what = `${s.frames} frames at ${W}x${H}, clip ${s.clip}${query ? `, ${query}` : ''}${presses.size ? `, Run Out on ${[...presses].join(' and ')}` : ''}`;
    const detail = `worst ${c.worst}/255; ${c.differing} of ${total} channel values differ, ${c.over} by more than ${TOLERANCE}; ring ${deepest} deep`
      + `${blank ? `; ${blank} frames of blank cells` : ''}${empty ? `, ${empty} with no film` : ''}`;
    const agrees = c.worst <= TOLERANCE;
    if (s.negative ? agrees : !agrees) failures += 1;
    if (s.negative) {
      console.log(`${agrees ? 'FAIL' : 'ok  '}  ${s.name}: ${agrees ? 'the comparer did not notice' : 'caught'} (${detail})`);
    } else {
      console.log(`${agrees ? 'ok  ' : 'FAIL'}  ${s.name}: ${what}; ${detail}`);
      if (!agrees) console.log(`      frames over tolerance (frame:worst): ${c.badFrames.slice(0, 20).join(' ')}${c.badFrames.length > 20 ? ' ...' : ''}`);
    }
  }
  if (page.exceptions.length) {
    failures += 1;
    console.log(`FAIL  the page threw: ${page.exceptions.join(' | ')}`);
  }
} finally {
  await chrome.close();
}
console.log(`\nwork files in ${WORK}`);
console.log(failures ? `${failures} check(s) failed` : 'the page agrees with hmtest --pipe on every scenario, and both negative controls are caught');
process.exit(failures ? 1 : 0);
