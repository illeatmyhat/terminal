// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Property fuzzer for the WorkspaceModel.
//
// Hand-rolled (~150 LOC of pure C++, no external dependency). Uses
// <random> only. Deterministic: a Fuzzer constructed with a given seed
// produces the same op sequence every run.
//
// The fuzzer's op generator inspects the current ModelState to pick op
// parameters that are *valid* for that state (e.g. closeTab picks a
// TabId that actually exists). This biases the explored space toward
// "things the app would actually do," which is where the cascade-wipe
// class of bugs lives.
//
// On any validate() failure during a run, the fuzzer saves the
// counterexample as a JSON file under `regressions/<timestamp>.json` so
// it can be replayed deterministically on subsequent builds.

#pragma once

#include "../WorkspaceModel/ActionLog.h"
#include "../WorkspaceModel/Mutators.h"
#include "../WorkspaceModel/Replay.h"
#include "../WorkspaceModel/Serializer.h"
#include "../WorkspaceModel/Validator.h"
#include "../WorkspaceModel/WorkspaceState.h"

#include <json/json.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace WorkspaceModelUnitTests
{
    // Outcome of one fuzzer run.
    struct FuzzerOutcome
    {
        // Final ModelState after the run.
        WorkspaceModel::ModelState finalState;

        // The full op sequence applied (in order). Empty if the start
        // state was already invalid before any op was applied.
        std::vector<WorkspaceModel::OpRecord> ops;

        // The op index that triggered a validate() violation, if any.
        // std::nullopt means the run completed clean.
        std::optional<std::size_t> failingOpIdx;

        // The validator violation observed. std::nullopt iff
        // failingOpIdx is std::nullopt.
        std::optional<WorkspaceModel::Violation> violation;

        // If a counterexample was written to disk, the path is here.
        std::optional<std::filesystem::path> regressionPath;
    };

    // The fuzzer itself. Thread-affine; one instance per test thread.
    class WorkspaceFuzzer
    {
    public:
        explicit WorkspaceFuzzer(std::uint64_t seed) noexcept;

        // Run a fixed number of mutator steps. Returns when steps are
        // exhausted or a validate() violation is hit (whichever first).
        FuzzerOutcome runSteps(WorkspaceModel::ModelState start, std::size_t numSteps);

        // Run until a wall-clock deadline. Returns the outcome of the
        // last completed step (or a violation, if one occurred earlier).
        // The fuzzer checks the clock every step, so worst-case overshoot
        // is one op's wall time (microseconds).
        FuzzerOutcome runFor(WorkspaceModel::ModelState start,
                             std::chrono::milliseconds budget);

        // The seed this fuzzer was constructed with.
        [[nodiscard]] std::uint64_t seed() const noexcept { return _seed; }

        // Generate one valid op for the given state. Public so test code
        // can call it directly (e.g. for the determinism test).
        [[nodiscard]] WorkspaceModel::OpRecord generateValidOp(
            const WorkspaceModel::WorkspaceModelData& state);

        // Serialize a counterexample to JSON: { seed, startState, ops, failingOpIdx, violation }.
        // The result file can be replayed via replayRegression() below.
        static Json::Value serializeRegression(std::uint64_t seed,
                                               const WorkspaceModel::WorkspaceModelData& startState,
                                               const std::vector<WorkspaceModel::OpRecord>& ops,
                                               std::optional<std::size_t> failingOpIdx,
                                               std::optional<WorkspaceModel::Violation> violation);

        struct RegressionFile
        {
            std::uint64_t seed{ 0 };
            WorkspaceModel::WorkspaceModelData startState;
            std::vector<WorkspaceModel::OpRecord> ops;
            std::optional<std::size_t> failingOpIdx;
            std::optional<WorkspaceModel::Violation> violation;
        };

        // Parse a regression JSON value (produced by serializeRegression).
        [[nodiscard]] static RegressionFile parseRegression(const Json::Value& j);

        // Apply the op sequence in the regression file starting from its
        // recorded start state. Returns the FuzzerOutcome as if the fuzzer
        // had just produced this op stream. Used by FuzzerTests to assert
        // that previously-captured bugs no longer reproduce.
        [[nodiscard]] static FuzzerOutcome replayRegression(const RegressionFile& f);

    private:
        std::uint64_t _seed;
        std::mt19937_64 _rng;

        // Helpers used by generateValidOp().
        WorkspaceModel::TabContent randomTabContent();
        WorkspaceModel::Color randomColor();
        WorkspaceModel::Axis randomAxis();
        WorkspaceModel::Edge randomEdge();
        double randomRatio();
        std::string randomName(const char* prefix);

        // Collect leaves / tabs from a state for op generation.
        struct EntityIndex
        {
            std::vector<WorkspaceModel::WorkspaceId> workspaces;
            std::vector<WorkspaceModel::PaneId> leaves;
            std::vector<WorkspaceModel::PaneId> splits;
            // For each leaf, which workspace it belongs to.
            std::vector<WorkspaceModel::WorkspaceId> leafWorkspace;
            std::vector<WorkspaceModel::TabId> tabs;
            std::vector<WorkspaceModel::PaneId> tabLeaf;
        };
        [[nodiscard]] EntityIndex indexEntities(const WorkspaceModel::WorkspaceModelData& s);
    };
}
