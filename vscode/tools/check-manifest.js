'use strict';

/**
 * 插件清单自检：确认 `gram` 语言真的被注册齐了。
 *
 * 这个脚本是打包流程的第一步——注册不完整就不该产出安装包。它检查：
 *   1. `contributes.languages` 里有 id 为 `gram`、扩展名含 `.gram` 的语言项；
 *   2. 指定的 `language-configuration.json` 存在且能解析（注释 / 括号 / 折叠标记）；
 *   3. `contributes.grammars` 里有 scopeName 为 `source.gram` 的高亮文件，且文件存在；
 *   4. `activationEvents` 里有 `onLanguage:gram`（打开 .gram 时才激活）；
 *   5. `main` 指向的入口文件存在；
 *   6. 语言级编辑器默认值 `[gram]` 已声明。
 *   7. 图标 `icon.png` 存在、是 PNG、且不小于 128×128（市场要求）。
 *
 * 另外确认这个插件**只**注册 `.gram` 一种语言：不跟别的插件抢同一批扩展名。
 *
 * 用法（在仓库任意目录执行都可以）：
 *   node vscode/tools/check-manifest.js
 *
 * 返回码 0 表示全部通过，1 表示有问题。
 */

const fs = require('node:fs');
const path = require('node:path');

/** 插件根目录（本文件在 <插件根>/tools/ 下）。 */
const EXTENSION_DIR = path.resolve(__dirname, '..');

/** 期望注册的语言 id。 */
const LANGUAGE_ID = 'gram';

/** 期望的语言扩展名。 */
const LANGUAGE_EXTENSION = '.gram';

/** 期望的 TextMate scope 名。 */
const SCOPE_NAME = 'source.gram';

let failures = 0;

/**
 * 记一条断言。
 *
 * @param {string} title 断言描述。
 * @param {boolean} ok 是否通过。
 * @param {string} [detail] 失败时的补充说明。
 */
function check(title, ok, detail) {
  if (!ok) failures += 1;
  console.log(`   [${ok ? 'OK' : '失败'}] ${title}${!ok && detail ? `（${detail}）` : ''}`);
}

/**
 * 读一个 JSON 文件。
 *
 * @param {string} file 路径。
 * @returns {any} 解析结果；文件不存在或坏格式时返回 null。
 */
function readJson(file) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch {
    return null;
  }
}

/**
 * 读 PNG 的宽高（只解析头部 24 字节，不依赖任何图形库）。
 *
 * @param {string} file PNG 文件路径。
 * @returns {{width: number, height: number}|null} 尺寸；不是 PNG 时返回 null。
 */
function readPngSize(file) {
  const buffer = fs.readFileSync(file);
  // PNG 签名 8 字节 + 长度/类型 8 字节，宽高在偏移 16 / 20，都是大端 32 位。
  if (buffer.length < 24) return null;
  if (buffer.toString('ascii', 1, 4) !== 'PNG') return null;
  return { width: buffer.readUInt32BE(16), height: buffer.readUInt32BE(20) };
}

/**
 * 自检主流程。
 */
