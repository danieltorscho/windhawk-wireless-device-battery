// ==WindhawkMod==
// @id              wireless-device-battery
// @name            Wireless Device Battery
// @description     Show battery percentages for Bluetooth accessories and supported USB wireless mice in the notification area.
// @version         0.3
// @author          danieltorscho
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lshell32 -lgdi32 -lole32 -loleaut32 -lruntimeobject -lcfgmgr32 -lhid
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Wireless Device Battery

Hover over the tray icon to see one battery reading per device. Click it for
the device list and **Refresh now**. If the icon is hidden, look under the
taskbar's **Show hidden icons** arrow.

By default, the taskbar shows up to two rows of clock-sized text on a transparent
background, one for a mouse and one for a keyboard. The hover tooltip uses the
native Windows notification-area tooltip, with one device per line.
In Settings, enter part of a device name to choose it for either row.
Clear a row's setting to hide it. If there are no matching devices, the icon
shows a battery symbol.

The mod reads Bluetooth battery levels from Windows. It also listens for
battery reports from the Attack Shark X11 SE and X11 through the 1D57:FA60
2.4 GHz receiver. A sleeping mouse can retain its last reported level. If no
reading has arrived yet, move the mouse to wake it.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- refreshSeconds: 30
  $name: Refresh interval in seconds
  $description: How often to check Bluetooth readings, from 10 to 300 seconds.
- trayTopDevice: auto
  $name: Top tray row
  $description: Enter a device name or part of it. Leave auto for the mouse or clear to hide the row.
- trayBottomDevice: auto
  $name: Bottom tray row
  $description: Enter a device name or part of it. Leave auto for the keyboard or clear to hide the row.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <hidsdi.h>
#include <roapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <windhawk_utils.h>

#undef GetCurrentTime

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Automation.Peers.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Data.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;

constexpr wchar_t kWindowClass[] = L"Windhawk.WirelessDeviceBattery.Tray";
constexpr wchar_t kUiHostClass[] = L"Windhawk.WirelessDeviceBattery.XamlHost";
constexpr wchar_t kTooltipMarker[] = L"\u2063\u2062";
constexpr UINT kRenderMessage = WM_APP + 2;
constexpr UINT kCloseUiMessage = WM_APP + 3;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayId = 1;
constexpr GUID kTrayGuid{0x39178bbc, 0xdc0c, 0x4219, {0x8b, 0x61, 0x43, 0xe0, 0xea, 0x9d, 0x5b, 0x38}};
constexpr UINT_PTR kMenuTimer = 1;
constexpr UINT kRefreshCommand = 1;

constexpr wchar_t kContainerId[] = L"System.Devices.Aep.ContainerId";
constexpr wchar_t kAddress[] = L"System.Devices.Aep.DeviceAddress";
constexpr wchar_t kBatteryLife[] = L"System.Devices.BatteryLife";
// Some Bluetooth drivers expose their reading under this property key.
constexpr wchar_t kBluetoothBattery[] = L"{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2";
constexpr DEVPROPKEY kNodeBattery{{0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2};
constexpr DEVPROPKEY kNodeAddress{{0x2bd67d8b, 0x8beb, 0x48d5, {0x87, 0xe0, 0x6c, 0xda, 0x34, 0x28, 0x04, 0x0a}}, 1};

HANDLE g_stopEvent;
HANDLE g_settingsEvent;
HANDLE g_thread;
std::atomic<bool> g_unloading{false};
bool g_taskbarHooked = false;

enum class DeviceKind { Other, Mouse, Keyboard };

struct Device {
    std::wstring name;
    std::optional<int> battery;
    DeviceKind kind = DeviceKind::Other;
    bool usbReceiver = false;
};

struct Tray {
    HWND window = nullptr;
    UINT taskbarCreated = 0;
    bool iconAdded = false;
    bool version4 = false;
    bool menuOpen = false;
    bool refreshRequested = true;
    bool queryFailed = false;
    int refreshSeconds = 30;
    std::array<std::wstring, 2> traySelection{L"auto", L"auto"};
    std::vector<Device> devices;
};

template <typename T>
T GetProperty(const DeviceInformation& device, const wchar_t* key) {
    const auto properties = device.Properties();
    if (properties.HasKey(key)) {
        return winrt::unbox_value_or<T>(properties.Lookup(key), T{});
    }
    return {};
}

std::optional<int> GetBattery(const DeviceInformation& device) {
    const auto properties = device.Properties();
    for (const auto* key : {kBluetoothBattery, kBatteryLife}) {
        if (!properties.HasKey(key)) {
            continue;
        }
        const int battery = winrt::unbox_value_or<uint8_t>(properties.Lookup(key), 255);
        if (battery <= 100) {
            return battery;
        }
    }
    return {};
}

std::wstring AddressKey(std::wstring_view raw) {
    std::wstring address;
    for (const wchar_t character : raw) {
        if (character == L':' || character == L'-') {
            continue;
        }
        if (!iswxdigit(character)) {
            return {};
        }
        address += static_cast<wchar_t>(towlower(character));
    }
    return address.size() == 12 ? address : std::wstring{};
}

template <typename T>
std::optional<T> ReadDeviceNodeProperty(DEVINST node, const DEVPROPKEY& key, DEVPROPTYPE expectedType) {
    T value{};
    ULONG size = sizeof(value);
    DEVPROPTYPE type;
    if (CM_Get_DevNode_PropertyW(node, &key, &type, reinterpret_cast<PBYTE>(&value), &size, 0) == CR_SUCCESS &&
        type == expectedType) {
        return value;
    }
    return {};
}

std::vector<wchar_t> PresentDeviceIds(const wchar_t* enumerator) {
    constexpr ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    ULONG length = 0;
    if (CM_Get_Device_ID_List_SizeW(&length, enumerator, flags) != CR_SUCCESS || length < 2) {
        return {};
    }
    std::vector<wchar_t> ids(length);
    if (CM_Get_Device_ID_ListW(enumerator, ids.data(), length, flags) != CR_SUCCESS) {
        return {};
    }
    return ids;
}

std::map<std::wstring, int> ReadDeviceNodeBatteries() {
    std::map<std::wstring, int> readings;
    for (const auto* enumerator : {L"BTHLE", L"BTHENUM", L"BTHHFENUM"}) {
        auto ids = PresentDeviceIds(enumerator);
        for (auto* id = ids.data(); !ids.empty() && *id; id += std::wcslen(id) + 1) {
            if (WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT) {
                return readings;
            }
            DEVINST node;
            if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
                continue;
            }
            const auto battery = ReadDeviceNodeProperty<BYTE>(node, kNodeBattery, DEVPROP_TYPE_BYTE);
            if (!battery || *battery > 100) {
                continue;
            }

            const auto container = ReadDeviceNodeProperty<winrt::guid>(node, DEVPKEY_Device_ContainerId,
                                                                       DEVPROP_TYPE_GUID);
            if (container && *container != winrt::guid{}) {
                readings.try_emplace(std::wstring{winrt::to_hstring(*container)}, *battery);
            }
            const auto rawAddress = ReadDeviceNodeProperty<std::array<wchar_t, 32>>(
                node, kNodeAddress, DEVPROP_TYPE_STRING);
            if (rawAddress && rawAddress->back() == L'\0') {
                const auto address = AddressKey(rawAddress->data());
                if (!address.empty()) {
                    readings.try_emplace(address, *battery);
                }
            }
        }
    }
    return readings;
}

