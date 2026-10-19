'use strict';

/**
 * VS Code 插件入口：把编辑器接到插件自带 / 仓库里的 ParseEngine。
 *
 * 设计取舍：
 *   - 不依赖任何 npm 包（`vscode` 由宿主提供），因此不需要构建步骤，也不需要联网；
 *   - 语言服务器内嵌在生成器里：进程是 `ParseEngine lsp --stdio`，不需要单独的
 *     `lsp-server`，发布包里直接带上 `bin/ParseEngine(.exe)`；
 *   - 语言能力全在 src/providers.js，协议在 src/lspClient.js，命令行在 src/parseEngine.js；
 *   - 后两个文件不引用 `vscode`，可以在纯 Node 下单独测试（见 test/smoke.js）。
 */

const vscode = require('vscode');
const path = require('node:path');
const fs = require('node:fs');

const { LspClient } = require('./src/lspClient');
const tools = require('./src/serverLocator');
const engine = require('./src/parseEngine');
const generation = require('./src/generation');
const generationManager = new generation.GenerationManager();
let extensionContext;
let serverEpoch = 0;
let serverReady = false;
const pendingChanges = new Map();
const providers = require('./src/providers');
const previewPanel = require('./src/previewPanel');

/** 文档改动后多久同步给服务器（毫秒）：跟打字节奏配合，避免每键一次往返。 */
const CHANGE_DEBOUNCE_MS = 300;

/** @type {LspClient|null} 当前的语言服务器客户端（内嵌在 ParseEngine 里）。 */
let client = null;

/** @type {vscode.DiagnosticCollection|null} 诊断集合。 */
let diagnosticCollection = null;

/** @type {vscode.StatusBarItem|null} 状态栏上的服务器状态。 */
let statusItem = null;

/** @type {vscode.OutputChannel|null} 输出面板（协议日志、校验与生成输出）。 */
let outputChannel = null;

/** @type {Map<string, number>} 文档 URI → 已同步的版本号。 */
const documentVersions = new Map();
const protocolVersions = new Map();
let nextDocumentVersion = 1;

/** @type {Map<string, NodeJS.Timeout>} 文档 URI → 防抖计时器。 */
const changeTimers = new Map();

/**
 * 插件激活：建面板、注册能力与命令、启动服务器。
 *
 * @param {vscode.ExtensionContext} context 扩展上下文。
 */
function activate(context) {
  extensionContext = context;
  outputChannel = vscode.window.createOutputChannel('GramEditor');
  diagnosticCollection = vscode.languages.createDiagnosticCollection(providers.LANGUAGE_ID);
  statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 90);
  statusItem.command = 'gram.restartServer';
  statusItem.tooltip = '点击重启 GramEditor 语言服务器';
  statusItem.show();
  setStatus('启动中…');

  context.subscriptions.push(outputChannel, diagnosticCollection, statusItem);

  // 1. 语言能力：诊断由服务器推送，其余按需请求。
  providers.registerProviders(context, {
    getClient: () => serverReady ? client : null,
    logLine,
    flushDocument: notifyChange,
  });

  // 2. 文档同步。
  registerDocumentSync(context);

  // 3. 命令。
  registerCommands(context);

  // 4. 设置变化时重启服务器（工具路径或协议跟踪开关可能变了）。
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (
        event.affectsConfiguration('gram.lspServerPath') ||
        event.affectsConfiguration('gram.parseEnginePath') ||
        event.affectsConfiguration('gram.traceServer')
      ) {
        void startServer(context);
      }
    }),
  );

  // 5. 真正启动。
  void startServer(context);
}

/**
 * 插件停用：关掉服务器。
 */
function deactivate() {
  generationManager.dispose();
  previewPanel.disposePreview();
  void stopServer();
}

// ---------------------------------------------------------------------------
//  设置与状态
// ---------------------------------------------------------------------------

/**
 * 读插件设置。
 *
 * @returns {{lspServerPath: string, parseEnginePath: string, pythonPath: string,
 *            defaultMode: string, defaultLang: string, traceServer: string}} 设置快照。
 */
function readConfig(uri) {
  const config = vscode.workspace.getConfiguration('gram', uri);
  return {
    lspServerPath: config.get('lspServerPath', ''),
    parseEnginePath: config.get('parseEnginePath', ''),
    pythonPath: config.get('pythonPath', ''),
    defaultMode: config.get('defaultMode', 'ielr'),
    defaultLang: config.get('defaultLang', 'cpp17'),
    traceServer: config.get('traceServer', 'off'),
    incremental: config.get('incrementalGeneration', true),
    cacheDir: config.get('cacheDirectory', ''),
    generationProfiles: config.get('generationProfiles', []),
  };
}

