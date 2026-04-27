---
author: Punleuk Oum
created on: 2026-04-26
last updated: 2026-04-26
issue id: TBD
---

# Workspaces

## Abstract

Windows Terminal today gives users a single tab strip at the top of the window, where each tab is one independently-running pane tree. As users have begun running long-lived background work — build watchers, log followers, AI coding agents, chat tools — inside their terminal, the limitations of a flat tab list have become apparent. There is no first-class way to keep many independent project contexts open side-by-side without losing them in a long row of tabs, no way to glance at which background processes are currently asking for attention, and no way to quickly switch between named contexts the way one switches between browser tabs.

This spec introduces *workspaces*, a new top-level navigation unit that sits above tabs and panes. Each workspace is an independently-running pane tree with its own working directory, color, title, and live status. Workspaces are listed in a left-edge sidebar that replaces the top tab strip; tabs continue to exist but live inside individual panes (each leaf pane has its own inline tab strip), allowing each pane to host an ordered list of terminal sessions, settings pages, markdown viewers, or any other `IPaneContent`. Background workspaces remain fully alive: their `ConPTY`s and `TermControl`s continue to run, output continues to be captured to scrollback, and bell events trigger an attention indicator on the sidebar row. A user running parallel agents in many workspaces can switch among them without interrupting any of their work.

The feature is inspired by [cmux](https://github.com/manaflow-ai/cmux), a macOS terminal that pioneered this layout for AI-agent workflows. Where Windows Terminal today is built around the assumption that the active tab is the primary unit of attention, cmux is built around the assumption that any number of independent contexts may need attention at any moment, and the user wants a glanceable index of all of them. This spec adopts the cmux model faithfully where it translates cleanly to WT's process model and WinUI/XAML stack, and explicitly defers the operations that don't (cross-window workspace moves, scrollback persistence) to follow-up specs.

The feature ships behind `experimental.workspaces.enabled` (default `false`). Users who do not enable it see no change. Users who enable it on first launch get their existing tabs migrated 1:1 into workspaces. Toggling the flag back off discards workspace state and resumes classic tab behavior; the flag is intended to become the default after the experimental period, at which point classic mode would be removed.

## Inspiration

cmux is a multi-window macOS terminal built on Ghostty whose chrome is dominated by a left-edge sidebar listing concurrent "workspaces." Each workspace owns a Bonsplit-managed split layout; each split-pane within a workspace owns its own tab strip; tabs within a pane can be terminals, browsers, or markdown viewers. The sidebar surfaces live status — title, working directory, attention indicator when an agent in a background workspace needs input, custom colors, pin state — so the user gets a single glance at every running context without context-switching through a top tab strip.

![cmux screenshot](img/cmux-screenshot.png)

The cmux author's stated north star is the tmux/tmux-resurrect experience: terminal sessions that survive restart with their pane layout, working directories, scrollback, and ideally their running commands all restored. cmux has implemented per-workspace persistence and restorable agent sessions, and is moving toward full session restore. This spec adopts cmux's architectural model now and notes the path toward tmux-resurrect-grade restore as future work.

## Solution Design

### Workspace as the new top-level navigation unit

`TerminalPage` gains an ordered list of workspaces, owned per-window. Each workspace is a standalone C++ object holding its own pane tree (a `std::shared_ptr<Pane>` rooted at the workspace, structured as today's `Pane`'s `_firstChild`/`_secondChild` tree), its own focus state, its title, custom color, custom description, pin state, and an internal id. The top `TabRowControl` is replaced by a left-edge sidebar listing workspaces, and a chrome strip across the top of the window holds an app menu, the new-workspace split button, the active workspace's title text, and the system buttons. The window has no per-window tab strip anymore; tabs live inside panes.

`TerminalPage` selects exactly one workspace as active at any moment. The active workspace's root pane element is mounted in a `ContentPresenter` in the main content area of the window; inactive workspaces are alive in memory (their `Pane`s, `IPaneContent`s, `TermControl`s, and `ConPTY`s all remain instantiated and running) but their root XAML element is not in the visual tree. This matches cmux's `WorkspaceMountPolicy`, which keeps `maxMountedWorkspaces = 1` in steady state to minimize layer-tree traversal overhead. Because `TermControl`'s renderer thread and ConPTY-pipe-reader are owned by the `TermControl` object rather than by its position in the visual tree, removing a workspace's root XAML from the tree does not interrupt any running work — agent processes continue writing to ConPTY, ConPTY continues feeding `TermControl`, `TermControl` continues appending to scrollback, and bell events continue to fire and update the workspace's attention indicator on the sidebar.

Workspace switching is implemented by clearing the `ContentPresenter` and reattaching the new workspace's root element. Focus is captured on deactivation (per-workspace last-focused-pane-and-tab) and restored on activation. The cost is approximately one `Loaded` event on the activating tree and one focus-restore traversal; sub-frame in observed cmux usage. A future polish pass can implement the "transition overlap" mode (briefly mounting both workspaces during the cycle, matching cmux's `maxMountedWorkspacesDuringCycle = 2`) without changing the architectural model.

