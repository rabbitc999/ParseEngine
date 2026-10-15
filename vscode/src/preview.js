'use strict';

/**
 * Token / 语法树预览运行器。
 *
 * 该模块只使用 Node 内置库：先让 ParseEngine 生成隔离的 Python 词法器与解析器，
 * 再由一段小型 Python 驱动把结果转成 JSON，供插件的 Webview 展示。
 */

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const engine = require('./parseEngine');

const SUPPORTED_MODES = new Set(['lalr', 'lr1', 'ielr', 'glr']);

const DRIVER_SOURCE = String.raw`import importlib.util
import json
import os
import sys

def load(module_dir, name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(module_dir, "parser.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def tree_json(node):
    location = node.location
    return {
        "name": node.name,
        "text": node.text,
        "line": location.line,
        "column": location.column,
        "children": [tree_json(child) for child in node.children],
    }

text = sys.stdin.read()
lexer_module = load(sys.argv[1], "preview_lexer")
parser_module = load(sys.argv[2], "preview_parser")
lexer = lexer_module.PreviewLexer()
lex_status, raw_tokens = lexer.tokenize(text)
tokens = [{
    "type": lexer_module.token_name(token.type) or "?",
    "text": token.text,
    "line": token.line,
    "column": token.column,
} for token in raw_tokens]

parser = parser_module.PreviewParser()
trees = []
if sys.argv[3] == "glr":
    parse_status = parser.run_glr_tree(text)
    if parse_status == parser_module.STATUS_OK:
        trees = [tree_json(parser.glr_tree(index))
                 for index in range(parser.glr_result_count())]
else:
    tree = parser.parse_tree(text)
    parse_status = parser_module.STATUS_OK if tree is not None else parser_module.STATUS_ERR_SYNTAX
    if tree is not None:
        trees = [tree_json(tree)]

errors = [str(error) for error in parser.errors]
if lex_status != lexer_module.STATUS_OK and lexer.last_error is not None:
    errors.insert(0, str(lexer.last_error))
print(json.dumps({
    "ok": lex_status == lexer_module.STATUS_OK and parse_status == parser_module.STATUS_OK,
    "tokens": tokens,
    "trees": trees,
    "errors": errors,
}, ensure_ascii=False))
`;

/**
 * 找可用的 Python 3 命令。
 *
 * @param {string} configured 用户设置的命令或可执行文件路径。
 * @returns {Promise<{exe: string, prefix: string[]}|null>}
 */
async function findPython(configured) {
  const candidates = [];
  if (configured && configured.trim()) candidates.push({ exe: configured.trim(), prefix: [] });
  candidates.push(
    { exe: 'python', prefix: [] },
    { exe: 'python3', prefix: [] },
    { exe: 'py', prefix: ['-3'] },
  );

  const seen = new Set();
  for (const candidate of candidates) {
    const key = `${candidate.exe}\0${candidate.prefix.join('\0')}`;
    if (seen.has(key)) continue;
    seen.add(key);
    const result = await engine.runProcess(candidate.exe, [...candidate.prefix, '--version'], {
      timeoutMs: 3000,
    });
    if (result.code === 0) return candidate;
  }
  return null;
}

/**
 * 运行一次解析预览。
 *
 * @param {string} parseEngine ParseEngine 路径。
 * @param {{grammarFile: string, input: string, mode: string, pythonPath?: string}} options 参数。
 * @returns {Promise<{ok: boolean, tokens: Array<object>, trees: Array<object>,
 *                    errors: string[], output: string}>}
 */
async function runPreview(parseEngine, options) {
  const mode = SUPPORTED_MODES.has(options.mode) ? options.mode : 'ielr';
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'gram-vscode-preview-'));
  const lexerDir = path.join(root, 'lexer');
  const parserDir = path.join(root, 'parser');

  const fail = (message, detail) => ({
    ok: false,
    tokens: [],
    trees: [],
    errors: [`${message}${detail ? `：${detail}` : ''}`],
    output: detail || '',
  });

  try {
    const lexer = await engine.generate(parseEngine, {
      grammarFile: options.grammarFile,
      outputDir: lexerDir,
      mode: 'lex',
      lang: 'python',
      className: 'PreviewLexer',
      syntaxOnly: true,
    });
    if (lexer.code !== 0) return fail('生成预览词法器失败', lexer.output);

    const parser = await engine.generate(parseEngine, {
      grammarFile: options.grammarFile,
      outputDir: parserDir,
      mode,
      lang: 'python',
      className: 'PreviewParser',
      syntaxOnly: true,
    });
    if (parser.code !== 0) return fail('生成预览解析器失败', parser.output);

    const python = await findPython(options.pythonPath || '');
    if (!python) {
      return fail('找不到 Python 3', '请安装 Python 3，或设置 gram.pythonPath。');
    }

    const driver = path.join(root, 'preview.py');
    fs.writeFileSync(driver, DRIVER_SOURCE, 'utf8');
    const result = await engine.runProcess(
      python.exe,
      [...python.prefix, driver, lexerDir, parserDir, mode],
      { cwd: root, timeoutMs: 120000, input: options.input || '' },
    );
    if (result.code !== 0) return fail('运行预览解析器失败', result.output);

    let parsed;
    try {
      parsed = JSON.parse(result.output);
    } catch (error) {
      return fail('预览结果不是有效 JSON', `${error.message}\n${result.output}`);
    }

    return {
      ok: !!parsed.ok,
      tokens: Array.isArray(parsed.tokens) ? parsed.tokens : [],
      trees: Array.isArray(parsed.trees) ? parsed.trees : [],
      errors: Array.isArray(parsed.errors) ? parsed.errors.map(String) : [],
      output: [lexer.output, parser.output].filter(Boolean).join('\n'),
    };
  } catch (error) {
    return fail('预览失败', error.message);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}

module.exports = {
  SUPPORTED_MODES,
  findPython,
  runPreview,
};
