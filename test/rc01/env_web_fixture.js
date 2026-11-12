// SPDX-License-Identifier: GPL-2.0
//
// RC01 UB03 host regression for the web ENV pages.
//
// Runs the real inline scripts of failsafe/html/pig/about.html and
// term.html (and the real common.js) in a Node vm with a minimal DOM,
// XMLHttpRequest/fetch and dialog stubs, then clicks the actual buttons.
//
//   node env_web_fixture.js <common.js> <about.html> <term.html> [--list|name]
//
// about.html: every /env_set modify/restore entry must check the reply
// body; "error", "save_failed", HTTP errors, network errors and timeouts
// must tell the user that persistence is not confirmed, "ok" must not.
//
// term.html: the commands posted to /webterm/cmd are executed by a model
// of the hush list operators (";" "&&" "||", as in run_list_real) with
// setenv/env default/saveenv stubs: a failing first command must not lead
// to saveenv and the list result must be a failure.
'use strict';

const fs = require('fs');
const vm = require('vm');

const [commonPath, aboutPath, termPath, which] = process.argv.slice(2);

// ---------------------------------------------------------------- mini DOM

class ClassList {
  constructor(el) { this.el = el; this.set = new Set(); }
  add(...c) { c.forEach((x) => this.set.add(x)); }
  remove(...c) { c.forEach((x) => this.set.delete(x)); }
  contains(c) { return this.set.has(c); }
  toggle(c, force) {
    const on = force === undefined ? !this.set.has(c) : !!force;
    if (on) this.set.add(c); else this.set.delete(c);
    return on;
  }
}

class El {
  constructor(tag, doc) {
    this.tagName = String(tag).toUpperCase();
    this.ownerDocument = doc;
    this.children = [];
    this.parentNode = null;
    this.attrs = {};
    this.classList = new ClassList(this);
    this.style = {};
    this._text = '';
    this._html = '';
    this.listeners = {};
    this.value = '';
    this.disabled = false;
  }
  get id() { return this.attrs.id || ''; }
  set id(v) { this.attrs.id = v; }
  get className() { return [...this.classList.set].join(' '); }
  set className(v) {
    this.classList.set = new Set(String(v).split(/\s+/).filter(Boolean));
  }
  setAttribute(k, v) {
    if (k === 'class') this.className = v; else this.attrs[k] = String(v);
  }
  getAttribute(k) {
    if (k === 'class') return this.className;
    return Object.prototype.hasOwnProperty.call(this.attrs, k) ? this.attrs[k] : null;
  }
  hasAttribute(k) { return this.getAttribute(k) !== null; }
  appendChild(c) {
    if (c.parentNode) c.parentNode.removeChild(c);
    c.parentNode = this;
    this.children.push(c);
    return c;
  }
  removeChild(c) {
    this.children = this.children.filter((x) => x !== c);
    c.parentNode = null;
    return c;
  }
  hasChildNodes() { return this.children.length > 0 || this._text !== ''; }
  get textContent() {
    return this._text + this.children.map((c) => c.textContent).join('');
  }
  set textContent(v) { this.children = []; this._html = ''; this._text = String(v); }
  get innerHTML() { return this._html; }
  set innerHTML(v) {
    this.children = [];
    this._text = String(v).replace(/<[^>]*>/g, '');
    this._html = String(v);
  }
  insertRow() { return this.appendChild(new El('tr', this.ownerDocument)); }
  insertCell() { return this.appendChild(new El('td', this.ownerDocument)); }
  addEventListener(t, fn) { (this.listeners[t] = this.listeners[t] || []).push(fn); }
  removeEventListener() {}
  click() {
    if (this.disabled) return;
    if (typeof this.onclick === 'function') this.onclick.call(this, { target: this });
    (this.listeners.click || []).forEach((fn) => fn.call(this, { target: this }));
  }
  dispatch(type, ev) {
    (this.listeners[type] || []).forEach((fn) => fn.call(this, ev || { target: this }));
  }
  focus() {}
  scrollTo() {}
  scrollIntoView() {}
  getBoundingClientRect() { return { left: 0, width: 0 }; }
  get scrollHeight() { return 0; }
  descendants() {
    const out = [];
    const walk = (e) => e.children.forEach((c) => { out.push(c); walk(c); });
    walk(this);
    return out;
  }
  querySelectorAll(sel) {
    const groups = sel.split(',').map((s) => parseSelector(s.trim()));
    return this.descendants().filter((e) => groups.some((g) => matches(e, g)));
  }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
}

