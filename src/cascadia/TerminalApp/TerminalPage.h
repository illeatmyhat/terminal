// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <unordered_map>

#include <ThrottledFunc.h>

#include "TerminalPage.g.h"
#include "Tab.h"
#include "AppKeyBindings.h"
#include "AppCommandlineArgs.h"
#include "RenameWindowRequestedArgs.g.h"
#include "RequestMoveContentArgs.g.h"
#include "LaunchPositionRequest.g.h"
#include "Toast.h"

#include "../WorkspaceModel/WorkspaceActions.h"

#include "WindowsPackageManagerFactory.h"

#define DECLARE_ACTION_HANDLER(action) void _Handle##action(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);

namespace TerminalAppLocalTests
{
    class TabTests;
    class SettingsTests;
    class WorkspaceTests;
}

namespace Microsoft::Terminal::Core
{
    class ControlKeyStates;
}

namespace winrt::Microsoft::Terminal::Settings
{
    struct TerminalSettingsCreateResult;
}

namespace winrt::TerminalApp::implementation
{
    struct TerminalSettingsCache;
    class WorkspaceView;

    inline constexpr uint32_t DefaultRowsToScroll{ 3 };
    inline constexpr std::wstring_view TabletInputServiceKey{ L"TabletInputService" };

    enum StartupState : int
    {
        NotInitialized = 0,
        InStartup = 1,
        Initialized = 2
    };

    enum ScrollDirection : int
    {
        ScrollUp = 0,
        ScrollDown = 1
    };

    enum class ConfirmCloseDialogKind
    {
        Pane,
        Tab,
        MultiplePanes,
        MultipleTabs,
        Window,
        CloseAll
    };

    struct RenameWindowRequestedArgs : RenameWindowRequestedArgsT<RenameWindowRequestedArgs>
    {
        WINRT_PROPERTY(winrt::hstring, ProposedName);

    public:
        RenameWindowRequestedArgs(const winrt::hstring& name) :
            _ProposedName{ name } {};
    };

    struct RequestMoveContentArgs : RequestMoveContentArgsT<RequestMoveContentArgs>
    {
        WINRT_PROPERTY(winrt::hstring, Window);
        WINRT_PROPERTY(winrt::hstring, Content);
        WINRT_PROPERTY(uint32_t, TabIndex);
        WINRT_PROPERTY(Windows::Foundation::IReference<Windows::Foundation::Point>, WindowPosition);

    public:
        RequestMoveContentArgs(const winrt::hstring window, const winrt::hstring content, uint32_t tabIndex) :
            _Window{ window },
            _Content{ content },
            _TabIndex{ tabIndex } {};
    };

    struct LaunchPositionRequest : LaunchPositionRequestT<LaunchPositionRequest>
    {
        LaunchPositionRequest() = default;

        til::property<winrt::Microsoft::Terminal::Settings::Model::LaunchPosition> Position;
    };

    struct WinGetSearchParams
    {
        winrt::Microsoft::Management::Deployment::PackageMatchField Field;
        winrt::Microsoft::Management::Deployment::PackageFieldMatchOption MatchOption;
    };

    struct TerminalPage : TerminalPageT<TerminalPage>
    {
    public:
        TerminalPage(TerminalApp::WindowProperties properties, const TerminalApp::ContentManager& manager);
        ~TerminalPage();

        // This implements shobjidl's IInitializeWithWindow, but due to a XAML Compiler bug we cannot
        // put it in our inheritance graph. https://github.com/microsoft/microsoft-ui-xaml/issues/3331
        STDMETHODIMP Initialize(HWND hwnd);

        void SetSettings(Microsoft::Terminal::Settings::Model::CascadiaSettings settings, bool needRefreshUI);

        void Create();
        Windows::UI::Xaml::Automation::Peers::AutomationPeer OnCreateAutomationPeer();

        bool ShouldImmediatelyHandoffToElevated(const Microsoft::Terminal::Settings::Model::CascadiaSettings& settings) const;
        void HandoffToElevated(const Microsoft::Terminal::Settings::Model::CascadiaSettings& settings);

        hstring Title();

        void TitlebarClicked();
        void WindowVisibilityChanged(const bool showOrHide);

        float CalcSnappedDimension(const bool widthOrHeight, const float dimension) const;

        winrt::hstring ApplicationDisplayName();
        winrt::hstring ApplicationVersion();

        CommandPalette LoadCommandPalette();
        SuggestionsControl LoadSuggestionsUI();

        safe_void_coroutine RequestQuit();
        safe_void_coroutine CloseWindow();
        void PersistState();
        std::vector<IPaneContent> Panes() const;

        void ToggleFocusMode();
        void ToggleFullscreen();
        void ToggleAlwaysOnTop();
        bool FocusMode() const;
        bool Fullscreen() const;
        bool AlwaysOnTop() const;
        bool ShowTabsFullscreen() const;
        void SetShowTabsFullscreen(bool newShowTabsFullscreen);
        void SetFullscreen(bool);
        void SetFocusMode(const bool inFocusMode);
        void Maximized(bool newMaximized);
        void RequestSetMaximized(bool newMaximized);

        void SetStartupActions(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> actions);
        void SetStartupConnection(winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection);

        static std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> ConvertExecuteCommandlineToActions(const Microsoft::Terminal::Settings::Model::ExecuteCommandlineArgs& args);

        winrt::TerminalApp::IDialogPresenter DialogPresenter() const;
        void DialogPresenter(winrt::TerminalApp::IDialogPresenter dialogPresenter);

        winrt::TerminalApp::TaskbarState TaskbarState() const;

        void ShowKeyboardServiceWarning() const;
        winrt::hstring KeyboardServiceDisabledText();

        void IdentifyWindow();
        void ActionSaved(winrt::hstring input, winrt::hstring name, winrt::hstring keyChord);
        void ActionSaveFailed(winrt::hstring message);
        void ShowTerminalWorkingDirectory();

        safe_void_coroutine ProcessStartupActions(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> actions,
                                                  const winrt::hstring cwd = winrt::hstring{},
                                                  const winrt::hstring env = winrt::hstring{});
        safe_void_coroutine CreateTabFromConnection(winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection);

        TerminalApp::WindowProperties WindowProperties() const noexcept { return _WindowProperties; };

        bool CanDragDrop() const noexcept;
        bool IsRunningElevated() const noexcept;

        void OpenSettingsUI();
        void WindowActivated(const bool activated);
        bool FocusTab(const winrt::TerminalApp::Tab& tab);

        bool OnDirectKeyEvent(const uint32_t vkey, const uint8_t scanCode, const bool down);

        void AttachContent(Windows::Foundation::Collections::IVector<Microsoft::Terminal::Settings::Model::ActionAndArgs> args, uint32_t tabIndex);
        void SendContentToOther(winrt::TerminalApp::RequestReceiveContentArgs args);

        uint32_t NumberOfTabs() const;

        til::property_changed_event PropertyChanged;

        // -------------------------------- WinRT Events ---------------------------------
        til::typed_event<IInspectable, IInspectable> TitleChanged;
        til::typed_event<IInspectable, IInspectable> CloseWindowRequested;
        til::typed_event<IInspectable, winrt::Windows::UI::Xaml::UIElement> SetTitleBarContent;
        til::typed_event<IInspectable, IInspectable> FocusModeChanged;
        til::typed_event<IInspectable, IInspectable> FullscreenChanged;
        til::typed_event<IInspectable, IInspectable> ChangeMaximizeRequested;
        til::typed_event<IInspectable, IInspectable> AlwaysOnTopChanged;
        til::typed_event<IInspectable, IInspectable> RaiseVisualBell;
        til::typed_event<IInspectable, IInspectable> SetTaskbarProgress;
        til::typed_event<IInspectable, IInspectable> Initialized;
        til::typed_event<IInspectable, IInspectable> IdentifyWindowsRequested;
        til::typed_event<IInspectable, winrt::TerminalApp::RenameWindowRequestedArgs> RenameWindowRequested;
        til::typed_event<IInspectable, IInspectable> SummonWindowRequested;
        til::typed_event<IInspectable, winrt::TerminalApp::Tab> FocusTabRequested;
        til::typed_event<IInspectable, winrt::Microsoft::Terminal::Control::WindowSizeChangedEventArgs> WindowSizeChanged;

