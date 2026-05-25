#include "settings_dialog.h"
#include "config.h"
#include "plugin_locale.h"
#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

// ── GDI+-Hilfsfunktion: PNG als HICON laden ──────────────────────────────────
static HICON loadIconFromPng(const std::wstring& pngPath, int size) {
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);

    HICON hIcon = nullptr;
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(pngPath.c_str());
    if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
        Gdiplus::Bitmap* scaled = new Gdiplus::Bitmap(size, size, PixelFormat32bppARGB);
        Gdiplus::Graphics g(scaled);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(bmp, 0, 0, size, size);
        scaled->GetHICON(&hIcon);
        delete scaled;
    }
    delete bmp;
    Gdiplus::GdiplusShutdown(token);
    return hIcon;
}

// ── Control-IDs ───────────────────────────────────────────────────────────────
#define IDC_SL_REMOTE   101
#define IDC_SL_LOCAL    102
#define IDC_VAL_REMOTE  103
#define IDC_VAL_LOCAL   104
#define IDC_LBL_VERSION 105
#define IDC_CHK_LOGGING 106
#ifndef IDC_STATIC
#define IDC_STATIC      (-1)
#endif

// Dialog-Klassen-Name
static const wchar_t* WNDCLASS_SETTINGS = L"TS3SB_SettingsDlg";

// ── Hilfsfunktion: Prozent-Label aktualisieren ────────────────────────────────
static void refreshLabel(HWND hDlg, int sliderId, int labelId) {
    int val = (int)SendDlgItemMessageW(hDlg, sliderId, TBM_GETPOS, 0, 0);
    std::wstring txt = std::to_wstring(val) + L" %";
    SetDlgItemTextW(hDlg, labelId, txt.c_str());
}

// Standardwert für Doppelklick-Reset
static constexpr int DEFAULT_SLIDER_VALUE = 60;

