// Executes the FilesPage script in a stubbed DOM and drives the upload-settings
// call graph so every missing symbol surfaces as a ReferenceError here, not on
// the user's browser.
const fs = require('fs');
const vm = require('vm');

const script = fs.readFileSync(process.argv[2] || '/tmp/files_scripts.js', 'utf8');

function makeElement(id) {
  const store = { id, style: {}, dataset: {}, classList: {
    add(){}, remove(){}, toggle(){}, contains(){ return false; },
  } };
  const children = [];
  const el = new Proxy(store, {
    get(t, p) {
      if (p in t) return t[p];
      if (p === 'checked' || p === 'value' || p === 'textContent' || p === 'innerHTML') return undefined;
      if (p === 'children' || p === 'childNodes') return children;
      if (p === 'firstChild' || p === 'lastChild') return null;
      if (p === 'parentNode' || p === 'parentElement' || p === 'ownerDocument') return null;
      if (p === 'files') return [];
      if (p === Symbol.toPrimitive) return () => `[el ${id}]`;
      return t[p] = (typeof p === 'string' && /addEventListener|removeEventListener|appendChild|removeChild|insertBefore|querySelector|querySelectorAll|setAttribute|getAttribute|removeAttribute|focus|blur|click|remove|reset|submit|closest|contains|getBoundingClientRect|scrollIntoView|requestSubmit|toggleAttribute/.test(p))
        ? function () { return undefined; }
        : makeElement(id + '.' + String(p));
    },
    set(t, p, v) { t[p] = v; return true; },
  });
  return el;
}

const elements = new Map();
const byId = (id) => { if (!elements.has(id)) elements.set(id, makeElement('#' + id)); return elements.get(id); };

const documentStub = {
  getElementById: byId,
  querySelector: (sel) => byId('qs:' + sel),
  querySelectorAll: () => [],
  createElement: (tag) => byId('new:' + tag),
  createTextNode: () => ({}),
  addEventListener() {}, removeEventListener() {},
  body: byId('body'), documentElement: byId('html'),
  activeElement: null, readyState: 'complete', visibilityState: 'visible',
  title: '', forms: [], images: [],
};

const sandbox = {
  console, document: documentStub,
  navigator: { userAgent: 'harness', clipboard: { writeText: async () => {} }, onLine: true },
  location: { href: 'http://x/', protocol: 'http:', host: 'x', pathname: '/', search: '', hash: '', reload() {} },
  history: { pushState() {}, replaceState() {} },
  localStorage: (() => { let s = {}; return { getItem: (k) => (k in s ? s[k] : null), setItem: (k, v) => { s[k] = String(v); }, removeItem: (k) => { delete s[k]; }, clear: () => { s = {}; } }; })(),
  sessionStorage: (() => { let s = {}; return { getItem: (k) => (k in s ? s[k] : null), setItem: (k, v) => { s[k] = String(v); }, removeItem: (k) => { delete s[k]; }, clear: () => { s = {}; } }; })(),
  setTimeout: () => 0, clearTimeout() {}, setInterval: () => 0, clearInterval() {},
  requestAnimationFrame: () => 0, fetch: async () => ({ ok: false, status: 404, json: async () => ({}) }),
  WebSocket: function () { this.close = () => {}; },
  alert() {}, confirm() { return false; }, prompt() { return null; },
  Image: function () { return makeElement('img'); },
  FileReader: function () {},
  FormData: function () { this.append = () => {}; },
  Blob: function () {}, URL: { createObjectURL: () => '', revokeObjectURL() {} },
  XMLHttpRequest: function () { this.open = () => {}; this.send = () => {}; this.setRequestHeader = () => {}; },
  AbortController: class { constructor() { this.signal = {}; } abort() {} },
  performance: { now: () => 0 }, crypto: { getRandomValues: () => {} },
  addEventListener() {}, removeEventListener() {}, dispatchEvent() {},
  CustomEvent: function (t) { this.type = t; }, Event: function (t) { this.type = t; },
  atob: (s) => Buffer.from(s, 'base64').toString('binary'),
  btoa: (s) => Buffer.from(s, 'binary').toString('base64'),
  structuredClone: (v) => JSON.parse(JSON.stringify(v)),
  matchMedia: () => ({ matches: false, addEventListener() {}, removeEventListener() {} }),
  getComputedStyle: () => ({ getPropertyValue: () => '' }),
  innerWidth: 1280, innerHeight: 800, devicePixelRatio: 1,
  URLSearchParams,
  TextEncoder, TextDecoder,
  Map, Set, WeakMap, WeakSet, Promise, Symbol, Reflect, Proxy,
  Uint8Array, Int8Array, Uint8ClampedArray, Uint16Array, Int16Array, Uint32Array, Int32Array,
  Float32Array, Float64Array, BigInt64Array, BigUint64Array, ArrayBuffer, SharedArrayBuffer, DataView,
  Date, RegExp, Error, TypeError, RangeError, ReferenceError, SyntaxError, URIError, EvalError,
  Number, String, Boolean, BigInt, Math, JSON, Object, Array, Function,
  isNaN, isFinite, parseInt, parseFloat, encodeURIComponent, decodeURIComponent, encodeURI, decodeURI,
  queueMicrotask,
  window: null,
};
sandbox.window = sandbox;
sandbox.self = sandbox;
sandbox.globalThis = sandbox;

const ctx = vm.createContext(sandbox);
const errors = [];

function run(name, fn) {
  try { vm.runInContext(fn, ctx, { filename: name }); }
  catch (e) {
    errors.push(`${name}: ${e.constructor.name}: ${e.message}`);
    if (e instanceof ReferenceError) return; // keep going to surface more
    throw e; // unexpected non-reference errors stop the chain
  }
}

run('script-top-level', script);
// Drive the upload-settings call graph (mirrors the user's click path).
const calls = [
  'typeof openUploadModal === "function" && openUploadModal();',
  'typeof restoreUploadSettingsFromStorage === "function" && restoreUploadSettingsFromStorage();',
  'typeof applyUploadSettings === "function" && applyUploadSettings({convertBeforeUpload:true,renameFromMetadata:true,autoCrop:true,quality:85,deviceTarget:"auto",handedness:"right",overlap:10,rememberSettings:true});',
  'typeof setQualityPreset === "function" && setQualityPreset(85);',
  'typeof updateQualitySettings === "function" && updateQualitySettings();',
  'typeof setDeviceTarget === "function" && setDeviceTarget("auto");',
  'typeof applyDeviceTarget === "function" && applyDeviceTarget();',
  'typeof setHandedness === "function" && setHandedness("right");',
  'typeof setOverlap === "function" && setOverlap(10);',
  'typeof toggleConvertOptions === "function" && toggleConvertOptions();',
  'typeof toggleAdvancedOptions === "function" && toggleAdvancedOptions();',
  'typeof updateUploadSettingsPersistence === "function" && updateUploadSettingsPersistence();',
  'typeof getCurrentUploadSettings === "function" && getCurrentUploadSettings();',
  'typeof toggleConvertOptions === "function" && toggleConvertOptions();',
  'typeof validateFile === "function" && validateFile();',
  'typeof updateRenamePreview === "function" && updateRenamePreview();',
  'typeof fetchVersion === "function" && fetchVersion();',
];
for (let i = 0; i < calls.length; i++) run(`call#${i}`, calls[i]);

if (errors.length) {
  console.log('FAILURES:');
  errors.forEach((e) => console.log(' - ' + e));
  process.exit(1);
} else {
  console.log('HARNESS CLEAN: no ReferenceError in the upload-settings call graph');
}
