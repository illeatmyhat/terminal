// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Tests for diff() and applyChanges().
//
// The tests hand-construct ModelState fixture pairs (prev, next) directly,
// without going through the workspace actions, so diff is exercised in
// isolation. A small handful of "round trip" tests at the bottom use the
// actions as a sanity check that action + diff compose correctly.

#include "pch.h"

#include "MockWorkspaceView.h"
#include "TestHelpers.h"

#include "../WorkspaceModel/Diff.h"
#include "../WorkspaceModel/IWorkspaceView.h"
#include "../WorkspaceModel/WorkspaceActions.h"
#include "../WorkspaceModel/WorkspaceChange.h"

using namespace WorkspaceModel;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace WorkspaceModelUnitTests;

namespace WorkspaceModelUnitTests
{
    namespace
    {
        // -----------------------------------------------------------------
        // Hand-rolled fixture builders. These produce minimal, well-formed
        // ModelState values for diff scenarios. We intentionally don't use
        // the workspace actions here so diff bugs aren't masked by action
        // bugs.
        // -----------------------------------------------------------------

        TerminalSpec spec(std::uint8_t seed)
        {
            TerminalSpec s{};
            s.profile[0] = seed;
            return s;
        }

        TabRecord makeTab(std::uint64_t tabIdVal, std::uint8_t specSeed = 0)
        {
            TabRecord t;
            t.id = TabId{ tabIdVal };
            t.description = spec(specSeed != 0 ? specSeed : static_cast<std::uint8_t>(tabIdVal));
            return t;
        }

        LeafPane makeLeaf(std::uint64_t paneIdVal, std::vector<TabRecord> tabs, std::size_t active = 0)
        {
            LeafPane l;
            l.id = PaneId{ paneIdVal };
            l.tabs = std::move(tabs);
            l.activeTabIdx = active;
            return l;
        }

        SplitPane makeSplit(std::uint64_t splitIdVal,
                            PaneNode left,
                            PaneNode right,
                            Axis axis = Axis::Vertical,
                            double ratio = 0.5)
        {
            SplitPane s;
            s.id = PaneId{ splitIdVal };
            s.axis = axis;
            s.ratio = ratio;
            s.left = std::make_shared<const PaneNode>(std::move(left));
            s.right = std::make_shared<const PaneNode>(std::move(right));
            return s;
        }

        WorkspaceState makeWs(std::uint64_t wsIdVal,
                              PaneNode root,
                              PaneId activePane,
                              std::string name = "ws")
        {
            WorkspaceState ws;
            ws.id = WorkspaceId{ wsIdVal };
            ws.name = std::move(name);
            ws.root = std::move(root);
            ws.activePaneId = activePane;
            return ws;
        }

        ModelState makeState(std::vector<WorkspaceState> wss,
                             std::optional<WorkspaceId> active = std::nullopt,
                             std::uint64_t idCounter = 100)
        {
            WorkspaceModelData d;
            d.workspaces = std::move(wss);
            d.activeWorkspaceId = active;
            for (const auto& w : d.workspaces)
            {
                d.mru.push_back(w.id);
            }
            d.idCounter = idCounter;
            return std::make_shared<const WorkspaceModelData>(std::move(d));
        }

        ModelState emptyState()
        {
            return std::make_shared<const WorkspaceModelData>();
        }

        // -----------------------------------------------------------------
        // Filtering helpers for tests that want to look at a specific
        // change kind without caring about ordering of other changes.
        // -----------------------------------------------------------------

        template<typename Change>
        std::vector<Change> changesOfKind(const std::vector<WorkspaceChange>& changes)
        {
            std::vector<Change> out;
            for (const auto& change : changes)
            {
                if (const auto* p = std::get_if<Change>(&change))
                {
                    out.push_back(*p);
                }
            }
            return out;
        }

        template<typename Change>
        std::size_t countChangesOfKind(const std::vector<WorkspaceChange>& changes)
        {
            return changesOfKind<Change>(changes).size();
        }

        // True iff `changes` contains a change of variant arm T.
        template<typename Change>
        bool containsChange(const std::vector<WorkspaceChange>& changes)
        {
            return countChangesOfKind<Change>(changes) > 0;
        }
    }

    class DiffTests
    {
        TEST_CLASS(DiffTests);

