'use strict';

const $ = (id) => document.getElementById(id);
const range = (count) => Array.from({ length: count }, (_, index) => index);
const bbx = (index) => 'BBX' + String(index).padStart(2, '0');
const emptyRoutes = () => range(8).map(() => 0);
const gpioMap = [18, 8, 6, 5, 19, 4, 3, 1];
const headerPins = [10, 9, 8, 7, 12, 11, 18, 17, 16, 15, 14, 13, 6, 5, 20, 19];
const captureLetters = ['A', 'B', 'C', 'D'];
const captureFields = ['a', 'b', 'c', 'd'];
const captureColors = ['#a7e7c5', '#f2bf71', '#9bcff7', '#f2a9c3'];
const captureRoles = { spi: ['CS', 'SCK', 'MOSI', 'MISO'], i2c: ['SCL', 'SDA'] };
const spiSlaveSignals = ['cs', 'sck', 'mosi', 'miso'];
const spiSlaveBenchNames = { rx: 'MOSI 仅接收', duplex: 'MOSI / MISO 全双工', gap: '连续两帧的 CS 间隔' };
const receiveModes = {
  i2c: { name: 'I²C 从机', op: 'i2c_slave', signals: ['sda', 'scl'], rateLabel: '目标 SCL（Hz）', rate: 100000, min: 10000, max: 400000 },
  uart: { name: 'UART 接收', op: 'uart_receive', signals: ['rx'], rateLabel: '目标波特率', rate: 115200, min: 9600, max: 5000000 },
};
const maxI2cRegisters = 8;
const modeNames = {
  input: '高阻输入', pullup: '上拉输入', pulldown: '下拉输入',
  low: '低电平输出', high: '高电平输出', pwm: 'PWM',
};
const driveModes = new Set(['low', 'high', 'pwm']);
const operationNames = {
  uart: 'UART 收发', i2c_scan: 'I²C 扫描', i2c_read: 'I²C 寄存器读',
  spi: 'SPI 交换', proxy: 'UART 单轮代理', uart_loopback: '板内 UART 回环',
};

function readStored(storage, name, fallback) {
  try {
    const value = storage.getItem('ai-board.' + name);
    return value === null ? fallback : JSON.parse(value);
  } catch {
    return fallback;
  }
}

function store(storage, name, value) {
  try {
    storage.setItem('ai-board.' + name, JSON.stringify(value));
  } catch {
    notice('浏览器空间不足，现有操作可以继续。', 'warning');
  }
}

let key = readStored(sessionStorage, 'key', '');
let live = { armed: false, routes: emptyRoutes(), levels: range(8).map(() => null),
  modes: range(8).map(() => 'input'), gpios: gpioMap };
let draft = emptyRoutes();
let hasState = false;
let selectedY = 0;
let selectedX = 0;
let lastCapture = null;
let lastTest = null;
let working = false;
let noticeTimer;
let aiControlActive = false;
let savedDraft = null;
let aiRevision = 0;
let monitoringAi = false;
let luaScripts = null;
let savedLua = null;
let luaCodeEditor = null;
let luaEditorLoad;
let luaErrorLine = null;
const luaScriptName = /^[A-Za-z0-9][A-Za-z0-9._-]{0,43}\.lua$/;
const luaSourceLimit = 8192;
const luaEncoder = new TextEncoder();
let aliases = readStored(localStorage, 'aliases', {});
let operationHistory = readStored(localStorage, 'history', []);
let presets = readStored(localStorage, 'presets', []);
const dirty = () => draft.some((mask, index) => mask !== live.routes[index]);
const aiReadOnlyMessage = 'AI 正在控制板卡，网页为只读模式。';

function node(tagName, className, text) {
  const element = document.createElement(tagName);
  if (className) element.className = className;
  if (text !== undefined) element.textContent = text;
  return element;
}

function dismissNotice() {
  clearTimeout(noticeTimer);
  $('notice').hidden = true;
  $('notice-live').textContent = '';
}

function notice(text, kind = '') {
  clearTimeout(noticeTimer);
  const toast = $('notice');
  toast.dataset.kind = kind;
  $('notice-message').textContent = text;
  $('notice-live').textContent = text;
  toast.hidden = false;
  if (kind !== 'pending') {
    noticeTimer = setTimeout(dismissNotice, kind === 'warning' || kind === 'error' ? 8000 : 4000);
  }
}

function confirmAction(title, message) {
  const dialog = $('confirm-dialog');
  if (dialog.open || $('key-dialog').open) return Promise.resolve(false);
  $('confirm-title').textContent = title;
  $('confirm-message').textContent = message;
  dialog.returnValue = '';
  return new Promise((resolve) => {
    dialog.addEventListener('close', () => resolve(dialog.returnValue === 'confirm'), { once: true });
    dialog.showModal();
  });
}

function requireKey() {
  if (!key) {
    if (!$('key-dialog').open) $('key-dialog').showModal();
    throw new Error('请输入板子的设备访问密钥。');
  }
}

async function rpc(path, { method = 'GET', body, headers = {}, authenticate = true, raw = false } = {}) {
  if (method !== 'GET' && aiControlActive) throw new Error(aiReadOnlyMessage);
  if (authenticate) requireKey();
  const options = { method, headers: { ...headers }, cache: 'no-store', signal: AbortSignal.timeout(path === '/api/ota' ? 65000 : 12000) };
  if (authenticate) options.headers['X-Board-Key'] = key;
  if (body !== undefined) options.body = body;
  if (body instanceof URLSearchParams) options.headers['Content-Type'] = 'application/x-www-form-urlencoded';
  const response = await fetch(path, options);
  const text = await response.text();
  if (response.status === 401) throw new Error('设备访问密钥不正确。');
  if (response.status === 423 && method !== 'GET') {
    setAiControl(true);
    throw new Error(aiReadOnlyMessage);
  }
  if (response.status === 412) {
    throw new Error('板上的同名脚本已变化。当前草稿已保留，请另存为新名称，或重新打开板上的版本。');
  }
  if (!response.ok) throw new Error(text || ('HTTP ' + response.status));
  if (raw) return { text, etag: response.headers.get('ETag') };
  return text ? JSON.parse(text) : null;
}

function workbench(params) {
  return rpc('/api/workbench', { method: 'POST', body: new URLSearchParams(params) });
}

function record(label, result, failed = false) {
  operationHistory.unshift({ label, result, failed, time: new Date().toISOString() });
  operationHistory = operationHistory.slice(0, 36);
  store(localStorage, 'history', operationHistory);
  renderHistory();
}

async function action(label, perform, successText = label + '完成', log = true) {
  if (working) return;
  working = true;
  syncLuaEditing();
  notice(label + '…', 'pending');
  try {
    const result = await perform();
    if (log) record(label, result);
    notice(successText, 'success');
    return result;
  } catch (error) {
    notice(error.message, 'error');
    if (log) record(label, { error: error.message }, true);
  } finally {
    working = false;
    syncBoardEditing();
  }
}

function connection(state, label) {
  $('connection').dataset.state = state;
  $('connection-label').textContent = label;
}

function statusDisplay(status) {
  $('board-ip').textContent = status.station_ip !== '0.0.0.0' ? status.station_ip : location.hostname;
  $('board-slot').textContent = status.partition || '—';
  $('board-arm').textContent = status.matrix_armed ? '已启用' : '待启用';
  connection('online', key ? '设备在线' : '设备在线 · 待授权');
}

async function readStatus() {
  let status;
  try {
    status = await rpc('/api/status', { authenticate: false });
  } catch (error) {
    connection('offline', '设备离线');
    throw error;
  }
  statusDisplay(status);
  return status;
}

function acceptState(state, preserveDraft = false) {
  if (!state || !Array.isArray(state.routes) || state.routes.length !== 8) throw new Error('设备返回了无效的矩阵状态。');
  const keepDraft = preserveDraft && !aiControlActive && dirty();
  live = state;
  hasState = true;
  if (!keepDraft) draft = live.routes.slice();
  $('board-arm').textContent = live.armed ? '已启用' : '待启用';
  renderMatrix();
}

async function readState(preserveDraft = true, changedOnly = false) {
  const state = await rpc('/api/workbench');
  if (!changedOnly || !hasState || JSON.stringify(state) !== JSON.stringify(live)) acceptState(state, preserveDraft);
  return state;
}

function syncBoardEditing() {
  const selector = 'button, input, select, textarea';
  for (const target of document.querySelectorAll('[data-board-edit]')) {
    const controls = target.matches(selector) ? [target] : target.querySelectorAll(selector);
    for (const control of controls) {
      if (control.hasAttribute('data-board-view')) continue;
      if (aiControlActive && !control.disabled) {
        control.disabled = true;
        control.dataset.aiDisabled = 'yes';
      } else if (!aiControlActive && control.dataset.aiDisabled) {
        control.disabled = false;
        delete control.dataset.aiDisabled;
      }
    }
  }
  syncLuaEditing();
}

function setAiControl(active) {
  if (active === aiControlActive) return;
  aiControlActive = active;
  if (active) {
    savedDraft = dirty() ? draft.slice() : null;
    draft = live.routes.slice();
  } else if (savedDraft) {
    draft = savedDraft;
    savedDraft = null;
  }
  document.body.dataset.aiControl = active ? 'active' : 'idle';
  $('ai-control-notice').hidden = !active;
  if (!active) {
    syncBoardEditing();
    updateCaptureEngine();
    updateCaptureDecode();
  }
  renderMatrix();
}

function renderAiResult({ revision, op, result }) {
  aiRevision = revision;
  if (!op || op === 'lua' || !result) return;
  try {
    if (result.operation === 'i2c_slave' || result.operation === 'uart_receive') {
      const mode = result.operation === 'i2c_slave' ? 'i2c' : 'uart';
      renderProtocolReceive(mode, result);
    } else if (result.operation === 'spi_slave' || result.operation === 'spi_slave_bench') {
      renderSpiSlaveResult(result);
    } else if (Array.isArray(result.channels)) {
      const capture = normalizeCapture(result);
      if (op.startsWith('i2c_')) $('capture-protocol').value = 'i2c';
      if (op.startsWith('spi_')) $('capture-protocol').value = 'spi';
      lastCapture = null;
      updateCaptureDecode();
      renderCapture(capture);
    } else {
      showOutput('protocol-result', result);
    }
  } catch {
    // Leave the current view intact if the published result cannot be rendered.
  }
}

async function refreshAiMonitor(forceState = false) {
  if (monitoringAi) return;
  monitoringAi = true;
  try {
    const wasActive = aiControlActive;
    const status = await rpc('/api/control', { authenticate: false });
    setAiControl(status.active === true);
    if (!key) return;
    if (forceState || aiControlActive || wasActive) await readState(true, true);
    if (status.revision !== aiRevision) {
      renderAiResult(await rpc('/api/workbench/result'));
    }
  } catch (error) {
    // The board may reboot while the viewer is open and restart revision numbering.
    aiRevision = null;
    throw error;
  } finally {
    monitoringAi = false;
  }
}

const luaEditorCdn = 'https://cdn.jsdelivr.net/npm/ace-builds@1.44.0/src-min-noconflict/';

function loadLuaEditorScript(file, integrity) {
  return new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = luaEditorCdn + file;
    script.integrity = integrity;
    script.crossOrigin = 'anonymous';
    script.referrerPolicy = 'no-referrer';
    script.addEventListener('load', resolve, { once: true });
    script.addEventListener('error', reject, { once: true });
    document.head.append(script);
  });
}

function mountLuaCodeEditor() {
  if (luaCodeEditor || $('lua').hidden) return;
  const ace = window.ace;
  if (!ace?.require('ace/mode/lua') || !ace.require('ace/ext/searchbox')) return;
  const host = $('lua-code-editor'), source = $('lua-source');
  let editor;
  try {
    host.hidden = false;
    editor = ace.edit(host);
    editor.setOptions({
      showPrintMargin: false, showLineNumbers: true, showFoldWidgets: true,
      highlightActiveLine: true, tabSize: 2, useSoftTabs: true, wrap: true,
      fontSize: 13, textInputAriaLabel: 'Lua 源码', placeholder: source.placeholder,
    });
    editor.session.setUseWorker(false);
    editor.session.setMode('ace/mode/lua');
    editor.session.setValue(source.value);
    const input = editor.textInput.getElement();
    input.id = 'lua-code-input';
    input.setAttribute('aria-describedby', 'lua-size');
    editor.on('change', () => {
      const value = editor.getValue();
      if (value === source.value) return;
      source.value = value;
      setLuaErrorLocation(null);
      syncLuaEditing();
    });
    luaCodeEditor = editor;
    syncLuaEditing();
    $('lua-source-label').htmlFor = input.id;
    source.hidden = true;
    $('lua-find').hidden = false;
  } catch {
    luaCodeEditor = null;
    editor?.destroy();
    host.replaceChildren();
    host.hidden = true;
  }
}

function loadLuaCodeEditor() {
  if (luaCodeEditor) {
    luaCodeEditor.resize();
    return;
  }
  luaEditorLoad ??= loadLuaEditorScript('ace.js', 'sha384-L35+Z0msDQr3oTrDusYCefF5a2MY3q7nK5sOTBFKQvjoZi15zWLUhzXntENo8d/5')
    .then(() => Promise.all([
      loadLuaEditorScript('mode-lua.js', 'sha384-uMeNZmXYC/i30xNCqBSA666a6ZJamdL8yPPUdD+mLdEojGmf4dHVqCtr05gbuXgF'),
      loadLuaEditorScript('ext-searchbox.js', 'sha384-LRJtdX7s/2zXGXuVjTTV2HRBTxhSnH1RSz7octXy7QHaXSlmCvRe2esC66Ox4l8o'),
    ]));
  luaEditorLoad.then(() => {
    const source = $('lua-source');
    // Keep the original input active while the user types or composes text.
    if (document.activeElement === source) source.addEventListener('blur', mountLuaCodeEditor, { once: true });
    else mountLuaCodeEditor();
  }).catch(() => {
    // The board's standalone AP can continue using the plain source field.
  });
}

function luaDraftChanged() {
  return $('lua-name').value !== (savedLua?.name ?? '') ||
    $('lua-source').value !== (savedLua?.source ?? '');
}

function syncLuaEditing() {
  const name = $('lua-name');
  const source = $('lua-source');
  const size = luaEncoder.encode(source.value).length;
  const draftChanged = luaDraftChanged();
  const locked = aiControlActive || working;
  name.readOnly = source.readOnly = $('lua-timeout').readOnly = locked;
  if (luaCodeEditor && luaCodeEditor.getReadOnly() !== locked) {
    luaCodeEditor.setReadOnly(locked);
    $('lua-code-editor').dataset.locked = String(locked);
  }
  $('lua-refresh').disabled = working;
  $('lua-new').disabled = locked;
  $('lua-save').disabled = locked || size === 0 || size > luaSourceLimit || !luaScriptName.test(name.value) || !draftChanged;
  $('lua-run').disabled = locked || size === 0 || size > luaSourceLimit;
  $('lua-size').textContent = size + ' / ' + luaSourceLimit + ' 字节';
  $('lua-size').dataset.state = size > luaSourceLimit ? 'limit' : '';
  const status = $('lua-editor-status');
  const message = aiControlActive ? (draftChanged ? 'AI 接管 · 草稿已保留' : 'AI 接管 · 只读')
    : draftChanged ? '有未保存的更改' : savedLua ? '已保存到板卡' : '新建脚本';
  if (status.textContent !== message) status.textContent = message;
  status.dataset.state = draftChanged ? 'draft' : '';
}

function syncLuaFileSelection() {
  for (const file of $('lua-file-list').querySelectorAll('.lua-file')) {
    if (file.dataset.name === savedLua?.name) file.setAttribute('aria-current', 'true');
    else file.removeAttribute('aria-current');
  }
}

function renderLuaLibrary() {
  const rows = (luaScripts ?? []).map(({ name, bytes }) => {
    const file = node('button', 'lua-file');
    file.type = 'button';
    file.dataset.name = name;
    file.title = name + ' · ' + bytes + ' 字节';
    file.append(node('span', 'lua-file-name', name));
    file.addEventListener('click', () => openLuaScript(name));
    const row = node('li');
    row.append(file);
    return row;
  });
  $('lua-file-list').replaceChildren(...rows);
  $('lua-library-empty').hidden = luaScripts === null || rows.length > 0;
  $('lua-library-status').textContent = luaScripts ? luaScripts.length + ' 份脚本' : '等待读取';
  syncLuaFileSelection();
  syncLuaEditing();
}

function readLuaLibrary() {
  return action('读取 Lua 脚本列表', async () => {
    const requestKey = key;
    const result = await rpc('/api/scripts');
    if (key !== requestKey) return;
    luaScripts = result.scripts.sort((a, b) => a.name.localeCompare(b.name));
    renderLuaLibrary();
  }, '已更新 Lua 脚本列表。', false);
}

async function openLuaScript(name) {
  if (working || (luaDraftChanged() && !await confirmAction('打开板上脚本', '放弃当前草稿并打开板上的脚本？'))) return;
  await action('打开 ' + name, async () => {
    const requestKey = key;
    const { text, etag } = await rpc('/api/scripts/source?' + new URLSearchParams({ name }), { raw: true });
    if (key !== requestKey) return;
    if (!etag) throw new Error('板卡未返回脚本版本，请刷新后重新打开。');
    setLuaDocument(name, text, etag);
  }, '已打开 ' + name + '。', false);
}

