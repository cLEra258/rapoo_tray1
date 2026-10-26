#define NOMINMAX  // 只保留这个，防止 std::max 冲突
#include <windows.h>
#include <shellapi.h>
#include <hidapi/hidapi.h>
#include <gdiplus.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <cstdio>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

// ==================== 协议常量 ====================
constexpr unsigned short RAPOO_VID         = 0x24AE;
constexpr unsigned short VENDOR_USAGE_PAGE = 0xFF00;
constexpr unsigned char  BATTERY_REPORT_ID = 0x07;
constexpr int            BATTERY_BYTE_INDEX = 7;

// ==================== 托盘常量 ====================
constexpr UINT WM_TRAYICON     = WM_APP + 1;
constexpr UINT_PTR TRAY_UID    = 1;
constexpr UINT ID_MENU_EXIT    = 1001;
constexpr UINT ID_MENU_RECONNECT = 1002;

// ==================== 全局状态 ====================
std::atomic<int>  g_battery{ -1 };
std::atomic<bool> g_running{ true };
std::atomic<bool> g_connected{ false };
std::atomic<bool> g_reconnect_requested{ false }; // 新增：安全重连标志
hid_device*       g_hid = nullptr;
HWND              g_hwnd = nullptr;
HICON             g_currentIcon = nullptr;
std::mutex        g_iconMutex;

ULONG_PTR         g_gdiplusToken = 0;

// ==================== 电池配色 ====================
static Color BatteryFillColor(int pct) {
    if (pct < 0)  return Color(255, 170, 170, 170);
    if (pct < 20) return Color(255, 239, 68, 68);   // 红
    if (pct < 50) return Color(255, 245, 158, 11);  // 黄
    return Color(255, 16, 185, 129);                // 绿
}

// ==================== GDI+ 绘制 64x64 电池图标 ====================
static HICON CreateBatteryIcon(int pct) {
    const int SZ = 64;
    Bitmap bitmap(SZ, SZ, PixelFormat32bppARGB);
    Graphics g(&bitmap);
    
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.Clear(Color(0, 0, 0, 0)); // 完全透明背景

    // 几何参数
    const float bodyL = 6.0f,  bodyT = 18.0f, bodyR = 52.0f, bodyB = 48.0f;
    const float capL  = 52.0f, capT  = 26.0f, capR  = 58.0f, capB  = 40.0f;
    
    // 1. 画外框（深灰，带圆角，包含头部的凸起）
    SolidBrush frameBrush(Color(255, 70, 70, 70));
    GraphicsPath path;
    float r = 8.0f; // 圆角半径
    
    path.AddArc(bodyL, bodyT, r, r, 180, 90);
    path.AddArc(bodyR - r, bodyT, r, r, 270, 90);
    path.AddLine(bodyR, bodyT + r, capL, bodyT + r);
    path.AddLine(capL, capT, capR, capT);
    path.AddLine(capR, capT, capR, capB);
    path.AddLine(capR, capB, capL, capB);
    path.AddLine(capL, bodyB - r, bodyR, bodyB - r);
    path.AddArc(bodyR - r, bodyB - r, r, r, 0, 90);
    path.AddArc(bodyL, bodyB - r, r, r, 90, 90);
    path.AddArc(bodyL, bodyT, r, r, 180, 90);
    path.CloseFigure();
    g.FillPath(&frameBrush, &path);

    // 2. 画内衬（白色背景）
    const float innerL = bodyL + 4, innerT = bodyT + 4;
    const float innerR = bodyR - 4, innerB = bodyB - 4;
    const float innerW = innerR - innerL;
    
    SolidBrush innerBrush(Color(255, 250, 250, 250));
    GraphicsPath innerPath;
    float ir = 4.0f;
    innerPath.AddArc(innerL, innerT, ir, ir, 180, 90);
    innerPath.AddArc(innerR - ir, innerT, ir, ir, 270, 90);
    innerPath.AddArc(innerR - ir, innerB - ir, ir, ir, 0, 90);
    innerPath.AddArc(innerL, innerB - ir, ir, ir, 90, 90);
    innerPath.CloseFigure();
    g.FillPath(&innerBrush, &innerPath);

    // 3. 画电量填充
    if (pct > 0) {
        float fillW = (innerW * pct) / 100.0f;
        if (fillW < 4.0f) fillW = 4.0f;
        if (fillW > innerW) fillW = innerW;

        SolidBrush fillBrush(BatteryFillColor(pct));
        GraphicsPath fillPath;
        fillPath.AddArc(innerL, innerT, ir, ir, 180, 90);
        fillPath.AddLine(innerL + ir, innerT, innerL + fillW, innerT);
        fillPath.AddLine(innerL + fillW, innerT, innerL + fillW, innerB);
        fillPath.AddLine(innerL + fillW, innerB, innerL + ir, innerB);
        fillPath.AddArc(innerL, innerB - ir, ir, ir, 90, 90);
        fillPath.AddArc(innerL, innerT, ir, ir, 180, 90);
        fillPath.CloseFigure();
        
        if (fillW < 8.0f) {
            g.FillRectangle(&fillBrush, innerL, innerT, fillW, innerB - innerT);
        } else {
            g.FillPath(&fillBrush, &fillPath);
        }
    }

    // 4. 画数字
    wchar_t txt[8];
    if (pct >= 0) swprintf_s(txt, L"%d", pct);
    else          wcscpy_s(txt, L"?");

    float fontSize = 28.0f;
    if (pct >= 100) fontSize = 20.0f;
    else if (pct >= 10) fontSize = 26.0f;

    FontFamily fontFamily(L"Segoe UI");
    Font font(&fontFamily, fontSize, FontStyleBold, UnitPixel);
    
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);

    RectF textRect(bodyL, bodyT, bodyR - bodyL, bodyB - bodyT);

    SolidBrush shadowBrush(Color(150, 0, 0, 0));
    RectF shadowRect = textRect;
    shadowRect.X += 1.0f;
    shadowRect.Y += 1.0f;
    g.DrawString(txt, -1, &font, shadowRect, &format, &shadowBrush);

    SolidBrush textBrush(Color(255, 255, 255, 255));
    g.DrawString(txt, -1, &font, textRect, &format, &textBrush);

    HICON hIcon = nullptr;
    bitmap.GetHICON(&hIcon);
    return hIcon;
}

