# ADR-001 — Per-leaf tab strip renders on the real MUX `TabView`, driven by `WorkspaceModel`

- **Status:** Accepted — 2026-05-25. **Supersedes the previously-locked "decision B".**
- **Branch:** `workspaces-slice-f` (fork `illeatmyhat/terminal`). Not merged to `main`.
- **Feature gate:** `experimental.workspaces.enabled` / `wtd --workspaces`. The flag-off path is byte-for-byte upstream; **rollback = flip the flag off.**
- **Scope:** the per-leaf tab strip only. The classic per-window tab row is untouched flag-off and is a separate (later) convergence target.

---

## Context

The Workspaces big-flip (Phase 2, slices A–F) cut the **flag-on** display over from the classic
per-window tab row to a `WorkspaceModel`-driven **pane tree**. Each *leaf* of that tree owns a
small horizontal tab strip.

Through slices 1 → 2a.4 (`946ce56c3 → f536419c7`) that strip was a **re-skinned WUX `ListView`**
(`TabStripView`), with the classic `TabView` chrome — rounded top, bottom flare/chamfer,
between-tab separators, hover highlight — **hand-rebuilt as a pure projection of the model**
(`PaneTabViewModel.Background` / `IsActive` / `ShowSeparator` / `IsHovered`).

The governing invariant is **model-as-truth**: `WorkspaceModel` is the single source of truth and
the view is a one-way projection. Existence flows `intent → model action → diff → view`; the UI is
**never** reconciled back into the model. (See `feedback_ui_downstream_of_model` in the project
memory.)

### The decision being reversed — "decision B"

Decision B chose the custom MVVM `ListView` strip **specifically to avoid the real MUX `TabView`**.
Its reasoning: `TabView` owns its **own** selection / container / drag state — a *competing source
of truth* — which forces the reconcile-UI-back-into-model anti-pattern that the big-flip forbids.
The classic path is the cautionary proof: it carries `_removing` / `_rearranging` suppression flags
and manually re-derives selection in `_RemoveTab` to keep the control and the app in sync.

### Why reverse it now

The user's framing: *"the maintainers used MUX for `TabView`, so we can too."* Concretely, the strip
must grow the **full** classic feature set — renamer, bell/attention, color flyout, per-tab context
menu, SettingsTab — **and leaf↔leaf drag is near-term.** Re-deriving each of those on a bespoke
`ListView` re-skin is slice-by-slice reinvention of machinery the maintainers already run crash-free.
Continuing on the re-skin is a sunk-cost trap; adopting the real control and *converging on the
classic view machinery* is the lower-tech-debt path (see `feedback_maintainability_over_aesthetics`).

---

## Decision

Rewire the per-leaf `TabStripView` from the re-skinned WUX `ListView` to the **real MUX `TabView`**
(`Microsoft.UI.Xaml.Controls.TabView` — the same control the classic WT tab row uses in
`TabRowControl.xaml`), **converging on classic WT's view machinery** (`TabHeaderControl`,
`ColorPickupFlyout`, the drag handlers) **driven by `WorkspaceModel` as the single source of truth**
— **not** a parallel reimplementation.

**Foundation = manual `TabView.TabItems()`** (the classic-WT shape), **not** `TabItemsSource`
data-binding. The model-as-truth invariant is preserved: imperative `TabItems` is only the
*projection mechanism*. Every mutation — select / close / rename / color / move — still flows
`intent → WorkspaceModel action → diff → re-project`. The view never writes back into the model.

### A note on "MUX vs WUX"

This repo is **WinUI 2** (`Microsoft.UI.Xaml.2.8.4`). MUX 2.x does **not** replace the framework:
`UIElement` / `FrameworkElement` / `Visibility` / `DependencyObject` stay **WUX**
(`Windows.UI.Xaml.*`). MUX 2.x adds *new controls* (notably `TabView`, which WUX lacks) and Fluent
styles. There is **no `Microsoft.UI.Xaml.Controls.ListView`** — so "make the strip MUX" has exactly
one meaning: **adopt the MUX `TabView` / `TabViewItem`.** Helper x:Bind return types like
`Windows.UI.Xaml.Visibility` stay WUX and are fine. The WUX↔MUX boundary stays contained to the
strip control: leaf **content** (`IPaneContent.GetRoot()`, WUX) is hosted separately in
`leafContentHost`, not inside the strip.

### Why manual `TabItems` (and not `TabItemsSource` binding)

1. **`TabItemsSource` + TwoWay `SelectedItem` would be cleaner** — it is the React "controlled
   component" shape, model-as-truth via binding, with the optimistic-DP-state caveat damped by the
   INPC round-trip + DP value-equality short-circuit. But it is viable **only** for a uniform,
   header-only, no-drag strip.
2. The full feature set (renamer, bell, color flyout, SettingsTab) **and** near-term drag each force
   owning the container → manual `TabItems`. Starting on binding = a second sunk-cost trap.