function setLuaDocument(name, source, etag = null) {
  $('lua-name').value = name;
  const field = $('lua-source');
  field.value = source;
  savedLua = etag ? { name, source: field.value, etag } : null;
  setLuaErrorLocation(null);
  if (luaCodeEditor) {
    luaCodeEditor.session.setValue(field.value);
    luaCodeEditor.gotoLine(1, 0);
  }
  syncLuaFileSelection();
  syncLuaEditing();
}

function luaSource() {
  const source = $('lua-source').value;
  const bytes = luaEncoder.encode(source).length;
  if (!bytes || bytes > luaSourceLimit || source.includes('\0')) {
    throw new Error('Lua 源码须为 1–8192 字节的文本。');
  }
  return source;
}

function sendLua(params, source, headers = {}) {
  return rpc('/api/workbench/lua?' + new URLSearchParams(params), {
    method: 'POST', body: source, headers: { 'Content-Type': 'text/plain; charset=utf-8', ...headers },
  });
}

function setLuaErrorLocation(line) {
  luaErrorLine = line;
  const jump = $('lua-error-jump');
  jump.hidden = line === null;
  jump.textContent = line === null ? '' : '定位到第 ' + line + ' 行';
}

function renderLuaResult(result) {
  const state = $('lua-result-state');
  state.textContent = result
    ? (result.ok ? '执行完成' : '执行失败') + ' · ' + result.elapsed_ms + ' ms'
    : '等待运行';
  state.dataset.state = result && !result.ok ? 'error' : '';
  $('lua-copy-result').disabled = !result;
  $('lua-result-content').hidden = !result;
  $('lua-raw-result').hidden = !result;
  $('lua-return-panel').hidden = !result?.ok;
  $('lua-error-panel').hidden = !result || result.ok;
  $('lua-return').textContent = result?.ok ? JSON.stringify(result.result, null, 2) : '';
  $('lua-error').textContent = result && !result.ok ? result.error : '';
  const error = /^\[string "ai-board"\]:(\d+):/.exec($('lua-error').textContent);
  const line = error ? Number(error[1]) : 0;
  setLuaErrorLocation(line > 0 && line <= $('lua-source').value.split('\n').length ? line : null);
  $('lua-log').textContent = result ? result.log || '没有打印日志。' : '';
  $('lua-result').textContent = result ? JSON.stringify(result, null, 2) : '';
}

function clearLuaSession() {
  luaScripts = null;
  renderLuaLibrary();
  setLuaDocument('', '');
  renderLuaResult(null);
}

function selectedChannels() {
  return range(8).filter((index) => draft[index] !== 0);
}

function portsIn(mask) {
  return range(16).filter((index) => mask & (1 << index));
}

function signalGroups(routes) {
  const groups = [];
  const seen = new Set();
  for (const first of routes.keys()) {
    if (seen.has(first)) continue;
    const channels = [first];
    seen.add(first);
    let ports = 0;
    for (let index = 0; index < channels.length; index++) {
      const connected = routes[channels[index]];
      ports |= connected;
      for (const [y, mask] of routes.entries()) {
        if (!(connected & mask) || seen.has(y)) continue;
        channels.push(y);
        seen.add(y);
      }
    }
    groups.push({ channels: channels.sort((a, b) => a - b), ports });
  }
  return groups;
}

function renderMatrix() {
  const grid = $('route-grid');
  const header = grid.tHead;
  const rows = grid.tBodies[0];
  header.replaceChildren();
  rows.replaceChildren();
  const headRow = header.insertRow();
  headRow.append(node('th', '', 'Y / X'));
  for (const x of range(16)) {
    const cell = node('th', x === selectedX ? 'selected' : '');
    cell.scope = 'col';
    cell.title = bbx(x) + ' · H1.' + headerPins[x];
    cell.append(node('span', 'port-head', bbx(x)));
    cell.append(node('span', 'port-alias', aliases[x] || 'H1.' + headerPins[x]));
    headRow.append(cell);
  }
  for (const y of range(8)) {
    const row = rows.insertRow();
    if (y === selectedY) row.className = 'selector-row';
    const label = node('th');
    label.scope = 'row';
    const rowButton = node('button', 'y-label' + (y === selectedY ? ' selected' : ''));
    rowButton.type = 'button';
    rowButton.dataset.yLabel = y;
    rowButton.append(node('span', '', 'Y' + y), node('small', '', 'IO' + live.gpios[y]));
    label.append(rowButton);
    row.append(label);
    for (const x of range(16)) {
      const cell = row.insertCell();
      const mask = 1 << x;
      const on = Boolean(draft[y] & mask);
      const changed = Boolean((draft[y] ^ live.routes[y]) & mask);
      const button = node('button', 'crosspoint' + (on ? ' on' : '') + (changed ? ' changed' : ''));
      button.type = 'button';
      button.setAttribute('data-board-edit', '');
      button.dataset.x = x;
      button.dataset.y = y;
      button.tabIndex = x === selectedX && y === selectedY ? 0 : -1;
      button.setAttribute('aria-label', bbx(x) + ' 到 Y' + y + (changed ? '，有待应用的更改' : ''));
      button.setAttribute('aria-pressed', String(on));
      cell.append(button);
    }
  }
  const count = draft.reduce((total, mask) => total + portsIn(mask).length, 0);
  $('crosspoint-count').textContent = count + ' 处连接';
  $('draft-label').textContent = hasState && aiControlActive ? '板卡实时接线'
    : dirty() ? '待应用 · ' + selectedChannels().length + ' 条通道'
    : (hasState ? '已读回矩阵' : '尚未读回设备');
  $('channel-picker').replaceChildren(...range(8).map((y) => {
    const button = node('button', '', 'Y' + y);
    button.type = 'button';
    button.dataset.channel = y;
    button.setAttribute('aria-pressed', String(y === selectedY));
    return button;
  }));
  $('selected-pin').textContent = 'Y' + selectedY;
  $('selected-gpio').textContent = 'GPIO' + live.gpios[selectedY];
  $('selected-level').textContent = '电平 ' + (live.levels[selectedY] ?? '—');
  $('gpio-form').elements.mode.value = live.modes[selectedY] || 'input';
  updatePwmFields();
  const outNames = { '5v': '5 V 供电', gnd: 'GND 放电' };
  $('out-card').hidden = !live.out_mode;
  $('out-state').textContent = outNames[live.out_mode] || '待读回';
  if (live.out_mode) $('out-form').elements.mode.value = live.out_mode;
  showAlias();
  renderWires();
  updateSpiSlaveRoutes();
  updateReceiveRoutes();
  syncBoardEditing();
}

function renderWires() {
  const pending = dirty();
  $('wire-list').replaceChildren(...signalGroups(draft).map(({ channels, ports }) => {
    const row = node('div', 'wire-row');
    const meta = node('div', 'wire-meta');
    const group = node('div', 'wire-nodes');
    for (const x of portsIn(ports)) group.append(node('span', 'wire-node', bbx(x) + (aliases[x] ? ' ' + aliases[x] : '')));
    if (!ports) group.append(node('span', 'wire-label', '未连接'));
    const modes = channels.map((y) => modeNames[live.modes[y]] || '高阻输入');
    const hint = pending ? '应用后为高阻输入'
      : new Set(modes).size === 1 ? modes[0]
      : modes.map((mode, index) => 'Y' + channels[index] + ' ' + mode).join(' · ');
    const modeLabel = node('div', 'wire-label', hint);
    modeLabel.title = hint;
    meta.append(group, modeLabel);
    const pins = node('div', 'wire-pins');
    pins.append(...channels.map((y) => node('span', 'channel-tag', 'Y' + y)));
    row.append(pins, meta);
    return row;
  }));
}

function moveFocus() {
  $('route-grid').querySelector('[data-y="' + selectedY + '"][data-x="' + selectedX + '"]').focus();
}

function togglePoint(x, y) {
  if (aiControlActive) return;
  selectedX = x;
  selectedY = y;
  draft[y] ^= 1 << x;
  renderMatrix();
  moveFocus();
}

function routeParams(routes) {
  return { op: 'route', ...Object.fromEntries(routes.map((mask, index) => ['y' + index, mask])) };
}

function requireArmed() {
  requireKey();
  if (!live.armed) throw new Error('请在设备管理中确认 DTR 已断开并启用矩阵。');
}

function requireApplied() {
  requireArmed();
  if (dirty()) throw new Error('接线草稿尚未应用。');
}

function hardwareActionReady(emptyBoard) {
  if (working || aiControlActive) return false;
  try {
    if (emptyBoard) {
      requireApplied();
      if (live.routes.some(Boolean)) throw new Error('请先清空并应用接线。');
    } else {
      requireKey();
    }
  } catch (error) {
    notice(error.message, 'error');
    return false;
  }
  return true;
}

async function confirmHardwareAction(title, message, emptyBoard = false) {
  return hardwareActionReady(emptyBoard) && await confirmAction(title, message) && hardwareActionReady(emptyBoard);
}

function confirmEmptyBoard(label) {
  return confirmHardwareAction(label, '请确认 BBX 已断开所有外部接线，接线矩阵已清空。', true);
}

async function applyRoutes(routes) {
  requireArmed();
  if (!hasState) throw new Error('请先读回设备状态。');
  const state = await workbench(routeParams(routes));
  acceptState(state);
  return { routes: state.routes };
}

function showAlias() {
  const form = $('alias-form');
  form.elements.port.value = selectedX;
  form.elements.label.value = aliases[selectedX] || '';
}

function updatePwmFields() {
  $('pwm-fields').hidden = $('gpio-form').elements.mode.value !== 'pwm';
}

function choiceChannels(select) {
  if (select.hasAttribute('data-off')) select.append(new Option(select.dataset.off || '不采集', '-1'));
  for (const y of range(8)) select.append(new Option('Y' + y + ' · GPIO' + gpioMap[y], y));
}

const choose = (name, y, title) => [name, title, 'channel', y];
const number = (name, title, value, min, max) => [name, title, 'number', value, min, max];
const hex = (title, value, optional = false) => ['hex', title, 'hex', value, optional];
const commands = {
  uart: { hint: '8N1。TX、RX 各连接一根信号线，收发前确认电平和波特率。',
    fields: [choose('tx', 0, 'C3 TX → 外设 RX'), choose('rx', 1, 'C3 RX ← 外设 TX'),
      number('baud', '波特率', 9600, 2400, 115200), number('read', '读取字节上限', 16, 1, 64),
      number('timeout', '读取窗口 (ms)', 200, 20, 500), hex('发送 HEX（可留空）', '', true)] },
  i2c_scan: { hint: '100 kHz。SDA、SCL 需要外部 3.3 V 上拉。',
    fields: [choose('sda', 0, 'SDA'), choose('scl', 1, 'SCL')] },
  i2c_read: { hint: '100 kHz。发送单字节寄存器地址，然后重复起始读取。SDA、SCL 需要外部上拉。',
    fields: [choose('sda', 0, 'SDA'), choose('scl', 1, 'SCL'),
      number('address', '器件地址（十进制）', 72, 8, 119), number('reg', '寄存器（十进制）', 0, 0, 255),
      number('count', '字节数', 4, 1, 64)] },
  spi: { hint: 'SPI 模式 0，CS 低有效。发送一个字节的同时接收一个字节。',
    fields: [choose('sck', 0, 'SCK'), choose('mosi', 1, 'MOSI'), choose('miso', 2, 'MISO'), choose('cs', 3, 'CS'),
      number('hz', '时钟频率 (Hz)', 10000, 1000, 20000), hex('发送 HEX', '9F000000')] },
  proxy: { hint: '四根线分别接到独立 Y。发起后等待主控的一次请求，再转发一次响应。两侧波特率一致。',
    fields: [choose('host_tx', 0, '主控 TX → C3'), choose('host_rx', 1, '主控 RX ← C3'),
      choose('device_tx', 2, '外设 TX → C3'), choose('device_rx', 3, '外设 RX ← C3'),
      number('baud', '波特率', 9600, 2400, 115200), number('timeout', '每方向超时 (ms)', 500, 20, 500)] },
};

function renderProtocol() {
  const spec = commands[$('protocol-select').value];
  const buttons = node('div', 'fields');
  for (const [name, title, kind, value, min, max] of spec.fields) {
    const label = node('label', '', title);
    const input = node(kind === 'channel' ? 'select' : 'input');
    input.name = name;
    if (kind === 'channel') choiceChannels(input);
    if (kind === 'number') {
      input.type = 'number';
      input.required = true;
      input.min = min;
      input.max = max;
    }
    if (kind === 'hex') {
      input.maxLength = 191;
      input.required = !min;
      input.placeholder = '例如 55 AA 01 · 最多 64 字节';
    }
    input.value = value;
    label.append(input);
    buttons.append(label);
  }
  const run = node('button', 'button primary', '运行操作');
  run.type = 'submit';
  $('protocol-form').replaceChildren(node('p', '', spec.hint), buttons, run);
}

function formParams(form) {
  return Object.fromEntries(new FormData(form));
}

function requireProtocolNets(channels) {
  const groups = signalGroups(live.routes);
  const used = new Set();
  for (const y of channels) {
    const group = groups.find(({ channels }) => channels.includes(y));
    if (!group || portsIn(group.ports).length !== 1 || live.modes[y] !== 'input') {
      throw new Error('Y' + y + ' 必须处于高阻输入，且所在信号组须恰好连接一个 BBX。');
    }
    if (used.has(group)) throw new Error('协议的不同信号须接到彼此独立的信号组。');
    if (group.channels.some((other) => driveModes.has(live.modes[other]))) {
      throw new Error('Y' + y + ' 所在信号组已有 GPIO 输出，请先将输出设为高阻输入。');
    }
    used.add(group);
  }
}

function validateProtocol(params) {
  if (typeof params.hex === 'string') {
    params.hex = params.hex.replace(/\s+/g, '').toUpperCase();
    if (!/^[0-9A-F]*$/.test(params.hex) || params.hex.length % 2 || params.hex.length > 128) {
      throw new Error('请使用成对的十六进制字符，最多 64 字节。');
    }
  }
  const fields = commands[params.op].fields;
  const channels = fields.filter((field) => field[2] === 'channel').map((field) => Number(params[field[0]]));
  requireProtocolNets(channels);
}

function showOutput(id, data) {
  $(id).textContent = JSON.stringify(data, null, 2);
}

async function runProtocol(params) {
  requireApplied();
  validateProtocol(params);
  const result = await workbench(params);
  showOutput('protocol-result', result);
  lastTest = { params: { ...params }, expected: result, routes: live.routes.slice() };
  return result;
}

function resetOutput(view, message) {
  $(view).textContent = message;
}

function updateSpiSlaveRoutes() {
  const fields = $('spi-slave-form').elements;
  const duplex = Number(fields.miso.value) >= 0;
  $('spi-slave-reply').hidden = !duplex;
  fields.reply.disabled = !duplex;
  for (const signal of spiSlaveSignals) {
    const y = Number(fields[signal].value);
    const label = fields[signal].parentElement.querySelector('small');
    if (y < 0) label.textContent = '从机不驱动 MISO';
    else label.textContent = receiveRouteLabel(y);
  }
}

function receiveRouteLabel(y) {
  if (!hasState) return '待读回矩阵';
  const group = signalGroups(live.routes).find(({ channels }) => channels.includes(y));
  if (!group?.ports) return '尚未接通 BBX';
  const ports = portsIn(group.ports).map(bbx);
  const taps = group.channels.filter((other) => other !== y).map((other) => 'Y' + other);
  return ports.join(' / ') + (taps.length ? ' · 同线 ' + taps.join(' / ') : '');
}

function spiSlaveParams(form) {
  requireApplied();
  const params = { op: 'spi_slave', ...formParams(form) };
  const active = spiSlaveSignals.filter((signal) => Number(params[signal]) >= 0);
  requireProtocolNets(active.map((signal) => Number(params[signal])));
  params.reply = (params.reply ?? '').replace(/\s+/g, '').toUpperCase();
  if (Number(params.miso) >= 0 && (!/^(?:[0-9A-F]{2})+$/.test(params.reply) || params.reply.length > Number(params.bytes) * 2)) {
    throw new Error('固定应答需为完整的 HEX 字节，长度最多为每帧容量。');
  }
  return params;
}

function resultMetric(list, label, value, hexValue = false) {
  const item = node('div');
  const detail = node('dd');
  detail.append(node(hexValue ? 'code' : 'span', '', value));
  item.append(node('dt', '', label), detail);
  list.append(item);
}

