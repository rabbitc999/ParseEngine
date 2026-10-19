'use strict';
/** @file 基于原生容错扫描的工作区符号索引、包含链导航与保守重命名。 */
const fs = require('node:fs/promises');
const path = require('node:path');
const vscode = require('vscode');
const { SELECTOR, isSupportedLanguage } = require('./providers');

/** @brief 统一已有文件和符号链接的身份，同时保留原始 URI 供编辑器定位。 */
async function fileKey(file) {
  let result;
  try { result = await fs.realpath(file); } catch { result = path.resolve(file); }
  return process.platform === 'win32' ? result.toLowerCase() : result;
}

/** @brief 判断 UTF-16 位置是否在左闭右开范围内。 */
function contains(range, position) {
  const before = (a, b) => a.line < b.line || (a.line === b.line && a.character < b.character);
  return !before(position, range.start) && before(position, range.end);
}

/** @brief 建立按文本内容缓存的工作区索引；修改后只重新扫描发生变化的文件。 */
class NavigationIndex {
  constructor(deps) { this.deps = deps; this.cache = new Map(); this.client = null; }

  /** @brief 收集工作区、已打开文档以及递归包含文件，未保存文本优先。 */
  async gather(document, token) {
    const client = this.deps.getClient();
    if (!client?.running || token?.isCancellationRequested) return null;
    if (client !== this.client) { this.cache.clear(); this.client = client; }
    this.deps.flushDocument?.(document);
    if (document.uri.scheme !== 'file') {
      const text = document.getText(), version = document.version;
      const model = await client.request('gram/symbolIndex', { text }, token);
      if (model.includes.length) throw new Error('请先保存文法，以便解析包含文件的位置。');
      const key = document.uri.toString();
      const current = { ...model, key, uri: document.uri, editor: document, version, text, includes: [] };
      return { client, local: true, current, files: new Map([[key, current]]) };
    }
    const open = new Map();
    for (const item of vscode.workspace.textDocuments) {
      if (isSupportedLanguage(item.languageId) && item.uri.scheme === 'file') open.set(await fileKey(item.uri.fsPath), item);
    }
    const uris = vscode.workspace.findFiles
      ? await vscode.workspace.findFiles('**/*.gram', '**/{.git,node_modules,.parseengine-cache,target}/**', 10001, token) : [];
    if (uris.length > 10000) throw new Error('工作区超过 10000 份文法，请缩小工作区后重试，以免遗漏引用。');
    const queue = [...uris, ...[...open.values()].map(item => item.uri), document.uri];
    const files = new Map();
    for (let index = 0; index < queue.length; ++index) {
      if (token?.isCancellationRequested || this.deps.getClient() !== client) return null;
      const uri = queue[index], key = await fileKey(uri.fsPath);
      if (files.has(key)) continue;
      if (files.size >= 10000) throw new Error('包含链超过 10000 份文法，请缩小工作区后重试。');
      const editor = open.get(key);
      let text;
      try { text = editor ? editor.getText() : await fs.readFile(uri.fsPath, 'utf8'); }
      catch {
        files.set(key, { key, uri, missing: true, includes: [], occurrences: [], code: [] });
        continue;
      }
      let cached = this.cache.get(key);
      if (!cached || cached.text !== text) {
        const model = await client.request('gram/symbolIndex', { text }, token);
        cached = { text, model }; this.cache.set(key, cached);
      }
      const includes = [];
      for (const include of cached.model.includes) {
        const included = path.resolve(path.dirname(uri.fsPath), include);
        includes.push(await fileKey(included)); queue.push(vscode.Uri.file(included));
      }
      files.set(key, { ...cached.model, key, uri: editor?.uri || uri, editor, version: editor?.version, text, includes });
    }
    for (const key of this.cache.keys()) if (!files.has(key)) this.cache.delete(key);
    return { client, files, current: files.get(await fileKey(document.uri.fsPath)) };
  }

  /** @brief 取得当前文法通过包含链可见的文件。 */
  closure(snapshot, key) {
    snapshot.closures ||= new Map();
    if (snapshot.closures.has(key)) return snapshot.closures.get(key);
    const result = new Set(), pending = [key];
    while (pending.length) {
      const next = pending.pop(); if (result.has(next)) continue;
      result.add(next); pending.push(...(snapshot.files.get(next)?.includes || []));
    }
    snapshot.closures.set(key, result); return result;
  }

