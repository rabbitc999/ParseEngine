'use strict';

/**
 * VS Code 插件入口：把编辑器接到仓库自带的 lsp-server 与 ParseEngine。
 *
 * 设计取舍：
 *   - 不依赖任何 npm 包（`vscode` 由宿主提供），因此不需要构建步骤，也不需要联网；
 *   - 语言能力全在 src/providers.js，协议在 src/lspClient.js，命令行在 src/parseEngine.js；
 *   - 后两个文件不引用 `vscode`，可以在纯 Node 下单独测试（见 test/smoke.js）。
 */

const vscode = require('vscode');
const path = require('node:path');
const fs = require('node:fs');

const { LspClient } = require('./src/lspClient');
const tools = require('./src/serverLocator');
const engine = require('./src/parseEngine');
const providers = require('./src/providers');
const previewPanel = require('./src/previewPanel');

/** 文档改动后多久同步给服务器（毫秒）：跟打字节奏配合，避免每键一次往返。 */
const CHANGE_DEBOUNCE_MS = 300;

/** @type {LspClient|null} 当前的 lsp-server 客户端。 */
let client = null;

/** @type {vscode.DiagnosticCollection|null} 诊断集合。 */
let diagnosticCollection = null;

/** @type {vscode.StatusBarItem|null} 状态栏上的服务器状态。 */
let statusItem = null;

/** @type {vscode.OutputChannel|null} 输出面板（协议日志、校验与生成输出）。 */
let outputChannel = null;

/** @type {Map<string, number>} 文档 URI → 已同步的版本号。 */
const documentVersions = new Map();

/** @type {Map<string, NodeJS.Timeout>} 文档 URI → 防抖计时器。 */
const changeTimers = new Map();

/**
 * 插件激活：建面板、注册能力与命令、启动服务器。
 *
 * @param {vscode.ExtensionContext} context 扩展上下文。
 */
