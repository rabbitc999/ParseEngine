# 03 Grammar Analysis: LL(1) / SLR(1) / LALR(1) / IELR(1)

> This is the English edition. The Chinese edition is the default and is the one maintained first — see [中文文档](../README.md)。

This chapter explains the analysis algorithms; backend mode support and incremental runtime capabilities are covered uniformly in [12 Support Matrix](../15-language-mode-support.md).

## 1. File layout

| File | Contents |
| --- | --- |
| `source/src/analysis.cpp` | Helper functions shared by all modes (FIRST/FOLLOW, LR(0) item-set construction, LR conflict resolution, small report helpers) + the `Analysis::analyze` dispatch; definitions of `k_lr0StartSymbol` / `k_slrStartSymbol` / `k_lalrStartSymbol` / `k_lr1StartSymbol` / `k_ielrStartSymbol` |
| `source/src/analysis_ll1.cpp` | LL(1): left-recursion elimination, FIRST/FOLLOW, SELECT, predictive parsing table, report |
| `source/src/analysis_lr0.cpp` | LR(0): canonical LR(0) item sets + whole-row reduce for complete items, ACTION/GOTO, report |
| `source/src/analysis_slr.cpp` | SLR(1): LR(0) item sets, FOLLOW, ACTION/GOTO, report |
| `source/src/analysis_lalr.cpp` | LALR(1): LR(1) item sets + merging lookaheads of states with the same core, ACTION/GOTO, report; the LR(1) construction in `lr1_detail` is reused by IELR |
| `source/src/analysis_lr1.cpp` | LR(1): canonical LR(1) item sets, no merging of states with the same core, ACTION/GOTO, report |
| `source/src/analysis_ielr.cpp` | IELR(1): LALR item sets + conflict-driven state splitting, ACTION/GOTO, report (`example/ielr_split.gram` uses a grammar with conflicts in two places to verify that splitting is iterative) |
| `source/src/mermaid.cpp` | State-machine diagrams: draw the LR automaton and the lexical DFA as Mermaid `flowchart`, and assemble them into a self-contained HTML page (controlled by the independent `%mermaid` / `--mermaid` switch, unrelated to teaching mode, see §5.4) |
| `source/include/ParseEngine/analysis.hpp` | Public data model (`AnalysisResult`, `LrItem`, `Lr1Item`, `LrAction` …) and the `Analysis` entry-point declarations |
| `source/include/ParseEngine/analysis_internal.hpp` | Internal declarations shared by the implementation files (`namespace analysis_detail` / `lr1_detail`), not a public API |

Shared functions (defined in `analysis.cpp`) include: `uniqueInOrder`, `computeFirst`,
`computeFollow`, `slrProductionIds`, `augmentGrammar`, `lr0Closure` / `lr0Goto` /
`lr0BuildAutomaton` / `lr0ItemText` / `lr0StateText`, `slrActionText`,
`slrConflictText`, `computeProductionPriority`, `tryResolveShiftReduce`,
`LrRecorder` / `makeLrRecorder`, `checkUnresolvedConflicts`,
`checkUnresolvedConflictsForMode` (the mode-agnostic switch for letting conflicts through), `appendTeachingSection`
(`[教学提示]` / `[教学对照]`), `teachingConflictNote`, as well as the report helpers
`orderedTokens`, `joinRhs`, `slrEntryText`, `resolutionLine`.

## 2. Theoretical foundation (formal)

This section gives the formal definitions shared by all modes and marks where each definition lands in the implementation. Notation:
$A,B,C,\dots\in N$ are non-terminals, $X,Y,Z\in N\cup T$ are arbitrary grammar symbols,
$a,b,c,\dots\in T$ are terminals, $\alpha,\beta,\gamma,\dots\in(N\cup T)^{*}$
are symbol strings, $\varepsilon$ is the empty string, and $\$$ is the end-of-input marker (the `END` in the output artifacts).

### 2.1 Grammars, derivations and languages

A context-free grammar is a quadruple $G=(N,T,P,S)$; the right-hand side of a production $A\to\omega$ is
$\omega\in(N\cup T)^{*}$. A one-step derivation $\gamma A\delta\Rightarrow\gamma\omega\delta$
means expanding a non-terminal using some production, $\Rightarrow^{*}$ is its reflexive-transitive closure,
and the language recognized by the grammar is

$$L(G)=\{\,w\in T^{*}\mid S\Rightarrow^{*}w\,\}.$$

The generator performs no derivations at run time; instead it compiles $P$ into an analysis table at generation time, and at run time only looks up the table:
this is the "table-driven" idea common to all modes. LL(1) uses a top-down table with an explicit stack,
SLR(1) / LALR(1) / IELR(1) use a state machine plus a bottom-up ACTION/GOTO table,
### 2.2 FIRST and FOLLOW

$$\mathrm{FIRST}(\alpha)=\{\,a\in T\mid\alpha\Rightarrow^{*}a\beta\,\}\cup\{\,\varepsilon\mid\alpha\Rightarrow^{*}\varepsilon\,\}$$

$$\mathrm{FOLLOW}(A)=\{\,a\in T\mid S\Rightarrow^{*}\alpha Aa\beta\,\}\cup\{\,\$\mid S\Rightarrow^{*}\alpha A\,\}$$

The FIRST of a sequence can be written as "scan until the first non-nullable symbol":

$$\mathrm{FIRST}(X_1X_2\cdots X_k)
=\Bigl(\bigcup_{i=1}^{m}\bigl(\mathrm{FIRST}(X_i)\setminus\{\varepsilon\}\bigr)\Bigr)
\cup\begin{cases}\{\varepsilon\}, & m=k\ \text{且每个 }X_i\text{ 都可空}\\ \varnothing, & \text{其它}\end{cases}$$

where $m=\min\{\,j\in[1,k]\mid\varepsilon\notin\mathrm{FIRST}(X_j)\,\}$,
and when no such $j$ exists, take $m=k$.

FOLLOW is relaxed to the least fixed point by the following three rules ($A\to\alpha B\beta\in P$):

1. $\$\in\mathrm{FOLLOW}(S)$;
2. $\bigl(\mathrm{FIRST}(\beta)\setminus\{\varepsilon\}\bigr)\subseteq\mathrm{FOLLOW}(B)$;
3. If $\varepsilon\in\mathrm{FIRST}(\beta)$, then $\mathrm{FOLLOW}(A)\subseteq\mathrm{FOLLOW}(B)$.

Implementation: `computeFirst` / `computeFollow` (`source/src/analysis.cpp`) are the
worklist-style iteration of the formulas above, with sets stored as `std::set<std::string>`; $\varepsilon$ is a nullable sentinel in the internal
table, and is never written into the ACTION/GOTO table.

### 2.3 LL(1): SELECT sets and the conflict-free condition

$$\mathrm{SELECT}(A\to\alpha)=
\begin{cases}
\bigl(\mathrm{FIRST}(\alpha)\setminus\{\varepsilon\}\bigr)\cup\mathrm{FOLLOW}(A),
  & \varepsilon\in\mathrm{FIRST}(\alpha)\\[2pt]
\mathrm{FIRST}(\alpha), & \varepsilon\notin\mathrm{FIRST}(\alpha)
\end{cases}$$

The predictive parsing table is filled in by SELECT: $M[A,a]$ takes the production $A\to\alpha$
satisfying $a\in\mathrm{SELECT}(A\to\alpha)$. A grammar $G$ is LL(1) if and only if every table entry has at most one
candidate, equivalently

