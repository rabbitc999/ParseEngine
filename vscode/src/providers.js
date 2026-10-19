'use strict';

/**
 * VS Code 语言能力：悬停、补全、跳转定义、文档大纲、格式化、快速修复。
 *
 * 这里只做「翻译」——把内嵌语言服务器（`ParseEngine lsp --stdio`）的结果换成
 * VS Code 类型，协议细节都在 src/lspClient.js 里。所有能力在服务器没起来时都
 * 安静地返回空结果。
 */

const vscode = require('vscode');

/** 语言 id，与 package.json 里的 contributes.languages 保持一致。 */
const LANGUAGE_ID = 'gram';

/** 这个插件只认 `.gram`。 */
const isSupportedLanguage = (languageId) => languageId === LANGUAGE_ID;

/** 语言选择器：磁盘文件与未命名文档都要支持。 */
const SELECTOR = [
  { language: LANGUAGE_ID, scheme: 'file' },
  { language: LANGUAGE_ID, scheme: 'untitled' },
];

/**
 * 把 LSP 位置换成 VS Code 位置。
 *
 * LSP 与 VS Code 都是 0 基行列、UTF-16 代码单元，直接映射即可。
 *
 * @param {{line: number, character: number}} position LSP 位置。
 * @returns {vscode.Position} VS Code 位置。
 */
function toPosition(position) {
  return new vscode.Position(position.line, position.character);
}

/**
 * 把 LSP 范围换成 VS Code 范围。
 *
 * @param {{start: object, end: object}} range LSP 范围。
 * @returns {vscode.Range} VS Code 范围。
 */
function toRange(range) {
  return new vscode.Range(toPosition(range.start), toPosition(range.end));
}

/**
 * LSP severity → VS Code severity。
 *
 * @param {number|undefined} severity LSP 级别（1..4）。
 * @returns {vscode.DiagnosticSeverity} VS Code 级别。
 */
function toDiagnosticSeverity(severity) {
  switch (severity) {
    case 1:
      return vscode.DiagnosticSeverity.Error;
    case 2:
      return vscode.DiagnosticSeverity.Warning;
    case 3:
      return vscode.DiagnosticSeverity.Information;
    default:
      return vscode.DiagnosticSeverity.Hint;
  }
}

/**
 * VS Code severity → LSP severity（发 codeAction 时要用）。
 *
 * @param {vscode.DiagnosticSeverity} severity VS Code 级别。
 * @returns {number} LSP 级别。
 */
function fromDiagnosticSeverity(severity) {
  switch (severity) {
    case vscode.DiagnosticSeverity.Error:
      return 1;
    case vscode.DiagnosticSeverity.Warning:
      return 2;
    case vscode.DiagnosticSeverity.Information:
      return 3;
    default:
      return 4;
  }
}

/**
 * LSP CompletionItemKind → VS Code CompletionItemKind。
 *
 * @param {number|undefined} kind LSP 种类。
 * @returns {vscode.CompletionItemKind} VS Code 种类。
 */
function toCompletionKind(kind) {
  const map = {
    2: vscode.CompletionItemKind.Method,
    3: vscode.CompletionItemKind.Function,
    5: vscode.CompletionItemKind.Field,
    6: vscode.CompletionItemKind.Variable,
    7: vscode.CompletionItemKind.Class,
    8: vscode.CompletionItemKind.Interface,
    9: vscode.CompletionItemKind.Module,
    10: vscode.CompletionItemKind.Property,
    13: vscode.CompletionItemKind.Enum,
    14: vscode.CompletionItemKind.Keyword,
    15: vscode.CompletionItemKind.Snippet,
    21: vscode.CompletionItemKind.Constant,
    22: vscode.CompletionItemKind.Struct,
    25: vscode.CompletionItemKind.TypeParameter,
  };
  return map[kind] || vscode.CompletionItemKind.Keyword;
}

/**
 * LSP SymbolKind → VS Code SymbolKind。
 *
 * @param {number|undefined} kind LSP 种类。
 * @returns {vscode.SymbolKind} VS Code 种类。
 */
function toSymbolKind(kind) {
  const map = {
    2: vscode.SymbolKind.Module,
    3: vscode.SymbolKind.Namespace,
    5: vscode.SymbolKind.Class,
    6: vscode.SymbolKind.Method,
    7: vscode.SymbolKind.Property,
    8: vscode.SymbolKind.Field,
    9: vscode.SymbolKind.Constructor,
    12: vscode.SymbolKind.Function,
    13: vscode.SymbolKind.Variable,
    14: vscode.SymbolKind.Constant,
    22: vscode.SymbolKind.EnumMember,
    23: vscode.SymbolKind.Struct,
  };
  return map[kind] || vscode.SymbolKind.Variable;
}