  /** @brief 沿反向包含边查找入口；共享片段可以有多个入口作用域。 */
  scopes(snapshot, key) {
    if (!snapshot.parents) {
      snapshot.parents = new Map();
      for (const file of snapshot.files.values()) for (const include of file.includes) {
        if (!snapshot.parents.has(include)) snapshot.parents.set(include, new Set());
        snapshot.parents.get(include).add(file.key);
      }
    }
    snapshot.scopes ||= new Map();
    if (!snapshot.scopes.has(key)) {
      const visited = new Set(), pending = [key], roots = new Set();
      while (pending.length) {
        const file = pending.pop(); if (visited.has(file)) continue;
        visited.add(file);
        const parents = snapshot.parents.get(file);
        if (!parents?.size) roots.add(file); else pending.push(...parents);
      }
      // 循环包含没有根时保守使用整条闭包，歧义仍由声明检查拒绝。
      snapshot.scopes.set(key, [...(roots.size ? roots : visited)]);
    }
    return snapshot.scopes.get(key);
  }

  /** @brief 每个入口仅建立一次声明表，避免每个引用重复遍历大型包含图。 */
  declarations(snapshot, key, name) {
    snapshot.symbols ||= new Map();
    const result = new Set();
    for (const scope of this.scopes(snapshot, key)) {
      if (!snapshot.symbols.has(scope)) {
        const table = new Map();
        for (const file of this.closure(snapshot, scope)) for (const item of snapshot.files.get(file)?.occurrences || []) {
          if (!item.declaration) continue;
          if (!table.has(item.name)) table.set(item.name, { strong: new Set(), weak: new Set() });
          table.get(item.name)[item.priority ? 'weak' : 'strong'].add(file);
        }
        snapshot.symbols.set(scope, table);
      }
      const entry = snapshot.symbols.get(scope).get(name);
      const declarations = entry ? (entry.strong.size ? entry.strong : entry.weak) : [];
      for (const declaration of declarations) result.add(declaration);
    }
    return [...result];
  }

  /** @brief 取得光标符号及其声明身份，拒绝隐式或歧义符号重命名。 */
  target(snapshot, position) {
    const occurrence = snapshot?.current?.occurrences.find(item => contains(item.range, position));
    if (!occurrence) return null;
    const declarations = this.declarations(snapshot, snapshot.current.key, occurrence.name);
    const kinds = new Set();
    for (const file of declarations) for (const item of snapshot.files.get(file).occurrences) {
      if (item.name === occurrence.name && item.declaration && item.kind) kinds.add(item.kind);
    }
    return { ...occurrence, declarations: kinds.size > 1 ? [] : declarations };
  }

  /** @brief 编辑提交前核对所有缓冲区和磁盘文本，防止异步索引覆盖新内容。 */
  async fresh(snapshot, token) {
    if (token?.isCancellationRequested || this.deps.getClient() !== snapshot.client) return false;
    const observed = new Map();
    for (const editor of snapshot.local ? [] : vscode.workspace.textDocuments) {
      if (!isSupportedLanguage(editor.languageId) || editor.uri.scheme !== 'file') continue;
      const file = snapshot.files.get(await fileKey(editor.uri.fsPath));
      if (!file || editor.getText() !== file.text) return false;
      observed.set(editor, editor.version);
    }
    for (const file of snapshot.files.values()) {
      if (file.missing) {
        try { await fs.access(file.uri.fsPath); return false; } catch { continue; }
      }
      if (file.editor) {
        if (file.editor.isClosed || file.editor.version !== file.version || file.editor.getText() !== file.text) return false;
      } else if (await fs.readFile(file.uri.fsPath, 'utf8') !== file.text) return false;
    }
    // 磁盘检查包含 await，返回前同步复核缓冲区，避免等待期间的编辑漏过验证。
    for (const [editor, version] of observed) if (editor.isClosed || editor.version !== version) return false;
    for (const file of snapshot.files.values()) if (file.editor &&
      (file.editor.isClosed || file.editor.version !== file.version || file.editor.getText() !== file.text)) return false;
    if (!snapshot.local && vscode.workspace.textDocuments.some(editor =>
      isSupportedLanguage(editor.languageId) && editor.uri.scheme === 'file' && !observed.has(editor))) return false;
    return !token?.isCancellationRequested && this.deps.getClient() === snapshot.client;
  }

  /** @brief 返回同一声明在整个工作区的使用位置。 */
  references(snapshot, target, includeDeclaration) {
    const result = [];
    for (const file of snapshot.files.values()) {
      const declarations = this.declarations(snapshot, file.key, target.name);
      if (declarations.length !== 1 || target.declarations.length !== 1 || declarations[0] !== target.declarations[0]) continue;
      for (const occurrence of file.occurrences) {
        const declaration = occurrence.declaration && (!occurrence.priority || !snapshot.files.get(declarations[0])
          .occurrences.some(item => item.name === target.name && item.declaration && !item.priority));
        if (occurrence.name === target.name && (includeDeclaration || !declaration)) result.push({ file, occurrence });
      }
    }
    return result;
  }
}

