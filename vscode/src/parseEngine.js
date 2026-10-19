'use strict';

/**
 * 调用 ParseEngine 的命令行封装：生成解析器、跑一次文法校验。
 *
 * 语言服务器内嵌在生成器里（`ParseEngine lsp --stdio`），所以这里只有生成与
 * `--check` 两条路；两者都不依赖 `vscode`，可以在纯 Node 下测试。
 */

const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');

/**
 * `--check` 每行诊断的正则。
 *
 * 输出形如：`D:\repo\example\x.gram:12:5: error: 消息 [code]`
 * 注意不能用 `split(':')`：Windows 路径自带盘符冒号，会把列切错。
 */
const DIAGNOSTIC_LINE = /^(?<file>.+):(?<line>\d+):(?<column>\d+):\s*(?<level>\w+)\s*:\s*(?<message>.*)$/;

/**
 * 跑一个子进程并收齐输出。
 *
 * @param {string} exe 可执行文件。
 * @param {string[]} args 参数列表。
 * @param {{cwd?: string, timeoutMs?: number, input?: string, signal?: AbortSignal}} [options] 选项。
 * @returns {Promise<{code: number, output: string}>} 退出码与合并后的输出。
 */
function runProcess(exe, args, options = {}) {
  const started = Date.now();
  return new Promise((resolve) => {
    let child, timer, cancellation, timedOut = false, settled = false;
    const finish = (code, output) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      options.signal?.removeEventListener('abort', abort);
      resolve({ code, output, cancelled: !!cancellation, timedOut, durationMs: Date.now() - started });
    };
    const abort = () => {
      if (settled || cancellation || timedOut) return;
      cancellation = true;
      terminate();
    };
    const terminate = () => {
      if (!child || child.exitCode !== null) return;
      if (process.platform === 'win32') {
        // 取消整个生成任务，避免后代进程仍持有输出目录。
        const killer = spawn(path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'taskkill.exe'),
          ['/pid', String(child.pid), '/T', '/F'], { windowsHide: true });
        killer.on('error', () => child.kill());
      } else {
        try { process.kill(-child.pid, 'SIGKILL'); } catch { child.kill('SIGKILL'); }
      }
    };
    if (options.signal?.aborted) { cancellation = true; finish(-1, '生成已取消'); return; }
    const hasPath = exe && (path.isAbsolute(exe) || exe.includes('/') || exe.includes('\\'));
    if (!exe || (hasPath && !fs.existsSync(exe))) { finish(-1, `找不到可执行文件：${exe}`); return; }
    try {
      child = spawn(exe, args, { cwd: options.cwd, windowsHide: true, detached: process.platform !== 'win32' });
    } catch (error) { finish(-1, `启动失败：${error.message}`); return; }
    let stdout = '', stderr = '';
    child.stdout.setEncoding('utf8'); child.stderr.setEncoding('utf8');
    child.stdout.on('data', chunk => { stdout += chunk; });
    child.stderr.on('data', chunk => { stderr += chunk; });
    child.stdin.on('error', () => {});
    child.stdin.end(typeof options.input === 'string' ? options.input : undefined);
    options.signal?.addEventListener('abort', abort, { once: true });
    if (options.signal?.aborted) abort();
    timer = setTimeout(() => { timedOut = true; terminate(); }, options.timeoutMs || 120000);
    child.on('error', error => finish(-1, `执行失败：${error.message}`));
    child.on('close', code => {
      const output = [stdout.trimEnd(), stderr.trimEnd(), cancellation ? '生成已取消' :
        timedOut ? '生成超时' : ''].filter(Boolean).join('\n');
      finish(cancellation || timedOut ? -1 : code ?? -1, output);
    });
  });
}

/**
 * 解析 `--check` 的输出。
 *
 * @param {string} output 原始输出。
 * @returns {Array<{file: string, line: number, column: number, severity: number, message: string}>}
 *          诊断列表；行、列是 1 基。
 */
function parseCheckOutput(output) {
  const diagnostics = [];

  for (const rawLine of output.split('\n')) {
    const line = rawLine.trimEnd();
    if (!line) continue;

    // 1. 按正则提取"文件 / 行 / 列 / 级别 / 消息"。
    const match = DIAGNOSTIC_LINE.exec(line);
    if (!match || !match.groups) continue;

    // 2. 级别：警告按 2、提示按 3，其余按 1（与 LSP 的 severity 对齐）。
    //    `--check` 会输出 error / warning / info 三档，info 是"能编但值得看一眼"，
    //    别把它算成错误，否则问题面板会给干净文法报红。
    const level = match.groups.level;
    const severity = /info|提示/i.test(level) ? 3 : (/警|warn/i.test(level) ? 2 : 1);

    diagnostics.push({
      file: match.groups.file,
      line: Number(match.groups.line),
      column: Number(match.groups.column),
      severity,
      message: match.groups.message.trim(),
    });
  }

  return diagnostics;
}

