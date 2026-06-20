// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Central color palette for the UI. A dark and a light variant are selected by
// the current system theme (like the dark title bar), so the whole app follows
// the OS setting. Colors are tuned for a Claude-desktop-style dark look with a
// warm coral accent. Pure GDI COLORREFs — no platform deps beyond <windows.h>.
#include <windows.h>

namespace sqlterm {

struct Theme {
    bool dark;
    COLORREF windowBg;       // deepest surface (editor, window erase)
    COLORREF panelBg;        // panels (tree, grid rows)
    COLORREF panelElevBg;    // headers / elevated bands (grid header, status bar)
    COLORREF altRowBg;       // zebra-stripe row
    COLORREF hoverBg;        // hover highlight
    COLORREF border;         // hairline separators
    COLORREF textPrimary;
    COLORREF textSecondary;
    COLORREF textMuted;
    COLORREF accent;         // primary action / active (coral)
    COLORREF accentText;     // text drawn on top of `accent`
    COLORREF selectionBg;    // selected grid row background
    COLORREF selectionText;
    COLORREF synKeyword;
    COLORREF synNumber;
    COLORREF synString;
    COLORREF synComment;
};

inline Theme makeDarkTheme() {
    Theme t{};
    t.dark = true;
    t.windowBg = RGB(22, 22, 24);
    t.panelBg = RGB(26, 26, 28);
    t.panelElevBg = RGB(32, 32, 35);
    t.altRowBg = RGB(28, 28, 31);
    t.hoverBg = RGB(42, 42, 45);
    t.border = RGB(48, 48, 52);
    t.textPrimary = RGB(230, 230, 232);
    t.textSecondary = RGB(154, 154, 160);
    t.textMuted = RGB(106, 106, 112);
    t.accent = RGB(217, 119, 87);       // Claude coral
    t.accentText = RGB(40, 18, 10);
    t.selectionBg = RGB(92, 52, 38);     // muted coral
    t.selectionText = RGB(247, 238, 233);
    t.synKeyword = RGB(201, 143, 214);
    t.synNumber = RGB(159, 209, 154);
    t.synString = RGB(224, 150, 107);
    t.synComment = RGB(118, 150, 118);
    return t;
}

inline Theme makeLightTheme() {
    Theme t{};
    t.dark = false;
    t.windowBg = GetSysColor(COLOR_WINDOW);
    t.panelBg = GetSysColor(COLOR_WINDOW);
    t.panelElevBg = RGB(244, 244, 246);
    t.altRowBg = RGB(245, 245, 245);
    t.hoverBg = RGB(232, 232, 234);
    t.border = RGB(214, 214, 218);
    t.textPrimary = GetSysColor(COLOR_WINDOWTEXT);
    t.textSecondary = RGB(96, 96, 102);
    t.textMuted = RGB(140, 140, 146);
    t.accent = RGB(193, 95, 60);          // coral, darkened for light bg
    t.accentText = RGB(255, 255, 255);
    t.selectionBg = RGB(250, 232, 224);
    t.selectionText = RGB(74, 27, 12);
    t.synKeyword = RGB(199, 37, 108);
    t.synNumber = RGB(128, 0, 128);
    t.synString = RGB(196, 26, 22);
    t.synComment = RGB(34, 139, 34);
    return t;
}

// Follow the system light/dark setting (Win10 2004+). Returns false (light) when
// the registry value is missing.
inline bool systemUsesDarkMode() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

inline const Theme& currentTheme() {
    static const Theme dark = makeDarkTheme();
    static const Theme light = makeLightTheme();
    return systemUsesDarkMode() ? dark : light;
}

}  // namespace sqlterm
