'use strict';
/** @file 验证范围同步、版本过滤、取消清理与持久化生成配置。 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const Module = require('node:module');
const { LspClient } = require('../src/lspClient');
const engine = require('../src/parseEngine');
const { GenerationManager, resolveProfile } = require('../src/generation');
const { createVscodeStub } = require('./extension-smoke');
const root = path.resolve(__dirname, '../..');
const exe = process.env.PARSE_ENGINE || path.join(root, 'build/bin', process.platform === 'win32' ? 'ParseEngine.exe' : 'ParseEngine');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

/** @brief 等待异步状态，超时给出可定位的断言。 */
async function until(test) {
  for (let i = 0; i < 300; ++i) { if (test()) return; await sleep(10); }
  assert.ok(test(), '异步状态未在期限内到达');
}

/** @brief 最小取消令牌，同时记录监听器是否正确释放。 */
function cancellation() {
  const callbacks = new Set();
  return { isCancellationRequested: false, callbacks,
    onCancellationRequested(fn) { callbacks.add(fn); return { dispose: () => callbacks.delete(fn) }; },
    cancel() { this.isCancellationRequested = true; for (const fn of [...callbacks]) fn(); } };
}

/** @brief 验证真实内嵌服务器的范围编辑与文档版本。 */
async function protocol() {
  const client = new LspClient(exe, ['lsp', '--stdio']);
  try {
    await client.start(); const initialized = await client.initialize();
    assert.equal(initialized.capabilities.textDocumentSync.change, 2);
    const events = []; client.on('diagnostics', e => events.push(e));
    const uri = 'untitled:unicode-p0';
    const text = '//🙂\r\n%token A "a"\r\nstart := A;\r\n';
    client.notify('textDocument/didOpen', { textDocument: { uri, text, version: 1, languageId: 'gram' } });
    await until(() => events.some(e => e.version === 1));
    client.notify('textDocument/didChange', { textDocument: { uri, version: 2 }, contentChanges: [
      { range: { start: { line: 0, character: 2 }, end: { line: 0, character: 4 } }, text: '中文' },
      { range: { start: { line: 1, character: 7 }, end: { line: 1, character: 8 } }, text: 'B' },
      { range: { start: { line: 2, character: 9 }, end: { line: 2, character: 10 } }, text: 'B' },
    ] });
    const params = { textDocument: { uri }, position: { line: 2, character: 9 } };
    const definition = await client.request('textDocument/definition', params);
    assert.equal(definition[0].range.start.line, 1); assert.equal(definition[0].range.start.character, 7);
    const formatted = await client.request('textDocument/formatting', params);
    assert.match(formatted[0].newText, /中文/);
    client.notify('textDocument/didChange', { textDocument: { uri, version: 1 }, contentChanges: [{ text: 'BROKEN' }] });
    client.notify('textDocument/didChange', { textDocument: { uri, version: 3 }, contentChanges: [{
      range: { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } }, text: ' ' }] });
    assert.deepEqual(await client.request('textDocument/definition', params), definition);
    client.notify('textDocument/didChange', { textDocument: { uri, version: 4 }, contentChanges: [{
      range: { start: { line: 0, character: 0 }, end: { line: 1, character: 0 } }, text: '' }] });
    const crossLine = await client.request('textDocument/definition', { ...params, position: { line: 1, character: 9 } });
    assert.equal(crossLine[0].range.start.line, 0);
    client.notify('textDocument/didClose', { textDocument: { uri } });
    const before = events.length;
    client.notify('textDocument/didOpen', { textDocument: { uri, text, version: 1, languageId: 'gram' } });
    await until(() => events.slice(before).some(e => e.version === 1));
    // 整份文本回退和多个连续范围变更都须按顺序应用，不能只保留最后一段。
    for (let version = 2; version < 20; ++version)
      client.notify('textDocument/didChange', { textDocument: { uri, version }, contentChanges: [{
        range: { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } }, text: ' ' }] });
    await client.request('textDocument/documentSymbol', params);
    await until(() => events.some(e => e.version === 19));
    assert.equal(client.pending.size, 0);
  } finally { await client.stop(); }
}

