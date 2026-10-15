# ParseEngine 的简易构建（不想用 CMake 时直接 make）
#
#   make -C source            编译出 build/bin/ParseEngine（Release 级：-O3 -DNDEBUG，
#                             链接期裁剪并 strip；Windows 下带 .exe）
#   make -C source release    同一套 Release 级标志，产物另放 build/bin/release/
#   make -C source templates  重新内联 source/templates/ 下的模板（需要 python3）
#   make -C source test-smoke 冒烟测试：遍历 example/ 生成 + 编译 + 喂输入 + 比对
#   make -C source test-repro 同文法生成两次，产物与输出逐字一致
#   make -C source clean      删除 make 的中间文件与产物
#   make -C source help       打印这份列表
#
# 与 CMake 的关系：两条路径都能构建。CMake 把整个 build/ 当自己的目录（多配置
# 生成器再分一层 build/<配置>/），make 用 build/make/ 放中间 .o、build/bin/ 放
# 产物，两边互不干扰。
#
# 冒烟自检由 ../tools/smoke.sh 承担（POSIX shell）：make -C source test-smoke 按
# example/manifest.tsv 逐份文法生成 + 编译 + 喂固定输入 + 比对期望输出；
# make -C source test-repro 把同一份文法生成两次，产物与输出逐字一致。
# Windows 上装了 Git Bash / MSYS2 时，整条 make 都交给它当 shell（mkdir / rm 两边
# 通吃）；一个都没装时退回 cmd，编译照常。

CXX      ?= g++
# BUILD 只决定**产物落盘位置**（default → build/bin/，release → build/bin/release/），
# 两个挡位共用同一套 Release 级编译标志（见下面 CXXFLAGS）。
BUILD    ?= default
ifeq ($(filter $(BUILD),default release),)
  $(error Unsupported BUILD '$(BUILD)'; use default or release)
endif
# Release 级标志：本仓库默认就要**小体积的产物**，所以常规构建也走 -O3 -DNDEBUG，
# 并把每个函数 / 数据段单独成节，好让链接器裁剪（配合下面的 --gc-sections）。
# 需要不带 NDEBUG 的对照构建时显式覆盖：make CXXFLAGS="-std=c++17 -O2 -Wall -Wextra"。
CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG -Wall -Wextra -ffunction-sections -fdata-sections
# --gc-sections 丢掉没被引用的节，-s 去掉符号表；这两项是体积下降的主要来源。
LDFLAGS  ?= -Wl,--gc-sections -s
# -Ilibrary：library/ 下是独立的小库（模板引擎 library/template 等），
# 生成器本体不依赖它们，留着这个搜索路径方便直接用。
# 产物模板放在 source/templates/，构建期内联，运行时不读盘。
INCLUDES := -Iinclude -I../library

# 源码是 UTF-8（含中文注释与提示）：把输入/执行字符集钉死为 UTF-8，中文才不会
# 在非 UTF-8 环境（如 Windows 简体中文代码页 GBK/936）下被读坏。换成 clang 等
# 不认这两个开关的编译器时，可以用 `make CXXFLAGS_CHARSET=` 覆盖掉。
CXXFLAGS_CHARSET ?= -finput-charset=UTF-8 -fexec-charset=UTF-8
CXXFLAGS += $(CXXFLAGS_CHARSET)

# 头文件依赖：改 source/include/ 下的头文件后无需手工 make clean
CXXFLAGS += -MMD -MP

