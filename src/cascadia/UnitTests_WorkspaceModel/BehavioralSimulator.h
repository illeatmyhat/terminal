// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// BehavioralSimulator — a TAEF-test-level DSL for scripting user flows
// against the pure-C++ workspace model.
//
// Each Script step is a thin wrapper around one mutator from Mutators.h
// that records the produced entity (workspace, leaf, tab) by a string
// LABEL. Subsequent steps then address entities by that label, which keeps
// the test scripts readable and isolates them from "mutator returns IDs in
// a particular order" assumptions.
//
// The simulator also captures the RenderOp sequence produced by reconcile()
// between successive snapshots, so scripts can assert on the renderer
// output without re-running reconcile manually.
//
// Pure C++: no winrt::*. Lives alongside the other unit-test fixtures.

#pragma once

#include "../WorkspaceModel/Mutators.h"
#include "../WorkspaceModel/PaneTree.h"
#include "../WorkspaceModel/Reconciler.h"
#include "../WorkspaceModel/RenderOp.h"
#include "../WorkspaceModel/TabContent.h"
#include "../WorkspaceModel/Validator.h"
#include "../WorkspaceModel/WorkspaceState.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace WorkspaceModelUnitTests
{
    // The simulator's primary handle. Tests instantiate a Script, chain
    // step calls on it, optionally interleave expectState / expectRenderOps
    // assertions, then call run() to get the final ModelState.
    //
    // Every mutator step validates the result via validate() and stores
    // the first violation it sees in `_violation`. expectNoViolation() (the
    // default state assertion at every step) reads it; tests can also
    // explicitly accept a violation in negative-path tests by checking
    // _violation after run().
    class Script
    {
    public:
        Script();

        // --------------------------------------------------------------
        // Workspace mutators
        // --------------------------------------------------------------

        Script& newWorkspace(std::string label,
                             std::string name,
                             WorkspaceModel::TabContent initialTab,
                             std::string firstTabLabel);

        Script& newWorkspace(std::string label,
                             std::string name,
                             WorkspaceModel::TabContent initialTab,
                             std::string firstTabLabel,
                             std::string firstLeafLabel);

        Script& closeWorkspace(std::string label);
        Script& closeOtherWorkspaces(std::string keepLabel);
        Script& closeAllWorkspaces();
        Script& switchToWorkspace(std::string label);
        Script& renameWorkspace(std::string label, std::string name);
        Script& setWorkspaceColor(std::string label, std::optional<WorkspaceModel::Color> color);
        Script& setWorkspaceDescription(std::string label, std::string description);
        Script& setWorkspacePinned(std::string label, bool pinned);
        Script& reorderWorkspace(std::string label, std::size_t dstIdx);

        // --------------------------------------------------------------
        // Tab mutators
        // --------------------------------------------------------------

        Script& newTab(std::string newTabLabel,
                       std::string workspaceLabel,
                       std::string leafLabel,
                       WorkspaceModel::TabContent description,
                       std::string customTitle = {},
                       std::optional<WorkspaceModel::Color> color = std::nullopt,
                       bool pinned = false);

        Script& closeTab(std::string tabLabel);
        Script& closeTabsRight(std::string tabLabel);
        Script& closeOtherTabs(std::string tabLabel);
        Script& selectTab(std::string tabLabel);
        Script& setTabTitle(std::string tabLabel, std::string title);
        Script& setTabColor(std::string tabLabel, std::optional<WorkspaceModel::Color> color);
        Script& setTabPinned(std::string tabLabel, bool pinned);

        // --------------------------------------------------------------
        // Pane mutators
        // --------------------------------------------------------------

        // Splits the leaf addressed by `leafLabel`. After the split the
        // original leaf retains its PaneId and the new sibling leaf is
        // labelled `newLeafLabel`. The new tab (always allocated by the
        // splitPane mutator) is labelled `newTabLabel`.
        Script& splitPane(std::string leafLabel,
                          WorkspaceModel::Axis axis,
                          double ratio,
                          WorkspaceModel::TabContent newTabDescription,
                          std::string newLeafLabel,
                          std::string newTabLabel);

        Script& closePane(std::string leafLabel);
        Script& resizePane(std::string splitLabel, double ratio);
        Script& focusPane(std::string leafLabel);

        // --------------------------------------------------------------
        // Move mutators
        // --------------------------------------------------------------

        Script& moveTab(std::string tabLabel, std::string dstLeafLabel, std::size_t dstIdx);
        Script& moveTabAsSplit(std::string tabLabel,
                               std::string dstLeafLabel,
                               WorkspaceModel::Edge edge,
                               std::string newLeafLabel);

        // --------------------------------------------------------------
        // UI prefs
        // --------------------------------------------------------------

        Script& setSidebarWidth(double width);

        // --------------------------------------------------------------
        // Assertions
        // --------------------------------------------------------------

        // Capture a snapshot of the current state. Subsequent calls to
        // expectRenderOpsSinceLast() diff against this snapshot.
        Script& snapshot();

        // Assert a predicate on the current model state.
        Script& expect(std::function<bool(const WorkspaceModel::WorkspaceModelData&)> predicate,
                       std::string msg = {});

        // Assert that no validate() violation has been observed at any
        // step so far. This is the implicit default after every mutator;
        // tests can call it explicitly to add a checkpoint comment.
        Script& expectNoViolation();

        // Assert that the RenderOp sequence emitted by reconcile() between
        // the last snapshot() and the current state matches the predicate
        // list (one predicate per op, in order, equal in size).
        Script& expectRenderOpsSinceLast(
            std::vector<std::function<bool(const WorkspaceModel::RenderOp&)>> predicates);

        // Assert that at least one op of the matching kind exists in the
        // RenderOp sequence since the last snapshot. Looser sibling to
        // expectRenderOpsSinceLast (full sequence equality).
        Script& expectRenderOpSinceLast(
            std::function<bool(const WorkspaceModel::RenderOp&)> predicate,
            std::string msg = {});

        // --------------------------------------------------------------
        // Resolution / accessors
        // --------------------------------------------------------------

        // After run(), tests can read the final state and the label map.
        [[nodiscard]] const WorkspaceModel::ModelState& state() const noexcept { return _state; }
        [[nodiscard]] std::optional<WorkspaceModel::Violation> firstViolation() const noexcept { return _violation; }
        [[nodiscard]] const std::vector<std::string>& failures() const noexcept { return _failures; }

        // Resolve a label to its id. Returns the zero id if unknown.
        [[nodiscard]] WorkspaceModel::WorkspaceId workspaceFor(const std::string& label) const noexcept;
        [[nodiscard]] WorkspaceModel::PaneId leafFor(const std::string& label) const noexcept;
        [[nodiscard]] WorkspaceModel::TabId tabFor(const std::string& label) const noexcept;

        // Run the script to completion. Returns the final state.
        WorkspaceModel::ModelState run();

        // --------------------------------------------------------------
        // RenderOp matchers (free helpers, exposed for readable tests)
        // --------------------------------------------------------------

        // Returns true iff the variant arm matches T.
        template<typename T>
        static std::function<bool(const WorkspaceModel::RenderOp&)> isOp()
        {
            return [](const WorkspaceModel::RenderOp& op) {
                return std::holds_alternative<T>(op);
            };
        }

    private:
        void runMutatorStep(std::function<void()> step);

        WorkspaceModel::ModelState _state;
        WorkspaceModel::ModelState _snapshotState;
        std::optional<WorkspaceModel::Violation> _violation;
        std::vector<std::string> _failures{};

        std::unordered_map<std::string, WorkspaceModel::WorkspaceId> _wsLabels{};
        std::unordered_map<std::string, WorkspaceModel::PaneId> _paneLabels{};
        std::unordered_map<std::string, WorkspaceModel::TabId> _tabLabels{};

        // Steps are recorded as lambdas so the script is fully data — we
        // could in principle log them or replay them later. For now this
        // is just bookkeeping.
        std::vector<std::function<void()>> _steps{};
        bool _ran{ false };
    };

    // ------------------------------------------------------------------
    // Free helpers used by behavioral tests. Kept here so test files
    // don't need to repeat them.
    // ------------------------------------------------------------------

    inline WorkspaceModel::TerminalSpec termSpecSim(std::uint8_t seed)
    {
        WorkspaceModel::TerminalSpec s{};
        s.profile[0] = seed;
        return s;
    }
}
