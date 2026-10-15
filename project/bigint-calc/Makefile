# project/bigint-calc/Makefile —— 生成 + 编译 + 自验（大整数计算器，C++17）
#
#   make            生成 C++17 产物到 build/ 并编译出 build/parser_demo
#   make generate   只生成（调仓库里的 ParseEngine，不改生成器）
#   make run        用 samples/pow_10000.expr 跑一遍（2 的 10000 次方）
#   make check      逐个样例跑，与 expected/*.txt 比对
#   make clean      删掉 build/
#
# ENGINE 指向仓库里已经构建好的生成器（默认 ../../build/bin/ParseEngine；
# 在 Git Bash / MSYS2 下会自动补 .exe，也可以传 ENGINE=.../ParseEngine.exe）。
# 本工程只针对 C++17 产物（--lang cpp17）：语义值直接是大数库的 BigInt。

ENGINE  ?= ../../build/bin/ParseEngine
GRAMMAR := bigint_calc.gram
GEN_DIR := build
DEMO    := $(GEN_DIR)/parser_demo

# Windows 上 make 找不到 sh：优先用 Git Bash / MSYS2 的 bash 跑 check.sh
ifeq ($(OS),Windows_NT)
  SH ?= $(firstword $(wildcard C:/PROGRA~1/Git/bin/bash.exe) \
                    $(wildcard C:/PROGRA~2/Git/bin/bash.exe) \
                    $(wildcard C:/msys64/usr/bin/bash.exe) sh)
else
  SH ?= sh
endif

.PHONY: all generate build run run-stdin check clean

all: build

generate:
	$(ENGINE) --grammar-file $(GRAMMAR) --out-dir $(GEN_DIR) --lang cpp17

# 生成目录里的 Makefile 由生成器产出，它自己会 `-include local.mk`
# （local.mk 由文法的 %copy 一起带过去），里面补大数库的头文件与源码。
build: generate
	$(MAKE) -C $(GEN_DIR)

run: build
	$(DEMO) samples/pow_10000.expr

# 从标准输入读一个表达式：printf '2^1000' | make run-stdin
run-stdin: build
	$(DEMO)

check: build
	$(SH) check.sh

clean:
	rm -rf $(GEN_DIR)