// compound selectors only: tag, .class, [attr], [attr="v"]
function parseSelector(s) {
  const out = { tag: null, classes: [], attrs: [] };
  const re = /^([a-zA-Z][\w-]*)|\.([\w-]+)|\[([\w-]+)(?:="([^"]*)")?\]/g;
  let m;
  while ((m = re.exec(s))) {
    if (m[1]) out.tag = m[1].toUpperCase();
    else if (m[2]) out.classes.push(m[2]);
    else out.attrs.push([m[3], m[4]]);
  }
  return out;
}

function matches(e, g) {
  if (g.tag && e.tagName !== g.tag) return false;
  if (!g.classes.every((c) => e.classList.contains(c))) return false;
  return g.attrs.every(([k, v]) => {
    const a = e.getAttribute(k);
    return a !== null && (v === undefined || a === v);
  });
}

function makeDocument() {
  const doc = {};
  const body = new El('body', doc);
  const ids = {};
  doc.body = body;
  doc.documentElement = new El('html', doc);
  doc.title = '';
  doc.createElement = (t) => new El(t, doc);
  doc.getElementById = function (id) {
    if (!ids[id]) {
      const e = new El(/tbody$/.test(id) ? 'tbody' : 'div', doc);
      e.id = id;
      body.appendChild(e);
      ids[id] = e;
    }
    return ids[id];
  };
  doc.querySelector = (s) => body.querySelector(s);
  doc.querySelectorAll = (s) => body.querySelectorAll(s);
  doc.addEventListener = () => {};
  return doc;
}

// ---------------------------------------------------------------- runtime

function makeRuntime() {
  const doc = makeDocument();
  const rt = {
    doc,
    xhrs: [],
    fetches: [],
    alerts: [],
    prompts: [],
    confirms: [],
    timers: [],
  };
  class XHR {
    constructor() { this.status = 0; this.responseText = ''; rt.xhrs.push(this); }
    open(method, url) { this.method = method; this.url = url; }
    setRequestHeader() {}
    send(body) { this.body = body === undefined ? null : body; this.sent = true; }
    respond(status, text) {
      this.status = status; this.responseText = text; this.done = true;
      if (this.onload) this.onload();
    }
    fail(kind) {
      this.done = true;
      const h = kind === 'timeout' ? this.ontimeout : this.onerror;
      if (h) h();
    }
  }
  const win = {
    document: doc,
    XMLHttpRequest: XHR,
    localStorage: { getItem: () => null, setItem: () => {} },
    location: { origin: 'http://192.168.1.1', href: '' },
    addEventListener: () => {},
    requestAnimationFrame: () => 0,
    setTimeout: (fn, ms) => { rt.timers.push({ fn, ms }); return rt.timers.length; },
    clearTimeout: () => {},
    setInterval: () => 0,
    clearInterval: () => {},
    alert: (m) => rt.alerts.push(String(m)),
    prompt: (m) => { rt.prompts.push(m); return rt.nextPrompt === undefined ? null : rt.nextPrompt; },
    confirm: (m) => { rt.confirms.push(m); return true; },
    fetch: (url, opts) => {
      rt.fetches.push({ url, opts: opts || {} });
      return Promise.resolve({ ok: true, text: () => Promise.resolve('0') });
    },
    console,
    Promise,
    Object, Array, String, JSON, Math, Set, parseInt, encodeURIComponent,
  };
  win.window = win;
  win.self = win;
  win.top = win;
  rt.ctx = vm.createContext(win);
  return rt;
}