std::map<std::wstring, DeviceKind> ReadHidDeviceKinds() {
    std::map<std::wstring, DeviceKind> kinds;
    auto ids = PresentDeviceIds(L"HID");
    for (auto* id = ids.data(); !ids.empty() && *id; id += std::wcslen(id) + 1) {
        DEVINST node;
        if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
            continue;
        }
        const auto classId = ReadDeviceNodeProperty<GUID>(node, DEVPKEY_Device_ClassGuid, DEVPROP_TYPE_GUID);
        if (!classId || (*classId != GUID_DEVCLASS_KEYBOARD && *classId != GUID_DEVCLASS_MOUSE)) {
            continue;
        }
        const auto container = ReadDeviceNodeProperty<winrt::guid>(node, DEVPKEY_Device_ContainerId, DEVPROP_TYPE_GUID);
        if (!container || *container == winrt::guid{}) {
            continue;
        }
        const std::wstring key{winrt::to_hstring(*container)};
        // Keyboards can also expose a mouse collection for a touchpad or other controls.
        if (*classId == GUID_DEVCLASS_KEYBOARD) {
            kinds[key] = DeviceKind::Keyboard;
        } else {
            kinds.try_emplace(key, DeviceKind::Mouse);
        }
    }
    return kinds;
}

class MouseReceiver {
public:
    MouseReceiver() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    MouseReceiver(const MouseReceiver&) = delete;
    MouseReceiver& operator=(const MouseReceiver&) = delete;

    ~MouseReceiver() {
        Disconnect();
        if (event_) {
            CloseHandle(event_);
        }
    }

    void Connect() {
        if (device_ != INVALID_HANDLE_VALUE || !event_) {
            return;
        }
        GUID hid;
        HidD_GetHidGuid(&hid);
        constexpr ULONG flags = CM_GET_DEVICE_INTERFACE_LIST_PRESENT;
        ULONG length = 0;
        if (CM_Get_Device_Interface_List_SizeW(&length, &hid, nullptr, flags) != CR_SUCCESS || length < 2) {
            return;
        }
        std::vector<wchar_t> paths(length);
        if (CM_Get_Device_Interface_ListW(&hid, nullptr, paths.data(), length, flags) != CR_SUCCESS) {
            return;
        }
        constexpr wchar_t prefix[] = LR"(\\?\hid#vid_1d57&pid_fa60&mi_02)";
        for (const wchar_t* path = paths.data(); *path; path += std::wcslen(path) + 1) {
            if (_wcsnicmp(path, prefix, ARRAYSIZE(prefix) - 1) != 0) {
                continue;
            }
            device_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (device_ == INVALID_HANDLE_VALUE) {
                continue;
            }
            HIDP_CAPS caps{};
            PHIDP_PREPARSED_DATA preparsed;
            if (HidD_GetPreparsedData(device_, &preparsed)) {
                HidP_GetCaps(preparsed, &caps);
                HidD_FreePreparsedData(preparsed);
            }
            if (caps.UsagePage == 0x000A && caps.Usage == 0 && caps.InputReportByteLength >= 5) {
                report_.resize(caps.InputReportByteLength);
                if (ReadNext()) {
                    return;
                }
            }
            Disconnect();
        }
    }

