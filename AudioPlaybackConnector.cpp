#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
winrt::fire_and_forget ConnectDevice(DevicePicker, DeviceInformation);
winrt::fire_and_forget ConnectDevice(DevicePicker, std::wstring_view);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();
winrt::fire_and_forget SetupMediaSession();
winrt::fire_and_forget UpdateNowPlayingCache();

// Passed via PostMessageW lParam to marshal DeviceInformation to main thread
struct ReconnectData {
	DeviceInformation device;
};

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	winrt::init_apartment();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	// Capture dispatcher queue for marshalling to UI thread from background callbacks
	g_dispatcherQueue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();

	LoadSettings();
	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();
	SetupMediaSession();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		KillTimer(hWnd, IDT_RECONNECT);
		g_pendingReconnect.clear();
		g_reconnectAttempts.clear();
		for (const auto& connection : g_audioPlaybackConnections)
		{
			connection.second.second.Close();
			g_devicePicker.SetDisplayStatus(connection.second.first, {}, DevicePickerDisplayStatusOptions::None);
		}
		if (g_reconnect)
		{
			SaveSettings();
			g_audioPlaybackConnections.clear();
		}
		else
		{
			g_audioPlaybackConnections.clear();
			SaveSettings();
		}
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
			SetForegroundWindow(hWnd);
			g_devicePicker.Show(rect, Placement::Above);
		}
		break;
		case WM_RBUTTONUP: // Menu activated by mouse click
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
		break;
	case WM_CONNECTDEVICE:
		// Startup reconnect to last saved devices
		if (g_reconnect)
		{
			for (const auto& i : g_lastDevices)
			{
				ConnectDevice(g_devicePicker, i);
			}
			g_lastDevices.clear();
		}
		break;
	case WM_RECONNECTDEVICE:
	{
		// Runtime reconnect: lParam is heap-allocated ReconnectData*, we own it
		auto pData = std::unique_ptr<ReconnectData>(reinterpret_cast<ReconnectData*>(lParam));
		auto deviceId = std::wstring(pData->device.Id());

		// Track this device for reconnection
		g_pendingReconnect.push_back(pData->device);
		g_reconnectAttempts[deviceId] = 0;

		// Start (or reset) the reconnect timer
		SetTimer(hWnd, IDT_RECONNECT, RECONNECT_DELAY_MS, nullptr);
		break;
	}
	case WM_TIMER:
		if (wParam == IDT_RECONNECT)
		{
			std::vector<DeviceInformation> stillPending;
			for (auto& device : g_pendingReconnect)
			{
				auto deviceId = std::wstring(device.Id());

				// Already reconnected by some other means
				if (g_audioPlaybackConnections.count(deviceId))
				{
					g_reconnectAttempts.erase(deviceId);
					continue;
				}

				auto& attempts = g_reconnectAttempts[deviceId];
				if (attempts < MAX_RECONNECT_ATTEMPTS)
				{
					attempts++;
					ConnectDevice(g_devicePicker, device);
					stillPending.push_back(device);
				}
				else
				{
					// Give up — show retry button so user can manually retry
					g_devicePicker.SetDisplayStatus(device, _(L"Connection lost"), DevicePickerDisplayStatusOptions::ShowRetryButton);
					g_reconnectAttempts.erase(deviceId);
				}
			}
			g_pendingReconnect = std::move(stillPending);

			if (g_pendingReconnect.empty())
				KillTimer(hWnd, IDT_RECONNECT);
		}
		break;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.Margin({ 0, 0, 0, 12 });

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Click([](const auto&, const auto&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(checkbox);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);

	g_xamlFlyout = flyout;
}