function inlineScripts(html) {
  const out = [];
  const re = /<script(\s[^>]*)?>([\s\S]*?)<\/script>/g;
  let m;
  while ((m = re.exec(html)))
    if (!/\bsrc=/.test(m[1] || '')) out.push(m[2]);
  return out;
}

function load(rt, path, isHtml) {
  const text = fs.readFileSync(path, 'utf8');
  const codes = isHtml ? inlineScripts(text) : [text];
  if (!codes.length) throw new Error('no inline script in ' + path);
  codes.forEach((c, i) => vm.runInContext(c, rt.ctx, { filename: path + '#' + i }));
}

function check(cond, what) {
  if (!cond) throw new Error('check failed: ' + what);
}

// ---------------------------------------------------------------- about.html

const ABOUT_DATA = {
  config_name: 'config@old',
  gpios: [
    { name: 'reset', gpio: 5, parent: 'key_gpio', source: 'env', override: 'reset_key', value: 1 },
    { name: 'wps', gpio: 6, parent: 'key_gpio', source: 'fdt', override: '', value: 1 },
  ],
};

function aboutPage(source) {
  const rt = makeRuntime();
  load(rt, commonPath, false);
  load(rt, aboutPath, true);
  rt.serveAbout = (src) => {
    const pending = rt.xhrs.filter((x) => x.sent && !x.done && x.url === '/about');
    check(pending.length === 1, 'one pending GET /about, got ' + pending.length);
    pending[0].respond(200, JSON.stringify(Object.assign({}, ABOUT_DATA,
      { config_name_source: src || source })));
    rt.xhrs.filter((x) => x.sent && !x.done && x.url === '/mac_info')
      .forEach((x) => x.respond(200, '{}'));
  };
  rt.serveAbout(source);
  return rt;
}

function button(rt, scope, text) {
  const all = scope.querySelectorAll('button').filter((b) => b.textContent === text);
  check(all.length === 1, 'one "' + text + '" button, got ' + all.length);
  return all[0];
}

function lastEnvSet(rt) {
  const posts = rt.xhrs.filter((x) => x.method === 'POST' && x.url === '/env_set');
  check(posts.length === 1, 'exactly one POST /env_set, got ' + posts.length);
  return posts[0];
}

const ACTIONS = {
  // [config_name_source, how to trigger, expected body]
  config_restore: ['env', (rt) => button(rt,
    rt.doc.getElementById('info-tbody').querySelector('[data-key="config_name"]'), '恢复'),
    'config_name='],
  config_modify: ['fdt', (rt) => { rt.nextPrompt = 'config@new'; return button(rt,
    rt.doc.getElementById('info-tbody').querySelector('[data-key="config_name"]'), '修改'); },
    'config_name=config%40new'],
  reset_key_restore: ['env', (rt) => rt.doc.getElementById('gpio-tbody')
    .querySelector('[data-override="reset_key"]'), 'reset_key='],
  reset_key_modify: ['env', (rt) => { rt.nextPrompt = '7'; return rt.doc
    .getElementById('gpio-tbody').querySelector('[data-modify="reset_key"]'); },
    'reset_key=7'],
};

const OUTCOMES = {
  ok: (x) => x.respond(200, 'ok'),
  error: (x) => x.respond(200, 'error'),
  save_failed: (x) => x.respond(200, 'save_failed'),
  http500: (x) => x.respond(500, 'ok'),
  network: (x) => x.fail('error'),
  timeout: (x) => x.fail('timeout'),
};

