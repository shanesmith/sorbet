#include "gtest/gtest.h"
// has to go first as it violates our requirements
#include "common/common.h"
#include "core/Error.h"
#include "core/ErrorQueue.h"
#include "core/Unfreeze.h"
#include "core/core.h"
#include "parser/Dedenter.h"
#include "parser/parser.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#include <fstream>
#include <string>
#include <vector>

namespace spd = spdlog;
using sorbet::u4;
using namespace std;

auto logger = spd::stderr_color_mt("parser_test");
auto errorQueue = make_shared<sorbet::core::ErrorQueue>(*logger, *logger);

TEST(ParserTest, SimpleParse) { // NOLINT
    sorbet::core::GlobalState gs(errorQueue);
    gs.initEmpty();
    sorbet::core::UnfreezeNameTable nameTableAccess(gs);
    sorbet::core::UnfreezeFileTable ft(gs);
    sorbet::core::FileRef fileId1 = gs.enterFile("<test1>", "def hello_world; p :hello; end");
    sorbet::parser::Parser::run(gs, fileId1);
    sorbet::core::FileRef fileId2 = gs.enterFile("<test2>", "class A; class B; end; end");
    sorbet::parser::Parser::run(gs, fileId2);
    sorbet::core::FileRef fileId3 = gs.enterFile("<test3>", "class A::B; module B; end; end");
    sorbet::parser::Parser::run(gs, fileId3);
}

struct DedentTest {
    int level;
    string_view in;
    string_view out;
};

TEST(ParserTest, TestDedent) { // NOLINT
    vector<DedentTest> cases = {
        {2, "    hi"sv, "  hi"sv},
        {10, "  \t    hi"sv, "  hi"sv},
        {2, "  a\n   b\n  c\n"sv, "a\n   b\n  c\n"sv},
        {4, "  a\n   b\n  c\n"sv, "a\n   b\n  c\n"sv},
    };
    for (auto &tc : cases) {
        sorbet::parser::Dedenter dedent(tc.level);
        string got = dedent.dedent(tc.in);
        EXPECT_EQ(got, tc.out);
    }
}