/**
 * 当前工作区目录（多根工作区取第一个）。
 *
 * @returns {string} 目录路径；没有工作区时返回空串。
 */
function workspaceDir() {
  const folders = vscode.workspace.workspaceFolders;
  return folders && folders.length ? folders[0].uri.fsPath : '';
}

/**
 * 更新状态栏文字。
 *
 * @param {string} text 状态描述。
 */
function setStatus(text) {
  if (statusItem) statusItem.text = `$(symbol-namespace) Gram: ${text}`;
}

/**
 * 往输出面板写一行。
 *
 * @param {string} text 文本。
 */
function logLine(text) {
  if (outputChannel) outputChannel.appendLine(text);
}

// ---------------------------------------------------------------------------
//  服务器生命周期
// ---------------------------------------------------------------------------

/**
 * 启动（或重启）语言服务器。
 *
 * @param {vscode.ExtensionContext} context 扩展上下文。
 */
async function startServer(context) {
  const epoch = ++serverEpoch;
  await stopServer(false);
  if (epoch !== serverEpoch) return;
  setStatus('启动中…');

  // 1. 定位编译器：设置里的路径优先，其次是插件自带的 bin/，最后在仓库里自动探测。
  const config = readConfig();
  const resolved = tools.resolveTools(workspaceDir(), config);
  if (!resolved.lspCommand) {
    setStatus('未找到 ParseEngine');
    logLine(
      '没找到 ParseEngine：打开插件的 bin/ 目录不方便时，' +
        '可以在设置里指定 gram.parseEnginePath，或把 ParseEngine 放进 PATH。',
    );
    const choice = await vscode.window.showWarningMessage(
      'GramEditor：没找到 ParseEngine，.gram 的诊断 / 补全等功能暂不可用。',
      '打开设置',
    );
    if (choice === '打开设置') {
      void vscode.commands.executeCommand('workbench.action.openSettings', 'gram.parseEnginePath');
    }
    return;
  }

  // 2. 起进程并握手（内嵌的那份是 `ParseEngine lsp --stdio`）。
  const instance = new LspClient(resolved.lspCommand, resolved.lspArgs, {
    cwd: resolved.repoRoot || workspaceDir() || undefined,
  });
  instance.on('diagnostics', (event) => { if (client === instance && epoch === serverEpoch) publishDiagnostics(event); });
  instance.on('log', (line) => {
    if (readConfig().traceServer !== 'off') logLine(`[server] ${line}`);
  });
  instance.on('exit', (code) => {
    if (client !== instance || epoch !== serverEpoch) return;
    setStatus(`已退出（${code}）`);
    client = null; serverReady = false;
  });

  client = instance;
  try {
    await instance.start();
    await instance.initialize();
    if (epoch !== serverEpoch) { await instance.stop(); return; }
    client = instance; serverReady = true;
    setStatus('运行中');
  } catch (error) {
    if (epoch !== serverEpoch) return;
    client = null;
    setStatus('启动失败');
    logLine(`启动失败：${error.message}`);
    try {
      await instance.stop();
    } catch {
      // 起不来就没什么可关的。
    }
    return;
  }

  // 3. 已经打开的 .gram 文档补一次 didOpen，让诊断立刻出来。
  for (const document of vscode.workspace.textDocuments) {
    if (providers.isSupportedLanguage(document.languageId)) notifyOpen(document);
  }
}

/**
 * 关掉语言服务器并清掉待办的防抖任务。
 */
async function stopServer(invalidate = true) {
  serverReady = false;
  if (invalidate) ++serverEpoch;
  for (const timer of changeTimers.values()) clearTimeout(timer);
  changeTimers.clear();
  documentVersions.clear();
  protocolVersions.clear();
  pendingChanges.clear();

  const instance = client;
  client = null;
  if (!instance) return;

  try {
    await instance.stop();
  } catch {
    // 退出期的异常忽略。
  }
}

// ---------------------------------------------------------------------------
//  文档同步
// ---------------------------------------------------------------------------

/**
 * 通知服务器：文档打开了。
 *
 * @param {vscode.TextDocument} document 文档。
 */
