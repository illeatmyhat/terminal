// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceList.h"

#include "../../types/inc/utils.hpp"

namespace TerminalApp
{
    using winrt::Microsoft::Terminal::Settings::Model::WorkspacePlacementPolicy;

    std::vector<bool> WorkspaceList::_pinnedFlags() const
    {
        std::vector<bool> flags;
        flags.reserve(_state.workspaces.size());
        for (const auto& ws : _state.workspaces)
        {
            flags.push_back(ws.pinned);
        }
        return flags;
    }

    size_t WorkspaceList::Insert(WorkspaceState ws, WorkspacePlacementPolicy policy, bool activate)
    {
        const auto flags = _pinnedFlags();
        const auto index = WorkspacePlacement::ResolveInsertionIndex(policy, flags, _state.activeIndex, ws.pinned);
        _state.workspaces.insert(_state.workspaces.begin() + index, std::move(ws));

        // Inserting before/at the current active index shifts it.
        if (_state.activeIndex.has_value() && *_state.activeIndex >= index)
        {
            *_state.activeIndex += 1;
        }
        if (_state.previousActiveIndex.has_value() && *_state.previousActiveIndex >= index)
        {
            *_state.previousActiveIndex += 1;
        }

        if (activate)
        {
            Activate(index);
        }
        return index;
    }

    bool WorkspaceList::Remove(size_t index)
    {
        if (index >= _state.workspaces.size())
        {
            return false;
        }

        const bool wasActive = _state.activeIndex == index;
        _state.workspaces.erase(_state.workspaces.begin() + index);

        // Slide indices that pointed past the removed slot.
        auto adjust = [index](std::optional<size_t>& slot) {
            if (!slot.has_value())
            {
                return;
            }
            if (*slot == index)
            {
                slot.reset();
            }
            else if (*slot > index)
            {
                *slot -= 1;
            }
        };
        adjust(_state.activeIndex);
        adjust(_state.previousActiveIndex);

        // If we just removed the active workspace, fall back to a neighbor.
        if (wasActive && !_state.workspaces.empty())
        {
            // Prefer the workspace that took the freed slot; otherwise the
            // last workspace.
            const size_t fallback = std::min(index, _state.workspaces.size() - 1);
            _state.activeIndex = fallback;
        }

        return true;
    }

    void WorkspaceList::Activate(size_t index)
    {
        if (index >= _state.workspaces.size())
        {
            return;
        }
        if (_state.activeIndex == index)
        {
            return;
        }
        _state.previousActiveIndex = _state.activeIndex;
        _state.activeIndex = index;
    }

    std::optional<size_t> WorkspaceList::SelectLastActive()
    {
        if (!_state.previousActiveIndex.has_value())
        {
            return std::nullopt;
        }
        const auto target = *_state.previousActiveIndex;
        if (target >= _state.workspaces.size())
        {
            _state.previousActiveIndex.reset();
            return std::nullopt;
        }
        Activate(target);
        return _state.activeIndex;
    }

    std::optional<size_t> WorkspaceList::NextIndex() const noexcept
    {
        if (_state.workspaces.empty() || !_state.activeIndex.has_value())
        {
            return std::nullopt;
        }
        return (*_state.activeIndex + 1) % _state.workspaces.size();
    }

    std::optional<size_t> WorkspaceList::PrevIndex() const noexcept
    {
        if (_state.workspaces.empty() || !_state.activeIndex.has_value())
        {
            return std::nullopt;
        }
        const auto count = _state.workspaces.size();
        return (*_state.activeIndex + count - 1) % count;
    }