3. **Content lifetime is *not* a reason** (hypothesis tested and rejected): classic WT's
   `TabViewItem.Content` is a throwaway empty `Border` (the BODGY drag-identity comment in
   `Tab.cpp`); the terminal lives in a separate host. Our strip is likewise header-only
   (`_projectLeafContainer` builds a 2-row Grid: strip + `leafContentHost`). Virtualization cannot
   kill a terminal — lifetime is decoupled in both designs.
4. **Drag does not compose with `TabItemsSource`**: native reorder mutates the bound collection (a
   write-back), and MUX resolves the dragged tab by `Content` identity (the empty-`Border` bodge).
   The drop handler must be a pure intent-raiser dispatching a **`moveTab`** action.
5. **Convergence is the documented strangler-fig endgame** (`TerminalPage.h`): the window tab row is
   itself slated to be replaced by a per-leaf model-driven `TabView`. So M1 is built as a reusable
   projection, anticipating the window row adopting it later.
6. **`TabView` crash-proneness argues *for* reuse, not against** (see Risks below).

---

## Verified facts (confirmed on this branch, 2026-05-25)

These three claims are load-bearing for the slicing and were re-confirmed against the tree:

1. **`TabHeaderControl` is pure view → lift-and-drive.** `TabHeaderControl.idl` exposes settable
   `Title` / `RenamerMaxWidth` / `TabStatus`, `BeginRename()` / `InRename`, and intent events
   `TitleChangeRequested` / `RenameEnded`. No model coupling. (Renamer drops into M2; the bell rides
   on `TabStatus`.)
2. **Cross-leaf `moveTab` + TabId preservation is real and tested.**
   `src/cascadia/UnitTests_WorkspaceModel/MoveActionTests.cpp` carries
   `MoveTab_AcrossLeafs_PreservesTabId`, `MoveTab_CrossWorkspace_PreservesTabIdAndMoves`,
   `MoveTab_WithinSameLeaf_Reorders`, plus the `moveTabAsSplit` family + edge cases. This proves the
   **model** move + TabId preservation; the "no ConPTY disconnect" guarantee is the separate job of
   the `TabMoved` projection arm. M6's drag handler only *dispatches* `moveTab`.
3. **The model gaps are real.** `src/cascadia/WorkspaceModel/PaneTree.h:47` — `TabRecord` =
   `{ id, description, mount (ContentId), customTitle, runtimeColor, pinned }`. `customTitle`
   (rename) and `runtimeColor` (color) **exist** → M2 / M4 are model-ready. There is **no
   bell/attention field** and **no tear-out/new-window action** in `WorkspaceModel` → M3 and M8
   genuinely require model additions.

---

## The slicing

| Slice | What | Model status |
|------|------|--------------|
| **M1** | MUX `TabView` shell. Manual `TabItems` mirroring classic WT (empty-`Border` drag-identity bodge; on UI thread; **append** IDL rows). Imperative selection: push `SelectedItem` from the `ActiveTabChanged` diff; `SelectionChanged` → `ActivateRequested` intent + **reentrancy guard**; `TabCloseRequested` → `CloseRequested` intent. **Drag OFF** (`CanReorderTabs` / `CanDragTabs` / `AllowDropTabs` = false). Native chrome / icon / tooltip replace the bespoke re-skin (delete the "RECONCILE BEFORE UPSTREAM" brush block + the hand-built shape / hover / separator). **First action: revert the uncommitted 2a.5 + the CRLF churn.** Foundational. | ready |
| **M2** | Rename — reuse `TabHeaderControl`; `TitleChangeRequested` → `setTabTitle`. | ready (`customTitle`) |
| **M3** | Bell — **model gap**: add a `TabRecord` field + `setBellState` action + projection arm; hook `IPaneContent.BellRequested`; render via `TabStatus.BellIndicator`. | needs addition |
| **M4** | Color flyout — reuse `ColorPickupFlyout`; re-route `ColorSelected` / `ColorCleared` → `setTabColor` / reset (refactor the event wiring off `Tab::SetRuntimeTabColor`). | ready (`runtimeColor`) |
| **M5** | Context menu — reuse the UI; route clicks by `TabId` intent instead of `_dispatch.DoAction(*this)`. | ready |
| **M6** | Drag leaf↔leaf (same window) — wire `TabView` drag events → `moveTab` intent; rides the existing `moveTab` + `TabMoved`. Full drag state machine (avoid `0xc000027b`). | ready (`moveTab`) |
| **M7** | SettingsTab — materialize `SettingsSpec` in a leaf via `ContentMounted` (no classic-Tab intermediary). *Re-verify `SettingsSpec` support at M7 — asserted by an Explore agent, not independently confirmed.* | re-verify |
| **M8** | *(DEFERRED — separate effort)* tear-out to a **new window**: needs a new cross-window `WorkspaceModel` action + monarch/IPC + ConPTY rehydration, and fixes the documented `Tab::Closed`-bypass in `_sendDraggedTabToWindow`. | needs addition |

### Reuse verdicts from the investigation

- **Lift-and-drive** (pure view, no refactor): `TabHeaderControl` (renamer, bell, status),
  `TerminalTabStatus`.
- **Refactor-needed** (re-route the event off Tab mutation → model intent): `ColorPickupFlyout`, the
  Tab context menu, icon/color projection.