### Tabs live inside panes, not above them

In classic Windows Terminal, a `Tab` owns a tree of `Pane`s and a leaf `Pane` hosts a single `IPaneContent`. In workspace mode, leaf `Pane`s instead host an ordered list of tabs. `Pane` gains a new internal struct `PaneTab` holding `winrt::TerminalApp::IPaneContent content`, `winrt::hstring customTitle`, `std::optional<winrt::Windows::UI::Color> runtimeColor`, `std::chrono::steady_clock::time_point lastFocused`, `uint32_t id`, and `bool pinned`. A leaf `Pane` adds `std::vector<PaneTab> _tabs`, `size_t _activeTabIndex`, and a `TabView` element that renders the tab strip header. Internal-node `Pane`s (those with `_firstChild` and `_secondChild`) are unchanged. Any `Pane` accessor that today returns `_content` becomes a method that returns the active tab's content (`_tabs[_activeTabIndex].content`); zoom, focus-revoking, settings cascade, and accessibility traversal each become a small per-site change.

`PaneTab` is a plain C++ struct, not a WinRT type. The rest of the app interacts with `Pane` through new methods (`AddTab`, `RemoveTab`, `MoveTab`, `ActivateTab`, `Tabs() -> span<const PaneTab>`) plus the existing accessor for the active content; `PaneTab` itself does not cross WinRT boundaries.

Heterogeneous content is supported. A single pane can hold a mix of `TermControl`-backed `IPaneContent`s, settings pages, markdown viewers, snippets pages, and any future content type, in any order. This matches both the existing `IPaneContent` polymorphism in WT and cmux's mixed terminal/browser/markdown panel model.

### Per-window ownership

Each Windows Terminal window owns its own `_workspaces` list. Opening a new window via `wt new-window` produces a fresh window with one bootstrap workspace; the two windows' workspace lists are independent. This mirrors cmux's `MainWindowContext` model (each window has its own `TabManager` with its own `tabs: [Workspace]` list) and matches Windows Terminal's existing per-window state invariant for tabs and panes.

cmux additionally supports cross-window operations — moving a workspace from one window to another via `moveWorkspaceToWindow`, or moving an individual surface across workspaces (potentially in different windows) via `moveSurface`. In cmux these operations are heap-cheap because all windows share one process. In Windows Terminal, where Process Model 2.0 places each window in a separate process, the same operations require cross-process IPC: serializing workspace state through the monarch, duplicating ConPTY pseudoconsole handles via `DuplicateHandle`, transferring `TextBuffer` contents (which are process-local), and reattaching to a fresh `TermControl` in the destination process. These are deferred to a separate spec; this proof of concept ships with workspaces strictly per-window.

### Persistence via action replay