/**
 * 跑一次文法校验。
 *
 * @param {string} command 可执行文件（ParseEngine 或独立 lsp-server）。
 * @param {string[]} args 前置子命令参数，如 `['lsp', '--check']`。
 * @param {string} file 文法文件。
 * @returns {Promise<{code: number, output: string, diagnostics: Array<object>}>}
 */
async function checkGrammar(command, args, file) {
  const result = await runProcess(command, [...(args || []), file], {
    cwd: path.dirname(file),
    timeoutMs: 60000,
  });
  return {
    code: result.code,
    output: result.output,
    diagnostics: parseCheckOutput(result.output),
  };
}

/**
 * 组装 ParseEngine 的命令行参数。
 *
 * @param {{grammarFile: string, outputDir: string, mode: string, lang: string,
 *          teach?: boolean, mermaid?: boolean, namespace?: string, prefix?: string,
 *          className?: string, defines?: string[], syntaxOnly?: boolean}} options 生成参数。
 * @returns {string[]} 参数数组。
 */
function buildGenerateArgs(options) {
  const args = [
    '--grammar-file',
    options.grammarFile,
    '--out-dir',
    options.outputDir,
    '--mode',
    options.mode || 'ielr',
    '--lang',
    options.lang || 'cpp17',
  ];

  // 1. 开关类。
  if (options.teach) args.push('--teach');
  if (options.mermaid) args.push('--mermaid');
  if (options.syntaxOnly) args.push('--syntax-only');
  if (options.incremental) args.push('--incremental');
  if (options.cacheDir) args.push('--cache-dir', options.cacheDir);
  if (options.incremental === false) args.push('--no-incremental');
  if (options.rebuild) args.push('--rebuild');
  if (options.cppLexer) args.push('--cpp-lexer', options.cppLexer);
  if (options.strictRegex) args.push('--strict-regex');

  // 2. 命名空间 / 前缀 / 类名。
  if (options.namespace) args.push('--namespace', options.namespace);
  if (options.prefix) args.push('--prefix', options.prefix);
  if (options.className) args.push('--class', options.className);

  // 3. 条件编译变量。
  for (const define of options.defines || []) {
    if (define && define.trim()) args.push('--define', define.trim());
  }

  return args;
}

/**
 * 列出输出目录里的文件（跳过 obj / bin 这类中间目录）。
 *
 * @param {string} dir 输出目录。
 * @returns {string[]} 文件绝对路径列表。
 */
function listGeneratedFiles(dir) {
  const files = [];
  try {
    const lines = fs.readFileSync(path.join(dir, '.parseengine-manifest'), 'utf8').split(/\r?\n/);
    if (lines.shift() === '# ParseEngine generated files v1') {
      return lines.filter(Boolean).map(relative => {
        const full = path.resolve(dir, relative);
        const inside = path.relative(path.resolve(dir), full);
        return inside && !inside.startsWith('..') && !path.isAbsolute(inside) && fs.existsSync(full) ? full : null;
      }).filter(Boolean).sort();
    }
  } catch { /* 兼容不提供清单的旧生成器。 */ }
  const skip = new Set(['obj', 'bin', 'target', 'node_modules', '.git', '.parseengine-cache']);

  /**
   * 递归遍历。
   *
   * @param {string} current 当前目录。
   */
  const walk = (current) => {
    let entries;
    try {
      entries = fs.readdirSync(current, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const full = path.join(current, entry.name);
      if (entry.isDirectory()) {
        if (!skip.has(entry.name)) walk(full);
      } else {
        files.push(full);
      }
    }
  };

  walk(dir);
  return files.sort();
}

/**
 * 生成解析器。
 *
 * @param {string} parseEngine ParseEngine 路径。
 * @param {object} options 生成参数（见 buildGenerateArgs）。
 * @returns {Promise<{code: number, output: string, files: string[]}>}
 */
async function generate(parseEngine, options) {
  const args = buildGenerateArgs(options);
  const result = await runProcess(parseEngine, args, {
    cwd: path.dirname(options.grammarFile),
    timeoutMs: options.timeoutMs || 180000,
    signal: options.signal,
  });

  return {
    ...result,
    cache: /分析缓存命中|Analysis cache hit/.test(result.output) ? 'hit' :
      /分析缓存未命中|Analysis cache miss/.test(result.output) ? 'miss' : 'off',
    files: fs.existsSync(options.outputDir) ? listGeneratedFiles(options.outputDir) : [],
  };
}

/**
 * 默认输出目录：文法旁边 `<名字>-<语言>`。
 *
 * @param {string} grammarFile 文法文件路径。
 * @param {string} lang 目标语言。
 * @returns {string} 输出目录。
 */
function defaultOutputDir(grammarFile, lang) {
  const dir = path.dirname(grammarFile);
  const name = path.basename(grammarFile, path.extname(grammarFile));
  return path.join(dir, `${name}-${lang}`);
}

module.exports = {
  runProcess,
  parseCheckOutput,
  checkGrammar,
  buildGenerateArgs,
  listGeneratedFiles,
  generate,
  defaultOutputDir,
};
