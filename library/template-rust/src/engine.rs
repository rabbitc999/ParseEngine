//! 模板引擎本体：`TemplateEngine`（外加便于调用的 `render` / `render_values`）。
//!
//! 用法就一行：
//!
//! ```
//! use template_engine::{encode_object, render, Encode, Value};
//!
//! struct Class {
//!     name: String,
//! }
//!
//! impl Encode for Class {
//!     fn encode(&self) -> Value {
//!         encode_object! { "name" => &self.name }
//!     }
//! }
//!
//! let class = Class { name: "Calc".to_string() };
//! let code = render("class {{ name }} { };", &class).unwrap();   // source 是字符串，不是文件
//! assert_eq!(code, "class Calc { };");
//! ```
//!
//! **引擎不读文件**：模板是调用方给的字符串（想从磁盘读就自己读，想从资源里取
//! 就从资源里取），这样引擎不掺和路径、编码、权限这些事。
//!
//! 支持的语法（只做代码生成够用的那一小套，刻意不做表达式求值）：
//!
//! ```text
//! {{ path.to.value }}      变量替换：缺失输出空串，不报错
//! {% if cond %} … {% else %} … {% endif %}    单路径真值判断，可无限嵌套
//! {% for item in list %} … {% endfor %}       遍历数组，循环体里能访问外层变量
//!                                             （循环里还有 loop_index / loop_first /
//!                                              loop_last / loop_count 可用）
//! {% include "名字" %}     插入另一段模板：片段表（名字 → 片段正文）由调用方给，
//!                                             片段里还能再 include
//! {% raw %} … {% endraw %}                   原样输出：里面的 {{ }} / {% %} 不当标签
//! {# 注释 #}               解析时直接丢掉
//! ```
//!
//! `{% include %}` 的几条约定：
//!
//!   * 片段正文**按模板渲染**，用的是**同一个上下文**——片段里能用外层变量，
//!     放进 `{% for %}` 里还能取到循环变量；
//!   * 片段名引号可省（`{% include "a" %}` / `{% include 'a' %}` / `{% include a %}`），
//!     名字里不认转义；
//!   * 片段里再 `{% include %}` 就是嵌套包含，层数上限
//!     [`TemplateEngine::MAX_INCLUDE_DEPTH`]，超了返回
//!     [`TemplateError::IncludeTooDeep`]——写坏的互相包含不会把栈转爆；
//!   * 片段表里没有这个名字、压根没给片段表、或者标签里没写出名字 → 输出空串：
//!     和"缺变量 / 空列表 / 不认识的指令"一个口径，不报错、不崩。
//!
//! 实现要点：
//!
//!   * **不用正则**：全程 `find` + 切片，可预测、不 backtracking；
//!   * **[`render_block`]**：一个主循环，从 `pos` 往下扫，遇到 `stop_tag` 就
//!     "吃掉它并返回"，控制权交回上层——`if` / `for` 的嵌套就是靠这个停-回机制
//!     递归起来的；`env` 里装着 `{% include %}` 的片段表与嵌套深度
//!     （引擎本身仍然无状态，这两个值只跟着递归走）；
//!   * **[`find_matching`]**：按**嵌套深度**找配对的 `{% endif %}` / `{% endfor %}`，
//!     所以内层标签不会被当成外层的结束；
//!   * **[`ChildModel`]**：循环体每轮一个子上下文，`item` 命中当前元素，其它名字
//!     顺着 `parent` 往上找——循环体里照样能用外层变量；
//!   * **未闭合标签返回错误**，其余（缺变量、空列表、缺片段、不认识的指令）
//!     一律安全跳过，不崩。
//!
//! 额外的"整行删除"规则（写代码生成模板时非常实用）：块标签（`{% %}` / `{# #}`）
//! 如果**独占一行**（前面只有缩进、后面只有行尾空白），那么这一行连同换行一起删掉。
//! 上面例子里的
//!
//! ```text
//! class {{ name }} {
//! {% for m in methods %}
//!     {{ m.ret_type }} {{ m.name }}({{ m.args }});
//! {% endfor %}
//! };
//! ```
//!
//! 渲染出来不会多出一堆空行；而 `{% if x %}public {% endif %}class` 这种
//! 同一行里有别的内容的标签不受影响，只删标签本身。

use crate::bind::Encode;
use crate::model::{resolve, EncodedModel, MapModel, Model};
use crate::value::Value;
use std::collections::BTreeMap;
use std::fmt;
use std::sync::OnceLock;

