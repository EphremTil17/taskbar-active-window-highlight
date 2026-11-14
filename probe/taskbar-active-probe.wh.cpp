// ==WindhawkMod==
// @id              taskbar-active-probe
// @name            Taskbar Active Probe (temporary diagnostic)
// @description     Logs taskbar active-window data to %TEMP%\taskbar-active-probe.log. Changes nothing on screen.
// @version         0.1
// @author          local
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// Temporary probe used to verify assumptions before writing the real mod:
//  1. TaskListButton::UpdateVisualStates `this` -> FrameworkElement mapping.
//  2. ITaskListButton::get_IsActive reflects the active taskbar button.
//  3. Thumbnail card -> ITaskItemThumbnailViewModel -> ITaskItemThumbnail
//     -> ParentItem (ITaskItem) -> WindowId gives the card's HWND.
//  4. TaskbarController::OnAppsListActiveItemChanged delivers the apps list,
//     and its ActiveItem's WindowId is the active window.
//  5. The XAML tree of a taskbar button and of a thumbnail card.

#include <windhawk_utils.h>

#include <inspectable.h>
#include <winstring.h>

#undef GetCurrentTime
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

using namespace winrt::Windows::UI::Xaml;

// ---------------------------------------------------------------------------
// Logging to a file (and Wh_Log)
// ---------------------------------------------------------------------------

std::mutex g_logMutex;
std::wstring g_logPath;

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[4096];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    Wh_Log(L"%s", buf);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logPath.empty()) {
        return;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(f, L"%02d:%02d:%02d.%03d [t%05lu] %s\n", st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds, GetCurrentThreadId(), buf);
        fclose(f);
    }
}

// ---------------------------------------------------------------------------
// ABI declarations (WindowsUdk.UI.Shell, from windowsudk.winmd)
// ---------------------------------------------------------------------------

constexpr GUID IID_UdkTaskItem = {0xB081D9D6, 0x9B45, 0x5363, {0x8A, 0x4D, 0x85, 0x4A, 0xDD, 0x6A, 0xBE, 0x7E}};
constexpr GUID IID_UdkTaskItemThumbnail = {0xD7DC08AE, 0x776E, 0x5871, {0xBC, 0xEE, 0x25, 0x0D, 0xBC, 0x94, 0xD0, 0x37}};
constexpr GUID IID_UdkTaskbarAppsList = {0xABD50A67, 0x1E54, 0x536C, {0x82, 0x27, 0xB4, 0xF1, 0xA1, 0x25, 0xA7, 0xFE}};
// Taskbar.View.dll internal interfaces (guid_v constants from the public PDB).
constexpr GUID IID_TaskListButton = {0x6D8E265B, 0x363C, 0x5067, {0x93, 0x85, 0x11, 0xD9, 0x4D, 0xFF, 0xBE, 0x04}};
constexpr GUID IID_TaskItemThumbnailViewModel = {0xA5D30F97, 0x5BC4, 0x58AE, {0xB4, 0x48, 0xB1, 0x54, 0x8A, 0x1A, 0x92, 0x9D}};

