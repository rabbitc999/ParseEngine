// 一个假装是前端的小文件，用来试 todo-scan
function render(items) {
    // TODO: 列表超过 200 条时改成虚拟滚动
    return items.map(function (item) {
        return item.name;   // FIXME: 空名字会渲染成 undefined
    });
}

// NOTE: 这里的顺序依赖后端返回的字段顺序
export default render;
