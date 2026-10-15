"""小工具模块：演示 todo-scan 扫多种注释写法。"""


def retry(times):
    # TODO: 支持指数退避
    # FIXME: times <= 0 时应当直接报错
    return times


def name_of(record):
    """NOTE: 老接口可能没有 name 字段。"""
    return record.get("name", "")
