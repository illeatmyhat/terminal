// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Tests for the property fuzzer.
//
// CI integration:
//   The fuzzer's wall-clock budget is controlled by the environment
//   variable WORKSPACE_FUZZ_SECONDS. Default 30 seconds. Pass a larger
//   value for nightly soak runs:
//       set WORKSPACE_FUZZ_SECONDS=600
//   Pass 0 or a non-numeric value to fall back to the default. Maximum
//   meaningful cap is ~3600 seconds; longer than that and you should
//   probably split into multiple runs with different seeds.

#include "pch.h"

#include "Fuzzer.h"

#include "../WorkspaceModel/WorkspaceActions.h"
#include "../WorkspaceModel/Serializer.h"
#include "../WorkspaceModel/Validator.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace WorkspaceModel;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace WorkspaceModelUnitTests;

namespace WorkspaceModelUnitTests
{
    namespace
    {
        // Read WORKSPACE_FUZZ_SECONDS, clamp, default 30. Uses getenv_s
        // (C11) rather than std::getenv because MSVC flags the latter as
        // deprecated under /WX.
        std::chrono::milliseconds fuzzBudget()
        {
            int seconds = 30;
            char buf[32]{};
            size_t actual = 0;
            if (getenv_s(&actual, buf, sizeof(buf), "WORKSPACE_FUZZ_SECONDS") == 0 && actual > 0)
            {
                try
                {
                    const int parsed = std::stoi(buf);
                    if (parsed > 0)
                    {
                        seconds = parsed;
                    }
                }
                catch (...)
                {
                    // ignore, keep default
                }
            }
            return std::chrono::seconds{ seconds };
        }

        // Where the checked-in regression .json files live. Resolved
        // relative to the test binary working directory. The vcxproj
        // doesn't copy files automatically, so for now we accept that
        // the directory may not exist in the binary CWD; tests that
        // depend on it will be no-ops when the directory is absent.
        //
        // Two candidate locations: ./regressions and ../regressions
        // (the latter handles common deploy layouts where the .json
        // files are at the project root).
        std::filesystem::path locateRegressionsDir()
        {
            namespace fs = std::filesystem;
            const std::vector<fs::path> candidates = {
                fs::path{ "regressions" },
                fs::path{ "..\\regressions" },
                fs::path{ "..\\..\\regressions" },
                fs::path{ "..\\..\\..\\src\\cascadia\\UnitTests_WorkspaceModel\\regressions" },
            };
            for (const auto& p : candidates)
            {
                std::error_code ec;
                if (fs::exists(p, ec) && fs::is_directory(p, ec))
                {
                    return p;
                }
            }
            // Fall back to "./regressions" — runSteps writes to this path
            // on failure.
            return fs::path{ "regressions" };
        }
    }

    class FuzzerTests
    {
        TEST_CLASS(FuzzerTests);

        TEST_METHOD(Fuzzer_30Seconds_NoInvariantViolations);
        TEST_METHOD(Fuzzer_DeterministicSeed_Reproducible);
        TEST_METHOD(Fuzzer_GenerateValidOp_EmptyState_ReturnsCreate);
        TEST_METHOD(Fuzzer_Regressions_ReplayClean);
        TEST_METHOD(Fuzzer_RegressionRoundTrip_SerializeAndParse);
    };

    // ------------------------------------------------------------------
    void FuzzerTests::Fuzzer_30Seconds_NoInvariantViolations()
    {
        // Pick a fresh seed per build so we sample new sequences over time.
        std::random_device rd;
        const std::uint64_t seed = (static_cast<std::uint64_t>(rd()) << 32) | rd();

        WorkspaceFuzzer fuzzer{ seed };
        auto outcome = fuzzer.runFor(std::make_shared<const WorkspaceModelData>(),
                                     fuzzBudget());

        std::ostringstream msg;
        msg << "fuzzer ran " << outcome.ops.size() << " ops with seed " << seed;
        Log::Comment(String(msg.str().c_str()));

        if (outcome.failingOpIdx.has_value())
        {
            std::ostringstream fail;
            fail << "fuzzer hit a validator violation at op idx "
                 << *outcome.failingOpIdx << " with seed " << seed;
            if (outcome.regressionPath.has_value())
            {
                fail << "; counterexample at " << outcome.regressionPath->string();
            }
            Log::Error(String(fail.str().c_str()));
        }

        VERIFY_IS_FALSE(outcome.failingOpIdx.has_value());
        // Also assert the final state validates clean as a belt-and-braces.
        if (outcome.finalState)
        {
            VERIFY_IS_FALSE(validate(*outcome.finalState).has_value());
        }
    }

    void FuzzerTests::Fuzzer_DeterministicSeed_Reproducible()
    {
        const std::uint64_t seed = 0xDEADBEEFCAFEBABEull;
        WorkspaceFuzzer a{ seed };
        WorkspaceFuzzer b{ seed };

        const std::size_t kSteps = 200;
        auto outA = a.runSteps(std::make_shared<const WorkspaceModelData>(), kSteps);
        auto outB = b.runSteps(std::make_shared<const WorkspaceModelData>(), kSteps);

        VERIFY_ARE_EQUAL(outA.ops.size(), outB.ops.size());
        for (std::size_t i = 0; i < outA.ops.size(); ++i)
        {
            VERIFY_IS_TRUE(outA.ops[i] == outB.ops[i]);
        }
        VERIFY_IS_TRUE(outA.failingOpIdx == outB.failingOpIdx);
    }

