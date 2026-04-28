
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"

#include <TerminalCore/ControlKeyStates.hpp>
#include <TerminalThemeHelpers.h>
#include <til/hash.h>
#include <til/unicode.h>
#include <Utils.h>

#include "../../types/inc/ColorFix.hpp"
#include "../../types/inc/utils.hpp"
#include "../TerminalSettingsAppAdapterLib/TerminalSettings.h"
#include "App.h"
#include "DebugTapConnection.h"
#include "MarkdownPaneContent.h"
#include "Remoting.h"
#include "ScratchpadContent.h"
#include "SettingsPaneContent.h"
#include "SnippetsPaneContent.h"
#include "TabRowControl.h"
#include "TerminalSettingsCache.h"
#include "WorkspaceList.h"

#include "LaunchPositionRequest.g.cpp"
#include "RenameWindowRequestedArgs.g.cpp"
#include "RequestMoveContentArgs.g.cpp"
#include "TerminalPage.g.cpp"

using namespace winrt;
using namespace winrt::Microsoft::Management::Deployment;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;
using namespace ::Microsoft::Terminal::Core;
using namespace std::chrono_literals;

#define HOOKUP_ACTION(action) _actionDispatch->action({ this, &TerminalPage::_Handle##action });

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
    using VirtualKeyModifiers = Windows::System::VirtualKeyModifiers;
}

namespace clipboard
{
    static SRWLOCK lock = SRWLOCK_INIT;

    struct ClipboardHandle
    {
        explicit ClipboardHandle(bool open) :
            _open{ open }
        {
        }

        ~ClipboardHandle()
        {
            if (_open)
            {
                ReleaseSRWLockExclusive(&lock);
                CloseClipboard();
            }
        }

        explicit operator bool() const noexcept
        {
            return _open;
        }

    private:
        bool _open = false;
    };

    ClipboardHandle open(HWND hwnd)
    {
        // Turns out, OpenClipboard/CloseClipboard are not thread-safe whatsoever,
        // and on CloseClipboard, the GetClipboardData handle may get freed.
        // The problem is that WinUI also uses OpenClipboard (through WinRT which uses OLE),
        // and so even with this mutex we can still crash randomly if you copy something via WinUI.
        // Makes you wonder how many Windows apps are subtly broken, huh.
        AcquireSRWLockExclusive(&lock);

        bool success = false;

        // OpenClipboard may fail to acquire the internal lock --> retry.
        for (DWORD sleep = 10;; sleep *= 2)
        {
            if (OpenClipboard(hwnd))
            {
                success = true;
                break;
            }
            // 10 iterations
            if (sleep > 10000)
            {
                break;
            }
            Sleep(sleep);
        }

        if (!success)
        {
            ReleaseSRWLockExclusive(&lock);
        }

        return ClipboardHandle{ success };
    }

    void write(wil::zwstring_view text, std::string_view html, std::string_view rtf)
    {
        static const auto regular = [](const UINT format, const void* src, const size_t bytes) {
            wil::unique_hglobal handle{ THROW_LAST_ERROR_IF_NULL(GlobalAlloc(GMEM_MOVEABLE, bytes)) };

            const auto locked = GlobalLock(handle.get());
            memcpy(locked, src, bytes);
            GlobalUnlock(handle.get());

            THROW_LAST_ERROR_IF_NULL(SetClipboardData(format, handle.get()));
            handle.release();
        };
        static const auto registered = [](const wchar_t* format, const void* src, size_t bytes) {
            const auto id = RegisterClipboardFormatW(format);
            if (!id)
            {
                LOG_LAST_ERROR();
                return;
            }
            regular(id, src, bytes);
        };

        EmptyClipboard();

        if (!text.empty())
        {
            // As per: https://learn.microsoft.com/en-us/windows/win32/dataxchg/standard-clipboard-formats
            //   CF_UNICODETEXT: [...] A null character signals the end of the data.
            // --> We add +1 to the length. This works because .c_str() is null-terminated.
            regular(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        }

        if (!html.empty())
        {
            registered(L"HTML Format", html.data(), html.size());
        }

        if (!rtf.empty())
        {
            registered(L"Rich Text Format", rtf.data(), rtf.size());
        }
    }

    winrt::hstring read()
    {
        // This handles most cases of pasting text as the OS converts most formats to CF_UNICODETEXT automatically.
        if (const auto handle = GetClipboardData(CF_UNICODETEXT))
        {
            const wil::unique_hglobal_locked lock{ handle };
            const auto str = static_cast<const wchar_t*>(lock.get());
            if (!str)
            {
                return {};
            }

            const auto maxLen = GlobalSize(handle) / sizeof(wchar_t);
            const auto len = wcsnlen(str, maxLen);
            return winrt::hstring{ str, gsl::narrow_cast<uint32_t>(len) };
        }

        // We get CF_HDROP when a user copied a file with Ctrl+C in Explorer and pastes that into the terminal (among others).
        if (const auto handle = GetClipboardData(CF_HDROP))
        {
            const wil::unique_hglobal_locked lock{ handle };
            const auto drop = static_cast<HDROP>(lock.get());
            if (!drop)
            {
                return {};
            }

            const auto cap = DragQueryFileW(drop, 0, nullptr, 0);
            if (cap == 0)
            {
                return {};
            }

            auto buffer = winrt::impl::hstring_builder{ cap };
            const auto len = DragQueryFileW(drop, 0, buffer.data(), cap + 1);
            if (len == 0)
            {
                return {};
            }

            return buffer.to_hstring();
        }

        return {};
    }
} // namespace clipboard

namespace winrt::TerminalApp::implementation
{
    TerminalPage::TerminalPage(TerminalApp::WindowProperties properties, const TerminalApp::ContentManager& manager) :
        _tabs{ winrt::single_threaded_observable_vector<TerminalApp::Tab>() },
        _mruTabs{ winrt::single_threaded_observable_vector<TerminalApp::Tab>() },
        _manager{ manager },
        _hostingHwnd{},
        _WindowProperties{ std::move(properties) }
    {
        InitializeComponent();
        _WindowProperties.PropertyChanged({ get_weak(), &TerminalPage::_windowPropertyChanged });
    }

    // Out-of-line so ~unique_ptr<::TerminalApp::WorkspaceList> sees the
    // complete type (the include is in this TU, not in the header).
    TerminalPage::~TerminalPage() = default;

    // Method Description:
    // - implements the IInitializeWithWindow interface from shobjidl_core.
    // - We're going to use this HWND as the owner for the ConPTY windows, via
    //   ConptyConnection::ReparentWindow. We need this for applications that
    //   call GetConsoleWindow, and attempt to open a MessageBox for the
    //   console. By marking the conpty windows as owned by the Terminal HWND,
    //   the message box will be owned by the Terminal window as well.
    //   - see GH#2988
    HRESULT TerminalPage::Initialize(HWND hwnd)
    {
        if (!_hostingHwnd.has_value())
        {
            // GH#13211 - if we haven't yet set the owning hwnd, reparent all the controls now.
            for (const auto& tab : _tabs)
            {
                if (auto tabImpl{ _GetTabImpl(tab) })
                {
                    tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                        if (const auto& term{ pane->GetTerminalControl() })
                        {
                            term.OwningHwnd(reinterpret_cast<uint64_t>(hwnd));
                        }
                    });
                }
                // We don't need to worry about resetting the owning hwnd for the
                // SUI here. GH#13211 only repros for a defterm connection, where
                // the tab is spawned before the window is created. It's not
                // possible to make a SUI tab like that, before the window is
                // created. The SUI could be spawned as a part of a window restore,
                // but that would still work fine. The window would be created
                // before restoring previous tabs in that scenario.
            }
        }