function notifyOpen(document) {
  if (!client || !client.running || !serverReady) return;
  const key = document.uri.toString();
  pendingChanges.delete(key);
  const timer = changeTimers.get(key); if (timer) clearTimeout(timer); changeTimers.delete(key);

  const version = nextDocumentVersion++;
  documentVersions.set(document.uri.toString(), document.version ?? 1);
  protocolVersions.set(document.uri.toString(), version);
  client.notify('textDocument/didOpen', {
    textDocument: {
      uri: document.uri.toString(),
      languageId: document.languageId,
      version,
      text: document.getText(),
    },
  });
}

/**
 * 通知服务器：文档内容变了（全量同步）。
 *
 * @param {vscode.TextDocument} document 文档。
 */
function notifyChange(document) {
  if (!document || !client || !client.running || !serverReady) return;
  const key = document.uri.toString();
  if (!documentVersions.has(key)) { notifyOpen(document); return; }
  const timer = changeTimers.get(key); if (timer) clearTimeout(timer); changeTimers.delete(key);
  const changes = pendingChanges.get(key);
  if (!changes?.length && documentVersions.get(key) === (document.version ?? 1)) return;
  const sync = client.capabilities?.textDocumentSync;
  const incremental = sync === 2 || sync?.change === 2;
  const version = nextDocumentVersion++;
  client.notify('textDocument/didChange', {
    textDocument: { uri: key, version },
    contentChanges: incremental && changes?.length ? changes : [{ text: document.getText() }],
  });
  documentVersions.set(key, document.version ?? 1); protocolVersions.set(key, version); pendingChanges.delete(key);
}

/**
 * 注册文档事件：打开 / 修改（防抖）/ 保存 / 关闭。
 *
 * @param {vscode.ExtensionContext} context 扩展上下文。
 */
function registerDocumentSync(context) {
  // 1. 打开。
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((document) => {
      if (providers.isSupportedLanguage(document.languageId)) notifyOpen(document);
    }),
  );

  // 2. 修改：停手 300ms 再同步，避免每个字符都发一次。
  context.subscriptions.push(
    vscode.workspace.onDidChangeTextDocument((event) => {
      const document = event.document;
      if (!providers.isSupportedLanguage(document.languageId)) return;

      const key = document.uri.toString();
      const changes = [...(event.contentChanges || [])].sort((a, b) => b.rangeOffset - a.rangeOffset)
        .map(change => ({ range: { start: { line: change.range.start.line, character: change.range.start.character },
          end: { line: change.range.end.line, character: change.range.end.character } }, text: change.text }));
      pendingChanges.set(key, [...(pendingChanges.get(key) || []), ...changes]);
      if (diagnosticCollection) diagnosticCollection.delete(document.uri);
      const existing = changeTimers.get(key);
      if (existing) clearTimeout(existing);
      changeTimers.set(
        key,
        setTimeout(() => {
          changeTimers.delete(key);
          notifyChange(document);
        }, CHANGE_DEBOUNCE_MS),
      );
    }),
  );

  // 3. 保存：让服务器知道（本服务器把它当作一次校验时机）。
  context.subscriptions.push(
    vscode.workspace.onDidSaveTextDocument((document) => {
      if (!providers.isSupportedLanguage(document.languageId)) return;
      notifyChange(document);
      if (client && client.running) {
        client.notify('textDocument/didSave', {
          textDocument: { uri: document.uri.toString() },
          text: document.getText(),
        });
      }
    }),
  );

  // 4. 关闭：清诊断、版本号与待办计时器。
  context.subscriptions.push(
    vscode.workspace.onDidCloseTextDocument((document) => {
      if (!providers.isSupportedLanguage(document.languageId)) return;

      const key = document.uri.toString();
      if (client && client.running) {
        client.notify('textDocument/didClose', { textDocument: { uri: key } });
      }
      documentVersions.delete(key);
      protocolVersions.delete(key);
      pendingChanges.delete(key);

      const timer = changeTimers.get(key);
      if (timer) {
        clearTimeout(timer);
        changeTimers.delete(key);
      }
      if (diagnosticCollection) diagnosticCollection.delete(document.uri);
    }),
  );
}

/**
 * 把服务器推送的诊断写进 VS Code。
 *
 * @param {{uri: string, diagnostics: Array<any>}} event 诊断事件。
 */