    HANDLE PendingEvent() const {
        return pending_ ? event_ : nullptr;
    }

    std::optional<Device> CurrentDevice() const {
        if (device_ == INVALID_HANDLE_VALUE) {
            return {};
        }
        const wchar_t* name = model_ == 0x07 ? L"Attack Shark X11 SE" :
                              model_ == 0x55 ? L"Attack Shark X11" : L"Attack Shark USB mouse";
        return Device{name, battery_, DeviceKind::Mouse, true};
    }

    bool ReadReady() {
        DWORD size = 0;
        pending_ = false;
        if (!GetOverlappedResult(device_, &read_, &size, FALSE)) {
            Disconnect();
            return true;
        }
        const bool changed = ReceiveReport(size);
        if (!ReadNext()) {
            Disconnect();
            return true;
        }
        return changed;
    }

private:
    bool ReadNext() {
        ResetEvent(event_);
        read_ = {};
        read_.hEvent = event_;
        pending_ = ReadFile(device_, report_.data(), static_cast<DWORD>(report_.size()), nullptr, &read_) ||
                   GetLastError() == ERROR_IO_PENDING;
        return pending_;
    }

    bool ReceiveReport(DWORD size) {
        if (size < 5 || report_[0] != 0x03 || report_[2] != 0x40 || report_[4] > 100 ||
            (report_[1] != 0x07 && report_[1] != 0x55)) {
            return false;
        }
        // Ignore a single transient value, including the receiver's initial 100% report.
        const std::array<uint8_t, 3> sample{report_[1], report_[3], report_[4]};
        const bool confirmed = candidate_ == sample;
        candidate_ = sample;
        if (!confirmed || (model_ == sample[0] && battery_ == sample[2])) {
            return false;
        }
        model_ = sample[0];
        battery_ = sample[2];
        return true;
    }

    void Disconnect() {
        if (device_ != INVALID_HANDLE_VALUE) {
            if (pending_) {
                CancelIoEx(device_, &read_);
                DWORD ignored;
                GetOverlappedResult(device_, &read_, &ignored, TRUE);
            }
            CloseHandle(device_);
            device_ = INVALID_HANDLE_VALUE;
        }
        pending_ = false;
        candidate_ = {};
        model_ = 0;
        battery_.reset();
    }

    HANDLE event_;
    HANDLE device_ = INVALID_HANDLE_VALUE;
    OVERLAPPED read_{};
    bool pending_ = false;
    std::vector<uint8_t> report_;
    std::array<uint8_t, 3> candidate_{};
    uint8_t model_ = 0;
    std::optional<int> battery_;
};

template <typename RuntimeClass, typename Interface>
Interface GetFactory() {
    // An Explorer mod can be disabled and reloaded after its MTA shuts down.
    winrt::hresult_error error;
    auto factory = winrt::try_get_activation_factory<RuntimeClass, Interface>(error);
    if (!factory) {
        throw error;
    }
    return factory;
}

DeviceInformationCollection FindConnected(const winrt::hstring& selector) {
    const auto enumeration = GetFactory<DeviceInformation, IDeviceInformationStatics2>();
    auto query = enumeration.FindAllAsync(
        selector, {kContainerId, kAddress, kBluetoothBattery, kBatteryLife},
        DeviceInformationKind::AssociationEndpoint);

    // Leave completion in Windows. A callback into the mod could outlive unload.
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (query.Status() == AsyncStatus::Started) {
        if (WaitForSingleObject(g_stopEvent, 25) != WAIT_TIMEOUT) {
            query.Cancel();
            return nullptr;
        }
        if (GetTickCount64() >= deadline) {
            query.Cancel();
            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        }
    }
    return query.GetResults();
}

std::wstring DeviceName(const DeviceInformation& device) {
    std::wstring name{device.Name()};
    std::replace_if(name.begin(), name.end(), [](wchar_t character) {
        return iswcntrl(character) != 0;
    }, L' ');
    return name.empty() ? L"Bluetooth device" : name;
}

void RefreshDevices(Tray& tray) {
    std::map<std::wstring, Device> byDevice;
    std::optional<std::map<std::wstring, int>> nodeBatteries;
    const auto kinds = ReadHidDeviceKinds();
    tray.queryFailed = false;

    for (const bool lowEnergy : {true, false}) {
        if (WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT) {
            return;
        }
        try {
            const auto selector = lowEnergy
                ? GetFactory<BluetoothLEDevice, IBluetoothLEDeviceStatics2>()
                    .GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected)
                : GetFactory<BluetoothDevice, IBluetoothDeviceStatics2>()
                    .GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected);
            const auto connected = FindConnected(selector);
            if (!connected) {
                return;
            }
            for (const auto& endpoint : connected) {
                const auto container = GetProperty<winrt::guid>(endpoint, kContainerId);
                const auto address = AddressKey(GetProperty<winrt::hstring>(endpoint, kAddress));
                std::wstring key;
                if (container != winrt::guid{}) {
                    key = winrt::to_hstring(container);
                } else if (!address.empty()) {
                    key = address;
                } else {
                    key = endpoint.Id();
                }

                auto& device = byDevice[key];
                if (device.name.empty()) {
                    device.name = DeviceName(endpoint);
                }
                if (const auto kind = kinds.find(key); kind != kinds.end()) {
                    device.kind = kind->second;
                }
                if (!device.battery.has_value()) {
                    device.battery = GetBattery(endpoint);
                    if (!device.battery) {
                        // HID and audio drivers can report the value on a PnP node.
                        if (!nodeBatteries) {
                            nodeBatteries = ReadDeviceNodeBatteries();
                        }
                        auto reading = nodeBatteries->find(key);
                        if (reading == nodeBatteries->end() && !address.empty()) {
                            reading = nodeBatteries->find(address);
                        }
                        if (reading != nodeBatteries->end()) {
                            device.battery = reading->second;
                        }
                    }
                }
            }
        } catch (const winrt::hresult_error& error) {
            tray.queryFailed = true;
            Wh_Log(L"%ls Bluetooth query failed (0x%08X)",
                   lowEnergy ? L"Low Energy" : L"Classic", static_cast<unsigned>(error.code()));
        }
    }

    tray.devices.clear();
    for (auto& [key, device] : byDevice) {
        tray.devices.push_back(std::move(device));
    }
}

