// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Tests for reconcile() and applyOps().
//
// The tests hand-construct ModelState fixture pairs (prev, next) directly,
// without going through the Slice 2 mutators, so the reconciler is exercised
// in isolation. A small handful of "round trip" tests at the bottom use the
// mutators as a sanity check that mutator + reconciler compose correctly.

#include "pch.h"

#include "MockRenderSurface.h"
#include "TestHelpers.h"

#include "../WorkspaceModel/IRenderSurface.h"
#include "../WorkspaceModel/Mutators.h"
#include "../WorkspaceModel/Reconciler.h"
#include "../WorkspaceModel/RenderOp.h"

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
        // the Slice 2 mutators here so reconciler bugs aren't masked by
        // mutator bugs.
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
        // Filtering helpers for tests that want to look at a specific op
        // kind without caring about ordering of other ops.
        // -----------------------------------------------------------------

        template<typename Op>
        std::vector<Op> opsOfKind(const std::vector<RenderOp>& ops)
        {
            std::vector<Op> out;
            for (const auto& op : ops)
            {
                if (const auto* p = std::get_if<Op>(&op))
                {
                    out.push_back(*p);
                }
            }
            return out;
        }

        template<typename Op>
        std::size_t countOpsOfKind(const std::vector<RenderOp>& ops)
        {
            return opsOfKind<Op>(ops).size();
        }

        // True iff `ops` contains an op of variant arm T.
        template<typename Op>
        bool containsOp(const std::vector<RenderOp>& ops)
        {
            return countOpsOfKind<Op>(ops) > 0;
        }
    }

    class ReconcilerTests
    {
        TEST_CLASS(ReconcilerTests);

        // ---- Per-arm tests (one fixture pair triggers exactly one arm) ----
        TEST_METHOD(AddWorkspace_FromEmpty);
        TEST_METHOD(RemoveWorkspace_ToEmpty);
        TEST_METHOD(SetActiveWorkspace_Changed);
        TEST_METHOD(CreateLeafPane_NewWorkspace);
        TEST_METHOD(CreateSplitPane_WrapLeaf);
        TEST_METHOD(CollapseSplitPane_ChildRemoved);
        TEST_METHOD(SetSplitRatio_Changed);
        TEST_METHOD(AddTab_NewTabInSurvivingLeaf);
        TEST_METHOD(RemoveTab_TabGoneFromSurvivingLeaf);
        TEST_METHOD(MoveTab_SameLeafReorder);
        TEST_METHOD(SetActiveTab_SurvivingLeaf);
        TEST_METHOD(MountContent_FirstMount);
        TEST_METHOD(UnmountContent_OnTabRemove);
        TEST_METHOD(UpdateTabDecoration_TitleChanged);

        // ---- Identity-keyed move detection (the load-bearing case) ----
        TEST_METHOD(MoveTab_CrossLeaf_SingleOp);
        TEST_METHOD(MoveTab_CrossWorkspace_SingleOp);

        // ---- Split preservation under wrap ----
        TEST_METHOD(SplitWrap_PreservesOriginalLeafIdentity);

        // ---- applyOps wiring ----
        TEST_METHOD(ApplyOps_RecordsInOrder);

        // ---- Empty / no-op edge cases ----
        TEST_METHOD(Reconcile_IdenticalStates_Empty);
        TEST_METHOD(Reconcile_NullPrev_ProducesAdds);

        // ---- Round-trip smoke tests using mutators ----
        TEST_METHOD(Roundtrip_SplitPane_EmitsExpectedShape);
        TEST_METHOD(Roundtrip_NewWorkspace_EmitsExpectedShape);
    };

    // =====================================================================
    // Per-arm tests
    // =====================================================================

    void ReconcilerTests::AddWorkspace_FromEmpty()
    {
        auto prev = emptyState();
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }, "alpha") },
            WorkspaceId{ 1 });

        const auto ops = reconcile(prev, next);
        const auto adds = opsOfKind<AddWorkspace>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), adds.size());
        VERIFY_ARE_EQUAL(WorkspaceId{ 1 }.v, adds[0].id.v);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), adds[0].position);
    }

    void ReconcilerTests::RemoveWorkspace_ToEmpty()
    {
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        auto next = emptyState();

        const auto ops = reconcile(prev, next);
        const auto removes = opsOfKind<RemoveWorkspace>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), removes.size());
        VERIFY_ARE_EQUAL(WorkspaceId{ 1 }.v, removes[0].id.v);
    }

    void ReconcilerTests::SetActiveWorkspace_Changed()
    {
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }),
              makeWs(2, makeLeaf(20, { makeTab(200) }), PaneId{ 20 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }),
              makeWs(2, makeLeaf(20, { makeTab(200) }), PaneId{ 20 }) },
            WorkspaceId{ 2 });

        const auto ops = reconcile(prev, next);
        const auto setActive = opsOfKind<SetActiveWorkspace>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), setActive.size());
        VERIFY_IS_TRUE(setActive[0].id.has_value());
        VERIFY_ARE_EQUAL(WorkspaceId{ 2 }.v, setActive[0].id->v);
    }

    void ReconcilerTests::CreateLeafPane_NewWorkspace()
    {
        auto prev = emptyState();
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = reconcile(prev, next);
        const auto creates = opsOfKind<CreateLeafPane>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), creates.size());
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, creates[0].id.v);
        VERIFY_IS_FALSE(creates[0].parent.has_value());
    }

    void ReconcilerTests::CreateSplitPane_WrapLeaf()
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

        const auto ops = reconcile(prev, next);
        const auto splits = opsOfKind<CreateSplitPane>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), splits.size());
        VERIFY_ARE_EQUAL(PaneId{ 50 }.v, splits[0].id.v);
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, splits[0].left.v);
        VERIFY_ARE_EQUAL(PaneId{ 20 }.v, splits[0].right.v);
    }

    void ReconcilerTests::CollapseSplitPane_ChildRemoved()
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

        const auto ops = reconcile(prev, next);
        const auto collapses = opsOfKind<CollapseSplitPane>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), collapses.size());
        VERIFY_ARE_EQUAL(PaneId{ 50 }.v, collapses[0].removedSplit.v);
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, collapses[0].survivor.v);
    }

    void ReconcilerTests::SetSplitRatio_Changed()
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

        const auto ops = reconcile(prev, next);
        const auto ratios = opsOfKind<SetSplitRatio>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), ratios.size());
        VERIFY_ARE_EQUAL(PaneId{ 50 }.v, ratios[0].id.v);
        VERIFY_ARE_EQUAL(0.75, ratios[0].ratio);
    }

    void ReconcilerTests::AddTab_NewTabInSurvivingLeaf()
    {
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100), makeTab(101) }, 1), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = reconcile(prev, next);
        const auto adds = opsOfKind<AddTab>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), adds.size());
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, adds[0].leafId.v);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), adds[0].idx);
        VERIFY_ARE_EQUAL(TabId{ 101 }.v, adds[0].id.v);
    }

    void ReconcilerTests::RemoveTab_TabGoneFromSurvivingLeaf()
    {
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100), makeTab(101) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = reconcile(prev, next);
        const auto removes = opsOfKind<RemoveTab>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), removes.size());
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, removes[0].leafId.v);
        VERIFY_ARE_EQUAL(TabId{ 101 }.v, removes[0].id.v);
    }

    void ReconcilerTests::MoveTab_SameLeafReorder()
    {
        // prev: leaf 10 has tabs [100, 101].
        // next: leaf 10 has tabs [101, 100] (swap order).
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100), makeTab(101) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(101), makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = reconcile(prev, next);
        const auto moves = opsOfKind<MoveTab>(ops);
        // Both tabs moved positions; we expect two MoveTab ops (not
        // Remove+Add), and definitely no Remove/Add ops for these tabs.
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), moves.size());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countOpsOfKind<RemoveTab>(ops));
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), countOpsOfKind<AddTab>(ops));
    }

    void ReconcilerTests::SetActiveTab_SurvivingLeaf()
    {
        auto prev = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100), makeTab(101) }, 0), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100), makeTab(101) }, 1), PaneId{ 10 }) },
            WorkspaceId{ 1 });

        const auto ops = reconcile(prev, next);
        const auto sets = opsOfKind<SetActiveTab>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), sets.size());
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, sets[0].leafId.v);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), sets[0].idx);
    }

    void ReconcilerTests::MountContent_FirstMount()
    {
        auto leafPrev = makeLeaf(10, { makeTab(100) });
        auto tabNext = makeTab(100);
        tabNext.mount = ContentId{ 555 };
        auto leafNext = makeLeaf(10, { tabNext });

        auto prev = makeState({ makeWs(1, PaneNode{ leafPrev }, PaneId{ 10 }) }, WorkspaceId{ 1 });
        auto next = makeState({ makeWs(1, PaneNode{ leafNext }, PaneId{ 10 }) }, WorkspaceId{ 1 });

        const auto ops = reconcile(prev, next);
        const auto mounts = opsOfKind<MountContent>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), mounts.size());
        VERIFY_ARE_EQUAL(TabId{ 100 }.v, mounts[0].tabId.v);
        VERIFY_ARE_EQUAL(ContentId{ 555 }.v, mounts[0].contentId.v);
    }

    void ReconcilerTests::UnmountContent_OnTabRemove()
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

        const auto ops = reconcile(prev, next);
        const auto unmounts = opsOfKind<UnmountContent>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), unmounts.size());
        VERIFY_ARE_EQUAL(TabId{ 100 }.v, unmounts[0].tabId.v);
        VERIFY_ARE_EQUAL(ContentId{ 555 }.v, unmounts[0].contentId.v);
    }

    void ReconcilerTests::UpdateTabDecoration_TitleChanged()
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

        const auto ops = reconcile(prev, next);
        const auto decos = opsOfKind<UpdateTabDecoration>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), decos.size());
        VERIFY_ARE_EQUAL(TabId{ 100 }.v, decos[0].id.v);
        VERIFY_IS_TRUE(decos[0].customTitle == "new title");
    }

    // =====================================================================
    // Identity-keyed move tests
    // =====================================================================

    void ReconcilerTests::MoveTab_CrossLeaf_SingleOp()
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

        const auto ops = reconcile(prev, next);

        // The reconciler must see TabId{100} in both states at different
        // (leafId, idx) and emit ONE MoveTab — never RemoveTab + AddTab.
        std::size_t moveCount = 0;
        bool sawT100Move = false;
        for (const auto& op : ops)
        {
            if (const auto* m = std::get_if<MoveTab>(&op))
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
        // Tab 100 must not appear in RemoveTab or AddTab.
        for (const auto& op : ops)
        {
            if (const auto* r = std::get_if<RemoveTab>(&op))
            {
                VERIFY_IS_FALSE(r->id == TabId{ 100 });
            }
            if (const auto* a = std::get_if<AddTab>(&op))
            {
                VERIFY_IS_FALSE(a->id == TabId{ 100 });
            }
        }
        VERIFY_IS_TRUE(moveCount >= 1u);
    }

    void ReconcilerTests::MoveTab_CrossWorkspace_SingleOp()
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

        const auto ops = reconcile(prev, next);

        bool sawT100Move = false;
        for (const auto& op : ops)
        {
            if (const auto* m = std::get_if<MoveTab>(&op))
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
            if (const auto* r = std::get_if<RemoveTab>(&op))
            {
                VERIFY_IS_FALSE(r->id == TabId{ 100 });
            }
            if (const auto* a = std::get_if<AddTab>(&op))
            {
                VERIFY_IS_FALSE(a->id == TabId{ 100 });
            }
        }
    }

    // =====================================================================
    // Split preservation under wrap
    // =====================================================================

    void ReconcilerTests::SplitWrap_PreservesOriginalLeafIdentity()
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

        const auto ops = reconcile(prev, next);

        // Expectations:
        //   - One CreateSplitPane{ id=50, left=10, right=20 }.
        //   - One CreateLeafPane{ id=20, parent=50 }.
        //   - One AddTab{ leafId=20, idx=0, id=200 }.
        //   - ZERO CreateLeafPane for id=10.
        //   - ZERO RemoveTab for tab 100 or tab 101.
        //   - ZERO AddTab for tab 100 or tab 101.
        //   - ZERO MoveTab for tab 100 or tab 101 (their (leafId, idx)
        //     is unchanged).

        const auto creates = opsOfKind<CreateLeafPane>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), creates.size());
        VERIFY_ARE_EQUAL(PaneId{ 20 }.v, creates[0].id.v);

        const auto splits = opsOfKind<CreateSplitPane>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), splits.size());
        VERIFY_ARE_EQUAL(PaneId{ 50 }.v, splits[0].id.v);
        VERIFY_ARE_EQUAL(PaneId{ 10 }.v, splits[0].left.v);
        VERIFY_ARE_EQUAL(PaneId{ 20 }.v, splits[0].right.v);

        // No leaf 10 in any Create op.
        for (const auto& op : ops)
        {
            if (const auto* c = std::get_if<CreateLeafPane>(&op))
            {
                VERIFY_IS_FALSE(c->id == PaneId{ 10 });
            }
        }

        // No tab 100 or 101 in Add/Remove/Move.
        for (const auto& op : ops)
        {
            if (const auto* a = std::get_if<AddTab>(&op))
            {
                VERIFY_IS_FALSE(a->id == TabId{ 100 });
                VERIFY_IS_FALSE(a->id == TabId{ 101 });
            }
            if (const auto* r = std::get_if<RemoveTab>(&op))
            {
                VERIFY_IS_FALSE(r->id == TabId{ 100 });
                VERIFY_IS_FALSE(r->id == TabId{ 101 });
            }
            if (const auto* m = std::get_if<MoveTab>(&op))
            {
                VERIFY_IS_FALSE(m->id == TabId{ 100 });
                VERIFY_IS_FALSE(m->id == TabId{ 101 });
            }
        }

        // The new sibling's tab is added.
        const auto adds = opsOfKind<AddTab>(ops);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), adds.size());
        VERIFY_ARE_EQUAL(TabId{ 200 }.v, adds[0].id.v);
        VERIFY_ARE_EQUAL(PaneId{ 20 }.v, adds[0].leafId.v);
    }

    // =====================================================================
    // applyOps wiring
    // =====================================================================

    void ReconcilerTests::ApplyOps_RecordsInOrder()
    {
        // Hand-construct an op sequence and feed it through applyOps; the
        // mock should record the exact sequence we provided.
        std::vector<RenderOp> ops;
        ops.emplace_back(AddWorkspace{ WorkspaceId{ 7 }, "alpha", std::nullopt, 0 });
        ops.emplace_back(CreateLeafPane{ PaneId{ 1 }, std::nullopt });
        ops.emplace_back(AddTab{ PaneId{ 1 }, 0, TabId{ 11 }, "", std::nullopt, false });
        ops.emplace_back(SetActiveWorkspace{ WorkspaceId{ 7 } });

        MockRenderSurface surface;
        applyOps(surface, std::span<const RenderOp>{ ops });

        const auto recorded = surface.recordedOps();
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(4), recorded.size());

        VERIFY_IS_TRUE(std::holds_alternative<AddWorkspace>(recorded[0]));
        VERIFY_IS_TRUE(std::holds_alternative<CreateLeafPane>(recorded[1]));
        VERIFY_IS_TRUE(std::holds_alternative<AddTab>(recorded[2]));
        VERIFY_IS_TRUE(std::holds_alternative<SetActiveWorkspace>(recorded[3]));

        // Spot-check field preservation.
        VERIFY_ARE_EQUAL(WorkspaceId{ 7 }.v, std::get<AddWorkspace>(recorded[0]).id.v);
        VERIFY_ARE_EQUAL(TabId{ 11 }.v, std::get<AddTab>(recorded[2]).id.v);
        VERIFY_IS_TRUE(std::get<SetActiveWorkspace>(recorded[3]).id.has_value());
        VERIFY_ARE_EQUAL(WorkspaceId{ 7 }.v, std::get<SetActiveWorkspace>(recorded[3]).id->v);
    }

    // =====================================================================
    // Edge cases
    // =====================================================================

    void ReconcilerTests::Reconcile_IdenticalStates_Empty()
    {
        auto s = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        const auto ops = reconcile(s, s);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), ops.size());
    }

    void ReconcilerTests::Reconcile_NullPrev_ProducesAdds()
    {
        ModelState prev{}; // null shared_ptr
        auto next = makeState(
            { makeWs(1, makeLeaf(10, { makeTab(100) }), PaneId{ 10 }) },
            WorkspaceId{ 1 });
        const auto ops = reconcile(prev, next);
        VERIFY_IS_TRUE(containsOp<AddWorkspace>(ops));
        VERIFY_IS_TRUE(containsOp<CreateLeafPane>(ops));
        VERIFY_IS_TRUE(containsOp<AddTab>(ops));
        VERIFY_IS_TRUE(containsOp<SetActiveWorkspace>(ops));
    }

    // =====================================================================
    // Round-trip smoke tests using mutators
    // =====================================================================

    void ReconcilerTests::Roundtrip_SplitPane_EmitsExpectedShape()
    {
        // Sanity: reconciler + Slice 2 mutators compose correctly. A
        // splitPane should emit exactly:
        //   - CreateSplitPane (new split id)
        //   - CreateLeafPane (new sibling leaf id)
        //   - AddTab (new sibling's tab)
        //   - SetActiveTab or focus-related ops are also fine.
        // And critically NOT a CreateLeafPane for the original leaf, nor
        // a RemoveTab/AddTab for the original leaf's tab.
        auto f = WorkspaceModelUnitTests::makeSingleWorkspace();
        auto r = WorkspaceModel::splitPane(f.state, f.leafId, Axis::Vertical, 0.5, WorkspaceModelUnitTests::termSpec(2));

        const auto ops = reconcile(f.state, r.state);

        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), countOpsOfKind<CreateSplitPane>(ops));
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), countOpsOfKind<CreateLeafPane>(ops));
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), countOpsOfKind<AddTab>(ops));

        for (const auto& op : ops)
        {
            if (const auto* c = std::get_if<CreateLeafPane>(&op))
            {
                // The single CreateLeafPane should be for the NEW sibling
                // pane, not the original.
                VERIFY_IS_FALSE(c->id == f.leafId);
            }
            if (const auto* a = std::get_if<AddTab>(&op))
            {
                VERIFY_IS_FALSE(a->id == f.tabId);
            }
            if (const auto* rm = std::get_if<RemoveTab>(&op))
            {
                VERIFY_IS_FALSE(rm->id == f.tabId);
            }
        }
    }

    void ReconcilerTests::Roundtrip_NewWorkspace_EmitsExpectedShape()
    {
        auto initial = WorkspaceModelUnitTests::emptyModel();
        auto r = WorkspaceModel::newWorkspace(initial, "alpha", WorkspaceModelUnitTests::termSpec(1));
        const auto ops = reconcile(initial, r.state);

        VERIFY_IS_TRUE(containsOp<AddWorkspace>(ops));
        VERIFY_IS_TRUE(containsOp<CreateLeafPane>(ops));
        VERIFY_IS_TRUE(containsOp<AddTab>(ops));
        VERIFY_IS_TRUE(containsOp<SetActiveWorkspace>(ops));
    }
}