/** @brief 注册工作区引用、跨文件跳转和安全重命名能力。 */
function registerNavigation(context, deps) {
  const index = new NavigationIndex(deps);
  const range = item => new vscode.Range(new vscode.Position(item.start.line, item.start.character),
    new vscode.Position(item.end.line, item.end.character));
  const run = async (document, position, token, operation, rename = false) => {
    try {
      const snapshot = await index.gather(document, token);
      if (!snapshot) return null;
      const target = index.target(snapshot, position);
      if (!target) return null;
      const result = await operation(snapshot, target);
      return await index.fresh(snapshot, token) ? result : null;
    } catch (error) {
      if (error.name === 'AbortError') return null;
      if (rename) throw error;
      deps.logLine(`工作区导航：${error.message}`); return null;
    }
  };
  context.subscriptions.push(
    vscode.languages.registerReferenceProvider(SELECTOR, {
      provideReferences: (document, position, options, token) => run(document, position, token, (snapshot, target) =>
        index.references(snapshot, target, options.includeDeclaration).map(({ file, occurrence }) =>
          new vscode.Location(file.uri, range(occurrence.range)))),
    }),
    vscode.languages.registerRenameProvider(SELECTOR, {
      prepareRename: (document, position, token) => run(document, position, token, (_snapshot, target) => {
        if (target.declarations.length !== 1) throw new Error('符号没有唯一声明；请先补齐声明或消除包含链中的同名定义。');
        if (['error', 'NUMBER', 'STRING', 'VARIABLE'].includes(target.name)) throw new Error('内建符号不能通过文法重命名。');
        return { range: range(target.range), placeholder: target.name };
      }, true),
      provideRenameEdits: (document, position, newName, token) => run(document, position, token, (snapshot, target) => {
        if (!/^[A-Za-z_][A-Za-z_0-9]*$/.test(newName) || ['error', 'NUMBER', 'STRING', 'VARIABLE', 'regex', 'alias', 'include'].includes(newName))
          throw new Error('新名称必须是非保留的文法标识符。');
        if (target.declarations.length !== 1) throw new Error('符号没有唯一声明，不能安全重命名。');
        if (['error', 'NUMBER', 'STRING', 'VARIABLE'].includes(target.name)) throw new Error('内建符号不能重命名。');
        const identity = target.declarations[0];
        // 检查所有包含该定义的文法，连隐式符号也参与捕获检查。
        const related = new Set();
        for (const root of index.scopes(snapshot, identity)) for (const visible of index.closure(snapshot, root)) related.add(visible);
        for (const visible of related) {
          const model = snapshot.files.get(visible);
          if (model.missing) throw new Error(`无法读取包含文件 ${model.uri.fsPath}，不能保证重命名完整。`);
          if (model.incomplete) throw new Error('包含范围内存在未闭合的字符串、注释或代码块，请先修复后再重命名。');
          if (newName !== target.name && model.occurrences.some(item => item.name === newName))
            throw new Error(`新名称 ${newName} 会与包含范围内的符号冲突。`);
          if (model.code.some(code => (code.match(/[A-Za-z_][A-Za-z_0-9]*/g) || []).includes(target.name)))
            throw new Error('语义代码中使用了该名称，请先处理代码引用，再执行文法符号重命名。');
          const declarations = index.declarations(snapshot, visible, target.name);
          if (declarations.length !== 1) throw new Error('某个引用文法存在同名声明，不能安全重命名。');
        }
        const edit = new vscode.WorkspaceEdit(), grouped = new Map();
        for (const { file, occurrence } of index.references(snapshot, target, true)) {
          if (!grouped.has(file.uri)) grouped.set(file.uri, []);
          grouped.get(file.uri).push(new vscode.TextEdit(range(occurrence.range), newName));
        }
        for (const [uri, edits] of grouped) edit.set(uri, edits);
        return edit;
      }, true),
    }),
    vscode.languages.registerDefinitionProvider(SELECTOR, {
      provideDefinition: (document, position, token) => run(document, position, token, (snapshot, target) =>
        target.declarations.flatMap(key => {
          const file = snapshot.files.get(key);
          const strong = file.occurrences.some(item => item.name === target.name && item.declaration && !item.priority);
          return file.occurrences.filter(item => item.name === target.name && item.declaration && (!strong || !item.priority))
            .map(item => new vscode.Location(file.uri, range(item.range)));
        })),
    }),
    { dispose: () => index.cache.clear() },
  );
}

module.exports = { NavigationIndex, registerNavigation, contains };
