// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "BehavioralSimulator.h"

#include <unordered_set>
#include <utility>

using namespace WorkspaceModel;

namespace WorkspaceModelUnitTests
{
    namespace
    {
        // Collect leaves of a workspace's root pane.
        void collectLeavesLocal(const PaneNode& node, std::vector<const LeafPane*>& out)
        {
            std::visit(
                [&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, LeafPane>)
                    {
                        out.push_back(&n);
                    }
                    else
                    {
                        if (n.left)
                        {
                            collectLeavesLocal(*n.left, out);
                        }
                        if (n.right)
                        {
                            collectLeavesLocal(*n.right, out);
                        }
                    }
                },
                node);
        }

    } // namespace

    // ------------------------------------------------------------------
    Script::Script()
    {
        _state = std::make_shared<const WorkspaceModelData>();
        _snapshotState = _state;
    }

    WorkspaceId Script::workspaceFor(const std::string& label) const noexcept
    {
        auto it = _wsLabels.find(label);
        return (it == _wsLabels.end()) ? WorkspaceId{} : it->second;
    }
    PaneId Script::leafFor(const std::string& label) const noexcept
    {
        auto it = _paneLabels.find(label);
        return (it == _paneLabels.end()) ? PaneId{} : it->second;
    }
    TabId Script::tabFor(const std::string& label) const noexcept
    {
        auto it = _tabLabels.find(label);
        return (it == _tabLabels.end()) ? TabId{} : it->second;
    }

    void Script::runMutatorStep(std::function<void()> step)
    {
        _steps.push_back(std::move(step));
    }

    // ------------------------------------------------------------------
    // Workspace mutators
    // ------------------------------------------------------------------

    Script& Script::newWorkspace(std::string label,
                                 std::string name,
                                 TabContent initialTab,
                                 std::string firstTabLabel)
    {
        return newWorkspace(std::move(label), std::move(name), std::move(initialTab),
                            std::move(firstTabLabel), label + ":root");
    }

    Script& Script::newWorkspace(std::string label,
                                 std::string name,
                                 TabContent initialTab,
                                 std::string firstTabLabel,
                                 std::string firstLeafLabel)
    {
        runMutatorStep([this,
                        label = std::move(label),
                        name = std::move(name),
                        initialTab = std::move(initialTab),
                        firstTabLabel = std::move(firstTabLabel),
                        firstLeafLabel = std::move(firstLeafLabel)]() mutable {
            auto r = WorkspaceModel::newWorkspace(_state, name, initialTab);
            _state = r.state;
            _wsLabels[label] = r.id;
            // newWorkspace creates exactly one leaf with exactly one tab —
            // identify it from the resulting state.
            const auto* ws = _state->workspace(r.id);
            if (ws && std::holds_alternative<LeafPane>(ws->root))
            {
                const auto& leaf = std::get<LeafPane>(ws->root);
                _paneLabels[firstLeafLabel] = leaf.id;
                if (!leaf.tabs.empty())
                {
                    _tabLabels[firstTabLabel] = leaf.tabs[0].id;
                }
            }
        });
        return *this;
    }

    Script& Script::closeWorkspace(std::string label)
    {
        runMutatorStep([this, label = std::move(label)]() {
            _state = WorkspaceModel::closeWorkspace(_state, workspaceFor(label));
        });
        return *this;
    }

    Script& Script::closeOtherWorkspaces(std::string keepLabel)
    {
        runMutatorStep([this, keepLabel = std::move(keepLabel)]() {
            _state = WorkspaceModel::closeOtherWorkspaces(_state, workspaceFor(keepLabel));
        });
        return *this;
    }

    Script& Script::closeAllWorkspaces()
    {
        runMutatorStep([this]() {
            _state = WorkspaceModel::closeAllWorkspaces(_state);
        });
        return *this;
    }

    Script& Script::switchToWorkspace(std::string label)
    {
        runMutatorStep([this, label = std::move(label)]() {
            _state = WorkspaceModel::switchToWorkspace(_state, workspaceFor(label));
        });
        return *this;
    }

    Script& Script::renameWorkspace(std::string label, std::string name)
    {
        runMutatorStep([this, label = std::move(label), name = std::move(name)]() {
            _state = WorkspaceModel::renameWorkspace(_state, workspaceFor(label), name);
        });
        return *this;
    }

    Script& Script::setWorkspaceColor(std::string label, std::optional<Color> color)
    {
        runMutatorStep([this, label = std::move(label), color]() {
            _state = WorkspaceModel::setWorkspaceColor(_state, workspaceFor(label), color);
        });
        return *this;
    }

    Script& Script::setWorkspaceDescription(std::string label, std::string description)
    {
        runMutatorStep([this, label = std::move(label), description = std::move(description)]() {
            _state = WorkspaceModel::setWorkspaceDescription(_state, workspaceFor(label), description);
        });
        return *this;
    }

    Script& Script::setWorkspacePinned(std::string label, bool pinned)
    {
        runMutatorStep([this, label = std::move(label), pinned]() {
            _state = WorkspaceModel::setWorkspacePinned(_state, workspaceFor(label), pinned);
        });
        return *this;
    }

    Script& Script::reorderWorkspace(std::string label, std::size_t dstIdx)
    {
        runMutatorStep([this, label = std::move(label), dstIdx]() {
            _state = WorkspaceModel::reorderWorkspace(_state, workspaceFor(label), dstIdx);
        });
        return *this;
    }

    // ------------------------------------------------------------------
    // Tab mutators
    // ------------------------------------------------------------------

    Script& Script::newTab(std::string newTabLabel,
                           std::string workspaceLabel,
                           std::string leafLabel,
                           TabContent description,
                           std::string customTitle,
                           std::optional<Color> color,
                           bool pinned)
    {
        runMutatorStep([this,
                        newTabLabel = std::move(newTabLabel),
                        workspaceLabel = std::move(workspaceLabel),
                        leafLabel = std::move(leafLabel),
                        description = std::move(description),
                        customTitle = std::move(customTitle),
                        color,
                        pinned]() mutable {
            const auto wsId = workspaceFor(workspaceLabel);
            const auto leafId = leafFor(leafLabel);
            auto r = WorkspaceModel::newTab(_state, wsId, leafId,
                                            std::move(description),
                                            std::move(customTitle),
                                            color, pinned);
            _state = r.state;
            if (r.id.valid())
            {
                _tabLabels[newTabLabel] = r.id;
            }
        });
        return *this;
    }

    Script& Script::closeTab(std::string tabLabel)
    {
        runMutatorStep([this, tabLabel = std::move(tabLabel)]() {
            _state = WorkspaceModel::closeTab(_state, tabFor(tabLabel));
        });
        return *this;
    }
    Script& Script::closeTabsRight(std::string tabLabel)
    {
        runMutatorStep([this, tabLabel = std::move(tabLabel)]() {
            _state = WorkspaceModel::closeTabsRight(_state, tabFor(tabLabel));
        });
        return *this;
    }
    Script& Script::closeOtherTabs(std::string tabLabel)
    {
        runMutatorStep([this, tabLabel = std::move(tabLabel)]() {
            _state = WorkspaceModel::closeOtherTabs(_state, tabFor(tabLabel));
        });
        return *this;
    }
    Script& Script::selectTab(std::string tabLabel)
    {
        runMutatorStep([this, tabLabel = std::move(tabLabel)]() {
            _state = WorkspaceModel::selectTab(_state, tabFor(tabLabel));
        });
        return *this;
    }
    Script& Script::setTabTitle(std::string tabLabel, std::string title)
    {
        runMutatorStep([this, tabLabel = std::move(tabLabel), title = std::move(title)]() {
            _state = WorkspaceModel::setTabTitle(_state, tabFor(tabLabel), title);
        });
        return *this;
    }
    Script& Script::setTabColor(std::string tabLabel, std::optional<Color> color)
    {
        runMutatorStep([this, tabLabel = std::move(tabLabel), color]() {
            _state = WorkspaceModel::setTabColor(_state, tabFor(tabLabel), color);
        });
        return *this;
    }
    Script& Script::setTabPinned(std::string tabLabel, bool pinned)
    {
        runMutatorStep([this, tabLabel = std::move(tabLabel), pinned]() {
            _state = WorkspaceModel::setTabPinned(_state, tabFor(tabLabel), pinned);
        });
        return *this;
    }

    // ------------------------------------------------------------------
    // Pane mutators
    // ------------------------------------------------------------------

    Script& Script::splitPane(std::string leafLabel,
                              Axis axis,
                              double ratio,
                              TabContent newTabDescription,
                              std::string newLeafLabel,
                              std::string newTabLabel)
    {
        runMutatorStep([this,
                        leafLabel = std::move(leafLabel),
                        axis,
                        ratio,
                        newTabDescription = std::move(newTabDescription),
                        newLeafLabel = std::move(newLeafLabel),
                        newTabLabel = std::move(newTabLabel)]() mutable {
            const auto leafId = leafFor(leafLabel);
            auto r = WorkspaceModel::splitPane(_state, leafId, axis, ratio,
                                               std::move(newTabDescription));
            _state = r.state;
            if (r.newPaneId.valid())
            {
                _paneLabels[newLeafLabel] = r.newPaneId;
            }
            if (r.newTabId.valid())
            {
                _tabLabels[newTabLabel] = r.newTabId;
            }
        });
        return *this;
    }

    Script& Script::closePane(std::string leafLabel)
    {
        runMutatorStep([this, leafLabel = std::move(leafLabel)]() {
            _state = WorkspaceModel::closePane(_state, leafFor(leafLabel));
        });
        return *this;
    }
    Script& Script::resizePane(std::string splitLabel, double ratio)
    {
        runMutatorStep([this, splitLabel = std::move(splitLabel), ratio]() {
            // splitLabel may be looked up in either pane label map; we
            // store split ids in _paneLabels under their assigned name.
            _state = WorkspaceModel::resizePane(_state, leafFor(splitLabel), ratio);
        });
        return *this;
    }
    Script& Script::focusPane(std::string leafLabel)
    {
        runMutatorStep([this, leafLabel = std::move(leafLabel)]() {
            _state = WorkspaceModel::focusPane(_state, leafFor(leafLabel));
        });
        return *this;
    }

    // ------------------------------------------------------------------
    // Move mutators
    // ------------------------------------------------------------------

    Script& Script::moveTab(std::string tabLabel, std::string dstLeafLabel, std::size_t dstIdx)
    {
        runMutatorStep([this,
                        tabLabel = std::move(tabLabel),
                        dstLeafLabel = std::move(dstLeafLabel),
                        dstIdx]() {
            _state = WorkspaceModel::moveTab(_state, tabFor(tabLabel), leafFor(dstLeafLabel), dstIdx);
        });
        return *this;
    }

    Script& Script::moveTabAsSplit(std::string tabLabel,
                                   std::string dstLeafLabel,
                                   Edge edge,
                                   std::string newLeafLabel)
    {
        runMutatorStep([this,
                        tabLabel = std::move(tabLabel),
                        dstLeafLabel = std::move(dstLeafLabel),
                        edge,
                        newLeafLabel = std::move(newLeafLabel)]() {
            const auto tabId = tabFor(tabLabel);
            const auto dstLeafId = leafFor(dstLeafLabel);
            // Snapshot existing pane ids in the destination's workspace,
            // so we can identify the newly-created leaf after the call.
            std::unordered_set<std::uint64_t> beforeLeafIds;
            for (const auto& w : _state->workspaces)
            {
                std::vector<const LeafPane*> leaves;
                collectLeavesLocal(w.root, leaves);
                for (const auto* l : leaves)
                {
                    beforeLeafIds.insert(l->id.v);
                }
            }
            _state = WorkspaceModel::moveTabAsSplit(_state, tabId, dstLeafId, edge);
            // The new sibling leaf is the one with id not present before.
            for (const auto& w : _state->workspaces)
            {
                std::vector<const LeafPane*> leaves;
                collectLeavesLocal(w.root, leaves);
                for (const auto* l : leaves)
                {
                    if (beforeLeafIds.find(l->id.v) == beforeLeafIds.end())
                    {
                        _paneLabels[newLeafLabel] = l->id;
                        return;
                    }
                }
            }
        });
        return *this;
    }

    Script& Script::setSidebarWidth(double width)
    {
        runMutatorStep([this, width]() {
            _state = WorkspaceModel::setSidebarWidth(_state, width);
        });
        return *this;
    }

    // ------------------------------------------------------------------
    // Assertions
    // ------------------------------------------------------------------

    Script& Script::snapshot()
    {
        // Snapshot is recorded as a step so it interleaves correctly
        // with the mutator sequence at run() time.
        runMutatorStep([this]() {
            _snapshotState = _state;
        });
        return *this;
    }

    Script& Script::expect(std::function<bool(const WorkspaceModelData&)> predicate,
                           std::string msg)
    {
        runMutatorStep([this, predicate = std::move(predicate), msg = std::move(msg)]() {
            if (!_state || !predicate(*_state))
            {
                _failures.push_back("expect() failed: " + msg);
            }
        });
        return *this;
    }

    Script& Script::expectNoViolation()
    {
        runMutatorStep([this]() {
            if (_state)
            {
                const auto v = validate(*_state);
                if (v.has_value() && !_violation.has_value())
                {
                    _violation = v;
                    _failures.push_back("validator violation observed at expectNoViolation checkpoint");
                }
            }
        });
        return *this;
    }

    Script& Script::expectRenderOpsSinceLast(
        std::vector<std::function<bool(const RenderOp&)>> predicates)
    {
        runMutatorStep([this, predicates = std::move(predicates)]() {
            const auto ops = reconcile(_snapshotState, _state);
            if (ops.size() != predicates.size())
            {
                _failures.push_back("expectRenderOpsSinceLast: size mismatch (expected " +
                                    std::to_string(predicates.size()) + ", got " +
                                    std::to_string(ops.size()) + ")");
                return;
            }
            for (std::size_t i = 0; i < ops.size(); ++i)
            {
                if (!predicates[i](ops[i]))
                {
                    _failures.push_back("expectRenderOpsSinceLast: predicate " +
                                        std::to_string(i) + " failed");
                }
            }
        });
        return *this;
    }

    Script& Script::expectRenderOpSinceLast(
        std::function<bool(const RenderOp&)> predicate, std::string msg)
    {
        runMutatorStep([this, predicate = std::move(predicate), msg = std::move(msg)]() {
            const auto ops = reconcile(_snapshotState, _state);
            for (const auto& op : ops)
            {
                if (predicate(op))
                {
                    return;
                }
            }
            _failures.push_back("expectRenderOpSinceLast: no matching op (" + msg + ")");
        });
        return *this;
    }

    ModelState Script::run()
    {
        if (_ran)
        {
            return _state;
        }
        _ran = true;
        for (auto& step : _steps)
        {
            step();
            // After every step, capture the first validator violation we
            // observe — but don't bail; tests want to see the full
            // sequence's failure mode.
            if (!_violation.has_value() && _state)
            {
                if (const auto v = validate(*_state); v.has_value())
                {
                    _violation = v;
                    _failures.push_back("validator violation after mutator step");
                }
            }
        }
        return _state;
    }
}