# 平台差异集中在这个分支里：可执行文件后缀、静态链接选项、Python 名字、清理命令，
# 以及 Windows 上要用的 POSIX shell（POSIX_SH 可以用 `make POSIX_SH=...` 覆盖）
ifeq ($(OS),Windows_NT)
  EXE       := .exe
  LDFLAGS   += -static -static-libgcc -static-libstdc++
  PYTHON    ?= python
  POSIX_SH  ?= $(firstword $(wildcard C:/PROGRA~1/Git/bin/bash.exe) \
                          $(wildcard C:/PROGRA~2/Git/bin/bash.exe) \
                          $(wildcard C:/msys64/usr/bin/bash.exe))
  ifneq ($(POSIX_SH),)
    SHELL     := $(POSIX_SH)
    # 顺带清掉旧版就地构建留下的东西：根目录的 ParseEngine.exe 与 source/src/*.o、source/src/*.d
    CLEAN_CMD := rm -rf ../build/make ../build/bin/ParseEngine.exe ../build/bin/release/ParseEngine.exe src/*.o src/*.d ../ParseEngine.exe
  else
    CLEAN_CMD := if exist "..\build\make" rmdir /s /q "..\build\make" & if exist "..\build\bin\ParseEngine.exe" del /q "..\build\bin\ParseEngine.exe" & if exist "..\build\bin\release\ParseEngine.exe" del /q "..\build\bin\release\ParseEngine.exe" & if exist "src\*.o" del /q "src\*.o" & if exist "src\*.d" del /q "src\*.d" & if exist "..\ParseEngine.exe" del /q "..\ParseEngine.exe"
  endif
else
  EXE       :=
  LDFLAGS   += -static-libgcc -static-libstdc++
  PYTHON    ?= python3
  POSIX_SH  ?=
  CLEAN_CMD := rm -rf ../build/make ../build/bin/ParseEngine ../build/bin/release/ParseEngine src/*.o src/*.d ../ParseEngine
endif

# 冒烟测试走 POSIX shell（../tools/smoke.sh）；Windows 上没找到 Git Bash / MSYS2
# 时给一句人话，别让 make 抛 shell 语法错误。
ifeq ($(OS),Windows_NT)
  ifeq ($(POSIX_SH),)
    SMOKE_CMD = @echo 需要 Git Bash / MSYS2 才能跑 make -C source test-$(1)（tools/smoke.sh 是 POSIX shell 脚本）& exit /b 2
  else
    SMOKE_CMD = cd .. && "$(POSIX_SH)" tools/smoke.sh $(1) "build/bin/ParseEngine$(EXE)"
  endif
else
  SMOKE_CMD = cd .. && sh tools/smoke.sh $(1) "build/bin/ParseEngine$(EXE)"
endif

TARGET   := ../build/bin/ParseEngine$(EXE)
OBJDIR   := ../build/make
ifeq ($(BUILD),release)
  TARGET := ../build/bin/release/ParseEngine$(EXE)
  OBJDIR := ../build/make/release
endif
SOURCES  := $(wildcard src/*.cpp)
OBJECTS  := $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(SOURCES))
# library/lztext 不在 src/ 下：内嵌模板靠它压缩存储、运行时解压（见 templates_data.cpp）。
# 单独列出来，否则会被下面的通配规则当成不存在的 src/lztext.cpp。
OBJECTS  += $(OBJDIR)/lztext.o
DEPS     := $(OBJECTS:.o=.d)

# 中间目录跟着源码树的形状走；用戳文件一次性建好：拿目录当目标会因目录时间戳变化反复触发
OBJDIRS  := $(sort $(dir $(OBJECTS)) $(dir $(TARGET)))
STAMP    := $(OBJDIR)/.stamp

ifeq ($(OS),Windows_NT)
  ifeq ($(POSIX_SH),)
    # cmd 的 & 严格顺序执行；已存在的目录会报错，正行用 - 让 make 忽略
    MKDIRS := $(foreach d,$(OBJDIRS),mkdir "$(subst /,\,$(d))" 2>NUL &)
  else
    MKDIRS := $(foreach d,$(OBJDIRS),mkdir -p "$(d)" 2>/dev/null;)
  endif
else
  MKDIRS := $(foreach d,$(OBJDIRS),mkdir -p "$(d)" 2>/dev/null;)
endif

all: $(TARGET)

# 消息与发布脚本在构建期内嵌，运行时不读取仓库文件。
include/ParseEngine/i18n_messages.hpp: i18n/messages.tsv ../tools/embed_parseengine_i18n.py ../tools/embed_i18n.py
	$(PYTHON) ../tools/embed_parseengine_i18n.py
include/ParseEngine/package_tool.hpp: ../tools/package_compiler.py ../tools/embed_package_tool.py
	$(PYTHON) ../tools/embed_package_tool.py
include/ParseEngine/tool_console.hpp: ../library/console/utf8.hpp ../tools/sync_tool_console.py
	$(PYTHON) ../tools/sync_tool_console.py
$(OBJDIR)/main.o $(OBJDIR)/package.o: include/ParseEngine/i18n_messages.hpp include/ParseEngine/package_tool.hpp

# ---- 共享工作区提示（见 AGENTS.md §2「并发与编译锁」）----
# `make -C source templates` 会把 source/templates/ 下**所有人**的模板一起内联（.tmpl 优先
# 于同名 .template），`source/src/templates_data.cpp` 与 build/bin/ParseEngine.exe 又是
# 全仓共享的生成物。所以裸跑 make 时若 tmp/build.lock 在，就提醒改用 tools/lock.sh
# build —— 它拿锁、编完自动放锁、编译失败时保留锁。
# 用 make 自己的 $(info)（不经过 shell），各平台都安全。
ifneq (,$(wildcard ../tmp/build.lock))
ifneq ($(PE_LOCK_HELD),1)
$(info 警告：tmp/build.lock 存在——有别人正在编译或改生成器（不该同时编译）。)
$(info       本次请改用：tools/lock.sh build   /   或 LOCK_REUSE=1 tools/lock.sh build)
endif
endif

release:
	$(MAKE) BUILD=release all

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 显式规则优先于上面的通配规则（否则会被解释成 src/lztext.cpp）。
$(OBJDIR)/lztext.o: ../library/lztext/lztext.cpp ../library/lztext/lztext.hpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(STAMP):
	-@$(MKDIRS) echo done
	@echo ok > $(STAMP)

$(OBJECTS) $(TARGET): | $(STAMP)

templates:
	$(PYTHON) ../tools/embed_templates.py

# 一键构建：生成器 + 所有库 + 所有工程（只编译，不跑测试；缺工具链的部分自动跳过）
build-all:
	cd .. && sh build-all.sh

# 冒烟测试：按 example/manifest.tsv 遍历示例文法，逐语言生成 + 编译 + 喂固定
# 输入 + 与 example/<name>.expected 比对；缺工具链的语言档跳过并打印一句。
test-smoke: $(TARGET)
	$(call SMOKE_CMD,smoke)

# 可复现性：同一份文法生成到两个目录，喂同一输入，产物与输出逐字一致。
test-repro: $(TARGET)
	$(call SMOKE_CMD,repro)

clean:
	-$(CLEAN_CMD)

help:
	$(info make -C source            编译 build/bin/ParseEngine$(EXE)（Release 级：-O3 -DNDEBUG，裁剪并 strip）)
	$(info make -C source release    同一套标志，产物另放 build/bin/release/ParseEngine$(EXE))
	$(info make -C source templates  重新内联模板（$(PYTHON) ../tools/embed_templates.py）)
	$(info make -C source build-all  一键构建：生成器 + 库 + 工程（cd .. && sh build-all.sh，不跑测试）)
	$(info make -C source test-smoke 冒烟测试：遍历 example/ 生成 + 编译 + 喂输入 + 比对)
	$(info make -C source test-repro 同文法生成两次，产物与输出逐字一致)
	$(info make -C source clean      删除 make 的中间文件与产物)
	$(info 没装 Git Bash / MSYS2 时测试目标会提示先装一个)

.PHONY: all release templates test-smoke test-repro clean help

# 自动依赖放在文件末尾，避免它成为默认目标
-include $(DEPS)
