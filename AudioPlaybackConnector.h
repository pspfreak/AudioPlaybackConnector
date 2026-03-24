#pragma once

#include "resource.h"

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
namespace fs = std::filesystem;

constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
constexpr UINT WM_RECONNECTDEVICE = WM_APP + 3;
constexpr UINT_PTR IDT_RECONNECT = 1;
constexpr UINT RECONNECT_DELAY_MS = 5000;
constexpr int MAX_RECONNECT_ATTEMPTS = 3;

HINSTANCE g_hInst;
HWND g_hWnd;
HWND g_hWndXaml;
Canvas g_xamlCanvas = nullptr;
Flyout g_xamlFlyout = nullptr;
MenuFlyout g_xamlMenu = nullptr;
FocusState g_menuFocusState = FocusState::Unfocused;
DevicePicker g_devicePicker = nullptr;
std::unordered_map<std::wstring, std::pair<DeviceInformation, AudioPlaybackConnection>> g_audioPlaybackConnections;
HICON g_hIconLight = nullptr;
HICON g_hIconDark = nullptr;
NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
NOTIFYICONIDENTIFIER g_niid = {
	.cbSize = sizeof(g_niid)
};
UINT WM_TASKBAR_CREATED = 0;
bool g_reconnect = false;
std::vector<std::wstring> g_lastDevices;

// New: media session and now-playing
GlobalSystemMediaTransportControlsSessionManager g_smtcSessionManager = nullptr;
winrt::Windows::System::DispatcherQueue g_dispatcherQueue = nullptr;
std::wstring g_nowPlayingText;
bool g_showNowPlaying = true;

// New: auto-reconnect at runtime
bool g_autoReconnect = true;
std::vector<DeviceInformation> g_pendingReconnect;
std::unordered_map<std::wstring, int> g_reconnectAttempts;

// Best-matched SMTC session (Bluetooth device preferred over PC apps), updated on UI thread
GlobalSystemMediaTransportControlsSession g_bluetoothSession = nullptr;

// Media control menu items (need global refs for dynamic updates)
MenuFlyoutItem g_nowPlayingItem = nullptr;
ToggleMenuFlyoutItem g_showNowPlayingToggle = nullptr;
MenuFlyoutItem g_prevItem = nullptr;
MenuFlyoutItem g_playPauseItem = nullptr;
MenuFlyoutItem g_nextItem = nullptr;
MenuFlyoutSeparator g_mediaSeparator = nullptr;
ToggleMenuFlyoutItem g_autoReconnectToggle = nullptr;

#include "Util.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "Direct2DSvg.hpp"
