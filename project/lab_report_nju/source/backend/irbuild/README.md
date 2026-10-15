# 四元式 / 三元式构建库（`irbuild`）

用链式调用**直接造 IR**，不必拼文本再解析；产出的东西可以直接喂给库里其它模块。

> 该模块已在合并库 [`library/transfer`](../README.md) 内，与其它模块共用
> **一套带位数信息的三地址 IR**（`ir/typed_ir.hpp`）。

```cpp
#include "irbuild.hpp"

t1t::QuadBuilder q;                                  // 四元式构建器
q.declare("i", "i16").declare("s", "i16")            // (decl, i16, global, i)
 .assign("0", "i")                                    // (:=, 0, _, i)
 .label("loop")
 .add("i", "1", "i")                                  // (+, i, 1, i)
 .add("s", "i", "s")
 .sub("5", "i", "t")
 .jumpIfZero("t", "done")                             // (jz, t, _, done)
 .jump("loop")
 .label("done")
 .write("s").write("i").halt();

q.text();          // 四元式文本 → transfer::read / 直接跑模拟器
q.lower();         // 统一 IR（TT3，带位数）：decl i, i16 … add:i16 i, i, 1

t1t::TripleBuilder t;                                // 三元式构建器（结果用下标引用）
t.assign("0", "i");                                  // (:=, 0, i)
t.label("loop");
const std::size_t inc = t.arithmetic("+", "i", "1"); // (+, i, 1) → 返回下标
t.assign(t.ref(inc), "i");                           // (:=, (k), i)
t.toQuads();       // 三元式 → 四元式
t.toTT3();         // 三元式 → 四元式 → TT3
```

## 字段惯例（与 `irbridge` 的 fixture 一致）

| | 算术 | 赋值 | 标签 / 跳转 | 结果引用 |
| --- | --- | --- | --- | --- |
| 四元式 | `(op, a, b, r)` | `(:=, 值, _, 目标)` | `(label, _, _, L)`、`(jz, 值, _, L)` | 名字 `t1` |
| 三元式 | `(op, a, b)` | `(:=, 值, 目标)` | `(label, L, _)`、`(jz, 值, L)` | 下标 `(k)` |

四元式构建器的二元运算可带类型后缀：`q.mul("a", "b", "t", "u16")` → `(mul:u16, a, b, t)`，
降级到 TT3 时位数信息会保留；其它指令族（read / cvt / 字符串 / 向量 / 调用 / 中断）
用 `q.raw(op, a, b, r)` 原样加。

## 命令行与自检

```text
t1t irbuild --kind=quad       # 四元式构建器：打印原 IR → TT3 → 运行结果
t1t irbuild --kind=triple     # 三元式构建器：同上（多一步三元式 → 四元式）
make -C library/transfer check      # 含 irbuild-tests
```

`irbuild-tests` 检查：文本格式稳定（写出 → 读回 → 再写出逐字相同）、两种构建器造出的
程序模拟结果都是 `15 5`、TT3 里带 `decl` 与 `add:i16`、以及引用越界时转四元式必须报错。