function renderSpiSlaveResult(result) {
  if (!result || !Array.isArray(result.frames)) throw new Error('设备返回了无效的 SPI 从机结果。');
  const bench = result.operation === 'spi_slave_bench';
  const frames = result.frames;
  const metrics = node('dl', 'spi-slave-measurements');
  if (bench) {
    $('spi-slave-summary').textContent = spiSlaveBenchNames[result.phase] + (result.passed ? ' · 通过' : ' · 未通过');
    if (result.fixture) resultMetric(metrics, '板内主机', result.fixture === 'i2s' ? 'I²S 硬件时钟' : 'RMT');
    resultMetric(metrics, '板内主机 SCK', result.clock_hz.toLocaleString('zh-CN') + ' Hz');
    if (result.clock_hz !== result.requested_hz) {
      resultMetric(metrics, '目标 SCK', result.requested_hz.toLocaleString('zh-CN') + ' Hz');
    }
    if (result.phase === 'gap') resultMetric(metrics, '板内主机 CS 高电平间隔', result.gap_ns.toLocaleString('zh-CN') + ' ns');
    resultMetric(metrics, 'MOSI 校验', result.rx_passed ? '通过' : '未通过');
    resultMetric(metrics, '预期 MOSI', result.expected_rx, true);
    if (result.phase !== 'rx') {
      resultMetric(metrics, '预装 MISO', result.reply_hex || '—', true);
      const replies = result.observed_reply?.match(/.{1,8}/g);
      resultMetric(metrics, '主机实际收到的 MISO', replies?.join(' / ') || '—', true);
      resultMetric(metrics, 'MISO 校验', result.tx_verified ? '通过' : '未通过');
    }
  } else {
    const summary = result.timed_out ? (result.captured ? '等待超时，已返回收到的帧' : '等待超时，未收到帧') : '采集完成';
    $('spi-slave-summary').textContent = summary + ' · ' + result.captured + ' / ' + result.queued + ' 帧';
    resultMetric(metrics, 'SPI 参数', 'Mode ' + result.mode + ' · ' + result.order.toUpperCase() + ' 优先');
    resultMetric(metrics, '外部主机 SCK', '未测量');
    if (result.reply_hex) resultMetric(metrics, '预装 MISO（从机配置）', result.reply_hex, true);
  }
  const rows = frames.map((frame, index) => {
    const row = node('div', 'spi-frame');
    row.append(node('strong', '', '帧 ' + (index + 1)), node('small', '', frame.bits + ' bit'));
    const data = node('div', 'spi-bytes');
    data.append(node('span', '', 'MOSI'), node('code', '', frame.rx_hex || '—'));
    row.append(data);
    if (frame.truncated) row.append(node('p', 'decode-warning', '帧超出保留容量，已截断。'));
    if (frame.length_saturated) row.append(node('p', 'decode-warning', '位数达到硬件上限，实收帧可能更长。'));
    if (frame.bits % 8) row.append(node('p', 'decode-warning', '帧末尾不足一个字节。'));
    return row;
  });
  $('spi-slave-frames').replaceChildren(metrics, ...rows);
  showOutput('spi-slave-json', result);
}

async function runSpiSlave(button, label, waiting, perform) {
  if (working) return;
  const modePicker = $('receive-mode');
  modePicker.disabled = true;
  button.disabled = true;
  $('spi-slave-summary').textContent = waiting;
  $('spi-slave-frames').replaceChildren();
  resetOutput('spi-slave-json', waiting);
  try {
    const result = await action(label, async () => {
      const response = await perform();
      renderSpiSlaveResult(response);
      return response;
    }, 'SPI 从机已返回本批结果。');
    if (!result) {
      $('spi-slave-summary').textContent = '操作未完成。';
      resetOutput('spi-slave-json', '操作未完成。');
    } else if (result.passed === false || result.timed_out) {
      notice($('spi-slave-summary').textContent + '。', 'warning');
    }
  } finally {
    button.disabled = false;
    modePicker.disabled = false;
    syncBoardEditing();
  }
}

function updateReceiveRoutes() {
  for (const [mode, { signals }] of Object.entries(receiveModes)) {
    const fields = $(mode + '-receive-form').elements;
    for (const signal of signals) {
      const select = fields[signal];
      select.parentElement.querySelector('small').textContent = receiveRouteLabel(Number(select.value));
    }
  }
}

function updateReceiveMode() {
  const current = $('receive-mode').value;
  for (const mode of Object.keys(receiveModes)) $(mode + '-receive-form').hidden = mode !== current;
  const spi = current === 'spi';
  $('spi-slave-form').hidden = $('spi-slave-bench-panel').hidden = $('spi-slave-output').hidden = !spi;
  $('receive-bench-panel').hidden = $('receive-output').hidden = spi;
  if (!spi) {
    const spec = receiveModes[current];
    const rate = $('receive-bench-form').elements.rate;
    rate.min = spec.min;
    rate.max = spec.max;
    rate.value = spec.rate;
    $('receive-bench-rate').textContent = spec.rateLabel;
  }
  updateI2cReceiveKind();
  syncBoardEditing();
}

function updateI2cReceiveKind() {
  const register = $('i2c-receive-kind').value === 'register';
  const autoIncrement = $('i2c-auto-increment').checked;
  const config = $('i2c-register-config');
  config.hidden = !register;
  config.disabled = !register;
  config.classList.toggle('auto-increment', autoIncrement);
  $('i2c-register-count').placeholder = autoIncrement ? '默认 1 笔' : '默认等于寄存器数';
  $('i2c-register-write-tip').hidden = autoIncrement;
  $('i2c-auto-increment-tip').hidden = !autoIncrement;
  $('i2c-bench-write-tip').hidden = autoIncrement;
  $('i2c-write-tip').hidden = register;
  $('i2c-receive-capacity').hidden = register;
  $('i2c-receive-count').hidden = register;
  const fields = $('i2c-receive-form').elements;
  fields.bytes.disabled = register;
  fields.count.disabled = register;
  const tableBench = $('receive-mode').value === 'i2c' && register;
  const burstBench = $('i2c-burst-bench');
  burstBench.hidden = burstBench.disabled = !tableBench || !autoIncrement;
  $('receive-bench-start').textContent = !tableBench ? '运行一组压测'
    : autoIncrement ? '验证递增读写' : '验证寄存器读写';
}

function i2cAddress(input) {
  input = input.trim();
  const address = Number(input);
  if (!/^(?:0x[0-9a-f]{1,2}|\d{1,3})$/i.test(input) || address < 8 || address > 119) {
    throw new Error('从机地址须为 0x08 至 0x77，也可填写十进制 8 至 119。');
  }
  return address;
}

function updateI2cRegisterButtons() {
  const rows = $('i2c-register-rows').children;
  $('i2c-add-register').disabled = rows.length >= maxI2cRegisters;
  for (const row of rows) row.querySelector('button').disabled = rows.length === 1;
}

function addI2cRegister(address = '', reply = '') {
  const rows = $('i2c-register-rows');
  if (rows.children.length >= maxI2cRegisters) return;
  const row = node('div', 'i2c-register-row');
  const register = node('input', 'mono i2c-register-address');
  register.value = address;
  register.placeholder = '0x00';
  register.maxLength = 4;
  const data = node('input', 'mono i2c-register-reply');
  data.value = reply;
  data.placeholder = 'A5 5A';
  data.maxLength = 127;
  data.required = true;
  register.required = true;
  const write = node('input', 'mono i2c-register-write');
  write.placeholder = '留空读取';
  write.maxLength = 127;
  for (const input of [register, data, write]) {
    input.autocomplete = 'off';
    input.spellcheck = false;
  }
  const addressLabel = node('label', '', '地址');
  addressLabel.append(register);
  const dataLabel = node('label', '', '初始值（HEX）');
  dataLabel.append(data);
  const writeLabel = node('label', 'i2c-register-test-write', '测试写入（HEX）');
  writeLabel.append(write);
  const remove = node('button', 'button compact', '移除');
  remove.type = 'button';
  remove.setAttribute('aria-label', '移除此寄存器');
  remove.addEventListener('click', () => {
    row.remove();
    updateI2cRegisterButtons();
    $('i2c-add-register').focus();
  });
  row.append(addressLabel, dataLabel, writeLabel, remove);
  rows.append(row);
  updateI2cRegisterButtons();
  return register;
}

function i2cHexByte(byte) {
  return byte.toString(16).toUpperCase().padStart(2, '0');
}

function i2cRegisterAddress(text, label) {
  const address = Number(text);
  if (!/^(?:0x[0-9a-f]{1,2}|\d{1,3})$/i.test(text) || address > 255) {
    throw new Error(label + '须为 0x00 至 0xFF，也可填写十进制 0 至 255。');
  }
  return address;
}

function i2cBurstParams(widths) {
  const start = i2cRegisterAddress($('i2c-burst-start').value.trim(), '起始寄存器地址');
  const burst = $('i2c-burst-data').value.replace(/\s+/g, '').toUpperCase();
  if (!/^(?:[0-9A-F]{2}){1,32}$/.test(burst)) throw new Error('连续写入须填写 1–32 个完整的 HEX 字节。');
  let remaining = burst.length / 2;
  for (let address = start; remaining; ++address) {
    if (address > 255) throw new Error('递增地址超出 0xFF。');
    const label = '寄存器 0x' + i2cHexByte(address);
    const width = widths.get(address);
    if (!width) throw new Error(label + ' 尚未配置。请使用连续地址。');
    if (remaining < width) throw new Error(label + ' 需要完整的 ' + width + ' 字节。');
    remaining -= width;
  }
  return { start, burst };
}

function i2cRegisterTable(withWrites = false) {
  const rows = $('i2c-register-rows').children;
  if (!rows.length || rows.length > maxI2cRegisters) throw new Error('请配置 1–8 个寄存器。');
  const autoIncrement = $('i2c-auto-increment').checked;
  const widths = new Map();
  let registers = '';
  let writes = '';
  for (const [index, row] of Array.from(rows).entries()) {
    const label = '第 ' + (index + 1) + ' 项';
    const address = i2cRegisterAddress(row.querySelector('.i2c-register-address').value.trim(), label + '的寄存器地址');
    if (widths.has(address)) throw new Error('寄存器地址 0x' + i2cHexByte(address) + ' 重复。');
    const register = i2cHexByte(address);
    const reply = row.querySelector('.i2c-register-reply').value.replace(/\s+/g, '').toUpperCase();
    if (!/^(?:[0-9A-F]{2}){1,32}$/.test(reply)) {
      throw new Error(label + '的初始值须填写 1–32 个完整的 HEX 字节。');
    }
    const width = reply.length / 2;
    widths.set(address, width);
    registers += register + i2cHexByte(width) + reply;
    if (withWrites && !autoIncrement) {
      const write = row.querySelector('.i2c-register-write').value.replace(/\s+/g, '').toUpperCase();
      if (write && !/^(?:[0-9A-F]{2}){1,32}$/.test(write)) {
        throw new Error(label + '的测试写入须填写 1–32 个完整的 HEX 字节。');
      }
      if (write) writes += register + i2cHexByte(write.length / 2) + write;
    }
  }
  if (autoIncrement) {
    return { registers, auto_increment: 'yes', ...(withWrites && i2cBurstParams(widths)) };
  }
  return writes ? { registers, writes } : { registers };
}

function receiveParams(mode, form) {
  requireApplied();
  const spec = receiveModes[mode];
  const params = { op: spec.op, ...formParams(form) };
  const pins = spec.signals.map((signal) => Number(params[signal]));
  requireProtocolNets(pins);
  if (mode === 'i2c') {
    params.address = i2cAddress(params.address);
    if ($('i2c-receive-kind').value === 'register') {
      const countField = $('i2c-register-count');
      if (!countField.validity.valid) throw new Error('目标交易数须为 1 至 8。');
      const table = i2cRegisterTable();
      Object.assign(params, table, { bytes: 64,
        count: countField.value || (table.auto_increment ? 1 : $('i2c-register-rows').children.length) });
    }
  } else {
    const idle = Number(params.idle_ms);
    if (idle !== 0 && (idle < 10 || idle > Number(params.wait_ms))) {
      throw new Error('空闲时间可设为 0，或设为至少 10 ms 且不超过最长等待。');
    }
  }
  return params;
}

function receivedBytes(title, text, hex, truncated = false) {
  const row = node('div', 'spi-frame');
  row.append(node('strong', '', title), node('small', '', text), node('code', '', hex || '—'));
  if (truncated) row.append(node('p', 'decode-warning', '超出每帧容量，数据已截断。'));
  return row;
}

function renderI2cBytes(row, label, hex) {
  const line = node('div', 'spi-bytes');
  line.append(node('span', '', label), node('code', '', hex ? hex.match(/.{1,2}/g).join(' ') : '—'));
  row.append(line);
}

function renderI2cTransaction(transaction, index, bench, autoIncrement) {
  const row = node('div', 'spi-frame i2c-transaction');
  const actualRegister = Number.isInteger(transaction.register);
  const expectedRegister = Number.isInteger(transaction.expected_register);
  const register = actualRegister ? (autoIncrement ? ' · 起始地址 0x' : ' · 寄存器 0x') + i2cHexByte(transaction.register)
    : expectedRegister ? ' · 测试寄存器 0x' + i2cHexByte(transaction.expected_register) : '';
  row.append(node('strong', '', '交易 ' + (index + 1) + register));
  if (typeof transaction.passed === 'boolean') {
    row.append(node('small', transaction.passed ? '' : 'decode-warning', transaction.passed ? '校验通过' : '校验未通过'));
  }
  const written = transaction.write_hex || '';
  const payload = written.slice(2);
  if (autoIncrement && transaction.registers?.length) {
    for (const register of transaction.registers) {
      const part = node('div', 'i2c-burst-register');
      part.append(node('strong', '', '寄存器 0x' + i2cHexByte(register.register)));
      renderI2cBytes(part, '写入数据', register.write_hex);
      renderI2cBytes(part, '从机应答', register.reply_hex);
      if (bench) renderI2cBytes(part, '主机实际读回', register.readback_hex);
      row.append(part);
    }
  } else {
    renderI2cBytes(row, payload ? '写入数据' : '主机写入', payload || written);
    if (transaction.reply_hex) renderI2cBytes(row, '从机应答', transaction.reply_hex);
    if (bench) renderI2cBytes(row, '主机实际读回', transaction.readback_hex);
  }
  if (actualRegister && expectedRegister && transaction.register !== transaction.expected_register) {
    row.append(node('p', 'decode-warning', '预期写入寄存器 0x' + i2cHexByte(transaction.expected_register) + '。'));
  }
  if (transaction.write_applied === true) {
    row.append(node('p', '', '寄存器已更新。'));
  } else if (transaction.matched === false && written) {
    row.append(node('p', 'decode-warning', autoIncrement ? '地址或数据长度未匹配连续寄存器。' : '写入未匹配寄存器表。'));
  } else if (transaction.write_applied === false && payload) {
    row.append(node('p', 'decode-warning', '写入数据未应用。'));
  }
  if (!bench && Number.isInteger(transaction.reply_loaded_bytes)) {
    const loaded = transaction.reply_loaded_bytes
      ? '从机已装载 ' + transaction.reply_loaded_bytes + ' 字节' : '从机未装载应答';
    const end = transaction.read_completed ? '，读阶段已结束。' : '，读阶段未确认结束。';
    row.append(node('p', transaction.read_completed ? '' : 'decode-warning', loaded + end));
  }
  if (transaction.fixture_status && transaction.fixture_status !== 'ESP_OK') {
    row.append(node('p', 'decode-warning mono', transaction.fixture_status));
  }
  return row;
}

function renderProtocolReceive(mode, result) {
  const i2c = mode === 'i2c';
  if (!result || (i2c ? !Array.isArray(result.frames) : !Number.isInteger(result.received))) {
    throw new Error('设备返回了无效的协议接收结果。');
  }
  const registerTable = i2c && result.register_table === true;
  const autoIncrement = registerTable && result.auto_increment === true;
  if (registerTable && !Array.isArray(result.transactions)) throw new Error('设备返回了无效的寄存器交易结果。');
  const registerRead = i2c && !registerTable && Number.isInteger(result.register);
  const metrics = node('dl', 'spi-slave-measurements');
  const count = i2c ? result.captured : result.received;
  const quantity = i2c
    ? count + ' / ' + result.queued + (registerTable ? ' 笔交易' : registerRead ? ' 写入帧' : ' 帧') : count + ' 字节';
  const receiveEnd = result.timed_out ? (count ? '超时，已返回收到的数据' : '超时，未收到数据')
    : result.idle ? '空闲结束' : result.limit_reached ? '达到字节上限' : '采集完成';
  const status = result.bench
    ? (autoIncrement ? '递增读写测试' : registerTable || registerRead ? '重复起始测试' : '板内压测') + (result.passed ? '通过' : '未通过') : receiveEnd;
  $('protocol-receive-summary').textContent = receiveModes[mode].name + ' · ' + status + ' · ' + quantity;
  if (result.bench) {
    const unit = i2c ? ' Hz' : ' baud';
    if (Number.isFinite(result.fixture_rate)) {
      resultMetric(metrics, i2c ? '夹具 SCL' : '夹具波特率', result.fixture_rate.toLocaleString('zh-CN') + unit);
    }
    if (Number.isFinite(result.requested_rate) && result.fixture_rate !== result.requested_rate) {
      resultMetric(metrics, '目标速率', result.requested_rate.toLocaleString('zh-CN') + unit);
    }
    if (!result.passed && result.fixture_status && result.fixture_status !== 'ESP_OK') {
      resultMetric(metrics, '夹具状态', result.fixture_status, true);
    }
    if (!registerTable) resultMetric(metrics, registerRead ? '预期主机写入' : '预期数据', result.expected_hex, true);
  }
  let rows;
  if (i2c) {
    resultMetric(metrics, '从机地址', '0x' + result.address.toString(16).toUpperCase().padStart(2, '0'));
    if (registerTable) {
      rows = result.transactions.map((transaction, index) => renderI2cTransaction(transaction, index, result.bench, autoIncrement));
    } else {
      rows = result.frames.map((frame, index) => {
        const size = frame.truncated ? frame.total_bytes + ' 字节 · 保留 ' + frame.bytes : frame.bytes + ' 字节';
        return receivedBytes('写入帧 ' + (index + 1), size, frame.rx_hex, frame.truncated);
      });
    }
    if (registerRead) {
      resultMetric(metrics, '寄存器地址', '0x' + result.register.toString(16).toUpperCase().padStart(2, '0'));
      resultMetric(metrics, '配置的固定应答', result.reply_hex, true);
      resultMetric(metrics, '读请求', result.read_requests + ' 次');
      resultMetric(metrics, '从机发送缓冲', result.reply_loaded ? '已装入固定应答' : '未装入固定应答');
      if (typeof result.read_completed === 'boolean') {
        resultMetric(metrics, '读阶段', result.read_completed ? '已结束' : '未确认结束');
      }
      if (result.bench) {
        const readback = result.readback_hex || '';
        resultMetric(metrics, '读回校验', readback && readback === result.reply_hex ? '通过' : '未通过');
        rows.push(receivedBytes('主机实测读回', (readback.length / 2) + ' 字节', readback));
      }
    }
  } else {
    resultMetric(metrics, '接收波特率', result.actual_baud.toLocaleString('zh-CN') + ' baud');
    if (!result.bench && result.actual_baud !== result.requested_baud) {
      resultMetric(metrics, '目标波特率', result.requested_baud.toLocaleString('zh-CN') + ' baud');
    }
    resultMetric(metrics, '接收溢出', result.overflow ? '有' : '无');
    resultMetric(metrics, '帧错误', result.frame_errors);
    resultMetric(metrics, '校验错误', result.parity_errors);
    resultMetric(metrics, 'Break 事件', result.break_events);
    resultMetric(metrics, '事件丢失', result.events_lost ? '有' : '无');
    rows = [receivedBytes('接收数据', quantity, result.rx_hex)];
  }
  $('protocol-receive-results').replaceChildren(metrics, ...rows);
  showOutput('protocol-receive-json', result);
}

