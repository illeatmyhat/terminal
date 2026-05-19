// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// MockWorkspaceView — a test-only IWorkspaceView that records every
// change applied to it in arrival order. Tests assert on the recorded
// sequence.

#pragma once

#include "../WorkspaceModel/IWorkspaceView.h"
#include "../WorkspaceModel/WorkspaceChange.h"

#include <span>
#include <vector>

namespace WorkspaceModelUnitTests
{
    class MockWorkspaceView final : public WorkspaceModel::IWorkspaceView
    {
    public:
        void apply(const WorkspaceModel::WorkspaceAdded& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::WorkspaceRemoved& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::ActiveWorkspaceChanged& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::LeafPaneCreated& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::SplitPaneCreated& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::SplitPaneCollapsed& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::SplitRatioChanged& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::TabAdded& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::TabRemoved& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::TabMoved& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::ActiveTabChanged& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::ContentMounted& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::ContentUnmounted& c) override { _changes.emplace_back(c); }
        void apply(const WorkspaceModel::TabDecorationUpdated& c) override { _changes.emplace_back(c); }

        [[nodiscard]] std::span<const WorkspaceModel::WorkspaceChange> recordedChanges() const noexcept
        {
            return _changes;
        }

        void clear() noexcept { _changes.clear(); }

    private:
        std::vector<WorkspaceModel::WorkspaceChange> _changes{};
    };
}
