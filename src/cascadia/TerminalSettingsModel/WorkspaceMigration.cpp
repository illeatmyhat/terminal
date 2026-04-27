// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceMigration.h"
#include "WorkspacePersistence.h"

namespace
{
    constexpr std::string_view ActionKey = "action";
    constexpr std::string_view NewTabAction = "newTab";
    constexpr std::string_view TabTitleKey = "tabTitle";
    constexpr std::string_view TabColorKey = "tabColor";
    constexpr std::string_view TitleKey = "title";
    constexpr std::string_view ColorKey = "color";
}

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    bool WorkspaceMigration::IsLegacyShape(const Json::Value& actions) noexcept
    {
        if (!actions.isArray())
        {
            return false;
        }
        for (const auto& step : actions)
        {
            if (!step.isObject())
            {
                continue;
            }
            const auto& actionVal = step[std::string{ ActionKey }];
            if (!actionVal.isString())
            {
                continue;
            }
            const auto name = actionVal.asString();
            if (name == WorkspacePersistence::NewWorkspaceAction ||
                name == WorkspacePersistence::SelectWorkspaceAction)
            {
                return false;
            }
        }
        // No workspace-specific actions seen; treat as legacy.
        return true;
    }

    Json::Value WorkspaceMigration::MigrateLegacyTabLayout(const Json::Value& legacyActions)
    {
        Json::Value migrated{ Json::arrayValue };
        if (!legacyActions.isArray())
        {
            return migrated;
        }

        for (const auto& step : legacyActions)
        {
            if (!step.isObject())
            {
                continue;
            }
            const auto& actionVal = step[std::string{ ActionKey }];
            const std::string actionName = actionVal.isString() ? actionVal.asString() : std::string{};

            if (actionName == NewTabAction)
            {
                Json::Value workspaceHeader{ Json::objectValue };
                workspaceHeader[std::string{ ActionKey }] = std::string{ WorkspacePersistence::NewWorkspaceAction };

                // Hoist the tab's title and color into workspace-level fields.
                if (const auto& title = step[std::string{ TabTitleKey }]; title.isString())
                {
                    workspaceHeader[std::string{ TitleKey }] = title;
                }
                if (const auto& color = step[std::string{ TabColorKey }]; !color.isNull())
                {
                    workspaceHeader[std::string{ ColorKey }] = color;
                }
                migrated.append(std::move(workspaceHeader));

                // Preserve the rest of the newTab args (profile, commandline,
                // startingDirectory, etc) as the first pane-tree action of
                // the new workspace by re-emitting it. The replay layer
                // already knows how to spawn the initial pane from a newTab
                // action targeted inside a workspace; keeping it ensures the
                // first pane runs the same profile in the same directory.
                migrated.append(step);
            }
            else
            {
                migrated.append(step);
            }
        }

        return migrated;
    }

    Json::Value WorkspaceMigration::MigrateWindowLayoutIfLegacy(const Json::Value& windowLayoutObj)
    {
        if (!windowLayoutObj.isObject())
        {
            return windowLayoutObj;
        }

        const auto& tabLayout = windowLayoutObj[std::string{ WorkspacePersistence::TabLayoutKey }];
        if (!tabLayout.isArray())
        {
            return windowLayoutObj;
        }
        if (!IsLegacyShape(tabLayout))
        {
            return windowLayoutObj;
        }

        Json::Value migrated = windowLayoutObj;
        migrated[std::string{ WorkspacePersistence::TabLayoutKey }] = MigrateLegacyTabLayout(tabLayout);
        return migrated;
    }
}
