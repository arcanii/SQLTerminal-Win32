// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/SqlEditorControl.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "core/SqlSyntaxHighlighter.h"
#include "core/SqlText.h"
#include "editor/EditorModel.h"
#include "ui/Theme.h"

namespace sqlterm {

namespace {

// Text inset (96-dpi design), mirroring NSTextView's textContainerInset (4x6).
constexpr int kInsetX = 4;
constexpr int kInsetY = 6;
constexpr UINT_PTR kBlinkTimer = 1;
constexpr UINT_PTR kAutoScrollTimer = 2;

// Device-independent factories: created once, live for the process (like the
// themeBrush cache in Theme.h). Never released.
ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dwrite = nullptr;

template <class T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Per-control state, hung off GWLP_USERDATA.
struct EditorState {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    editor::EditorModel model;

    // Device-dependent (recreated on resize/device-loss).
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brText = nullptr;
    ID2D1SolidColorBrush* brKeyword = nullptr;
    ID2D1SolidColorBrush* brNumber = nullptr;
    ID2D1SolidColorBrush* brString = nullptr;
    ID2D1SolidColorBrush* brComment = nullptr;
    ID2D1SolidColorBrush* brSelection = nullptr;
    ID2D1SolidColorBrush* brCaret = nullptr;

    // Device-independent layout (survives device-loss; rebuilt on text/width/dpi).
    IDWriteTextFormat* format = nullptr;
    IDWriteTextLayout* layout = nullptr;
    float lineH = 0.0f;
    bool layoutDirty = true;
    bool spansDirty = true;
    std::vector<sqlcore::HighlightSpan> spans;

    // View state.
    int scrollY = 0;
    int lastClientW = -1;
    float desiredX = -1.0f;  // sticky column for up/down; <0 = recompute
    bool hasFocus = false;
    bool selecting = false;
    bool caretOn = true;
    wchar_t pendingHigh = 0;  // buffered high surrogate from WM_CHAR
    UINT_PTR blinkTimer = 0;
    UINT_PTR autoTimer = 0;
};

EditorState* state(HWND h) {
    return reinterpret_cast<EditorState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

int dpx(EditorState* st, int v) { return MulDiv(v, static_cast<int>(st->dpi), 96); }

D2D1_COLOR_F colorToD2D(COLORREF c) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, 1.0f);
}

// ---- factories / format ----------------------------------------------------

bool ensureFactories() {
    if (!g_d2d) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return false;
    }
    if (!g_dwrite) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&g_dwrite))))
            return false;
    }
    return true;
}

void measureMetrics(EditorState* st) {
    st->lineH = 0;
    if (!g_dwrite || !st->format) return;
    IDWriteTextLayout* tmp = nullptr;
    if (SUCCEEDED(g_dwrite->CreateTextLayout(L"Ag", 2, st->format, 1.0e5f, 1.0e5f, &tmp)) && tmp) {
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(tmp->GetMetrics(&tm))) st->lineH = tm.height;
        tmp->Release();
    }
    if (st->lineH <= 0) st->lineH = static_cast<float>(dpx(st, 16));
}

void ensureFormat(EditorState* st) {
    if (st->format) return;
    if (!g_dwrite) return;
    // Match the previous RichEdit size (11pt). RT DPI is fixed at 96, so the
    // em-size (in DIPs == pixels) carries the DPI scaling.
    const float emSize = 11.0f * static_cast<float>(st->dpi) / 72.0f;
    if (FAILED(g_dwrite->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          emSize, L"", &st->format)) ||
        !st->format) {
        st->format = nullptr;
        return;
    }
    st->format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    measureMetrics(st);
}

// ---- brushes ---------------------------------------------------------------

void releaseBrushes(EditorState* st) {
    SafeRelease(st->brText);
    SafeRelease(st->brKeyword);
    SafeRelease(st->brNumber);
    SafeRelease(st->brString);
    SafeRelease(st->brComment);
    SafeRelease(st->brSelection);
    SafeRelease(st->brCaret);
}