void UpdateReceiverDevice(Tray& tray, const MouseReceiver& receiver) {
    std::erase_if(tray.devices, [](const Device& device) { return device.usbReceiver; });
    if (auto mouse = receiver.CurrentDevice()) {
        tray.devices.push_back(std::move(*mouse));
    }
    std::sort(tray.devices.begin(), tray.devices.end(), [](const Device& first, const Device& second) {
        const auto firstLevel = first.battery.value_or(101);
        const auto secondLevel = second.battery.value_or(101);
        if (firstLevel != secondLevel) {
            return firstLevel < secondLevel;
        }
        return first.name < second.name;
    });
}

std::array<const Device*, 2> SelectedDevices(const Tray& tray) {
    std::array<const Device*, 2> selected{};
    for (size_t row = 0; row < selected.size(); ++row) {
        const auto& name = tray.traySelection[row];
        if (name.empty()) {
            continue;
        }
        const auto automaticKind = row == 0 ? DeviceKind::Mouse : DeviceKind::Keyboard;
        for (const auto& device : tray.devices) {
            if (&device == selected[0]) {
                continue;
            }
            if (name == L"auto") {
                if (device.kind == automaticKind) {
                    selected[row] = &device;
                    break;
                }
                continue;
            }
            if (FindStringOrdinal(FIND_FROMSTART, device.name.c_str(), -1, name.c_str(), -1, TRUE) >= 0) {
                if (!selected[row]) {
                    selected[row] = &device;
                }
                if (device.name.size() == name.size()) {
                    selected[row] = &device;
                    break;
                }
            }
        }
    }
    return selected;
}

namespace Xaml = winrt::Windows::UI::Xaml;
namespace Controls = Xaml::Controls;

struct TaskbarRow {
    std::wstring text;
    DeviceKind kind;
};

std::mutex g_presentationMutex;
std::vector<TaskbarRow> g_presentation;
std::mutex g_uiHostsMutex;
std::vector<HWND> g_uiHosts;
HMODULE g_module;

Xaml::FrameworkElement FindElement(Xaml::DependencyObject root, std::wstring_view name) {
    if (!root) {
        return nullptr;
    }
    if (auto element = root.try_as<Xaml::FrameworkElement>(); element && element.Name() == name) {
        return element;
    }
    const int count = Xaml::Media::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        if (auto match = FindElement(Xaml::Media::VisualTreeHelper::GetChild(root, i), name)) {
            return match;
        }
    }
    return nullptr;
}

struct PropertyChange {
    winrt::weak_ref<Xaml::DependencyObject> target;
    Xaml::DependencyProperty property;
    winrt::Windows::Foundation::IInspectable value;
    Xaml::Data::BindingBase binding{nullptr};
};

struct TaskbarView {
    winrt::weak_ref<Xaml::FrameworkElement> icon;
    winrt::weak_ref<Controls::Grid> grid;
    Controls::StackPanel rows{nullptr};
    std::vector<PropertyChange> changes;
};

struct UiHost {
    std::vector<winrt::weak_ref<Xaml::FrameworkElement>> candidates;
    std::vector<TaskbarView> views;
};

void SetViewProperty(TaskbarView& view, Xaml::DependencyObject target,
                     Xaml::DependencyProperty property, const winrt::Windows::Foundation::IInspectable& value) {
    Xaml::Data::BindingBase binding{nullptr};
    if (const auto element = target.try_as<Xaml::FrameworkElement>()) {
        if (const auto expression = element.GetBindingExpression(property)) {
            binding = expression.ParentBinding();
        }
    }
    view.changes.push_back({winrt::make_weak(target), property, target.ReadLocalValue(property), binding});
    target.SetValue(property, value);
}

void RestoreView(TaskbarView& view) {
    if (const auto grid = view.grid.get()) {
        uint32_t index;
        if (grid.Children().IndexOf(view.rows, index)) {
            grid.Children().RemoveAt(index);
        }
    }
    for (const auto& change : view.changes) {
        if (const auto target = change.target.get()) {
            if (change.binding) {
                target.as<Xaml::FrameworkElement>().SetBinding(change.property, change.binding);
            } else if (change.value == Xaml::DependencyProperty::UnsetValue()) {
                target.ClearValue(change.property);
            } else {
                target.SetValue(change.property, change.value);
            }
        }
    }
}