        // ---- Per-arm tests (one fixture pair triggers exactly one arm) ----
        TEST_METHOD(WorkspaceAdded_FromEmpty);
        TEST_METHOD(WorkspaceRemoved_ToEmpty);
        TEST_METHOD(ActiveWorkspaceChanged_Changed);
        TEST_METHOD(LeafPaneCreated_NewWorkspace);
        TEST_METHOD(SplitPaneCreated_WrapLeaf);
        TEST_METHOD(SplitPaneCollapsed_ChildRemoved);
        TEST_METHOD(SplitRatioChanged_Changed);
        TEST_METHOD(TabAdded_NewTabInSurvivingLeaf);
        TEST_METHOD(TabRemoved_TabGoneFromSurvivingLeaf);
        TEST_METHOD(TabMoved_SameLeafReorder);
        TEST_METHOD(ActiveTabChanged_SurvivingLeaf);
        TEST_METHOD(ContentMounted_FirstMount);
        TEST_METHOD(ContentUnmounted_OnTabRemove);
        // Phase 2 Slice 3 (#47): a workspace-switch-like prev->next where the
        // newly-active workspace's tab gains a mount (ContentMounted) and the
        // now-inactive workspace's tab loses its mount (ContentUnmounted) in
        // the SAME diff. This is the prev->next shape that drives the
        // ContentRegistry's mount/keep-alive lifecycle once S4 wires switching.
        TEST_METHOD(WorkspaceSwitch_MountsActive_UnmountsInactive);
        TEST_METHOD(TabDecorationUpdated_TitleChanged);

        // ---- Enriched-payload coverage (the model->view contract that
        //      replaced WorkspaceView's held state) ----
        TEST_METHOD(LeafPaneCreated_InSplit_CarriesParentAxisRatio);
        TEST_METHOD(TabDecorationUpdated_SecondWorkspace_CarriesIndex);
        TEST_METHOD(TabAdded_SecondWorkspace_CarriesOwningWorkspace);

        // ---- Stable-id arms (#45/#44): the three arms that used to carry a
        //      raw display index now carry a WorkspaceId, independent of
        //      display order. ----
        TEST_METHOD(IdentityArms_CarryWorkspaceId_IndependentOfDisplayOrder);

        // ---- Identity-keyed move detection (the load-bearing case) ----
        TEST_METHOD(TabMoved_CrossLeaf_SingleChange);
        TEST_METHOD(TabMoved_CrossWorkspace_SingleChange);

        // ---- Split preservation under wrap ----
        TEST_METHOD(SplitWrap_PreservesOriginalLeafIdentity);

        // ---- applyChanges wiring ----
        TEST_METHOD(ApplyChanges_RecordsInOrder);

        // ---- Empty / no-op edge cases ----
        TEST_METHOD(Diff_IdenticalStates_Empty);
        TEST_METHOD(Diff_NullPrev_ProducesAdds);

        // ---- Round-trip smoke tests using actions ----
        TEST_METHOD(Roundtrip_SplitPane_EmitsExpectedShape);
        TEST_METHOD(Roundtrip_NewWorkspace_EmitsExpectedShape);
    };

    // =====================================================================
    // Per-arm tests
    // =====================================================================

