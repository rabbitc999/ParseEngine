# Build all three DSL compilers with ParseEngine, then compile and test their codecs.

ENGINE ?= ../../build/bin/ParseEngine
PYTHON ?= python
CARGO ?= cargo
CARGO_FLAGS ?=
GRAMMAR := protocol.gram
BUILD_DIR := build
GENERATED_DIR := $(BUILD_DIR)/generated
CPP_PARSER_DIR := $(BUILD_DIR)/native-cpp-parser
RUST_PARSER_DIR := $(BUILD_DIR)/native-rust-parser
CPP_NATIVE := $(BUILD_DIR)/protocol-dsl-cpp$(if $(filter Windows_NT,$(OS)),.exe,)
RUST_BUILT := $(RUST_PARSER_DIR)/target/debug/parser_demo$(if $(filter Windows_NT,$(OS)),.exe,)
RUST_NATIVE := $(BUILD_DIR)/protocol-dsl-rust$(if $(filter Windows_NT,$(OS)),.exe,)

ifeq ($(OS),Windows_NT)
BUILD_DIR_WIN := $(subst /,\,$(BUILD_DIR))
CLEAN := cmd /c if exist "$(BUILD_DIR_WIN)" rmdir /s /q "$(BUILD_DIR_WIN)"
else
CLEAN := rm -rf "$(BUILD_DIR)"
endif

.PHONY: all parser generate native native-cpp native-rust run check clean

all: generate

parser:
	$(ENGINE) --grammar-file $(GRAMMAR) --out-dir $(BUILD_DIR) --lang python

generate: parser
	$(PYTHON) $(BUILD_DIR)/main.py examples/telemetry.pdl --out-dir $(GENERATED_DIR) --target all

native: native-cpp native-rust

native-cpp:
	$(ENGINE) --grammar-file protocol_syntax.gram --out-dir $(CPP_PARSER_DIR) --lang cpp17
	g++ -std=c++17 -Wall -Wextra -Werror -Wno-switch -I$(CPP_PARSER_DIR) native/cpp/main.cpp $(CPP_PARSER_DIR)/parser.cpp -o $(CPP_NATIVE)

native-rust:
	$(ENGINE) --grammar-file protocol_syntax.gram --out-dir $(RUST_PARSER_DIR) --lang rust
	$(PYTHON) -c "import shutil; shutil.copy2('native/rust/main.rs', '$(RUST_PARSER_DIR)/src/main.rs')"
	$(CARGO) build $(CARGO_FLAGS) --manifest-path $(RUST_PARSER_DIR)/Cargo.toml
	$(PYTHON) -c "import shutil; shutil.copy2('$(RUST_BUILT)', '$(RUST_NATIVE)')"

run: generate
	$(PYTHON) -c "import sys; sys.path.insert(0, '$(GENERATED_DIR)'); import telemetry_protocol as p; x=p.Header(7,p.MessageKind.heartbeat,0); print(x.encode().hex())"

check: generate native
	$(PYTHON) tests/check.py

clean:
	$(CLEAN)