/// `{% include "名字" %}` 的片段表：名字 → 片段正文。
///
/// 用 `BTreeMap` 而不是 `HashMap`：片段就那几个，**键有序**（调试时看得舒服）
/// 比 O(1) 更值——C++ 版用的也是有序的 `std::map`，两边取证方式一致。
///
/// 表是**调用方**的东西：引擎不读文件，想让片段来自磁盘就自己读进来填表，
/// 想懒加载就在填表这一步做（引擎只在渲染到那一行时才查表，不会预先展开）。
pub type SnippetTable = BTreeMap<String, String>;

/// 空片段表：`render` / `render_values` 没给片段表时用的那份。
///
/// 模板里写了 `{% include %}` 但没给表时，所有 include 都当"取不到"跳过。
pub fn no_snippets() -> &'static SnippetTable {
    static EMPTY: OnceLock<SnippetTable> = OnceLock::new();
    EMPTY.get_or_init(SnippetTable::new)
}

/// 造片段表的小工具：`snippet_table([("header", "…"), ("body", "…")])`。
///
/// 存在的意义只是省掉一堆 `.to_string()`——`BTreeMap::from([...])` 要求键值已经是
/// `String`，模板字面量写起来会很啰嗦。
///
/// ```
/// use template_engine::{snippet_table, render_with, template_model};
///
/// template_model! { struct Doc { name: String } }
///
/// let snippets = snippet_table([("head", "// {{ name }}\n")]);
/// let text = render_with("{% include \"head\" %}int main() {}\n", &Doc::new("calc"), &snippets)
///     .unwrap();
/// assert_eq!(text, "// calc\nint main() {}\n");
/// ```
pub fn snippet_table<K, V, I>(pairs: I) -> SnippetTable
where
    K: Into<String>,
    V: Into<String>,
    I: IntoIterator<Item = (K, V)>,
{
    pairs.into_iter().map(|(key, value)| (key.into(), value.into())).collect()
}

/// 渲染时的错误。**只有两类**：标签没闭合、include 嵌套过深。
///
/// 其余情况（缺变量、空列表、缺片段、不认识的指令）都不算错误，安全跳过。
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TemplateError {
    /// 变量标签没闭合：`{{` 找不到 `}}`。
    UnclosedVariable,
    /// 注释没闭合：`{#` 找不到 `#}`。
    UnclosedComment,
    /// 块标签没闭合：`{%` 找不到 `%}`，或者 `{% if %}` / `{% for %}` / `{% raw %}`
    /// 找不到配对的 `{% endif %}` / `{% endfor %}` / `{% endraw %}`（参数是那个
    /// 没配上的标签）。
    UnclosedTag(String),
    /// `{% include %}` 嵌套层数超过 [`TemplateEngine::MAX_INCLUDE_DEPTH`]
    /// （参数是出事的片段名）：多半是片段互相包含。
    IncludeTooDeep(String),
}

impl fmt::Display for TemplateError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TemplateError::UnclosedVariable => formatter.write_str("未闭合变量标签: {{"),
            TemplateError::UnclosedComment => formatter.write_str("未闭合注释: {#"),
            TemplateError::UnclosedTag(tag) => write!(formatter, "未闭合标签: {tag}"),
            TemplateError::IncludeTooDeep(name) => write!(
                formatter,
                "{{% include %}} 嵌套过深（疑似片段互相包含）: {name}"
            ),
        }
    }
}

impl std::error::Error for TemplateError {}

/// 循环体的子上下文：`item` 自己的值 + 指向外层的引用。
///
/// `{% for m in methods %}` 每轮构造一个 `ChildModel`：`get("m")` 命中当前元素，
/// 其它名字顺着 `parent` 往上找。这就是"循环体里还能用外层变量"的实现——
/// 一个手写的动态作用域链，不需要额外的符号表。
pub struct ChildModel<'a> {
    /// 外层上下文。
    pub parent: &'a dyn Model,
    /// 循环变量名（`{% for m in … %}` 里的 `m`）。
    pub item_name: String,
    /// 当前元素。
    pub item_value: Value,
    /// 当前元素下标（0 起）。
    pub item_index: i64,
    /// 这一轮循环的元素个数。
    pub item_count: i64,
}

impl<'a> ChildModel<'a> {
    /// 造一个子上下文。
    pub fn new(
        parent: &'a dyn Model,
        name: impl Into<String>,
        value: Value,
        index: i64,
        count: i64,
    ) -> Self {
        ChildModel {
            parent,
            item_name: name.into(),
            item_value: value,
            item_index: index,
            item_count: count,
        }
    }
}

