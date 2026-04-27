// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspacePersistence.h"

#include "JsonUtils.h"

using namespace ::Microsoft::Terminal::Settings::Model;

namespace
{
    constexpr std::string_view ActionKey = "action";
    constexpr std::string_view TitleKey = "title";
    constexpr std::string_view ColorKey = "color";
    constexpr std::string_view DescriptionKey = "description";
    constexpr std::string_view PinnedKey = "pinned";
    constexpr std::string_view IdKey = "id";
    constexpr std::string_view IndexKey = "index";
}

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    Json::Value WorkspacePersistence::_workspaceHeader(const WorkspaceState& ws)
    {
        Json::Value header{ Json::objectValue };
        header[JsonKey(ActionKey)] = std::string{ NewWorkspaceAction };
        if (!ws.title.empty())
        {
            JsonUtils::SetValueForKey(header, TitleKey, winrt::hstring{ ws.title });
        }
        if (ws.runtimeColor.has_value())
        {
            JsonUtils::SetValueForKey(header, ColorKey, *ws.runtimeColor);
        }
        if (!ws.customDescription.empty())
        {
            JsonUtils::SetValueForKey(header, DescriptionKey, winrt::hstring{ ws.customDescription });
        }
        if (ws.pinned)
        {
            header[JsonKey(PinnedKey)] = true;
        }
        if (ws.id != 0)
        {
            header[JsonKey(IdKey)] = static_cast<Json::UInt64>(ws.id);
        }
        return header;
    }

    bool WorkspacePersistence::_readWorkspaceHeader(const Json::Value& header, WorkspaceState& out)
    {
        if (!header.isObject())
        {
            return false;
        }

        winrt::hstring title;
        if (JsonUtils::GetValueForKey(header, TitleKey, title))
        {
            out.title = std::wstring{ title };
        }

        winrt::Windows::UI::Color color{};
        if (JsonUtils::GetValueForKey(header, ColorKey, color))
        {
            out.runtimeColor = color;
        }

        winrt::hstring description;
        if (JsonUtils::GetValueForKey(header, DescriptionKey, description))
        {
            out.customDescription = std::wstring{ description };
        }

        if (const auto& pinned = header[JsonKey(PinnedKey)]; pinned.isBool())
        {
            out.pinned = pinned.asBool();
        }

        if (const auto& id = header[JsonKey(IdKey)]; id.isUInt64())
        {
            out.id = id.asUInt64();
        }
        else if (id.isUInt())
        {
            out.id = id.asUInt();
        }

        return true;
    }

    Json::Value WorkspacePersistence::SerializeActions(const WorkspaceListState& state)
    {
        Json::Value actions{ Json::arrayValue };

        for (const auto& ws : state.workspaces)
        {
            actions.append(_workspaceHeader(ws));
            if (ws.paneTree.isArray())
            {
                for (const auto& step : ws.paneTree)
                {
                    actions.append(step);
                }
            }
        }

        if (state.activeIndex.has_value() && *state.activeIndex < state.workspaces.size())
        {
            Json::Value selectActive{ Json::objectValue };
            selectActive[JsonKey(ActionKey)] = std::string{ SelectWorkspaceAction };
            selectActive[JsonKey(IndexKey)] = static_cast<Json::UInt>(*state.activeIndex);
            actions.append(std::move(selectActive));
        }

        return actions;
    }

    std::optional<WorkspaceListState> WorkspacePersistence::DeserializeActions(const Json::Value& actions)
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
            const auto& actionVal = step[JsonKey(ActionKey)];
            const std::string actionName = actionVal.isString() ? actionVal.asString() : std::string{};

            if (actionName == NewWorkspaceAction)
            {
                flushCurrent();
                current = WorkspaceState{};
                if (!_readWorkspaceHeader(step, current))
                {
                    return std::nullopt;
                }
                inWorkspace = true;
            }
            else if (actionName == SelectWorkspaceAction)
            {
                const auto& idxVal = step[JsonKey(IndexKey)];
                if (idxVal.isUInt())
                {
                    activeIndex = idxVal.asUInt();
                }
                else if (idxVal.isInt() && idxVal.asInt() >= 0)
                {
                    activeIndex = static_cast<size_t>(idxVal.asInt());
                }
                // selectWorkspace is allowed anywhere in the stream; we only
                // honor the most recent one (matching the spec's "the active
                // workspace at shutdown is captured as a final
                // selectWorkspace").
            }
            else
            {
                if (!inWorkspace)
                {
                    // Pane action with no preceding newWorkspace: treat as
                    // belonging to a synthesized default workspace so a
                    // legacy-shaped blob without an explicit newWorkspace
                    // still loads.
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

    Json::Value WorkspacePersistence::SerializeWindowLayout(const WorkspaceListState& state)
    {
        Json::Value root{ Json::objectValue };
        root[JsonKey(TabLayoutKey)] = SerializeActions(state);
        if (state.sidebarWidth.has_value())
        {
            root[JsonKey(SidebarWidthKey)] = *state.sidebarWidth;
        }
        return root;
    }

    std::optional<WorkspaceListState> WorkspacePersistence::DeserializeWindowLayout(const Json::Value& obj)
    {
        if (!obj.isObject())
        {
            return std::nullopt;
        }
        auto state = DeserializeActions(obj[JsonKey(TabLayoutKey)]);
        if (!state.has_value())
        {
            return std::nullopt;
        }
        if (const auto& w = obj[JsonKey(SidebarWidthKey)]; w.isNumeric())
        {
            state->sidebarWidth = w.asDouble();
        }
        return state;
    }
}
