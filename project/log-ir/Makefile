# project/log-ir/Makefile —— 生成 + 自验（结构化日志 → JSON，Python 产物）
#
#   make            生成 Python 产物到 build/（没有编译这一步）
#   make run        跑 samples/app.log
#   make check      逐个样例跑：正常样例比 stdout，错误样例比报错首行
#   make clean      删掉 build/
#
# 目标语言是 Python（--lang python）：产物只有 parser.py + main.py，
# 用 python3 直接跑，不需要编译器，也不依赖第三方库。
# ENGINE 指向仓库里已经构建好的生成器（默认 ../../build/bin/ParseEngine）。

ENGINE  ?= ../../build/bin/ParseEngine
GRAMMAR := log.gram
GEN_DIR := build
PYTHON  ?= python

# Windows 上 make 找不到 sh：优先用 Git Bash / MSYS2 的 bash 跑 check.sh
ifeq ($(OS),Windows_NT)
  SH ?= $(firstword $(wildcard C:/PROGRA~1/Git/bin/bash.exe) \
                    $(wildcard C:/PROGRA~2/Git/bin/bash.exe) \
                    $(wildcard C:/msys64/usr/bin/bash.exe) sh)
else
  SH ?= sh
endif

.PHONY: all generate run check clean

all: generate

generate:
	$(ENGINE) --grammar-file $(GRAMMAR) --out-dir $(GEN_DIR) --lang python

run: generate
	$(PYTHON) $(GEN_DIR)/main.py samples/app.log

check: generate
	$(SH) check.sh

clean:
	rm -rf $(GEN_DIR)