impl Model for ChildModel<'_> {
    /// 按名字取值：先找循环变量与循环元信息，再顺着作用域链往上找。
    ///
    /// 循环元信息（模板里排版数组、`switch` 分支这些就靠它，省得在 Rust 里拼
    /// 分隔符 / 换行）：
    ///
    ///   * `loop_index`：当前元素下标（0 起）；
    ///   * `loop_first`：是不是第一个；
    ///   * `loop_last`：是不是最后一个；
    ///   * `loop_count`：这一轮循环一共几个元素。
    fn get(&self, key: &str) -> Value {
        if key == self.item_name {
            return self.item_value.clone();
        }
        match key {
            "loop_index" => Value::Int(self.item_index),
            "loop_first" => Value::Bool(self.item_index == 0),
            "loop_last" => {
                Value::Bool(self.item_count > 0 && self.item_index + 1 == self.item_count)
            }
            "loop_count" => Value::Int(self.item_count),
            _ => self.parent.get(key), // 作用域链向上查找
        }
    }
}

/// 模板引擎：把模板字符串 + [`Model`] 渲染成文本。
///
/// 无状态、全关联函数，直接用 `TemplateEngine::render(...)`；平时更推荐用
/// 文件末尾那几个 `render(...)` / `render_values(...)` 便捷函数。
pub struct TemplateEngine;

impl TemplateEngine {
    /// `{% include %}` 的嵌套深度上限（C++ 版的 `k_maxIncludeDepth`）。
    ///
    /// 片段 A 包含 B、B 又包含 A 这种写坏的模板，光靠"深度"兜住：到了这个层数
    /// 就返回错误，不会一直递归到爆栈。正常模板嵌套几层就够，给到 32 是留足
    /// 余量（层数是"同时打开了多少个片段"，不是"一共 include 了几次"）。
    pub const MAX_INCLUDE_DEPTH: usize = 32;

    /// 渲染整份模板。
    ///
    /// * `source` —— 模板字符串；
    /// * `root` —— 根上下文；
    /// * `snippets` —— `{% include %}` 的片段表（名字 → 片段正文）；给
    ///   [`no_snippets`] 就是空表，模板里的 include 一律输出空串。
    ///
    /// 返回渲染结果；标签没闭合、或 `{% include %}` 嵌套超过
    /// [`MAX_INCLUDE_DEPTH`](TemplateEngine::MAX_INCLUDE_DEPTH) 时返回错误。
    pub fn render(
        source: &str,
        root: &dyn Model,
        snippets: &SnippetTable,
    ) -> Result<String, TemplateError> {
        let env = Env { snippets, include_depth: 0 };
        let mut pos = 0usize;
        render_block(source, &mut pos, root, "", &env)
    }
}

/// 渲染一趟的"外部环境"：片段表 + include 嵌套深度。
///
/// 引擎不把这两个值放进成员（那样就不是无状态的了），而是当参数一路跟着递归
/// 往下传。每进一层 `{% include %}` 换一个 `include_depth + 1` 的副本，
/// 这就是循环包含的刹车。
struct Env<'a> {
    /// `{% include %}` 的片段表。
    snippets: &'a SnippetTable,
    /// 当前片段嵌套层数（0 = 不在片段里）。
    include_depth: usize,
}

// ---------------------------------------------------------------------------
//  主循环
// ---------------------------------------------------------------------------

