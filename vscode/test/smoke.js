'use strict';

/**
 * 插件冒烟测试（纯 Node，不需要装 VS Code）。
 *
 * 覆盖两条链路：
 *   1. LSP：握手 → 打开文档 → 诊断 → 悬停 / 补全 / 跳转 / 大纲 / 格式化 / 快速修复；
 *   2. 生成：ParseEngine 产出 Python 与 C++17 产物，以及 `ParseEngine lsp --check` 的输出解析。
 *
 * 语言服务器已内嵌进生成器，所以这里只需要一个 ParseEngine 可执行文件；
 * 想测独立构建的 `lsp-server` 时，把路径塞进环境变量 `LSP_SERVER` 即可
 * （此时改用原来的 `--stdio` / `--check` 参数形状）。
 *
 * 用法：
 *   node vscode/test/smoke.js
 *
 * 返回码 0 表示全部通过，1 表示有失败项。
 */

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { LspClient } = require('../src/lspClient');
const tools = require('../src/serverLocator');
const engine = require('../src/parseEngine');
const preview = require('../src/preview');

let checks = 0;
let failures = 0;

/**
 * 记一条断言。
 *
 * @param {string} title 断言描述。
 * @param {boolean} ok 是否通过。
 */
function check(title, ok) {
  checks += 1;
  if (!ok) failures += 1;
  console.log(`   [${ok ? 'OK' : '失败'}] ${title}`);
}

/**
 * 建一个临时目录。
 *
 * @returns {string} 目录路径。
 */
function tempDir() {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'gram-vscode-smoke-'));
}

/**
 * 打开文档并等第一次诊断推送。
 *
 * @param {LspClient} client 客户端。
 * @param {string} uri 文档 URI。
 * @param {string} text 文档内容。
 * @returns {Promise<Array<any>>} 诊断列表（超时返回空数组）。
 */
function openAndWait(client, uri, text, languageId = 'gram') {
  return new Promise((resolve) => {
    // 1. 先订阅：只认当前文档的第一次推送。
    const onDiagnostics = (event) => {
      if (event.uri !== uri) return;
      client.off('diagnostics', onDiagnostics);
      clearTimeout(timer);
      resolve(event.diagnostics || []);
    };
    client.on('diagnostics', onDiagnostics);

    // 2. 10 秒还没推就当作超时。
    const timer = setTimeout(() => {
      client.off('diagnostics', onDiagnostics);
      resolve([]);
    }, 10000);

    // 3. 打开文档。
    client.notify('textDocument/didOpen', {
      textDocument: { uri, languageId, version: 1, text },
    });
  });
}

/**
 * 跑一遍 LSP 全流程。
 *
 * @param {string} lspCommand 语言服务器可执行文件（内嵌时就是 ParseEngine）。
 * @param {string[]} lspArgs 启动服务器用的前置参数，内嵌为 `['lsp', '--stdio']`。
 * @param {string} file 示例文法。
 * @param {boolean} expectErrors true 表示这个示例本来就该报错。
 */