        til::typed_event<IInspectable, IInspectable> OpenSystemMenu;
        til::typed_event<IInspectable, IInspectable> QuitRequested;
        til::typed_event<IInspectable, winrt::Microsoft::Terminal::Control::ShowWindowArgs> ShowWindowChanged;
        til::typed_event<Windows::Foundation::IInspectable, Windows::Foundation::Collections::IVectorView<winrt::Microsoft::Terminal::Settings::Model::SettingsLoadWarnings>> ShowLoadWarningsDialog;

        til::typed_event<Windows::Foundation::IInspectable, winrt::TerminalApp::RequestMoveContentArgs> RequestMoveContent;
        til::typed_event<Windows::Foundation::IInspectable, winrt::TerminalApp::RequestReceiveContentArgs> RequestReceiveContent;

        til::typed_event<IInspectable, winrt::TerminalApp::LaunchPositionRequest> RequestLaunchPosition;

        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, TitlebarBrush, PropertyChanged.raise, nullptr);
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, FrameBrush, PropertyChanged.raise, nullptr);

        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, SavedActionName, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, SavedActionKeyChord, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, SavedActionCommandLine, PropertyChanged.raise, L"");

    private:
        friend struct TerminalPageT<TerminalPage>; // for Xaml to bind events
        std::optional<HWND> _hostingHwnd;

        // If you add controls here, but forget to null them either here or in
        // the ctor, you're going to have a bad time. It'll mysteriously fail to
        // activate the app.
        // ALSO: If you add any UIElements as roots here, make sure they're
        // updated in App::_ApplyTheme. The roots currently is _tabRow
        // (which is a root when the tabs are in the titlebar.)
        Microsoft::UI::Xaml::Controls::TabView _tabView{ nullptr };
        TerminalApp::TabRowControl _tabRow{ nullptr };
        Windows::UI::Xaml::Controls::Grid _tabContent{ nullptr };

        // Big-flip Slice B (#54): the realized flag-on content host (the
        // x:Load="False" WorkspaceContentHost Grid inside TabContent),
        // resolved via FindName in _initializeWorkspaceShell. nullptr when the
        // flag is off / the shell hasn't initialized. Kept Collapsed this
        // slice; the active workspace's factory-built content GetRoot() is
        // parented in here by _attachContentToWorkspaceHost. The classic
        // _tabContent swap still owns the VISIBLE display.
        Windows::UI::Xaml::Controls::Grid _workspaceContentHost{ nullptr };

        Microsoft::UI::Xaml::Controls::SplitButton _newTabButton{ nullptr };
        winrt::TerminalApp::ColorPickupFlyout _tabColorPicker{ nullptr };

        Microsoft::Terminal::Settings::Model::CascadiaSettings _settings{ nullptr };

        Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab> _tabs;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab> _mruTabs;
        static winrt::com_ptr<Tab> _GetTabImpl(const TerminalApp::Tab& tab);

        void _UpdateTabIndices();

        TerminalApp::Tab _settingsTab{ nullptr };

        bool _isInFocusMode{ false };
        bool _isFullscreen{ false };
        bool _isMaximized{ false };
        bool _isAlwaysOnTop{ false };
        bool _showTabsFullscreen{ false };

        std::optional<uint32_t> _loadFromPersistedLayoutIdx{};

        bool _rearranging{ false };
        std::optional<int> _rearrangeFrom{};
        std::optional<int> _rearrangeTo{};
        bool _removing{ false };

        bool _activated{ false };
        bool _visible{ true };

        std::vector<std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs>> _previouslyClosedPanesAndTabs{};

        uint32_t _systemRowsToScroll{ DefaultRowsToScroll };

        // use a weak reference to prevent circular dependency with AppLogic
        winrt::weak_ref<winrt::TerminalApp::IDialogPresenter> _dialogPresenter;

        winrt::com_ptr<AppKeyBindings> _bindings{ winrt::make_self<implementation::AppKeyBindings>() };
        winrt::com_ptr<ShortcutActionDispatch> _actionDispatch{ winrt::make_self<implementation::ShortcutActionDispatch>() };

        winrt::Windows::UI::Xaml::Controls::Grid::LayoutUpdated_revoker _layoutUpdatedRevoker;
        StartupState _startupState{ StartupState::NotInitialized };

        std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> _startupActions;
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _startupConnection{ nullptr };

        std::shared_ptr<Toast> _windowIdToast{ nullptr };
        std::shared_ptr<Toast> _actionSavedToast{ nullptr };
        std::shared_ptr<Toast> _actionSaveFailedToast{ nullptr };
        std::shared_ptr<Toast> _windowCwdToast{ nullptr };

        winrt::Windows::UI::Xaml::Controls::TextBox::LayoutUpdated_revoker _renamerLayoutUpdatedRevoker;
        int _renamerLayoutCount{ 0 };
        bool _renamerPressedEnter{ false };

        TerminalApp::WindowProperties _WindowProperties{ nullptr };
        PaneResources _paneResources;

        TerminalApp::ContentManager _manager{ nullptr };

        std::shared_ptr<TerminalSettingsCache> _terminalSettingsCache{};

        struct StashedDragData
        {
            winrt::com_ptr<winrt::TerminalApp::implementation::Tab> draggedTab{ nullptr };
            winrt::Windows::Foundation::Point dragOffset{ 0, 0 };
        } _stashed;

        safe_void_coroutine _NewTerminalByDrop(const Windows::Foundation::IInspectable&, winrt::Windows::UI::Xaml::DragEventArgs e);

        __declspec(noinline) CommandPalette _loadCommandPaletteSlowPath();
        bool _commandPaletteIs(winrt::Windows::UI::Xaml::Visibility visibility);
        __declspec(noinline) SuggestionsControl _loadSuggestionsElementSlowPath();
        bool _suggestionsControlIs(winrt::Windows::UI::Xaml::Visibility visibility);

        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowDialogHelper(const std::wstring_view& name);

        void _ShowAboutDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowConfirmCloseDialog(ConfirmCloseDialogKind kind);
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowCloseReadOnlyDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowMultiLinePasteWarningDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowLargePasteWarningDialog();

        void _CreateNewTabFlyout();
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _buildNewItemFlyout(std::function<void(winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs)> onChosen);
        std::vector<winrt::Windows::UI::Xaml::Controls::MenuFlyoutItemBase> _CreateNewTabFlyoutItems(winrt::Windows::Foundation::Collections::IVector<Microsoft::Terminal::Settings::Model::NewTabMenuEntry> entries, std::function<void(winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs)> onChosen);
        winrt::Windows::UI::Xaml::Controls::IconElement _CreateNewTabFlyoutIcon(const winrt::hstring& icon);
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _CreateNewTabFlyoutProfile(const Microsoft::Terminal::Settings::Model::Profile profile, int profileIndex, const winrt::hstring& iconPathOverride, std::function<void(winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs)> onChosen);
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _CreateNewTabFlyoutAction(const winrt::hstring& actionId, const winrt::hstring& iconPathOverride);

        void _OpenNewTabDropdown();
        HRESULT _OpenNewTab(const Microsoft::Terminal::Settings::Model::INewContentArgs& newContentArgs);
        TerminalApp::Tab _CreateNewTabFromPane(std::shared_ptr<Pane> pane, uint32_t insertPosition = -1);

        std::wstring _evaluatePathForCwd(std::wstring_view path);

        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _CreateConnectionFromSettings(Microsoft::Terminal::Settings::Model::Profile profile, Microsoft::Terminal::Control::IControlSettings settings, const bool inheritCursor);
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _duplicateConnectionForRestart(const TerminalApp::TerminalPaneContent& paneContent);
        void _restartPaneConnection(const TerminalApp::TerminalPaneContent&, const winrt::Windows::Foundation::IInspectable&);

        safe_void_coroutine _OpenNewWindow(const Microsoft::Terminal::Settings::Model::INewContentArgs newContentArgs);

        void _OpenNewTerminalViaDropdown(const Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs);

        bool _displayingCloseDialog{ false };
        void _SettingsButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _CommandPaletteButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _AboutButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);

        void _KeyDownHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);
        static ::Microsoft::Terminal::Core::ControlKeyStates _GetPressedModifierKeys() noexcept;
        static void _ClearKeyboardState(const WORD vkey, const WORD scanCode) noexcept;
        void _HookupKeyBindings(const Microsoft::Terminal::Settings::Model::IActionMapView& actionMap) noexcept;
        void _RegisterActionCallbacks();

        void _UpdateTitle(const Tab& tab);
        void _UpdateTabIcon(Tab& tab);
        void _UpdateTabView();
        void _UpdateTabWidthMode();
        void _SetBackgroundImage(const winrt::Microsoft::Terminal::Settings::Model::IAppearanceConfig& newAppearance);

        void _DuplicateFocusedTab();
        void _DuplicateTab(const Tab& tab);

        safe_void_coroutine _ExportTab(const Tab& tab, winrt::hstring filepath);

        winrt::Windows::Foundation::IAsyncAction _HandleCloseTabRequested(winrt::TerminalApp::Tab tab, bool skipConfirmClose = false);
        void _CloseTabAtIndex(uint32_t index);
        void _RemoveTab(const winrt::TerminalApp::Tab& tab);
        safe_void_coroutine _RemoveTabs(const std::vector<winrt::TerminalApp::Tab> tabs);

        void _InitializeTab(winrt::com_ptr<Tab> newTabImpl, uint32_t insertPosition = -1);
        void _RegisterTerminalEvents(Microsoft::Terminal::Control::TermControl term);
        void _RegisterTabEvents(Tab& hostingTab);

        void _DismissTabContextMenus();
        void _FocusCurrentTab(const bool focusAlways);
        bool _HasMultipleTabs() const;

        void _SelectNextTab(const bool bMoveRight, const Windows::Foundation::IReference<Microsoft::Terminal::Settings::Model::TabSwitcherMode>& customTabSwitcherMode);
        bool _SelectTab(uint32_t tabIndex);
        bool _MoveFocus(const Microsoft::Terminal::Settings::Model::FocusDirection& direction);
        bool _SwapPane(const Microsoft::Terminal::Settings::Model::FocusDirection& direction);
        bool _MovePane(const Microsoft::Terminal::Settings::Model::MovePaneArgs args);
        bool _MoveTab(winrt::com_ptr<Tab> tab, const Microsoft::Terminal::Settings::Model::MoveTabArgs args);

        std::shared_ptr<ThrottledFunc<>> _adjustProcessPriorityThrottled;
        void _adjustProcessPriority() const;

        template<typename F>
        bool _ApplyToActiveControls(F f) const
        {
            if (const auto tab{ _GetFocusedTabImpl() })
            {
                if (const auto activePane = tab->GetActivePane())
                {
                    activePane->WalkTree([&](auto p) {
                        if (const auto& control{ p->GetTerminalControl() })
                        {
                            f(control);
                        }
                    });

                    return true;
                }
            }
            return false;
        }

        winrt::Microsoft::Terminal::Control::TermControl _GetActiveControl() const;
        std::optional<uint32_t> _GetFocusedTabIndex() const noexcept;
        std::optional<uint32_t> _GetTabIndex(const TerminalApp::Tab& tab) const noexcept;
        TerminalApp::Tab _GetFocusedTab() const noexcept;
        winrt::com_ptr<Tab> _GetFocusedTabImpl() const noexcept;
        TerminalApp::Tab _GetTabByTabViewItem(const IInspectable& tabViewItem) const noexcept;

        void _HandleClosePaneRequested(std::shared_ptr<Pane> pane);
        bool _ShouldWarnOnClose() const;
        bool _ShouldWarnOnCloseTab(const winrt::com_ptr<Tab>& tab) const;
        safe_void_coroutine _SetFocusedTab(const winrt::TerminalApp::Tab tab);
        safe_void_coroutine _CloseFocusedPane();
        safe_void_coroutine _ClosePanes(weak_ref<Tab> weakTab, std::vector<uint32_t> paneIds);
        void _CloseRemainingPanes(weak_ref<Tab> weakTab, std::vector<uint32_t> paneIds);
        winrt::Windows::Foundation::IAsyncOperation<bool> _PaneConfirmCloseReadOnly(std::shared_ptr<Pane> pane);
        void _AddPreviouslyClosedPaneOrTab(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs>&& args);

        void _Scroll(ScrollDirection scrollDirection, const Windows::Foundation::IReference<uint32_t>& rowsToScroll);

        void _SplitPane(const winrt::com_ptr<Tab>& tab,
                        const Microsoft::Terminal::Settings::Model::SplitDirection splitType,
                        const float splitSize,
                        std::shared_ptr<Pane> newPane);
        bool _ResizePane(const Microsoft::Terminal::Settings::Model::ResizeDirection& direction);
        void _ToggleSplitOrientation();

        void _ScrollPage(ScrollDirection scrollDirection);
        void _ScrollToBufferEdge(ScrollDirection scrollDirection);
        void _SetAcceleratorForMenuItem(Windows::UI::Xaml::Controls::MenuFlyoutItem& menuItem, const winrt::Microsoft::Terminal::Control::KeyChord& keyChord);

        safe_void_coroutine _PasteFromClipboardHandler(const IInspectable sender,
                                                       const Microsoft::Terminal::Control::PasteFromClipboardEventArgs eventArgs);

        safe_void_coroutine _OpenHyperlinkHandler(const IInspectable sender, const Microsoft::Terminal::Control::OpenHyperlinkEventArgs eventArgs);
        static bool _IsUriSupported(const winrt::Windows::Foundation::Uri& parsedUri);
        bool _IsUriConsideredSomewhatSafe(const winrt::Windows::Foundation::Uri& parsedUri) const;

        void _ShowCouldNotOpenDialog(winrt::hstring reason, winrt::hstring uri);
        bool _CopyText(bool dismissSelection, bool singleLine, bool withControlSequences, Microsoft::Terminal::Control::CopyFormat formats);

        safe_void_coroutine _SetTaskbarProgressHandler(const IInspectable sender, const IInspectable eventArgs);

        void _copyToClipboard(IInspectable, Microsoft::Terminal::Control::WriteToClipboardEventArgs args) const;
        void _PasteText();

        safe_void_coroutine _ControlNoticeRaisedHandler(const IInspectable sender, const Microsoft::Terminal::Control::NoticeEventArgs eventArgs);
        void _ShowControlNoticeDialog(const winrt::hstring& title, const winrt::hstring& message);

        safe_void_coroutine _LaunchSettings(const Microsoft::Terminal::Settings::Model::SettingsTarget target);

        void _TabDragStarted(const IInspectable& sender, const IInspectable& eventArgs);
        void _TabDragCompleted(const IInspectable& sender, const IInspectable& eventArgs);

        // BODGY: WinUI's TabView has a broken close event handler:
        // If the close button is disabled, middle-clicking the tab raises no close
        // event. Because that's dumb, we implement our own middle-click handling.
        // `_tabItemMiddleClickHookEnabled` is true whenever the close button is hidden,
        // and that enables all of the rest of this machinery (and this workaround).
        bool _tabItemMiddleClickHookEnabled = false;
        bool _tabItemMiddleClickExited = false;
        PointerEntered_revoker _tabItemMiddleClickPointerEntered;
        PointerExited_revoker _tabItemMiddleClickPointerExited;
        PointerCaptureLost_revoker _tabItemMiddleClickPointerCaptureLost;
        void _OnTabPointerPressed(const IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs);
        safe_void_coroutine _OnTabPointerReleasedCloseTab(IInspectable sender);

        void _OnTabSelectionChanged(const IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& eventArgs);
        void _OnTabItemsChanged(const IInspectable& sender, const Windows::Foundation::Collections::IVectorChangedEventArgs& eventArgs);
        void _OnTabCloseRequested(const IInspectable& sender, const Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs& eventArgs);
        void _OnFirstLayout(const IInspectable& sender, const IInspectable& eventArgs);
        void _UpdatedSelectedTab(const winrt::TerminalApp::Tab& tab);
        void _UpdateBackground(const winrt::Microsoft::Terminal::Settings::Model::Profile& profile);

        void _OnDispatchCommandRequested(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::Command& command);
        void _OnCommandLineExecutionRequested(const IInspectable& sender, const winrt::hstring& commandLine);
        void _OnSwitchToTabRequested(const IInspectable& sender, const winrt::TerminalApp::Tab& tab);

        void _Find(const Tab& tab);

        winrt::Microsoft::Terminal::Control::TermControl _CreateNewControlAndContent(const winrt::Microsoft::Terminal::Settings::TerminalSettingsCreateResult& settings,
                                                                                     const winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection& connection);
        winrt::Microsoft::Terminal::Control::TermControl _SetupControl(const winrt::Microsoft::Terminal::Control::TermControl& term);
        winrt::Microsoft::Terminal::Control::TermControl _AttachControlToContent(const uint64_t& contentGuid);

        TerminalApp::IPaneContent _makeSettingsContent();
        std::shared_ptr<Pane> _MakeTerminalPane(const Microsoft::Terminal::Settings::Model::NewTerminalArgs& newTerminalArgs = nullptr,
                                                const winrt::TerminalApp::Tab& sourceTab = nullptr,
                                                winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection existingConnection = nullptr);
        std::shared_ptr<Pane> _MakePane(const Microsoft::Terminal::Settings::Model::INewContentArgs& newContentArgs = nullptr,
                                        const winrt::TerminalApp::Tab& sourceTab = nullptr,
                                        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection existingConnection = nullptr);

        // Big-flip Slice A (#54): the ContentMounted factory. Materialises a
        // live IPaneContent from a model TabContent spec, for the
        // ContentRegistry to own. Called by WorkspaceView::apply(ContentMounted)
        // well after construction (so get_weak() is valid). Returns nullptr —
        // "do not attach" — for any non-Terminal variant
        // (Settings/Snippets/Markdown/Scratchpad) or on spawn failure /
        // elevation handoff. This replicates the minimal TerminalSpec
        // content-build core of _MakeTerminalPane (leaving _MakeTerminalPane
        // untouched); it does NOT split, duplicate, debug-tap, or restore a
        // session — those are not part of a from-spec materialisation.
        winrt::TerminalApp::IPaneContent _makePaneContentForSpec(const ::WorkspaceModel::TabContent& spec);

        void _RefreshUIForSettingsReload();

        void _SetNewTabButtonColor(til::color color, til::color accentColor);
        void _ClearNewTabButtonColor();

        safe_void_coroutine _CompleteInitialization();

        void _FocusActiveControl(IInspectable sender, IInspectable eventArgs);

        void _UnZoomIfNeeded();

        static int _ComputeScrollDelta(ScrollDirection scrollDirection, const uint32_t rowsToScroll);
        static uint32_t _ReadSystemRowsToScroll();

        void _UpdateMRUTab(const winrt::TerminalApp::Tab& tab);

        void _TryMoveTab(const uint32_t currentTabIndex, const int32_t suggestedNewTabIndex);

        void _PreviewAction(const Microsoft::Terminal::Settings::Model::ActionAndArgs& args);
        void _PreviewActionHandler(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::Command& args);
        void _EndPreview();
        void _RunRestorePreviews();
        void _PreviewColorScheme(const Microsoft::Terminal::Settings::Model::SetColorSchemeArgs& args);
        void _PreviewAdjustOpacity(const Microsoft::Terminal::Settings::Model::AdjustOpacityArgs& args);
        void _PreviewSendInput(const Microsoft::Terminal::Settings::Model::SendInputArgs& args);

        winrt::Microsoft::Terminal::Settings::Model::ActionAndArgs _lastPreviewedAction{ nullptr };
        std::vector<std::function<void()>> _restorePreviewFuncs{};

        HRESULT _OnNewConnection(const winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection& connection);
        void _HandleToggleInboundPty(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);

        void _WindowRenamerActionClick(const IInspectable& sender, const IInspectable& eventArgs);
        void _RequestWindowRename(const winrt::hstring& newName);
        void _WindowRenamerKeyDown(const IInspectable& sender, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);
        void _WindowRenamerKeyUp(const IInspectable& sender, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);

        void _UpdateTeachingTipTheme(winrt::Windows::UI::Xaml::FrameworkElement element);

        winrt::Microsoft::Terminal::Settings::Model::Profile GetClosestProfileForDuplicationOfProfile(const winrt::Microsoft::Terminal::Settings::Model::Profile& profile) const noexcept;

        bool _maybeElevate(const winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs& newTerminalArgs,
                           const winrt::Microsoft::Terminal::Settings::TerminalSettingsCreateResult& controlSettings,
                           const winrt::Microsoft::Terminal::Settings::Model::Profile& profile);
        void _OpenElevatedWT(winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs);

        safe_void_coroutine _ConnectionStateChangedHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args);
        void _CloseOnExitInfoDismissHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args) const;
        void _KeyboardServiceWarningInfoDismissHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args) const;
        static bool _IsMessageDismissed(const winrt::Microsoft::Terminal::Settings::Model::InfoBarMessage& message);
        static void _DismissMessage(const winrt::Microsoft::Terminal::Settings::Model::InfoBarMessage& message);

        void _updateThemeColors();
        void _updateAllTabCloseButtons();
        void _updatePaneResources(const winrt::Windows::UI::Xaml::ElementTheme& requestedTheme);

        safe_void_coroutine _ControlCompletionsChangedHandler(const winrt::Windows::Foundation::IInspectable sender, const winrt::Microsoft::Terminal::Control::CompletionsChangedEventArgs args);

        void _OpenSuggestions(const Microsoft::Terminal::Control::TermControl& sender, Windows::Foundation::Collections::IVector<winrt::Microsoft::Terminal::Settings::Model::Command> commandsCollection, winrt::TerminalApp::SuggestionsMode mode, winrt::hstring filterText);

        void _ShowWindowChangedHandler(const IInspectable sender, const winrt::Microsoft::Terminal::Control::ShowWindowArgs args);
        Windows::Foundation::IAsyncAction _SearchMissingCommandHandler(const IInspectable sender, const winrt::Microsoft::Terminal::Control::SearchMissingCommandEventArgs args);
        static Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<winrt::Microsoft::Management::Deployment::MatchResult>> _FindPackageAsync(hstring query);

        void _WindowSizeChanged(const IInspectable sender, const winrt::Microsoft::Terminal::Control::WindowSizeChangedEventArgs args);
        void _windowPropertyChanged(const IInspectable& sender, const winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs& args);

        void _onTabDragStarting(const winrt::Microsoft::UI::Xaml::Controls::TabView& sender, const winrt::Microsoft::UI::Xaml::Controls::TabViewTabDragStartingEventArgs& e);
        void _onTabStripDragOver(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::DragEventArgs& e);
        void _onTabStripDrop(winrt::Windows::Foundation::IInspectable sender, winrt::Windows::UI::Xaml::DragEventArgs e);
        void _onTabDroppedOutside(winrt::Windows::Foundation::IInspectable sender, winrt::Microsoft::UI::Xaml::Controls::TabViewTabDroppedOutsideEventArgs e);

        void _DetachPaneFromWindow(std::shared_ptr<Pane> pane);
        void _DetachTabFromWindow(const winrt::com_ptr<Tab>& tabImpl);
        void _MoveContent(std::vector<winrt::Microsoft::Terminal::Settings::Model::ActionAndArgs>&& actions,
                          const winrt::hstring& windowName,
                          const uint32_t tabIndex,
                          const std::optional<winrt::Windows::Foundation::Point>& dragPoint = std::nullopt);
        void _sendDraggedTabToWindow(const winrt::hstring& windowId, const uint32_t tabIndex, std::optional<winrt::Windows::Foundation::Point> dragPoint);

        void _PopulateContextMenu(const Microsoft::Terminal::Control::TermControl& control, const Microsoft::UI::Xaml::Controls::CommandBarFlyout& sender, const bool withSelection);
        void _PopulateQuickFixMenu(const Microsoft::Terminal::Control::TermControl& control, const Windows::UI::Xaml::Controls::MenuFlyout& sender);
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _CreateRunAsAdminFlyout(int profileIndex);

        winrt::Microsoft::Terminal::Control::TermControl _senderOrActiveControl(const winrt::Windows::Foundation::IInspectable& sender);
        winrt::com_ptr<Tab> _senderOrFocusedTab(const IInspectable& sender);

        void _activePaneChanged(winrt::TerminalApp::Tab tab, Windows::Foundation::IInspectable args);
        safe_void_coroutine _doHandleSuggestions(Microsoft::Terminal::Settings::Model::SuggestionsArgs realArgs);

        void _SendDesktopNotification(const winrt::hstring& tabTitle, const winrt::hstring& body, const winrt::com_ptr<Tab>& tab, const winrt::TerminalApp::IPaneContent& content);

