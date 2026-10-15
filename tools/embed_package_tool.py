"""将独立 Python 发布脚本嵌入 ParseEngine；C++ 入口仅释放并运行该脚本。"""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def main():
    """生成静态源码，确保可执行文件脱离仓库仍能使用 zip 指令。"""
    script=(ROOT/'tools/package_compiler.py').read_text(encoding='utf-8')
    assert ')PE_PACKAGE"' not in script
    text='/** @brief 自动生成的独立 Python ZIP 发布脚本。 */\ninline constexpr const char packageScript[] = R"PE_PACKAGE('+script+')PE_PACKAGE";\n'
    target=ROOT/'source/include/ParseEngine/package_tool.hpp'
    if not target.exists() or target.read_text(encoding='utf-8')!=text:target.write_text(text,encoding='utf-8')

if __name__=='__main__':main()