async function testLsp(lspCommand, lspArgs, file, expectErrors) {
  const name = path.basename(file);
  console.log(`\n== LSP：${name} ==`);

  // 1. 起服务器、握手。
  const client = new LspClient(lspCommand, lspArgs);
  client.on('log', () => {});
  await client.start();
  await client.initialize();
  check(`${name}：初始化成功`, client.running);

  const uri = `file:///${file.replace(/\\/g, '/')}`;
  const text = fs.readFileSync(file, 'utf8');

  // 2. 打开文档并等诊断。
  const diagnostics = await openAndWait(client, uri, text);
  const errors = diagnostics.filter((item) => item.severity === 1).length;
  const warnings = diagnostics.filter((item) => item.severity === 2).length;
  console.log(`   诊断 ${diagnostics.length} 条（其中错误 ${errors} 条、警告 ${warnings} 条）`);
  check(
    expectErrors ? `${name}：有错误诊断` : `${name}：没有错误诊断`,
    expectErrors ? errors > 0 : errors === 0,
  );
  // 干净示例的期望是「诊断为空」：连警告也不该有。`%token NAME include "表.csv"`
  // 曾把 include 当成 Token 名，于是报出假的 unused-token 警告 —— 这条盯着它。
  if (!expectErrors) check(`${name}：也没有警告诊断`, warnings === 0);

  // 3. 悬停：挑一行 %define，位置放在指令名中间。
  const lines = text.split(/\r?\n/);
  let defineLine = lines.findIndex((line) => line.trimStart().startsWith('%define'));
  if (defineLine < 0) defineLine = 0;
  const position = { line: defineLine, character: 3 };

  const hover = await client.request('textDocument/hover', {
    textDocument: { uri },
    position,
  });
  const hoverText = hover && hover.contents ? JSON.stringify(hover.contents) : '';
  check(`${name}：悬停有说明`, hoverText.length > 2);

  // 4. 补全。
  const completion = await client.request('textDocument/completion', {
    textDocument: { uri },
    position,
  });
  const completionItems = Array.isArray(completion)
    ? completion
    : (completion && completion.items) || [];
  console.log(`   补全候选 ${completionItems.length} 个`);
  check(`${name}：补全有候选`, completionItems.length > 0);

  // 5. 跳转定义。
  let ruleLine = lines.findIndex((line) => line.includes(':=') && !line.trimStart().startsWith('%'));
  if (ruleLine < 0) ruleLine = lines.length - 1;
  const definition = await client.request('textDocument/definition', {
    textDocument: { uri },
    position: { line: ruleLine, character: 0 },
  });
  console.log(`   跳转目标 ${Array.isArray(definition) ? definition.length : 0} 个`);

  // 6. 文档大纲。
  const symbols = await client.request('textDocument/documentSymbol', { textDocument: { uri } });
  console.log(`   大纲符号 ${Array.isArray(symbols) ? symbols.length : 0} 个`);
  check(`${name}：大纲非空`, Array.isArray(symbols) && symbols.length > 0);

  // 7. 格式化。
  const edits = await client.request('textDocument/formatting', {
    textDocument: { uri },
    options: { tabSize: 4, insertSpaces: true },
  });
  console.log(`   格式化编辑 ${Array.isArray(edits) ? edits.length : 0} 处`);

  // 8. 快速修复：坏例子应该能拿到修法。
  if (expectErrors && diagnostics.length > 0) {
    const actions = await client.request('textDocument/codeAction', {
      textDocument: { uri },
      range: diagnostics[0].range,
      context: { diagnostics },
    });
    console.log(`   快速修复 ${Array.isArray(actions) ? actions.length : 0} 个`);
    check(`${name}：有快速修复`, Array.isArray(actions) && actions.length > 0);
  }

  await client.stop();
}

/**
 * 测试生成与 `--check`。
 *
 * @param {string} parseEngine ParseEngine 路径。
 * @param {string} lspCommand 语言服务器可执行文件（`--check` 走它）。
 * @param {string[]} checkArgs `--check` 的前置参数，内嵌为 `['lsp', '--check']`。
 * @param {string} pythonGrammar 用于生成 Python 产物的文法。
 * @param {string} cppGrammar 用于生成 C++17 产物的文法（也是干净文法）。
 */
async function testGenerate(parseEngine, lspCommand, checkArgs, pythonGrammar, cppGrammar) {
  console.log('\n== 生成：ParseEngine ==');

  // 1. Python：免编译，产物少，当作主用例。
  const pythonDir = tempDir();
  const pythonResult = await engine.generate(parseEngine, {
    grammarFile: pythonGrammar,
    outputDir: pythonDir,
    mode: 'lalr',
    lang: 'python',
  });
  console.log(`   Python：退出码 ${pythonResult.code}，产物 ${pythonResult.files.length} 个`);
  if (pythonResult.code !== 0) console.log(pythonResult.output);
  check('生成：Python 退出码为 0', pythonResult.code === 0);
  check('生成：产出 parser.py', fs.existsSync(path.join(pythonDir, 'parser.py')));
  check('生成：产出 main.py', fs.existsSync(path.join(pythonDir, 'main.py')));

  // 2. C++17：覆盖带 %value_type 的文法。
  const cppDir = tempDir();
  const cppResult = await engine.generate(parseEngine, {
    grammarFile: cppGrammar,
    outputDir: cppDir,
    mode: 'ielr',
    lang: 'cpp17',
  });
  console.log(`   C++17：退出码 ${cppResult.code}，产物 ${cppResult.files.length} 个`);
  if (cppResult.code !== 0) console.log(cppResult.output);
  check('生成：C++17 退出码为 0', cppResult.code === 0);
  check('生成：产出 parser.cpp', fs.existsSync(path.join(cppDir, 'parser.cpp')));

  // 3. --check：干净文法没有错误，坏例子能解析出错误。
  //    退出码也要对：干净 0、有错 1（`ParseEngine lsp` 的子命令分派曾经把 `lsp`
  //    自己当文件读，于是干净文法也返回 1、还多一条"无法读取文件"，这里盯着它）。
  const clean = await engine.checkGrammar(lspCommand, checkArgs, cppGrammar);
  console.log(`   校验（干净）：退出码 ${clean.code}，诊断 ${clean.diagnostics.length} 条`);
  check('校验：干净文法退出码为 0', clean.code === 0);
  check('校验：干净文法没有错误诊断', clean.diagnostics.every((item) => item.severity !== 1));

  const brokenPath = path.join(path.dirname(cppGrammar), 'sample_broken.gram');
  const broken = await engine.checkGrammar(lspCommand, checkArgs, brokenPath);
  console.log(`   校验（坏例子）：退出码 ${broken.code}，诊断 ${broken.diagnostics.length} 条`);
  check('校验：坏例子退出码为 1', broken.code === 1);
  check('校验：坏例子解析出错误诊断', broken.diagnostics.some((item) => item.severity === 1));
  // info 是"能编但值得看一眼"，不能算进错误里。
  check('校验：info 归到提示级', broken.diagnostics.some((item) => item.severity === 3));

  // 4. 清理临时目录。
  fs.rmSync(pythonDir, { recursive: true, force: true });
  fs.rmSync(cppDir, { recursive: true, force: true });
}

