# 03 文法分析：LL(1) / SLR(1) / LALR(1) / IELR(1)

本章说明分析算法；后端模式支持与增量运行时能力统一见 [12 支持矩阵](15-language-mode-support.md)。

## 1. 文件拆分

| 文件 | 内容 |
| --- | --- |
| `source/src/analysis.cpp` | 各模式共用的辅助函数（FIRST/FOLLOW、LR(0) 项集构造、LR 冲突消解、报告小工具）+ `Analysis::analyze` 分派；`k_lr0StartSymbol` / `k_slrStartSymbol` / `k_lalrStartSymbol` / `k_lr1StartSymbol` / `k_ielrStartSymbol` 定义 |
| `source/src/analysis_ll1.cpp` | LL(1)：左递归消除、FIRST/FOLLOW、SELECT、预测分析表、报告 |
| `source/src/analysis_lr0.cpp` | LR(0)：规范 LR(0) 项集 + 完整项整行归约、ACTION/GOTO、报告 |
| `source/src/analysis_slr.cpp` | SLR(1)：LR(0) 项集、FOLLOW、ACTION/GOTO、报告 |
| `source/src/analysis_lalr.cpp` | LALR(1)：LR(1) 项集 + 同心合并 lookahead、ACTION/GOTO、报告；`lr1_detail` 里的 LR(1) 构造被 IELR 复用 |
| `source/src/analysis_lr1.cpp` | LR(1)：规范 LR(1) 项集、不合并同心状态、ACTION/GOTO、报告 |
| `source/src/analysis_ielr.cpp` | IELR(1)：LALR 项集 + 冲突驱动的状态拆分、ACTION/GOTO、报告（`example/ielr_split.gram` 用两处冲突的文法验证拆分是迭代的） |
| `source/src/mermaid.cpp` | 状态机图示：把 LR 自动机与词法 DFA 画成 Mermaid `flowchart`，并拼成自包含 HTML 页面（由 `%mermaid` / `--mermaid` 独立开关控制，与教学模式无关，见 §5.4） |
| `source/include/ParseEngine/analysis.hpp` | 公开数据模型（`AnalysisResult`、`LrItem`、`Lr1Item`、`LrAction` …）与 `Analysis` 入口声明 |
| `source/include/ParseEngine/analysis_internal.hpp` | 各实现文件共用的内部声明（`namespace analysis_detail` / `lr1_detail`），非公开 API |

共享函数（定义在 `analysis.cpp`）包括：`uniqueInOrder`、`computeFirst`、
`computeFollow`、`slrProductionIds`、`augmentGrammar`、`lr0Closure` / `lr0Goto` /
`lr0BuildAutomaton` / `lr0ItemText` / `lr0StateText`、`slrActionText`、
`slrConflictText`、`computeProductionPriority`、`tryResolveShiftReduce`、
`LrRecorder` / `makeLrRecorder`、`checkUnresolvedConflicts`、
`checkUnresolvedConflictsForMode`（模式无关的冲突放行开关）、`appendTeachingSection`
（`[教学提示]` / `[教学对照]`）、`teachingConflictNote`，以及报告小工具
`orderedTokens`、`joinRhs`、`slrEntryText`、`resolutionLine`。

## 2. 理论基础（形式化）

本节给出各模式共用的形式定义，并标注每个定义在实现里的落点。记号约定：
$A,B,C,\dots\in N$ 是非终结符，$X,Y,Z\in N\cup T$ 是任意文法符号，
$a,b,c,\dots\in T$ 是终结符，$\alpha,\beta,\gamma,\dots\in(N\cup T)^{*}$
是符号串，$\varepsilon$ 是空串，$\$$ 是输入结束符（产物里的 `END`）。

### 2.1 文法、推导与语言

上下文无关文法是一个四元组 $G=(N,T,P,S)$，产生式 $A\to\omega$ 的右部
$\omega\in(N\cup T)^{*}$。一步推导 $\gamma A\delta\Rightarrow\gamma\omega\delta$
表示用某条产生式展开一个非终结符，$\Rightarrow^{*}$ 是它的自反传递闭包，
文法识别的语言是

$$L(G)=\{\,w\in T^{*}\mid S\Rightarrow^{*}w\,\}.$$

生成器运行时不做推导，而是把 $P$ 在生成期编译成分析表，运行期只查表：
这是所有模式共有的"表驱动"思路。LL(1) 用带显式栈的自顶向下表，
SLR(1) / LALR(1) / IELR(1) 用状态机加自底向上的 ACTION/GOTO 表，
### 2.2 FIRST 与 FOLLOW

$$\mathrm{FIRST}(\alpha)=\{\,a\in T\mid\alpha\Rightarrow^{*}a\beta\,\}\cup\{\,\varepsilon\mid\alpha\Rightarrow^{*}\varepsilon\,\}$$

$$\mathrm{FOLLOW}(A)=\{\,a\in T\mid S\Rightarrow^{*}\alpha Aa\beta\,\}\cup\{\,\$\mid S\Rightarrow^{*}\alpha A\,\}$$

序列的 FIRST 可以写成"扫描到第一个不可空符号为止"：

$$\mathrm{FIRST}(X_1X_2\cdots X_k)
=\Bigl(\bigcup_{i=1}^{m}\bigl(\mathrm{FIRST}(X_i)\setminus\{\varepsilon\}\bigr)\Bigr)
\cup\begin{cases}\{\varepsilon\}, & m=k\ \text{且每个 }X_i\text{ 都可空}\\ \varnothing, & \text{其它}\end{cases}$$

其中 $m=\min\{\,j\in[1,k]\mid\varepsilon\notin\mathrm{FIRST}(X_j)\,\}$，
不存在这样的 $j$ 时取 $m=k$。

FOLLOW 由下面三条规则松弛到最小不动点（$A\to\alpha B\beta\in P$）：

1. $\$\in\mathrm{FOLLOW}(S)$；
2. $\bigl(\mathrm{FIRST}(\beta)\setminus\{\varepsilon\}\bigr)\subseteq\mathrm{FOLLOW}(B)$；
3. 若 $\varepsilon\in\mathrm{FIRST}(\beta)$，则 $\mathrm{FOLLOW}(A)\subseteq\mathrm{FOLLOW}(B)$。

实现：`computeFirst` / `computeFollow`（`source/src/analysis.cpp`）就是上面公式的
工作列表式迭代，集合用 `std::set<std::string>` 保存；$\varepsilon$ 在内部
表里是一个可空哨兵，永远不会写进 ACTION/GOTO 表。

### 2.3 LL(1)：SELECT 集合与无冲突条件

$$\mathrm{SELECT}(A\to\alpha)=
\begin{cases}
\bigl(\mathrm{FIRST}(\alpha)\setminus\{\varepsilon\}\bigr)\cup\mathrm{FOLLOW}(A),
  & \varepsilon\in\mathrm{FIRST}(\alpha)\\[2pt]
\mathrm{FIRST}(\alpha), & \varepsilon\notin\mathrm{FIRST}(\alpha)
\end{cases}$$

预测分析表按 SELECT 填写：$M[A,a]$ 取满足 $a\in\mathrm{SELECT}(A\to\alpha)$
的那条产生式 $A\to\alpha$。文法 $G$ 是 LL(1) 的充要条件是每个表项至多一条
候选，等价地

$$\forall A\in N,\ \forall\,\alpha\neq\beta,\ A\to\alpha,A\to\beta\in P:\quad
\mathrm{SELECT}(A\to\alpha)\cap\mathrm{SELECT}(A\to\beta)=\varnothing .$$