function aboutScenario(action, outcome) {
  return () => {
    const [src, find, body] = ACTIONS[action];
    const rt = aboutPage(src);
    const btn = find(rt);
    check(btn, 'button for ' + action);
    btn.click();
    const x = lastEnvSet(rt);
    check(x.body === body, 'body ' + JSON.stringify(x.body) + ' != ' + body);
    check(!rt.alerts.length, 'no message before the reply');
    OUTCOMES[outcome](x);
    if (outcome === 'ok') {
      check(!rt.alerts.length, 'no failure message on ok: ' + rt.alerts.join('|'));
    } else {
      check(rt.alerts.length === 1, 'one failure message, got ' + rt.alerts.length);
      check(/持久化未确认/.test(rt.alerts[0]), 'says persistence not confirmed: ' + rt.alerts[0]);
      check(!/(保存|设置|修改)成功/.test(rt.alerts[0]), 'no success wording: ' + rt.alerts[0]);
      if (outcome === 'save_failed')
        check(/内存/.test(rt.alerts[0]) && /未恢复/.test(rt.alerts[0]),
          'save_failed says RAM changed and nothing restored: ' + rt.alerts[0]);
    }
    // the page reloads the (RAM) values afterwards in every case
    const reloads = rt.xhrs.filter((y) => y.url === '/about' && !y.done);
    check(reloads.length === 1, 'reload after the reply');
    // a late second event (e.g. timeout after load) must not re-report
    if (x.ontimeout) x.ontimeout();
    if (x.onerror) x.onerror();
    check(rt.alerts.length === (outcome === 'ok' ? 0 : 1), 'reported once');
  };
}

// ---------------------------------------------------------------- term.html

// Model of the U-Boot hush list runner (common/cli_hush.c run_list_real,
// plain lists): after a pipe with "&&" fails or a pipe with "||" succeeds,
// every following pipe is skipped up to and including the next one that
// ends with ";" (or the end).  Words split on blanks, double quotes group.
function hushRun(cmd, env) {
  const pipes = [];
  let words = [], cur = null, i = 0;
  const word = () => { if (cur !== null) { words.push(cur); cur = null; } };
  const end = (op) => { word(); pipes.push({ words, followup: op }); words = []; };
  while (i < cmd.length) {
    const c = cmd[i];
    if (c === '"') {
      const j = cmd.indexOf('"', i + 1);
      cur = (cur || '') + cmd.slice(i + 1, j);
      i = j + 1;
    } else if (cmd.startsWith('&&', i) || cmd.startsWith('||', i)) {
      end(cmd.slice(i, i + 2)); i += 2;
    } else if (c === ';') {
      end(';'); i++;
    } else if (c === ' ' || c === '\t') {
      word(); i++;
    } else {
      cur = (cur || '') + c; i++;
    }
  }
  end(';');
  let rcode = 0, skipping = false;
  for (const pi of pipes) {
    if (skipping) {
      if (pi.followup === ';') skipping = false;
      continue;
    }
    if (!pi.words.length) continue;
    rcode = env.exec(pi.words);
    if ((rcode === 0 && pi.followup === '||') || (rcode !== 0 && pi.followup === '&&'))
      skipping = true;
  }
  return rcode;
}

function shell(opts) {
  const env = { saves: 0, ran: [] };
  env.exec = (w) => {
    env.ran.push(w.join(' '));
    switch (w[0]) {
      case 'setenv': return opts.setenvFail ? 1 : 0;
      case 'env': return opts.defaultFail ? 1 : 0;
      case 'saveenv': env.saves++; return opts.saveFail ? 1 : 0;
      default: return 1;
    }
  };
  return env;
}

function termPage() {
  const rt = makeRuntime();
  load(rt, commonPath, false);
  load(rt, termPath, true);
  rt.cmds = () => rt.fetches.filter((f) => f.url === '/webterm/cmd');
  return rt;
}

const TERM_ACTIONS = {
  modify: ['updateEnv', 'foo', 'bar baz', 'setenv'],
  delete: ['deleteEnv', 'foo', undefined, 'setenv'],
  reset_one: ['resetEnv', 'foo', undefined, 'env'],
  reset_all: ['resetEnv', '', undefined, 'env'],
};