$$\forall A\in N,\ \forall\,\alpha\neq\beta,\ A\to\alpha,A\to\beta\in P:\quad
\mathrm{SELECT}(A\to\alpha)\cap\mathrm{SELECT}(A\to\beta)=\varnothing .$$

When this condition holds, each analysis step needs only to look at the top-of-stack non-terminal and the current Token, with total complexity
$O(n)$, where $n$ is the input length. In the implementation, `selectSet` and `formatGrammarReport`
compute SELECT and output $M[A,\text{Token}]=P_k$, respectively.

### 2.4 Eliminating left recursion and the left-corner graph

**Direct left recursion** refers to productions of the form $A\to A\alpha$. Let

$$A\to A\alpha_1\mid\cdots\mid A\alpha_m\mid\beta_1\mid\cdots\mid\beta_n
\qquad(n\ge 1,\ m\ge 1),$$

where none of the $\beta_i$ begins with $A$; then it can be equivalently rewritten as

$$A\to\beta_1 A'\mid\cdots\mid\beta_n A',\qquad
A'\to\alpha_1 A'\mid\cdots\mid\alpha_m A'\mid\varepsilon ,$$

The two grammars have the same language:

$$L(A)=\bigcup_{i=1}^{n}\beta_i\,\bigl(\alpha_1\mid\cdots\mid\alpha_m\bigr)^{*} .$$

In the implementation, $A'$ is named `<左部>__tail` (an underscore is appended when it conflicts with an existing symbol); the lengths produced,
$|\beta_i|$, are recorded in `baseRhsLength`, and together with `transform`
(`left_base` / `left_continue` / `left_empty`) they let AST folding hide this auxiliary chain.
If $n=0$ (only left-recursive candidates, no base candidate), the grammar cannot terminate and an error is reported directly.

**Indirect left recursion** refers to the case $A\Rightarrow^{+}A\alpha$ where the first derivation step is not a direct rewriting of $A$.
The criterion is the left-corner graph $\mathcal{L}$:

$$\mathrm{LC}(A)=\{\,B\in N\mid A\Rightarrow^{*}B\beta\,\},\qquad
\mathcal{L}=\{\,(A,B)\mid B\in\mathrm{LC}(A)\,\}.$$

Thus

$$A\Rightarrow^{+}A\alpha
\iff\mathcal{L}\ \text{中存在经过 }A\ \text{的环}.$$

Implementation: `checkIndirectLeftRecursion` first computes the set of nullable non-terminals, then scans the right-hand side of each
production, continuing to add edges rightward while the symbol is still nullable, and finally uses DFS to find a cycle in $\mathcal{L}$.
This repository only automatically eliminates direct left recursion; hitting indirect left recursion immediately reports
`文法包含间接左递归：...`, leaving the rewriting work to the grammar author.

### 2.5 LR(0) / LR(1) item sets

An LR(0) item is a production with a dot, $[A\to\alpha\cdot\beta]$. The closure and goto of an item set $I$:

$$\mathrm{closure}(I)=I\cup\{\,[B\to\cdot\gamma]\mid
[A\to\alpha\cdot B\beta]\in I,\ B\to\gamma\in P\,\}\quad(\text{取最小不动点})$$

$$\mathrm{goto}(I,X)=\mathrm{closure}\bigl(\{\,[A\to\alpha X\cdot\beta]\mid
[A\to\alpha\cdot X\beta]\in I\,\}\bigr)$$