/// 从 `pos` 开始渲染，直到模板结束或撞上 `stop_tag`。
///
/// 这是整个引擎的心脏：`if` / `for` 的分支内容都是"再用一个 `stop_tag` 调用一次
/// 自己"渲染出来的，所以嵌套是天然的，不需要建语法树。
///
/// * `source` —— 模板字符串；
/// * `pos` —— 当前位置；返回时停在**停下来的那个标签之后**（没停就是
///   `source.len()`）。调用方靠它继续往下渲染；
/// * `ctx` —— 当前上下文（可能是 [`ChildModel`]，往上还能找到外层变量）；
/// * `stop_tag` —— 停止标签（如 `"{% endif %}"`）；空表示一路渲染到底；
/// * `env` —— 片段表与 include 嵌套深度（`{% include %}` 要用；其余分支只是
///   原样往下传）。
///
/// 返回这段模板的输出。
fn render_block(
    source: &str,
    pos: &mut usize,
    ctx: &dyn Model,
    stop_tag: &str,
    env: &Env<'_>,
) -> Result<String, TemplateError> {
    let mut out = String::new();

    while *pos < source.len() {
        // 1. 到达停止标签（比如 {% endif %}）：吃掉它，把控制权交回上层。
        if !stop_tag.is_empty() && is_stop_tag(source, *pos, stop_tag) {
            let end = match find_from(source, "%}", *pos) {
                Some(end) => end,
                None => return Err(TemplateError::UnclosedTag(stop_tag.to_string())),
            };
            *pos = consume_block_tag_line(source, *pos, end + 2, &mut out);
            return Ok(out);
        }

        // 2. 找下一个 {{ / {% / {#，取最近的那个。
        let var_pos = find_from(source, "{{", *pos);
        let tag_pos = find_from(source, "{%", *pos);
        let cmt_pos = find_from(source, "{#", *pos);
        let next_pos = match [var_pos, tag_pos, cmt_pos].into_iter().flatten().min() {
            Some(next_pos) => next_pos,
            None => {
                // 3. 后面没有标签了：剩下的全是纯文本。
                out.push_str(&source[*pos..]);
                *pos = source.len();
                return Ok(out);
            }
        };

        // 4. 先把标签之前的纯文本原样输出。
        out.push_str(&source[*pos..next_pos]);
        *pos = next_pos;

        // 5. 按标签种类分发。
        if Some(next_pos) == cmt_pos {
            // ---- 注释：{# … #} 整段丢掉，不输出 ----
            let end = match find_from(source, "#}", *pos) {
                Some(end) => end,
                None => return Err(TemplateError::UnclosedComment),
            };
            *pos = consume_block_tag_line(source, *pos, end + 2, &mut out);
        } else if Some(next_pos) == var_pos {
            // ---- 变量：{{ path.to.value }} ----
            let end = match find_from(source, "}}", *pos) {
                Some(end) => end,
                None => return Err(TemplateError::UnclosedVariable),
            };
            let path = trim(&source[*pos + 2..end]);
            // 缺失变量 → 空串。
            out.push_str(&resolve(ctx, path).to_text());
            *pos = end + 2;
        } else {
            // ---- 块标签：{% … %} ----
            let end = match find_from(source, "%}", *pos) {
                Some(end) => end,
                None => return Err(TemplateError::UnclosedTag("{%".to_string())),
            };
            let tag_start = *pos;
            let tag_end = end + 2;
            let tag = trim(&source[*pos + 2..end]);

            // 内容式停止判定：{%endif%} 这种不留空格的写法也能停住。
            if !stop_tag.is_empty() && matches_stop_command(tag, stop_tag) {
                *pos = consume_block_tag_line(source, tag_start, tag_end, &mut out);
                return Ok(out);
            }

            // 标签独占一行时整行删掉；body_start 就是"标签之后该从哪继续"。
            let body_start = consume_block_tag_line(source, tag_start, tag_end, &mut out);

            if command_is(tag, "if") {
                // ---- 条件：{% if cond %} … {% else %} … {% endif %} ----
                let cond_path = trim(&tag[2..]);
                let cond = resolve(ctx, cond_path).truthy();

                // 配对的 endif：按嵌套深度找，内层的 if/endif 只是把深度加减一下。
                let endif_pos = find_matching(source, body_start, "{% if ", "{% endif %}")?;
                // 与它配对的**第一个顶层** else（没有就是 None）。
                let else_pos = find_top_level_else(source, body_start, Some(endif_pos));

                if cond {
                    // then 分支：渲染到 {% else %} 或 {% endif %} 为止。
                    let stop = match else_pos {
                        Some(_) => "{% else %}",
                        None => "{% endif %}",
                    };
                    let mut branch_pos = body_start;
                    out.push_str(&render_block(source, &mut branch_pos, ctx, stop, env)?);
                }
                if !cond {
                    if let Some(else_pos) = else_pos {
                        // else 分支：从 {% else %} 之后渲染到 {% endif %} 为止。
                        let mut branch_pos = after_tag(source, Some(else_pos));
                        out.push_str(&render_block(
                            source,
                            &mut branch_pos,
                            ctx,
                            "{% endif %}",
                            env,
                        )?);
                    }
                }

                // 不管走了哪条分支，都从配对的 {% endif %} 之后接着渲染。
                *pos = after_tag(source, Some(endif_pos));
            } else if command_is(tag, "for") {
                // ---- 循环：{% for item in list.path %} … {% endfor %} ----
                let endfor_pos = find_matching(source, body_start, "{% for ", "{% endfor %}")?;
                let spec = trim(&tag[3..]); // "item in list.path"
                match spec.find(" in ") {
                    // 写坏的 for：整块跳过（不能只跳标签，否则会连着把后面吃掉）。
                    None => *pos = after_tag(source, Some(endfor_pos)),
                    Some(in_pos) => {
                        let item_name = trim(&spec[..in_pos]);
                        let list_path = trim(&spec[in_pos + 4..]);
                        let list = resolve(ctx, list_path);

                        // 列表不存在或为空：整块跳过（但位置要挪到 endfor 之后）。
                        if let Some(elements) = list.as_array() {
                            if !elements.is_empty() {
                                let element_count = elements.len() as i64;
                                for (index, element) in elements.iter().enumerate() {
                                    // 每轮一个子上下文：item 指向当前元素，loop_* 给
                                    // 下标 / 首尾标志，其余走作用域链。
                                    let child = ChildModel::new(
                                        ctx,
                                        item_name,
                                        element.clone(),
                                        index as i64,
                                        element_count,
                                    );
                                    let mut body_pos = body_start;
                                    out.push_str(&render_block(
                                        source,
                                        &mut body_pos,
                                        &child,
                                        "{% endfor %}",
                                        env,
                                    )?);
                                }
                            }
                        }
                        *pos = after_tag(source, Some(endfor_pos));
                    }
                }
            } else if command_is(tag, "raw") {
                // ---- 原样段：{% raw %} … {% endraw %} ----
                // 里面的 {{ }} / {% %} / {# #} 一个字都不当标签，直接抄进产物。
                // 生成 C / C# 这类语言的模板常要写字面量花括号（比如说明
                // `{{ target_code }}` 这种文法写法），就靠它来"转义"。
                let raw_end = find_matching(source, body_start, "{% raw %}", "{% endraw %}")?;
                out.push_str(&source[body_start..raw_end]);
                *pos = after_tag(source, Some(raw_end));
            } else if command_is(tag, "include") {
                // ---- 片段：{% include "名字" %} ----
                // 片段正文取自调用方给的片段表，**按模板渲染**：用的是同一个
                // ctx，所以片段里能用外层变量、放进循环体里也能取到循环变量。
                let name = include_name(tag);
                let snippets = env.snippets;
                if !name.is_empty() {
                    if let Some(snippet) = snippets.get(&name) {
                        if env.include_depth >= TemplateEngine::MAX_INCLUDE_DEPTH {
                            // 片段互相包含（哪怕绕了好几层）在此打住，别把栈转爆。
                            return Err(TemplateError::IncludeTooDeep(name));
                        }
                        // 独占一行时，这一行前面的缩进已经被 consume_block_tag_line
                        // 从 out 末尾收回去了——所以片段正文接在"收干净之后"的
                        // out 后面，不会多出缩进，也不会多出空行。
                        let nested =
                            Env { snippets, include_depth: env.include_depth + 1 };
                        let mut snippet_pos = 0usize;
                        out.push_str(&render_block(snippet, &mut snippet_pos, ctx, "", &nested)?);
                    }
                }
                // 表里没这个名字 / 没给表 / 标签里没写名字：输出空串，安全跳过。
                *pos = body_start;
            } else {
                // {% else %} / {% endif %} / {% endfor %} 走到这里说明配对错乱，
                // 不认识的指令也在这里：安全跳过（只删标签本身，不删内容）。
                *pos = body_start;
            }
        }
    }

    Ok(out)
}