function termScenario(action, failure) {
  return () => {
    const [id, input, promptValue, first] = TERM_ACTIONS[action];
    const rt = termPage();
    const status = rt.doc.getElementById('connectionStatus');
    const statusBefore = [status.textContent, status.style.color];
    rt.doc.getElementById('terminalInput').value = input;
    rt.nextPrompt = promptValue;
    rt.doc.getElementById(id).click();
    const cmds = rt.cmds();
    check(cmds.length === 1, 'one /webterm/cmd POST, got ' + cmds.length);
    const c = cmds[0];
    // unchanged transport: queued plain text POST, then fast status poll
    check(c.opts.method === 'POST' && c.opts.headers['Content-Type'] === 'text/plain',
      'POST text/plain');
    check(rt.timers.some((t) => t.ms === 100), 'fast poll scheduled');
    check(status.textContent === statusBefore[0] && status.style.color === statusBefore[1],
      'connection mark not touched by the command');
    const body = c.opts.body;
    check(body.split(/\s+/)[0] === first, 'starts with ' + first + ': ' + body);
    if (action === 'modify') check(body.indexOf('setenv foo bar baz') === 0, body);
    if (action === 'delete') check(/^setenv foo\b/.test(body) && !/setenv foo \S+ /.test(body.split('&&')[0]), body);
    if (action === 'reset_one') check(body.indexOf('env default -f foo') === 0, body);
    if (action === 'reset_all') check(body.indexOf('env default -a') === 0, body);

    const env = shell({
      setenvFail: failure === 'first', defaultFail: failure === 'first',
      saveFail: failure === 'save',
    });
    const rc = hushRun(body, env);
    // never an unconditional save
    check(!/;\s*saveenv/.test(body), 'no "; saveenv": ' + body);
    if (failure === 'none') {
      check(env.saves === 1 && rc === 0, 'saved once, rc 0');
    } else if (failure === 'first') {
      check(env.saves === 0, 'no saveenv after a failed ' + first + ': ' + env.ran.join(' / '));
      check(rc !== 0, 'list fails');
    } else {
      check(env.saves === 1 && rc !== 0, 'save attempted once, list fails');
    }
  };
}

// query is unchanged and does not save
function termQuery() {
  const rt = termPage();
  rt.doc.getElementById('terminalInput').value = 'foo';
  rt.doc.getElementById('queryEnv').click();
  const cmds = rt.cmds();
  check(cmds.length === 1 && cmds[0].opts.body === 'printenv foo', 'printenv foo');
}

// ---------------------------------------------------------------- main

// Export the commands posted by actual button handlers for real hush tests.
if (which === '--commands') {
  const commands = {};
  for (const [action, [id, input, promptValue]] of Object.entries(TERM_ACTIONS)) {
    const rt = termPage();
    rt.doc.getElementById('terminalInput').value = input;
    rt.nextPrompt = promptValue;
    rt.doc.getElementById(id).click();
    check(rt.cmds().length === 1, 'one command for ' + action);
    commands[action] = rt.cmds()[0].opts.body;
  }
  process.stdout.write(JSON.stringify(commands));
  process.exit(0);
}

const scenarios = {};
for (const a of Object.keys(ACTIONS))
  for (const o of Object.keys(OUTCOMES))
    scenarios['about_' + a + '_' + o] = aboutScenario(a, o);
for (const a of Object.keys(TERM_ACTIONS))
  for (const f of ['none', 'first', 'save'])
    scenarios['term_' + a + '_' + f] = termScenario(a, f);
scenarios.term_query_unchanged = termQuery;

for (const [name, fn] of Object.entries(scenarios)) {
  if (which === '--list') { process.stderr.write(name + '\n'); continue; }
  if (which && which !== name) continue;
  let ok = true;
  try { fn(); } catch (e) { ok = false; process.stdout.write(name + ': ' + e.message + '\n'); }
  process.stderr.write((ok ? 'PASS ' : 'FAIL ') + name + '\n');
}
