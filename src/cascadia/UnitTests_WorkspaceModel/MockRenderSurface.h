// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// MockRenderSurface — a test-only IRenderSurface that records every op
// applied to it in arrival order. Tests assert on the recorded sequence.

#pragma once

#include "../WorkspaceModel/IRenderSurface.h"
#include "../WorkspaceModel/RenderOp.h"

#include <span>
#include <vector>

namespace WorkspaceModelUnitTests
{
    class MockRenderSurface final : public WorkspaceModel::IRenderSurface
    {
    public:
        void apply(const WorkspaceModel::AddWorkspace& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::RemoveWorkspace& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::SetActiveWorkspace& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::CreateLeafPane& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::CreateSplitPane& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::CollapseSplitPane& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::SetSplitRatio& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::AddTab& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::RemoveTab& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::MoveTab& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::SetActiveTab& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::MountContent& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::UnmountContent& op) override { _ops.emplace_back(op); }
        void apply(const WorkspaceModel::UpdateTabDecoration& op) override { _ops.emplace_back(op); }

        [[nodiscard]] std::span<const WorkspaceModel::RenderOp> recordedOps() const noexcept
        {
            return _ops;
        }

        void clear() noexcept { _ops.clear(); }

    private:
        std::vector<WorkspaceModel::RenderOp> _ops{};
    };
}