    void DiffTests::WorkspaceAdded_FromEmpty()
    {
        auto prev = emptyState();
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }, "alpha") },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto adds = changesOfKind<WorkspaceAdded>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), adds.size());
        // The arm carries the stable id only; no positional display index.
        // The view resolves the insertion position from this id.
        VERIFY_ARE_EQUAL(WorkspaceId{ 1 }.v, adds[0].id.v);
    }

    void DiffTests::WorkspaceRemoved_ToEmpty()
    {
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        auto next = emptyState();

        const auto ops = diff(prev, next);
        const auto removes = changesOfKind<WorkspaceRemoved>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), removes.size());
        VERIFY_ARE_EQUAL(WorkspaceId{ 1 }.v, removes[0].id.v);
    }

    void DiffTests::ActiveWorkspaceChanged_Changed()
    {
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }),
              makeWs(2, makeLeaf(20, { makeTab(200) }), PaneId{ 20 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }),
              makeWs(2, makeLeaf(20, { makeTab(200) }), PaneId{ 20 }) },
            WorkspaceId{ 2 });

        const auto ops = diff(prev, next);
        const auto setActive = changesOfKind<ActiveWorkspaceChanged>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), setActive.size());
        // The arm carries the newly-active workspace's stable id only; the
        // view resolves it to the classic tab to select. No display index.
        VERIFY_IS_TRUE(setActive[0].id.has_value());
        VERIFY_ARE_EQUAL(WorkspaceId{ 2 }.v, setActive[0].id->v);
    }

    void DiffTests::LeafPaneCreated_NewWorkspace()
    {
        auto prev = emptyState();
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto creates = changesOfKind<LeafPaneCreated>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), creates.size());
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, creates[0].id.v);
        VERIFY_IS_FALSE(creates[0].parent.has_value());
    }

    void DiffTests::SplitPaneCreated_WrapLeaf()
    {
        // prev: ws1 has a single leaf (paneId=10) with one tab.
        // next: ws1 has a SplitPane (id=50) wrapping the original leaf
        //       (id=10) on the left and a new leaf (id=20) on the right.
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        PaneNode leftLeaf = makeLeaf(10, { makeTab(100) });
        PaneNode rightLeaf = makeLeaf(20, { makeTab(200) });
        auto split = makeSplit(50, std::move(leftLeaf), std::move(rightLeaf));
        auto next = makeState(
            { makeWs(1, PaneNode{ split }, PaneId{ 20 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto splits = changesOfKind<SplitPaneCreated>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), splits.size());
        VERIFY_ARE_EQUAL(PaneId{ 50 }.v, splits[0].id.v);
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, splits[0].left.v);
        VERIFY_ARE_EQUAL(PaneId{ 20 }.v, splits[0].right.v);
    }

    void DiffTests::SplitPaneCollapsed_ChildRemoved()
    {
        // prev: ws1 root is split(50, leftLeaf=10, rightLeaf=20).
        // next: ws1 root is leftLeaf=10 alone (split + right leaf gone).
        PaneNode leftPrev = makeLeaf(10, { makeTab(100) });
        PaneNode rightPrev = makeLeaf(20, { makeTab(200) });
        auto splitPrev = makeSplit(50, leftPrev, rightPrev);
        auto prev = makeState(
            { makeWs(1, PaneNode{ splitPrev }, PaneId{ 10 }) },
            WorkspaceId{ 1 });

        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto collapses = changesOfKind<SplitPaneCollapsed>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), collapses.size());
        VERIFY_ARE_EQUAL(PaneId{ 50 }.v, collapses[0].removedSplit.v);
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, collapses[0].survivor.v);
    }

    void DiffTests::SplitRatioChanged_Changed()
    {
        PaneNode left = makeLeaf(10, { makeTab(100) });
        PaneNode right = makeLeaf(20, { makeTab(200) });
        auto splitPrev = makeSplit(50, left, right, Axis::Vertical, 0.5);
        auto splitNext = makeSplit(50, left, right, Axis::Vertical, 0.75);

        auto prev = makeState(
            { makeWs(1, PaneNode{ splitPrev }, PaneId{ 10 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, PaneNode{ splitNext }, PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto ratios = changesOfKind<SplitRatioChanged>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), ratios.size());
        VERIFY_ARE_EQUAL(PaneId{ 50 }.v, ratios[0].id.v);
        VERIFY_ARE_EQUAL(0.75, ratios[0].ratio);
    }

    void DiffTests::TabAdded_NewTabInSurvivingLeaf()
    {
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100), makeTab(101) }, 1), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto adds = changesOfKind<TabAdded>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), adds.size());
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, adds[0].leafId.v);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), adds[0].idx);
        VERIFY_ARE_EQUAL(TabId{ 101 }.v, adds[0].id.v);
        // Enriched payload: the change carries the content spec, the owning
        // workspace, and a "leaf is nested in a split" flag (false here —
        // leaf 10 is the workspace root).
        VERIFY_IS_TRUE(std::holds_alternative<TerminalSpec>(adds[0].description));
        VERIFY_IS_TRUE(std::get<TerminalSpec>(adds[0].description) == spec(101));
        VERIFY_ARE_EQUAL(WorkspaceId{ 1 }.v, adds[0].owningWorkspace.v);
        VERIFY_IS_FALSE(adds[0].leafInsideSplit);
    }

    void DiffTests::TabRemoved_TabGoneFromSurvivingLeaf()
    {
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100), makeTab(101) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto removes = changesOfKind<TabRemoved>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), removes.size());
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, removes[0].leafId.v);
        VERIFY_ARE_EQUAL(TabId{ 101 }.v, removes[0].id.v);
    }

    void DiffTests::TabMoved_SameLeafReorder()
    {
        // prev: leaf 10 has tabs [100, 101].
        // next: leaf 10 has tabs [101, 100] (swap order).
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100), makeTab(101) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(101), makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto moves = changesOfKind<TabMoved>(ops);
        // Both tabs moved positions; we expect two TabMoved ops (not
        // Remove+Add), and definitely no Remove/Add ops for these tabs.
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), moves.size());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<TabRemoved>(ops));
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<TabAdded>(ops));
    }

    void DiffTests::ActiveTabChanged_SurvivingLeaf()
    {
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100), makeTab(101) }, 0), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100), makeTab(101) }, 1), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto sets = changesOfKind<ActiveTabChanged>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), sets.size());
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, sets[0].leafId.v);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), sets[0].idx);
    }

    void DiffTests::ContentMounted_FirstMount()
    {
        auto leafPrev = makeLeaf(10, { makeTab(100) });
        auto tabNext = makeTab(100);
        tabNext.mount = ContentId{ 555 };
        auto leafNext = makeLeaf(10, { tabNext });

        auto prev = makeState({ makeWs(1, PaneNode{ leafPrev }, PaneId{ 10 }) }, WorkspaceId{ 1 });
        auto next = makeState({ makeWs(1, PaneNode{ leafNext }, PaneId{ 10 }) }, WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto mounts = changesOfKind<ContentMounted>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), mounts.size());
        VERIFY_ARE_EQUAL(TabId{ 100 }.v, mounts[0].tabId.v);
        VERIFY_ARE_EQUAL(ContentId{ 555 }.v, mounts[0].contentId.v);
    }

    void DiffTests::ContentUnmounted_OnTabRemove()
    {
        auto t100 = makeTab(100);
        t100.mount = ContentId{ 555 };
        auto t101 = makeTab(101);

        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { t100, t101 }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        // next: tab 100 removed (still has mount=555 in prev).
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { t101 }), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto unmounts = changesOfKind<ContentUnmounted>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), unmounts.size());
        VERIFY_ARE_EQUAL(TabId{ 100 }.v, unmounts[0].tabId.v);
        VERIFY_ARE_EQUAL(ContentId{ 555 }.v, unmounts[0].contentId.v);
    }

    void DiffTests::WorkspaceSwitch_MountsActive_UnmountsInactive()
    {
        // Two workspaces. In prev, ws1 is active and its tab is mounted
        // (ContentId 901); ws2 is inactive and unmounted. A workspace switch
        // makes ws2 active: the model mounts ws2's content (902) and unmounts
        // ws1's (901, which becomes detached but — at the ContentRegistry layer
        // — stays alive). This is the prev->next shape S4 will produce.
        auto ws1TabPrev = makeTab(100);
        ws1TabPrev.mount = ContentId{ 901 }; // ws1 active+mounted in prev
        auto ws2TabPrev = makeTab(200); // ws2 inactive, no mount in prev

        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { ws1TabPrev }), PaneId{ 10 }, "alpha"),
              makeWs(2, makeLeaf(20, { ws2TabPrev }), PaneId{ 20 }, "beta") },
            WorkspaceId{ 1 });

        auto ws1TabNext = makeTab(100); // ws1 now inactive: mount cleared
        auto ws2TabNext = makeTab(200);
        ws2TabNext.mount = ContentId{ 902 }; // ws2 now active+mounted

        auto next = makeState(
            { makeWs(1, makeLeaf(10, { ws1TabNext }), PaneId{ 10 }, "alpha"),
              makeWs(2, makeLeaf(20, { ws2TabNext }), PaneId{ 20 }, "beta") },
            WorkspaceId{ 2 });

        const auto ops = diff(prev, next);

        // The newly-active workspace's content is mounted.
        const auto mounts = changesOfKind<ContentMounted>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), mounts.size());
        VERIFY_ARE_EQUAL(TabId{ 200 }.v, mounts[0].tabId.v);
        VERIFY_ARE_EQUAL(ContentId{ 902 }.v, mounts[0].contentId.v);

        // The now-inactive workspace's content is unmounted (NOT removed — the
        // tab still exists in next, so the registry keeps it alive).
        const auto unmounts = changesOfKind<ContentUnmounted>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), unmounts.size());
        VERIFY_ARE_EQUAL(TabId{ 100 }.v, unmounts[0].tabId.v);
        VERIFY_ARE_EQUAL(ContentId{ 901 }.v, unmounts[0].contentId.v);

        // No tab was removed: both tabs survive the switch, so the unmount is a
        // keep-alive detach, not a teardown.
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<TabRemoved>(ops));

        // And the active workspace flips to ws2.
        const auto active = changesOfKind<ActiveWorkspaceChanged>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), active.size());
        VERIFY_IS_TRUE(active[0].id.has_value());
        VERIFY_ARE_EQUAL(WorkspaceId{ 2 }.v, active[0].id->v);
    }

    void DiffTests::TabDecorationUpdated_TitleChanged()
    {
        auto tabPrev = makeTab(100);
        tabPrev.customTitle = "old title";
        auto tabNext = makeTab(100);
        tabNext.customTitle = "new title";

        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { tabPrev }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { tabNext }), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto decos = changesOfKind<TabDecorationUpdated>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), decos.size());
        VERIFY_ARE_EQUAL(TabId{ 100 }.v, decos[0].id.v);
        VERIFY_IS_TRUE(decos[0].customTitle == "new title");
        // Enriched payload: the owning workspace's stable id (workspace 1).
        // The view resolves this to the classic tab; no display index.
        VERIFY_ARE_EQUAL(WorkspaceId{ 1 }.v, decos[0].workspaceId.v);
    }

    // =====================================================================
    // Enriched-payload coverage
    // =====================================================================

    void DiffTests::LeafPaneCreated_InSplit_CarriesParentAxisRatio()
    {
        // prev: ws1 is a single leaf (id=10).
        // next: ws1 root is split(50, leaf10, newLeaf20) with a non-default
        //       orientation + ratio, so the carried fields are meaningful.
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        PaneNode leftLeaf = makeLeaf(10, { makeTab(100) });
        PaneNode rightLeaf = makeLeaf(20, { makeTab(200) });
        auto split = makeSplit(50, std::move(leftLeaf), std::move(rightLeaf), Axis::Horizontal, 0.3);
        auto next = makeState(
            { makeWs(1, PaneNode{ split }, PaneId{ 20 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);

        // The new sibling leaf carries its containing split's axis + ratio.
        const auto creates = changesOfKind<LeafPaneCreated>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), creates.size());
        VERIFY_ARE_EQUAL(PaneId{ 20 }.v, creates[0].id.v);
        VERIFY_IS_TRUE(creates[0].parent.has_value());
        VERIFY_ARE_EQUAL(PaneId{ 50 }.v, creates[0].parent->id.v);
        VERIFY_IS_TRUE(creates[0].parent->axis == Axis::Horizontal);
        VERIFY_ARE_EQUAL(0.3, creates[0].parent->ratio);

        // The new sibling's tab must be flagged as living inside a split so
        // the view skips opening an additional classic tab for it — and the
        // rest of its payload must still be correct on that special path.
        const auto adds = changesOfKind<TabAdded>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), adds.size());
        VERIFY_ARE_EQUAL(TabId{ 200 }.v, adds[0].id.v);
        VERIFY_IS_TRUE(adds[0].leafInsideSplit);
        VERIFY_ARE_EQUAL(WorkspaceId{ 1 }.v, adds[0].owningWorkspace.v);
        VERIFY_IS_TRUE(std::holds_alternative<TerminalSpec>(adds[0].description));
        VERIFY_IS_TRUE(std::get<TerminalSpec>(adds[0].description) == spec(200));
    }

    void DiffTests::TabAdded_SecondWorkspace_CarriesOwningWorkspace()
    {
        // Two workspaces; add a new tab to the SECOND workspace's leaf. The
        // owner must be resolved to ws 2 — a single-workspace fixture would
        // pass even if emitTabAdds mis-attributed the owner.
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }),
              makeWs(2, makeLeaf(20, { makeTab(200) }), PaneId{ 20 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }),
              makeWs(2, makeLeaf(20, { makeTab(200), makeTab(201) }, 1), PaneId{ 20 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto adds = changesOfKind<TabAdded>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), adds.size());
        VERIFY_ARE_EQUAL(TabId{ 201 }.v, adds[0].id.v);
        VERIFY_ARE_EQUAL(WorkspaceId{ 2 }.v, adds[0].owningWorkspace.v);
        VERIFY_IS_FALSE(adds[0].leafInsideSplit);
    }

    void DiffTests::TabDecorationUpdated_SecondWorkspace_CarriesIndex()
    {
        // Two workspaces; the decoration changes on a tab in the SECOND
        // workspace, so the carried id isn't the first workspace's. Proves
        // diff() routes by stable id, not by the trivial first slot.
        auto t200Prev = makeTab(200);
        t200Prev.customTitle = "old";
        auto t200Next = makeTab(200);
        t200Next.customTitle = "new";

        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }),
              makeWs(2, makeLeaf(20, { t200Prev }), PaneId{ 20 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }),
              makeWs(2, makeLeaf(20, { t200Next }), PaneId{ 20 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto decos = changesOfKind<TabDecorationUpdated>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), decos.size());
        VERIFY_ARE_EQUAL(TabId{ 200 }.v, decos[0].id.v);
        // Owning workspace is id 2, carried as a stable id (not a position).
        VERIFY_ARE_EQUAL(WorkspaceId{ 2 }.v, decos[0].workspaceId.v);
    }

    // #45/#44: the three arms that historically carried a raw display index
    // (WorkspaceAdded.position, ActiveWorkspaceChanged.index,
    // TabDecorationUpdated.workspaceIndex) now carry a stable WorkspaceId.
    // This fixture deliberately makes display order disagree with id order
    // (higher id at display index 0, lower id at display index 1) so a test
    // that accidentally still asserted a position would catch the wrong
    // value. Every assertion is on the id, independent of slot.
    void DiffTests::IdentityArms_CarryWorkspaceId_IndependentOfDisplayOrder()
    {
        // prev: a single workspace (id 99) at display index 0, active.
        auto prev = makeState(
            { makeWs(99, makeLeaf(990, { makeTab(9900) }), PaneId{ 990 }) },
            WorkspaceId{ 99 });

        // next: a NEW workspace (id 7) is inserted at display index 0, so it
        // sits BEFORE the pre-existing id-99 workspace (now at display index
        // 1). id 7 becomes the active workspace, and the id-99 tab is
        // re-decorated. Display order (7 then 99) deliberately does not match
        // creation/id order (99 then 7).
        auto t9900Next = makeTab(9900);
        t9900Next.customTitle = "renamed";
        auto next = makeState(
            { makeWs(7, makeLeaf(70, { makeTab(700) }), PaneId{ 70 }),
              makeWs(99, makeLeaf(990, { t9900Next }), PaneId{ 990 }) },
            WorkspaceId{ 7 });

        const auto ops = diff(prev, next);

        // WorkspaceAdded carries the new workspace's stable id (7), even
        // though it lives at display index 0.
        const auto adds = changesOfKind<WorkspaceAdded>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), adds.size());
        VERIFY_ARE_EQUAL(WorkspaceId{ 7 }.v, adds[0].id.v);

        // ActiveWorkspaceChanged carries the newly-active id (7), not a
        // display index.
        const auto setActive = changesOfKind<ActiveWorkspaceChanged>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), setActive.size());
        VERIFY_IS_TRUE(setActive[0].id.has_value());
        VERIFY_ARE_EQUAL(WorkspaceId{ 7 }.v, setActive[0].id->v);

        // TabDecorationUpdated for the id-99 tab carries the OWNING
        // workspace id (99), even though that workspace is now at display
        // index 1. A positional projection would have emitted 1 here.
        const auto decos = changesOfKind<TabDecorationUpdated>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), decos.size());
        VERIFY_ARE_EQUAL(TabId{ 9900 }.v, decos[0].id.v);
        VERIFY_ARE_EQUAL(WorkspaceId{ 99 }.v, decos[0].workspaceId.v);
    }

    // =====================================================================
    // Identity-keyed move tests
    // =====================================================================

    void DiffTests::TabMoved_CrossLeaf_SingleChange()
    {
        // ws1 has split(50, leaf10[t100], leaf20[t200]).
        // After the move, t100 has migrated from leaf10 to leaf20 at idx 1.
        auto leftPrev = makeLeaf(10, { makeTab(100) });
        auto rightPrev = makeLeaf(20, { makeTab(200) });
        auto splitPrev = makeSplit(50, PaneNode{ leftPrev }, PaneNode{ rightPrev });
        auto prev = makeState(
            { makeWs(1, PaneNode{ splitPrev }, PaneId{ 10 }) },
            WorkspaceId{ 1 });

        // next: leaf10 is gone (closed via the cascade); the split
        // collapses to leaf20 alone which now holds [t200, t100].
        // BUT — the spec specifically focuses on the *cross-leaf* case
        // where both leaves remain. Keep both leaves: leaf10 keeps a
        // different tab so it doesn't go empty.
        auto leftNext = makeLeaf(10, { makeTab(150) }); // fresh tab so leaf survives
        auto rightNext = makeLeaf(20, { makeTab(200), makeTab(100) });
        auto splitNext = makeSplit(50, PaneNode{ leftNext }, PaneNode{ rightNext });
        auto next = makeState(
            { makeWs(1, PaneNode{ splitNext }, PaneId{ 20 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);

        // The diffr must see TabId{100} in both states at different
        // (leafId, idx) and emit ONE TabMoved — never TabRemoved + TabAdded.
        std::size_t moveCount = 0;
        bool sawT100Move = false;
        for (const auto& op : ops)
        {
            if (const auto* m = std::get_if<TabMoved>(&op))
            {
                ++moveCount;
                if (m->id == TabId{ 100 })
                {
                    sawT100Move = true;
                    VERIFY_ARE_EQUAL(PaneId{ 10 }.v, m->srcLeafId.v);
                    VERIFY_ARE_EQUAL(PaneId{ 20 }.v, m->dstLeafId.v);
                    VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), m->dstIdx);
                }
            }
        }
        VERIFY_IS_TRUE(sawT100Move);
        // Tab 100 must not appear in TabRemoved or TabAdded.
        for (const auto& op : ops)
        {
            if (const auto* r = std::get_if<TabRemoved>(&op))
            {
                VERIFY_IS_FALSE(r->id == TabId{ 100 });
            }
            if (const auto* a = std::get_if<TabAdded>(&op))
            {
                VERIFY_IS_FALSE(a->id == TabId{ 100 });
            }
        }
        VERIFY_IS_TRUE(moveCount >= 1u);
    }

    void DiffTests::TabMoved_CrossWorkspace_SingleChange()
    {
        // ws1 has leaf10 [t100]; ws2 has leaf20 [t200].
        // next: leaf10 still exists with [t150]; leaf20 has [t200, t100].
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }),
              makeWs(2, makeLeaf(20, { makeTab(200) }), PaneId{ 20 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(150) }), PaneId{ 10 }),
              makeWs(2, makeLeaf(20, { makeTab(200), makeTab(100) }), PaneId{ 20 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);

        bool sawT100Move = false;
        for (const auto& op : ops)
        {
            if (const auto* m = std::get_if<TabMoved>(&op))
            {
                if (m->id == TabId{ 100 })
                {
                    sawT100Move = true;
                    VERIFY_ARE_EQUAL(PaneId{ 10 }.v, m->srcLeafId.v);
                    VERIFY_ARE_EQUAL(PaneId{ 20 }.v, m->dstLeafId.v);
                    VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), m->dstIdx);
                }
            }
        }
        VERIFY_IS_TRUE(sawT100Move);
        for (const auto& op : ops)
        {
            if (const auto* r = std::get_if<TabRemoved>(&op))
            {
                VERIFY_IS_FALSE(r->id == TabId{ 100 });
            }
            if (const auto* a = std::get_if<TabAdded>(&op))
            {
                VERIFY_IS_FALSE(a->id == TabId{ 100 });
            }
        }
    }

    // =====================================================================
    // Split preservation under wrap
    // =====================================================================

    void DiffTests::SplitWrap_PreservesOriginalLeafIdentity()
    {
        // prev: ws1 has a single leaf (paneId=10) with two tabs.
        auto prevLeaf = makeLeaf(10, { makeTab(100), makeTab(101) });
        auto prev = makeState(
            { makeWs(1, PaneNode{ prevLeaf }, PaneId{ 10 }) },
            WorkspaceId{ 1 });

        // next: a new split (id=50) wraps the original leaf10 unchanged
        // plus a new leaf20 with a single new tab (id=200).
        auto leftNext = makeLeaf(10, { makeTab(100), makeTab(101) });
        auto rightNext = makeLeaf(20, { makeTab(200) });
        auto splitNext = makeSplit(50, PaneNode{ leftNext }, PaneNode{ rightNext });
        auto next = makeState(
            { makeWs(1, PaneNode{ splitNext }, PaneId{ 20 }) },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);

        // Expectations:
        //   - One SplitPaneCreated{ id=50, left=10, right=20 }.
        //   - One LeafPaneCreated{ id=20, parent=50 }.
        //   - One TabAdded{ leafId=20, idx=0, id=200 }.
        //   - ZERO LeafPaneCreated for id=10.
        //   - ZERO TabRemoved for tab 100 or tab 101.
        //   - ZERO TabAdded for tab 100 or tab 101.
        //   - ZERO TabMoved for tab 100 or tab 101 (their (leafId, idx)
        //     is unchanged).

        const auto creates = changesOfKind<LeafPaneCreated>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), creates.size());
        VERIFY_ARE_EQUAL(PaneId{ 20 }.v, creates[0].id.v);

        const auto splits = changesOfKind<SplitPaneCreated>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), splits.size());
        VERIFY_ARE_EQUAL(PaneId{ 50 }.v, splits[0].id.v);
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, splits[0].left.v);
        VERIFY_ARE_EQUAL(PaneId{ 20 }.v, splits[0].right.v);

        // No leaf 10 in any Create op.
        for (const auto& op : ops)
        {
            if (const auto* c = std::get_if<LeafPaneCreated>(&op))
            {
                VERIFY_IS_FALSE(c->id == PaneId{ 10 });
            }
        }

        // No tab 100 or 101 in Add/Remove/Move.
        for (const auto& op : ops)
        {
            if (const auto* a = std::get_if<TabAdded>(&op))
            {
                VERIFY_IS_FALSE(a->id == TabId{ 100 });
                VERIFY_IS_FALSE(a->id == TabId{ 101 });
            }
            if (const auto* r = std::get_if<TabRemoved>(&op))
            {
                VERIFY_IS_FALSE(r->id == TabId{ 100 });
                VERIFY_IS_FALSE(r->id == TabId{ 101 });
            }
            if (const auto* m = std::get_if<TabMoved>(&op))
            {
                VERIFY_IS_FALSE(m->id == TabId{ 100 });
                VERIFY_IS_FALSE(m->id == TabId{ 101 });
            }
        }

        // The new sibling's tab is added.
        const auto adds = changesOfKind<TabAdded>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), adds.size());
        VERIFY_ARE_EQUAL(TabId{ 200 }.v, adds[0].id.v);
        VERIFY_ARE_EQUAL(PaneId{ 20 }.v, adds[0].leafId.v);
    }

    // =====================================================================
    // applyChanges wiring
    // =====================================================================

    void DiffTests::ApplyChanges_RecordsInOrder()
    {
        // Hand-construct a change sequence and feed it through
        // applyChanges; the mock should record the exact sequence we
        // provided.
        std::vector<WorkspaceChange> changes;
        changes.emplace_back(WorkspaceAdded{ WorkspaceId{ 7 }, "alpha", std::nullopt });
        changes.emplace_back(LeafPaneCreated{ PaneId{ 1 }, std::nullopt });
        changes.emplace_back(TabAdded{ PaneId{ 1 }, 0, TabId{ 11 }, "", std::nullopt, false });
        changes.emplace_back(ActiveWorkspaceChanged{ WorkspaceId{ 7 } });

        MockWorkspaceView view;
        applyChanges(view, std::span<const WorkspaceChange>{ changes });

        const auto recorded = view.recordedChanges();
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(4), recorded.size());

        VERIFY_IS_TRUE(std::holds_alternative<WorkspaceAdded>(recorded[0]));
        VERIFY_IS_TRUE(std::holds_alternative<LeafPaneCreated>(recorded[1]));
        VERIFY_IS_TRUE(std::holds_alternative<TabAdded>(recorded[2]));
        VERIFY_IS_TRUE(std::holds_alternative<ActiveWorkspaceChanged>(recorded[3]));

        // Spot-check field preservation.
        VERIFY_ARE_EQUAL(WorkspaceId{ 7 }.v, std::get<WorkspaceAdded>(recorded[0]).id.v);
        VERIFY_ARE_EQUAL(TabId{ 11 }.v, std::get<TabAdded>(recorded[2]).id.v);
        VERIFY_IS_TRUE(std::get<ActiveWorkspaceChanged>(recorded[3]).id.has_value());
        VERIFY_ARE_EQUAL(WorkspaceId{ 7 }.v, std::get<ActiveWorkspaceChanged>(recorded[3]).id->v);
    }

    // =====================================================================
    // Edge cases
    // =====================================================================

    void DiffTests::Diff_IdenticalStates_Empty()
    {
        auto s = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        const auto ops = diff(s, s);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), ops.size());
    }

    void DiffTests::Diff_NullPrev_ProducesAdds()
    {
        ModelState prev{}; // null shared_ptr
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        const auto ops = diff(prev, next);
        VERIFY_IS_TRUE(containsChange<WorkspaceAdded>(ops));
        VERIFY_IS_TRUE(containsChange<LeafPaneCreated>(ops));
        VERIFY_IS_TRUE(containsChange<TabAdded>(ops));
        VERIFY_IS_TRUE(containsChange<ActiveWorkspaceChanged>(ops));
    }

    // =====================================================================
    // Round-trip smoke tests using actions
    // =====================================================================

    void DiffTests::Roundtrip_SplitPane_EmitsExpectedShape()
    {
        // Sanity: diff + actions compose correctly. A
        // splitPane should emit exactly:
        //   - SplitPaneCreated (new split id)
        //   - LeafPaneCreated (new sibling leaf id)
        //   - TabAdded (new sibling's tab)
        //   - ActiveTabChanged or focus-related ops are also fine.
        // And critically NOT a LeafPaneCreated for the original leaf, nor
        // a TabRemoved/TabAdded for the original leaf's tab.
        auto f = WorkspaceModelUnitTests::makeSingleWorkspace();
        auto r = WorkspaceModel::splitPane(f.state, f.leafId, Axis::Vertical, 0.5, WorkspaceModelUnitTests::termSpec(2));

        const auto ops = diff(f.state, r.state);

        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), countChangesOfKind<SplitPaneCreated>(ops));
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), countChangesOfKind<LeafPaneCreated>(ops));
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), countChangesOfKind<TabAdded>(ops));

        for (const auto& op : ops)
        {
            if (const auto* c = std::get_if<LeafPaneCreated>(&op))
            {
                // The single LeafPaneCreated should be for the NEW sibling
                // pane, not the original.
                VERIFY_IS_FALSE(c->id == f.leafId);
            }
            if (const auto* a = std::get_if<TabAdded>(&op))
            {
                VERIFY_IS_FALSE(a->id == f.tabId);
            }
            if (const auto* rm = std::get_if<TabRemoved>(&op))
            {
                VERIFY_IS_FALSE(rm->id == f.tabId);
            }
        }
    }

    void DiffTests::Roundtrip_NewWorkspace_EmitsExpectedShape()
    {
        auto initial = WorkspaceModelUnitTests::emptyModel();
        auto r = WorkspaceModel::newWorkspace(initial, "alpha", WorkspaceModelUnitTests::termSpec(1));
        const auto ops = diff(initial, r.state);

        VERIFY_IS_TRUE(containsChange<WorkspaceAdded>(ops));
        VERIFY_IS_TRUE(containsChange<LeafPaneCreated>(ops));
        VERIFY_IS_TRUE(containsChange<TabAdded>(ops));
        VERIFY_IS_TRUE(containsChange<ActiveWorkspaceChanged>(ops));
    }
}
