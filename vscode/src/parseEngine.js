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
 * @param {{cwd?: string, timeoutMs?: number, input?: string}} [options] 选项。
 * @returns {Promise<{code: number, output: string}>} 退出码与合并后的输出。
 */
function runProcess(exe, args, options) {
  return new Promise((resolve) => {
    const timeoutMs = (options && options.timeoutMs) || 120000;

    // 1. 找不到可执行文件就直接返回，交给上层提示。
    const hasPath = exe && (path.isAbsolute(exe) || exe.includes('/') || exe.includes('\\'));
    if (!exe || (hasPath && !fs.existsSync(exe))) {
      resolve({ code: -1, output: `找不到可执行文件：${exe}` });
      return;
    }

    // 2. 起进程，stdout / stderr 一起收。
    let child;
    try {
      child = spawn(exe, args, {
        cwd: (options && options.cwd) || undefined,
        windowsHide: true,
      });
    } catch (error) {
      resolve({ code: -1, output: `启动失败：${error.message}` });
      return;
    }

    let stdout = '';
    let stderr = '';
    let finished = false;

    child.stdout.on('data', (chunk) => {
      stdout += chunk.toString('utf8');
    });
    child.stderr.on('data', (chunk) => {
      stderr += chunk.toString('utf8');
    });
    child.stdin.on('error', () => {
      // 子进程提前退出时可能关闭管道，最终错误由 close/error 分支统一返回。
    });
    child.stdin.end(options && typeof options.input === 'string' ? options.input : undefined);

    // 3. 超时兜底：杀掉进程并返回超时提示。
    const timer = setTimeout(() => {
      if (finished) return;
      finished = true;
      try {
        child.kill();
      } catch {
        // 已经退了。
      }
      resolve({ code: -1, output: `命令超时（>${Math.round(timeoutMs / 1000)} 秒）：${exe}` });
    }, timeoutMs);

    child.on('error', (error) => {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      resolve({ code: -1, output: `执行失败：${error.message}` });
    });

    child.on('close', (code) => {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      const output = stderr.trim() ? `${stdout}${stdout.trim() ? '\n' : ''}${stderr}` : stdout;
      resolve({ code: code === null ? -1 : code, output: output.trimEnd() });
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
  const skip = new Set(['obj', 'bin', 'node_modules', '.git']);

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
    timeoutMs: 180000,
  });

  return {
    code: result.code,
    output: result.output,
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