async function runProtocolReceive(mode, button, bench, perform) {
  if (working) return;
  const modePicker = $('receive-mode');
  const summary = $('protocol-receive-summary');
  const waiting = bench ? '正在通过 CH446Q 执行回环压测…' : '正在采集，等待外部数据…';
  const name = receiveModes[mode].name;
  modePicker.disabled = true;
  button.disabled = true;
  summary.textContent = waiting;
  $('protocol-receive-results').replaceChildren();
  resetOutput('protocol-receive-json', waiting);
  try {
    const result = await action(name + (bench ? '板内压测' : '采集'), async () => {
      const response = await perform();
      renderProtocolReceive(mode, response);
      return response;
    }, '已返回本批接收结果。');
    if (!result) {
      summary.textContent = '操作未完成。';
      resetOutput('protocol-receive-json', summary.textContent);
    } else if (result.passed === false || result.timed_out || result.overflow || result.events_lost ||
               result.frame_errors || result.parity_errors || result.break_events ||
               result.frames?.some((frame) => frame.truncated) ||
               result.transactions?.some((transaction) => transaction.matched === false || transaction.read_completed === false ||
                 (transaction.write_applied === false && transaction.write_hex?.length > 2))) {
      notice(summary.textContent + '。', 'warning');
    }
  } finally {
    button.disabled = false;
    modePicker.disabled = false;
    syncBoardEditing();
  }
}

function captureSlot(channel, index) {
  return channel.slot ?? index;
}

function captureLabel(slot) {
  const role = captureRoles[$('capture-protocol').value]?.[slot];
  return captureLetters[slot] + (role ? ' · ' + role : '');
}

function captureChannelsBySlot(channels) {
  const slots = Array(4).fill(null);
  channels.forEach((channel, index) => { slots[captureSlot(channel, index)] = channel; });
  return slots;
}

function captureEndUs(capture) {
  return Math.round(capture.duration_ms * 1e6) / 1000;
}

function captureLevelBefore(channel, time) {
  let level = channel.initial;
  for (const [edgeTime, value] of channel.edges) {
    if (edgeTime >= time) break;
    level = value;
  }
  return level;
}

const scopeView = {
  capture: null, start: 0, span: 0, cursors: [null, null], activeCursor: 0,
  placing: null, channel: null, pointer: null, drag: null, annotations: [],
};
const scopeCursorColors = ['#fbd28b', '#beccff'];

function scopeClamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function scopeTimeText(us) {
  if (us !== 0 && Math.abs(us) < 1) return Math.round(us * 1000) + ' ns';
  if (Math.abs(us) >= 1000) return Number((us / 1000).toFixed(6)) + ' ms';
  return Number(us.toFixed(3)) + ' µs';
}

function scopeFrequencyText(hz) {
  const units = hz >= 1e6 ? [1e6, 'MHz'] : hz >= 1000 ? [1000, 'kHz'] : [1, 'Hz'];
  return Number((hz / units[0]).toPrecision(4)) + ' ' + units[1];
}

function scopeBounds(capture = lastCapture) {
  const end = capture ? captureEndUs(capture) : 150000;
  const resolution = capture?.engine === 'dma' && capture.sample_hz > 0 ? 1e6 / capture.sample_hz : 1;
  const step = Number.isFinite(resolution) ? Math.max(0.001, resolution) : 1;
  return { end, step, minSpan: Math.min(end, Math.max(0.008, 8 * step)) };
}

function setScopeWindowError(message = '') {
  const error = $('scope-window-error');
  error.textContent = message;
  error.hidden = !message;
}

function syncScopeWindowFields() {
  const start = $('scope-window-start'), end = $('scope-window-end');
  const hasData = Boolean(lastCapture?.channels.length);
  const milliseconds = (us) => String(Number((us / 1000).toFixed(6)));
  start.disabled = end.disabled = $('scope-window-apply').disabled = !hasData;
  start.value = hasData ? milliseconds(scopeView.start) : '';
  end.value = hasData ? milliseconds(scopeView.start + scopeView.span) : '';
  setScopeWindowError();
}

function resetScopeForCapture() {
  if (scopeView.capture === lastCapture) return;
  const channels = lastCapture?.channels || [];
  Object.assign(scopeView, {
    capture: lastCapture, start: 0, span: scopeBounds().end, cursors: [null, null],
    activeCursor: 0, placing: null, channel: channels[0]?.slot ?? null,
    pointer: null, drag: null, annotations: [],
  });
  const picker = $('scope-channel');
  picker?.replaceChildren(...channels.map((channel, index) =>
    new Option(captureLabel(captureSlot(channel, index)) + ' / Y' + channel.y, captureSlot(channel, index))));
  syncScopeWindowFields();
}

function scopeAnnotations() {
  return scopeView.annotations.filter(({ slot }) => slot == null || slot === scopeView.channel);
}

function scopeGeometry(width, count = lastCapture?.channels.length || 0,
                       annotated = scopeAnnotations().length > 0) {
  const left = Math.min(78, width * 0.24);
  const right = Math.max(left + 1, width - 16);
  const top = annotated ? 56 : 32;
  const lanes = Math.max(1, count);
  const height = Math.max(176, top + lanes * 58 + 14);
  const bottom = height - 14;
  return { width, height, left, right, top, bottom, lane: (bottom - top) / lanes };
}

function scopeTimeAt(x, view) {
  return scopeView.start + scopeClamp((x - view.left) / (view.right - view.left), 0, 1) * scopeView.span;
}

function scopeTimeX(us, view) {
  return view.left + (us - scopeView.start) * (view.right - view.left) / scopeView.span;
}

function setScopeViewport(start, span) {
  const { end, minSpan } = scopeBounds();
  scopeView.span = scopeClamp(span, minSpan, end);
  scopeView.start = scopeClamp(start, 0, end - scopeView.span);
  syncScopeWindowFields();
  renderScope();
}

function zoomScope(factor, anchor = 0.5) {
  if (!lastCapture?.channels.length) return;
  const { end, minSpan } = scopeBounds();
  const span = scopeClamp(scopeView.span * factor, minSpan, end);
  setScopeViewport(scopeView.start + (scopeView.span - span) * anchor, span);
}

function fitScope() {
  setScopeViewport(0, scopeBounds().end);
}

function scopeEdgesBefore(edges, time, inclusive = false) {
  let start = 0, end = edges.length;
  while (start < end) {
    const mid = (start + end) >>> 1;
    if (edges[mid][0] < time || (inclusive && edges[mid][0] === time)) start = mid + 1;
    else end = mid;
  }
  return start;
}

function scopeLevelAt(channel, time) {
  const index = scopeEdgesBefore(channel.edges, time, true);
  return index ? channel.edges[index - 1][1] : channel.initial;
}

function scopeChannelAt(y, view) {
  if (y < view.top || y >= view.bottom) return null;
  return lastCapture?.channels[Math.floor((y - view.top) / view.lane)] ?? null;
}

function scopeSelectedChannel() {
  return lastCapture?.channels.find((channel, index) =>
    captureSlot(channel, index) === scopeView.channel) ?? null;
}

function scopeNearestEdge(channel, time) {
  const start = lastCapture.sample_start_us ?? 0;
  const next = scopeEdgesBefore(channel.edges, time);
  let nearest = null;
  for (const edge of [channel.edges[next - 1], channel.edges[next]]) {
    if (edge && edge[0] >= start && (!nearest || Math.abs(edge[0] - time) < Math.abs(nearest[0] - time))) nearest = edge;
  }
  return nearest;
}

function scopeSnapTime(x, y, view) {
  const time = scopeTimeAt(x, view);
  const channel = scopeChannelAt(y, view) ?? scopeSelectedChannel();
  const edge = channel && scopeNearestEdge(channel, time);
  if (edge && Math.abs(scopeTimeX(edge[0], view) - x) <= 8) return edge[0];
  const { step, end } = scopeBounds();
  return scopeClamp(Number((Math.round(time / step) * step).toFixed(3)), 0, end);
}

function scopePulse(channel, time) {
  const start = lastCapture.sample_start_us ?? 0;
  if (time < start || time > channel.window_us) return null;
  const index = scopeEdgesBefore(channel.edges, time, true) - 1;
  const before = channel.edges[index];
  const after = channel.edges[index + 1];
  const pulse = { level: before?.[1] ?? channel.initial, width: null, period: null, duty: null };
  if (!before || !after || before[0] < start || after[0] <= before[0] || after[1] === before[1]) return pulse;
  pulse.width = after[0] - before[0];
  const following = channel.edges[index + 2];
  const preceding = channel.edges[index - 1];
  if (following && following[1] === before[1] && following[0] > after[0]) {
    pulse.period = following[0] - before[0];
  } else if (preceding && preceding[0] >= start && preceding[1] === after[1] && preceding[0] < before[0]) {
    pulse.period = after[0] - preceding[0];
  }
  if (pulse.period) pulse.duty = 100 * (pulse.level ? pulse.width : pulse.period - pulse.width) / pulse.period;
  return pulse;
}

function scopeEdgeCounts(channel, from, to) {
  const start = lastCapture.sample_start_us ?? 0;
  const low = Math.max(from, start);
  const high = Math.min(to, channel.window_us);
  let rising = 0, falling = 0;
  for (let i = scopeEdgesBefore(channel.edges, low, true);
       i < channel.edges.length && channel.edges[i][0] <= high; i++) {
    if (channel.edges[i][1]) rising++;
    else falling++;
  }
  return { rising, falling, partial: from < start || to > channel.window_us };
}

function scopeRelativeEdge(direction) {
  const channel = scopeSelectedChannel();
  if (!channel) return null;
  const cursor = scopeView.cursors[scopeView.activeCursor];
  const time = cursor ?? scopeView.start;
  const before = scopeEdgesBefore(channel.edges, time, direction > 0 && cursor !== null);
  const sampleStart = lastCapture.sample_start_us ?? 0;
  const firstSampled = scopeEdgesBefore(channel.edges, sampleStart);
  const index = direction < 0 ? before - 1 : Math.max(before, firstSampled);
  return index >= firstSampled ? channel.edges[index]?.[0] ?? null : null;
}

function jumpScopeEdge(direction) {
  const time = scopeRelativeEdge(direction);
  if (time === null) return;
  scopeView.cursors[scopeView.activeCursor] = time;
  scopeView.placing = null;
  setScopeViewport(time - scopeView.span / 2, scopeView.span);
}

function scopeReadouts(view) {
  const hasData = Boolean(lastCapture?.channels.length);
  const { end, minSpan } = scopeBounds();
  const [a, b] = scopeView.cursors;
  for (const [id, disabled] of Object.entries({
    'scope-fit': !hasData,
    'scope-zoom-in': !hasData || scopeView.span <= minSpan,
    'scope-zoom-out': !hasData || scopeView.span >= end,
    'scope-trigger': !hasData || lastCapture.trigger === 'manual',
    'scope-cursor-a': !hasData, 'scope-cursor-b': !hasData,
    'scope-cursor-clear': a === null && b === null && scopeView.placing === null,
    'scope-channel': !hasData,
    'scope-edge-prev': !hasData || scopeRelativeEdge(-1) === null,
    'scope-edge-next': !hasData || scopeRelativeEdge(1) === null,
  })) {
    if ($(id)) $(id).disabled = disabled;
  }
  scopeView.cursors.forEach((marker, index) => {
    const letter = index === 0 ? 'A' : 'B';
    const button = $('scope-cursor-' + letter.toLowerCase());
    if (!button) return;
    button.setAttribute('aria-pressed', String(scopeView.activeCursor === index && (marker !== null || scopeView.placing === index)));
    button.dataset.placing = String(scopeView.placing === index);
  });
  if ($('scope-viewport')) {
    $('scope-viewport').textContent = hasData
      ? scopeTimeText(scopeView.start) + ' – ' + scopeTimeText(scopeView.start + scopeView.span) + ' / ' + scopeTimeText(end) +
        ' · ' + scopeTimeText(scopeTickStep(view)) + '/格'
      : '—';
  }
  if ($('scope-measure')) {
    const markers = [a, b].map((time, index) => '游标 ' + (index === 0 ? 'A' : 'B') + ' ' + (time === null ? '—' : scopeTimeText(time)));
    if (a !== null && b !== null) {
      const delta = Math.abs(b - a);
      markers.push('Δt ' + scopeTimeText(delta));
      if (delta > 0) markers.push('1/Δt ' + scopeFrequencyText(1e6 / delta));
      const channel = scopeSelectedChannel();
      if (channel) {
        const { rising, falling, partial } = scopeEdgeCounts(channel, Math.min(a, b), Math.max(a, b));
        markers.push('Y' + channel.y + ' ↑ ' + rising + ' ↓ ' + falling + (partial ? '（部分区间无数据）' : ''));
      }
    }
    $('scope-measure').textContent = markers.join(' · ');
  }
  const pointer = scopeView.pointer;
  const channel = pointer && scopeChannelAt(pointer.y, view);
  let hover = scopeView.placing === null ? '' : '点击波形放置游标 ' + (scopeView.placing === 0 ? 'A' : 'B');
  if (hasData && pointer && pointer.x >= view.left && pointer.x <= view.right) {
    const time = scopeTimeAt(pointer.x, view);
    const text = [scopeTimeText(time)];
    if (channel) {
      const pulse = scopePulse(channel, time);
      text.push(captureLabel(channel.slot) + ' / Y' + channel.y, pulse ? '电平 ' + pulse.level : '无采样');
      if (pulse && pulse.width !== null) text.push('脉宽 ' + scopeTimeText(pulse.width));
      if (pulse?.period) text.push('周期 ' + scopeTimeText(pulse.period), scopeFrequencyText(1e6 / pulse.period),
        '占空比 ' + Number(pulse.duty.toFixed(2)) + '%');
      const edge = scopeNearestEdge(channel, time);
      if (edge && Math.abs(scopeTimeX(edge[0], view) - pointer.x) <= 8) {
        text.push((edge[1] ? '上升沿 ' : '下降沿 ') + scopeTimeText(edge[0]));
      }
    }
    if (hover) text.push(hover);
    hover = text.join(' · ');
  }
  if ($('scope-hover')) $('scope-hover').textContent = hover || '—';
}

function scopeTickStep(view) {
  const target = scopeView.span / Math.max(2, (view.right - view.left) / 110);
  const power = 10 ** Math.floor(Math.log10(target));
  const base = target / power;
  return (base > 5 ? 10 : base > 2 ? 5 : base > 1 ? 2 : 1) * power;
}

function drawScopeTicks(context, view) {
  const step = scopeTickStep(view);
  const end = scopeView.start + scopeView.span;
  const first = Math.ceil(scopeView.start / step - 1e-8);
  context.font = '11px ui-monospace, monospace';
  context.textAlign = 'center';
  context.lineWidth = 1;
  for (let tick = first; tick * step <= end + 1e-8; tick++) {
    const time = tick * step;
    const x = scopeTimeX(time, view);
    context.strokeStyle = '#456257';
    context.beginPath();
    context.moveTo(x, view.top - 5);
    context.lineTo(x, view.bottom);
    context.stroke();
    const text = scopeTimeText(time);
    const half = context.measureText(text).width / 2;
    context.fillStyle = '#d2e1da';
    context.fillText(text, scopeClamp(x, view.left + half, view.right - half), 17);
  }
}

function drawScopeTrace(context, channel, start, end, high, low, timeX) {
  if (end < start) return;
  const index = scopeEdgesBefore(channel.edges, start);
  let level = index ? channel.edges[index - 1][1] : channel.initial;
  context.beginPath();
  context.moveTo(timeX(start), level ? high : low);
  for (let i = index; i < channel.edges.length && channel.edges[i][0] <= end; i++) {
    const [time, next] = channel.edges[i];
    const x = timeX(time);
    context.lineTo(x, level ? high : low);
    level = next;
    context.lineTo(x, level ? high : low);
  }
  context.lineTo(timeX(end), level ? high : low);
  context.stroke();
}

function scopeContext(canvas, width, height) {
  const context = canvas.getContext('2d');
  const dpr = Math.min(devicePixelRatio || 1, 2);
  const pixelsWide = Math.round(width * dpr), pixelsHigh = Math.round(height * dpr);
  if (canvas.width !== pixelsWide) canvas.width = pixelsWide;
  if (canvas.height !== pixelsHigh) canvas.height = pixelsHigh;
  context.setTransform(dpr, 0, 0, dpr, 0, 0);
  context.clearRect(0, 0, width, height);
  return context;
}

