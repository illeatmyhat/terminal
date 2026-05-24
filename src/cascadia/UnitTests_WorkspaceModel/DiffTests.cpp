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
        // Big-flip Slice E (#54): the ContentMounted arm now carries the
        // owning workspace's stable id (resolved from the tab's leaf the same
        // way the decoration path does) so the view can build a
        // workspace->contents reverse index for whole-workspace-close teardown.
        TEST_METHOD(Diff_ContentMounted_CarriesOwningWorkspace);
        TEST_METHOD(ContentUnmounted_OnTabRemove);
        // Phase 2 Slice 3 (#47): a workspace-switch-like prev->next where the
        // newly-active workspace's tab gains a mount (ContentMounted) and the
        // now-inactive workspace's tab loses its mount (ContentUnmounted) in
        // the SAME diff. This is the prev->next shape that drives the
        // ContentRegistry's mount/keep-alive lifecycle once S4 wires switching.
        TEST_METHOD(WorkspaceSwitch_MountsActive_UnmountsInactive);
        TEST_METHOD(TabDecorationUpdated_TitleChanged);

        // ---- Workspace metadata (#52): a surviving workspace's name / color /
        //      pin changed; diff projects it as WorkspaceMetadataUpdated. ----
        TEST_METHOD(WorkspaceMetadataUpdated_Rename);
        TEST_METHOD(WorkspaceMetadataUpdated_Recolor);
        TEST_METHOD(WorkspaceMetadataUpdated_Repin);
        TEST_METHOD(WorkspaceMetadataUpdated_NoOp_EmitsNothing);
        TEST_METHOD(WorkspaceAdded_CarriesPinned_NotMetadataUpdated);

        // ---- Pinned-float reorder: a surviving workspace's display order
        //      changed; diff projects it as WorkspaceReordered carrying the
        //      full new id-order. Pinning floats to the bottom of the pinned
        //      block; unpinning sinks to the top of the unpinned block. ----
        TEST_METHOD(Pin_MovesToBottomOfPinnedBlock);
        TEST_METHOD(Pin_SecondPin_GoesBelowFirstPinned);
        TEST_METHOD(Unpin_MovesToTopOfUnpinnedBlock);
        TEST_METHOD(Unpinned_RelativeOrderPreserved);
        TEST_METHOD(Diff_Pin_EmitsReorderArm);
        TEST_METHOD(Diff_Reorder_NoOp_EmitsNothing);
        TEST_METHOD(Diff_AddRemove_DoesNotEmitReorder);

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

        // ---- Workspace-switch projection guard (this slice): a pure
        //      switch is non-structural, so diff() must emit ONLY
        //      ActiveWorkspaceChanged — never a TabAdded/TabRemoved/
        //      WorkspaceRemoved that the view could mistake for a
        //      membership change. The view-level "show only the active
        //      workspace's tabs" reconcile is keyed off exactly this arm. ----
        TEST_METHOD(Diff_SwitchToWorkspace_EmitsOnlyActiveWorkspaceChanged);
        TEST_METHOD(Diff_NewWorkspace_EmitsActiveWorkspaceChangedToNew);

        // ---- Mount policy (Phase 2 Slice 1): actions now materialise the
        //      active workspace's active content (set TabRecord.mount), so
        //      the previously-unreachable ContentMounted/ContentUnmounted
        //      diff arms become reachable from real actions. The contract is
        //      option I (lifetime mount): a mount is allocated once and never
        //      reallocated, so a tab's ContentId is stable across switches and
        //      within-leaf tab switches (no teardown of live content). ----
        TEST_METHOD(NewWorkspace_ActiveTab_GetsMount);
        TEST_METHOD(Switch_BetweenMaterialised_MountsNothing);
        TEST_METHOD(SwitchBack_ReusesSameContentId);
        TEST_METHOD(SelectTab_WithinLeaf_RemountsSelected);
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

    void DiffTests::Diff_ContentMounted_CarriesOwningWorkspace()
    {
        // Action path: newWorkspace materialises the active tab (sets its
        // mount), so diff(empty -> new) emits a ContentMounted. That arm must
        // carry the NEW workspace's stable id as owningWorkspace — the field
        // the view keys its workspace->contents reverse index off so
        // apply(WorkspaceRemoved) can tear the content down on a whole-
        // workspace close.
        //
        // RED before Diff resolves it: owningWorkspace is default (zero), not
        // the workspace id. GREEN after: it equals the workspace's id.
        auto f = WorkspaceModelUnitTests::makeSingleWorkspace();
        auto initial = WorkspaceModelUnitTests::emptyModel();
        const auto ops = diff(initial, f.state);
        const auto mounts = changesOfKind<ContentMounted>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), mounts.size());
        VERIFY_ARE_EQUAL(f.tabId.v, mounts[0].tabId.v);
        VERIFY_IS_TRUE(mounts[0].owningWorkspace.valid(),
                       L"the ContentMounted arm must carry a valid owning workspace id");
        VERIFY_ARE_EQUAL(f.wsId.v, mounts[0].owningWorkspace.v,
                         L"owningWorkspace must be the workspace that owns the mounted tab");

        // Hand-built second-workspace variant: the mount appears on the SECOND
        // workspace's tab, so a diff that mis-attributed the owner to the
        // first slot would be caught. Mirrors TabAdded_SecondWorkspace_*.
        auto t200Next = makeTab(200);
        t200Next.mount = ContentId{ 902 };
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }),
              makeWs(2, makeLeaf(20, { makeTab(200) }), PaneId{ 20 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }),
              makeWs(2, makeLeaf(20, { t200Next }), PaneId{ 20 }) },
            WorkspaceId{ 1 });

        const auto ops2 = diff(prev, next);
        const auto mounts2 = changesOfKind<ContentMounted>(ops2);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), mounts2.size());
        VERIFY_ARE_EQUAL(TabId{ 200 }.v, mounts2[0].tabId.v);
        VERIFY_ARE_EQUAL(ContentId{ 902 }.v, mounts2[0].contentId.v);
        VERIFY_ARE_EQUAL(WorkspaceId{ 2 }.v, mounts2[0].owningWorkspace.v,
                         L"owningWorkspace is the SECOND workspace's id, not the first slot");
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
        // Exercises diff()'s mount-op MACHINERY in isolation. This test
        // hand-builds a prev->next pair of states (it runs NO action) where a
        // mount appears on one tab and is cleared on another, then asserts diff
        // emits the matching ContentMounted / ContentUnmounted ops. It is a
        // unit test of the diff arms, NOT a model of the action-level switch
        // contract: under the chosen lifetime mount contract (option I) a real
        // switch NEVER clears a mount (a mount is a stable lifetime id) and
        // emits only ActiveWorkspaceChanged — see Switch_BetweenMaterialised_
        // MountsNothing for the actual switch behaviour. The hand-built states
        // below deliberately violate that contract to drive diff's unmount arm.
        auto ws1TabPrev = makeTab(100);
        ws1TabPrev.mount = ContentId{ 901 }; // ws1 mounted in prev (hand-built)
        auto ws2TabPrev = makeTab(200); // ws2 has no mount in prev

        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { ws1TabPrev }), PaneId{ 10 }, "alpha"),
              makeWs(2, makeLeaf(20, { ws2TabPrev }), PaneId{ 20 }, "beta") },
            WorkspaceId{ 1 });

        auto ws1TabNext = makeTab(100); // mount removed in next (hand-built, to drive diff's unmount arm)
        auto ws2TabNext = makeTab(200);
        ws2TabNext.mount = ContentId{ 902 }; // ws2 gains a mount in next (hand-built)

        auto next = makeState(
            { makeWs(1, makeLeaf(10, { ws1TabNext }), PaneId{ 10 }, "alpha"),
              makeWs(2, makeLeaf(20, { ws2TabNext }), PaneId{ 20 }, "beta") },
            WorkspaceId{ 2 });

        const auto ops = diff(prev, next);

        // A tab that gained a mount between the two states yields ContentMounted.
        const auto mounts = changesOfKind<ContentMounted>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), mounts.size());
        VERIFY_ARE_EQUAL(TabId{ 200 }.v, mounts[0].tabId.v);
        VERIFY_ARE_EQUAL(ContentId{ 902 }.v, mounts[0].contentId.v);

        // A tab that lost a mount yields ContentUnmounted (NOT removed — the tab
        // still exists in next, so the registry keeps it alive).
        const auto unmounts = changesOfKind<ContentUnmounted>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), unmounts.size());
        VERIFY_ARE_EQUAL(TabId{ 100 }.v, unmounts[0].tabId.v);
        VERIFY_ARE_EQUAL(ContentId{ 901 }.v, unmounts[0].contentId.v);

        // No tab was removed: both tabs survive, so the unmount is a keep-alive
        // detach, not a teardown.
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<TabRemoved>(ops));

        // And the active workspace id change yields ActiveWorkspaceChanged(ws2).
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

    void DiffTests::WorkspaceMetadataUpdated_Rename()
    {
        // A surviving workspace renamed via the renameWorkspace action emits
        // exactly one WorkspaceMetadataUpdated with the new name; color/pinned
        // are unchanged and carried at their (default) values.
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }, "old name") },
            WorkspaceId{ 1 });
        auto next = renameWorkspace(prev, WorkspaceId{ 1 }, "new name");

        const auto ops = diff(prev, next);
        const auto meta = changesOfKind<WorkspaceMetadataUpdated>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), meta.size());
        VERIFY_ARE_EQUAL(WorkspaceId{ 1 }.v, meta[0].id.v);
        VERIFY_IS_TRUE(meta[0].name == "new name");
        VERIFY_IS_FALSE(meta[0].color.has_value());
        VERIFY_IS_FALSE(meta[0].pinned);
        // A rename is not an add/remove of the workspace.
        VERIFY_IS_FALSE(containsChange<WorkspaceAdded>(ops));
    }

    void DiffTests::WorkspaceMetadataUpdated_Recolor()
    {
        // setWorkspaceColor emits WorkspaceMetadataUpdated carrying the new
        // color (and the unchanged name).
        const Color red{ 200, 30, 30, 0xFF };
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }, "alpha") },
            WorkspaceId{ 1 });
        auto next = setWorkspaceColor(prev, WorkspaceId{ 1 }, red);

        const auto ops = diff(prev, next);
        const auto meta = changesOfKind<WorkspaceMetadataUpdated>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), meta.size());
        VERIFY_ARE_EQUAL(WorkspaceId{ 1 }.v, meta[0].id.v);
        VERIFY_IS_TRUE(meta[0].color.has_value());
        VERIFY_IS_TRUE(*meta[0].color == red);
        VERIFY_IS_TRUE(meta[0].name == "alpha");
    }

    void DiffTests::WorkspaceMetadataUpdated_Repin()
    {
        // setWorkspacePinned emits WorkspaceMetadataUpdated carrying the new
        // pinned value.
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }, "alpha") },
            WorkspaceId{ 1 });
        auto next = setWorkspacePinned(prev, WorkspaceId{ 1 }, true);

        const auto ops = diff(prev, next);
        const auto meta = changesOfKind<WorkspaceMetadataUpdated>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), meta.size());
        VERIFY_ARE_EQUAL(WorkspaceId{ 1 }.v, meta[0].id.v);
        VERIFY_IS_TRUE(meta[0].pinned);
    }

    void DiffTests::WorkspaceMetadataUpdated_NoOp_EmitsNothing()
    {
        // Identical workspace metadata in prev and next emits no
        // WorkspaceMetadataUpdated.
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }, "alpha") },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }, "alpha") },
            WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        VERIFY_IS_FALSE(containsChange<WorkspaceMetadataUpdated>(ops));
    }

    void DiffTests::WorkspaceAdded_CarriesPinned_NotMetadataUpdated()
    {
        // Creating a workspace is projected as WorkspaceAdded (now carrying
        // the initial pinned value), NOT WorkspaceMetadataUpdated.
        auto prev = emptyState();
        auto pinnedWs = makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }, "alpha");
        pinnedWs.pinned = true;
        auto next = makeState({ pinnedWs }, WorkspaceId{ 1 });

        const auto ops = diff(prev, next);
        const auto adds = changesOfKind<WorkspaceAdded>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), adds.size());
        VERIFY_ARE_EQUAL(WorkspaceId{ 1 }.v, adds[0].id.v);
        VERIFY_IS_TRUE(adds[0].pinned);
        VERIFY_IS_FALSE(containsChange<WorkspaceMetadataUpdated>(ops));
    }

    // =====================================================================
    // Pinned-float reorder
    //
    // Semantics (user-locked): the display order always satisfies "all pinned
    // before all unpinned". PIN(x) moves x to the END of the pinned block
    // (most-recently-pinned at the bottom of that block); UNPIN(x) moves x to
    // the START of the unpinned block. Unpinned workspaces keep their relative
    // order; they only shift to accommodate.
    // =====================================================================

    // Helper: the workspace ids of a state in display order.
    namespace
    {
        std::vector<WorkspaceId> orderOf(const ModelState& s)
        {
            std::vector<WorkspaceId> out;
            for (const auto& ws : s->workspaces)
            {
                out.push_back(ws.id);
            }
            return out;
        }
    }

    void DiffTests::Pin_MovesToBottomOfPinnedBlock()
    {
        // workspaces [a(1), b(2), c(3)], all unpinned. Pin c → it lands at the
        // end of the (empty) pinned block, i.e. the front: [c, a, b].
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        auto c = newWorkspace(b.state, "c", termSpec(3));
        VERIFY_IS_TRUE((orderOf(c.state) == std::vector<WorkspaceId>{ a.id, b.id, c.id }));

        auto next = setWorkspacePinned(c.state, c.id, true);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE((orderOf(next) == std::vector<WorkspaceId>{ c.id, a.id, b.id }));
        VERIFY_IS_TRUE(next->workspace(c.id)->pinned);
    }

    void DiffTests::Pin_SecondPin_GoesBelowFirstPinned()
    {
        // [a, b, c] unpinned. Pin a → [a, b, c] (a already at the front of an
        // empty pinned block). Then pin c → c goes BELOW a (most-recently-
        // pinned at the bottom of the pinned block): [a, c, b].
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        auto c = newWorkspace(b.state, "c", termSpec(3));

        auto s1 = setWorkspacePinned(c.state, a.id, true);
        VERIFY_IS_FALSE(validate(*s1).has_value());
        VERIFY_IS_TRUE((orderOf(s1) == std::vector<WorkspaceId>{ a.id, b.id, c.id }));

        auto s2 = setWorkspacePinned(s1, c.id, true);
        VERIFY_IS_FALSE(validate(*s2).has_value());
        // a stays at the top of the pinned block; c is the more-recently
        // pinned, so it sits below a but still before the unpinned b.
        VERIFY_IS_TRUE((orderOf(s2) == std::vector<WorkspaceId>{ a.id, c.id, b.id }));
    }

    void DiffTests::Unpin_MovesToTopOfUnpinnedBlock()
    {
        // Build [a(pinned), b(pinned), c, d] then unpin a → a sinks to the TOP
        // of the unpinned block (immediately after the last pinned b):
        // [b, a, c, d].
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        auto c = newWorkspace(b.state, "c", termSpec(3));
        auto d = newWorkspace(c.state, "d", termSpec(4));
        auto p1 = setWorkspacePinned(d.state, a.id, true); // [a, b, c, d]
        auto p2 = setWorkspacePinned(p1, b.id, true); // a pinned, then b pinned -> [a, b, c, d]
        VERIFY_IS_TRUE((orderOf(p2) == std::vector<WorkspaceId>{ a.id, b.id, c.id, d.id }));

        auto next = setWorkspacePinned(p2, a.id, false);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE((orderOf(next) == std::vector<WorkspaceId>{ b.id, a.id, c.id, d.id }));
        VERIFY_IS_FALSE(next->workspace(a.id)->pinned);
    }

    void DiffTests::Unpinned_RelativeOrderPreserved()
    {
        // [a, b, c, d] all unpinned. Pin b → [b, a, c, d]: a, c, d keep their
        // relative order among the unpinned block (a before c before d).
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        auto c = newWorkspace(b.state, "c", termSpec(3));
        auto d = newWorkspace(c.state, "d", termSpec(4));

        auto next = setWorkspacePinned(d.state, b.id, true);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE((orderOf(next) == std::vector<WorkspaceId>{ b.id, a.id, c.id, d.id }));
    }

    void DiffTests::Diff_Pin_EmitsReorderArm()
    {
        // Pinning the 3rd of [a, b, c] changes display order to [c, a, b];
        // diff emits exactly one WorkspaceReordered carrying that id-order,
        // AND a WorkspaceMetadataUpdated for the pin glyph/bool.
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        auto c = newWorkspace(b.state, "c", termSpec(3));
        auto next = setWorkspacePinned(c.state, c.id, true);

        const auto ops = diff(c.state, next);
        const auto reorders = changesOfKind<WorkspaceReordered>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), reorders.size());
        VERIFY_IS_TRUE((reorders[0].order == std::vector<WorkspaceId>{ c.id, a.id, b.id }));

        // The pin bool/glyph still rides its own metadata arm.
        const auto meta = changesOfKind<WorkspaceMetadataUpdated>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), meta.size());
        VERIFY_ARE_EQUAL(c.id.v, meta[0].id.v);
        VERIFY_IS_TRUE(meta[0].pinned);
    }

    void DiffTests::Diff_Reorder_NoOp_EmitsNothing()
    {
        // Identical display order in prev and next emits no WorkspaceReordered.
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        const auto ops = diff(b.state, b.state);
        VERIFY_IS_FALSE(containsChange<WorkspaceReordered>(ops));
    }

    void DiffTests::Diff_AddRemove_DoesNotEmitReorder()
    {
        // Appending a workspace (WorkspaceAdded) does not perturb the relative
        // order of the surviving ids, so no WorkspaceReordered is emitted.
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        const auto opsAdd = diff(a.state, b.state);
        VERIFY_IS_TRUE(containsChange<WorkspaceAdded>(opsAdd));
        VERIFY_IS_FALSE(containsChange<WorkspaceReordered>(opsAdd));

        // Removing a workspace likewise leaves the survivors in their relative
        // order — no spurious reorder.
        auto c = newWorkspace(b.state, "c", termSpec(3));
        auto removed = closeWorkspace(c.state, b.id);
        const auto opsRemove = diff(c.state, removed);
        VERIFY_IS_TRUE(containsChange<WorkspaceRemoved>(opsRemove));
        VERIFY_IS_FALSE(containsChange<WorkspaceReordered>(opsRemove));
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

    // =====================================================================
    // Workspace-switch projection guard (this slice)
    // =====================================================================

    // A pure switch between two existing workspaces is non-structural: no
    // tab is born or destroyed and no workspace is added or removed. diff()
    // must therefore emit EXACTLY ONE ActiveWorkspaceChanged carrying the
    // newly-active id, and nothing else that the view could mistake for a
    // membership change. The view's "show only the active workspace's tabs"
    // reconcile keys off this arm alone, so this pins the contract it relies
    // on: it can hide every other workspace's strip item precisely because a
    // switch never re-issues a TabAdded for the workspace being shown.
    void DiffTests::Diff_SwitchToWorkspace_EmitsOnlyActiveWorkspaceChanged()
    {
        // Two workspaces; ws1 (id 2) is active. Switch active back to ws0.
        auto initial = WorkspaceModelUnitTests::emptyModel();
        auto r0 = WorkspaceModel::newWorkspace(initial, "ws0", WorkspaceModelUnitTests::termSpec(1));
        auto r1 = WorkspaceModel::newWorkspace(r0.state, "ws1", WorkspaceModelUnitTests::termSpec(2));

        // newWorkspace makes the just-created workspace active, so ws1 is
        // active in `before`. Switching to ws0 is the only change.
        const auto before = r1.state;
        const auto after = WorkspaceModel::switchToWorkspace(before, r0.id);

        const auto ops = diff(before, after);

        // Exactly one ActiveWorkspaceChanged, carrying ws0's id.
        const auto setActive = changesOfKind<ActiveWorkspaceChanged>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), setActive.size());
        VERIFY_IS_TRUE(setActive[0].id.has_value());
        VERIFY_ARE_EQUAL(r0.id.v, setActive[0].id->v);

        // A switch is non-structural: nothing that smells like a membership
        // change may ride along.
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<TabAdded>(ops),
                         L"a switch must not add a tab");
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<TabRemoved>(ops),
                         L"a switch must not remove a tab");
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<TabMoved>(ops),
                         L"a switch must not move a tab");
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<WorkspaceAdded>(ops),
                         L"a switch must not add a workspace");
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<WorkspaceRemoved>(ops),
                         L"a switch must not remove a workspace");
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<LeafPaneCreated>(ops),
                         L"a switch must not create a leaf");

        // The whole change set is just the single switch.
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), ops.size(),
                         L"a pure switch is exactly one ActiveWorkspaceChanged");
    }

    // newWorkspace on a non-empty model is the OTHER path that drives the
    // switch reconcile: it both materialises a new tab AND makes that new
    // workspace active. diff() emits the additive arms (WorkspaceAdded ->
    // ... -> TabAdded) BEFORE the ActiveWorkspaceChanged{newId}, so the
    // view's TabAdded arm inserts the new strip item first and the trailing
    // ActiveWorkspaceChanged then hides the previously-active workspace's
    // item. This pins that phase ordering (additive before active-change).
    void DiffTests::Diff_NewWorkspace_EmitsActiveWorkspaceChangedToNew()
    {
        auto initial = WorkspaceModelUnitTests::emptyModel();
        auto r0 = WorkspaceModel::newWorkspace(initial, "ws0", WorkspaceModelUnitTests::termSpec(1));
        auto r1 = WorkspaceModel::newWorkspace(r0.state, "ws1", WorkspaceModelUnitTests::termSpec(2));

        const auto ops = diff(r0.state, r1.state);

        // The new workspace, its leaf and its tab are all added, and the
        // active workspace flips to the new id.
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), countChangesOfKind<WorkspaceAdded>(ops));
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), countChangesOfKind<LeafPaneCreated>(ops));
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), countChangesOfKind<TabAdded>(ops));

        const auto setActive = changesOfKind<ActiveWorkspaceChanged>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), setActive.size());
        VERIFY_IS_TRUE(setActive[0].id.has_value());
        VERIFY_ARE_EQUAL(r1.id.v, setActive[0].id->v,
                         L"newWorkspace makes the new workspace active");

        // Phase ordering the view relies on: every additive arm precedes the
        // ActiveWorkspaceChanged, so the new strip item exists before the
        // switch reconcile hides the old one.
        std::optional<std::size_t> activeIdx;
        std::size_t lastAdditiveIdx = 0;
        for (std::size_t i = 0; i < ops.size(); ++i)
        {
            if (std::holds_alternative<ActiveWorkspaceChanged>(ops[i]))
            {
                activeIdx = i;
            }
            else if (std::holds_alternative<WorkspaceAdded>(ops[i]) ||
                     std::holds_alternative<LeafPaneCreated>(ops[i]) ||
                     std::holds_alternative<TabAdded>(ops[i]))
            {
                lastAdditiveIdx = i;
            }
        }
        VERIFY_IS_TRUE(activeIdx.has_value());
        VERIFY_IS_TRUE(*activeIdx > lastAdditiveIdx,
                       L"WorkspaceAdded/TabAdded must precede ActiveWorkspaceChanged");
    }

    // =====================================================================
    // Mount policy (Phase 2 Slice 1)
    // =====================================================================

    namespace
    {
        // Resolve a tab's mount across the whole model by walking every
        // workspace's pane tree. Returns std::nullopt if the tab is unknown
        // or carries no mount.
        std::optional<ContentId> mountOf(const ModelState& state, TabId id)
        {
            if (!state)
            {
                return std::nullopt;
            }
            for (const auto& ws : state->workspaces)
            {
                std::vector<const LeafPane*> leaves;
                std::function<void(const PaneNode&)> walk = [&](const PaneNode& n) {
                    if (const auto* leaf = std::get_if<LeafPane>(&n))
                    {
                        leaves.push_back(leaf);
                        return;
                    }
                    const auto& sp = std::get<SplitPane>(n);
                    if (sp.left)
                    {
                        walk(*sp.left);
                    }
                    if (sp.right)
                    {
                        walk(*sp.right);
                    }
                };
                walk(ws.root);
                for (const auto* leaf : leaves)
                {
                    for (const auto& t : leaf->tabs)
                    {
                        if (t.id == id)
                        {
                            return t.mount;
                        }
                    }
                }
            }
            return std::nullopt;
        }
    }

    void DiffTests::NewWorkspace_ActiveTab_GetsMount()
    {
        // The mount policy materialises the new (active) workspace's active
        // tab, so its TabRecord.mount is set right after newWorkspace.
        auto f = WorkspaceModelUnitTests::makeSingleWorkspace();
        const auto mount = mountOf(f.state, f.tabId);
        VERIFY_IS_TRUE(mount.has_value(), L"newWorkspace must materialise the active tab");

        // And diff(empty -> new) emits a ContentMounted for that tab/content.
        auto initial = WorkspaceModelUnitTests::emptyModel();
        const auto ops = diff(initial, f.state);
        const auto mounts = changesOfKind<ContentMounted>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), mounts.size());
        VERIFY_ARE_EQUAL(f.tabId.v, mounts[0].tabId.v);
        VERIFY_ARE_EQUAL(mount->v, mounts[0].contentId.v);
    }

    void DiffTests::Switch_BetweenMaterialised_MountsNothing()
    {
        // Two workspaces. newWorkspace makes each active in turn and the
        // policy materialises each at creation, so by the time both exist
        // both active tabs are already mounted. A switch BACK to ws0 is then
        // a pure ActiveWorkspaceChanged: under the lifetime mount contract
        // (option I) content stays alive, so a switch between two
        // already-materialised workspaces mounts and unmounts NOTHING.
        auto initial = WorkspaceModelUnitTests::emptyModel();
        auto r0 = WorkspaceModel::newWorkspace(initial, "ws0", WorkspaceModelUnitTests::termSpec(1));
        auto r1 = WorkspaceModel::newWorkspace(r0.state, "ws1", WorkspaceModelUnitTests::termSpec(2));

        const auto ws0Tab = std::get<LeafPane>(r1.state->workspaces[0].root).tabs[0].id;
        const auto ws1Tab = std::get<LeafPane>(r1.state->workspaces[1].root).tabs[0].id;

        // Both active tabs are materialised in r1 (ws0 from its creation, ws1
        // from its creation), and ws1 is active.
        VERIFY_IS_TRUE(mountOf(r1.state, ws0Tab).has_value());
        VERIFY_IS_TRUE(mountOf(r1.state, ws1Tab).has_value());

        const auto before = r1.state;
        const auto after = WorkspaceModel::switchToWorkspace(before, r0.id);

        const auto ops = diff(before, after);

        // Exactly one ActiveWorkspaceChanged carrying ws0; no mount churn,
        // because both contents were already materialised and option I keeps
        // them alive across the switch.
        const auto active = changesOfKind<ActiveWorkspaceChanged>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), active.size());
        VERIFY_IS_TRUE(active[0].id.has_value());
        VERIFY_ARE_EQUAL(r0.id.v, active[0].id->v);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<ContentMounted>(ops),
                         L"switching to an already-materialised workspace re-mounts nothing");
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<ContentUnmounted>(ops),
                         L"option I keeps the previously-active content alive — no unmount");
    }

    void DiffTests::SwitchBack_ReusesSameContentId()
    {
        // THE load-bearing survival test. Switch away from ws0 and back; the
        // ContentId on ws0's active tab must be the SAME value both times, so
        // the view's EnsureMounted resolves the same live content (ConPTY /
        // scrollback survive). Under option I the mount is allocated once and
        // never reallocated, so this holds by construction.
        auto initial = WorkspaceModelUnitTests::emptyModel();
        auto r0 = WorkspaceModel::newWorkspace(initial, "ws0", WorkspaceModelUnitTests::termSpec(1));
        auto r1 = WorkspaceModel::newWorkspace(r0.state, "ws1", WorkspaceModelUnitTests::termSpec(2));

        const auto ws0Tab = std::get<LeafPane>(r1.state->workspaces[0].root).tabs[0].id;
        const auto before = mountOf(r1.state, ws0Tab);
        VERIFY_IS_TRUE(before.has_value());

        // Switch to ws0, then back to ws1, then back to ws0 again.
        auto s0 = WorkspaceModel::switchToWorkspace(r1.state, r0.id);
        const auto whileActive = mountOf(s0, ws0Tab);
        VERIFY_IS_TRUE(whileActive.has_value());
        VERIFY_ARE_EQUAL(before->v, whileActive->v, L"mount is stable when ws0 becomes active");

        auto s1 = WorkspaceModel::switchToWorkspace(s0, r1.id);
        const auto whileInactive = mountOf(s1, ws0Tab);
        VERIFY_IS_TRUE(whileInactive.has_value(),
                       L"option I keeps the mount on the now-inactive workspace");
        VERIFY_ARE_EQUAL(before->v, whileInactive->v);

        auto s2 = WorkspaceModel::switchToWorkspace(s1, r0.id);
        const auto andBack = mountOf(s2, ws0Tab);
        VERIFY_IS_TRUE(andBack.has_value());
        VERIFY_ARE_EQUAL(before->v, andBack->v,
                         L"ContentId must never change across an unmount->remount cycle");

        // A switch back to an already-materialised workspace emits no
        // ContentMounted (the view would otherwise build fresh content).
        const auto ops = diff(s1, s2);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<ContentMounted>(ops),
                         L"switch-back must not re-mount (would tear down + rebuild content)");
    }

    void DiffTests::SelectTab_WithinLeaf_RemountsSelected()
    {
        // A leaf with two tabs. The active tab is materialised; selecting the
        // sibling materialises the sibling while the originally-active tab
        // KEEPS its mount (option I: a leaf may hold multiple materialised
        // tabs so switching tabs within a pane never tears content down).
        // Re-selecting the first tab reuses its original ContentId.
        auto f = WorkspaceModelUnitTests::makeSingleWorkspace();
        auto added = WorkspaceModel::newTab(f.state, f.wsId, f.leafId,
                                            WorkspaceModelUnitTests::termSpec(2));

        // After newTab, the new tab is active and materialised; the first tab
        // retains the mount it got at workspace creation.
        const auto firstMountInitial = mountOf(added.state, f.tabId);
        const auto secondMount = mountOf(added.state, added.id);
        VERIFY_IS_TRUE(firstMountInitial.has_value(), L"first tab keeps its creation mount");
        VERIFY_IS_TRUE(secondMount.has_value(), L"the newly-active second tab is materialised");
        VERIFY_IS_FALSE(firstMountInitial->v == secondMount->v,
                        L"distinct tabs get distinct ContentIds");

        // Select the first tab again. It must reuse its ORIGINAL ContentId.
        auto selected = WorkspaceModel::selectTab(added.state, f.tabId);
        const auto firstMountReselected = mountOf(selected, f.tabId);
        VERIFY_IS_TRUE(firstMountReselected.has_value());
        VERIFY_ARE_EQUAL(firstMountInitial->v, firstMountReselected->v,
                         L"re-selecting a tab reuses its stable ContentId");

        // The second tab still carries its mount (kept alive while inactive).
        const auto secondStill = mountOf(selected, added.id);
        VERIFY_IS_TRUE(secondStill.has_value());
        VERIFY_ARE_EQUAL(secondMount->v, secondStill->v);

        // diff(added -> selected) is the within-leaf tab switch: it flips the
        // leaf's active tab (ActiveTabChanged) but mounts nothing new.
        const auto ops = diff(added.state, selected);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countChangesOfKind<ContentMounted>(ops),
                         L"a within-leaf tab switch must not re-mount either tab");
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), countChangesOfKind<ActiveTabChanged>(ops));
    }
}