// Returns false if any brush failed to create (treated as device-loss by the
// caller, so paint never runs with a partially-null brush set).
bool createBrushes(EditorState* st) {
    releaseBrushes(st);
    if (!st->rt) return false;
    const Theme& th = currentTheme();
    bool ok = true;
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.textPrimary), &st->brText));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.synKeyword), &st->brKeyword));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.synNumber), &st->brNumber));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.synString), &st->brString));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.synComment), &st->brComment));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.selectionBg), &st->brSelection));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.textPrimary), &st->brCaret));
    return ok;
}

ID2D1SolidColorBrush* brushForType(EditorState* st, sqlcore::SyntaxToken t) {
    switch (t) {
        case sqlcore::SyntaxToken::Keyword: return st->brKeyword;
        case sqlcore::SyntaxToken::Number: return st->brNumber;
        case sqlcore::SyntaxToken::StringLiteral: return st->brString;
        case sqlcore::SyntaxToken::Comment: return st->brComment;
    }
    return st->brText;
}

// ---- device resources ------------------------------------------------------

void discardDeviceResources(EditorState* st) {
    releaseBrushes(st);
    SafeRelease(st->rt);
}

// Idempotent — called at the top of every paint, so the device-loss recovery
// path is the same as the cold-start path.
bool ensureDeviceResources(EditorState* st) {
    if (!ensureFactories()) return false;
    ensureFormat(st);
    if (st->rt) return true;

    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>((std::max<LONG>)(1, rc.right - rc.left)),
                                         static_cast<UINT32>((std::max<LONG>)(1, rc.bottom - rc.top)));
    // DPI fixed at 96 => 1 DIP == 1 physical pixel; all geometry is in pixels.
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(st->hwnd, size);
    if (FAILED(g_d2d->CreateHwndRenderTarget(props, hp, &st->rt)) || !st->rt) {
        st->rt = nullptr;
        return false;
    }
    st->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    if (!createBrushes(st)) {
        discardDeviceResources(st);
        return false;
    }
    st->layoutDirty = true;  // re-apply color drawing effects with the new brushes
    return true;
}

// ---- layout ----------------------------------------------------------------

void rebuildLayout(EditorState* st) {
    if (!ensureFactories()) return;
    ensureFormat(st);
    if (!st->format) return;
    SafeRelease(st->layout);

    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int inset = dpx(st, kInsetX);
    const float maxW =
        (rc.right - rc.left > inset * 2) ? static_cast<float>(rc.right - rc.left - inset * 2) : 600.0f;

    const std::wstring& text = st->model.text();
    if (FAILED(g_dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), st->format,
                                          maxW, 1.0e6f, &st->layout)) ||
        !st->layout) {
        st->layout = nullptr;
        st->layoutDirty = true;
        return;
    }
    if (st->spansDirty) {
        st->spans = sqlcore::SqlSyntaxHighlighter::computeSpans(text);
        st->spansDirty = false;
    }
    if (st->brKeyword) {  // brushes exist -> color the spans (else applied post-RT)
        for (const auto& s : st->spans) {
            const DWRITE_TEXT_RANGE r{static_cast<UINT32>(s.location), static_cast<UINT32>(s.length)};
            st->layout->SetDrawingEffect(brushForType(st, s.type), r);
        }
    }
    st->layoutDirty = false;
}

float layoutHeight(EditorState* st) {
    if (!st->layout) return 0;
    DWRITE_TEXT_METRICS tm{};
    if (FAILED(st->layout->GetMetrics(&tm))) return st->lineH;
    return (std::max)(tm.height, st->lineH);
}

void clampScroll(EditorState* st) {
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int viewH = rc.bottom - rc.top;
    const int contentH = static_cast<int>(layoutHeight(st)) + dpx(st, kInsetY) * 2;
    const int maxScroll = (std::max)(0, contentH - viewH);
    if (st->scrollY < 0) st->scrollY = 0;
    if (st->scrollY > maxScroll) st->scrollY = maxScroll;
}

