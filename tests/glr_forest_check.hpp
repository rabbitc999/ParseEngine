#include "parser.hpp"
#include <algorithm>
#include <cassert>
#include <map>
#include <set>
#include <string>
#include <vector>

inline std::vector<std::string> forestSignature(const Parser::GlrIncrementalCache &cache)
{
    std::map<std::uint64_t, const Parser::GlrForestNode *> nodes;
    for (const auto &node : cache.forest) assert(nodes.emplace(node.id, &node).second);
    auto key = [&](std::uint64_t id) {
        const auto &node = *nodes.at(id);
        return std::to_string(node.symbol) + ":" + std::to_string(node.startToken) +
               ":" + std::to_string(node.endToken);
    };
    std::vector<std::string> signature;
    std::set<std::uint64_t> visited;
    auto work = cache.forestRoots;
    for (std::size_t i = 0; i < work.size(); ++i) {
        const auto id = work[i];
        if (!visited.insert(id).second) continue;
        const auto &node = *nodes.at(id);
        assert(node.startToken <= node.endToken && node.endToken <= cache.tokens.size());
        assert(node.terminal || !node.alternatives.empty());
        signature.push_back("node:" + key(id) + ":" + node.text);
        for (const auto &alternative : node.alternatives) {
            std::string row = key(id) + "=" + std::to_string(alternative.production);
            for (auto child : alternative.children) {
                const auto &parents = nodes.at(child)->parents;
                assert(std::find(parents.begin(), parents.end(), id) != parents.end());
                row += "/" + key(child);
                work.push_back(child);
            }
            signature.push_back(row);
        }
        for (auto parent : node.parents) {
            bool found = false;
            for (const auto &alternative : nodes.at(parent)->alternatives)
                found |= std::find(alternative.children.begin(), alternative.children.end(), id)
                    != alternative.children.end();
            assert(found);
        }
    }
    assert(visited.size() == nodes.size());
    for (auto id : cache.forestRoots) signature.push_back("root:" + key(id));
    std::sort(signature.begin(), signature.end());
    return signature;
}

inline void checkForestEdits(const std::vector<std::string> &inputs)
{
    Parser::Engine engine;
    Parser::GlrIncrementalCache cache;
    for (const auto &input : inputs) {
        const auto old = cache;
        const auto status = engine.runGlrTreeIncremental(input, cache);
        Parser::Engine fresh;
        Parser::GlrIncrementalCache baseline;
        assert(fresh.runGlrTreeIncremental(input, baseline) == status);
        assert(forestSignature(cache) == forestSignature(baseline));
        assert(engine.errors().size() == fresh.errors().size());
        for (std::size_t i = 0; i < engine.errors().size(); ++i)
            assert(engine.errors()[i].toString() == fresh.errors()[i].toString());
        assert(engine.glrResultCount() == fresh.glrResultCount());
        for (std::size_t i = 0; i < engine.glrResultCount(); ++i)
            assert(engine.glrTree(i)->toString() == fresh.glrTree(i)->toString());
        std::set<std::uint64_t> invalid(cache.invalidatedForestNodes.begin(), cache.invalidatedForestNodes.end());
        for (const auto &node : old.forest)
            if (invalid.count(node.id))
                for (auto parent : node.parents) assert(invalid.count(parent));
        for (const auto &node : cache.forest) assert(!invalid.count(node.id));
        assert(cache.forest.size() == cache.reusedForestNodeCount + cache.rebuiltForestNodeCount);
    }
}

inline void checkForestRepair()
{
    using namespace Parser;
    Engine engine;
    GlrIncrementalCache cache;
    assert(engine.runGlrTreeIncremental("2 + 3 * 4", cache) == Status::Ok);
    assert(cache.forestRoots.size() >= 2);
    const auto root = cache.forestRoots.front();
    for (const auto &node : cache.forest) if (node.id == root) assert(node.alternatives.size() == 2);
    const auto findToken = [](const GlrIncrementalCache &c, const std::string &text) {
        for (const auto &node : c.forest) if (node.terminal && node.text == text) return node.id;
        assert(false); return std::uint64_t(0);
    };
    const auto suffix = findToken(cache, "4");
    assert(engine.runGlrTreeIncremental("2 + 8 * 4", cache) == Status::Ok);
    assert(std::find(cache.invalidatedForestNodes.begin(), cache.invalidatedForestNodes.end(), root)
           != cache.invalidatedForestNodes.end());
    assert(cache.reusedForestNodeCount > 0 && cache.rebuiltForestNodeCount > 0);
    assert(findToken(cache, "4") == suffix);
    assert(engine.runGlrTreeIncremental("0 + 2 + 8 * 4", cache) == Status::Ok);
    assert(findToken(cache, "4") == suffix);
    for (const auto &node : cache.forest) if (node.id == suffix) assert(node.startToken == 6);
    checkForestEdits({"2 + 3 * 4", "2 + 8 * 4", "0 + 2 + 8 * 4", "2 + 8 * 4",
                      " 2 +\n8 * 4", "2 * 8 + 4", "2 + + 4", "@", "", "2 + 3"});
}