/** @brief 模拟迟到的响应，验证取消不等待服务器并释放客户端订阅。 */
async function cancelClient() {
  const client = new LspClient('unused');
  client.process = { exitCode: null };
  const sent = []; client._send = message => sent.push(message);
  const token = cancellation();
  const promise = client.request('textDocument/hover', {}, token);
  const rejected = assert.rejects(promise, { name: 'AbortError' }); token.cancel(); await rejected;
  assert.equal(client.pending.size, 0); assert.equal(token.callbacks.size, 0);
  assert.equal(sent[1].method, '$/cancelRequest');
  client._handle({ id: sent[0].id, result: '迟到的旧响应' });
  const cancelled = cancellation(); cancelled.cancel();
  await assert.rejects(client.request('unused', {}, cancelled), { name: 'AbortError' });
  assert.equal(sent.length, 2);
}

/** @brief 验证真实生成任务的取消、目录互斥和缓存参数。 */
async function generation(directory) {
  const manager = new GenerationManager();
  const out = path.join(directory, 'shared out');
  const options = { grammarFile: path.join(root, 'example/c89_subset.gram'), outputDir: out, mode: 'lalr', lang: 'cpp17', incremental: true };
  const first = manager.run(exe, options);
  assert.equal((await manager.run(exe, { ...options, outputDir: path.join(out, '..', 'shared out') })).busy, true);
  manager.dispose(); assert.equal((await first).cancelled, true); assert.equal(manager.tasks.size, 0);
  assert.throws(() => resolveProfile({ mode: 'lalr', lang: 'js' }, options.grammarFile, root));
  const grammar = path.join(directory, 'simple.gram');
  fs.writeFileSync(grammar, '%mode dd\n%tree\n%token A "a"\nstart := A;\n');
  options.grammarFile = grammar; options.mode = 'dd';
  assert.equal((await manager.run(exe, options)).code, 0);
  fs.writeFileSync(path.join(out, 'user.txt'), 'not generated');
  const second = await manager.run(exe, options);
  assert.equal(second.cache, 'hit'); assert.ok(second.durationMs >= 0);
  assert.ok(second.files.every(file => !file.includes('.parseengine-cache') && !file.endsWith('user.txt')));
  assert.equal((await manager.run(exe, { ...options, rebuild: true })).cache, 'miss');
  const result = await engine.runProcess(process.execPath, ['-e', 'setInterval(()=>{},1000)'], { timeoutMs: 20 });
  assert.equal(result.timedOut, true);
}