#pragma region ActionHandlers
        // These are all defined in AppActionHandlers.cpp
#define ON_ALL_ACTIONS(action) DECLARE_ACTION_HANDLER(action);
        ALL_SHORTCUT_ACTIONS
        INTERNAL_SHORTCUT_ACTIONS
#undef ON_ALL_ACTIONS
#pragma endregion

        // ----------------------------------------------------------------
        // Workspaces (experimental.workspaces.enabled). When the flag is
        // ON, the migrated user actions for this slice (startup-replay
        // and default-profile new-tab) route through WorkspaceActions ->
        // diff -> WorkspaceView, which reaches back into the methods
        // below to mutate classic XAML state. When the flag is OFF, the
        // model state stays empty and these helpers are not invoked.
        // ----------------------------------------------------------------
        ::WorkspaceModel::ModelState _workspaceModelState{ nullptr };
        std::unique_ptr<WorkspaceView> _workspaceView{ nullptr };

        // Big-flip Slice B (#54): TEST-ONLY factory override for the
        // ContentMounted content factory. When set, _makePaneContentForSpec
        // returns this instead of building a real TermControl-backed
        // TerminalPaneContent. Lets a headless attach test mount a
        // MockPaneContent whose GetRoot() is a known, stable FrameworkElement,
        // so the host-attach plumbing can be asserted by identity without
        // depending on a real control's root. Empty in production — the live
        // factory path is unchanged.
        std::function<winrt::TerminalApp::IPaneContent(const ::WorkspaceModel::TabContent&)> _makePaneContentForSpecOverrideForTest{ nullptr };

        // Phase 1: one model workspace == one classic window-level tab.
        // The view's TabAdded arm registers the classic Tab against the
        // workspace's id after creation; the WorkspaceRemoved arm looks
        // the Tab up to drive the classic teardown. Phase 2 Slice 4
        // replaces this with the ContentRegistry / per-leaf TabView
        // mapping.
        std::unordered_map<::WorkspaceModel::WorkspaceId, winrt::weak_ref<winrt::TerminalApp::Tab>> _workspaceClassicTabs;

        bool _workspacesFlagEnabled() const noexcept;

        // Big-flip F-4 (#46): the single window-stay-open predicate the two
        // close-decision guards (_CompleteInitialization, _RemoveTab) consult.
        // Flag-off it is the exact inverse of the upstream `_tabs.Size() == 0`
        // checks (byte-for-byte). Flag-on it reflects the model's workspace
        // count instead — but only once the model exists; before the shell is
        // populated it falls back to the classic `_tabs.Size()` count so
        // startup never spuriously closes. F-5 relies on this once classic
        // tabs stop being built.
        bool _windowShouldStayOpen() const;

        void _ensureWorkspaceView();
        void _applyWorkspaceAction(::WorkspaceModel::ModelState newState);

        // Called by WorkspaceView::apply(TabAdded) for a default-profile
        // TerminalSpec. Invokes the classic _OpenNewTab(nullptr) path so
        // the observable result is identical to the flag-off path.
        // Returns the newly-created classic Tab, or nullptr if
        // _OpenNewTab failed to append a tab (e.g. spawn failure). The
        // caller MUST treat nullptr as "do not bind a registry entry"
        // — see _registerClassicTabForWorkspace.
        winrt::TerminalApp::Tab _openDefaultTabForWorkspace();

        // Slice 3 wiring. Called by the WorkspaceView arms to bind /
        // dispatch classic XAML lifecycle around the model. The Tab is
        // passed in explicitly (rather than inferred from _tabs.back())
        // so spawn failures don't mis-bind the new workspace to a
        // pre-existing tab. Callers must pass nullptr if no Tab was
        // actually created.
        void _registerClassicTabForWorkspace(::WorkspaceModel::WorkspaceId ws,
                                             const winrt::TerminalApp::Tab& tab);
        void _removeClassicTabForRemovedWorkspace(::WorkspaceModel::WorkspaceId ws);

        // Phase 2 id-resolver foundation (#45/#44). Resolves a stable
        // WorkspaceId to the CURRENT display index of its classic Tab in
        // _tabs, by looking the workspace up in _workspaceClassicTabs and
        // asking the live _tabs vector for that Tab's index. Returns
        // std::nullopt when the id is unknown (no registry entry), the
        // weak_ref has expired, or the Tab is no longer in _tabs — so a
        // missing/stale id can never route a selection or decoration to the
        // wrong tab via a positional cast. This replaces the Phase-1
        // "workspace display index == classic tab index" assumption the
        // WorkspaceChange arms used to bake in.
        std::optional<std::uint32_t> _classicTabIndexForWorkspace(::WorkspaceModel::WorkspaceId ws) const;

        // Big-flip Slice C (#54): true when this workspace ALREADY has a classic
        // Tab registered (its first tab was materialised by an earlier TabAdded
        // arm + _registerClassicTabForWorkspace). The TabAdded arm uses this to
        // distinguish a NEW workspace's first tab (no entry yet -> create the
        // classic Tab, as today) from an ADDITIONAL tab in an existing leaf
        // (entry exists -> the strip VM is the SOLE representation; create NO
        // second classic Tab). This is the view-side distinguisher the model
        // arm lacks: a newTab on an existing leaf emits TabAdded with
        // leafInsideSplit==false, indistinguishable from a new-workspace first
        // tab by the arm's own fields, but the owning workspace's classic Tab
        // is already registered at that point only in the additional-tab case
        // (newWorkspace fires WorkspaceAdded — which does NOT register a tab —
        // before its first TabAdded does).
        [[nodiscard]] bool _workspaceHasClassicTab(::WorkspaceModel::WorkspaceId ws) const;

        // Drag tear-out / move-tab-to-window destroy a classic Tab
        // without firing Tab::Closed. When the workspaces flag is on,
        // those paths would otherwise leave a stale weak_ref<Tab> in
        // _workspaceClassicTabs (the workspace becomes a zombie in the
        // model). This helper erases any registry entry whose weak_ref
        // resolves to the given Tab. Phase 2 will route move-tab/tear-
        // out through closeWorkspace + newWorkspace dispatch via the
        // model; until then this keeps the registry tidy.
        void _eraseClassicTabFromRegistry(const winrt::TerminalApp::Tab& tab);

        // Flag-on routing: triggered by the registered Tab::Closed
        // handler when a classic Tab raises Closed via tab.Close() (the
        // tab strip's close button and the CloseTab / ClosePane action
        // handlers both end up here). Finds the model TabId/WorkspaceId
        // for this Tab and dispatches the model-side closeTab. The
        // diff's WorkspaceRemoved arm calls _RemoveTab to actually tear
        // down the XAML.
        void _closeTabViaWorkspaceModel(const winrt::TerminalApp::Tab& tab);

        // Called by WorkspaceView::apply(TabAdded) for a non-default
        // TerminalSpec. `profileBytes` is the 16-byte canonical GUID
        // layout that the model carries (matches winrt::guid in-memory
        // order on Windows). Routes through _OpenNewTab so the
        // observable result mirrors flag-off explicit-profile new-tab.
        winrt::TerminalApp::Tab _openProfileTabForWorkspace(const std::array<std::uint8_t, 16>& profileBytes);

        // Called by WorkspaceView::apply(TabDecorationUpdated). Applies
        // the rename + color combination to the classic Tab at index
        // `classicTabIdx` (which Phase 1 keeps in lockstep with the
        // workspace's display index).
        void _applyTabDecoration(std::uint32_t classicTabIdx,
                                 const std::string& customTitle,
                                 const std::optional<::WorkspaceModel::Color>& runtimeColor);

        // Slice 6 helpers used by the AppActionHandlers when the
        // workspaces flag is on. Each one resolves the focused classic
        // Tab to its model TabId (Phase 1: tab idx == workspace idx)
        // and returns std::nullopt if the focused tab has no model
        // counterpart (which means the action should fall back to the
        // classic path).
        std::optional<::WorkspaceModel::TabId> _focusedTabModelId() const;

        // Slice 6 review fix: maps an arbitrary classic Tab back to its
        // model TabId. Right-click tab-strip actions (rename / color)
        // arrive with `sender` = the right-clicked tab, which may not
        // be the focused tab. Using _focusedTabModelId there mutates
        // the wrong workspace; use this helper instead so the flag-on
        // path matches the classic `_senderOrFocusedTab(sender)`
        // semantics. Phase 1 invariant: classic _tabs index ==
        // workspace display index, with exactly one leaf and one tab
        // per workspace.
        std::optional<::WorkspaceModel::TabId> _modelIdForTab(const winrt::TerminalApp::Tab& tab) const;

        // Called by WorkspaceView::apply(LeafPaneCreated) when a new
        // sibling leaf is added inside an existing workspace's split
        // tree. Drives the classic _SplitPane on the focused window-
        // tab so the visible Pane tree matches the model.
        void _splitFocusedPaneForWorkspace(::WorkspaceModel::Axis axis,
                                           double ratio);

        // Slice 5: after a classic _ResizePane has moved the visible
        // separator, nudge the focused-pane-adjacent SplitPane ratio in
        // the same direction so the model stays in step. A no-op when the
        // active leaf has no parent split (single-pane workspace).
        void _mirrorResizeIntoModel(const winrt::Microsoft::Terminal::Settings::Model::ResizeDirection& direction);

        // Helpers used by the slice 5 flag-on routes. They look up the
        // active workspace's active leaf model id (or std::nullopt when
        // the model is empty / the active leaf isn't known yet).
        std::optional<::WorkspaceModel::PaneId> _activeLeafModelId() const;
        // Flag-on new-tab dispatch (Slice F follow-up): create a tab in the
        // active workspace's focused leaf via WorkspaceModel::newTab, falling
        // back to newWorkspace ONLY when there is no active workspace/leaf
        // (startup / empty model). Used by every flag-on new-tab intent in
        // _HandleNewTab so the keybinding (Ctrl+Shift+T) adds a tab to the
        // focused leaf rather than spawning a whole new workspace.
        void _dispatchNewTabInActiveLeafOrWorkspace(const ::WorkspaceModel::TerminalSpec& spec);
        // Find the SplitPane that is the immediate parent of the active
        // leaf in the active workspace, or std::nullopt when the active
        // leaf is the workspace root (single-pane workspace). This is the
        // separator XAML resizes nearest the focused pane, so it is the
        // split resize-pane must address for the model's resizePane action.
        std::optional<::WorkspaceModel::PaneId> _focusedSplitIdInActiveWorkspace() const;

        // ----------------------------------------------------------------
        // Phase 2 Slice 2 (#46): the visible workspaces UI shell. None of
        // this drives the model — the sidebar is a pure read-only mirror of
        // workspace state projected through the WorkspaceView arms, and the
        // chrome buttons are present-but-inert this slice.
        // ----------------------------------------------------------------

        // Realized only on the flag-on path from Create(): loads the inline
        // WorkspaceChrome + sidebar (x:Load="False" elements), shows the
        // sidebar column, and hands the chrome to the host titlebar so the
        // window tab strip drops into the client area under it. A no-op when
        // the flag is off, so flag-off rendering is byte-for-byte upstream.
        void _initializeWorkspaceShell();

        // Big-flip Slice B (#54): parent the given factory-built content's
        // GetRoot() into the (collapsed) WorkspaceContentHost as its SOLE
        // child. Called by WorkspaceView's attach helper for the ACTIVE
        // workspace's content on ContentMounted and on ActiveWorkspaceChanged
        // (AFTER the classic _SelectTab swap, which clears _tabContent's
        // children — including this host — so we re-append the host first).
        // A no-op when the flag-on host hasn't been realized (flag-off /
        // pre-shell) or the content has no root. The host stays Collapsed, so
        // this changes NOTHING the user sees — the classic _tabContent swap
        // still owns the visible display this slice. Guards the "element
        // already has a parent" reparent by clearing the host first.
        void _attachContentToWorkspaceHost(const winrt::TerminalApp::IPaneContent& content);

        // Test-only observer (Big-flip Slice B, #54): the FrameworkElement
        // currently parented as the WorkspaceContentHost's sole child, or
        // nullptr. Lets a page test assert the attach plumbing parented the
        // active workspace's content GetRoot() by identity, without driving
        // any real geometry.
        [[nodiscard]] winrt::Windows::UI::Xaml::FrameworkElement _workspaceHostChildForTest() const;

        // New-workspace chrome button helpers. _createNewWorkspace dispatches
        // the Phase-1 create path (newWorkspace -> _applyWorkspaceAction). The
        // chevron's per-profile menu is built by the shared _buildNewItemFlyout;
        // _createWorkspaceFromArgs bridges that menu's chosen NewTerminalArgs to
        // the workspace create path.
        void _createNewWorkspace(std::optional<winrt::guid> profile);
        void _createWorkspaceFromArgs(winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs args);

        // Sidebar projection helpers, called by the WorkspaceView arms (#52,
        // Stage 2). They are the ONLY mutators of the observable view-model
        // collection below; each resolves a workspace by its stable
        // WorkspaceId.v identity (matching the S1 resolver philosophy) rather
        // than by a positional index into the workspace list. The UI is a pure
        // downstream projection — these never read XAML state back.
        void _addWorkspaceVm(::WorkspaceModel::WorkspaceId ws, const std::string& name, const std::optional<::WorkspaceModel::Color>& color, bool pinned);
        void _removeWorkspaceVm(::WorkspaceModel::WorkspaceId ws);
        void _setActiveWorkspaceVm(::WorkspaceModel::WorkspaceId active);
        void _updateWorkspaceVm(::WorkspaceModel::WorkspaceId ws, const std::string& name, const std::optional<::WorkspaceModel::Color>& color, bool pinned);
        void _reorderWorkspaceVms(const std::vector<::WorkspaceModel::WorkspaceId>& order);

        // The observable per-workspace view-models, in declared (top→bottom)
        // order. Set once as the sidebar ItemsControl's ItemsSource; mutated
        // ONLY by the helpers above.
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::TerminalApp::WorkspaceViewModel> _workspaceViewModels{ winrt::single_threaded_observable_vector<winrt::TerminalApp::WorkspaceViewModel>() };

        // The realized sidebar ItemsControl, or nullptr when the flag is off /
        // the shell hasn't been initialized. Its ItemsSource is the observable
        // collection above; its DataTemplate renders one row per view-model.
        winrt::Windows::UI::Xaml::Controls::ItemsControl _workspaceSidebar{ nullptr };

        // Big-flip Slice C (#54): per-leaf tab-strip projection helpers, called
        // by the WorkspaceView arms (TabAdded / TabRemoved / ActiveTabChanged).
        // They are the ONLY mutators of the per-leaf observable collections
        // below; each resolves a tab by its stable TabId.v identity, never by a
        // positional index. The strip is a pure downstream projection — these
        // never read XAML state back, and they create NO classic Tab (the strip
        // is the sole representation of an additional leaf tab; the classic Tab
        // creation for a new workspace's FIRST tab stays in the TabAdded arm).
        // INVISIBLE this slice: the strip ListView lives in the still-Collapsed
        // WorkspaceContentHost, so nothing here is on screen.
        void _appendPaneTabVm(::WorkspaceModel::PaneId leaf, ::WorkspaceModel::TabId tab, const std::string& title);
        void _removePaneTabVm(::WorkspaceModel::PaneId leaf, ::WorkspaceModel::TabId tab);
        void _setActivePaneTabVm(::WorkspaceModel::PaneId leaf, ::WorkspaceModel::TabId tab);

        // Live pane-tab title. Set the strip view-model whose stable Id matches
        // `tab` to `title`, resolving the VM across ALL leaf strips by id (the
        // ContentMounted arm that drives this carries only a TabId, not the
        // leaf). The WorkspaceView's ContentMounted arm calls this with the live
        // IPaneContent's current Title() and again from its TitleChanged
        // subscription, so the row label tracks the running terminal's title
        // (mirroring how the classic Tab derives its title from content.Title()).
        // A no-op when no strip holds a VM with that id. Returns the matching VM
        // (empty when none) so the caller can verify the bind without a re-scan.
        winrt::TerminalApp::PaneTabViewModel _setPaneTabTitleForTab(::WorkspaceModel::TabId tab, const winrt::hstring& title);

        // Big-flip Slice C (#54): flip the strip's active row to the tab at
        // model index `idx` in `leaf` (ActiveTabChanged carries the leaf + the
        // new activeTabIdx). The strip collection holds one VM per model tab in
        // declared order — the same order the leaf's tabs are emitted/appended —
        // so `idx` indexes the strip directly. Returns the TabId now made active
        // (so the arm can swap the host's content to it), or an invalid TabId
        // when the leaf has no strip or `idx` is out of range.
        [[nodiscard]] ::WorkspaceModel::TabId _activatePaneTabByIndex(::WorkspaceModel::PaneId leaf, std::size_t idx);

        // Resolve the observable strip collection for `leaf`, creating it on
        // first use. The strip ListView's ItemsSource is bound to the ACTIVE
        // leaf's collection (Slice F selects which); this slice only maintains
        // the per-leaf projection behind the collapsed host.
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::TerminalApp::PaneTabViewModel>& _paneTabStripForLeaf(::WorkspaceModel::PaneId leaf);

        // Test-only observer (Big-flip Slice C, #54): the count of strip
        // view-models for a given leaf (0 when the leaf has no strip yet), and
        // whether a given leaf's tab is the active one. Lets a page test assert
        // the strip projection grows / flips / shrinks per arm without driving
        // any real geometry.
        [[nodiscard]] uint32_t _paneTabStripSizeForTest(::WorkspaceModel::PaneId leaf) const;
        [[nodiscard]] std::optional<uint64_t> _activePaneTabIdForTest(::WorkspaceModel::PaneId leaf) const;
        [[nodiscard]] std::optional<winrt::hstring> _paneTabStripFirstTitleForTest(::WorkspaceModel::PaneId leaf) const;

        // The per-leaf observable tab-strip view-models, keyed by the leaf's
        // PaneId. Mutated ONLY by the helpers above (driven by the arms). A leaf
        // gets an entry the first time a tab is projected into it; entries are
        // dropped when their leaf disappears (a later slice; single-leaf scope
        // here). The strip ListView in TerminalPage.xaml binds to the ACTIVE
        // leaf's collection — invisible this slice (host Collapsed).
        std::unordered_map<::WorkspaceModel::PaneId, winrt::Windows::Foundation::Collections::IObservableVector<winrt::TerminalApp::PaneTabViewModel>> _paneTabStrips;

        // Big-flip Slice F-0 (#54): the per-leaf content host Grids, keyed by the
        // leaf's PaneId. _projectLeafContainer creates one host per leaf in the
        // projected pane tree and records it here; the WorkspaceView arms resolve
        // each leaf's active-tab content and parent its GetRoot() into the
        // matching host (via _attachContentToLeafHost), so a SPLIT workspace
        // renders each leaf's terminal in its OWN cell. A rebuild re-creates the
        // hosts (the old tree is discarded), so this map is REPLACED — not
        // appended — on each rebuild; stale entries for leaves no longer in the
        // tree are pruned there. Lives inside the Collapsed _workspaceContentHost,
        // so the hosts are invisible this slice.
        std::unordered_map<::WorkspaceModel::PaneId, winrt::Windows::UI::Xaml::Controls::Grid> _paneContentHosts;

        // Big-flip Slice C (#54): the realized flag-on strip ListView (the
        // x:Load="False" PaneTabStrip inside the WorkspaceContentHost), resolved
        // via FindName in _initializeWorkspaceShell. nullptr when the flag is off
        // / the shell hasn't initialized. Its ItemsSource is the active leaf's
        // strip collection; it lives inside the Collapsed host, so it is
        // invisible this slice. Production triggers (row tap / close / +) are
        // deferred to Slice F.
        winrt::Windows::UI::Xaml::Controls::ListView _paneTabStrip{ nullptr };

        // Big-flip Slice D (#54): rebuild the projected SPLIT pane tree of the
        // ACTIVE workspace inside the (Collapsed) WorkspacePaneTreeRoot. Called
        // by the WorkspaceView split arms (SplitPaneCreated / SplitPaneCollapsed
        // / SplitRatioChanged) and on the per-leaf strip arms so the projection
        // tracks the model. We REBUILD the whole active-workspace subtree from
        // the model's `root` (the single source of truth) rather than do
        // incremental tree surgery: a rebuild keeps the XAML structurally
        // identical to `root` by construction (illegal divergent states are
        // unrepresentable), and it REUSES each leaf's existing Slice-C strip
        // collection by PaneId, so no PaneTabViewModel or content is dropped or
        // duplicated across a rebuild. Each SplitPane node projects to a Grid
        // with two star-sized cells (ratio / 1-ratio) along the axis plus a
        // CUSTOM draggable separator Border between them (Slice F-1, #54 — NOT a
        // toolkit GridSplitter; see _projectPaneNode); each LeafPane projects to
        // a leaf container holding a per-leaf strip
        // ListView (bound to that leaf's strip collection). The leaf's live
        // content-host attach is also a Slice F concern (only one content host
        // exists today). A no-op when the flag-on container hasn't been realized
        // (flag-off / pre-shell) or there is no active workspace. INVISIBLE: the
        // tree lives inside the still-Collapsed host.
        void _rebuildActiveWorkspacePaneTree();

        // Big-flip Slice D (#54): project one model PaneNode (resolved by id from
        // the active workspace's tree) into a FrameworkElement. A SplitPane ->
        // a split Grid (two star-sized cells + a placeholder separator); a
        // LeafPane -> a leaf container holding the leaf's reused Slice-C strip.
        // Each projected node carries its PaneId.v in Tag() so a test (and a
        // later incremental optimisation) can resolve nodes by id. Returns
        // nullptr for an unknown/empty node.
        winrt::Windows::UI::Xaml::FrameworkElement _projectPaneNode(::WorkspaceModel::PaneId nodeId);

        // Build a leaf container for `leaf`: a Grid (tagged with the leaf's id)
        // holding a per-leaf strip ListView bound to that leaf's reused Slice-C
        // strip collection, and (Big-flip Slice F-0, #54) a per-leaf content
        // host Grid in the star row that the leaf's live content GetRoot() is
        // parented into. The content host is recorded in _paneContentHosts by
        // PaneId so a fresh rebuild re-registers each leaf's host.
        winrt::Windows::UI::Xaml::FrameworkElement _projectLeafContainer(::WorkspaceModel::PaneId leaf);

        // Big-flip Slice F-0 (#54): parent `contentRoot` into the per-leaf
        // content host for `leaf` as its SOLE child. This is the per-leaf
        // analogue of _attachContentToWorkspaceHost: the single shared host is
        // the OUTER wrapper (it holds the WorkspacePaneTreeRoot), and the
        // projected pane tree now carries one content host PER LEAF inside its
        // leaf containers, so a SPLIT workspace renders each leaf's terminal in
        // its own cell (the single shared host cannot represent a split). Clears
        // the leaf host first so re-parenting a root that still has a prior
        // parent is always safe (a FrameworkElement has a single parent). A
        // no-op when the leaf has no realized host (e.g. flag-off / pre-shell /
        // a not-yet-projected leaf) or the root is null. INVISIBLE: the whole
        // tree lives inside the still-Collapsed _workspaceContentHost.
        void _attachContentToLeafHost(::WorkspaceModel::PaneId leaf, const winrt::Windows::UI::Xaml::FrameworkElement& contentRoot);

        // Big-flip Slice F-0 (#54): the (leaf, tab-to-show) pairs for every leaf
        // that currently has a projected content host, so the WorkspaceView can
        // resolve each leaf's content (via _contentByTab + the registry) and
        // drive _attachContentToLeafHost per leaf. The tab-to-show is the leaf's
        // ACTIVE strip VM (IsActive()) when one exists, else the leaf's FIRST
        // strip row — so a startup / split-sibling leaf whose single tab has not
        // yet been flipped active (the first-tab IsActive seed is F-2) still
        // gets its one content attached. A leaf with a host but an empty strip is
        // omitted (nothing to attach). The page owns the leaf->tab projection;
        // the view owns tab->content — this seam keeps each on its own side.
        [[nodiscard]] std::vector<std::pair<::WorkspaceModel::PaneId, ::WorkspaceModel::TabId>> _leafContentTabs() const;

        // Test-only observer (Big-flip Slice F-0, #54): the FrameworkElement
        // currently parented as `leaf`'s per-leaf content host's sole child, or
        // nullptr when the leaf has no host / no content attached. Lets a page
        // test assert that EACH split leaf's host holds its OWN distinct content
        // root by identity, without driving any laid-out geometry.
        [[nodiscard]] winrt::Windows::UI::Xaml::FrameworkElement _leafHostChildForTest(::WorkspaceModel::PaneId leaf) const;

        // Test-only observer (Big-flip Slice D, #54): the single projected child
        // of WorkspacePaneTreeRoot — the active workspace's projected pane-tree
        // root (a split Grid or a leaf container), or nullptr when the container
        // is unrealized / empty. Lets a page test assert the projected structure
        // + per-split star ratios by walking the element tree, never measuring
        // laid-out pixels (the headless-resize-clamp trap).
        [[nodiscard]] winrt::Windows::UI::Xaml::FrameworkElement _workspacePaneTreeRootChildForTest() const;

        // Big-flip Slice F-1 (#54): the custom draggable split divider.
        // _computeSplitRatioFromDrag is the HEADLESS-TESTABLE core: given the
        // split's stable id, a drag delta (px along the axis), and the cell's
        // laid-out total extent, it returns the clamped new ratio
        // (currentRatio + dragDelta/totalExtent), or the current ratio unchanged
        // when the id is not a split or the extent is non-positive (the
        // invisible/headless case — no layout). _resizeSplitFromDrag computes via
        // that helper and, only if the ratio changed, dispatches the model's
        // resizePane action through _applyWorkspaceAction (the same diff→
        // re-project path every action takes), which re-projects the split Grid's
        // two GridLengths. BOTH the live pointer handler (which reads the real
        // extent at drag time, fires only once visible at F-5) and the TAEF drag-
        // simulation test call _resizeSplitFromDrag, so the path is identical.
        [[nodiscard]] double _computeSplitRatioFromDrag(::WorkspaceModel::PaneId splitId, double dragDelta, double totalExtent) const;
        void _resizeSplitFromDrag(::WorkspaceModel::PaneId splitId, double dragDelta, double totalExtent);

        // Test-only observer (Big-flip Slice F-1, #54): the custom split-divider
        // Border inside a projected split Grid (the direct Border child whose
        // Tag() == the split id), or nullptr when none exists (e.g. a flag-off
        // mirror builds no separator). Lets a page test assert the separator was
        // built — by element type AND the split id it controls — without driving
        // any laid-out geometry.
        [[nodiscard]] winrt::Windows::UI::Xaml::Controls::Border _splitSeparatorForTest(const winrt::Windows::UI::Xaml::Controls::Grid& splitGrid) const;

        // Big-flip Slice D (#54): the realized flag-on projected-pane-tree
        // container (the x:Load="False" WorkspacePaneTreeRoot inside the
        // WorkspaceContentHost), resolved via FindName in
        // _initializeWorkspaceShell. nullptr when the flag is off / the shell
        // hasn't initialized. Holds the active workspace's projected pane tree;
        // lives inside the Collapsed host, so it is invisible this slice.
        winrt::Windows::UI::Xaml::Controls::Grid _workspacePaneTreeRoot{ nullptr };

        friend class WorkspaceView;
        friend class TerminalAppLocalTests::TabTests;
        friend class TerminalAppLocalTests::SettingsTests;
        friend class TerminalAppLocalTests::WorkspaceTests;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TerminalPage);
}