void RenderView(TaskbarView& view, const std::vector<TaskbarRow>& presentation) {
    const auto icon = view.icon.get();
    if (!icon || !view.rows) {
        return;
    }
    view.rows.Children().Clear();
    const auto control = icon.try_as<Controls::Control>();
    const auto foreground = control ? control.Foreground() : Xaml::Media::Brush{nullptr};
    Xaml::DependencyObject root = icon;
    while (const auto parent = Xaml::Media::VisualTreeHelper::GetParent(root)) {
        root = parent;
    }
    const auto clock = FindElement(root, L"TimeInnerTextBlock").try_as<Controls::TextBlock>();
    const double fontSize = clock ? clock.FontSize() : 14;
    for (const auto& item : presentation) {
        Controls::StackPanel row;
        row.Orientation(Controls::Orientation::Horizontal);
        row.HorizontalAlignment(Xaml::HorizontalAlignment::Right);
        Controls::TextBlock percent;
        percent.Text(item.text);
        percent.FontFamily(clock ? clock.FontFamily() : Xaml::Media::FontFamily{L"Segoe UI Variable Text"});
        percent.FontSize(fontSize);
        percent.LineHeight(fontSize + 4);
        percent.VerticalAlignment(Xaml::VerticalAlignment::Center);
        if (clock || foreground) {
            percent.Foreground(clock ? clock.Foreground() : foreground);
        }
        row.Children().Append(percent);

        Controls::TextBlock symbol;
        symbol.Text(item.kind == DeviceKind::Mouse ? L"\uE962" :
                    item.kind == DeviceKind::Keyboard ? L"\uE765" : L"\uE8CE");
        symbol.FontFamily(Xaml::Media::FontFamily{L"Segoe Fluent Icons"});
        symbol.FontSize(fontSize);
        symbol.Margin({5, 0, 0, 0});
        symbol.VerticalAlignment(Xaml::VerticalAlignment::Center);
        if (clock || foreground) {
            symbol.Foreground(clock ? clock.Foreground() : foreground);
        }
        row.Children().Append(symbol);
        view.rows.Children().Append(row);
    }
}

std::optional<TaskbarView> AttachView(const Xaml::FrameworkElement& icon) {
    const auto grid = FindElement(icon, L"ContentGrid").try_as<Controls::Grid>();
    if (!grid) {
        return {};
    }
    TaskbarView view;
    view.icon = winrt::make_weak(icon);
    view.grid = winrt::make_weak(grid);
    view.rows = Controls::StackPanel{};
    view.rows.Name(L"WirelessDeviceBatteryRows");
    view.rows.VerticalAlignment(Xaml::VerticalAlignment::Center);
    view.rows.HorizontalAlignment(Xaml::HorizontalAlignment::Center);
    view.rows.IsHitTestVisible(false);
    try {
        SetViewProperty(view, icon, Xaml::FrameworkElement::WidthProperty(), winrt::box_value(80.0));
        SetViewProperty(view, icon, Xaml::FrameworkElement::MinWidthProperty(), winrt::box_value(80.0));
        for (const auto* name : {L"ContainerGrid", L"ContentPresenter", L"ContentGrid"}) {
            if (const auto element = FindElement(icon, name)) {
                SetViewProperty(view, element, Xaml::FrameworkElement::WidthProperty(),
                                winrt::box_value(std::numeric_limits<double>::quiet_NaN()));
                SetViewProperty(view, element, Xaml::FrameworkElement::HorizontalAlignmentProperty(),
                                winrt::box_value(Xaml::HorizontalAlignment::Stretch));
            }
        }
        for (const auto& child : grid.Children()) {
            SetViewProperty(view, child, Xaml::UIElement::VisibilityProperty(),
                            winrt::box_value(Xaml::Visibility::Collapsed));
        }
        grid.Children().Append(view.rows);
        return view;
    } catch (...) {
        RestoreView(view);
        throw;
    }
}

void RenderUiHost(UiHost& host) {
    std::vector<TaskbarRow> presentation;
    {
        std::lock_guard lock(g_presentationMutex);
        presentation = g_presentation;
    }
    if (presentation.empty()) {
        for (auto& view : host.views) {
            RestoreView(view);
        }
        host.views.clear();
        return;
    }
    for (auto& candidate : host.candidates) {
        const auto icon = candidate.get();
        if (!icon || !icon.IsLoaded()) {
            continue;
        }
        const auto peer = Xaml::Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(icon);
        const auto name = peer ? peer.GetName() : Xaml::Automation::AutomationProperties::GetName(icon);
        if (!std::wstring_view{name}.starts_with(kTooltipMarker)) {
            continue;
        }
        const bool attached = std::any_of(host.views.begin(), host.views.end(), [&](const TaskbarView& view) {
            return view.icon.get() == icon;
        });
        if (!attached) {
            if (auto view = AttachView(icon)) {
                host.views.push_back(std::move(*view));
            }
        }
    }
    std::erase_if(host.candidates, [](const auto& candidate) { return !candidate.get(); });
    for (auto& view : host.views) {
        RenderView(view, presentation);
    }
    std::erase_if(host.views, [](const TaskbarView& view) { return !view.icon.get(); });
}