// ---------------------------------------------------------------------------
//  嵌套匹配
// ---------------------------------------------------------------------------

/// 找与 `open_tag` 配对的 `close_tag`，按**嵌套深度**取最外层那个。
///
/// 例如从 `{% if a %}` 之后找 `{% endif %}`：中间每遇到一个 `{% if … %}`
/// 深度 +1、每遇到一个 `{% endif %}` 深度 -1，深度回到 0 的那个才是要的
/// ——内层的 endif 不会被当成外层的结束。
///
/// 找不到配对的结束标签时返回 [`TemplateError::UnclosedTag`]。
fn find_matching(
    source: &str,
    pos: usize,
    open_tag: &str,
    close_tag: &str,
) -> Result<usize, TemplateError> {
    let open = tag_command(open_tag);
    let close = tag_command(close_tag);
    let mut depth = 1i32;
    let mut cur = pos;

    loop {
        let tag_pos = match next_tag(source, cur, source.len()) {
            Some(tag_pos) => tag_pos,
            None => return Err(TemplateError::UnclosedTag(close_tag.to_string())),
        };
        let end = match find_from(source, "%}", tag_pos) {
            Some(end) => end,
            None => return Err(TemplateError::UnclosedTag(close_tag.to_string())),
        };

        let command = trim(&source[tag_pos + 2..end]);
        if !open.is_empty() && command_is(command, open) {
            depth += 1; // 又开了一层
        } else if command == close {
            depth -= 1; // 关掉一层
            if depth == 0 {
                return Ok(tag_pos);
            }
        }

        cur = end + 2;
    }
}

