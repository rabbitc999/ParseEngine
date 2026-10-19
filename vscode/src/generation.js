'use strict';
/** @file 生成配置校验、路径解析及同一输出目录的任务互斥。 */
const fs = require('node:fs');
const path = require('node:path');
const engine = require('./parseEngine');
const MODES = ['ll1', 'lr0', 'slr', 'lalr', 'lr1', 'ielr', 'lrk', 'lalrk', 'slrk', 'llk', 'll0', 'glr', 'earley', 'cyk', 'dd', 'lex'];
const LANGUAGES = ['c', 'cpp17', 'python', 'csharp', 'rust', 'go', 'java', 'js', 'gram'];

/** @brief 按目标支持矩阵筛选模式。@param {string} lang 目标。@returns {string[]} 可用模式。 */
function allowedModes(lang) {
  if (['java', 'js'].includes(lang)) return ['dd', 'earley', 'cyk'];
  return lang === 'c' ? MODES.filter(mode => !['glr', 'earley', 'cyk'].includes(mode)) : [...MODES];
}

/** @brief 校验生成配置，并把相对路径固定到所属工作区。
 * @param {object} profile 配置。@param {string} grammarFile 文法。@param {string} root 工作区。
 * @returns {object} 可传给生成器的配置；错误时抛出说明。
 */
function resolveProfile(profile, grammarFile, root) {
  const mode = profile.mode || 'ielr', lang = profile.lang || 'cpp17';
  if (!MODES.includes(mode) || !LANGUAGES.includes(lang)) throw new Error('生成模式或目标语言无效');
  if (['java', 'js'].includes(lang) && !['dd', 'earley', 'cyk'].includes(mode))
    throw new Error(`${lang} 只支持 dd、earley、cyk`);
  if (lang === 'c' && ['glr', 'earley', 'cyk'].includes(mode))
    throw new Error('C 目标不支持 glr、earley、cyk');
  const base = root || path.dirname(grammarFile);
  const result = { ...profile, grammarFile, mode, lang,
    outputDir: path.resolve(base, profile.outputDir || engine.defaultOutputDir(grammarFile, lang)) };
  if (profile.cacheDir) result.cacheDir = path.resolve(base, profile.cacheDir);
  if (profile.cppLexer && !['std', 'dfa', 'vm'].includes(profile.cppLexer)) throw new Error('C++ 词法引擎无效');
  return result;
}

/** @brief 用真实父路径识别输出目录的符号链接别名。
 * @param {string} directory 输出目录。@returns {string} 互斥键。
 */
function outputKey(directory) {
  let current = path.resolve(directory);
  const parts = [];
  while (!fs.existsSync(current)) {
    const parent = path.dirname(current);
    if (parent === current) break;
    parts.unshift(path.basename(current)); current = parent;
  }
  try { current = fs.realpathSync.native(current); } catch { /* 交给生成器报告无法访问。 */ }
  const key = path.join(current, ...parts);
  return process.platform === 'win32' ? key.toLowerCase() : key;
}

/** @brief 管理本扩展启动的生成任务，关闭扩展时取消全部任务。 */
class GenerationManager {
  constructor() { this.tasks = new Map(); }
  /** @brief 执行并持有目录锁直到进程退出。
   * @param {string} exe 生成器。@param {object} options 参数。@returns {Promise<object>} 结果。
   */
  async run(exe, options) {
    const key = outputKey(options.outputDir);
    if (this.tasks.has(key)) return { code: -1, output: '同一输出目录已有生成任务', files: [], busy: true };
    const controller = new AbortController();
    const abort = () => controller.abort();
    this.tasks.set(key, controller);
    options.signal?.addEventListener('abort', abort, { once: true });
    if (options.signal?.aborted) abort();
    try { return await engine.generate(exe, { ...options, signal: controller.signal }); }
    finally { options.signal?.removeEventListener('abort', abort); this.tasks.delete(key); }
  }
  /** @brief 取消所有正在运行的生成任务。 */
  dispose() { for (const task of this.tasks.values()) task.abort(); }
}
module.exports = { MODES, LANGUAGES, allowedModes, resolveProfile, GenerationManager };
