# project/pl0-super/Makefile —— 生成 + 编译 + 自验（PL/0 超集 → 目标机汇编）
#   make            构建 build/pl0-super（库里的前端 + 简化 + 目标机）
#   make grammar    试着用 ParseEngine 生成文法前端（pl0-super.gram，见 README 的状态）
#   make run        跑 samples/array_sum.p0s 看输出
#   make stats      跑 samples/array_sum.p0s 并打印各趟简化次数
#   make asm        打印 samples/struct_ref.p0s 的目标机汇编
#   make check      逐个样例跑：正常样例比输出，错误样例比报错
#   make clean      删掉 build/
#
# 两份前端并存：
#   生成的那份（build/，来自 pl0-super.gram）——工程约定用的、负责语言定义与语法检查；
#   库里的手写前端（pl0super.cpp 里的词法 + 递归下降）——语义那一半（建树/简化/出汇编）。
# 所以本工程既能 `make check` 真跑，也能把文法单独拿去改。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

ENGINE  ?= ../../build/bin/ParseEngine
# 前端分两目录：lalr1/ 是 LALR(1) 的文法（本 Makefile 走它），ll1/ 是 LL(1) 的手写前端
GRAMMAR := lalr1/pl0-super.gram
GEN_DIR := build
TOOL    := $(GEN_DIR)/pl0-super
DEMO    := $(TOOL)

ifeq ($(findstring g++,$(CXX)),g++)
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
endif

ifeq ($(OS),Windows_NT)
SH ?= $(firstword $(wildcard C:/PROGRA~1/Git/bin/bash.exe) \
                  $(wildcard C:/PROGRA~2/Git/bin/bash.exe) \
                  $(wildcard C:/msys64/usr/bin/bash.exe) sh)
else
SH ?= sh
endif

SOURCES := ll1/lexer.cpp ll1/parser.cpp simplify.cpp codegen.cpp vm.cpp
OBJECTS := $(SOURCES:%.cpp=$(GEN_DIR)/%.o)
HEADERS := $(wildcard *.hpp)

# 生成出来的前端（make grammar 产出）：解析器 + %main 驱动
GEN_SOURCES := $(GEN_DIR)/pl0sgram_parser.cpp $(GEN_DIR)/pl0sgram_main.cpp
GEN_OBJECTS := $(GEN_SOURCES:.cpp=.o)

ifeq ($(OS),Windows_NT)
MKDIR = cmd /c if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
CLEAN = cmd /c if exist "$(subst /,\,$(1))" rmdir /s /q "$(subst /,\,$(1))"
else
MKDIR = mkdir -p "$(1)"
CLEAN = rm -rf "$(1)"
endif

.PHONY: all grammar tool lib demo run stats asm check clean

all: demo

# 文法前端由 grammar 生成；显式登记依赖，让 make 在检查对象隐式规则前先看到源码。
$(GEN_SOURCES): grammar

grammar:
	$(ENGINE) --grammar-file $(GRAMMAR) --out-dir $(GEN_DIR) --lang cpp17

$(GEN_DIR)/%.o: %.cpp $(HEADERS) | $(GEN_DIR)
	@$(call MKDIR,$(dir $@))
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. -I$(GEN_DIR) -c $< -o $@

lib: $(OBJECTS)

tool: $(TOOL)

$(GEN_DIR)/pl0sgram_%.o: $(GEN_DIR)/pl0sgram_%.cpp | $(GEN_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I$(GEN_DIR) -I. -c $< -o $@

# 最终可执行 = **生成出来的前端**（文法 + %main）+ 库（简化 / 代码生成 / 目标机）
$(TOOL): grammar $(OBJECTS) $(GEN_OBJECTS)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(OBJECTS) $(GEN_OBJECTS) -o $@

$(GEN_DIR):
	@$(call MKDIR,$(GEN_DIR))

demo: tool

run: demo
	$(DEMO) samples/array_sum.p0s

stats: demo
	$(DEMO) --stats samples/array_sum.p0s

asm: demo
	$(DEMO) --asm samples/struct_ref.p0s

check: tool
	$(SH) check.sh
	python tools/gen_diagrams.py --check

clean:
	@$(call CLEAN,$(GEN_DIR))
