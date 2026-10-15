#include "parser.hpp"
#include <cassert>
#include <iostream>
#include "glr_forest_check.hpp"

int actionCalls = 0;
int semanticBias = 0;
void recordAction() { ++actionCalls; }

int main()
{
    using namespace Parser;
    Engine engine;
    GlrIncrementalCache cache;
    auto run = [&](const char *text = "2 + 3 * 4") {
        assert(engine.runGlrIncremental(text, cache) == Status::Ok);
    };
    run();
    const int first = actionCalls;
    assert(first > 0 && !cache.reusedSemanticResults && cache.results.empty());
    run();
    assert(actionCalls == 2 * first && !cache.reusedSemanticResults);

    // Instrumentation deliberately counts actions to verify the Pure opt-in.
    cache.semanticReuse = GlrSemanticReusePolicy::Pure;
    run();
    assert(actionCalls == 3 * first && !cache.reusedSemanticResults);
    const auto results = cache.results;
    run();
    assert(actionCalls == 3 * first && cache.reusedSemanticResults);
    assert(cache.results == results && engine.glrResultCount() == results.size());
    for (std::size_t i = 0; i < results.size(); ++i)
        assert(*engine.glrResult(i) == results[i]);

    semanticBias = 1;
    ++cache.semanticRevision;
    run();
    assert(actionCalls == 4 * first && !cache.reusedSemanticResults);
    assert(cache.results != results);
    run();
    assert(actionCalls == 4 * first && cache.reusedSemanticResults);

    int resolverCalls = 0;
    engine.setAmbiguityResolver([&](const auto &, const auto &) {
        ++resolverCalls;
        return true;
    });
    run();
    const int callsWithResolver = actionCalls;
    const int resolved = resolverCalls;
    assert(resolved > 0 && !cache.reusedSemanticResults && cache.results.empty());
    run();
    assert(actionCalls > callsWithResolver && resolverCalls > resolved);
    engine.setAmbiguityResolver({});
    run();
    assert(!cache.reusedSemanticResults);
    run();
    assert(cache.reusedSemanticResults);

    run("2 + 3 + 4");
    assert(!cache.reusedSemanticResults && cache.tokenChange.changed);
    assert(!cache.invalidatedReduceEntries.empty());
    run("2 + 3 + 4");
    assert(cache.reusedSemanticResults && !cache.tokenChange.changed);
    assert(cache.invalidatedReduceEntries.empty());

    cache.semanticReuse = GlrSemanticReusePolicy::Disabled;
    const int beforeDisable = actionCalls;
    run();
    assert(actionCalls > beforeDisable && !cache.reusedSemanticResults);
    cache.semanticReuse = GlrSemanticReusePolicy::Pure;
    run();
    assert(!cache.reusedSemanticResults);
    cache.clear();
    assert(cache.semanticReuse == GlrSemanticReusePolicy::Pure);
    assert(cache.semanticRevision == 1 && !cache.semanticResultsReusable);
    run();
    assert(!cache.reusedSemanticResults);

    for (int i = 0; i < 2; ++i) {
        assert(engine.runGlrIncremental("@", cache) != Status::Ok);
        assert(!cache.reusedSemanticResults && !cache.semanticResultsReusable);
    }
    run();
    assert(!cache.reusedSemanticResults);
    checkForestRepair();
    std::cout << "GLR semantic reuse and forest repair checks passed\n";
}
