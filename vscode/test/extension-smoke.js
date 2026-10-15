'use strict';

/**
 * 插件集成测试：用一个"桩 vscode"加载 extension.js，验证插件本体能跑。
 *
 * 为什么需要它：test/smoke.js 只测 src/ 下的纯 Node 模块，测不到 extension.js
 * 本身（它第一行就 require('vscode')）。这里把 `vscode` 换成一个最小实现，
 * 就能在没有 VS Code 的机器上验证：
 *   1. activate 注册了 8 个语言 provider 与 6 个命令；
 *   2. 语言服务器真的被启动起来了（状态栏变成"运行中"）；
 *   3. deactivate 会把服务器关掉。
 *
 * 用法：
 *   node vscode/test/extension-smoke.js
 */

const Module = require('node:module');
const path = require('node:path');
const vm = require('node:vm');
const manifest = require('../package.json');

const tools = require('../src/serverLocator');

let checks = 0;
let failures = 0;

/**
 * 记一条断言。
 *
 * @param {string} title 断言描述。
 * @param {boolean} ok 是否通过。
 */
function check(title, ok) {
  checks += 1;
  if (!ok) failures += 1;
  console.log(`   [${ok ? 'OK' : '失败'}] ${title}`);
}

/**
 * 等一会儿。
 *
 * @param {number} ms 毫秒。
 * @returns {Promise<void>} 计时结束。
 */
function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/**
 * 造一个够用的 vscode 桩：只实现 extension.js 与 providers.js 真正用到的 API。
 *
 * @param {string} workspacePath 工作区目录。
 * @returns {{api: object, state: object}} 桩对象与可断言的内部状态。
 */
