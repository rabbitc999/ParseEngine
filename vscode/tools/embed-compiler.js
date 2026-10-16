'use strict';

/**
 * 把 ParseEngine 内嵌进插件包（复制到本扩展的 `bin/` 下）。
 *
 * 语言服务器已经内嵌在生成器里（`ParseEngine lsp --stdio`），所以这一个可执行文件
 * 同时提供诊断 / 补全 / 生成 / 内嵌手册。打出来的 `.vsix` 自带它，装上就能用，
 * 不需要用户先构建工具链。`tools/package.js` 在打包前会自动跑本脚本。
 *
 * 用法（在 vscode/ 里或任意目录都行）：
 *   node tools/embed-compiler.js              # 自动找：--from → PARSEENGINE_BIN → 仓库 build/bin → PATH
 *   node tools/embed-compiler.js --from build/bin/ParseEngine.exe
 *   node tools/embed-compiler.js --check      # 只看 bin/ 里有没有，不复制
 *
 * 找不到就退出 2，并说明怎么构建（在仓库根跑 build.sh / make -C source）。
 */

const fs = require('node:fs');
const path = require('node:path');

const locator = require('../src/serverLocator');

/** 插件根目录。 */
const EXT_ROOT = path.join(__dirname, '..');

/** 内嵌编译器落地的目录。 */
const TARGET_DIR = path.join(EXT_ROOT, 'bin');

/** 内嵌编译器路径。 */
const TARGET = path.join(TARGET_DIR, `ParseEngine${locator.EXE}`);

/**
 * 是不是普通文件。
 *
 * @param {string} file 路径。
 * @returns {boolean} 是普通文件时为真。
 */
function isFile(file) {
  try {
    return fs.statSync(file).isFile();
  } catch {
    return false;
  }
}

/**
 * 取 `--from <路径>`。
 *
 * @returns {string} 参数值；没给返回空串。
 */
function fromArgument() {
  const index = process.argv.indexOf('--from');
  if (index < 0) return '';
  const value = process.argv[index + 1];
  if (!value) {
    console.error('--from 后面要跟编译器路径');
    process.exit(2);
  }
  return value;
}

/**
 * 按顺序找源编译器。
 *
 * @returns {{file: string|null, from: string}} 源文件与来源说明。
 */
function findSource() {
  const explicit = fromArgument() || (process.env.PARSEENGINE_BIN || '').trim();
  if (explicit) {
    if (!isFile(explicit)) {
      console.error(`找不到指定的编译器：${explicit}`);
      process.exit(2);
    }
    return { file: explicit, from: '参数 / PARSEENGINE_BIN' };
  }

  const candidates = [
    path.join(EXT_ROOT, '..', 'build', 'bin', `ParseEngine${locator.EXE}`),
    path.join(EXT_ROOT, '..', 'build', 'bin', 'release', `ParseEngine${locator.EXE}`),
  ];
  for (const candidate of candidates) {
    if (isFile(candidate)) return { file: candidate, from: '仓库 build/bin' };
  }

  const onPath = locator.findOnPath();
  if (onPath) return { file: onPath, from: 'PATH' };
  return { file: null, from: '' };
}

/**
 * 主流程。
 */
function main() {
  // 1. `--check`：只确认包里有编译器，供打包脚本在没构建时做判断。
  if (process.argv.includes('--check')) {
    if (!isFile(TARGET)) {
      console.error(`插件里还没有内嵌编译器：${path.relative(EXT_ROOT, TARGET)}`);
      console.error('先跑 `node tools/embed-compiler.js`（或 `node tools/package.js` 会自动跑）。');
      process.exit(1);
    }
    console.log(`已内嵌编译器：${path.relative(EXT_ROOT, TARGET)}`
      + `（${fs.statSync(TARGET).size} 字节）`);
    return;
  }

  // 2. 找源编译器。
  const source = findSource();
  if (!source.file) {
    console.error('找不到 ParseEngine 编译器：先在仓库根跑 build.sh 或 make -C source，');
    console.error('或用 `--from <路径>` / 环境变量 PARSEENGINE_BIN 指定。');
    process.exit(2);
  }

  // 3. 复制（内容一致就跳过）。
  fs.mkdirSync(TARGET_DIR, { recursive: true });
  const content = fs.readFileSync(source.file);
  const before = isFile(TARGET) ? fs.readFileSync(TARGET) : null;
  if (before && before.equals(content)) {
    console.log(`内嵌编译器已是最新：${path.relative(EXT_ROOT, TARGET)}`
      + `（${content.length} 字节，来自${source.from}）`);
    return;
  }
  fs.writeFileSync(TARGET, content);
  if (process.platform !== 'win32') fs.chmodSync(TARGET, 0o755);
  console.log(`已内嵌编译器：${path.relative(EXT_ROOT, TARGET)}`
    + `（${content.length} 字节，来自${source.from}：${source.file}）`);
}

main();