function drawScopeLane(context, channel, index, view) {
  const laneTop = view.top + index * view.lane;
  const high = laneTop + view.lane * 0.23;
  const low = laneTop + view.lane * 0.72;
  const color = captureColors[channel.slot];
  const end = scopeView.start + scopeView.span;
  const timeX = (time) => scopeTimeX(time, view);
  const sampleStart = lastCapture.sample_start_us ?? 0;
  context.fillStyle = scopeView.channel === channel.slot ? '#273f35' : '#1e332c';
  context.fillRect(0, laneTop, view.left, view.lane - 1);
  context.font = '11px ui-monospace, monospace';
  context.textAlign = 'left';
  context.fillStyle = color;
  context.fillText(captureLabel(channel.slot), 8, high + 2, view.left - 13);
  context.fillStyle = '#b6cac0';
  context.fillText('Y' + channel.y, 8, low + 1);
  context.strokeStyle = '#3b574c';
  context.lineWidth = 1;
  context.beginPath();
  context.moveTo(view.left, laneTop + view.lane - 0.5);
  context.lineTo(view.right, laneTop + view.lane - 0.5);
  context.stroke();
  const gap = (from, to, text) => {
    const lowX = scopeClamp(timeX(from), view.left, view.right);
    const highX = scopeClamp(timeX(to), view.left, view.right);
    if (highX <= lowX) return;
    context.fillStyle = '#ffffff18';
    context.fillRect(lowX, laneTop + 1, highX - lowX, view.lane - 3);
    if (highX - lowX >= 72) {
      context.fillStyle = '#c1cfc8';
      context.fillText(text, lowX + 5, high + 12, highX - lowX - 10);
    }
  };
  gap(0, sampleStart, '尚未采样');
  gap(channel.window_us, captureEndUs(lastCapture), '记录截断');
  context.strokeStyle = color;
  context.lineWidth = 1.8;
  drawScopeTrace(context, channel, Math.max(scopeView.start, sampleStart),
    Math.min(end, channel.window_us), high, low, timeX);
}

function drawScopeMarker(context, view, time, label, color) {
  const x = scopeTimeX(time, view);
  if (x < view.left - 0.1 || x > view.right + 0.1) return;
  const labelX = scopeClamp(x, view.left + 10, view.right - 10);
  context.fillStyle = color;
  context.strokeStyle = color;
  context.lineWidth = 1;
  context.beginPath();
  context.moveTo(x, view.top - 9);
  context.lineTo(x, view.bottom);
  context.stroke();
  context.font = 'bold 11px ui-monospace, monospace';
  context.textAlign = 'center';
  context.fillRect(labelX - 9, view.top - 12, 18, 13);
  context.fillStyle = '#152520';
  context.fillText(label, labelX, view.top - 2);
}

function drawScopeAnnotations(context, view) {
  context.font = '11px ui-monospace, monospace';
  context.textAlign = 'left';
  for (const mark of scopeAnnotations()) {
    const left = scopeClamp(scopeTimeX(mark.start, view), view.left, view.right);
    const right = scopeClamp(scopeTimeX(mark.end, view), view.left, view.right);
    if (right <= left) continue;
    context.fillStyle = '#447363';
    context.fillRect(left, 23, Math.max(1, right - left - 1), 15);
    const columns = Math.floor((right - left - 6) / 7);
    if (columns < 2) continue;
    context.fillStyle = '#e7f4ed';
    const label = mark.label.length > columns ? mark.label.slice(0, columns - 1) + '…' : mark.label;
    context.fillText(label, left + 3, 34, right - left - 6);
  }
}

function drawScopeOverview() {
  const canvas = $('scope-overview');
  if (!canvas) return;
  const width = canvas.clientWidth, height = canvas.clientHeight;
  if (!width || !height) return;
  const context = scopeContext(canvas, width, height);
  const channels = lastCapture?.channels || [];
  if (!channels.length) return;
  const end = scopeBounds().end;
  const timeX = (us) => 6 + us * Math.max(1, width - 12) / end;
  const rowHeight = (height - 10) / channels.length;
  for (const [i, channel] of channels.entries()) {
    context.lineWidth = 1;
    context.strokeStyle = captureColors[channel.slot];
    drawScopeTrace(context, channel, lastCapture.sample_start_us ?? 0, channel.window_us,
      6 + rowHeight * i, 3 + rowHeight * (i + 1), timeX);
  }
  const left = timeX(scopeView.start), right = timeX(scopeView.start + scopeView.span);
  context.fillStyle = '#101e19a8';
  context.fillRect(0, 0, left, height);
  context.fillRect(right, 0, width - right, height);
  context.strokeStyle = '#bad8c9';
  context.strokeRect(left, 1.5, Math.max(1, right - left), height - 3);
  context.fillStyle = '#bad8c9';
  for (const x of [left, right]) context.fillRect(x - 2, (height - 15) / 2, 4, 15);
}

function renderScope() {
  resetScopeForCapture();
  const canvas = $('scope-canvas');
  const width = canvas.clientWidth;
  const view = scopeGeometry(width);
  scopeReadouts(view);
  canvas.style.height = view.height + 'px';
  if (!width) return;
  const context = scopeContext(canvas, width, view.height);
  const channels = lastCapture?.channels || [];
  drawScopeOverview();
  if (!channels.length) return;
  drawScopeTicks(context, view);
  drawScopeAnnotations(context, view);
  channels.forEach((channel, index) => drawScopeLane(context, channel, index, view));
  const [a, b] = scopeView.cursors;
  if (a !== null && b !== null) {
    const low = scopeClamp(scopeTimeX(Math.min(a, b), view), view.left, view.right);
    const high = scopeClamp(scopeTimeX(Math.max(a, b), view), view.left, view.right);
    context.fillStyle = '#c9d8ff19';
    context.fillRect(low, view.top, high - low, view.bottom - view.top);
  }
  if (lastCapture.trigger !== 'manual') drawScopeMarker(context, view, 0, 'T', '#c6d9ce');
  scopeView.cursors.forEach((time, i) => {
    if (time !== null) drawScopeMarker(context, view, time, i ? 'B' : 'A', scopeCursorColors[i]);
  });
  const x = scopeView.pointer?.x;
  if (!scopeView.drag && x >= view.left && x <= view.right) {
    context.strokeStyle = '#d1e4da90';
    context.lineWidth = 1;
    context.setLineDash([3, 4]);
    context.beginPath();
    context.moveTo(x, view.top);
    context.lineTo(x, view.bottom);
    context.stroke();
    context.setLineDash([]);
  }
}

function scopeEventPoint(event, canvas) {
  const bounds = canvas.getBoundingClientRect();
  return { x: event.clientX - bounds.left, y: event.clientY - bounds.top };
}

function scopeSelectChannel(channel) {
  if (!channel) return;
  scopeView.channel = channel.slot;
  if ($('scope-channel')) $('scope-channel').value = String(channel.slot);
}

function scopeFocusRange(start, end) {
  const bounds = scopeBounds();
  const span = scopeClamp((end - start) * 1.35, bounds.minSpan, bounds.end);
  setScopeViewport((start + end - span) / 2, span);
}

function setupScopeControls() {
  const canvas = $('scope-canvas');
  const listen = (id, callback) => $(id)?.addEventListener('click', callback);
  listen('scope-fit', fitScope);
  listen('scope-zoom-in', () => zoomScope(0.5));
  listen('scope-zoom-out', () => zoomScope(2));
  listen('scope-trigger', () => setScopeViewport(0, scopeView.span));
  listen('scope-edge-prev', () => jumpScopeEdge(-1));
  listen('scope-edge-next', () => jumpScopeEdge(1));
  for (const [i, name] of ['a', 'b'].entries()) {
    listen('scope-cursor-' + name, () => {
      scopeView.activeCursor = i;
      scopeView.placing = scopeView.placing === i ? null : i;
      canvas.focus({ preventScroll: true });
      renderScope();
    });
  }
  listen('scope-cursor-clear', () => {
    scopeView.cursors = [null, null];
    scopeView.placing = null;
    renderScope();
  });
  $('scope-channel')?.addEventListener('change', (event) => {
    scopeView.channel = Number(event.target.value);
    renderScope();
  });
  const windowForm = $('scope-window-form');
  windowForm.addEventListener('input', () => setScopeWindowError());
  windowForm.addEventListener('submit', (event) => {
    event.preventDefault();
    if (!lastCapture?.channels.length) return;
    const start = $('scope-window-start').valueAsNumber * 1000;
    const end = $('scope-window-end').valueAsNumber * 1000;
    const { end: limit, minSpan } = scopeBounds();
    const span = end - start;
    if (!Number.isFinite(start) || !Number.isFinite(end) || start < 0 || end > limit + 1e-7 || span <= 0) {
      setScopeWindowError('请输入记录范围内的起止时间，终点须大于起点。');
    } else if (span + 0.001 < minSpan) {
      setScopeWindowError('窗口宽度至少为 ' + scopeTimeText(minSpan) + '。');
    } else {
      setScopeViewport(start, span);
    }
  });

  let hoverPending = false;
  function requestHoverRender() {
    if (hoverPending) return;
    hoverPending = true;
    requestAnimationFrame(() => {
      hoverPending = false;
      renderScope();
    });
  }

  canvas.addEventListener('wheel', (event) => {
    if (!lastCapture?.channels.length || !event.deltaY) return;
    const { x } = scopeEventPoint(event, canvas);
    const view = scopeGeometry(canvas.clientWidth);
    if (x < view.left || x > view.right) return;
    event.preventDefault();
    zoomScope(event.deltaY > 0 ? 1.35 : 1 / 1.35, (x - view.left) / (view.right - view.left));
  }, { passive: false });

  canvas.addEventListener('pointerdown', (event) => {
    if (!lastCapture?.channels.length || event.button !== 0 || scopeView.drag) return;
    const point = scopeEventPoint(event, canvas);
    const { x, y } = point;
    const view = scopeGeometry(canvas.clientWidth);
    if (x > view.right || y > view.bottom) return;
    scopeSelectChannel(scopeChannelAt(y, view));
    if (x < view.left) {
      renderScope();
      return;
    }

    scopeView.pointer = point;
    const cursor = scopeView.placing ?? scopeView.cursors.findIndex((time) =>
      time !== null && Math.abs(scopeTimeX(time, view) - x) <= 8);
    if (cursor < 0 && y >= 23 && y <= 38) {
      const time = scopeTimeAt(x, view);
      const target = scopeAnnotations().find(({ start, end }) => start <= time && time <= end);
      if (target) {
        scopeFocusRange(target.start, target.end);
        return;
      }
    }
    scopeView.drag = { id: event.pointerId, cursor, x, start: scopeView.start };
    if (event.isTrusted) canvas.setPointerCapture(event.pointerId);
    if (cursor >= 0) {
      scopeView.activeCursor = cursor;
      scopeView.placing = null;
      scopeView.cursors[cursor] = scopeSnapTime(x, y, view);
    }
    renderScope();
  });

  canvas.addEventListener('pointermove', (event) => {
    if (!lastCapture?.channels.length) return;
    const point = scopeEventPoint(event, canvas);
    scopeView.pointer = point;
    const drag = scopeView.drag;
    if (!drag || drag.id !== event.pointerId) {
      requestHoverRender();
      return;
    }
    const view = scopeGeometry(canvas.clientWidth);
    if (drag.cursor >= 0) {
      scopeView.cursors[drag.cursor] = scopeSnapTime(point.x, point.y, view);
      requestHoverRender();
    } else {
      const offset = (drag.x - point.x) * scopeView.span / (view.right - view.left);
      setScopeViewport(drag.start + offset, scopeView.span);
    }
  });

  function finishScopeDrag(event) {
    if (scopeView.drag?.id === event.pointerId) {
      scopeView.drag = null;
      renderScope();
    }
  }

  canvas.addEventListener('pointerup', finishScopeDrag);
  canvas.addEventListener('pointercancel', finishScopeDrag);
  canvas.addEventListener('lostpointercapture', finishScopeDrag);
  canvas.addEventListener('pointerleave', () => {
    scopeView.pointer = null;
    requestHoverRender();
  });
  canvas.addEventListener('keydown', (event) => {
    if (!lastCapture?.channels.length || event.ctrlKey || event.metaKey || event.altKey) return;
    if (event.key === '+' || event.key === '=') zoomScope(0.65);
    else if (event.key === '-') zoomScope(1 / 0.65);
    else if (event.key === 'Home') fitScope();
    else if (event.key === 'Escape' && scopeView.placing !== null) {
      scopeView.placing = null;
      renderScope();
    } else if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      const direction = event.key === 'ArrowLeft' ? -1 : 1;
      if (event.shiftKey) jumpScopeEdge(direction);
      else setScopeViewport(scopeView.start + direction * scopeView.span * 0.15, scopeView.span);
    } else return;
    event.preventDefault();
  });

  const overview = $('scope-overview');
  if (!overview) return;
  let overviewDrag = null;
  const overviewUsPerPixel = () => scopeBounds().end / Math.max(1, overview.clientWidth - 12);
  const overviewTime = (event) => (scopeEventPoint(event, overview).x - 6) * overviewUsPerPixel();
  function overviewAction(time, pointerType) {
    const start = scopeView.start, end = start + scopeView.span;
    const fromStart = Math.abs(time - start), fromEnd = Math.abs(time - end);
    const tolerance = (pointerType === 'touch' ? 12 : 8) * overviewUsPerPixel();
    if (Math.min(fromStart, fromEnd) <= tolerance) return fromStart < fromEnd ? 'start' : 'end';
    return time > start && time < end ? 'pan' : 'jump';
  }
  overview.addEventListener('pointerdown', (event) => {
    if (!lastCapture?.channels.length || event.button !== 0 || overviewDrag) return;
    const time = overviewTime(event);
    const action = overviewAction(time, event.pointerType);
    if (action === 'jump') setScopeViewport(time - scopeView.span / 2, scopeView.span);
    const { start, span } = scopeView;
    overviewDrag = { id: event.pointerId, capture: lastCapture, action, time, previousTime: time, start, end: start + span };
    overview.dataset.action = action;
    if (event.isTrusted) overview.setPointerCapture(event.pointerId);
  });
  function moveOverviewDrag(event) {
    if (overviewDrag?.id !== event.pointerId || overviewDrag.capture !== lastCapture) return;
    const time = overviewTime(event);
    if (time === overviewDrag.previousTime) return;
    overviewDrag.previousTime = time;
    const { action, start, end } = overviewDrag;
    const { end: limit, minSpan } = scopeBounds();
    const delta = time - overviewDrag.time;
    if (action === 'start') {
      const nextStart = scopeClamp(start + delta, 0, end - minSpan);
      setScopeViewport(nextStart, end - nextStart);
    } else if (action === 'end') {
      const nextEnd = scopeClamp(end + delta, start + minSpan, limit);
      setScopeViewport(start, nextEnd - start);
    } else {
      setScopeViewport(start + delta, end - start);
    }
  }
  overview.addEventListener('pointermove', (event) => {
    if (!lastCapture?.channels.length) return;
    if (!overviewDrag) {
      overview.dataset.action = overviewAction(overviewTime(event), event.pointerType);
      return;
    }
    moveOverviewDrag(event);
  });
  overview.addEventListener('pointerup', moveOverviewDrag);
  for (const eventName of ['pointerup', 'pointercancel', 'lostpointercapture']) {
    overview.addEventListener(eventName, (event) => {
      if (overviewDrag?.id === event.pointerId) {
        overviewDrag = null;
        delete overview.dataset.action;
      }
    });
  }
  overview.addEventListener('pointerleave', () => {
    if (!overviewDrag) delete overview.dataset.action;
  });
}

function renderCapture(result) {
  lastCapture = result;
  resetScopeForCapture();
  for (const option of $('scope-channel')?.options ?? []) {
    const channel = result.channels.find(({ slot }) => slot === Number(option.value));
    if (channel) option.textContent = captureLabel(channel.slot) + ' / Y' + channel.y;
  }
  const timedOut = result.triggered === false;
  $('scope-empty').hidden = !timedOut;
  if (timedOut) $('scope-empty').textContent = '等待超时，未检测到指定边沿';
  $('capture-vcd').disabled = timedOut || !result.channels.length;
  $('capture-summary').textContent = timedOut ? '本次无采集数据'
    : result.trigger && result.trigger !== 'manual'
      ? '通道 ' + captureLabel(Number(result.trigger_channel) || 0) +
        (result.trigger === 'rising' ? '上升沿' : '下降沿') + '触发 · 记录 ' + result.duration_ms + ' ms'
      : '手动采集 · 记录 ' + result.duration_ms + ' ms';
  if (!timedOut && result.engine === 'dma') {
    $('capture-summary').textContent += ' · ' + (result.sample_hz / 1e6).toFixed(2) + ' MHz 同步采样';
    if (result.sample_start_us > 0) {
      $('capture-summary').textContent += ' · 触发后约 ' + result.sample_start_us + ' µs 开始采样';
    }
  }
  const uart = $('capture-protocol').value === 'uart';
  $('capture-channels').replaceChildren(...result.channels.map((channel, index) => {
    const item = node('div', 'capture-summary');
    const summary = channel.edges.length + ' 个边沿' + (uart ?
      ' · ' + (channel.baud ? ('UART ' + channel.baud + ' baud') : '未识别 UART') : '');
    item.append(node('strong', '', captureLabel(captureSlot(channel, index)) + ' / Y' + channel.y),
      node('div', '', summary));
    if (uart && channel.hex) item.append(node('code', '', channel.hex));
    if (channel.window_us < captureEndUs(result)) item.append(node('small', '', '边沿记录已截断'));
    else if (uart && channel.overflow) item.append(node('small', '', 'UART 结果已截断'));
    if (uart && channel.framing_errors) item.append(node('small', '', channel.framing_errors + ' 个帧错误'));
    return item;
  }));
  showOutput('capture-result', result);
  renderDecodes(result);
  renderScope();
}