/// 找 `[from, to)` 之间**顶层**的第一个 `{% else %}`。
///
/// "顶层"= `if` 嵌套深度为 0：内层 if 自己的 else 不算（它属于内层的
/// `{% endif %}`），否则 `{% else %}` 会被判错。
///
/// 返回 `{%` 的位置；没有顶层 else 返回 `None`。
fn find_top_level_else(source: &str, from: usize, to: Option<usize>) -> Option<usize> {
    let limit = to.unwrap_or(source.len());
    let mut depth = 0i32;
    let mut pos = from;

    loop {
        let tag_pos = next_tag(source, pos, limit)?;
        let end = find_from(source, "%}", tag_pos)?;

        let command = trim(&source[tag_pos + 2..end]);
        if command_is(command, "if") {
            depth += 1; // 内层 if
        } else if command == "endif" {
            if depth > 0 {
                depth -= 1; // 内层 endif
            }
        } else if command == "else" && depth == 0 {
            return Some(tag_pos); // 外层等着的那个
        }

        pos = end + 2;
    }
}

// ---------------------------------------------------------------------------
//  空白处理
// ---------------------------------------------------------------------------

/// 去掉字符串两端的空白。
fn trim(text: &str) -> &str {
    let bytes = text.as_bytes();
    let mut begin = 0usize;
    let mut end = bytes.len();
    while begin < end && is_space(bytes[begin]) {
        begin += 1;
    }
    while end > begin && is_space(bytes[end - 1]) {
        end -= 1;
    }
    &text[begin..end]
}

/// 去掉字符串左端的空白。
fn trim_left(text: &str) -> &str {
    let bytes = text.as_bytes();
    let mut begin = 0usize;
    while begin < bytes.len() && is_space(bytes[begin]) {
        begin += 1;
    }
    &text[begin..]
}

/// 去掉前缀 `[0, limit)` 右端的**行内空白**（空格 / 制表符）。
///
/// 块标签独占一行时要"整行删掉"：标签左边那点缩进在发现标签之前就已经
/// 写进 `out` 了，用它把缩进收回来（换行要留着，否则两行会粘在一起）。
fn trim_right(text: &str, limit: usize) -> &str {
    let bytes = text.as_bytes();
    let mut end = limit.min(bytes.len());
    while end > 0 && (bytes[end - 1] == b' ' || bytes[end - 1] == b'\t') {
        end -= 1;
    }
    &text[..end]
}

/// 判断 `[tag_start, tag_end)` 这个块标签是不是**独占一行**。
///
/// 条件：标签前面（本行内）只有空格 / 制表符，标签后面只有空格 / 制表符
/// 然后就是换行或模板结束。这种标签通常只是"给模板用的"，整行删掉才不会
/// 在生成代码里留一堆空行；`{% if x %}public {% endif %}class` 这种同行里
/// 还有别的内容的标签不会被判成独占一行。
fn is_standalone_line(source: &str, tag_start: usize, tag_end: usize) -> bool {
    let bytes = source.as_bytes();

    // 前面：从标签往左退到行首，中间只能有空格 / 制表符。
    let mut line_begin = tag_start;
    while line_begin > 0 && bytes[line_begin - 1] != b'\n' {
        line_begin -= 1;
    }
    if !trim_left(&source[line_begin..tag_start]).is_empty() {
        return false;
    }

    // 后面：从标签往右退到行尾，中间只能有空格 / 制表符（\r 交给 trim）。
    let mut line_end = tag_end;
    while line_end < bytes.len() && bytes[line_end] != b'\n' {
        line_end += 1;
    }
    trim(&source[tag_end..line_end]).is_empty()
}

/// 吃掉 `pos` 起的行尾行内空白与一个换行。
fn skip_line_end(source: &str, pos: usize) -> usize {
    let bytes = source.as_bytes();
    let mut pos = pos;
    while pos < bytes.len() && (bytes[pos] == b' ' || bytes[pos] == b'\t' || bytes[pos] == b'\r') {
        pos += 1;
    }
    if pos < bytes.len() && bytes[pos] == b'\n' {
        pos += 1;
    }
    pos
}

