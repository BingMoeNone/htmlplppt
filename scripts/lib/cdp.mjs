import { spawn } from 'node:child_process';
import { setTimeout as delay } from 'node:timers/promises';

export async function launchChrome({ chromeBin = process.env.CHROME_BIN || 'google-chrome', port = 9333, userDataDir }) {
  if (!userDataDir) throw new Error('userDataDir is required');
  const proc = spawn(chromeBin, [
    '--headless=new',
    `--remote-debugging-port=${port}`,
    `--user-data-dir=${userDataDir}`,
    '--no-first-run',
    '--no-default-browser-check',
    '--disable-gpu',
    '--hide-scrollbars',
    '--allow-file-access-from-files',
    '--force-device-scale-factor=1',
    '--window-size=1920,1080',
    'about:blank'
  ], { stdio: ['ignore', 'ignore', 'pipe'] });

  let stderr = '';
  proc.stderr.setEncoding('utf8');
  proc.stderr.on('data', (chunk) => { stderr += chunk; });

  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`http://127.0.0.1:${port}/json/version`);
      if (res.ok) {
        const info = await res.json();
        return { proc, port, browserWsUrl: info.webSocketDebuggerUrl };
      }
    } catch {
      // Chrome not ready yet.
    }
    await delay(100);
  }
  proc.kill('SIGKILL');
  throw new Error(`Chrome did not start on port ${port}: ${stderr.slice(-500)}`);
}

export async function openPage(port, url) {
  const res = await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(url)}`, { method: 'PUT' });
  if (!res.ok) throw new Error(`/json/new failed: ${res.status}`);
  const target = await res.json();
  const cdp = new Cdp(target.webSocketDebuggerUrl);
  await cdp.open();
  return cdp;
}

export class Cdp {
  #ws;
  #nextId = 1;
  #pending = new Map();
  #listeners = new Map();

  constructor(url) {
    this.#ws = new WebSocket(url);
  }

  async open() {
    await new Promise((resolve, reject) => {
      this.#ws.addEventListener('open', () => resolve(), { once: true });
      this.#ws.addEventListener('error', () => reject(new Error('CDP WebSocket error')), { once: true });
    });
    this.#ws.addEventListener('message', (event) => this.#onMessage(event));
  }

  #onMessage(event) {
    const message = JSON.parse(event.data);
    if (message.id && this.#pending.has(message.id)) {
      const { resolve, reject } = this.#pending.get(message.id);
      this.#pending.delete(message.id);
      if (message.error) reject(new Error(`${message.error.code}: ${message.error.message}`));
      else resolve(message.result);
      return;
    }
    if (message.method && this.#listeners.has(message.method)) {
      for (const listener of this.#listeners.get(message.method)) listener(message.params);
    }
  }

  command(method, params = {}) {
    const id = this.#nextId++;
    const promise = new Promise((resolve, reject) => {
      this.#pending.set(id, { resolve, reject });
    });
    this.#ws.send(JSON.stringify({ id, method, params }));
    return promise;
  }

  once(method) {
    return new Promise((resolve) => {
      const listener = (params) => {
        this.off(method, listener);
        resolve(params);
      };
      this.on(method, listener);
    });
  }

  on(method, listener) {
    if (!this.#listeners.has(method)) this.#listeners.set(method, new Set());
    this.#listeners.get(method).add(listener);
  }

  off(method, listener) {
    this.#listeners.get(method)?.delete(listener);
  }

  close() {
    this.#ws.close();
  }
}
