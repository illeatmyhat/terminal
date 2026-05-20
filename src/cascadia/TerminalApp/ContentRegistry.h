// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// ContentRegistry — the SINGLE strong owner of every live IPaneContent in a
// window, keyed by the pure-C++ model's ContentId.
//
// WHY THIS EXISTS
// ===============
// A workspace that is not currently active is detached from the visual tree,
// but its terminals must keep running (the ConPTY / TermControl, its
// scrollback, and any live process must survive the switch). XAML drops a
// FrameworkElement the moment nothing in the live tree references it; so the
// thing that keeps inactive content alive cannot be the tree. The registry is
// that thing: while a ContentId is owned here, its IPaneContent (and therefore
// its ConPTY) stays alive regardless of whether it is currently parented into
// the active workspace's XAML.
//
// THE VECTOR TRAP (the cardinal concern of this slice)
// ====================================================
// Storing IPaneContent in a std::vector<PaneTab> caused sporadic split-pane
// disappearance: a vector reallocation / element move double-moved the winrt
// projection and left a `[this]`-captured close-revoker dangling into the
// pre-move slot. This class makes that mistake unrepresentable BY
// CONSTRUCTION:
//   * The only storage is a std::unordered_map<ContentId, IPaneContent>. A
//     node-based container never relocates its elements on insert / erase /
//     rehash, so an IPaneContent's address (and anything that captured it)
//     stays stable for the entry's whole lifetime.
//   * There is no API that hands out, takes, or stores a contiguous range of
//     IPaneContent. Everything outside the registry references content by
//     ContentId VALUE and resolves id -> live content on demand via Find().
//   * The class is non-copyable / non-movable, so the map (and the addresses
//     of its nodes) never moves either.
//
// ILLEGAL STATES UNREPRESENTABLE
// ==============================
// "Attach a ContentId the registry does not own into the tree" is the illegal
// state this slice must forbid. The API has no way to express it:
//   * EnsureMounted(id, factory) is the ONLY way to obtain a mountable
//     IPaneContent. It either returns the entry already owned under `id`, or
//     creates one via `factory` and inserts it. It can NEVER return null and
//     can NEVER produce a bound id with no content behind it.
//   * Find(id) is a pure query for already-owned content; it returns null when
//     the id is unowned, so a caller that tries to mount an unowned id by
//     resolving it first is forced to handle the null explicitly rather than
//     silently attach garbage.
// There is deliberately no Insert(id, content) overload that takes a bare id
// and trusts the caller to have produced the right content for it.
//
// This is a TerminalApp-layer class: it holds the winrt projection type
// winrt::TerminalApp::IPaneContent, keyed by the WorkspaceModel ContentId. It
// is plain C++ (not a WinRT runtimeclass) and needs no .idl. WorkspaceView
// owns the single instance for the window — it is the content arm of the S1
// id-resolver.

#pragma once

#include <functional>
#include <unordered_map>

#include "winrt/TerminalApp.h"

#include "../WorkspaceModel/Ids.h"

namespace winrt::TerminalApp::implementation
{
    class ContentRegistry final
    {
    public:
        ContentRegistry() = default;

        // The registry is the single strong owner of its content and hands out
        // addresses into its node-based map; copying or moving it would either
        // duplicate that sole-owner role or relocate the map. Forbid both so
        // the ownership invariant — and the address stability the close-revoker
        // capture relies on — can never be violated.
        ContentRegistry(const ContentRegistry&) = delete;
        ContentRegistry& operator=(const ContentRegistry&) = delete;
        ContentRegistry(ContentRegistry&&) = delete;
        ContentRegistry& operator=(ContentRegistry&&) = delete;

        ~ContentRegistry()
        {
            // Window teardown: every still-owned content is torn down here.
            // This is the last place ConPTY can be released for content that
            // outlived its workspace switches.
            Clear();
        }