    size_t WorkspaceList::TogglePin(size_t index)
    {
        if (index >= _state.workspaces.size())
        {
            return index;
        }

        // Pull the workspace out of the list, flip its pin state, then
        // reinsert at the end of its new region. This preserves intra-region
        // relative ordering for the other workspaces.
        auto ws = std::move(_state.workspaces[index]);
        _state.workspaces.erase(_state.workspaces.begin() + index);

        // Track the active/previous indices through the removal.
        auto adjustForRemoval = [index](std::optional<size_t>& slot) -> std::optional<bool> {
            if (!slot.has_value())
            {
                return std::nullopt;
            }
            if (*slot == index)
            {
                return true; // marker: this slot pointed at the moved workspace
            }
            if (*slot > index)
            {
                *slot -= 1;
            }
            return false;
        };
        const auto activePointedAtMoved = adjustForRemoval(_state.activeIndex).value_or(false);
        const auto previousPointedAtMoved = adjustForRemoval(_state.previousActiveIndex).value_or(false);

        ws.pinned = !ws.pinned;

        const auto flags = _pinnedFlags();
        const auto pinnedCount = WorkspacePlacement::CountPinnedPrefix(flags);
        const size_t newIndex = ws.pinned ? pinnedCount : _state.workspaces.size();
        _state.workspaces.insert(_state.workspaces.begin() + newIndex, std::move(ws));

        // Anything that lived at or above newIndex shifted up by one. Apply
        // that shift, then restore the original active/previous if they
        // pointed at the moved workspace.
        auto adjustForInsertion = [newIndex](std::optional<size_t>& slot) {
            if (slot.has_value() && *slot >= newIndex)
            {
                *slot += 1;
            }
        };
        adjustForInsertion(_state.activeIndex);
        adjustForInsertion(_state.previousActiveIndex);

        if (activePointedAtMoved)
        {
            _state.activeIndex = newIndex;
        }
        if (previousPointedAtMoved)
        {
            _state.previousActiveIndex = newIndex;
        }

        return newIndex;
    }

    bool WorkspaceList::Move(size_t from, size_t to)
    {
        if (from >= _state.workspaces.size() || to >= _state.workspaces.size())
        {
            return false;
        }
        if (from == to)
        {
            return true;
        }
        // Pin-region invariant: a move can't cross the pinned/unpinned
        // boundary. Use TogglePin for that.
        if (_state.workspaces[from].pinned != _state.workspaces[to].pinned)
        {
            return false;
        }

        auto ws = std::move(_state.workspaces[from]);
        _state.workspaces.erase(_state.workspaces.begin() + from);
        // Inserting at `to` lands the workspace at index `to` whether the
        // move was forward or backward: if `to > from`, the erase shifted
        // everything past `from` down by one, so what was originally at
        // index `to` now sits at `to - 1` and inserting at `to` places the
        // moved workspace right after it. If `to < from`, no shift, and
        // inserting at `to` places it before the workspace originally at
        // that slot.
        _state.workspaces.insert(_state.workspaces.begin() + to, std::move(ws));

        // Adjust active/previous to follow the move.
        auto adjust = [from, to](std::optional<size_t>& slot) {
            if (!slot.has_value())
            {
                return;
            }
            const auto s = *slot;
            if (s == from)
            {
                *slot = to;
                return;
            }
            if (from < to)
            {
                if (s > from && s <= to)
                {
                    *slot = s - 1;
                }
            }
            else
            {
                if (s >= to && s < from)
                {
                    *slot = s + 1;
                }
            }
        };
        adjust(_state.activeIndex);
        adjust(_state.previousActiveIndex);
        return true;
    }

    WorkspaceList WorkspaceList::FromState(WorkspaceListState state)
    {
        WorkspaceList list;
        list._state = std::move(state);
        return list;
    }

    namespace
    {
        // Action keys / field keys, kept in sync with
        // `WorkspacePersistence` in TerminalSettingsModel.
        constexpr std::string_view kActionKey = "action";
        constexpr std::string_view kTitleKey = "title";
        constexpr std::string_view kColorKey = "color";
        constexpr std::string_view kDescriptionKey = "description";
        constexpr std::string_view kPinnedKey = "pinned";
        constexpr std::string_view kIdKey = "id";
        constexpr std::string_view kIndexKey = "index";
        constexpr std::string_view kTabLayoutKey = "tabLayout";
        constexpr std::string_view kSidebarWidthKey = "sidebarWidth";
        constexpr std::string_view kNewWorkspaceAction = "newWorkspace";
        constexpr std::string_view kSelectWorkspaceAction = "selectWorkspace";

        std::wstring jsonStringToWide(const Json::Value& v)
        {
            if (!v.isString())
            {
                return {};
            }
            const auto utf8 = v.asString();
            return winrt::to_hstring(utf8).c_str();
        }