// ---- geometry --------------------------------------------------------------

bool caretMetrics(EditorState* st, float& x, float& top, float& height) {
    if (!st->layout) return false;
    DWRITE_HIT_TEST_METRICS m{};
    float px = 0, py = 0;
    if (FAILED(st->layout->HitTestTextPosition(static_cast<UINT32>(st->model.caret()), FALSE, &px,
                                               &py, &m)))
        return false;
    x = px;
    top = m.top;
    height = (m.height > 0) ? m.height : st->lineH;
    return true;
}

size_t offsetFromPoint(EditorState* st, int mx, int my) {
    if (!st->layout) return st->model.caret();
    const float lx = static_cast<float>(mx - dpx(st, kInsetX));
    const float ly = static_cast<float>(my - (dpx(st, kInsetY) - st->scrollY));
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    if (FAILED(st->layout->HitTestPoint(lx, ly, &trailing, &inside, &m))) return st->model.caret();
    return static_cast<size_t>(m.textPosition) + (trailing ? static_cast<size_t>(m.length) : 0);
}

size_t verticalTarget(EditorState* st, int dirLines) {
    if (!st->layout) return st->model.caret();
    DWRITE_HIT_TEST_METRICS m{};
    float px = 0, py = 0;
    if (FAILED(st->layout->HitTestTextPosition(static_cast<UINT32>(st->model.caret()), FALSE, &px,
                                               &py, &m)))
        return st->model.caret();
    if (st->desiredX < 0) st->desiredX = px;
    float targetY = m.top + m.height * 0.5f + static_cast<float>(dirLines) * st->lineH;
    if (targetY < 0) return 0;  // above the first line -> document start
    DWRITE_TEXT_METRICS tm{};
    if (SUCCEEDED(st->layout->GetMetrics(&tm)) && targetY > tm.height)
        return st->model.length();  // below the last line -> document end
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS hm{};
    st->layout->HitTestPoint(st->desiredX, targetY, &trailing, &inside, &hm);
    return static_cast<size_t>(hm.textPosition) + (trailing ? static_cast<size_t>(hm.length) : 0);
}

void ensureCaretVisible(EditorState* st) {
    float x, top, h;
    if (!caretMetrics(st, x, top, h)) return;
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int viewH = rc.bottom - rc.top;
    const int insetY = dpx(st, kInsetY);
    const int caretTop = insetY + static_cast<int>(top);
    const int caretBot = insetY + static_cast<int>(top + h);
    if (caretTop < st->scrollY)
        st->scrollY = caretTop;
    else if (caretBot > st->scrollY + viewH)
        st->scrollY = caretBot - viewH;
    clampScroll(st);
}

void updateScrollbar(EditorState* st) {
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int viewH = rc.bottom - rc.top;
    const int contentH = static_cast<int>(layoutHeight(st)) + dpx(st, kInsetY) * 2;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = (std::max)(0, contentH - 1);
    si.nPage = static_cast<UINT>((std::max)(0, viewH));
    si.nPos = st->scrollY;
    SetScrollInfo(st->hwnd, SB_VERT, &si, TRUE);
}

// ---- painting --------------------------------------------------------------