function publishDiagnostics(event) {
  if (!diagnosticCollection || !event.uri) return;
  const document = vscode.workspace.textDocuments.find(item => item.uri.toString() === event.uri);
  if (!document || pendingChanges.has(event.uri)) return;
  if (documentVersions.get(event.uri) !== (document.version ?? 1)) return;
  if (event.version !== undefined && event.version !== protocolVersions.get(event.uri)) return;
  diagnosticCollection.set(vscode.Uri.parse(event.uri), providers.toDiagnostics(event.diagnostics));
}

// ---------------------------------------------------------------------------
//  命令
// ---------------------------------------------------------------------------

/**
 * 注册六个命令。
 *
 * @param {vscode.ExtensionContext} context 扩展上下文。
 */
function registerCommands(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand('gram.check', () => checkActiveGrammar()),
    vscode.commands.registerCommand('gram.generate', () => generateForActiveGrammar()),
    vscode.commands.registerCommand('gram.generateAgain', () => repeatGeneration(false)),
    vscode.commands.registerCommand('gram.rebuild', () => repeatGeneration(true)),
    vscode.commands.registerCommand('gram.generateProfile', () => generateFromProfile()),
    vscode.commands.registerCommand('gram.cancelGeneration', () => generationManager.dispose()),
    vscode.commands.registerCommand('gram.preview', () => previewActiveGrammar()),
    vscode.commands.registerCommand('gram.restartServer', () => startServer(context)),
    vscode.commands.registerCommand('gram.openDoc', () => openGrammarDoc()),
  );
}

/**
 * 取当前活动编辑器里的 `.gram` 文档。
 *
 * @returns {vscode.TextDocument|null} 文档；不是 .gram 时提示并返回 null。
 */
function activeGrammarDocument() {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== providers.LANGUAGE_ID) {
    void vscode.window.showInformationMessage('GramEditor：请先打开一个 .gram 文件。');
    return null;
  }
  return editor.document;
}

/**
 * 保证文档已存盘（`--check` 与生成都要读磁盘上的文件）。
 *
 * @param {vscode.TextDocument} document 文档。
 * @returns {Promise<string|null>} 文件路径；未保存或用户取消时返回 null。
 */
async function ensureSaved(document) {
  // 1. 未命名文档先走"另存为"。
  if (document.uri.scheme === 'untitled') {
    await vscode.window.showTextDocument(document);
    await vscode.commands.executeCommand('workbench.action.files.saveAs');
    if (document.uri.scheme === 'untitled') return null;
  }

  // 2. 有未保存修改就保存一次。
  if (document.isDirty) {
    const saved = await document.save();
    if (!saved) return null;
  }

  return document.uri.scheme === 'file' ? document.uri.fsPath : null;
}

/**
 * 命令：校验当前文法（`ParseEngine lsp --check`）。
 */
async function checkActiveGrammar() {
  const document = activeGrammarDocument();
  if (!document) return;

  const file = await ensureSaved(document);
  if (!file) return;

  const resolved = tools.resolveTools(workspaceDir(), readConfig());
  if (!resolved.lspCommand) {
    void vscode.window.showWarningMessage(
      'GramEditor：没找到 ParseEngine，先跑 `make -C source` 或在设置里指定路径。',
    );
    return;
  }

  // 1. 跑一次 --check，原始输出写进输出面板。
  const version = document.version;
  const result = await engine.checkGrammar(resolved.lspCommand, resolved.checkArgs, file);
  if (document.version !== version || document.isClosed) { logLine('已丢弃过期的校验结果'); return; }
  logLine(`$ ${resolved.lspCommand} ${resolved.checkArgs.join(' ')} "${file}"`);
  logLine(result.output || '（无输出）');

  // 2. 解析出的诊断刷进编辑器，和实时诊断保持同一套显示。
  const items = result.diagnostics.map((diagnostic) => {
    const line = Math.max(0, diagnostic.line - 1);
    const column = Math.max(0, diagnostic.column - 1);
    const start = new vscode.Position(line, column);
    const item = new vscode.Diagnostic(
      new vscode.Range(start, start.translate(0, 1)),
      diagnostic.message,
      diagnostic.severity === 2
        ? vscode.DiagnosticSeverity.Warning
        : diagnostic.severity === 3 ? vscode.DiagnosticSeverity.Information : vscode.DiagnosticSeverity.Error,
    );
    item.source = 'ParseEngine lsp --check';
    return item;
  });
  if (diagnosticCollection) diagnosticCollection.set(document.uri, items);

  // 3. 给一句结论。
  if (result.code !== 0 && !result.diagnostics.length) {
    setStatus('校验失败'); await vscode.window.showErrorMessage(result.output || '校验进程失败'); return;
  }
  const errors = result.diagnostics.filter((item) => item.severity === 1).length;
  const warnings = result.diagnostics.filter((item) => item.severity === 2).length;
  setStatus(errors ? `校验：${errors} 错 ${warnings} 警` : '校验通过');
  void vscode.window.showInformationMessage(
    errors || warnings
      ? `GramEditor：${errors} 个错误，${warnings} 个警告（详见输出面板）`
      : 'GramEditor：文法校验通过',
  );
  outputChannel.show(true);
}