    void FuzzerTests::Fuzzer_GenerateValidOp_EmptyState_ReturnsCreate()
    {
        // From an empty state, generateValidOp should never produce an op
        // that requires a pre-existing entity (closeWorkspace, closeTab,
        // splitPane, etc.) — its op-selection guard re-rolls in that case
        // and falls back to newWorkspace if nothing else fits.
        WorkspaceFuzzer f{ 42 };
        WorkspaceModelData empty;
        for (int i = 0; i < 200; ++i)
        {
            auto op = f.generateValidOp(empty);
            const auto kindOk = std::visit(
                [](const auto& rec) -> bool {
                    using T = std::decay_t<decltype(rec)>;
                    return std::is_same_v<T, NewWorkspaceRecord> ||
                           std::is_same_v<T, CloseAllWorkspacesRecord> ||
                           std::is_same_v<T, SetSidebarWidthRecord>;
                },
                op);
            VERIFY_IS_TRUE(kindOk);
        }
    }

    void FuzzerTests::Fuzzer_Regressions_ReplayClean()
    {
        namespace fs = std::filesystem;
        const auto dir = locateRegressionsDir();
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
        {
            Log::Comment(L"No regressions directory present at runtime; skipping replay (this is expected when the test runs from the build output without the source-tree regressions folder copied alongside).");
            return;
        }

        std::size_t replayed = 0;
        for (const auto& entry : fs::directory_iterator(dir, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;

            std::ifstream is{ entry.path() };
            std::ostringstream ss;
            ss << is.rdbuf();
            const auto text = ss.str();

            Json::Value root;
            try
            {
                root = parseJson(text);
            }
            catch (const std::exception& e)
            {
                std::ostringstream msg;
                msg << "regression file " << entry.path().string()
                    << " failed to parse: " << e.what();
                Log::Error(String(msg.str().c_str()));
                VERIFY_FAIL(L"regression file did not parse");
                continue;
            }

            auto regression = WorkspaceFuzzer::parseRegression(root);
            auto outcome = WorkspaceFuzzer::replayRegression(regression);

            // Replay must either:
            //   - succeed (failingOpIdx absent) if the bug has been fixed
            //   - reproduce the same violation if it hasn't been fixed
            // Either is acceptable; what we forbid is a *different* failure
            // (e.g. a new bug introduced by recent action changes that
            // collapses an unrelated regression file).
            if (regression.failingOpIdx.has_value())
            {
                // The original counterexample was a failure. If we now
                // reproduce a failure too, the violation kind should match.
                if (outcome.failingOpIdx.has_value())
                {
                    VERIFY_IS_TRUE(outcome.violation == regression.violation);
                }
                // else: bug fixed since capture — fine.
            }
            else
            {
                // The captured file was a no-failure replay sample. It
                // must replay without failure.
                if (outcome.failingOpIdx.has_value())
                {
                    std::ostringstream msg;
                    msg << "regression file " << entry.path().string()
                        << " expected to replay clean but failed at op "
                        << *outcome.failingOpIdx;
                    Log::Error(String(msg.str().c_str()));
                }
                VERIFY_IS_FALSE(outcome.failingOpIdx.has_value());
            }
            ++replayed;
        }
        std::ostringstream msg;
        msg << "replayed " << replayed << " regression file(s) from " << dir.string();
        Log::Comment(String(msg.str().c_str()));
    }

    void FuzzerTests::Fuzzer_RegressionRoundTrip_SerializeAndParse()
    {
        // Build a small in-memory regression: empty start, a few valid
        // ops, no violation. Serialize → parse → replay → assert.
        WorkspaceFuzzer fuzzer{ 12345 };
        auto outcome = fuzzer.runSteps(std::make_shared<const WorkspaceModelData>(), 25);

        const auto j = WorkspaceFuzzer::serializeRegression(
            fuzzer.seed(),
            WorkspaceModelData{}, // empty start
            outcome.ops,
            outcome.failingOpIdx,
            outcome.violation);

        const auto text = writePretty(j);
        const auto parsed = parseJson(text);
        auto regression = WorkspaceFuzzer::parseRegression(parsed);

        VERIFY_ARE_EQUAL(fuzzer.seed(), regression.seed);
        VERIFY_ARE_EQUAL(outcome.ops.size(), regression.ops.size());
        for (std::size_t i = 0; i < outcome.ops.size(); ++i)
        {
            VERIFY_IS_TRUE(outcome.ops[i] == regression.ops[i]);
        }

        // Replay reproduces the same outcome.
        auto replayed = WorkspaceFuzzer::replayRegression(regression);
        VERIFY_IS_TRUE(replayed.failingOpIdx == outcome.failingOpIdx);
    }
}