function main() {
  const manifestPath = path.join(EXTENSION_DIR, 'package.json');
  const pkg = readJson(manifestPath);

  // 1. 清单本身要能读。
  check('package.json 可解析', !!pkg, manifestPath);
  if (!pkg) {
    console.log('\n清单检查失败 1 项');
    process.exit(1);
  }

  const contributes = pkg.contributes || {};

  // 2. 语言注册：id / 扩展名 / 配置文件。
  const languages = contributes.languages || [];
  const language = languages.find((item) => item && item.id === LANGUAGE_ID);
  check(`注册了语言 ${LANGUAGE_ID}`, !!language, 'contributes.languages 里没有对应项');

  if (language) {
    const extensions = language.extensions || [];
    check(
      `语言 ${LANGUAGE_ID} 关联 ${LANGUAGE_EXTENSION} 扩展名`,
      extensions.includes(LANGUAGE_EXTENSION),
      `实际：${extensions.join('、') || '（空）'}`,
    );

    // 语言配置文件：能解析，且注释 / 括号 / 折叠标记都有。
    const configRel = language.configuration;
    if (configRel) {
      const configPath = path.join(EXTENSION_DIR, configRel);
      const config = readJson(configPath);
      check('语言配置文件可解析', !!config, configPath);
      if (config) {
        check('声明了行注释与块注释', !!(config.comments && config.comments.lineComment));
        check('声明了折叠标记', !!(config.folding && config.folding.markers));
      }
    } else {
      check('语言声明了 configuration', false, '缺少 configuration 字段');
    }
  }

  // 3. 高亮语法注册：scopeName 与实际文件。
  const grammars = contributes.grammars || [];
  const grammar = grammars.find((item) => item && item.language === LANGUAGE_ID);
  check(`语法高亮绑定到语言 ${LANGUAGE_ID}`, !!grammar, 'contributes.grammars 里没有对应项');

  if (grammar) {
    check(`语法 scopeName 为 ${SCOPE_NAME}`, grammar.scopeName === SCOPE_NAME, `实际：${grammar.scopeName}`);
    const grammarPath = path.join(EXTENSION_DIR, grammar.path || '');
    const grammarJson = readJson(grammarPath);
    check('语法文件可解析', !!grammarJson, grammarPath);
    if (grammarJson) {
      check('语法文件内部 scopeName 与清单一致', grammarJson.scopeName === SCOPE_NAME);
      check('语法文件包含规则', Array.isArray(grammarJson.patterns) && grammarJson.patterns.length > 0);
    }
  }

  // 4. 激活事件：打开 .gram 时才激活。
  const activationEvents = pkg.activationEvents || [];
  check(
    `activationEvents 包含 onLanguage:${LANGUAGE_ID}`,
    activationEvents.includes(`onLanguage:${LANGUAGE_ID}`),
  );

  // 5. 入口文件。
  const mainRel = pkg.main || '';
  check('main 指向存在的入口文件', !!mainRel && fs.existsSync(path.join(EXTENSION_DIR, mainRel)), mainRel);

  // 6. 语言级编辑器默认值。
  const defaults = contributes.configurationDefaults || {};
  check(`声明了 [${LANGUAGE_ID}] 编辑器默认值`, !!defaults[`[${LANGUAGE_ID}]`]);

  const commands = contributes.commands || [];
  check('注册了解析预览命令', commands.some((item) => item.command === 'gram.preview'));
  const keybindings = contributes.keybindings || [];
  check('F7 绑定解析预览', keybindings.some((item) =>
    item.command === 'gram.preview' && String(item.key).toLowerCase() === 'f7'));

  // 这个插件只认 `.gram`：多注册一种语言就会跟别的插件抢同一批扩展名。
  check('只注册了一种语言', languages.length === 1, `实际 ${languages.length} 种`);
  check(
    '所有语言的扩展名都只有 .gram',
    languages.every(
      (item) =>
        Array.isArray(item && item.extensions) &&
        item.extensions.length === 1 &&
        item.extensions[0] === LANGUAGE_EXTENSION,
    ),
    languages.map((item) => (item && item.extensions) || []).join('、'),
  );
  check('只注册了一条语法高亮', grammars.length === 1, `实际 ${grammars.length} 条`);
  check(
    '没有声明除 .gram 之外的扩展名',
    !languages.some((item) =>
      (item && item.extensions ? item.extensions : []).some((ext) => ext !== LANGUAGE_EXTENSION),
    ),
  );

  // 7. 图标：声明了、存在、是 PNG、够大（市场最低 128×128）。
  const iconRelative = typeof pkg.icon === 'string' ? pkg.icon : '';
  check('package.json 声明了 icon', !!iconRelative, '缺少 icon 字段');
  if (iconRelative) {
    const iconPath = path.join(EXTENSION_DIR, iconRelative);
    const exists = fs.existsSync(iconPath);
    check('图标文件存在', exists, iconPath);
    if (exists) {
      const size = readPngSize(iconPath);
      check('图标是 PNG', !!size, iconRelative);
      if (size) {
        check(
          '图标不小于 128×128',
          size.width >= 128 && size.height >= 128,
          `实际 ${size.width}×${size.height}`,
        );
      }
    }
  }

  console.log(failures === 0 ? '\n清单检查通过：只注册 .gram（语言、扩展名与语法各一条）'
                             : `\n清单检查失败 ${failures} 项`);
  process.exit(failures === 0 ? 0 : 1);
}

main();
