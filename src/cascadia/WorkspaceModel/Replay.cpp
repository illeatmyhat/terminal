// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "Replay.h"

#include "Validator.h"

namespace WorkspaceModel
{
    namespace
    {
        // Apply one OpRecord against `state`. Returns the new state.
        // Throws if a mutator throws; the caller (replaySafe) catches.
        ModelState applyOne(ModelState state, const OpRecord& op)
        {
            return std::visit(
                [&state](const auto& rec) -> ModelState {
                    using T = std::decay_t<decltype(rec)>;
                    if constexpr (std::is_same_v<T, NewWorkspaceRecord>)
                    {
                        return newWorkspace(state,
                                            rec.name,
                                            rec.initialTab,
                                            rec.initialTabTitle,
                                            rec.initialTabColor,
                                            rec.initialTabPinned)
                            .state;
                    }
                    else if constexpr (std::is_same_v<T, CloseWorkspaceRecord>)
                    {
                        return closeWorkspace(state, rec.id);
                    }
                    else if constexpr (std::is_same_v<T, CloseOtherWorkspacesRecord>)
                    {
                        return closeOtherWorkspaces(state, rec.keep);
                    }
                    else if constexpr (std::is_same_v<T, CloseAllWorkspacesRecord>)
                    {
                        return closeAllWorkspaces(state);
                    }
                    else if constexpr (std::is_same_v<T, SwitchToWorkspaceRecord>)
                    {
                        return switchToWorkspace(state, rec.id);
                    }
                    else if constexpr (std::is_same_v<T, RenameWorkspaceRecord>)
                    {
                        return renameWorkspace(state, rec.id, rec.name);
                    }
                    else if constexpr (std::is_same_v<T, SetWorkspaceColorRecord>)
                    {
                        return setWorkspaceColor(state, rec.id, rec.color);
                    }
                    else if constexpr (std::is_same_v<T, SetWorkspaceDescriptionRecord>)
                    {
                        return setWorkspaceDescription(state, rec.id, rec.description);
                    }
                    else if constexpr (std::is_same_v<T, SetWorkspacePinnedRecord>)
                    {
                        return setWorkspacePinned(state, rec.id, rec.pinned);
                    }
                    else if constexpr (std::is_same_v<T, ReorderWorkspaceRecord>)
                    {
                        return reorderWorkspace(state, rec.id, rec.dstIdx);
                    }
                    else if constexpr (std::is_same_v<T, NewTabRecord>)
                    {
                        return newTab(state,
                                      rec.workspaceId,
                                      rec.leafId,
                                      rec.description,
                                      rec.customTitle,
                                      rec.runtimeColor,
                                      rec.pinned)
                            .state;
                    }
                    else if constexpr (std::is_same_v<T, CloseTabRecord>)
                    {
                        return closeTab(state, rec.id);
                    }
                    else if constexpr (std::is_same_v<T, CloseTabsRightRecord>)
                    {
                        return closeTabsRight(state, rec.id);
                    }
                    else if constexpr (std::is_same_v<T, CloseOtherTabsRecord>)
                    {
                        return closeOtherTabs(state, rec.id);
                    }
                    else if constexpr (std::is_same_v<T, SelectTabRecord>)
                    {
                        return selectTab(state, rec.id);
                    }
                    else if constexpr (std::is_same_v<T, SetTabTitleRecord>)
                    {
                        return setTabTitle(state, rec.id, rec.customTitle);
                    }
                    else if constexpr (std::is_same_v<T, SetTabColorRecord>)
                    {
                        return setTabColor(state, rec.id, rec.color);
                    }
                    else if constexpr (std::is_same_v<T, SetTabPinnedRecord>)
                    {
                        return setTabPinned(state, rec.id, rec.pinned);
                    }
                    else if constexpr (std::is_same_v<T, SplitPaneRecord>)
                    {
                        return splitPane(state,
                                         rec.leafId,
                                         rec.axis,
                                         rec.ratio,
                                         rec.newTabDescription,
                                         rec.newTabCustomTitle,
                                         rec.newTabColor,
                                         rec.newTabPinned)
                            .state;
                    }
                    else if constexpr (std::is_same_v<T, ClosePaneRecord>)
                    {
                        return closePane(state, rec.leafId);
                    }
                    else if constexpr (std::is_same_v<T, ResizePaneRecord>)
                    {
                        return resizePane(state, rec.splitId, rec.ratio);
                    }
                    else if constexpr (std::is_same_v<T, FocusPaneRecord>)
                    {
                        return focusPane(state, rec.leafId);
                    }
                    else if constexpr (std::is_same_v<T, MoveTabRecord>)
                    {
                        return moveTab(state, rec.tabId, rec.dstLeafId, rec.dstIdx);
                    }
                    else if constexpr (std::is_same_v<T, MoveTabAsSplitRecord>)
                    {
                        return moveTabAsSplit(state, rec.tabId, rec.dstLeafId, rec.edge);
                    }
                    else if constexpr (std::is_same_v<T, SetSidebarWidthRecord>)
                    {
                        return setSidebarWidth(state, rec.width);
                    }
                    else
                    {
                        static_assert(!std::is_same_v<T, T>, "applyOne() missing an OpRecord variant arm");
                        return state;
                    }
                },
                op);
        }
    } // namespace

    ModelState replay(ModelState start, const std::vector<LogEntry>& entries)
    {
        auto state = std::move(start);
        for (const auto& e : entries)
        {
            state = applyOne(state, e.op);
        }
        return state;
    }

    ReplayResult replaySafe(ModelState start, const std::vector<LogEntry>& entries)
    {
        ReplayResult out;
        auto state = std::move(start);
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            ModelState next;
            try
            {
                next = applyOne(state, entries[i].op);
            }
            catch (const std::exception& e)
            {
                out.state = state;
                out.error = std::string{ "mutator threw at seq=" } +
                            std::to_string(entries[i].seq) + ": " + e.what();
                out.entriesApplied = i;
                return out;
            }
            // Post-mutation invariant check. A model that fails validate()
            // is the renderer's worst-case scenario, so we halt and return
            // the previous state as the last-good model rather than
            // proceed past corruption.
            if (next == nullptr)
            {
                out.state = state;
                out.error = std::string{ "mutator returned null at seq=" } +
                            std::to_string(entries[i].seq);
                out.entriesApplied = i;
                return out;
            }
            if (const auto v = validate(*next); v.has_value())
            {
                out.state = state;
                out.error = std::string{ "mutator produced invalid state at seq=" } +
                            std::to_string(entries[i].seq);
                out.entriesApplied = i;
                return out;
            }
            state = std::move(next);
        }
        out.state = state;
        out.error = std::nullopt;
        out.entriesApplied = entries.size();
        return out;
    }
}
