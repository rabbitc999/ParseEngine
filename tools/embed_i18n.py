"""生成无运行期文件依赖的中英日静态消息表；校验重复键与格式参数。"""
from pathlib import Path
import json
import re
ROOT=Path(__file__).resolve().parents[1]
CATALOG=ROOT/'magel/i18n/messages.tsv'
HEADER=ROOT/'magel/magel_i18n.hpp'

def load(catalog=CATALOG):
    """读取三语消息；保持 printf 参数顺序和换行原样。"""
    rows={}
    for line in catalog.read_text(encoding='utf-8-sig').splitlines():
        if not line or line.startswith('#'):continue
        parts=[x.replace(r'\p','|').replace(r'\n','\n').replace(r'\x20',' ') for x in line.split('|')]
        if len(parts)!=3 or not all(parts):raise ValueError(f'消息需要三个非空字段：{line}')
        if parts[0] in rows and rows[parts[0]]!=parts[1:]:raise ValueError(f'重复键：{parts[0]}')
        formats=lambda s:re.findall(r'%(?:[-+ #0]*\d*(?:\.\d+)?(?:z|ll|l|h)?[sduifgxX])(?![A-Za-z_])',s)
        if formats(parts[0])!=formats(parts[1]) or formats(parts[0])!=formats(parts[2]):raise ValueError(f'格式参数不一致：{line}')
        placeholders=lambda value:set(re.findall(r"\{([A-Za-z_][A-Za-z_0-9]*)\}",value))
        if placeholders(parts[0])!=placeholders(parts[1]) or placeholders(parts[0])!=placeholders(parts[2]):raise ValueError(f'命名占位符不一致：{line}')
        rows[parts[0]]=parts[1:]
    return rows

def generate(rows=None):
    """按 UTF-8 字节序生成可二分查询的只读表。"""
    rows=load() if rows is None else rows
    quote=lambda x:json.dumps(x,ensure_ascii=False)
    body='inline constexpr Message messages[] = {\n'+''.join('    {'+', '.join(map(quote,[zh,*rows[zh]]))+'},\n' for zh in sorted(rows,key=lambda s:s.encode('utf-8')))+'};\n'
    text=HEADER.read_text(encoding='utf-8')
    text=re.sub(r'(?<=// MAGEL_CATALOG_BEGIN\n)[\s\S]*?(?=// MAGEL_CATALOG_END)',lambda _:body,text)
    HEADER.write_text(text,encoding='utf-8')
    script=ROOT/'magel/tools/mageldev.py'
    python=script.read_text(encoding='utf-8')
    table='_MESSAGES = '+json.dumps(rows,ensure_ascii=False,indent=2)+'\n'
    python=re.sub(r'(?<=# MAGEL_CATALOG_BEGIN\n)[\s\S]*?(?=# MAGEL_CATALOG_END)',lambda _:table,python)
    script.write_text(python,encoding='utf-8')
    print(f'Magel: embedded {len(rows)} Chinese / English / Japanese messages')
if __name__=='__main__':generate()
