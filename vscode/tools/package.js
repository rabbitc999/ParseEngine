'use strict';

/**
 * 打包 VS Code 插件为 `.vsix`。
 *
 * 为什么自己写而不是用 `vsce`：
 *   - `vsce` 要从 npm 装（需要联网），本仓库的插件本身是零依赖的，不该为了打包破例；
 *   - `.vsix` 就是一个 ZIP：根放 `[Content_Types].xml` 和 `extension.vsixmanifest`，
 *     插件文件统一放在 `extension/` 下。这里用 Node 内置的 zlib 自己写 ZIP，
 *     因此 Windows / Linux / macOS 都能跑。
 *
 * 用法（在仓库任意目录执行都可以）：
 *   node vscode/tools/package.js                 # 跑测试 + 打包
 *   node vscode/tools/package.js --skip-tests    # 只打包
 *   node vscode/tools/package.js --list          # 只列出会被打包的文件
 *   node vscode/tools/package.js --out <目录>    # 指定输出目录（默认 vscode/dist）
 *   node vscode/tools/package.js --universal     # 不打平台标记（默认会按当前平台打）
 *
 * 平台标记：包里带的是**当前平台**编译出来的 `ParseEngine`，所以清单的
 * `Identity` 会写上 `TargetPlatform`、文件名也缀上平台（如 `-win32-x64`），
 * VS Code 就只会把它装到匹配的机器上——不然 macOS / Linux 用户装上会得到一个
 * 跑不起来的 Windows 可执行文件。要从源码在别的平台用，就先在那个平台重新
 * 构建并打包；想强制产出通用包（例如编译器放在 `PATH` 里），加 `--universal`。
 */

const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const { execFileSync } = require('node:child_process');

/** 插件根目录（本文件在 <插件根>/tools/ 下）。 */
const EXTENSION_DIR = path.resolve(__dirname, '..');

/** 默认输出目录。 */
const DEFAULT_OUT_DIR = path.join(EXTENSION_DIR, 'dist');

/**
 * 打印一行日志。
 *
 * @param {string} message 文本。
 */
function log(message) {
  console.log(message);
}

/**
 * 解析命令行参数。
 *
 * @param {string[]} argv 参数（不含 node 与脚本名）。
 * @returns {{skipTests: boolean, listOnly: boolean, universal: boolean, outDir: string}} 解析结果。
 */
function parseArgs(argv) {
  const options = {
    skipTests: false,
    listOnly: false,
    universal: false,
    outDir: DEFAULT_OUT_DIR,
  };

  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    if (arg === '--skip-tests') options.skipTests = true;
    else if (arg === '--list') options.listOnly = true;
    else if (arg === '--universal') options.universal = true;
    else if (arg === '--out') options.outDir = path.resolve(argv[++index] || DEFAULT_OUT_DIR);
    else if (arg === '--help' || arg === '-h') {
      log('用法：node tools/package.js [--skip-tests] [--list] [--universal] [--out <目录>]');
      process.exit(0);
    }
  }
  return options;
}

/**
 * 当前平台的 VS Code 平台标识（`TargetPlatform` 属性取值）。
 *
 * 形如 `win32-x64` / `darwin-arm64` / `linux-x64`，与 VS Code 的
 * [平台命名](https://code.visualstudio.com/api/working-with-extensions/publishing-extension#platformspecific-extensions)一致。
 *
 * @returns {string} 平台标识。
 */
function vscodePlatform() {
  return `${process.platform}-${process.arch}`;
}

/**
 * 读一个 JSON 文件。
 *
 * @param {string} file 文件路径。
 * @returns {any} 解析结果。
 */
function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

/**
 * 读取 `.vscodeignore`：返回去掉注释与空行的模式列表。
 *
 * @returns {string[]} 模式列表。
 */
function loadIgnorePatterns() {
  const file = path.join(EXTENSION_DIR, '.vscodeignore');
  if (!fs.existsSync(file)) return [];

  return fs
    .readFileSync(file, 'utf8')
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line && !line.startsWith('#'));
}

/**
 * 转义正则里的特殊字符。
 *
 * @param {string} text 原始文本。
 * @returns {string} 转义后的文本。
 */