LRESULT CALLBACK UiHostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* host = reinterpret_cast<UiHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        host = new UiHost;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    try {
        if (host && (message == kRenderMessage || message == WM_TIMER) && !g_unloading.load()) {
            RenderUiHost(*host);
            return 0;
        }
        if (message == kCloseUiMessage) {
            DestroyWindow(window);
            return 0;
        }
        if (host && message == WM_DESTROY) {
            KillTimer(window, 1);
            for (auto& view : host->views) {
                RestoreView(view);
            }
        }
    } catch (...) {
        Wh_Log(L"Taskbar presentation error (0x%08X)", static_cast<unsigned>(winrt::to_hresult()));
    }
    if (message == WM_NCDESTROY) {
        delete host;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        std::lock_guard lock(g_uiHostsMutex);
        std::erase(g_uiHosts, window);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

using IconViewConstructor = void*(WINAPI*)(void*);
IconViewConstructor g_originalIconViewConstructor;

void* WINAPI IconViewConstructorHook(void* self) {
    void* result = g_originalIconViewConstructor(self);
    if (g_unloading.load()) {
        return result;
    }
    try {
        Xaml::FrameworkElement icon{nullptr};
        reinterpret_cast<::IUnknown**>(self)[1]->QueryInterface(winrt::guid_of<Xaml::FrameworkElement>(),
                                                                winrt::put_abi(icon));
        if (!icon) {
            return result;
        }
        HWND window = nullptr;
        {
            std::lock_guard lock(g_uiHostsMutex);
            for (const HWND existing : g_uiHosts) {
                if (GetWindowThreadProcessId(existing, nullptr) == GetCurrentThreadId()) {
                    window = existing;
                    break;
                }
            }
        }
        if (!window) {
            window = CreateWindowExW(0, kUiHostClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_module, nullptr);
            if (!window) {
                return result;
            }
            SetTimer(window, 1, 1000, nullptr);
            std::lock_guard lock(g_uiHostsMutex);
            g_uiHosts.push_back(window);
        }
        auto* host = reinterpret_cast<UiHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        host->candidates.push_back(winrt::make_weak(icon));
        PostMessageW(window, kRenderMessage, 0, 0);
    } catch (...) {
        Wh_Log(L"Could not track taskbar view (0x%08X)", static_cast<unsigned>(winrt::to_hresult()));
    }
    return result;
}

bool InitializeTaskbarPresentation() {
    const WNDCLASSW windowClass{0, UiHostProc, 0, 0, g_module, nullptr, nullptr, nullptr, nullptr, kUiHostClass};
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    HMODULE trayModule = GetModuleHandleW(L"SystemTray.dll");
    if (!trayModule) {
        trayModule = GetModuleHandleW(L"Taskbar.View.dll");
    }
    if (!trayModule) {
        trayModule = GetModuleHandleW(L"ExplorerExtensions.dll");
    }
    WindhawkUtils::SYMBOL_HOOK hooks[] = {{
        {LR"(public: __cdecl winrt::SystemTray::implementation::IconView::IconView(void))"},
        &g_originalIconViewConstructor, IconViewConstructorHook,
    }};
    g_taskbarHooked = trayModule && WindhawkUtils::HookSymbols(trayModule, hooks, ARRAYSIZE(hooks));
    return g_taskbarHooked;
}

void PublishTaskbarRows(const Tray& tray) {
    std::vector<TaskbarRow> rows;
    for (const auto* device : SelectedDevices(tray)) {
        if (device) {
            rows.push_back({device->battery ? std::to_wstring(*device->battery) + L"%" : L"--%", device->kind});
        }
    }
    {
        std::lock_guard lock(g_presentationMutex);
        g_presentation = std::move(rows);
    }
    std::lock_guard lock(g_uiHostsMutex);
    for (const HWND host : g_uiHosts) {
        PostMessageW(host, kRenderMessage, 0, 0);
    }
}

HICON CreateBatteryIcon(const Tray& tray) {
    constexpr int size = 32;
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = size;
    bitmapInfo.bmiHeader.biHeight = -size;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) {
        return nullptr;
    }
    auto* pixels = static_cast<uint32_t*>(bits);
    std::fill_n(pixels, size * size, 0u);
    const auto fill = [&](int left, int top, int right, int bottom, uint32_t argb) {
        for (int row = top; row < bottom; ++row) {
            std::fill(pixels + row * size + left, pixels + row * size + right, argb);
        }
    };

    // A transparent fallback icon remains available while the XAML view loads.
    fill(1, 7, 28, 25, 0xFFF5F5F5);
    fill(28, 12, 31, 20, 0xFFF5F5F5);
    fill(3, 9, 26, 23, 0);
    const auto level = tray.devices.empty() ? std::nullopt : tray.devices.front().battery;
    if (level && *level > 0) {
        const int width = std::max(1, (19 * *level + 50) / 100);
        fill(5, 11, 5 + width, 21, 0xFFF5F5F5);
    }

    const std::array<uint8_t, size * size / 8> maskBits{};
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    HICON icon = nullptr;
    if (mask) {
        ICONINFO iconInfo{TRUE, 0, 0, mask, color};
        icon = CreateIconIndirect(&iconInfo);
        DeleteObject(mask);
    }
    DeleteObject(color);
    return icon;
}

const wchar_t* EmptyMessage(const Tray& tray) {
    return tray.queryFailed ? L"Could not check Bluetooth devices" : L"No supported wireless devices connected";
}