/**
 * 命令：生成解析器（选模式 / 语言 / 输出目录后调 ParseEngine）。
 */
async function generateForActiveGrammar() {
  const document = activeGrammarDocument();
  if (!document) return;

  const file = await ensureSaved(document);
  if (!file) return;

  const config = readConfig(document.uri);
  const resolved = tools.resolveTools(workspaceDir(), config);
  if (!resolved.parseEngine) {
    void vscode.window.showWarningMessage('GramEditor：没找到 ParseEngine，先跑 `make`。');
    return;
  }

  // 先选目标，再只展示该目标支持的模式；默认值排在首位。
  const languages = [config.defaultLang, ...generation.LANGUAGES.filter(lang => lang !== config.defaultLang)];
  const lang = await vscode.window.showQuickPick(languages, { title: '目标语言' });
  if (!lang) return;
  const available = generation.allowedModes(lang);
  const modes = available.includes(config.defaultMode) ?
    [config.defaultMode, ...available.filter(mode => mode !== config.defaultMode)] : available;
  const mode = await vscode.window.showQuickPick(modes, { title: '生成模式', placeHolder: `目标：${lang}` });
  if (!mode) return;

  // 3. 输出目录：默认放在文法旁边 `<名字>-<语言>`。
  const outputDir = await vscode.window.showInputBox({
    title: '输出目录',
    value: engine.defaultOutputDir(file, lang),
    valueSelection: [0, 0],
  });
  if (!outputDir) return;

  await executeGeneration(document, { mode, lang, outputDir, incremental: config.incremental, cacheDir: config.cacheDir });
}

/** @brief 使用当前文法上次保存的配置。@param {boolean} rebuild 是否强制重新分析。 */
async function repeatGeneration(rebuild) {
  const document = activeGrammarDocument(); if (!document) return;
  const profile = extensionContext.workspaceState?.get('generation:' + document.uri.toString());
  if (!profile) { await vscode.window.showInformationMessage('尚无生成配置，请先生成一次或选择预设。'); return; }
  await executeGeneration(document, { ...profile, rebuild });
}

/** @brief 选择工作区设置中的具名生成配置。 */
async function generateFromProfile() {
  const document = activeGrammarDocument(); if (!document) return;
  const config = readConfig(document.uri);
  const profiles = config.generationProfiles;
  if (!profiles.length) { await vscode.window.showInformationMessage('请先在 gram.generationProfiles 中添加生成配置。'); return; }
  const selected = await vscode.window.showQuickPick(profiles.map((profile, index) => ({ label: profile.name, index })), { title: '生成配置' });
  if (selected) await executeGeneration(document, { incremental: config.incremental, cacheDir: config.cacheDir, ...profiles[selected.index] });
}

/** @brief 校验配置、记忆参数并执行可取消的生成任务。
 * @param {vscode.TextDocument} document 当前文法。@param {object} profile 生成配置。
 */