/// 块标签之后该从哪继续：独占一行时连行尾空白与换行一起跳过。
fn next_pos_after_tag(source: &str, tag_start: usize, tag_end: usize) -> usize {
    if !is_standalone_line(source, tag_start, tag_end) {
        return tag_end;
    }
    skip_line_end(source, tag_end)
}

/// 已经处理完一个块标签：独占一行时把这一行整行删掉。
///
/// 两件事：把 `out` 末尾已经写出去的本行缩进收回来（[`trim_right`]），再把
/// 标签之后到行尾的空白连同换行吃掉（[`skip_line_end`]）。
fn consume_block_tag_line(
    source: &str,
    tag_start: usize,
    tag_end: usize,
    out: &mut String,
) -> usize {
    if !is_standalone_line(source, tag_start, tag_end) {
        return tag_end;
    }
    // 收回本行的缩进（换行保留）。
    let keep = trim_right(out, out.len()).len();
    out.truncate(keep);
    skip_line_end(source, tag_end)
}

/// 从 `tag_pos`（`{%` 的位置）出发，返回这个标签之后该继续的位置。
///
/// `tag_pos` 给 `None` 表示"没有这个标签"，直接落到模板末尾（C++ 版传 `npos`
/// 时就是这个行为）。
fn after_tag(source: &str, tag_pos: Option<usize>) -> usize {
    let tag_pos = match tag_pos {
        Some(tag_pos) => tag_pos,
        None => return source.len(),
    };
    match find_from(source, "%}", tag_pos) {
        Some(end) => next_pos_after_tag(source, tag_pos, end + 2),
        None => source.len(),
    }
}

// ---------------------------------------------------------------------------
//  标签扫描与识别
// ---------------------------------------------------------------------------

/// 从 `from` 开始找 `needle`，返回**整串里的**字节下标；找不到 / `from` 越界返回 `None`。
fn find_from(source: &str, needle: &str, from: usize) -> Option<usize> {
    if from > source.len() {
        return None;
    }
    source[from..].find(needle).map(|offset| offset + from)
}

/// 从 `pos` 开始找下一个 `{% … %}` 标签，沿途跳过 `{# … #}` 注释。
///
/// 只在这个 `limit` 之前找（不含）；找不到返回 `None`。
fn next_tag(source: &str, mut pos: usize, limit: usize) -> Option<usize> {
    while pos < limit {
        let tag_pos = find_from(source, "{%", pos);
        let cmt_pos = find_from(source, "{#", pos);
        if tag_pos.is_none() && cmt_pos.is_none() {
            return None;
        }

        // 注释里的 {% endif %} 之类的文字不算标签。
        if let Some(cmt_pos) = cmt_pos {
            let comment_first = match tag_pos {
                Some(tag_pos) => cmt_pos < tag_pos,
                None => true,
            };
            if comment_first {
                // 注释没闭合：后面都不算标签。
                let end = find_from(source, "#}", cmt_pos)?;
                pos = end + 2;
                continue;
            }
        }

        return match tag_pos {
            Some(tag_pos) if tag_pos < limit => Some(tag_pos),
            _ => None,
        };
    }
    None
}

/// 把字面标签拆成指令名：`"{% endif %}"` → `"endif"`。
fn tag_command(tag: &str) -> &str {
    let mut out = trim(tag);
    if let Some(rest) = out.strip_prefix("{%") {
        out = rest;
    }
    if let Some(rest) = out.strip_suffix("%}") {
        out = rest;
    }
    trim(out)
}

/// 标签（已 trim）的指令名是不是 `command`。
///
/// `command_is("if has_ctor", "if")` 为真，`command_is("endif", "if")` 为假
/// ——多了个 `end` 前缀就是另一条指令。
fn command_is(tag: &str, command: &str) -> bool {
    if tag == command {
        return true;
    }
    tag.len() > command.len()
        && tag.starts_with(command)
        && is_space(tag.as_bytes()[command.len()])
}

/// 从 `{% include … %}` 的标签里取出**片段名**。
///
/// 三种写法都认：
///
/// ```text
/// {% include "header" %}      // 双引号
/// {% include 'header' %}      // 单引号
/// {% include header %}        // 不带引号（取到空白为止）
/// ```
///
/// 名字里不认转义（片段名不是变量，走的是片段表的键，不需要转义）；
/// `include` 后面什么都没有、或者引号没闭合，都返回空串——调用方按
/// "取不到片段"处理（输出空串，不报错）。
fn include_name(tag: &str) -> String {
    let blank = match tag.find([' ', '\t', '\r', '\n']) {
        Some(blank) => blank,
        None => return String::new(), // 光有指令名，没写名字
    };
    let rest = trim(&tag[blank..]);
    if rest.is_empty() {
        return String::new();
    }

    let quote = rest.as_bytes()[0];
    if quote == b'"' || quote == b'\'' {
        return match rest[1..].find(quote as char) {
            Some(close) => rest[1..1 + close].to_string(),
            None => String::new(), // 引号没闭合
        };
    }
    match rest.find([' ', '\t', '\r', '\n']) {
        Some(stop) => rest[..stop].to_string(),
        None => rest.to_string(),
    }
}