std::wstring FitText(std::wstring value, size_t maxLength) {
    if (value.size() > maxLength) {
        value.resize(maxLength - 1);
        if (IS_HIGH_SURROGATE(value.back())) {
            value.pop_back();
        }
        value += L'\u2026';
    }
    return value;
}

std::wstring BatteryText(const Device& device) {
    return device.battery ? std::to_wstring(*device.battery) + L"%" : L"Battery unavailable";
}

std::vector<std::wstring> TooltipRows(const Tray& tray) {
    if (tray.devices.empty()) {
        return {EmptyMessage(tray)};
    }
    std::vector<std::wstring> rows;
    for (const auto& device : tray.devices) {
        rows.push_back(BatteryText(device) + L" - " + FitText(device.name, 90));
    }
    return rows;
}

std::optional<RECT> TrayIconBounds(HWND window) {
    NOTIFYICONIDENTIFIER icon{sizeof(icon), window, kTrayId, kTrayGuid};
    RECT bounds;
    if (SUCCEEDED(Shell_NotifyIconGetRect(&icon, &bounds))) {
        return bounds;
    }
    return {};
}

NOTIFYICONDATAW TrayIconData(HWND window) {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window;
    data.uID = kTrayId;
    data.uFlags = NIF_GUID;
    data.guidItem = kTrayGuid;
    return data;
}

void UpdateTrayIcon(Tray& tray) {
    auto data = TrayIconData(tray.window);
    data.uFlags |= NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uFlags |= NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = CreateBatteryIcon(tray);
    if (!data.hIcon) {
        return;
    }
    std::wstring multiline{kTooltipMarker};
    for (const auto& row : TooltipRows(tray)) {
        if (multiline.size() > std::wcslen(kTooltipMarker)) {
            multiline += L'\n';
        }
        multiline += row;
    }
    const auto tip = FitText(multiline, ARRAYSIZE(data.szTip) - 1);
    std::copy(tip.begin(), tip.end(), data.szTip);

    if (!tray.iconAdded || !Shell_NotifyIconW(NIM_MODIFY, &data)) {
        tray.iconAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        tray.version4 = false;
        if (tray.iconAdded) {
            data.uVersion = NOTIFYICON_VERSION_4;
            tray.version4 = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE;
        }
    }
    DestroyIcon(data.hIcon);
    PublishTaskbarRows(tray);
}

std::wstring MenuLabel(const Device& device) {
    std::wstring name;
    for (const wchar_t character : FitText(device.name, 70)) {
        name += character;
        if (character == L'&') {
            name += L'&';
        }
    }
    return name + L"  \u2014  " + BatteryText(device);
}

void ShowMenu(Tray& tray, POINT anchor) {
    if (tray.menuOpen || WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    if (tray.devices.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, EmptyMessage(tray));
    }
    for (const auto& device : tray.devices) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, MenuLabel(device).c_str());
    }
    if (tray.queryFailed && !tray.devices.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Some Bluetooth devices could not be checked");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kRefreshCommand, L"&Refresh now");

    // Check the stop event in the menu's modal loop so disable can join the worker.
    if (SetTimer(tray.window, kMenuTimer, 100, nullptr)) {
        tray.menuOpen = true;
        SetForegroundWindow(tray.window);
        const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                              anchor.x, anchor.y, tray.window, nullptr);
        PostMessageW(tray.window, WM_NULL, 0, 0);
        tray.menuOpen = false;
        KillTimer(tray.window, kMenuTimer);
        auto data = TrayIconData(tray.window);
        Shell_NotifyIconW(NIM_SETFOCUS, &data);
        tray.refreshRequested = tray.refreshRequested || command == kRefreshCommand;
    }
    DestroyMenu(menu);
}