function createVscodeStub(workspacePath) {
  const state = {
    providers: [],
    commands: new Map(),
    statusText: '',
    diagnosticsByUri: new Map(),
    disposables: 0,
    quickPicks: [],
    webviews: [],
  };

  /**
   * 建一个可丢弃对象（VS Code 的 Disposable）。
   *
   * @returns {{dispose: Function}} 可丢弃对象。
   */
  const disposable = () => ({
    dispose() {
      state.disposables += 1;
    },
  });

  const api = {
    StatusBarAlignment: { Left: 1, Right: 2 },
    ViewColumn: { Beside: 2 },
    DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2, Hint: 3 },
    CompletionItemKind: {
      Method: 2, Function: 3, Field: 5, Variable: 6, Class: 7, Interface: 8,
      Module: 9, Property: 10, Enum: 13, Keyword: 14, Snippet: 15,
      Constant: 21, Struct: 22, TypeParameter: 25,
    },
    SymbolKind: {
      Module: 2, Namespace: 3, Class: 5, Method: 6, Property: 7, Field: 8,
      Constructor: 9, Function: 12, Variable: 13, Constant: 14, EnumMember: 22, Struct: 23,
    },
    CodeActionKind: { QuickFix: { value: 'quickfix' } },

    Uri: {
      parse: (value) => ({ toString: () => value, fsPath: value, scheme: 'file' }),
      file: (value) => ({ toString: () => `file:///${value.replace(/\\/g, '/')}`, fsPath: value, scheme: 'file' }),
    },
    Position: class Position {
      constructor(line, character) {
        this.line = line;
        this.character = character;
      }

      translate(lineDelta, characterDelta) {
        return new api.Position(this.line + lineDelta, this.character + characterDelta);
      }
    },
    Range: class Range {
      constructor(start, end) {
        this.start = start;
        this.end = end;
      }
    },
    Diagnostic: class Diagnostic {
      constructor(range, message, severity) {
        this.range = range;
        this.message = message;
        this.severity = severity;
      }
    },
    MarkdownString: class MarkdownString {
      constructor(value) {
        this.value = value;
        this.isTrusted = false;
      }
    },
    Hover: class Hover {
      constructor(contents, range) {
        this.contents = contents;
        this.range = range;
      }
    },
    CompletionItem: class CompletionItem {
      constructor(label, kind) {
        this.label = label;
        this.kind = kind;
      }
    },
    Location: class Location {
      constructor(uri, range) {
        this.uri = uri;
        this.range = range;
      }
    },
    DocumentSymbol: class DocumentSymbol {
      constructor(name, detail, kind, range, selectionRange) {
        this.name = name;
        this.detail = detail;
        this.kind = kind;
        this.range = range;
        this.selectionRange = selectionRange;
        this.children = [];
      }
    },
    TextEdit: class TextEdit {
      constructor(range, newText) {
        this.range = range;
        this.newText = newText;
      }
    },
    CodeAction: class CodeAction {
      constructor(title, kind) {
        this.title = title;
        this.kind = kind;
      }
    },
    WorkspaceEdit: class WorkspaceEdit {
      set() {}
    },

    window: {
      createOutputChannel: () => ({
        appendLine() {},
        show() {},
        dispose() {},
      }),
      createStatusBarItem: () => {
        // 状态栏对象留个引用，测试要靠它判断服务器是不是起来了。
        const item = {
          text: '',
          tooltip: '',
          command: '',
          show() {},
          dispose() {},
        };
        state.statusBar = item;
        return item;
      },
      createWebviewPanel: (viewType, title) => {
        const panel = {
          viewType,
          title,
          webview: {
            html: '',
            onDidReceiveMessage: (handler) => {
              panel.messageHandler = handler;
              return disposable();
            },
            postMessage: async () => true,
          },
          onDidDispose: (handler) => {
            panel.disposeHandler = handler;
            return disposable();
          },
          dispose() {
            panel.disposed = true;
            if (panel.disposeHandler) panel.disposeHandler();
          },
          reveal() {},
        };
        state.webviews.push(panel);
        return panel;
      },
      showInformationMessage: async () => undefined,
      showWarningMessage: async () => undefined,
      showErrorMessage: async () => undefined,
      showQuickPick: async (items) => {
        state.quickPicks.push(items);
        return undefined;
      },
      showInputBox: async () => undefined,
      showTextDocument: async () => undefined,
      activeTextEditor: undefined,
    },

    workspace: {
      workspaceFolders: [{ uri: { fsPath: workspacePath } }],
      textDocuments: [],
      getConfiguration: () => ({ get: (key, fallback) =>
        key === 'lspServerPath' ? (process.env.LSP_SERVER || fallback) : fallback }),
      onDidChangeConfiguration: disposable,
      onDidOpenTextDocument: disposable,
      onDidChangeTextDocument: disposable,
      onDidSaveTextDocument: disposable,
      onDidCloseTextDocument: disposable,
      openTextDocument: async () => ({}),
    },

    languages: {
      createDiagnosticCollection: () => ({
        set(uri, items) {
          state.diagnosticsByUri.set(uri.toString(), items);
        },
        delete(uri) {
          state.diagnosticsByUri.delete(uri.toString());
        },
        dispose() {},
      }),
      registerHoverProvider: (selector, provider) => {
        state.providers.push({ kind: 'hover', selector, provider });
        return disposable();
      },
      registerCompletionItemProvider: (selector, provider, ...triggers) => {
        state.providers.push({ kind: 'completion', selector, provider, triggers });
        return disposable();
      },
      registerDefinitionProvider: (selector, provider) => {
        state.providers.push({ kind: 'definition', selector, provider });
        return disposable();
      },
      registerDocumentSymbolProvider: (selector, provider) => {
        state.providers.push({ kind: 'symbols', selector, provider });
        return disposable();
      },
      registerDocumentFormattingEditProvider: (selector, provider) => {
        state.providers.push({ kind: 'formatting', selector, provider });
        return disposable();
      },
      registerCodeActionsProvider: (selector, provider) => {
        state.providers.push({ kind: 'codeAction', selector, provider });
        return disposable();
      },
      registerFoldingRangeProvider: (selector, provider) => {
        state.providers.push({ kind: 'folding', selector, provider });
        return disposable();
      },
    },

    commands: {
      registerCommand: (id, handler) => {
        state.commands.set(id, handler);
        return disposable();
      },
      executeCommand: async () => undefined,
    },
  };

  return { api, state };
}

/**
 * 入口。
 */