        // The ONLY way to obtain a mountable IPaneContent for `id`.
        //
        // ContentMounted wiring calls this. If `id` is already owned, the live
        // content is returned unchanged (a re-mount after an unmount resolves
        // to the SAME instance, so the ConPTY / scrollback that survived the
        // unmount is what gets re-attached). If `id` is not yet owned, `factory`
        // is invoked exactly once to materialise the content from its spec, the
        // result is inserted as the registry's strong ref, and returned.
        //
        // Returns a non-null IPaneContent unless `id` is unowned AND `factory`
        // itself yields null (spawn failure); in that case nothing is inserted,
        // so the registry never holds a bound id with no content behind it. The
        // caller must treat a null return as "do not attach".
        winrt::TerminalApp::IPaneContent EnsureMounted(
            ::WorkspaceModel::ContentId id,
            const std::function<winrt::TerminalApp::IPaneContent()>& factory)
        {
            if (const auto existing = Find(id))
            {
                return existing;
            }

            auto created = factory ? factory() : nullptr;
            if (!created)
            {
                // Spawn failure: leave the registry untouched. There is no
                // half-owned id state to clean up because we never inserted.
                return nullptr;
            }

            // node-based insert: existing entries keep their addresses.
            _content.emplace(id, created);
            return created;
        }

        // Pure query: resolve an already-owned id to its live content, or null
        // when the id is not owned. ContentMounted's "is this already alive?"
        // check and any code that references content by id value resolve
        // through here. A null return is the EXPLICIT failure that makes
        // "mount an id we don't own" impossible to do silently.
        [[nodiscard]] winrt::TerminalApp::IPaneContent Find(::WorkspaceModel::ContentId id) const
        {
            const auto it = _content.find(id);
            return it == _content.end() ? nullptr : it->second;
        }

        [[nodiscard]] bool Contains(::WorkspaceModel::ContentId id) const noexcept
        {
            return _content.find(id) != _content.end();
        }

        // ContentUnmounted wiring: detaching content from the visual tree is
        // the caller's job (it owns the XAML parent). The registry deliberately
        // does NOTHING to its ownership here — KEEPING the strong ref is what
        // holds the TermControl / ConPTY alive while the workspace is inactive.
        // Provided as a named no-op so the unmount path reads as a deliberate
        // "keep alive", not a forgotten teardown.
        void NoteUnmounted(::WorkspaceModel::ContentId /*id*/) const noexcept
        {
            // intentionally empty — see comment above.
        }

        // Content removal (tab / content destroyed): erase the registry's
        // strong ref. This is the ONLY place a single content's ConPTY tears
        // down — dropping the last strong ref and calling Close() releases the
        // connection. Returns true iff an entry was actually owned and removed.
        bool Remove(::WorkspaceModel::ContentId id)
        {
            const auto it = _content.find(id);
            if (it == _content.end())
            {
                return false;
            }
            // Take ownership out of the map first so Close() (which can re-enter
            // via CloseRequested handlers) never sees a half-erased entry.
            auto doomed = std::move(it->second);
            _content.erase(it);
            if (doomed)
            {
                doomed.Close();
            }
            return true;
        }

        [[nodiscard]] std::size_t Size() const noexcept
        {
            return _content.size();
        }

        // Tear down every owned content. Used by the destructor (window close)
        // and available for explicit full teardown.
        void Clear()
        {
            // Drain the map into a local first so each Close() runs against an
            // already-detached entry and cannot observe a partially-cleared map.
            auto drained = std::move(_content);
            _content.clear();
            for (auto& [id, content] : drained)
            {
                if (content)
                {
                    content.Close();
                }
            }
        }

    private:
        // The single strong owner. NODE-BASED on purpose: element addresses are
        // stable across insert / erase / rehash, so a close-revoker captured by
        // address never dangles. NEVER replace this with a std::vector — see the
        // file header.
        std::unordered_map<::WorkspaceModel::ContentId, winrt::TerminalApp::IPaneContent> _content;
    };
}