/**
 * 用带 C/C++ 语义动作的文法验证 --syntax-only 预览链路。
 *
 * @param {string} parseEngine ParseEngine 路径。
 * @param {string} grammarFile 测试文法。
 */
async function testPreview(parseEngine, grammarFile) {
  console.log('\n== Token / 语法树预览 ==');
  const lalr = await preview.runPreview(parseEngine, {
    grammarFile,
    input: '1 + 2 * 3',
    mode: 'lalr',
  });
  if (!lalr.ok) console.log(lalr.errors.join('\n'));
  check('预览：LALR 解析成功', lalr.ok);
  check('预览：返回 Token', lalr.tokens.length >= 5);
  check('预览：返回完整语法树', lalr.trees.length === 1 && lalr.trees[0].children.length > 0);

  const glr = await preview.runPreview(parseEngine, {
    grammarFile,
    input: '1 + 2',
    mode: 'glr',
  });
  if (!glr.ok) console.log(glr.errors.join('\n'));
  check('预览：GLR 解析成功', glr.ok);
  check('预览：GLR 返回语法树', glr.trees.length > 0);
}

/**
 * 入口。
 */
async function main() {
  // 1. 定位工具：找不到就直接失败，后面测不了。走插件同一条定位链
  //    （显式路径 → 仓库 build/bin → 插件自带 bin → PATH）。
  const repoRoot = tools.findRepoRoot(process.cwd());
  check('定位仓库根', !!repoRoot);
  const resolved = tools.resolveTools(repoRoot || process.cwd(), {});
  const parseEngine = process.env.PARSE_ENGINE || resolved.parseEngine;
  check('找到 ParseEngine', !!parseEngine);
  if (!repoRoot || !parseEngine) {
    console.log(`\n共 ${checks} 项检查，失败 ${failures} 项`);
    process.exit(1);
  }

  if (process.env.GRAM_SMOKE_PREVIEW_ONLY === '1') {
    await testPreview(parseEngine, path.join(repoRoot, 'example', 'calc.gram'));
    console.log(`\n共 ${checks} 项检查，失败 ${failures} 项`);
    process.exit(failures === 0 ? 0 : 1);
  }

  // 2. 语言服务器：默认用内嵌的那份；`LSP_SERVER` 指到独立构建时换回老参数。
  const standalone = process.env.LSP_SERVER || '';
  const lspCommand = standalone || parseEngine;
  const lspArgs = standalone ? ['--stdio'] : [tools.LSP_SUBCOMMAND, '--stdio'];
  const checkArgs = standalone ? ['--check'] : [tools.LSP_SUBCOMMAND, '--check'];
  check(standalone ? '找到独立 lsp-server' : '语言服务器内嵌在 ParseEngine 里', !!lspCommand);

  // 3. LSP 全流程：干净示例 + 坏示例。
  const sample = path.join(repoRoot, 'library', 'lsp', 'examples', 'sample.gram');
  const broken = path.join(repoRoot, 'library', 'lsp', 'examples', 'sample_broken.gram');
  await testLsp(lspCommand, lspArgs, sample, false);
  await testLsp(lspCommand, lspArgs, broken, true);

  // 4. 生成与校验。
  await testGenerate(
    parseEngine,
    lspCommand,
    checkArgs,
    path.join(repoRoot, 'example', 'python_demo.gram'),
    sample,
  );
  await testPreview(parseEngine, path.join(repoRoot, 'example', 'calc.gram'));

  console.log(`\n共 ${checks} 项检查，失败 ${failures} 项`);
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((error) => {
  console.error('冒烟测试异常：', error);
  process.exit(1);
});