function activate(context) {
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
    getClient: () => client,
    logLine,
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
function readConfig() {
  const config = vscode.workspace.getConfiguration('gram');
  return {
    lspServerPath: config.get('lspServerPath', ''),
    parseEnginePath: config.get('parseEnginePath', ''),
    pythonPath: config.get('pythonPath', ''),
    defaultMode: config.get('defaultMode', 'ielr'),
    defaultLang: config.get('defaultLang', 'cpp17'),
    traceServer: config.get('traceServer', 'off'),
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
  await stopServer();
  setStatus('启动中…');

  // 1. 定位 lsp-server：设置里的路径优先，否则在仓库里自动探测。
  const config = readConfig();
  const resolved = tools.resolveTools(workspaceDir(), config);
  if (!resolved.lspServer) {
    setStatus('未找到 lsp-server');
    logLine('没找到 lsp-server：先跑 `make -C library/lsp`，或在设置里指定 gram.lspServerPath。');
    const choice = await vscode.window.showWarningMessage(
      'GramEditor：没找到 lsp-server，.gram 的诊断 / 补全等功能暂不可用。',
      '打开设置',
    );
    if (choice === '打开设置') {
      void vscode.commands.executeCommand('workbench.action.openSettings', 'gram.lspServerPath');
    }
    return;
  }

  // 2. 起进程并握手。
  const instance = new LspClient(resolved.lspServer, ['--stdio'], {
    cwd: resolved.repoRoot || workspaceDir() || undefined,
  });
  instance.on('diagnostics', (event) => publishDiagnostics(event));
  instance.on('log', (line) => {
    if (readConfig().traceServer !== 'off') logLine(`[server] ${line}`);
  });
  instance.on('exit', (code) => {
    setStatus(`已退出（${code}）`);
    client = null;
  });

  try {
    await instance.start();
    await instance.initialize();
    client = instance;
    setStatus('运行中');
  } catch (error) {
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
async function stopServer() {
  for (const timer of changeTimers.values()) clearTimeout(timer);
  changeTimers.clear();
  documentVersions.clear();

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
  if (!client || !client.running) return;

  const version = 1;
  documentVersions.set(document.uri.toString(), version);
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
  if (!client || !client.running) return;

  const key = document.uri.toString();
  const version = (documentVersions.get(key) || 1) + 1;
  documentVersions.set(key, version);
  client.notify('textDocument/didChange', {
    textDocument: { uri: key, version },
    contentChanges: [{ text: document.getText() }],
  });
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
 * 命令：校验当前文法（`lsp-server --check`）。
 */
async function checkActiveGrammar() {
  const document = activeGrammarDocument();
  if (!document) return;

  const file = await ensureSaved(document);
  if (!file) return;

  const resolved = tools.resolveTools(workspaceDir(), readConfig());
  if (!resolved.lspServer) {
    void vscode.window.showWarningMessage('GramEditor：没找到 lsp-server，先跑 `make -C library/lsp`。');
    return;
  }

  // 1. 跑一次 --check，原始输出写进输出面板。
  const result = await engine.checkGrammar(resolved.lspServer, file);
  logLine(`$ lsp-server --check "${file}"`);
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
        : vscode.DiagnosticSeverity.Error,
    );
    item.source = 'lsp-server --check';
    return item;
  });
  if (diagnosticCollection) diagnosticCollection.set(document.uri, items);

  // 3. 给一句结论。
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

  const config = readConfig();
  const resolved = tools.resolveTools(workspaceDir(), config);
  if (!resolved.parseEngine) {
    void vscode.window.showWarningMessage('GramEditor：没找到 ParseEngine，先跑 `make`。');
    return;
  }

  // 1. 选生成模式。
  const mode = await vscode.window.showQuickPick(
    ['ll1', 'lr0', 'slr', 'lalr', 'lr1', 'ielr', 'lrk', 'lalrk', 'slrk', 'llk', 'll0', 'glr', 'earley', 'cyk', 'dd', 'lex'],
    { title: '生成模式', placeHolder: `默认：${config.defaultMode}` },
  );
  if (!mode) return;

  // 2. 选目标语言。
  const lang = await vscode.window.showQuickPick(
    ['c', 'cpp17', 'python', 'csharp', 'rust', 'go', 'gram'],
    { title: '目标语言', placeHolder: `默认：${config.defaultLang}` },
  );
  if (!lang) return;

  // 3. 输出目录：默认放在文法旁边 `<名字>-<语言>`。
  const outputDir = await vscode.window.showInputBox({
    title: '输出目录',
    value: engine.defaultOutputDir(file, lang),
    valueSelection: [0, 0],
  });
  if (!outputDir) return;

  // 4. 跑生成器。
  setStatus('生成中…');
  logLine(`$ ParseEngine --grammar-file "${file}" --out-dir "${outputDir}" --mode ${mode} --lang ${lang}`);
  const result = await engine.generate(resolved.parseEngine, {
    grammarFile: file,
    outputDir,
    mode,
    lang,
  });
  logLine(result.output || '（无输出）');
  outputChannel.show(true);
  setStatus(result.code === 0 ? `生成成功（${result.files.length} 个文件）` : `生成失败（${result.code}）`);

  // 5. 成功时给一个"打开输出目录"按钮。
  if (result.code !== 0) {
    void vscode.window.showErrorMessage(
      `GramEditor：生成失败（退出码 ${result.code}），详见输出面板。`,
    );
    return;
  }
  const choice = await vscode.window.showInformationMessage(
    `GramEditor：生成成功，共 ${result.files.length} 个文件。`,
    '打开输出目录',
  );
  if (choice === '打开输出目录') {
    void vscode.commands.executeCommand('revealFileInOS', vscode.Uri.file(outputDir));
  }
}

/**
 * 命令：打开 Token / 完整语法树预览。
 */
async function previewActiveGrammar() {
  const document = activeGrammarDocument();
  if (!document) return;

  const file = await ensureSaved(document);
  if (!file) return;

  const config = readConfig();
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
 * 命令：打开文法指令手册（仓库里的 doc/12-grammar-usage.md）。
 */
async function openGrammarDoc() {
  const resolved = tools.resolveTools(workspaceDir(), readConfig());
  if (!resolved.repoRoot) {
    void vscode.window.showWarningMessage('GramEditor：没定位到仓库根，找不到指令手册。');
    return;
  }

  const docPath = path.join(resolved.repoRoot, 'doc', '12-grammar-usage.md');
  if (!fs.existsSync(docPath)) {
    void vscode.window.showWarningMessage(`GramEditor：没找到 ${docPath}。`);
    return;
  }

  const doc = await vscode.workspace.openTextDocument(vscode.Uri.file(docPath));
  await vscode.window.showTextDocument(doc, { preview: true });
}

module.exports = {
  activate,
  deactivate,
};
