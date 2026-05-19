// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Replay engine tests. Because replay calls the existing mutators -- which
// allocate fresh IDs -- the raw ID values in the replayed model generally
// differ from the IDs in the model that produced the log. To assert
// equivalence, we use a structural-equality helper that compares
// everything except the IDs: workspace names, descriptions, colors,
// pinned flags, tab contents, tab titles, pane axes/ratios, active-by-
// position, and so on. This is the "structural equality" property the
// Slice 4 plan calls out.

#include "pch.h"

#include "TestHelpers.h"

#include "../WorkspaceModel/ActionLog.h"
#include "../WorkspaceModel/Replay.h"
#include "../WorkspaceModel/WAL.h"

using namespace WorkspaceModel;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace WorkspaceModelUnitTests;

namespace WorkspaceModelUnitTests
{
    namespace
    {
        // Strip every ID from a TabRecord; keep everything else.
        TabRecord stripTabIds(const TabRecord& t)
        {
            TabRecord c = t;
            c.id = TabId{};
            c.mount = std::nullopt;
            return c;
        }

        // Walk a pane subtree and zero out every PaneId / TabId / ContentId.
        PaneNode stripPaneIds(const PaneNode& n)
        {
            if (std::holds_alternative<LeafPane>(n))
            {
                auto leaf = std::get<LeafPane>(n);
                leaf.id = PaneId{};
                for (auto& t : leaf.tabs)
                {
                    t = stripTabIds(t);
                }
                return PaneNode{ leaf };
            }
            auto s = std::get<SplitPane>(n);
            s.id = PaneId{};
            if (s.left)
            {
                s.left = std::make_shared<const PaneNode>(stripPaneIds(*s.left));
            }
            if (s.right)
            {
                s.right = std::make_shared<const PaneNode>(stripPaneIds(*s.right));
            }
            return PaneNode{ s };
        }

        // Build a struct-equal pair of WorkspaceStates: every ID zeroed
        // (so positions/names/contents are what gets compared) and the
        // counter dropped. The activePaneId is also zeroed because the
        // active leaf's position in the tree is captured by the tree
        // shape itself, not its raw ID.
        WorkspaceModelData stripModelIds(const WorkspaceModelData& d)
        {
            WorkspaceModelData out;
            out.workspaces.reserve(d.workspaces.size());
            for (const auto& ws : d.workspaces)
            {
                WorkspaceState s = ws;
                s.id = WorkspaceId{};
                s.activePaneId = PaneId{};
                s.root = stripPaneIds(ws.root);
                out.workspaces.push_back(std::move(s));
            }
            // We don't compare mru-by-id (since ids reshuffle); instead
            // we compare mru-by-position-in-workspaces. Reconstruct that
            // here: for each id in the original mru, find its index in
            // the workspaces vector and treat the index as the canonical
            // identity. Then build a synthetic mru deque of WorkspaceId
            // values where v = (index + 1) so different positions stay
            // distinguishable.
            std::unordered_map<std::uint64_t, std::size_t> idToIdx;
            for (std::size_t i = 0; i < d.workspaces.size(); ++i)
            {
                idToIdx[d.workspaces[i].id.v] = i;
            }
            for (const auto& wid : d.mru)
            {
                const auto it = idToIdx.find(wid.v);
                if (it != idToIdx.end())
                {
                    out.mru.push_back(WorkspaceId{ it->second + 1 });
                }
            }
            // Active workspace by position, same trick.
            if (d.activeWorkspaceId.has_value())
            {
                const auto it = idToIdx.find(d.activeWorkspaceId->v);
                if (it != idToIdx.end())
                {
                    out.activeWorkspaceId = WorkspaceId{ it->second + 1 };
                }
            }
            out.sidebarWidth = d.sidebarWidth;
            // idCounter is intentionally NOT compared.
            out.idCounter = 0;
            return out;
        }

        // The structural-equality predicate. Two models are structurally
        // equal iff their non-ID fields match after the ID-stripping
        // transform above.
        bool structurallyEqual(const WorkspaceModelData& a,
                               const WorkspaceModelData& b)
        {
            return stripModelIds(a) == stripModelIds(b);
        }
    }