// ==================== 托盘更新 ====================
static void UpdateTray(int pct) {
    if (!g_hwnd) return;

    HICON newIcon = CreateBatteryIcon(pct);
    {
        std::lock_guard<std::mutex> lk(g_iconMutex);
        if (g_currentIcon) DestroyIcon(g_currentIcon);
        g_currentIcon = newIcon;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = newIcon;

    if (pct < 0) wcscpy_s(nid.szTip, L"Rapoo 鼠标 — 等待数据…");
    else         swprintf_s(nid.szTip, L"Rapoo 鼠标 — 电量 %d%%", pct);

    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ==================== 打开 0xFF00 厂商接口 ====================
static hid_device* OpenRapooBatteryInterface() {
    hid_device_info* list = hid_enumerate(RAPOO_VID, 0);
    hid_device* dev = nullptr;
    for (auto* p = list; p; p = p->next) {
        if (p->usage_page == VENDOR_USAGE_PAGE) {
            dev = hid_open_path(p->path);
            if (dev) break;
        }
    }
    hid_free_enumeration(list);
    return dev;
}

// ==================== 后台读取线程 ====================
static void ReaderThread() {
    while (g_running.load()) {
        hid_device* dev = OpenRapooBatteryInterface();
        if (!dev) {
            g_connected = false;
            if (g_battery.load() != -1) { g_battery = -1; UpdateTray(-1); }
            for (int i = 0; i < 30 && g_running.load(); ++i) {
                if (g_reconnect_requested.load()) break;
                Sleep(100);
            }
            continue;
        }

        g_hid = dev;
        g_connected = true;

        unsigned char buf[64];
        while (g_running.load()) {
            // 安全响应主线程的“重新连接”请求
            if (g_reconnect_requested.load()) {
                g_reconnect_requested = false;
                break;
            }

            int n = hid_read_timeout(dev, buf, sizeof(buf), 500);
            if (n < 0) break;
            if (n == 0) continue;
            
            if (n > 1 + BATTERY_BYTE_INDEX && buf[0] == BATTERY_REPORT_ID) {
                int pct = buf[1 + BATTERY_BYTE_INDEX];
                if (pct >= 0 && pct <= 100 && pct != g_battery.load()) {
                    g_battery = pct;
                    UpdateTray(pct);
                }
            }
        }

        // 句柄由读取线程自己安全关闭
        hid_close(dev);
        g_hid = nullptr;
        g_connected = false;
        if (g_battery.load() != -1) { g_battery = -1; UpdateTray(-1); }

        if (!g_running.load()) break;
        for (int i = 0; i < 10 && g_running.load(); ++i) Sleep(100);
    }
}

// ==================== 窗口过程 ====================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();

            wchar_t info[64];
            if (g_battery.load() >= 0) swprintf_s(info, L"电量：%d%%", g_battery.load());
            else                       wcscpy_s(info, L"电量：未知");
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, info);

            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ID_MENU_RECONNECT, L"重新连接");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"退出");

            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            PostMessage(hwnd, WM_NULL, 0, 0);
            DestroyMenu(menu);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_MENU_EXIT) {
            g_running = false;
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        } else if (LOWORD(wp) == ID_MENU_RECONNECT) {
            // 只发信号，交给读取线程自己断开重连，避免句柄冲突崩溃
            g_reconnect_requested = true;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ==================== 入口 ====================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);

    if (hid_init() != 0) {
        MessageBoxW(nullptr, L"hidapi 初始化失败", L"错误", MB_ICONERROR);
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"RapooTrayWnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"RapooTrayWnd", L"RapooTray", 0,
                             0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_hwnd) { 
        hid_exit(); 
        GdiplusShutdown(g_gdiplusToken);
        return 1; 
    }

    HICON initIcon = CreateBatteryIcon(-1);
    g_currentIcon = initIcon;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = initIcon;
    wcscpy_s(nid.szTip, L"Rapoo 鼠标 — 等待数据…");
    Shell_NotifyIconW(NIM_ADD, &nid);

    std::thread reader(ReaderThread);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = false;
    if (reader.joinable()) reader.join();
    // 退出时不再强制 hid_close，后台线程已经自己收尾了

    Shell_NotifyIconW(NIM_DELETE, &nid);
    {
        std::lock_guard<std::mutex> lk(g_iconMutex);
        if (g_currentIcon) DestroyIcon(g_currentIcon);
    }

    hid_exit();
    GdiplusShutdown(g_gdiplusToken);
    return 0;
}