        bool readWorkspaceHeader(const Json::Value& header, WorkspaceState& out)
        {
            if (!header.isObject())
            {
                return false;
            }

            if (const auto& title = header[Json::StaticString{ kTitleKey.data() }]; title.isString())
            {
                out.title = jsonStringToWide(title);
            }
            if (const auto& color = header[Json::StaticString{ kColorKey.data() }]; color.isString())
            {
                try
                {
                    const auto parsed = ::Microsoft::Console::Utils::ColorFromHexString(color.asString());
                    out.runtimeColor = static_cast<winrt::Windows::UI::Color>(parsed);
                }
                catch (...)
                {
                    // Bad color string: leave runtimeColor unset rather than
                    // failing the whole workspace.
                }
            }
            if (const auto& desc = header[Json::StaticString{ kDescriptionKey.data() }]; desc.isString())
            {
                out.customDescription = jsonStringToWide(desc);
            }
            if (const auto& pinned = header[Json::StaticString{ kPinnedKey.data() }]; pinned.isBool())
            {
                out.pinned = pinned.asBool();
            }
            if (const auto& id = header[Json::StaticString{ kIdKey.data() }]; id.isUInt64())
            {
                out.id = id.asUInt64();
            }
            else if (id.isUInt())
            {
                out.id = id.asUInt();
            }
            return true;
        }

        std::optional<WorkspaceListState> deserializeActions(const Json::Value& actions)
        {
            if (!actions.isArray())
            {
                return std::nullopt;
            }

            WorkspaceListState state;
            std::optional<size_t> activeIndex;
            bool inWorkspace = false;
            WorkspaceState current;

            auto flushCurrent = [&]() {
                if (inWorkspace)
                {
                    state.workspaces.push_back(std::move(current));
                    current = WorkspaceState{};
                    inWorkspace = false;
                }
            };

            for (const auto& step : actions)
            {
                if (!step.isObject())
                {
                    return std::nullopt;
                }
                const auto& actionVal = step[Json::StaticString{ kActionKey.data() }];
                const std::string actionName = actionVal.isString() ? actionVal.asString() : std::string{};

                if (actionName == kNewWorkspaceAction)
                {
                    flushCurrent();
                    current = WorkspaceState{};
                    if (!readWorkspaceHeader(step, current))
                    {
                        return std::nullopt;
                    }
                    inWorkspace = true;
                }
                else if (actionName == kSelectWorkspaceAction)
                {
                    const auto& idxVal = step[Json::StaticString{ kIndexKey.data() }];
                    if (idxVal.isUInt())
                    {
                        activeIndex = idxVal.asUInt();
                    }
                    else if (idxVal.isInt() && idxVal.asInt() >= 0)
                    {
                        activeIndex = static_cast<size_t>(idxVal.asInt());
                    }
                }
                else
                {
                    if (!inWorkspace)
                    {
                        // Pane action with no preceding `newWorkspace`:
                        // synthesize a default workspace so a legacy-shaped
                        // blob still loads.
                        current = WorkspaceState{};
                        inWorkspace = true;
                    }
                    if (!current.paneTree.isArray())
                    {
                        current.paneTree = Json::Value{ Json::arrayValue };
                    }
                    current.paneTree.append(step);
                }
            }
            flushCurrent();

            if (activeIndex.has_value() && *activeIndex < state.workspaces.size())
            {
                state.activeIndex = activeIndex;
            }
            else if (!state.workspaces.empty())
            {
                state.activeIndex = 0;
            }

            return state;
        }
    }

    std::optional<WorkspaceListState> DeserializeWorkspaceLayoutBlob(const winrt::hstring& blob)
    {
        if (blob.empty())
        {
            return std::nullopt;
        }

        const auto utf8 = winrt::to_string(blob);

        Json::CharReaderBuilder rb;
        std::string errs;
        Json::Value root;
        std::unique_ptr<Json::CharReader> reader{ rb.newCharReader() };
        if (!reader->parse(utf8.data(), utf8.data() + utf8.size(), &root, &errs))
        {
            return std::nullopt;
        }

        // The persisted `workspaceLayout` field today is a FLAT action array
        // (what `WorkspaceMigration::MigrateLegacyTabLayout` produces, since
        // that's the path `WindowLayout::MigrateLegacyTabLayoutToWorkspace
        // Layout()` writes). The future "live `_workspaces` write" path may
        // emit the wrapped `{tabLayout: [...], sidebarWidth: N}` shape that
        // `WorkspacePersistence::SerializeWindowLayout` produces. Accept
        // both — sidebar width flows through `WindowLayout::SidebarWidth`
        // independently today, so the wrapper-side sidebarWidth is purely
        // additive when present.
        if (root.isArray())
        {
            return deserializeActions(root);
        }
        if (!root.isObject())
        {
            return std::nullopt;
        }

        auto state = deserializeActions(root[Json::StaticString{ kTabLayoutKey.data() }]);
        if (!state.has_value())
        {
            return std::nullopt;
        }

        if (const auto& w = root[Json::StaticString{ kSidebarWidthKey.data() }]; w.isNumeric())
        {
            state->sidebarWidth = w.asDouble();
        }

        return state;
    }
}
