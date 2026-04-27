/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- WorkspaceMigration.h

Abstract:
- Forward migration of a legacy WindowLayout.tabLayout (action sequence
  populated by the classic top-level tab-strip Windows Terminal) into the
  workspace-shaped action sequence consumed by WorkspacePersistence.
- The transformation is lossless 1:1: each top-level `newTab` becomes a
  `newWorkspace`, carrying the tab's title and runtime color into the
  workspace; the tab's pane/split actions are preserved verbatim inside the
  workspace's scope.
- The reverse migration (workspace-shaped → legacy) is intentionally absent;
  toggling the experimental flag back off discards workspace state per spec.
--*/
#pragma once

#include "WorkspaceState.h"

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    struct WorkspaceMigration
    {
        // Returns a workspace-shaped action array.
        static Json::Value MigrateLegacyTabLayout(const Json::Value& legacyActions);

        // Migrates a workspace-shaped WindowLayout if the input is legacy
        // tab-shaped; returns the input unchanged otherwise. Detection is
        // structural: if any top-level action is `newWorkspace`, the input is
        // already in the new shape.
        static Json::Value MigrateWindowLayoutIfLegacy(const Json::Value& windowLayoutObj);

        static bool IsLegacyShape(const Json::Value& actions) noexcept;
    };
}