function captureVcd(capture) {
  const channels = capture.channels;
  const duration = Math.round(capture.duration_ms * 1e6);
  const startUs = capture.sample_start_us ?? 0;
  const startNs = Math.round(startUs * 1000);
  const ids = channels.map((_, index) => String.fromCharCode(33 + index));
  const changes = channels.flatMap((channel, index) => {
    const id = ids[index];
    const edges = channel.edges.filter(([time]) => time >= startUs)
      .map(([time, level]) => ({ time: Math.round(time * 1000), value: level + id }));
    if (startNs > 0) {
      edges.unshift({ time: startNs, value: captureLevelBefore(channel, startUs) + id });
      const confirmedTrigger = captureSlot(channel, index) === capture.trigger_channel && channel.edges[0]?.[0] === 0;
      if (confirmedTrigger) {
        edges.unshift({ time: 0, value: channel.edges[0][1] + id });
        if (startNs > 1) edges.splice(1, 0, { time: 1, value: 'x' + id });
      }
    }
    const channelEnd = Math.round(channel.window_us * 1000);
    if (channel.overflow && channelEnd < duration) {
      const lastEdge = edges.at(-1)?.time ?? -1;
      const time = Math.max(channelEnd, lastEdge + 1);
      edges.push({ time, value: 'x' + id });
    }
    return edges;
  }).sort((a, b) => a.time - b.time);
  const lines = ['$version AI Board $end', '$timescale 1ns $end', '$scope module ai_board $end',
    ...channels.map((channel, index) => '$var wire 1 ' + ids[index] + ' ' +
      captureLetters[captureSlot(channel, index)] + '_Y' + channel.y + ' $end'),
    '$upscope $end', '$enddefinitions $end', '#0', '$dumpvars',
    ...channels.map((channel, index) => (startNs > 0 ? 'x' : channel.initial) + ids[index]), '$end'];
  let time = 0;
  for (const change of changes) {
    if (change.time !== time) {
      time = change.time;
      lines.push('#' + time);
    }
    lines.push(change.value);
  }
  if (time < duration) lines.push('#' + duration);
  return lines.join('\n') + '\n';
}

function localTime(value) {
  return new Date(value).toLocaleString('zh-CN', { hour12: false });
}

function renderHistory() {
  const list = $('history-list');
  list.replaceChildren();
  if (!operationHistory.length) list.append(node('p', '', '尚无记录。运行操作后可在这里查看结果。'));
  for (const item of operationHistory) {
    const line = node('div', 'record');
    line.dataset.result = item.failed ? 'error' : 'ok';
    const detail = node('details');
    detail.append(node('summary', '', '展开结果'), node('pre', 'output inline', JSON.stringify(item.result, null, 2)));
    line.append(node('strong', '', item.label), node('p', '', localTime(item.time)), detail);
    list.append(line);
  }
}

function createAction(label, actionName, index) {
  const button = node('button', 'button compact', label);
  button.type = 'button';
  button.dataset.presetAction = actionName;
  button.dataset.index = index;
  return button;
}

function renderPresets() {
  const list = $('preset-list');
  list.replaceChildren();
  if (!presets.length) list.append(node('p', '', '尚无预设。'));
  for (const [i, preset] of presets.entries()) {
    const line = node('div', 'record');
    const actions = node('div', 'actions');
    actions.append(createAction('载入草稿', 'load', i));
    if (preset.test) actions.append(createAction('运行回归', 'run', i));
    actions.append(createAction('删除', 'remove', i));
    const label = preset.routes.reduce((total, mask) => total + portsIn(mask).length, 0) + ' 处接线' +
      (preset.test ? ' · ' + operationNames[preset.test.params.op] : '');
    line.append(node('strong', '', preset.name), node('p', '', label), actions);
    list.append(line);
  }
}

function loadPreset(preset) {
  draft = preset.routes.slice();
  aliases = { ...preset.aliases };
  store(localStorage, 'aliases', aliases);
  renderMatrix();
}

function showTab() {
  const [name, mode] = location.hash.slice(1).split('/');
  const requested = name === 'spi-slave' ? 'protocol-receive' : name;
  const links = [...document.querySelectorAll('[data-tab]')];
  const tab = links.some((link) => link.dataset.tab === requested) ? requested : 'matrix';
  for (const link of links) {
    const active = link.dataset.tab === tab;
    $(link.dataset.tab).hidden = !active;
    if (active) link.setAttribute('aria-current', 'page');
    else link.removeAttribute('aria-current');
  }
  if (tab === 'protocol-receive') {
    const selected = name === 'spi-slave' ? 'spi' : mode;
    const picker = $('receive-mode');
    if ((selected === 'spi' || Object.hasOwn(receiveModes, selected)) && selected !== picker.value) {
      picker.value = selected;
      updateReceiveMode();
    }
  }
  if (tab === 'trace') renderScope();
  if (tab === 'lua') {
    loadLuaCodeEditor();
    if (key && !luaScripts) readLuaLibrary();
  }
}

function parseMappings(text) {
  const routes = emptyRoutes();
  for (const entry of text.trim().split(/[,，;；、\s]+(?=BBX)/i)) {
    const match = /^BBX(\d{1,2})\s*(?:→|->|:|：|=)\s*Y([0-7])$/i.exec(entry.trim());
    if (!match || Number(match[1]) > 15) throw new Error('映射格式为 BBX02→Y1, BBX02→Y3。');
    const x = Number(match[1]), y = Number(match[2]);
    routes[y] |= 1 << x;
  }
  return routes;
}

function comparison(value) {
  if (Array.isArray(value)) return value.map(comparison);
  if (value && typeof value === 'object') {
    return Object.fromEntries(Object.keys(value).sort().map((key) => [key, comparison(value[key])]));
  }
  return value;
}

async function copyResult(id) {
  const value = $(id).textContent;
  if (navigator.clipboard) {
    await navigator.clipboard.writeText(value);
    return;
  }
  const area = node('textarea');
  area.className = 'clipboard-target';
  area.value = value;
  document.body.append(area);
  area.select();
  try {
    if (!document.execCommand('copy')) throw new Error('请选中结果后手动复制。');
  } finally {
    area.remove();
  }
}

async function waitForFirmware(previous, wanted) {
  const deadline = Date.now() + 65000;
  while (Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 1500));
    try {
      const status = await readStatus();
      if (!status.ota_pending && status.partition !== previous && status.app_sha256 === wanted) {
        if (key) await readState();
        return status;
      }
    } catch {
      // The HTTP service is temporarily unavailable during restart.
    }
  }
  throw new Error('设备暂未回报预期分区和镜像哈希，请读回状态。');
}

async function appHash(file) {
  const header = new Uint8Array(await file.slice(0, 208).arrayBuffer());
  const projectName = new TextDecoder().decode(header.slice(80, 112)).split('\0')[0];
  if (header.length !== 208 || header[0] !== 0xe9 || projectName !== 'ai_board' ||
      header[32] !== 0x32 || header[33] !== 0x54 || header[34] !== 0xcd || header[35] !== 0xab) {
    throw new Error('请选择 ai_board.bin 应用镜像。');
  }
  return Array.from(header.slice(176, 208), (byte) => byte.toString(16).padStart(2, '0')).join('');
}

function i2cFrames(channels, durationUs) {
  const [clock, data] = captureChannelsBySlot(channels);
  if (!clock || !data) return null;
  const coverageUs = Math.min(durationUs, clock.window_us ?? durationUs, data.window_us ?? durationUs);
  const events = [clock, data].flatMap((signal, slot) =>
    signal.edges.map(([time, level]) => ({ time, level, slot }))).sort((a, b) => a.time - b.time);
  const levels = [Number(clock.initial), Number(data.initial)];
  const frames = [];
  let pending = undefined;
  let active = null;

  function commitBit() {
    if (!active || pending === undefined) return;
    active.tail.push(pending);
    pending = undefined;
    if (active.tail.length === 9) {
      const data = active.tail.slice(0, 8);
      active.bytes.push({
        value: data.includes(null) ? null : data.reduce((byte, bit) => (byte << 1) | bit, 0),
        ack: active.tail[8] === null ? null : active.tail[8] === 0,
      });
      active.tail = [];
    }
  }

  function finish(time, boundary) {
    frames.push({ startUs: active.startUs, endUs: time, start: active.start, end: boundary,
      bytes: active.bytes, partialBits: active.tail.length, uncertainEdges: active.uncertainEdges });
    active = null;
  }

  for (let i = 0; i < events.length;) {
    const time = events[i].time;
    if (time > coverageUs) break;
    const [previousClock, previousData] = levels;
    const changes = [0, 0];
    do {
      const { slot, level } = events[i++];
      if (levels[slot] !== level) changes[slot]++;
      levels[slot] = level;
    } while (i < events.length && events[i].time === time);

    const [scl, sda] = levels;
    const dataChanged = sda !== previousData;
    if (scl && previousClock && !changes[0] && changes[1] === 1 && dataChanged) {
      // A START or STOP setup phase may raise SCL without carrying another bit.
      pending = undefined;
      const repeated = !sda && !!active;
      if (active) finish(time, sda ? 'stop' : 'repeated_start');
      if (!sda) active = { startUs: time, start: repeated ? 'repeated_start' : 'start',
        bytes: [], tail: [], uncertainEdges: false };
      continue;
    }
    if (!active) continue;
    if (changes[0] > 1 || changes[1] > 1 || (changes[0] && changes[1])) active.uncertainEdges = true;
    if (!previousClock && scl) pending = changes[0] === 1 && !changes[1] ? sda : null;
    if (previousClock && !scl) commitBit();
  }
  if (active) finish(coverageUs, 'capture_end');
  return { frames, coverageUs, truncated: coverageUs < durationUs };
}

function renderI2C(result) {
  const panel = $('capture-i2c');
  const decoded = i2cFrames(result.channels, captureEndUs(result));
  panel.append(node('h3', '', 'I²C 解码 · A = SCL，B = SDA'));
  if (!decoded) {
    panel.append(node('p', '', 'I²C 需要同时采集 A 和 B。'));
    return;
  }
  if (decoded.truncated) {
    panel.append(node('p', 'decode-warning', '至少一路已截断，只解码共同有效的前 ' + decoded.coverageUs + ' µs。'));
  }
  if (result.sample_start_us > 0) {
    panel.append(node('p', 'decode-warning', '触发后采样启动前的 SCL/SDA 边沿可能缺失。'));
  }
  if (!decoded.frames.length) {
    panel.append(node('p', '', '这段采集里未看到 START。'));
    return;
  }
  const fmt = (value) => value === null ? '??' : '0x' + value.toString(16).toUpperCase().padStart(2, '0');
  const ackText = (ack) => ack === null ? 'ACK 位不明' : ack ? 'ACK' : 'NACK';
  for (const frame of decoded.frames) {
    const [address, ...data] = frame.bytes;
    const start = frame.start === 'start' ? 'START' : '重复 START';
    const tokens = [start];
    if (address) {
      const addressLabel = address.value === null ? '地址/方向不明'
        : (address.value & 0xf8) === 0xf0 ? fmt(address.value) + '（10 位地址头）'
        : fmt(address.value >> 1) + ((address.value & 1) ? ' 读' : ' 写');
      tokens.push(addressLabel + ' ' + ackText(address.ack),
        ...data.map((byte) => fmt(byte.value) + ' ' + ackText(byte.ack)));
    }
    if (frame.end === 'stop') tokens.push('STOP');
    const row = node('div', 'spi-frame');
    row.append(node('small', 'mono', frame.startUs + '–' + frame.endUs + ' µs'),
      node('code', '', tokens.join(' → ')));
    appendScopeFrame(row, result, frame, tokens.slice(1, 3).join(' · ') || start);
    const warnings = [];
    if (frame.end === 'capture_end') warnings.push('采集结束前未看到 STOP 或重复 START');
    if (frame.partialBits) warnings.push('尾部 ' + frame.partialBits + ' 位不足一个字节');
    if (frame.uncertainEdges) warnings.push('SCL/SDA 边沿重叠或同一时间刻度多次跳变');
    if (warnings.length) row.append(node('p', 'decode-warning', warnings.join(' · ')));
    panel.append(row);
  }
}

function spiFrames(channels, mode, bitOrder, durationUs) {
  const signals = captureChannelsBySlot(channels);
  if (signals.some((signal) => !signal)) return null;
  const cpol = (mode >> 1) & 1;
  const cpha = mode & 1;
  const coverageUs = Math.min(durationUs, ...signals.map((signal) => signal.window_us ?? durationUs));
  const events = signals.flatMap((signal, slot) =>
    signal.edges.map(([time, level]) => ({ time, level, slot }))).sort((a, b) => a.time - b.time);
  const levels = signals.map((signal) => Number(signal.initial));
  const frames = [];

  function startFrame(time, missingStart = false) {
    return { startUs: time, missingStart, missingEnd: false, ambiguousClock: false,
      mosi: [], miso: [], bits: 0, partialBits: 0 };
  }

  function packByte(bits, offset) {
    let value = 0;
    for (let bit = 0; bit < 8; bit++) {
      const level = bits[offset + bit];
      if (level === null) return null;
      value |= level << (bitOrder === 'lsb' ? bit : 7 - bit);
    }
    return value;
  }

  let active = levels[0] === 0 ? startFrame(0, true) : null;
  function finishFrame(time, missingEnd = false) {
    active.endUs = time;
    active.missingEnd = missingEnd;
    active.bits = active.mosi.length;
    active.partialBits = active.bits % 8;
    for (const name of ['mosi', 'miso']) {
      const bits = active[name];
      active[name] = Array.from({ length: Math.floor(bits.length / 8) }, (_, i) => packByte(bits, i * 8));
    }
    frames.push(active);
    active = null;
  }

  for (let i = 0; i < events.length;) {
    const time = events[i].time;
    if (time > coverageUs) break;
    const before = levels.slice();
    const changes = [0, 0, 0, 0];
    // Equal timestamps have no stable ordering at the device's time resolution.
    do {
      const { slot, level } = events[i++];
      if (levels[slot] !== level) changes[slot]++;
      levels[slot] = level;
    } while (i < events.length && events[i].time === time);

    const selectChanged = before[0] !== levels[0];
    const clockChanged = before[1] !== levels[1];
    const isSample = clockChanged && (levels[1] !== cpol) !== Boolean(cpha);
    if (selectChanged && !levels[0]) active = startFrame(time);
    if (active && (changes[0] > 1 || changes[1] > 1 || (selectChanged && clockChanged))) {
      active.ambiguousClock = true;
    } else if (active && !levels[0] && isSample) {
      active.mosi.push(changes[2] ? null : levels[2]);
      active.miso.push(changes[3] ? null : levels[3]);
    }
    if (selectChanged && levels[0] && active) finishFrame(time);
  }
  if (active) finishFrame(coverageUs, true);
  return { frames, coverageUs, truncated: coverageUs < durationUs };
}

function renderSPI(result) {
  const panel = $('capture-spi');
  const mode = Number($('capture-spi-mode').value);
  const order = $('capture-spi-order').value;
  const decoded = spiFrames(result.channels, mode, order, captureEndUs(result));
  panel.append(node('h3', '', 'SPI 解码 · Mode ' + mode + ' · ' + order.toUpperCase() + ' 优先'));
  if (!decoded) {
    panel.append(node('p', '', 'SPI 需要四路采集：A = CS（低有效），B = SCK，C = MOSI，D = MISO。'));
    return;
  }
  if (decoded.truncated) {
    panel.append(node('p', 'decode-warning', '至少一路已截断，仅解码各路都有效的前 ' + decoded.coverageUs + ' µs。'));
  }
  if (result.sample_start_us > 0) {
    panel.append(node('p', 'decode-warning', '触发后采样启动前的时钟和数据可能缺失。'));
  }
  if (!decoded.frames.length) {
    panel.append(node('p', '', '这段采集里未看到 CS 低电平。'));
    return;
  }
  const bytesText = (values) => values.map((value) => value === null ? '??'
    : value.toString(16).toUpperCase().padStart(2, '0')).join(' ') || '—';
  decoded.frames.forEach((frame, index) => {
    const row = node('div', 'spi-frame');
    row.append(node('strong', 'mono', '帧 ' + (index + 1) + ' · ' + frame.startUs + '–' + frame.endUs + ' µs'),
      node('small', '', frame.bits + ' 个采样位'));
    appendScopeFrame(row, result, frame, bytesText(frame.mosi) + ' / ' + bytesText(frame.miso));
    for (const name of ['mosi', 'miso']) {
      const line = node('div', 'spi-bytes');
      line.append(node('span', '', name.toUpperCase()), node('code', '', bytesText(frame[name])));
      row.append(line);
    }
    const warnings = [];
    if (frame.missingStart) warnings.push('采集开始时 CS 已为低，字节可能未对齐');
    if (frame.missingEnd) warnings.push('未记录 CS 上升沿');
    if (frame.partialBits) warnings.push('末尾 ' + frame.partialBits + ' 位不足一个字节');
    if (frame.ambiguousClock) warnings.push('CS/SCK 边沿重叠或同一时间刻度发生多次跳变');
    if (frame.mosi.includes(null) || frame.miso.includes(null)) warnings.push('?? 表示数据与采样沿同一时间刻度');
    if (warnings.length) row.append(node('p', 'decode-warning', warnings.join(' · ')));
    panel.append(row);
  });
}