The canonical family starts from $I_0=\mathrm{closure}(\{\,[S'\to\cdot S]\,\})$ and is obtained by BFS deduplication over each
$\mathrm{goto}(I,X)$. The implementation is `slrClosure` / `slrGoto`
and `slrBuildAutomaton`.

An LR(1) item adds a lookahead $a\in T\cup\{\$\}$, written $[A\to\alpha\cdot\beta,\ a]$:

$$\mathrm{closure}(I)=I\cup\{\,[B\to\cdot\gamma,\ b]\mid
[A\to\alpha\cdot B\beta,\ a]\in I,\ B\to\gamma\in P,\
b\in\mathrm{FIRST}(\beta a)\,\}$$

$$\mathrm{FIRST}(\beta a)=
\begin{cases}
\bigl(\mathrm{FIRST}(\beta)\setminus\{\varepsilon\}\bigr)\cup\{a\},
  & \varepsilon\in\mathrm{FIRST}(\beta)\\[2pt]
\mathrm{FIRST}(\beta), & \varepsilon\notin\mathrm{FIRST}(\beta)
\end{cases}$$

The implementation is `lr1Closure` / `lr1Goto`, in which `lr1FirstOfSequence` computes
$\mathrm{FIRST}(\beta a)$. Different lookaheads of the same LR(0) core are first grouped by core,
and each group propagates its lookahead set only once, avoiding the "lookahead count $\times$ production count" repeated insertion.

### 2.6 LALR(1): merging states with the same core

States with the same core are LR(1) states whose LR(0) cores, after the lookahead is removed, are identical, where

$$\mathrm{core}(I)=\{\,A\to\alpha\cdot\beta\mid
[A\to\alpha\cdot\beta,\ a]\in I\,\}.$$

Merging means taking the union of these states' lookaheads:

$$[A\to\alpha\cdot\beta,\ a_1],\dots,[A\to\alpha\cdot\beta,\ a_t]
\ \longmapsto\ [A\to\alpha\cdot\beta,\ \{a_1,\dots,a_t\}].$$

$$\mathrm{core}(I)=\mathrm{core}(J)\ \Longrightarrow\
\mathrm{LA}(I\cup J)=\mathrm{LA}(I)\cup\mathrm{LA}(J).$$

After merging, the number of states does not increase, and the "same lookahead for the same core" property required by LR analysis is still preserved;
the cost is that two originally different LR(1) states may be kneaded together, newly introducing a reduce/reduce conflict.
The implementation is `lalrMergeStates`, which also produces the mapping
`stateMap` from LR(1) state numbers to LALR state numbers, then uses it to remap all transitions.

### 2.7 ACTION / GOTO tables and conflicts

For each state $I$:

$$\mathrm{goto}(I,a)=J\ \Longrightarrow\ \mathrm{ACTION}[I,a]=\mathrm{shift}\ J$$

$$[A\to\alpha\cdot]\in I,\ a\in\mathrm{LA}(I,A\to\alpha)
\ \Longrightarrow\ \mathrm{ACTION}[I,a]=\mathrm{reduce}\ (A\to\alpha)$$

$$[S'\to S\cdot]\ \Longrightarrow\ \mathrm{ACTION}[I,\$]=\mathrm{accept},
\qquad
\mathrm{goto}(I,B)=J\ \Longrightarrow\ \mathrm{GOTO}[I,B]=J$$

Here the way the reduce lookahead $\mathrm{LA}$ is obtained distinguishes the LR modes (IELR follows LALR's
per-item lookahead, only splitting states first according to [§2.8](#28-ielr1-splitting-states-with-the-same-core-on-demand)):

$$\mathrm{LA}^{\mathrm{SLR}}(I,A\to\alpha)=\mathrm{FOLLOW}(A),\qquad
\mathrm{LA}^{\mathrm{LALR}}(I,A\to\alpha)=\mathrm{LA}^{\mathrm{IELR}}(I,A\to\alpha)
=\text{项自带的展望符集合}$$

(The table-generation code for SLR and LALR / IELR shares `LrRecorder`, with only the `hasFollow` switch toggling
the source of $\mathrm{LA}$; LALR can therefore accept some grammars that SLR finds conflicting, and IELR can in turn accept
all LR(1) grammars that LALR finds conflicting.)

A table entry with more than one action is a conflict:

$$\mathrm{ACTION}[I,a]=\{\mathrm{shift}\ J,\ \mathrm{reduce}\ (A\to\alpha)\}
\quad(\text{移进}/\text{归约})$$

$$\mathrm{ACTION}[I,a]=\{\mathrm{reduce}\ (A\to\alpha_1),\ \mathrm{reduce}\ (A\to\alpha_2)\}
\quad(\text{归约}/\text{归约})$$

Priority and associativity resolution is in [§9](#9-conflict-resolution-shared); the count of unresolvable conflicts must exactly match
`%expect N`, otherwise generation fails.

### 2.8 IELR(1): splitting states with the same core on demand

LALR's only flaw comes from the merge in [§2.6](#26-lalr1-merging-states-with-the-same-core): it merges LR(1) states that have the same $\mathrm{core}$
but different lookaheads into one, taking the union of the lookaheads. The union can only make a state "gain" reduce actions, while shift actions are entirely determined by the $\mathrm{core}$,
so the conflicts that LALR has in excess of canonical LR(1) are always reduce/reduce conflicts.

IELR(1) views the merge as a partition: initially each LR(1) state is blocked by $\mathrm{core}$,
and the result is exactly the LALR states. If a block has an unresolved reduce/reduce conflict,
then on the lookahead $a$ that triggers the conflict, the block is split finer according to "the set of reduce items each LR(1) state in the block enables on $a$":

$$\mathrm{red}(I,a)=\{\,A\to\alpha\mid[A\to\alpha\cdot,\,a]\in I\,\}$$

$$\mathrm{red}(I,a)\neq\mathrm{red}(J,a)\ \Longrightarrow\
I\ \text{与}\ J\ \text{分属不同的块}.$$

Splitting only makes the lookahead sets smaller and the reduce actions fewer, so it only removes conflicts and never creates them. After splitting,
a partition refinement with respect to $\mathrm{goto}$ is also performed, ensuring "the successors under every symbol of states in the same block stay in the same block", so that block-level $\mathrm{goto}$ is well-defined:

$$\forall I,J\ \text{同块},\ \forall X:\quad
\mathrm{goto}(I,X)\ \text{与}\ \mathrm{goto}(J,X)\ \text{同块}.$$

Repeat "build tables → find reduce/reduce conflicts → split → goto-stability refinement" until there are no unresolved conflicts,
or the partition is already as fine as canonical LR(1) (if conflicts remain then, the grammar is not LR(1)). Because splitting only happens at conflicts,
IELR's state count is usually far smaller than canonical LR(1), of the same order as LALR; for a grammar that is itself LALR(1),
no splitting is triggered and the output is exactly identical to LALR.

### 2.9 Language-class inclusion

Measured by "which grammars are accepted", the three deterministic LR modes have strictly increasing power, and
$\mathrm{IELR}(1)$ has reached the upper bound of canonical $\mathrm{LR}(1)$; LL(1) cannot be directly compared with them,
but is likewise weaker than $\mathrm{LR}(1)$:

$$\mathrm{LR}(0)\subsetneq\mathrm{SLR}(1)\subsetneq\mathrm{LALR}(1)
\subsetneq\mathrm{IELR}(1)=\mathrm{LR}(1),\qquad
\mathrm{LL}(1)\subsetneq\mathrm{LR}(1).$$

| Mode | Source of reduce lookahead | Items | State scale | Notes |
| --- | --- | --- | --- | --- |
| LL(1) | SELECT | No item sets | Smallest table | Most demanding; left recursion must be eliminated |
| LR(0) | No lookahead: complete items reduce on **all** terminals | LR(0) | Smallest | Looks only at item sets, not lookahead, so almost every practical grammar has conflicts; the most intuitive reference starting point when teaching the LR analysis drill (`--mode lr0`) |
| SLR(1) | $\mathrm{FOLLOW}(A)$ | LR(0) | Small | Lookahead is coarse, prone to spurious conflicts |
| LALR(1) | Per-item lookahead (union over the same core) | LR(1) merged | Medium | For non-LALR grammars it produces reduce/reduce conflicts (for how the deterministic modes handle this see §9: pick one automatically + a big warning) |
| LR(1) | Per-item lookahead | LR(1) | Large | Canonical LR(1), no merging; the upper bound when contrasting with LALR (`--mode lr1`, no switch needed) |
| IELR(1) | Per-item lookahead within blocks after splitting | LR(1) merged + on-demand splitting | Same order as LALR | Power equal to LR(1), the strongest mode this repository supports |

### 2.10 Formula-to-implementation mapping

| Formula / concept | Implementation |
| --- | --- |
| $\mathrm{FIRST}$ / $\mathrm{FOLLOW}$ | `computeFirst` / `computeFollow` |
| $\mathrm{SELECT}$ | `selectSet` |
| Direct left-recursion rewriting | `eliminateDirectLeftRecursion` |
| Cycle finding in the left-corner graph $\mathcal{L}$ | `checkIndirectLeftRecursion` |
| LR(0) closure / goto | `lr0Closure` / `lr0Goto` (defined in `analysis.cpp`, shared by SLR and LR(0)) |
| LR(0) item-set family | `lr0BuildAutomaton` |
| LR(1) closure / goto | `lr1Closure` / `lr1Goto` |
| $\mathrm{FIRST}(\beta a)$ | `lr1FirstOfSequence` |
| Canonical LR(1) states (no merging) | `lr1ToCanonicalStates` |
| Merging states with the same core | `lalrMergeStates` |
| Block-level state rebuilding / conflict splitting / goto refinement | `buildQuotient` / `splitBlockOnToken` / `stabilizePartition` |
| ACTION / GOTO table filling | `slrBuildTables` (SLR) / `lr0BuildTables` (LR(0)) / `buildLalrStyleTables` (shared by LALR / LR(1) / IELR) |
| Conflict notes in teaching mode | `teachingConflictNote` |
| Shift/reduce resolution | `computeProductionPriority` / `tryResolveShiftReduce` |

### 2.11 The `error` column and `%sync`: where error recovery lands in the analysis layer

Error recovery has two layers (the `error` production takes priority, `%sync` is the fallback); the analysis layer participates in only half of the first layer—
treating `error` as an ordinary terminal and computing it into the table; the second layer is entirely at run time:

| Thing | Into the analysis layer? | Notes |
| --- | --- | --- |
| `error` (a pseudo-terminal) | Yes | Added to the terminal set, participating in closure / goto / the ACTION table, producing an extra `error` column |
| `%sync TOKEN...` | **No** | Merely a set of anchors at run time; at run time it is used to look up the existing ACTION table |
| `FOLLOW` (the recovery table for the LL family) | Yes (it was computed anyway) | The generator renders FOLLOW into `s_ParserFollow` / `kLlFollow` / `_LL_FOLLOW` in the output artifacts, consulted during top-down recovery (see §4) |

**How `error` enters the table.** Before `computeFirst`, `error` is appended to the **end** of the terminal set,
so in a grammar that never writes `error`, none of the terminals' indices move at all.
After that everything proceeds as usual:

$$[A\to\alpha\cdot error\,\beta]\in I
\ \Longrightarrow\ \mathrm{ACTION}[I,error]=\mathrm{shift}\ \mathrm{goto}(I,error)$$

`error` is a terminal, so closure does not expand on it; $\mathrm{goto}(I,error)$ is computed normally; lookaheads
propagate as usual by $\mathrm{FIRST}(\beta a)$—because `error` is a terminal, $\mathrm{FIRST}$ stops as soon as it sees it
($\mathrm{FIRST}(error\,\beta\,a)=\{error\}$), and it does not, like a non-terminal, carry the lookaheads of $\beta$ and $a$ into new items.

**Why FOLLOW excludes `error`.** By definition, $\mathrm{FOLLOW}(A)$ is "the set of
terminals that can follow $A$". `error` is not a symbol that can appear in the input, but a pseudo-Token **manufactured** at run time:
stuffing it into FOLLOW would inflate SLR's reduce lookaheads, LL(1)'s synchronization decision (§4) and `%expect`'s conflict
count, and could even lead to non-executable table entries like "reduce on `error`". So
`computeFollow` treats `error` as "not nullable and stop here" as soon as it scans it (it is not added to the set and
not propagated).

**Conflicts on the `error` column.** `error` is in no FOLLOW set, so a normal grammar hardly ever conflicts. When there is a real conflict,
the yacc convention **shift first** applies, a warning is written (generation is not aborted), and it is also **not counted** in
`%expect`'s unresolved-conflict count—`%expect` counts the user grammar's own shift/reduce conflicts.

**Why `%sync` can stay entirely out of the analysis layer.** Panic mode needs only two pieces of information: whether the current state can
shift some synchronization Token, and which Token to discard input up to; both can be obtained at run time by looking up the existing
ACTION table. So `%sync` is just a string of indices in `ProjectRequest::syncTokens`, written into
`Parser_IsSyncToken` in the output artifacts at generation time (LR additionally has `Parser_LRCanShiftSyncToken`).
The analysis layer (item sets, FIRST/FOLLOW,
ACTION/GOTO, conflict counting) needs not a single line changed.

### 2.12 LR(k) / LALR(k): k-tuple lookaheads and a two-level ACTION table

The implementation has only one newly added piece of construction logic; everything else reuses the LR(1) machinery (`source/src/analysis_lrk.cpp`):

**Item representation.** An item is still an `Lr1Item`; what goes in the `lookahead` field changes from "one Token" into
a "k-tuple key"—the k terminal names are joined with `'\x01'` (`lookaheadKeyOf`). For k = 1 the key
is exactly the Token name itself, so `operator<` / `operator==` / state hashing / merging states with the same core /
the GOTO table all work unchanged.

**FIRST_k.** `computeFirstK` computes each non-terminal's $\mathrm{FIRST}_k$: the set of terminal strings of length ≤ k
(the empty string means it can derive $\varepsilon$; a length of exactly k means to truncate once k have been seen).
In the closure, $[A\to\alpha\cdot B\beta,\ u]$ propagates
$\mathrm{FIRST}_k(\beta u)$ to each production of $B$—when $\beta$ is nullable, $u$ is appended and then truncated to k. The initial item is
$[S'\to\cdot S,\ \#^k]$ (k `END`s).

**Why the ACTION table must be two-level.** A shift looks only at the **next** Token, independent of the lookahead k-tuple;
only a reduce depends on the full k-tuple. If both were packed into a single dense table, the shift column would have to be written
once for every continuation $(a,\ast,\dots,\ast)$. So the table is built in two levels:

| Level | Key | Action |
| --- | --- | --- |
| Level 1 `akAction` / `s_ParserAction` | A single Token | shift |
| Level 2 `kReduce` / `s_ParserReduce` | The k-tuples used (numbered into columns) | reduce / accept |

At run time it looks up `<state, window>`: first look up the second level by the window's k-tuple (encoded into an integer, then binary-searched); if it hits and is
non-zero, use it; otherwise look up the first level's shift by the window's head. This is exactly the standard Knuth LR(k) table lookup, and it also
makes "resolve by priority" more precise along the way: on a reduce-vs-shift conflict, choosing the reduce affects only **this one**
k-tuple, and other tuples in the same state beginning with the same Token shift as before.

**Conflicts and resolution.** A cross-level conflict (where the reduce column's first Token happens to also have a shift) uses
`tryResolveShiftReduce` to judge priority by the **tuple's first Token** (`%prec` / `%left` are single-Token
concepts; for k > 1 there is no better counterpart); if it cannot be resolved, the yacc convention shift-first applies and it is recorded in
`[未消解冲突]` (the `error` column only records a warning, consistent with §2.11). `lrk`, like `lr1`, "reports but does not
abort"; `lalrk`, like `lalr`, is strict by default and lets through with `%teach`.

**LALR(k) = merging + LR(k)'s lookahead.** Merging states with the same core (union the k-tuple sets when the cores are identical) directly
reuses `lalrMergeStates`—it groups by core and unions the lookahead sets, making no assumption about "whether the lookahead
is a single Token or a k-tuple". The cost is the same: **after merging, LALR(k) may have more
reduce/reduce conflicts than LR(k), and lengthening k cannot save it** (see §7).

| Quantity | LR(k) | LALR(k) |
| --- | --- | --- |
| States | Canonical LR(k) item sets, no merging | Canonical LR(k) item sets merged by core |
| Lookahead | k-tuple | k-tuple (unioned after merging) |
| Power | The $\mathrm{LR}(k)$ language class | Strictly weaker: the conflicts introduced by merging cannot be removed even by lengthening k |

In terms of language-class inclusion, $\mathrm{LR}(0)\subsetneq\mathrm{SLR}(1)\subsetneq\mathrm{LALR}(1)
\subsetneq\mathrm{LR}(1)\subseteq\mathrm{LALR}(2)\subseteq\cdots\subseteq
\mathrm{LR}(k)\subseteq\cdots\subsetneq\mathrm{CFG}$ (the LR side of the diagram in §2.9 gains an extra
k-step staircase).

**The ceiling on k: no policy ceiling.** `k_maxLookaheadK = INT_MAX`, and `%lookahead N` /
`--lookahead N` / `LL(7)` in a mode name are all accepted—at your own cost: the set sizes of $\mathrm{FIRST}_k$ /
$\mathrm{FOLLOW}_k$ and the number of k-tuple columns both blow up exponentially with k, and the usable k usually
does not exceed 3–4. Exceeding `k_lookaheadWarnThreshold = 3` prompts a reminder at generation time, but does not block it.

The one hard limit comes from **encoding width**, not policy: the k-tuple column encoding is
$\sum_i \mathrm{Token号}_i\cdot(\text{Token 个数})^i$, accumulated with 64 bits at run time, so
$(\text{Token 个数})^k$ must fit in `INT64_MAX` (`lookaheadEncodingMaxK` computes exactly
this limit). When it does not fit, generation reports an error directly and tells the user the maximum k this grammar can reach—better than silently emitting
a table that would look up incorrectly.

### 2.13 SLR(k) and LL(k): two other routes on the same k

The two tools $\mathrm{FIRST}_k$ / $\mathrm{FOLLOW}_k$ are not only used by LR(k).
`source/src/analysis_slrk.cpp` and `source/src/analysis_llk.cpp` wire them into two other classic routes:

**SLR(k) = LR(0) item sets + $\mathrm{FOLLOW}_k$.** The state machine is identical, item for item, to SLR(1)'s
(`lr0BuildAutomaton` is reused as-is)—SLR's simplification relative to LR is precisely "when reducing, do not look at the
context in the item, only at the FOLLOW of the production's left-hand side". Generalizing to k changes only one thing: the lookahead
set of a complete item goes from $\mathrm{FOLLOW}(A)$ to $\mathrm{FOLLOW}_k(A)$. Thus:

$$[A\to\alpha\,\cdot]\in I
\ \Longrightarrow\ \mathrm{ACTION}[I,u]=\mathrm{reduce}\quad
\forall u\in\mathrm{FOLLOW}_k(A)$$

**The tables and output artifacts directly reuse the LR(k) set**: the $k$-tuple reduce columns, the binary search over column encodings, and the run-time window are all
the same, so `slrk`'s output-artifact layer comes for free. For k = 1, the ACTION/GOTO of `slrk` and `slr` are identical
line for line (measured to differ only in the fictitious start-symbol name).

**LL(k) = a SELECT_k predictive parsing table.** The top-down route: $\mathrm{FIRST}_k$ /
$\mathrm{FOLLOW}_k$ both come after it,

$$\mathrm{Select}_k(A\to\alpha)=\mathrm{FIRST}_k(\alpha\cdot\mathrm{FOLLOW}_k(A))$$

(the k-prefix derived by α is appended with each k-tuple of $\mathrm{FOLLOW}_k(A)$ and then truncated to k,
using the same `extendLookahead` as lookahead propagation in the LR closure). A table entry is
$M[A,u]=\text{产生式}$; the same $(A,u)$ hitting two productions reports "the grammar is not LL(k)".
The object being decided is the grammar **after eliminating direct left recursion** (reusing the LL(1) transformation), so a grammar like
`calc` that has left recursion can also be generated under LL(k).

**LL(0) is the starting point of this chain.** For k = 0 there is no lookahead, and $\mathrm{Select}_0(A\to\alpha)
=\{\varepsilon\}$ is the same for every production, so "the same $M[A,\varepsilon]$ can correspond to
only one production"—that is, every non-terminal has exactly one production and the language has only one string left. The implementation needs no
special branch: `k` is allowed to be 0, the predictive parsing table degenerates to a single column, and the run-time column number is always 0.

**The inclusion relations on k (the LL side looks the same as the LR side, but the two sides are not one chain):**

$$\mathrm{LL}(0)\subsetneq\mathrm{LL}(1)\subsetneq\cdots\subsetneq\mathrm{LL}(k)
\subsetneq\cdots\subsetneq\mathrm{LR}(1)$$ (both chains are proper subsets of CFG, but
there is no inclusion relation between $\mathrm{LL}(k)$ and $\mathrm{LR}(k)$)

| Quantity | SLR(k) | LL(k) |
| --- | --- | --- |
| States | LR(0) item sets (same as SLR(1)) | No state machine; an explicit stack |
| Decision basis | $\mathrm{FOLLOW}_k(A)$ | $\mathrm{Select}_k(A\to\alpha)$ |
| Table | Two-level ACTION (same shape as LR(k)) | $M[A,u]$, u is a k-tuple |
| k = 1 | `slr` | `ll1` |

## 3. Data model

`AnalysisResult` (`source/include/ParseEngine/analysis.hpp`) is shared by all modes:

| Field | Meaning |
| --- | --- |
| `mode` | `"ll1"` / `"lr0"` / `"slr"` / `"lalr"` / `"lr1"` / `"ielr"` |
| `grammar` | The processed grammar (for LL, left recursion eliminated; for LR, a fictitious start symbol added) |
| `terminals` / `nonterms` | The terminal and non-terminal lists (with a stable order) |
| `first` / `follow` | The FIRST / FOLLOW sets; LALR / LR(1) do not compute FOLLOW (`hasFollow=false`), and LR(0) uses neither |
| `productions` | The flattened productions (with a global number `id`, which a reduce item in ACTION points to) |
| `slrStates` / `lalrStates` | The LR(0) item-set family (LR(0) / SLR) / the LALR / LR(1) / IELR states (core → lookahead; under LR(1) each core corresponds to a canonical LR(1) item) |
| `lr1StateCount` | The number of canonical LR(1) states (the actual state count for LR(1); the upper bound before LALR merging / IELR splitting) |
| `transitions` | The GOTO transitions `(state, symbol) -> state` (both terminals and non-terminals) |
| `action` / `gotoTable` | The ACTION table / the non-terminal GOTO table |
| `table` | The LL(1) predictive parsing table `(non-terminal, terminal) -> production index` |
| `unresolved` / `resolutions` | Records of unresolved / resolved conflicts |
| `ielrLalrStateCount` | The number of LALR states before IELR splitting (IELR only) |
| `ielrSplits` | IELR state-split records: original LALR state number, triggering Token, kind of conflict, new state number, reason |
| `lr1MergedStateCount` | The number of LALR states after merging states with the same core (LR(1) only, written into the report's `[教学对照]`) |
| `teach` | Whether teaching mode is on (`%teach` / `--teach`); only when true are `[教学提示]` / `[教学对照]` output, and `slr` / `lalr` / `ielr` tolerate unresolved conflicts. Unrelated to the `mermaid/` state-machine diagrams (those are `ProjectRequest::mermaid`) |
| `stateCount()` | The total number of states in LR modes (LR(0) / SLR count `slrStates`, the rest count `lalrStates`) |

## 4. LL(1) (`analysis_ll1.cpp`)

The entry point is `Analysis::prepareGrammar(grammar, tokens)` (LL has no separate `ll1Analyze`
name; it is semantically equivalent to the other two modes' methods):

1. **Eliminate direct left recursion**: by the formula in
   [§2.4](#24-eliminating-left-recursion-and-the-left-corner-graph), rewrite
   $A\to A\alpha\mid\beta$ as $A\to\beta A'$, $A'\to\alpha A'\mid\varepsilon$
   ($A'$ being `<左部>__tail`); if a left-recursion rule has no base candidate ($n=0$), an error is reported.
2. **Detect indirect left recursion**: find a cycle in the left-corner graph $\mathcal{L}$; on a hit it directly reports
   `文法包含间接左递归：...`.
3. **FIRST / FOLLOW**: `computeFirst` / `computeFollow` (shared).
4. **SELECT sets**: `selectSet` computes each production's SELECT; when the right-hand side is nullable, FOLLOW is folded in.
5. **Predictive parsing table**: if the same `(non-terminal, terminal)` gets two productions it reports
   `文法不是 LL(1)：M[X, token] ...`.
6. **Report**: `formatGrammarReport` outputs the final productions, FIRST, FOLLOW,
   `M[非终结符, Token] = Pk`.

The terminal set = `k_defaultTokenNames` + the Tokens declared in the grammar + `"END"`, deduplicated in order.

The FOLLOW computed by the analysis layer does more than go into the report: the generator renders it into
`s_ParserFollow` (C) / `kLlFollow` (C++17) / `_LL_FOLLOW` (Python) in the output artifacts, for run-time **FOLLOW-set error
recovery**—the FOLLOW of a non-nullable non-terminal is not in the prediction table, and recovery's "current Token is in the FOLLOW of this
top-of-stack item → give up this layer" relies entirely on it (see
[`05-grammar.md` §3.2](../05-grammar.md)).

## 5. LR(0) / LR(1) canonical-automaton modes (`analysis_lr0.cpp` / `analysis_lr1.cpp`)

`--mode lr0` / `--mode lr1` (`%mode lr0` / `%mode lr1` in a `.gram`) compute the
two item-set families of §2.5 as-is—LR(0) does not consult FOLLOW, LR(1) does not merge states with the same core—then fill in
ACTION / GOTO. These two modes are entirely on a par with the rest: they share the lexer, front end, output-artifact templates,
LR runtime and semantic-action translation, and **need no switch at all** to generate, compile and run code. The only
differences are that they never abort generation because of unresolved conflicts (see §5.3), and that the report may gain teaching sections.

The LR family (`lr0` / `slr` / `slrk` / `lalr` / `lalrk` / `lr1` / `lrk` / `ielr`) all produce state-machine diagrams when
`%mermaid` / `--mermaid` is turned on (§5.4); this switch is unrelated to teaching mode.

**Teaching mode (`%teach` / `--teach`) is another independent switch, governing only the report and the
conflict strictness of `slr` / `lalr` / `ielr`** (the command line takes priority over `%teach`):

| Item | Default (teaching mode off) | Teaching mode (`%teach` / `--teach`) |
| --- | --- | --- |
| Applicable modes | All modes | The report sections take effect for the eight deterministic LR modes `lr0` / `slr` / `slrk` / `lalr` / `lalrk` / `lr1` / `lrk` / `ielr`; the LL family has no conflicts to discuss and `lex` has no analysis layer, so they are ignored with a one-line notice |
| Unresolved conflicts of `lr0` / `lr1` | Report only, no abort (these two modes have always behaved this way) | Also report only, no abort, plus a `[教学提示]` explaining why it is not an LR(0) / LR(1) grammar |
| Unresolved conflicts of `slr` / `lalr` / `ielr` | An error is raised directly; no code is produced | Report only, no abort: written into `[未消解冲突]`, and table entries continue to be generated with shift-first |
| Report | Consistent with the other LR modes | Gains `[教学提示]` (`teachingConflictNote`) and `[教学对照]` (state counts, conflict counts) |
| Output artifacts | Isomorphic with the other modes | Isomorphic with the other modes (diagrams are controlled separately by `%mermaid`) |
| `%expect N` | Validated by `%expect` (both cases work as usual) | Same as left |

In other words: teaching mode neither changes the usability of `lr0` / `lr1` nor decides whether diagrams are produced.

### 5.1 LR(0) (`analysis_lr0.cpp`)

1. `augmentGrammar(grammar, "__lr0_start")` adds a fictitious start symbol and computes FIRST (only for
   the report; LR(0)'s closure and table filling do not use it), and does not compute FOLLOW.
2. `lr0BuildAutomaton` (defined in `analysis.cpp`, with SLR and LR(0) sharing the same
   `lr0Closure` / `lr0Goto`) does a BFS, obtaining the LR(0) item-set family and `transitions`.
3. `lr0BuildTables` differs from SLR's `slrBuildTables` in exactly one place: a complete item with the dot at the end
   **writes a reduce on every Token in `terminals`**, without consulting FOLLOW:

   $$\text{ACTION}[I,\ a]=\text{reduce }(A\to\alpha)\quad
   \forall\,[A\to\alpha\,\cdot\,]\in I,\ \forall a\in T$$

   So as soon as a state has both a shiftable item and a complete item, a shift/reduce conflict necessarily appears—
   this is precisely the intuitive explanation of $\mathrm{LR}(0)\subsetneq\mathrm{SLR}(1)$.
4. Report `formatLr0Report`: `[LR(0) 自动机]`, `[ACTION 表]`, `[GOTO 表]`,
   `[冲突消解]`, `[未消解冲突]` (plus `[教学提示]`) and `[教学对照]`
   (`LR(0) 状态数` / `LR(0) 表未消解冲突数`). LR(0) uses neither FIRST nor
   FOLLOW, so the report deliberately does not print these two sections.

### 5.2 LR(1) (`analysis_lr1.cpp`)

1. `augmentGrammar(grammar, "__lr1_start")`; computes FIRST (the LR(1) closure uses it to compute
   $\mathrm{FIRST}(\beta a)$).
2. `lr1_detail::lr1BuildAutomaton` builds the canonical LR(1) item-set family (the same
   code as LALR / IELR), and `lr1StateCount` records the state count.
3. `lr1_detail::lr1ToCanonicalStates` converts each state as-is into core → lookahead
   form (**without merging**), with state numbers in one-to-one correspondence with the canonical LR(1) states; the transition table is used directly.
4. `buildLalrStyleTables(..., "LR(1)", ...)`: when the dot is at the end, a reduce is written on the item's own lookahead,
   using the same function as LALR—the only difference being whether the states passed in have been merged.
5. Incidentally, `lalrMergeStates` is run once on the same item-set family, and the merged state count is recorded in
   `lr1MergedStateCount`, written into the report's `[教学对照]`:

   ```
   [教学对照]
   规范 LR(1) 状态数 = 26
   LALR 同心合并后状态数 = 24
   ```

   The difference is the state count saved by merging states with the same core. `example/ielr_split.gram` with `--mode lr1`
   goes 26 → 24: merging saves 2 states, at the cost of merging the lookaheads of `x -> c .` and `y -> c .`
   together, so LALR reports a reduce/reduce conflict while LR(1) does not.

Under `--dump-lr-automaton`, LR(1) prints one item per line in textbook style
(`[A -> α . β, a]`), while LR(0), like SLR, prints the LR(0) item-set family.

### 5.3 Conflict handling (three degrees of permissiveness)

The entry point for conflict handling is `checkUnresolvedConflictsForMode(label, result, allowConflicts)`
—the mode itself decides `allowConflicts`, so "who is strict, who lets through" is written in the mode file, not reverse-looked-up from
`%teach`:

| Mode | `allowConflicts` | Consequence of unresolved conflicts |
| --- | --- | --- |
| `lr0` / `lr1` | Always true | Report only, no abort, usable in any mode: conflicts are written into the report's `[未消解冲突]`, table entries are written with the yacc default, and a warning is printed to stderr |
| `slr` / `lalr` / `ielr` | `result.teach` | Strict by default—goes through §9's `checkUnresolvedConflicts`, raising an error directly and producing no code; only with `%teach` / `--teach` on is it let through |

When letting through (`lr0` / `lr1` always, `slr` / `lalr` / `ielr` in teaching mode):

- The actions written into the table follow the yacc default: shift / reduce takes the shift, reduce / reduce takes the earlier reduce
  (`LrRecorder::record` already has this default; it is just that strict mode reports an error afterwards);
- The command line prints a warning to stderr, "table written with shift-first, generation continues", and in parentheses points to
  the "teaching notes" (teaching mode) or "unresolved conflicts" (`lr0` / `lr1`) section—so nothing is
  missed even without writing the report;
- In teaching mode the report gains a `[教学提示]` section (`teachingConflictNote`): the grammar is not
  LR(0) / LR(1), conflicts are counted by kind (how many shift/reduce, how many reduce/reduce), and
  what the generator will do next;
- `checkUnresolvedConflicts` is called to validate the count only when `%expect N` is written explicitly.

An ambiguous expression grammar can directly show the complete automaton and the conflicts themselves via `--mode lr0` (the conflicts are the teaching
material), or show "why it is not SLR(1)" via `--mode slr --teach`. For what a genuine LR(0) grammar looks
like, see `example/lr0.gram` (5 states, whole-row
reduce for complete items, zero conflicts, with `%mode lr0` / `%teach` / `%mermaid` built into the grammar).

Key points of these two modes: `example/lr0.gram` has no conflicts and parses; an ambiguous grammar under `lr0`
is generated as usual and reports conflicts **without turning on any switch**, gains a teaching section in the report after `--teach`, and
`--no-teach` can push back the `%teach` in the grammar (while `slr` remains strict on the same grammar);
`ielr_split` generates and parses successfully under `lr1` with a canonical state count strictly greater than after merging; grammars with no states with the same core
to merge (`ini` / `or_branch` / `lr0` / `lex_demo`) get the same analysis table under `lr1` and `lalr`.

### 5.4 State-machine diagrams (`source/src/mermaid.cpp`)

Turning on `%mermaid` / `--mermaid` draws the state machines already computed at generation time as diagrams
(produced by `Mermaid::projectFiles`). This switch is completely independent of teaching mode: diagrams are produced without `%teach`,
and turning on `%teach` does not automatically produce them either; whether `mermaid/` is in the artifact list is decided solely by
`ProjectRequest::mermaid`.

| Diagram | Data source | Drawing method |
| --- | --- | --- |
| Grammar automaton | `slrStates` (`lr0` / `slr`) or `lalrStates` (`lalr` / `lr1` / `ielr`) + `transitions` + `action` + `unresolved` | States = LR item sets (`lalrStates` lists the lookaheads item by item in textbook style), edges = GOTO transitions (terminals labeled as shifts, non-terminals labeled `GOTO`); states are colored by "conflict / accept / reduce / shift" via `classDef`, and the last line inside a box is the action summary from the ACTION table; `ll1`, whose `isLrMode` is false, does not produce this diagram |
| Lexical DFA | `RegexDfa::compileAll` (the same compilation as `CodeGenC99::lexDfa`) | One per `%token regex` / `%identifier`: states = DFA states (double circles are accepting states), edges = readable text for shared character classes (consecutive characters compressed to `a-z`, more than 16 characters truncated by segment), trap states and the edges pointing to them are not drawn |

A few implementation points:

- What is generated is a Mermaid `flowchart LR`. The `[` `]` `(` `)` `{` `}` `"` `|` `#`
  in labels are escaped with Mermaid's own `#nn;` (`Mermaid::escapeLabel`), and line breaks use `<br/>`;
  this combination has been verified diagram by diagram against Mermaid's official parser—`flowchart`'s quoted labels withstand
  odd characters (such as a Token name like `:`) better than `stateDiagram-v2`.
- `Mermaid::htmlPage` puts the source directly into `<div class="mermaid">` (Mermaid reads the
  node's `innerHTML`, so neither `<br/>` nor `#91;` is resolved away by HTML first), with an additional
  HTML-escaped copy of the source in `<details><pre>`, readable and copyable even offline.
  The page loads `mermaid@11` from a CDN and raises Mermaid's own `maxEdges` / `maxTextSize`
  limits.
- **Size gate**: a diagram exceeding 150 states / 400 edges / 120 KB draws just one sentence, pointing back to the analysis
  report and `--dump-lr-automaton`. A real grammar's automaton really does get too large to study—
  `example/c89_subset.gram`'s LALR is 340 states / 3117 transitions, and canonical LR(1) is
  1691 states / 16586 transitions; even if drawn, the browser cannot open it.
- The output artifacts are only `mermaid/index.html` / `grammar.html` / `lexer.html` and
  `grammar.mmd` / `lexer.mmd` (when there are no regex Tokens, `lexer.*` is not generated);
  the other artifacts like `parser.c` are unaffected; when there are no diagrams at all (`ll1` with no regex Tokens), not even
  the `mermaid/` directory is created.
- The command line `--dump-mermaid` prints the same source (in Markdown fenced format), usable in any mode;
  when there is no diagram to draw, it prints a line to stderr and leaves standard output empty.

## 6. SLR(1) (`analysis_slr.cpp`)

Build the LR(0) item-set family per [§2.5](#25-lr0--lr1-item-sets), then fill the table per
[§2.7](#27-action--goto-tables-and-conflicts). `Analysis::slrAnalyze`:

1. `augmentGrammar(grammar, "__slr_start")` adds a fictitious start symbol, yielding the new start production.
2. Compute FIRST / FOLLOW (FOLLOW is used to decide the lookahead when reducing).
3. `lr0BuildAutomaton`: starting from the initial item set `{__slr_start -> . S}`, do the
   BFS of `lr0Closure` / `lr0Goto` (sharing the same code as the LR(0) mode of §5.1),
   obtaining the LR(0) item-set family and `transitions`.
4. `slrBuildTables`: for each item set
   - the symbol after the dot is a terminal → look up `transitions` and write a shift;
   - the dot is at the end and the left-hand side is the start symbol → write accept;
   - the dot is at the end → write a reduce for each Token in `FOLLOW(left-hand side)`;
   - non-terminal transitions write `gotoTable`.
5. `checkUnresolvedConflicts("SLR(1)", ...)` handles unresolved conflicts per `%expect`.

## 7. LALR(1) (`analysis_lalr.cpp`)

Build the LR(1) item-set family per [§2.5](#25-lr0--lr1-item-sets), merge states with the same core per
[§2.6](#26-lalr1-merging-states-with-the-same-core), then fill the table per
[§2.7](#27-action--goto-tables-and-conflicts). `Analysis::lalrAnalyze`:

1. `augmentGrammar(grammar, "__lalr_start")`.
2. `lr1BuildAutomaton`: LR(1) closure / goto, where an item is `Lr1Item{lhs, prod, dot,
   lookahead}`; `lr1ItemHash` / `lr1StateHash` stuff the item sets into an `unordered_map`
   for deduplication, which is faster than comparing `std::set`s one by one.
3. `lalrMergeStates`: merge states with the same core keyed by LR(0) core, unioning the lookaheads;
   it also outputs `stateMap` (LR(1) state number → LALR state number).
4. Use `stateMap` to remap `lr1Transitions`, obtaining LALR's `transitions`.
5. `lalrBuildTables`: similar to SLR, but a reduce's lookahead uses the item's own set
   (not FOLLOW), which is where LALR outdoes SLR; writes `hasFollow = false`.
6. `checkUnresolvedConflicts("LALR", ...)`.

`lr1StateCount` records the pre-merge state count, and the report prints
`LR(1) 状态数 = N，合并后 LALR 状态数 = M`.

## 8. IELR(1) (`analysis_ielr.cpp`)

IELR fully reuses LALR's LR(1) construction, splitting only at conflicts. `Analysis::ielrAnalyze`:

1. `prepareLrAnalysis(grammar, tokens, "__ielr_start", "ielr", ...)`: add a fictitious start symbol,
   compute FIRST, flatten Tokens (the same shared preparation as SLR / LALR).
2. `lr1BuildAutomaton` builds the canonical LR(1) item-set family and transitions (the same code as LALR, defined in
   the `lr1_detail` namespace of `analysis_lalr.cpp`).
3. `lalrMergeStates` yields the initial partition (LR(1) state → block); the block-level `states` / `transitions`
   are the LALR states; `ielrLalrStateCount` records the state count before splitting.
4. `buildLalrStyleTables` builds the tables once; as long as there are still reduce/reduce conflicts in `unresolved`:
   - `splitBlockOnToken` splits the block by the set of reduce items of each sub-state on the triggering Token;
   - `stabilizePartition` refines the partition until goto-stable;
   - `buildQuotient` rebuilds the block-level states and transitions, then builds the tables again. Loop until there are no conflicts or the partition is already as fine as
     canonical LR(1).
5. Collect the split records into `AnalysisResult::ielrSplits`: the LALR state number before splitting, the triggering Token,
   the kind of conflict, the state number after splitting, and the reason text for "which two reduce items are fighting over which Token".
6. `checkUnresolvedConflicts("IELR(1)", ...)` does the final validation; if conflicts remain at this point, the grammar
   is not even LR(1), and an error is raised by the existing rules (the `%expect` semantics are the same as the other modes).

Table rendering directly reuses `formatSlrTables`, so IELR's ACTION/GOTO output format is exactly the same as
SLR / LALR's. The report is output by `formatIelrReport`, with two sections more than LALR:
`[IELR 自动机]` (LALR state count / IELR state count after splitting / canonical LR(1) state count /
ACTION table entry count / GOTO transition count) and `[IELR 状态拆分]`; `formatIelrAutomaton` serves
`--dump-lr-automaton`. In `RESULT.md`, `generatorResultMd` (`source/src/project.cpp`)
renders the same "IELR(1) state splitting" section.

For a pure LALR(1) grammar (`calc` / `pl0` / `c89_subset`) you can check for yourself whether IELR's and LALR's
`parser_table.c` are equivalent—they differ only in the mode name and the fictitious start-symbol name, with the ACTION/GOTO data exactly
identical.

## 9. Conflict resolution (shared)

Sources of priority:

- The priority of a shift action: the declaration of the conflicting Token in `%left` / `%right` / `%nonassoc`;
  the lower the line, the higher, and within a line it increases from left to right.
- The priority of a reduce action: the `%prec TOKEN` at the end of a production; if not written, the rightmost terminal of the right-hand side is taken;
  `computeProductionPriority` handles this computation.

`tryResolveShiftReduce`'s comparison rules:

1. Both have priority: the higher wins; on a tie, look at associativity—`%left` reduces, `%right` shifts,
   `%nonassoc` errors.
2. Only one has priority: the one with priority wins.
3. Neither: return empty, and `LrRecorder::record` handles it by "shift-first by default", recording it in
   `unresolved`.

`checkUnresolvedConflicts`:

- Reduce/reduce conflicts **do not come here**: deterministic LR modes decide in place while building the table (see below),
  and only IELR's probing phase sees them;
- With no `%expect` written, any unresolved conflict is an error;
- With `%expect N` written, the actual unresolved count must exactly equal N, otherwise an error;
- On a matching count, a warning is printed and generation continues (writing the table shift-first).

**Reduce/reduce conflicts: pick one automatically + a big warning.** A reduce/reduce conflict is constrained by nothing but priority,
but letting it crash generation outright is not the only option—bison's approach is to pick one and continue. Since v0.8 this generator
applies one set of rules to the **deterministic LR modes** (`lr0` / `slr` / `lalr` / `lr1` / `ielr` / `lrk` /
`lalrk` / `slrk`):

1. First look at the two productions' priorities (`%prec` or the rightmost terminal of the right-hand side)—**if they
   differ, the higher priority wins**, which is "the production the user recommends";
2. If the priorities are equal (or both unset), take **the production appearing earlier in the grammar** (the one with the smaller
   production number, the yacc convention);
3. The chosen one is written into the ACTION table, and the abandoned one is recorded in `[冲突消解]`;
4. At generation time a **prominent big warning** is printed (`AnalysisResult::warnings`, summarized into one banner,
   listing at most 8 and aggregating the rest—so a conflict-heavy mode like LR(0) does not flood the screen), making clear "who was chosen,
   who was dropped, and on what basis".

So "LALR has reduce/reduce conflicts" is no longer a fatal error, but a **warned automatic decision**; if you want to decide for yourself,
add `%prec` to the two productions. Two exceptions:

- **IELR's probing phase** still needs to see reduce/reduce conflicts (it decides which state to split based on them), so
  `buildLalrStyleTables` gains a `resolveReduceReduce` switch: off during probing,
  on for the final pass after splitting is done. Only when splitting to the end still conflicts (that is, an ambiguity that even canonical LR(1) cannot resolve)
  does it go through "pick one + big warning".
State item-set text is rendered by `lr0StateText` / `lalrStateText` and carried in the conflict description, making it
easy to locate which state and which two items are fighting.

Modes that let conflicts through do not go this way: `lr0` / `lr1` always let through, `slr` / `lalr` / `ielr` only in
teaching mode (`%teach` / `--teach`). They call `checkUnresolvedConflicts` only when `%expect` is written; otherwise conflicts are merely written into the report (and, in teaching mode,
`teachingConflictNote` generates `[教学提示]`). `checkUnresolvedConflictsForMode`
is that fork point; see §5.3.

## 10. Reports and dump

| Function | Location | Purpose |
| --- | --- | --- |
| `formatGrammarReport` | `analysis_ll1.cpp` | LL(1) analysis report (written into `RESULT.md` / `--report`) |
| `formatLr0Report` | `analysis_lr0.cpp` | LR(0) report (teaching mode includes `[教学对照]` and `[教学提示]`) |
| `formatSlrReport` | `analysis_slr.cpp` | SLR report |
| `formatLalrReport` | `analysis_lalr.cpp` | LALR report |
| `formatLr1Report` | `analysis_lr1.cpp` | LR(1) report (including the state-count comparison between canonical LR(1) and LALR after merging) |
| `formatIelrReport` | `analysis_ielr.cpp` | IELR report (including `[IELR 自动机]` / `[IELR 状态拆分]`) |
| `formatLr0Automaton` / `formatSlrAutomaton` | `analysis_lr0.cpp` / `analysis_slr.cpp` | `--dump-lr-automaton` (both are LR(0) item-set families, the same rendering) |
| `formatLr1Automaton` | `analysis_lr1.cpp` | `--dump-lr-automaton` (one item per line `[A -> α . β, a]`) |
| `formatLalrAutomaton` / `formatIelrAutomaton` | The corresponding mode files | `--dump-lr-automaton` |
| `formatLr0Tables` / `formatSlrTables` / `formatLr1Tables` / `formatLalrTables` / `formatIelrTables` | The corresponding mode files | `--dump-slr-tables` (the ACTION / GOTO matrices share the same rendering) |
| The `[教学对照]` / `[教学提示]` sections | `analysis.cpp` (`appendTeachingSection`) | Teaching-mode-specific: state and conflict counts, the state-count comparison between LR(1) and LALR, an explanation of conflict handling; shared by the five deterministic LR modes |
| `Mermaid::grammarDiagrams` / `lexerDiagrams` | `mermaid.cpp` | State-machine diagrams: the `flowchart` source of the grammar automaton and the lexical DFA (§5.4) |
| `Mermaid::htmlPage` / `projectFiles` | `mermaid.cpp` | The HTML page of the state-machine diagrams and the `mermaid/*` artifact list (returns empty when `enabled=false`) |
| `--dump-mermaid` | `source/src/main.cpp` | Print the state-machine diagram source (Markdown fenced format), usable in any mode |
| `resolutionLine` | `analysis.cpp` | Render one resolved conflict as a single line |
| The split section in `formatIelrReport` | `source/src/project.cpp` | Additionally written into `RESULT.md` as "IELR(1) state splitting" |

## 11. Adding a new analysis mode

1. Add a static method to `Analysis` in `source/include/ParseEngine/analysis.hpp`.
2. Create `source/src/analysis_<mode>.cpp`, use `using namespace analysis_detail;`
   to reuse the shared functions, and put private helpers in the file's anonymous namespace.
3. Add a branch in `Analysis::analyze` in `source/src/analysis.cpp`; if a new mode symbol is needed, define it
   following the pattern of `k_slrStartSymbol`.
4. Wire up the artifact differences in `source/include/ParseEngine/project.hpp` / `source/src/project.cpp`,
   and let the new value through in the `--mode` validation in `source/src/main.cpp`.
5. Add templates in `source/templates/` as needed, and re-run `tools/embed_templates.py`.
6. To let the new mode's automaton also be drawn as state-machine diagrams, wire its item-set family into the
   `grammarDiagrams` / `usesLalrStates` of `source/src/mermaid.cpp` (§5.4).
