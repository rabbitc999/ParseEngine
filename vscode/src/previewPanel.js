'use strict';

/** VS Code 解析预览 Webview：输入文本、展示 Token 与完整语法树并导出结果。 */

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const preview = require('./preview');

let currentPanel = null;

function csvCell(value) {
  const text = String(value == null ? '' : value);
  return /[",\r\n]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
}

function tokenCsv(tokens) {
  const lines = ['类型,词素,行,列'];
  for (const token of tokens) {
    lines.push([
      token.type,
      token.text,
      token.line,
      token.column,
    ].map(csvCell).join(','));
  }
  return `\uFEFF${lines.join('\r\n')}\r\n`;
}

class PreviewPanel {
  constructor(vscode, panel, options) {
    this.vscode = vscode;
    this.panel = panel;
    this.options = options;
    this.result = null;
    this.exportTargets = new Map();
    this.disposables = [];

    panel.webview.html = getHtml(panel.webview, path.basename(options.grammarFile));
    this.disposables.push(
      panel.webview.onDidReceiveMessage((message) => this.onMessage(message)),
      panel.onDidDispose(() => this.dispose()),
    );
  }

  update(options) {
    this.options = options;
    this.panel.title = `Gram 解析预览 - ${path.basename(options.grammarFile)}`;
    this.panel.reveal(this.vscode.ViewColumn.Beside);
    this.panel.webview.postMessage({
      type: 'grammar',
      name: path.basename(options.grammarFile),
    });
  }

  async onMessage(message) {
    if (!message || typeof message.type !== 'string') return;
    if (message.type === 'run') {
      const result = await preview.runPreview(this.options.parseEngine, {
        grammarFile: this.options.grammarFile,
        input: typeof message.input === 'string' ? message.input : '',
        mode: typeof message.mode === 'string' ? message.mode : 'ielr',
        pythonPath: this.options.pythonPath || '',
      });
      this.result = result;
      await this.panel.webview.postMessage({ type: 'result', result });
      return;
    }

    if (message.type === 'saveTokens') {
      await this.saveTokens();
      return;
    }

    if (message.type === 'saveTree') {
      await this.chooseTreeTarget();
      return;
    }

    if (message.type === 'treeExport') {
      await this.writeTreeExport(message);
    }
  }

  async saveTokens() {
    if (!this.result || !this.result.tokens.length) {
      void this.vscode.window.showInformationMessage('GramEditor：当前没有可保存的 Token。');
      return;
    }
    const name = path.basename(this.options.grammarFile, path.extname(this.options.grammarFile));
    const target = await this.vscode.window.showSaveDialog({
      defaultUri: this.vscode.Uri.file(path.join(path.dirname(this.options.grammarFile), `${name}-tokens.csv`)),
      filters: { 'CSV 表格': ['csv'] },
      saveLabel: '保存 Token 表',
    });
    if (!target) return;
    fs.writeFileSync(target.fsPath, tokenCsv(this.result.tokens), 'utf8');
    void this.vscode.window.showInformationMessage(`GramEditor：Token 表已保存到 ${target.fsPath}`);
  }

  async chooseTreeTarget() {
    if (!this.result || !this.result.trees.length) {
      void this.vscode.window.showInformationMessage('GramEditor：当前没有可保存的语法树。');
      return;
    }
    const name = path.basename(this.options.grammarFile, path.extname(this.options.grammarFile));
    const choice = await this.vscode.window.showQuickPick([
      { label: 'SVG', description: '矢量图，适合继续编辑和无损缩放', format: 'svg', extension: 'svg' },
      { label: 'PNG', description: '无损位图', format: 'png', extension: 'png' },
      { label: 'JPEG', description: '高质量有损位图', format: 'jpeg', extension: 'jpg' },
    ], { title: '语法树图片格式', placeHolder: '选择导出格式' });
    if (!choice) return;

    const target = await this.vscode.window.showSaveDialog({
      defaultUri: this.vscode.Uri.file(path.join(
        path.dirname(this.options.grammarFile), `${name}-tree.${choice.extension}`)),
      filters: { [choice.label]: [choice.extension] },
      saveLabel: '保存完整语法树',
    });
    if (!target) return;

    const requestId = crypto.randomBytes(12).toString('hex');
    this.exportTargets.set(requestId, target.fsPath);
    await this.panel.webview.postMessage({ type: 'exportTree', requestId, format: choice.format });
  }

  async writeTreeExport(message) {
    const target = this.exportTargets.get(message.requestId);
    if (!target) return;
    this.exportTargets.delete(message.requestId);

    try {
      if (message.error) throw new Error(String(message.error));
      if (message.format === 'svg') {
        fs.writeFileSync(target, String(message.content || ''), 'utf8');
      } else {
        const match = /^data:image\/(?:png|jpeg);base64,(.+)$/s.exec(String(message.content || ''));
        if (!match) throw new Error('Webview 返回的图片数据无效');
        fs.writeFileSync(target, Buffer.from(match[1], 'base64'));
      }
      void this.vscode.window.showInformationMessage(`GramEditor：语法树已保存到 ${target}`);
    } catch (error) {
      void this.vscode.window.showErrorMessage(`GramEditor：保存语法树失败：${error.message}`);
    }
  }

  dispose() {
    if (currentPanel === this) currentPanel = null;
    this.exportTargets.clear();
    while (this.disposables.length) {
      const disposable = this.disposables.pop();
      try { disposable.dispose(); } catch { /* 已释放。 */ }
    }
  }
}

function showPreview(vscode, options) {
  if (currentPanel) {
    currentPanel.update(options);
    return currentPanel;
  }
  const panel = vscode.window.createWebviewPanel(
    'gramPreview',
    `Gram 解析预览 - ${path.basename(options.grammarFile)}`,
    vscode.ViewColumn.Beside,
    { enableScripts: true, retainContextWhenHidden: true },
  );
  currentPanel = new PreviewPanel(vscode, panel, options);
  return currentPanel;
}

function disposePreview() {
  if (!currentPanel) return;
  const instance = currentPanel;
  currentPanel = null;
  instance.dispose();
  try { instance.panel.dispose(); } catch { /* 扩展宿主可能已先释放面板。 */ }
}

function getHtml(webview, grammarName) {
  const nonce = crypto.randomBytes(16).toString('base64');
  const safeName = grammarName.replace(/[&<>"']/g, (character) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  })[character]);
  return `<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta http-equiv="Content-Security-Policy"
        content="default-src 'none'; img-src data: blob:; style-src 'nonce-${nonce}'; script-src 'nonce-${nonce}';">
  <title>Gram 解析预览</title>
  <style nonce="${nonce}">
    * { box-sizing: border-box; }
    body { margin: 0; color: var(--vscode-foreground); background: var(--vscode-editor-background); font: 13px var(--vscode-font-family); }
    main { display: grid; grid-template-rows: auto 150px auto minmax(260px, 1fr); height: 100vh; min-width: 520px; }
    .toolbar { display: flex; align-items: center; gap: 6px; padding: 8px 10px; border-bottom: 1px solid var(--vscode-panel-border); }
    .title { font-weight: 600; margin-right: auto; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    button, select { height: 28px; border: 1px solid var(--vscode-button-border, transparent); color: var(--vscode-button-foreground); background: var(--vscode-button-background); padding: 0 10px; }
    button:hover { background: var(--vscode-button-hoverBackground); }
    button:disabled { opacity: .55; }
    select { color: var(--vscode-dropdown-foreground); background: var(--vscode-dropdown-background); border-color: var(--vscode-dropdown-border); }
    textarea { width: 100%; height: 100%; resize: none; border: 0; border-bottom: 1px solid var(--vscode-panel-border); outline: none; padding: 10px; color: var(--vscode-input-foreground); background: var(--vscode-input-background); font: 13px var(--vscode-editor-font-family); }
    textarea:focus { box-shadow: inset 0 0 0 1px var(--vscode-focusBorder); }
    .resultbar { display: flex; align-items: center; gap: 4px; padding: 6px 10px; border-bottom: 1px solid var(--vscode-panel-border); }
    .tab { color: var(--vscode-foreground); background: transparent; border: 0; border-bottom: 2px solid transparent; }
    .tab.active { border-bottom-color: var(--vscode-focusBorder); }
    #status { margin-left: auto; max-width: 55%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--vscode-descriptionForeground); }
    #status.error { color: var(--vscode-errorForeground); }
    .pane { display: none; min-height: 0; overflow: auto; }
    .pane.active { display: block; }
    table { width: 100%; border-collapse: collapse; }
    th { position: sticky; top: 0; z-index: 1; text-align: left; background: var(--vscode-editorGroupHeader-tabsBackground); }
    th, td { padding: 6px 10px; border-bottom: 1px solid var(--vscode-panel-border); }
    td:nth-child(2) { white-space: pre-wrap; font-family: var(--vscode-editor-font-family); }
    #treePane { background: #fff; }
    #treeHost { width: max-content; min-width: 100%; min-height: 100%; padding: 8px; }
    #treeHost svg { display: block; }
  </style>
</head>
<body>
  <main>
    <div class="toolbar">
      <span class="title" id="grammarName">${safeName}</span>
      <select id="mode" title="解析模式">
        <option value="lalr">LALR(1)</option>
        <option value="lr1">LR(1)</option>
        <option value="ielr" selected>IELR(1)</option>
        <option value="glr">GLR</option>
      </select>
      <button id="run" title="解析输入 (Ctrl+Enter)">解析</button>
      <button id="saveTokens" title="保存为 UTF-8 CSV" disabled>保存 Token</button>
      <button id="saveTree" title="保存为 PNG、JPEG 或 SVG" disabled>保存树图</button>
    </div>
    <textarea id="input" spellcheck="false" aria-label="待解析文本"></textarea>
    <div class="resultbar">
      <button class="tab active" data-pane="tokensPane">Token</button>
      <button class="tab" data-pane="treePane">语法树</button>
      <span id="status">尚未解析</span>
    </div>
    <section id="tokensPane" class="pane active">
      <table><thead><tr><th>类型</th><th>词素</th><th>位置</th></tr></thead><tbody id="tokens"></tbody></table>
    </section>
    <section id="treePane" class="pane"><div id="treeHost"></div></section>
  </main>
  <script nonce="${nonce}">
    const vscode = acquireVsCodeApi();
    const input = document.getElementById('input');
    const mode = document.getElementById('mode');
    const runButton = document.getElementById('run');
    const saveTokens = document.getElementById('saveTokens');
    const saveTree = document.getElementById('saveTree');
    const status = document.getElementById('status');
    const tokenBody = document.getElementById('tokens');
    const treeHost = document.getElementById('treeHost');
    let currentSvg = '';
    let treeWidth = 0;
    let treeHeight = 0;

    function esc(value) {
      return String(value == null ? '' : value).replace(/[&<>"']/g, function(character) {
        return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&apos;' }[character];
      });
    }

    function nodeLabel(node) {
      let label = String(node.name || '?');
      if (node.text) label += '  "' + String(node.text).replace(/\\n/g, '\\\\n') + '"';
      if (node.line > 0) label += '  ' + node.line + ':' + node.column;
      return label;
    }

    function shortLabel(label, width) {
      const limit = Math.max(10, Math.floor((width - 18) / 7));
      return label.length > limit ? label.slice(0, Math.max(1, limit - 1)) + '…' : label;
    }

    function buildTreeSvg(trees) {
      if (!trees.length) return '';
      const root = trees.length === 1 ? trees[0] : {
        name: 'GLR 结果 (' + trees.length + ')', text: '', line: 0, column: 0, children: trees,
      };
      const horizontalGap = 20;
      const nodeHeight = 30;
      const verticalGap = 52;
      const margin = 22;
      let maxDepth = 0;

      function measure(node, depth) {
        const children = Array.isArray(node.children) ? node.children.map(function(child) {
          return measure(child, depth + 1);
        }) : [];
        const label = nodeLabel(node);
        const nodeWidth = Math.max(100, Math.min(320, 22 + Array.from(label).length * 7));
        const childWidth = children.reduce(function(sum, child) { return sum + child.width; }, 0)
          + Math.max(0, children.length - 1) * horizontalGap;
        maxDepth = Math.max(maxDepth, depth);
        return { source: node, label: label, nodeWidth: nodeWidth, width: Math.max(nodeWidth, childWidth), children: children, depth: depth };
      }

      const layoutRoot = measure(root, 0);
      const nodes = [];
      const edges = [];
      function place(node, left) {
        const center = left + node.width / 2;
        const y = margin + node.depth * (nodeHeight + verticalGap);
        nodes.push({ node: node, x: center, y: y });
        let childrenWidth = node.children.reduce(function(sum, child) { return sum + child.width; }, 0)
          + Math.max(0, node.children.length - 1) * horizontalGap;
        let childLeft = left + (node.width - childrenWidth) / 2;
        for (const child of node.children) {
          const childCenter = childLeft + child.width / 2;
          const childY = margin + child.depth * (nodeHeight + verticalGap);
          edges.push({ x1: center, y1: y + nodeHeight, x2: childCenter, y2: childY });
          place(child, childLeft);
          childLeft += child.width + horizontalGap;
        }
      }
      place(layoutRoot, margin);

      treeWidth = Math.ceil(layoutRoot.width + margin * 2);
      treeHeight = Math.ceil(margin * 2 + (maxDepth + 1) * nodeHeight + maxDepth * verticalGap);
      let body = '<rect width="100%" height="100%" fill="#ffffff"/>';
      for (const edge of edges) {
        body += '<line x1="' + edge.x1 + '" y1="' + edge.y1 + '" x2="' + edge.x2 + '" y2="' + edge.y2 + '" stroke="#9aa0a6" stroke-width="1.5"/>';
      }
      for (const item of nodes) {
        const leaf = item.node.children.length === 0;
        const x = item.x - item.node.nodeWidth / 2;
        body += '<g><title>' + esc(item.node.label) + '</title>'
          + '<rect x="' + x + '" y="' + item.y + '" width="' + item.node.nodeWidth + '" height="' + nodeHeight
          + '" rx="4" fill="' + (leaf ? '#eaf6ee' : '#f5f6f7') + '" stroke="' + (leaf ? '#2e7d4f' : '#5f6368') + '"/>'
          + '<text x="' + item.x + '" y="' + (item.y + 19) + '" text-anchor="middle" font-family="Consolas,monospace" font-size="12" fill="#202124">'
          + esc(shortLabel(item.node.label, item.node.nodeWidth)) + '</text></g>';
      }
      return '<svg xmlns="http://www.w3.org/2000/svg" width="' + treeWidth + '" height="' + treeHeight
        + '" viewBox="0 0 ' + treeWidth + ' ' + treeHeight + '">' + body + '</svg>';
    }

    function renderTokens(tokens) {
      tokenBody.textContent = '';
      for (const token of tokens) {
        const row = document.createElement('tr');
        for (const value of [token.type, token.text, token.line + ':' + token.column]) {
          const cell = document.createElement('td');
          cell.textContent = value == null ? '' : String(value);
          row.appendChild(cell);
        }
        tokenBody.appendChild(row);
      }
    }

    function showPane(id) {
      document.querySelectorAll('.pane').forEach(function(item) { item.classList.toggle('active', item.id === id); });
      document.querySelectorAll('.tab').forEach(function(item) { item.classList.toggle('active', item.dataset.pane === id); });
    }

    function run() {
      runButton.disabled = true;
      saveTokens.disabled = true;
      saveTree.disabled = true;
      status.className = '';
      status.textContent = '正在生成并解析…';
      vscode.postMessage({ type: 'run', input: input.value, mode: mode.value });
    }

    document.querySelectorAll('.tab').forEach(function(item) {
      item.addEventListener('click', function() { showPane(item.dataset.pane); });
    });
    runButton.addEventListener('click', run);
    input.addEventListener('keydown', function(event) {
      if (event.ctrlKey && event.key === 'Enter') { event.preventDefault(); run(); }
    });
    saveTokens.addEventListener('click', function() { vscode.postMessage({ type: 'saveTokens' }); });
    saveTree.addEventListener('click', function() { vscode.postMessage({ type: 'saveTree' }); });

    window.addEventListener('message', function(event) {
      const message = event.data;
      if (message.type === 'grammar') {
        document.getElementById('grammarName').textContent = message.name;
        return;
      }
      if (message.type === 'result') {
        const result = message.result;
        runButton.disabled = false;
        renderTokens(result.tokens || []);
        currentSvg = buildTreeSvg(result.trees || []);
        treeHost.innerHTML = currentSvg;
        saveTokens.disabled = !(result.tokens || []).length;
        saveTree.disabled = !(result.trees || []).length;
        status.className = result.ok ? '' : 'error';
        status.textContent = result.ok
          ? ((result.tokens || []).length + ' 个 Token，' + (result.trees || []).length + ' 棵语法树')
          : ((result.errors || []).join('；') || '解析失败');
        if ((result.trees || []).length) showPane('treePane');
        return;
      }
      if (message.type === 'exportTree') exportTree(message);
    });

    function exportTree(message) {
      if (!currentSvg) {
        vscode.postMessage({ type: 'treeExport', requestId: message.requestId, error: '没有可导出的语法树' });
        return;
      }
      if (message.format === 'svg') {
        vscode.postMessage({ type: 'treeExport', requestId: message.requestId, format: 'svg', content: currentSvg });
        return;
      }
      const blob = new Blob([currentSvg], { type: 'image/svg+xml;charset=utf-8' });
      const url = URL.createObjectURL(blob);
      const image = new Image();
      image.onload = function() {
        try {
          const scale = Math.max(1, Math.min(2, 16000 / treeWidth, 16000 / treeHeight));
          const canvas = document.createElement('canvas');
          canvas.width = Math.ceil(treeWidth * scale);
          canvas.height = Math.ceil(treeHeight * scale);
          const context = canvas.getContext('2d');
          context.fillStyle = '#ffffff';
          context.fillRect(0, 0, canvas.width, canvas.height);
          context.scale(scale, scale);
          context.drawImage(image, 0, 0);
          const mime = message.format === 'jpeg' ? 'image/jpeg' : 'image/png';
          const data = canvas.toDataURL(mime, message.format === 'jpeg' ? 0.92 : undefined);
          vscode.postMessage({ type: 'treeExport', requestId: message.requestId, format: message.format, content: data });
        } catch (error) {
          vscode.postMessage({ type: 'treeExport', requestId: message.requestId, error: error.message });
        } finally {
          URL.revokeObjectURL(url);
        }
      };
      image.onerror = function() {
        URL.revokeObjectURL(url);
        vscode.postMessage({ type: 'treeExport', requestId: message.requestId, error: 'SVG 渲染失败' });
      };
      image.src = url;
    }
  </script>
</body>
</html>`;
}

module.exports = {
  disposePreview,
  tokenCsv,
  showPreview,
};