void paint(EditorState* st) {
    PAINTSTRUCT ps;
    BeginPaint(st->hwnd, &ps);
    if (ensureDeviceResources(st)) {
        if (st->layoutDirty || !st->layout) rebuildLayout(st);
        clampScroll(st);
        updateScrollbar(st);

        const Theme& th = currentTheme();
        st->rt->BeginDraw();
        st->rt->Clear(colorToD2D(th.windowBg));
        const float ox = static_cast<float>(dpx(st, kInsetX));
        const float oy = static_cast<float>(dpx(st, kInsetY)) - static_cast<float>(st->scrollY);

        if (st->layout && st->brText) {
            const auto sel = st->model.selection();
            if (!sel.empty() && st->brSelection) {
                const UINT32 start = static_cast<UINT32>(sel.min());
                const UINT32 len = static_cast<UINT32>(sel.max() - sel.min());
                UINT32 n = 0;
                st->layout->HitTestTextRange(start, len, ox, oy, nullptr, 0, &n);
                if (n) {
                    std::vector<DWRITE_HIT_TEST_METRICS> hm(n);
                    if (SUCCEEDED(st->layout->HitTestTextRange(start, len, ox, oy, hm.data(), n, &n))) {
                        for (UINT32 i = 0; i < n; ++i) {
                            const D2D1_RECT_F r = D2D1::RectF(hm[i].left, hm[i].top,
                                                             hm[i].left + hm[i].width,
                                                             hm[i].top + hm[i].height);
                            st->rt->FillRectangle(r, st->brSelection);
                        }
                    }
                }
            }

            st->rt->DrawTextLayout(D2D1::Point2F(ox, oy), st->layout, st->brText,
                                   D2D1_DRAW_TEXT_OPTIONS_NONE);

            if (st->hasFocus && st->caretOn) {
                float cx, ctop, ch;
                if (caretMetrics(st, cx, ctop, ch) && st->brCaret) {
                    const float w = static_cast<float>((std::max)(1, dpx(st, 1)));
                    const D2D1_RECT_F r = D2D1::RectF(ox + cx, oy + ctop, ox + cx + w, oy + ctop + ch);
                    st->rt->FillRectangle(r, st->brCaret);
                }
            }
        }

        if (st->rt->EndDraw() == D2DERR_RECREATE_TARGET) discardDeviceResources(st);
    }
    EndPaint(st->hwnd, &ps);
}

// ---- post-edit / caret-move bookkeeping ------------------------------------

