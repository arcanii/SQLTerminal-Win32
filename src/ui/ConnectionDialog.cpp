// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/ConnectionDialog.h"

#include <commdlg.h>

#include <algorithm>
#include <string>
#include <vector>

#include "models/ConnectionProfile.h"
#include "persistence/Stores.h"
#include "security/CredentialStore.h"

namespace sqlterm {
namespace {

constexpr wchar_t kDlgClass[] = L"SQLTerminalConnectionDialog";

enum : int {
    IDC_ENGINE = 100,
    IDC_PROFILE,
    IDC_FILE_LBL, IDC_FILE, IDC_BROWSE,
    IDC_HOST_LBL, IDC_HOST,
    IDC_PORT_LBL, IDC_PORT,
    IDC_DB_LBL, IDC_DB,
    IDC_USER_LBL, IDC_USER,
    IDC_PASS_LBL, IDC_PASS, IDC_SHOWPASS,
    IDC_SSL_LBL, IDC_SSL,
    IDC_REMEMBER,
    IDC_SAVEAS_LBL, IDC_SAVEAS,
    IDC_STATUS,
};

constexpr int W = 470;
constexpr int H = 412;

struct DlgState {
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    std::vector<ConnectionProfile> profiles;  // aligned to profile combo items 1..N
    std::optional<ConnectionRequest> result;
    bool done = false;
};

std::wstring getText(HWND c) {
    const int len = GetWindowTextLengthW(c);
    std::wstring s;
    if (len <= 0) return s;
    s.resize(static_cast<size_t>(len));
    GetWindowTextW(c, &s[0], len + 1);
    return s;
}
HWND ctrl(DlgState* st, int id) { return GetDlgItem(st->hwnd, id); }
void setText(DlgState* st, int id, const std::wstring& s) { SetWindowTextW(ctrl(st, id), s.c_str()); }
bool isChecked(DlgState* st, int id) { return SendMessageW(ctrl(st, id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void setCheck(DlgState* st, int id, bool on) {
    SendMessageW(ctrl(st, id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}
int comboSel(DlgState* st, int id) { return static_cast<int>(SendMessageW(ctrl(st, id), CB_GETCURSEL, 0, 0)); }
void setComboSel(DlgState* st, int id, int i) { SendMessageW(ctrl(st, id), CB_SETCURSEL, i, 0); }

DatabaseEngine selectedEngine(DlgState* st) {
    return comboSel(st, IDC_ENGINE) == 1 ? DatabaseEngine::Postgres : DatabaseEngine::Sqlite;
}

std::wstring trimWs(const std::wstring& s) {
    auto ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r'; };
    size_t b = 0, e = s.size();
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

void setStatus(DlgState* st, const std::wstring& s) { setText(st, IDC_STATUS, s); }

void updateVisibility(DlgState* st) {
    const bool sqlite = selectedEngine(st) == DatabaseEngine::Sqlite;
    const int sqliteIds[] = {IDC_FILE_LBL, IDC_FILE, IDC_BROWSE};
    const int pgIds[] = {IDC_HOST_LBL, IDC_HOST,  IDC_PORT_LBL,  IDC_PORT,
                         IDC_DB_LBL,   IDC_DB,     IDC_USER_LBL,  IDC_USER,
                         IDC_PASS_LBL, IDC_PASS,   IDC_SHOWPASS,  IDC_SSL_LBL,
                         IDC_SSL,      IDC_REMEMBER};
    for (int id : sqliteIds) ShowWindow(ctrl(st, id), sqlite ? SW_SHOW : SW_HIDE);
    for (int id : pgIds) ShowWindow(ctrl(st, id), sqlite ? SW_HIDE : SW_SHOW);
}

bool canConnect(DlgState* st) {
    if (selectedEngine(st) == DatabaseEngine::Sqlite) {
        return !trimWs(getText(ctrl(st, IDC_FILE))).empty();
    }
    return !getText(ctrl(st, IDC_HOST)).empty() && !getText(ctrl(st, IDC_PORT)).empty() &&
           !getText(ctrl(st, IDC_DB)).empty() && !getText(ctrl(st, IDC_USER)).empty();
}

void applyProfile(DlgState* st, const ConnectionProfile& p) {
    setComboSel(st, IDC_ENGINE, p.engine == DatabaseEngine::Postgres ? 1 : 0);
    setText(st, IDC_FILE, p.filePath);
    setText(st, IDC_HOST, p.host);
    setText(st, IDC_PORT, p.port);
    setText(st, IDC_DB, p.databaseName);
    setText(st, IDC_USER, p.username);
    const SslMode mode = p.sslMode.value_or(SslMode::Prefer);
    setComboSel(st, IDC_SSL, mode == SslMode::Off ? 0 : mode == SslMode::Require ? 2 : 1);
    setText(st, IDC_PASS, L"");
    setCheck(st, IDC_REMEMBER, false);

    if (p.engine == DatabaseEngine::Postgres) {
        DatabaseConnection c;
        c.engine = p.engine;
        c.host = p.host;
        c.port = p.port;
        c.databaseName = p.databaseName;
        c.username = p.username;
        if (auto pw = CredentialStore::password(CredentialStore::accountKey(c))) {
            setText(st, IDC_PASS, *pw);
            setCheck(st, IDC_REMEMBER, true);
        }
    }
    updateVisibility(st);
}

void doBrowse(DlgState* st) {
    wchar_t file[1024] = L"";
    const std::wstring cur = getText(ctrl(st, IDC_FILE));
    if (!cur.empty()) lstrcpynW(file, cur.c_str(), 1024);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = L"SQLite Database\0*.db;*.sqlite;*.sqlite3\0All Files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Open or Create SQLite Database";
    if (GetOpenFileNameW(&ofn)) setText(st, IDC_FILE, file);
}

void buildRequest(DlgState* st) {
    DatabaseConnection c;
    c.engine = selectedEngine(st);
    c.filePath = getText(ctrl(st, IDC_FILE));
    c.host = getText(ctrl(st, IDC_HOST));
    c.port = getText(ctrl(st, IDC_PORT));
    c.databaseName = getText(ctrl(st, IDC_DB));
    c.username = getText(ctrl(st, IDC_USER));
    c.password = getText(ctrl(st, IDC_PASS));
    const int ssl = comboSel(st, IDC_SSL);
    c.sslMode = ssl == 0 ? SslMode::Off : ssl == 2 ? SslMode::Require : SslMode::Prefer;

    ConnectionRequest req;
    req.connection = c;
    const std::wstring saveAs = trimWs(getText(ctrl(st, IDC_SAVEAS)));
    if (!saveAs.empty()) req.saveAsName = saveAs;
    req.rememberPassword = isChecked(st, IDC_REMEMBER);
    st->result = req;
    st->done = true;
}

// ---- control creation -------------------------------------------------------

HWND mk(DlgState* st, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
        int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, st->hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                             nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
    return c;
}
void label(DlgState* st, int id, const wchar_t* text, int x, int y, int w) {
    mk(st, L"STATIC", text, SS_RIGHT, x, y + 3, w, 18, id);
}

void createControls(DlgState* st) {
    const int lx = 14, lw = 92, fx = 112, fw = 344;
    label(st, 0, L"Engine:", lx, 14, lw);
    HWND eng = mk(st, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, fx, 12, 180, 200, IDC_ENGINE);
    SendMessageW(eng, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"SQLite"));
    SendMessageW(eng, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PostgreSQL"));

    label(st, 0, L"Load profile:", lx, 46, lw);
    HWND prof = mk(st, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, fx, 44, fw, 240, IDC_PROFILE);
    SendMessageW(prof, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"— New connection —"));
    for (const auto& p : RecentConnectionsStore::load()) {
        st->profiles.push_back(p);
        SendMessageW(prof, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((L"Recent — " + p.displayName()).c_str()));
    }
    for (const auto& p : SavedProfilesStore::load()) {
        st->profiles.push_back(p);
        SendMessageW(prof, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((L"Saved — " + p.displayName()).c_str()));
    }
    setComboSel(st, IDC_PROFILE, 0);

    // SQLite
    label(st, IDC_FILE_LBL, L"Database file:", lx, 86, lw);
    mk(st, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, fx, 84, 244, 22, IDC_FILE);
    mk(st, L"BUTTON", L"Browse…", BS_PUSHBUTTON | WS_TABSTOP, fx + 252, 83, 92, 24, IDC_BROWSE);

    // PostgreSQL
    label(st, IDC_HOST_LBL, L"Host:", lx, 86, lw);
    mk(st, L"EDIT", L"localhost", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, fx, 84, fw, 22, IDC_HOST);
    label(st, IDC_PORT_LBL, L"Port:", lx, 118, lw);
    mk(st, L"EDIT", L"5432", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, fx, 116, 120, 22, IDC_PORT);
    label(st, IDC_DB_LBL, L"Database:", lx, 150, lw);
    mk(st, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, fx, 148, fw, 22, IDC_DB);
    label(st, IDC_USER_LBL, L"User:", lx, 182, lw);
    mk(st, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, fx, 180, fw, 22, IDC_USER);
    label(st, IDC_PASS_LBL, L"Password:", lx, 214, lw);
    mk(st, L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, fx, 212, 244, 22, IDC_PASS);
    mk(st, L"BUTTON", L"Show", BS_AUTOCHECKBOX | WS_TABSTOP, fx + 252, 214, 92, 20, IDC_SHOWPASS);
    label(st, IDC_SSL_LBL, L"SSL mode:", lx, 246, lw);
    HWND ssl = mk(st, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, fx, 244, 180, 160, IDC_SSL);
    SendMessageW(ssl, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Off"));
    SendMessageW(ssl, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Prefer"));
    SendMessageW(ssl, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Require"));
    setComboSel(st, IDC_SSL, 1);
    mk(st, L"BUTTON", L"Remember password", BS_AUTOCHECKBOX | WS_TABSTOP, fx, 278, fw, 20, IDC_REMEMBER);

    label(st, IDC_SAVEAS_LBL, L"Save as profile:", lx, 320, lw);
    mk(st, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, fx, 318, fw, 22, IDC_SAVEAS);

    mk(st, L"STATIC", L"", SS_LEFTNOWORDWRAP, lx, 350, W - 28, 18, IDC_STATUS);

    mk(st, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, W - 200, 372, 90, 26, IDCANCEL);
    mk(st, L"BUTTON", L"Connect", BS_DEFPUSHBUTTON | WS_TABSTOP, W - 104, 372, 90, 26, IDOK);

    setComboSel(st, IDC_ENGINE, 0);
    // Prefill from the most recent connection (= last connection), if any.
    if (!st->profiles.empty() && !RecentConnectionsStore::load().empty()) {
        setComboSel(st, IDC_PROFILE, 1);
        applyProfile(st, st->profiles.front());
    } else {
        updateVisibility(st);
    }
}

LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<DlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* s = reinterpret_cast<DlgState*>(cs->lpCreateParams);
            s->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_CREATE:
            createControls(st);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == IDC_ENGINE && code == CBN_SELCHANGE) {
                updateVisibility(st);
            } else if (id == IDC_PROFILE && code == CBN_SELCHANGE) {
                const int sel = comboSel(st, IDC_PROFILE);
                if (sel >= 1 && sel - 1 < static_cast<int>(st->profiles.size()))
                    applyProfile(st, st->profiles[static_cast<size_t>(sel - 1)]);
            } else if (id == IDC_SHOWPASS && code == BN_CLICKED) {
                const bool show = isChecked(st, IDC_SHOWPASS);
                SendMessageW(ctrl(st, IDC_PASS), EM_SETPASSWORDCHAR,
                             show ? 0 : static_cast<WPARAM>(L'\x25CF'), 0);
                InvalidateRect(ctrl(st, IDC_PASS), nullptr, TRUE);
            } else if (id == IDC_BROWSE && code == BN_CLICKED) {
                doBrowse(st);
            } else if (id == IDOK) {
                if (canConnect(st))
                    buildRequest(st);
                else
                    setStatus(st, L"Please fill in all required fields.");
            } else if (id == IDCANCEL) {
                st->result = std::nullopt;
                st->done = true;
            }
            return 0;
        }
        case WM_CLOSE:
            st->result = std::nullopt;
            st->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

std::optional<ConnectionRequest> showConnectionDialog(HWND owner) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kDlgClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassExW(&wc);
        registered = true;
    }

    DlgState st;
    st.font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    // Center over the owner.
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int fullW = W + 16, fullH = H + 39;  // account for frame + caption (approx)
    const int x = orc.left + ((orc.right - orc.left) - fullW) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - fullH) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kDlgClass, L"Connect to Database",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, fullW, fullH, owner, nullptr, hInst, &st);
    if (!hwnd) return std::nullopt;

    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    DestroyWindow(hwnd);
    return st.result;
}

}  // namespace sqlterm
