'use strict';

/**
 * 用 `vsce` 打 VSIX（`npm run package` 的自写 ZIP 路线的替代品）。
 *
 * `npm run package` 用 Node 内置 zlib 自己写 ZIP，零依赖、离线可用；这条路走官方
 * `vsce`，校验更严（README 链接、图标尺寸、仓库字段都会查），产物也能直接
 * `vsce publish` 到市场。两条路的产物名对齐：`dist/gram-editor-<版本>-<平台>.vsix`。
 *
 * 仓库在 Gitee 上，vsce 只自动识别 GitHub / GitLab，所以这里显式传
 * `--baseContentUrl` / `--baseImagesUrl`，README 里的链接才不会被判成会失效。
 *
 * 用法（在 vscode/ 里跑）：
 *   npm run package:vsce                 # 内嵌 → 清单自检 → 测试 → vsce package
 *   npm run package:vsce -- --skip-tests # 只内嵌 + 打包
 *   npm run package:vsce -- --dry-run    # 只打印要跑的命令
 *   npm run package:vsce -- --out other-dir
 *   npm run package:vsce -- --target win32-x64   # 显式指定平台（默认用当前平台）
 *
 * `vsce` 优先用 PATH 上的那份；没有就退回 `npx --yes @vscode/vsce`（需要网络）。
 */

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const EXT_ROOT = path.join(__dirname, '..');
const manifest = require(path.join(EXT_ROOT, 'package.json'));

/**
 * 取 `--xxx` 后面的值。
 *
 * @param {string} name 选项名（含 `--`）。
 * @param {string} fallback 没给时的默认值。
 * @returns {string} 选项值。
 */
function option(name, fallback) {
  const index = process.argv.indexOf(name);
  if (index < 0) return fallback;
  const value = process.argv[index + 1];
  if (!value || value.startsWith('--')) {
    console.error(`${name} 后面要跟一个值`);
    process.exit(2);
  }
  return value;
}

const DRY_RUN = process.argv.includes('--dry-run');
const SKIP_TESTS = process.argv.includes('--skip-tests');
const outDir = path.resolve(EXT_ROOT, option('--out', 'dist'));
const target = option('--target', `${process.platform}-${process.arch}`);
const vsix = path.join(outDir, `${manifest.name}-${manifest.version}-${target}.vsix`);

// 仓库在 Gitee 上——vsce 只自动识别 GitHub / GitLab，所以显式告诉它内容与图片的
// 根地址，README 里的相对链接才不会被判成"会失效"。相对路径以 vscode/ 为基准。
const REPO_ROOT = 'https://gitee.com/rabbitc999/ParseEngine/raw/master';
const baseContentUrl = option('--baseContentUrl', `${REPO_ROOT}/vscode`);
const baseImagesUrl = option('--baseImagesUrl', `${REPO_ROOT}/vscode`);

/**
 * 跑一条命令，失败就退出。
 *
 * @param {string} command 程序。
 * @param {string[]} args 参数。
 * @param {boolean} [shell] 是否经系统 shell（Windows 上的 vsce / npx 是 .cmd 垫片）。
 * @returns {void}
 */
function run(command, args, shell = false) {
  console.log(`\n$ ${command} ${args.join(' ')}`);
  if (DRY_RUN) return;
  // 走 shell 时把命令与参数拼成一行（带空格的参数加引号）——给 spawnSync 同时传
  // args 数组和 shell:true 会触发 Node 的 DEP0190 警告，这样写就没有了。
  const quote = (value) => (/[\s"]/.test(value) ? `"${value.replace(/"/g, '\\"')}"` : value);
  const result = shell
    ? spawnSync([command, ...args.map(quote)].join(' '), {
        cwd: EXT_ROOT,
        stdio: 'inherit',
        shell: true,
      })
    : spawnSync(command, args, {
        cwd: EXT_ROOT,
        stdio: 'inherit',
      });
  if (result.status !== 0) {
    console.error(`\n失败（退出码 ${result.status ?? 'null'}）：${command}`);
    process.exit(result.status || 1);
  }
}

/**
 * PATH 上有没有这个程序。
 *
 * @param {string} command 程序名。
 * @returns {boolean} 存在为真。
 */
function onPath(command) {
  const probe = process.platform === 'win32' ? 'where' : 'which';
  return spawnSync(probe, [command], { stdio: 'ignore' }).status === 0;
}

/**
 * 挑一个能用的 vsce。
 *
 * @returns {[string, string[]]} 命令与前置参数。
 */
function vsce() {
  if (onPath('vsce')) return ['vsce', []];
  console.log('（PATH 上没有 vsce，退回 npx --yes @vscode/vsce）');
  return ['npx', ['--yes', '@vscode/vsce']];
}

/**
 * 主流程。
 */
function main() {
  console.log(`== 打包 ${manifest.displayName || manifest.name} ${manifest.version} ==`);
  run(process.execPath, [path.join('tools', 'embed-compiler.js')]);
  run(process.execPath, [path.join('tools', 'check-manifest.js')]);
  if (!SKIP_TESTS) {
    run(process.execPath, [path.join('test', 'smoke.js')]);
    run(process.execPath, [path.join('test', 'extension-smoke.js')]);
  }

  const [command, prefix] = vsce();
  if (!DRY_RUN) fs.mkdirSync(outDir, { recursive: true });
  run(
    command,
    [
      ...prefix,
      'package',
      '--out', vsix,
      '--target', target,
      '--baseContentUrl', baseContentUrl,
      '--baseImagesUrl', baseImagesUrl,
      '--allow-missing-repository',
    ],
    process.platform === 'win32',
  );

  if (DRY_RUN) {
    console.log(`\n（dry-run：没有真的产出 ${path.relative(EXT_ROOT, vsix)}）`);
    return;
  }
  const size = fs.statSync(vsix).size;
  console.log(`\n已打包：${path.relative(EXT_ROOT, vsix)}（${(size / 1048576).toFixed(2)} MB）`);
  console.log(`安装：code --install-extension "${vsix}"`);
}

main();
