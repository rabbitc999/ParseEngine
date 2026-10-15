# project/mini-basic/Makefile —— 生成 + 编译 + 自验（Mini-BASIC → 栈式字节码）
#
#   make            生成 C++17 产物并编译出 build/parser_demo
#   make generate   只生成（调仓库里的 ParseEngine，不改生成器）
#   make run        跑 samples/while.bas 看输出
#   make check      逐个样例跑：正常样例比 stdout，错误样例比报错首行
#   make dump       带 MINI_BASIC_DUMP=1 跑 samples/nested.bas（打印字节码）
#   make clean      删掉 build/
#
# ENGINE 指向仓库里已经构建好的生成器（默认 ../../build/bin/ParseEngine，
# 在 Git Bash / MSYS2 下会自动补 .exe；也可以传 ENGINE=.../ParseEngine.exe）。
# 本工程只针对 C++17 产物（--lang cpp17），不改生成器本身。

ENGINE  ?= ../../build/bin/ParseEngine
GRAMMAR := mini_basic.gram
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

.PHONY: all generate build run check dump clean

all: build

generate:
	$(ENGINE) --grammar-file $(GRAMMAR) --out-dir $(GEN_DIR) --lang cpp17

build: generate
	$(MAKE) -C $(GEN_DIR)

run: build
	$(DEMO) samples/while.bas

check: build
	$(SH) check.sh

dump: build
	MINI_BASIC_DUMP=1 $(DEMO) samples/nested.bas

clean:
	rm -rf $(GEN_DIR)