        _hostingHwnd = hwnd;
        return S_OK;
    }

    // INVARIANT: This needs to be called on OUR UI thread!
    void TerminalPage::SetSettings(CascadiaSettings settings, bool needRefreshUI)
    {
        assert(Dispatcher().HasThreadAccess());
        if (_settings == nullptr)
        {
            // Create this only on the first time we load the settings.
            _terminalSettingsCache = std::make_shared<TerminalSettingsCache>(settings);
        }
        _settings = settings;

        // Make sure to call SetCommands before _RefreshUIForSettingsReload.
        // SetCommands will make sure the KeyChordText of Commands is updated, which needs
        // to happen before the Settings UI is reloaded and tries to re-read those values.
        if (const auto p = CommandPaletteElement())
        {
            p.SetActionMap(_settings.ActionMap());
        }

        if (needRefreshUI)
        {
            _RefreshUIForSettingsReload();
        }

        // Upon settings update we reload the system settings for scrolling as well.
        // TODO: consider reloading this value periodically.
        _systemRowsToScroll = _ReadSystemRowsToScroll();
    }

    bool TerminalPage::IsRunningElevated() const noexcept
    {
        // GH#2455 - Make sure to try/catch calls to Application::Current,
        // because that _won't_ be an instance of TerminalApp::App in the
        // LocalTests
        try
        {
            return Application::Current().as<TerminalApp::App>().Logic().IsRunningElevated();
        }
        CATCH_LOG();
        return false;
    }
    bool TerminalPage::CanDragDrop() const noexcept
    {
        try
        {
            return Application::Current().as<TerminalApp::App>().Logic().CanDragDrop();
        }
        CATCH_LOG();
        return true;
    }

    void TerminalPage::Create()
    {
        // Hookup the key bindings
        _HookupKeyBindings(_settings.ActionMap());

        _tabContent = this->TabContent();
        _tabRow = this->TabRow();
        _tabView = _tabRow.TabView();
        _rearranging = false;

        const auto canDragDrop = CanDragDrop();

        _tabView.CanReorderTabs(canDragDrop);
        _tabView.CanDragTabs(canDragDrop);
        _tabView.TabDragStarting({ get_weak(), &TerminalPage::_TabDragStarted });
        _tabView.TabDragCompleted({ get_weak(), &TerminalPage::_TabDragCompleted });

        auto tabRowImpl = winrt::get_self<implementation::TabRowControl>(_tabRow);
        _newTabButton = tabRowImpl->NewTabButton();

        if (_settings.GlobalSettings().ShowTabsInTitlebar())
        {
            // Remove the TabView from the page. We'll hang on to it, we need to
            // put it in the titlebar.
            uint32_t index = 0;
            if (this->Root().Children().IndexOf(_tabRow, index))
            {
                this->Root().Children().RemoveAt(index);
            }

            // Inform the host that our titlebar content has changed.
            SetTitleBarContent.raise(*this, _tabRow);

            // GH#13143 Manually set the tab row's background to transparent here.
            //
            // We're doing it this way because ThemeResources are tricky. We
            // default in XAML to using the appropriate ThemeResource background
            // color for our TabRow. When tabs in the titlebar are _disabled_,
            // this will ensure that the tab row has the correct theme-dependent
            // value. When tabs in the titlebar are _enabled_ (the default),
            // we'll switch the BG to Transparent, to let the Titlebar Control's
            // background be used as the BG for the tab row.
            //
            // We can't do it the other way around (default to Transparent, only
            // switch to a color when disabling tabs in the titlebar), because
            // looking up the correct ThemeResource from and App dictionary is a
            // capital-H Hard problem.
            const auto transparent = Media::SolidColorBrush();
            transparent.Color(Windows::UI::Colors::Transparent());
            _tabRow.Background(transparent);
        }
        _updateThemeColors();

        // Initialize the state of the CloseButtonOverlayMode property of
        // our TabView, to match the tab.showCloseButton property in the theme.
        if (const auto theme = _settings.GlobalSettings().CurrentTheme())
        {
            const auto visibility = theme.Tab() ? theme.Tab().ShowCloseButton() : Settings::Model::TabCloseButtonVisibility::Always;

            _tabItemMiddleClickHookEnabled = visibility == Settings::Model::TabCloseButtonVisibility::Never;

            switch (visibility)
            {
            case Settings::Model::TabCloseButtonVisibility::Never:
                _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::Auto);
                break;
            case Settings::Model::TabCloseButtonVisibility::Hover:
                _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::OnPointerOver);
                break;
            default:
                _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::Always);
                break;
            }
        }

        // Hookup our event handlers to the ShortcutActionDispatch
        _RegisterActionCallbacks();

        //Event Bindings (Early)
        _newTabButton.Click([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuDefaultButtonClicked",
                    TraceLoggingDescription("Event emitted when the default button from the new tab split button is invoked"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

                page->_OpenNewTerminalViaDropdown(NewTerminalArgs());
            }
        });
        _newTabButton.Drop({ get_weak(), &TerminalPage::_NewTerminalByDrop });
        _tabView.SelectionChanged({ this, &TerminalPage::_OnTabSelectionChanged });
        _tabView.TabCloseRequested({ this, &TerminalPage::_OnTabCloseRequested });
        _tabView.TabItemsChanged({ this, &TerminalPage::_OnTabItemsChanged });

        _tabView.TabDragStarting({ this, &TerminalPage::_onTabDragStarting });
        _tabView.TabStripDragOver({ this, &TerminalPage::_onTabStripDragOver });
        _tabView.TabStripDrop({ this, &TerminalPage::_onTabStripDrop });
        _tabView.TabDroppedOutside({ this, &TerminalPage::_onTabDroppedOutside });

        _CreateNewTabFlyout();

        _UpdateTabWidthMode();

        // Settings AllowDependentAnimations will affect whether animations are
        // enabled application-wide, so we don't need to check it each time we
        // want to create an animation.
        WUX::Media::Animation::Timeline::AllowDependentAnimations(!_settings.GlobalSettings().DisableAnimations());

        // Once the page is actually laid out on the screen, trigger all our
        // startup actions. Things like Panes need to know at least how big the
        // window will be, so they can subdivide that space.
        //
        // _OnFirstLayout will remove this handler so it doesn't get called more than once.
        _layoutUpdatedRevoker = _tabContent.LayoutUpdated(winrt::auto_revoke, { this, &TerminalPage::_OnFirstLayout });

        _isAlwaysOnTop = _settings.GlobalSettings().AlwaysOnTop();
        _showTabsFullscreen = _settings.GlobalSettings().ShowTabsFullscreen();

        // Workspaces sidebar shell (gated by experimental.workspaces.enabled).
        // Slice 1 step A: render an empty sidebar + a thin draggable splitter,
        // restoring the persisted width when present so the layout survives
        // restart. Live workspace rows land in subsequent steps.
        if (_settings.GlobalSettings().WorkspacesEnabled())
        {
            constexpr double kDefaultSidebarWidth = 240.0;
            constexpr double kMinSidebarWidth = 120.0;
            constexpr double kMaxSidebarWidth = 600.0;
            const auto width = std::clamp(_persistedSidebarWidth.value_or(kDefaultSidebarWidth), kMinSidebarWidth, kMaxSidebarWidth);
            WorkspaceSidebarColumn().Width(GridLengthHelper::FromPixels(width));
            WorkspaceSplitterColumn().Width(GridLengthHelper::FromPixels(4.0));
            WorkspaceSidebar().Visibility(Visibility::Visible);
            WorkspaceSplitter().Visibility(Visibility::Visible);

            // Re-clamp on window-size changes so a too-narrow window can't
            // push the splitter past the content edge.
            WorkspaceContentRoot().SizeChanged([weakThis = get_weak()](auto&&, auto&&) {
                if (auto self = weakThis.get())
                {
                    self->_ClampWorkspaceSidebarWidth();
                }
            });

            // Take ownership of the structural workspace state for this
            // window. Restore from the persisted `workspaceLayout` blob when
            // TerminalWindow handed one over; otherwise (no prior state, or
            // a parse-failure that the TerminalWindow side suppressed)
            // bootstrap one default-profile workspace via the cascade
            // endpoint. The two paths converge on a non-empty `_workspaces`.
            _workspaces = std::make_unique<::TerminalApp::WorkspaceList>();
            if (_persistedWorkspaceState.has_value())
            {
                _HydrateWorkspacesFromPersistedState();
            }
            if (_workspaces->Count() == 0)
            {
                _BootstrapDefaultWorkspace();
            }
            _RebuildWorkspaceSidebar();

            // Slice 1 step #4: hide the legacy top tab strip and integrate
            // a workspace chrome strip in its place. The chrome (hamburger,
            // +New Workspace SplitButton, centered title) lives inside the
            // existing TabRowControl — same control, swapped contents — so
            // its placement in the titlebar via SetTitleBarContent and the
            // surrounding DragBar/MinMaxClose wiring stay unchanged.
            //
            // EnableWorkspacesMode (which collapses the TabView) is deferred
            // to _OnFirstLayout, AFTER _MaterializeWorkspace creates the
            // first tab and its SelectionChanged-driven content mount runs.
            // Collapsing TabView before its initial layout/selection cycle
            // skips the pane mount into _tabContent — symptom: chrome
            // visible but pane area stays transparent.
            _BuildWorkspaceChromeMenus();
            _UpdateWorkspaceTitleInChrome();
        }

        // DON'T set up Toasts/TeachingTips here. They should be loaded and
        // initialized the first time they're opened, in whatever method opens
        // them.

        _tabRow.ShowElevationShield(IsRunningElevated() && _settings.GlobalSettings().ShowAdminShield());

        _adjustProcessPriorityThrottled = std::make_shared<ThrottledFunc<>>(
            DispatcherQueue::GetForCurrentThread(),
            til::throttled_func_options{
                .delay = std::chrono::milliseconds{ 100 },
                .debounce = true,
                .trailing = true,
            },
            [=]() {
                _adjustProcessPriority();
            });
    }

    Windows::UI::Xaml::Automation::Peers::AutomationPeer TerminalPage::OnCreateAutomationPeer()
    {
        return Automation::Peers::FrameworkElementAutomationPeer(*this);
    }

    // Method Description:
    // - This is a bit of trickiness: If we're running unelevated, and the user
    //   passed in only --elevate actions, the we don't _actually_ want to
    //   restore the layouts here. We're not _actually_ about to create the
    //   window. We're simply going to toss the commandlines
    // Arguments:
    // - <none>
    // Return Value:
    // - true if we're not elevated but all relevant pane-spawning actions are elevated
    bool TerminalPage::ShouldImmediatelyHandoffToElevated(const CascadiaSettings& settings) const
    {
        if (_startupActions.empty() || _startupConnection || IsRunningElevated())
        {
            // No point in handing off if we got no startup actions, or we're already elevated.
            // Also, we shouldn't need to elevate handoff ConPTY connections.
            assert(!_startupConnection);
            return false;
        }

        // Check that there's at least one action that's not just an elevated newTab action.
        for (const auto& action : _startupActions)
        {
            // Only new terminal panes will be requesting elevation.
            NewTerminalArgs newTerminalArgs{ nullptr };

            if (action.Action() == ShortcutAction::NewTab)
            {
                const auto& args{ action.Args().try_as<NewTabArgs>() };
                if (args)
                {
                    newTerminalArgs = args.ContentArgs().try_as<NewTerminalArgs>();
                }
                else
                {
                    // This was a nt action that didn't have any args. The default
                    // profile may want to be elevated, so don't just early return.
                }
            }
            else if (action.Action() == ShortcutAction::SplitPane)
            {
                const auto& args{ action.Args().try_as<SplitPaneArgs>() };
                if (args)
                {
                    newTerminalArgs = args.ContentArgs().try_as<NewTerminalArgs>();
                }
                else
                {
                    // This was a nt action that didn't have any args. The default
                    // profile may want to be elevated, so don't just early return.
                }
            }
            else
            {
                // This was not a new tab or split pane action.
                // This doesn't affect the outcome
                continue;
            }

            // It's possible that newTerminalArgs is null here.
            // GetProfileForArgs should be resilient to that.
            const auto profile{ settings.GetProfileForArgs(newTerminalArgs) };
            if (profile.Elevate())
            {
                continue;
            }

            // The profile didn't want to be elevated, and we aren't elevated.
            // We're going to open at least one tab, so return false.
            return false;
        }
        return true;
    }

    // Method Description:
    // - Escape hatch for immediately dispatching requests to elevated windows
    //   when first launched. At this point in startup, the window doesn't exist
    //   yet, XAML hasn't been started, but we need to dispatch these actions.
    //   We can't just go through ProcessStartupActions, because that processes
    //   the actions async using the XAML dispatcher (which doesn't exist yet)
    // - DON'T CALL THIS if you haven't already checked
    //   ShouldImmediatelyHandoffToElevated. If you're thinking about calling
    //   this outside of the one place it's used, that's probably the wrong
    //   solution.
    // Arguments:
    // - settings: the settings we should use for dispatching these actions. At
    //   this point in startup, we hadn't otherwise been initialized with these,
    //   so use them now.
    // Return Value:
    // - <none>
    void TerminalPage::HandoffToElevated(const CascadiaSettings& settings)
    {
        if (_startupActions.empty())
        {
            return;
        }

        // Hookup our event handlers to the ShortcutActionDispatch
        _settings = settings;
        _HookupKeyBindings(_settings.ActionMap());
        _RegisterActionCallbacks();

        for (const auto& action : _startupActions)
        {
            // only process new tabs and split panes. They're all going to the elevated window anyways.
            if (action.Action() == ShortcutAction::NewTab || action.Action() == ShortcutAction::SplitPane)
            {
                _actionDispatch->DoAction(action);
            }
        }
    }

    safe_void_coroutine TerminalPage::_NewTerminalByDrop(const Windows::Foundation::IInspectable&, winrt::Windows::UI::Xaml::DragEventArgs e)
    try
    {
        const auto data = e.DataView();
        if (!data.Contains(StandardDataFormats::StorageItems()))
        {
            co_return;
        }

        const auto weakThis = get_weak();
        const auto items = co_await data.GetStorageItemsAsync();
        const auto strongThis = weakThis.get();
        if (!strongThis)
        {
            co_return;
        }

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "NewTabByDragDrop",
            TraceLoggingDescription("Event emitted when the user drag&drops onto the new tab button"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

        for (const auto& item : items)
        {
            auto directory = item.Path();

            std::filesystem::path path(std::wstring_view{ directory });
            if (!std::filesystem::is_directory(path))
            {
                directory = winrt::hstring{ path.parent_path().native() };
            }

            NewTerminalArgs args;
            args.StartingDirectory(directory);
            _OpenNewTerminalViaDropdown(args);
        }
    }
    CATCH_LOG()

    // Method Description:
    // - This method is called once command palette action was chosen for dispatching
    //   We'll use this event to dispatch this command.
    // Arguments:
    // - command - command to dispatch
    // Return Value:
    // - <none>
    void TerminalPage::_OnDispatchCommandRequested(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::Command& command)
    {
        const auto& actionAndArgs = command.ActionAndArgs();
        _actionDispatch->DoAction(sender, actionAndArgs);
    }

    // Method Description:
    // - This method is called once command palette command line was chosen for execution
    //   We'll use this event to create a command line execution command and dispatch it.
    // Arguments:
    // - command - command to dispatch
    // Return Value:
    // - <none>
    void TerminalPage::_OnCommandLineExecutionRequested(const IInspectable& /*sender*/, const winrt::hstring& commandLine)
    {
        ExecuteCommandlineArgs args{ commandLine };
        ActionAndArgs actionAndArgs{ ShortcutAction::ExecuteCommandline, args };
        _actionDispatch->DoAction(actionAndArgs);
    }

    // Method Description:
    // - This method is called once on startup, on the first LayoutUpdated event.
    //   We'll use this event to know that we have an ActualWidth and
    //   ActualHeight, so we can now attempt to process our list of startup
    //   actions.
    // - We'll remove this event handler when the event is first handled.
    // - If there are no startup actions, we'll open a single tab with the
    //   default profile.
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void TerminalPage::_OnFirstLayout(const IInspectable& /*sender*/, const IInspectable& /*eventArgs*/)
    {
        // Only let this succeed once.
        _layoutUpdatedRevoker.revoke();

        // This event fires every time the layout changes, but it is always the
        // last one to fire in any layout change chain. That gives us great
        // flexibility in finding the right point at which to initialize our
        // renderer (and our terminal). Any earlier than the last layout update
        // and we may not know the terminal's starting size.
        if (_startupState == StartupState::NotInitialized)
        {
            _startupState = StartupState::InStartup;

            if (_startupConnection)
            {
                CreateTabFromConnection(std::move(_startupConnection));
            }
            else if (_workspaces)
            {
                // Workspaces mode owns the initial-content path. Materialize
                // the active workspace's tab via the workspace pipeline and
                // skip legacy startup-actions replay; persistence-restore
                // for `_workspaces` is a separate (unlanded) step.
                if (const auto active = _workspaces->ActiveIndex())
                {
                    _MaterializeWorkspace(*active);
                }

                // Now that the first tab is in place and its initial
                // SelectionChanged has mounted the pane into _tabContent,
                // it's safe to collapse the legacy tab strip and let the
                // workspace chrome take over TabRow's appearance. Doing
                // this before _MaterializeWorkspace skips the pane mount.
                _tabRow.EnableWorkspacesMode();
            }
            else if (!_startupActions.empty())
            {
                ProcessStartupActions(std::move(_startupActions));
            }

            _CompleteInitialization();
        }
    }

    // Method Description:
    // - Process all the startup actions in the provided list of startup
    //   actions. We'll do this all at once here.
    // Arguments:
    // - actions: a winrt vector of actions to process. Note that this must NOT
    //   be an IVector&, because we need the collection to be accessible on the
    //   other side of the co_await.
    // - initial: if true, we're parsing these args during startup, and we
    //   should fire an Initialized event.
    // - cwd: If not empty, we should try switching to this provided directory
    //   while processing these actions. This will allow something like `wt -w 0
    //   nt -d .` from inside another directory to work as expected.
    // Return Value:
    // - <none>
    safe_void_coroutine TerminalPage::ProcessStartupActions(std::vector<ActionAndArgs> actions, const winrt::hstring cwd, const winrt::hstring env)
    {
        const auto strong = get_strong();

        // If the caller provided a CWD, "switch" to that directory, then switch
        // back once we're done.
        auto originalVirtualCwd{ _WindowProperties.VirtualWorkingDirectory() };
        auto originalVirtualEnv{ _WindowProperties.VirtualEnvVars() };
        auto restoreCwd = wil::scope_exit([&]() {
            if (!cwd.empty())
            {
                // ignore errors, we'll just power on through. We'd rather do
                // something rather than fail silently if the directory doesn't
                // actually exist.
                _WindowProperties.VirtualWorkingDirectory(originalVirtualCwd);
                _WindowProperties.VirtualEnvVars(originalVirtualEnv);
            }
        });
        if (!cwd.empty())
        {
            _WindowProperties.VirtualWorkingDirectory(cwd);
            _WindowProperties.VirtualEnvVars(env);
        }

        // The current TerminalWindow & TerminalPage architecture is rather instable
        // and fails to start up if the first tab isn't created synchronously.
        //
        // While that's a fair assumption in on itself, simultaneously WinUI will
        // not assign tab contents a size if they're not shown at least once,
        // which we need however in order to initialize ControlCore with a size.
        //
        // So, we do two things here:
        // * DO NOT suspend if this is the first tab.
        // * DO suspend between the creation of panes (or tabs) in order to allow
        //   WinUI to layout the new controls and for ControlCore to get a size.
        //
        // This same logic is also applied to CreateTabFromConnection.
        //
        // See GH#13136.
        auto suspend = _tabs.Size() > 0;

        for (size_t i = 0; i < actions.size(); ++i)
        {
            if (suspend)
            {
                co_await wil::resume_foreground(Dispatcher(), CoreDispatcherPriority::Low);
            }

            _actionDispatch->DoAction(actions[i]);
            suspend = true;
        }

        // GH#6586: now that we're done processing all startup commands,
        // focus the active control. This will work as expected for both
        // commandline invocations and for `wt` action invocations.
        if (const auto& tabImpl{ _GetFocusedTabImpl() })
        {
            if (const auto& content{ tabImpl->GetActiveContent() })
            {
                content.Focus(FocusState::Programmatic);
            }
        }
    }

    safe_void_coroutine TerminalPage::CreateTabFromConnection(ITerminalConnection connection)
    {
        const auto strong = get_strong();

        // This is the exact same logic as in ProcessStartupActions.
        if (_tabs.Size() > 0)
        {
            co_await wil::resume_foreground(Dispatcher(), CoreDispatcherPriority::Low);
        }

        NewTerminalArgs newTerminalArgs;

        if (const auto conpty = connection.try_as<ConptyConnection>())
        {
            newTerminalArgs.Commandline(conpty.Commandline());
            newTerminalArgs.TabTitle(conpty.StartingTitle());
        }

        // GH #12370: We absolutely cannot allow a defterm connection to
        // auto-elevate. Defterm doesn't work for elevated scenarios in the
        // first place. If we try accepting the connection, the spawning an
        // elevated version of the Terminal with that profile... that's a
        // recipe for disaster. We won't ever open up a tab in this window.
        newTerminalArgs.Elevate(false);

        const auto newPane = _MakePane(newTerminalArgs, nullptr, std::move(connection));
        newPane->WalkTree([](const auto& pane) {
            pane->FinalizeConfigurationGivenDefault();
        });
        _CreateNewTabFromPane(newPane);
    }

    // Method Description:
    // - Perform and steps that need to be done once our initial state is all
    //   set up. This includes entering fullscreen mode and firing our
    //   Initialized event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    safe_void_coroutine TerminalPage::_CompleteInitialization()
    {
        _startupState = StartupState::Initialized;

        // GH#632 - It's possible that the user tried to create the terminal
        // with only one tab, with only an elevated profile. If that happens,
        // we'll create _another_ process to host the elevated version of that
        // profile. This can happen from the jumplist, or if the default profile
        // is `elevate:true`, or from the commandline.
        //
        // However, we need to make sure to close this window in that scenario.
        // Since there aren't any _tabs_ in this window, we won't ever get a
        // closed event. So do it manually.
        //
        // GH#12267: Make sure that we don't instantly close ourselves when
        // we're readying to accept a defterm connection. In that case, we don't
        // have a tab yet, but will once we're initialized.
        if (_tabs.Size() == 0)
        {
            CloseWindowRequested.raise(*this, nullptr);
            co_return;
        }
        else
        {
            // GH#11561: When we start up, our window is initially just a frame
            // with a transparent content area. We're gonna do all this startup
            // init on the UI thread, so the UI won't actually paint till it's
            // all done. This results in a few frames where the frame is
            // visible, before the page paints for the first time, before any
            // tabs appears, etc.
            //
            // To mitigate this, we're gonna wait for the UI thread to finish
            // everything it's gotta do for the initial init, and _then_ fire
            // our Initialized event. By waiting for everything else to finish
            // (CoreDispatcherPriority::Low), we let all the tabs and panes
            // actually get created. In the window layer, we're gonna cloak the
            // window till this event is fired, so we don't actually see this
            // frame until we're actually all ready to go.
            //
            // This will result in the window seemingly not loading as fast, but
            // it will actually take exactly the same amount of time before it's
            // usable.
            //
            // We also experimented with drawing a solid BG color before the
            // initialization is finished. However, there are still a few frames
            // after the frame is displayed before the XAML content first draws,
            // so that didn't actually resolve any issues.
            _ShowCorruptStateRecoveredInfoBarIfNeeded();

            Dispatcher().RunAsync(CoreDispatcherPriority::Low, [weak = get_weak()]() {
                if (auto self{ weak.get() })
                {
                    self->Initialized.raise(*self, nullptr);
                }
            });
        }
    }

    // Method Description:
    // - Show a dialog with "About" information. Displays the app's Display
    //   Name, version, getting started link, source code link, documentation link, release
    //   Notes link, send feedback link and privacy policy link.
    void TerminalPage::_ShowAboutDialog()
    {
        _ShowDialogHelper(L"AboutDialog");
    }

    winrt::hstring TerminalPage::ApplicationDisplayName()
    {
        return CascadiaSettings::ApplicationDisplayName();
    }

    winrt::hstring TerminalPage::ApplicationVersion()
    {
        return CascadiaSettings::ApplicationVersion();
    }

    // Method Description:
    // - Helper to show a content dialog
    // - We only open a content dialog if there isn't one open already
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowDialogHelper(const std::wstring_view& name)
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            co_return co_await presenter.ShowDialog(FindName(name).try_as<WUX::Controls::ContentDialog>());
        }
        co_return ContentDialogResult::None;
    }

    // Method Description:
    // - Displays a dialog to warn the user that they are about to close all open windows.
    //   Once the user clicks the OK button, shut down the application.
    //   If cancel is clicked, the dialog will close.
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowQuitDialog()
    {
        return _ShowDialogHelper(L"QuitDialog");
    }

    // Method Description:
    // - Displays a dialog for warnings found while closing the terminal app using
    //   key binding with multiple tabs opened. Display messages to warn user
    //   that more than 1 tab is opened, and once the user clicks the OK button, remove
    //   all the tabs and shut down and app. If cancel is clicked, the dialog will close
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowCloseWarningDialog()
    {
        // Force-load the deferred dialog so the named TextBlock child exists
        // before we set its text. Same pattern as MultiLinePasteDialog.
        std::ignore = FindName(L"CloseAllDialog");

        if (_workspaces)
        {
            const auto wsCount = _workspaces->Count();
            const auto paneCount = _CountAllPanesAcrossWorkspaces();
            const auto fmtTemplate = wsCount == 1
                ? RS_(L"CloseAllDialog_WorkspacesContent_Single")
                : RS_(L"CloseAllDialog_WorkspacesContent_Multi");
            const auto body = wsCount == 1
                ? fmt::format(fmt::runtime(std::wstring{ fmtTemplate }), paneCount)
                : fmt::format(fmt::runtime(std::wstring{ fmtTemplate }), wsCount, paneCount);
            CloseAllDialogContentText().Text(body);
            CloseAllDialogContentText().Visibility(Visibility::Visible);
        }
        else
        {
            CloseAllDialogContentText().Visibility(Visibility::Collapsed);
        }
        return _ShowDialogHelper(L"CloseAllDialog");
    }

    // Method Description:
    // - Displays a dialog for warnings found while closing the terminal tab marked as read-only
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowCloseReadOnlyDialog()
    {
        return _ShowDialogHelper(L"CloseReadOnlyDialog");
    }

    // Method Description:
    // - Displays a dialog to warn the user about the fact that the text that
    //   they are trying to paste contains the "new line" character which can
    //   have the effect of starting commands without the user's knowledge if
    //   it is pasted on a shell where the "new line" character marks the end
    //   of a command.
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowMultiLinePasteWarningDialog()
    {
        return _ShowDialogHelper(L"MultiLinePasteDialog");
    }

    // Method Description:
    // - Displays a dialog to warn the user about the fact that the text that
    //   they are trying to paste is very long, in case they did not mean to
    //   paste it but pressed the paste shortcut by accident.
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowLargePasteWarningDialog()
    {
        return _ShowDialogHelper(L"LargePasteDialog");
    }

    // Method Description:
    // - Builds the flyout (dropdown) attached to the new tab button, and
    //   attaches it to the button. Populates the flyout with one entry per
    //   Profile, displaying the profile's name. Clicking each flyout item will
    //   open a new tab with that profile.
    //   Below the profiles are the static menu items: settings, command palette
    void TerminalPage::_CreateNewTabFlyout()
    {
        auto newTabFlyout = WUX::Controls::MenuFlyout{};
        newTabFlyout.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);

        // Create profile entries from the NewTabMenu configuration using a
        // recursive helper function. This returns a std::vector of FlyoutItemBases,
        // that we then add to our Flyout.
        auto entries = _settings.GlobalSettings().NewTabMenu();
        auto items = _CreateNewTabFlyoutItems(entries);
        for (const auto& item : items)
        {
            newTabFlyout.Items().Append(item);
        }

        // add menu separator
        auto separatorItem = WUX::Controls::MenuFlyoutSeparator{};
        newTabFlyout.Items().Append(separatorItem);

        // add static items
        {
            // Create the settings button.
            auto settingsItem = WUX::Controls::MenuFlyoutItem{};
            settingsItem.Text(RS_(L"SettingsMenuItem"));
            const auto settingsToolTip = RS_(L"SettingsToolTip");

            WUX::Controls::ToolTipService::SetToolTip(settingsItem, box_value(settingsToolTip));
            Automation::AutomationProperties::SetHelpText(settingsItem, settingsToolTip);

            WUX::Controls::SymbolIcon ico{};
            ico.Symbol(WUX::Controls::Symbol::Setting);
            settingsItem.Icon(ico);

            settingsItem.Click({ this, &TerminalPage::_SettingsButtonOnClick });
            newTabFlyout.Items().Append(settingsItem);

            auto actionMap = _settings.ActionMap();
            const auto settingsKeyChord{ actionMap.GetKeyBindingForAction(L"Terminal.OpenSettingsUI") };
            if (settingsKeyChord)
            {
                _SetAcceleratorForMenuItem(settingsItem, settingsKeyChord);
            }

            // Create the command palette button.
            auto commandPaletteFlyout = WUX::Controls::MenuFlyoutItem{};
            commandPaletteFlyout.Text(RS_(L"CommandPaletteMenuItem"));
            const auto commandPaletteToolTip = RS_(L"CommandPaletteToolTip");

            WUX::Controls::ToolTipService::SetToolTip(commandPaletteFlyout, box_value(commandPaletteToolTip));
            Automation::AutomationProperties::SetHelpText(commandPaletteFlyout, commandPaletteToolTip);

            WUX::Controls::FontIcon commandPaletteIcon{};
            commandPaletteIcon.Glyph(L"\xE945");
            commandPaletteIcon.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            commandPaletteFlyout.Icon(commandPaletteIcon);

            commandPaletteFlyout.Click({ this, &TerminalPage::_CommandPaletteButtonOnClick });
            newTabFlyout.Items().Append(commandPaletteFlyout);

            const auto commandPaletteKeyChord{ actionMap.GetKeyBindingForAction(L"Terminal.ToggleCommandPalette") };
            if (commandPaletteKeyChord)
            {
                _SetAcceleratorForMenuItem(commandPaletteFlyout, commandPaletteKeyChord);
            }

            // Create the about button.
            auto aboutFlyout = WUX::Controls::MenuFlyoutItem{};
            aboutFlyout.Text(RS_(L"AboutMenuItem"));
            const auto aboutToolTip = RS_(L"AboutToolTip");

            WUX::Controls::ToolTipService::SetToolTip(aboutFlyout, box_value(aboutToolTip));
            Automation::AutomationProperties::SetHelpText(aboutFlyout, aboutToolTip);

            WUX::Controls::SymbolIcon aboutIcon{};
            aboutIcon.Symbol(WUX::Controls::Symbol::Help);
            aboutFlyout.Icon(aboutIcon);

            aboutFlyout.Click({ this, &TerminalPage::_AboutButtonOnClick });
            newTabFlyout.Items().Append(aboutFlyout);
        }

        // Before opening the fly-out set focus on the current tab
        // so no matter how fly-out is closed later on the focus will return to some tab.
        // We cannot do it on closing because if the window loses focus (alt+tab)
        // the closing event is not fired.
        // It is important to set the focus on the tab
        // Since the previous focus location might be discarded in the background,
        // e.g., the command palette will be dismissed by the menu,
        // and then closing the fly-out will move the focus to wrong location.
        newTabFlyout.Opening([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                page->_FocusCurrentTab(true);

                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuOpened",
                    TraceLoggingDescription("Event emitted when the new tab menu is opened"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The Count of tabs currently opened in this window"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
            }
        });
        // Necessary for fly-out sub items to get focus on a tab before collapsing. Related to #15049
        newTabFlyout.Closing([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                if (!page->_commandPaletteIs(Visibility::Visible))
                {
                    page->_FocusCurrentTab(true);
                }

                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuClosed",
                    TraceLoggingDescription("Event emitted when the new tab menu is closed"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The Count of tabs currently opened in this window"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
            }
        });
        _newTabButton.Flyout(newTabFlyout);
    }

    // Method Description:
    // - For a given list of tab menu entries, this method will create the corresponding
    //   list of flyout items. This is a recursive method that calls itself when it comes
    //   across a folder entry.
    std::vector<WUX::Controls::MenuFlyoutItemBase> TerminalPage::_CreateNewTabFlyoutItems(IVector<NewTabMenuEntry> entries, const NewTabFlyoutLeafHandler& leafHandler)
    {
        std::vector<WUX::Controls::MenuFlyoutItemBase> items;

        if (entries == nullptr || entries.Size() == 0)
        {
            return items;
        }

        for (const auto& entry : entries)
        {
            if (entry == nullptr)
            {
                continue;
            }

            switch (entry.Type())
            {
            case NewTabMenuEntryType::Separator:
            {
                items.push_back(WUX::Controls::MenuFlyoutSeparator{});
                break;
            }
            // A folder has a custom name and icon, and has a number of entries that require
            // us to call this method recursively.
            case NewTabMenuEntryType::Folder:
            {
                const auto folderEntry = entry.as<FolderEntry>();
                const auto folderEntries = folderEntry.Entries();

                // If the folder is empty, we should skip the entry if AllowEmpty is false, or
                // when the folder should inline.
                // The IsEmpty check includes semantics for nested (empty) folders
                if (folderEntries.Size() == 0 && (!folderEntry.AllowEmpty() || folderEntry.Inlining() == FolderEntryInlining::Auto))
                {
                    break;
                }

                // Recursively generate flyout items
                auto folderEntryItems = _CreateNewTabFlyoutItems(folderEntries, leafHandler);

                // If the folder should auto-inline and there is only one item, do so.
                if (folderEntry.Inlining() == FolderEntryInlining::Auto && folderEntryItems.size() == 1)
                {
                    for (auto const& folderEntryItem : folderEntryItems)
                    {
                        items.push_back(folderEntryItem);
                    }

                    break;
                }

                // Otherwise, create a flyout
                auto folderItem = WUX::Controls::MenuFlyoutSubItem{};
                folderItem.Text(folderEntry.Name());

                auto icon = _CreateNewTabFlyoutIcon(folderEntry.Icon().Resolved());
                folderItem.Icon(icon);

                for (const auto& folderEntryItem : folderEntryItems)
                {
                    folderItem.Items().Append(folderEntryItem);
                }

                // If the folder is empty, and by now we know we set AllowEmpty to true,
                // create a placeholder item here
                if (folderEntries.Size() == 0)
                {
                    auto placeholder = WUX::Controls::MenuFlyoutItem{};
                    placeholder.Text(RS_(L"NewTabMenuFolderEmpty"));
                    placeholder.IsEnabled(false);

                    folderItem.Items().Append(placeholder);
                }

                items.push_back(folderItem);
                break;
            }
            // Any "collection entry" will simply make us add each profile in the collection
            // separately. This collection is stored as a map <int, Profile>, so the correct
            // profile index is already known.
            case NewTabMenuEntryType::RemainingProfiles:
            case NewTabMenuEntryType::MatchProfiles:
            {
                const auto remainingProfilesEntry = entry.as<ProfileCollectionEntry>();
                if (remainingProfilesEntry.Profiles() == nullptr)
                {
                    break;
                }

                for (auto&& [profileIndex, remainingProfile] : remainingProfilesEntry.Profiles())
                {
                    items.push_back(_CreateNewTabFlyoutProfile(remainingProfile, profileIndex, {}, leafHandler));
                }

                break;
            }
            // A single profile, the profile index is also given in the entry
            case NewTabMenuEntryType::Profile:
            {
                const auto profileEntry = entry.as<ProfileEntry>();
                if (profileEntry.Profile() == nullptr)
                {
                    break;
                }

                auto profileItem = _CreateNewTabFlyoutProfile(profileEntry.Profile(), profileEntry.ProfileIndex(), profileEntry.Icon().Resolved(), leafHandler);
                items.push_back(profileItem);
                break;
            }
            case NewTabMenuEntryType::Action:
            {
                const auto actionEntry = entry.as<ActionEntry>();
                const auto actionId = actionEntry.ActionId();
                if (_settings.ActionMap().GetActionByID(actionId))
                {
                    auto actionItem = _CreateNewTabFlyoutAction(actionId, actionEntry.Icon().Resolved());
                    items.push_back(actionItem);
                }

                break;
            }
            }
        }

        return items;
    }

    // Method Description:
    // - This method creates a flyout menu item for a given profile with the given index.
    //   It makes sure to set the correct icon, keybinding, and click-action.
    WUX::Controls::MenuFlyoutItem TerminalPage::_CreateNewTabFlyoutProfile(const Profile profile, int profileIndex, const winrt::hstring& iconPathOverride, const NewTabFlyoutLeafHandler& leafHandler)
    {
        auto profileMenuItem = WUX::Controls::MenuFlyoutItem{};

        // Add the keyboard shortcuts based on the number of profiles defined
        // Look for a keychord that is bound to the equivalent
        // NewTab(ProfileIndex=N) action
        NewTerminalArgs newTerminalArgs{ profileIndex };
        NewTabArgs newTabArgs{ newTerminalArgs };
        const auto id = fmt::format(FMT_COMPILE(L"Terminal.OpenNewTabProfile{}"), profileIndex);
        const auto profileKeyChord{ _settings.ActionMap().GetKeyBindingForAction(id) };

        // make sure we find one to display
        if (profileKeyChord)
        {
            _SetAcceleratorForMenuItem(profileMenuItem, profileKeyChord);
        }

        auto profileName = profile.Name();
        profileMenuItem.Text(profileName);

        // If a custom icon path has been specified, set it as the icon for
        // this flyout item. Otherwise, if an icon is set for this profile, set that icon
        // for this flyout item.
        const auto& iconPath = iconPathOverride.empty() ? profile.Icon().Resolved() : iconPathOverride;
        if (!iconPath.empty())
        {
            const auto icon = _CreateNewTabFlyoutIcon(iconPath);
            profileMenuItem.Icon(icon);
        }

        if (profile.Guid() == _settings.GlobalSettings().DefaultProfile())
        {
            // Contrast the default profile with others in font weight.
            profileMenuItem.FontWeight(FontWeights::Bold());
        }

        auto newTabRun = WUX::Documents::Run();
        newTabRun.Text(RS_(L"NewTabRun/Text"));
        auto newPaneRun = WUX::Documents::Run();
        newPaneRun.Text(RS_(L"NewPaneRun/Text"));
        newPaneRun.FontStyle(FontStyle::Italic);
        auto newWindowRun = WUX::Documents::Run();
        newWindowRun.Text(RS_(L"NewWindowRun/Text"));
        newWindowRun.FontStyle(FontStyle::Italic);
        auto elevatedRun = WUX::Documents::Run();
        elevatedRun.Text(RS_(L"ElevatedRun/Text"));
        elevatedRun.FontStyle(FontStyle::Italic);

        auto textBlock = WUX::Controls::TextBlock{};
        textBlock.Inlines().Append(newTabRun);
        textBlock.Inlines().Append(WUX::Documents::LineBreak{});
        textBlock.Inlines().Append(newPaneRun);
        textBlock.Inlines().Append(WUX::Documents::LineBreak{});
        textBlock.Inlines().Append(newWindowRun);
        textBlock.Inlines().Append(WUX::Documents::LineBreak{});
        textBlock.Inlines().Append(elevatedRun);

        auto toolTip = WUX::Controls::ToolTip{};
        toolTip.Content(textBlock);
        WUX::Controls::ToolTipService::SetToolTip(profileMenuItem, toolTip);

        profileMenuItem.Click([profileIndex, weakThis{ get_weak() }, leafHandler](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuItemClicked",
                    TraceLoggingDescription("Event emitted when an item from the new tab menu is invoked"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
                    TraceLoggingValue("Profile", "ItemType", "The type of item that was clicked in the new tab menu"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

                NewTerminalArgs newTerminalArgs{ profileIndex };
                if (leafHandler)
                {
                    leafHandler(newTerminalArgs);
                }
                else
                {
                    page->_OpenNewTerminalViaDropdown(newTerminalArgs);
                }
            }
        });

        // Using the static method on the base class seems to do what we want in terms of placement.
        WUX::Controls::Primitives::FlyoutBase::SetAttachedFlyout(profileMenuItem, _CreateRunAsAdminFlyout(profileIndex));

        // Since we are not setting the ContextFlyout property of the item we have to handle the ContextRequested event
        // and rely on the base class to show our menu.
        profileMenuItem.ContextRequested([profileMenuItem](auto&&, auto&&) {
            WUX::Controls::Primitives::FlyoutBase::ShowAttachedFlyout(profileMenuItem);
        });

        return profileMenuItem;
    }

    // Method Description:
    // - This method creates a flyout menu item for a given action
    //   It makes sure to set the correct icon, keybinding, and click-action.
    WUX::Controls::MenuFlyoutItem TerminalPage::_CreateNewTabFlyoutAction(const winrt::hstring& actionId, const winrt::hstring& iconPathOverride)
    {
        auto actionMenuItem = WUX::Controls::MenuFlyoutItem{};
        const auto action{ _settings.ActionMap().GetActionByID(actionId) };
        const auto actionKeyChord{ _settings.ActionMap().GetKeyBindingForAction(actionId) };

        if (actionKeyChord)
        {
            _SetAcceleratorForMenuItem(actionMenuItem, actionKeyChord);
        }

        actionMenuItem.Text(action.Name());

        // If a custom icon path has been specified, set it as the icon for
        // this flyout item. Otherwise, if an icon is set for this action, set that icon
        // for this flyout item.
        const auto& iconPath = iconPathOverride.empty() ? action.Icon().Resolved() : iconPathOverride;
        if (!iconPath.empty())
        {
            const auto icon = _CreateNewTabFlyoutIcon(iconPath);
            actionMenuItem.Icon(icon);
        }

        actionMenuItem.Click([action, weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuItemClicked",
                    TraceLoggingDescription("Event emitted when an item from the new tab menu is invoked"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
                    TraceLoggingValue("Action", "ItemType", "The type of item that was clicked in the new tab menu"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

                page->_actionDispatch->DoAction(action.ActionAndArgs());
            }
        });

        return actionMenuItem;
    }

    // Method Description:
    // - Helper method to create an IconElement that can be passed to MenuFlyoutItems and
    //   MenuFlyoutSubItems
    IconElement TerminalPage::_CreateNewTabFlyoutIcon(const winrt::hstring& iconSource)
    {
        if (iconSource.empty())
        {
            return nullptr;
        }

        auto icon = UI::IconPathConverter::IconWUX(iconSource);
        Automation::AutomationProperties::SetAccessibilityView(icon, Automation::Peers::AccessibilityView::Raw);

        return icon;
    }

    // Function Description:
    // Called when the openNewTabDropdown keybinding is used.
    // Shows the dropdown flyout.
    void TerminalPage::_OpenNewTabDropdown()
    {
        _newTabButton.Flyout().ShowAt(_newTabButton);
    }

    void TerminalPage::_OpenNewTerminalViaDropdown(const NewTerminalArgs newTerminalArgs)
    {
        // if alt is pressed, open a pane
        const auto window = CoreWindow::GetForCurrentThread();
        const auto rAltState = window.GetKeyState(VirtualKey::RightMenu);
        const auto lAltState = window.GetKeyState(VirtualKey::LeftMenu);
        const auto altPressed = WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) ||
                                WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);

        const auto shiftState{ window.GetKeyState(VirtualKey::Shift) };
        const auto rShiftState = window.GetKeyState(VirtualKey::RightShift);
        const auto lShiftState = window.GetKeyState(VirtualKey::LeftShift);
        const auto shiftPressed{ WI_IsFlagSet(shiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(lShiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(rShiftState, CoreVirtualKeyStates::Down) };

        const auto ctrlState{ window.GetKeyState(VirtualKey::Control) };
        const auto rCtrlState = window.GetKeyState(VirtualKey::RightControl);
        const auto lCtrlState = window.GetKeyState(VirtualKey::LeftControl);
        const auto ctrlPressed{ WI_IsFlagSet(ctrlState, CoreVirtualKeyStates::Down) ||
                                WI_IsFlagSet(rCtrlState, CoreVirtualKeyStates::Down) ||
                                WI_IsFlagSet(lCtrlState, CoreVirtualKeyStates::Down) };

        // Check for DebugTap
        auto debugTap = this->_settings.GlobalSettings().DebugFeaturesEnabled() &&
                        WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) &&
                        WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);

        const auto dispatchToElevatedWindow = ctrlPressed && !IsRunningElevated();

        auto sessionType = "";
        if ((shiftPressed || dispatchToElevatedWindow) && !debugTap)
        {
            // Manually fill in the evaluated profile.
            if (newTerminalArgs.ProfileIndex() != nullptr)
            {
                // We want to promote the index to a GUID because there is no "launch to profile index" command.
                const auto profile = _settings.GetProfileForArgs(newTerminalArgs);
                if (profile)
                {
                    newTerminalArgs.Profile(::Microsoft::Console::Utils::GuidToString(profile.Guid()));
                    newTerminalArgs.StartingDirectory(_evaluatePathForCwd(profile.EvaluatedStartingDirectory()));
                }
            }

            if (dispatchToElevatedWindow)
            {
                _OpenElevatedWT(newTerminalArgs);
                sessionType = "ElevatedWindow";
            }
            else
            {
                _OpenNewWindow(newTerminalArgs);
                sessionType = "Window";
            }
        }
        else
        {
            const auto newPane = _MakePane(newTerminalArgs);
            // If the newTerminalArgs caused us to open an elevated window
            // instead of creating a pane, it may have returned nullptr. Just do
            // nothing then.
            if (!newPane)
            {
                return;
            }
            if (altPressed && !debugTap)
            {
                this->_SplitPane(_GetFocusedTabImpl(),
                                 SplitDirection::Automatic,
                                 0.5f,
                                 newPane);
                sessionType = "Pane";
            }
            else
            {
                _CreateNewTabFromPane(newPane);
                sessionType = "Tab";
            }
        }

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "NewTabMenuCreatedNewTerminalSession",
            TraceLoggingDescription("Event emitted when a new terminal was created via the new tab menu"),
            TraceLoggingValue(NumberOfTabs(), "NewTabCount", "The count of tabs currently opened in this window"),
            TraceLoggingValue(sessionType, "SessionType", "The type of session that was created"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    std::wstring TerminalPage::_evaluatePathForCwd(const std::wstring_view path)
    {
        return Utils::EvaluateStartingDirectory(_WindowProperties.VirtualWorkingDirectory(), path);
    }

    // Method Description:
    // - Creates a new connection based on the profile settings
    // Arguments:
    // - the profile we want the settings from
    // - the terminal settings
    // Return value:
    // - the desired connection
    namespace
    {
        // PowerShell builtin Set-Location updates $PWD but does NOT call
        // SetCurrentDirectoryW, so the process's PEB CurrentDirectory stays
        // pinned at the launch dir. PEB introspection (used to populate a
        // new workspace's CWD) reads stale data as a result. Wrap the
        // PowerShell launch with a -Command bootstrap that captures
        // whatever `prompt` the user's auto-loaded $PROFILE defined, then
        // installs a wrapper prompt that:
        //   - syncs [Environment]::CurrentDirectory = $PWD.Path on every
        //     prompt (this calls SetCurrentDirectoryW under the hood, so
        //     the PEB is now accurate);
        //   - delegates to the user's original `prompt` so their visual
        //     customization survives.
        // -Command runs AFTER profile auto-load (we don't pass -NoProfile),
        // so $orig captures whatever the user defined. Errors are swallowed
        // so a broken environment doesn't break the shell. No double quotes
        // inside the script — single quotes only — so the C-level argument
        // quoting stays simple.
        constexpr std::wstring_view kPowerShellAutoIntegrationArgs =
            LR"-( -NoExit -Command "& { $orig = (Get-Item function:prompt -EA SilentlyContinue).ScriptBlock; function global:prompt { try { [Environment]::CurrentDirectory = $PWD.Path } catch {}; if ($orig) { & $orig } else { 'PS ' + $PWD.Path + '> ' } } }")-";

        bool _isPowerShellExeBasename(std::wstring_view path) noexcept
        {
            if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
            {
                path.remove_prefix(1);
                path.remove_suffix(1);
            }
            const auto slash = path.find_last_of(LR"(\/)");
            const auto base = (slash == std::wstring_view::npos) ? path : path.substr(slash + 1);
            const std::wstring lower{ base };
            return _wcsicmp(lower.c_str(), L"powershell.exe") == 0 ||
                   _wcsicmp(lower.c_str(), L"pwsh.exe") == 0;
        }

        // Decide whether `commandline` is "just powershell with no entry-
        // point override." If so, append our shell-integration bootstrap.
        // Otherwise return unchanged — user-provided -Command, -File, or
        // -EncodedCommand args are alternative entry points and our wrap
        // would conflict with them.
        winrt::hstring _wrapPowerShellForShellIntegration(const winrt::hstring& commandline)
        {
            int argc = 0;
            wil::unique_hlocal_ptr<LPWSTR[]> argv{ CommandLineToArgvW(commandline.c_str(), &argc) };
            if (!argv || argc == 0)
            {
                return commandline;
            }
            if (!_isPowerShellExeBasename(argv[0]))
            {
                return commandline;
            }
            for (int i = 1; i < argc; ++i)
            {
                const std::wstring arg{ argv[i] };
                if (_wcsicmp(arg.c_str(), L"-Command") == 0 ||
                    _wcsicmp(arg.c_str(), L"-c") == 0 ||
                    _wcsicmp(arg.c_str(), L"-File") == 0 ||
                    _wcsicmp(arg.c_str(), L"-f") == 0 ||
                    _wcsicmp(arg.c_str(), L"-EncodedCommand") == 0 ||
                    _wcsicmp(arg.c_str(), L"-e") == 0 ||
                    _wcsicmp(arg.c_str(), L"-ec") == 0)
                {
                    return commandline;
                }
            }

            std::wstring result{ commandline };
            result.append(kPowerShellAutoIntegrationArgs);
            return winrt::hstring{ result };
        }
    }

    TerminalConnection::ITerminalConnection TerminalPage::_CreateConnectionFromSettings(Profile profile,
                                                                                        IControlSettings settings,
                                                                                        const bool inheritCursor)
    {
        static const auto textMeasurement = [&]() -> std::wstring_view {
            switch (_settings.GlobalSettings().TextMeasurement())
            {
            case TextMeasurement::Graphemes:
                return L"graphemes";
            case TextMeasurement::Wcswidth:
                return L"wcswidth";
            case TextMeasurement::Console:
                return L"console";
            default:
                return {};
            }
        }();
        static const auto ambiguousIsWide = [&]() -> bool {
            return _settings.GlobalSettings().AmbiguousWidth() == AmbiguousWidth::Wide;
        }();

        TerminalConnection::ITerminalConnection connection{ nullptr };

        auto connectionType = profile.ConnectionType();
        Windows::Foundation::Collections::ValueSet valueSet;

        if (connectionType == TerminalConnection::AzureConnection::ConnectionType() &&
            TerminalConnection::AzureConnection::IsAzureConnectionAvailable())
        {
            connection = TerminalConnection::AzureConnection{};
            valueSet = TerminalConnection::ConptyConnection::CreateSettings(winrt::hstring{},
                                                                            L".",
                                                                            L"Azure",
                                                                            false,
                                                                            L"",
                                                                            nullptr,
                                                                            settings.InitialRows(),
                                                                            settings.InitialCols(),
                                                                            winrt::guid(),
                                                                            profile.Guid());
        }

        else
        {
            auto settingsInternal{ winrt::get_self<Settings::TerminalSettings>(settings) };
            const auto environment = settingsInternal->EnvironmentVariables();

            // Update the path to be relative to whatever our CWD is.
            //
            // Refer to the examples in
            // https://en.cppreference.com/w/cpp/filesystem/path/append
            //
            // We need to do this here, to ensure we tell the ConptyConnection
            // the correct starting path. If we're being invoked from another
            // terminal instance (e.g. `wt -w 0 -d .`), then we have switched our
            // CWD to the provided path. We should treat the StartingDirectory
            // as relative to the current CWD.
            //
            // The connection must be informed of the current CWD on
            // construction, because the connection might not spawn the child
            // process until later, on another thread, after we've already
            // restored the CWD to its original value.
            auto newWorkingDirectory{ _evaluatePathForCwd(settings.StartingDirectory()) };
            connection = TerminalConnection::ConptyConnection{};

            auto effectiveCommandline = settings.Commandline();
            if (_settings.GlobalSettings().AutoShellIntegration())
            {
                effectiveCommandline = _wrapPowerShellForShellIntegration(effectiveCommandline);
            }

            valueSet = TerminalConnection::ConptyConnection::CreateSettings(effectiveCommandline,
                                                                            newWorkingDirectory,
                                                                            settings.StartingTitle(),
                                                                            settingsInternal->ReloadEnvironmentVariables(),
                                                                            _WindowProperties.VirtualEnvVars(),
                                                                            environment,
                                                                            settings.InitialRows(),
                                                                            settings.InitialCols(),
                                                                            winrt::guid(),
                                                                            profile.Guid());

            if (inheritCursor)
            {
                valueSet.Insert(L"inheritCursor", Windows::Foundation::PropertyValue::CreateBoolean(true));
            }
        }

        if (!textMeasurement.empty())
        {
            valueSet.Insert(L"textMeasurement", Windows::Foundation::PropertyValue::CreateString(textMeasurement));
        }
        if (ambiguousIsWide)
        {
            valueSet.Insert(L"ambiguousIsWide", Windows::Foundation::PropertyValue::CreateBoolean(true));
        }

        if (const auto id = settings.SessionId(); id != winrt::guid{})
        {
            valueSet.Insert(L"sessionId", Windows::Foundation::PropertyValue::CreateGuid(id));
        }

        connection.Initialize(valueSet);

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "ConnectionCreated",
            TraceLoggingDescription("Event emitted upon the creation of a connection"),
            TraceLoggingGuid(connectionType, "ConnectionTypeGuid", "The type of the connection"),
            TraceLoggingGuid(profile.Guid(), "ProfileGuid", "The profile's GUID"),
            TraceLoggingGuid(connection.SessionId(), "SessionGuid", "The WT_SESSION's GUID"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

        return connection;
    }

    TerminalConnection::ITerminalConnection TerminalPage::_duplicateConnectionForRestart(const TerminalApp::TerminalPaneContent& paneContent)
    {
        if (paneContent == nullptr)
        {
            return nullptr;
        }

        const auto& control{ paneContent.GetTermControl() };
        if (control == nullptr)
        {
            return nullptr;
        }
        const auto& connection = control.Connection();
        auto profile{ paneContent.GetProfile() };

        Settings::TerminalSettingsCreateResult controlSettings{ nullptr };

        if (profile)
        {
            // TODO GH#5047 If we cache the NewTerminalArgs, we no longer need to do this.
            profile = GetClosestProfileForDuplicationOfProfile(profile);
            controlSettings = Settings::TerminalSettings::CreateWithProfile(_settings, profile);

            // Replace the Starting directory with the CWD, if given
            const auto workingDirectory = control.WorkingDirectory();
            if (Utils::IsValidDirectory(workingDirectory.c_str()))
            {
                controlSettings.DefaultSettings()->StartingDirectory(workingDirectory);
            }

            // To facilitate restarting defterm connections: grab the original
            // commandline out of the connection and shove that back into the
            // settings.
            if (const auto& conpty{ connection.try_as<TerminalConnection::ConptyConnection>() })
            {
                controlSettings.DefaultSettings()->Commandline(conpty.Commandline());
            }
        }

        return _CreateConnectionFromSettings(profile, *controlSettings.DefaultSettings(), true);
    }

    // Method Description:
    // - Called when the settings button is clicked. Launches a background
    //   thread to open the settings file in the default JSON editor.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_SettingsButtonOnClick(const IInspectable&,
                                              const RoutedEventArgs&)
    {
        const auto window = CoreWindow::GetForCurrentThread();

        // check alt state
        const auto rAltState{ window.GetKeyState(VirtualKey::RightMenu) };
        const auto lAltState{ window.GetKeyState(VirtualKey::LeftMenu) };
        const auto altPressed{ WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) ||
                               WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down) };

        // check shift state
        const auto shiftState{ window.GetKeyState(VirtualKey::Shift) };
        const auto lShiftState{ window.GetKeyState(VirtualKey::LeftShift) };
        const auto rShiftState{ window.GetKeyState(VirtualKey::RightShift) };
        const auto shiftPressed{ WI_IsFlagSet(shiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(lShiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(rShiftState, CoreVirtualKeyStates::Down) };

        auto target{ SettingsTarget::SettingsUI };
        if (shiftPressed)
        {
            target = SettingsTarget::SettingsFile;
        }
        else if (altPressed)
        {
            target = SettingsTarget::DefaultsFile;
        }

        const auto targetAsString = [&target]() {
            switch (target)
            {
            case SettingsTarget::SettingsFile:
                return "SettingsFile";
            case SettingsTarget::DefaultsFile:
                return "DefaultsFile";
            case SettingsTarget::SettingsUI:
            default:
                return "UI";
            }
        }();

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "NewTabMenuItemClicked",
            TraceLoggingDescription("Event emitted when an item from the new tab menu is invoked"),
            TraceLoggingValue(NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
            TraceLoggingValue("Settings", "ItemType", "The type of item that was clicked in the new tab menu"),
            TraceLoggingValue(targetAsString, "SettingsTarget", "The target settings file or UI"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

        _LaunchSettings(target);
    }

    // Method Description:
    // - Called when the command palette button is clicked. Opens the command palette.
    void TerminalPage::_CommandPaletteButtonOnClick(const IInspectable&,
                                                    const RoutedEventArgs&)
    {
        auto p = LoadCommandPalette();
        p.EnableCommandPaletteMode(CommandPaletteLaunchMode::Action);
        p.Visibility(Visibility::Visible);

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "NewTabMenuItemClicked",
            TraceLoggingDescription("Event emitted when an item from the new tab menu is invoked"),
            TraceLoggingValue(NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
            TraceLoggingValue("CommandPalette", "ItemType", "The type of item that was clicked in the new tab menu"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    // Method Description:
    // - Called when the about button is clicked. See _ShowAboutDialog for more info.
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void TerminalPage::_AboutButtonOnClick(const IInspectable&,
                                           const RoutedEventArgs&)
    {
        _ShowAboutDialog();

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "NewTabMenuItemClicked",
            TraceLoggingDescription("Event emitted when an item from the new tab menu is invoked"),
            TraceLoggingValue(NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
            TraceLoggingValue("About", "ItemType", "The type of item that was clicked in the new tab menu"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    // Method Description:
    // - Called when the users pressed keyBindings while CommandPaletteElement is open.
    // - As of GH#8480, this is also bound to the TabRowControl's KeyUp event.
    //   That should only fire when focus is in the tab row, which is hard to
    //   do. Notably, that's possible:
    //   - When you have enough tabs to make the little scroll arrows appear,
    //     click one, then hit tab
    //   - When Narrator is in Scan mode (which is the a11y bug we're fixing here)
    // - This method is effectively an extract of TermControl::_KeyHandler and TermControl::_TryHandleKeyBinding.
    // Arguments:
    // - e: the KeyRoutedEventArgs containing info about the keystroke.
    // Return Value:
    // - <none>
    void TerminalPage::_KeyDownHandler(const Windows::Foundation::IInspectable& /*sender*/, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e)
    {
        const auto keyStatus = e.KeyStatus();
        const auto vkey = gsl::narrow_cast<WORD>(e.OriginalKey());
        const auto scanCode = gsl::narrow_cast<WORD>(keyStatus.ScanCode);
        const auto modifiers = _GetPressedModifierKeys();

        // GH#11076:
        // For some weird reason we sometimes receive a WM_KEYDOWN
        // message without vkey or scanCode if a user drags a tab.
        // The KeyChord constructor has a debug assertion ensuring that all KeyChord
        // either have a valid vkey/scanCode. This is important, because this prevents
        // accidental insertion of invalid KeyChords into classes like ActionMap.
        if (!vkey && !scanCode)
        {
            return;
        }

        // Alt-Numpad# input will send us a character once the user releases
        // Alt, so we should be ignoring the individual keydowns. The character
        // will be sent through the TSFInputControl. See GH#1401 for more
        // details
        if (modifiers.IsAltPressed() && (vkey >= VK_NUMPAD0 && vkey <= VK_NUMPAD9))
        {
            return;
        }

        // GH#2235: Terminal::Settings hasn't been modified to differentiate
        // between AltGr and Ctrl+Alt yet.
        // -> Don't check for key bindings if this is an AltGr key combination.
        if (modifiers.IsAltGrPressed())
        {
            return;
        }

        const auto actionMap = _settings.ActionMap();
        if (!actionMap)
        {
            return;
        }

        const auto cmd = actionMap.GetActionByKeyChord({
            modifiers.IsCtrlPressed(),
            modifiers.IsAltPressed(),
            modifiers.IsShiftPressed(),
            modifiers.IsWinPressed(),
            vkey,
            scanCode,
        });
        if (!cmd)
        {
            return;
        }

        if (!_actionDispatch->DoAction(cmd.ActionAndArgs()))
        {
            return;
        }

        if (_commandPaletteIs(Visibility::Visible) &&
            cmd.ActionAndArgs().Action() != ShortcutAction::ToggleCommandPalette)
        {
            CommandPaletteElement().Visibility(Visibility::Collapsed);
        }
        if (_suggestionsControlIs(Visibility::Visible) &&
            cmd.ActionAndArgs().Action() != ShortcutAction::ToggleCommandPalette)
        {
            SuggestionsElement().Visibility(Visibility::Collapsed);
        }

        // Let's assume the user has bound the dead key "^" to a sendInput command that sends "b".
        // If the user presses the two keys "^a" it'll produce "bâ", despite us marking the key event as handled.
        // The following is used to manually "consume" such dead keys and clear them from the keyboard state.
        _ClearKeyboardState(vkey, scanCode);
        e.Handled(true);
    }

    bool TerminalPage::OnDirectKeyEvent(const uint32_t vkey, const uint8_t scanCode, const bool down)
    {
        const auto modifiers = _GetPressedModifierKeys();
        if (vkey == VK_SPACE && modifiers.IsAltPressed() && down)
        {
            if (const auto actionMap = _settings.ActionMap())
            {
                if (const auto cmd = actionMap.GetActionByKeyChord({
                        modifiers.IsCtrlPressed(),
                        modifiers.IsAltPressed(),
                        modifiers.IsShiftPressed(),
                        modifiers.IsWinPressed(),
                        gsl::narrow_cast<int32_t>(vkey),
                        scanCode,
                    }))
                {
                    return _actionDispatch->DoAction(cmd.ActionAndArgs());
                }
            }
        }
        return false;
    }

    // Method Description:
    // - Get the modifier keys that are currently pressed. This can be used to
    //   find out which modifiers (ctrl, alt, shift) are pressed in events that
    //   don't necessarily include that state.
    // - This is a copy of TermControl::_GetPressedModifierKeys.
    // Return Value:
    // - The Microsoft::Terminal::Core::ControlKeyStates representing the modifier key states.
    ControlKeyStates TerminalPage::_GetPressedModifierKeys() noexcept
    {
        const auto window = CoreWindow::GetForCurrentThread();
        // DONT USE
        //      != CoreVirtualKeyStates::None
        // OR
        //      == CoreVirtualKeyStates::Down
        // Sometimes with the key down, the state is Down | Locked.
        // Sometimes with the key up, the state is Locked.
        // IsFlagSet(Down) is the only correct solution.

        struct KeyModifier
        {
            VirtualKey vkey;
            ControlKeyStates flags;
        };

        constexpr std::array<KeyModifier, 7> modifiers{ {
            { VirtualKey::RightMenu, ControlKeyStates::RightAltPressed },
            { VirtualKey::LeftMenu, ControlKeyStates::LeftAltPressed },
            { VirtualKey::RightControl, ControlKeyStates::RightCtrlPressed },
            { VirtualKey::LeftControl, ControlKeyStates::LeftCtrlPressed },
            { VirtualKey::Shift, ControlKeyStates::ShiftPressed },
            { VirtualKey::RightWindows, ControlKeyStates::RightWinPressed },
            { VirtualKey::LeftWindows, ControlKeyStates::LeftWinPressed },
        } };

        ControlKeyStates flags;

        for (const auto& mod : modifiers)
        {
            const auto state = window.GetKeyState(mod.vkey);
            const auto isDown = WI_IsFlagSet(state, CoreVirtualKeyStates::Down);

            if (isDown)
            {
                flags |= mod.flags;
            }
        }

        return flags;
    }

    // Method Description:
    // - Discards currently pressed dead keys.
    // - This is a copy of TermControl::_ClearKeyboardState.
    // Arguments:
    // - vkey: The vkey of the key pressed.
    // - scanCode: The scan code of the key pressed.
    void TerminalPage::_ClearKeyboardState(const WORD vkey, const WORD scanCode) noexcept
    {
        std::array<BYTE, 256> keyState;
        if (!GetKeyboardState(keyState.data()))
        {
            return;
        }

        // As described in "Sometimes you *want* to interfere with the keyboard's state buffer":
        //   http://archives.miloush.net/michkap/archive/2006/09/10/748775.html
        // > "The key here is to keep trying to pass stuff to ToUnicode until -1 is not returned."
        std::array<wchar_t, 16> buffer;
        while (ToUnicodeEx(vkey, scanCode, keyState.data(), buffer.data(), gsl::narrow_cast<int>(buffer.size()), 0b1, nullptr) < 0)
        {
        }
    }

    // Method Description:
    // - Configure the AppKeyBindings to use our ShortcutActionDispatch and the updated ActionMap
    //    as the object to handle dispatching ShortcutAction events.
    // Arguments:
    // - bindings: An IActionMapView object to wire up with our event handlers
    void TerminalPage::_HookupKeyBindings(const IActionMapView& actionMap) noexcept
    {
        _bindings->SetDispatch(*_actionDispatch);
        _bindings->SetActionMap(actionMap);
    }

    // Method Description:
    // - Register our event handlers with our ShortcutActionDispatch. The
    //   ShortcutActionDispatch is responsible for raising the appropriate
    //   events for an ActionAndArgs. WE'll handle each possible event in our
    //   own way.
    // Arguments:
    // - <none>
    void TerminalPage::_RegisterActionCallbacks()
    {
        // Hook up the ShortcutActionDispatch object's events to our handlers.
        // They should all be hooked up here, regardless of whether or not
        // there's an actual keychord for them.
#define ON_ALL_ACTIONS(action) HOOKUP_ACTION(action);
        ALL_SHORTCUT_ACTIONS
        INTERNAL_SHORTCUT_ACTIONS
#undef ON_ALL_ACTIONS
    }

    // Method Description:
    // - Get the title of the currently focused terminal control. If this tab is
    //   the focused tab, then also bubble this title to any listeners of our
    //   TitleChanged event.
    // Arguments:
    // - tab: the Tab to update the title for.
    void TerminalPage::_UpdateTitle(const Tab& tab)
    {
        if (tab == _GetFocusedTab())
        {
            TitleChanged.raise(*this, nullptr);
        }
    }

    // Method Description:
    // - Connects event handlers to the TermControl for events that we want to
    //   handle. This includes:
    //    * the Copy and Paste events, for setting and retrieving clipboard data
    //      on the right thread
    // Arguments:
    // - term: The newly created TermControl to connect the events for
    void TerminalPage::_RegisterTerminalEvents(TermControl term)
    {
        term.RaiseNotice({ this, &TerminalPage::_ControlNoticeRaisedHandler });

        term.WriteToClipboard({ get_weak(), &TerminalPage::_copyToClipboard });
        term.PasteFromClipboard({ this, &TerminalPage::_PasteFromClipboardHandler });

        term.OpenHyperlink({ this, &TerminalPage::_OpenHyperlinkHandler });

        // Add an event handler for when the terminal or tab wants to set a
        // progress indicator on the taskbar
        term.SetTaskbarProgress({ get_weak(), &TerminalPage::_SetTaskbarProgressHandler });

        term.ConnectionStateChanged({ get_weak(), &TerminalPage::_ConnectionStateChangedHandler });

        term.PropertyChanged([weakThis = get_weak()](auto& /*sender*/, auto& e) {
            if (auto page{ weakThis.get() })
            {
                if (e.PropertyName() == L"BackgroundBrush")
                {
                    page->_updateThemeColors();
                }
            }
        });

        term.ShowWindowChanged({ get_weak(), &TerminalPage::_ShowWindowChangedHandler });
        term.SearchMissingCommand({ get_weak(), &TerminalPage::_SearchMissingCommandHandler });
        term.WindowSizeChanged({ get_weak(), &TerminalPage::_WindowSizeChanged });

        // Don't even register for the event if the feature is compiled off.
        if constexpr (Feature_ShellCompletions::IsEnabled())
        {
            term.CompletionsChanged({ get_weak(), &TerminalPage::_ControlCompletionsChangedHandler });
        }
        winrt::weak_ref<TermControl> weakTerm{ term };
        term.ContextMenu().Opening([weak = get_weak(), weakTerm](auto&& sender, auto&& /*args*/) {
            if (const auto& page{ weak.get() })
            {
                page->_PopulateContextMenu(weakTerm.get(), sender.try_as<MUX::Controls::CommandBarFlyout>(), false);
            }
        });
        term.SelectionContextMenu().Opening([weak = get_weak(), weakTerm](auto&& sender, auto&& /*args*/) {
            if (const auto& page{ weak.get() })
            {
                page->_PopulateContextMenu(weakTerm.get(), sender.try_as<MUX::Controls::CommandBarFlyout>(), true);
            }
        });
        if constexpr (Feature_QuickFix::IsEnabled())
        {
            term.QuickFixMenu().Opening([weak = get_weak(), weakTerm](auto&& sender, auto&& /*args*/) {
                if (const auto& page{ weak.get() })
                {
                    page->_PopulateQuickFixMenu(weakTerm.get(), sender.try_as<Controls::MenuFlyout>());
                }
            });
        }
    }

    // Method Description:
    // - Connects event handlers to the Tab for events that we want to
    //   handle. This includes:
    //    * the TitleChanged event, for changing the text of the tab
    //    * the Color{Selected,Cleared} events to change the color of a tab.
    // Arguments:
    // - hostingTab: The Tab that's hosting this TermControl instance
    void TerminalPage::_RegisterTabEvents(Tab& hostingTab)
    {
        auto weakTab{ hostingTab.get_weak() };
        auto weakThis{ get_weak() };
        // PropertyChanged is the generic mechanism by which the Tab
        // communicates changes to any of its observable properties, including
        // the Title
        hostingTab.PropertyChanged([weakTab, weakThis](auto&&, const WUX::Data::PropertyChangedEventArgs& args) {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };
            if (page && tab)
            {
                const auto propertyName = args.PropertyName();
                if (propertyName == L"Title")
                {
                    page->_UpdateTitle(*tab);
                }
                else if (propertyName == L"Content")
                {
                    if (*tab == page->_GetFocusedTab())
                    {
                        const auto children = page->_tabContent.Children();

                        children.Clear();
                        if (auto content = tab->Content())
                        {
                            page->_tabContent.Children().Append(std::move(content));
                        }

                        tab->Focus(FocusState::Programmatic);
                    }
                }
            }
        });

        // Add an event handler for when the terminal or tab wants to set a
        // progress indicator on the taskbar
        hostingTab.TaskbarProgressChanged({ get_weak(), &TerminalPage::_SetTaskbarProgressHandler });

        hostingTab.RestartTerminalRequested({ get_weak(), &TerminalPage::_restartPaneConnection });
    }

    // Method Description:
    // - Helper to manually exit "zoom" when certain actions take place.
    //   Anything that modifies the state of the pane tree should probably
    //   un-zoom the focused pane first, so that the user can see the full pane
    //   tree again. These actions include:
    //   * Splitting a new pane
    //   * Closing a pane
    //   * Moving focus between panes
    //   * Resizing a pane
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_UnZoomIfNeeded()
    {
        if (const auto activeTab{ _GetFocusedTabImpl() })
        {
            if (activeTab->IsZoomed())
            {
                // Remove the content from the tab first, so Pane::UnZoom can
                // re-attach the content to the tree w/in the pane
                _tabContent.Children().Clear();
                // In ExitZoom, we'll change the Tab's Content(), triggering the
                // content changed event, which will re-attach the tab's new content
                // root to the tree.
                activeTab->ExitZoom();
            }
        }
    }

    // Method Description:
    // - Attempt to move focus between panes, as to focus the child on
    //   the other side of the separator. See Pane::NavigateFocus for details.
    // - Moves the focus of the currently focused tab.
    // Arguments:
    // - direction: The direction to move the focus in.
    // Return Value:
    // - Whether changing the focus succeeded. This allows a keychord to propagate
    //   to the terminal when no other panes are present (GH#6219)
    bool TerminalPage::_MoveFocus(const FocusDirection& direction)
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            return tabImpl->NavigateFocus(direction);
        }
        return false;
    }

    // Method Description:
    // - Attempt to swap the positions of the focused pane with another pane.
    //   See Pane::SwapPane for details.
    // Arguments:
    // - direction: The direction to move the focused pane in.
    // Return Value:
    // - true if panes were swapped.
    bool TerminalPage::_SwapPane(const FocusDirection& direction)
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();
            return tabImpl->SwapPane(direction);
        }
        return false;
    }

    TermControl TerminalPage::_GetActiveControl() const
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            return tabImpl->GetActiveTerminalControl();
        }
        return nullptr;
    }

    CommandPalette TerminalPage::LoadCommandPalette()
    {
        if (const auto p = CommandPaletteElement())
        {
            return p;
        }

        return _loadCommandPaletteSlowPath();
    }
    bool TerminalPage::_commandPaletteIs(WUX::Visibility visibility)
    {
        const auto p = CommandPaletteElement();
        return p && p.Visibility() == visibility;
    }

    CommandPalette TerminalPage::_loadCommandPaletteSlowPath()
    {
        const auto p = FindName(L"CommandPaletteElement").as<CommandPalette>();

        p.SetActionMap(_settings.ActionMap());

        // When the visibility of the command palette changes to "collapsed",
        // the palette has been closed. Toss focus back to the currently active control.
        p.RegisterPropertyChangedCallback(UIElement::VisibilityProperty(), [this](auto&&, auto&&) {
            if (_commandPaletteIs(Visibility::Collapsed))
            {
                _FocusActiveControl(nullptr, nullptr);
            }
        });
        p.DispatchCommandRequested({ this, &TerminalPage::_OnDispatchCommandRequested });
        p.CommandLineExecutionRequested({ this, &TerminalPage::_OnCommandLineExecutionRequested });
        p.SwitchToTabRequested({ this, &TerminalPage::_OnSwitchToTabRequested });
        p.PreviewAction({ this, &TerminalPage::_PreviewActionHandler });

        return p;
    }

    SuggestionsControl TerminalPage::LoadSuggestionsUI()
    {
        if (const auto p = SuggestionsElement())
        {
            return p;
        }

        return _loadSuggestionsElementSlowPath();
    }
    bool TerminalPage::_suggestionsControlIs(WUX::Visibility visibility)
    {
        const auto p = SuggestionsElement();
        return p && p.Visibility() == visibility;
    }

    SuggestionsControl TerminalPage::_loadSuggestionsElementSlowPath()
    {
        const auto p = FindName(L"SuggestionsElement").as<SuggestionsControl>();

        p.RegisterPropertyChangedCallback(UIElement::VisibilityProperty(), [this](auto&&, auto&&) {
            if (SuggestionsElement().Visibility() == Visibility::Collapsed)
            {
                _FocusActiveControl(nullptr, nullptr);
            }
        });
        p.DispatchCommandRequested({ this, &TerminalPage::_OnDispatchCommandRequested });
        p.PreviewAction({ this, &TerminalPage::_PreviewActionHandler });

        return p;
    }

    // Method Description:
    // - Warn the user that they are about to close all open windows, then
    //   signal that we want to close everything.
    safe_void_coroutine TerminalPage::RequestQuit()
    {
        if (!_displayingCloseDialog)
        {
            _displayingCloseDialog = true;

            const auto weak = get_weak();
            auto warningResult = co_await _ShowQuitDialog();
            const auto strong = weak.get();
            if (!strong)
            {
                co_return;
            }

            _displayingCloseDialog = false;

            if (warningResult != ContentDialogResult::Primary)
            {
                co_return;
            }

            QuitRequested.raise(nullptr, nullptr);
        }
    }

    void TerminalPage::PersistState()
    {
        // This method may be called for a window even if it hasn't had a tab yet or lost all of them.
        // We shouldn't persist such windows.
        const auto tabCount = _tabs.Size();
        if (_startupState != StartupState::Initialized || tabCount == 0)
        {
            return;
        }

        std::vector<ActionAndArgs> actions;

        for (auto tab : _tabs)
        {
            auto t = winrt::get_self<implementation::Tab>(tab);
            auto tabActions = t->BuildStartupActions(BuildStartupKind::Persist);
            actions.insert(actions.end(), std::make_move_iterator(tabActions.begin()), std::make_move_iterator(tabActions.end()));
        }

        // Avoid persisting a window with zero tabs, because `BuildStartupActions` happened to return an empty vector.
        if (actions.empty())
        {
            return;
        }

        // if the focused tab was not the last tab, restore that
        auto idx = _GetFocusedTabIndex();
        if (idx && idx != tabCount - 1)
        {
            ActionAndArgs action;
            action.Action(ShortcutAction::SwitchToTab);
            SwitchToTabArgs switchToTabArgs{ idx.value() };
            action.Args(switchToTabArgs);

            actions.emplace_back(std::move(action));
        }

        // If the user set a custom name, save it
        if (const auto& windowName{ _WindowProperties.WindowName() }; !windowName.empty())
        {
            ActionAndArgs action;
            action.Action(ShortcutAction::RenameWindow);
            RenameWindowArgs args{ windowName };
            action.Args(args);

            actions.emplace_back(std::move(action));
        }

        WindowLayout layout;
        layout.TabLayout(winrt::single_threaded_vector<ActionAndArgs>(std::move(actions)));

        // Workspaces mode: serialize `workspaceLayout` directly from
        // `_workspaces` in sidebar order. The earlier
        // `MigrateLegacyTabLayoutToWorkspaceLayout()` bridging path derived
        // workspaceLayout from `_tabs`, which is in *materialization*
        // order (workspace tabs are appended as the user clicks each row),
        // not sidebar order. That made the sidebar shuffle on restart
        // because materialization order rarely matches the user's chosen
        // sidebar arrangement. Iterating `_workspaces` instead keeps the
        // persisted order canonical.
        //
        // For materialized workspaces, capture session edits (splits made
        // after restore) by calling `Tab::BuildStartupActions(Persist)` on
        // the live tab. For dormant workspaces (never clicked, so
        // `_workspaceTabs` has no entry), emit `ws.paneTree` verbatim —
        // those actions came from the previous session's persisted state
        // and round-trip exactly.
        if (_settings.GlobalSettings().WorkspacesEnabled() && _workspaces && _workspaces->Count() > 0)
        {
            // First, flatten every materialized workspace's live actions
            // into one IVector and round-trip through the projected
            // `ActionAndArgs::Serialize` once, then parse back. This is
            // the only path that converts the impl-namespace
            // `ActionAndArgs::ToJson` output into a usable `Json::Value` —
            // calling it directly from TerminalApp would leave us with
            // unresolved-external link errors (same constraint
            // `WorkspaceList::DeserializeWorkspaceLayoutBlob` is built
            // around). Slice boundaries below remember where each
            // workspace's chunk ends in the merged JSON array.
            std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> mergedLive;
            std::vector<size_t> sliceEnds;
            sliceEnds.reserve(_workspaces->Count());
            constexpr size_t kDormantMarker = std::numeric_limits<size_t>::max();

            for (size_t i = 0; i < _workspaces->Count(); ++i)
            {
                const auto& ws = _workspaces->At(i);
                const auto it = _workspaceTabs.find(ws.id);
                if (it != _workspaceTabs.end() && it->second)
                {
                    auto tabImpl = winrt::get_self<implementation::Tab>(it->second);
                    auto live = tabImpl->BuildStartupActions(BuildStartupKind::Persist);
                    mergedLive.insert(mergedLive.end(),
                                      std::make_move_iterator(live.begin()),
                                      std::make_move_iterator(live.end()));
                    sliceEnds.push_back(mergedLive.size());
                }
                else
                {
                    sliceEnds.push_back(kDormantMarker);
                }
            }

            Json::Value liveActionsJson{ Json::arrayValue };
            if (!mergedLive.empty())
            {
                auto liveVec = winrt::single_threaded_vector<Microsoft::Terminal::Settings::Model::ActionAndArgs>(std::move(mergedLive));
                const auto serialized = Microsoft::Terminal::Settings::Model::ActionAndArgs::Serialize(liveVec);
                const auto serializedUtf8 = winrt::to_string(serialized);
                Json::CharReaderBuilder rb;
                std::string errs;
                Json::Value parsedRoot;
                std::unique_ptr<Json::CharReader> reader{ rb.newCharReader() };
                if (reader->parse(serializedUtf8.data(), serializedUtf8.data() + serializedUtf8.size(), &parsedRoot, &errs)
                    && parsedRoot.isObject())
                {
                    if (auto& arr = parsedRoot["actions"]; arr.isArray())
                    {
                        liveActionsJson = std::move(arr);
                    }
                }
            }

            Json::Value wsArray{ Json::arrayValue };
            size_t liveSliceStart = 0;
            for (size_t i = 0; i < _workspaces->Count(); ++i)
            {
                const auto& ws = _workspaces->At(i);

                Json::Value header{ Json::objectValue };
                header["action"] = "newWorkspace";
                if (!ws.title.empty())
                {
                    header["title"] = winrt::to_string(winrt::hstring{ ws.title });
                }
                if (ws.runtimeColor.has_value())
                {
                    const auto& c = *ws.runtimeColor;
                    header["color"] = fmt::format("#{:02X}{:02X}{:02X}", c.R, c.G, c.B);
                }
                if (!ws.customDescription.empty())
                {
                    header["description"] = winrt::to_string(winrt::hstring{ ws.customDescription });
                }
                if (ws.pinned)
                {
                    header["pinned"] = true;
                }
                wsArray.append(std::move(header));

                if (sliceEnds[i] != kDormantMarker)
                {
                    for (size_t j = liveSliceStart;
                         j < sliceEnds[i] && j < liveActionsJson.size();
                         ++j)
                    {
                        wsArray.append(liveActionsJson[static_cast<Json::ArrayIndex>(j)]);
                    }
                    liveSliceStart = sliceEnds[i];
                }
                else if (ws.paneTree.isArray())
                {
                    for (const auto& step : ws.paneTree)
                    {
                        wsArray.append(step);
                    }
                }
            }

            if (const auto active = _workspaces->ActiveIndex(); active.has_value())
            {
                Json::Value selectMarker{ Json::objectValue };
                selectMarker["action"] = "selectWorkspace";
                selectMarker["index"] = static_cast<Json::UInt>(*active);
                wsArray.append(std::move(selectMarker));
            }

            Json::StreamWriterBuilder wb;
            const auto blob = Json::writeString(wb, wsArray);
            layout.WorkspaceLayout(winrt::hstring{ til::u8u16(blob) });

            const auto sidebarWidth = WorkspaceSidebarColumn().Width().Value;
            layout.SidebarWidth(winrt::Windows::Foundation::IReference<double>{ sidebarWidth });
        }

        auto mode = LaunchMode::DefaultMode;
        WI_SetFlagIf(mode, LaunchMode::FullscreenMode, _isFullscreen);
        WI_SetFlagIf(mode, LaunchMode::FocusMode, _isInFocusMode);
        WI_SetFlagIf(mode, LaunchMode::MaximizedMode, _isMaximized);

        layout.LaunchMode({ mode });

        // Only save the content size because the tab size will be added on load.
        // Use WorkspaceContentRoot rather than TabContent — TabContent is the
        // right-hand column inside the sidebar wrapper and excludes the
        // sidebar+splitter columns, which would shrink the persisted size by
        // ~sidebar width every save.
        //
        // Clamp to sensible minimums so a transient layout state during
        // shutdown can't persist a tiny size that becomes unrecoverable on
        // next launch (the sidebar would consume all space and grab pointer
        // input intended for the window edge).
        constexpr float kMinPersistedWidth = 400.0f;
        constexpr float kMinPersistedHeight = 200.0f;
        const auto rawWidth = static_cast<float>(WorkspaceContentRoot().ActualWidth());
        const auto rawHeight = static_cast<float>(WorkspaceContentRoot().ActualHeight());
        const winrt::Windows::Foundation::Size windowSize{
            std::max(rawWidth, kMinPersistedWidth),
            std::max(rawHeight, kMinPersistedHeight)
        };

        layout.InitialSize(windowSize);

        // We don't actually know our own position. So we have to ask the window
        // layer for that.
        const auto launchPosRequest{ winrt::make<LaunchPositionRequest>() };
        RequestLaunchPosition.raise(*this, launchPosRequest);
        layout.InitialPosition(launchPosRequest.Position());

        ApplicationState::SharedInstance().AppendPersistedWindowLayout(layout);
    }

    // Method Description:
    // - Close the terminal app. If there is more
    //   than one tab opened, show a warning dialog.
    safe_void_coroutine TerminalPage::CloseWindow()
    {
        if (_ShouldConfirmCloseWindow() &&
            _settings.GlobalSettings().ConfirmCloseAllTabs() &&
            !_displayingCloseDialog)
        {
            if (_newTabButton && _newTabButton.Flyout())
            {
                _newTabButton.Flyout().Hide();
            }
            _DismissTabContextMenus();
            _displayingCloseDialog = true;
            auto warningResult = co_await _ShowCloseWarningDialog();
            _displayingCloseDialog = false;

            if (warningResult != ContentDialogResult::Primary)
            {
                co_return;
            }
        }

        CloseWindowRequested.raise(*this, nullptr);
    }

    std::vector<IPaneContent> TerminalPage::Panes() const
    {
        std::vector<IPaneContent> panes;

        for (const auto tab : _tabs)
        {
            const auto impl = _GetTabImpl(tab);
            if (!impl)
            {
                continue;
            }

            impl->GetRootPane()->WalkTree([&](auto&& pane) {
                if (auto content = pane->GetContent())
                {
                    panes.push_back(std::move(content));
                }
            });
        }

        return panes;
    }

    // Method Description:
    // - Move the viewport of the terminal of the currently focused tab up or
    //      down a number of lines.
    // Arguments:
    // - scrollDirection: ScrollUp will move the viewport up, ScrollDown will move the viewport down
    // - rowsToScroll: a number of lines to move the viewport. If not provided we will use a system default.
    void TerminalPage::_Scroll(ScrollDirection scrollDirection, const Windows::Foundation::IReference<uint32_t>& rowsToScroll)
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            uint32_t realRowsToScroll;
            if (rowsToScroll == nullptr)
            {
                // The magic value of WHEEL_PAGESCROLL indicates that we need to scroll the entire page
                realRowsToScroll = _systemRowsToScroll == WHEEL_PAGESCROLL ?
                                       tabImpl->GetActiveTerminalControl().ViewHeight() :
                                       _systemRowsToScroll;
            }
            else
            {
                // use the custom value specified in the command
                realRowsToScroll = rowsToScroll.Value();
            }
            auto scrollDelta = _ComputeScrollDelta(scrollDirection, realRowsToScroll);
            tabImpl->Scroll(scrollDelta);
        }
    }

    // Method Description:
    // - Moves the currently active pane on the currently active tab to the
    //   specified tab. If the tab index is greater than the number of
    //   tabs, then a new tab will be created for the pane. Similarly, if a pane
    //   is the last remaining pane on a tab, that tab will be closed upon moving.
    // - No move will occur if the tabIdx is the same as the current tab, or if
    //   the specified tab is not a host of terminals (such as the settings tab).
    // - If the Window is specified, the pane will instead be detached and moved
    //   to the window with the given name/id.
    // Return Value:
    // - true if the pane was successfully moved to the new tab.
    bool TerminalPage::_MovePane(MovePaneArgs args)
    {
        const auto tabIdx{ args.TabIndex() };
        const auto windowId{ args.Window() };

        auto focusedTab{ _GetFocusedTabImpl() };

        if (!focusedTab)
        {
            return false;
        }

        // If there was a windowId in the action, try to move it to the
        // specified window instead of moving it in our tab row.
        if (!windowId.empty())
        {
            if (const auto tabImpl{ _GetFocusedTabImpl() })
            {
                if (const auto pane{ tabImpl->GetActivePane() })
                {
                    auto startupActions = pane->BuildStartupActions(0, 1, BuildStartupKind::MovePane);
                    _DetachPaneFromWindow(pane);
                    _MoveContent(std::move(startupActions.args), windowId, tabIdx);
                    focusedTab->DetachPane();

                    if (auto autoPeer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(*this))
                    {
                        if (windowId == L"new")
                        {
                            autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                            Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                            RS_(L"TerminalPage_PaneMovedAnnouncement_NewWindow"),
                                                            L"TerminalPageMovePaneToNewWindow" /* unique name for this notification category */);
                        }
                        else
                        {
                            autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                            Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                            RS_fmt(L"TerminalPage_PaneMovedAnnouncement_ExistingWindow2", windowId),
                                                            L"TerminalPageMovePaneToExistingWindow" /* unique name for this notification category */);
                        }
                    }
                    return true;
                }
            }
        }

        // If we are trying to move from the current tab to the current tab do nothing.
        if (_GetFocusedTabIndex() == tabIdx)
        {
            return false;
        }

        // Moving the pane from the current tab might close it, so get the next
        // tab before its index changes.
        if (tabIdx < _tabs.Size())
        {
            auto targetTab = _GetTabImpl(_tabs.GetAt(tabIdx));
            // if the selected tab is not a host of terminals (e.g. settings)
            // don't attempt to add a pane to it.
            if (!targetTab)
            {
                return false;
            }
            auto pane = focusedTab->DetachPane();
            targetTab->AttachPane(pane);
            _SetFocusedTab(*targetTab);

            if (auto autoPeer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(*this))
            {
                const auto tabTitle = targetTab->Title();
                autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                RS_fmt(L"TerminalPage_PaneMovedAnnouncement_ExistingTab", tabTitle),
                                                L"TerminalPageMovePaneToExistingTab" /* unique name for this notification category */);
            }
        }
        else
        {
            auto pane = focusedTab->DetachPane();
            _CreateNewTabFromPane(pane);
            if (auto autoPeer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(*this))
            {
                autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                RS_(L"TerminalPage_PaneMovedAnnouncement_NewTab"),
                                                L"TerminalPageMovePaneToNewTab" /* unique name for this notification category */);
            }
        }

        return true;
    }

    // Detach a tree of panes from this terminal. Helper used for moving panes
    // and tabs to other windows.
    void TerminalPage::_DetachPaneFromWindow(std::shared_ptr<Pane> pane)
    {
        pane->WalkTree([&](auto p) {
            if (const auto& control{ p->GetTerminalControl() })
            {
                _manager.Detach(control);
            }
        });
    }

    void TerminalPage::_DetachTabFromWindow(const winrt::com_ptr<Tab>& tab)
    {
        // Detach the root pane, which will act like the whole tab got detached.
        if (const auto rootPane = tab->GetRootPane())
        {
            _DetachPaneFromWindow(rootPane);
        }
    }

    // Method Description:
    // - Serialize these actions to json, and raise them as a RequestMoveContent
    //   event. Our Window will raise that to the window manager / monarch, who
    //   will dispatch this blob of json back to the window that should handle
    //   this.
    // - `actions` will be emptied into a winrt IVector as a part of this method
    //   and should be expected to be empty after this call.
    void TerminalPage::_MoveContent(std::vector<Settings::Model::ActionAndArgs>&& actions,
                                    const winrt::hstring& windowName,
                                    const uint32_t tabIndex,
                                    const std::optional<winrt::Windows::Foundation::Point>& dragPoint)
    {
        const auto winRtActions{ winrt::single_threaded_vector<ActionAndArgs>(std::move(actions)) };
        const auto str{ ActionAndArgs::Serialize(winRtActions) };
        const auto request = winrt::make_self<RequestMoveContentArgs>(windowName,
                                                                      str,
                                                                      tabIndex);
        if (dragPoint.has_value())
        {
            request->WindowPosition(*dragPoint);
        }
        RequestMoveContent.raise(*this, *request);
    }

    bool TerminalPage::_MoveTab(winrt::com_ptr<Tab> tab, MoveTabArgs args)
    {
        if (!tab)
        {
            return false;
        }

        // If there was a windowId in the action, try to move it to the
        // specified window instead of moving it in our tab row.
        const auto windowId{ args.Window() };
        if (!windowId.empty())
        {
            // if the windowId is the same as our name, do nothing
            if (windowId == WindowProperties().WindowName() ||
                windowId == winrt::to_hstring(WindowProperties().WindowId()))
            {
                return true;
            }

            if (tab)
            {
                auto startupActions = tab->BuildStartupActions(BuildStartupKind::Content);
                _DetachTabFromWindow(tab);
                _MoveContent(std::move(startupActions), windowId, 0);
                _RemoveTab(*tab);
                if (auto autoPeer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(*this))
                {
                    const auto tabTitle = tab->Title();
                    if (windowId == L"new")
                    {
                        autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                        Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                        RS_fmt(L"TerminalPage_TabMovedAnnouncement_NewWindow", tabTitle),
                                                        L"TerminalPageMoveTabToNewWindow" /* unique name for this notification category */);
                    }
                    else
                    {
                        autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                        Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                        RS_fmt(L"TerminalPage_TabMovedAnnouncement_Default", tabTitle, windowId),
                                                        L"TerminalPageMoveTabToExistingWindow" /* unique name for this notification category */);
                    }
                }
                return true;
            }
        }

        const auto direction = args.Direction();
        if (direction != MoveTabDirection::None)
        {
            // Use the requested tab, if provided. Otherwise, use the currently
            // focused tab.
            const auto tabIndex = til::coalesce(_GetTabIndex(*tab),
                                                _GetFocusedTabIndex());
            if (tabIndex)
            {
                const auto currentTabIndex = tabIndex.value();
                const auto delta = direction == MoveTabDirection::Forward ? 1 : -1;
                _TryMoveTab(currentTabIndex, currentTabIndex + delta);
            }
        }

        return true;
    }

    // When the tab's active pane changes, we'll want to lookup a new icon
    // for it. The Title change will be propagated upwards through the tab's
    // PropertyChanged event handler.
    void TerminalPage::_activePaneChanged(winrt::TerminalApp::Tab sender,
                                          Windows::Foundation::IInspectable /*args*/)
    {
        if (const auto tab{ _GetTabImpl(sender) })
        {
            // Possibly update the icon of the tab.
            _UpdateTabIcon(*tab);

            _updateThemeColors();

            // Update the taskbar progress as well. We'll raise our own
            // SetTaskbarProgress event here, to get tell the hosting
            // application to re-query this value from us.
            SetTaskbarProgress.raise(*this, nullptr);

            auto profile = tab->GetFocusedProfile();
            _UpdateBackground(profile);
        }

        _adjustProcessPriorityThrottled->Run();
    }

    uint32_t TerminalPage::NumberOfTabs() const
    {
        return _tabs.Size();
    }

    // Method Description:
    // - Called when it is determined that an existing tab or pane should be
    //   attached to our window. content represents a blob of JSON describing
    //   some startup actions for rebuilding the specified panes. They will
    //   include `__content` properties with the GUID of the existing
    //   ControlInteractivity's we should use, rather than starting new ones.
    // - _MakePane is already enlightened to use the ContentId property to
    //   reattach instead of create new content, so this method simply needs to
    //   parse the JSON and pump it into our action handler. Almost the same as
    //   doing something like `wt -w 0 nt`.
    void TerminalPage::AttachContent(IVector<Settings::Model::ActionAndArgs> args, uint32_t tabIndex)
    {
        if (args == nullptr ||
            args.Size() == 0)
        {
            return;
        }

        const auto& firstAction = args.GetAt(0);
        const bool firstIsSplitPane{ firstAction.Action() == ShortcutAction::SplitPane };

        // `splitPane` allows the user to specify which tab to split. In that
        // case, split specifically the requested pane.
        //
        // If there's not enough tabs, then just turn this pane into a new tab.
        //
        // If the first action is `newTab`, the index is always going to be 0,
        // so don't do anything in that case.
        if (firstIsSplitPane && tabIndex < _tabs.Size())
        {
            _SelectTab(tabIndex);
        }

        for (const auto& action : args)
        {
            _actionDispatch->DoAction(action);
        }

        // After handling all the actions, then re-check the tabIndex. We might
        // have been called as a part of a tab drag/drop. In that case, the
        // tabIndex is actually relevant, and we need to move the tab we just
        // made into position.
        if (!firstIsSplitPane && tabIndex != -1)
        {
            // Move the currently active tab to the requested index Use the
            // currently focused tab index, because we don't know if the new tab
            // opened at the end of the list, or adjacent to the previously
            // active tab. This is affected by the user's "newTabPosition"
            // setting.
            if (const auto focusedTabIndex = _GetFocusedTabIndex())
            {
                const auto source = *focusedTabIndex;
                _TryMoveTab(source, tabIndex);
            }
            // else: This shouldn't really be possible, because the tab we _just_ opened should be active.
        }
    }

    // Method Description:
    // - Split the focused pane of the given tab, either horizontally or vertically, and place the
    //   given pane accordingly
    // Arguments:
    // - tab: The tab that is going to be split.
    // - newPane: the pane to add to our tree of panes
    // - splitDirection: one value from the TerminalApp::SplitDirection enum, indicating how the
    //   new pane should be split from its parent.
    // - splitSize: the size of the split
    void TerminalPage::_SplitPane(const winrt::com_ptr<Tab>& tab,
                                  const SplitDirection splitDirection,
                                  const float splitSize,
                                  std::shared_ptr<Pane> newPane)
    {
        auto activeTab = tab;
        // Clever hack for a crash in startup, with multiple sub-commands. Say
        // you have the following commandline:
        //
        //   wtd nt -p "elevated cmd" ; sp -p "elevated cmd" ; sp -p "Command Prompt"
        //
        // Where "elevated cmd" is an elevated profile.
        //
        // In that scenario, we won't dump off the commandline immediately to an
        // elevated window, because it's got the final unelevated split in it.
        // However, when we get to that command, there won't be a tab yet. So
        // we'd crash right about here.
        //
        // Instead, let's just promote this first split to be a tab instead.
        // Crash avoided, and we don't need to worry about inserting a new-tab
        // command in at the start.
        if (!tab)
        {
            if (_tabs.Size() == 0)
            {
                _CreateNewTabFromPane(newPane);
                return;
            }
            else
            {
                activeTab = _GetFocusedTabImpl();
            }
        }

        // For now, prevent splitting the _settingsTab. We can always revisit this later.
        if (*activeTab == _settingsTab)
        {
            return;
        }

        // If the caller is calling us with the return value of _MakePane
        // directly, it's possible that nullptr was returned, if the connections
        // was supposed to be launched in an elevated window. In that case, do
        // nothing here. We don't have a pane with which to create the split.
        if (!newPane)
        {
            return;
        }
        const auto contentWidth = static_cast<float>(_tabContent.ActualWidth());
        const auto contentHeight = static_cast<float>(_tabContent.ActualHeight());
        const winrt::Windows::Foundation::Size availableSpace{ contentWidth, contentHeight };

        const auto realSplitType = activeTab->PreCalculateCanSplit(splitDirection, splitSize, availableSpace);
        if (!realSplitType)
        {
            return;
        }

        _UnZoomIfNeeded();
        auto [original, newGuy] = activeTab->SplitPane(*realSplitType, splitSize, newPane);

        // After GH#6586, the control will no longer focus itself
        // automatically when it's finished being laid out. Manually focus
        // the control here instead.
        if (_startupState == StartupState::Initialized)
        {
            if (const auto& content{ newGuy->GetContent() })
            {
                content.Focus(FocusState::Programmatic);
            }
        }
    }

    // Method Description:
    // - Switches the split orientation of the currently focused pane.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_ToggleSplitOrientation()
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();
            tabImpl->ToggleSplitOrientation();
        }
    }

    // Method Description:
    // - Attempt to move a separator between panes, as to resize each child on
    //   either size of the separator. See Pane::ResizePane for details.
    // - Moves a separator on the currently focused tab.
    // Arguments:
    // - direction: The direction to move the separator in.
    // Return Value:
    // - <none>
    void TerminalPage::_ResizePane(const ResizeDirection& direction)
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();
            tabImpl->ResizePane(direction);
        }
    }

    // Method Description:
    // - Move the viewport of the terminal of the currently focused tab up or
    //      down a page. The page length will be dependent on the terminal view height.
    // Arguments:
    // - scrollDirection: ScrollUp will move the viewport up, ScrollDown will move the viewport down
    void TerminalPage::_ScrollPage(ScrollDirection scrollDirection)
    {
        // Do nothing if for some reason, there's no terminal tab in focus. We don't want to crash.
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            if (const auto& control{ _GetActiveControl() })
            {
                const auto termHeight = control.ViewHeight();
                auto scrollDelta = _ComputeScrollDelta(scrollDirection, termHeight);
                tabImpl->Scroll(scrollDelta);
            }
        }
    }

    void TerminalPage::_ScrollToBufferEdge(ScrollDirection scrollDirection)
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            auto scrollDelta = _ComputeScrollDelta(scrollDirection, INT_MAX);
            tabImpl->Scroll(scrollDelta);
        }
    }

    // Method Description:
    // - Gets the title of the currently focused terminal control. If there
    //   isn't a control selected for any reason, returns "Terminal"
    // Arguments:
    // - <none>
    // Return Value:
    // - the title of the focused control if there is one, else "Terminal"
    hstring TerminalPage::Title()
    {
        if (_settings.GlobalSettings().ShowTitleInTitlebar())
        {
            if (const auto tab{ _GetFocusedTab() })
            {
                return tab.Title();
            }
        }
        return { L"Terminal" };
    }

    // Method Description:
    // - Handles the special case of providing a text override for the UI shortcut due to VK_OEM issue.
    //      Looks at the flags from the KeyChord modifiers and provides a concatenated string value of all
    //      in the same order that XAML would put them as well.
    // Return Value:
    // - a string representation of the key modifiers for the shortcut
    //NOTE: This needs to be localized with https://github.com/microsoft/terminal/issues/794 if XAML framework issue not resolved before then
    static std::wstring _FormatOverrideShortcutText(VirtualKeyModifiers modifiers)
    {
        std::wstring buffer{ L"" };

        if (WI_IsFlagSet(modifiers, VirtualKeyModifiers::Control))
        {
            buffer += L"Ctrl+";
        }

        if (WI_IsFlagSet(modifiers, VirtualKeyModifiers::Shift))
        {
            buffer += L"Shift+";
        }

        if (WI_IsFlagSet(modifiers, VirtualKeyModifiers::Menu))
        {
            buffer += L"Alt+";
        }

        if (WI_IsFlagSet(modifiers, VirtualKeyModifiers::Windows))
        {
            buffer += L"Win+";
        }

        return buffer;
    }

    // Method Description:
    // - Takes a MenuFlyoutItem and a corresponding KeyChord value and creates the accelerator for UI display.
    //   Takes into account a special case for an error condition for a comma
    // Arguments:
    // - MenuFlyoutItem that will be displayed, and a KeyChord to map an accelerator
    void TerminalPage::_SetAcceleratorForMenuItem(WUX::Controls::MenuFlyoutItem& menuItem,
                                                  const KeyChord& keyChord)
    {
#ifdef DEP_MICROSOFT_UI_XAML_708_FIXED
        // work around https://github.com/microsoft/microsoft-ui-xaml/issues/708 in case of VK_OEM_COMMA
        if (keyChord.Vkey() != VK_OEM_COMMA)
        {
            // use the XAML shortcut to give us the automatic capabilities
            auto menuShortcut = Windows::UI::Xaml::Input::KeyboardAccelerator{};

            // TODO: Modify this when https://github.com/microsoft/terminal/issues/877 is resolved
            menuShortcut.Key(static_cast<Windows::System::VirtualKey>(keyChord.Vkey()));

            // add the modifiers to the shortcut
            menuShortcut.Modifiers(keyChord.Modifiers());

            // add to the menu
            menuItem.KeyboardAccelerators().Append(menuShortcut);
        }
        else // we've got a comma, so need to just use the alternate method
#endif
        {
            // extract the modifier and key to a nice format
            auto overrideString = _FormatOverrideShortcutText(keyChord.Modifiers());
            auto mappedCh = MapVirtualKeyW(keyChord.Vkey(), MAPVK_VK_TO_CHAR);
            if (mappedCh != 0)
            {
                menuItem.KeyboardAcceleratorTextOverride(overrideString + gsl::narrow_cast<wchar_t>(mappedCh));
            }
        }
    }

    // Method Description:
    // - Calculates the appropriate size to snap to in the given direction, for
    //   the given dimension. If the global setting `snapToGridOnResize` is set
    //   to `false`, this will just immediately return the provided dimension,
    //   effectively disabling snapping.
    // - See Pane::CalcSnappedDimension
    float TerminalPage::CalcSnappedDimension(const bool widthOrHeight, const float dimension) const
    {
        if (_settings && _settings.GlobalSettings().SnapToGridOnResize())
        {
            if (const auto tabImpl{ _GetFocusedTabImpl() })
            {
                return tabImpl->CalcSnappedDimension(widthOrHeight, dimension);
            }
        }
        return dimension;
    }

    // Function Description:
    // - This function is called when the `TermControl` requests that we send
    //   it the clipboard's content.
    // - Retrieves the data from the Windows Clipboard and converts it to text.
    // - Shows warnings if the clipboard is too big or contains multiple lines
    //   of text.
    // - Sends the text back to the TermControl through the event's
    //   `HandleClipboardData` member function.
    // - Does some of this in a background thread, as to not hang/crash the UI thread.
    // Arguments:
    // - eventArgs: the PasteFromClipboard event sent from the TermControl
    safe_void_coroutine TerminalPage::_PasteFromClipboardHandler(const IInspectable sender, const PasteFromClipboardEventArgs eventArgs)
    try
    {
        // The old Win32 clipboard API as used below is somewhere in the order of 300-1000x faster than
        // the WinRT one on average, depending on CPU load. Don't use the WinRT clipboard API if you can.
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        const auto globalSettings = _settings.GlobalSettings();
        const auto bracketedPaste = eventArgs.BracketedPasteEnabled();
        const auto sourceId = sender.try_as<ControlInteractivity>().Id();

        // GetClipboardData might block for up to 30s for delay-rendered contents.
        co_await winrt::resume_background();

        winrt::hstring text;
        if (const auto clipboard = clipboard::open(nullptr))
        {
            text = clipboard::read();
        }

        if (!bracketedPaste && globalSettings.TrimPaste())
        {
            text = winrt::hstring{ Utils::TrimPaste(text) };
        }

        // LOAD BEARING: Send an empty bracketed paste even if the clipboard was empty.
        // Bracketed Paste provides an application a way to know whether the
        // user pasted, even if there was no applicable content on it. This
        // behavior is observed in GNOME Terminal, among others.
        if (!bracketedPaste && text.empty())
        {
            co_return;
        }

        bool warnMultiLine = false;
        switch (globalSettings.WarnAboutMultiLinePaste())
        {
        case WarnAboutMultiLinePaste::Automatic:
            // NOTE that this is unsafe, because a shell that doesn't support bracketed paste
            // will allow an attacker to enable the mode, not realize that, and then accept
            // the paste as if it was a series of legitimate commands. See GH#13014.
            warnMultiLine = !bracketedPaste;
            break;
        case WarnAboutMultiLinePaste::Always:
            warnMultiLine = true;
            break;
        default:
            warnMultiLine = false;
            break;
        }

        if (warnMultiLine)
        {
            const std::wstring_view view{ text };
            warnMultiLine = view.find_first_of(L"\r\n") != std::wstring_view::npos;
        }

        constexpr std::size_t minimumSizeForWarning = 1024 * 5; // 5 KiB
        const auto warnLargeText = text.size() > minimumSizeForWarning && globalSettings.WarnAboutLargePaste();

        if (warnMultiLine || warnLargeText)
        {
            co_await wil::resume_foreground(dispatcher);

            if (const auto strongThis = weakThis.get())
            {
                // We have to initialize the dialog here to be able to change the text of the text block within it
                std::ignore = FindName(L"MultiLinePasteDialog");

                // WinUI absolutely cannot deal with large amounts of text (at least O(n), possibly O(n^2),
                // so we limit the string length here and add an ellipsis if necessary.
                auto clipboardText = text;
                if (clipboardText.size() > 1024)
                {
                    const std::wstring_view view{ text };
                    // Make sure we don't cut in the middle of a surrogate pair
                    const auto len = til::utf16_iterate_prev(view, 512);
                    clipboardText = til::hstring_format(FMT_COMPILE(L"{}\n…"), view.substr(0, len));
                }

                ClipboardText().Text(std::move(clipboardText));

                // The vertical offset on the scrollbar does not reset automatically, so reset it manually
                ClipboardContentScrollViewer().ScrollToVerticalOffset(0);

                auto warningResult = ContentDialogResult::Primary;
                if (warnMultiLine)
                {
                    warningResult = co_await _ShowMultiLinePasteWarningDialog();
                }
                else if (warnLargeText)
                {
                    warningResult = co_await _ShowLargePasteWarningDialog();
                }

                // Clear the clipboard text so it doesn't lie around in memory
                ClipboardText().Text({});

                if (warningResult != ContentDialogResult::Primary)
                {
                    // user rejected the paste
                    co_return;
                }
            }

            co_await winrt::resume_background();
        }

        // This will end up calling ConptyConnection::WriteInput which calls WriteFile which may block for
        // an indefinite amount of time. Avoid freezes and deadlocks by running this on a background thread.
        assert(!dispatcher.HasThreadAccess());
        eventArgs.HandleClipboardData(text);

        // GH#18821: If broadcast input is active, paste the same text into all other
        // panes on the tab. We do this here (rather than re-reading the
        // clipboard per-pane) so that only one paste warning is shown.
        co_await wil::resume_foreground(dispatcher);
        if (const auto strongThis = weakThis.get())
        {
            if (const auto& tab{ strongThis->_GetFocusedTabImpl() })
            {
                if (tab->TabStatus().IsInputBroadcastActive())
                {
                    tab->GetRootPane()->WalkTree([&](auto&& pane) {
                        if (const auto control = pane->GetTerminalControl())
                        {
                            if (control.ContentId() != sourceId && !control.ReadOnly())
                            {
                                control.RawWriteString(text);
                            }
                        }
                    });
                }
            }
        }
    }
    CATCH_LOG();

    safe_void_coroutine TerminalPage::_OpenHyperlinkHandler(const IInspectable /*sender*/, const Microsoft::Terminal::Control::OpenHyperlinkEventArgs eventArgs)
    {
        try
        {
            auto uriString{ eventArgs.Uri() };
            auto parsed = winrt::Windows::Foundation::Uri(uriString);
            if (_IsUriSupported(parsed))
            {
                bool shouldLaunch{ _IsUriConsideredSomewhatSafe(parsed) };

                if (!shouldLaunch)
                {
                    if (auto presenter{ _dialogPresenter.get() })
                    {
                        // FindName needs to be called first to actually load the xaml object
                        auto unopenedUriDialog = FindName(L"UriErrorDialog").try_as<WUX::Controls::ContentDialog>();

                        // Insert the reason and the URI
                        unopenedUriDialog.SecondaryButtonText(RS_(L"UnsafeUrlConfirmAllowAction"));
                        CouldNotOpenUriReason().Text(RS_(L"UnsafeUrlConfirmText"));
                        UnopenedUri().Text(uriString);

                        // Show the dialog
                        auto result = co_await presenter.ShowDialog(unopenedUriDialog);
                        shouldLaunch = result == ContentDialogResult::Secondary;
                    }
                }

                if (shouldLaunch)
                {
                    ShellExecuteW(nullptr, L"open", uriString.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            else
            {
                _ShowCouldNotOpenDialog(RS_(L"UnsupportedSchemeText"), uriString);
            }
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            _ShowCouldNotOpenDialog(RS_(L"InvalidUriText"), eventArgs.Uri());
        }
    }

    // Method Description:
    // - Opens up a dialog box explaining why we could not open a URI
    // Arguments:
    // - The reason (unsupported scheme, invalid uri, potentially more in the future)
    // - The uri
    void TerminalPage::_ShowCouldNotOpenDialog(winrt::hstring reason, winrt::hstring uri)
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            // FindName needs to be called first to actually load the xaml object
            auto unopenedUriDialog = FindName(L"UriErrorDialog").try_as<WUX::Controls::ContentDialog>();

            // Insert the reason and the URI
            unopenedUriDialog.SecondaryButtonText({});
            CouldNotOpenUriReason().Text(reason);
            UnopenedUri().Text(uri);

            // Show the dialog
            presenter.ShowDialog(unopenedUriDialog);
        }
    }

    // Method Description:
    // - Determines if the given URI is currently supported
    // Arguments:
    // - The parsed URI
    // Return value:
    // - True if we support it, false otherwise
    bool TerminalPage::_IsUriSupported(const winrt::Windows::Foundation::Uri& parsedUri)
    {
        if (parsedUri.SchemeName() == L"http" || parsedUri.SchemeName() == L"https")
        {
            return true;
        }
        if (parsedUri.SchemeName() == L"file")
        {
            const auto host = parsedUri.Host();
            // If no hostname was provided or if the hostname was "localhost", Host() will return an empty string
            // and we allow it
            if (host == L"")
            {
                return true;
            }

            // GH#10188: WSL paths are okay. We'll let those through.
            if (host == L"wsl$" || host == L"wsl.localhost")
            {
                return true;
            }

            // TODO: by the OSC 8 spec, if a hostname (other than localhost) is provided, we _should_ be
            // comparing that value against what is returned by GetComputerNameExW and making sure they match.
            // However, ShellExecute does not seem to be happy with file URIs of the form
            //          file://{hostname}/path/to/file.ext
            // and so while we could do the hostname matching, we do not know how to actually open the URI
            // if its given in that form. So for now we ignore all hostnames other than localhost
            return false;
        }

        // In this case, the app manually output a URI other than file:// or
        // http(s)://. We'll trust the user knows what they're doing when
        // clicking on those sorts of links.
        // See discussion in GH#7562 for more details.
        return true;
    }

    bool TerminalPage::_IsUriConsideredSomewhatSafe(const winrt::Windows::Foundation::Uri& parsedUri)
    {
        if (parsedUri.SchemeName() == L"http" || parsedUri.SchemeName() == L"https")
        {
            return true;
        }
        if (parsedUri.SchemeName() == L"file")
        {
            static const auto pathext{ wil::TryGetEnvironmentVariableW<std::wstring>(L"PATHEXT") };
            const auto filename = parsedUri.Path();
            for (const auto& e : til::split_iterator{ std::wstring_view{ pathext }, L';' })
            {
                if (til::ends_with_insensitive_ascii(filename, e))
                {
                    return false;
                }
            }

            return true;
        }

        return false;
    }

    // Important! Don't take this eventArgs by reference, we need to extend the
    // lifetime of it to the other side of the co_await!
    safe_void_coroutine TerminalPage::_ControlNoticeRaisedHandler(const IInspectable /*sender*/,
                                                                  const Microsoft::Terminal::Control::NoticeEventArgs eventArgs)
    {
        auto weakThis = get_weak();
        co_await wil::resume_foreground(Dispatcher());
        if (auto page = weakThis.get())
        {
            auto message = eventArgs.Message();

            winrt::hstring title;

            switch (eventArgs.Level())
            {
            case NoticeLevel::Debug:
                title = RS_(L"NoticeDebug"); //\xebe8
                break;
            case NoticeLevel::Info:
                title = RS_(L"NoticeInfo"); // \xe946
                break;
            case NoticeLevel::Warning:
                title = RS_(L"NoticeWarning"); //\xe7ba
                break;
            case NoticeLevel::Error:
                title = RS_(L"NoticeError"); //\xe783
                break;
            }

            page->_ShowControlNoticeDialog(title, message);
        }
    }

    void TerminalPage::_ShowControlNoticeDialog(const winrt::hstring& title, const winrt::hstring& message)
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            // FindName needs to be called first to actually load the xaml object
            auto controlNoticeDialog = FindName(L"ControlNoticeDialog").try_as<WUX::Controls::ContentDialog>();

            ControlNoticeDialog().Title(winrt::box_value(title));

            // Insert the message
            NoticeMessage().Text(message);

            // Show the dialog
            presenter.ShowDialog(controlNoticeDialog);
        }
    }

    // Method Description:
    // - Copy text from the focused terminal to the Windows Clipboard
    // Arguments:
    // - dismissSelection: if not enabled, copying text doesn't dismiss the selection
    // - singleLine: if enabled, copy contents as a single line of text
    // - withControlSequences: if enabled, the copied plain text contains color/style ANSI escape codes from the selection
    // - formats: dictate which formats need to be copied
    // Return Value:
    // - true iff we we able to copy text (if a selection was active)
    bool TerminalPage::_CopyText(const bool dismissSelection, const bool singleLine, const bool withControlSequences, const CopyFormat formats)
    {
        if (const auto& control{ _GetActiveControl() })
        {
            return control.CopySelectionToClipboard(dismissSelection, singleLine, withControlSequences, formats);
        }
        return false;
    }

    // Method Description:
    // - Send an event (which will be caught by AppHost) to set the progress indicator on the taskbar
    // Arguments:
    // - sender (not used)
    // - eventArgs: the arguments specifying how to set the progress indicator
    safe_void_coroutine TerminalPage::_SetTaskbarProgressHandler(const IInspectable /*sender*/, const IInspectable /*eventArgs*/)
    {
        const auto weak = get_weak();
        co_await wil::resume_foreground(Dispatcher());
        if (const auto strong = weak.get())
        {
            SetTaskbarProgress.raise(*this, nullptr);
        }
    }

    // Method Description:
    // - Send an event (which will be caught by AppHost) to change the show window state of the entire hosting window
    // Arguments:
    // - sender (not used)
    // - args: the arguments specifying how to set the display status to ShowWindow for our window handle
    void TerminalPage::_ShowWindowChangedHandler(const IInspectable /*sender*/, const Microsoft::Terminal::Control::ShowWindowArgs args)
    {
        ShowWindowChanged.raise(*this, args);
    }

    Windows::Foundation::IAsyncOperation<IVectorView<MatchResult>> TerminalPage::_FindPackageAsync(hstring query)
    {
        const PackageManager packageManager = WindowsPackageManagerFactory::CreatePackageManager();
        PackageCatalogReference catalogRef{
            packageManager.GetPredefinedPackageCatalog(PredefinedPackageCatalog::OpenWindowsCatalog)
        };
        catalogRef.PackageCatalogBackgroundUpdateInterval(std::chrono::hours(24));

        ConnectResult connectResult{ nullptr };
        for (int retries = 0;;)
        {
            connectResult = catalogRef.Connect();
            if (connectResult.Status() == ConnectResultStatus::Ok)
            {
                break;
            }

            if (++retries == 3)
            {
                co_return nullptr;
            }
        }

        PackageCatalog catalog = connectResult.PackageCatalog();
        PackageMatchFilter filter = WindowsPackageManagerFactory::CreatePackageMatchFilter();
        filter.Value(query);
        filter.Field(PackageMatchField::Command);
        filter.Option(PackageFieldMatchOption::Equals);

        FindPackagesOptions options = WindowsPackageManagerFactory::CreateFindPackagesOptions();
        options.Filters().Append(filter);
        options.ResultLimit(20);

        const auto result = co_await catalog.FindPackagesAsync(options);
        const IVectorView<MatchResult> pkgList = result.Matches();
        co_return pkgList;
    }

    Windows::Foundation::IAsyncAction TerminalPage::_SearchMissingCommandHandler(const IInspectable /*sender*/, const Microsoft::Terminal::Control::SearchMissingCommandEventArgs args)
    {
        if (!Feature_QuickFix::IsEnabled())
        {
            co_return;
        }

        const auto weak = get_weak();
        const auto dispatcher = Dispatcher();

        // All of the code until resume_foreground is static and
        // doesn't touch `this`, so we don't need weak/strong_ref.
        co_await winrt::resume_background();

        // no packages were found, nothing to suggest
        const auto pkgList = co_await _FindPackageAsync(args.MissingCommand());
        if (!pkgList || pkgList.Size() == 0)
        {
            co_return;
        }

        std::vector<hstring> suggestions;
        suggestions.reserve(pkgList.Size());
        for (const auto& pkg : pkgList)
        {
            // --id and --source ensure we don't collide with another package catalog
            suggestions.emplace_back(fmt::format(FMT_COMPILE(L"winget install --id {} -s winget"), pkg.CatalogPackage().Id()));
        }

        co_await wil::resume_foreground(dispatcher);
        const auto strong = weak.get();
        if (!strong)
        {
            co_return;
        }

        auto term = _GetActiveControl();
        if (!term)
        {
            co_return;
        }
        term.UpdateWinGetSuggestions(single_threaded_vector<hstring>(std::move(suggestions)));
        term.RefreshQuickFixMenu();
    }

    void TerminalPage::_WindowSizeChanged(const IInspectable sender, const Microsoft::Terminal::Control::WindowSizeChangedEventArgs args)
    {
        // Raise if:
        // - Not in quake mode
        // - Not in fullscreen
        // - Only one tab exists
        // - Only one pane exists
        // else:
        // - Reset conpty to its original size back
        if (!WindowProperties().IsQuakeWindow() && !Fullscreen() &&
            NumberOfTabs() == 1 && _GetFocusedTabImpl()->GetLeafPaneCount() == 1)
        {
            WindowSizeChanged.raise(*this, args);
        }
        else if (const auto& control{ sender.try_as<TermControl>() })
        {
            const auto& connection = control.Connection();

            if (const auto& conpty{ connection.try_as<TerminalConnection::ConptyConnection>() })
            {
                conpty.ResetSize();
            }
        }
    }

    void TerminalPage::_copyToClipboard(const IInspectable, const WriteToClipboardEventArgs args) const
    {
        if (const auto clipboard = clipboard::open(_hostingHwnd.value_or(nullptr)))
        {
            const auto plain = args.Plain();
            const auto html = args.Html();
            const auto rtf = args.Rtf();

            clipboard::write(
                { plain.data(), plain.size() },
                { reinterpret_cast<const char*>(html.data()), html.size() },
                { reinterpret_cast<const char*>(rtf.data()), rtf.size() });
        }
    }

    // Method Description:
    // - Paste text from the Windows Clipboard to the focused terminal
    void TerminalPage::_PasteText()
    {
        if (const auto& control{ _GetActiveControl() })
        {
            control.PasteTextFromClipboard();
        }
    }

    // Function Description:
    // - Called when the settings button is clicked. ShellExecutes the settings
    //   file, as to open it in the default editor for .json files. Does this in
    //   a background thread, as to not hang/crash the UI thread.
    safe_void_coroutine TerminalPage::_LaunchSettings(const SettingsTarget target)
    {
        if (target == SettingsTarget::SettingsUI)
        {
            OpenSettingsUI();
        }
        else
        {
            // This will switch the execution of the function to a background (not
            // UI) thread. This is IMPORTANT, because the Windows.Storage API's
            // (used for retrieving the path to the file) will crash on the UI
            // thread, because the main thread is a STA.
            //
            // NOTE: All remaining code of this function doesn't touch `this`, so we don't need weak/strong_ref.
            // NOTE NOTE: Don't touch `this` when you make changes here.
            co_await winrt::resume_background();

            auto openFile = [](const auto& filePath) {
                HINSTANCE res = ShellExecute(nullptr, nullptr, filePath.c_str(), nullptr, nullptr, SW_SHOW);
                if (static_cast<int>(reinterpret_cast<uintptr_t>(res)) <= 32)
                {
                    ShellExecute(nullptr, nullptr, L"notepad", filePath.c_str(), nullptr, SW_SHOW);
                }
            };

            auto openFolder = [](const auto& filePath) {
                HINSTANCE res = ShellExecute(nullptr, nullptr, filePath.c_str(), nullptr, nullptr, SW_SHOW);
                if (static_cast<int>(reinterpret_cast<uintptr_t>(res)) <= 32)
                {
                    ShellExecute(nullptr, nullptr, L"open", filePath.c_str(), nullptr, SW_SHOW);
                }
            };

            switch (target)
            {
            case SettingsTarget::DefaultsFile:
                openFile(CascadiaSettings::DefaultSettingsPath());
                break;
            case SettingsTarget::SettingsFile:
                openFile(CascadiaSettings::SettingsPath());
                break;
            case SettingsTarget::Directory:
                openFolder(CascadiaSettings::SettingsDirectory());
                break;
            case SettingsTarget::AllFiles:
                openFile(CascadiaSettings::DefaultSettingsPath());
                openFile(CascadiaSettings::SettingsPath());
                break;
            }
        }
    }

    // Method Description:
    // - Responds to the TabView control's Tab Closing event by removing
    //      the indicated tab from the set and focusing another one.
    //      The event is cancelled so App maintains control over the
    //      items in the tabview.
    // Arguments:
    // - sender: the control that originated this event
    // - eventArgs: the event's constituent arguments
    void TerminalPage::_OnTabCloseRequested(const IInspectable& /*sender*/, const MUX::Controls::TabViewTabCloseRequestedEventArgs& eventArgs)
    {
        const auto tabViewItem = eventArgs.Tab();
        if (auto tab{ _GetTabByTabViewItem(tabViewItem) })
        {
            _HandleCloseTabRequested(tab);
        }
    }

    TermControl TerminalPage::_CreateNewControlAndContent(const Settings::TerminalSettingsCreateResult& settings, const ITerminalConnection& connection)
    {
        // Do any initialization that needs to apply to _every_ TermControl we
        // create here.
        const auto content = _manager.CreateCore(*settings.DefaultSettings(), settings.UnfocusedSettings().try_as<IControlAppearance>(), connection);
        const TermControl control{ content };
        return _SetupControl(control);
    }

    TermControl TerminalPage::_AttachControlToContent(const uint64_t& contentId)
    {
        if (const auto& content{ _manager.TryLookupCore(contentId) })
        {
            // We have to pass in our current keybindings, because that's an
            // object that belongs to this TerminalPage, on this thread. If we
            // don't, then when we move the content to another thread, and it
            // tries to handle a key, it'll callback on the original page's
            // stack, inevitably resulting in a wrong_thread
            return _SetupControl(TermControl::NewControlByAttachingContent(content));
        }
        return nullptr;
    }

    TermControl TerminalPage::_SetupControl(const TermControl& term)
    {
        // GH#12515: ConPTY assumes it's hidden at the start. If we're not, let it know now.
        if (_visible)
        {
            term.WindowVisibilityChanged(_visible);
        }

        // Even in the case of re-attaching content from another window, this
        // will correctly update the control's owning HWND
        if (_hostingHwnd.has_value())
        {
            term.OwningHwnd(reinterpret_cast<uint64_t>(*_hostingHwnd));
        }

        term.KeyBindings(*_bindings);

        _RegisterTerminalEvents(term);
        return term;
    }

    // Method Description:
    // - Creates a pane and returns a shared_ptr to it
    // - The caller should handle where the pane goes after creation,
    //   either to split an already existing pane or to create a new tab with it
    // Arguments:
    // - newTerminalArgs: an object that may contain a blob of parameters to
    //   control which profile is created and with possible other
    //   configurations. See CascadiaSettings::BuildSettings for more details.
    // - sourceTab: an optional tab reference that indicates that the created
    //   pane should be a duplicate of the tab's focused pane
    // - existingConnection: optionally receives a connection from the outside
    //   world instead of attempting to create one
    // Return Value:
    // - If the newTerminalArgs required us to open the pane as a new elevated
    //   connection, then we'll return nullptr. Otherwise, we'll return a new
    //   Pane for this connection.
    std::shared_ptr<Pane> TerminalPage::_MakeTerminalPane(const NewTerminalArgs& newTerminalArgs,
                                                          const winrt::TerminalApp::Tab& sourceTab,
                                                          TerminalConnection::ITerminalConnection existingConnection)
    {
        // First things first - Check for making a pane from content ID.
        if (newTerminalArgs &&
            newTerminalArgs.ContentId() != 0)
        {
            // Don't need to worry about duplicating or anything - we'll
            // serialize the actual profile's GUID along with the content guid.
            const auto& profile = _settings.GetProfileForArgs(newTerminalArgs);
            const auto control = _AttachControlToContent(newTerminalArgs.ContentId());
            auto paneContent{ winrt::make<TerminalPaneContent>(profile, _terminalSettingsCache, control) };
            return std::make_shared<Pane>(paneContent);
        }

        Settings::TerminalSettingsCreateResult controlSettings{ nullptr };
        Profile profile{ nullptr };

        if (const auto& tabImpl{ _GetTabImpl(sourceTab) })
        {
            profile = tabImpl->GetFocusedProfile();
            if (profile)
            {
                // TODO GH#5047 If we cache the NewTerminalArgs, we no longer need to do this.
                profile = GetClosestProfileForDuplicationOfProfile(profile);
                controlSettings = Settings::TerminalSettings::CreateWithProfile(_settings, profile);
                const auto workingDirectory = tabImpl->GetActiveTerminalControl().WorkingDirectory();
                if (Utils::IsValidDirectory(workingDirectory.c_str()))
                {
                    controlSettings.DefaultSettings()->StartingDirectory(workingDirectory);
                }
            }
        }
        if (!profile)
        {
            profile = _settings.GetProfileForArgs(newTerminalArgs);
            controlSettings = Settings::TerminalSettings::CreateWithNewTerminalArgs(_settings, newTerminalArgs);
        }

        // Try to handle auto-elevation
        if (_maybeElevate(newTerminalArgs, controlSettings, profile))
        {
            return nullptr;
        }

        const auto sessionId = controlSettings.DefaultSettings()->SessionId();
        const auto hasSessionId = sessionId != winrt::guid{};

        auto connection = existingConnection ? existingConnection : _CreateConnectionFromSettings(profile, *controlSettings.DefaultSettings(), hasSessionId);
        if (existingConnection)
        {
            connection.Resize(controlSettings.DefaultSettings()->InitialRows(), controlSettings.DefaultSettings()->InitialCols());
        }

        TerminalConnection::ITerminalConnection debugConnection{ nullptr };
        if (_settings.GlobalSettings().DebugFeaturesEnabled())
        {
            const auto window = CoreWindow::GetForCurrentThread();
            const auto rAltState = window.GetKeyState(VirtualKey::RightMenu);
            const auto lAltState = window.GetKeyState(VirtualKey::LeftMenu);
            const auto bothAltsPressed = WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) &&
                                         WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);
            if (bothAltsPressed)
            {
                std::tie(connection, debugConnection) = OpenDebugTapConnection(connection);
            }
        }

        const auto control = _CreateNewControlAndContent(controlSettings, connection);

        if (hasSessionId)
        {
            using namespace std::string_view_literals;

            const auto settingsDir = CascadiaSettings::SettingsDirectory();
            const auto admin = IsRunningElevated();
            const auto filenamePrefix = admin ? L"elevated_"sv : L"buffer_"sv;
            const auto path = fmt::format(FMT_COMPILE(L"{}\\{}{}.txt"), settingsDir, filenamePrefix, sessionId);
            control.RestoreFromPath(path);
        }

        auto paneContent{ winrt::make<TerminalPaneContent>(profile, _terminalSettingsCache, control) };

        auto resultPane = std::make_shared<Pane>(paneContent);

        if (debugConnection) // this will only be set if global debugging is on and tap is active
        {
            auto newControl = _CreateNewControlAndContent(controlSettings, debugConnection);
            // Split (auto) with the debug tap.
            auto debugContent{ winrt::make<TerminalPaneContent>(profile, _terminalSettingsCache, newControl) };
            auto debugPane = std::make_shared<Pane>(debugContent);

            // Since we're doing this split directly on the pane (instead of going through Tab,
            // we need to handle the panes 'active' states

            // Set the pane we're splitting to active (otherwise Split will not do anything)
            resultPane->SetActive();
            auto [original, _] = resultPane->Split(SplitDirection::Automatic, 0.5f, debugPane);

            // Set the non-debug pane as active
            resultPane->ClearActive();
            original->SetActive();
        }

        return resultPane;
    }

    // NOTE: callers of _MakePane should be able to accept nullptr as a return
    // value gracefully.
    std::shared_ptr<Pane> TerminalPage::_MakePane(const INewContentArgs& contentArgs,
                                                  const winrt::TerminalApp::Tab& sourceTab,
                                                  TerminalConnection::ITerminalConnection existingConnection)

    {
        const auto& newTerminalArgs{ contentArgs.try_as<NewTerminalArgs>() };
        if (contentArgs == nullptr || newTerminalArgs != nullptr || contentArgs.Type().empty())
        {
            // Terminals are of course special, and have to deal with debug taps, duplicating the tab, etc.
            return _MakeTerminalPane(newTerminalArgs, sourceTab, existingConnection);
        }

        IPaneContent content{ nullptr };

        const auto& paneType{ contentArgs.Type() };
        if (paneType == L"scratchpad")
        {
            const auto& scratchPane{ winrt::make_self<ScratchpadContent>() };

            // This is maybe a little wacky - add our key event handler to the pane
            // we made. So that we can get actions for keys that the content didn't
            // handle.
            scratchPane->GetRoot().KeyDown({ get_weak(), &TerminalPage::_KeyDownHandler });

            content = *scratchPane;
        }
        else if (paneType == L"settings")
        {
            content = _makeSettingsContent();
        }
        else if (paneType == L"snippets")
        {
            // Prevent the user from opening a bunch of snippets panes.
            //
            // Look at the focused tab, and if it already has one, then just focus it.
            if (const auto& focusedTab{ _GetFocusedTabImpl() })
            {
                const auto rootPane{ focusedTab->GetRootPane() };
                const bool found = rootPane == nullptr ? false : rootPane->WalkTree([](const auto& p) -> bool {
                    if (const auto& snippets{ p->GetContent().try_as<SnippetsPaneContent>() })
                    {
                        snippets->Focus(FocusState::Programmatic);
                        return true;
                    }
                    return false;
                });
                // Bail out if we already found one.
                if (found)
                {
                    return nullptr;
                }
            }

            const auto& tasksContent{ winrt::make_self<SnippetsPaneContent>() };
            tasksContent->UpdateSettings(_settings);
            tasksContent->GetRoot().KeyDown({ this, &TerminalPage::_KeyDownHandler });
            tasksContent->DispatchCommandRequested({ this, &TerminalPage::_OnDispatchCommandRequested });
            if (const auto& termControl{ _GetActiveControl() })
            {
                tasksContent->SetLastActiveControl(termControl);
            }

            content = *tasksContent;
        }
        else if (paneType == L"x-markdown")
        {
            if (Feature_MarkdownPane::IsEnabled())
            {
                const auto& markdownContent{ winrt::make_self<MarkdownPaneContent>(L"") };
                markdownContent->UpdateSettings(_settings);
                markdownContent->GetRoot().KeyDown({ this, &TerminalPage::_KeyDownHandler });

                // This one doesn't use DispatchCommand, because we don't create
                // Command's freely at runtime like we do with just plain old actions.
                markdownContent->DispatchActionRequested([weak = get_weak()](const auto& sender, const auto& actionAndArgs) {
                    if (const auto& page{ weak.get() })
                    {
                        page->_actionDispatch->DoAction(sender, actionAndArgs);
                    }
                });
                if (const auto& termControl{ _GetActiveControl() })
                {
                    markdownContent->SetLastActiveControl(termControl);
                }

                content = *markdownContent;
            }
        }

        assert(content);

        return std::make_shared<Pane>(content);
    }

    void TerminalPage::_restartPaneConnection(
        const TerminalApp::TerminalPaneContent& paneContent,
        const winrt::Windows::Foundation::IInspectable&)
    {
        // Note: callers are likely passing in `nullptr` as the args here, as
        // the TermControl.RestartTerminalRequested event doesn't actually pass
        // any args upwards itself. If we ever change this, make sure you check
        // for nulls
        if (const auto& connection{ _duplicateConnectionForRestart(paneContent) })
        {
            // Reset the terminal's VT state before attaching the new connection.
            // The previous client may have left dirty modes (e.g., bracketed
            // paste, mouse tracking, alternate buffer, kitty keyboard) that
            // would corrupt input/output for the new shell process.
            const auto& termControl = paneContent.GetTermControl();
            termControl.HardResetWithoutErase();
            termControl.Connection(connection);
            connection.Start();
        }
    }

    // Method Description:
    // - Sets background image and applies its settings (stretch, opacity and alignment)
    // - Checks path validity
    // Arguments:
    // - newAppearance
    // Return Value:
    // - <none>
    void TerminalPage::_SetBackgroundImage(const winrt::Microsoft::Terminal::Settings::Model::IAppearanceConfig& newAppearance)
    {
        if (!_settings.GlobalSettings().UseBackgroundImageForWindow())
        {
            _tabContent.Background(nullptr);
            return;
        }

        const auto path = newAppearance.BackgroundImagePath().Resolved();
        if (path.empty())
        {
            _tabContent.Background(nullptr);
            return;
        }

        Windows::Foundation::Uri imageUri{ nullptr };
        try
        {
            imageUri = Windows::Foundation::Uri{ path };
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            _tabContent.Background(nullptr);
            return;
        }
        // Check if the image brush is already pointing to the image
        // in the modified settings; if it isn't (or isn't there),
        // set a new image source for the brush

        auto brush = _tabContent.Background().try_as<Media::ImageBrush>();
        Media::Imaging::BitmapImage imageSource = brush == nullptr ? nullptr : brush.ImageSource().try_as<Media::Imaging::BitmapImage>();

        if (imageSource == nullptr ||
            imageSource.UriSource() == nullptr ||
            !imageSource.UriSource().Equals(imageUri))
        {
            Media::ImageBrush b{};
            // Note that BitmapImage handles the image load asynchronously,
            // which is especially important since the image
            // may well be both large and somewhere out on the
            // internet.
            Media::Imaging::BitmapImage image(imageUri);
            b.ImageSource(image);
            _tabContent.Background(b);
        }

        // Pull this into a separate block. If the image didn't change, but the
        // properties of the image did, we should still update them.
        if (const auto newBrush{ _tabContent.Background().try_as<Media::ImageBrush>() })
        {
            newBrush.Stretch(newAppearance.BackgroundImageStretchMode());
            newBrush.Opacity(newAppearance.BackgroundImageOpacity());
        }
    }

    // Method Description:
    // - Hook up keybindings, and refresh the UI of the terminal.
    //   This includes update the settings of all the tabs according
    //   to their profiles, update the title and icon of each tab, and
    //   finally create the tab flyout
    void TerminalPage::_RefreshUIForSettingsReload()
    {
        // Re-wire the keybindings to their handlers, as we'll have created a
        // new AppKeyBindings object.
        _HookupKeyBindings(_settings.ActionMap());

        // Refresh UI elements

        // Recreate the TerminalSettings cache here. We'll use that as we're
        // updating terminal panes, so that we don't have to build a _new_
        // TerminalSettings for every profile we update - we can just look them
        // up the previous ones we built.
        _terminalSettingsCache->Reset(_settings);

        for (const auto& tab : _tabs)
        {
            if (auto tabImpl{ _GetTabImpl(tab) })
            {
                // Let the tab know that there are new settings. It's up to each content to decide what to do with them.
                tabImpl->UpdateSettings(_settings);

                // Update the icon of the tab for the currently focused profile in that tab.
                // Only do this for TerminalTabs. Other types of tabs won't have multiple panes
                // and profiles so the Title and Icon will be set once and only once on init.
                _UpdateTabIcon(*tabImpl);

                // Force the TerminalTab to re-grab its currently active control's title.
                tabImpl->UpdateTitle();
            }

            auto tabImpl{ winrt::get_self<Tab>(tab) };
            tabImpl->SetActionMap(_settings.ActionMap());
        }

        if (const auto focusedTab{ _GetFocusedTabImpl() })
        {
            if (const auto profile{ focusedTab->GetFocusedProfile() })
            {
                _SetBackgroundImage(profile.DefaultAppearance());
            }
        }

        // repopulate the new tab button's flyout with entries for each
        // profile, which might have changed
        _UpdateTabWidthMode();
        _CreateNewTabFlyout();

        // Reload the current value of alwaysOnTop from the settings file. This
        // will let the user hot-reload this setting, but any runtime changes to
        // the alwaysOnTop setting will be lost.
        _isAlwaysOnTop = _settings.GlobalSettings().AlwaysOnTop();
        AlwaysOnTopChanged.raise(*this, nullptr);

        _showTabsFullscreen = _settings.GlobalSettings().ShowTabsFullscreen();

        // Settings AllowDependentAnimations will affect whether animations are
        // enabled application-wide, so we don't need to check it each time we
        // want to create an animation.
        WUX::Media::Animation::Timeline::AllowDependentAnimations(!_settings.GlobalSettings().DisableAnimations());

        _tabRow.ShowElevationShield(IsRunningElevated() && _settings.GlobalSettings().ShowAdminShield());

        Media::SolidColorBrush transparent{ Windows::UI::Colors::Transparent() };
        _tabView.Background(transparent);

        ////////////////////////////////////////////////////////////////////////
        // Begin Theme handling
        _updateThemeColors();

        _updateAllTabCloseButtons();

        // The user may have changed the "show title in titlebar" setting.
        TitleChanged.raise(*this, nullptr);
    }

    void TerminalPage::_updateAllTabCloseButtons()
    {
        // Update the state of the CloseButtonOverlayMode property of
        // our TabView, to match the tab.showCloseButton property in the theme.
        //
        // Also update every tab's individual IsClosable to match the same property.
        const auto theme = _settings.GlobalSettings().CurrentTheme();
        const auto visibility = (theme && theme.Tab()) ?
                                    theme.Tab().ShowCloseButton() :
                                    Settings::Model::TabCloseButtonVisibility::Always;

        _tabItemMiddleClickHookEnabled = visibility == Settings::Model::TabCloseButtonVisibility::Never;

        for (const auto& tab : _tabs)
        {
            tab.CloseButtonVisibility(visibility);
        }

        switch (visibility)
        {
        case Settings::Model::TabCloseButtonVisibility::Never:
            _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::Auto);
            break;
        case Settings::Model::TabCloseButtonVisibility::Hover:
            _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::OnPointerOver);
            break;
        case Settings::Model::TabCloseButtonVisibility::ActiveOnly:
        default:
            _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::Always);
            break;
        }
    }

    // Method Description:
    // - Sets the initial actions to process on startup. We'll make a copy of
    //   this list, and process these actions when we're loaded.
    // - This function will have no effective result after Create() is called.
    // Arguments:
    // - actions: a list of Actions to process on startup.
    // Return Value:
    // - <none>
    void TerminalPage::SetStartupActions(std::vector<ActionAndArgs> actions)
    {
        _startupActions = std::move(actions);
    }

    void TerminalPage::SetStartupConnection(ITerminalConnection connection)
    {
        _startupConnection = std::move(connection);
    }

    void TerminalPage::SetPersistedSidebarWidth(double width)
    {
        _persistedSidebarWidth = width;
    }

    // Receives the workspace list state already deserialized by
    // TerminalWindow from the persisted `workspaceLayout` blob. Consumed once
    // by `_HydrateWorkspacesFromPersistedState()` during `Create()`. If
    // TerminalWindow couldn't parse the blob (corruption, schema drift), it
    // simply doesn't call this — and Create() falls through to
    // `_BootstrapDefaultWorkspace()`, the cascade endpoint.
    void TerminalPage::SetPersistedWorkspaceState(::TerminalApp::WorkspaceListState state)
    {
        _persistedWorkspaceState = std::move(state);
    }

    winrt::TerminalApp::IDialogPresenter TerminalPage::DialogPresenter() const
    {
        return _dialogPresenter.get();
    }

    void TerminalPage::DialogPresenter(winrt::TerminalApp::IDialogPresenter dialogPresenter)
    {
        _dialogPresenter = dialogPresenter;
    }

    // Method Description:
    // - Get the combined taskbar state for the page. This is the combination of
    //   all the states of all the tabs, which are themselves a combination of
    //   all their panes. Taskbar states are given a priority based on the rules
    //   in:
    //   https://docs.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-itaskbarlist3-setprogressstate
    //   under "How the Taskbar Button Chooses the Progress Indicator for a Group"
    // Arguments:
    // - <none>
    // Return Value:
    // - A TaskbarState object representing the combined taskbar state and
    //   progress percentage of all our tabs.
    winrt::TerminalApp::TaskbarState TerminalPage::TaskbarState() const
    {
        auto state{ winrt::make<winrt::TerminalApp::implementation::TaskbarState>() };

        for (const auto& tab : _tabs)
        {
            if (auto tabImpl{ _GetTabImpl(tab) })
            {
                auto tabState{ tabImpl->GetCombinedTaskbarState() };
                // lowest priority wins
                if (tabState.Priority() < state.Priority())
                {
                    state = tabState;
                }
            }
        }

        return state;
    }

    // Method Description:
    // - This is the method that App will call when the titlebar
    //   has been clicked. It dismisses any open flyouts.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::TitlebarClicked()
    {
        if (_newTabButton && _newTabButton.Flyout())
        {
            _newTabButton.Flyout().Hide();
        }
        _DismissTabContextMenus();
    }

    // Method Description:
    // - Notifies all attached console controls that the visibility of the
    //   hosting window has changed. The underlying PTYs may need to know this
    //   for the proper response to `::GetConsoleWindow()` from a Win32 console app.
    // Arguments:
    // - showOrHide: Show is true; hide is false.
    // Return Value:
    // - <none>
    void TerminalPage::WindowVisibilityChanged(const bool showOrHide)
    {
        _visible = showOrHide;
        for (const auto& tab : _tabs)
        {
            if (auto tabImpl{ _GetTabImpl(tab) })
            {
                // Manually enumerate the panes in each tab; this will let us recycle TerminalSettings
                // objects but only have to iterate one time.
                tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                    if (auto control = pane->GetTerminalControl())
                    {
                        control.WindowVisibilityChanged(showOrHide);
                    }
                });
            }
        }
    }

    // Method Description:
    // - Called when the user tries to do a search using keybindings.
    //   This will tell the active terminal control of the passed tab
    //   to create a search box and enable find process.
    // Arguments:
    // - tab: the tab where the search box should be created
    // Return Value:
    // - <none>
    void TerminalPage::_Find(const Tab& tab)
    {
        if (const auto& control{ tab.GetActiveTerminalControl() })
        {
            control.CreateSearchBoxControl();
        }
    }

    // Method Description:
    // - Toggles borderless mode. Hides the tab row, and raises our
    //   FocusModeChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::ToggleFocusMode()
    {
        SetFocusMode(!_isInFocusMode);
    }

    void TerminalPage::SetFocusMode(const bool inFocusMode)
    {
        const auto newInFocusMode = inFocusMode;
        if (newInFocusMode != FocusMode())
        {
            _isInFocusMode = newInFocusMode;
            _UpdateTabView();
            FocusModeChanged.raise(*this, nullptr);
        }
    }

    // Method Description:
    // - Toggles fullscreen mode. Hides the tab row, and raises our
    //   FullscreenChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::ToggleFullscreen()
    {
        SetFullscreen(!_isFullscreen);
    }

    // Method Description:
    // - Toggles always on top mode. Raises our AlwaysOnTopChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::ToggleAlwaysOnTop()
    {
        _isAlwaysOnTop = !_isAlwaysOnTop;
        AlwaysOnTopChanged.raise(*this, nullptr);
    }

    // Method Description:
    // - Sets the tab split button color when a new tab color is selected
    // Arguments:
    // - color: The color of the newly selected tab, used to properly calculate
    //          the foreground color of the split button (to match the font
    //          color of the tab)
    // - accentColor: the actual color we are going to use to paint the tab row and
    //                split button, so that there is some contrast between the tab
    //                and the non-client are behind it
    // Return Value:
    // - <none>
    void TerminalPage::_SetNewTabButtonColor(const til::color color, const til::color accentColor)
    {
        constexpr auto lightnessThreshold = 0.6f;
        // TODO GH#3327: Look at what to do with the tab button when we have XAML theming
        const auto isBrightColor = ColorFix::GetLightness(color) >= lightnessThreshold;
        const auto isLightAccentColor = ColorFix::GetLightness(accentColor) >= lightnessThreshold;
        const auto hoverColorAdjustment = isLightAccentColor ? -0.05f : 0.05f;
        const auto pressedColorAdjustment = isLightAccentColor ? -0.1f : 0.1f;

        const auto foregroundColor = isBrightColor ? Colors::Black() : Colors::White();
        const auto hoverColor = til::color{ ColorFix::AdjustLightness(accentColor, hoverColorAdjustment) };
        const auto pressedColor = til::color{ ColorFix::AdjustLightness(accentColor, pressedColorAdjustment) };

        Media::SolidColorBrush backgroundBrush{ accentColor };
        Media::SolidColorBrush backgroundHoverBrush{ hoverColor };
        Media::SolidColorBrush backgroundPressedBrush{ pressedColor };
        Media::SolidColorBrush foregroundBrush{ foregroundColor };

        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonBackground"), backgroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonBackgroundPointerOver"), backgroundHoverBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonBackgroundPressed"), backgroundPressedBrush);

        // Load bearing: The SplitButton uses SplitButtonForegroundSecondary for
        // the secondary button, but {TemplateBinding Foreground} for the
        // primary button.
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForeground"), foregroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForegroundPointerOver"), foregroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForegroundPressed"), foregroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForegroundSecondary"), foregroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForegroundSecondaryPressed"), foregroundBrush);

        _newTabButton.Background(backgroundBrush);
        _newTabButton.Foreground(foregroundBrush);

        // This is just like what we do in Tab::_RefreshVisualState. We need
        // to manually toggle the visual state, so the setters in the visual
        // state group will re-apply, and set our currently selected colors in
        // the resources.
        VisualStateManager::GoToState(_newTabButton, L"FlyoutOpen", true);
        VisualStateManager::GoToState(_newTabButton, L"Normal", true);
    }

    // Method Description:
    // - Clears the tab split button color to a system color
    //   (or white if none is found) when the tab's color is cleared
    // - Clears the tab row color to a system color
    //   (or white if none is found) when the tab's color is cleared
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_ClearNewTabButtonColor()
    {
        // TODO GH#3327: Look at what to do with the tab button when we have XAML theming
        winrt::hstring keys[] = {
            L"SplitButtonBackground",
            L"SplitButtonBackgroundPointerOver",
            L"SplitButtonBackgroundPressed",
            L"SplitButtonForeground",
            L"SplitButtonForegroundSecondary",
            L"SplitButtonForegroundPointerOver",
            L"SplitButtonForegroundPressed",
            L"SplitButtonForegroundSecondaryPressed"
        };

        // simply clear any of the colors in the split button's dict
        for (auto keyString : keys)
        {
            auto key = winrt::box_value(keyString);
            if (_newTabButton.Resources().HasKey(key))
            {
                _newTabButton.Resources().Remove(key);
            }
        }

        const auto res = Application::Current().Resources();

        const auto defaultBackgroundKey = winrt::box_value(L"TabViewItemHeaderBackground");
        const auto defaultForegroundKey = winrt::box_value(L"SystemControlForegroundBaseHighBrush");
        winrt::Windows::UI::Xaml::Media::SolidColorBrush backgroundBrush;
        winrt::Windows::UI::Xaml::Media::SolidColorBrush foregroundBrush;

        // TODO: Related to GH#3917 - I think if the system is set to "Dark"
        // theme, but the app is set to light theme, then this lookup still
        // returns to us the dark theme brushes. There's gotta be a way to get
        // the right brushes...
        // See also GH#5741
        if (res.HasKey(defaultBackgroundKey))
        {
            auto obj = res.Lookup(defaultBackgroundKey);
            backgroundBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
        }
        else
        {
            backgroundBrush = winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::Colors::Black() };
        }

        if (res.HasKey(defaultForegroundKey))
        {
            auto obj = res.Lookup(defaultForegroundKey);
            foregroundBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
        }
        else
        {
            foregroundBrush = winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::Colors::White() };
        }

        _newTabButton.Background(backgroundBrush);
        _newTabButton.Foreground(foregroundBrush);
    }

    // Function Description:
    // - This is a helper method to get the commandline out of a
    //   ExecuteCommandline action, break it into subcommands, and attempt to
    //   parse it into actions. This is used by _HandleExecuteCommandline for
    //   processing commandlines in the current WT window.
    // Arguments:
    // - args: the ExecuteCommandlineArgs to synthesize a list of startup actions for.
    // Return Value:
    // - an empty list if we failed to parse; otherwise, a list of actions to execute.
    std::vector<ActionAndArgs> TerminalPage::ConvertExecuteCommandlineToActions(const ExecuteCommandlineArgs& args)
    {
        ::TerminalApp::AppCommandlineArgs appArgs;
        if (appArgs.ParseArgs(args) == 0)
        {
            return appArgs.GetStartupActions();
        }

        return {};
    }

    void TerminalPage::_FocusActiveControl(IInspectable /*sender*/,
                                           IInspectable /*eventArgs*/)
    {
        _FocusCurrentTab(false);
    }

    bool TerminalPage::FocusMode() const
    {
        return _isInFocusMode;
    }

    bool TerminalPage::Fullscreen() const
    {
        return _isFullscreen;
    }

    // Method Description:
    // - Returns true if we're currently in "Always on top" mode. When we're in
    //   always on top mode, the window should be on top of all other windows.
    //   If multiple windows are all "always on top", they'll maintain their own
    //   z-order, with all the windows on top of all other non-topmost windows.
    // Arguments:
    // - <none>
    // Return Value:
    // - true if we should be in "always on top" mode
    bool TerminalPage::AlwaysOnTop() const
    {
        return _isAlwaysOnTop;
    }

    // Method Description:
    // - Returns true if the tab row should be visible when we're in full screen
    //   state.
    // Arguments:
    // - <none>
    // Return Value:
    // - true if the tab row should be visible in full screen state
    bool TerminalPage::ShowTabsFullscreen() const
    {
        return _showTabsFullscreen;
    }

    // Method Description:
    // - Updates the visibility of the tab row when in fullscreen state.
    void TerminalPage::SetShowTabsFullscreen(bool newShowTabsFullscreen)
    {
        if (_showTabsFullscreen == newShowTabsFullscreen)
        {
            return;
        }

        _showTabsFullscreen = newShowTabsFullscreen;

        // if we're currently in fullscreen, update tab view to make
        // sure tabs are given the correct visibility
        if (_isFullscreen)
        {
            _UpdateTabView();
        }
    }

    void TerminalPage::SetFullscreen(bool newFullscreen)
    {
        if (_isFullscreen == newFullscreen)
        {
            return;
        }
        _isFullscreen = newFullscreen;
        _UpdateTabView();
        FullscreenChanged.raise(*this, nullptr);
    }

    // Method Description:
    // - Updates the page's state for isMaximized when the window changes externally.
    void TerminalPage::Maximized(bool newMaximized)
    {
        _isMaximized = newMaximized;
    }

    // Method Description:
    // - Asks the window to change its maximized state.
    void TerminalPage::RequestSetMaximized(bool newMaximized)
    {
        if (_isMaximized == newMaximized)
        {
            return;
        }
        _isMaximized = newMaximized;
        ChangeMaximizeRequested.raise(*this, nullptr);
    }

    TerminalApp::IPaneContent TerminalPage::_makeSettingsContent()
    {
        if (auto app{ winrt::Windows::UI::Xaml::Application::Current().try_as<winrt::TerminalApp::App>() })
        {
            if (auto appPrivate{ winrt::get_self<implementation::App>(app) })
            {
                // Lazily load the Settings UI components so that we don't do it on startup.
                appPrivate->PrepareForSettingsUI();
            }
        }

        // Create the SUI pane content
        auto settingsContent{ winrt::make_self<SettingsPaneContent>(_settings) };
        auto sui = settingsContent->SettingsUI();

        if (_hostingHwnd)
        {
            sui.SetHostingWindow(reinterpret_cast<uint64_t>(*_hostingHwnd));
        }

        // GH#8767 - let unhandled keys in the SUI try to run commands too.
        sui.KeyDown({ get_weak(), &TerminalPage::_KeyDownHandler });

        sui.OpenJson([weakThis{ get_weak() }](auto&& /*s*/, winrt::Microsoft::Terminal::Settings::Model::SettingsTarget e) {
            if (auto page{ weakThis.get() })
            {
                page->_LaunchSettings(e);
            }
        });

        sui.ShowLoadWarningsDialog([weakThis{ get_weak() }](auto&& /*s*/, const Windows::Foundation::Collections::IVectorView<winrt::Microsoft::Terminal::Settings::Model::SettingsLoadWarnings>& warnings) {
            if (auto page{ weakThis.get() })
            {
                page->ShowLoadWarningsDialog.raise(*page, warnings);
            }
        });

        return *settingsContent;
    }

    // Method Description:
    // - Creates a settings UI tab and focuses it. If there's already a settings UI tab open,
    //   just focus the existing one.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::OpenSettingsUI()
    {
        // If we're holding the settings tab's switch command, don't create a new one, switch to the existing one.
        if (!_settingsTab)
        {
            // Create the tab
            auto resultPane = std::make_shared<Pane>(_makeSettingsContent());
            _settingsTab = _CreateNewTabFromPane(resultPane);
        }
        else
        {
            _tabView.SelectedItem(_settingsTab.TabViewItem());
        }
    }

    // Method Description:
    // - Returns a com_ptr to the implementation type of the given tab if it's a Tab.
    //   If the tab is not a TerminalTab, returns nullptr.
    // Arguments:
    // - tab: the projected type of a Tab
    // Return Value:
    // - If the tab is a TerminalTab, a com_ptr to the implementation type.
    //   If the tab is not a TerminalTab, nullptr
    winrt::com_ptr<Tab> TerminalPage::_GetTabImpl(const TerminalApp::Tab& tab)
    {
        winrt::com_ptr<Tab> tabImpl;
        tabImpl.copy_from(winrt::get_self<Tab>(tab));
        return tabImpl;
    }

    // Method Description:
    // - Computes the delta for scrolling the tab's viewport.
    // Arguments:
    // - scrollDirection - direction (up / down) to scroll
    // - rowsToScroll - the number of rows to scroll
    // Return Value:
    // - delta - Signed delta, where a negative value means scrolling up.
    int TerminalPage::_ComputeScrollDelta(ScrollDirection scrollDirection, const uint32_t rowsToScroll)
    {
        return scrollDirection == ScrollUp ? -1 * rowsToScroll : rowsToScroll;
    }

    // Method Description:
    // - Reads system settings for scrolling (based on the step of the mouse scroll).
    // Upon failure fallbacks to default.
    // Return Value:
    // - The number of rows to scroll or a magic value of WHEEL_PAGESCROLL
    // indicating that we need to scroll an entire view height
    uint32_t TerminalPage::_ReadSystemRowsToScroll()
    {
        uint32_t systemRowsToScroll;
        if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &systemRowsToScroll, 0))
        {
            LOG_LAST_ERROR();

            // If SystemParametersInfoW fails, which it shouldn't, fall back to
            // Windows' default value.
            return DefaultRowsToScroll;
        }

        return systemRowsToScroll;
    }

    // Method Description:
    // - Displays a dialog stating the "Touch Keyboard and Handwriting Panel
    //   Service" is disabled.
    void TerminalPage::ShowKeyboardServiceWarning() const
    {
        if (!_IsMessageDismissed(InfoBarMessage::KeyboardServiceWarning))
        {
            if (const auto keyboardServiceWarningInfoBar = FindName(L"KeyboardServiceWarningInfoBar").try_as<MUX::Controls::InfoBar>())
            {
                keyboardServiceWarningInfoBar.IsOpen(true);
            }
        }
    }

    // Function Description:
    // - Helper function to get the OS-localized name for the "Touch Keyboard
    //   and Handwriting Panel Service". If we can't open up the service for any
    //   reason, then we'll just return the service's key, "TabletInputService".
    // Return Value:
    // - The OS-localized name for the TabletInputService
    winrt::hstring _getTabletServiceName()
    {
        wil::unique_schandle hManager{ OpenSCManagerW(nullptr, nullptr, 0) };

        if (LOG_LAST_ERROR_IF(!hManager.is_valid()))
        {
            return winrt::hstring{ TabletInputServiceKey };
        }

        DWORD cchBuffer = 0;
        const auto ok = GetServiceDisplayNameW(hManager.get(), TabletInputServiceKey.data(), nullptr, &cchBuffer);

        // Windows 11 doesn't have a TabletInputService.
        // (It was renamed to TextInputManagementService, because people kept thinking that a
        // service called "tablet-something" is system-irrelevant on PCs and can be disabled.)
        if (ok || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            return winrt::hstring{ TabletInputServiceKey };
        }

        std::wstring buffer;
        cchBuffer += 1; // Add space for a null
        buffer.resize(cchBuffer);

        if (LOG_LAST_ERROR_IF(!GetServiceDisplayNameW(hManager.get(),
                                                      TabletInputServiceKey.data(),
                                                      buffer.data(),
                                                      &cchBuffer)))
        {
            return winrt::hstring{ TabletInputServiceKey };
        }
        return winrt::hstring{ buffer };
    }

    // Method Description:
    // - Return the fully-formed warning message for the
    //   "KeyboardServiceDisabled" InfoBar. This InfoBar is used to warn the user
    //   if the keyboard service is disabled, and uses the OS localization for
    //   the service's actual name. It's bound to the bar in XAML.
    // Return Value:
    // - The warning message, including the OS-localized service name.
    winrt::hstring TerminalPage::KeyboardServiceDisabledText()
    {
        const auto serviceName{ _getTabletServiceName() };
        const auto text{ RS_fmt(L"KeyboardServiceWarningText", serviceName) };
        return winrt::hstring{ text };
    }

    // Method Description:
    // - Update the RequestedTheme of the specified FrameworkElement and all its
    //   Parent elements. We need to do this so that we can actually theme all
    //   of the elements of the TeachingTip. See GH#9717
    // Arguments:
    // - element: The TeachingTip to set the theme on.
    // Return Value:
    // - <none>
    void TerminalPage::_UpdateTeachingTipTheme(winrt::Windows::UI::Xaml::FrameworkElement element)
    {
        auto theme{ _settings.GlobalSettings().CurrentTheme() };
        auto requestedTheme{ theme.RequestedTheme() };
        while (element)
        {
            element.RequestedTheme(requestedTheme);
            element = element.Parent().try_as<winrt::Windows::UI::Xaml::FrameworkElement>();
        }
    }

    // Method Description:
    // - Display the name and ID of this window in a TeachingTip. If the window
    //   has no name, the name will be presented as "<unnamed-window>".
    // - This can be invoked by either:
    //   * An identifyWindow action, that displays the info only for the current
    //     window
    //   * An identifyWindows action, that displays the info for all windows.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::IdentifyWindow()
    {
        // If we haven't ever loaded the TeachingTip, then do so now and
        // create the toast for it.
        if (_windowIdToast == nullptr)
        {
            if (auto tip{ FindName(L"WindowIdToast").try_as<MUX::Controls::TeachingTip>() })
            {
                _windowIdToast = std::make_shared<Toast>(tip);
                // IsLightDismissEnabled == true is bugged and poorly interacts with multi-windowing.
                // It causes the tip to be immediately dismissed when another tip is opened in another window.
                tip.IsLightDismissEnabled(false);
                // Make sure to use the weak ref when setting up this callback.
                tip.Closed({ get_weak(), &TerminalPage::_FocusActiveControl });
            }
        }
        _UpdateTeachingTipTheme(WindowIdToast().try_as<winrt::Windows::UI::Xaml::FrameworkElement>());

        if (_windowIdToast != nullptr)
        {
            _windowIdToast->Open();
        }
    }

    void TerminalPage::ShowTerminalWorkingDirectory()
    {
        // If we haven't ever loaded the TeachingTip, then do so now and
        // create the toast for it.
        if (_windowCwdToast == nullptr)
        {
            if (auto tip{ FindName(L"WindowCwdToast").try_as<MUX::Controls::TeachingTip>() })
            {
                _windowCwdToast = std::make_shared<Toast>(tip);
                // Make sure to use the weak ref when setting up this
                // callback.
                tip.Closed({ get_weak(), &TerminalPage::_FocusActiveControl });
            }
        }
        _UpdateTeachingTipTheme(WindowCwdToast().try_as<winrt::Windows::UI::Xaml::FrameworkElement>());

        if (_windowCwdToast != nullptr)
        {
            _windowCwdToast->Open();
        }
    }

    // Method Description:
    // - Called when the user hits the "Ok" button on the WindowRenamer TeachingTip.
    // - Will raise an event that will bubble up to the monarch, asking if this
    //   name is acceptable.
    //   - we'll eventually get called back in TerminalPage::WindowName(hstring).
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void TerminalPage::_WindowRenamerActionClick(const IInspectable& /*sender*/,
                                                 const IInspectable& /*eventArgs*/)
    {
        auto newName = WindowRenamerTextBox().Text();
        _RequestWindowRename(newName);
    }

    void TerminalPage::_RequestWindowRename(const winrt::hstring& newName)
    {
        auto request = winrt::make<implementation::RenameWindowRequestedArgs>(newName);
        // The WindowRenamer is _not_ a Toast - we want it to stay open until
        // the user dismisses it.
        if (WindowRenamer())
        {
            WindowRenamer().IsOpen(false);
        }
        RenameWindowRequested.raise(*this, request);
        // We can't just use request.Successful here, because the handler might
        // (will) be handling this asynchronously, so when control returns to
        // us, this hasn't actually been handled yet. We'll get called back in
        // RenameFailed if this fails.
        //
        // Theoretically we could do a IAsyncOperation<RenameWindowResult> kind
        // of thing with co_return winrt::make<RenameWindowResult>(false).
    }

    // Method Description:
    // - Used to track if the user pressed enter with the renamer open. If we
    //   immediately focus it after hitting Enter on the command palette, then
    //   the Enter keydown will dismiss the command palette and open the
    //   renamer, and then the enter keyup will go to the renamer. So we need to
    //   make sure both a down and up go to the renamer.
    // Arguments:
    // - e: the KeyRoutedEventArgs describing the key that was released
    // Return Value:
    // - <none>
    void TerminalPage::_WindowRenamerKeyDown(const IInspectable& /*sender*/,
                                             const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e)
    {
        const auto key = e.OriginalKey();
        if (key == Windows::System::VirtualKey::Enter)
        {
            _renamerPressedEnter = true;
        }
    }

    // Method Description:
    // - Manually handle Enter and Escape for committing and dismissing a window
    //   rename. This is highly similar to the TabHeaderControl's KeyUp handler.
    // Arguments:
    // - e: the KeyRoutedEventArgs describing the key that was released
    // Return Value:
    // - <none>
    void TerminalPage::_WindowRenamerKeyUp(const IInspectable& sender,
                                           const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e)
    {
        const auto key = e.OriginalKey();
        if (key == Windows::System::VirtualKey::Enter && _renamerPressedEnter)
        {
            // User is done making changes, close the rename box
            _WindowRenamerActionClick(sender, nullptr);
        }
        else if (key == Windows::System::VirtualKey::Escape)
        {
            // User wants to discard the changes they made
            WindowRenamerTextBox().Text(_WindowProperties.WindowName());
            WindowRenamer().IsOpen(false);
            _renamerPressedEnter = false;
        }
    }

    // Method Description:
    // - This function stops people from duplicating the base profile, because
    //   it gets ~ ~ weird ~ ~ when they do. Remove when TODO GH#5047 is done.
    Profile TerminalPage::GetClosestProfileForDuplicationOfProfile(const Profile& profile) const noexcept
    {
        if (profile == _settings.ProfileDefaults())
        {
            return _settings.FindProfile(_settings.GlobalSettings().DefaultProfile());
        }
        return profile;
    }

    // Function Description:
    // - Helper to launch a new WT instance elevated. It'll do this by spawning
    //   a helper process, that will ask the shell to elevate the process for
    //   us. This might cause a UAC prompt. The elevation is performed on a
    //   background thread, as to not block the UI thread.
    // Arguments:
    // - newTerminalArgs: A NewTerminalArgs describing the terminal instance
    //   that should be spawned. The Profile should be filled in with the GUID
    //   of the profile we want to launch.
    // Return Value:
    // - <none>
    // Important: Don't take the param by reference, since we'll be doing work
    // on another thread.
    void TerminalPage::_OpenElevatedWT(NewTerminalArgs newTerminalArgs)
    {
        // BODGY
        //
        // We're going to construct the commandline we want, then toss it to a
        // helper process called `elevate-shim.exe` that happens to live next to
        // us. elevate-shim.exe will be the one to call ShellExecute with the
        // args that we want (to elevate the given profile).
        //
        // We can't be the one to call ShellExecute ourselves. ShellExecute
        // requires that the calling process stays alive until the child is
        // spawned. However, in the case of something like `wt -p
        // AlwaysElevateMe`, then the original WT will try to ShellExecute a new
        // wt.exe (elevated) and immediately exit, preventing ShellExecute from
        // successfully spawning the elevated WT.

        std::filesystem::path exePath = wil::GetModuleFileNameW<std::wstring>(nullptr);
        exePath.replace_filename(L"elevate-shim.exe");

        // Build the commandline to pass to wt for this set of NewTerminalArgs
        auto cmdline{
            fmt::format(FMT_COMPILE(L"new-tab {}"), newTerminalArgs.ToCommandline())
        };

        wil::unique_process_information pi;
        STARTUPINFOW si{};
        si.cb = sizeof(si);

        LOG_IF_WIN32_BOOL_FALSE(CreateProcessW(exePath.c_str(),
                                               cmdline.data(),
                                               nullptr,
                                               nullptr,
                                               FALSE,
                                               0,
                                               nullptr,
                                               nullptr,
                                               &si,
                                               &pi));

        // TODO: GH#8592 - It may be useful to pop a Toast here in the original
        // Terminal window informing the user that the tab was opened in a new
        // window.
    }

    // Method Description:
    // - If the requested settings want us to elevate this new terminal
    //   instance, and we're not currently elevated, then open the new terminal
    //   as an elevated instance (using _OpenElevatedWT). Does nothing if we're
    //   already elevated, or if the control settings don't want to be elevated.
    // Arguments:
    // - newTerminalArgs: The NewTerminalArgs for this terminal instance
    // - controlSettings: The constructed TerminalSettingsCreateResult for this Terminal instance
    // - profile: The Profile we're using to launch this Terminal instance
    // Return Value:
    // - true iff we tossed this request to an elevated window. Callers can use
    //   this result to early-return if needed.
    bool TerminalPage::_maybeElevate(const NewTerminalArgs& newTerminalArgs,
                                     const Settings::TerminalSettingsCreateResult& controlSettings,
                                     const Profile& profile)
    {
        // When duplicating a tab there aren't any newTerminalArgs.
        if (!newTerminalArgs)
        {
            return false;
        }

        const auto defaultSettings = controlSettings.DefaultSettings();

        // If we don't even want to elevate we can return early.
        // If we're already elevated we can also return, because it doesn't get any more elevated than that.
        if (!defaultSettings->Elevate() || IsRunningElevated())
        {
            return false;
        }

        // Manually set the Profile of the NewTerminalArgs to the guid we've
        // resolved to. If there was a profile in the NewTerminalArgs, this
        // will be that profile's GUID. If there wasn't, then we'll use
        // whatever the default profile's GUID is.
        newTerminalArgs.Profile(::Microsoft::Console::Utils::GuidToString(profile.Guid()));
        newTerminalArgs.StartingDirectory(_evaluatePathForCwd(defaultSettings->StartingDirectory()));
        _OpenElevatedWT(newTerminalArgs);
        return true;
    }

    // Method Description:
    // - Handles the change of connection state.
    // If the connection state is failure show information bar suggesting to configure termination behavior
    // (unless user asked not to show this message again)
    // Arguments:
    // - sender: the ICoreState instance containing the connection state
    // Return Value:
    // - <none>
    safe_void_coroutine TerminalPage::_ConnectionStateChangedHandler(const IInspectable& sender, const IInspectable& /*args*/)
    {
        if (const auto coreState{ sender.try_as<winrt::Microsoft::Terminal::Control::ICoreState>() })
        {
            const auto newConnectionState = coreState.ConnectionState();
            const auto weak = get_weak();
            co_await wil::resume_foreground(Dispatcher());
            const auto strong = weak.get();
            if (!strong)
            {
                co_return;
            }

            _adjustProcessPriorityThrottled->Run();

            if (newConnectionState == ConnectionState::Failed && !_IsMessageDismissed(InfoBarMessage::CloseOnExitInfo))
            {
                if (const auto infoBar = FindName(L"CloseOnExitInfoBar").try_as<MUX::Controls::InfoBar>())
                {
                    infoBar.IsOpen(true);
                }
            }
        }
    }

    // Method Description:
    // - Persists the user's choice not to show information bar guiding to configure termination behavior.
    // Then hides this information buffer.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_CloseOnExitInfoDismissHandler(const IInspectable& /*sender*/, const IInspectable& /*args*/) const
    {
        _DismissMessage(InfoBarMessage::CloseOnExitInfo);
        if (const auto infoBar = FindName(L"CloseOnExitInfoBar").try_as<MUX::Controls::InfoBar>())
        {
            infoBar.IsOpen(false);
        }
    }

    // Method Description:
    // - Persists the user's choice not to show information bar warning about "Touch keyboard and Handwriting Panel Service" disabled
    // Then hides this information buffer.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_KeyboardServiceWarningInfoDismissHandler(const IInspectable& /*sender*/, const IInspectable& /*args*/) const
    {
        _DismissMessage(InfoBarMessage::KeyboardServiceWarning);
        if (const auto infoBar = FindName(L"KeyboardServiceWarningInfoBar").try_as<MUX::Controls::InfoBar>())
        {
            infoBar.IsOpen(false);
        }
    }

    // The corruption-recovery InfoBar is a transient, one-shot surface: it
    // fires when ApplicationState quarantined a malformed state.json on
    // startup, and is acknowledged on dismiss so it never reappears for the
    // same recovery. Unlike CloseOnExit/KeyboardService, we do NOT plumb
    // through DismissedMessages — the underlying flag is per-recovery, not
    // per-user-preference.
    void TerminalPage::_ShowCorruptStateRecoveredInfoBarIfNeeded() const
    {
        if (!ApplicationState::SharedInstance().RecoveredFromCorruption())
        {
            return;
        }
        if (const auto infoBar = FindName(L"CorruptStateRecoveredInfoBar").try_as<MUX::Controls::InfoBar>())
        {
            infoBar.IsOpen(true);
        }
    }

    void TerminalPage::_CorruptStateRecoveredInfoDismissHandler(const IInspectable& /*sender*/, const IInspectable& /*args*/) const
    {
        ApplicationState::SharedInstance().AcknowledgeCorruptionRecovery();
        if (const auto infoBar = FindName(L"CorruptStateRecoveredInfoBar").try_as<MUX::Controls::InfoBar>())
        {
            infoBar.IsOpen(false);
        }
    }

    // Splitter drag handler: clamps the sidebar column to a usable range and
    // applies the per-event horizontal delta from the Thumb.
    void TerminalPage::_WorkspaceSplitterDragDelta(const IInspectable& /*sender*/,
                                                   const Windows::UI::Xaml::Controls::Primitives::DragDeltaEventArgs& args)
    {
        const auto current = WorkspaceSidebarColumn().Width().Value;
        const auto next = current + args.HorizontalChange();
        WorkspaceSidebarColumn().Width(GridLengthHelper::FromPixels(next));
        _ClampWorkspaceSidebarWidth();
    }

    // Keep the sidebar width within a usable range relative to the wrapper
    // grid's actual width. Without this, a saved-too-wide sidebar (or a window
    // shrunk smaller than the sidebar+splitter) would push the splitter past
    // the visible content edge and steal pointer events meant for the window
    // chrome.
    void TerminalPage::_ClampWorkspaceSidebarWidth()
    {
        constexpr double kMinSidebarWidth = 120.0;
        constexpr double kMaxSidebarWidth = 600.0;
        // Never let sidebar+splitter exceed this fraction of the wrapping grid
        // — leaves room for at least one usable terminal pane.
        constexpr double kMaxFractionOfWrapper = 0.6;
        constexpr double kSplitterWidth = 4.0;

        const auto wrapperWidth = WorkspaceContentRoot().ActualWidth();
        const auto fractionalCap = std::max(kMinSidebarWidth, wrapperWidth * kMaxFractionOfWrapper - kSplitterWidth);
        const auto effectiveCap = std::min(kMaxSidebarWidth, fractionalCap);

        const auto current = WorkspaceSidebarColumn().Width().Value;
        const auto clamped = std::clamp(current, kMinSidebarWidth, effectiveCap);
        if (clamped != current)
        {
            WorkspaceSidebarColumn().Width(GridLengthHelper::FromPixels(clamped));
        }
    }

    // Seed `_workspaces` with one workspace so that brand-new windows in
    // workspaces mode start with a single entry instead of an empty list.
    // The workspace's title mirrors the default profile's name; its paneTree
    // carries a single `newTab` action with the default profile so future
    // mount/unmount steps can replay it. Live tab/pane creation still flows
    // through the normal startup-actions path — `_workspaces` is parallel
    // dormant state until the sidebar mount wiring lands.
    //
    // Cascade-endpoint contract (spec §Reliability, line 196): this is also
    // the fallback the app reaches on persisted-state corruption recovery.
    // On corruption, ApplicationState quarantines `state.json` to
    // `state.json.corrupt.<stamp>` and reports `RecoveredFromCorruption() ==
    // true` with empty `PersistedWindowLayouts()`. `LoadPersistedLayout()`
    // then returns null (no TabLayout to restore), so `_workspaces` is
    // freshly empty by the time we reach here and the early-out below does
    // not trigger. Future persistence-restore code MUST short-circuit to
    // this path when `RecoveredFromCorruption()` is true (or whenever its
    // own deserialization fails) — i.e., never call this if a partial
    // restore populated `_workspaces`, and never skip it after a failed
    // restore. The InfoBar surfaced by `_ShowCorruptStateRecoveredInfoBarIf
    // Needed` is the user-visible signal that this fallback fired.
    void TerminalPage::_BootstrapDefaultWorkspace()
    {
        if (!_workspaces || _workspaces->Count() != 0)
        {
            return;
        }

        ::TerminalApp::WorkspaceState ws;
        ws.id = 1;
        ws.pinned = false;

        const auto defaultProfileGuid = _settings.GlobalSettings().DefaultProfile();
        if (const auto profile = _settings.FindProfile(defaultProfileGuid))
        {
            ws.title = profile.Name();
        }

        Json::Value newTab{ Json::objectValue };
        newTab["action"] = "newTab";
        newTab["profile"] = winrt::to_string(::Microsoft::Console::Utils::GuidToString(defaultProfileGuid));
        ws.paneTree = Json::Value{ Json::arrayValue };
        ws.paneTree.append(std::move(newTab));

        _workspaces->Insert(std::move(ws),
                            winrt::Microsoft::Terminal::Settings::Model::WorkspacePlacementPolicy::End,
                            true);
    }

    // Drain `_persistedWorkspaceState` into `_workspaces`. Called once from
    // `Create()` when TerminalWindow successfully parsed the persisted
    // `workspaceLayout` blob. Each restored workspace carries forward its
    // title, runtime color, custom description, pinned state, and full
    // paneTree (which `_MaterializeWorkspace` + `_ReplayWorkspacePaneTree`
    // will replay on activation). The persisted active index, sidebar
    // width, and pin/unpin partition are preserved bit-for-bit via
    // `WorkspaceList::FromState` so insert ordering can't drift workspaces
    // across the pin region. After hydration we reset
    // `_persistedWorkspaceState` to release the Json::Value memory (paneTree
    // is the heaviest field).
    //
    // **Workspace ids are NOT preserved.** They're a per-session monotonic
    // counter used as the key into `_workspaceTabs` and the lookup id for
    // `_ReplayWorkspacePaneTree`. The migration path
    // (`WorkspaceMigration::MigrateLegacyTabLayout`) emits `newWorkspace`
    // headers without an `id` field, so restored workspaces would all share
    // `id == 0` — `_workspaceTabs` would alias every workspace onto a single
    // map slot, and `_SwitchToWorkspace` / `_ReplayWorkspacePaneTree` would
    // route every row to the first-restored tab. We re-assign each restored
    // workspace a fresh id from `_nextWorkspaceId` to keep keys unique
    // within the live session.
    void TerminalPage::_HydrateWorkspacesFromPersistedState()
    {
        if (!_workspaces || !_persistedWorkspaceState.has_value())
        {
            return;
        }

        auto state = std::move(*_persistedWorkspaceState);
        _persistedWorkspaceState.reset();

        if (state.workspaces.empty())
        {
            return;
        }

        // Honor the persisted sidebar width when TerminalWindow didn't
        // already supply one from the same WindowLayout (it does today;
        // this is a defense-in-depth path).
        if (state.sidebarWidth.has_value() && !_persistedSidebarWidth.has_value())
        {
            _persistedSidebarWidth = *state.sidebarWidth;
        }

        // Re-assign session-unique ids to every restored workspace, then
        // bump `_nextWorkspaceId` past the last one. See comment above.
        for (auto& ws : state.workspaces)
        {
            ws.id = _nextWorkspaceId++;
        }

        *_workspaces = ::TerminalApp::WorkspaceList::FromState(std::move(state));
    }

    // Imperatively rebuild the sidebar's row visuals from `_workspaces`. This
    // is the read-only render path for slice 1 step #2 — `_workspaces` doesn't
    // mutate after Create() yet, so a one-shot rebuild on construction is
    // sufficient. When mount/unmount and click-to-switch land we'll either
    // re-call this on activation, or project a snapshotted
    // IObservableVector<WorkspaceRowViewModel> and let XAML diff for us.
    void TerminalPage::_RebuildWorkspaceSidebar()
    {
        if (!_workspaces)
        {
            return;
        }
        auto host = WorkspaceRowsHost();
        if (!host)
        {
            return;
        }

        host.Children().Clear();

        const auto activeIndex = _workspaces->ActiveIndex();
        const auto count = _workspaces->Count();

        const auto resources = Application::Current().Resources();
        Media::Brush activeBackground{ nullptr };
        if (const auto key = winrt::box_value(L"TabViewItemHeaderBackgroundSelected"); resources.HasKey(key))
        {
            activeBackground = resources.Lookup(key).try_as<Media::Brush>();
        }

        for (size_t i = 0; i < count; ++i)
        {
            const auto& ws = _workspaces->At(i);
            const bool isActive = activeIndex.has_value() && *activeIndex == i;

            // Two-column layout: a 3px reservation for the runtime-color
            // stripe (left blank when ws.runtimeColor is unset, per spec) and
            // the row's main content. Reserving the column even when blank
            // avoids reflow when a workspace later gains a color.
            Grid rowGrid;
            ColumnDefinition stripeCol;
            stripeCol.Width(GridLengthHelper::FromPixels(3.0));
            ColumnDefinition contentCol;
            contentCol.Width(GridLengthHelper::FromValueAndType(1.0, GridUnitType::Star));
            rowGrid.ColumnDefinitions().Append(stripeCol);
            rowGrid.ColumnDefinitions().Append(contentCol);

            if (ws.runtimeColor.has_value())
            {
                Shapes::Rectangle stripe;
                stripe.Fill(Media::SolidColorBrush{ *ws.runtimeColor });
                Grid::SetColumn(stripe, 0);
                rowGrid.Children().Append(stripe);
            }

            TextBlock title;
            title.Text(winrt::hstring{ ws.title });
            title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            title.TextTrimming(TextTrimming::CharacterEllipsis);
            title.Margin(ThicknessHelper::FromLengths(10.0, 8.0, 8.0, 8.0));
            Grid::SetColumn(title, 1);
            rowGrid.Children().Append(title);

            Border row;
            row.Child(rowGrid);
            if (isActive && activeBackground)
            {
                row.Background(activeBackground);
            }
            // Defer the switch to the next dispatcher tick. _SwitchToWorkspace
            // sets _tabView.SelectedItem, which synchronously fires
            // SelectionChanged → _UpdatedSelectedTab → mutations of
            // _tabContent.Children. Doing that from inside the Tapped
            // handler crashed WinUI's input dispatch (0xC0000005 in
            // Windows.UI.Xaml.dll's post-input bookkeeping).
            row.Tapped([weakThis = get_weak(), i](auto&&, auto&&) {
                if (auto self = weakThis.get())
                {
                    self->Dispatcher().RunAsync(
                        Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [weakThis, i]() {
                            if (auto inner = weakThis.get())
                            {
                                inner->_SwitchToWorkspace(i);
                            }
                        });
                }
            });

            host.Children().Append(row);
        }
    }

    // Post a sidebar rebuild to the dispatcher so it runs after the current
    // event handler unwinds. Direct `_RebuildWorkspaceSidebar()` calls from
    // inside an event handler attached to a row Border (or the "+" button)
    // would destroy that Border before WinUI finished dispatching the
    // originating Tapped/Click event, producing a 0xC0000005 access violation
    // deep in the input pipeline as it tried to post-process the detached
    // element. Deferral lets the handler return cleanly first.
    void TerminalPage::_QueueSidebarRebuild()
    {
        Dispatcher().RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
                              [weakThis = get_weak()]() {
                                  if (auto self = weakThis.get())
                                  {
                                      self->_RebuildWorkspaceSidebar();
                                  }
                              });
    }

    // Bring the workspace at `index` to "live" state: build a Pane from its
    // first paneTree action's profile, append a Tab to `_tabs` end-positioned
    // (so we can deterministically retrieve it), and record it in
    // `_workspaceTabs`. `_CreateNewTabFromPane` synchronously appends to
    // `_tabs` and selects the new TabViewItem, which fires the existing
    // `_OnTabSelectionChanged` → `_UpdatedSelectedTab` chain — that's the
    // path that mounts content into `_tabContent`. Reused as-is so ConPTYs
    // get the same survives-unmount guarantee tabs already enjoy: the Tab
    // stays alive in `_tabs` (and in this map) when its UIElement is removed
    // from the visual tree on switch-away.
    void TerminalPage::_MaterializeWorkspace(size_t index)
    {
        if (!_workspaces || index >= _workspaces->Count())
        {
            return;
        }
        auto& ws = _workspaces->At(index);
        if (const auto it = _workspaceTabs.find(ws.id); it != _workspaceTabs.end() && it->second)
        {
            return;
        }

        Microsoft::Terminal::Settings::Model::NewTerminalArgs termArgs{};
        if (ws.paneTree.isArray() && !ws.paneTree.empty() && ws.paneTree[0u].isObject())
        {
            const auto& action = ws.paneTree[0u];
            if (action.isMember("profile") && action["profile"].isString())
            {
                termArgs.Profile(winrt::to_hstring(action["profile"].asString()));
            }
            if (action.isMember("startingDirectory") && action["startingDirectory"].isString())
            {
                termArgs.StartingDirectory(winrt::to_hstring(action["startingDirectory"].asString()));
            }
        }

        auto pane = _MakePane(termArgs, nullptr);
        if (!pane)
        {
            return;
        }

        // Force end-of-list insertion so we can pull the tab back out of
        // `_tabs` deterministically; the user's NewTabPosition setting
        // doesn't apply to materialization.
        const auto insertPosition = _tabs.Size();
        auto tab = _CreateNewTabFromPane(pane, insertPosition);
        _workspaceTabs.insert_or_assign(ws.id, tab);

        // Replay paneTree[1..n] (split / focus actions) onto the just-created
        // tab. This is the "preserves the pane tree" half of the persistence
        // contract: action[0] is the workspace's seed `newTab` (already
        // applied via _MakePane above); everything after it splits or
        // focuses against the tab we just selected. Replay is async so it
        // can yield to WinUI between actions (otherwise splitPane racks up
        // before the previous pane's TermControl gets a layout pass — the
        // GH#13136 race that ProcessStartupActions also paces around).
        // Replay-once invariant: this is the only path that creates a
        // workspace's tab, so the early-out above guarantees we never
        // double-replay when the user clicks back to a materialized
        // workspace.
        _ReplayWorkspacePaneTree(ws.id);
    }

    // Replay paneTree[1..n] for `workspaceId`. Action[0] (the seed `newTab`)
    // was applied synchronously by `_MaterializeWorkspace` via `_MakePane` +
    // `_CreateNewTabFromPane`; everything else (splitPane, focusPane, …)
    // dispatches against the now-active tab through the same
    // `_actionDispatch->DoAction` path that user-driven splits use, with
    // `wil::resume_foreground` between actions to let WinUI layout each
    // intermediate pane. If the user switches workspaces mid-replay, the
    // remaining actions still target the old tab via `_actionDispatch` (it
    // routes by `_GetFocusedTab` at dispatch time) — that's a known sharp
    // edge worth flagging if it bites; for slice 1, splits land on whatever
    // tab is focused at each await resumption.
    safe_void_coroutine TerminalPage::_ReplayWorkspacePaneTree(uint64_t workspaceId)
    {
        const auto strong = get_strong();

        // Snapshot the paneTree synchronously so concurrent mutations to
        // `_workspaces` (theoretical: nothing mutates paneTree post-create
        // today) can't tear the action stream we're about to replay.
        Json::Value paneTree;
        if (_workspaces)
        {
            for (size_t i = 0; i < _workspaces->Count(); ++i)
            {
                const auto& ws = _workspaces->At(i);
                if (ws.id == workspaceId)
                {
                    paneTree = ws.paneTree;
                    break;
                }
            }
        }
        if (!paneTree.isArray() || paneTree.size() <= 1)
        {
            co_return;
        }

        // Convert paneTree[1..n] to typed `ActionAndArgs` projections via
        // the public `ActionAndArgs::Deserialize(hstring)` API. That path
        // expects `{"actions": [...]}`, so we wrap. Doing this in one
        // round-trip avoids an additional cross-project header include for
        // the impl-side `FromJson` helper.
        Json::Value wrapper{ Json::objectValue };
        Json::Value actionsArray{ Json::arrayValue };
        for (Json::ArrayIndex i = 1; i < paneTree.size(); ++i)
        {
            if (paneTree[i].isObject())
            {
                actionsArray.append(paneTree[i]);
            }
        }
        wrapper["actions"] = std::move(actionsArray);

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        const auto wrapperString = Json::writeString(wb, wrapper);

        winrt::Windows::Foundation::Collections::IVector<Microsoft::Terminal::Settings::Model::ActionAndArgs> actions{ nullptr };
        try
        {
            actions = Microsoft::Terminal::Settings::Model::ActionAndArgs::Deserialize(winrt::to_hstring(wrapperString));
        }
        catch (...)
        {
            // Malformed paneTree action entries — skip replay, the
            // workspace's seed pane is still alive so the user has
            // *something* usable in this workspace.
            co_return;
        }

        if (!actions || actions.Size() == 0)
        {
            co_return;
        }

        for (const auto& action : actions)
        {
            if (!action)
            {
                continue;
            }
            co_await wil::resume_foreground(Dispatcher(), winrt::Windows::UI::Core::CoreDispatcherPriority::Low);
            if (!_actionDispatch)
            {
                co_return;
            }
            _actionDispatch->DoAction(action);
        }
    }

    // Drive the active-workspace change. Activation is a no-op in
    // `WorkspaceList` if `index` is already active, so re-clicking the
    // active row costs only a sidebar rebuild.
    void TerminalPage::_SwitchToWorkspace(size_t index)
    {
        if (!_workspaces || index >= _workspaces->Count())
        {
            return;
        }
        if (_workspaces->ActiveIndex() == index)
        {
            return;
        }

        _workspaces->Activate(index);
        _MaterializeWorkspace(index);

        const auto& ws = _workspaces->At(index);
        if (const auto it = _workspaceTabs.find(ws.id); it != _workspaceTabs.end() && it->second)
        {
            // Selecting the TabViewItem fires `_OnTabSelectionChanged`,
            // which calls `_UpdatedSelectedTab` to clear and re-attach
            // `_tabContent.Children`. The reverse-sync hook in
            // `_OnTabSelectionChanged` will see the activeIndex is already
            // correct and just re-render the sidebar.
            _tabView.SelectedItem(it->second.TabViewItem());
        }

        _RebuildWorkspaceSidebar();
        _UpdateWorkspaceTitleInChrome();
    }

    // Synchronously build a new workspace running `profileGuid`, append it
    // to `_workspaces` per the user's newWorkspacePlacement, materialize
    // its content, and bring focus. Callers must invoke this from the
    // dispatcher (not directly from a Click/Tapped handler) — materialize
    // mutates the visual tree, and doing that inside an input-dispatch
    // frame crashes WinUI in `Windows.UI.Xaml.dll`'s post-input bookkeeping.
    void TerminalPage::_CreateNewWorkspaceWithProfile(const winrt::guid& profileGuid)
    {
        if (!_workspaces)
        {
            return;
        }

        const auto profile = _settings.FindProfile(profileGuid);

        // Inherit the previously-active workspace's focused-pane CWD when
        // available; fall back to the profile's evaluated startingDirectory.
        // CWD sources tried in order:
        //   1. TermControl::WorkingDirectory() — but only if it differs from
        //      the profile's pre-seeded starting directory. ControlCore
        //      pre-seeds the field at construction, so the same value comes
        //      back forever for shells that don't emit OSC 7 / OSC 9;9.
        //   2. ConptyConnection::ClientWorkingDirectory() — best-effort PEB
        //      introspection. Works for shells whose builtin `cd` calls
        //      SetCurrentDirectoryW (cmd.exe, git bash, nushell, etc.).
        //   3. profile.EvaluatedStartingDirectory().
        std::wstring cwd;
        if (const auto activeIndex = _workspaces->ActiveIndex())
        {
            const auto& activeWs = _workspaces->At(*activeIndex);
            if (const auto it = _workspaceTabs.find(activeWs.id); it != _workspaceTabs.end() && it->second)
            {
                if (const auto& tabImpl = _GetTabImpl(it->second))
                {
                    if (const auto& termControl = tabImpl->GetActiveTerminalControl())
                    {
                        const auto wd = termControl.WorkingDirectory();

                        // Determine whether `wd` is just the unmodified
                        // pre-seed. Compare against the focused profile's
                        // resolved StartingDirectory, normalizing trailing
                        // backslashes so `C:\foo` and `C:\foo\` match.
                        bool wdLooksShellReported = false;
                        if (!wd.empty())
                        {
                            const auto activeProfile = tabImpl->GetFocusedProfile();
                            if (activeProfile)
                            {
                                const auto seeded = _evaluatePathForCwd(activeProfile.EvaluatedStartingDirectory());
                                auto trim = [](std::wstring_view s) {
                                    while (s.size() > 1 && s.back() == L'\\')
                                    {
                                        s.remove_suffix(1);
                                    }
                                    return s;
                                };
                                const auto a = trim(std::wstring_view{ wd });
                                const auto b = trim(std::wstring_view{ seeded });
                                const bool equal = (a.size() == b.size()) &&
                                                   (CompareStringOrdinal(a.data(),
                                                                         static_cast<int>(a.size()),
                                                                         b.data(),
                                                                         static_cast<int>(b.size()),
                                                                         TRUE) == CSTR_EQUAL);
                                wdLooksShellReported = !equal;
                            }
                            else
                            {
                                // No profile to compare against — trust the
                                // value if it's a real directory.
                                wdLooksShellReported = true;
                            }
                        }

                        if (wdLooksShellReported && Utils::IsValidDirectory(wd.c_str()))
                        {
                            cwd.assign(wd);
                        }

                        if (cwd.empty())
                        {
                            if (const auto conn = termControl.Connection())
                            {
                                if (const auto pty = conn.try_as<TerminalConnection::ConptyConnection>())
                                {
                                    const auto introspected = pty.ClientWorkingDirectory();
                                    if (Utils::IsValidDirectory(introspected.c_str()))
                                    {
                                        cwd.assign(introspected);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (cwd.empty() && profile)
        {
            cwd = _evaluatePathForCwd(profile.EvaluatedStartingDirectory());
        }

        // Title: basename(cwd), unless the cwd is the user's home directory
        // or the basename is empty — both fall back to profile.Name().
        winrt::hstring title;
        if (!cwd.empty())
        {
            std::wstring userHome;
            try
            {
                userHome = wil::ExpandEnvironmentStringsW<std::wstring>(L"%USERPROFILE%");
            }
            CATCH_LOG();

            const bool isUserHome = !userHome.empty() &&
                                    CompareStringOrdinal(cwd.c_str(), -1, userHome.c_str(), -1, TRUE) == CSTR_EQUAL;
            if (!isUserHome)
            {
                const auto basename = std::filesystem::path{ cwd }.filename().wstring();
                if (!basename.empty())
                {
                    title = winrt::hstring{ basename };
                }
            }
        }
        if (title.empty() && profile)
        {
            title = profile.Name();
        }

        ::TerminalApp::WorkspaceState ws;
        ws.id = _nextWorkspaceId++;
        ws.pinned = false;
        ws.title = title;

        Json::Value newTab{ Json::objectValue };
        newTab["action"] = "newTab";
        newTab["profile"] = winrt::to_string(::Microsoft::Console::Utils::GuidToString(profileGuid));
        if (!cwd.empty())
        {
            newTab["startingDirectory"] = winrt::to_string(cwd);
        }
        ws.paneTree = Json::Value{ Json::arrayValue };
        ws.paneTree.append(std::move(newTab));

        const auto policy = _settings.GlobalSettings().NewWorkspacePlacement();
        const auto inserted = _workspaces->Insert(std::move(ws), policy, /*activate*/ true);
        _MaterializeWorkspace(inserted);
        _RebuildWorkspaceSidebar();
        _UpdateWorkspaceTitleInChrome();
    }

    // Build the hamburger MenuFlyout and the "+ New Workspace" SplitButton's
    // primary-click + chevron flyout. Called once from Create() after
    // EnableWorkspacesMode flips TabRowControl into chrome mode.
    //
    // The chevron flyout reuses `_CreateNewTabFlyoutItems` with a workspace
    // leaf handler, so the recursive NewTabMenu picker (folders /
    // RemainingProfiles / MatchProfiles / Action entries) is shared with
    // the legacy new-tab dropdown.
    //
    // "Switch Workspace…" is included as a disabled MenuFlyoutItem because
    // it depends on the command palette's `@`-mode workspace switcher,
    // which is not yet built. "Manage Workspaces…" is omitted entirely
    // per spec ("deferred POC — hidden").
    void TerminalPage::_BuildWorkspaceChromeMenus()
    {
        if (!_tabRow)
        {
            return;
        }

        // Hamburger menu.
        if (const auto hamburger = _tabRow.HamburgerButton())
        {
            auto flyout = WUX::Controls::MenuFlyout{};
            flyout.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);

            auto switchItem = WUX::Controls::MenuFlyoutItem{};
            switchItem.Text(RS_(L"WorkspaceChromeSwitchMenuItem"));
            switchItem.IsEnabled(false);
            flyout.Items().Append(switchItem);

            auto closeAllItem = WUX::Controls::MenuFlyoutItem{};
            closeAllItem.Text(RS_(L"WorkspaceChromeCloseAllMenuItem"));
            closeAllItem.Click([weakThis = get_weak()](auto&&, auto&&) {
                if (auto self = weakThis.get())
                {
                    self->CloseWindow();
                }
            });
            flyout.Items().Append(closeAllItem);

            flyout.Items().Append(WUX::Controls::MenuFlyoutSeparator{});

            auto settingsItem = WUX::Controls::MenuFlyoutItem{};
            settingsItem.Text(RS_(L"SettingsMenuItem"));
            WUX::Controls::SymbolIcon settingsIcon{};
            settingsIcon.Symbol(WUX::Controls::Symbol::Setting);
            settingsItem.Icon(settingsIcon);
            settingsItem.Click({ this, &TerminalPage::_SettingsButtonOnClick });
            flyout.Items().Append(settingsItem);

            auto paletteItem = WUX::Controls::MenuFlyoutItem{};
            paletteItem.Text(RS_(L"CommandPaletteMenuItem"));
            WUX::Controls::FontIcon paletteIcon{};
            paletteIcon.Glyph(L"\xE945");
            paletteIcon.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            paletteItem.Icon(paletteIcon);
            paletteItem.Click({ this, &TerminalPage::_CommandPaletteButtonOnClick });
            flyout.Items().Append(paletteItem);

            auto aboutItem = WUX::Controls::MenuFlyoutItem{};
            aboutItem.Text(RS_(L"AboutMenuItem"));
            WUX::Controls::SymbolIcon aboutIcon{};
            aboutIcon.Symbol(WUX::Controls::Symbol::Help);
            aboutItem.Icon(aboutIcon);
            aboutItem.Click({ this, &TerminalPage::_AboutButtonOnClick });
            flyout.Items().Append(aboutItem);

            hamburger.Flyout(flyout);
        }

        // + New Workspace SplitButton.
        if (const auto newWsButton = _tabRow.NewWorkspaceButton())
        {
            // Primary-click: create a new workspace running the default profile.
            // Defer to the dispatcher so the body runs after WinUI finishes
            // dispatching the click — _MaterializeWorkspace mutates the visual
            // tree and would AV inside the input pipeline otherwise.
            newWsButton.Click([weakThis = get_weak()](auto&&, auto&&) {
                if (auto self = weakThis.get())
                {
                    self->Dispatcher().RunAsync(
                        Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [weakThis]() {
                            if (auto inner = weakThis.get())
                            {
                                const auto defaultGuid = inner->_settings.GlobalSettings().DefaultProfile();
                                inner->_CreateNewWorkspaceWithProfile(defaultGuid);
                            }
                        });
                }
            });

            // Chevron flyout: drive from the user's NewTabMenu so folders,
            // RemainingProfiles, MatchProfiles and Action entries all show
            // up the same way they do in the legacy new-tab dropdown. Profile
            // leaves create a workspace; action leaves dispatch the action
            // verbatim (matches legacy semantics — the action retargeting
            // for newTab → newPaneTab in workspace mode is slice 6's job).
            //
            // The leaf handler defers each create to the dispatcher so the
            // body runs after WinUI finishes input dispatch — same WinUI AV
            // trap as the primary-click path.
            auto flyout = WUX::Controls::MenuFlyout{};
            flyout.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);

            NewTabFlyoutLeafHandler workspaceLeafHandler = [weakThis = get_weak()](const NewTerminalArgs& args) {
                if (auto self = weakThis.get())
                {
                    self->Dispatcher().RunAsync(
                        Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [weakThis, args]() {
                            if (auto inner = weakThis.get())
                            {
                                inner->_CreateNewWorkspaceFromNewTerminalArgs(args);
                            }
                        });
                }
            };

            const auto entries = _settings.GlobalSettings().NewTabMenu();
            const auto items = _CreateNewTabFlyoutItems(entries, workspaceLeafHandler);
            for (const auto& item : items)
            {
                flyout.Items().Append(item);
            }
            newWsButton.Flyout(flyout);
        }
    }

    // Resolve the profile from `args` and forward to
    // `_CreateNewWorkspaceWithProfile`. Used by the workspace chrome chevron
    // when a profile leaf is clicked in the shared NewTabMenu picker.
    // Modifier-key elevation/window-routing logic from
    // `_OpenNewTerminalViaDropdown` is intentionally not applied here — the
    // chevron's purpose is "create a workspace using this profile", not
    // "open a new tab with these modifiers."
    void TerminalPage::_CreateNewWorkspaceFromNewTerminalArgs(const NewTerminalArgs& args)
    {
        if (!_workspaces)
        {
            return;
        }
        const auto profile = _settings.GetProfileForArgs(args);
        if (!profile)
        {
            return;
        }
        _CreateNewWorkspaceWithProfile(profile.Guid());
    }

    // Sync the chrome's centered title TextBlock to the active workspace's
    // title. Cheap (~one string copy) so we just call it from every site
    // that mutates `_workspaces` or active-index — Create(), _SwitchToWorkspace,
    // _CreateNewWorkspaceWithProfile, and the legacy-strip reverse-sync.
    void TerminalPage::_UpdateWorkspaceTitleInChrome()
    {
        if (!_tabRow || !_workspaces)
        {
            return;
        }
        const auto active = _workspaces->ActiveIndex();
        if (!active.has_value())
        {
            return;
        }
        if (const auto titleText = _tabRow.WorkspaceTitleText())
        {
            titleText.Text(winrt::hstring{ _workspaces->At(*active).title });
        }
    }

    // Method Description:
    // - Checks whether information bar message was dismissed earlier (in the application state)
    // Arguments:
    // - message: message to look for in the state
    // Return Value:
    // - true, if the message was dismissed
    bool TerminalPage::_IsMessageDismissed(const InfoBarMessage& message)
    {
        if (const auto dismissedMessages{ ApplicationState::SharedInstance().DismissedMessages() })
        {
            for (const auto& dismissedMessage : dismissedMessages)
            {
                if (dismissedMessage == message)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Method Description:
    // - Persists the user's choice to dismiss information bar message (in application state)
    // Arguments:
    // - message: message to dismiss
    // Return Value:
    // - <none>
    void TerminalPage::_DismissMessage(const InfoBarMessage& message)
    {
        const auto applicationState = ApplicationState::SharedInstance();
        std::vector<InfoBarMessage> messages;

        if (const auto values = applicationState.DismissedMessages())
        {
            messages.resize(values.Size());
            values.GetMany(0, messages);
        }

        if (std::none_of(messages.begin(), messages.end(), [&](const auto& m) { return m == message; }))
        {
            messages.emplace_back(message);
        }

        applicationState.DismissedMessages(std::move(messages));
    }

    void TerminalPage::_updateThemeColors()
    {
        if (_settings == nullptr)
        {
            return;
        }

        const auto theme = _settings.GlobalSettings().CurrentTheme();
        auto requestedTheme{ theme.RequestedTheme() };

        {
            _updatePaneResources(requestedTheme);

            for (const auto& tab : _tabs)
            {
                if (auto tabImpl{ _GetTabImpl(tab) })
                {
                    // The root pane will propagate the theme change to all its children.
                    if (const auto& rootPane{ tabImpl->GetRootPane() })
                    {
                        rootPane->UpdateResources(_paneResources);
                    }
                }
            }
        }

        const auto res = Application::Current().Resources();

        // Use our helper to lookup the theme-aware version of the resource.
        const auto tabViewBackgroundKey = winrt::box_value(L"TabViewBackground");
        const auto backgroundSolidBrush = ThemeLookup(res, requestedTheme, tabViewBackgroundKey).as<Media::SolidColorBrush>();

        til::color bgColor = backgroundSolidBrush.Color();

        Media::Brush terminalBrush{ nullptr };
        if (const auto tab{ _GetFocusedTabImpl() })
        {
            if (const auto& pane{ tab->GetActivePane() })
            {
                if (const auto& lastContent{ pane->GetLastFocusedContent() })
                {
                    terminalBrush = lastContent.BackgroundBrush();
                }
            }
        }

        // GH#19604: Get the theme's tabRow color to use as the acrylic tint.
        const auto tabRowBg{ theme.TabRow() ? (_activated ? theme.TabRow().Background() :
                                                            theme.TabRow().UnfocusedBackground()) :
                                              ThemeColor{ nullptr } };

        if (_settings.GlobalSettings().UseAcrylicInTabRow() && (_activated || _settings.GlobalSettings().EnableUnfocusedAcrylic()))
        {
            if (tabRowBg)
            {
                bgColor = ThemeColor::ColorFromBrush(tabRowBg.Evaluate(res, terminalBrush, true));
            }

            const auto acrylicBrush = Media::AcrylicBrush();
            acrylicBrush.BackgroundSource(Media::AcrylicBackgroundSource::HostBackdrop);
            acrylicBrush.FallbackColor(bgColor);
            acrylicBrush.TintColor(bgColor);
            acrylicBrush.TintOpacity(0.5);

            TitlebarBrush(acrylicBrush);
        }
        else if (tabRowBg)
        {
            const auto themeBrush{ tabRowBg.Evaluate(res, terminalBrush, true) };
            bgColor = ThemeColor::ColorFromBrush(themeBrush);
            // If the tab content returned nullptr for the terminalBrush, we
            // _don't_ want to use it as the tab row background. We want to just
            // use the default tab row background.
            TitlebarBrush(themeBrush ? themeBrush : backgroundSolidBrush);
        }
        else
        {
            // Nothing was set in the theme - fall back to our original `TabViewBackground` color.
            TitlebarBrush(backgroundSolidBrush);
        }

        if (!_settings.GlobalSettings().ShowTabsInTitlebar())
        {
            _tabRow.Background(TitlebarBrush());
        }

        // Second: Update the colors of our individual TabViewItems. This
        // applies tab.background to the tabs via Tab::ThemeColor.
        //
        // Do this second, so that we already know the bgColor of the titlebar.
        {
            const auto tabBackground = theme.Tab() ? theme.Tab().Background() : nullptr;
            const auto tabUnfocusedBackground = theme.Tab() ? theme.Tab().UnfocusedBackground() : nullptr;
            for (const auto& tab : _tabs)
            {
                winrt::com_ptr<Tab> tabImpl;
                tabImpl.copy_from(winrt::get_self<Tab>(tab));
                tabImpl->ThemeColor(tabBackground, tabUnfocusedBackground, bgColor);
            }
        }
        // Update the new tab button to have better contrast with the new color.
        // In theory, it would be convenient to also change these for the
        // inactive tabs as well, but we're leaving that as a follow up.
        _SetNewTabButtonColor(bgColor, bgColor);

        // Third: the window frame. This is basically the same logic as the tab row background.
        // We'll set our `FrameBrush` property, for the window to later use.
        const auto windowTheme{ theme.Window() };
        if (auto windowFrame{ windowTheme ? (_activated ? windowTheme.Frame() :
                                                          windowTheme.UnfocusedFrame()) :
                                            ThemeColor{ nullptr } })
        {
            const auto themeBrush{ windowFrame.Evaluate(res, terminalBrush, true) };
            FrameBrush(themeBrush);
        }
        else
        {
            // Nothing was set in the theme - fall back to null. The window will
            // use that as an indication to use the default window frame.
            FrameBrush(nullptr);
        }
    }

    // Function Description:
    // - Attempts to load some XAML resources that Panes will need. This includes:
    //   * The Color they'll use for active Panes's borders - SystemAccentColor
    //   * The Brush they'll use for inactive Panes - TabViewBackground (to match the
    //     color of the titlebar)
    // Arguments:
    // - requestedTheme: this should be the currently active Theme for the app
    // Return Value:
    // - <none>
    void TerminalPage::_updatePaneResources(const winrt::Windows::UI::Xaml::ElementTheme& requestedTheme)
    {
        const auto res = Application::Current().Resources();
        const auto accentColorKey = winrt::box_value(L"SystemAccentColor");
        if (res.HasKey(accentColorKey))
        {
            const auto colorFromResources = ThemeLookup(res, requestedTheme, accentColorKey);
            // If SystemAccentColor is _not_ a Color for some reason, use
            // Transparent as the color, so we don't do this process again on
            // the next pane (by leaving s_focusedBorderBrush nullptr)
            auto actualColor = winrt::unbox_value_or<Color>(colorFromResources, Colors::Black());
            _paneResources.focusedBorderBrush = SolidColorBrush(actualColor);
        }
        else
        {
            // DON'T use Transparent here - if it's "Transparent", then it won't
            // be able to hittest for clicks, and then clicking on the border
            // will eat focus.
            _paneResources.focusedBorderBrush = SolidColorBrush{ Colors::Black() };
        }

        const auto unfocusedBorderBrushKey = winrt::box_value(L"UnfocusedBorderBrush");
        if (res.HasKey(unfocusedBorderBrushKey))
        {
            // MAKE SURE TO USE ThemeLookup, so that we get the correct resource for
            // the requestedTheme, not just the value from the resources (which
            // might not respect the settings' requested theme)
            auto obj = ThemeLookup(res, requestedTheme, unfocusedBorderBrushKey);
            _paneResources.unfocusedBorderBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
        }
        else
        {
            // DON'T use Transparent here - if it's "Transparent", then it won't
            // be able to hittest for clicks, and then clicking on the border
            // will eat focus.
            _paneResources.unfocusedBorderBrush = SolidColorBrush{ Colors::Black() };
        }

        const auto broadcastColorKey = winrt::box_value(L"BroadcastPaneBorderColor");
        if (res.HasKey(broadcastColorKey))
        {
            // MAKE SURE TO USE ThemeLookup
            auto obj = ThemeLookup(res, requestedTheme, broadcastColorKey);
            _paneResources.broadcastBorderBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
        }
        else
        {
            // DON'T use Transparent here - if it's "Transparent", then it won't
            // be able to hittest for clicks, and then clicking on the border
            // will eat focus.
            _paneResources.broadcastBorderBrush = SolidColorBrush{ Colors::Black() };
        }
    }

    void TerminalPage::_adjustProcessPriority() const
    {
        // Windowing is single-threaded, so this will not cause a race condition.
        static uint64_t s_lastUpdateHash{ 0 };
        static bool s_supported{ true };

        if (!s_supported || !_hostingHwnd.has_value())
        {
            return;
        }

        std::array<HANDLE, 32> processes;
        auto it = processes.begin();
        const auto end = processes.end();

        auto&& appendFromControl = [&](auto&& control) {
            if (it == end)
            {
                return;
            }
            if (control)
            {
                if (const auto conn{ control.Connection() })
                {
                    if (const auto pty{ conn.try_as<winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection>() })
                    {
                        if (const uint64_t process{ pty.RootProcessHandle() }; process != 0)
                        {
                            *it++ = reinterpret_cast<HANDLE>(process);
                        }
                    }
                }
            }
        };

        auto&& appendFromTab = [&](auto&& tabImpl) {
            if (const auto pane{ tabImpl->GetRootPane() })
            {
                pane->WalkTree([&](auto&& child) {
                    if (const auto& control{ child->GetTerminalControl() })
                    {
                        appendFromControl(control);
                    }
                });
            }
        };

        if (!_activated)
        {
            // When a window is out of focus, we want to attach all of the processes
            // under it to the window so they all go into the background at the same time.
            for (auto&& tab : _tabs)
            {
                if (auto tabImpl{ _GetTabImpl(tab) })
                {
                    appendFromTab(tabImpl);
                }
            }
        }
        else
        {
            // When a window is in focus, propagate our foreground boost (if we have one)
            // to current all panes in the current tab.
            if (auto tabImpl{ _GetFocusedTabImpl() })
            {
                appendFromTab(tabImpl);
            }
        }

        const auto count{ gsl::narrow_cast<DWORD>(it - processes.begin()) };
        const auto hash = til::hash((void*)processes.data(), count * sizeof(HANDLE));

        if (hash == s_lastUpdateHash)
        {
            return;
        }

        s_lastUpdateHash = hash;
        const auto hr = TerminalTrySetWindowAssociatedProcesses(_hostingHwnd.value(), count, count ? processes.data() : nullptr);

        if (S_FALSE == hr)
        {
            // Don't bother trying again or logging. The wrapper tells us it's unsupported.
            s_supported = false;
            return;
        }

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "CalledNewQoSAPI",
            TraceLoggingValue(reinterpret_cast<uintptr_t>(_hostingHwnd.value()), "hwnd"),
            TraceLoggingValue(count),
            TraceLoggingHResult(hr));
#ifdef _DEBUG
        OutputDebugStringW(fmt::format(FMT_COMPILE(L"Submitted {} processes to TerminalTrySetWindowAssociatedProcesses; return=0x{:08x}\n"), count, hr).c_str());
#endif
    }

    void TerminalPage::WindowActivated(const bool activated)
    {
        // Stash if we're activated. Use that when we reload
        // the settings, change active panes, etc.
        _activated = activated;
        _updateThemeColors();

        _adjustProcessPriorityThrottled->Run();

        if (const auto& tab{ _GetFocusedTabImpl() })
        {
            if (tab->TabStatus().IsInputBroadcastActive())
            {
                tab->GetRootPane()->WalkTree([activated](const auto& p) {
                    if (const auto& control{ p->GetTerminalControl() })
                    {
                        control.CursorVisibility(activated ?
                                                     Microsoft::Terminal::Control::CursorDisplayState::Shown :
                                                     Microsoft::Terminal::Control::CursorDisplayState::Default);
                    }
                });
            }
        }
    }

    safe_void_coroutine TerminalPage::_ControlCompletionsChangedHandler(const IInspectable sender,
                                                                        const CompletionsChangedEventArgs args)
    {
        // This won't even get hit if the velocity flag is disabled - we gate
        // registering for the event based off of
        // Feature_ShellCompletions::IsEnabled back in _RegisterTerminalEvents

        // User must explicitly opt-in on Preview builds
        if (!_settings.GlobalSettings().EnableShellCompletionMenu())
        {
            co_return;
        }

        // Parse the json string into a collection of actions
        try
        {
            auto commandsCollection = Command::ParsePowerShellMenuComplete(args.MenuJson(),
                                                                           args.ReplacementLength());

            auto weakThis{ get_weak() };
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weakThis, commandsCollection, sender]() {
                // On the UI thread...
                if (const auto& page{ weakThis.get() })
                {
                    // Open the Suggestions UI with the commands from the control
                    page->_OpenSuggestions(sender.try_as<TermControl>(), commandsCollection, SuggestionsMode::Menu, L"");
                }
            });
        }
        CATCH_LOG();
    }

    void TerminalPage::_OpenSuggestions(
        const TermControl& sender,
        IVector<Command> commandsCollection,
        winrt::TerminalApp::SuggestionsMode mode,
        winrt::hstring filterText)

    {
        // ON THE UI THREAD
        assert(Dispatcher().HasThreadAccess());

        if (commandsCollection == nullptr)
        {
            return;
        }
        if (commandsCollection.Size() == 0)
        {
            if (const auto p = SuggestionsElement())
            {
                p.Visibility(Visibility::Collapsed);
            }
            return;
        }

        const auto& control{ sender ? sender : _GetActiveControl() };
        if (!control)
        {
            return;
        }

        const auto& sxnUi{ LoadSuggestionsUI() };

        const auto characterSize{ control.CharacterDimensions() };
        // This is in control-relative space. We'll need to convert it to page-relative space.
        const auto cursorPos{ control.CursorPositionInDips() };
        const auto controlTransform = control.TransformToVisual(this->Root());
        const auto realCursorPos{ controlTransform.TransformPoint({ cursorPos.X, cursorPos.Y }) }; // == controlTransform + cursorPos
        const Windows::Foundation::Size windowDimensions{ gsl::narrow_cast<float>(ActualWidth()), gsl::narrow_cast<float>(ActualHeight()) };

        sxnUi.Open(mode,
                   commandsCollection,
                   filterText,
                   realCursorPos,
                   windowDimensions,
                   characterSize.Height);
    }

    void TerminalPage::_PopulateContextMenu(const TermControl& control,
                                            const MUX::Controls::CommandBarFlyout& menu,
                                            const bool withSelection)
    {
        // withSelection can be used to add actions that only appear if there's
        // selected text, like "search the web"

        if (!control || !menu)
        {
            return;
        }

        // Helper lambda for dispatching an ActionAndArgs onto the
        // ShortcutActionDispatch. Used below to wire up each menu entry to the
        // respective action.

        auto weak = get_weak();
        auto makeCallback = [weak](const ActionAndArgs& actionAndArgs) {
            return [weak, actionAndArgs](auto&&, auto&&) {
                if (auto page{ weak.get() })
                {
                    page->_actionDispatch->DoAction(actionAndArgs);
                }
            };
        };

        auto makeItem = [&makeCallback](const winrt::hstring& label,
                                        const winrt::hstring& icon,
                                        const auto& action,
                                        auto& targetMenu) {
            AppBarButton button{};

            if (!icon.empty())
            {
                auto iconElement = UI::IconPathConverter::IconWUX(icon);
                Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
                button.Icon(iconElement);
            }

            button.Label(label);
            button.Click(makeCallback(action));
            targetMenu.SecondaryCommands().Append(button);
        };

        auto makeMenuItem = [](const winrt::hstring& label,
                               const winrt::hstring& icon,
                               const auto& subMenu,
                               auto& targetMenu) {
            AppBarButton button{};

            if (!icon.empty())
            {
                auto iconElement = UI::IconPathConverter::IconWUX(icon);
                Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
                button.Icon(iconElement);
            }

            button.Label(label);
            button.Flyout(subMenu);
            targetMenu.SecondaryCommands().Append(button);
        };

        auto makeContextItem = [&makeCallback](const winrt::hstring& label,
                                               const winrt::hstring& icon,
                                               const winrt::hstring& tooltip,
                                               const auto& action,
                                               const auto& subMenu,
                                               auto& targetMenu) {
            AppBarButton button{};

            if (!icon.empty())
            {
                auto iconElement = UI::IconPathConverter::IconWUX(icon);
                Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
                button.Icon(iconElement);
            }

            button.Label(label);
            button.Click(makeCallback(action));
            WUX::Controls::ToolTipService::SetToolTip(button, box_value(tooltip));
            button.ContextFlyout(subMenu);
            targetMenu.SecondaryCommands().Append(button);
        };

        const auto focusedProfile = _GetFocusedTabImpl()->GetFocusedProfile();
        auto separatorItem = AppBarSeparator{};
        auto activeProfiles = _settings.ActiveProfiles();
        auto activeProfileCount = gsl::narrow_cast<int>(activeProfiles.Size());
        MUX::Controls::CommandBarFlyout splitPaneMenu{};

        // Wire up each item to the action that should be performed. By actually
        // connecting these to actions, we ensure the implementation is
        // consistent. This also leaves room for customizing this menu with
        // actions in the future.

        makeItem(RS_(L"DuplicateTabText"), L"\xF5ED", ActionAndArgs{ ShortcutAction::DuplicateTab, nullptr }, menu);

        const auto focusedProfileName = focusedProfile.Name();
        const auto focusedProfileIcon = focusedProfile.Icon().Resolved();
        const auto splitPaneDuplicateText = RS_(L"SplitPaneDuplicateText") + L" " + focusedProfileName; // SplitPaneDuplicateText

        const auto splitPaneRightText = RS_(L"SplitPaneRightText");
        const auto splitPaneDownText = RS_(L"SplitPaneDownText");
        const auto splitPaneUpText = RS_(L"SplitPaneUpText");
        const auto splitPaneLeftText = RS_(L"SplitPaneLeftText");
        const auto splitPaneToolTipText = RS_(L"SplitPaneToolTipText");

        MUX::Controls::CommandBarFlyout splitPaneContextMenu{};
        makeItem(splitPaneRightText, focusedProfileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Duplicate, SplitDirection::Right, .5, nullptr } }, splitPaneContextMenu);
        makeItem(splitPaneDownText, focusedProfileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Duplicate, SplitDirection::Down, .5, nullptr } }, splitPaneContextMenu);
        makeItem(splitPaneUpText, focusedProfileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Duplicate, SplitDirection::Up, .5, nullptr } }, splitPaneContextMenu);
        makeItem(splitPaneLeftText, focusedProfileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Duplicate, SplitDirection::Left, .5, nullptr } }, splitPaneContextMenu);

        makeContextItem(splitPaneDuplicateText, focusedProfileIcon, splitPaneToolTipText, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Duplicate, SplitDirection::Automatic, .5, nullptr } }, splitPaneContextMenu, splitPaneMenu);

        // add menu separator
        const auto separatorAutoItem = AppBarSeparator{};

        splitPaneMenu.SecondaryCommands().Append(separatorAutoItem);

        for (auto profileIndex = 0; profileIndex < activeProfileCount; profileIndex++)
        {
            const auto profile = activeProfiles.GetAt(profileIndex);
            const auto profileName = profile.Name();
            const auto profileIcon = profile.Icon().Resolved();

            NewTerminalArgs args{};
            args.Profile(profileName);

            MUX::Controls::CommandBarFlyout splitPaneContextMenu{};
            makeItem(splitPaneRightText, profileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Manual, SplitDirection::Right, .5, args } }, splitPaneContextMenu);
            makeItem(splitPaneDownText, profileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Manual, SplitDirection::Down, .5, args } }, splitPaneContextMenu);
            makeItem(splitPaneUpText, profileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Manual, SplitDirection::Up, .5, args } }, splitPaneContextMenu);
            makeItem(splitPaneLeftText, profileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Manual, SplitDirection::Left, .5, args } }, splitPaneContextMenu);

            makeContextItem(profileName, profileIcon, splitPaneToolTipText, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Manual, SplitDirection::Automatic, .5, args } }, splitPaneContextMenu, splitPaneMenu);
        }

        makeMenuItem(RS_(L"SplitPaneText"), L"\xF246", splitPaneMenu, menu);

        // Only wire up "Close Pane" if there's multiple panes.
        if (_GetFocusedTabImpl()->GetLeafPaneCount() > 1)
        {
            MUX::Controls::CommandBarFlyout swapPaneMenu{};
            const auto rootPane = _GetFocusedTabImpl()->GetRootPane();
            const auto mruPanes = _GetFocusedTabImpl()->GetMruPanes();
            auto activePane = _GetFocusedTabImpl()->GetActivePane();
            rootPane->WalkTree([&](auto p) {
                if (const auto& c{ p->GetTerminalControl() })
                {
                    if (c == control)
                    {
                        activePane = p;
                    }
                }
            });

            if (auto neighbor = rootPane->NavigateDirection(activePane, FocusDirection::Down, mruPanes))
            {
                makeItem(RS_(L"SwapPaneDownText"), neighbor->GetProfile().Icon().Resolved(), ActionAndArgs{ ShortcutAction::SwapPane, SwapPaneArgs{ FocusDirection::Down } }, swapPaneMenu);
            }

            if (auto neighbor = rootPane->NavigateDirection(activePane, FocusDirection::Right, mruPanes))
            {
                makeItem(RS_(L"SwapPaneRightText"), neighbor->GetProfile().Icon().Resolved(), ActionAndArgs{ ShortcutAction::SwapPane, SwapPaneArgs{ FocusDirection::Right } }, swapPaneMenu);
            }

            if (auto neighbor = rootPane->NavigateDirection(activePane, FocusDirection::Up, mruPanes))
            {
                makeItem(RS_(L"SwapPaneUpText"), neighbor->GetProfile().Icon().Resolved(), ActionAndArgs{ ShortcutAction::SwapPane, SwapPaneArgs{ FocusDirection::Up } }, swapPaneMenu);
            }

            if (auto neighbor = rootPane->NavigateDirection(activePane, FocusDirection::Left, mruPanes))
            {
                makeItem(RS_(L"SwapPaneLeftText"), neighbor->GetProfile().Icon().Resolved(), ActionAndArgs{ ShortcutAction::SwapPane, SwapPaneArgs{ FocusDirection::Left } }, swapPaneMenu);
            }

            makeMenuItem(RS_(L"SwapPaneText"), L"\xF1CB", swapPaneMenu, menu);

            makeItem(RS_(L"TogglePaneZoomText"), L"\xE8A3", ActionAndArgs{ ShortcutAction::TogglePaneZoom, nullptr }, menu);
            makeItem(RS_(L"CloseOtherPanesText"), L"\xE89F", ActionAndArgs{ ShortcutAction::CloseOtherPanes, nullptr }, menu);
            makeItem(RS_(L"PaneClose"), L"\xE89F", ActionAndArgs{ ShortcutAction::ClosePane, nullptr }, menu);
        }

        if (control.ConnectionState() >= ConnectionState::Closed)
        {
            makeItem(RS_(L"RestartConnectionText"), L"\xE72C", ActionAndArgs{ ShortcutAction::RestartConnection, nullptr }, menu);
        }

        if (withSelection)
        {
            makeItem(RS_(L"SearchWebText"), L"\xF6FA", ActionAndArgs{ ShortcutAction::SearchForText, nullptr }, menu);
        }

        makeItem(RS_(L"TabClose"), L"\xE711", ActionAndArgs{ ShortcutAction::CloseTab, CloseTabArgs{ _GetFocusedTabIndex().value() } }, menu);
    }

    void TerminalPage::_PopulateQuickFixMenu(const TermControl& control,
                                             const Controls::MenuFlyout& menu)
    {
        if (!control || !menu)
        {
            return;
        }

        // Helper lambda for dispatching a SendInput ActionAndArgs onto the
        // ShortcutActionDispatch. Used below to wire up each menu entry to the
        // respective action. Then clear the quick fix menu.
        auto weak = get_weak();
        auto makeCallback = [weak](const hstring& suggestion) {
            return [weak, suggestion](auto&&, auto&&) {
                if (auto page{ weak.get() })
                {
                    const auto actionAndArgs = ActionAndArgs{ ShortcutAction::SendInput, SendInputArgs{ hstring{ L"\u0003" } + suggestion } };
                    page->_actionDispatch->DoAction(actionAndArgs);
                    if (auto ctrl = page->_GetActiveControl())
                    {
                        ctrl.ClearQuickFix();
                    }

                    TraceLoggingWrite(
                        g_hTerminalAppProvider,
                        "QuickFixSuggestionUsed",
                        TraceLoggingDescription("Event emitted when a winget suggestion from is used"),
                        TraceLoggingValue("QuickFixMenu", "Source"),
                        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                        TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
                }
            };
        };

        // Wire up each item to the action that should be performed. By actually
        // connecting these to actions, we ensure the implementation is
        // consistent. This also leaves room for customizing this menu with
        // actions in the future.

        menu.Items().Clear();
        const auto quickFixes = control.CommandHistory().QuickFixes();
        for (const auto& qf : quickFixes)
        {
            MenuFlyoutItem item{};

            auto iconElement = UI::IconPathConverter::IconWUX(L"\ue74c");
            Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
            item.Icon(iconElement);

            item.Text(qf);
            item.Click(makeCallback(qf));
            ToolTipService::SetToolTip(item, box_value(qf));
            menu.Items().Append(item);
        }
    }

    // Handler for our WindowProperties's PropertyChanged event. We'll use this
    // to pop the "Identify Window" toast when the user renames our window.
    void TerminalPage::_windowPropertyChanged(const IInspectable& /*sender*/, const WUX::Data::PropertyChangedEventArgs& args)
    {
        if (args.PropertyName() != L"WindowName")
        {
            return;
        }

        // DON'T display the confirmation if this is the name we were
        // given on startup!
        if (_startupState == StartupState::Initialized)
        {
            IdentifyWindow();
        }
    }

    void TerminalPage::_onTabDragStarting(const winrt::Microsoft::UI::Xaml::Controls::TabView&,
                                          const winrt::Microsoft::UI::Xaml::Controls::TabViewTabDragStartingEventArgs& e)
    {
        // Get the tab impl from this event.
        const auto eventTab = e.Tab();
        const auto tabBase = _GetTabByTabViewItem(eventTab);
        winrt::com_ptr<Tab> tabImpl;
        tabImpl.copy_from(winrt::get_self<Tab>(tabBase));
        if (tabImpl)
        {
            // First: stash the tab we started dragging.
            // We're going to be asked for this.
            _stashed.draggedTab = tabImpl;

            // Stash the offset from where we started the drag to the
            // tab's origin. We'll use that offset in the future to help
            // position the dropped window.
            const auto inverseScale = 1.0f / static_cast<float>(eventTab.XamlRoot().RasterizationScale());
            POINT cursorPos;
            GetCursorPos(&cursorPos);
            ScreenToClient(*_hostingHwnd, &cursorPos);
            _stashed.dragOffset.X = cursorPos.x * inverseScale;
            _stashed.dragOffset.Y = cursorPos.y * inverseScale;

            // Into the DataPackage, let's stash our own window ID.
            const auto id{ _WindowProperties.WindowId() };

            // Get our PID
            const auto pid{ GetCurrentProcessId() };

            e.Data().Properties().Insert(L"windowId", winrt::box_value(id));
            e.Data().Properties().Insert(L"pid", winrt::box_value<uint32_t>(pid));
            e.Data().RequestedOperation(DataPackageOperation::Move);

            // The next thing that will happen:
            //  * Another TerminalPage will get a TabStripDragOver, then get a
            //    TabStripDrop
            //    * This will be handled by the _other_ page asking the monarch
            //      to ask us to send our content to them.
            //  * We'll get a TabDroppedOutside to indicate that this tab was
            //    dropped _not_ on a TabView.
            //    * This will be handled by _onTabDroppedOutside, which will
            //      raise a MoveContent (to a new window) event.
        }
    }

    void TerminalPage::_onTabStripDragOver(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                           const winrt::Windows::UI::Xaml::DragEventArgs& e)
    {
        // We must mark that we can accept the drag/drop. The system will never
        // call TabStripDrop on us if we don't indicate that we're willing.
        const auto& props{ e.DataView().Properties() };
        if (props.HasKey(L"windowId") &&
            props.HasKey(L"pid") &&
            (winrt::unbox_value_or<uint32_t>(props.TryLookup(L"pid"), 0u) == GetCurrentProcessId()))
        {
            e.AcceptedOperation(DataPackageOperation::Move);
        }

        // You may think to yourself, this is a great place to increase the
        // width of the TabView artificially, to make room for the new tab item.
        // However, we'll never get a message that the tab left the tab view
        // (without being dropped). So there's no good way to resize back down.
    }

    // Method Description:
    // - Called on the TARGET of a tab drag/drop. We'll unpack the DataPackage
    //   to find who the tab came from. We'll then ask the Monarch to ask the
    //   sender to move that tab to us.
    void TerminalPage::_onTabStripDrop(winrt::Windows::Foundation::IInspectable /*sender*/,
                                       winrt::Windows::UI::Xaml::DragEventArgs e)
    {
        // Get the PID and make sure it is the same as ours.
        if (const auto& pidObj{ e.DataView().Properties().TryLookup(L"pid") })
        {
            const auto pid{ winrt::unbox_value_or<uint32_t>(pidObj, 0u) };
            if (pid != GetCurrentProcessId())
            {
                // The PID doesn't match ours. We can't handle this drop.
                return;
            }
        }
        else
        {
            // No PID? We can't handle this drop. Bail.
            return;
        }

        const auto& windowIdObj{ e.DataView().Properties().TryLookup(L"windowId") };
        if (windowIdObj == nullptr)
        {
            // No windowId? Bail.
            return;
        }
        const uint64_t src{ winrt::unbox_value<uint64_t>(windowIdObj) };

        // Figure out where in the tab strip we're dropping this tab. Add that
        // index to the request. This is largely taken from the WinUI sample
        // app.

        // First we need to get the position in the List to drop to
        auto index = -1;

        // Determine which items in the list our pointer is between.
        for (auto i = 0u; i < _tabView.TabItems().Size(); i++)
        {
            if (const auto& item{ _tabView.ContainerFromIndex(i).try_as<winrt::MUX::Controls::TabViewItem>() })
            {
                const auto posX{ e.GetPosition(item).X }; // The point of the drop, relative to the tab
                const auto itemWidth{ item.ActualWidth() }; // The right of the tab
                // If the drag point is on the left half of the tab, then insert here.
                if (posX < itemWidth / 2)
                {
                    index = i;
                    break;
                }
            }
        }

        // `this` is safe to use
        const auto request = winrt::make_self<RequestReceiveContentArgs>(src, _WindowProperties.WindowId(), index);

        // This will go up to the monarch, who will then dispatch the request
        // back down to the source TerminalPage, who will then perform a
        // RequestMoveContent to move their tab to us.
        RequestReceiveContent.raise(*this, *request);
    }

    // Method Description:
    // - This is called on the drag/drop SOURCE TerminalPage, when the monarch has
    //   requested that we send our tab to another window. We'll need to
    //   serialize the tab, and send it to the monarch, who will then send it to
    //   the destination window.
    // - Fortunately, sending the tab is basically just a MoveTab action, so we
    //   can largely reuse that.
    void TerminalPage::SendContentToOther(winrt::TerminalApp::RequestReceiveContentArgs args)
    {
        // validate that we're the source window of the tab in this request
        if (args.SourceWindow() != _WindowProperties.WindowId())
        {
            return;
        }
        if (!_stashed.draggedTab)
        {
            return;
        }

        _sendDraggedTabToWindow(winrt::to_hstring(args.TargetWindow()), args.TabIndex(), std::nullopt);
    }

    void TerminalPage::_onTabDroppedOutside(winrt::IInspectable /*sender*/,
                                            winrt::MUX::Controls::TabViewTabDroppedOutsideEventArgs /*e*/)
    {
        // Get the current pointer point from the CoreWindow
        const auto& pointerPoint{ CoreWindow::GetForCurrentThread().PointerPosition() };

        // This is called when a tab FROM OUR WINDOW was dropped outside the
        // tabview. We already know which tab was being dragged. We'll just
        // invoke a moveTab action with the target window being -1. That will
        // force the creation of a new window.

        if (!_stashed.draggedTab)
        {
            return;
        }

        // We need to convert the pointer point to a point that we can use
        // to position the new window. We'll use the drag offset from before
        // so that the tab in the new window is positioned so that it's
        // basically still directly under the cursor.

        // -1 is the magic number for "new window"
        // 0 as the tab index, because we don't care. It's making a new window. It'll be the only tab.
        const winrt::Windows::Foundation::Point adjusted = {
            pointerPoint.X - _stashed.dragOffset.X,
            pointerPoint.Y - _stashed.dragOffset.Y,
        };
        _sendDraggedTabToWindow(winrt::hstring{ L"-1" }, 0, adjusted);
    }

    void TerminalPage::_sendDraggedTabToWindow(const winrt::hstring& windowId,
                                               const uint32_t tabIndex,
                                               std::optional<winrt::Windows::Foundation::Point> dragPoint)
    {
        auto startupActions = _stashed.draggedTab->BuildStartupActions(BuildStartupKind::Content);
        _DetachTabFromWindow(_stashed.draggedTab);

        _MoveContent(std::move(startupActions), windowId, tabIndex, dragPoint);
        // _RemoveTab will make sure to null out the _stashed.draggedTab
        _RemoveTab(*_stashed.draggedTab);
    }

    /// <summary>
    /// Creates a sub flyout menu for profile items in the split button menu that when clicked will show a menu item for
    /// Run as Administrator
    /// </summary>
    /// <param name="profileIndex">The index for the profileMenuItem</param>
    /// <returns>MenuFlyout that will show when the context is request on a profileMenuItem</returns>
    WUX::Controls::MenuFlyout TerminalPage::_CreateRunAsAdminFlyout(int profileIndex)
    {
        // Create the MenuFlyout and set its placement
        WUX::Controls::MenuFlyout profileMenuItemFlyout{};
        profileMenuItemFlyout.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);

        // Create the menu item and an icon to use in the menu
        WUX::Controls::MenuFlyoutItem runAsAdminItem{};
        WUX::Controls::FontIcon adminShieldIcon{};

        adminShieldIcon.Glyph(L"\xEA18");
        adminShieldIcon.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });

        runAsAdminItem.Icon(adminShieldIcon);
        runAsAdminItem.Text(RS_(L"RunAsAdminFlyout/Text"));

        // Click handler for the flyout item
        runAsAdminItem.Click([profileIndex, weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuItemElevateSubmenuItemClicked",
                    TraceLoggingDescription("Event emitted when the elevate submenu item from the new tab menu is invoked"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

                NewTerminalArgs args{ profileIndex };
                args.Elevate(true);
                page->_OpenNewTerminalViaDropdown(args);
            }
        });

        profileMenuItemFlyout.Items().Append(runAsAdminItem);

        return profileMenuItemFlyout;
    }
}