void SetupMenu()
{
	// --- Now Playing display ---
	FontIcon musicIcon;
	musicIcon.Glyph(L"\xE8D6");

	g_nowPlayingItem = MenuFlyoutItem();
	g_nowPlayingItem.Icon(musicIcon);
	g_nowPlayingItem.Visibility(Visibility::Collapsed);
	// Display-only item — clicking it has no action

	// --- Media transport controls ---
	FontIcon prevIcon;
	prevIcon.Glyph(L"\xE892");
	g_prevItem = MenuFlyoutItem();
	g_prevItem.Text(_(L"Previous"));
	g_prevItem.Icon(prevIcon);
	g_prevItem.Visibility(Visibility::Collapsed);
	g_prevItem.Click([](const auto&, const auto&) {
		if (g_smtcSessionManager)
		{
			auto session = g_smtcSessionManager.GetCurrentSession();
			if (session) session.TrySkipPreviousAsync();
		}
	});

	g_playPauseItem = MenuFlyoutItem();
	g_playPauseItem.Visibility(Visibility::Collapsed);
	g_playPauseItem.Click([](const auto&, const auto&) {
		if (g_smtcSessionManager)
		{
			auto session = g_smtcSessionManager.GetCurrentSession();
			if (session) session.TryTogglePlayPauseAsync();
		}
	});

	FontIcon nextIcon;
	nextIcon.Glyph(L"\xE893");
	g_nextItem = MenuFlyoutItem();
	g_nextItem.Text(_(L"Next"));
	g_nextItem.Icon(nextIcon);
	g_nextItem.Visibility(Visibility::Collapsed);
	g_nextItem.Click([](const auto&, const auto&) {
		if (g_smtcSessionManager)
		{
			auto session = g_smtcSessionManager.GetCurrentSession();
			if (session) session.TrySkipNextAsync();
		}
	});

	g_mediaSeparator = MenuFlyoutSeparator();
	g_mediaSeparator.Visibility(Visibility::Collapsed);

	// --- Toggles ---
	g_showNowPlayingToggle = ToggleMenuFlyoutItem();
	g_showNowPlayingToggle.Text(_(L"Show Now Playing"));
	g_showNowPlayingToggle.IsChecked(g_showNowPlaying);
	g_showNowPlayingToggle.Click([](const auto&, const auto&) {
		g_showNowPlaying = g_showNowPlayingToggle.IsChecked();
		SaveSettings();
	});

	g_autoReconnectToggle = ToggleMenuFlyoutItem();
	g_autoReconnectToggle.Text(_(L"Auto-Reconnect"));
	g_autoReconnectToggle.IsChecked(g_autoReconnect);
	g_autoReconnectToggle.Click([](const auto&, const auto&) {
		g_autoReconnect = g_autoReconnectToggle.IsChecked();
		SaveSettings();
	});

	MenuFlyoutSeparator separator;

	// --- Existing items ---
	// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		if (g_audioPlaybackConnections.size() == 0)
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	MenuFlyout menu;
	menu.Items().Append(g_nowPlayingItem);
	menu.Items().Append(g_prevItem);
	menu.Items().Append(g_playPauseItem);
	menu.Items().Append(g_nextItem);
	menu.Items().Append(g_mediaSeparator);
	menu.Items().Append(g_showNowPlayingToggle);
	menu.Items().Append(g_autoReconnectToggle);
	menu.Items().Append(separator);
	menu.Items().Append(settingsItem);
	menu.Items().Append(exitItem);

	menu.Opened([](const auto& sender, const auto&) {
		GlobalSystemMediaTransportControlsSession session = nullptr;
		if (g_smtcSessionManager)
			session = g_smtcSessionManager.GetCurrentSession();

		bool hasSession = session != nullptr;
		bool showNowPlayingItem = g_showNowPlaying && hasSession && !g_nowPlayingText.empty();

		g_nowPlayingItem.Visibility(showNowPlayingItem ? Visibility::Visible : Visibility::Collapsed);
		if (showNowPlayingItem)
			g_nowPlayingItem.Text(g_nowPlayingText);

		Visibility mediaVis = hasSession ? Visibility::Visible : Visibility::Collapsed;
		g_prevItem.Visibility(mediaVis);
		g_playPauseItem.Visibility(mediaVis);
		g_nextItem.Visibility(mediaVis);
		g_mediaSeparator.Visibility(mediaVis);

		if (hasSession)
		{
			// Query playback status live so icon is always accurate
			auto playbackInfo = session.GetPlaybackInfo();
			bool isPlaying = playbackInfo.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
			FontIcon playPauseIcon;
			playPauseIcon.Glyph(isPlaying ? L"\xE769" : L"\xE768");
			g_playPauseItem.Icon(playPauseIcon);
			g_playPauseItem.Text(isPlaying ? _(L"Pause") : _(L"Play"));
		}

		g_showNowPlayingToggle.IsChecked(g_showNowPlaying);
		g_autoReconnectToggle.IsChecked(g_autoReconnect);

		// Focus last visible item for keyboard navigation
		auto menuFlyout = sender.as<MenuFlyout>();
		auto menuItems = menuFlyout.Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
		{
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
	});
	menu.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = menu;
}

winrt::fire_and_forget SetupMediaSession()
{
	try
	{
		g_smtcSessionManager = co_await GlobalSystemMediaTransportControlsSessionManager::RequestAsync();

		// Initial cache update
		UpdateNowPlayingCache();

		// Subscribe to session and playback changes
		g_smtcSessionManager.CurrentSessionChanged([](const auto&, const auto&) {
			UpdateNowPlayingCache();
		});
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}

winrt::fire_and_forget UpdateNowPlayingCache()
{
	try
	{
		if (!g_smtcSessionManager)
			co_return;

		auto session = g_smtcSessionManager.GetCurrentSession();
		if (!session)
		{
			if (g_dispatcherQueue)
			{
				g_dispatcherQueue.TryEnqueue([]{
					g_nowPlayingText.clear();
				});
			}
			co_return;
		}

		auto props = co_await session.TryGetMediaPropertiesAsync();
		std::wstring text;
		if (props)
		{
			auto title = std::wstring(props.Title());
			auto artist = std::wstring(props.Artist());
			if (artist.empty())
				text = title;
			else
				text = artist + L" \u2013 " + title; // en-dash separator
		}

		if (g_dispatcherQueue)
		{
			g_dispatcherQueue.TryEnqueue([text = std::move(text)]{
				g_nowPlayingText = text;
			});
		}
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	picker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

	bool success = false;
	std::wstring errorMessage;

	try
	{
		auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			g_audioPlaybackConnections.emplace(device.Id(), std::pair(device, connection));

			connection.StateChanged([](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
				{
					auto deviceId = std::wstring(sender.DeviceId());
					auto it = g_audioPlaybackConnections.find(deviceId);
					if (it != g_audioPlaybackConnections.end())
					{
						if (g_autoReconnect)
						{
							// Show reconnecting status and schedule retry on main thread
							g_devicePicker.SetDisplayStatus(it->second.first, _(L"Reconnecting..."),
								DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);
							auto pData = new ReconnectData{ it->second.first };
							PostMessageW(g_hWnd, WM_RECONNECTDEVICE, 0, reinterpret_cast<LPARAM>(pData));
						}
						else
						{
							g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None);
						}
						g_audioPlaybackConnections.erase(it);
					}
					sender.Close();
				}
			});

			co_await connection.StartAsync();
			auto result = co_await connection.OpenAsync();

			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
				success = true;
				break;
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				success = false;
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				success = false;
				winrt::throw_hresult(result.ExtendedError());
				break;
			}
		}
		else
		{
			success = false;
			errorMessage = _(L"Unknown error");
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		success = false;
		errorMessage.resize(64);
		while (1)
		{
			auto result = swprintf(errorMessage.data(), errorMessage.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
			if (result < 0)
			{
				errorMessage.resize(errorMessage.size() * 2);
			}
			else
			{
				errorMessage.resize(result);
				break;
			}
		}
		LOG_CAUGHT_EXCEPTION();
	}

	if (success)
	{
		picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	}
	else
	{
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}
		picker.SetDisplayStatus(device, errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
	auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
	ConnectDevice(picker, device);
}

void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
	g_devicePicker.DeviceSelected([](const auto& sender, const auto& args) {
		ConnectDevice(sender, args.SelectedDevice());
	});
	g_devicePicker.DisconnectButtonClicked([](const auto& sender, const auto& args) {
		auto device = args.Device();
		auto deviceId = std::wstring(device.Id());

		// Cancel any pending reconnect for this device (user-initiated disconnect)
		g_reconnectAttempts.erase(deviceId);
		auto it2 = std::find_if(g_pendingReconnect.begin(), g_pendingReconnect.end(),
			[&deviceId](const DeviceInformation& d) { return std::wstring(d.Id()) == deviceId; });
		if (it2 != g_pendingReconnect.end())
			g_pendingReconnect.erase(it2);
		if (g_pendingReconnect.empty())
			KillTimer(g_hWnd, IDT_RECONNECT);

		auto it = g_audioPlaybackConnections.find(deviceId);
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
	});
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
}

void UpdateNotifyIcon()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	g_nid.hIcon = value != 0 ? g_hIconLight : g_hIconDark;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}