/// 判断 `source` 的 `pos` 处是不是 `stop_tag`。
///
/// `stop_tag` 传的是字面标签（`"{% endif %}"`），但这里同时接受
/// `{%endif%}` 这种不留空格的写法：字面匹配失败时，把标签内容 trim 出来
/// 再和 `stop_tag` 的指令名比一次。
fn is_stop_tag(source: &str, pos: usize, stop_tag: &str) -> bool {
    if source[pos..].starts_with(stop_tag) {
        return true;
    }
    if !source[pos..].starts_with("{%") {
        return false;
    }
    match find_from(source, "%}", pos + 2) {
        Some(end) => trim(&source[pos + 2..end]) == tag_command(stop_tag),
        None => false,
    }
}

/// 标签内容（已 trim）是不是 `stop_tag` 对应的指令。
fn matches_stop_command(tag: &str, stop_tag: &str) -> bool {
    tag == tag_command(stop_tag)
}

/// 是不是空白字符（模板里的空白按 ASCII 处理，不认 locale）。
fn is_space(byte: u8) -> bool {
    matches!(byte, b' ' | b'\t' | b'\n' | b'\r' | 0x0c | 0x0b)
}

// ---------------------------------------------------------------------------
//  便捷入口
// ---------------------------------------------------------------------------

/// 直接按"名字 → 值"渲染模板（省掉为此专门写模型结构体）。
///
/// ```
/// use template_engine::{render_values, Value};
///
/// let text = render_values(
///     "{{ mode_name }}",
///     &[("mode_name", Value::text("LALR(1)"))],
/// )
/// .unwrap();
/// assert_eq!(text, "LALR(1)");
/// ```
///
/// 模板里有 `{% include %}` 时用 [`render_values_with`] 把片段表一起传进来
/// （不传就是空表，include 输出空串）。
pub fn render_values(
    source: &str,
    values: &[(&str, Value)],
) -> Result<String, TemplateError> {
    let model = MapModel::from_pairs(values.iter().map(|(key, value)| (*key, value.clone())));
    TemplateEngine::render(source, &model, no_snippets())
}

/// 同 [`render_values`]，但带 `{% include %}` 的片段表。
///
/// ```
/// use template_engine::{render_values_with, snippet_table, Value};
///
/// let snippets = snippet_table([("v", "{{ x }}!")]);
/// let text = render_values_with("{% include \"v\" %}", &[("x", Value::Int(1))], &snippets)
///     .unwrap();
/// assert_eq!(text, "1!");
/// ```
pub fn render_values_with(
    source: &str,
    values: &[(&str, Value)],
    snippets: &SnippetTable,
) -> Result<String, TemplateError> {
    let model = MapModel::from_pairs(values.iter().map(|(key, value)| (*key, value.clone())));
    TemplateEngine::render(source, &model, snippets)
}

/// 便捷渲染：把模型投影一次再交给 [`TemplateEngine`]。
///
/// ```
/// use template_engine::{render, template_model};
///
/// template_model! {
///     struct Point { x: i64, y: i64 }
/// }
///
/// let text = render("({{ x }}, {{ y }})", &Point::new(3, 4)).unwrap();
/// assert_eq!(text, "(3, 4)");
/// ```
///
/// * `source` —— 模板字符串（引擎不读文件，模板由调用方给）；
/// * `model` —— 实现了 [`Encode`] 的模型；
///
/// 模板里有 `{% include %}` 时用 [`render_with`]。
pub fn render<T: Encode + ?Sized>(source: &str, model: &T) -> Result<String, TemplateError> {
    TemplateEngine::render(source, &EncodedModel::new(model), no_snippets())
}

/// 同 [`render`]，但带 `{% include %}` 的片段表。
pub fn render_with<T: Encode + ?Sized>(
    source: &str,
    model: &T,
    snippets: &SnippetTable,
) -> Result<String, TemplateError> {
    TemplateEngine::render(source, &EncodedModel::new(model), snippets)
}
