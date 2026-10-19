/** @file generation_cache.cpp
 * @brief 二进制缓存字段、SHA-256、损坏恢复与动作刷新回归。
 */
#include "ParseEngine/generation_cache.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
using namespace ParseEngine;

/** @brief 验证所有缓存字段和摘要。@param argc 参数数。@param argv 缓存目录。
 * @return 全部断言通过时返回零。
 */
int main(int argc,char **argv)
{
    assert(argc==2);
    assert(GenerationCache::digest("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(GenerationCache::digest("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(GenerationCache::digest(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    Grammar grammar;grammar.start="input";
    Production p;p.rhs={"A"};p.action="$$ = 1;";p.lhs="input";p.actionLine=3;p.sourceOffset=10;
    grammar.rules["input"]={p};
    AnalysisResult result;
    result.mode="ielr";result.grammar=grammar;result.terminals={"A","END"};result.nonterms={"input"};
    result.first["input"]={"A"};result.follow["input"]={"END"};result.hasFollow=false;
    result.lr1StateCount=7;result.productions={p};result.slrStates={{{{"input",0,0}}}};
    LalrState state;state.items[{"input",0,1}]={"END"};result.lalrStates={state};
    result.transitions[{0,"A"}]=1;result.action[{0,"A"}]={LrAction::Kind::Shift,1};
    result.lookaheadK=2;result.lookaheadColumns={{"A","END"}};result.lookaheadColumnIndex["key"]=0;
    result.firstK["input"]={"key"};result.followK["input"]={"end"};result.gotoTable[{0,0}]=1;
    result.table[{"input","A"}]=0;result.unresolved={{"kind",1,"A","text"}};
    result.resolutions={{1,"A","kind","reason","chosen","discarded"}};result.warnings={"warning"};
    result.priority.precedence["A"]=2;result.priority.associativity["A"]="left";
    result.priority.productionPrec[{"input",0}]="A";result.priority.expectConflicts=0;
    result.ielrLalrStateCount=3;result.ielrSplits={{1,"A","kind","reason",{1,2}}};
    result.glrAlternatives[{0,"A"}]={{LrAction::Kind::Shift,1},{LrAction::Kind::Reduce,0}};
    result.ddLeftRecursionCycle={"input"};result.lr1MergedStateCount=4;result.teach=true;
    std::string reason,report;
    const auto key=GenerationCache::key(grammar,{"A"},result.priority,"ielr",1,true,"wide","build");
    assert(GenerationCache::save(argv[1],key,result,"report",reason));
    AnalysisResult restored;
    assert(GenerationCache::load(argv[1],key,grammar,restored,report,reason));
    assert(report=="report");
    // 同一份对象再序列化必须逐字节相同，涵盖没有逐个断言的字段。
    const auto original=std::filesystem::path(argv[1])/("analysis-"+key+".bin");
    std::ifstream first(original,std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(first)),{});first.close();
    assert(GenerationCache::save(argv[1],key,restored,report,reason));
    std::ifstream second(original,std::ios::binary);
    assert(bytes==std::string((std::istreambuf_iterator<char>(second)),{}));second.close();
    grammar.rules["input"][0].action="$$ = 2;";
    grammar.rules["input"][0].actionLine=30;grammar.rules["input"][0].sourceOffset=100;
    assert(key==GenerationCache::key(grammar,{"A"},result.priority,"ielr",1,true,"wide","build"));
    assert(GenerationCache::load(argv[1],key,grammar,restored,report,reason));
    assert(restored.productions[0].action=="$$ = 2;" && restored.productions[0].actionLine==30);
    assert(restored.grammar.rules.at("input")[0].sourceOffset==100);
    const auto llKey=GenerationCache::key(grammar,{"A"},result.priority,"ll1",1,false,"wide","build");
    grammar.rules["input"][0].action="$$ = 3;";
    assert(llKey!=GenerationCache::key(grammar,{"A"},result.priority,"ll1",1,false,"wide","build"));
    assert(key!=GenerationCache::key(grammar,{"A"},result.priority,"ielr",1,true,"wide","new-build"));
    for (const auto &bad : {std::string(),bytes.substr(0,20),bytes+"trailing"}) {
        std::ofstream out(original,std::ios::binary|std::ios::trunc);out.write(bad.data(),bad.size());out.close();
        assert(!GenerationCache::load(argv[1],key,grammar,restored,report,reason));
    }
    return 0;
}