满足该条件时，分析每步只需看栈顶非终结符和当前 Token，总复杂度是
$O(n)$，$n$ 为输入长度。实现里的 `selectSet` 与 `formatGrammarReport`
分别负责计算 SELECT 与输出 $M[A,\text{Token}]=P_k$。

### 2.4 左递归消除与左角图

**直接左递归**指 $A\to A\alpha$ 形式的产生式。设

$$A\to A\alpha_1\mid\cdots\mid A\alpha_m\mid\beta_1\mid\cdots\mid\beta_n
\qquad(n\ge 1,\ m\ge 1),$$

其中每个 $\beta_i$ 都不以 $A$ 开头，则等价改写为

$$A\to\beta_1 A'\mid\cdots\mid\beta_n A',\qquad
A'\to\alpha_1 A'\mid\cdots\mid\alpha_m A'\mid\varepsilon ,$$

两个文法的语言相同：

$$L(A)=\bigcup_{i=1}^{n}\beta_i\,\bigl(\alpha_1\mid\cdots\mid\alpha_m\bigr)^{*} .$$

实现里 $A'$ 命名为 `<左部>__tail`（与已有符号冲突时追加下划线），产生的
长度标记 $|\beta_i|$ 记在 `baseRhsLength`，再配合 `transform`
（`left_base` / `left_continue` / `left_empty`）让 AST 折叠把这条辅助链隐去。
若 $n=0$（只有左递归候选、没有基础候选），文法无法终止，直接报错。

**间接左递归**指 $A\Rightarrow^{+}A\alpha$ 且推导首步不是直接改写 $A$ 的情形。
判断依据是左角图 $\mathcal{L}$：

$$\mathrm{LC}(A)=\{\,B\in N\mid A\Rightarrow^{*}B\beta\,\},\qquad
\mathcal{L}=\{\,(A,B)\mid B\in\mathrm{LC}(A)\,\}.$$

于是

$$A\Rightarrow^{+}A\alpha
\iff\mathcal{L}\ \text{中存在经过 }A\ \text{的环}.$$

实现：`checkIndirectLeftRecursion` 先算可空非终结符集合，再扫描每条产生式的
右部、在符号仍可空时继续向右加边，最后用 DFS 在 $\mathcal{L}$ 上找环。
本仓库只自动消除直接左递归，命中间接左递归即报
`文法包含间接左递归：...`，把改写工作留给文法作者。

### 2.5 LR(0) / LR(1) 项集

LR(0) 项是带圆点的产生式 $[A\to\alpha\cdot\beta]$。项集 $I$ 的闭包与转移：

$$\mathrm{closure}(I)=I\cup\{\,[B\to\cdot\gamma]\mid
[A\to\alpha\cdot B\beta]\in I,\ B\to\gamma\in P\,\}\quad(\text{取最小不动点})$$

$$\mathrm{goto}(I,X)=\mathrm{closure}\bigl(\{\,[A\to\alpha X\cdot\beta]\mid
[A\to\alpha\cdot X\beta]\in I\,\}\bigr)$$