// ── TrackBar-Subklasse: Doppelklick → Standardwert setzen ────────────────────
static LRESULT CALLBACK SliderSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    if (msg == WM_LBUTTONDBLCLK) {
        SendMessageW(hwnd, TBM_SETPOS, TRUE, DEFAULT_SLIDER_VALUE);
        // WM_HSCROLL mit TB_ENDTRACK an Parent → Label-Update + sofort speichern
        SendMessageW(GetParent(hwnd), WM_HSCROLL,
                     MAKEWPARAM(TB_ENDTRACK, 0), (LPARAM)hwnd);
        return 0;
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, SliderSubclassProc, uIdSubclass);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// ── Window-Prozedur ───────────────────────────────────────────────────────────
struct DlgState { PluginConfig cfg; bool accepted = false; };

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DlgState* st = reinterpret_cast<DlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        st = reinterpret_cast<DlgState*>(cs->lpCreateParams);

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        auto addCtrl = [&](const wchar_t* cls, const wchar_t* txt, DWORD style,
                           int x, int y, int w, int h, int id) -> HWND {
            HWND hc = CreateWindowW(cls, txt, WS_CHILD | WS_VISIBLE | style,
                                    x, y, w, h, hwnd, (HMENU)(UINT_PTR)id, nullptr, nullptr);
            SendMessageW(hc, WM_SETFONT, (WPARAM)hFont, TRUE);
            return hc;
        };

        // Zeile 1: Remote
        addCtrl(L"STATIC", Locale::instance().tw("lbl_remote").c_str(), SS_LEFT, 10, 14, 130, 16, IDC_STATIC);
        HWND hRem = addCtrl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | TBS_NOTIFYBEFOREMOVE,
                            10, 32, 200, 24, IDC_SL_REMOTE);
        addCtrl(L"STATIC", L"", SS_LEFT, 216, 37, 36, 16, IDC_VAL_REMOTE);

        SendMessageW(hRem, TBM_SETRANGE,  TRUE, MAKELPARAM(0, 100));
        SendMessageW(hRem, TBM_SETTICFREQ, 10, 0);
        SendMessageW(hRem, TBM_SETPOS,    TRUE, (LPARAM)(int)(st->cfg.baseVolumeRemote * 100.0f + 0.5f));
        SetWindowSubclass(hRem, SliderSubclassProc, IDC_SL_REMOTE, 0);

        // Zeile 2: Lokal
        addCtrl(L"STATIC", Locale::instance().tw("lbl_local").c_str(), SS_LEFT, 10, 62, 130, 16, IDC_STATIC);
        HWND hLoc = addCtrl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | TBS_NOTIFYBEFOREMOVE,
                            10, 80, 200, 24, IDC_SL_LOCAL);
        addCtrl(L"STATIC", L"", SS_LEFT, 216, 85, 36, 16, IDC_VAL_LOCAL);

        SendMessageW(hLoc, TBM_SETRANGE,  TRUE, MAKELPARAM(0, 100));
        SendMessageW(hLoc, TBM_SETTICFREQ, 10, 0);
        SendMessageW(hLoc, TBM_SETPOS,    TRUE, (LPARAM)(int)(st->cfg.baseVolumeLocal * 100.0f + 0.5f));
        SetWindowSubclass(hLoc, SliderSubclassProc, IDC_SL_LOCAL, 0);

        // Hinweis
        addCtrl(L"STATIC",
            Locale::instance().tw("lbl_hint").c_str(),
            SS_LEFT, 10, 112, 250, 28, IDC_STATIC);

        // Logging-Checkbox
        HWND hChk = addCtrl(L"BUTTON",
            Locale::instance().tw("chk_logging").c_str(),
            BS_AUTOCHECKBOX, 10, 144, 220, 18, IDC_CHK_LOGGING);
        if (st->cfg.enableLogging)
            SendMessageW(hChk, BM_SETCHECK, BST_CHECKED, 0);

        // Versionsanzeige unten links
        {
            std::wstring verTxt = L"v" + std::wstring(
                PLUGIN_VERSION,
                PLUGIN_VERSION + strlen(PLUGIN_VERSION));
            addCtrl(L"STATIC", verTxt.c_str(), SS_LEFT, 10, 175, 110, 14, IDC_LBL_VERSION);
        }

        // Buttons
        addCtrl(L"BUTTON", Locale::instance().tw("btn_ok").c_str(),     BS_DEFPUSHBUTTON, 130, 168, 60, 24, IDOK);
        addCtrl(L"BUTTON", Locale::instance().tw("btn_cancel").c_str(), BS_PUSHBUTTON,    198, 168, 60, 24, IDCANCEL);

        // Fenster-Icon aus menu_icon.png setzen
            {
                wchar_t dllPath[MAX_PATH] = {};
                HMODULE hMod = nullptr;
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCWSTR)&loadIconFromPng, &hMod);
                GetModuleFileNameW(hMod, dllPath, MAX_PATH);
                std::wstring iconPath(dllPath);
                auto slash = iconPath.find_last_of(L"\\/");
                if (slash != std::wstring::npos)
                    iconPath = iconPath.substr(0, slash + 1);
                iconPath += L"ts3_soundboard\\menu_icon.png";
                HICON hBig   = loadIconFromPng(iconPath, 32);
                HICON hSmall = loadIconFromPng(iconPath, 16);
                if (hBig)   SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hBig);
                if (hSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
            }

            // Labels initial befüllen
            auto fillLabel = [&](int slId, int lblId) {
            int v = (int)SendDlgItemMessageW(hwnd, slId, TBM_GETPOS, 0, 0);
            std::wstring t = std::to_wstring(v) + L" %";
            SetDlgItemTextW(hwnd, lblId, t.c_str());
        };
        fillLabel(IDC_SL_REMOTE, IDC_VAL_REMOTE);
        fillLabel(IDC_SL_LOCAL,  IDC_VAL_LOCAL);
        return 0;
    }

    case WM_HSCROLL: {
        if ((HWND)lParam == GetDlgItem(hwnd, IDC_SL_REMOTE))
            refreshLabel(hwnd, IDC_SL_REMOTE, IDC_VAL_REMOTE);
        else if ((HWND)lParam == GetDlgItem(hwnd, IDC_SL_LOCAL))
            refreshLabel(hwnd, IDC_SL_LOCAL, IDC_VAL_LOCAL);

        // Bei Loslassen des Sliders sofort in die INI-Datei schreiben
        if (LOWORD(wParam) == TB_ENDTRACK && st) {
            st->cfg.baseVolumeRemote =
                (int)SendDlgItemMessageW(hwnd, IDC_SL_REMOTE, TBM_GETPOS, 0, 0) / 100.0f;
            st->cfg.baseVolumeLocal =
                (int)SendDlgItemMessageW(hwnd, IDC_SL_LOCAL,  TBM_GETPOS, 0, 0) / 100.0f;
            st->cfg.enableLogging =
                (SendDlgItemMessageW(hwnd, IDC_CHK_LOGGING, BM_GETCHECK, 0, 0) == BST_CHECKED);
            saveConfig(st->cfg);
        }
        return 0;
    }

    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wParam) == IDOK) {
            st->cfg.baseVolumeRemote =
                (int)SendDlgItemMessageW(hwnd, IDC_SL_REMOTE, TBM_GETPOS, 0, 0) / 100.0f;
            st->cfg.baseVolumeLocal =
                (int)SendDlgItemMessageW(hwnd, IDC_SL_LOCAL, TBM_GETPOS, 0, 0) / 100.0f;
            st->cfg.enableLogging =
                (SendDlgItemMessageW(hwnd, IDC_CHK_LOGGING, BM_GETCHECK, 0, 0) == BST_CHECKED);
            st->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── Öffentliche Funktion ──────────────────────────────────────────────────────

void showSettingsDialog(HWND hwndParent) {
    Locale::instance().init(reinterpret_cast<const void*>(&showSettingsDialog));

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // Fensterklasse registrieren (einmalig)
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = SettingsWndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WNDCLASS_SETTINGS;
    wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    RegisterClassW(&wc); // Fehler ignorieren wenn schon registriert

    // Dialog-Zustand
    DlgState state;
    state.cfg      = loadConfig();
    state.accepted = false;

    // Fenstergröße: Client 270 x 200 → inkl. Titelleiste ca. 270 x 232
    int cw = 270, ch = 200;
    RECT r{ 0, 0, cw, ch };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    int ww = r.right - r.left;
    int wh = r.bottom - r.top;

    // Zentriert über Parent (oder Bildschirm)
    int sx = CW_USEDEFAULT, sy = CW_USEDEFAULT;
    if (hwndParent) {
        RECT pr; GetWindowRect(hwndParent, &pr);
        sx = pr.left + (pr.right  - pr.left - ww) / 2;
        sy = pr.top  + (pr.bottom - pr.top  - wh) / 2;
    }

    HWND hwnd = CreateWindowW(
        WNDCLASS_SETTINGS,
        Locale::instance().tw("dlg_title").c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        sx, sy, ww, wh,
        hwndParent, nullptr, hInst, &state
    );
    if (!hwnd) return;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Modaler Message-Loop
    if (hwndParent) EnableWindow(hwndParent, FALSE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hwndParent) {
        EnableWindow(hwndParent, TRUE);
        SetForegroundWindow(hwndParent);
    }

	if (state.accepted)
		saveConfig(state.cfg);
}