void afterEdit(EditorState* st) {
    st->spansDirty = true;
    st->layoutDirty = true;
    st->desiredX = -1;
    st->caretOn = true;
    rebuildLayout(st);  // text changed -> rebuild now so caret geometry is current
    ensureCaretVisible(st);
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

void caretMoved(EditorState* st, bool resetDesiredX) {
    if (resetDesiredX) st->desiredX = -1;
    st->caretOn = true;
    ensureCaretVisible(st);
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

void pageMove(EditorState* st, int dir, bool shift) {
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int viewH = rc.bottom - rc.top;
    const int lines = (std::max)(1, static_cast<int>(viewH / (std::max)(1.0f, st->lineH)) - 1);
    const size_t t = verticalTarget(st, dir * lines);
    st->model.setCaret(t, shift);
    caretMoved(st, false);
}

// ---- clipboard -------------------------------------------------------------

void copyToClipboard(EditorState* st) {
    const auto sel = st->model.selection();
    if (sel.empty()) return;
    const std::wstring s = st->model.text().substr(sel.min(), sel.max() - sel.min());
    if (!OpenClipboard(st->hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(h)) {
            std::memcpy(p, s.c_str(), bytes);
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);  // ownership stays ours on fail
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

void cutToClipboard(EditorState* st) {
    copyToClipboard(st);
    st->model.deleteSelection();
}

void pasteFromClipboard(EditorState* st) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
    if (!OpenClipboard(st->hwnd)) return;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            st->model.insertText(editor::normalizeNewlines(p));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

void selectWordAt(EditorState* st, size_t off) {
    using sqlcore::text::isWordChar;
    const std::wstring& t = st->model.text();
    size_t a = off, b = off;
    if (off < t.size() && isWordChar(t[off])) {
        while (a > 0 && isWordChar(t[a - 1])) --a;
        while (b < t.size() && isWordChar(t[b])) ++b;
    } else if (off > 0 && isWordChar(t[off - 1])) {
        while (a > 0 && isWordChar(t[a - 1])) --a;
        b = off;
    } else {
        st->model.setCaret(off, false);
        return;
    }
    st->model.setSelection(a, b);
}

// ---- caret blink -----------------------------------------------------------

void startBlink(EditorState* st) {
    const UINT bt = GetCaretBlinkTime();
    if (bt == 0 || bt == INFINITE) {
        st->caretOn = true;  // blinking disabled by the user
        return;
    }
    st->blinkTimer = SetTimer(st->hwnd, kBlinkTimer, bt, nullptr);
}

void stopBlink(EditorState* st) {
    if (st->blinkTimer) {
        KillTimer(st->hwnd, kBlinkTimer);
        st->blinkTimer = 0;
    }
}

// ---- IME -------------------------------------------------------------------

void positionImeWindow(EditorState* st) {
    HIMC himc = ImmGetContext(st->hwnd);
    if (!himc) return;
    float cx, ctop, ch;
    if (caretMetrics(st, cx, ctop, ch)) {
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos.x = dpx(st, kInsetX) + static_cast<LONG>(cx);
        cf.ptCurrentPos.y = dpx(st, kInsetY) + static_cast<LONG>(ctop) - st->scrollY;
        ImmSetCompositionWindow(himc, &cf);
    }
    ImmReleaseContext(st->hwnd, himc);
}

// ---- input -----------------------------------------------------------------

void onChar(EditorState* st, wchar_t c) {
    std::wstring s;
    if (IS_HIGH_SURROGATE(c)) {
        st->pendingHigh = c;
        return;
    }
    if (IS_LOW_SURROGATE(c)) {
        if (!st->pendingHigh) return;  // stray low surrogate
        s.push_back(st->pendingHigh);
        s.push_back(c);
        st->pendingHigh = 0;
    } else {
        st->pendingHigh = 0;
        if (c == L'\r')
            s = L"\n";
        else if (c == L'\t')
            s = L"\t";
        else if (c < 0x20)
            return;  // control chars (Ctrl+letters, Backspace, etc.) handled in WM_KEYDOWN
        else
            s.push_back(c);
    }
    st->model.insertText(s);
    afterEdit(st);
}

// Returns true if the key was handled.
bool onKeyDown(EditorState* st, WPARAM vk) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (vk) {
        case VK_LEFT: st->model.moveLeft(shift, ctrl); caretMoved(st, true); return true;
        case VK_RIGHT: st->model.moveRight(shift, ctrl); caretMoved(st, true); return true;
        case VK_UP: st->model.setCaret(verticalTarget(st, -1), shift); caretMoved(st, false); return true;
        case VK_DOWN: st->model.setCaret(verticalTarget(st, +1), shift); caretMoved(st, false); return true;
        case VK_PRIOR: pageMove(st, -1, shift); return true;
        case VK_NEXT: pageMove(st, +1, shift); return true;
        case VK_HOME:
            if (ctrl)
                st->model.moveDocStart(shift);
            else
                st->model.moveLineHome(shift);
            caretMoved(st, true);
            return true;
        case VK_END:
            if (ctrl)
                st->model.moveDocEnd(shift);
            else
                st->model.moveLineEnd(shift);
            caretMoved(st, true);
            return true;
        case VK_BACK:
            if (ctrl)
                st->model.deleteWordLeft();
            else
                st->model.backspace();
            afterEdit(st);
            return true;
        case VK_DELETE:
            if (shift)
                cutToClipboard(st);
            else if (ctrl)
                st->model.deleteWordRight();
            else
                st->model.deleteForward();
            afterEdit(st);
            return true;
        case 'A':
            if (ctrl) {
                st->model.selectAll();
                caretMoved(st, true);
                return true;
            }
            return false;
        case 'C':
            if (ctrl) {
                copyToClipboard(st);
                return true;
            }
            return false;
        case 'X':
            if (ctrl) {
                cutToClipboard(st);
                afterEdit(st);
                return true;
            }
            return false;
        case 'V':
            if (ctrl) {
                pasteFromClipboard(st);
                afterEdit(st);
                return true;
            }
            return false;
        case 'Z':
            if (ctrl) {
                if (shift) {
                    if (st->model.canRedo()) {
                        st->model.redo();
                        afterEdit(st);
                    }
                } else if (st->model.canUndo()) {
                    st->model.undo();
                    afterEdit(st);
                }
                return true;
            }
            return false;
        case 'Y':
            if (ctrl) {
                if (st->model.canRedo()) {
                    st->model.redo();
                    afterEdit(st);
                }
                return true;
            }
            return false;
        case VK_INSERT:
            if (ctrl) {
                copyToClipboard(st);
                return true;
            }
            if (shift) {
                pasteFromClipboard(st);
                afterEdit(st);
                return true;
            }
            return false;
        default: return false;
    }
}

void onMouseMove(EditorState* st, int mx, int my, WPARAM keys) {
    if (!st->selecting || !(keys & MK_LBUTTON)) return;
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const bool outside = (my < 0 || my > rc.bottom);
    if (outside) {
        if (!st->autoTimer) st->autoTimer = SetTimer(st->hwnd, kAutoScrollTimer, 40, nullptr);
    } else if (st->autoTimer) {
        KillTimer(st->hwnd, kAutoScrollTimer);
        st->autoTimer = 0;
    }
    const int cy = (std::min)((std::max)(my, 0), static_cast<int>(rc.bottom));
    st->model.setCaret(offsetFromPoint(st, mx, cy), true);
    caretMoved(st, true);
}

void onAutoScroll(EditorState* st) {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(st->hwnd, &pt);
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    if (pt.y < 0)
        st->scrollY -= dpx(st, 24);
    else if (pt.y > rc.bottom)
        st->scrollY += dpx(st, 24);
    clampScroll(st);
    const int cy = (std::min)((std::max)(static_cast<int>(pt.y), 0), static_cast<int>(rc.bottom));
    st->model.setCaret(offsetFromPoint(st, pt.x, cy), true);
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

void onVScroll(EditorState* st, WPARAM wParam) {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(st->hwnd, SB_VERT, &si);
    int pos = si.nPos;
    const int line = static_cast<int>((std::max)(1.0f, st->lineH));
    switch (LOWORD(wParam)) {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        default: break;
    }
    st->scrollY = pos;
    clampScroll(st);
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

// ---- window procedure ------------------------------------------------------

LRESULT CALLBACK SqlEditorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* st = new (std::nothrow) EditorState();
        if (!st) return FALSE;  // fail CreateWindowEx cleanly under memory pressure
        st->hwnd = hwnd;
        const UINT d = GetDpiForWindow(hwnd);
        st->dpi = d ? d : 96;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    EditorState* st = state(hwnd);
    if (!st) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_PAINT: paint(st); return 0;
        case WM_ERASEBKGND: return 1;  // D2D clears the whole client
        case WM_SIZE: {
            if (st->rt) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                if (st->rt->Resize(D2D1::SizeU(
                        static_cast<UINT32>((std::max<LONG>)(1, rc.right - rc.left)),
                        static_cast<UINT32>((std::max<LONG>)(1, rc.bottom - rc.top)))) ==
                    D2DERR_RECREATE_TARGET)
                    discardDeviceResources(st);
            }
            RECT rc2;
            GetClientRect(hwnd, &rc2);
            const int w = rc2.right - rc2.left;
            if (w != st->lastClientW) {
                st->layoutDirty = true;  // width changed -> re-wrap (the anti-RichEdit win)
                st->lastClientW = w;
            }
            clampScroll(st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CHAR: onChar(st, static_cast<wchar_t>(wParam)); return 0;
        case WM_KEYDOWN:
            if (onKeyDown(st, wParam)) return 0;
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            SetCapture(hwnd);
            st->selecting = true;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            st->model.setCaret(offsetFromPoint(st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)), shift);
            caretMoved(st, true);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            selectWordAt(st, offsetFromPoint(st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
            caretMoved(st, true);
            return 0;
        case WM_MOUSEMOVE: onMouseMove(st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam); return 0;
        case WM_LBUTTONUP:
            if (st->selecting) {
                st->selecting = false;
                if (GetCapture() == hwnd) ReleaseCapture();
            }
            if (st->autoTimer) {
                KillTimer(hwnd, kAutoScrollTimer);
                st->autoTimer = 0;
            }
            return 0;
        case WM_CAPTURECHANGED:  // capture stolen mid-drag (no WM_LBUTTONUP)
            st->selecting = false;
            if (st->autoTimer) {
                KillTimer(hwnd, kAutoScrollTimer);
                st->autoTimer = 0;
            }
            return 0;
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const int line = static_cast<int>((std::max)(1.0f, st->lineH));
            st->scrollY -= (delta / WHEEL_DELTA) * 3 * line;
            clampScroll(st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: onVScroll(st, wParam); return 0;
        case WM_SETFOCUS:
            st->hasFocus = true;
            st->caretOn = true;
            startBlink(st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KILLFOCUS:
            st->hasFocus = false;
            st->pendingHigh = 0;  // drop a half-typed surrogate rather than cross a focus change
            stopBlink(st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wParam == kBlinkTimer) {
                st->caretOn = !st->caretOn;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wParam == kAutoScrollTimer) {
                onAutoScroll(st);
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_IME_STARTCOMPOSITION:
            positionImeWindow(st);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_SETTEXT: {
            const wchar_t* s = reinterpret_cast<const wchar_t*>(lParam);
            st->model.setText(s ? s : L"");
            st->model.setCaret(0, false);  // external set: caret/view to the top
            st->scrollY = 0;
            st->pendingHigh = 0;
            afterEdit(st);
            return TRUE;
        }
        case WM_GETTEXTLENGTH: return static_cast<LRESULT>(st->model.length());
        case WM_GETTEXT: {
            const size_t cap = static_cast<size_t>(wParam);
            if (cap == 0) return 0;
            const std::wstring& t = st->model.text();
            const size_t n = (std::min)(t.size(), cap - 1);
            auto* buf = reinterpret_cast<wchar_t*>(lParam);
            std::memcpy(buf, t.c_str(), n * sizeof(wchar_t));
            buf[n] = L'\0';
            return static_cast<LRESULT>(n);
        }
        case WM_NCDESTROY:
            stopBlink(st);
            if (st->autoTimer) KillTimer(hwnd, kAutoScrollTimer);
            discardDeviceResources(st);
            SafeRelease(st->layout);
            SafeRelease(st->format);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        default: return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

// ---- public API ------------------------------------------------------------

bool registerSqlEditorClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = SqlEditorProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.hbrBackground = nullptr;  // we paint the whole client via Direct2D
    wc.lpszClassName = L"SqlD2DEditor";
    if (!RegisterClassExW(&wc)) return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return true;
}

std::wstring editorText(HWND edit) {
    EditorState* st = state(edit);
    return st ? st->model.text() : std::wstring();
}

LONG caretOffset(HWND edit) {
    EditorState* st = state(edit);
    return st ? static_cast<LONG>(st->model.selection().min()) : 0;
}

void editorApplyTheme(HWND edit) {
    EditorState* st = state(edit);
    if (!st) return;
    SetWindowTheme(edit, currentTheme().dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    if (st->rt) {
        if (!createBrushes(st)) discardDeviceResources(st);  // recreate cleanly on next paint
        st->layoutDirty = true;  // re-apply color effects with the new brushes
    }
    InvalidateRect(edit, nullptr, FALSE);
}

void editorUpdateDpi(HWND edit, UINT dpi) {
    EditorState* st = state(edit);
    if (!st) return;
    st->dpi = dpi ? dpi : 96;
    SafeRelease(st->format);  // font em-size depends on DPI
    st->lineH = 0;
    ensureFormat(st);
    st->layoutDirty = true;
    InvalidateRect(edit, nullptr, FALSE);
}

}  // namespace sqlterm
