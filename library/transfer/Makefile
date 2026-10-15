# transfer-one-thread：单线程后端工具链（构建 + 自检）
#
# 一个静态库 + 一个命令行（t1t）+ 六套模块自检 + 端到端流水线自检。
# 产物全部在仓库根目录 build/transfer-one-thread/，源码目录只放源码。
#
#   make            # 构建
#   make check      # 单元测试 + 各模块示例 + 端到端流水线
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -pthread
AR       ?= ar
ARFLAGS  ?= rcs

ifeq ($(findstring g++,$(CXX)),g++)
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
endif

ROOT_DIR  := ../..
BUILD_DIR ?= $(ROOT_DIR)/build/transfer-one-thread
LIB       := $(BUILD_DIR)/libtransfer-one-thread.a
TOOL      := $(BUILD_DIR)/t1t
INCLUDES  := -Icore -Iir -Iirbridge -Iirbuild -Idataflow -Issa -Iregalloc -Iapp

# Windows 用 PowerShell 创建嵌套目录，清理仍交给 cmd；其余平台用 POSIX 写法。
ifeq ($(OS),Windows_NT)
MKDIR = powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(subst /,\,$(1))' | Out-Null"
CLEAN = cmd /c if exist "$(subst /,\,$(1))" rmdir /s /q "$(subst /,\,$(1))"
else
MKDIR = mkdir -p "$(1)"
CLEAN = rm -rf "$(1)"
endif

# 库：五个模块的 C++ 实现
LIB_SOURCES := core/types.cpp core/transfer.cpp core/optimizer.cpp core/config.cpp core/hydra86.cpp core/llvm.cpp core/llvm_hydra.cpp \
               irbridge/irbridge.cpp dataflow/dataflow.cpp ssa/ssa.cpp regalloc/regalloc.cpp \
               ir/typed_ir.cpp irbuild/irbuild.cpp app/pipeline.cpp
LIB_OBJECTS := $(LIB_SOURCES:%.cpp=$(BUILD_DIR)/%.o)

# 命令行：总调度 + transfer / pipeline 两个新子命令 + 各模块原有入口
APP_SOURCES := app/main.cpp app/transfer_main.cpp \
               irbridge/main.cpp dataflow/main.cpp ssa/main.cpp regalloc/main.cpp irbuild/main.cpp
APP_OBJECTS := $(APP_SOURCES:%.cpp=$(BUILD_DIR)/%.o)

TESTS := $(BUILD_DIR)/t1t-tests-irbridge $(BUILD_DIR)/t1t-tests-dataflow \
         $(BUILD_DIR)/t1t-tests-irbuild \
         $(BUILD_DIR)/t1t-tests-ssa $(BUILD_DIR)/t1t-tests-regalloc \
         $(BUILD_DIR)/t1t-tests-hydra86 $(BUILD_DIR)/t1t-tests-llvm
CORE_EXAMPLES := $(BUILD_DIR)/t1t-example $(BUILD_DIR)/t1t-example-config \
                 $(BUILD_DIR)/t1t-example-operators $(BUILD_DIR)/t1t-example-suite
SUITES := $(BUILD_DIR)/t1t-dataflow-suite $(BUILD_DIR)/t1t-ssa-suite $(BUILD_DIR)/t1t-pipeline-suite

.PHONY: all tool tests examples check clean

all: tool tests examples
tool: $(TOOL)
tests: $(TESTS)
examples: $(CORE_EXAMPLES) $(SUITES)

$(LIB): $(LIB_OBJECTS) | $(BUILD_DIR)
	$(AR) $(ARFLAGS) $@ $^

# 头文件必须进依赖，否则改了 hpp 不重编 .o，跨 TU 的结构布局不一致会在 Windows 堆上报
# 0xC0000374（堆损坏）。新增模块时记得把它的 *.hpp 加进来。
HEADERS := $(wildcard ir/*.hpp) $(wildcard core/*.hpp) $(wildcard irbridge/*.hpp) \
           $(wildcard irbuild/*.hpp) \
           $(wildcard dataflow/*.hpp) \
           $(wildcard ssa/*.hpp) $(wildcard regalloc/*.hpp) $(wildcard app/*.hpp)

$(BUILD_DIR)/%.o: %.cpp $(HEADERS) | $(BUILD_DIR)
	@$(call MKDIR,$(@D))
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) -c $< -o $@

$(TOOL): $(APP_OBJECTS) $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(APP_OBJECTS) $(LIB) -o $@

$(BUILD_DIR)/t1t-tests-irbridge: irbridge/test_irbridge.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@
$(BUILD_DIR)/t1t-tests-dataflow: dataflow/test_dataflow.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@
$(BUILD_DIR)/t1t-tests-ssa: ssa/test_ssa.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@
$(BUILD_DIR)/t1t-tests-irbuild: irbuild/test_irbuild.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@
$(BUILD_DIR)/t1t-tests-regalloc: regalloc/test_regalloc.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@
$(BUILD_DIR)/t1t-tests-hydra86: core/hydra86_test.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@
$(BUILD_DIR)/t1t-tests-llvm: core/llvm_test.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@

$(BUILD_DIR)/t1t-example: core/example.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@
$(BUILD_DIR)/t1t-example-config: core/example_config.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@
$(BUILD_DIR)/t1t-example-operators: core/example_operators.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@
$(BUILD_DIR)/t1t-example-suite: core/example_suite.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@
$(BUILD_DIR)/t1t-dataflow-suite: dataflow/example_suite.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@
$(BUILD_DIR)/t1t-ssa-suite: ssa/example_suite.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@
$(BUILD_DIR)/t1t-pipeline-suite: examples/pipeline_suite.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $< $(LIB) -o $@

check: all
	$(BUILD_DIR)/t1t-tests-irbridge
	$(BUILD_DIR)/t1t-tests-dataflow
	$(BUILD_DIR)/t1t-tests-ssa
	$(BUILD_DIR)/t1t-tests-irbuild
	$(BUILD_DIR)/t1t-tests-regalloc
	$(BUILD_DIR)/t1t-tests-hydra86
	$(BUILD_DIR)/t1t-tests-llvm
	$(BUILD_DIR)/t1t-example core/example/minimal.quad
	$(BUILD_DIR)/t1t-example-config core/example/config.quad core/example_machine.ini
	$(BUILD_DIR)/t1t-example-operators core/example/operators.quad
	$(BUILD_DIR)/t1t-example-suite core/example mips
	$(BUILD_DIR)/t1t-dataflow-suite dataflow/examples
	$(BUILD_DIR)/t1t-ssa-suite ssa/examples
	$(BUILD_DIR)/t1t-pipeline-suite examples
	$(TOOL) regalloc --regs=4 regalloc/examples/pressure.ra
	$(TOOL) ssa --mode=summary ssa/examples/04_loop_swap.ssa
	$(TOOL) dataflow --analysis=live --mode=sets dataflow/examples/02_loop.df
	$(TOOL) irbridge stats irbridge/examples/if_while.quads
	$(TOOL) irbuild --kind=triple

$(BUILD_DIR):
	$(call MKDIR,$(BUILD_DIR))

clean:
	$(call CLEAN,$(BUILD_DIR))