LRESULT CALLBACK TrayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* tray = reinterpret_cast<Tray*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (tray && message == tray->taskbarCreated) {
        tray->iconAdded = false;
        UpdateTrayIcon(*tray);
        return 0;
    }
    if (tray && message == kTrayMessage) {
        const auto event = LOWORD(lParam);
        if (tray->version4 && (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT)) {
            ShowMenu(*tray, {GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)});
        } else if (tray->version4 && event == WM_CONTEXTMENU) {
            POINT anchor{};
            if (const auto rect = TrayIconBounds(window)) {
                ShowMenu(*tray, {rect->left, rect->top});
            } else if (GetCursorPos(&anchor)) {
                ShowMenu(*tray, anchor);
            }
        } else if (!tray->version4 && (event == WM_LBUTTONUP || event == WM_RBUTTONUP)) {
            POINT anchor{};
            if (GetCursorPos(&anchor)) {
                ShowMenu(*tray, anchor);
            }
        }
        return 0;
    }
    if (tray && message == WM_DEVICECHANGE && wParam == DBT_DEVNODES_CHANGED) {
        tray->refreshRequested = true;
    }
    if (message == WM_TIMER && wParam == kMenuTimer &&
        WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) {
        EndMenu();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void LoadSettings(Tray& tray) {
    const int seconds = Wh_GetIntSetting(L"refreshSeconds");
    tray.refreshSeconds = seconds == 0 ? 30 : std::clamp(seconds, 10, 300);
    const wchar_t* settings[] = {L"trayTopDevice", L"trayBottomDevice"};
    for (size_t i = 0; i < tray.traySelection.size(); ++i) {
        if (const wchar_t* value = Wh_GetStringSetting(settings[i])) {
            tray.traySelection[i] = value;
            Wh_FreeStringSetting(value);
        }
    }
}

void RunTray() {
    if (!g_taskbarHooked && InitializeTaskbarPresentation()) {
        Wh_ApplyHookOperations();
    }
    HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&TrayWindowProc), &module)) {
        return;
    }
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Tray tray;
    LoadSettings(tray);
    tray.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    const WNDCLASSW windowClass{0, TrayWindowProc, 0, 0, module, nullptr, nullptr, nullptr, nullptr, kWindowClass};
    if (!tray.taskbarCreated || !RegisterClassW(&windowClass)) {
        Wh_Log(L"Could not register the tray window (%lu)", GetLastError());
        return;
    }
    tray.window = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"Wireless Device Battery",
                                 WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, module, &tray);
    if (!tray.window) {
        Wh_Log(L"Could not create the tray window (%lu)", GetLastError());
        UnregisterClassW(kWindowClass, module);
        return;
    }

    MouseReceiver receiver;
    ULONGLONG nextRefresh = 0;
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        if (tray.refreshRequested) {
            tray.refreshRequested = false;
            if (!g_taskbarHooked && InitializeTaskbarPresentation()) {
                Wh_ApplyHookOperations();
                if (tray.iconAdded) {
                    auto data = TrayIconData(tray.window);
                    Shell_NotifyIconW(NIM_DELETE, &data);
                    tray.iconAdded = false;
                }
            }
            receiver.Connect();
            RefreshDevices(tray);
            if (WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT) {
                break;
            }
            UpdateReceiverDevice(tray, receiver);
            UpdateTrayIcon(tray);
            nextRefresh = GetTickCount64() + tray.refreshSeconds * 1000;
        }

        const HANDLE events[] = {g_stopEvent, g_settingsEvent, receiver.PendingEvent()};
        const DWORD eventCount = events[2] ? 3 : 2;
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now < nextRefresh ? static_cast<DWORD>(nextRefresh - now) : 0;
        const DWORD result = MsgWaitForMultipleObjectsEx(eventCount, events,
            remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) {
            break;
        }
        if (result == WAIT_OBJECT_0 + 1) {
            LoadSettings(tray);
        }
        if (result == WAIT_TIMEOUT || result == WAIT_OBJECT_0 + 1) {
            tray.refreshRequested = true;
            continue;
        }
        if (eventCount == 3 && result == WAIT_OBJECT_0 + 2 && receiver.ReadReady()) {
            UpdateReceiverDevice(tray, receiver);
            UpdateTrayIcon(tray);
        }
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    auto data = TrayIconData(tray.window);
    Shell_NotifyIconW(NIM_DELETE, &data);
    DestroyWindow(tray.window);
    UnregisterClassW(kWindowClass, module);
}

DWORD WINAPI TrayThread(void*) {
    // At sign-in the desktop shell can finish starting after the mod loads.
    for (;;) {
        if (const HWND shell = GetShellWindow()) {
            DWORD processId = 0;
            GetWindowThreadProcessId(shell, &processId);
            if (processId != GetCurrentProcessId()) {
                return 0;
            }
            break;
        }
        if (WaitForSingleObject(g_stopEvent, 500) != WAIT_TIMEOUT) {
            return 0;
        }
    }
    const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(apartment)) {
        Wh_Log(L"Could not initialize Windows Runtime (0x%08X)", static_cast<unsigned>(apartment));
        return 0;
    }
    RunTray();
    RoUninitialize();
    return 0;
}

}  // namespace

BOOL Wh_ModInit() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_settingsEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stopEvent || !g_settingsEvent) {
        if (g_stopEvent) {
            CloseHandle(g_stopEvent);
        }
        if (g_settingsEvent) {
            CloseHandle(g_settingsEvent);
        }
        return FALSE;
    }
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&UiHostProc), &g_module);
    if (!InitializeTaskbarPresentation()) {
        Wh_Log(L"Taskbar text integration will be retried when the taskbar is ready");
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    // A separate File Explorer process must not create another tray icon.
    DWORD shellProcessId = 0;
    GetWindowThreadProcessId(GetShellWindow(), &shellProcessId);
    if (shellProcessId != 0 && shellProcessId != GetCurrentProcessId()) {
        return;
    }
    g_thread = CreateThread(nullptr, 0, TrayThread, nullptr, 0, nullptr);
    if (!g_thread) {
        Wh_Log(L"Could not start the tray thread (%lu)", GetLastError());
    }
}

void Wh_ModSettingsChanged() {
    SetEvent(g_settingsEvent);
}

void Wh_ModBeforeUninit() {
    g_unloading.store(true);
    SetEvent(g_stopEvent);
}

void Wh_ModUninit() {
    g_unloading.store(true);
    SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
    }
    std::vector<HWND> hosts;
    {
        std::lock_guard lock(g_uiHostsMutex);
        hosts = g_uiHosts;
    }
    for (const HWND host : hosts) {
        SendMessageW(host, kCloseUiMessage, 0, 0);
    }
    UnregisterClassW(kUiHostClass, g_module);
    CloseHandle(g_settingsEvent);
    CloseHandle(g_stopEvent);
}
