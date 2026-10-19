"""从仓库固定的 Unicode 17.0.0 数据生成 C/C++ 共用正则字符表。"""
from pathlib import Path
import hashlib

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "source" / "unicode" / "17.0.0"
OUTPUT = ROOT / "source" / "templates" / "regex_unicode_data.h.tmpl"


def ranges(points):
    result = []
    for point in points:
        if result and result[-1][1] + 1 == point:
            result[-1][1] = point
        else:
            result.append([point, point])
    return result


def main():
    names = "Lu Ll Lt Lm Lo Mn Mc Me Nd Nl No Pc Pd Ps Pe Pi Pf Po Sm Sc Sk So Zs Zl Zp Cc Cf Cs Co Cn".split()
    ids = {name: index for index, name in enumerate(names)}
    categories = bytearray([ids["Cn"]]) * 0x110000
    first = None
    for line in (DATA / "UnicodeData.txt").read_text(encoding="utf-8").splitlines():
        fields = line.split(";")
        point, name, category = int(fields[0], 16), fields[1], fields[2]
        if name.endswith(", First>"):
            first = point
        elif name.endswith(", Last>"):
            categories[first:point + 1] = bytes([ids[category]]) * (point + 1 - first)
            first = None
        else:
            categories[point] = ids[category]
    properties = {name: ranges(p for p, value in enumerate(categories) if value == ids[name]) for name in names}
    for group in "LMNPSZC":
        accepted = {ids[name] for name in names if name.startswith(group)}
        properties[group] = ranges(p for p, value in enumerate(categories) if value in accepted)
    word = {ids[name] for name in names if name[0] in "LMN" or name == "Pc"}
    properties["Word"] = ranges(p for p, value in enumerate(categories) if value in word or p in (0x200C, 0x200D))
    spaces = []
    for line in (DATA / "PropList.txt").read_text(encoding="utf-8").splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        code, prop = [field.strip() for field in line.split(";")]
        if prop == "White_Space":
            ends = code.split("..")
            spaces.extend(range(int(ends[0], 16), int(ends[-1], 16) + 1))
    properties["White_Space"] = ranges(sorted(spaces))
    properties["ASCII"] = [[0, 127]]
    properties["Any"] = [[0, 0x10FFFF]]
    folding = {}
    for line in (DATA / "CaseFolding.txt").read_text(encoding="utf-8").splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        source, status, target, _ = [part.strip() for part in line.split(";")]
        if status in ("C", "S"):
            folding[int(source, 16)] = int(target, 16)
    assert all(target not in folding for target in folding.values())
    aliases = dict(zip("L M N P S Z C Lu Ll Lt Lm Lo Mn Mc Me Nd Nl No Pc Pd Ps Pe Pi Pf Po Sm Sc Sk So Zs Zl Zp Cc Cf Cs Co Cn".split(),
        "Letter Mark Number Punctuation Symbol Separator Other Uppercase_Letter Lowercase_Letter Titlecase_Letter Modifier_Letter Other_Letter Nonspacing_Mark Spacing_Mark Enclosing_Mark Decimal_Number Letter_Number Other_Number Connector_Punctuation Dash_Punctuation Open_Punctuation Close_Punctuation Initial_Punctuation Final_Punctuation Other_Punctuation Math_Symbol Currency_Symbol Modifier_Symbol Other_Symbol Space_Separator Line_Separator Paragraph_Separator Control Format Surrogate Private_Use Unassigned".split()))
    notice = [' * ' + line if line else ' *' for line in (DATA / "LICENSE.txt").read_text(encoding="utf-8").splitlines()]
    out = ['/* Unicode 17.0.0，生成自 source/unicode/17.0.0。'] + notice + [' */',
           '#ifndef PE_REGEX_UNICODE_DATA_H', '#define PE_REGEX_UNICODE_DATA_H',
           '#include <stdint.h>', '#include <stddef.h>', '#include <string.h>']
    for filename in ("UnicodeData.txt", "CaseFolding.txt", "PropList.txt"):
        out.append('/* ' + filename + ' SHA256: ' + hashlib.sha256((DATA / filename).read_bytes().replace(b"\r\n", b"\n")).hexdigest() + ' */')
    for name, spans in properties.items():
        out.append('static const uint32_t PeUnicode' + name + '[] = {')
        out.extend('    0x%Xu, 0x%Xu,' % tuple(span) for span in spans)
        out.append('};')
    out.append('static const uint32_t PeUnicodeCasePairs[][2] = {')
    out.extend('    { 0x%Xu, 0x%Xu },' % item for item in sorted(folding.items()))
    out.append('};')
    out.append(r'''/** @brief 二分查找码点区间。 @param ranges 区间表。 @param count 区间数。 @param point 码点。 @return 包含返回真。 */
static inline int PeUnicodeContains(const uint32_t *ranges, size_t count, uint32_t point)
{
    size_t low = 0, high = count;
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        if (point < ranges[2 * middle]) high = middle;
        else if (point > ranges[2 * middle + 1]) low = middle + 1;
        else return 1;
    }
    return 0;
}
/** @brief Unicode 默认简单大小写折叠（C/S 映射）。 @param point 码点。 @return 折叠后的码点。 */
static inline uint32_t PeUnicodeFold(uint32_t point)
{
    size_t low = 0, high = sizeof(PeUnicodeCasePairs) / sizeof(PeUnicodeCasePairs[0]);
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        if (point < PeUnicodeCasePairs[middle][0]) high = middle;
        else if (point > PeUnicodeCasePairs[middle][0]) low = middle + 1;
        else return PeUnicodeCasePairs[middle][1];
    }
    return point;
}
/** @brief Unicode 属性查表。 @param name 属性名。 @param count 输出区间数。 @return 区间表，未知属性返回空指针。 */
static inline const uint32_t *PeUnicodeProperty(const char *name, size_t *count)
{''')
    for name in properties:
        values = [name]
        if name in aliases:
            values.append(aliases[name])
        if name == "Nd":
            values.append("Decimal_Digit_Number")
        condition = ' || '.join('strcmp(name, "' + value + '") == 0' for value in values)
        out.append('    if (' + condition + ') { *count = sizeof(PeUnicode' + name + ') / (2 * sizeof(uint32_t)); return PeUnicode' + name + '; }')
    out.extend(['    *count = 0; return NULL;', '}', '#endif', ''])
    OUTPUT.write_text('\n'.join(out), encoding="utf-8")
    print(f"wrote {OUTPUT}: {len(folding)} folds, {len(properties)} properties")


if __name__ == "__main__":
    main()
