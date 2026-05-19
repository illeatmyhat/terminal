// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Per-workspace state and the top-level WorkspaceModelData container.
//
// NB: the name `WorkspaceState` here lives in the WorkspaceModel namespace
// and is unrelated to the existing
// winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceState
// in src/cascadia/TerminalSettingsModel/WorkspaceState.h. The two
// namespaces are intentionally independent.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "Ids.h"
#include "PaneTree.h"

namespace WorkspaceModel
{
    // The complete state of one workspace.
    //
    // The root is held by value (a std::variant) rather than by shared_ptr
    // because every workspace must always have exactly one root (invariant
    // 3 in validate()); a shared_ptr would invite a "what if it's null"
    // question that the model never wants to admit.
    struct WorkspaceState
    {
        WorkspaceId id{};
        std::string name{};
        std::optional<Color> color{};
        std::string customDescription{};
        bool pinned{ false };

        PaneNode root{ LeafPane{} };
        PaneId activePaneId{};

        [[nodiscard]] friend bool operator==(const WorkspaceState&,
                                             const WorkspaceState&) noexcept = default;
    };

    // The top-level model state. A `shared_ptr<const WorkspaceModelData>` is
    // intended to be the application's `ModelState` handle; this header
    // intentionally does not impose that alias.
    struct WorkspaceModelData
    {
        // All workspaces, in display order (sidebar top → bottom).
        std::vector<WorkspaceState> workspaces{};

        // The currently visible workspace, or std::nullopt when the model is
        // empty (no workspaces exist).
        std::optional<WorkspaceId> activeWorkspaceId{};

        // Most-recently-used order. MRU.front() is the most recent.
        // Must be a permutation of {w.id : w in workspaces}.
        std::deque<WorkspaceId> mru{};

        // Sidebar width in DIPs. Persists across launches.
        double sidebarWidth{ 240.0 };

        // Monotonic ID counter. Every new id allocated by an action is the
        // current value + 1; the counter is then incremented. Persisted so
        // that ids remain stable across launches and so the WAL/log records
        // referring to ids stay interpretable.
        std::uint64_t idCounter{ 0 };

        [[nodiscard]] friend bool operator==(const WorkspaceModelData&,
                                             const WorkspaceModelData&) noexcept = default;

        // --------------------------------------------------------------
        // Queries (10 total)
        //
        // These read-only accessors are the API for inspecting model
        // state. They are pure const member functions; the implementations
        // live in Queries.cpp (except findFirstTabOfKind which is a
        // template and therefore defined inline below).
        // --------------------------------------------------------------

        // Q1: every workspace in display order.
        [[nodiscard]] const std::vector<WorkspaceState>& workspaces_view() const noexcept;

        // Q2: a particular workspace by id, or nullptr if not found.
        [[nodiscard]] const WorkspaceState* workspace(WorkspaceId id) const noexcept;

        // Q3: the currently active workspace id, or std::nullopt for an
        // empty model.
        [[nodiscard]] std::optional<WorkspaceId> activeWorkspaceId_view() const noexcept;

        // Q4: MRU order (front = most recent).
        [[nodiscard]] const std::deque<WorkspaceId>& mru_view() const noexcept;

        // Q5: a pane (leaf or split) by id, or nullptr if not found.
        [[nodiscard]] const PaneNode* pane(PaneId id) const noexcept;

        // Q6: the parent split of the pane with id `id`, or nullptr if
        // the pane is a root or unknown.
        [[nodiscard]] const SplitPane* parentOf(PaneId id) const noexcept;

        // Q7: all leaves in the workspace's pane tree, in left-to-right
        // tree order. Returns an empty vector if the workspace is unknown.
        [[nodiscard]] std::vector<const LeafPane*> leaves(WorkspaceId id) const;

        // Q8: a tab by id, or nullptr if not found.
        [[nodiscard]] const TabRecord* tab(TabId id) const noexcept;

        // Q9: the first tab whose description.index() matches the given
        // alternative type, scanned across all workspaces in display order.
        // Returns nullptr if no such tab exists. Useful for "focus existing
        // Settings tab" policy in the UI.
        template<typename SpecT>
        [[nodiscard]] const TabRecord* findFirstTabOfKind() const noexcept
        {
            for (const auto& ws : workspaces)
            {
                std::vector<const LeafPane*> leafList;
                detail_collectLeavesPublic(ws.root, leafList);
                for (const auto* leaf : leafList)
                {
                    for (const auto& t : leaf->tabs)
                    {
                        if (std::holds_alternative<SpecT>(t.description))
                        {
                            return &t;
                        }
                    }
                }
            }
            return nullptr;
        }

        // Q10: sidebar width in DIPs.
        [[nodiscard]] double sidebarWidth_view() const noexcept;

    private:
        // Public-but-internal helper: collects leaves of a subtree, used
        // by the template findFirstTabOfKind which has to be defined in
        // this header. This is the only function on WorkspaceModelData
        // that's marked "use the cascade helpers directly instead, please".
        static void detail_collectLeavesPublic(const PaneNode& node,
                                               std::vector<const LeafPane*>& out);
    };
}
