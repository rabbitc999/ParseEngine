"""校验并内嵌 ParseEngine 的中英日消息，不在运行时读取资源文件。"""
from pathlib import Path
import json
from embed_i18n import load

ROOT=Path(__file__).resolve().parents[1]

def main():
    """按 UTF-8 字节排序生成只读 C++ 消息表。"""
    rows=load(ROOT/'source/i18n/messages.tsv')
    quote=lambda text:json.dumps(text,ensure_ascii=False)
    content='/** @brief 自动生成的三语消息；修改 source/i18n/messages.tsv 后重新生成。 */\ninline constexpr Message messages[] = {\n'
    content+=''.join('    {'+', '.join(map(quote,[key,*rows[key]]))+'},\n' for key in sorted(rows,key=lambda value:value.encode('utf-8')))
    content+='};\n'
    target=ROOT/'source/include/ParseEngine/i18n_messages.hpp'
    if not target.exists() or target.read_text(encoding='utf-8')!=content:target.write_text(content,encoding='utf-8')
    print(f'ParseEngine: embedded {len(rows)} Chinese / English / Japanese messages')

if __name__=='__main__':main()