async function executeGeneration(document, profile) {
  const file = await ensureSaved(document); if (!file) return;
  const root = vscode.workspace.getWorkspaceFolder?.(document.uri)?.uri.fsPath || path.dirname(file);
  let options;
  try { options = generation.resolveProfile(profile, file, root); }
  catch (error) { await vscode.window.showErrorMessage(error.message); return; }
  const resolved = tools.resolveTools(root, readConfig(document.uri));
  if (!resolved.parseEngine) { await vscode.window.showErrorMessage('未找到 ParseEngine'); return; }
  const remembered = { ...profile, outputDir: options.outputDir, cacheDir: options.cacheDir };
  delete remembered.rebuild;
  await extensionContext.workspaceState?.update('generation:' + document.uri.toString(), remembered);
  setStatus('生成中…');
  logLine(`$ ${resolved.parseEngine} ${engine.buildGenerateArgs(options).map(value => JSON.stringify(value)).join(' ')}`);
  const result = await vscode.window.withProgress({ location: vscode.ProgressLocation.Notification,
    title: 'Gram：生成解析器', cancellable: true }, async (progress, token) => {
    const controller = new AbortController();
    const subscription = token.onCancellationRequested(() => controller.abort());
    if (token.isCancellationRequested) controller.abort();
    try { return await generationManager.run(resolved.parseEngine, { ...options, signal: controller.signal }); }
    finally { subscription.dispose(); }
  });
  logLine(result.output || '（无输出）'); outputChannel.show(true);
  if (result.cancelled) { setStatus('生成已取消'); return; }
  if (result.code !== 0) {
    setStatus(result.busy ? '已有生成任务' : '生成失败');
    await vscode.window.showErrorMessage(result.output || `生成失败：${result.code}`); return;
  }
  const cache = result.cache === 'hit' ? '缓存命中' : result.cache === 'miss' ? '重新分析' : '未启用缓存';
  setStatus(`生成成功 · ${cache} · ${(result.durationMs / 1000).toFixed(2)}s`);
  const choice = await vscode.window.showInformationMessage(`生成成功，${cache}，共 ${result.files.length} 个文件。`, '打开输出目录');
  if (choice === '打开输出目录') await vscode.commands.executeCommand('revealFileInOS', vscode.Uri.file(options.outputDir));
}

/**
 * 命令：打开 Token / 完整语法树预览。
 */
async function previewActiveGrammar() {
  const document = activeGrammarDocument();
  if (!document) return;

  const file = await ensureSaved(document);
  if (!file) return;

  const config = readConfig(document.uri);
  const resolved = tools.resolveTools(workspaceDir(), config);
  if (!resolved.parseEngine) {
    void vscode.window.showWarningMessage('GramEditor：没找到 ParseEngine，先跑 `tools/lock.sh build`。');
    return;
  }

  previewPanel.showPreview(vscode, {
    grammarFile: file,
    parseEngine: resolved.parseEngine,
    pythonPath: config.pythonPath,
  });
}

/**
 * 命令：打开文法指令手册。
 *
 * 在仓库里就直接开 `doc/12-grammar-usage.md`（能编辑、能搜）；装成 `.vsix` 单独用时
 * 没有仓库，就退回编译器里的内嵌手册（`ParseEngine man 12-grammar-usage`），
 * 放到一个只读的虚拟文档里看。
 */
async function openGrammarDoc() {
  const resolved = tools.resolveTools(workspaceDir(), readConfig());

  // 1. 仓库里的 Markdown 优先。
  if (resolved.repoRoot) {
    const docPath = path.join(resolved.repoRoot, 'doc', '12-grammar-usage.md');
    if (fs.existsSync(docPath)) {
      const doc = await vscode.workspace.openTextDocument(vscode.Uri.file(docPath));
      await vscode.window.showTextDocument(doc, { preview: true });
      return;
    }
  }

  // 2. 没有仓库就用内嵌手册。
  if (!resolved.parseEngine) {
    void vscode.window.showWarningMessage(
      'GramEditor：没定位到仓库，也没找到 ParseEngine，看不了指令手册。',
    );
    return;
  }
  const result = await engine.runProcess(
    resolved.parseEngine,
    ['man', '12-grammar-usage', '--lang', manualLanguage()],
    { timeoutMs: 30000 },
  );
  if (result.code !== 0) {
    logLine(result.output);
    void vscode.window.showWarningMessage('GramEditor：内嵌手册读取失败，详见输出面板。');
    return;
  }
  const document = await vscode.workspace.openTextDocument({
    content: result.output,
    language: 'markdown',
  });
  await vscode.window.showTextDocument(document, { preview: true });
}

/**
 * 内嵌手册用哪种正文语言：跟着 VS Code 的显示语言走，非中文一律用英文。
 *
 * @returns {string} `zh`、`en` 或 `ja`。
 */
function manualLanguage() {
  const language = (vscode.env.language || '').toLowerCase();
  if (language.startsWith('zh')) return 'zh';
  if (language.startsWith('ja')) return 'ja';
  return 'en';
}

module.exports = {
  activate,
  deactivate,
};