function appendScopeFrame(row, result, frame, label) {
  const start = Math.max(result.sample_start_us ?? 0, frame.startUs);
  const end = frame.endUs;
  if (end <= start) return;
  scopeView.annotations.push({ start, end, label });
  const focus = node('button', 'button compact scope-focus', '定位');
  focus.type = 'button';
  focus.setAttribute('aria-label', '在波形中定位 ' + scopeTimeText(start) + ' 至 ' + scopeTimeText(end));
  focus.addEventListener('click', () => {
    scopeFocusRange(start, end);
    $('scope-canvas').scrollIntoView({ block: 'nearest' });
    $('scope-canvas').focus({ preventScroll: true });
  });
  row.append(focus);
}

function uartScopeFrames(result, channel) {
  if (!(channel.baud > 0) || !channel.hex) return [];
  const sampleStart = result.sample_start_us ?? 0;
  const bit = 1e6 / channel.baud;
  const step = scopeBounds(result).step;
  const tolerance = step <= 0.25 ? 2 * step + bit * 0.12 : 1.5 + bit * 0.18;
  const frames = [];
  let next = scopeEdgesBefore(channel.edges, sampleStart);
  for (const expected of channel.hex.match(/[0-9a-f]{2}/gi) ?? []) {
    let matched = false;
    while (next < channel.edges.length) {
      const startIndex = next++;
      const [start, level] = channel.edges[startIndex];
      if (level) continue;
      const stop = start + 9.5 * bit;
      if (stop >= channel.window_us) break;
      next = scopeEdgesBefore(channel.edges, stop, true);
      if (scopeLevelAt(channel, start + 0.5 * bit) || !scopeLevelAt(channel, stop)) continue;
      let aligned = true;
      for (let i = startIndex + 1; i < next; i++) {
        const elapsed = channel.edges[i][0] - start;
        const offset = Math.round(elapsed / bit);
        if (!offset || Math.abs(elapsed - offset * bit) > tolerance) {
          aligned = false;
          break;
        }
      }
      if (!aligned) continue;
      let byte = 0;
      for (let i = 0; i < 8; i++) byte |= scopeLevelAt(channel, start + (1.5 + i) * bit) << i;
      if (byte !== parseInt(expected, 16)) return [];
      frames.push({ start, end: Math.min(channel.window_us, start + 10 * bit),
        label: expected.toUpperCase(), slot: channel.slot });
      matched = true;
      break;
    }
    if (!matched) return [];
  }
  return frames;
}

function renderDecodes(result) {
  $('capture-i2c').replaceChildren();
  $('capture-spi').replaceChildren();
  scopeView.annotations = [];
  if (!result || result.triggered === false) return;
  const protocol = $('capture-protocol').value;
  if (protocol === 'i2c') renderI2C(result);
  if (protocol === 'spi') renderSPI(result);
  if (protocol === 'uart') scopeView.annotations = result.channels.flatMap((channel) => uartScopeFrames(result, channel));
}

function normalizeCapture(value) {
  const invalid = () => new Error('采集 JSON 格式无效。请选择板卡导出的采集结果。');
  if (!value || !Number.isFinite(value.duration_ms) || value.duration_ms < 0.1 || value.duration_ms > 500 ||
      !Array.isArray(value.channels) || value.channels.length > 4 ||
      (value.triggered === false ? value.channels.length !== 0 : value.channels.length === 0) ||
      (value.trigger != null && !['manual', 'rising', 'falling'].includes(value.trigger)) ||
      (value.trigger_channel != null && ![0, 1, 2, 3].includes(value.trigger_channel))) throw invalid();
  const slots = new Set(), sources = new Set();
  const end = captureEndUs(value);
  const engine = value.engine ?? 'gpio';
  const start = value.sample_start_us ?? 0;
  if (!['gpio', 'dma'].includes(engine) || !Number.isFinite(start) || start < 0 || start > end ||
      (engine === 'dma' && value.triggered !== false &&
       (!Number.isFinite(value.sample_hz) || value.sample_hz <= 0))) throw invalid();
  const isBit = (level) => [0, 1, false, true].includes(level);
  const channels = value.channels.map((channel, index) => {
    if (!channel || typeof channel !== 'object') throw invalid();
    const slot = captureSlot(channel, index);
    const y = channel.y;
    const window = channel.window_us ?? end;
    if (![0, 1, 2, 3].includes(slot) || slots.has(slot) || !Number.isInteger(y) || y < 0 || y >= 8 ||
        sources.has(y) || !isBit(channel.initial) || !Number.isFinite(window) || window < start ||
        window > end || !Array.isArray(channel.edges) || channel.edges.length > 2048) throw invalid();
    let previous = -1;
    const edges = channel.edges.map((edge) => {
      if (!Array.isArray(edge) || edge.length !== 2 || !Number.isFinite(edge[0]) ||
          edge[0] < 0 || edge[0] < previous || edge[0] > window || !isBit(edge[1])) throw invalid();
      previous = edge[0];
      return [edge[0], Number(edge[1])];
    });
    slots.add(slot);
    sources.add(y);
    return { slot, y, initial: Number(channel.initial), edges, window_us: window, edge_count: edges.length,
      overflow: channel.overflow === true || window < end,
      ...(Number.isInteger(channel.baud) ? { baud: channel.baud } : {}),
      ...(typeof channel.hex === 'string' ? { hex: channel.hex.slice(0, 256) } : {}),
      ...(Number.isInteger(channel.framing_errors) ? { framing_errors: channel.framing_errors } : {}) };
  });
  return { duration_ms: value.duration_ms, trigger: value.trigger ?? 'manual',
    trigger_channel: value.trigger_channel ?? 0, triggered: value.triggered !== false, engine,
    ...(engine === 'dma' ? { sample_hz: value.sample_hz ?? 0, sample_start_us: start } : {}),
    channels: channels.sort((a, b) => a.slot - b.slot) };
}

$('route-grid').addEventListener('click', (event) => {
  const point = event.target.closest('.crosspoint');
  const yLabel = event.target.closest('[data-y-label]');
  if (point) togglePoint(Number(point.dataset.x), Number(point.dataset.y));
  if (yLabel) {
    selectedY = Number(yLabel.dataset.yLabel);
    renderMatrix();
    $('route-grid').querySelector('[data-y-label="' + selectedY + '"]').focus();
  }
});

$('route-grid').addEventListener('keydown', (event) => {
  if (!event.target.matches('.crosspoint')) return;
  const delta = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[event.key];
  if (!delta) return;
  event.preventDefault();
  selectedX = Math.min(15, Math.max(0, selectedX + delta[0]));
  selectedY = Math.min(7, Math.max(0, selectedY + delta[1]));
  renderMatrix();
  moveFocus();
});

document.addEventListener('submit', (event) => {
  if (!aiControlActive || !event.target.matches('form[data-board-edit]')) return;
  event.preventDefault();
  event.stopImmediatePropagation();
  notice(aiReadOnlyMessage, 'warning');
}, true);

document.addEventListener('click', (event) => {
  if (!aiControlActive || !event.target.closest('button[data-board-edit], [data-board-edit] button')) return;
  event.preventDefault();
  event.stopImmediatePropagation();
}, true);

$('channel-picker').addEventListener('click', (event) => {
  const button = event.target.closest('[data-channel]');
  if (!button) return;
  selectedY = Number(button.dataset.channel);
  renderMatrix();
  $('channel-picker').querySelector('[data-channel="' + selectedY + '"]').focus();
});

$('alias-form').addEventListener('change', (event) => {
  if (event.target.name !== 'port') return;
  selectedX = Number(event.target.value);
  renderMatrix();
  $('alias-form').elements.label.focus();
});

$('alias-form').addEventListener('submit', (event) => {
  event.preventDefault();
  const { port, label } = formParams(event.target);
  if (label.trim()) aliases[port] = label.trim();
  else delete aliases[port];
  store(localStorage, 'aliases', aliases);
  renderMatrix();
  notice('已保存 ' + bbx(Number(port)) + ' 的线名。', 'success');
});

$('refresh').addEventListener('click', () => {
  action('读回板卡状态', async () => {
    await readStatus();
    const state = await readState();
    await refreshAiMonitor();
    return state;
  }, '已读回设备状态。', false);
});

$('route-text-form').addEventListener('submit', (event) => {
  event.preventDefault();
  action('解析接线映射', () => {
    draft = parseMappings(formParams(event.target).mappings);
    renderMatrix();
  }, '接线已填入草稿，请检查后应用。', false);
});

$('draft-clear').addEventListener('click', () => {
  draft = emptyRoutes();
  renderMatrix();
  notice('草稿已清空，点击应用接线后才会更新硬件。', 'warning');
});

$('apply-routes').addEventListener('click', () =>
  action('应用接线', () => applyRoutes(draft.slice()), '已接通交叉点，各 Y 已恢复高阻输入。'));

$('gpio-form').elements.mode.addEventListener('change', updatePwmFields);
$('gpio-form').addEventListener('submit', (event) => {
  event.preventDefault();
  const params = { op: 'gpio', y: selectedY, ...formParams(event.target) };
  action('设置 Y' + params.y, async () => {
    requireApplied();
    if (driveModes.has(params.mode)) {
      const group = signalGroups(live.routes).find(({ channels }) => channels.includes(params.y));
      if (portsIn(group.ports).length !== 1) {
        throw new Error('GPIO 输出所在的信号组须恰好连接一个 BBX。');
      }
      if (group.channels.some((y) => y !== params.y && driveModes.has(live.modes[y]))) {
        throw new Error('同一信号组已有 GPIO 输出，请先将它设为高阻输入。');
      }
    }
    if (params.mode === 'pwm' && live.modes.some((mode, y) => mode === 'pwm' && y !== params.y)) {
      throw new Error('同一时间仅支持一条 PWM，请先停止另一条通道的 PWM。');
    }
    const state = await workbench(params);
    acceptState(state);
    return { y: params.y, mode: state.modes[params.y], level: state.levels[params.y] };
  });
});

$('out-form').addEventListener('submit', (event) => {
  event.preventDefault();
  const { mode } = formParams(event.target);
  action('设置 OUT 供电', async () => {
    const state = await workbench({ op: 'power', mode });
    acceptState(state, true);
    return { out_mode: state.out_mode };
  });
});

$('protocol-select').addEventListener('change', renderProtocol);
$('protocol-form').addEventListener('submit', (event) => {
  event.preventDefault();
  const op = $('protocol-select').value;
  const params = { op, ...formParams(event.target) };
  resetOutput('protocol-result', '等待设备响应…');
  action(operationNames[op], () => runProtocol(params)).then((result) => {
    if (!result) resetOutput('protocol-result', '操作未完成。');
  });
});

$('loopback-button').addEventListener('click', async () => {
  if (!await confirmEmptyBoard('运行板内 UART 回环')) return;
  action(operationNames.uart_loopback, async () => {
    const result = await workbench({ op: 'uart_loopback', confirm_empty: 'yes' });
    showOutput('protocol-result', result);
    if (!result.passed) throw new Error('板内 UART 回环未通过。');
    return result;
  });
});

const spiSlaveForm = $('spi-slave-form');
for (const signal of spiSlaveSignals) spiSlaveForm.elements[signal].addEventListener('change', updateSpiSlaveRoutes);
spiSlaveForm.addEventListener('submit', (event) => {
  event.preventDefault();
  runSpiSlave($('spi-slave-start'), '启动 SPI 从机采集', '板卡正在建立本批采集，等待主机拉低 CS…',
    () => workbench(spiSlaveParams(event.target)));
});

const spiSlaveBench = $('spi-slave-bench');
spiSlaveBench.elements.phase.addEventListener('change', () => {
  const gap = spiSlaveBench.elements.phase.value === 'gap';
  $('spi-slave-bench-gap').hidden = !gap;
  spiSlaveBench.elements.gap_ns.disabled = !gap;
  spiSlaveBench.elements.hz.max = gap ? '3333334' : '20000000';
});
spiSlaveBench.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!await confirmEmptyBoard('运行板内 SPI 压测')) return;
  runSpiSlave($('spi-slave-bench-start'), '运行板内 SPI 压测', '正在通过 CH446Q 执行板内压测…', () => {
    const { phase, hz, gap_ns } = formParams(spiSlaveBench);
    return workbench({ op: 'spi_slave_bench', confirm_empty: 'yes', phase, hz, ...(gap_ns && { gap_ns }) });
  });
});

$('receive-mode').addEventListener('change', (event) => {
  updateReceiveMode();
  window.history.replaceState(null, '', '#protocol-receive/' + event.target.value);
});
$('i2c-receive-kind').addEventListener('change', updateI2cReceiveKind);
$('i2c-auto-increment').addEventListener('change', updateI2cReceiveKind);
$('i2c-add-register').addEventListener('click', () => addI2cRegister()?.focus());
for (const [mode, { signals }] of Object.entries(receiveModes)) {
  const form = $(mode + '-receive-form');
  for (const signal of signals) form.elements[signal].addEventListener('change', updateReceiveRoutes);
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    runProtocolReceive(mode, $(mode + '-receive-start'), false, () => workbench(receiveParams(mode, form)));
  });
}

const receiveBenchForm = $('receive-bench-form');
receiveBenchForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  const mode = $('receive-mode').value;
  if (!Object.hasOwn(receiveModes, mode)) return;
  if (!await confirmEmptyBoard('运行' + receiveModes[mode].name + '板内压测')) return;
  runProtocolReceive(mode, $('receive-bench-start'), true, () => {
    const rate = receiveBenchForm.elements.rate.value;
    if (mode === 'i2c' && $('i2c-receive-kind').value === 'register') {
      const i2cForm = $('i2c-receive-form');
      return workbench({ op: 'i2c_register_table_bench', confirm_empty: 'yes', rate,
        address: i2cAddress(i2cForm.elements.address.value), ...i2cRegisterTable(true) });
    }
    return workbench({ op: 'protocol_receive_bench', confirm_empty: 'yes', mode, rate });
  });
});

function updateCaptureTrigger() {
  const fields = $('capture-form').elements;
  const manual = fields.trigger.value === 'manual';
  captureFields.forEach((field, slot) => {
    const option = fields.trigger_channel.options[slot];
    option.textContent = '通道 ' + captureLabel(slot);
    option.disabled = fields[field].value === '-1';
  });
  if (fields.trigger_channel.selectedOptions[0]?.disabled) fields.trigger_channel.value = '0';
  fields.trigger_channel.disabled = manual;
  fields.wait_ms.disabled = manual;
  $('capture-trigger-fields').hidden = manual;
  $('capture-trigger-hint').hidden = manual;
  $('capture-trigger-hint').textContent = fields.engine.value === 'dma'
    ? '指定边沿为时间零点，硬件采样随后启动。快速帧请用手动采集预留启动时间。'
    : '指定边沿为时间零点。等待期间的边沿不计入记录。';
  $('capture-start').textContent = manual ? '开始采集' : '等待触发';
}

function updateCaptureEngine() {
  const fields = $('capture-form').elements;
  const dma = fields.engine.value === 'dma';
  $('capture-dma-settings').hidden = !dma;
  $('capture-gpio-window').hidden = dma;
  fields.ms.disabled = dma;
  fields.sample_hz.disabled = !dma;
  fields.window_us.disabled = !dma;
  const sampleRate = Number(fields.sample_hz.value);
  const maxWindow = Math.floor(65536e6 / sampleRate);
  const maxBaud = dma ? Math.min(4000000, Math.floor(sampleRate / 4)) : 115200;
  fields.baud.max = String(maxBaud);
  $('capture-baud-limit').textContent = '0 表示自动，当前采集档可指定至 ' + maxBaud.toLocaleString('zh-CN') + ' baud';
  fields.window_us.max = String(maxWindow);
  if (Number(fields.window_us.value) > maxWindow) fields.window_us.value = String(maxWindow);
  $('capture-dma-limit').textContent = '最长 ' + maxWindow + ' µs · 65,536 组样本';
  updateCaptureTrigger();
}

function updateCaptureDecode() {
  const protocol = $('capture-protocol').value;
  $('capture-spi-settings').hidden = protocol !== 'spi';
  $('capture-uart-settings').hidden = protocol !== 'uart';
  $('capture-form').elements.baud.disabled = protocol !== 'uart';
  captureFields.forEach((field, slot) => {
    document.getElementById('capture-label-' + field).textContent = '通道 ' + captureLabel(slot);
  });
  updateCaptureTrigger();
  if (lastCapture) renderCapture(lastCapture);
  syncBoardEditing();
}

$('capture-form').elements.trigger.addEventListener('change', updateCaptureTrigger);
$('capture-form').elements.engine.addEventListener('change', updateCaptureEngine);
$('capture-form').elements.sample_hz.addEventListener('change', updateCaptureEngine);
for (const field of captureFields) $('capture-form').elements[field].addEventListener('change', updateCaptureTrigger);
$('capture-protocol').addEventListener('change', updateCaptureDecode);
for (const id of ['capture-spi-mode', 'capture-spi-order']) {
  $(id).addEventListener('change', () => {
    renderDecodes(lastCapture);
    renderScope();
  });
}