struct IUdkTaskItem : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_AppId(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_UniqueId(UINT64* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_WindowId(UINT64* value) = 0;
};

struct IUdkTaskItemThumbnail : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AccessibleName(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ParentItem(IUdkTaskItem** value) = 0;
};

struct IUdkTaskbarAppsList : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Groups(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ActiveGroup(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ActiveItem(IUdkTaskItem** value) = 0;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::wstring ClassNameOf(IUnknown* unk) {
    if (!unk) {
        return L"(null)";
    }
    winrt::com_ptr<IInspectable> insp;
    if (FAILED(unk->QueryInterface(IID_PPV_ARGS(insp.put()))) || !insp) {
        return L"(not IInspectable)";
    }
    HSTRING name = nullptr;
    if (FAILED(insp->GetRuntimeClassName(&name))) {
        return L"(GetRuntimeClassName failed)";
    }
    std::wstring result = WindowsGetStringRawBuffer(name, nullptr);
    WindowsDeleteString(name);
    return result;
}

std::wstring WindowTitle(HWND hwnd) {
    wchar_t buf[256] = {};
    InternalGetWindowText(hwnd, buf, ARRAYSIZE(buf));
    return buf;
}

std::wstring WindowClass(HWND hwnd) {
    wchar_t buf[128] = {};
    GetClassNameW(hwnd, buf, ARRAYSIZE(buf));
    return buf;
}

HWND WindowFromTaskItem(IUdkTaskItem* item, HRESULT* hrOut = nullptr) {
    UINT64 id = 0;
    HRESULT hr = item ? item->get_WindowId(&id) : E_POINTER;
    if (hrOut) {
        *hrOut = hr;
    }
    return SUCCEEDED(hr) ? reinterpret_cast<HWND>(id) : nullptr;
}

std::wstring DescribeHwnd(HWND hwnd) {
    if (!hwnd) {
        return L"(null)";
    }
    wchar_t buf[512];
    _snwprintf_s(buf, _TRUNCATE, L"%p IsWindow=%d class=%s title=\"%s\"", hwnd,
                 IsWindow(hwnd) ? 1 : 0, WindowClass(hwnd).c_str(),
                 WindowTitle(hwnd).c_str());
    return buf;
}

std::wstring BrushText(Media::Brush const& brush) {
    if (!brush) {
        return L"null";
    }
    if (auto solid = brush.try_as<Media::SolidColorBrush>()) {
        auto c = solid.Color();
        wchar_t buf[64];
        _snwprintf_s(buf, _TRUNCATE, L"#%02X%02X%02X%02X op=%.2f", c.A, c.R, c.G,
                     c.B, solid.Opacity());
        return buf;
    }
    return std::wstring(winrt::get_class_name(brush).c_str());
}

int g_dumpBudget = 0;

void DumpTree(DependencyObject const& obj, int depth) {
    if (!obj || depth > 14 || g_dumpBudget-- <= 0) {
        return;
    }
    std::wstring line(depth * 2, L' ');
    line += winrt::get_class_name(obj).c_str();
    if (auto fe = obj.try_as<FrameworkElement>()) {
        auto name = fe.Name();
        if (!name.empty()) {
            line += L" #";
            line += name.c_str();
        }
        wchar_t buf[256];
        auto m = fe.Margin();
        _snwprintf_s(buf, _TRUNCATE,
                     L" size=%.1fx%.1f margin=%.0f,%.0f,%.0f,%.0f vis=%d op=%.2f "
                     L"halign=%d valign=%d",
                     fe.ActualWidth(), fe.ActualHeight(), m.Left, m.Top, m.Right,
                     m.Bottom, (int)fe.Visibility(), fe.Opacity(),
                     (int)fe.HorizontalAlignment(), (int)fe.VerticalAlignment());
        line += buf;
    }
    if (auto border = obj.try_as<Controls::Border>()) {
        auto cr = border.CornerRadius();
        auto bt = border.BorderThickness();
        wchar_t buf[256];
        _snwprintf_s(buf, _TRUNCATE, L" corner=%.0f,%.0f,%.0f,%.0f bt=%.0f,%.0f,%.0f,%.0f",
                     cr.TopLeft, cr.TopRight, cr.BottomRight, cr.BottomLeft,
                     bt.Left, bt.Top, bt.Right, bt.Bottom);
        line += buf;
        line += L" bg=" + BrushText(border.Background());
        line += L" bb=" + BrushText(border.BorderBrush());
    } else if (auto panel = obj.try_as<Controls::Panel>()) {
        line += L" bg=" + BrushText(panel.Background());
    }
    if (auto ui = obj.try_as<UIElement>()) {
        // Visual state groups live on the template root.
        if (auto fe = obj.try_as<FrameworkElement>()) {
            auto groups = VisualStateManager::GetVisualStateGroups(fe);
            for (auto const& g : groups) {
                auto cur = g.CurrentState();
                line += L" [VSG ";
                line += g.Name().c_str();
                line += L"=";
                line += cur ? cur.Name().c_str() : L"(none)";
                line += L"]";
            }
        }
    }
    Log(L"%s", line.c_str());
    int count = Media::VisualTreeHelper::GetChildrenCount(obj);
    for (int i = 0; i < count; ++i) {
        DumpTree(Media::VisualTreeHelper::GetChild(obj, i), depth + 1);
    }
}

// ---------------------------------------------------------------------------
// Resolved private functions
// ---------------------------------------------------------------------------

using TaskListButton_get_IsActive_t = int32_t(WINAPI*)(void* pThis, bool* value);
TaskListButton_get_IsActive_t TaskListButton_get_IsActive;

using ThumbVM_get_TaskItemThumbnail_t = int32_t(WINAPI*)(void* pThis, void** value);
ThumbVM_get_TaskItemThumbnail_t ThumbVM_get_TaskItemThumbnail;

// ---------------------------------------------------------------------------
// Active item tracking
// ---------------------------------------------------------------------------

std::atomic<HWND> g_activeHwnd{nullptr};

using OnAppsListActiveItemChanged_t = void(WINAPI*)(void* pThis, void* appsList, void* args);
OnAppsListActiveItemChanged_t OnAppsListActiveItemChanged_Original;
void WINAPI OnAppsListActiveItemChanged_Hook(void* pThis, void* appsList, void* args) {
    OnAppsListActiveItemChanged_Original(pThis, appsList, args);
    try {
        // `TaskbarAppsList const&` is a reference to a projected object, which
        // holds exactly one ABI pointer: the default interface.
        auto* abi = appsList ? *reinterpret_cast<IUnknown**>(appsList) : nullptr;
        Log(L"ActiveItemChanged: controller=%p appsList abi=%p class=%s", pThis, abi,
            ClassNameOf(abi).c_str());
        if (!abi) {
            return;
        }
        winrt::com_ptr<IUdkTaskbarAppsList> list;
        HRESULT hr = abi->QueryInterface(IID_UdkTaskbarAppsList, list.put_void());
        if (FAILED(hr) || !list) {
            Log(L"  QI ITaskbarAppsList failed %08X", hr);
            return;
        }
        winrt::com_ptr<IUdkTaskItem> item;
        hr = list->get_ActiveItem(item.put());
        HRESULT hrWin = S_OK;
        HWND hwnd = WindowFromTaskItem(item.get(), &hrWin);
        g_activeHwnd = hwnd;
        HWND fg = GetForegroundWindow();
        Log(L"  get_ActiveItem hr=%08X item=%p class=%s winId hr=%08X -> %s",
            hr, item.get(), ClassNameOf(item.get()).c_str(), hrWin,
            DescribeHwnd(hwnd).c_str());
        Log(L"  foreground now: %s (same=%d)", DescribeHwnd(fg).c_str(),
            fg == hwnd ? 1 : 0);
    } catch (...) {
        Log(L"  exception %08X", winrt::to_hresult());
    }
}

// ---------------------------------------------------------------------------
// Taskbar buttons
// ---------------------------------------------------------------------------

std::mutex g_buttonsMutex;
std::unordered_map<void*, int> g_buttonActive;  // identity -> last IsActive
std::atomic<int> g_uvsCalls{0};
std::atomic<int> g_putIsActiveCalls{0};
bool g_dumpedActiveButton = false;
bool g_dumpedInactiveButton = false;

int ReadIsActive(FrameworkElement const& element) {
    if (!TaskListButton_get_IsActive) {
        return -2;
    }
    winrt::com_ptr<IUnknown> iface;
    auto* unk = reinterpret_cast<IUnknown*>(winrt::get_abi(element));
    if (FAILED(unk->QueryInterface(IID_TaskListButton, iface.put_void())) || !iface) {
        return -3;
    }
    bool active = false;
    int32_t hr = TaskListButton_get_IsActive(iface.get(), &active);
    return hr < 0 ? -4 : (active ? 1 : 0);
}

void InspectButton(FrameworkElement const& element, PCWSTR source) {
    const int active = ReadIsActive(element);
    void* id = winrt::get_abi(element.as<winrt::Windows::Foundation::IUnknown>());
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_buttonsMutex);
        auto it = g_buttonActive.find(id);
        if (it == g_buttonActive.end() || it->second != active) {
            g_buttonActive[id] = active;
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    auto label = Automation::AutomationProperties::GetName(element);
    Log(L"%s: button %p \"%s\" IsActive=%d (uvs calls so far %d, put_IsActive %d)",
        source, id, label.c_str(), active, g_uvsCalls.load(),
        g_putIsActiveCalls.load());
    if (active == 1 && !g_dumpedActiveButton) {
        g_dumpedActiveButton = true;
        Log(L"---- ACTIVE button tree ----");
        g_dumpBudget = 200;
        DumpTree(element, 0);
    } else if (active == 0 && !g_dumpedInactiveButton) {
        g_dumpedInactiveButton = true;
        Log(L"---- INACTIVE button tree ----");
        g_dumpBudget = 200;
        DumpTree(element, 0);
    }
}

using TaskListButton_UpdateVisualStates_t = void(WINAPI*)(void* pThis);
TaskListButton_UpdateVisualStates_t TaskListButton_UpdateVisualStates_Original;
void WINAPI TaskListButton_UpdateVisualStates_Hook(void* pThis) {
    TaskListButton_UpdateVisualStates_Original(pThis);
    ++g_uvsCalls;
    try {
        FrameworkElement element = nullptr;
        // Convention used by Windhawk taskbar mods: an interface vtable of the
        // implementation object at pointer slot 3.
        ((IUnknown*)pThis + 3)->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                               winrt::put_abi(element));
        if (!element) {
            Log(L"UVS: QI FrameworkElement failed");
            return;
        }
        auto cls = winrt::get_class_name(element);
        if (cls != L"Taskbar.TaskListButton") {
            static int warned = 0;
            if (warned++ < 5) {
                Log(L"UVS: unexpected class %s", cls.c_str());
            }
            return;
        }
        InspectButton(element, L"UVS");
    } catch (...) {
        Log(L"UVS exception %08X", winrt::to_hresult());
    }
}

using TaskListButton_put_IsActive_t = int32_t(WINAPI*)(void* pThis, bool value);
TaskListButton_put_IsActive_t TaskListButton_put_IsActive_Original;
int32_t WINAPI TaskListButton_put_IsActive_Hook(void* pThis, bool value) {
    int32_t hr = TaskListButton_put_IsActive_Original(pThis, value);
    ++g_putIsActiveCalls;
    try {
        FrameworkElement element = nullptr;
        ((IUnknown*)pThis)->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                           winrt::put_abi(element));
        auto label = element ? Automation::AutomationProperties::GetName(element)
                             : winrt::hstring(L"?");
        Log(L"put_IsActive(%d) on \"%s\" class=%s", value ? 1 : 0, label.c_str(),
            element ? winrt::get_class_name(element).c_str() : L"(no element)");
    } catch (...) {
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Thumbnail cards
// ---------------------------------------------------------------------------

std::atomic<int> g_thumbUvsCalls{0};
bool g_dumpedThumb = false;

HWND ThumbnailWindow(FrameworkElement const& card, bool verbose) {
    auto dc = card.DataContext();
    auto* dcAbi = reinterpret_cast<IUnknown*>(winrt::get_abi(dc));
    if (verbose) {
        Log(L"  DataContext class=%s", ClassNameOf(dcAbi).c_str());
    }
    if (!dcAbi) {
        return nullptr;
    }
    winrt::com_ptr<IUdkTaskItemThumbnail> thumb;
    // Direct: is the DataContext itself the WindowsUdk thumbnail?
    if (SUCCEEDED(dcAbi->QueryInterface(IID_UdkTaskItemThumbnail, thumb.put_void())) && thumb) {
        if (verbose) {
            Log(L"  DataContext is ITaskItemThumbnail directly");
        }
    } else {
        winrt::com_ptr<IUnknown> vm;
        HRESULT hr = dcAbi->QueryInterface(IID_TaskItemThumbnailViewModel, vm.put_void());
        if (verbose) {
            Log(L"  QI ITaskItemThumbnailViewModel hr=%08X", hr);
        }
        if (FAILED(hr) || !vm || !ThumbVM_get_TaskItemThumbnail) {
            return nullptr;
        }
        void* thumbAbi = nullptr;
        int32_t r = ThumbVM_get_TaskItemThumbnail(vm.get(), &thumbAbi);
        winrt::com_ptr<IUnknown> thumbUnk;
        thumbUnk.attach(reinterpret_cast<IUnknown*>(thumbAbi));
        if (verbose) {
            Log(L"  get_TaskItemThumbnail hr=%08X -> %p class=%s", r, thumbAbi,
                ClassNameOf(thumbUnk.get()).c_str());
        }
        if (r < 0 || !thumbUnk) {
            return nullptr;
        }
        hr = thumbUnk->QueryInterface(IID_UdkTaskItemThumbnail, thumb.put_void());
        if (FAILED(hr) || !thumb) {
            if (verbose) {
                Log(L"  QI ITaskItemThumbnail failed %08X", hr);
            }
            return nullptr;
        }
    }
    winrt::com_ptr<IUdkTaskItem> parent;
    HRESULT hr = thumb->get_ParentItem(parent.put());
    HRESULT hrWin = S_OK;
    HWND hwnd = WindowFromTaskItem(parent.get(), &hrWin);
    if (verbose) {
        Log(L"  ParentItem hr=%08X class=%s WindowId hr=%08X -> %s", hr,
            ClassNameOf(parent.get()).c_str(), hrWin, DescribeHwnd(hwnd).c_str());
    }
    return hwnd;
}

using ThumbView_OnApplyTemplate_t = int32_t(WINAPI*)(void* pThis);
ThumbView_OnApplyTemplate_t ThumbView_OnApplyTemplate_Original;
int32_t WINAPI ThumbView_OnApplyTemplate_Hook(void* pThis) {
    int32_t ret = ThumbView_OnApplyTemplate_Original(pThis);
    try {
        FrameworkElement card = nullptr;
        ((IUnknown*)pThis)->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                           winrt::put_abi(card));
        if (!card) {
            Log(L"Thumb OnApplyTemplate: no FrameworkElement");
            return ret;
        }
        Log(L"Thumb OnApplyTemplate: card %p class=%s active=%p", winrt::get_abi(card),
            winrt::get_class_name(card).c_str(), g_activeHwnd.load());
        HWND hwnd = ThumbnailWindow(card, true);
        Log(L"  => card window %p, is active=%d", hwnd,
            hwnd && hwnd == g_activeHwnd.load() ? 1 : 0);
    } catch (...) {
        Log(L"Thumb OnApplyTemplate exception %08X", winrt::to_hresult());
    }
    return ret;
}

// Hovering a card: layout is complete, DataContext is final. No event
// handlers or deferred callbacks are registered, so unloading is always safe.
using ThumbView_OnPointerEntered_t = int32_t(WINAPI*)(void* pThis, void* args);
ThumbView_OnPointerEntered_t ThumbView_OnPointerEntered_Original;
int32_t WINAPI ThumbView_OnPointerEntered_Hook(void* pThis, void* args) {
    int32_t ret = ThumbView_OnPointerEntered_Original(pThis, args);
    try {
        FrameworkElement card = nullptr;
        ((IUnknown*)pThis)->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                           winrt::put_abi(card));
        if (!card) {
            return ret;
        }
        HWND hwnd = ThumbnailWindow(card, false);
        Log(L"Thumb hover: card %p window %s, is active=%d (active=%p fg=%p)",
            winrt::get_abi(card), DescribeHwnd(hwnd).c_str(),
            hwnd && hwnd == g_activeHwnd.load() ? 1 : 0, g_activeHwnd.load(),
            GetForegroundWindow());
        if (!g_dumpedThumb) {
            g_dumpedThumb = true;
            Log(L"---- THUMBNAIL card tree ----");
            g_dumpBudget = 200;
            DumpTree(card, 0);
            DependencyObject p = Media::VisualTreeHelper::GetParent(card);
            for (int i = 0; p && i < 10; ++i) {
                auto fe = p.try_as<FrameworkElement>();
                Log(L"  parent[%d] %s #%s", i, winrt::get_class_name(p).c_str(),
                    fe ? fe.Name().c_str() : L"");
                p = Media::VisualTreeHelper::GetParent(p);
            }
        }
    } catch (...) {
        Log(L"Thumb hover exception %08X", winrt::to_hresult());
    }
    return ret;
}

using ThumbView_UpdateVisualStates_t = void(WINAPI*)(void* pThis);
ThumbView_UpdateVisualStates_t ThumbView_UpdateVisualStates_Original;
void WINAPI ThumbView_UpdateVisualStates_Hook(void* pThis) {
    ThumbView_UpdateVisualStates_Original(pThis);
    int n = ++g_thumbUvsCalls;
    if (n <= 20 || n % 50 == 0) {
        Log(L"Thumb UpdateVisualStates call #%d", n);
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

BOOL Wh_ModInit() {
    wchar_t temp[MAX_PATH];
    if (GetTempPathW(ARRAYSIZE(temp), temp)) {
        g_logPath = std::wstring(temp) + L"taskbar-active-probe.log";
    }
    Log(L"==== probe init ====");

    HMODULE view = GetModuleHandleW(L"Taskbar.View.dll");
    if (!view) {
        view = GetModuleHandleW(L"ExplorerExtensions.dll");
    }
    if (!view) {
        Log(L"Taskbar.View.dll not loaded; restart Explorer after enabling.");
        return FALSE;
    }

    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateVisualStates(void))"},
         &TaskListButton_UpdateVisualStates_Original,
         TaskListButton_UpdateVisualStates_Hook},
        {{LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskListButton,struct winrt::Taskbar::ITaskListButton>::get_IsActive(bool *))"},
         &TaskListButton_get_IsActive},
        {{LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskListButton,struct winrt::Taskbar::ITaskListButton>::put_IsActive(bool))"},
         &TaskListButton_put_IsActive_Original,
         TaskListButton_put_IsActive_Hook,
         true},
        {{LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskItemThumbnailView,struct winrt::Windows::UI::Xaml::IFrameworkElementOverrides>::OnApplyTemplate(void))"},
         &ThumbView_OnApplyTemplate_Original,
         ThumbView_OnApplyTemplate_Hook,
         true},
        {{LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskItemThumbnailView,struct winrt::Windows::UI::Xaml::Controls::IControlOverrides>::OnPointerEntered(void *))"},
         &ThumbView_OnPointerEntered_Original,
         ThumbView_OnPointerEntered_Hook,
         true},
        {{LR"(private: void __cdecl winrt::Taskbar::implementation::TaskItemThumbnailView::UpdateVisualStates(void))"},
         &ThumbView_UpdateVisualStates_Original,
         ThumbView_UpdateVisualStates_Hook,
         true},
        {{LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskItemThumbnailViewModel,struct winrt::Taskbar::ITaskItemThumbnailViewModel>::get_TaskItemThumbnail(void * *))"},
         &ThumbVM_get_TaskItemThumbnail,
         nullptr,
         true},
        {{LR"(private: void __cdecl winrt::Taskbar::implementation::TaskbarController::OnAppsListActiveItemChanged(struct winrt::WindowsUdk::UI::Shell::TaskbarAppsList const &,struct winrt::Windows::Foundation::IInspectable const &))"},
         &OnAppsListActiveItemChanged_Original,
         OnAppsListActiveItemChanged_Hook,
         true},
    };

    if (!WindhawkUtils::HookSymbols(view, hooks, ARRAYSIZE(hooks))) {
        Log(L"HookSymbols failed");
        return FALSE;
    }
    Log(L"Resolved: UVS=%d get_IsActive=%d put_IsActive=%d thumbApplyTemplate=%d "
        L"thumbUVS=%d getTaskItemThumbnail=%d activeItemChanged=%d thumbHover=%d",
        TaskListButton_UpdateVisualStates_Original != nullptr,
        TaskListButton_get_IsActive != nullptr,
        TaskListButton_put_IsActive_Original != nullptr,
        ThumbView_OnApplyTemplate_Original != nullptr,
        ThumbView_UpdateVisualStates_Original != nullptr,
        ThumbVM_get_TaskItemThumbnail != nullptr,
        OnAppsListActiveItemChanged_Original != nullptr,
        ThumbView_OnPointerEntered_Original != nullptr);
    return TRUE;
}

void Wh_ModUninit() {
    Log(L"==== probe uninit ====");
}