/**
 * 把服务器推送的诊断转成 VS Code 诊断（一次推送即全量）。
 *
 * @param {Array<any>} diagnostics LSP 诊断列表。
 * @returns {vscode.Diagnostic[]} VS Code 诊断列表。
 */
function toDiagnostics(diagnostics) {
  return (diagnostics || []).map((diagnostic) => {
    const item = new vscode.Diagnostic(
      toRange(diagnostic.range),
      diagnostic.message || '',
      toDiagnosticSeverity(diagnostic.severity),
    );

    // 来源与短码保留下来，方便用户按 code 过滤或抑制。
    item.source = diagnostic.source || 'gram-lsp';
    if (diagnostic.code) item.code = diagnostic.code;
    return item;
  });
}

/**
 * 注册全部语言能力。
 *
 * @param {vscode.ExtensionContext} context 扩展上下文。
 * @param {{getClient: () => any, logLine: (text: string) => void}} deps 依赖：
 *        getClient 取当前 LSP 客户端（可能为 null），logLine 写输出面板。
 */
function registerProviders(context, deps) {
  /**
   * 取一个可用的客户端；没有就返回 null。
   *
   * @returns {any|null} LSP 客户端。
   */
  const usableClient = () => {
    const client = deps.getClient();
    return client && client.running ? client : null;
  };

  /**
   * 发请求并兜住异常：语言服务出问题不该把编辑器搞崩。
   *
   * @param {string} method LSP 方法名。
   * @param {object} params 参数。
   * @returns {Promise<any>} 结果；失败时返回 null。
   */
  const request = async (method, params, document, token) => {
    const client = usableClient();
    if (!client || token?.isCancellationRequested) return null;
    deps.flushDocument?.(document);
    const version = document.version;
    try {
      const result = await client.request(method, params, token);
      return token?.isCancellationRequested || document.isClosed || document.version !== version || deps.getClient() !== client ? null : result;
    } catch (error) {
      if (error.name === 'AbortError') return null;
      deps.logLine(`[请求失败] ${method}：${error.message}`);
      return null;
    }
  };

  context.subscriptions.push(
    vscode.languages.registerHoverProvider(SELECTOR, {
      /**
       * 悬停：显示指令 / Token / 非终结符的说明。
       *
       * @param {vscode.TextDocument} document 文档。
       * @param {vscode.Position} position 位置。
       * @returns {Promise<vscode.Hover|null>} 悬停内容。
       */
      provideHover: async (document, position, token) => {
        const result = await request('textDocument/hover', {
          textDocument: { uri: document.uri.toString() },
          position: { line: position.line, character: position.character },
        }, document, token);
        if (!result || !result.contents) return null;

        // 1. 协议允许 contents 是字符串、{value} 对象或数组，这里都兼容。
        const parts = [];
        const items = Array.isArray(result.contents) ? result.contents : [result.contents];
        for (const item of items) {
          if (typeof item === 'string') parts.push(item);
          else if (item && item.value) parts.push(item.value);
        }
        if (!parts.length) return null;

        // 2. 用 markdown 渲染；带上范围可以让 VS Code 高亮鼠标压住的那段。
        const markdown = new vscode.MarkdownString(parts.join('\n\n'));
        markdown.isTrusted = false;
        const range = result.range ? toRange(result.range) : undefined;
        return new vscode.Hover(markdown, range);
      },
    }),

    vscode.languages.registerCompletionItemProvider(
      SELECTOR,
      {
        /**
         * 补全：行首补 %指令，取值处补候选，规则区补 Token / 非终结符。
         *
         * @param {vscode.TextDocument} document 文档。
         * @param {vscode.Position} position 位置。
         * @returns {Promise<vscode.CompletionItem[]>} 候选列表。
         */
        provideCompletionItems: async (document, position, token) => {
          const result = await request('textDocument/completion', {
            textDocument: { uri: document.uri.toString() },
            position: { line: position.line, character: position.character },
          }, document, token);
          const items = Array.isArray(result) ? result : (result && result.items) || [];

          // 逐条翻译；detail 与文档都原样带过去。
          return items.map((item) => {
            const completion = new vscode.CompletionItem(item.label, toCompletionKind(item.kind));
            if (item.detail) completion.detail = item.detail;
            completion.insertText = item.insertText || item.label;
            if (item.documentation && item.documentation.value) {
              completion.documentation = new vscode.MarkdownString(item.documentation.value);
            }
            return completion;
          });
        },
      },
      '%',
      '<',
      '$',
      // 空格：服务器在 triggerCharacters 里就声明了它，客户端原来没跟上。
      // 落后一步的后果是 `%token NAME ` 敲完空格补不出 include / alias。
      ' ',
    ),

    vscode.languages.registerDefinitionProvider(SELECTOR, {
      /**
       * 跳转定义：Token / 非终结符引用 → 声明处。
       *
       * @param {vscode.TextDocument} document 文档。
       * @param {vscode.Position} position 位置。
       * @returns {Promise<vscode.Location[]>} 定义位置。
       */
      provideDefinition: async (document, position, token) => {
        const result = await request('textDocument/definition', {
          textDocument: { uri: document.uri.toString() },
          position: { line: position.line, character: position.character },
        }, document, token);
        if (!Array.isArray(result)) return [];

        return result
          .filter((item) => item && item.uri && item.range)
          .map((item) => new vscode.Location(vscode.Uri.parse(item.uri), toRange(item.range)));
      },
    }),

    vscode.languages.registerDocumentSymbolProvider(SELECTOR, {
      /**
       * 文档大纲。
       *
       * @param {vscode.TextDocument} document 文档。
       * @returns {Promise<vscode.DocumentSymbol[]>} 符号树。
       */
      provideDocumentSymbols: async (document, token) => {
        const result = await request('textDocument/documentSymbol', {
          textDocument: { uri: document.uri.toString() },
        }, document, token);
        if (!Array.isArray(result)) return [];

        /**
         * 递归翻译一个符号节点。
         *
         * @param {any} node LSP 符号。
         * @returns {vscode.DocumentSymbol} VS Code 符号。
         */
        const convert = (node) => {
          const range = toRange(node.range || (node.location && node.location.range));
          const symbol = new vscode.DocumentSymbol(
            node.name || '?',
            '',
            toSymbolKind(node.kind),
            range,
            range,
          );
          for (const child of node.children || []) symbol.children.push(convert(child));
          return symbol;
        };

        return result.map(convert);
      },
    }),

    vscode.languages.registerDocumentFormattingEditProvider(SELECTOR, {
      /**
       * 格式化：服务器只做保守整理（去行尾空白、折叠空行、保证结尾换行）。
       *
       * @param {vscode.TextDocument} document 文档。
       * @returns {Promise<vscode.TextEdit[]>} 编辑列表。
       */
      provideDocumentFormattingEdits: async (document, options, token) => {
        const result = await request('textDocument/formatting', {
          textDocument: { uri: document.uri.toString() },
          options: { tabSize: 4, insertSpaces: true },
        }, document, token);
        if (!Array.isArray(result)) return [];

        return result
          .filter((edit) => edit && edit.range)
          .map((edit) => new vscode.TextEdit(toRange(edit.range), edit.newText || ''));
      },
    }),

    vscode.languages.registerCodeActionsProvider(
      SELECTOR,
      {
        /**
         * 快速修复：把光标处的诊断交给服务器，换回可应用的编辑。
         *
         * @param {vscode.TextDocument} document 文档。
         * @param {vscode.Range} range 选中的范围。
         * @param {vscode.CodeActionContext} context 诊断上下文。
         * @returns {Promise<vscode.CodeAction[]>} 可用的修复动作。
         */
        provideCodeActions: async (document, range, context, token) => {
          // 1. 把 VS Code 诊断转回 LSP 形状（服务器要看 range 与 message）。
          const diagnostics = context.diagnostics.map((diagnostic) => ({
            range: {
              start: {
                line: diagnostic.range.start.line,
                character: diagnostic.range.start.character,
              },
              end: {
                line: diagnostic.range.end.line,
                character: diagnostic.range.end.character,
              },
            },
            severity: fromDiagnosticSeverity(diagnostic.severity),
            code: typeof diagnostic.code === 'object' ? diagnostic.code.value : diagnostic.code,
            source: diagnostic.source,
            message: diagnostic.message,
          }));

          const result = await request('textDocument/codeAction', {
            textDocument: { uri: document.uri.toString() },
            range: {
              start: { line: range.start.line, character: range.start.character },
              end: { line: range.end.line, character: range.end.character },
            },
            context: { diagnostics },
          }, document, token);
          if (!Array.isArray(result)) return [];

          // 2. 逐条翻译成本地 CodeAction（本服务器只会编辑当前文件）。
          const actions = [];
          for (const item of result) {
            if (!item || !item.title) continue;

            const edits = [];
            const changes = item.edit && item.edit.changes ? item.edit.changes : {};
            for (const [uri, fileEdits] of Object.entries(changes)) {
              if (uri !== document.uri.toString()) continue;
              for (const edit of fileEdits) {
                edits.push(new vscode.TextEdit(toRange(edit.range), edit.newText || ''));
              }
            }
            if (!edits.length) continue;

            const action = new vscode.CodeAction(item.title, vscode.CodeActionKind.QuickFix);
            action.edit = new vscode.WorkspaceEdit();
            action.edit.set(document.uri, edits);
            actions.push(action);
          }
          return actions;
        },
      },
      { providedCodeActionKinds: [vscode.CodeActionKind.QuickFix] },
    ),
  );
}

module.exports = {
  LANGUAGE_ID,
  isSupportedLanguage,
  SELECTOR,
  registerProviders,
  toDiagnostics,
};
