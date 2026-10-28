/**
 * Headless Chrome over the DevTools protocol, on a PIPE rather than a port.
 *
 * `--remote-debugging-pipe` puts the protocol on file descriptors 3 (Chrome
 * reads) and 4 (Chrome writes), each message a JSON object ended by a NUL. No
 * debugging port is opened, so this cannot collide with another session's
 * Chrome or dev server, and nothing else on the machine can attach to it.
 *
 *     const chrome = await launch({ width: 1600, height: 1000 });
 *     const page = await chrome.open('http://127.0.0.1:8717/');
 *     await page.evaluate('1 + 1');
 *     await chrome.close();
 *
 * Node 22+ (no packages). CHROME names the binary; the default is Google
 * Chrome's on macOS.
 */
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const CHROME = process.env.CHROME ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';

export async function launch({ width = 1600, height = 1000, gl = 'metal', extraArgs = [] } = {}) {
  const profile = mkdtempSync(join(tmpdir(), 'hm-cdp-'));
  const glArgs = gl === 'swiftshader'
    ? ['--use-angle=swiftshader', '--enable-unsafe-swiftshader']
    : ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist'];
  const child = spawn(CHROME, [
    '--headless=new', '--no-first-run', '--no-default-browser-check', '--remote-debugging-pipe',
    `--user-data-dir=${profile}`, `--window-size=${width},${height}`, '--hide-scrollbars',
    '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
    ...glArgs, ...extraArgs, 'about:blank',
  ], { stdio: ['ignore', 'ignore', 'ignore', 'pipe', 'pipe'] });

  const toChrome = child.stdio[3];
  const fromChrome = child.stdio[4];
  let id = 0;
  const waiting = new Map();
  const listeners = new Set();
  let buffer = Buffer.alloc(0);

  fromChrome.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    let end;
    while ((end = buffer.indexOf(0)) >= 0) {
      const message = JSON.parse(buffer.subarray(0, end).toString('utf8'));
      buffer = buffer.subarray(end + 1);
      if (message.id !== undefined && waiting.has(message.id)) {
        const { resolve, reject } = waiting.get(message.id);
        waiting.delete(message.id);
        if (message.error) reject(new Error(`${message.error.message} ${message.error.data ?? ''}`));
        else resolve(message.result);
      } else {
        for (const listen of listeners) listen(message);
      }
    }
  });

  const send = (method, params = {}, sessionId = undefined) => new Promise((resolve, reject) => {
    id += 1;
    waiting.set(id, { resolve, reject });
    const message = { id, method, params };
    if (sessionId) message.sessionId = sessionId;
    toChrome.write(`${JSON.stringify(message)}\0`);
  });

  await send('Browser.getVersion');

  return {
    async open(url) {
      const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
      const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
      const call = (method, params) => send(method, params, sessionId);
      const console = [];
      const exceptions = [];
      listeners.add((m) => {
        if (m.sessionId !== sessionId) return;
        if (m.method === 'Runtime.consoleAPICalled') {
          console.push({ type: m.params.type, text: m.params.args.map((a) => a.value ?? a.description ?? '').join(' ') });
        } else if (m.method === 'Runtime.exceptionThrown') {
          const d = m.params.exceptionDetails;
          exceptions.push(`${d.text} ${d.exception?.description ?? ''}`);
        } else if (m.method === 'Log.entryAdded') {
          console.push({ type: m.params.entry.level, text: `${m.params.entry.source}: ${m.params.entry.text}` });
        }
      });
      await call('Page.enable');
      await call('Runtime.enable');
      await call('Log.enable');
      await call('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
      await call('Page.navigate', { url });
      const page = {
        console,
        exceptions,
        call,
        async evaluate(expression) {
          const r = await call('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
          if (r.exceptionDetails) throw new Error(`${r.exceptionDetails.text} ${r.exceptionDetails.exception?.description ?? ''}`);
          return r.result.value;
        },
        async waitFor(expression, timeoutMs = 20000) {
          const start = Date.now();
          for (;;) {
            try {
              if (await page.evaluate(expression)) return true;
            } catch { /* not there yet */ }
            if (Date.now() - start > timeoutMs) throw new Error(`timed out waiting for: ${expression}`);
            await new Promise((r) => setTimeout(r, 100));
          }
        },
        async screenshot() {
          const r = await call('Page.captureScreenshot', { format: 'png' });
          return Buffer.from(r.data, 'base64');
        },
      };
      return page;
    },
    async close() {
      const exited = new Promise((resolve) => {
        if (child.exitCode !== null) resolve();
        else child.once('exit', resolve);
      });
      try { await send('Browser.close'); } catch { /* already gone */ }
      const timer = setTimeout(() => child.kill('SIGKILL'), 5000);
      await exited;
      clearTimeout(timer);
      // Chrome can still be flushing its profile as it goes.
      rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
    },
  };
}