    class ReplayTests
    {
        TEST_CLASS(ReplayTests);

        TEST_METHOD(EmptyLog_OnEmpty_IsEmpty);
        TEST_METHOD(SingleNewWorkspace_ReproducesShape);
        TEST_METHOD(WorkspaceCreateThenRename_ReproducesShape);
        TEST_METHOD(SidebarWidth_PersistsThroughReplay);
        TEST_METHOD(BadReferences_HaltReplaySafe);
    };

    void ReplayTests::EmptyLog_OnEmpty_IsEmpty()
    {
        auto start = emptyModel();
        const auto out = replay(start, {});
        VERIFY_ARE_EQUAL(*start, *out);
    }

    void ReplayTests::SingleNewWorkspace_ReproducesShape()
    {
        // Build "expected" via direct mutator chain.
        auto expected = newWorkspace(emptyModel(), "alpha", termSpec(1)).state;

        // Build a log that does the same.
        std::vector<LogEntry> log;
        log.push_back(LogEntry{ 1, OpRecord{ NewWorkspaceRecord{ "alpha", termSpec(1), "", std::nullopt, false } }, "ts" });

        const auto actual = replay(emptyModel(), log);
        VERIFY_IS_TRUE(structurallyEqual(*expected, *actual));
    }

    void ReplayTests::WorkspaceCreateThenRename_ReproducesShape()
    {
        // Expected: create then rename.
        auto a = newWorkspace(emptyModel(), "alpha", termSpec(1));
        auto expected = renameWorkspace(a.state, a.id, "beta");

        // Log: same two ops. We use the IDs from the expected run but
        // replay will reshuffle them -- the structural equality helper
        // ignores raw ids.
        std::vector<LogEntry> log;
        log.push_back(LogEntry{ 1, OpRecord{ NewWorkspaceRecord{ "alpha", termSpec(1), "", std::nullopt, false } }, "" });
        // After replay, the new workspace's id is fresh; we have to use
        // *some* id in the log. Use a.id which won't exist in the
        // replayed state -- renameWorkspace will then no-op on the
        // unknown id and the test catches that wrong-id scenario instead
        // of testing the rename. To test correctly, build the log by
        // first replaying NewWorkspace and reading the fresh id.
        auto afterFirst = replay(emptyModel(), { log[0] });
        const auto freshWs = afterFirst->workspaces[0].id;
        log.push_back(LogEntry{ 2, OpRecord{ RenameWorkspaceRecord{ freshWs, "beta" } }, "" });

        const auto actual = replay(emptyModel(), log);
        VERIFY_IS_TRUE(structurallyEqual(*expected, *actual));
        VERIFY_ARE_EQUAL(std::string{ "beta" }, actual->workspaces[0].name);
    }

    void ReplayTests::SidebarWidth_PersistsThroughReplay()
    {
        auto expected = setSidebarWidth(emptyModel(), 333.0);

        std::vector<LogEntry> log;
        log.push_back(LogEntry{ 1, OpRecord{ SetSidebarWidthRecord{ 333.0 } }, "" });

        const auto actual = replay(emptyModel(), log);
        VERIFY_ARE_EQUAL(333.0, actual->sidebarWidth);
        VERIFY_IS_TRUE(structurallyEqual(*expected, *actual));
    }

    void ReplayTests::BadReferences_HaltReplaySafe()
    {
        // closeTab against a tab id that doesn't exist is a no-op in the
        // mutator, so the resulting state is still valid. replaySafe
        // therefore should NOT halt -- it only halts on validator
        // failure or thrown exceptions. Verify the safe wrapper returns
        // a complete run with no error.
        std::vector<LogEntry> log;
        log.push_back(LogEntry{ 1, OpRecord{ CloseTabRecord{ TabId{ 999 } } }, "" });

        const auto r = replaySafe(emptyModel(), log);
        VERIFY_IS_FALSE(r.error.has_value());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), r.entriesApplied);
    }
}