function escapeRegExp(text) {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

/**
 * 判断一个相对路径是否匹配某条 ignore 模式。
 *
 * 支持四类写法：`目录/**`（整棵子树）、`** 加斜杠加名字`（任意层级下的同名文件）、
 * `*.ext`（按扩展名）、精确路径。
 *
 * @param {string} relativePath 相对插件根的路径（正斜杠）。
 * @param {string} pattern 模式。
 * @returns {boolean} 是否匹配。
 */
function matchesPattern(relativePath, pattern) {
  // 1. dir/** → 该目录及其下全部内容。
  if (pattern.endsWith('/**')) {
    const prefix = pattern.slice(0, -3).replace(/\/+$/, '');
    return relativePath === prefix || relativePath.startsWith(`${prefix}/`);
  }

  // 2. **/name → 任意层级下的这个名字。
  if (pattern.startsWith('**/')) {
    const rest = pattern.slice(3);
    return relativePath === rest || relativePath.endsWith(`/${rest}`);
  }

  // 3. 带通配符（本仓库只用 *.ext 这种文件名形式）。
  if (pattern.includes('*')) {
    const regex = new RegExp(`^${pattern.split('*').map(escapeRegExp).join('[^/]*')}$`);
    return regex.test(relativePath) || regex.test(path.basename(relativePath));
  }

  // 4. 精确路径或目录前缀。
  return relativePath === pattern || relativePath.startsWith(`${pattern}/`);
}

/**
 * 判断一个相对路径是否应当被打包排除。
 *
 * @param {string} relativePath 相对插件根的路径（正斜杠）。
 * @param {string[]} patterns `.vscodeignore` 模式。
 * @returns {boolean} true 表示不打包。
 */
function isIgnored(relativePath, patterns) {
  let ignored = false;

  // 按顺序应用；`!` 开头的模式用来"取消忽略"。
  for (const raw of patterns) {
    const negate = raw.startsWith('!');
    const pattern = negate ? raw.slice(1) : raw;
    if (matchesPattern(relativePath, pattern)) ignored = !negate;
  }
  return ignored;
}

/**
 * 递归收集要打包的文件。
 *
 * @param {string} dir 当前目录。
 * @param {string[]} patterns ignore 模式。
 * @param {string[]} acc 结果累加（相对插件根的路径，正斜杠）。
 */
function collectFiles(dir, patterns, acc) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    const relative = path.relative(EXTENSION_DIR, full).replace(/\\/g, '/');

    if (isIgnored(relative, patterns)) continue;

    if (entry.isDirectory()) {
      collectFiles(full, patterns, acc);
    } else {
      acc.push(relative);
    }
  }
}

/** CRC32 查表，延迟创建。 */
let crcTable = null;

/**
 * 计算 CRC32（ZIP 每个文件头都要写）。
 *
 * @param {Buffer} buffer 数据。
 * @returns {number} 无符号 CRC32。
 */
function crc32(buffer) {
  if (!crcTable) {
    crcTable = new Int32Array(256);
    for (let index = 0; index < 256; index += 1) {
      let value = index;
      for (let bit = 0; bit < 8; bit += 1) {
        value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
      }
      crcTable[index] = value;
    }
  }

  let crc = -1;
  for (let index = 0; index < buffer.length; index += 1) {
    crc = (crc >>> 8) ^ crcTable[(crc ^ buffer[index]) & 0xff];
  }
  return (crc ^ -1) >>> 0;
}

/**
 * 把 Date 转成 ZIP 用的 DOS 时间/日期。
 *
 * @param {Date} date 时间。
 * @returns {{time: number, date: number}} DOS 时间与日期。
 */
