"""校验并内嵌 ParseEngine 的中英双语消息，不在运行时读取资源文件。"""
from pathlib import Path
import json
import re

ROOT=Path(__file__).resolve().parents[1]

def load(catalog=None):
    """读取中英双语消息；保持 printf 参数顺序和换行原样。

    目录格式是 `中文|English`：`|` 左侧既是运行时查找的键，也是中文原文。
    """
    catalog=catalog or (ROOT/'source/i18n/messages.tsv')
    rows={}
    for line in catalog.read_text(encoding='utf-8-sig').splitlines():
        if not line or line.startswith('#'):continue
        parts=[x.replace(r'\p','|').replace(r'\n','\n').replace(r'\x20',' ') for x in line.split('|')]
        if len(parts)!=2 or not all(parts):raise ValueError(f'消息需要两个非空字段：{line}')
        if parts[0] in rows and rows[parts[0]]!=parts[1]:raise ValueError(f'重复键：{parts[0]}')
        formats=lambda s:re.findall(r'%(?:[-+ #0]*\d*(?:\.\d+)?(?:z|ll|l|h)?[sduifgxX])(?![A-Za-z_])',s)
        if formats(parts[0])!=formats(parts[1]):raise ValueError(f'格式参数不一致：{line}')
        placeholders=lambda value:set(re.findall(r"\{([A-Za-z_][A-Za-z_0-9]*)\}",value))
        if placeholders(parts[0])!=placeholders(parts[1]):raise ValueError(f'命名占位符不一致：{line}')
        rows[parts[0]]=parts[1]
    return rows

def main():
    """按 UTF-8 字节排序生成只读 C++ 消息表。"""
    rows=load()
    quote=lambda text:json.dumps(text,ensure_ascii=False)
    content='/** @brief 自动生成的中英双语消息；修改 source/i18n/messages.tsv 后重新生成。 */\ninline constexpr Message messages[] = {\n'
    content+=''.join('    {'+', '.join(map(quote,[key,rows[key]]))+'},\n' for key in sorted(rows,key=lambda value:value.encode('utf-8')))
    content+='};\n'
    target=ROOT/'source/include/ParseEngine/i18n_messages.hpp'
    if not target.exists() or target.read_text(encoding='utf-8')!=content:target.write_text(content,encoding='utf-8')
    print(f'ParseEngine: embedded {len(rows)} Chinese / English messages')

if __name__=='__main__':main()
