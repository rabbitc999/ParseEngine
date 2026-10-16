'use strict';

/**
 * 工具定位：找 ParseEngine（生成器 + 内嵌的 `.gram` 语言服务器）。
 *
 * 查找顺序（先命中先用）：
 *   1. 设置里的显式路径（`gram.parseEnginePath`）；
 *   2. 仓库里的 `build/bin/`（**只在真的身处源码仓库里时才查**，开发时刚 `make`
 *      出来的那份最新，不该被旧的打包副本盖住）；
 *   3. **插件自带的 `bin/`**——发布出去的 `.vsix` 里就有，装上就能用，
 *      不需要仓库、也不需要另装编译器；
 *   4. `PATH` 里的 `ParseEngine`。
 *
 * 语言服务器不再单独找一个 `lsp-server`：它已经内嵌进生成器，用
 * `ParseEngine lsp --stdio` 启动、`ParseEngine lsp --check FILE` 校验。
 * 仍然支持 `gram.lspServerPath` 指到独立构建的 `lsp-server`（老配置 / 自定义构建），
 * 那条路用原来的 `--stdio` / `--check` 参数。
 *
 * 这个模块故意不依赖 `vscode`，因此可以在纯 Node 下单独测试（见 test/smoke.js）。
 */

const fs = require('node:fs');
const path = require('node:path');

/** 可执行文件后缀：Windows 上是 .exe，其余平台为空。 */
const EXE = process.platform === 'win32' ? '.exe' : '';

/** 插件自带的编译器目录（发布包里跟着走）。 */
const BUNDLED_DIR = path.resolve(__dirname, '..', 'bin');

/** 内嵌语言服务器需要的子命令：`ParseEngine lsp …`。 */
const LSP_SUBCOMMAND = 'lsp';

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
 * 插件自带的那份编译器（`<插件根>/bin/`）。
 *
 * @returns {string|null} 可执行文件路径。
 */
function findBundledCompiler() {
  return firstExisting([
    path.join(BUNDLED_DIR, `ParseEngine${EXE}`),
    path.join(BUNDLED_DIR, 'ParseEngine'),
  ]);
}

/**
 * 仓库里 `make -C source` 编出来的编译器。
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
 * 在 `PATH` 里找编译器。
 *
 * @returns {string|null} 可执行文件路径。
 */
function findOnPath() {
  const dirs = (process.env.PATH || '').split(path.delimiter);
  for (const dir of dirs) {
    if (!dir) continue;
    const candidate = firstExisting([
      path.join(dir, `ParseEngine${EXE}`),
      path.join(dir, 'ParseEngine'),
    ]);
    if (candidate) return candidate;
  }
  return null;
}

/**
 * 解析出本次要用的工具。
 *
 * 规则：先看设置里的显式路径，其次（身处源码仓库时）仓库里刚编出来的，
 * 再退到插件自带的 `bin/`，最后查 `PATH`。工作区根目录优先用作向上查找的起点
 * （`workspaceFolders` 为空时退回进程工作目录）。
 *
 * @param {string} workspaceDir 工作区目录；没有就传空串。
 * @param {{lspServerPath?: string, parseEnginePath?: string}} [configured] 设置里的路径。
 * @returns {{repoRoot: string|null, parseEngine: string|null, bundled: string|null,
 *            lspCommand: string|null, lspArgs: string[], checkArgs: string[],
 *            standaloneServer: boolean}}
 *          工具集合：`parseEngine` 是生成器；`lspCommand` + `lspArgs` 起语言服务器，
 *          `checkArgs` 后面接文件路径就是一次 `--check`。
 */
function resolveTools(workspaceDir, configured) {
  // 1. 先定仓库根：显式路径所在目录也能当起点。
  const startDir =
    workspaceDir ||
    (configured && configured.parseEnginePath
      ? path.dirname(configured.parseEnginePath)
      : '') ||
    process.cwd();
  const repoRoot = findRepoRoot(startDir);

  // 2. 编译器：显式路径 → 仓库（开发态）→ 插件自带 → PATH。
  const bundled = findBundledCompiler();
  const fromRepo = findParseEngine(repoRoot);
  const parseEngine =
    (configured && configured.parseEnginePath && configured.parseEnginePath.trim()) ||
    fromRepo ||
    bundled ||
    findOnPath();

  // 3. 语言服务器：显式指到独立 lsp-server 就用它（原来的参数形状），
  //    否则用内嵌的那份（`ParseEngine lsp`）。
  const standalone =
    (configured && configured.lspServerPath && configured.lspServerPath.trim()) || '';
  const lspCommand = standalone || parseEngine || null;
  const useSubcommand = !standalone && !!parseEngine;

  return {
    repoRoot,
    parseEngine: parseEngine || null,
    bundled: bundled || null,
    lspCommand,
    lspArgs: useSubcommand ? [LSP_SUBCOMMAND, '--stdio'] : ['--stdio'],
    checkArgs: useSubcommand ? [LSP_SUBCOMMAND, '--check'] : ['--check'],
    standaloneServer: !!standalone,
  };
}

module.exports = {
  EXE,
  BUNDLED_DIR,
  LSP_SUBCOMMAND,
  findRepoRoot,
  findBundledCompiler,
  findParseEngine,
  findOnPath,
  resolveTools,
};