Existing Windows Terminal persistence (`ApplicationState.h`, spec [#8324]) stores per-window state as `WindowLayout.TabLayout: IVector<ActionAndArgs>` — a sequence of actions that, when replayed at startup, reconstruct the tab and pane structure. Workspaces extend this vocabulary. New action types — `newWorkspace`, `selectWorkspace`, `setWorkspaceColor`, `setWorkspaceDescription`, `pinWorkspace`, `newPaneTab`, `selectPaneTab` — are added to the action map and emitted by the persistence writer. The reader replays them in order; nested boundaries between workspaces are implicit in the position of `newWorkspace` actions in the action sequence.

`WindowLayout` gains a new field `SidebarWidth` so the sidebar's width is restored. The previously active workspace (the last `selectWorkspace` in the sequence) becomes active on restore. Per-pane focus and per-pane active-tab are restored from the corresponding `focusPane` and `selectPaneTab` actions. ConPTYs are spawned eagerly across all restored workspaces in parallel, matching the existing eager-spawn behavior for tabs; users with many workspaces accept the cost in startup time in exchange for having all their concurrent contexts running immediately rather than only on activation.

A representative shape for the persisted action sequence:

```jsonc
{
  "persistedWindowLayouts": [{
    "initialPosition": "...",
    "initialSize": "...",
    "sidebarWidth": 240,
    "tabLayout": [
      { "action": "newWorkspace", "title": "cmux cli/unix socket", "color": "#3B82F6", "pinned": true },
        { "action": "splitPane", "split": "right", "size": 0.5 },
        { "action": "newPaneTab", "profile": "{powershell-guid}", "startingDirectory": "C:\\Users\\..." },
        { "action": "newPaneTab", "profile": "{wsl-guid}", "startingDirectory": "/home/..." },
        { "action": "selectPaneTab", "index": 1 },
        { "action": "focusPane", "id": 0 },
      { "action": "newWorkspace", "title": "ssh", "color": null, "pinned": false },
        { "action": "newPaneTab", "profile": "{ssh-guid}" },
      { "action": "selectWorkspace", "index": 0 }
    ]
  }]
}
```

Indentation in the example is illustrative only; the JSON is a flat array, with workspace boundaries implicit in the order of `newWorkspace` actions.

Persistence in this proof of concept does not include scrollback (consistent with WT today; ConPTYs spawn fresh and scrollback starts empty), running process state (no terminal can preserve in-flight processes across restart without OS-level support that does not exist on Windows), or unread/attention indicators (transient runtime state). The Future Considerations section sketches a path toward tmux-resurrect-grade restore.

When the experimental flag flips on at startup and the existing TSM blob is in the legacy tab-mode shape, a forward migration runs in-line: the reader synthesizes one `newWorkspace` action per legacy tab, with the tab's title and color carried into the workspace, and replays the tab's existing pane/split actions inside the workspace's scope. This is a lossless transformation (every tab becomes a workspace; pane structure and content are preserved) and matches the user's mental model of "my tabs become my workspaces." When the flag flips off, persisted workspace state is discarded and classic mode resumes from its own (possibly stale or empty) prior persistence state; running workspace processes are terminated. This is a deliberate design choice: the flag is intended to become the default after the experimental period, and the toggle-off path is the punishment switch for stepping back to legacy behavior, not a graceful round-trip.

### Action and keybinding catalog

New workspace-level actions are added to `AllShortcutActions.h` and the `ActionMap`: `newWorkspace`, `closeWorkspace`, `closeOtherWorkspaces`, `nextWorkspace`, `prevWorkspace`, `switchToWorkspace` (with index or id), `selectLastWorkspace` (MRU toggle between current and previously-active), `renameWorkspace`, `setWorkspaceColor`, `setWorkspaceDescription`, `togglePinWorkspace`, `moveWorkspace` (up/down), and `openNewWorkspaceDropdown`. Each gets a corresponding `*ActionArgs` IDL struct and a handler in `TerminalPage`.

Existing tab-related actions are retargeted in workspace mode. `newTab` continues to exist and continues to be triggerable by `Ctrl+Shift+T`, but its handler dispatches to "new PaneTab in the focused pane of the active workspace" rather than "new top-level tab." The same is true for `closeTab`, `nextTab`, `prevTab`, `switchToTab`, `duplicateTab`, and `openNewTabDropdown`. New explicit alias action ids — `newPaneTab`, `closePaneTab`, `nextPaneTab`, `prevPaneTab`, `switchToPaneTab`, `duplicatePaneTab`, `openNewPaneTabDropdown` — are registered against the same handlers so persisted replay JSON and command palette entries can use unambiguous names without forcing users to rebind their existing keychords. `splitPane`, `closePane`, `togglePaneZoom`, and `toggleSplitOrientation` are unchanged — they continue to operate on the pane structure within whatever tab tree they target, which is now the active workspace's tab tree.

Default keybindings in workspace mode shift to reflect the new top-level navigation. The principle is that chords that historically meant "switch among the top-level things" retarget to workspace-level operations.

| Chord | Classic mode | Workspace mode |
|---|---|---|
| `Ctrl+Shift+T` | newTab | newPaneTab (alias kicks in) |
| `Ctrl+Tab` | nextTab | nextWorkspace |
| `Ctrl+Shift+Tab` | prevTab | prevWorkspace |
| `Ctrl+Alt+1..8` | switchToTab N | switchToWorkspace N |
| `Ctrl+Alt+9` | switchToLastTab | selectLastWorkspace |
| `Ctrl+Shift+1..9` | newTab w/ profile N | newWorkspace w/ profile N |
| `Ctrl+Shift+D` | duplicateTab | duplicatePaneTab (alias) |
| `Ctrl+Shift+Space` | openNewTabDropdown | openNewWorkspaceDropdown |
| `Ctrl+PageUp` | (free) | prevPaneTab (new) |
| `Ctrl+PageDown` | (free) | nextPaneTab (new) |
| `Ctrl+Shift+Alt+W` | (free) | closeWorkspace |
| `Ctrl+Shift+Alt+P` | (free) | togglePinWorkspace |
| `Ctrl+Shift+Alt+Up` / `Down` | (free) | moveWorkspace up / down |
| `F2` (sidebar focused) | — | renameWorkspace |

All defaults are user-overridable via `keybindings` in `settings.json`.

Command palette integration reuses the existing `CommandPalette` control. Typing `@` in the palette filters to workspace names and Enter activates; this mirrors VS Code's symbol-prefix convention and Windows Terminal's existing tab switcher ([#1502]). The legacy tab switcher action retargets to PaneTab switching in the focused pane when workspaces is on; users who use the tab switcher today get a pane-tab switcher in workspace mode. The CLI gains `wt new-workspace` and `wt switch-workspace`; `wt new-tab` and `wt split-pane` continue to work, retargeted to operate on the active workspace's focused pane. A `--workspace` selector for cross-workspace CLI operations is deferred to follow-up work.

### Settings additions

The single mandatory new setting is `experimental.workspaces.enabled` (bool, default `false`). When `true` and the feature was not enabled at the previous launch, forward migration runs and the workspace UI takes over; when `true` and the previous launch was also workspace mode, persisted workspace state is restored as described above. Toggling the flag requires a Windows Terminal restart to take effect, matching the convention used by other invasive `compatibility.*` settings.

A secondary setting `experimental.workspaces.newWorkspacePlacement` (string, default `"afterCurrent"`, accepting `"top"`, `"afterCurrent"`, or `"end"`) controls where new workspaces are inserted in the sidebar list, mirroring cmux's `WorkspacePlacement` enum. Pinned workspaces always sort above unpinned and the placement setting only affects the unpinned region.

The sidebar width is persisted per-window inside `WindowLayout.SidebarWidth` rather than as a user-level setting; users who want a consistent width across all windows can set their initial width and rely on TSM to round-trip it.

## UI/UX Design

### Top chrome

The top of the window is a single thin horizontal strip approximately 32 pixels tall, integrating the system's title-bar drag region, the app's chrome controls, and the system min/max/close buttons. From left to right, the strip contains a hamburger menu button, a `+ New Workspace` split button whose chevron opens the profile picker, the active workspace's title text, a draggable region (the empty area surrounding the title text), and the standard Windows min/max/close buttons on the right. There is no separate title bar; the chrome strip serves both purposes, participating in the existing `Window.SetTitleBar` / `ExtendsContentIntoTitleBar` API that Windows Terminal already uses.

The active workspace's title is displayed alone in the strip's center — no `— Windows Terminal` suffix, no active-pane title prepended. cmux follows the same convention; the workspace title is often the working directory the workspace was initially opened in, which is sufficient context for window-list disambiguation. Each workspace's title can be overridden by the user via `renameWorkspace`; absent an override, the title is auto-derived from the focused pane's working directory basename, falling back to the profile name when the directory is the user's home or unset.

The hamburger menu at the top-left contains app-level commands that historically lived in the new-tab split-button's chevron menu in classic mode. The new arrangement separates "create a new thing" from "configure the app." Hamburger menu contents are: Switch Workspace… (opens command palette in workspace-switcher mode), Manage Workspaces… (placeholder, deferred to follow-up — hidden in POC), Close All Workspaces in This Window, Settings, Command Palette, About. The `+ New Workspace` chevron menu contents are the existing `NewTabMenu`-derived profile picker (one entry per profile, plus `RemainingProfiles` / `MatchProfiles` / custom action / folder entries), with the same recursive expansion logic as today; clicking an entry creates a new workspace whose initial pane runs that profile.

Windows Terminal's existing `showTabsInTitlebar` setting becomes a no-op in workspace mode (the chrome is unconditionally integrated). When the user toggles the flag on, a one-time settings-validation message points to the new behavior.

### Sidebar layout and per-row content

The sidebar is a vertical list of workspace rows along the left edge of the window, between the top chrome strip and the window's bottom edge, separated from the active workspace's content area by a draggable splitter. The sidebar is always visible (no auto-hide, no rail-collapse mode) and is user-resizable via the splitter; the resized width persists per-window through TSM. The sidebar's interior — workspace rows and the area between them — is not a window drag region; the strip above it is.

Each workspace row occupies two visible lines plus a layout slot reserved for future status entries. Line one carries, from left to right, a thin color stripe along the leading edge (the workspace's runtime color, blank if unset), a small dot indicator for attention (filled when the workspace has had a bell or other unread output since last activation, cleared on activation), a pushpin glyph if the workspace is pinned, the workspace's title (truncated with ellipsis if it exceeds the available width), and an overflow menu button revealing the workspace's context menu. Line two carries the focused pane's working directory, head-truncated so the trailing path component is always visible — or, if the user has set a custom description via `setWorkspaceDescription`, the custom description in place of the directory. The third layout slot is empty in this proof of concept; it is reserved for a future `SidebarStatusEntry`-style extension API analogous to cmux's per-workspace status pushed by agent processes through the cmux socket.

The selected (active) workspace gets a stronger background fill, a slightly thicker color stripe, and a prominent leading focus ring. Pinned workspaces sort to the top of the list, separated from the unpinned region by a subtle horizontal divider; within each region, ordering is manual (drag-to-reorder; persists through TSM). When the user drags a workspace across the pinned/unpinned boundary, the workspace auto-pins or auto-unpins to match its destination region.

### Per-pane tab strip

Each leaf pane in a workspace's split tree has its own inline tab strip at the top of the pane's allotted rectangle, implemented with a `TabView` configured for inline use (compact tab heights, a `+` button at the right end of the strip via `TabView.TabStripFooter`). The strip shows one tab per `PaneTab` in the pane's `_tabs` vector, with the active tab highlighted; the close-X on each tab closes that PaneTab; clicking the `+` creates a new PaneTab using the default profile, and its chevron opens the same `NewTabMenu`-derived profile picker as the workspace `+` (creating in this pane rather than as a new workspace).

Tabs can be reordered within a pane via drag — the existing `CanReorderTabs="True"` configuration suffices. Tabs can also be dragged from one pane's tab strip to another pane's tab strip in the same workspace, which moves the tab; existing `AllowDropTabs="True"` cross-`TabView` drag handles this. Dragging onto a pane's body rather than its tab strip enters split-creation mode: drop zones along the four edges (top, bottom, left, right) appear; dropping on an edge creates a new split in that direction, with the dragged tab as the only tab in the new pane, matching VS Code's editor-group split semantics. Dropping in the center of the pane body is equivalent to dropping on the tab strip (move/append).

When the last tab in a pane is closed, the pane closes and its sibling expands to fill the parent split. When the last pane in a workspace is closed (by closing its last tab, or by `closePane`), the workspace closes and the active workspace selection moves to a neighbor in the sidebar (next, falling back to previous). When the last workspace in a window is closed, the window closes. The cascade reuses Windows Terminal's existing `confirmCloseAllTabs` setting; when set, a confirmation dialog at window close lists "this window has N workspaces with M panes across them" and asks the user to confirm.

Tabs are heterogeneous — a single pane can hold a mix of `TermControl`-backed terminals, settings pages, markdown viewers, and any other `IPaneContent` type. Closing a tab whose process has exited (via the existing per-profile `closeOnExit` setting) removes it from the pane's `_tabs` vector; if it was the last, the cascade applies.

Cross-pane drag of tabs is supported within the same workspace; drag of a tab to a different workspace's pane is not implemented in POC. A context-menu "Move to workspace…" action on a PaneTab is similarly deferred. Tear-out of an entire pane into a new window is deferred. These three operations would all require either cross-process state transfer or substantial new drop-zone hit-testing across the sidebar; they are left to a follow-up spec.

### Workspace creation, reordering, renaming, recoloring, pinning

Creating a workspace via the `+ New Workspace` button (or its chevron) creates a workspace whose first pane runs the chosen profile (the default profile for the main button face; the selected profile for chevron entries) in a working directory inherited from the focused pane of the previously active workspace, falling back to the profile's `startingDirectory` if there is no active workspace. The new workspace is inserted into the sidebar according to `experimental.workspaces.newWorkspacePlacement` (default `afterCurrent`); pinned workspaces are not affected. The new workspace is auto-focused immediately on creation. Its title is auto-derived from the working directory basename, falling back to the profile name when the directory is the user's home or empty.

Renaming a workspace is reachable via right-click → Rename, the `renameWorkspace` action, or `F2` when the sidebar has focus; an inline `TextBox` replaces the title row temporarily. Setting a color is reachable via right-click → Set Color… and opens the existing color-picker control already used for tab runtime colors. Setting a description is reachable via right-click → Set Description… and opens a small inline editor. Pinning is a context-menu item or the `togglePinWorkspace` action; pin state moves the workspace to or from the top region of the sidebar with a brief animation.

Reordering workspaces is by drag within the sidebar; the user grabs a row and drops it between two others. Pinned workspaces can only be reordered within the pinned region; unpinned can only reorder within the unpinned region; dragging across the boundary toggles pin state.

### Right-click context menus

The workspace-row context menu (right-click on a sidebar row, or click on the row's overflow button) exposes Rename, Set Color…, Set Description…, Pin / Unpin, Move Up, Move Down, Close, and Close Others. Future-deferred entries — Duplicate Workspace, Move to New Window — are not present in POC menus and will be added when the corresponding actions land.

The pane-tab context menu (right-click on a tab in a per-pane tab strip) exposes Rename Tab, Set Tab Color…, Reset Tab Color, Duplicate Tab, Close Tab, Close Other Tabs, and Close Tabs to the Right. This mirrors today's per-tab context menu, retargeted to the per-pane tab strip.

The pane-body context menu is unchanged from today; it continues to expose Split, Close Pane, and other pane-level operations.

### Command palette integration

The existing `CommandPalette` gets a new prefix mode. Typing `@` in the palette switches to workspace-switcher mode: the palette's command list is replaced with the window's workspaces in MRU order, names fuzzy-matched against the rest of the user's input. Selecting a workspace activates it. Pressing backspace past `@` returns to normal command-palette mode. The legacy tab switcher ([#1502]) action retargets to PaneTab switching in the focused pane when workspaces is on; users who use the tab switcher today get a pane-tab switcher in workspace mode. Calling `switchToWorkspace` with no arguments opens the palette directly in workspace-switcher mode; calling it with an `index` or `id` argument switches directly without opening the palette.

## Capabilities

### Accessibility

The sidebar is a `ListView` with `AutomationProperties.Name = "Workspaces"`; each row exposes its name, position, selected state, pinned state, and attention state. The attention indicator is announced via `LiveSetting = Polite` so screen readers narrate attention triggers, throttled by the same coalesce-on-bell-after-idle policy used to update the visual indicator (so a bell-spamming pane does not produce a torrent of UIA announcements). The color stripe is decorative (`AccessibilityView.Raw`) and is not surfaced to screen readers; its meaning is conveyed through the row's name and any user-set description.

Keyboard navigation in the sidebar uses Up/Down arrows to move selection without activation (highlight only), Enter or Space to activate the highlighted workspace, F2 to rename, and Delete to close (cascade applies). F6 cycles top-level focus between the sidebar, the chrome strip, and the active workspace's content area, mirroring Windows Terminal's existing F6-style focus cycling for `MoveFocus` actions. Tab moves focus from the sidebar into the active workspace's content; Shift+Tab moves out.

Per-pane tab strips inherit `TabView`'s built-in UIA support, which Windows Terminal already exercises for the existing top-level tab strip. High-contrast theme support uses system theme brushes (`WindowText`, `Highlight`, `HighlightText`) for sidebar selection and color-stripe rendering rather than theme-resource colors that could collapse to invisible in HC.

### Security

Workspaces introduce no new attack surface beyond what exists today. Workspace state is persisted in the existing `ApplicationState` blob, which is already integrity-checked through the same path as legacy tab state. The new actions follow the same trust model as existing actions and are subject to the same `confirmCloseAllTabs`-style confirmation gates where appropriate. ConPTYs in workspace mode are spawned with the same restrictions as in classic mode; profile-level `commandline` and `startingDirectory` are honored without expansion.

### Reliability

A bell-spamming pane in a background workspace does not flicker the sidebar attention dot or flood UIA announcements; the attention indicator transitions to "raised" on first bell-after-idle and does not transition again until the workspace has been activated and de-activated. This matches Windows Terminal's existing bell coalescing for top-level tabs.

A pane spawn failure (mis-configured profile, exec failure, missing executable) creates the PaneTab and surfaces the existing spawn-error UI inside the tab; the workspace stays alive. Closing a failed PaneTab applies the same close cascade as a successful one.

Persisted state corruption (an unknown action type from a future version, malformed JSON, schema mismatch) does not crash the app. On read failure, Windows Terminal falls back to a fresh window with one default-profile workspace and surfaces a one-time toast notifying the user; the corrupted blob is moved aside (renamed with a `.corrupt` suffix) to allow forensic recovery without blocking the user's session.

This proof of concept ships without a hard cap on workspace count; the design has been validated up to sixteen concurrent workspaces. Beyond that, users are exploring uncharted territory; performance regressions or memory pressure may appear and should be reported.

A window close with multiple workspaces reuses the existing `confirmCloseAllTabs` setting; the dialog enumerates the workspaces and total pane count.

### Compatibility

The feature is gated by `experimental.workspaces.enabled` (default `false`). Users who do not enable it see no behavioral change; their settings, keybindings, and persisted state continue to work exactly as before.

Forward migration on first opt-in is lossless: each existing top-level tab becomes a workspace 1:1, with the tab's title and runtime color carried into the workspace, the tab's pane tree preserved as the workspace's pane tree, and each leaf pane's existing `IPaneContent` becoming the sole `PaneTab` of its pane. A user with twelve tabs of running work who flips the flag on at startup gets twelve workspaces in the sidebar with all the same panes and processes intact.

Reverse migration on opt-out is destructive: persisted workspace state is discarded, all running workspace processes are terminated, and classic mode resumes from its own (possibly stale or empty) prior persistence. A confirmation toast at toggle-off time announces that workspace processes will be terminated and asks the user to confirm. This is intentional. The flag is intended to become the default after the experimental period, at which point classic mode would be removed; the toggle-off path is a fallback for users who want to revert to legacy behavior, not a graceful round-trip.

Existing keybindings continue to work because the `newTab`/`closeTab`/`nextTab`/etc. action ids retain their action-map registration; their handlers dispatch to PaneTab operations in workspace mode rather than top-level-tab operations. Users who have customized their keybindings in `settings.json` see no breakage. New explicit `*PaneTab` action ids are aliases of the same handlers and exist for command-palette and persistence readability, not for binding migration.

The CLI surfaces (`wt new-tab`, `wt split-pane`) continue to work, retargeted to the active workspace's focused pane. New `wt new-workspace` and `wt switch-workspace` verbs are added. A `--workspace` selector for cross-workspace CLI targeting is deferred to follow-up work.

### Performance

Workspace switching is one `Children.Clear()` and one `Children.Append()` on a `ContentPresenter`, plus a focus-restore traversal. Measured cost is sub-frame in cmux's analogous implementation and expected to be comparable here.

Memory scales linearly with workspace count: each workspace owns its `Pane` tree, `IPaneContent` instances, `TermControl` instances, and `ConPTY` handles. A user with sixteen workspaces, each with two panes and a single TermControl per pane, holds approximately the memory of thirty-two terminal sessions — comparable to a power user with thirty-two tabs in classic mode. The tested limit is sixteen concurrent workspaces.

Inactive workspaces' `TermControl`s continue to render their output to scrollback even though no one is watching. This is intentional — it preserves scrollback continuity and avoids backpressure on the agent processes that the feature is designed to support — but represents continuous CPU cost proportional to the number of workspaces with output activity. A future polish pass can introduce render-pause hints for inactive workspaces (skipping renderer work while still pumping the ConPTY pipe and appending to the buffer); this is out of POC scope.

Eager spawn at restore means N workspaces with M panes spawn N×M ConPTYs in parallel at startup. Windows Terminal already handles this for tabs; users with many workspaces see startup time scale with their concurrent context count. A lazy-spawn-on-first-activation alternative would reduce startup time at the cost of agents not running until first activation, defeating the feature's core promise; lazy spawn is intentionally not implemented.

## Potential Issues

Per-pane tab strips at narrow window widths produce cramped tab rows. The existing `TabView` minimum-tab-width policy applies, but stacking multiple narrow `TabView`s in a workspace with many splits may produce strips below readability. The implementation should fall back to a count badge ("3 tabs") with click-to-expand when `TabView`'s minimum cannot be satisfied. The threshold is to be tuned during implementation.

Focus restoration after workspace switch is load-bearing. Failing to restore focus to the previously focused PaneTab content after a switch leaves the user typing into the void. The implementation must include an integration test exercising the full switch path (deactivate workspace A, activate workspace B, type, verify input reaches the right `TermControl`); test-quality policy requires runtime behavior verification rather than source-shape assertions.

Round-trip persistence — forward migration on opt-in, write workspace blob on shutdown, read on next launch — and the toggle-off / toggle-on cycle each have failure modes that can corrupt user state. Unit tests for round-trip are required; the `.corrupt` fallback path described in Reliability provides a safety net.

`TermControl` detach-and-reattach on workspace switch (removing the workspace's root XAML element from the `ContentPresenter` while the `TermControl` continues to read from ConPTY) depends on `TermControl`'s renderer thread and pipe-reader being independent of visual-tree attachment. Windows Terminal already exercises this path during tab drag-out, so the requirement is met today. If implementation surfaces an issue (e.g. `Loaded`/`Unloaded` resetting renderer state in some edge case), the fallback is the Visibility-Collapsed model: every workspace's root element stays mounted continuously, with `Visibility` controlling display. The user-facing behavior is identical; only the implementation differs.

The `@` prefix in the command palette must not collide with any existing prefix convention. The implementation should audit the command palette's existing accepted prefixes (currently `>` for commands and the legacy tab switcher); if a collision exists, fall back to a separate command-palette mode or use a different prefix.

## Future Considerations

### Cross-window workspace and surface moves

cmux supports moving a workspace from one window to another (`moveWorkspaceToWindow`, `moveWorkspaceToNewWindow`) and moving an individual surface across workspaces (`moveSurface`). In Windows Terminal, both require cross-process IPC because each window is a separate process: serializing workspace state, duplicating ConPTY pseudoconsole handles to the destination process via `DuplicateHandle`, transferring `TextBuffer` contents (process-local) or accepting a fresh-scrollback variant, and reattaching to a new `TermControl` in the destination. The operations should be designed against Windows Terminal's monarch/peasant architecture ([#5000]), with a transfer protocol mediated by the monarch and recovery semantics for the destination-process-died-mid-transfer case. A separate spec.

### tmux-resurrect-grade restore

The cmux author's stated north star is full session restore: pane layout and CWDs (already handled), scrollback (preserved across restart), and ideally the foreground commands at the top of each pane's process stack (relaunched on restart). For Windows Terminal, the natural sequencing is: first introduce a headless `TextBuffer` decoupled from `TermControl` so scrollback can outlive the visual element (this also unlocks hibernation of inactive workspaces to reduce memory pressure); then add scrollback serialization on shutdown and deserialization on startup, with format versioning; then add foreground-command capture via `ConPty::GetClientProcesses` and replay via shell input on restart, with the heuristic complexity of dealing with PowerShell, zsh, fish, cmd, and other shells' quoting and command-substitution differences. Each step is a separate spec. True process freeze/thaw across restart is not possible on Windows without OS-level support that does not exist today; tmux-resurrect itself does not cross this line either.

### Sidebar status entries / metadata blocks

cmux exposes a per-workspace status API through its socket: `SidebarStatusEntry` (key/value/icon/color/url/priority) and `SidebarMetadataBlock` (key/markdown/priority) let agent processes push live state into the sidebar without coupling to the cmux app code. This proof of concept reserves a third layout slot in each sidebar row for these but does not implement the IPC API. A follow-up spec should design the API surface (extension to the existing Windows Terminal control-channel protocol, or a new socket like cmux's), the trust model (which processes can write what), and the rendering rules (priority sort, length limits, refresh cadence).

### Drop-on-edge to split for cross-workspace drag

Dropping a tab on a pane's body to create a split (within the same workspace) ships in this POC. The same affordance for cross-workspace drag — drag a tab into another workspace's sidebar row, then drop on the workspace's pane to create or update structure there — is deferred. It depends on the cross-window/cross-workspace surface-move infrastructure described above.

### Tear-out of pane to new window

In classic Windows Terminal, dragging a tab from the top tab strip out of the window creates a new window with that tab. In workspace mode, the analogous "tear out a pane" operation requires moving a pane (with its PaneTabs) into a new window's workspace. Defer to the cross-window infrastructure spec.

### Auto-reorder on attention

cmux has a `workspaceAutoReorderOnNotification` setting that automatically bumps a workspace with attention to the top of the unpinned region. This proof of concept ships with static order (manual reorder only); auto-reorder is a polish feature for a follow-up.

### Workspace duplicate / clone

A "Duplicate Workspace" action that clones the structural template (panes, profile per PaneTab, working dirs, splits, names) into a new workspace, but with fresh ConPTYs, is a natural extension. It is deferred from the POC but easy to add later without architectural changes.

### Manage Workspaces UI

A dedicated workspace-management surface (overview of all workspaces in all windows, bulk operations, search, filter by status) is sketched in the hamburger menu as a placeholder. Designing it properly requires the cross-window infrastructure and the status-entries API above, and is left to a follow-up spec.

### Multi-workspace selection

cmux's sidebar supports multi-selecting workspaces (Cmd-click, Shift-click) for bulk operations like "close 5 selected workspaces." The corresponding `closeWorkspacesWithConfirmation` flow with grouped confirmation message is implemented in cmux's `TabManager.closeWorkspacesPlan`. Defer to follow-up.

### Per-pane render pause for inactive workspaces

To reduce CPU cost when many background workspaces have output activity, a future polish pass can introduce render-pause hints: inactive workspaces' `TermControl`s skip renderer work while still pumping the ConPTY pipe and appending to the buffer, then resume rendering on activation. This is purely a performance optimization; the user-observable behavior is unchanged.

## Resources

cmux source code, particularly `Sources/Workspace.swift`, `Sources/TabManager.swift`, `Sources/ContentView.swift` (`WorkspaceMountPolicy`), and `Sources/AppDelegate.swift` (`MainWindowContext`, `moveWorkspaceToWindow`): https://github.com/manaflow-ai/cmux

Tab Switcher Spec [#1502]

Process Model 2.0 Spec [#5000]

Application State / TSM Spec [#8324]

Panes and Split Windows Spec [#532]

Settings Keybindings Spec [#2557]

Command Palette Spec [#2046]

<!-- Footnotes -->
[#532]: https://github.com/microsoft/terminal/issues/532
[#1502]: https://github.com/microsoft/terminal/issues/1502
[#2046]: https://github.com/microsoft/terminal/issues/2046
[#2557]: https://github.com/microsoft/terminal/issues/2557
[#5000]: https://github.com/microsoft/terminal/issues/5000
[#8324]: https://github.com/microsoft/terminal/issues/8324
