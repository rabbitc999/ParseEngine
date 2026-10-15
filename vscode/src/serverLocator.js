'use strict';

/**
 * 工具定位：在仓库里找 lsp-server 与 ParseEngine。
 *
 * 这个模块故意不依赖 `vscode`，因此可以在纯 Node 下单独测试（见 test/smoke.js）。
 */

const fs = require('node:fs');
const path = require('node:path');

/** 可执行文件后缀：Windows 上是 .exe，其余平台为空。 */
const EXE = process.platform === 'win32' ? '.exe' : '';

/**
 * 从某个目录向上找仓库根。
 *
 * 判定标准：同时存在 AGENTS.md、source/templates 与 library/lsp。这组路径与当前
 * 仓库布局一致，也允许从 vscode/ 子目录或显式工具路径向上定位。
 *
 * @param {string} [startDir] 起始目录，默认取当前工作目录。
 * @returns {string|null} 仓库根绝对路径；找不到返回 null。
 */
function findRepoRoot(startDir) {
  let dir = path.resolve(startDir || process.cwd());

  // 1. 逐级向上，最多走到盘符根。
  while (true) {
    if (
      fs.existsSync(path.join(dir, 'AGENTS.md')) &&
      fs.existsSync(path.join(dir, 'source', 'templates')) &&
      fs.existsSync(path.join(dir, 'library', 'lsp'))
    ) {
      return dir;
    }

    const parent = path.dirname(dir);
    if (parent === dir) return null;
    dir = parent;
  }
}

/**
 * 返回第一个存在的候选路径。
 *
 * @param {string[]} candidates 候选路径。
 * @returns {string|null} 存在的路径；都不存在返回 null。
 */
function firstExisting(candidates) {
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) return candidate;
  }
  return null;
}

/**
 * 找 lsp-server。
 *
 * @param {string|null} repoRoot 仓库根。
 * @returns {string|null} 可执行文件路径。
 */
function findLspServer(repoRoot) {
  if (!repoRoot) return null;
  return firstExisting([
    path.join(repoRoot, 'build', 'lsp', `lsp-server${EXE}`),
    path.join(repoRoot, 'build', 'lsp', 'lsp-server'),
  ]);
}

/**
 * 找 ParseEngine。
 *
 * @param {string|null} repoRoot 仓库根。
 * @returns {string|null} 可执行文件路径。
 */
function findParseEngine(repoRoot) {
  if (!repoRoot) return null;
  return firstExisting([
    path.join(repoRoot, 'build', 'bin', `ParseEngine${EXE}`),
    path.join(repoRoot, 'build', 'bin', 'ParseEngine'),
    path.join(repoRoot, 'build', 'bin', 'release', `ParseEngine${EXE}`),
    path.join(repoRoot, 'build', 'bin', 'release', 'ParseEngine'),
  ]);
}

/**
 * 解析出本次要用的工具集合。
 *
 * 规则：先看设置里的显式路径，再按仓库约定自动探测；工作区根目录优先用作
 * 向上查找的起点（`workspaceFolders` 为空时退回进程工作目录）。
 *
 * @param {string} workspaceDir 工作区目录；没有就传空串。
 * @param {{lspServerPath?: string, parseEnginePath?: string}} [configured] 设置里的路径。
 * @returns {{repoRoot: string|null, lspServer: string|null, parseEngine: string|null}}
 */
function resolveTools(workspaceDir, configured) {
  // 1. 先定仓库根：显式路径所在目录也能当起点。
  const startDir =
    workspaceDir ||
    (configured && configured.lspServerPath ? path.dirname(configured.lspServerPath) : '') ||
    process.cwd();
  const repoRoot = findRepoRoot(startDir);

  // 2. 显式路径优先，其次自动探测。
  const lspServer =
    (configured && configured.lspServerPath && configured.lspServerPath.trim()) ||
    findLspServer(repoRoot);
  const parseEngine =
    (configured && configured.parseEnginePath && configured.parseEnginePath.trim()) ||
    findParseEngine(repoRoot);

  return {
    repoRoot,
    lspServer: lspServer || null,
    parseEngine: parseEngine || null,
  };
}

module.exports = {
  EXE,
  findRepoRoot,
  findLspServer,
  findParseEngine,
  resolveTools,
};
