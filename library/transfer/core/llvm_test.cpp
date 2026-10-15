#include "llvm.hpp"
#include "llvm_hydra.hpp"
#include "hydra86.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        const transfer::Program arithmetic{
            {"decl", "i32", "global", "answer", ""},
            {":=", "2", "_", "x", ""},
            {"+", "x", "3", "sum", ""},
            {"*", "sum", "4", "answer", ""},
            {"write", "answer", "_", "_", ""},
            {"halt", "_", "_", "_", ""},
        };
        const std::string ir = transfer::generateLlvm(arithmetic);
        expect(ir.find("define i32 @main()") != std::string::npos, "missing LLVM main");
        expect(ir.find(" = add i64 ") != std::string::npos, "add was not lowered");
        expect(ir.find(" = mul i64 ") != std::string::npos, "multiply was not lowered");
        expect(ir.find("@printf") != std::string::npos, "write was not lowered");
        const std::string hydra = transfer::generateHydra86FromLlvm(ir);
        expect(hydra.find(".code") != std::string::npos, "LLVM to Hydra86 missed code");
        expect(hydra.find("add r8, r9") != std::string::npos,
               "LLVM to Hydra86 missed arithmetic");
        if (transfer::hydra86Embedded()) {
            const transfer::Hydra86Execution run = transfer::runHydra86(hydra);
            expect(run.ok, "LLVM to Hydra86 arithmetic failed: " + run.error);
            expect(run.output == "20\n", "LLVM to Hydra86 arithmetic output mismatch");
        }

        const transfer::Program recursion{
            {"decl", "i32", "global", "answer", ""},
            {"param", "5", "_", "k", ""},
            {"call", "fact", "_", "answer", ""},
            {"write", "answer", "_", "_", ""},
            {"halt", "_", "_", "_", ""},
            {"label", "_", "_", "fact", ""},
            {"decl", "i32", "local", "k", ""},
            {"<=", "k", "1", "isBase", ""},
            {"jz", "isBase", "_", "recurse", ""},
            {"ret", "1", "_", "_", ""},
            {"label", "_", "_", "recurse", ""},
            {"-", "k", "1", "next", ""},
            {"param", "next", "_", "k", ""},
            {"call", "fact", "_", "nested", ""},
            {"*", "k", "nested", "value", ""},
            {"ret", "value", "_", "_", ""},
        };
        const std::string recursiveIr = transfer::generateLlvm(recursion);
        expect(recursiveIr.find("call i64 @t1t_fn_0()") != std::string::npos,
               "recursive call was not lowered");
        expect(recursiveIr.find("br i1") != std::string::npos,
               "conditional branch was not lowered");
        const std::string recursiveHydra = transfer::generateHydra86FromLlvm(recursiveIr);
        if (transfer::hydra86Embedded()) {
            const transfer::Hydra86Execution run = transfer::runHydra86(recursiveHydra);
            expect(run.ok, "LLVM to Hydra86 recursion failed: " + run.error);
            expect(run.output == "120\n", "LLVM to Hydra86 recursion output mismatch");
        }

        bool rejectedForeignIr = false;
        try {
            transfer::generateHydra86FromLlvm("define i32 @main() { ret i32 0 }");
        } catch (const std::runtime_error&) {
            rejectedForeignIr = true;
        }
        expect(rejectedForeignIr, "foreign LLVM IR without a t1t payload was accepted");

        bool rejectedModifiedIr = false;
        try {
            std::string modified = ir;
            modified.replace(modified.find("add i64"), 3, "sub");
            transfer::generateHydra86FromLlvm(modified);
        } catch (const std::runtime_error&) {
            rejectedModifiedIr = true;
        }
        expect(rejectedModifiedIr, "modified LLVM IR with stale t1t payload was accepted");

        bool rejectedFloat = false;
        try {
            transfer::generateLlvm({{"decl", "f64", "global", "x", ""},
                                    {"halt", "_", "_", "_", ""}});
        } catch (const std::runtime_error&) {
            rejectedFloat = true;
        }
        expect(rejectedFloat, "unsupported floating-point input was not rejected");

        std::cout << "LLVM backend: generation ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LLVM backend test failed: " << error.what() << "\n";
        return 1;
    }
}
