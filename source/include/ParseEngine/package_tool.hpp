/** @brief 自动生成的独立 Python ZIP 发布脚本。 */
inline constexpr const char packageScript[] = R"PE_PACKAGE("""独立 ZIP 发布脚本：使用 Python 标准库打包 Magel 或 ParseEngine 可执行文件。"""
from pathlib import Path
import argparse
import os
import platform
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def display_language(tool):
    """按工具环境、区域和 Windows UI 语言选择三语提示。"""
    setting = os.environ.get('MAGEL_LANG' if tool == 'magel' else 'PARSEENGINE_LANG', '')
    if not setting or setting == 'auto':
        setting = next((os.environ[key] for key in ('LC_ALL', 'LC_MESSAGES', 'LANG') if os.environ.get(key)), '')
    if not setting and os.name == 'nt':
        import ctypes
        primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff
        return {0x04: 'zh', 0x11: 'ja'}.get(primary, 'en')
    code = setting.lower().replace('-', '_').split('_')[0].split('.')[0]
    return code if code in ('zh', 'en', 'ja') else 'en'

def package(compiler, output, name):
    """排他创建 DEFLATE 归档，失败清理本次文件，成功返回路径及大小。"""
    compiler = Path(compiler).resolve(strict=True)
    if not compiler.is_file() or not compiler.stat().st_size:
        raise ValueError(f'compiler: {compiler}')
    output = Path(output).absolute()
    if output.suffix.lower() != '.zip':
        raise ValueError('.zip')
    created = False
    try:
        with output.open('xb') as stream:
            created = True
            with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED,
                                 compresslevel=9, allowZip64=True) as archive:
                archive.write(compiler, name + ('.exe' if os.name == 'nt' else ''))
    except BaseException:
        if created:
            output.unlink(missing_ok=True)
        raise
    return output, compiler.stat().st_size, output.stat().st_size

def main(argv=None):
    """独立发布入口，CLI 包装器也调用相同脚本。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tool', choices=('magel', 'ParseEngine'))
    parser.add_argument('output_word', nargs='?', choices=('out',))
    parser.add_argument('output_path', nargs='?', type=Path)
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--ui-lang', choices=('zh', 'en', 'ja', 'auto'), default='auto')
    args = parser.parse_intermixed_args(argv)
    if bool(args.output_word) != bool(args.output_path) or (args.out and args.output_path):
        parser.error('use out FILE.zip or --out FILE.zip')
    language = display_language(args.tool) if args.ui_lang == 'auto' else args.ui_lang
    def text(zh, en, ja):
        return {'zh': zh, 'en': en, 'ja': ja}[language]
    if sys.version_info < (3, 11):
        print(text('打包需要 Python 3.11+', 'Packaging requires Python 3.11+', 'パッケージ化には Python 3.11+ が必要です'), file=sys.stderr)
        return 2
    executable = '.exe' if os.name == 'nt' else ''
    compiler = args.compiler or ROOT / ('magel/magel' + executable if args.tool == 'magel' else 'build/bin/ParseEngine' + executable)
    system = {'win32': 'windows', 'darwin': 'macos'}.get(sys.platform, sys.platform)
    machine = {'amd64': 'x64', 'x86_64': 'x64', 'aarch64': 'arm64'}.get(platform.machine().lower(), platform.machine().lower())
    version = ''
    if args.tool == 'magel':
        version_file = ROOT / 'magel/VERSION'
        if version_file.is_file():
            version = '-' + version_file.read_text(encoding='utf-8').strip()
    output = args.out or args.output_path or Path(f'{args.tool}{version}-{system}-{machine}.zip')
    try:
        path, original, packed = package(compiler, output, args.tool)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(text('打包失败：', 'Packaging failed: ', 'パッケージ化に失敗しました：') + str(error), file=sys.stderr)
        return 1
    print(text('已打包：', 'Packaged: ', 'パッケージ化しました：') + str(path) + f' (ZIP {packed}, original {original} bytes)')
    return 0

if __name__ == '__main__':
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8')
    raise SystemExit(main())
)PE_PACKAGE";