async function main() {
  // 1. 定位仓库根：桩工作区就设成它，插件才能自动探测到工具。
  const repoRoot = tools.findRepoRoot(process.cwd());
  check('定位仓库根', !!repoRoot);
  if (!repoRoot) {
    console.log(`\n共 ${checks} 项检查，失败 ${failures} 项`);
    process.exit(1);
  }

  // 2. 把 'vscode' 换成桩，再加载 extension.js。
  const { api, state } = createVscodeStub(repoRoot);
  const originalLoad = Module._load;
  Module._load = function patchedLoad(request, parent, isMain) {
    if (request === 'vscode') return api;
    return originalLoad.call(this, request, parent, isMain);
  };

  // 3. 统计服务器被关过几次：验证 deactivate 真的收尾。
  const lspModule = require('../src/lspClient');
  const originalStop = lspModule.LspClient.prototype.stop;
  let stopCalls = 0;
  lspModule.LspClient.prototype.stop = async function patchedStop(...args) {
    stopCalls += 1;
    return originalStop.apply(this, args);
  };

  const extension = require('../extension.js');
  let context;
  try {
    // 4. 激活：给一个最小的 ExtensionContext。
    const subscriptions = [];
    context = { subscriptions: { push: (...items) => subscriptions.push(...items), items: subscriptions } };
    extension.activate(context);

    // 5. 断言注册结果。
    const kinds = state.providers.map((item) => item.kind).sort();
    console.log(`   注册的能力：${kinds.join('、')}`);
    check('注册了 6 个语言能力', state.providers.length === 6);
    // `.mgl`（Magel）归 magel/vscode 插件，`.metro` / `.m`（MetroLang）归
    // metro-lang/vscode 插件；这里只认 .gram。
    check('不声明 .mgl 与 .metro',
      !state.providers.some((item) =>
        item.selector.some((entry) => entry.language === 'magel' || entry.language === 'metro-lang')));
    check('宏补全支持 $ 触发', state.providers.some((item) =>
      item.kind === 'completion' && item.triggers.includes('$')));
    check('包含悬停 / 补全 / 跳转 / 大纲 / 格式化 / 快速修复', [
      'codeAction', 'completion', 'definition', 'formatting', 'hover', 'symbols',
    ].every((kind) => kinds.includes(kind)));

    const commandIds = [...state.commands.keys()].sort();
    console.log(`   注册的命令：${commandIds.join('、')}`);
    check('注册了 5 个命令', commandIds.length === 5);
    check(
      '命令 id 正确',
      ['gram.check', 'gram.generate', 'gram.preview', 'gram.restartServer', 'gram.openDoc']
        .every((id) => commandIds.includes(id)),
    );
    check('注册了全部可释放资源', context.subscriptions.items.length >= 10);

    const modeSettings = manifest.contributes.configuration.properties['gram.defaultMode'].enum;
    check('设置允许 Earley / CYK', ['earley', 'cyk'].every((mode) => modeSettings.includes(mode)));
    api.window.activeTextEditor = {
      document: { languageId: 'gram', uri: api.Uri.file(path.join(repoRoot, 'example', 'calc.gram')), isDirty: false },
    };
    await state.commands.get('gram.generate')();
    check('生成菜单列出 Earley / CYK', state.quickPicks.some((items) =>
      ['earley', 'cyk'].every((mode) => items.includes(mode))));

    await state.commands.get('gram.preview')();
    const previewPanel = state.webviews[0];
    check('预览命令创建 Webview', !!previewPanel);
    const html = previewPanel ? previewPanel.webview.html : '';
    check('Webview 包含 Token、语法树与导出控件',
      html.includes('tokensPane') && html.includes('treePane') && html.includes('saveTree'));
    const scriptMatch = /<script nonce="[^"]+">([\s\S]*)<\/script>/.exec(html);
    let scriptValid = false;
    try {
      if (scriptMatch) {
        new vm.Script(scriptMatch[1], { filename: 'preview-webview.js' });
        scriptValid = true;
      }
    } catch (error) {
      console.log(`   Webview 脚本错误：${error.stack || error.message}`);
      scriptValid = false;
    }
    check('Webview 内联脚本语法正确', scriptValid);

    // 6. 等服务器起来：插件的状态栏文字会变成"运行中"。
    await sleep(3000);
    const statusText = state.statusBar ? state.statusBar.text : '';
    console.log(`   状态栏：${statusText}`);
    check('语言服务器进入运行状态', statusText.includes('运行中'));

    // 7. 停用：应当至少关一次服务器。
    extension.deactivate();
    await sleep(800);
    check('deactivate 关闭了语言服务器', stopCalls >= 1);
    check('deactivate 关闭了预览 Webview', !!(previewPanel && previewPanel.disposed));

    // 8. 模拟 VS Code 释放扩展资源：每个订阅都应当能安全 dispose。
    for (const item of context.subscriptions.items) {
      try {
        if (item && typeof item.dispose === 'function') item.dispose();
      } catch {
        // 单个释放失败不该影响其它。
      }
    }
    check('订阅可以正常释放', state.disposables >= 1);
  } finally {
    Module._load = originalLoad;
    lspModule.LspClient.prototype.stop = originalStop;
    if (context) extension.deactivate();
  }

  console.log(`\n共 ${checks} 项检查，失败 ${failures} 项`);
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((error) => {
  console.error('集成测试异常：', error);
  process.exit(1);
});