/** @brief 在 VS Code 桩中执行真实生成与语言请求，校验旧诊断和旧悬停被过滤。 */
async function extension(directory) {
  const { api, state } = createVscodeStub(root);
  const originalLoad = Module._load;
  Module._load = function (request, ...args) { return request === 'vscode' ? api : originalLoad.call(this, request, ...args); };
  const extension = require('../extension');
  const originalInitialize = LspClient.prototype.initialize;
  let active;
  LspClient.prototype.initialize = async function (...args) { active = this; return originalInitialize.apply(this, args); };
  const grammar = path.join(directory, 'workspace.gram');
  let text = '%mode dd\n%tree\n%token A "a"\nstart := A;\n'; fs.writeFileSync(grammar, text);
  const document = { languageId: 'gram', uri: api.Uri.file(grammar), version: 1, isDirty: false, getText: () => text };
  api.workspace.textDocuments.push(document); api.window.activeTextEditor = { document };
  const stored = new Map(), subscriptions = [];
  const context = { subscriptions: { push: (...items) => subscriptions.push(...items) }, workspaceState: {
    get: key => stored.get(key), update: async (key, value) => stored.set(key, value) } };
  state.settings.parseEnginePath = exe;
  try {
    extension.activate(context); await until(() => state.statusBar.text.includes('运行中'));
    await until(() => state.diagnosticsByUri.has(document.uri.toString()));
    active.emit('diagnostics', { uri: document.uri.toString(), version: 0, diagnostics: [{ message: '旧结果' }] });
    assert.ok(!JSON.stringify(state.diagnosticsByUri.get(document.uri.toString())).includes('旧结果'));
    const frames = [], originalNotify = active.notify;
    active.notify = function (method, params) {
      if (method === 'textDocument/didChange') frames.push(params);
      return originalNotify.call(this, method, params);
    };
    const previous = text;
    text = text.replace('token A', 'token B').replace(':= A', ':= B'); document.version = 2;
    state.events.onDidChangeTextDocument({ document, contentChanges: [
      { rangeOffset: previous.indexOf('A'), range: { start: { line: 2, character: 7 }, end: { line: 2, character: 8 } }, text: 'B' },
      { rangeOffset: previous.lastIndexOf('A'), range: { start: { line: 3, character: 9 }, end: { line: 3, character: 10 } }, text: 'B' },
    ] });
    const definitions = await state.providers.find(item => item.kind === 'definition').provider
      .provideDefinition(document, new api.Position(3, 9));
    assert.equal(definitions[0].range.start.line, 2);
    assert.equal(frames.length, 1); assert.equal(frames[0].contentChanges.length, 2);
    assert.equal(frames[0].contentChanges[0].range.start.line, 3);
    fs.writeFileSync(grammar, text);
    const hover = state.providers.find(item => item.kind === 'hover').provider;
    const originalRequest = active.request;
    let finish;
    active.request = () => new Promise(resolve => { finish = resolve; });
    const pending = hover.provideHover(document, new api.Position(2, 7));
    document.version = 3; finish({ contents: { value: '旧悬停' } }); assert.equal(await pending, null);
    active.request = originalRequest;
    state.selections.push('cpp17', 'dd'); state.inputs.push(path.join(directory, 'ui-output'));
    await state.commands.get('gram.generate')();
    assert.ok(state.statusBar.text.includes('生成成功')); assert.equal(stored.size, 1);
    await state.commands.get('gram.generateAgain')(); assert.ok(state.statusBar.text.includes('缓存命中'));
    await state.commands.get('gram.rebuild')(); assert.ok(state.statusBar.text.includes('重新分析'));
    assert.ok(state.progress.every(item => item.cancellable));
    fs.writeFileSync(grammar, '%mode dd\n%tree\nstart := IMPLICIT;\n');
    await state.commands.get('gram.check')();
    const checked = state.diagnosticsByUri.get(document.uri.toString());
    assert.ok(checked.some(item => item.severity === api.DiagnosticSeverity.Information), JSON.stringify(checked));
    assert.ok(checked.every(item => item.severity !== api.DiagnosticSeverity.Error));
    fs.writeFileSync(grammar, text);
    state.settings.generationProfiles = [{ name: 'Java', mode: 'dd', lang: 'java', outputDir: path.join(directory, 'java') }];
    state.selections.push({ index: 0 }); await state.commands.get('gram.generateProfile')();
    assert.ok(fs.existsSync(path.join(directory, 'java/Parser.java')));

    // 跨文件引用只连接同一包含链，未保存的编辑器内容优先于磁盘。
    const common = path.join(directory, 'tokens.gram'), parent = path.join(directory, 'parent.gram');
    const separate = path.join(directory, 'separate.gram');
    fs.writeFileSync(common, '%token A "a" alias "A"\n');
    fs.writeFileSync(parent, '%include "tokens.gram"\n%start start\n%left A\nstart := A; // A\n');
    fs.writeFileSync(separate, '%token A "other"\nother := A;\n');
    api.workspace.findFiles = async () => [common, parent, separate].map(file => api.Uri.file(file));
    const imported = { languageId: 'gram', uri: api.Uri.file(common), version: 1,
      getText: () => '%token A "unsaved" alias "A"\n' };
    api.workspace.textDocuments.push(imported);
    const referenceProvider = state.providers.find(item => item.kind === 'references').provider;
    const renameProvider = state.providers.find(item => item.kind === 'rename').provider;
    const crossDefinition = state.providers.filter(item => item.kind === 'definition').at(-1).provider;
    const uses = await referenceProvider.provideReferences(imported, new api.Position(0, 7), { includeDeclaration: false });
    assert.equal(uses.length, 2); assert.ok(uses.every(item => item.uri.fsPath === parent));
    const parentDocument = { languageId: 'gram', uri: api.Uri.file(parent), version: 1,
      getText: () => fs.readFileSync(parent, 'utf8') };
    api.workspace.textDocuments.push(parentDocument);
    const cross = await crossDefinition.provideDefinition(parentDocument, new api.Position(3, 9));
    assert.equal(cross.length, 1); assert.equal(cross[0].uri.fsPath, common);
    const prepared = await renameProvider.prepareRename(imported, new api.Position(0, 7));
    assert.equal(prepared.placeholder, 'A');
    const renamed = await renameProvider.provideRenameEdits(imported, new api.Position(0, 7), 'ITEM');
    assert.equal(renamed.changes.size, 2);
    assert.equal([...renamed.changes.values()].flat().length, 3);
    assert.ok(!renamed.changes.has(api.Uri.file(separate).toString()));
    fs.writeFileSync(parent, '%include "tokens.gram"\n%token ITEM "item"\nstart := A;\n');
    parentDocument.version++;
    await assert.rejects(() => renameProvider.provideRenameEdits(imported, new api.Position(0, 7), 'ITEM'), /冲突/);
    fs.writeFileSync(parent, '%include "tokens.gram"\nstart := A { $$ = A; };\n'); parentDocument.version++;
    await assert.rejects(() => renameProvider.provideRenameEdits(imported, new api.Position(0, 7), 'ITEM'), /语义代码/);
    await assert.rejects(() => renameProvider.provideRenameEdits(imported, new api.Position(0, 7), 'bad-name'), /标识符/);
    fs.writeFileSync(parent, '%include "tokens.gram"\n%include "missing.gram"\nstart := A;\n'); parentDocument.version++;
    await assert.rejects(() => renameProvider.provideRenameEdits(imported, new api.Position(0, 7), 'ITEM'), /无法读取包含文件/);
    const { NavigationIndex } = require('../src/navigation');
    let indexRequests = 0;
    const countedClient = { running: true, request: (...args) => { ++indexRequests; return active.request(...args); } };
    const index = new NavigationIndex({ getClient: () => countedClient });
    const snapshot = await index.gather(imported);
    const initialRequests = indexRequests;
    await index.gather(imported);
    assert.equal(indexRequests, initialRequests, '未变化的文法应复用原生符号索引');
    imported.version++;
    assert.equal(await index.fresh(snapshot), false);
    const duringRead = await index.gather(imported);
    const originalRead = fs.promises.readFile;
    try {
      fs.promises.readFile = async function (file, ...args) {
        const result = await originalRead.call(this, file, ...args);
        if (file === separate) imported.version++;
        return result;
      };
      assert.equal(await index.fresh(duringRead), false, '磁盘核对期间的缓冲区编辑也应使结果失效');
    } finally { fs.promises.readFile = originalRead; }
    // 包含片段也可以引用入口文法声明的符号；多个入口产生歧义时拒绝修改。
    fs.writeFileSync(parent, '%token ROOT "root"\n%include "tokens.gram"\nstart := part;\n'); parentDocument.version++;
    imported.getText = () => 'part := ROOT;\n'; imported.version++;
    const inherited = await crossDefinition.provideDefinition(imported, new api.Position(0, 9));
    assert.equal(inherited[0].uri.fsPath, parent);
    const inheritedRename = await renameProvider.provideRenameEdits(imported, new api.Position(0, 9), 'ROOT_NEW');
    assert.equal([...inheritedRename.changes.values()].flat().length, 2);
    fs.writeFileSync(separate, '%token ROOT "another"\n%include "tokens.gram"\nother := part;\n');
    await assert.rejects(() => renameProvider.provideRenameEdits(imported, new api.Position(0, 9), 'ROOT_NEW'), /唯一声明/);
    const parsedIndex = await active.request('gram/symbolIndex', { text:
      '%include=\'sub/tokens.gram\'\n%code\n%include "not-a-grammar.gram"\n%endcode\nstart := ROOT;\n' });
    assert.deepEqual(parsedIndex.includes, ['sub/tokens.gram']);
    const incompleteIndex = await active.request('gram/symbolIndex', { text: 'start := ROOT { missing' });
    assert.equal(incompleteIndex.incomplete, true);
    const bomIndex = await active.request('gram/symbolIndex', { text: '\uFEFF%token BOM "bom"\nstart := BOM;' });
    assert.equal(bomIndex.occurrences[0].name, 'BOM');
    assert.equal(bomIndex.occurrences[0].range.start.character, 7);
    const untitled = { languageId: 'gram', version: 1,
      uri: { scheme: 'untitled', toString: () => 'untitled:Untitled-1' },
      getText: () => '%token LOCAL "local"\nstart := LOCAL;\n' };
    const localRename = await renameProvider.provideRenameEdits(untitled, new api.Position(0, 7), 'ITEM');
    assert.equal([...localRename.changes.values()].flat().length, 2);
    untitled.getText = () => '%token CLASH "clash"\nCLASH := CLASH;\n'; untitled.version++;
    await assert.rejects(() => renameProvider.provideRenameEdits(untitled, new api.Position(0, 7), 'ITEM'), /唯一声明/);
  } finally {
    extension.deactivate(); Module._load = originalLoad; LspClient.prototype.initialize = originalInitialize;
    for (const item of subscriptions) item.dispose?.();
  }
}

(async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'gram-p0-中文-'));
  try { await cancelClient(); await protocol(); await generation(directory); await extension(directory); }
  finally { fs.rmSync(directory, { recursive: true, force: true }); }
  console.log('扩展回归通过：范围同步、取消、增量生成、跨文件引用与安全重命名');
})().catch(error => { console.error(error); process.exitCode = 1; });
