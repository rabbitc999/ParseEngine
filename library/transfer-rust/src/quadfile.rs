// .quad 示例文件读取（由 lib.rs 用 include! 拼进同一模块）。

use std::path::Path;

/// 一个 `.quad` 示例：程序 + 注释里的元信息。
#[derive(Clone, Debug, Default)]
pub struct QuadFile {
    /// 这个 `.quad` 文件自己的路径（`# 配置:` 的相对路径按它解析）。
    pub path: String,
    pub name: String,
    pub input: Vec<String>,
    pub expect: String,
    pub expect_text: String,
    /// 可选的机器配置文件（`# 配置: example/arm32.ini`）。
    pub config_path: String,
    /// 可选的数据区字节镜像（`# 数据: example/21_data_area.data.bin`，`addr` / `load` /
    /// `store` 三件套用）。
    pub data_path: String,
    /// 可选的数据区符号布局表（`# 布局: example/21_data_area.layout`）；留空时按
    /// `<数据>.layout` 找。
    pub layout_path: String,
    /// `# 期望错误:` 的负例（C++ 版检查；Rust 版跳过）。
    pub expect_error: String,
    pub program: Program,
}

/// 读取 `.quad` 文件：`# 名称:` / `# 输入:` / `# 期望:` / `# 期望文本:` 注释
/// 由本函数解析，正文交给 `read_quads`。
pub fn load_quad_file(path: &str) -> Result<QuadFile, String> {
    let text = std::fs::read_to_string(path)
        .map_err(|error| format!("无法打开四元式文件 {path}：{error}"))?;
    let mut source =
        QuadFile { path: path.to_string(), name: path.to_string(), ..Default::default() };
    for line in text.lines() {
        let trimmed = line.trim();
        if let Some(comment) = trimmed.strip_prefix('#') {
            if let Some((key, value)) = comment.split_once(':') {
                let key = key.trim();
                let value = value.trim().to_string();
                match key {
                    "名称" => source.name = value,
                    "期望" => source.expect = value,
                    "期望文本" => source.expect_text = value,
                    "配置" => source.config_path = value,
                    "数据" => source.data_path = value,
                    "布局" => source.layout_path = value,
                    "期望错误" => source.expect_error = value,
                    "输入" => {
                        source.input = value.split_whitespace().map(str::to_string).collect()
                    }
                    _ => {}
                }
            }
        }
    }
    source.program = read_quads(&text)?;
    Ok(source)
}

/// 解析 `# 配置:` 的相对路径：先按当前目录找，再相对 `.quad` 文件目录找，
/// 最后试同一目录下只用文件名的形式（兼容 `example/arm32.ini` 这类历史写法）。
pub fn resolve_config_path(quad_file: &str, config_path: &str) -> String {
    if config_path.is_empty() || Path::new(config_path).is_file() {
        return config_path.to_string();
    }
    let parent = Path::new(quad_file).parent().unwrap_or_else(|| Path::new(""));
    let file_name = Path::new(config_path).file_name().unwrap_or_default();
    for candidate in [parent.join(config_path), parent.join(file_name)] {
        if candidate.is_file() {
            return candidate.to_string_lossy().into_owned();
        }
    }
    config_path.to_string()
}

/// 数值输出渲染成空格分隔的文本（便于和 `# 期望:` 比对）。
pub fn render_output(execution: &Execution) -> String {
    execution.output.iter().map(display_value).collect::<Vec<_>>().join(" ")
}