- **Quagmire** (keep at page/window level): cross-window drag tear-out + monarch coordination + the
  `_RemoveTab`-bypasses-`Closed` gap.

---

## Consequences

### What this simplifies (it is a net deletion, not additive)

Adopting `TabView` makes most of the bespoke re-skin unnecessary:

- chrome (rounded top, flare/foot, separators, hover highlight) → native `TabViewItem`.
- selected-tab background that tracks the live terminal color (the point of 2a.2's
  `PaneTabViewModel.Background` projection) → this is exactly what classic WT does via
  `Tab::_RecalculateAndApplyTabColor` + the default theme `tab.background = terminalBackground`.
  **Reuse that mechanism**, not the custom projection.
- hover (2a.4: content color @ 0.6) → native `TabViewItemHeaderBackgroundPointerOver` = WT's
  `hoverTabBrush`.
- icon → `IconPathConverter::IconSourceMUX(path, convertToGrayscale)` on `TabViewItem().IconSource`.
- tooltip → native `TabViewItem` tooltip / `ToolTipService`.

So M1 *deletes* much of `TabStripView.xaml`'s transitional `PaneTab*` brush/template block (the one
marked "RECONCILE BEFORE UPSTREAM PR") and the bespoke shape / hover / separator projections, keeping
only what bridges the model to `TabView`. `PaneTabViewModel` shrinks accordingly.

### Risks — `TabView` is a failfast minefield; reuse inherits the maintainers' crash-avoidance

XAML callbacks are an ABI boundary: an escaped exception becomes a `0xc000027b`
(`STATUS_STOWED_EXCEPTION`) failfast with no usable stack. The intrinsic corners:

- **(a) Reentrancy** on items/selection mutation from inside the control's own callbacks → the M1
  **reentrancy guard** is crash-avoidance, not hygiene.
- **(b) The all-or-nothing drag state machine** — `AllowDropTabs` defaults **true**; pairing
  `CanReorderTabs(true)` with no `TabDroppedOutside` handler crashes `Windows.UI.Xaml.dll`. **M1
  sets all three drag flags OFF**; M6 wires the full state machine. (See
  `reference_mux_tabview_drag`.)
- **(c) Container virtualization / lifetime** — mitigated by the header-only design above.

Ambient traps that also apply: the **IDL vtable trap** — *append* IDL rows, never insert mid-file
(`reference_idl_insertion_trap`); the WUX/MUX `IconSource` split; UI-thread affinity
(`Tab.cpp` `ASSERT_UI_THREAD`).

The maintainers run this control crash-free by wiring all of the above correctly. **Reusing their
machinery inherits the crash-avoidance; rolling our own re-derives every failfast.** This is the
core argument for convergence over reimplementation.

### What we consciously trade away

`feedback_maintainability_over_aesthetics` recorded a preference for an MVVM per-pane strip over an
imperative `TabView`. This ADR trades that for native chrome + the maintainers' battle-tested control,
**without** surrendering model-as-truth: the contained, well-guarded imperative adapter keeps the
view a one-way projection. The transitional duplication during M1 (both the old re-skin and the new
control exist briefly) is cleaned before the upstream PR; the unchosen path is deleted, so there is
no parallel maintenance.

---

## Alternatives considered

1. **Keep the custom MVVM `ListView` re-skin (status quo / decision B).** Rejected: re-deriving the
   full feature set + drag slice-by-slice is higher long-term tech debt than converging on the
   maintainers' control, and each re-derivation re-encounters a `TabView` failfast the maintainers
   already solved.
2. **`TabView` via `TabItemsSource` data-binding.** Rejected for the strip's real requirements:
   clean only for a uniform, header-only, no-drag strip; the full feature set + drag force owning the
   container (see "Why manual `TabItems`" above). Recorded as the right shape *if* the requirements
   were narrower.
3. **Defer the whole rewire and ship the re-skin.** Rejected: the re-skin's "RECONCILE BEFORE
   UPSTREAM PR" block is itself unshippable, and the next features (rename/bell/color/drag) all push
   toward the real control regardless.

---

## Implementation & verification constraints

- Build **foreground**, in the **main repo** (not worktrees — PCH/obj races). Pin **VS 2022's
  MSBuild** explicitly. Two-step packaged loop: wapproj `/p:AppxSymbolPackageEnabled=false` **then**
  `DeployAppRecipe`. Also build `TestHostApp.vcxproj` for page tests.
- Tests stay green: **WorkspaceTests 88/88**, **UnitTests_WorkspaceModel 198/198**. New
  selection-adapter logic + each model addition (M3 bell, M8 tear-out) get headless TAEF against the
  VM/contract — **never** drive real layout/resize (the `std::clamp` headless trap).
- Smoke-launch by **PID-diff only** (host and dev build are both `WindowsTerminal.exe`; a
  name/wildcard kill crashes the session — now hook-enforced). Hover/tooltip states are not
  screenshottable; the user is the authoritative visual arbiter.
- **No self-review** — a separate adversarial reviewer per slice, watching the no-write-back
  invariant, the reentrancy guard, and the drag failfast guards.
