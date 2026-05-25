
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"

#include <cstring>

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
#include "WorkspaceView.h"
#include "WorkspaceViewModel.h"
#include "PaneTabViewModel.h"
#include "TabStripView.h"

#include "../WorkspaceModel/Diff.h"

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
    // Big-flip Slice F-1 (#54): thickness (px) of the custom split divider Border
    // built in _projectPaneNode. Matches the classic Pane's combined border size
    // (2 * PaneBorderSize) so the boundary reads the same once visible at F-5.
    // Minimal by design — the separator is invisible until F-5, so we do NOT
    // gold-plate it here.
    static constexpr double kSplitSeparatorThickness = 2.0;

    TerminalPage::~TerminalPage() = default;

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

        // Phase 2 Slice 2 (#46): when the workspaces flag is ON, the chrome
        // (hamburger + new-workspace split-button) takes the window titlebar
        // and the tab strip stays in the client area UNDER it — i.e. we reuse
        // the existing "tabs under titlebar" rendering by simply leaving the
        // TabRow in the page rather than handing it to the host. The sidebar
        // also gets realized here. When the flag is OFF, _initializeWorkspaceShell
        // is a no-op and we fall through to the byte-for-byte upstream path
        // below (sidebar collapsed, tabs in titlebar exactly as today).
        if (_workspacesFlagEnabled())
        {
            _initializeWorkspaceShell();
        }
        else if (_settings.GlobalSettings().ShowTabsInTitlebar())
        {
            // Remove the TabView from the page. We'll hang on to it, we need to
            // put it in the titlebar.
            //
            // The #46 root restructure keeps TabRow a DIRECT child of Root (now
            // at row 1 / column 1 via attached properties), so this is the exact
            // upstream removal — flag-off tabs-in-titlebar is byte-for-byte.
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

        // Big-flip F-5 fix (#46): this is the incoming-defterm path (called from
        // TerminalWindow when an external process hands us a ConPTY). Flag-on it
        // builds a CLASSIC Tab from the connection, inserts it into _tabs, and
        // selects it — which routes through _OnTabSelectionChanged ->
        // _UpdatedSelectedTab -> _tabContent.Children().Clear(), dropping the
        // workspace host (the sole child of _tabContent) and leaving a blank
        // window. It would also leave a live second ConPTY orphaned in a tab the
        // workspace display can never show. Defterm connections are not yet
        // representable in the WorkspaceModel (no model action seeds an existing
        // connection into a workspace), so we cannot route this through the model.
        //
        // Interim: decline the connection flag-on (drop it; do not build a tab).
        // This preserves the sole-child host invariant — no classic tab is built,
        // _tabContent is never cleared. Deferred follow-up: accept an incoming
        // defterm connection as a new workspace/pane via the model
        // (defterm-in-workspace), tracked separately.
        if (_workspacesFlagEnabled())
        {
            connection.Close();
            co_return;
        }

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
        //
        // F-4 (#46): routed through _windowShouldStayOpen(). Flag-off this is
        // the byte-for-byte inverse of `_tabs.Size() == 0`. Flag-on, the
        // startup-replay NewTab has already populated the model with one
        // workspace by the time we get here (ProcessStartupActions runs before
        // _CompleteInitialization), so the model is non-empty and we stay open.
        // On the defterm-readying path no workspace exists yet, so the model is
        // still null and _windowShouldStayOpen() falls back to `_tabs.Size()`,
        // preserving the GH#12267 don't-close-yet intent.
        if (!_windowShouldStayOpen())
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
    // - Displays the unified close confirmation dialog configured for the
    //   given scenario. Resets the "don't ask me again" checkbox before showing.
    //   If the user confirms and checked "don't ask me again", sets
    //   confirmOnClose to Never and writes settings to disk.
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowConfirmCloseDialog(ConfirmCloseDialogKind kind)
    {
        // Load the dialog (triggers x:Load) and configure its strings.
        const auto dialog = FindName(L"ConfirmCloseDialog").as<ContentDialog>();

        winrt::hstring title;
        winrt::hstring primary;
        switch (kind)
        {
        case ConfirmCloseDialogKind::CloseAll:
            title = RS_(L"ConfirmCloseDialog_CloseAllTitle");
            primary = RS_(L"ConfirmCloseDialog_CloseAllPrimary");
            break;
        case ConfirmCloseDialogKind::Window:
            title = RS_(L"ConfirmCloseDialog_WindowTitle");
            primary = RS_(L"ConfirmCloseDialog_WindowPrimary");
            break;
        case ConfirmCloseDialogKind::Tab:
            title = RS_(L"ConfirmCloseDialog_TabTitle");
            primary = RS_(L"ConfirmCloseDialog_TabPrimary");
            break;
        case ConfirmCloseDialogKind::MultiplePanes:
            title = RS_(L"ConfirmCloseDialog_MultiplePanesTitle");
            primary = RS_(L"ConfirmCloseDialog_MultiplePanesPrimary");
            break;
        case ConfirmCloseDialogKind::MultipleTabs:
            title = RS_(L"ConfirmCloseDialog_MultipleTabsTitle");
            primary = RS_(L"ConfirmCloseDialog_MultipleTabsPrimary");
            break;
        case ConfirmCloseDialogKind::Pane:
            title = RS_(L"ConfirmCloseDialog_PaneTitle");
            primary = RS_(L"ConfirmCloseDialog_PanePrimary");
            break;
        }
        dialog.Title(winrt::box_value(title));
        dialog.PrimaryButtonText(primary);
        dialog.CloseButtonText(RS_(L"ConfirmCloseDialog_Cancel"));

        // BODGY: After a ContentDialog is dismissed, FindName() can no longer
        // resolve children inside it. Use Content() to get the checkbox directly.
        const auto checkbox = dialog.Content().as<CheckBox>();
        checkbox.IsChecked(false);

        auto result = ContentDialogResult::None;
        if (auto presenter{ _dialogPresenter.get() })
        {
            const auto weak = get_weak();
            result = co_await presenter.ShowDialog(dialog);

            // ShowDialog blocks until the dialog is dismissed, so it is
            // possible for `this` to be torn down while we wait. Re-acquire
            // a strong reference before touching any of our state.
            const auto strong = weak.get();
            if (!strong)
            {
                co_return ContentDialogResult::None;
            }

            if (result == ContentDialogResult::Primary && checkbox.IsChecked().Value())
            {
                _settings.GlobalSettings().ConfirmOnClose(ConfirmOnClose::Never);
                _settings.WriteSettingsToDisk();
            }
        }

        co_return result;
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
        _newTabButton.Flyout(_buildNewItemFlyout([weakThis{ get_weak() }](NewTerminalArgs args) {
            if (auto page{ weakThis.get() })
            {
                page->_OpenNewTerminalViaDropdown(args);
            }
        }));
    }

    // Method Description:
    // - Builds (and returns) the "+ | v" dropdown MenuFlyout shared by the tab
    //   strip's NewTabButton and the workspaces chrome's NewWorkspaceButton. The
    //   per-profile menu items are parameterized by `onChosen`, which decides
    //   what choosing a profile does (open a new terminal vs. create a new
    //   workspace). The static Settings / Command Palette / About items and the
    //   Opening/Closing focus handlers are identical for both consumers.
    WUX::Controls::MenuFlyout TerminalPage::_buildNewItemFlyout(std::function<void(NewTerminalArgs)> onChosen)
    {
        auto newTabFlyout = WUX::Controls::MenuFlyout{};
        newTabFlyout.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);

        // Create profile entries from the NewTabMenu configuration using a
        // recursive helper function. This returns a std::vector of FlyoutItemBases,
        // that we then add to our Flyout.
        auto entries = _settings.GlobalSettings().NewTabMenu();
        auto items = _CreateNewTabFlyoutItems(entries, onChosen);
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
        return newTabFlyout;
    }

    // Method Description:
    // - For a given list of tab menu entries, this method will create the corresponding
    //   list of flyout items. This is a recursive method that calls itself when it comes
    //   across a folder entry.
    std::vector<WUX::Controls::MenuFlyoutItemBase> TerminalPage::_CreateNewTabFlyoutItems(IVector<NewTabMenuEntry> entries, std::function<void(NewTerminalArgs)> onChosen)
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
                auto folderEntryItems = _CreateNewTabFlyoutItems(folderEntries, onChosen);

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
                    items.push_back(_CreateNewTabFlyoutProfile(remainingProfile, profileIndex, {}, onChosen));
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

                auto profileItem = _CreateNewTabFlyoutProfile(profileEntry.Profile(), profileEntry.ProfileIndex(), profileEntry.Icon().Resolved(), onChosen);
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
    WUX::Controls::MenuFlyoutItem TerminalPage::_CreateNewTabFlyoutProfile(const Profile profile, int profileIndex, const winrt::hstring& iconPathOverride, std::function<void(NewTerminalArgs)> onChosen)
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

        profileMenuItem.Click([profileIndex, onChosen, weakThis{ get_weak() }](auto&&, auto&&) {
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
                onChosen(newTerminalArgs);
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
            valueSet = TerminalConnection::ConptyConnection::CreateSettings(settings.Commandline(),
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
        const auto setting = _settings.GlobalSettings().ConfirmOnClose();
        if (setting != ConfirmOnClose::Never && !_displayingCloseDialog)
        {
            _displayingCloseDialog = true;

            const auto weak = get_weak();
            auto warningResult = co_await _ShowConfirmCloseDialog(ConfirmCloseDialogKind::CloseAll);
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
        }

        QuitRequested.raise(nullptr, nullptr);
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

        auto mode = LaunchMode::DefaultMode;
        WI_SetFlagIf(mode, LaunchMode::FullscreenMode, _isFullscreen);
        WI_SetFlagIf(mode, LaunchMode::FocusMode, _isInFocusMode);
        WI_SetFlagIf(mode, LaunchMode::MaximizedMode, _isMaximized);

        layout.LaunchMode({ mode });

        // Only save the content size because the tab size will be added on load.
        const auto contentWidth = static_cast<float>(_tabContent.ActualWidth());
        const auto contentHeight = static_cast<float>(_tabContent.ActualHeight());
        const winrt::Windows::Foundation::Size windowSize{ contentWidth, contentHeight };

        layout.InitialSize(windowSize);

        // We don't actually know our own position. So we have to ask the window
        // layer for that.
        const auto launchPosRequest{ winrt::make<LaunchPositionRequest>() };
        RequestLaunchPosition.raise(*this, launchPosRequest);
        layout.InitialPosition(launchPosRequest.Position());

        ApplicationState::SharedInstance().AppendPersistedWindowLayout(layout);
    }

    // Method Description:
    // - Determines whether a close-window action should show a confirmation
    //   dialog, based on the confirmOnClose setting and the current window state.
    // Arguments:
    // - <none>
    // Return Value:
    // - true, if a warning dialog should be shown before closing the window
    bool TerminalPage::_ShouldWarnOnClose() const
    {
        const auto setting = _settings.GlobalSettings().ConfirmOnClose();
        switch (setting)
        {
        case ConfirmOnClose::Always:
            return true;
        case ConfirmOnClose::Automatic:
        {
            // Warn if there's more than one tab, or the one tab has more than one pane.
            return _HasMultipleTabs() || _GetTabImpl(_tabs.GetAt(0))->GetLeafPaneCount() > 1;
        }
        case ConfirmOnClose::Never:
        default:
            return false;
        }
    }

    // Method Description:
    // - Determines whether closing a specific tab should show a confirmation
    //   dialog, based on the confirmOnClose setting and the tab's state.
    // Arguments:
    // - tab: The tab being closed
    // Return Value:
    // - true, if a warning dialog should be shown before closing the tab
    bool TerminalPage::_ShouldWarnOnCloseTab(const winrt::com_ptr<Tab>& tab) const
    {
        const auto setting = _settings.GlobalSettings().ConfirmOnClose();
        switch (setting)
        {
        case ConfirmOnClose::Always:
            return true;
        case ConfirmOnClose::Automatic:
            // Warn if this tab has more than one pane.
            return tab->GetLeafPaneCount() > 1;
        case ConfirmOnClose::Never:
        default:
            return false;
        }
    }

    // Method Description:
    // - Close the terminal app. If the confirmOnClose setting indicates we should
    //   warn for the current window state, show a warning dialog.
    safe_void_coroutine TerminalPage::CloseWindow()
    {
        if (_ShouldWarnOnClose() &&
            !_displayingCloseDialog)
        {
            if (_newTabButton && _newTabButton.Flyout())
            {
                _newTabButton.Flyout().Hide();
            }
            _DismissTabContextMenus();
            _displayingCloseDialog = true;

            const auto weak = get_weak();
            auto warningResult = co_await _ShowConfirmCloseDialog(ConfirmCloseDialogKind::Window);
            // Hold a strong reference to `this` after the co_await; we may
            // be the last holder if the window was already being torn down.
            auto strong = weak.get();
            if (!strong)
            {
                co_return;
            }

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
                // Phase 2: route move-tab through closeWorkspace +
                // newWorkspace dispatch via the model so the receiving
                // window's workspace lifecycle stays under model
                // control. For now, this bypass path destroys the Tab
                // directly via _RemoveTab without firing Tab::Closed,
                // so the flag-on close-routing never runs. Erase any
                // registry entry for this Tab to avoid a zombie
                // workspace binding.
                _eraseClassicTabFromRegistry(*tab);
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
    // - whether a pane was resized
    bool TerminalPage::_ResizePane(const ResizeDirection& direction)
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();
            return tabImpl->ResizePane(direction);
        }
        return false;
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

    bool TerminalPage::_IsUriConsideredSomewhatSafe(const winrt::Windows::Foundation::Uri& parsedUri) const
    {
        const auto& schemeName = parsedUri.SchemeName();

        if (schemeName == L"http" || schemeName == L"https")
        {
            return true;
        }
        if (schemeName == L"file")
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
        if (const auto& safeSchemes = _settings.GlobalSettings().SafeUriSchemes())
        {
            for (const auto& scheme : safeSchemes)
            {
                if (til::equals_insensitive_ascii(schemeName, scheme))
                {
                    return true;
                }
            }
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

    // Big-flip Slice A (#54): the real ContentMounted factory. Materialises a
    // live IPaneContent from a model TabContent spec for the ContentRegistry to
    // own (the registry — not the classic Tab — becomes the strong owner of the
    // content). Invoked from WorkspaceView::apply(ContentMounted), which runs
    // well after page construction, so get_weak() (used by the close-revoker
    // the TerminalPaneContent wires) is valid.
    //
    // This REPLICATES the minimal TerminalSpec content-build core of
    // _MakeTerminalPane (recover the profile GUID -> NewTerminalArgs ->
    // GetProfileForArgs -> CreateWithNewTerminalArgs -> connection -> control ->
    // TerminalPaneContent) rather than extracting a shared helper:
    // _MakeTerminalPane's body is entangled with the duplicate-from-sourceTab,
    // content-id-attach, auto-elevation, debug-tap and session-restore paths
    // and returns a std::shared_ptr<Pane>, not an IPaneContent — factoring a
    // shared content-build helper out of it risked perturbing those classic
    // paths (which this slice must leave byte-for-byte intact). The replicated
    // steps below are exactly the from-spec subset: no split, no duplication,
    // no debug-tap, no session restore.
    //
    // Returns nullptr ("do not attach") for any non-Terminal variant
    // (Settings/Snippets/Markdown/Scratchpad — not yet model-materialisable) or
    // on spawn failure / auto-elevation handoff (_maybeElevate reports the new
    // window took over).
    winrt::TerminalApp::IPaneContent TerminalPage::_makePaneContentForSpec(const ::WorkspaceModel::TabContent& spec)
    {
        // Big-flip Slice B (#54): test-only factory override. When a test has
        // installed one, return its content instead of building a real
        // TermControl — the host-attach tests want a MockPaneContent with a
        // known root. Empty in production, so the live path below is unchanged.
        if (_makePaneContentForSpecOverrideForTest)
        {
            return _makePaneContentForSpecOverrideForTest(spec);
        }

        const auto* terminalSpec = std::get_if<::WorkspaceModel::TerminalSpec>(&spec);
        if (!terminalSpec)
        {
            // Settings / Snippets / Markdown / Scratchpad are not yet
            // materialisable from the model; the classic Tab still owns them.
            return nullptr;
        }

        // The 16-byte profile bytes are the canonical winrt::guid in-memory
        // layout on Windows; reinterpret to recover the winrt::guid, then format
        // as the bracketed-GUID string NewTerminalArgs::Profile accepts. A
        // zero-GUID maps to the default profile (GetProfileForArgs falls back),
        // so we leave Profile unset in that case — mirroring
        // _openProfileTabForWorkspace.
        winrt::guid g{};
        static_assert(sizeof(winrt::guid) == 16, "winrt::guid must be 16 bytes");
        std::memcpy(&g, terminalSpec->profile.data(), 16);

        NewTerminalArgs newTerminalArgs{};
        if (g != winrt::guid{})
        {
            newTerminalArgs.Profile(::Microsoft::Console::Utils::GuidToString(g));
        }

        const auto profile = _settings.GetProfileForArgs(newTerminalArgs);
        const auto controlSettings = Settings::TerminalSettings::CreateWithNewTerminalArgs(_settings, newTerminalArgs);

        // Auto-elevation handoff: if the profile wants elevation and we're not
        // elevated, _maybeElevate spawns an elevated window to host this content
        // and returns true. There is then no content to own in this window —
        // return nullptr so the registry attaches nothing.
        if (_maybeElevate(newTerminalArgs, controlSettings, profile))
        {
            return nullptr;
        }

        const auto sessionId = controlSettings.DefaultSettings()->SessionId();
        const auto hasSessionId = sessionId != winrt::guid{};

        auto connection = _CreateConnectionFromSettings(profile, *controlSettings.DefaultSettings(), hasSessionId);
        const auto control = _CreateNewControlAndContent(controlSettings, connection);

        return winrt::make<TerminalPaneContent>(profile, _terminalSettingsCache, control);
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
        // Big-flip F-5 fix (#46): flag-on the workspace pane tree (hosted in
        // _workspaceContentHost) is the SOLE child of _tabContent and IS the
        // visible display. The classic settings-UI path builds a Tab, inserts
        // it into _tabs, and selects it via _tabView.SelectedItem(...), which
        // routes through _OnTabSelectionChanged -> _UpdatedSelectedTab ->
        // _tabContent.Children().Clear() — DROPPING the workspace host and
        // leaving a blank window. The Settings UI is a Terminal-variant XAML
        // page, not a WorkspaceModel content type (the content factory only
        // materialises Terminal panes), so it cannot yet live inside the model.
        //
        // Interim: no-op flag-on. Settings UI is temporarily unavailable in
        // workspaces mode (reachable flag-off as before). This preserves the
        // sole-child host invariant: no classic tab is built, _tabContent is
        // never cleared. Deferred follow-up: render the Settings UI as a
        // first-class WorkspaceModel content kind (settings-as-model-content),
        // tracked separately; until then this guard is the least-bad behavior.
        if (_workspacesFlagEnabled())
        {
            return;
        }

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
        // Phase 2: route drag tear-out through closeWorkspace +
        // newWorkspace dispatch via the model so the receiving
        // window's workspace lifecycle stays under model control. For
        // now, this bypass path destroys the Tab directly via
        // _RemoveTab without firing Tab::Closed, so the flag-on
        // close-routing never runs. Erase any registry entry for this
        // Tab to avoid a zombie workspace binding.
        _eraseClassicTabFromRegistry(*_stashed.draggedTab);
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

    // -----------------------------------------------------------------
    // Workspaces (experimental.workspaces.enabled)
    // -----------------------------------------------------------------

    bool TerminalPage::_workspacesFlagEnabled() const noexcept
    {
        if (_settings == nullptr)
        {
            return false;
        }
        return _settings.GlobalSettings().WorkspacesEnabled();
    }

    // Big-flip F-4 (#46): the single window-stay-open predicate. The two
    // close-decision guards (_CompleteInitialization and _RemoveTab) route
    // through this instead of testing `_tabs.Size() == 0` directly, so the
    // close semantics live in one place ahead of the F-5 cutover.
    //
    // Flag-off: `_tabs.Size() > 0` — EXACTLY the inverse of the upstream
    // `_tabs.Size() == 0` close checks, so flag-off is byte-for-byte upstream.
    //
    // Flag-on: the window should stay open while the model still holds a
    // workspace. `_workspaceModelState` is a `shared_ptr<const ...>` that is
    // null until the workspace shell is populated (the startup-replay NewTab
    // dispatches newWorkspace -> _applyWorkspaceAction; the defterm-readying
    // path of GH#12267 reaches _CompleteInitialization with NO workspace yet).
    // Dereferencing a null model would crash, so when the model is not yet
    // initialized we fall back to the classic `_tabs.Size()` count. That
    // fallback is also behavior-preserving: it keeps the GH#632 elevated-only
    // close AND the GH#12267 don't-close-while-readying-a-defterm-connection
    // intent intact (no tab and no workspace -> stay-open false at
    // _CompleteInitialization, exactly as `_tabs.Size() == 0` did today).
    //
    // NOTE (spawn-failure divergence, F-4): once the model is populated, a
    // workspace whose backing classic tab failed to spawn
    // (_openDefaultTabForWorkspace returned nullptr -> no _workspaceClassicTabs
    // binding) makes `_tabs.Size()` and `workspaces_view().size()` diverge:
    // `_tabs` can be 0 while a workspace still exists. Flag-on then returns
    // true (stay open with an empty workspace) where the old
    // `_tabs.Size() == 0` would have closed. The new behavior is arguably more
    // correct, but it is NOT strictly byte-for-byte. We do NOT paper over it
    // with a fallback classic tab — tabs must belong to a workspace.
    bool TerminalPage::_windowShouldStayOpen() const
    {
        if (_workspacesFlagEnabled() && _workspaceModelState)
        {
            return !_workspaceModelState->workspaces_view().empty();
        }
        return _tabs.Size() > 0;
    }

    void TerminalPage::_ensureWorkspaceView()
    {
        if (!_workspaceView)
        {
            _workspaceView = std::make_unique<WorkspaceView>(get_weak());
        }
    }

    void TerminalPage::_applyWorkspaceAction(::WorkspaceModel::ModelState newState)
    {
        _ensureWorkspaceView();
        auto changes = ::WorkspaceModel::diff(_workspaceModelState, newState);
        _workspaceModelState = std::move(newState);
        // Each WorkspaceChange is self-describing — the view resolves no
        // model state of its own, so there is no held state to seed here.
        ::WorkspaceModel::applyChanges(*_workspaceView, std::span<const ::WorkspaceModel::WorkspaceChange>{ changes });

        // Big-flip Slice F-5 (#54): THE model-driven window close. Pre-cutover,
        // closing the last classic tab fired CloseWindowRequested through the
        // classic _RemoveTab cascade (apply(WorkspaceRemoved) -> _RemoveTab ->
        // last-tab => raise). Flag-on, no classic Tab is built (apply(TabAdded)
        // gates out _OpenNewTab), so _workspaceClassicTabs is empty and _RemoveTab
        // never runs — that cascade can no longer fire the close. We re-raise it
        // here off the MODEL: when an action empties the workspace list (closing
        // the last workspace, OR closing the last tab which cascades in the model
        // to remove the workspace — see WorkspaceActions.h cascade rules),
        // _windowShouldStayOpen() flips false and we raise CloseWindowRequested
        // exactly once.
        //
        // This fires at most once per action and ONLY when the model is now empty
        // — the very first action is newWorkspace (non-empty), so startup never
        // spuriously closes; an action that leaves a workspace standing leaves the
        // predicate true and raises nothing. Flag-OFF, _applyWorkspaceAction is
        // never called (the flag-on dispatch is the only caller), so this is
        // inert flag-off — the classic _RemoveTab close path is byte-for-byte
        // upstream.
        if (_workspacesFlagEnabled() && !_windowShouldStayOpen())
        {
            CloseWindowRequested.raise(*this, nullptr);
        }
    }

    // Mirrors the classic _HandleNewTab(args==nullptr) branch:
    // _OpenNewTab(nullptr) lets the page consult its settings for the
    // default profile. Returns the newly-appended Tab, or nullptr if
    // _OpenNewTab did not append a tab (spawn failure, elevation
    // handoff, etc.). Callers use the return value to decide whether
    // to bind a workspace -> Tab registry entry; binding the wrong
    // Tab (e.g. a pre-existing one when _tabs.back() didn't change)
    // would later tear down the wrong tab in
    // _removeClassicTabForRemovedWorkspace.
    winrt::TerminalApp::Tab TerminalPage::_openDefaultTabForWorkspace()
    {
        const auto sizeBefore = _tabs.Size();
        LOG_IF_FAILED(_OpenNewTab(nullptr));
        const auto sizeAfter = _tabs.Size();
        if (sizeAfter <= sizeBefore)
        {
            // No new tab was appended (HRESULT==S_FALSE from a missing
            // profile, _MakePane returning nullptr, elevation handoff,
            // or any thrown exception caught by _OpenNewTab's
            // CATCH_RETURN). Tell the caller to skip registry binding
            // so the new workspace stays unbound rather than mis-bound
            // to a pre-existing Tab.
            return nullptr;
        }
        return _tabs.GetAt(sizeAfter - 1);
    }

    // Called by WorkspaceView::apply(TabAdded) right after the classic
    // _OpenNewTab path appends the new tab to _tabs. Phase 1 binds the
    // tab to its owning workspace id so the WorkspaceRemoved arm can
    // find the matching classic tab when closing.
    //
    // The Tab is passed in explicitly (rather than inferred from
    // _tabs.back()) so spawn failures don't mis-bind the new workspace
    // to a pre-existing tab. If the classic side appended nothing
    // (spawn failure), the caller must pass nullptr and the registry
    // stays unchanged for this workspace; closing the workspace later
    // just becomes a model-only operation, which is what the validator
    // requires.
    void TerminalPage::_registerClassicTabForWorkspace(::WorkspaceModel::WorkspaceId ws,
                                                       const winrt::TerminalApp::Tab& tab)
    {
        if (!ws.valid() || !tab)
        {
            return;
        }
        _workspaceClassicTabs[ws] = winrt::make_weak(tab);
    }

    // Erases any registry entries whose weak_ref<Tab> currently
    // resolves to the given Tab. Called from the move-tab-to-window
    // and drag tear-out callsites — those paths destroy a Tab without
    // firing Tab::Closed, so the flag-on Tab::Closed routing
    // (_closeTabViaWorkspaceModel) never gets a chance to delete the
    // mapping. Without this cleanup, _workspaceClassicTabs would hold
    // a stale weak_ref for the duration of the page, and the model's
    // owning workspace would be a zombie. The classic teardown
    // (_RemoveTab) still runs at the bypass callsite — this helper
    // only fixes the registry side.
    //
    // Phase 2 will route these tear-out paths through closeWorkspace
    // dispatch via the model (TODO: tracked in issue #21 follow-up).
    void TerminalPage::_eraseClassicTabFromRegistry(const winrt::TerminalApp::Tab& tab)
    {
        if (!tab)
        {
            return;
        }
        for (auto it = _workspaceClassicTabs.begin(); it != _workspaceClassicTabs.end();)
        {
            const auto resolved = it->second.get();
            if (resolved && resolved == tab)
            {
                it = _workspaceClassicTabs.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Called by WorkspaceView::apply(WorkspaceRemoved). Phase 1 maps one
    // model workspace to one classic tab; tearing down the workspace
    // means tearing down that tab. _RemoveTab fires CloseWindowRequested
    // when it drops the last tab, so the cascade's window-close
    // behaviour reuses the existing path.
    void TerminalPage::_removeClassicTabForRemovedWorkspace(::WorkspaceModel::WorkspaceId ws)
    {
        auto it = _workspaceClassicTabs.find(ws);
        if (it == _workspaceClassicTabs.end())
        {
            return;
        }
        auto tab = it->second.get();
        _workspaceClassicTabs.erase(it);
        if (tab)
        {
            _RemoveTab(tab);
        }
    }

    // Phase 2 id-resolver foundation (#45/#44). Resolves a stable
    // WorkspaceId to the current display index of its classic Tab. The
    // WorkspaceChange arms (ActiveWorkspaceChanged / TabDecorationUpdated)
    // used to carry a display index that the view consumed via a positional
    // cast, baking in the Phase-1 "workspace display index == classic tab
    // index" invariant. They now carry the stable id and resolve it here.
    //
    // Every failure mode collapses to std::nullopt rather than a guessed
    // index, so an unknown / stale id can never select or decorate the wrong
    // tab:
    //   - no registry entry for `ws`           -> nullopt
    //   - the weak_ref<Tab> has expired         -> nullopt
    //   - the Tab is no longer present in _tabs  -> nullopt (via _GetTabIndex)
    std::optional<std::uint32_t> TerminalPage::_classicTabIndexForWorkspace(::WorkspaceModel::WorkspaceId ws) const
    {
        if (!ws.valid())
        {
            return std::nullopt;
        }
        const auto it = _workspaceClassicTabs.find(ws);
        if (it == _workspaceClassicTabs.end())
        {
            return std::nullopt;
        }
        const auto tab = it->second.get();
        if (!tab)
        {
            return std::nullopt;
        }
        return _GetTabIndex(tab);
    }

    // Big-flip Slice C (#54): true when `ws` already has a registered classic
    // Tab whose weak_ref still resolves. The TabAdded arm uses this to tell a
    // new workspace's FIRST tab (no entry yet) apart from an ADDITIONAL tab in
    // an existing leaf (entry present) — see the header comment. We require the
    // weak_ref to still resolve so a closed-then-reopened workspace id can't
    // false-positive on a stale entry.
    bool TerminalPage::_workspaceHasClassicTab(::WorkspaceModel::WorkspaceId ws) const
    {
        if (!ws.valid())
        {
            return false;
        }
        const auto it = _workspaceClassicTabs.find(ws);
        if (it == _workspaceClassicTabs.end())
        {
            return false;
        }
        return static_cast<bool>(it->second.get());
    }

    // Routed from the Tab::Closed handler (registered in
    // _InitializeTab) when the workspaces flag is on. The classic Tab
    // raised Closed via tab.Close(); instead of doing the classic
    // _RemoveTab directly we route through the model so the cascade /
    // active-workspace fallback runs. The model emits WorkspaceRemoved,
    // whose arm calls _RemoveTab against the same Tab.
    //
    // If we can't locate a workspace id for this Tab in the registry
    // (defensive: the Tab pre-dates the workspace machinery, or the
    // registry was lost across a settings reload), fall back to the
    // classic _RemoveTab so the user still sees their tab close.
    void TerminalPage::_closeTabViaWorkspaceModel(const winrt::TerminalApp::Tab& tab)
    {
        ::WorkspaceModel::WorkspaceId owningWs{};
        for (const auto& [wsId, weakTab] : _workspaceClassicTabs)
        {
            if (auto bound = weakTab.get(); bound && bound == tab)
            {
                owningWs = wsId;
                break;
            }
        }
        if (!owningWs.valid())
        {
            _RemoveTab(tab);
            return;
        }

        auto newState = ::WorkspaceModel::closeWorkspace(_workspaceModelState, owningWs);
        _applyWorkspaceAction(std::move(newState));
    }

    winrt::TerminalApp::Tab TerminalPage::_openProfileTabForWorkspace(const std::array<std::uint8_t, 16>& profileBytes)
    {
        // The 16-byte profile bytes are the canonical winrt::guid
        // in-memory layout on Windows; reinterpret to recover the
        // winrt::guid, then format as the bracketed-GUID-string that
        // NewTerminalArgs::Profile accepts.
        winrt::guid g{};
        static_assert(sizeof(winrt::guid) == 16, "winrt::guid must be 16 bytes");
        std::memcpy(&g, profileBytes.data(), 16);

        NewTerminalArgs newTerminalArgs{};
        newTerminalArgs.Profile(::Microsoft::Console::Utils::GuidToString(g));

        // Same spawn-failure contract as _openDefaultTabForWorkspace:
        // return the newly-appended Tab, or nullptr if _OpenNewTab didn't
        // add one, so the caller skips registry binding rather than
        // mis-binding the new workspace to a pre-existing Tab.
        const auto sizeBefore = _tabs.Size();
        LOG_IF_FAILED(_OpenNewTab(newTerminalArgs));
        const auto sizeAfter = _tabs.Size();
        if (sizeAfter <= sizeBefore)
        {
            return nullptr;
        }
        return _tabs.GetAt(sizeAfter - 1);
    }

    void TerminalPage::_applyTabDecoration(std::uint32_t classicTabIdx,
                                           const std::string& customTitle,
                                           const std::optional<::WorkspaceModel::Color>& runtimeColor)
    {
        if (classicTabIdx >= _tabs.Size())
        {
            return;
        }
        auto tab = _GetTabImpl(_tabs.GetAt(classicTabIdx));
        if (!tab)
        {
            return;
        }

        // Title: empty model string == "reset to the dynamic title".
        if (customTitle.empty())
        {
            tab->ResetTabText();
        }
        else
        {
            tab->SetTabText(winrt::hstring{ til::u8u16(customTitle) });
        }

        // Color: nullopt == reset.
        if (runtimeColor.has_value())
        {
            const auto& c = runtimeColor.value();
            winrt::Windows::UI::Color uiColor{};
            uiColor.A = c.a;
            uiColor.R = c.r;
            uiColor.G = c.g;
            uiColor.B = c.b;
            tab->SetRuntimeTabColor(uiColor);
        }
        else
        {
            tab->ResetRuntimeTabColor();
        }
    }

    std::optional<::WorkspaceModel::TabId> TerminalPage::_focusedTabModelId() const
    {
        if (!_workspaceModelState)
        {
            return std::nullopt;
        }
        const auto focusedIdx = _GetFocusedTabIndex();
        if (!focusedIdx)
        {
            return std::nullopt;
        }
        const auto& workspaces = _workspaceModelState->workspaces_view();
        const auto idx = static_cast<std::size_t>(focusedIdx.value());
        if (idx >= workspaces.size())
        {
            return std::nullopt;
        }
        // Phase 1: each workspace has exactly one leaf with exactly one
        // tab; the workspace's display position equals the classic tab
        // index.
        const auto leaves = _workspaceModelState->leaves(workspaces[idx].id);
        if (leaves.empty() || leaves[0]->tabs.empty())
        {
            return std::nullopt;
        }
        return leaves[0]->tabs[0].id;
    }

    // Slice 6 review fix: resolves an arbitrary classic Tab to its
    // model TabId. The tab-strip context menu fires rename / color
    // actions with sender == the right-clicked tab, which need not be
    // the focused tab. Reuses the Phase 1 invariant (classic tab idx
    // == workspace display idx, one leaf and one tab per workspace),
    // but resolves the index from the supplied tab instead of the
    // focused index.
    //
    // Slice 7 (#20) is expected to introduce a TerminalApp::Tab ->
    // model TabId registry. If that registry is present at flag-on
    // launch, it would be the more direct mapping; today (#20 not yet
    // landed on main) we walk the classic _tabs collection.
    std::optional<::WorkspaceModel::TabId> TerminalPage::_modelIdForTab(const winrt::TerminalApp::Tab& tab) const
    {
        if (!_workspaceModelState)
        {
            return std::nullopt;
        }
        if (!tab)
        {
            return std::nullopt;
        }
        uint32_t tabIdx{};
        if (!_tabs.IndexOf(tab, tabIdx))
        {
            return std::nullopt;
        }
        const auto& workspaces = _workspaceModelState->workspaces_view();
        const auto idx = static_cast<std::size_t>(tabIdx);
        if (idx >= workspaces.size())
        {
            return std::nullopt;
        }
        const auto leaves = _workspaceModelState->leaves(workspaces[idx].id);
        if (leaves.empty() || leaves[0]->tabs.empty())
        {
            return std::nullopt;
        }
        return leaves[0]->tabs[0].id;
    }

    void TerminalPage::_splitFocusedPaneForWorkspace(::WorkspaceModel::Axis axis,
                                                     double ratio)
    {
        // Drive the classic _SplitPane on the currently focused window-
        // level tab. Mirrors what _HandleSplitPane would do for a
        // default-profile split: create the new pane via
        // _MakePane(nullptr, ...) which yields a default-profile
        // terminal, and split at the requested axis/ratio.
        const auto focusedTabImpl = _GetFocusedTabImpl();
        if (!focusedTabImpl)
        {
            return;
        }
        const auto focusedTab = _GetFocusedTab();
        if (!focusedTab)
        {
            return;
        }
        const auto direction = axis == ::WorkspaceModel::Axis::Vertical
                                   ? SplitDirection::Right
                                   : SplitDirection::Down;
        const auto splitSize = static_cast<float>(ratio);
        auto newPane = _MakePane(nullptr, focusedTab, nullptr);
        if (!newPane)
        {
            return;
        }
        _SplitPane(focusedTabImpl, direction, splitSize, std::move(newPane));
    }

    void TerminalPage::_mirrorResizeIntoModel(const ResizeDirection& direction)
    {
        const auto splitIdOpt = _focusedSplitIdInActiveWorkspace();
        if (!splitIdOpt.has_value())
        {
            return;
        }
        // Mirror classic Pane::_Resize semantics:
        //   Right / Down → first (left/top) child grows.
        //   Left  / Up   → first child shrinks (right/bottom grows).
        // The model's SplitPane.ratio is the first child's fraction so
        // it moves in the same direction.
        const bool grewFirstChild =
            direction == ResizeDirection::Right ||
            direction == ResizeDirection::Down;
        const auto* node = _workspaceModelState->pane(*splitIdOpt);
        double currentRatio = 0.5;
        if (node)
        {
            if (const auto* split = std::get_if<::WorkspaceModel::SplitPane>(node))
            {
                currentRatio = split->ratio;
            }
        }
        constexpr double kRatioStep = 0.05;
        const double nextRatio = std::clamp(
            currentRatio + (grewFirstChild ? kRatioStep : -kRatioStep),
            0.0,
            1.0);
        auto newState = ::WorkspaceModel::resizePane(_workspaceModelState,
                                                     *splitIdOpt,
                                                     nextRatio);
        _applyWorkspaceAction(std::move(newState));
    }

    std::optional<::WorkspaceModel::PaneId> TerminalPage::_activeLeafModelId() const
    {
        if (!_workspaceModelState)
        {
            return std::nullopt;
        }
        const auto activeWsId = _workspaceModelState->activeWorkspaceId_view();
        if (!activeWsId.has_value())
        {
            return std::nullopt;
        }
        const auto* ws = _workspaceModelState->workspace(*activeWsId);
        if (!ws)
        {
            return std::nullopt;
        }
        return ws->activePaneId;
    }

    // Flag-on new-tab dispatch shared by every new-tab intent in
    // _HandleNewTab. The rule: if there is an active workspace AND an active
    // leaf, append a tab to that focused leaf via WorkspaceModel::newTab;
    // otherwise (no active workspace/leaf — startup / empty model) fall back
    // to newWorkspace so a new-tab intent still produces a tab rather than a
    // silent no-op. Either way we _applyWorkspaceAction the result, which
    // drives the diff -> view (strip VM append + ContentMounted factory).
    void TerminalPage::_dispatchNewTabInActiveLeafOrWorkspace(const ::WorkspaceModel::TerminalSpec& spec)
    {
        const auto activeWsId = _workspaceModelState
                                    ? _workspaceModelState->activeWorkspaceId_view()
                                    : std::nullopt;
        const auto activeLeaf = _activeLeafModelId();
        if (activeWsId.has_value() && activeLeaf.has_value())
        {
            auto created = ::WorkspaceModel::newTab(_workspaceModelState,
                                                    *activeWsId,
                                                    *activeLeaf,
                                                    ::WorkspaceModel::TabContent{ spec });
            _applyWorkspaceAction(std::move(created.state));
        }
        else
        {
            auto created = ::WorkspaceModel::newWorkspace(_workspaceModelState, std::string{}, ::WorkspaceModel::TabContent{ spec });
            _applyWorkspaceAction(std::move(created.state));
        }
    }

    std::optional<::WorkspaceModel::PaneId> TerminalPage::_focusedSplitIdInActiveWorkspace() const
    {
        if (!_workspaceModelState)
        {
            return std::nullopt;
        }
        const auto activeWsId = _workspaceModelState->activeWorkspaceId_view();
        if (!activeWsId.has_value())
        {
            return std::nullopt;
        }
        const auto* ws = _workspaceModelState->workspace(*activeWsId);
        if (!ws)
        {
            return std::nullopt;
        }
        // Address the split nearest the focused pane, not the workspace
        // root. XAML's classic resize handler moves the separator that is
        // the immediate parent of the focused pane; with nested splits the
        // outermost (root) split is a different separator, so recording
        // ratio drift against the root would diverge from what the user
        // sees and corrupt Phase 3 persistence replay. Walk UP one level
        // from the active leaf via WorkspaceModelData::parentOf.
        //
        // Returns std::nullopt when the active leaf has no parent split
        // (single-pane workspace); _mirrorResizeIntoModel no-ops on that.
        if (const auto* parent = _workspaceModelState->parentOf(ws->activePaneId))
        {
            return parent->id;
        }
        return std::nullopt;
    }

    // -----------------------------------------------------------------
    // Phase 2 Slice 2 (#46): the workspaces UI shell.
    // -----------------------------------------------------------------

    // Realize and show the flag-on chrome: the inline WorkspaceChrome (loaded
    // via FindName because it is x:Load="False") is lifted into the host
    // titlebar via SetTitleBarContent, which leaves the page's tab strip in
    // the client area UNDER the titlebar (reusing the existing tabs-under-
    // titlebar rendering — we simply do NOT hand the TabRow to the host). The
    // sidebar ItemsControl is realized and made visible so its Auto column takes
    // up width. Called only when _workspacesFlagEnabled() is true, so the
    // flag-off path never touches any of this and stays byte-for-byte upstream.
    void TerminalPage::_initializeWorkspaceShell()
    {
        // Realize the chrome (hamburger + new-workspace split-button) and hand
        // it to the host titlebar. The host's TitlebarControl supplies the
        // min/max/close caption buttons + drag bar around our content, so the
        // chrome only carries the workspace-specific buttons (inert this slice).
        //
        // The host sets the chrome as its titlebar ContentPresenter content,
        // which reparents it — and a ContentPresenter REJECTS a UIElement that
        // still has a logical parent ("element is already the child of another
        // element"). So the chrome must be detached from Root FIRST, exactly
        // like the classic tabs-in-titlebar path detaches the TabRow.
        //
        // We remove it directly from Root().Children() (its authored parent),
        // mirroring the proven classic path above — NOT via FrameworkElement
        // .Parent(). For an x:Load element freshly realized by FindName, the
        // logical Parent() can still report null on this pass, which silently
        // skipped the detach and left the chrome stuck in Root row 0 in the
        // client area (the host couldn't re-host an already-parented element),
        // which is the bug this fixes. (When no host is listening — e.g.
        // headless LocalTests — the chrome is still detached from Root here,
        // and the no-op raise just means it isn't re-hosted anywhere.)
        if (const auto chrome = FindName(L"WorkspaceChrome").try_as<UIElement>())
        {
            // Wire the new-workspace split button while the chrome is still in
            // the page's tree (FindName resolves against the page namescope; do
            // it before the chrome is handed to the host titlebar). The primary
            // button creates a default-profile workspace; the chevron flyout
            // offers one entry per profile. Creation reuses the Phase-1 create
            // path (newWorkspace -> _applyWorkspaceAction -> the TabAdded arm
            // materializes the backing classic tab), so this is not new
            // lifecycle code — just a new entry point into it.
            if (const auto newWsButton = FindName(L"NewWorkspaceButton").try_as<MUX::Controls::SplitButton>())
            {
                newWsButton.Click([weakThis{ get_weak() }](auto&&, auto&&) {
                    if (auto page{ weakThis.get() })
                    {
                        page->_createNewWorkspace(std::nullopt);
                    }
                });
                newWsButton.Flyout(_buildNewItemFlyout([weakThis{ get_weak() }](NewTerminalArgs args) {
                    if (auto page{ weakThis.get() })
                    {
                        page->_createWorkspaceFromArgs(args);
                    }
                }));
            }

            uint32_t index = 0;
            if (Root().Children().IndexOf(chrome, index))
            {
                Root().Children().RemoveAt(index);
            }
            SetTitleBarContent.raise(*this, chrome);
        }

        // Realize the sidebar and make its column take width. Flag-off it
        // stays x:Load="False" / Collapsed, so the Auto column is zero-width.
        _workspaceSidebar = FindName(L"WorkspaceSidebar").try_as<Controls::ItemsControl>();
        if (_workspaceSidebar)
        {
            _workspaceSidebar.Visibility(Visibility::Visible);
            // Bind the observable collection once; from here the sidebar is a
            // pure projection — only the by-id view-model mutators below touch
            // it, and the DataTemplate re-renders each row from its bindings.
            _workspaceSidebar.ItemsSource(_workspaceViewModels);

            // Seed a view-model for any workspace that already exists in the
            // model. The live WorkspaceAdded arm only appends once the shell is
            // initialized, so a workspace created before realization would
            // otherwise have no row. The model is the source of truth, so
            // re-project its current workspaces here in declared order.
            if (_workspaceModelState)
            {
                for (const auto& ws : _workspaceModelState->workspaces_view())
                {
                    _addWorkspaceVm(ws.id, ws.name, ws.color, ws.pinned);
                }
            }
        }

        // Big-flip Slice B (#54): realize the (collapsed) content host that
        // sits inside TabContent behind the classic content. It is
        // x:Load="False" so the flag-off path never realizes it; here on the
        // flag-on path we materialize it via FindName, exactly like the
        // sidebar/chrome above. We do NOT change its Visibility — it stays
        // Collapsed this slice, so the user keeps seeing only the classic
        // _tabContent display. _attachContentToWorkspaceHost parents the active
        // workspace's content into it.
        _workspaceContentHost = FindName(L"WorkspaceContentHost").try_as<Controls::Grid>();

        // Big-flip per-pane strip Slice 1 (#54): the inline PaneTabStrip
        // template-source ListView was retired — each projected leaf now builds
        // its own TabStripView UserControl (which carries the row template,
        // horizontal ItemsPanel, scroll settings, and SelectionMode internally),
        // so there is no longer a shared XAML strip to FindName/realize here.

        // Big-flip Slice D (#54): realize the (invisible) projected-pane-tree
        // container that lives in the host's star row. It is x:Load="False" so
        // the flag-off path never realizes it; here on the flag-on path we
        // materialize it via FindName like the strip/host above. The split arms
        // rebuild its single child from the active workspace's model pane tree.
        // It stays inside the Collapsed host, so this changes nothing visible
        // this slice — the classic Pane tree remains the displayed split.
        _workspacePaneTreeRoot = FindName(L"WorkspacePaneTreeRoot").try_as<Controls::Grid>();

        // Big-flip Slice F-5 (#54): THE CUTOVER. Flip the projected pane-tree
        // host VISIBLE and make it the SOLE child of _tabContent — the model's
        // pane tree (per-leaf strips + content hosts) is now what the user sees,
        // not the classic Tab. Flag-on, the classic _OpenNewTab path no longer
        // runs (apply(TabAdded)/apply(LeafPaneCreated) gate it out), so no
        // classic Tab is ever built and the classic _UpdatedSelectedTab swap
        // (which clears _tabContent's children) never fires. We therefore clear
        // _tabContent ONCE here and append only the host, establishing the
        // single-render invariant for the life of the page. Rollback is flipping
        // the flag off: this whole method is unreachable then, the host stays
        // x:Load="False" / Collapsed, and the classic display is byte-for-byte
        // upstream.
        if (_workspaceContentHost && _tabContent)
        {
            _workspaceContentHost.Visibility(Visibility::Visible);

            // Detach the host from any prior parent, then make it _tabContent's
            // sole child. The host is the only thing _tabContent renders flag-on.
            if (const auto priorParent = _workspaceContentHost.Parent().try_as<Controls::Panel>())
            {
                uint32_t idx = 0;
                if (priorParent.Children().IndexOf(_workspaceContentHost, idx))
                {
                    priorParent.Children().RemoveAt(idx);
                }
            }
            _tabContent.Children().Clear();
            _tabContent.Children().Append(_workspaceContentHost);
        }
    }

    // Big-flip Slice B (#54): parent `content`'s GetRoot() into the collapsed
    // WorkspaceContentHost as its SOLE child. The host shares TabContent's cell
    // with the classic content, but it is Collapsed, so this changes nothing
    // visible — the classic _tabContent swap still owns the display this slice.
    //
    // The classic _UpdatedSelectedTab swap does _tabContent.Children().Clear()
    // then appends the selected tab's content; that Clear() also drops THIS
    // host from _tabContent. So we re-append the host into _tabContent when it
    // has been cleared away before attaching, restoring the plumbing the
    // classic swap removed without touching the classic swap itself.
    //
    // Reparenting a FrameworkElement that still has a parent throws ("element
    // is already the child of another element"); we clear the host's children
    // first so a re-attach of the same or a different root is always safe.
    void TerminalPage::_attachContentToWorkspaceHost(const winrt::TerminalApp::IPaneContent& content)
    {
        if (!_workspaceContentHost || !_tabContent || !content)
        {
            return;
        }

        // Big-flip Slice F-0 (#54): the single shared WorkspaceContentHost is now
        // the OUTER WRAPPER around the projected pane tree — content no longer
        // lands here directly (a single element can't represent a split). The
        // per-leaf attach (_attachContentToLeafHost), driven by the same arms,
        // parents each leaf's content into its own per-leaf host INSIDE the
        // WorkspacePaneTreeRoot. This helper now only maintains the host's
        // plumbing: re-append the host into _tabContent (the classic
        // _UpdatedSelectedTab swap clears _tabContent's children, dropping the
        // host) and ensure its declared child WorkspacePaneTreeRoot is present,
        // so the projected tree (and its per-leaf hosts) is always reachable
        // behind the still-Collapsed host. `content` being non-null is the
        // caller's signal that the active workspace has content to display; the
        // arms record _hostContentId alongside this call.
        uint32_t hostIndex = 0;
        if (!_tabContent.Children().IndexOf(_workspaceContentHost, hostIndex))
        {
            _tabContent.Children().Append(_workspaceContentHost);
        }

        // The classic swap's _tabContent.Children().Clear() does NOT clear the
        // host's OWN children, so WorkspacePaneTreeRoot normally stays put; this
        // is a defensive re-attach in case a code path emptied the host. The
        // pane-tree root is the host's sole structural child; the per-leaf hosts
        // (and their content) live within it.
        if (_workspacePaneTreeRoot)
        {
            uint32_t treeIndex = 0;
            if (!_workspaceContentHost.Children().IndexOf(_workspacePaneTreeRoot, treeIndex))
            {
                // Detach the tree root from any prior parent before re-parenting.
                if (const auto priorParent = _workspacePaneTreeRoot.Parent().try_as<Controls::Panel>())
                {
                    uint32_t idx = 0;
                    if (priorParent.Children().IndexOf(_workspacePaneTreeRoot, idx))
                    {
                        priorParent.Children().RemoveAt(idx);
                    }
                }
                _workspaceContentHost.Children().Append(_workspacePaneTreeRoot);
            }
        }
    }

    winrt::Windows::UI::Xaml::FrameworkElement TerminalPage::_workspaceHostChildForTest() const
    {
        if (!_workspaceContentHost || _workspaceContentHost.Children().Size() == 0)
        {
            return nullptr;
        }
        return _workspaceContentHost.Children().GetAt(0).try_as<winrt::Windows::UI::Xaml::FrameworkElement>();
    }

    // Create a new workspace. nullopt profile -> the default profile; otherwise
    // the workspace's initial tab uses the given profile. Flag-on only. Mirrors
    // the _HandleNewTab flag-on dispatch: a workspace is the Phase-1 unit that
    // materializes one classic backing tab through the TabAdded arm.
    void TerminalPage::_createNewWorkspace(std::optional<winrt::guid> profile)
    {
        if (!_workspacesFlagEnabled())
        {
            return;
        }

        ::WorkspaceModel::TerminalSpec spec{};
        if (profile)
        {
            const auto g = *profile;
            std::memcpy(spec.profile.data(), &g, sizeof(g));
        }

        auto created = ::WorkspaceModel::newWorkspace(_workspaceModelState,
                                                      std::string{},
                                                      ::WorkspaceModel::TabContent{ spec });
        _applyWorkspaceAction(std::move(created.state));
    }

    // Bridge from the shared new-item dropdown's profile callback to the
    // workspace create path. Resolves the chosen NewTerminalArgs to a concrete
    // profile (so "remaining/match" collection entries and explicit profile
    // entries both land on the right profile) and creates a workspace whose
    // initial tab uses it; falls back to the default-profile workspace when no
    // profile resolves.
    void TerminalPage::_createWorkspaceFromArgs(NewTerminalArgs args)
    {
        const auto profile{ _settings.GetProfileForArgs(args) };
        if (profile)
        {
            _createNewWorkspace(profile.Guid());
        }
        else
        {
            _createNewWorkspace(std::nullopt);
        }
    }

    // Translate a model Color (optional, RGBA) to a winrt::Windows::UI::Color.
    // Returns Transparent when unset; the row's HasColor flag is what actually
    // gates the swatch's visibility, so the value here is only meaningful when
    // HasColor is true.
    static winrt::Windows::UI::Color _toWinrtColor(const std::optional<::WorkspaceModel::Color>& color)
    {
        if (!color)
        {
            return Windows::UI::Colors::Transparent();
        }
        return winrt::Windows::UI::Color{ color->a, color->r, color->g, color->b };
    }

    // The inverse of _toWinrtColor: a winrt Color (ARGB) to the model's RGBA
    // Color. Used by the row color-picker handler before dispatching
    // setWorkspaceColor.
    static ::WorkspaceModel::Color _toModelColor(const winrt::Windows::UI::Color& color) noexcept
    {
        return ::WorkspaceModel::Color{ color.R, color.G, color.B, color.A };
    }

    // Append one view-model for a newly-added workspace, in declared order
    // (WorkspaceAdded arrives in diff Phase 1, before any ActiveWorkspaceChanged,
    // and in workspace display order). The view-model carries its stable
    // WorkspaceId.v as Id, so the active highlight, removal, and metadata update
    // resolve by id identity rather than positional index. New rows start
    // inactive; the ActiveWorkspaceChanged arm flips IsActive.
    void TerminalPage::_addWorkspaceVm(::WorkspaceModel::WorkspaceId ws, const std::string& name, const std::optional<::WorkspaceModel::Color>& color, bool pinned)
    {
        if (!ws.valid())
        {
            return;
        }

        winrt::TerminalApp::WorkspaceViewModel vm{};
        vm.Id(ws.v);
        vm.Title(winrt::hstring{ til::u8u16(name.empty() ? std::string{ "Workspace" } : name) });
        vm.HasColor(color.has_value());
        vm.Color(_toWinrtColor(color));
        vm.IsPinned(pinned);
        vm.IsActive(false);

        // Subscribe to the row's intent signals (#52, Stage 3a). The VM never
        // touches the model; it raises these and the page dispatches the model
        // action, so the resulting diff re-projects the row (the
        // WorkspaceMetadataUpdated arm calls _updateWorkspaceVm). We capture the
        // page weakly; the sender VM carries its stable Id so we resolve the
        // WorkspaceId from the event args rather than the captured local. These
        // run on the UI thread.
        vm.RenameCommitted([weakThis = get_weak()](const winrt::TerminalApp::WorkspaceViewModel& sender, const winrt::hstring& newName) {
            auto page{ weakThis.get() };
            if (!page)
            {
                return;
            }
            if (!page->_workspacesFlagEnabled() || !page->_workspaceModelState)
            {
                return;
            }
            const ::WorkspaceModel::WorkspaceId id{ sender.Id() };
            if (!id.valid())
            {
                return;
            }
            auto newState = ::WorkspaceModel::renameWorkspace(page->_workspaceModelState, id, winrt::to_string(newName));
            page->_applyWorkspaceAction(std::move(newState));
        });

        vm.PinToggleRequested([weakThis = get_weak()](const winrt::TerminalApp::WorkspaceViewModel& sender, bool desired) {
            auto page{ weakThis.get() };
            if (!page)
            {
                return;
            }
            if (!page->_workspacesFlagEnabled() || !page->_workspaceModelState)
            {
                return;
            }
            const ::WorkspaceModel::WorkspaceId id{ sender.Id() };
            if (!id.valid())
            {
                return;
            }
            auto newState = ::WorkspaceModel::setWorkspacePinned(page->_workspaceModelState, id, desired);
            page->_applyWorkspaceAction(std::move(newState));
        });

        // Color picker (#52, Stage 3b). The VM owns/shows the picker (view
        // layer) and raises ColorCommitted with the chosen color, or a null
        // IReference to clear it. We map that to setWorkspaceColor(value) /
        // setWorkspaceColor(std::nullopt); the WorkspaceMetadataUpdated diff arm
        // re-projects the swatch.
        vm.ColorCommitted([weakThis = get_weak()](const winrt::TerminalApp::WorkspaceViewModel& sender, const winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color>& color) {
            auto page{ weakThis.get() };
            if (!page)
            {
                return;
            }
            if (!page->_workspacesFlagEnabled() || !page->_workspaceModelState)
            {
                return;
            }
            const ::WorkspaceModel::WorkspaceId id{ sender.Id() };
            if (!id.valid())
            {
                return;
            }
            std::optional<::WorkspaceModel::Color> colorOpt{ std::nullopt };
            if (color)
            {
                colorOpt = _toModelColor(color.Value());
            }
            auto newState = ::WorkspaceModel::setWorkspaceColor(page->_workspaceModelState, id, colorOpt);
            page->_applyWorkspaceAction(std::move(newState));
        });

        // Single-tap activation (#52, Stage 3c). The row raises ActivateRequested
        // (the args carry no payload — the workspace is resolved from the
        // sender's stable Id). We dispatch switchToWorkspace; the resulting
        // ActiveWorkspaceChanged diff arm flips the active highlight
        // (_setActiveWorkspaceVm) and selects the workspace's classic tab
        // (_SelectTab). Activating the already-active workspace is a harmless
        // no-op (switchToWorkspace re-finalizes; the diff emits at most a no-op
        // ActiveWorkspaceChanged for the unchanged id).
        vm.ActivateRequested([weakThis = get_weak()](const winrt::TerminalApp::WorkspaceViewModel& sender, const winrt::Windows::Foundation::IInspectable& /*args*/) {
            auto page{ weakThis.get() };
            if (!page)
            {
                return;
            }
            if (!page->_workspacesFlagEnabled() || !page->_workspaceModelState)
            {
                return;
            }
            const ::WorkspaceModel::WorkspaceId id{ sender.Id() };
            if (!id.valid())
            {
                return;
            }
            auto next = ::WorkspaceModel::switchToWorkspace(page->_workspaceModelState, id);
            page->_applyWorkspaceAction(std::move(next));
        });

        _workspaceViewModels.Append(vm);
    }

    // Remove the view-model that backs a removed workspace, located by its
    // stored Id (id identity, not positional).
    void TerminalPage::_removeWorkspaceVm(::WorkspaceModel::WorkspaceId ws)
    {
        if (!ws.valid())
        {
            return;
        }
        for (uint32_t i = 0; i < _workspaceViewModels.Size(); ++i)
        {
            if (_workspaceViewModels.GetAt(i).Id() == ws.v)
            {
                _workspaceViewModels.RemoveAt(i);
                return;
            }
        }
    }

    // Mark the view-model whose Id matches the newly-active workspace active,
    // clearing IsActive on all others. Resolution is by id identity (the
    // S1-resolver philosophy): we never index the collection positionally to
    // find the active row.
    void TerminalPage::_setActiveWorkspaceVm(::WorkspaceModel::WorkspaceId active)
    {
        for (const auto& vm : _workspaceViewModels)
        {
            vm.IsActive(active.valid() && vm.Id() == active.v);
        }
    }

    // Re-project a surviving workspace's display metadata (name / color / pin)
    // onto its view-model, located by id identity. The workspace analogue of
    // _applyTabDecoration; the bound DataTemplate re-renders from the observable
    // property changes.
    void TerminalPage::_updateWorkspaceVm(::WorkspaceModel::WorkspaceId ws, const std::string& name, const std::optional<::WorkspaceModel::Color>& color, bool pinned)
    {
        if (!ws.valid())
        {
            return;
        }
        for (const auto& vm : _workspaceViewModels)
        {
            if (vm.Id() == ws.v)
            {
                vm.Title(winrt::hstring{ til::u8u16(name.empty() ? std::string{ "Workspace" } : name) });
                vm.HasColor(color.has_value());
                vm.Color(_toWinrtColor(color));
                vm.IsPinned(pinned);
                return;
            }
        }
    }

    // Re-sequence the observable view-model collection to match the model's new
    // display order (the WorkspaceReordered arm carries the full id-order, e.g.
    // after a pin floats a workspace). This is a pure projection: we resolve
    // each id to its EXISTING row view-model (preserving its observable state)
    // and Move it into place, never rebuilding rows from model data. We perform
    // a selection-sort of Move operations against the observable vector so the
    // bound ItemsControl animates each row to its target slot rather than
    // tearing the list down. Ids in `order` that have no matching row (or
    // surplus rows not named in `order`) are left untouched — the add/remove
    // arms own membership; this arm only reorders what already exists.
    void TerminalPage::_reorderWorkspaceVms(const std::vector<::WorkspaceModel::WorkspaceId>& order)
    {
        uint32_t target = 0;
        for (const auto& id : order)
        {
            if (!id.valid())
            {
                continue;
            }
            // Find the row currently holding this id at or after `target`.
            uint32_t cur = target;
            bool found = false;
            for (; cur < _workspaceViewModels.Size(); ++cur)
            {
                if (_workspaceViewModels.GetAt(cur).Id() == id.v)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                continue; // id has no row (membership arms own that) — skip.
            }
            if (cur != target)
            {
                const auto vm = _workspaceViewModels.GetAt(cur);
                _workspaceViewModels.RemoveAt(cur);
                _workspaceViewModels.InsertAt(target, vm);
            }
            ++target;
        }
    }

    // Big-flip Slice C (#54): resolve (creating on first use) the observable
    // strip collection for `leaf`. The per-leaf strips are keyed by PaneId;
    // each holds one PaneTabViewModel per model tab in that leaf, in declared
    // order. A pure projection store — only the arm-driven helpers below mutate
    // it.
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::TerminalApp::PaneTabViewModel>& TerminalPage::_paneTabStripForLeaf(::WorkspaceModel::PaneId leaf)
    {
        auto it = _paneTabStrips.find(leaf);
        if (it == _paneTabStrips.end())
        {
            it = _paneTabStrips.emplace(leaf, winrt::single_threaded_observable_vector<winrt::TerminalApp::PaneTabViewModel>()).first;
        }
        return it->second;
    }

    // Append one strip view-model for a tab projected into `leaf`, in declared
    // order (TabAdded arrives in diff Phase 1). The view-model carries its
    // stable TabId.v as Id, so activation / removal resolve by id identity. New
    // rows start inactive, EXCEPT when this tab is the leaf's model-active tab
    // (Big-flip Slice F-2, #54): in that case IsActive is seeded true at
    // append time, because ActiveTabChanged for tab index 0 is suppressed by
    // the diff engine when the new workspace's first tab is added (index 0 was
    // already the active index; no change to emit). The query resolves the
    // leaf's model PaneNode -> LeafPane -> activeTabIdx -> tabs[idx].id and
    // compares it to `tab` by id — not by positional "is it the first row". The
    // existing ActiveTabChanged handling for later switches remains intact and
    // must NOT double-activate (it calls _setActivePaneTabVm, which clears all
    // others). This creates NO classic Tab — the strip VM is the SOLE
    // representation of an additional leaf tab (the classic Tab for a new
    // workspace's first tab is created by the TabAdded arm before it calls
    // here). INVISIBLE this slice (host Collapsed).
    void TerminalPage::_appendPaneTabVm(::WorkspaceModel::PaneId leaf, ::WorkspaceModel::TabId tab, const std::string& title)
    {
        if (!leaf.valid() || !tab.valid())
        {
            return;
        }

        auto& strip = _paneTabStripForLeaf(leaf);

        // Idempotent: a re-project of an already-present tab id must not
        // duplicate the row (defensive — the arms emit one TabAdded per new
        // tab, but keep the projection a set on id).
        for (const auto& existing : strip)
        {
            if (existing.Id() == tab.v)
            {
                return;
            }
        }

        // Big-flip Slice F-2 (#54): seed IsActive from the model. Query the
        // leaf's PaneNode, cast to LeafPane, and check whether the tab at
        // activeTabIdx matches `tab`. This is the model-authoritative answer —
        // not "is it the first row appended" — so it stays correct if append
        // order ever changes.
        bool isActive = false;
        if (_workspaceModelState)
        {
            if (const auto* node = _workspaceModelState->pane(leaf))
            {
                if (const auto* leafPane = std::get_if<::WorkspaceModel::LeafPane>(node))
                {
                    if (!leafPane->tabs.empty() && leafPane->activeTabIdx < leafPane->tabs.size())
                    {
                        isActive = (leafPane->tabs[leafPane->activeTabIdx].id == tab);
                    }
                }
            }
        }

        winrt::TerminalApp::PaneTabViewModel vm{};
        vm.Id(tab.v);
        // Workspaces M2 (#54, ADR-001): `title` here is the model's
        // TabRecord.customTitle (TabAdded.customTitle) — the user-chosen rename
        // title, empty for a fresh tab. Seed it onto CustomTitle (custom-wins
        // precedence), and seed the LIVE title to the static "Tab" placeholder.
        // The live shell title arrives momentarily via
        // _bindTabChromeToContent → _setPaneTabTitleForTab (the Title setter, which
        // writes the live title); while a custom title is in force the computed
        // Title ignores it, so a rename survives the first live TitleChanged.
        vm.Title(winrt::hstring{ L"Tab" });
        if (!title.empty())
        {
            vm.CustomTitle(winrt::hstring{ til::u8u16(title) });
        }
        vm.IsActive(isActive);

        // Subscribe to the row's intent signals (Big-flip Slice C, #54). The VM
        // never touches the model; it raises these and the page dispatches the
        // model action, so the resulting diff re-projects the strip. We capture
        // the page weakly; the sender VM carries its stable Id so we resolve the
        // TabId from the event args. Production wiring of the VISIBLE triggers
        // (row tap / close affordance) is deferred to Slice F — these
        // subscriptions stand up the dispatch path now, but no on-screen control
        // raises them this slice (the strip is invisible in the Collapsed host).
        vm.ActivateRequested([weakThis = get_weak()](const winrt::TerminalApp::PaneTabViewModel& sender, const winrt::Windows::Foundation::IInspectable& /*args*/) {
            auto page{ weakThis.get() };
            if (!page)
            {
                return;
            }
            if (!page->_workspacesFlagEnabled() || !page->_workspaceModelState)
            {
                return;
            }
            const ::WorkspaceModel::TabId id{ sender.Id() };
            if (!id.valid())
            {
                return;
            }
            auto next = ::WorkspaceModel::selectTab(page->_workspaceModelState, id);
            page->_applyWorkspaceAction(std::move(next));
        });

        vm.CloseRequested([weakThis = get_weak()](const winrt::TerminalApp::PaneTabViewModel& sender, const winrt::Windows::Foundation::IInspectable& /*args*/) {
            auto page{ weakThis.get() };
            if (!page)
            {
                return;
            }
            if (!page->_workspacesFlagEnabled() || !page->_workspaceModelState)
            {
                return;
            }
            const ::WorkspaceModel::TabId id{ sender.Id() };
            if (!id.valid())
            {
                return;
            }
            auto next = ::WorkspaceModel::closeTab(page->_workspaceModelState, id);
            page->_applyWorkspaceAction(std::move(next));
        });

        // Workspaces M2 (#54, ADR-001): the rename intent. The strip's hosted
        // TabHeaderControl commits a new title → the VM raises RenameRequested
        // carrying the boxed title → we dispatch setTabTitle for this tab's id.
        // The resulting TabDecorationUpdated diff arm projects the new customTitle
        // back onto the VM's CustomTitle (model-as-truth — the committed title is
        // NEVER written onto the view here; it returns via the projection). This
        // mirrors the ActivateRequested/CloseRequested dispatch shape exactly.
        vm.RenameRequested([weakThis = get_weak()](const winrt::TerminalApp::PaneTabViewModel& sender, const winrt::Windows::Foundation::IInspectable& args) {
            auto page{ weakThis.get() };
            if (!page)
            {
                return;
            }
            if (!page->_workspacesFlagEnabled() || !page->_workspaceModelState)
            {
                return;
            }
            const ::WorkspaceModel::TabId id{ sender.Id() };
            if (!id.valid())
            {
                return;
            }
            // args is the boxed committed title (an empty string is a valid
            // "reset to live title" intent — the model stores empty customTitle
            // and the TabDecorationUpdated arm clears CustomTitle). unbox_value_or
            // yields an empty hstring if args is null/not a boxed string.
            const winrt::hstring newTitle{ winrt::unbox_value_or<winrt::hstring>(args, winrt::hstring{}) };
            auto next = ::WorkspaceModel::setTabTitle(page->_workspaceModelState, id, winrt::to_string(newTitle));
            page->_applyWorkspaceAction(std::move(next));
        });

        // Workspaces M1 (#54, ADR-001): the hover-highlight + separator hide are
        // now rendered natively by the MUX TabView, so the old hover-intent
        // subscriptions (HoverEnter/Exit → _recomputePaneTabSeparators) are gone.

        strip.Append(vm);
    }

    // Remove the strip view-model that backs a removed tab, located by its
    // stored Id (id identity, not positional). A no-op when the leaf has no
    // strip or the tab isn't projected. Leaves an emptied leaf's (now-empty)
    // collection in place — leaf-disappearance cleanup is a later slice
    // (single-leaf scope here); an empty collection projects an empty strip.
    void TerminalPage::_removePaneTabVm(::WorkspaceModel::PaneId leaf, ::WorkspaceModel::TabId tab)
    {
        if (!leaf.valid() || !tab.valid())
        {
            return;
        }
        const auto it = _paneTabStrips.find(leaf);
        if (it == _paneTabStrips.end())
        {
            return;
        }
        auto& strip = it->second;
        for (uint32_t i = 0; i < strip.Size(); ++i)
        {
            if (strip.GetAt(i).Id() == tab.v)
            {
                strip.RemoveAt(i);
                return;
            }
        }
    }

    // Mark the strip view-model whose Id matches the newly-active tab active,
    // clearing IsActive on all others in the SAME leaf. Resolution is by id
    // identity (the S1-resolver philosophy): we never index the collection
    // positionally to find the active row. A no-op when the leaf has no strip.
    void TerminalPage::_setActivePaneTabVm(::WorkspaceModel::PaneId leaf, ::WorkspaceModel::TabId tab)
    {
        const auto it = _paneTabStrips.find(leaf);
        if (it == _paneTabStrips.end())
        {
            return;
        }
        for (const auto& vm : it->second)
        {
            vm.IsActive(tab.valid() && vm.Id() == tab.v);
        }
    }

    // Live pane-tab title (#54): set the strip VM whose stable Id matches `tab`
    // to `title`. The ContentMounted arm that drives this carries only a TabId
    // (no leaf), so we resolve the VM across ALL leaf strips by id identity —
    // each tab id is unique, so at most one VM matches. Returns the matching VM
    // (empty when none was found) so the caller can verify the bind by identity
    // without re-scanning. A no-op when no strip holds the tab (e.g. content
    // mounted for a tab whose strip row was never appended, or already pruned).
    winrt::TerminalApp::PaneTabViewModel TerminalPage::_setPaneTabTitleForTab(::WorkspaceModel::TabId tab, const winrt::hstring& title)
    {
        if (!tab.valid())
        {
            return nullptr;
        }
        for (const auto& [leaf, strip] : _paneTabStrips)
        {
            for (const auto& vm : strip)
            {
                if (vm.Id() == tab.v)
                {
                    vm.Title(title);
                    return vm;
                }
            }
        }
        return nullptr;
    }

    // Workspaces M2 (#54, ADR-001): set the strip VM whose stable Id matches
    // `tab` to carry the model's customTitle (the rename result). Mirrors
    // _setPaneTabTitleForTab — resolve across ALL leaf strips by id identity (the
    // TabDecorationUpdated diff arm carries a TabId directly) — but writes
    // CustomTitle, not the live title. The VM's computed Title shows the custom
    // title when it is non-empty, else the live shell title; so a rename
    // surfaces immediately AND a later live TitleChanged cannot clobber it. An
    // empty customTitle clears the override (reset to the live title). A pure
    // downstream projection — the committed rename returns through here, not
    // written on the view at the intent site. Returns the matching VM (empty when
    // none).
    winrt::TerminalApp::PaneTabViewModel TerminalPage::_setPaneTabCustomTitleForTab(::WorkspaceModel::TabId tab, const winrt::hstring& customTitle)
    {
        if (!tab.valid())
        {
            return nullptr;
        }
        for (const auto& [leaf, strip] : _paneTabStrips)
        {
            for (const auto& vm : strip)
            {
                if (vm.Id() == tab.v)
                {
                    vm.CustomTitle(customTitle);
                    return vm;
                }
            }
        }
        return nullptr;
    }

    // Slice 2a.2 (#54): set the strip VM whose stable Id matches `tab` to carry
    // the content's background COLOR. Mirrors _setPaneTabTitleForTab exactly:
    // resolve the VM across ALL leaf strips by id identity (the ContentMounted
    // arm carries only a TabId), set the VM's Background, and return the matching
    // VM (empty when none). The selected pane-tab's connected background x:Binds
    // to this so the active tab merges into the terminal content the way the
    // classic WT focused tab does ("tab.background = terminalBackground").
    //
    // We EXTRACT the color from `contentBrush` and build a FRESH SolidColorBrush
    // — we never store/share the content's own brush, which may be acrylic
    // (classic does the same, Tab.cpp: ColorFromBrush then a flat tab color —
    // "we don't really want to have the tab items themselves be acrylic"). When
    // `contentBrush` is null (no live brush yet) we set the VM Background to
    // null (the Border renders neutral) — we must NOT call ColorFromBrush on a
    // null brush (it dereferences the brush).
    //
    // Slice 2a.2 follow-up: we short-circuit on color equality. This runs on the
    // content's TitleChanged (per-prompt for many shells), and WINRT_OBSERVABLE_PROPERTY's
    // setter guards Background only by REFERENCE identity — a fresh SolidColorBrush
    // is never reference-equal to the stored one, so without this check we'd
    // re-allocate a brush and raise PropertyChanged(Background) on every refresh
    // even when the color is unchanged, churning the bound Border re-eval. So we
    // only push a new brush when the resolved color actually differs from what the
    // VM already carries. This does NOT change the rendered color.
    winrt::TerminalApp::PaneTabViewModel TerminalPage::_setPaneTabBackgroundForTab(::WorkspaceModel::TabId tab, const winrt::Windows::UI::Xaml::Media::Brush& contentBrush)
    {
        if (!tab.valid())
        {
            return nullptr;
        }

        for (const auto& [leaf, strip] : _paneTabStrips)
        {
            for (const auto& vm : strip)
            {
                if (vm.Id() == tab.v)
                {
                    if (contentBrush == nullptr)
                    {
                        // Only clear when the VM currently carries a brush; a
                        // null→null set would raise PropertyChanged needlessly.
                        if (vm.Background() != nullptr)
                        {
                            vm.Background(nullptr);
                        }
                        return vm;
                    }

                    const til::color newColor{ ThemeColor::ColorFromBrush(contentBrush) };
                    if (const auto current = vm.Background().try_as<Media::SolidColorBrush>())
                    {
                        if (til::color{ current.Color() } == newColor)
                        {
                            // Same color already in place — no churn.
                            return vm;
                        }
                    }
                    vm.Background(Media::SolidColorBrush{ static_cast<winrt::Windows::UI::Color>(newColor) });
                    return vm;
                }
            }
        }
        return nullptr;
    }

    // Big-flip Slice C (#54): flip the strip's active row to the tab at model
    // index `idx` in `leaf`, and return the TabId now made active. The strip
    // holds one VM per model tab in declared order, so `idx` indexes it
    // directly; we resolve the target VM's stable Id, then flip IsActive across
    // the leaf's rows by id identity (so the active highlight is single and
    // correct). The strip chrome is a PURE PROJECTION of IsActive: the
    // selected-tab background + selection indicator + foreground all x:Bind to
    // PaneTabViewModel.IsActive (OneWay). The ListView's own SelectedItem is NOT
    // bound to IsActive and is not a source of truth — it never drives the
    // chrome and never writes back to the model (Slice 2a, #54).
    // Returns an invalid TabId when the leaf has no strip or `idx` is out of
    // range, so the caller skips the host swap rather than guessing.
    ::WorkspaceModel::TabId TerminalPage::_activatePaneTabByIndex(::WorkspaceModel::PaneId leaf, std::size_t idx)
    {
        const auto it = _paneTabStrips.find(leaf);
        if (it == _paneTabStrips.end())
        {
            return ::WorkspaceModel::TabId{};
        }
        auto& strip = it->second;
        if (idx >= strip.Size())
        {
            return ::WorkspaceModel::TabId{};
        }
        const ::WorkspaceModel::TabId active{ strip.GetAt(static_cast<uint32_t>(idx)).Id() };
        _setActivePaneTabVm(leaf, active);
        return active;
    }

    uint32_t TerminalPage::_paneTabStripSizeForTest(::WorkspaceModel::PaneId leaf) const
    {
        const auto it = _paneTabStrips.find(leaf);
        if (it == _paneTabStrips.end())
        {
            return 0;
        }
        return it->second.Size();
    }

    std::optional<uint64_t> TerminalPage::_activePaneTabIdForTest(::WorkspaceModel::PaneId leaf) const
    {
        const auto it = _paneTabStrips.find(leaf);
        if (it == _paneTabStrips.end())
        {
            return std::nullopt;
        }
        for (const auto& vm : it->second)
        {
            if (vm.IsActive())
            {
                return vm.Id();
            }
        }
        return std::nullopt;
    }

    // Returns the Title of the first VM in `leaf`'s strip — cheap string read, no
    // rendering. Used by tests to pin that _appendPaneTabVm stores the title
    // correctly (and therefore that the per-leaf strip ListView's
    // Text="{x:Bind Title}" DataTemplate binding will show the right label).
    std::optional<winrt::hstring> TerminalPage::_paneTabStripFirstTitleForTest(::WorkspaceModel::PaneId leaf) const
    {
        const auto it = _paneTabStrips.find(leaf);
        if (it == _paneTabStrips.end() || it->second.Size() == 0)
        {
            return std::nullopt;
        }
        return it->second.GetAt(0).Title();
    }

    // Slice 2a.2 (#54): the background COLOR of the first VM in `leaf`'s strip,
    // or nullopt when the leaf has no strip / the VM's Background is unset (the
    // pre-mount state). Extracts the color from the VM's projected
    // SolidColorBrush so a test can assert the projection followed the content's
    // bg (no rendering, no layout pass). Mirrors _paneTabStripFirstTitleForTest.
    std::optional<til::color> TerminalPage::_paneTabStripFirstBackgroundForTest(::WorkspaceModel::PaneId leaf) const
    {
        const auto it = _paneTabStrips.find(leaf);
        if (it == _paneTabStrips.end() || it->second.Size() == 0)
        {
            return std::nullopt;
        }
        const auto brush = it->second.GetAt(0).Background();
        if (brush == nullptr)
        {
            return std::nullopt;
        }
        return til::color{ ThemeColor::ColorFromBrush(brush) };
    }

    // Slice 2a.2 follow-up (#54): the first VM in `leaf`'s strip (nullptr when
    // the leaf has no strip / it's empty). Lets a test subscribe to the VM's
    // PropertyChanged and count Background raises, to pin the color-equality
    // short-circuit (no raise on an unchanged-color refresh; a raise on a real
    // color change).
    winrt::TerminalApp::PaneTabViewModel TerminalPage::_paneTabStripFirstVmForTest(::WorkspaceModel::PaneId leaf) const
    {
        const auto it = _paneTabStrips.find(leaf);
        if (it == _paneTabStrips.end() || it->second.Size() == 0)
        {
            return nullptr;
        }
        return it->second.GetAt(0);
    }

    // The strip VM at `index` in `leaf` (nullptr when no strip / index out of
    // range). Lets a headless test read a row's projected state (Id / IsActive /
    // Title) without a layout pass.
    winrt::TerminalApp::PaneTabViewModel TerminalPage::_paneTabStripVmAtForTest(::WorkspaceModel::PaneId leaf, uint32_t index) const
    {
        const auto it = _paneTabStrips.find(leaf);
        if (it == _paneTabStrips.end() || index >= it->second.Size())
        {
            return nullptr;
        }
        return it->second.GetAt(index);
    }

    // Big-flip Slice D (#54): rebuild the active workspace's projected SPLIT pane
    // tree inside the (Collapsed) WorkspacePaneTreeRoot. See the header comment
    // for the rebuild-vs-incremental rationale; in short we re-derive the whole
    // active-workspace subtree from the model's `root` (the source of truth) and
    // REUSE each leaf's existing Slice-C strip collection by PaneId, so the
    // projection can never diverge from the model and no VM/content is dropped.
    void TerminalPage::_rebuildActiveWorkspacePaneTree()
    {
        if (!_workspacePaneTreeRoot || !_workspaceModelState)
        {
            return;
        }

        // Big-flip Slice F-0 (#54): the rebuild discards the old projected tree
        // and its per-leaf content hosts, so drop the host map first. We DETACH
        // each old host's content child BEFORE discarding the host, so every
        // content root is left parent-free — the subsequent re-attach
        // (_reattachLeafContents) can then re-parent it into its fresh host
        // without hitting the "element is already the child of another element"
        // throw (a FrameworkElement has a single parent). The re-projection below
        // re-registers a fresh host for every leaf still in the tree (a
        // collapsed-away leaf's host is therefore pruned), and the re-attach
        // re-populates them.
        for (auto& [leaf, host] : _paneContentHosts)
        {
            if (host)
            {
                host.Children().Clear();
            }
        }
        _paneContentHosts.clear();

        const auto activeWsId = _workspaceModelState->activeWorkspaceId_view();
        if (!activeWsId.has_value())
        {
            _workspacePaneTreeRoot.Children().Clear();
            return;
        }
        const auto* ws = _workspaceModelState->workspace(*activeWsId);
        if (!ws)
        {
            _workspacePaneTreeRoot.Children().Clear();
            return;
        }

        // Big-flip Slice F-2 (#54): prune stale _paneTabStrips entries for
        // PaneIds that no longer exist ANYWHERE in the model. Dead entries
        // accumulate after a SplitPaneCollapsed (the collapsed-away leaf's
        // strip collection was left in place by Slice D pending this GC). We
        // scan ALL workspaces' leaves (not just the active workspace) because
        // inactive workspaces retain their leaf strips so a workspace switch
        // can immediately resolve their active tab. Pruning by model-global
        // existence is the correct boundary: a PaneId is dead only when it has
        // been removed from the pane tree entirely (collapsed away), not merely
        // when its workspace is inactive.
        {
            std::unordered_set<::WorkspaceModel::PaneId> liveSet;
            for (const auto& anyWs : _workspaceModelState->workspaces_view())
            {
                for (const auto* lp : _workspaceModelState->leaves(anyWs.id))
                {
                    liveSet.insert(lp->id);
                }
            }
            for (auto it = _paneTabStrips.begin(); it != _paneTabStrips.end();)
            {
                if (liveSet.find(it->first) == liveSet.end())
                {
                    it = _paneTabStrips.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // Resolve the root pane's id, then project from it. Both LeafPane and
        // SplitPane carry an `id`, so we read it off the variant.
        ::WorkspaceModel::PaneId rootId{};
        if (const auto* leaf = std::get_if<::WorkspaceModel::LeafPane>(&ws->root))
        {
            rootId = leaf->id;
        }
        else if (const auto* split = std::get_if<::WorkspaceModel::SplitPane>(&ws->root))
        {
            rootId = split->id;
        }

        _workspacePaneTreeRoot.Children().Clear();
        if (const auto projected = _projectPaneNode(rootId))
        {
            _workspacePaneTreeRoot.Children().Append(projected);
        }
    }

    // Project the PaneNode with id `nodeId` (resolved from the model) into a
    // FrameworkElement, recursing for splits. Each node carries its PaneId.v in
    // Tag() for id-based resolution. Returns nullptr for an unknown node.
    winrt::Windows::UI::Xaml::FrameworkElement TerminalPage::_projectPaneNode(::WorkspaceModel::PaneId nodeId)
    {
        if (!_workspaceModelState || !nodeId.valid())
        {
            return nullptr;
        }
        const auto* node = _workspaceModelState->pane(nodeId);
        if (!node)
        {
            return nullptr;
        }

        // A leaf projects to its container (strip over content host).
        if (std::holds_alternative<::WorkspaceModel::LeafPane>(*node))
        {
            return _projectLeafContainer(nodeId);
        }

        // A split projects to a Grid with exactly two star-sized cells along the
        // axis, each holding the projected child. Big-flip Slice F-1 (#54) adds a
        // CUSTOM draggable separator (a thin Border) between the cells — NOT a
        // community-toolkit GridSplitter (adding that would touch the vcxproj + a
        // PackageReference + a new xmlns; explicitly rejected, no new deps). The
        // separator does NOT add a third column/row: it lives INSIDE the first
        // cell, edge-aligned to the boundary, so the clean two-cell ratio
        // structure D asserts is preserved (ColumnDefinitions/RowDefinitions stay
        // == 2 and carry ratio / 1-ratio). Its drag dispatches the resizePane
        // model action, which re-projects the two GridLengths from the new ratio.
        // INVISIBLE this slice: the host is still Collapsed, so there is no layout
        // and the separator's pointer interaction does not fire until F-5 flips
        // the host visible — that is expected. The drag→ratio math is factored
        // into _computeSplitRatioFromDrag so a headless test can simulate a drag
        // with a synthetic delta + a known extent (no laid-out pixels).
        const auto& split = std::get<::WorkspaceModel::SplitPane>(*node);

        Controls::Grid grid{};
        grid.Tag(winrt::box_value(static_cast<uint64_t>(split.id.v)));
        grid.HorizontalAlignment(HorizontalAlignment::Stretch);
        grid.VerticalAlignment(VerticalAlignment::Stretch);

        const double first = split.ratio;
        const double second = 1.0 - split.ratio;

        ::WorkspaceModel::PaneId leftId{};
        ::WorkspaceModel::PaneId rightId{};
        if (split.left)
        {
            if (const auto* l = std::get_if<::WorkspaceModel::LeafPane>(split.left.get()))
            {
                leftId = l->id;
            }
            else
            {
                leftId = std::get<::WorkspaceModel::SplitPane>(*split.left).id;
            }
        }
        if (split.right)
        {
            if (const auto* r = std::get_if<::WorkspaceModel::LeafPane>(split.right.get()))
            {
                rightId = r->id;
            }
            else
            {
                rightId = std::get<::WorkspaceModel::SplitPane>(*split.right).id;
            }
        }

        const auto leftChild = _projectPaneNode(leftId);
        const auto rightChild = _projectPaneNode(rightId);

        // Vertical axis = a vertical splitter between two side-by-side children
        // (two columns); Horizontal axis = two stacked children (two rows). The
        // star VALUES carry the model ratio so a test reads the ratio straight
        // out of the GridLength without any laid-out geometry.
        if (split.axis == ::WorkspaceModel::Axis::Vertical)
        {
            auto firstCol = Controls::ColumnDefinition{};
            auto secondCol = Controls::ColumnDefinition{};
            firstCol.Width(GridLengthHelper::FromValueAndType(first, GridUnitType::Star));
            secondCol.Width(GridLengthHelper::FromValueAndType(second, GridUnitType::Star));
            grid.ColumnDefinitions().Append(firstCol);
            grid.ColumnDefinitions().Append(secondCol);

            if (leftChild)
            {
                Controls::Grid::SetColumn(leftChild, 0);
                grid.Children().Append(leftChild);
            }
            if (rightChild)
            {
                Controls::Grid::SetColumn(rightChild, 1);
                grid.Children().Append(rightChild);
            }
        }
        else
        {
            auto firstRow = Controls::RowDefinition{};
            auto secondRow = Controls::RowDefinition{};
            firstRow.Height(GridLengthHelper::FromValueAndType(first, GridUnitType::Star));
            secondRow.Height(GridLengthHelper::FromValueAndType(second, GridUnitType::Star));
            grid.RowDefinitions().Append(firstRow);
            grid.RowDefinitions().Append(secondRow);

            if (leftChild)
            {
                Controls::Grid::SetRow(leftChild, 0);
                grid.Children().Append(leftChild);
            }
            if (rightChild)
            {
                Controls::Grid::SetRow(rightChild, 1);
                grid.Children().Append(rightChild);
            }
        }

        // Big-flip Slice F-1 (#54): the custom draggable separator. It lives in
        // the FIRST cell (column/row 0), edge-aligned to the boundary between the
        // two cells (the trailing edge of cell 0 == the leading edge of cell 1),
        // so no third column/row is introduced and the two-cell ratio structure
        // is untouched. A thin transparent-ish Border (the classic Pane uses a
        // 1px-ish border between panes; we keep it minimal — it is invisible
        // until F-5 makes the host visible, so we do NOT gold-plate cursors/hit
        // areas here). We capture the split's stable id by VALUE in the drag
        // handler (and the Grid carries the same id in Tag() as the
        // authoritative fallback / for tests). The handler reads the real laid-
        // out extent at drag time and delegates the ratio math to the shared
        // _resizeSplitFromDrag → _computeSplitRatioFromDrag helper.
        Controls::Border separator{};
        const auto splitId = split.id;
        if (split.axis == ::WorkspaceModel::Axis::Vertical)
        {
            // Vertical split = two side-by-side columns; the separator is a
            // vertical bar at the right edge of column 0.
            Controls::Grid::SetColumn(separator, 0);
            separator.Width(kSplitSeparatorThickness);
            separator.HorizontalAlignment(HorizontalAlignment::Right);
            separator.VerticalAlignment(VerticalAlignment::Stretch);
        }
        else
        {
            // Horizontal split = two stacked rows; the separator is a horizontal
            // bar at the bottom edge of row 0.
            Controls::Grid::SetRow(separator, 0);
            separator.Height(kSplitSeparatorThickness);
            separator.HorizontalAlignment(HorizontalAlignment::Stretch);
            separator.VerticalAlignment(VerticalAlignment::Bottom);
        }
        // Tag the separator with the split id too, so a test can find it by
        // identity AND read which split it controls without depending on the
        // capture. (The handler uses the captured value; the Tag is for the
        // test accessor and as an audit anchor.)
        separator.Tag(winrt::box_value(static_cast<uint64_t>(splitId.v)));

        // Track a drag in element-local coordinates. PointerPressed captures the
        // pointer + records the origin; PointerMoved computes the cumulative
        // delta along the split axis and dispatches a resize using the cell's
        // CURRENT laid-out extent (read live — valid only once visible at F-5);
        // PointerReleased ends the capture. These shared_ptrs live as long as the
        // captured lambdas (i.e. as long as the separator element).
        auto dragging = std::make_shared<bool>(false);
        auto origin = std::make_shared<double>(0.0);
        const auto axis = split.axis;
        // Capture the impl `this` plus a weak projected ref: the weak ref guards
        // the page's lifetime (resolve + null-check before use), and the raw impl
        // pointer lets the handler call the private dispatch helper directly. The
        // separator lives inside the page's own visual tree, so it cannot outlive
        // the page — but we guard anyway.
        auto weakThis = get_weak();
        auto* implThis = this;

        separator.PointerPressed([dragging, origin, axis](const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e) {
            const auto element = sender.try_as<Controls::Border>();
            if (!element)
            {
                return;
            }
            const auto point = e.GetCurrentPoint(element);
            *origin = (axis == ::WorkspaceModel::Axis::Vertical) ? point.Position().X : point.Position().Y;
            *dragging = true;
            element.CapturePointer(e.Pointer());
            e.Handled(true);
        });

        separator.PointerMoved([weakThis, implThis, dragging, origin, axis, splitId](const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e) {
            if (!*dragging)
            {
                return;
            }
            const auto element = sender.try_as<Controls::Border>();
            // Resolve the weak ref purely to confirm the page is still alive
            // before using the raw impl pointer.
            const auto strongThis = weakThis.get();
            if (!element || !strongThis)
            {
                return;
            }
            // The split Grid that owns this separator (its parent). Its laid-out
            // extent along the axis is the total the ratio divides. Read live —
            // only meaningful once the host is visible (F-5).
            const auto grid = element.Parent().try_as<Controls::Grid>();
            if (!grid)
            {
                return;
            }
            const auto point = e.GetCurrentPoint(element);
            const auto current = (axis == ::WorkspaceModel::Axis::Vertical) ? point.Position().X : point.Position().Y;
            const auto delta = current - *origin;
            const auto extent = (axis == ::WorkspaceModel::Axis::Vertical) ? grid.ActualWidth() : grid.ActualHeight();
            // Call the private dispatch helper (the same one the TAEF test calls).
            implThis->_resizeSplitFromDrag(splitId, delta, extent);
            e.Handled(true);
        });

        separator.PointerReleased([dragging](const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e) {
            const auto element = sender.try_as<Controls::Border>();
            *dragging = false;
            if (element)
            {
                element.ReleasePointerCapture(e.Pointer());
            }
            e.Handled(true);
        });

        grid.Children().Append(separator);

        return grid;
    }

    // Big-flip Slice F-1 (#54): the HEADLESS-TESTABLE core of the split-divider
    // drag. Given the split's stable `splitId`, a drag `dragDelta` in pixels
    // along the split axis, and the split cell's `totalExtent` (the laid-out
    // length the ratio divides), compute the NEW ratio: the boundary moves by
    // `dragDelta` pixels, so the first cell's fraction changes by
    // `dragDelta / totalExtent`. The result is clamped to [0,1] (the model also
    // clamps, but clamping here keeps the helper honest in isolation). Returns
    // the CURRENT model ratio unchanged when `splitId` is not a SplitPane, or
    // when `totalExtent` is non-positive (the invisible/headless case — there is
    // no layout, so a drag cannot meaningfully move the boundary; F-5 supplies a
    // real extent once visible). Factored out of the live pointer handler so a
    // TAEF test can drive it with a synthetic delta + a known extent WITHOUT any
    // laid-out geometry (the headless-resize-clamp trap), and the live handler
    // and the test share the exact same math.
    double TerminalPage::_computeSplitRatioFromDrag(::WorkspaceModel::PaneId splitId, double dragDelta, double totalExtent) const
    {
        double currentRatio = 0.5;
        if (_workspaceModelState)
        {
            if (const auto* node = _workspaceModelState->pane(splitId))
            {
                if (const auto* split = std::get_if<::WorkspaceModel::SplitPane>(node))
                {
                    currentRatio = split->ratio;
                }
                else
                {
                    // Not a SplitPane — nothing to resize.
                    return currentRatio;
                }
            }
            else
            {
                return currentRatio;
            }
        }
        if (totalExtent <= 0.0)
        {
            // No layout (invisible/headless) — a drag cannot move the boundary.
            return currentRatio;
        }
        const double next = currentRatio + (dragDelta / totalExtent);
        return std::clamp(next, 0.0, 1.0);
    }

    // Big-flip Slice F-1 (#54): dispatch a split-divider drag through the model.
    // Computes the new ratio via the shared headless-testable helper, then — only
    // if it actually changed — dispatches the model's resizePane action through
    // _applyWorkspaceAction (the same diff→re-project path every other action
    // takes), which re-projects the split Grid's two GridLengths from the new
    // ratio. A no-op when the ratio is unchanged (e.g. a zero-delta move, a
    // non-split id, or a non-positive extent) so an idle PointerMoved does not
    // churn the model. Both the live pointer handler and the TAEF drag-simulation
    // test call THIS, so the dispatch path is identical in both.
    void TerminalPage::_resizeSplitFromDrag(::WorkspaceModel::PaneId splitId, double dragDelta, double totalExtent)
    {
        if (!_workspaceModelState)
        {
            return;
        }
        const auto newRatio = _computeSplitRatioFromDrag(splitId, dragDelta, totalExtent);
        // Skip the dispatch when nothing changed — read the current ratio back to
        // compare (the helper returns the current ratio in the no-op cases).
        const auto* node = _workspaceModelState->pane(splitId);
        const auto* split = node ? std::get_if<::WorkspaceModel::SplitPane>(node) : nullptr;
        if (!split || split->ratio == newRatio)
        {
            return;
        }
        auto next = ::WorkspaceModel::resizePane(_workspaceModelState, splitId, newRatio);
        if (next != nullptr)
        {
            _applyWorkspaceAction(std::move(next));
        }
    }

    // Build a leaf container for `leaf`: a Grid tagged with the leaf's id holding
    // a per-leaf strip ListView whose ItemsSource is that leaf's REUSED Slice-C
    // strip collection (so the split-sibling leaves' strips — which Slice C
    // skipped — are now projected), AND (Big-flip Slice F-0, #54) a per-leaf
    // content host Grid in the star row that the leaf's live content GetRoot() is
    // parented into. The single shared _workspaceContentHost is the OUTER wrapper
    // (it holds the WorkspacePaneTreeRoot); these per-leaf hosts live INSIDE the
    // projected tree so a SPLIT workspace can render each leaf's terminal in its
    // own cell — the single shared host cannot represent a split. The host is
    // recorded in _paneContentHosts by PaneId; a rebuild discards the old tree,
    // so _rebuildActiveWorkspacePaneTree clears the map first and the rebuild
    // re-registers each surviving leaf's fresh host here.
    winrt::Windows::UI::Xaml::FrameworkElement TerminalPage::_projectLeafContainer(::WorkspaceModel::PaneId leaf)
    {
        Controls::Grid container{};
        container.Tag(winrt::box_value(static_cast<uint64_t>(leaf.v)));
        container.HorizontalAlignment(HorizontalAlignment::Stretch);
        container.VerticalAlignment(VerticalAlignment::Stretch);

        auto firstRow = Controls::RowDefinition{};
        firstRow.Height(GridLengthHelper::Auto());
        auto secondRow = Controls::RowDefinition{};
        secondRow.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        container.RowDefinitions().Append(firstRow);
        container.RowDefinitions().Append(secondRow);

        // The per-leaf tab strip, bound to the leaf's reused Slice-C collection.
        // _paneTabStripForLeaf creates the collection on first use, so a
        // freshly-split sibling leaf (whose strip the TabAdded arm now appends)
        // projects its row here.
        //
        // Workspaces M1 (#54, ADR-001): this is the TabStripView UserControl,
        // which hosts the REAL MUX TabView (Microsoft.UI.Xaml.Controls.TabView —
        // the same control the classic per-window tab row uses), NOT a re-skinned
        // WUX ListView. The TabView renders the tab chrome (rounded top / flare /
        // hover / between-tab separators), header text/icon/tooltip, horizontal
        // layout, scroll overflow, and single-selection NATIVELY — none of that is
        // hand-built here anymore. TabStripView projects the leaf's
        // PaneTabViewModel collection into TabView.TabItems() manually (not via
        // TabItemsSource binding). Selection stays a pure projection of the model:
        // SelectedItem is pushed from each VM's IsActive; the activate/close
        // intents are still raised by the VM (RequestActivate/RequestClose, wired
        // in _appendPaneTabVm) and the page dispatches the model action.
        winrt::TerminalApp::TabStripView strip{};
        strip.ItemsSource(_paneTabStripForLeaf(leaf));
        Controls::Grid::SetRow(strip, 0);
        container.Children().Append(strip);

        // Big-flip Slice F-0 (#54): the per-leaf content host fills the star
        // row. The leaf's active-tab content GetRoot() is parented in here by
        // _attachContentToLeafHost, driven by the WorkspaceView arms. Record it
        // by PaneId so the arms (and a rebuild's re-attach) can find this exact
        // host. A rebuild created this fresh container, so overwrite any prior
        // entry for the leaf (the old host belongs to the discarded tree).
        Controls::Grid leafContentHost{};
        leafContentHost.HorizontalAlignment(HorizontalAlignment::Stretch);
        leafContentHost.VerticalAlignment(VerticalAlignment::Stretch);
        Controls::Grid::SetRow(leafContentHost, 1);
        container.Children().Append(leafContentHost);
        _paneContentHosts[leaf] = leafContentHost;

        return container;
    }

    // Big-flip Slice F-0 (#54): parent `contentRoot` into `leaf`'s per-leaf
    // content host as its SOLE child. A FrameworkElement has a single parent, so
    // detach the root from any other tracked leaf host first, then clear this
    // host and append — re-attaching the same or a different root across a
    // rebuild / switch is always safe. A no-op when the leaf has no realized host
    // (flag-off / pre-shell / a leaf not in the current tree) or the root is
    // null. INVISIBLE: the host lives inside the still-Collapsed
    // _workspaceContentHost.
    void TerminalPage::_attachContentToLeafHost(::WorkspaceModel::PaneId leaf, const winrt::Windows::UI::Xaml::FrameworkElement& contentRoot)
    {
        const auto it = _paneContentHosts.find(leaf);
        if (it == _paneContentHosts.end() || !it->second || !contentRoot)
        {
            return;
        }

        auto& host = it->second;

        // Already this leaf host's child — nothing to do (idempotent re-attach).
        // Compare as the same projected element by going through FrameworkElement
        // so the identity check is on the underlying object, not the interface.
        if (host.Children().Size() == 1 &&
            host.Children().GetAt(0).try_as<winrt::Windows::UI::Xaml::FrameworkElement>() == contentRoot)
        {
            return;
        }

        // A FrameworkElement has a single parent; appending one that still has a
        // parent throws ("element is already the child of another element"). The
        // content root may currently be parented in a DIFFERENT tracked leaf host
        // (e.g. an ActiveTabChanged that moves a content between cells). Scan the
        // tracked hosts and detach it from any that holds it. (A rebuild already
        // cleared the discarded hosts' children, so the cross-rebuild case is
        // handled there; this covers the within-tree move.) We rely on tracked
        // hosts rather than FrameworkElement.Parent() because the logical Parent
        // is not reliably set for programmatically-parented, un-laid-out grids.
        for (auto& [otherLeaf, otherHost] : _paneContentHosts)
        {
            if (!otherHost)
            {
                continue;
            }
            uint32_t idx = 0;
            if (otherHost.Children().IndexOf(contentRoot, idx))
            {
                otherHost.Children().RemoveAt(idx);
            }
        }
        host.Children().Clear();
        host.Children().Append(contentRoot);
    }

    // Big-flip Slice F-0 (#54): enumerate (leaf, tab-to-show) for every leaf with
    // a realized content host. The tab-to-show is the leaf's ACTIVE strip VM
    // (IsActive()) when one exists, else its FIRST strip row — so a startup /
    // split-sibling leaf whose single tab has not yet been flipped active (the
    // first-tab IsActive seed is F-2) still gets its one content attached. A leaf
    // with a host but an empty strip is skipped. The WorkspaceView resolves each
    // TabId to its live content and drives _attachContentToLeafHost per leaf —
    // the page owns leaf->tab, the view owns tab->content.
    std::vector<std::pair<::WorkspaceModel::PaneId, ::WorkspaceModel::TabId>> TerminalPage::_leafContentTabs() const
    {
        std::vector<std::pair<::WorkspaceModel::PaneId, ::WorkspaceModel::TabId>> result;
        for (const auto& [leaf, host] : _paneContentHosts)
        {
            const auto stripIt = _paneTabStrips.find(leaf);
            if (stripIt == _paneTabStrips.end() || stripIt->second.Size() == 0)
            {
                continue;
            }
            const auto& strip = stripIt->second;

            // Prefer the active row; fall back to the first row when no row is
            // active yet (the startup / split-sibling first tab, before F-2's
            // active seed). One-tab-per-leaf in F-0, so the fallback is exact.
            ::WorkspaceModel::TabId chosen{};
            for (const auto& vm : strip)
            {
                if (vm.IsActive())
                {
                    chosen = ::WorkspaceModel::TabId{ vm.Id() };
                    break;
                }
            }
            if (!chosen.valid())
            {
                chosen = ::WorkspaceModel::TabId{ strip.GetAt(0).Id() };
            }
            result.emplace_back(leaf, chosen);
        }
        return result;
    }

    winrt::Windows::UI::Xaml::FrameworkElement TerminalPage::_leafHostChildForTest(::WorkspaceModel::PaneId leaf) const
    {
        const auto it = _paneContentHosts.find(leaf);
        if (it == _paneContentHosts.end() || !it->second || it->second.Children().Size() == 0)
        {
            return nullptr;
        }
        return it->second.Children().GetAt(0).try_as<winrt::Windows::UI::Xaml::FrameworkElement>();
    }

    winrt::Windows::UI::Xaml::FrameworkElement TerminalPage::_workspacePaneTreeRootChildForTest() const
    {
        if (!_workspacePaneTreeRoot || _workspacePaneTreeRoot.Children().Size() == 0)
        {
            return nullptr;
        }
        return _workspacePaneTreeRoot.Children().GetAt(0).try_as<winrt::Windows::UI::Xaml::FrameworkElement>();
    }

    // Big-flip Slice F-1 (#54): the custom split-divider Border inside the
    // projected split Grid `splitGrid`, identified by being the direct Border
    // child whose Tag() == the split id. Returns nullptr when `splitGrid` is null
    // or carries no such separator (e.g. a flag-off mirror that builds no
    // separator). Lets a page test assert the separator was built — by element
    // type AND by the split id it controls — without driving any laid-out
    // geometry.
    winrt::Windows::UI::Xaml::Controls::Border TerminalPage::_splitSeparatorForTest(const winrt::Windows::UI::Xaml::Controls::Grid& splitGrid) const
    {
        if (!splitGrid)
        {
            return nullptr;
        }
        const auto tag = splitGrid.Tag().try_as<uint64_t>();
        for (uint32_t i = 0; i < splitGrid.Children().Size(); ++i)
        {
            if (const auto border = splitGrid.Children().GetAt(i).try_as<winrt::Windows::UI::Xaml::Controls::Border>())
            {
                const auto borderTag = border.Tag().try_as<uint64_t>();
                if (tag && borderTag && *borderTag == *tag)
                {
                    return border;
                }
            }
        }
        return nullptr;
    }

}
