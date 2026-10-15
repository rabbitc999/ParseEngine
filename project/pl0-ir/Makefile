# project/pl0-ir/Makefile —— 生成 + 编译 + 自验（PL/0 → 三元式 / 四元式）
#
#   make            生成 C++17 产物并编译出 build/parser_demo（三元式）
#   make generate   只生成（调仓库里的 ParseEngine，不改生成器）
#   make run        用 samples/if_while.pl0 跑一遍
#   make check      逐个样例跑，与 expected/*.triples 比对
#   make quads      另一份文法：build-quads/parser_demo（四元式）
#   make run-quads  同上，四元式产物
#   make clean      删掉 build/ 与 build-quads/
#
# ENGINE 指向仓库里已经构建好的生成器（默认 ../../build/bin/ParseEngine，
# 在 Git Bash / MSYS2 下会自动补 .exe；也可以传 ENGINE=.../ParseEngine.exe）。
# 本工程只针对 C++17 产物（--lang cpp17），不改生成器本身。
#
# 注释由文法里的 %trivia 在词法器里跳过（不是 %main 预剥文本），其余与
# 常规 PL/0 示例一致。

ENGINE    ?= ../../build/bin/ParseEngine
GRAMMAR   := pl0.gram
QUAD_GRAMMAR := pl0_quads.gram
GEN_DIR   := build
QUAD_DIR  := build-quads
DEMO      := $(GEN_DIR)/parser_demo
QUAD_DEMO := $(QUAD_DIR)/parser_demo

# Windows 上 make 找不到 sh：优先用 Git Bash / MSYS2 的 bash 跑 ../check.sh
ifeq ($(OS),Windows_NT)
  SH ?= $(firstword $(wildcard C:/PROGRA~1/Git/bin/bash.exe) \
                    $(wildcard C:/PROGRA~2/Git/bin/bash.exe) \
                    $(wildcard C:/msys64/usr/bin/bash.exe) sh)
else
  SH ?= sh
endif

.PHONY: all generate build quads run run-quads check clean

all: build

generate:
	$(ENGINE) --grammar-file $(GRAMMAR) --out-dir $(GEN_DIR) --lang cpp17

build: generate
	$(MAKE) -C $(GEN_DIR)

quads:
	$(ENGINE) --grammar-file $(QUAD_GRAMMAR) --out-dir $(QUAD_DIR) --lang cpp17
	$(MAKE) -C $(QUAD_DIR)

run: build
	$(DEMO) samples/if_while.pl0

run-quads: quads
	$(QUAD_DEMO) samples/if_while.pl0

check: build
	$(SH) check.sh

clean:
	rm -rf $(GEN_DIR) $(QUAD_DIR)