function dosDateTime(date) {
  const time =
    (date.getHours() << 11) | (date.getMinutes() << 5) | Math.floor(date.getSeconds() / 2);
  const day = ((date.getFullYear() - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate();
  return { time, date: day };
}

/**
 * 用 deflate 生成一个 ZIP 文件（只放文件，不放目录项）。
 *
 * @param {Array<{name: string, data: Buffer}>} entries 条目（name 用正斜杠）。
 * @returns {Buffer} ZIP 字节。
 */
function createZip(entries) {
  const now = new Date();
  const { time, date } = dosDateTime(now);
  const locals = [];
  const centrals = [];
  let offset = 0;

  for (const entry of entries) {
    const nameBuffer = Buffer.from(entry.name, 'utf8');
    const raw = entry.data;
    const deflated = zlib.deflateRawSync(raw);
    const checksum = crc32(raw);

    // 1. 本地文件头 + 文件名 + 压缩数据。
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4); // 解压所需版本
    local.writeUInt16LE(0x0800, 6); // 文件名用 UTF-8
    local.writeUInt16LE(8, 8); // deflate
    local.writeUInt16LE(time, 10);
    local.writeUInt16LE(date, 12);
    local.writeUInt32LE(checksum, 14);
    local.writeUInt32LE(deflated.length, 18);
    local.writeUInt32LE(raw.length, 22);
    local.writeUInt16LE(nameBuffer.length, 26);
    local.writeUInt16LE(0, 28); // 无扩展字段
    locals.push(local, nameBuffer, deflated);

    // 2. 中央目录项（末尾的偏移量记下来，EOCD 要用）。
    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(20, 4); // 打包工具版本
    central.writeUInt16LE(20, 6); // 解压所需版本
    central.writeUInt16LE(0x0800, 8);
    central.writeUInt16LE(8, 10);
    central.writeUInt16LE(time, 12);
    central.writeUInt16LE(date, 14);
    central.writeUInt32LE(checksum, 16);
    central.writeUInt32LE(deflated.length, 20);
    central.writeUInt32LE(raw.length, 24);
    central.writeUInt16LE(nameBuffer.length, 28);
    central.writeUInt16LE(0, 30); // 扩展字段
    central.writeUInt16LE(0, 32); // 注释
    central.writeUInt16LE(0, 34); // 起始磁盘
    central.writeUInt16LE(0, 36); // 内部属性
    // 外部属性：普通文件 644；位运算会变成有符号 32 位，先转回无符号。
    central.writeUInt32LE((0o100644 << 16) >>> 0, 38);
    central.writeUInt32LE(offset, 42); // 本地头偏移
    centrals.push(central, nameBuffer);

    offset += local.length + nameBuffer.length + deflated.length;
  }

  const centralBuffer = Buffer.concat(centrals);

  // 3. 中央目录结束记录。
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(0, 4);
  end.writeUInt16LE(0, 6);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(centralBuffer.length, 12);
  end.writeUInt32LE(offset, 16);
  end.writeUInt16LE(0, 20);

  return Buffer.concat([...locals, centralBuffer, end]);
}

/**
 * 转义 XML 文本。
 *
 * @param {string} text 原始文本。
 * @returns {string} 转义后的文本。
 */
function escapeXml(text) {
  return String(text)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

/**
 * 生成 `extension.vsixmanifest`。
 *
 * @param {any} pkg package.json 内容。
 * @param {string[]} packagedFiles 已打包的相对路径（用于决定可选资产）。
 * @param {string} platform VS Code 平台标识；空串表示通用包（不写 `TargetPlatform`）。
 * @returns {string} XML 文本。
 */
function buildManifest(pkg, packagedFiles, platform) {
  const categories = (pkg.categories || []).join(',');
  const tags = (pkg.keywords || []).join(',');
  const engine = (pkg.engines && pkg.engines.vscode) || '*';
  const hasChangelog = packagedFiles.includes('CHANGELOG.md');
  // 图标：package.json 里声明的相对路径确实进了包，才写进清单。
  const iconRelative = typeof pkg.icon === 'string' ? pkg.icon : '';
  const hasIcon = !!iconRelative && packagedFiles.includes(iconRelative);
  const target = platform ? ` TargetPlatform="${escapeXml(platform)}"` : '';

  return `<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Identity Language="en-US" Id="${escapeXml(pkg.name)}" Version="${escapeXml(pkg.version)}" Publisher="${escapeXml(pkg.publisher || 'local')}"${target} />
    <DisplayName>${escapeXml(pkg.displayName || pkg.name)}</DisplayName>
    <Description xml:space="preserve">${escapeXml(pkg.description || '')}</Description>
    <Tags>${escapeXml(tags)}</Tags>
    <Categories>${escapeXml(categories)}</Categories>
    <GalleryFlags>Public</GalleryFlags>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="${escapeXml(engine)}" />
      <Property Id="Microsoft.VisualStudio.Code.ExtensionDependencies" Value="" />
      <Property Id="Microsoft.VisualStudio.Code.ExtensionPack" Value="" />
      <Property Id="Microsoft.VisualStudio.Code.ExtensionKind" Value="workspace" />
    </Properties>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code" />
  </Installation>
  <Dependencies />
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
    <Asset Type="Microsoft.VisualStudio.Services.Content.Details" Path="extension/README.md" Addressable="true" />
${hasChangelog ? '    <Asset Type="Microsoft.VisualStudio.Services.Content.Changelog" Path="extension/CHANGELOG.md" Addressable="true" />\n' : ''}${hasIcon ? `    <Asset Type="Microsoft.VisualStudio.Services.Icons.Default" Path="extension/${iconRelative}" Addressable="true" />\n` : ''}  </Assets>
</PackageManifest>
`;
}

/** 常见扩展名对应的 MIME 类型。 */
const CONTENT_TYPES = {
  '.json': 'application/json',
  '.js': 'application/javascript',
  '.md': 'text/markdown',
  '.txt': 'text/plain',
  '.xml': 'text/xml',
  '.vsixmanifest': 'text/xml',
  '.xshd': 'text/xml',
  '.css': 'text/css',
  '.html': 'text/html',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
};

/**
 * 生成 `[Content_Types].xml`：按实际打包用到的扩展名声明 MIME。
 *
 * @param {string[]} packagedFiles 已打包的相对路径。
 * @returns {string} XML 文本。
 */
function buildContentTypes(packagedFiles) {
  // 1. 收集出现过的扩展名（没有扩展名的文件用 "extension" 兜底）。
  const extensions = new Set(['.vsixmanifest']);
  for (const file of packagedFiles) {
    const ext = path.extname(file).toLowerCase();
    extensions.add(ext || '.bin');
  }

  // 2. 按固定顺序输出，保证每次打包结果稳定。
  const lines = [...extensions]
    .sort()
    .map(
      (ext) =>
        `  <Default Extension="${escapeXml(ext)}" ContentType="${
          CONTENT_TYPES[ext] || 'application/octet-stream'
        }" />`,
    );

  return `<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
${lines.join('\n')}
</Types>
`;
}

/**
 * 跑一个脚本；返回码非 0 会抛错，从而中止打包。
 *
 * @param {string} title 日志标题。
 * @param {string} script 相对插件根的脚本路径。
 */
function runScript(title, script) {
  log(`\n== ${title}：${script} ==`);
  execFileSync(process.execPath, [path.join(EXTENSION_DIR, script)], {
    cwd: EXTENSION_DIR,
    stdio: 'inherit',
  });
}

/**
 * 打包前的检查：
 *   1. 清单检查始终执行——`gram` 语言注册不完整就不该产出安装包；
 *   2. 两个冒烟测试可以用 --skip-tests 跳过（例如只想快速看包大小）。
 *
 * @param {boolean} skipTests 是否跳过冒烟测试。
 */
function runChecks(skipTests) {
  runScript('清单检查', 'tools/check-manifest.js');

  if (skipTests) {
    log('\n（--skip-tests：跳过冒烟测试）');
    return;
  }

  runScript('测试', 'test/smoke.js');
  runScript('测试', 'test/extension-smoke.js');
}

/**
 * 打包主流程。
 */
function main() {
  const options = parseArgs(process.argv.slice(2));

  // 1. 读清单并检查必需文件。
  const manifestPath = path.join(EXTENSION_DIR, 'package.json');
  if (!fs.existsSync(manifestPath)) {
    log(`找不到插件清单：${manifestPath}`);
    process.exit(1);
  }
  const pkg = readJson(manifestPath);

  // 先把编译器拷进 bin/：发布出去的 .vsix 自带 ParseEngine（内嵌语言服务器 + 生成器）。
  runScript('内嵌编译器', 'tools/embed-compiler.js');

  const required = [
    'extension.js',
    'language-configuration.json',
    `bin/ParseEngine${process.platform === 'win32' ? '.exe' : ''}`,
    'syntaxes/gram.tmLanguage.json',
    'icon.png',
    'README.md',
    'src/lspClient.js',
    'src/parseEngine.js',
    'src/providers.js',
    'src/serverLocator.js',
  ];
  const missing = required.filter((file) => !fs.existsSync(path.join(EXTENSION_DIR, file)));
  if (missing.length) {
    log(`插件缺少必需文件：${missing.join('、')}`);
    process.exit(1);
  }

  // 2. 按 .vscodeignore 收集文件。
  const patterns = loadIgnorePatterns();
  const files = [];
  collectFiles(EXTENSION_DIR, patterns, files);
  files.sort();

  log(`插件：${pkg.displayName || pkg.name} v${pkg.version}`);
  log(`打包文件 ${files.length} 个：`);
  for (const file of files) log(`  ${file}`);

  if (options.listOnly) {
    log('\n（--list：只列出文件，未打包）');
    return;
  }

  // 3. 默认先做清单检查与冒烟测试：不通过就不该产出安装包。
  runChecks(options.skipTests);

  // 4. 平台标记：包里带的是当前平台编出来的编译器，所以默认按当前平台标记。
  const platform = options.universal ? '' : vscodePlatform();

  // 5. 组装 ZIP 条目：清单与内容类型放根，插件文件放 extension/ 下。
  const entries = [
    { name: '[Content_Types].xml', data: Buffer.from(buildContentTypes(files), 'utf8') },
    { name: 'extension.vsixmanifest', data: Buffer.from(buildManifest(pkg, files, platform), 'utf8') },
  ];
  for (const file of files) {
    entries.push({
      name: `extension/${file}`,
      data: fs.readFileSync(path.join(EXTENSION_DIR, file)),
    });
  }

  // 6. 写文件。
  fs.mkdirSync(options.outDir, { recursive: true });
  const vsixName = `${pkg.name}-${pkg.version}${platform ? `-${platform}` : ''}.vsix`;
  const vsixPath = path.join(options.outDir, vsixName);
  const zip = createZip(entries);
  fs.writeFileSync(vsixPath, zip);

  log(`\n打包完成：${vsixPath}`);
  log(`目标平台：${platform || '通用（不带 TargetPlatform，需要机器上自己解决编译器）'}`);
  log(`大小：${(zip.length / 1024).toFixed(1)} KB，条目 ${entries.length} 个`);
  log(`安装：code --install-extension "${vsixPath}"`);
}

main();
