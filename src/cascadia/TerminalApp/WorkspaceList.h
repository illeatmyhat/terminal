/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- WorkspaceList.h

Abstract:
- Per-window ordered list of workspaces. Owns insertion (delegated to
  WorkspacePlacement), pin/unpin region sort, MRU tracking, and the
  next/prev/select-last navigation primitives.
- Lives in TerminalApp because this is runtime/UX state — it tracks the
  active workspace, MRU history, and sidebar width that have no presence
  in the on-disk persistence. The data carriers (WorkspaceState,
  WorkspaceListState) it operates on stay in TerminalSettingsModel since
  they are also the persistence-boundary type consumed by
  WorkspacePersistence and WorkspaceMigration.
--*/
#pragma once

#include "../TerminalSettingsModel/WorkspaceState.h"
#include "WorkspacePlacement.h"

namespace TerminalApp
{
    using WorkspaceState = winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceState;
    using WorkspaceListState = winrt::Microsoft::Terminal::Settings::Model::implementation::WorkspaceListState;

    struct WorkspaceList
    {
        WorkspaceList() = default;

        size_t Count() const noexcept { return _state.workspaces.size(); }
        std::optional<size_t> ActiveIndex() const noexcept { return _state.activeIndex; }
        std::optional<size_t> PreviousActiveIndex() const noexcept { return _state.previousActiveIndex; }
        std::optional<double> SidebarWidth() const noexcept { return _state.sidebarWidth; }
        void SidebarWidth(std::optional<double> width) noexcept { _state.sidebarWidth = width; }

        const WorkspaceState& At(size_t index) const { return _state.workspaces.at(index); }
        WorkspaceState& At(size_t index) { return _state.workspaces.at(index); }

        // Returns the resolved insertion index. The new workspace becomes the
        // active one only if explicitly requested; the caller's `activate`
        // flag mirrors the spec's "new workspace is auto-focused on creation".
        size_t Insert(WorkspaceState ws, winrt::Microsoft::Terminal::Settings::Model::WorkspacePlacementPolicy policy, bool activate);

        // Removes the workspace at the given index. If it was active, the
        // active selection shifts to a neighbor (next, falling back to
        // previous, or none if the list is empty). Returns true if removed.
        bool Remove(size_t index);

        // Sets the active workspace and rotates MRU history.
        void Activate(size_t index);

        // Switches to the previously-active workspace (cmux's MRU toggle).
        // Returns the new active index, or std::nullopt if there's no
        // previously-active workspace to swap with.
        std::optional<size_t> SelectLastActive();

        // Returns the index of the next/previous workspace in cyclic order,
        // or std::nullopt if the list is empty / has no active selection.
        std::optional<size_t> NextIndex() const noexcept;
        std::optional<size_t> PrevIndex() const noexcept;

        // Toggles the pin state and relocates the workspace to the end of
        // its new region. Returns the new index of the workspace.
        size_t TogglePin(size_t index);

        // Moves a workspace within its region. Returns true if the move was
        // legal (i.e. did not cross the pin/unpin boundary). Use TogglePin
        // for cross-region moves.
        bool Move(size_t from, size_t to);

        const WorkspaceListState& State() const noexcept { return _state; }
        WorkspaceListState& MutableState() noexcept { return _state; }

        static WorkspaceList FromState(WorkspaceListState state);

    private:
        WorkspaceListState _state;

        std::vector<bool> _pinnedFlags() const;
    };

    // Read-side counterpart to
    // `WorkspacePersistence::SerializeWindowLayout` (which lives in
    // TerminalSettingsModel because that's where the runtimeclass-projected
    // serializer is wired into `WindowLayout::MigrateLegacyTabLayoutToWork
    // spaceLayout`). The deserializer can't be reached from TerminalApp
    // through the projected `Microsoft.Terminal.Settings.Model.dll` surface
    // — its impl-namespace static method isn't dllexport — so we mirror its
    // logic here. Source-of-truth pairing is enforced by
    // `WorkspaceWindowLayoutIntegrationTests::WorkspaceLayoutRoundTripsThro
    // ughWindowLayout` in SettingsModel which round-trips through
    // `WorkspacePersistence`, plus this side has its own coverage in
    // `WorkspaceListTests::FromStatePreservesPersistedShape`. Returns
    // nullopt on malformed input so the cascade-endpoint (bootstrap) can
    // take over per the corruption-recovery contract.
    std::optional<WorkspaceListState> DeserializeWorkspaceLayoutBlob(const winrt::hstring& blob);
}