规范族从 $I_0=\mathrm{closure}(\{\,[S'\to\cdot S]\,\})$ 出发，对每个
$\mathrm{goto}(I,X)$ 做 BFS 去重得到。实现是 `slrClosure` / `slrGoto`
与 `slrBuildAutomaton`。

LR(1) 项再加一个展望符 $a\in T\cup\{\$\}$，记 $[A\to\alpha\cdot\beta,\ a]$：

$$\mathrm{closure}(I)=I\cup\{\,[B\to\cdot\gamma,\ b]\mid
[A\to\alpha\cdot B\beta,\ a]\in I,\ B\to\gamma\in P,\
b\in\mathrm{FIRST}(\beta a)\,\}$$

$$\mathrm{FIRST}(\beta a)=
\begin{cases}
\bigl(\mathrm{FIRST}(\beta)\setminus\{\varepsilon\}\bigr)\cup\{a\},
  & \varepsilon\in\mathrm{FIRST}(\beta)\\[2pt]
\mathrm{FIRST}(\beta), & \varepsilon\notin\mathrm{FIRST}(\beta)
\end{cases}$$

实现是 `lr1Closure` / `lr1Goto`，其中 `lr1FirstOfSequence` 计算
$\mathrm{FIRST}(\beta a)$。同一个 LR(0) core 的不同展望符会先按 core 分组，
每组只传播一次展望符集合，避免"展望符数 $\times$ 产生式数"的重复插入。

### 2.6 LALR(1)：同心合并

同心状态指去掉展望符后 LR(0) core 相同的 LR(1) 状态，其中

$$\mathrm{core}(I)=\{\,A\to\alpha\cdot\beta\mid
[A\to\alpha\cdot\beta,\ a]\in I\,\}.$$

合并就是把这些状态的展望符取并集：

$$[A\to\alpha\cdot\beta,\ a_1],\dots,[A\to\alpha\cdot\beta,\ a_t]
\ \longmapsto\ [A\to\alpha\cdot\beta,\ \{a_1,\dots,a_t\}].$$

$$\mathrm{core}(I)=\mathrm{core}(J)\ \Longrightarrow\
\mathrm{LA}(I\cup J)=\mathrm{LA}(I)\cup\mathrm{LA}(J).$$

合并后状态数不增，且仍保持 LR 分析所需的"同一 core 展望符一致"性质；
代价是可能把两个原本不同的 LR(1) 状态揉在一起而新引入归约/归约冲突。
实现是 `lalrMergeStates`，同时产出 LR(1) 状态号到 LALR 状态号的映射
`stateMap`，再用它重映射全部转移。

### 2.7 ACTION / GOTO 表与冲突

对每个状态 $I$：

$$\mathrm{goto}(I,a)=J\ \Longrightarrow\ \mathrm{ACTION}[I,a]=\mathrm{shift}\ J$$

$$[A\to\alpha\cdot]\in I,\ a\in\mathrm{LA}(I,A\to\alpha)
\ \Longrightarrow\ \mathrm{ACTION}[I,a]=\mathrm{reduce}\ (A\to\alpha)$$

$$[S'\to S\cdot]\ \Longrightarrow\ \mathrm{ACTION}[I,\$]=\mathrm{accept},
\qquad
\mathrm{goto}(I,B)=J\ \Longrightarrow\ \mathrm{GOTO}[I,B]=J$$

其中归约展望符 $\mathrm{LA}$ 的取法区分了各 LR 模式（IELR 沿用 LALR 的
项自带展望符，只是先按 [§2.8](#28-ielr1按需拆分同心状态) 拆分状态）：

$$\mathrm{LA}^{\mathrm{SLR}}(I,A\to\alpha)=\mathrm{FOLLOW}(A),\qquad
\mathrm{LA}^{\mathrm{LALR}}(I,A\to\alpha)=\mathrm{LA}^{\mathrm{IELR}}(I,A\to\alpha)
=\text{项自带的展望符集合}$$

（SLR 与 LALR / IELR 的表生成代码共用 `LrRecorder`，只用 `hasFollow` 开关切换
$\mathrm{LA}$ 的来源；LALR 因此能接受一部分 SLR 会冲突的文法，IELR 又能接受
全部 LALR 会冲突的 LR(1) 文法。）

同一表项出现多个动作即冲突：

$$\mathrm{ACTION}[I,a]=\{\mathrm{shift}\ J,\ \mathrm{reduce}\ (A\to\alpha)\}
\quad(\text{移进}/\text{归约})$$

$$\mathrm{ACTION}[I,a]=\{\mathrm{reduce}\ (A\to\alpha_1),\ \mathrm{reduce}\ (A\to\alpha_2)\}
\quad(\text{归约}/\text{归约})$$

优先级与结合性消解见 [§9](#9-冲突消解共享)；无法消解的冲突计数必须与
`%expect N` 完全一致，否则生成失败。

### 2.8 IELR(1)：按需拆分同心状态

LALR 的唯一缺陷来自 [§2.6](#26-lalr1同心合并) 的合并：它把 $\mathrm{core}$
相同、展望符不同的 LR(1) 状态并成一个，展望符取并集。并集只会让状态"多出"
归约动作，而移进动作完全由 $\mathrm{core}$ 决定，所以 LALR 相对规范 LR(1)
多出来的冲突一定是归约/归约冲突。

IELR(1) 把合并看成一次划分（partition）：初始按 $\mathrm{core}$ 把每个
LR(1) 状态分块，得到的正是 LALR 状态。若某个块出现未消解的归约/归约冲突，
就在触发冲突的展望符 $a$ 上，按"块内每个 LR(1) 状态在 $a$ 上启用的归约项
集合"把块拆细：

$$\mathrm{red}(I,a)=\{\,A\to\alpha\mid[A\to\alpha\cdot,\,a]\in I\,\}$$

$$\mathrm{red}(I,a)\neq\mathrm{red}(J,a)\ \Longrightarrow\
I\ \text{与}\ J\ \text{分属不同的块}.$$

拆分只会让展望符集合变小、归约动作变少，因此只消除冲突、不制造冲突。拆分后
还要做一次到 $\mathrm{goto}$ 的划分细化（partition refinement），保证"同块
状态的每个符号后继仍在同一块"，块级 $\mathrm{goto}$ 才良定义：

$$\forall I,J\ \text{同块},\ \forall X:\quad
\mathrm{goto}(I,X)\ \text{与}\ \mathrm{goto}(J,X)\ \text{同块}.$$

反复"建表 → 找归约/归约冲突 → 拆分 → goto 稳定性细化"，直到没有未消解冲突，
或划分已经细到规范 LR(1)（此时仍冲突说明文法不是 LR(1)）。因为只在冲突处
拆分，IELR 的状态数通常远小于规范 LR(1)、与 LALR 同量级；对本身就是 LALR(1)
的文法不触发任何拆分，输出与 LALR 完全一致。

### 2.9 语言类包含关系

以"接受哪些文法"衡量，三种确定性 LR 模式的能力严格递增，且
$\mathrm{IELR}(1)$ 已到达规范 $\mathrm{LR}(1)$ 的上界；LL(1) 与它们
不可直接比较，但同样弱于 $\mathrm{LR}(1)$：

$$\mathrm{LR}(0)\subsetneq\mathrm{SLR}(1)\subsetneq\mathrm{LALR}(1)
\subsetneq\mathrm{IELR}(1)=\mathrm{LR}(1),\qquad
\mathrm{LL}(1)\subsetneq\mathrm{LR}(1).$$

| 模式 | 归约展望符来源 | 项 | 状态规模 | 说明 |
| --- | --- | --- | --- | --- |
| LL(1) | SELECT | 无项集 | 表最小 | 要求最苛刻，需消除左递归 |
| LR(0) | 没有展望符：完整项在**所有**终结符上归约 | LR(0) | 最小 | 只看项集不看展望，几乎所有实用文法都有冲突；讲 LR 分析套路时最直观的对照起点（`--mode lr0`） |
| SLR(1) | $\mathrm{FOLLOW}(A)$ | LR(0) | 小 | 展望偏粗，易产生虚假冲突 |
| LALR(1) | 项自带展望符（同心并集） | LR(1) 合并 | 中 | 对非 LALR 文法会出现归约/归约冲突（确定性模式的处置见 §9：自动选一条 + 大警告） |
| LR(1) | 项自带展望符 | LR(1) | 大 | 规范 LR(1)，不合并；与 LALR 对照时的上界（`--mode lr1`，不需要任何开关） |
| IELR(1) | 拆分后块内自带的展望符 | LR(1) 合并 + 按需拆分 | 与 LALR 同量级 | 能力等同 LR(1)，本仓库支持的最强模式 |

### 2.10 公式与实现对照

| 公式 / 概念 | 实现 |
| --- | --- |
| $\mathrm{FIRST}$ / $\mathrm{FOLLOW}$ | `computeFirst` / `computeFollow` |
| $\mathrm{SELECT}$ | `selectSet` |
| 直接左递归改写 | `eliminateDirectLeftRecursion` |
| 左角图 $\mathcal{L}$ 找环 | `checkIndirectLeftRecursion` |
| LR(0) 闭包 / 转移 | `lr0Closure` / `lr0Goto`（定义在 `analysis.cpp`，SLR 与 LR(0) 共用） |
| LR(0) 项集族 | `lr0BuildAutomaton` |
| LR(1) 闭包 / 转移 | `lr1Closure` / `lr1Goto` |
| $\mathrm{FIRST}(\beta a)$ | `lr1FirstOfSequence` |
| 规范 LR(1) 状态（不合并） | `lr1ToCanonicalStates` |
| 同心状态合并 | `lalrMergeStates` |
| 块级状态重建 / 冲突拆分 / goto 细化 | `buildQuotient` / `splitBlockOnToken` / `stabilizePartition` |
| ACTION / GOTO 填表 | `slrBuildTables`（SLR）/ `lr0BuildTables`（LR(0)）/ `buildLalrStyleTables`（LALR / LR(1) / IELR 共用） |
| 教学模式的冲突提示 | `teachingConflictNote` |
| 移进/归约消解 | `computeProductionPriority` / `tryResolveShiftReduce` |

### 2.11 `error` 列与 `%sync`：错误恢复在分析层的落点

错误恢复有两层（`error` 产生式优先，`%sync` 兜底），分析层只参与第一层的一半——
把 `error` 当成一个普通终结符算进表里；第二层完全在运行时：

| 东西 | 进分析层吗 | 说明 |
| --- | --- | --- |
| `error`（伪终结符） | 进 | 加进终结符集合、参与闭包 / 转移 / ACTION 表，多出一列 `error` |
| `%sync TOKEN...` | **不进** | 只是运行时的一个锚点集合，运行时拿它去查已有的 ACTION 表 |
| `FOLLOW`（LL 家族的恢复表） | 进（本来就算了） | 生成器把 FOLLOW 渲染成产物里的 `s_ParserFollow` / `kLlFollow` / `_LL_FOLLOW`，自顶向下恢复时查它（见 §4） |

**`error` 怎么进表。** `computeFirst` 之前把 `error` 追加到终结符集合的**末尾**，
于是没写 `error` 的文法里各终结符的下标一个都不动。
之后一切照旧：

$$[A\to\alpha\cdot error\,\beta]\in I
\ \Longrightarrow\ \mathrm{ACTION}[I,error]=\mathrm{shift}\ \mathrm{goto}(I,error)$$

`error` 是终结符，闭包不会在它上面展开；$\mathrm{goto}(I,error)$ 正常计算；展望符
照常按 $\mathrm{FIRST}(\beta a)$ 传播——`error` 是终结符，$\mathrm{FIRST}$ 扫到它
就停下（$\mathrm{FIRST}(error\,\beta\,a)=\{error\}$），不会像非终结符那样把
$\beta$、$a$ 的展望符带进新项。

**FOLLOW 为什么排除 `error`。** 定义上 $\mathrm{FOLLOW}(A)$ 是「能跟在 $A$ 后面的
终结符集合」。`error` 不是输入里可能出现的符号，而是运行时**造出来**的伪 Token：
把它塞进 FOLLOW 会让 SLR 的归约展望符、LL(1) 的同步判定（§4）、`%expect` 的冲突
计数全部虚增，甚至导致「在 `error` 上归约」这种没法执行的表项。所以
`computeFollow` 扫到 `error` 直接把它当「不可空且到此为止」处理（不加入集合、
不传播）。

**`error` 列上的冲突。** `error` 不在任何 FOLLOW 里，正常文法几乎不会冲突。真冲突
时按 yacc 惯例**移进优先**，并写一条 warning（不中止生成），也**不计入**
`%expect` 的未消解冲突数——`%expect` 数的是用户文法自己的移进/归约冲突。

**`%sync` 为什么可以完全不进分析层。** panic mode 只需要两件信息：当前状态能不能
移进某个同步 Token、以及丢输入要丢到哪个 Token，两者都能在运行时查现成的
ACTION 表得到。所以 `%sync` 只是 `ProjectRequest::syncTokens` 里的一串下标，生成期
写进产物的 `Parser_IsSyncToken`（LR 还多一个 `Parser_LRCanShiftSyncToken`）。
分析层（项集、FIRST/FOLLOW、
ACTION/GOTO、冲突计数）一行都不用改。

### 2.12 LR(k) / LALR(k)：k 元组展望符与两层 ACTION 表

实现只有一处新增的构造逻辑，其余全部复用 LR(1) 的机器（`source/src/analysis_lrk.cpp`）：

**项的表示。** 项还是 `Lr1Item`，`lookahead` 字段里放的从「一个 Token」变成
「k 元组键」——k 个终结符名用 `'\x01'` 拼起来（`lookaheadKeyOf`）。k = 1 时键
就等于 Token 名本身，所以 `operator<` / `operator==` / 状态哈希 / 同心合并 /
GOTO 表全都原样能用。

**FIRST_k。** `computeFirstK` 算的是每个非终结符的 $\mathrm{FIRST}_k$：长度 ≤ k 的
终结符串集合（空串表示能推出 $\varepsilon$，长度正好 k 表示看满 k 个就截断）。
闭包里 $[A\to\alpha\cdot B\beta,\ u]$ 给 $B$ 的各产生式传播
$\mathrm{FIRST}_k(\beta u)$——$\beta$ 可空时接上 $u$ 再截断到 k。初始项是
$[S'\to\cdot S,\ \#^k]$（k 个 `END`）。

**为什么 ACTION 表要分两层。** 移进只看**下一个** Token，与展望符 k 元组无关；
归约才依赖完整的 k 元组。若把两者塞进同一张稠密表，移进那一列就得为
$(a,\ast,\dots,\ast)$ 的每一种续接各写一遍。所以表按两层建：

| 层 | 键 | 动作 |
| --- | --- | --- |
| 第一层 `akAction` / `s_ParserAction` | 单个 Token | 移进 |
| 第二层 `kReduce` / `s_ParserReduce` | 用到的 k 元组（编号成列） | 归约 / 接受 |

运行时查 `<state, 窗口>`：先按窗口的 k 元组（编码成整数后二分）查第二层，命中且
非 0 就用它；否则按窗口头查第一层的移进。这正是 Knuth LR(k) 表的标准查法，也
顺带把「按优先级消解」做得更精确：归约与移进冲突时，选归约只影响**这一个**
k 元组，同状态其它以同一 Token 开头的元组照旧移进。

**冲突与消解。** 跨层冲突（归约列的首 Token 上正好有移进）用
`tryResolveShiftReduce` 按**元组首 Token** 判优先级（`%prec` / `%left` 是单 Token
概念，k > 1 时没有更好的对应物）；消不掉按 yacc 惯例移进优先并记进
`[未消解冲突]`（`error` 列只记 warning，与 §2.11 一致）。`lrk` 与 `lr1` 一样「只报
不中止」，`lalrk` 与 `lalr` 一样默认严格、`%teach` 放行。

**LALR(k) = 合并 + LR(k) 的展望符。** 同心合并（core 相同就并 k 元组集合）直接
复用 `lalrMergeStates`——它按 core 分组、把 lookahead 集合并起来，对「lookahead
是单个 Token 还是 k 元组」没有假设。代价也一样：**合并之后 LALR(k) 可能比 LR(k)
多出归约/归约冲突，而且加长 k 也救不回来**（见 §7）。

| 量 | LR(k) | LALR(k) |
| --- | --- | --- |
| 状态 | 规范 LR(k) 项集，不合并 | 规范 LR(k) 项集按 core 合并 |
| 展望符 | k 元组 | k 元组（合并后取并集） |
| 能力 | $\mathrm{LR}(k)$ 语言类 | 严格弱：合并引入的冲突加长 k 也去不掉 |

语言类包含关系上，$\mathrm{LR}(0)\subsetneq\mathrm{SLR}(1)\subsetneq\mathrm{LALR}(1)
\subsetneq\mathrm{LR}(1)\subseteq\mathrm{LALR}(2)\subseteq\cdots\subseteq
\mathrm{LR}(k)\subseteq\cdots\subsetneq\mathrm{CFG}$（§2.9 那张图里的 LR 侧多出一层
k 的台阶）。

**k 的上限：没有策略上限。** `k_maxLookaheadK = INT_MAX`，`%lookahead N` /
`--lookahead N` / 模式名里的 `LL(7)` 都收——代价自负：$\mathrm{FIRST}_k$ /
$\mathrm{FOLLOW}_k$ 的集合大小与 k 元组列数都随 k 指数级膨胀，实际能用的 k 通常
不超过 3～4。超过 `k_lookaheadWarnThreshold = 3` 时生成期提醒一句，但不拦。

唯一一条硬限制来自**编码宽度**而不是策略：k 元组列编码是
$\sum_i \mathrm{Token号}_i\cdot(\text{Token 个数})^i$，运行时用 64 位累加，所以
$(\text{Token 个数})^k$ 必须放得进 `INT64_MAX`（`lookaheadEncodingMaxK` 算的就是
这个上限）。放不下时生成期直接报错并告诉用户本文法最多能到多少 k——比静默发出
一张会查错的表要好。

### 2.13 SLR(k) 与 LL(k)：同一套 k 的另外两条路线

$\mathrm{FIRST}_k$ / $\mathrm{FOLLOW}_k$ 这两个工具不止 LR(k) 用得上。
`source/src/analysis_slrk.cpp` 与 `source/src/analysis_llk.cpp` 把它们接到另外两条经典路线上：

**SLR(k) = LR(0) 项集 + $\mathrm{FOLLOW}_k$。** 状态机与 SLR(1) 逐个一样
（`lr0BuildAutomaton` 原样复用）——SLR 相对 LR 的简化本来就是「归约时不看项里
的上下文，只看产生式左部的 FOLLOW」。推广到 k 只换一样东西：完整项的展望符
集合从 $\mathrm{FOLLOW}(A)$ 换成 $\mathrm{FOLLOW}_k(A)$。于是：

$$[A\to\alpha\,\cdot]\in I
\ \Longrightarrow\ \mathrm{ACTION}[I,u]=\mathrm{reduce}\quad
\forall u\in\mathrm{FOLLOW}_k(A)$$

**表与产物直接复用 LR(k) 那一套**：$k$ 元组归约列、列编码二分、运行时窗口全都
一样，所以 `slrk` 的产物层是白捡的。k = 1 时 `slrk` 与 `slr` 的 ACTION/GOTO
逐行相同（实测只差虚拟起始符号名）。

**LL(k) = SELECT_k 预测分析表。** 自顶向下那条路：$\mathrm{FIRST}_k$ /
$\mathrm{FOLLOW}_k$ 都在之后，

$$\mathrm{Select}_k(A\to\alpha)=\mathrm{FIRST}_k(\alpha\cdot\mathrm{FOLLOW}_k(A))$$

（α 推出的 k 前缀接上 $\mathrm{FOLLOW}_k(A)$ 的每个 k 元组再截断到 k，
与 LR 闭包里传播展望符用的是同一个 `extendLookahead`）。表项是
$M[A,u]=\text{产生式}$；同一个 $(A,u)$ 撞上两条产生式就报「文法不是 LL(k)」。
判定对象是**消除直接左递归之后**的文法（沿用 LL(1) 那套变换），所以 `calc`
这类左递归文法在 LL(k) 下同样能生成。

**LL(0) 是这条链的起点。** k = 0 时没有展望符，$\mathrm{Select}_0(A\to\alpha)
=\{\varepsilon\}$ 对每条产生式都一样，于是「同一个 $M[A,\varepsilon]$ 只能对应
一条产生式」——也就是每个非终结符恰好一条产生式、语言只剩一条串。实现上不需要
特殊分支：`k` 允许取 0，预测分析表退化成单列，运行时列号恒为 0。

**k 的包含关系（LL 侧与 LR 侧长得一样，但两边不是一条链）：**

$$\mathrm{LL}(0)\subsetneq\mathrm{LL}(1)\subsetneq\cdots\subsetneq\mathrm{LL}(k)
\subsetneq\cdots\subsetneq\mathrm{LR}(1)$$（两条链都是 CFG 的真子集，但
$\mathrm{LL}(k)$ 与 $\mathrm{LR}(k)$ 之间没有包含关系）

| 量 | SLR(k) | LL(k) |
| --- | --- | --- |
| 状态 | LR(0) 项集（与 SLR(1) 相同） | 没有状态机，用显式栈 |
| 决策依据 | $\mathrm{FOLLOW}_k(A)$ | $\mathrm{Select}_k(A\to\alpha)$ |
| 表 | 两层 ACTION（与 LR(k) 同形） | $M[A,u]$，u 是 k 元组 |
| k = 1 | `slr` | `ll1` |

## 3. 数据模型

`AnalysisResult`（`source/include/ParseEngine/analysis.hpp`）各模式共用：

| 字段 | 含义 |
| --- | --- |
| `mode` | `"ll1"` / `"lr0"` / `"slr"` / `"lalr"` / `"lr1"` / `"ielr"` |
| `grammar` | 处理后的文法（LL 已消除左递归，LR 已加虚拟起始符） |
| `terminals` / `nonterms` | 终结符、非终结符列表（顺序稳定） |
| `first` / `follow` | FIRST / FOLLOW 集合；LALR / LR(1) 不算 FOLLOW（`hasFollow=false`），LR(0) 两样都不用 |
| `productions` | 摊平后的产生式（带全局编号 `id`，ACTION 的归约项指向它） |
| `slrStates` / `lalrStates` | LR(0) 项集族（LR(0) / SLR）/ LALR / LR(1) / IELR 状态（core → lookahead；LR(1) 下每个 core 对应规范 LR(1) 项） |
| `lr1StateCount` | 规范 LR(1) 状态数（LR(1) 的实际状态数；LALR 合并前 / IELR 拆分前的上界） |
| `transitions` | `(状态, 符号) -> 状态` 的 GOTO 转移（终结符与非终结符都有） |
| `action` / `gotoTable` | ACTION 表 / 非终结符 GOTO 表 |
| `table` | LL(1) 预测分析表 `(非终结符, 终结符) -> 产生式下标` |
| `unresolved` / `resolutions` | 未消解 / 已消解冲突记录 |
| `ielrLalrStateCount` | IELR 拆分前的 LALR 状态数（仅 IELR） |
| `ielrSplits` | IELR 状态拆分记录：原 LALR 状态号、触发 Token、冲突种类、新状态号、原因 |
| `lr1MergedStateCount` | 同心合并后的 LALR 状态数（仅 LR(1)，写进报告的 `[教学对照]`） |
| `teach` | 是否教学模式（`%teach` / `--teach`）；为真才输出 `[教学提示]` / `[教学对照]`，并让 `slr` / `lalr` / `ielr` 容忍未消解冲突。与 `mermaid/` 状态机图示无关（那是 `ProjectRequest::mermaid`） |
| `stateCount()` | LR 模式的状态总数（LR(0) / SLR 数 `slrStates`，其余数 `lalrStates`） |

## 4. LL(1)（`analysis_ll1.cpp`）

入口是 `Analysis::prepareGrammar(grammar, tokens)`（LL 没有单独的 `ll1Analyze`
名字，语义等价于另外两个模式的方法）：

1. **消除直接左递归**：按
   [§2.4](#24-左递归消除与左角图) 的公式把
   $A\to A\alpha\mid\beta$ 改写成 $A\to\beta A'$、$A'\to\alpha A'\mid\varepsilon$
   （$A'$ 即 `<左部>__tail`）；若某条左递归规则没有基础候选（$n=0$）则报错。
2. **检测间接左递归**：在左角图 $\mathcal{L}$ 上找环，命中直接报
   `文法包含间接左递归：...`。
3. **FIRST / FOLLOW**：`computeFirst` / `computeFollow`（共享）。
4. **SELECT 集合**：`selectSet` 计算每条产生式的 SELECT；右部可空时并入 FOLLOW。
5. **预测分析表**：同一 `(非终结符, 终结符)` 出现两条产生式即报
   `文法不是 LL(1)：M[X, token] ...`。
6. **报告**：`formatGrammarReport` 输出最终产生式、FIRST、FOLLOW、
   `M[非终结符, Token] = Pk`。

终结符集合 = `k_defaultTokenNames` + 文法声明的 Token + `"END"`，顺序去重。

分析层算出的 FOLLOW 不止进报告：生成器会把它渲染成产物的 `s_ParserFollow`
（C）/ `kLlFollow`（C++17）/ `_LL_FOLLOW`（Python），供运行时做 **FOLLOW 集错误
恢复**——非可空非终结符的 FOLLOW 不在预测表里，恢复时「当前 Token 在栈顶这一项
的 FOLLOW 里 → 放弃这一层」全靠它（见
[`05-grammar.md` §3.2](05-grammar.md)）。

## 5. LR(0) / LR(1) 规范自动机模式（`analysis_lr0.cpp` / `analysis_lr1.cpp`）

`--mode lr0` / `--mode lr1`（`.gram` 里写 `%mode lr0` / `%mode lr1`）把 §2.5 的
两套项集族原样算出来——LR(0) 不查 FOLLOW、LR(1) 不做同心合并——再填
ACTION / GOTO。这两个模式和其余模式完全平权：共用词法、前端、产物模板、
LR 运行时与语义动作翻译，**不需要任何开关**就能生成代码、编译、运行。唯一的
区别是它们从不因未消解冲突中止生成（见 §5.3），以及报告里可能多出教学小节。

LR 家族（`lr0` / `slr` / `slrk` / `lalr` / `lalrk` / `lr1` / `lrk` / `ielr`）都会在
`%mermaid` / `--mermaid` 打开时产出状态机图（§5.4），这个开关与教学模式无关。

**教学模式（`%teach` / `--teach`）是另一个独立开关，只管报告与
`slr` / `lalr` / `ielr` 的冲突严格程度**（命令行优先级高于 `%teach`）：

| 项目 | 缺省（不开教学模式） | 教学模式（`%teach` / `--teach`） |
| --- | --- | --- |
| 适用模式 | 全部模式 | 报告小节对 `lr0` / `slr` / `slrk` / `lalr` / `lalrk` / `lr1` / `lrk` / `ielr` 八个确定性 LR 模式生效；LL 家族无冲突可讲、`lex` 没有分析层，都会被忽略并提示一句 |
| `lr0` / `lr1` 的未消解冲突 | 只报不中止（这两个模式从来如此） | 同样只报不中止，另加 `[教学提示]` 说明为什么不是 LR(0) / LR(1) 文法 |
| `slr` / `lalr` / `ielr` 的未消解冲突 | 直接报错、不产出代码 | 只报不中止：写进 `[未消解冲突]`，表项按移进优先继续生成 |
| 报告 | 与其它 LR 模式一致 | 多出 `[教学提示]`（`teachingConflictNote`）与 `[教学对照]`（状态数、冲突数） |
| 产物 | 与其它模式同构 | 与其它模式同构（图示由 `%mermaid` 单独控制） |
| `%expect N` | 按 `%expect` 校验（两种情形都照常生效） | 同左 |

也就是说：教学模式既不改变 `lr0` / `lr1` 的可用性，也不决定要不要出图。

### 5.1 LR(0)（`analysis_lr0.cpp`）

1. `augmentGrammar(grammar, "__lr0_start")` 加虚拟起始符，算 FIRST（只为
   报告，LR(0) 的闭包与填表都不用它），不算 FOLLOW。
2. `lr0BuildAutomaton`（定义在 `analysis.cpp`，SLR 与 LR(0) 共用同一份
   `lr0Closure` / `lr0Goto`）做 BFS，得到 LR(0) 项集族与 `transitions`。
3. `lr0BuildTables` 与 SLR 的 `slrBuildTables` 只差一处：圆点在末尾的完整项
   **在 `terminals` 的每一个 Token 上**写归约，不查 FOLLOW：

   $$\text{ACTION}[I,\ a]=\text{reduce }(A\to\alpha)\quad
   \forall\,[A\to\alpha\,\cdot\,]\in I,\ \forall a\in T$$

   于是只要某个状态里既有可移进的项、又有完整项，移进/归约冲突必然出现——
   这正是 $\mathrm{LR}(0)\subsetneq\mathrm{SLR}(1)$ 的直观解释。
4. 报告 `formatLr0Report`：`[LR(0) 自动机]`、`[ACTION 表]`、`[GOTO 表]`、
   `[冲突消解]`、`[未消解冲突]`（外加 `[教学提示]`）与 `[教学对照]`
   （`LR(0) 状态数` / `LR(0) 表未消解冲突数`）。LR(0) 既不用 FIRST 也不用
   FOLLOW，报告里刻意不印这两节。

### 5.2 LR(1)（`analysis_lr1.cpp`）

1. `augmentGrammar(grammar, "__lr1_start")`，算 FIRST（LR(1) 闭包要用它算
   $\mathrm{FIRST}(\beta a)$）。
2. `lr1_detail::lr1BuildAutomaton` 建规范 LR(1) 项集族（与 LALR / IELR 同一
   份代码），`lr1StateCount` 记下状态数。
3. `lr1_detail::lr1ToCanonicalStates` 把每个状态原样转成 core → lookahead
   形式（**不合并**），状态编号与规范 LR(1) 一一对应；转移表直接沿用。
4. `buildLalrStyleTables(..., "LR(1)", ...)`：圆点在末尾就在项自带的展望符上
   写归约，与 LALR 用的是同一个函数——差别只在传入的状态有没有合并过。
5. 顺手对同一份项集族跑一次 `lalrMergeStates`，把合并后的状态数记进
   `lr1MergedStateCount`，写进报告的 `[教学对照]`：

   ```
   [教学对照]
   规范 LR(1) 状态数 = 26
   LALR 同心合并后状态数 = 24
   ```

   差就是同心合并省下来的状态数。`example/ielr_split.gram` 用 `--mode lr1`
   是 26 → 24：合并省下 2 个状态，代价是把 `x -> c .` 与 `y -> c .` 的展望符
   并到一起，于是 LALR 报归约/归约冲突、LR(1) 不报。

`--dump-lr-automaton` 下 LR(1) 按教材写法一项一行打印
（`[A -> α . β, a]`），LR(0) 与 SLR 一样打印 LR(0) 项集族。

### 5.3 冲突处理（三种放行程度）

冲突处理的入口是 `checkUnresolvedConflictsForMode(label, result, allowConflicts)`
——模式自己决定 `allowConflicts`，所以「谁严格、谁放行」写在模式文件里，不靠
`%teach` 反查：

| 模式 | `allowConflicts` | 未消解冲突的后果 |
| --- | --- | --- |
| `lr0` / `lr1` | 恒为真 | 只报不中止，任何模式下都能用：冲突写进报告 `[未消解冲突]`，表项按 yacc 默认写，stderr 打一条警告 |
| `slr` / `lalr` / `ielr` | `result.teach` | 缺省严格——走 §9 的 `checkUnresolvedConflicts`，直接报错、不产出代码；开了 `%teach` / `--teach` 才放行 |

放行时（`lr0` / `lr1` 恒放行，`slr` / `lalr` / `ielr` 在教学模式下）：

- 写进表的动作按 yacc 默认：移进/归约取移进，归约/归约取先到的归约
  （`LrRecorder::record` 里本来就是这个默认，只是严格模式随后会报错）；
- 命令行往 stderr 打一条「已按移进优先写表继续生成」的警告，并在括号里指向
  「教学提示」（教学模式）或「未消解冲突」（`lr0` / `lr1`）小节——不写报告也
  不会漏掉；
- 教学模式下报告里多一节 `[教学提示]`（`teachingConflictNote`）：该文法不是
  LR(0) / LR(1) 文法、冲突按种类计数（移进/归约多少处、归约/归约多少处）、
  生成器接下来怎么处理；
- 只有显式写了 `%expect N` 时才调用 `checkUnresolvedConflicts` 校验数量。

二义表达式文法既能直接 `--mode lr0` 看到完整自动机与冲突本身（冲突就是教学
素材），也能 `--mode slr --teach` 看「为什么不是 SLR(1)」。真 LR(0) 的文法长
什么样，见 `example/lr0.gram`（5 个状态、完整项整行
归约、零冲突，文法里自带 `%mode lr0` / `%teach` / `%mermaid`）。

这两个模式要点：`example/lr0.gram` 无冲突且能解析；二义文法在 `lr0` 下
**不开任何开关**也照常生成并报出冲突、加 `--teach` 后报告里多出教学小节、
`--no-teach` 又能把文法里的 `%teach` 压回去（而 `slr` 在同一条文法上仍然严格）；
`ielr_split` 在 `lr1` 下生成并解析通过且规范状态数严格多于合并后；没有同心合并
可合并的文法（`ini` / `or_branch` / `lr0` / `lex_demo`）在 `lr1` 与 `lalr` 下
得到同一张分析表。

### 5.4 状态机图示（`source/src/mermaid.cpp`）

`%mermaid` / `--mermaid` 打开后把生成期已经算出来的状态机画成图
（`Mermaid::projectFiles` 产出）。这个开关与教学模式完全独立：不开 `%teach`
一样出图，开了 `%teach` 也不会自动出图；产物清单里有没有 `mermaid/` 只由
`ProjectRequest::mermaid` 决定。

| 图 | 数据来源 | 画法 |
| --- | --- | --- |
| 文法自动机 | `slrStates`（`lr0` / `slr`）或 `lalrStates`（`lalr` / `lr1` / `ielr`）+ `transitions` + `action` + `unresolved` | 状态 = LR 项集（`lalrStates` 按教材写法把展望符逐项列出），边 = GOTO 转移（终结符标注为移进、非终结符标 `GOTO`）；状态按「冲突 / 接受 / 归约 / 移进」用 `classDef` 上色，框内最后一行是 ACTION 表里的动作摘要；`isLrMode` 为假的 `ll1` 不出这张图 |
| 词法 DFA | `RegexDfa::compileAll`（与 `CodeGenC99::lexDfa` 同一份编译） | 每个 `%token regex` / `%identifier` 一张：状态 = DFA 状态（双圆是接受状态），边 = 共享字符类的可读文本（连续字符压成 `a-z`，超过 16 个字符按区段截断），陷阱状态与指向它的边不画 |

几个实现要点：

- 生成的是 Mermaid `flowchart LR`。标签里的 `[` `]` `(` `)` `{` `}` `"` `|` `#`
  用 Mermaid 自己的 `#nn;` 转义（`Mermaid::escapeLabel`），换行用 `<br/>`；
  这套组合是拿 Mermaid 官方解析器逐图验证过的——`flowchart` 的引号标签比
  `stateDiagram-v2` 更经得起怪字符（比如 `:` 这种 Token 名）。
- `Mermaid::htmlPage` 把源码直接放进 `<div class="mermaid">`（Mermaid 读的是
  节点的 `innerHTML`，所以 `<br/>` 与 `#91;` 都不会被 HTML 先解掉），另有一份
  HTML 转义过的源码放在 `<details><pre>` 里，断网时也能读、能复制。
  页面从 CDN 加载 `mermaid@11`，并把 Mermaid 自己的 `maxEdges` / `maxTextSize`
  上限调高。
- **尺寸闸门**：超过 150 状态 / 400 条边 / 120 KB 的图只画一句说明，指回分析
  报告与 `--dump-lr-automaton`。真实文法的自动机确实会大到没法学——
  `example/c89_subset.gram` 的 LALR 是 340 状态 / 3117 转移，规范 LR(1) 是
  1691 状态 / 16586 转移，画出来浏览器也打不开。
- 产物只有 `mermaid/index.html` / `grammar.html` / `lexer.html` 与
  `grammar.mmd` / `lexer.mmd`（没有正则 Token 时不生成 `lexer.*`），
  `parser.c` 等其余产物不受影响；两张图都没有（`ll1` 且没有正则 Token）时连
  `mermaid/` 目录都不建。
- 命令行 `--dump-mermaid` 打印同样的源码（Markdown 围栏格式），任何模式下都能
  用；没有可画的图时向 stderr 提示一句、标准输出为空。

## 6. SLR(1)（`analysis_slr.cpp`）

按 [§2.5](#25-lr0--lr1-项集) 建 LR(0) 项集族，再按
[§2.7](#27-action--goto-表与冲突) 填表。`Analysis::slrAnalyze`：

1. `augmentGrammar(grammar, "__slr_start")` 加虚拟起始符，得到新的起始产生式。
2. 算 FIRST / FOLLOW（FOLLOW 用于归约时决定 lookahead）。
3. `lr0BuildAutomaton`：从初始项集 `{__slr_start -> . S}` 出发做
   `lr0Closure` / `lr0Goto` 的 BFS（与 §5.1 的 LR(0) 模式共用同一份代码），
   得到 LR(0) 项集族与 `transitions`。
4. `slrBuildTables`：对每个项集
   - 圆点后是终结符 → 查 `transitions` 写移进；
   - 圆点在末尾且左部是起始符 → 写接受；
   - 圆点在末尾 → 对 `FOLLOW(左部)` 每个 Token 写归约；
   - 非终结符转移写 `gotoTable`。
5. `checkUnresolvedConflicts("SLR(1)", ...)` 按 `%expect` 处理未消解冲突。

## 7. LALR(1)（`analysis_lalr.cpp`）

按 [§2.5](#25-lr0--lr1-项集) 建 LR(1) 项集族、按
[§2.6](#26-lalr1同心合并) 合并同心状态，再按
[§2.7](#27-action--goto-表与冲突) 填表。`Analysis::lalrAnalyze`：

1. `augmentGrammar(grammar, "__lalr_start")`。
2. `lr1BuildAutomaton`：LR(1) 闭包/转移，项是 `Lr1Item{lhs, prod, dot,
   lookahead}`；用 `lr1ItemHash` / `lr1StateHash` 把项集塞进 `unordered_map`
   做去重，比逐个 `std::set` 比较快。
3. `lalrMergeStates`：以 LR(0) core 为键合并同心状态，lookahead 取并集；
   同时输出 `stateMap`（LR(1) 状态号 → LALR 状态号）。
4. 用 `stateMap` 重映射 `lr1Transitions`，得到 LALR 的 `transitions`。
5. `lalrBuildTables`：与 SLR 类似，但归约的 lookahead 用项自带的集合
   （不是 FOLLOW），这就是 LALR 比 SLR 强的点；写 `hasFollow = false`。
6. `checkUnresolvedConflicts("LALR", ...)`。

`lr1StateCount` 记录合并前状态数，报告里会打印
`LR(1) 状态数 = N，合并后 LALR 状态数 = M`。

## 8. IELR(1)（`analysis_ielr.cpp`）

IELR 完全复用 LALR 的 LR(1) 构造，只在冲突处拆分。`Analysis::ielrAnalyze`：

1. `prepareLrAnalysis(grammar, tokens, "__ielr_start", "ielr", ...)`：加虚拟起始符、
   算 FIRST、摊平 Token（与 SLR / LALR 同一套共享准备）。
2. `lr1BuildAutomaton` 建规范 LR(1) 项集族与转移（与 LALR 同一份代码，定义在
   `analysis_lalr.cpp` 的 `lr1_detail` 命名空间）。
3. `lalrMergeStates` 得到初始划分（LR(1) 状态 → 块），块级 `states` / `transitions`
   即 LALR 状态；`ielrLalrStateCount` 记录拆分前的状态数。
4. `buildLalrStyleTables` 建一次表；只要 `unresolved` 里还有归约/归约冲突：
   - `splitBlockOnToken` 按触发 Token 上各子状态的归约项集合拆块；
   - `stabilizePartition` 把划分细化到 goto 稳定；
   - `buildQuotient` 重建块级状态与转移，再建表。循环直到无冲突或划分已细到
     规范 LR(1)。
5. 汇总拆分记录到 `AnalysisResult::ielrSplits`：拆分前的 LALR 状态号、触发 Token、
   冲突种类、拆分后的状态号，以及"哪个 Token 上哪两个归约项在打架"的原因文本。
6. `checkUnresolvedConflicts("IELR(1)", ...)` 做最终校验；此时若仍冲突，说明文法
   连 LR(1) 都不是，按既有规则报错（`%expect` 语义与其他模式一致）。

表格渲染直接复用 `formatSlrTables`，因此 IELR 的 ACTION/GOTO 输出格式与
SLR / LALR 完全相同。报告由 `formatIelrReport` 输出，比 LALR 多两节：
`[IELR 自动机]`（LALR 状态数 / 拆分后 IELR 状态数 / 规范 LR(1) 状态数 /
ACTION 表项数 / GOTO 转移数）与 `[IELR 状态拆分]`；`formatIelrAutomaton` 服务
`--dump-lr-automaton`。`RESULT.md` 里由 `generatorResultMd`（`source/src/project.cpp`）
渲染同样的「IELR(1) 状态拆分」小节。

对纯 LALR(1) 文法（`calc` / `pl0` / `c89_subset`）可以自己核对 IELR 与 LALR 的
`parser_table.c` 是否等价——两者只差模式名与虚拟起始符名，ACTION/GOTO 数据完全
相同。

## 9. 冲突消解（共享）

优先级来源：

- 移进动作的优先级：触发冲突的 Token 在 `%left` / `%right` / `%nonassoc`
  里的声明，行越靠下越高，同一行从左到右递增。
- 归约动作的优先级：产生式末尾的 `%prec TOKEN`，没写就取右部最右终结符；
  `computeProductionPriority` 负责这项计算。

`tryResolveShiftReduce` 的比较规则：

1. 双方都有优先级：高者胜；相等时看结合性——`%left` 归约、`%right` 移进、
   `%nonassoc` 报错。
2. 只有一方有优先级：有优先级的一方胜。
3. 都没有：返回空，`LrRecorder::record` 按“默认移进优先”处理，并记入
   `unresolved`。

`checkUnresolvedConflicts`：

- 归约/归约冲突**不进这里**：确定性 LR 模式会在建表时就地决策（见下），
  只有 IELR 的探测阶段才会看见它们；
- 没写 `%expect` 时任何未消解冲突都报错；
- 写了 `%expect N` 时，实际未消解数必须正好等于 N，否则报错；
- 数量一致时打印警告并继续生成（按移进优先写表）。

**归约/归约冲突：自动选一条 + 大警告。** 归约/归约冲突不受优先级之外的任何东西
约束，但让它直接崩掉生成并不是唯一选择——bison 的做法就是挑一条继续。本生成器
从 v0.8 起对**确定性 LR 模式**（`lr0` / `slr` / `lalr` / `lr1` / `ielr` / `lrk` /
`lalrk` / `slrk`）采用同一套规则：

1. 先看两条产生式的优先级（`%prec` 或右部最右终结符）——**不一样就优先级高的胜出**，
   这就是「用户推荐的产生式」；
2. 优先级一样（或都没写）就取**文法里更早出现的产生式**（产生式编号更小的那个，
   yacc 的惯例）；
3. 选中的那条写进 ACTION 表，被放弃的记进 `[冲突消解]`；
4. 生成期打一条**醒目的大警告**（`AnalysisResult::warnings`，汇总成一条横幅，
   最多列 8 条、其余汇总——LR(0) 这类冲突多的模式不至于刷屏），把「选了谁、
   弃了谁、依据是什么」都说清楚。

于是「LALR 有归约/归约冲突」不再是致命错误，而是**带警告的自动决策**；想自己拍板
就给两条产生式加 `%prec`。两个例外：

- **IELR 的探测阶段**仍然要看见归约/归约冲突（它靠这个决定拆哪个状态），所以
  `buildLalrStyleTables` 多了一个 `resolveReduceReduce` 开关：探测时关、
  拆分结束后的最后一遍开。拆分到头还是冲突（也就是规范 LR(1) 都解不掉的二义）
  才走「选一条 + 大警告」。
状态项集文本由 `lr0StateText` / `lalrStateText` 渲染，冲突说明里会带上，
方便定位是哪个状态、哪两个项在打架。

放行冲突的模式不走这里：`lr0` / `lr1` 恒放行，`slr` / `lalr` / `ielr` 只在
教学模式（`%teach` / `--teach`）下放行。它们只在写了 `%expect` 时才调用
`checkUnresolvedConflicts`，否则冲突只写进报告（教学模式下再用
`teachingConflictNote` 生成 `[教学提示]`）。`checkUnresolvedConflictsForMode`
是那个分叉点，见 §5.3。

## 10. 报告与 dump

| 函数 | 位置 | 用途 |
| --- | --- | --- |
| `formatGrammarReport` | `analysis_ll1.cpp` | LL(1) 分析报告（写进 `RESULT.md` / `--report`） |
| `formatLr0Report` | `analysis_lr0.cpp` | LR(0) 报告（教学模式含 `[教学对照]` 与 `[教学提示]`） |
| `formatSlrReport` | `analysis_slr.cpp` | SLR 报告 |
| `formatLalrReport` | `analysis_lalr.cpp` | LALR 报告 |
| `formatLr1Report` | `analysis_lr1.cpp` | LR(1) 报告（含规范 LR(1) / LALR 合并后的状态数对照） |
| `formatIelrReport` | `analysis_ielr.cpp` | IELR 报告（含 `[IELR 自动机]` / `[IELR 状态拆分]`） |
| `formatLr0Automaton` / `formatSlrAutomaton` | `analysis_lr0.cpp` / `analysis_slr.cpp` | `--dump-lr-automaton`（两者都是 LR(0) 项集族，同一份渲染） |
| `formatLr1Automaton` | `analysis_lr1.cpp` | `--dump-lr-automaton`（一项一行 `[A -> α . β, a]`） |
| `formatLalrAutomaton` / `formatIelrAutomaton` | 对应模式文件 | `--dump-lr-automaton` |
| `formatLr0Tables` / `formatSlrTables` / `formatLr1Tables` / `formatLalrTables` / `formatIelrTables` | 对应模式文件 | `--dump-slr-tables`（ACTION / GOTO 矩阵共用同一份渲染） |
| `[教学对照]` / `[教学提示]` 小节 | `analysis.cpp`（`appendTeachingSection`） | 教学模式特有：状态数与冲突数、LR(1) 与 LALR 的状态数对照、冲突处理说明；五个确定性 LR 模式共用 |
| `Mermaid::grammarDiagrams` / `lexerDiagrams` | `mermaid.cpp` | 状态机图示：文法自动机与词法 DFA 的 `flowchart` 源码（§5.4） |
| `Mermaid::htmlPage` / `projectFiles` | `mermaid.cpp` | 状态机图示的 HTML 页面与 `mermaid/*` 产物清单（`enabled=false` 时返回空） |
| `--dump-mermaid` | `source/src/main.cpp` | 打印状态机图示源码（Markdown 围栏格式），任何模式下都能用 |
| `resolutionLine` | `analysis.cpp` | 把一条已消解冲突渲染成一行 |
| `formatIelrReport` 里的拆分小节 | `source/src/project.cpp` | 额外写进 `RESULT.md` 的「IELR(1) 状态拆分」 |

## 11. 想加一种新分析模式

1. 在 `source/include/ParseEngine/analysis.hpp` 给 `Analysis` 加一个静态方法。
2. 新建 `source/src/analysis_<mode>.cpp`，用 `using namespace analysis_detail;`
   复用共享函数，把私有辅助放进该文件的匿名命名空间。
3. 在 `source/src/analysis.cpp` 的 `Analysis::analyze` 里加分支；需要新模式符号就
   照 `k_slrStartSymbol` 的样子定义。
4. 在 `source/include/ParseEngine/project.hpp` / `source/src/project.cpp` 里接上产物差异，
   在 `source/src/main.cpp` 的 `--mode` 校验里放行新值。
5. `source/templates/` 里按需加模板，并重跑 `tools/embed_templates.py`。
6. 想让新模式的自动机也能画成状态机图示，在 `source/src/mermaid.cpp` 的
   `grammarDiagrams` / `usesLalrStates` 里接上它的项集族即可（§5.4）。
