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
        const transfer::Program program{
            {":=", "2", "_", "x", ""},
            {"+", "x", "3", "sum", ""},
            {"*", "sum", "4", "product", ""},
            {"write", "product", "_", "_", ""},
            {"halt", "_", "_", "_", ""},
        };
        const std::string assembly = transfer::generateHydra86(program);
        expect(assembly.find(".data\n") != std::string::npos, "missing Hydra86 data section");
        expect(assembly.find(".code\n_start:\n") != std::string::npos, "missing Hydra86 entry");
        expect(assembly.find("mul r8, r9") != std::string::npos, "multiply was not lowered");
        expect(assembly.find("call __t1t_print_i64") != std::string::npos,
               "write was not lowered");

        const transfer::Hydra86Execution run = transfer::runHydra86(assembly);
        if (run.available) {
            expect(run.ok, "Hydra86 execution failed: " + run.error);
            expect(run.output == "20\n", "Hydra86 output mismatch: " + run.output);

            const transfer::Hydra86Binary binary = transfer::compileHydra86(assembly);
            expect(binary.available && binary.ok,
                   "Hydra86 binary generation failed: " + binary.error);
            expect(binary.bytes.size() > 8, "Hydra86 binary is unexpectedly short");
            const std::uint32_t version = binary.bytes.size() >= 8
                                              ? static_cast<std::uint32_t>(binary.bytes[4]) |
                                                    (static_cast<std::uint32_t>(binary.bytes[5]) << 8) |
                                                    (static_cast<std::uint32_t>(binary.bytes[6]) << 16) |
                                                    (static_cast<std::uint32_t>(binary.bytes[7]) << 24)
                                              : 0;
            expect(version == 2, "Hydra86 binary is not the v2 variable-length format");
            const transfer::Hydra86Execution binaryRun =
                transfer::runHydra86Binary(binary.bytes);
            expect(binaryRun.ok, "Hydra86 binary execution failed: " + binaryRun.error);
            expect(binaryRun.output == "20\n",
                   "Hydra86 binary output mismatch: " + binaryRun.output);

            const transfer::Program controlFlow{
                {"call", "main", "_", "_", ""},
                {"halt", "_", "_", "_", ""},
                {"label", "_", "_", "main", ""},
                {":=", "0", "_", "i", ""},
                {":=", "0", "_", "sum", ""},
                {"label", "_", "_", "loop", ""},
                {"<", "i", "5", "again", ""},
                {"jz", "again", "_", "done", ""},
                {"+", "sum", "i", "sum", ""},
                {"+", "i", "1", "i", ""},
                {"j", "_", "_", "loop", ""},
                {"label", "_", "_", "done", ""},
                {"write", "sum", "_", "_", ""},
                {"ret", "0", "_", "_", ""},
            };
            const transfer::Hydra86Execution branchRun =
                transfer::runHydra86(transfer::generateHydra86(controlFlow));
            expect(branchRun.ok, "Hydra86 control-flow execution failed: " + branchRun.error);
            expect(branchRun.output == "10\n", "Hydra86 control-flow mismatch: " + branchRun.output);

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
            const std::string recursionAssembly = transfer::generateHydra86(recursion);
            expect(recursionAssembly.find("; local") == std::string::npos,
                   "declaration scope leaked into Hydra86 variables");
            const transfer::Hydra86Execution recursionRun = transfer::runHydra86(recursionAssembly);
            expect(recursionRun.ok, "Hydra86 recursion execution failed: " + recursionRun.error);
            expect(recursionRun.output == "120\n", "Hydra86 recursion mismatch: " + recursionRun.output);

            const transfer::Program signedDivision{
                {"/", "-7", "2", "quotient", ""},
                {"%", "-7", "2", "remainder", ""},
                {"write", "quotient", "_", "_", ""},
                {"write", "remainder", "_", "_", ""},
                {"halt", "_", "_", "_", ""},
            };
            const transfer::Hydra86Execution divisionRun =
                transfer::runHydra86(transfer::generateHydra86(signedDivision));
            expect(divisionRun.ok, "Hydra86 signed division failed: " + divisionRun.error);
            expect(divisionRun.output == "-3\n-1\n",
                   "Hydra86 signed division mismatch: " + divisionRun.output);
        }
        std::cout << "Hydra86 backend: generation ok, embedded="
                  << (run.available ? "yes" : "no") << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Hydra86 backend test failed: " << error.what() << "\n";
        return 1;
    }
}