$('capture-form').addEventListener('submit', (event) => {
  event.preventDefault();
  if (working) return;
  const params = { op: 'capture', ...formParams(event.target) };
  const manual = params.trigger === 'manual';
  let sampling = false;
  action('信号捕获', async () => {
    requireApplied();
    const channels = captureFields.map((field) => Number(params[field])).filter((y) => y >= 0);
    if (new Set(channels).size !== channels.length) throw new Error('各采集通道必须选用不同的 Y。');
    const protocol = $('capture-protocol').value;
    if (protocol === 'spi' && channels.length !== 4) throw new Error('SPI 解码需要同时选择四路通道。');
    if (protocol === 'i2c' && Number(params.b) < 0) throw new Error('I²C 解码需要选择 A 和 B 两路。');
    if (channels.some((y) => !live.routes[y])) throw new Error('请先把外部信号接入所选 Y。');
    if (channels.some((y) => live.modes[y] !== 'input')) throw new Error('旁听通道必须处于高阻输入。');
    if (params.baud === '0' || params.baud === '') delete params.baud;
    sampling = true;
    $('capture-start').disabled = true;
    lastCapture = null;
    $('capture-vcd').disabled = true;
    $('capture-channels').replaceChildren();
    renderDecodes(null);
    const waiting = manual ? '正在采集，结束后读取数据'
      : '正在等待通道 ' + captureLabel(Number(params.trigger_channel)) +
        (params.trigger === 'rising' ? ' 上升沿' : ' 下降沿');
    notice(waiting + '…', 'pending');
    $('capture-summary').textContent = waiting;
    $('scope-empty').textContent = waiting;
    $('scope-empty').hidden = false;
    resetOutput('capture-result', waiting);
    renderScope();
    const result = normalizeCapture(await workbench(params));
    renderCapture(result);
    return result;
  }, manual ? '采集完成，数据已读回。' : '触发采集结束。').then((result) => {
    if (result?.triggered === false) {
      notice('等待超时，未检测到指定边沿。', 'warning');
    } else if (!result && sampling) {
      $('capture-summary').textContent = '采集未完成';
      $('scope-empty').textContent = '采集未完成';
      resetOutput('capture-result', '采集未完成。');
    }
  }).finally(() => {
    $('capture-start').disabled = false;
    syncBoardEditing();
  });
});

$('capture-import').addEventListener('click', () => {
  if (!working) $('capture-import-file').click();
});
$('capture-import-file').addEventListener('change', (event) => {
  const file = event.target.files[0];
  event.target.value = '';
  if (!file) return;
  action('导入采集', async () => {
    if (file.size > 262144) throw new Error('采集 JSON 文件不能超过 256 KiB。');
    renderCapture(normalizeCapture(JSON.parse(await file.text())));
  }, '已载入采集记录。', false);
});

async function readCaptureFixture(op, protocol) {
  const result = normalizeCapture(await workbench({ op, confirm_empty: 'yes' }));
  $('capture-protocol').value = protocol;
  if (protocol === 'spi') {
    $('capture-spi-mode').value = '0';
    $('capture-spi-order').value = 'msb';
  }
  lastCapture = null;
  updateCaptureDecode();
  renderCapture(result);
  return result;
}

async function captureFixtureI2C(probe) {
  const label = probe ? 'I²C 主机采集' : 'I²C 参考波形';
  if (!await confirmEmptyBoard(label)) return;
  action(label, async () => {
    const op = probe ? 'i2c_host_capture' : 'i2c_capture_loopback';
    const result = await readCaptureFixture(op, 'i2c');
    const decoded = i2cFrames(result.channels, result.duration_ms * 1000);
    const expected = probe
      ? [{ start: 'start', end: 'stop', bytes: [{ value: 0x90, ack: false }] }]
      : [{ start: 'start', end: 'repeated_start', bytes: [{ value: 0x90, ack: true }, { value: 0x01, ack: true }] },
        { start: 'repeated_start', end: 'stop', bytes: [{ value: 0x91, ack: true }, { value: 0xa5, ack: false }] }];
    const frames = decoded?.frames ?? [];
    if (frames.length !== expected.length || !frames.every((frame, index) =>
      frame.start === expected[index].start && frame.end === expected[index].end &&
      !frame.partialBits && !frame.uncertainEdges &&
      JSON.stringify(frame.bytes) === JSON.stringify(expected[index].bytes))) {
      throw new Error(label + '与预期不符，请查看波形。');
    }
    return { frames };
  }, probe ? '已抓到 I²C 主机访问 0x48，地址 NACK 后发送 STOP。' : 'I²C 参考波形校验通过。');
}

async function captureFixtureSPI(host) {
  const label = host ? 'SPI 主机回环采集' : 'SPI 参考波形';
  if (!await confirmEmptyBoard(label)) return;
  action(label, async () => {
    const op = host ? 'spi_host_capture' : 'spi_capture_loopback';
    const result = await readCaptureFixture(op, 'spi');
    const decoded = spiFrames(result.channels, 0, 'msb', result.duration_ms * 1000);
    const frame = decoded?.frames[0];
    const expectedMosi = '159,0,0,165';
    const expectedMiso = host ? expectedMosi : '255,239,64,22';
    if (decoded?.frames.length !== 1 || frame.bits !== 32 || frame.missingStart || frame.missingEnd ||
        frame.ambiguousClock || frame.mosi.join(',') !== expectedMosi || frame.miso.join(',') !== expectedMiso) {
      throw new Error(label + '与预期不符，请查看波形。');
    }
    return frame;
  }, host ? 'SPI 主机回环通过，MOSI 和 MISO 都解码为 9F 00 00 A5。' : 'SPI 参考波形校验通过。');
}

$('i2c-host-button').addEventListener('click', () => captureFixtureI2C(true));
$('spi-host-button').addEventListener('click', () => captureFixtureSPI(true));
$('i2c-loopback-button').addEventListener('click', () => captureFixtureI2C(false));
$('spi-loopback-button').addEventListener('click', () => captureFixtureSPI(false));

$('capture-vcd').addEventListener('click', () => {
  if (!lastCapture?.channels.length || lastCapture.triggered === false) return;
  const url = URL.createObjectURL(new Blob([captureVcd(lastCapture)], { type: 'text/plain' }));
  const link = node('a');
  link.href = url;
  link.download = 'ai-board-capture-' + new Date().toISOString().replace(/[:.]/g, '-') + '.vcd';
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});

$('export-history').addEventListener('click', () => {
  const data = JSON.stringify(operationHistory, null, 2);
  const url = URL.createObjectURL(new Blob([data], { type: 'application/json' }));
  const link = node('a');
  link.href = url;
  link.download = 'ai-board-tests-' + new Date().toISOString().slice(0, 10) + '.json';
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});

$('preset-form').addEventListener('submit', (event) => {
  event.preventDefault();
  const { name, with_test } = formParams(event.target);
  const cleanName = name.trim();
  if (!cleanName) return;
  if (with_test && (!lastTest || !lastTest.routes.every((mask, index) => mask === draft[index]))) {
    notice('请先在同一组接线上成功执行一次协议操作。', 'warning');
    return;
  }
  presets.unshift({ name: cleanName, routes: draft.slice(), aliases: { ...aliases }, test: with_test ? lastTest : null });
  presets = presets.slice(0, 24);
  store(localStorage, 'presets', presets);
  renderPresets();
  event.target.reset();
  notice('已保存预设 ' + cleanName + '。', 'success');
});

$('preset-list').addEventListener('click', async (event) => {
  const button = event.target.closest('[data-preset-action]');
  if (!button) return;
  const index = Number(button.dataset.index);
  const preset = presets[index];
  if (!preset) return;
  if (button.dataset.presetAction === 'remove') {
    presets.splice(index, 1);
    store(localStorage, 'presets', presets);
    renderPresets();
  }
  if (button.dataset.presetAction === 'load') {
    loadPreset(preset);
    historyReplace('matrix');
    notice('已载入草稿，请核对物理线序后应用。', 'warning');
  }
  if (button.dataset.presetAction === 'run') {
    if (!await confirmHardwareAction('运行回归测试', '测试将应用保存的接线并发送协议请求。请确认外部线序和信号方向正确。')) return;
    action('运行回归 ' + preset.name, async () => {
      await applyRoutes(preset.routes);
      const actual = await runProtocol({ ...preset.test.params });
      const passed = JSON.stringify(comparison(actual)) === JSON.stringify(comparison(preset.test.expected));
      const result = { passed, expected: preset.test.expected, actual };
      if (!passed) {
        record('回归未通过 ' + preset.name, result, true);
        throw new Error('回归结果与预期不同，详情见测试记录。');
      }
      return result;
    }, '回归结果与预期一致。');
  }
});

$('arm-button').addEventListener('click', async () => {
  if (!await confirmHardwareAction('启用矩阵', '请确认 DTR→BOOT 飞线已物理断开。')) return;
  action('启用矩阵', async () => {
    const result = await rpc('/api/matrix/arm', { method: 'POST', headers: { 'X-DTR-Disconnected': 'yes' } });
    await readState();
    return result;
  });
});

$('selftest-button').addEventListener('click', async () => {
  if (!await confirmEmptyBoard('运行矩阵自检')) return;
  action('矩阵自检', async () => {
    const result = await rpc('/api/matrix/selftest', { method: 'POST' });
    $('selftest-result').textContent = result.passed + ' / ' + result.total + ' 通过 · ' + result.failed + ' 项失败';
    await readState();
    if (result.failed) {
      record('矩阵自检未通过', result, true);
      throw new Error('矩阵自检发现失败项，详情见测试记录。');
    }
    return result;
  }, '矩阵自检通过。');
});

$('wifi-form').addEventListener('submit', (event) => {
  event.preventDefault();
  const { ssid, password } = formParams(event.target);
  action('保存 Wi-Fi', async () => {
    const result = await rpc('/api/wifi', { method: 'POST', body: new URLSearchParams({ ssid, password }) });
    event.target.elements.password.value = '';
    return result;
  }, '配网已保存。板卡重启后可从配网 AP 的 /setup 查看新地址。', false);
});

$('ota-form').addEventListener('submit', (event) => {
  event.preventDefault();
  const file = event.target.elements.binary.files[0];
  if (!file) return;
  action('上传 OTA', async () => {
    const expectedHash = await appHash(file);
    const previous = await readStatus();
    $('ota-result').textContent = '正在上传 ' + file.name + '…';
    await rpc('/api/ota', { method: 'POST', body: file, headers: { 'Content-Type': 'application/octet-stream' } });
    $('ota-result').textContent = '镜像已上传，等待重启与哈希验证…';
    const status = await waitForFirmware(previous.partition, expectedHash);
    $('ota-result').textContent = '验证成功 · ' + status.partition;
    return { partition: status.partition, sha256: status.app_sha256 };
  }, 'OTA 已切换分区，运行镜像与上传文件一致。').then((result) => {
    if (!result) $('ota-result').textContent = '更新尚未验证。';
  });
});

$('diagnostics-button').addEventListener('click', () => {
  action('读取诊断', async () => {
    const result = await rpc('/api/diagnostics');
    showOutput('diagnostics-result', result);
    return result;
  }, '已更新诊断。', false);
});

$('lua-refresh').addEventListener('click', readLuaLibrary);
$('lua-file-list').addEventListener('keydown', (event) => {
  const files = [...$('lua-file-list').querySelectorAll('.lua-file')];
  const index = files.indexOf(event.target);
  if (index < 0) return;
  let next;
  switch (event.key) {
    case 'ArrowUp': next = Math.max(0, index - 1); break;
    case 'ArrowDown': next = Math.min(index + 1, files.length - 1); break;
    case 'Home': next = 0; break;
    case 'End': next = files.length - 1; break;
    default: return;
  }
  event.preventDefault();
  files[next].focus();
});
$('lua-name').addEventListener('input', syncLuaEditing);
$('lua-source').addEventListener('input', () => {
  setLuaErrorLocation(null);
  syncLuaEditing();
});
$('lua-find').addEventListener('click', () => luaCodeEditor?.execCommand('find'));
$('lua-error-jump').addEventListener('click', () => {
  if (luaErrorLine === null) return;
  if (luaCodeEditor) {
    luaCodeEditor.gotoLine(luaErrorLine, 0);
    luaCodeEditor.focus();
    return;
  }
  const source = $('lua-source'), lines = source.value.split('\n');
  const offset = lines.slice(0, luaErrorLine - 1).join('\n').length + (luaErrorLine > 1 ? 1 : 0);
  source.focus();
  source.setSelectionRange(offset, offset + lines[luaErrorLine - 1].length);
  source.scrollTop = (luaErrorLine - 1) * parseFloat(getComputedStyle(source).lineHeight);
});

$('lua-editor').addEventListener('keydown', (event) => {
  if (event.isComposing || !(event.ctrlKey || event.metaKey) || event.altKey || event.shiftKey) return;
  const key = event.key.toLowerCase();
  const button = key === 's' ? $('lua-save') : key === 'enter' ? $('lua-run') : null;
  if (!button) return;
  event.preventDefault();
  event.stopPropagation();
  if (!event.repeat && !button.disabled) button.click();
}, true);

$('lua-new').addEventListener('click', async () => {
  if (luaDraftChanged() && !await confirmAction('新建 Lua 脚本', '放弃当前草稿并新建脚本？')) return;
  if (working || aiControlActive) return;
  setLuaDocument('', '');
  $('lua-name').focus();
});

$('lua-editor').addEventListener('submit', (event) => {
  event.preventDefault();
  if ($('lua-save').disabled) return;
  action('保存 Lua 脚本', async () => {
    const name = $('lua-name').value;
    const source = luaSource();
    const headers = savedLua?.name === name ? { 'If-Match': savedLua.etag } : { 'If-None-Match': '*' };
    const requestKey = key;
    const result = await sendLua({ op: 'save', name }, source, headers);
    if (key !== requestKey) return;
    $('lua-name').value = result.name;
    savedLua = { name: result.name, source, etag: result.etag };
    const scripts = (luaScripts ?? []).filter((script) => script.name !== result.name);
    scripts.push({ name: result.name, bytes: result.bytes });
    luaScripts = scripts.sort((a, b) => a.name.localeCompare(b.name));
    renderLuaLibrary();
  }, 'Lua 脚本已保存到板卡。', false);
});

$('lua-run').addEventListener('click', () => {
  action('运行 Lua', async () => {
    const timeout = $('lua-timeout').valueAsNumber;
    if (!Number.isInteger(timeout) || timeout < 100 || timeout > 10000) {
      throw new Error('运行超时可设为 100–10000 ms 的整数。');
    }
    const requestKey = key;
    const result = await sendLua({ op: 'run', timeout_ms: timeout }, luaSource());
    if (key !== requestKey) return;
    renderLuaResult(result);
    await readState().catch(() => {});
    if (!result.ok) throw new Error('Lua 脚本执行失败，请查看运行结果。');
  }, 'Lua 脚本执行完成。', false);
});

$('open-key').addEventListener('click', () => {
  $('board-key').value = '';
  if (!$('key-dialog').open) $('key-dialog').showModal();
});
$('close-key').addEventListener('click', () => $('key-dialog').close());
$('clear-key').addEventListener('click', () => {
  key = '';
  renderAiResult({ revision: 0, op: null, result: null });
  clearLuaSession();
  sessionStorage.removeItem('ai-board.key');
  $('board-key').value = '';
  $('key-dialog').close();
  live.armed = false;
  live.levels.fill(null);
  hasState = false;
  renderMatrix();
  notice('本会话的设备密钥已清除。');
  readStatus().catch(() => {});
});
$('key-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const previousKey = key;
  key = event.target.elements.key.value.trim();
  try {
    await readStatus();
    await readState();
    await refreshAiMonitor().catch(() => {});
    store(sessionStorage, 'key', key);
    if (previousKey && previousKey !== key) clearLuaSession();
    event.target.reset();
    $('key-dialog').close();
    notice(live.armed ? '已连接设备。' : '已连接，请在设备管理中启用矩阵。', 'success');
    if (!$('lua').hidden && !luaScripts) readLuaLibrary();
  } catch (error) {
    key = previousKey;
    notice(error.message, 'error');
    $('key-dialog').close();
  }
});

function historyReplace(tab) {
  window.history.pushState(null, '', '#' + tab);
  showTab();
}

for (const tab of document.querySelectorAll('[data-tab]')) {
  tab.addEventListener('click', (event) => {
    event.preventDefault();
    historyReplace(tab.dataset.tab);
  });
}
for (const button of document.querySelectorAll('[data-copy]')) {
  button.addEventListener('click', async () => {
    try {
      await copyResult(button.dataset.copy);
      notice('结果已复制。', 'success');
    } catch (error) {
      notice(error.message, 'error');
    }
  });
}
for (const select of document.querySelectorAll('select[data-channels]')) choiceChannels(select);
spiSlaveForm.elements.cs.value = '0';
spiSlaveForm.elements.sck.value = '1';
spiSlaveForm.elements.mosi.value = '2';
$('i2c-receive-form').elements.scl.value = '1';
addI2cRegister('0x10', 'DE AD BE EF');
addI2cRegister('0x11', '00 7F');
addI2cRegister('0x20', 'A5 5A 12 80');
updateReceiveMode();
updateCaptureEngine();
updateCaptureDecode();
for (const x of range(16)) $('alias-port').append(new Option(bbx(x), x));
window.addEventListener('hashchange', showTab);
window.addEventListener('popstate', showTab);
window.addEventListener('resize', () => {
  if (!$('trace').hidden) renderScope();
});

async function initialRefresh() {
  try {
    await readStatus();
    await refreshAiMonitor(true);
  } catch (error) {
    notice(error.message, 'error');
  }
}
renderMatrix();
renderProtocol();
renderPresets();
renderHistory();
setupScopeControls();
showTab();
$('notice-close').addEventListener('click', dismissNotice);
initialRefresh();
setInterval(() => {
  if (!working && !$('key-dialog').open) refreshAiMonitor().catch(() => {});
}, 2000);
setInterval(() => {
  if (!working && !$('key-dialog').open) readStatus().catch(() => {});
}, 10000);
