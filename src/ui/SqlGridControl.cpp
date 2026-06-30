// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/SqlGridControl.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "app/ResultFormat.h"
#include "ui/CellDetailDialog.h"
#include "ui/D2DSupport.h"
#include "ui/Theme.h"

namespace sqlterm {

namespace {

constexpr int kCellPadX = 8;     // 96-dpi cell text inset (each side)
constexpr int kHeaderPadX = 10;  // header text inset
constexpr int kArrowZone = 16;   // reserved width for the sort arrow
constexpr int kDividerHit = 4;   // half-width of the column-resize hit zone
constexpr int kMinColW = 48;
constexpr int kAutoMaxColW = 440;  // auto-size cap only; manual resize is unbounded above
constexpr int kColPad = 16;  // auto-size padding added to measured width

// Context-menu command ids (local; resolved inline via TPM_RETURNCMD).
enum { CMD_VIEW = 1, CMD_COPYVAL, CMD_COPYROW, CMD_TSV, CMD_CSV };

struct GridState {
    HWND hwnd = nullptr;
    UINT dpi = 96;

    QueryResult result;
    std::vector<size_t> rowOrder;  // display index -> data row index
    std::wstring filterLower;      // lower-cased full-text row filter ("" = no filter)
    std::vector<int> colWidth;     // device px, per column
    int sortColumn = -1;
    bool sortAscending = true;
    int selectedRow = -1;  // display index, or -1
    int ctxRow = -1, ctxCol = -1;

    int scrollX = 0, scrollY = 0;
    int rowH = 0, headerH = 0;  // device px

    // Column resize drag.
    bool resizing = false;
    int resizeCol = -1, resizeStartX = 0, resizeStartW = 0;

    // Device-dependent.
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brText = nullptr;
    ID2D1SolidColorBrush* brHeaderText = nullptr;
    ID2D1SolidColorBrush* brAccent = nullptr;
    ID2D1SolidColorBrush* brPanel = nullptr;
    ID2D1SolidColorBrush* brAlt = nullptr;
    ID2D1SolidColorBrush* brHeaderBg = nullptr;
    ID2D1SolidColorBrush* brSelBg = nullptr;
    ID2D1SolidColorBrush* brSelText = nullptr;
    ID2D1SolidColorBrush* brBorder = nullptr;

    // Device-independent.
    IDWriteTextFormat* fmtCell = nullptr;
    IDWriteTextFormat* fmtHeader = nullptr;
};

GridState* state(HWND h) {
    return reinterpret_cast<GridState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

int dpx(GridState* st, int v) { return MulDiv(v, static_cast<int>(st->dpi), 96); }

int columnCount(GridState* st) { return static_cast<int>(st->result.columns.size()); }
int rowCount(GridState* st) { return static_cast<int>(st->rowOrder.size()); }

const std::wstring& cellAt(GridState* st, int displayRow, int col) {
    static const std::wstring empty;
    if (displayRow < 0 || static_cast<size_t>(displayRow) >= st->rowOrder.size()) return empty;
    const size_t src = st->rowOrder[static_cast<size_t>(displayRow)];
    if (src >= st->result.rows.size()) return empty;
    const auto& row = st->result.rows[src];
    if (col < 0 || static_cast<size_t>(col) >= row.size()) return empty;
    return row[static_cast<size_t>(col)];
}

std::wstring columnNameAt(GridState* st, int col) {
    if (col >= 0 && static_cast<size_t>(col) < st->result.columns.size())
        return st->result.columns[static_cast<size_t>(col)];
    return L"col" + std::to_wstring(col);
}

std::vector<std::vector<std::wstring>> displayRows(GridState* st) {
    std::vector<std::vector<std::wstring>> out;
    out.reserve(st->rowOrder.size());
    for (size_t d : st->rowOrder)
        if (d < st->result.rows.size()) out.push_back(st->result.rows[d]);
    return out;
}

int totalColumnsWidth(GridState* st) {
    int w = 0;
    for (int c : st->colWidth) w += c;
    return w;
}

int columnLeft(GridState* st, int col) {
    int x = 0;
    for (int i = 0; i < col && i < static_cast<int>(st->colWidth.size()); ++i) x += st->colWidth[i];
    return x;
}

int columnAtX(GridState* st, int clientX) {
    const int world = clientX + st->scrollX;
    int x = 0;
    for (int i = 0; i < static_cast<int>(st->colWidth.size()); ++i) {
        if (world >= x && world < x + st->colWidth[i]) return i;
        x += st->colWidth[i];
    }
    return -1;
}

// Column whose RIGHT edge is within the resize hit zone of clientX, or -1.
int dividerAtX(GridState* st, int clientX) {
    const int world = clientX + st->scrollX;
    const int hit = dpx(st, kDividerHit);
    int x = 0;
    for (int i = 0; i < static_cast<int>(st->colWidth.size()); ++i) {
        x += st->colWidth[i];
        if (std::abs(world - x) <= hit) return i;
    }
    return -1;
}

int rowAtY(GridState* st, int clientY) {
    if (st->rowH <= 0 || clientY < st->headerH) return -1;
    const int bodyY = clientY - st->headerH + st->scrollY;
    if (bodyY < 0) return -1;
    const int r = bodyY / st->rowH;
    return (r >= 0 && r < rowCount(st)) ? r : -1;
}

// ---- formats / metrics -----------------------------------------------------

void configFormat(IDWriteTextFormat* f) {
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);  // vertical center
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    IDWriteInlineObject* sign = nullptr;
    if (SUCCEEDED(dwriteFactory()->CreateEllipsisTrimmingSign(f, &sign))) {
        DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&t, sign);
    }
    SafeRelease(sign);
}

float measureHeight(IDWriteTextFormat* f) {
    if (!f || !dwriteFactory()) return 0;
    IDWriteTextLayout* layout = nullptr;
    float h = 0;
    if (SUCCEEDED(dwriteFactory()->CreateTextLayout(L"Ag", 2, f, 1.0e5f, 1.0e5f, &layout)) && layout) {
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(layout->GetMetrics(&m))) h = m.height;
        layout->Release();
    }
    return h;
}

float measureWidth(IDWriteTextFormat* f, const std::wstring& s) {
    if (!f || s.empty() || !dwriteFactory()) return 0;
    IDWriteTextLayout* layout = nullptr;
    float w = 0;
    if (SUCCEEDED(dwriteFactory()->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f,
                                                    1.0e5f, 1.0e5f, &layout)) &&
        layout) {
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(layout->GetMetrics(&m))) w = m.width;
        layout->Release();
    }
    return w;
}

void measureMetrics(GridState* st) {
    float ch = measureHeight(st->fmtCell);
    float hh = measureHeight(st->fmtHeader);
    if (ch <= 0) ch = static_cast<float>(dpx(st, 16));
    if (hh <= 0) hh = static_cast<float>(dpx(st, 16));
    st->rowH = static_cast<int>(ch) + dpx(st, 8);
    st->headerH = static_cast<int>(hh) + dpx(st, 14);
}

void ensureFormats(GridState* st) {
    if (!dwriteFactory()) return;
    const float em = 11.0f * static_cast<float>(st->dpi) / 72.0f;
    if (!st->fmtCell) {
        if (SUCCEEDED(dwriteFactory()->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                        DWRITE_FONT_STYLE_NORMAL,
                                                        DWRITE_FONT_STRETCH_NORMAL, em, L"",
                                                        &st->fmtCell)) &&
            st->fmtCell)
            configFormat(st->fmtCell);
    }
    if (!st->fmtHeader) {
        if (SUCCEEDED(dwriteFactory()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                        DWRITE_FONT_STYLE_NORMAL,
                                                        DWRITE_FONT_STRETCH_NORMAL, em, L"",
                                                        &st->fmtHeader)) &&
            st->fmtHeader)
            configFormat(st->fmtHeader);
    }
    measureMetrics(st);
}

void autoSizeColumns(GridState* st) {
    const int nc = columnCount(st);
    st->colWidth.assign(static_cast<size_t>(nc), dpx(st, kMinColW));
    const int pad = dpx(st, kColPad), minW = dpx(st, kMinColW), maxW = dpx(st, kAutoMaxColW);
    const size_t sample = (std::min)(st->result.rows.size(), static_cast<size_t>(120));
    for (int c = 0; c < nc; ++c) {
        float w = measureWidth(st->fmtHeader, st->result.columns[static_cast<size_t>(c)]) +
                  static_cast<float>(dpx(st, kArrowZone));
        for (size_t r = 0; r < sample; ++r) {
            const auto& row = st->result.rows[r];
            if (static_cast<size_t>(c) < row.size())
                w = (std::max)(w, measureWidth(st->fmtCell, row[static_cast<size_t>(c)]));
        }
        int cw = static_cast<int>(w) + pad;
        cw = (std::max)(minW, (std::min)(maxW, cw));
        st->colWidth[static_cast<size_t>(c)] = cw;
    }
}

// ---- brushes / device resources --------------------------------------------

void releaseBrushes(GridState* st) {
    SafeRelease(st->brText);
    SafeRelease(st->brHeaderText);
    SafeRelease(st->brAccent);
    SafeRelease(st->brPanel);
    SafeRelease(st->brAlt);
    SafeRelease(st->brHeaderBg);
    SafeRelease(st->brSelBg);
    SafeRelease(st->brSelText);
    SafeRelease(st->brBorder);
}

bool createBrushes(GridState* st) {
    releaseBrushes(st);
    if (!st->rt) return false;
    const Theme& th = currentTheme();
    bool ok = true;
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.textPrimary), &st->brText));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.textSecondary), &st->brHeaderText));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.accent), &st->brAccent));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.panelBg), &st->brPanel));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.altRowBg), &st->brAlt));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.panelElevBg), &st->brHeaderBg));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.selectionBg), &st->brSelBg));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.selectionText), &st->brSelText));
    ok &= SUCCEEDED(st->rt->CreateSolidColorBrush(colorToD2D(th.border), &st->brBorder));
    return ok;
}

void discardDeviceResources(GridState* st) {
    releaseBrushes(st);
    SafeRelease(st->rt);
}

bool ensureDeviceResources(GridState* st) {
    if (!d2dFactory() || !dwriteFactory()) return false;
    ensureFormats(st);
    if (st->rt) return true;
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>((std::max<LONG>)(1, rc.right - rc.left)),
                                         static_cast<UINT32>((std::max<LONG>)(1, rc.bottom - rc.top)));
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(st->hwnd, size);
    if (FAILED(d2dFactory()->CreateHwndRenderTarget(props, hp, &st->rt)) || !st->rt) {
        SafeRelease(st->rt);
        return false;
    }
    st->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    if (!createBrushes(st)) {
        discardDeviceResources(st);
        return false;
    }
    return true;
}

// ---- scrolling -------------------------------------------------------------

void clampScroll(GridState* st) {
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int viewW = rc.right, viewH = rc.bottom;
    const int bodyH = rowCount(st) * st->rowH;
    const int maxY = (std::max)(0, bodyH - (viewH - st->headerH));
    const int maxX = (std::max)(0, totalColumnsWidth(st) - viewW);
    st->scrollX = (std::max)(0, (std::min)(st->scrollX, maxX));
    st->scrollY = (std::max)(0, (std::min)(st->scrollY, maxY));
}

void updateScrollbars(GridState* st) {
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int viewW = rc.right, viewH = rc.bottom;
    SCROLLINFO v{};
    v.cbSize = sizeof(v);
    v.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    v.nMin = 0;
    v.nMax = (std::max)(0, rowCount(st) * st->rowH - 1);
    v.nPage = static_cast<UINT>((std::max)(0, viewH - st->headerH));
    v.nPos = st->scrollY;
    SetScrollInfo(st->hwnd, SB_VERT, &v, TRUE);
    SCROLLINFO h{};
    h.cbSize = sizeof(h);
    h.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    h.nMin = 0;
    h.nMax = (std::max)(0, totalColumnsWidth(st) - 1);
    h.nPage = static_cast<UINT>((std::max)(0, viewW));
    h.nPos = st->scrollX;
    SetScrollInfo(st->hwnd, SB_HORZ, &h, TRUE);
}

void ensureRowVisible(GridState* st, int displayRow) {
    if (displayRow < 0 || st->rowH <= 0) return;
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int viewBody = rc.bottom - st->headerH;
    const int top = displayRow * st->rowH;
    const int bot = top + st->rowH;
    if (top < st->scrollY)
        st->scrollY = top;
    else if (bot > st->scrollY + viewBody)
        st->scrollY = bot - viewBody;
    clampScroll(st);
}

// ---- painting --------------------------------------------------------------

void paint(GridState* st) {
    PAINTSTRUCT ps;
    BeginPaint(st->hwnd, &ps);
    if (ensureDeviceResources(st)) {
        clampScroll(st);
        updateScrollbars(st);
        RECT rc;
        GetClientRect(st->hwnd, &rc);
        const int viewW = rc.right, viewH = rc.bottom;
        const int nc = columnCount(st), nr = rowCount(st);
        const Theme& th = currentTheme();

        st->rt->BeginDraw();
        st->rt->Clear(colorToD2D(th.panelBg));

        if (nc > 0 && st->fmtCell && st->fmtHeader && st->brText) {
            // Body: only visible rows.
            const int first = (st->rowH > 0) ? st->scrollY / st->rowH : 0;
            for (int r = (std::max)(0, first); r < nr; ++r) {
                const int y0 = st->headerH + r * st->rowH - st->scrollY;
                if (y0 >= viewH) break;
                const int y1 = y0 + st->rowH;
                ID2D1SolidColorBrush* bg =
                    (r == st->selectedRow) ? st->brSelBg : ((r & 1) ? st->brAlt : st->brPanel);
                ID2D1SolidColorBrush* fg = (r == st->selectedRow) ? st->brSelText : st->brText;
                int rowRight = totalColumnsWidth(st) - st->scrollX;
                if (rowRight > viewW) rowRight = viewW;
                st->rt->FillRectangle(
                    D2D1::RectF(0, static_cast<float>(y0), static_cast<float>(rowRight),
                                static_cast<float>(y1)),
                    bg);
                for (int c = 0; c < nc; ++c) {
                    const int cx0 = columnLeft(st, c) - st->scrollX;
                    const int cx1 = cx0 + st->colWidth[static_cast<size_t>(c)];
                    if (cx1 <= 0) continue;
                    if (cx0 >= viewW) break;
                    const std::wstring& v = cellAt(st, r, c);
                    if (v.empty()) continue;
                    const D2D1_RECT_F tr =
                        D2D1::RectF(static_cast<float>(cx0 + dpx(st, kCellPadX)),
                                    static_cast<float>(y0),
                                    static_cast<float>(cx1 - dpx(st, kCellPadX)),
                                    static_cast<float>(y1));
                    st->rt->DrawText(v.c_str(), static_cast<UINT32>(v.size()), st->fmtCell, tr, fg,
                                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }

            // Header band (drawn over the body's top).
            st->rt->FillRectangle(
                D2D1::RectF(0, 0, static_cast<float>(viewW), static_cast<float>(st->headerH)),
                st->brHeaderBg);
            for (int c = 0; c < nc; ++c) {
                const int cx0 = columnLeft(st, c) - st->scrollX;
                const int cx1 = cx0 + st->colWidth[static_cast<size_t>(c)];
                if (cx1 <= 0) continue;
                if (cx0 >= viewW) break;
                const bool sorted = (c == st->sortColumn);
                ID2D1SolidColorBrush* htf = sorted ? st->brAccent : st->brHeaderText;
                // Always reserve the arrow gap so the title width doesn't shift on sort.
                const float textRight =
                    static_cast<float>(cx1 - dpx(st, kHeaderPadX) - dpx(st, kArrowZone));
                const std::wstring& nm = st->result.columns[static_cast<size_t>(c)];
                st->rt->DrawText(nm.c_str(), static_cast<UINT32>(nm.size()), st->fmtHeader,
                                 D2D1::RectF(static_cast<float>(cx0 + dpx(st, kHeaderPadX)), 0,
                                             textRight, static_cast<float>(st->headerH)),
                                 htf, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                if (sorted) {
                    const wchar_t* arrow = st->sortAscending ? L"▲" : L"▼";
                    st->rt->DrawText(arrow, 1, st->fmtHeader,
                                     D2D1::RectF(static_cast<float>(cx1 - dpx(st, kArrowZone)), 0,
                                                 static_cast<float>(cx1 - dpx(st, 4)),
                                                 static_cast<float>(st->headerH)),
                                     st->brAccent, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
            // Header bottom hairline.
            st->rt->FillRectangle(
                D2D1::RectF(0, static_cast<float>(st->headerH - 1), static_cast<float>(viewW),
                            static_cast<float>(st->headerH)),
                st->brBorder);
        }

        if (st->rt->EndDraw() == D2DERR_RECREATE_TARGET) discardDeviceResources(st);
    }
    EndPaint(st->hwnd, &ps);
}

// ---- actions ---------------------------------------------------------------

// Rebuild rowOrder = (current filter) applied over (current sort, or identity).
void applyFilterSort(GridState* st) {
    std::vector<size_t> base;
    if (st->sortColumn >= 0) {
        base = sortedRowOrder(st->result.rows, static_cast<size_t>(st->sortColumn), st->sortAscending);
    } else {
        base.resize(st->result.rows.size());
        for (size_t i = 0; i < base.size(); ++i) base[i] = i;
    }
    if (st->filterLower.empty()) {
        st->rowOrder = std::move(base);
    } else {
        st->rowOrder.clear();
        st->rowOrder.reserve(base.size());
        for (size_t src : base)
            if (rowMatchesFilter(st->result.rows[src], st->filterLower)) st->rowOrder.push_back(src);
    }
}

void sortByColumn(GridState* st, int col) {
    if (col < 0 || col >= columnCount(st)) return;
    if (col == st->sortColumn)
        st->sortAscending = !st->sortAscending;
    else {
        st->sortColumn = col;
        st->sortAscending = true;
    }
    applyFilterSort(st);
    st->selectedRow = -1;  // a display index would point at a different row after reordering
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

void copyTextToClipboard(GridState* st, const std::wstring& text) {
    if (!OpenClipboard(st->hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(h)) {
            std::memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

void showContextMenu(GridState* st, int screenX, int screenY) {
    const HWND grid = st->hwnd;  // stable handle; the modal menu loop may outlive `st`
    HMENU pm = CreatePopupMenu();
    if (!pm) return;
    AppendMenuW(pm, MF_STRING, CMD_VIEW, L"View value…");
    AppendMenuW(pm, MF_STRING, CMD_COPYVAL, L"Copy value");
    AppendMenuW(pm, MF_STRING, CMD_COPYROW, L"Copy row");
    AppendMenuW(pm, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(pm, MF_STRING, CMD_TSV, L"Copy all as TSV");
    AppendMenuW(pm, MF_STRING, CMD_CSV, L"Copy all as CSV");
    const int cmd = static_cast<int>(
        TrackPopupMenu(pm, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenX, screenY, 0, grid, nullptr));
    DestroyMenu(pm);
    st = state(grid);  // re-resolve: the menu's modal loop could have destroyed the control
    if (!st) return;
    switch (cmd) {
        case CMD_VIEW:
            showCellDetail(GetParent(grid), columnNameAt(st, st->ctxCol),
                           cellAt(st, st->ctxRow, st->ctxCol));
            break;
        case CMD_COPYVAL:
            copyTextToClipboard(st, cellAt(st, st->ctxRow, st->ctxCol));
            break;
        case CMD_COPYROW: {
            std::wstring line;
            for (int c = 0; c < columnCount(st); ++c) {
                if (c) line += L'\t';
                line += cellAt(st, st->ctxRow, c);
            }
            copyTextToClipboard(st, line);
            break;
        }
        case CMD_TSV:
            copyTextToClipboard(st, buildTsv(st->result.columns, displayRows(st)));
            break;
        case CMD_CSV:
            copyTextToClipboard(st, buildCsv(st->result.columns, displayRows(st)));
            break;
        default: break;
    }
}

// ---- input -----------------------------------------------------------------

void onKeyDown(GridState* st, WPARAM vk) {
    const int nr = rowCount(st);
    if (nr == 0) return;
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int viewBody = static_cast<int>(rc.bottom) - st->headerH;
    const int page = (std::max)(1, viewBody / (std::max)(1, st->rowH) - 1);
    int sel = st->selectedRow;
    switch (vk) {
        case VK_UP: sel = (sel < 0) ? 0 : (std::max)(0, sel - 1); break;
        case VK_DOWN: sel = (sel < 0) ? 0 : (std::min)(nr - 1, sel + 1); break;
        case VK_PRIOR: sel = (sel < 0) ? 0 : (std::max)(0, sel - page); break;
        case VK_NEXT: sel = (sel < 0) ? 0 : (std::min)(nr - 1, sel + page); break;
        case VK_HOME: sel = 0; break;
        case VK_END: sel = nr - 1; break;
        case VK_LEFT: st->scrollX -= dpx(st, 48); clampScroll(st); InvalidateRect(st->hwnd, nullptr, FALSE); return;
        case VK_RIGHT: st->scrollX += dpx(st, 48); clampScroll(st); InvalidateRect(st->hwnd, nullptr, FALSE); return;
        default: return;
    }
    st->selectedRow = sel;
    ensureRowVisible(st, sel);
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

// Abort an in-progress column-resize drag. Called when the data is swapped out
// from under it (a worker-posted WM_APP_RESULT can run gridSetResult/gridClear
// mid-drag, shrinking colWidth — the message pump dispatches it even while the
// mouse is captured).
void cancelResize(GridState* st) {
    if (st->resizing) {
        st->resizing = false;
        st->resizeCol = -1;
        if (GetCapture() == st->hwnd) ReleaseCapture();
    }
}

LRESULT CALLBACK SqlGridProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* st = new (std::nothrow) GridState();
        if (!st) return FALSE;
        st->hwnd = hwnd;
        const UINT d = GetDpiForWindow(hwnd);
        st->dpi = d ? d : 96;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    GridState* st = state(hwnd);
    if (!st) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_PAINT: paint(st); return 0;
        case WM_ERASEBKGND: return 1;
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
            clampScroll(st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_GETDLGCODE: return DLGC_WANTARROWS;
        case WM_KEYDOWN:
            onKeyDown(st, wParam);
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            const int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
            if (my < st->headerH) {
                const int dc = dividerAtX(st, mx);
                if (dc >= 0) {
                    st->resizing = true;
                    st->resizeCol = dc;
                    st->resizeStartX = mx;
                    st->resizeStartW = st->colWidth[static_cast<size_t>(dc)];
                    SetCapture(hwnd);
                } else {
                    const int c = columnAtX(st, mx);
                    if (c >= 0) sortByColumn(st, c);
                }
            } else {
                const int r = rowAtY(st, my);
                if (r >= 0) st->selectedRow = r;  // keep selection on a dead-space click
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            const int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
            if (my >= st->headerH) {  // double-click a cell -> open its detail
                const int r = rowAtY(st, my), c = columnAtX(st, mx);
                if (r >= 0 && c >= 0) {
                    st->selectedRow = r;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    showCellDetail(GetParent(hwnd), columnNameAt(st, c), cellAt(st, r, c));
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (st->resizing && (wParam & MK_LBUTTON) && st->resizeCol >= 0 &&
                static_cast<size_t>(st->resizeCol) < st->colWidth.size()) {
                const int mx = GET_X_LPARAM(lParam);
                int nw = st->resizeStartW + (mx - st->resizeStartX);
                nw = (std::max)(dpx(st, kMinColW), nw);  // no upper cap — drag as wide as needed
                st->colWidth[static_cast<size_t>(st->resizeCol)] = nw;
                clampScroll(st);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (st->resizing) {
                st->resizing = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                updateScrollbars(st);
            }
            return 0;
        case WM_CAPTURECHANGED:
            st->resizing = false;
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (pt.y < st->headerH && dividerAtX(st, pt.x) >= 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_CONTEXTMENU: {
            if (columnCount(st) == 0) return 0;
            POINT scr{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int r, c;
            if (scr.x == -1 && scr.y == -1) {  // keyboard (Shift+F10 / menu key)
                r = (st->selectedRow >= 0) ? st->selectedRow : 0;
                c = 0;
                RECT rc;
                GetClientRect(hwnd, &rc);
                POINT cli{dpx(st, 12), st->headerH + r * st->rowH - st->scrollY + st->rowH / 2};
                cli.y = (std::min)((std::max)(cli.y, static_cast<LONG>(st->headerH)), rc.bottom);
                scr = cli;
                ClientToScreen(hwnd, &scr);
            } else {
                POINT cli = scr;
                ScreenToClient(hwnd, &cli);
                r = rowAtY(st, cli.y);
                c = columnAtX(st, cli.x);
                if (r < 0) r = st->selectedRow;
                if (c < 0) c = 0;
            }
            if (r < 0) return 0;
            st->ctxRow = r;
            st->ctxCol = c;
            st->selectedRow = r;
            InvalidateRect(hwnd, nullptr, FALSE);
            showContextMenu(st, scr.x, scr.y);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            st->scrollY -= (delta / WHEEL_DELTA) * 3 * (std::max)(1, st->rowH);
            clampScroll(st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEHWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            st->scrollX += (delta / WHEEL_DELTA) * dpx(st, 48);
            clampScroll(st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int pos = si.nPos;
            switch (LOWORD(wParam)) {
                case SB_LINEUP: pos -= st->rowH; break;
                case SB_LINEDOWN: pos += st->rowH; break;
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
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_HSCROLL: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_HORZ, &si);
            int pos = si.nPos;
            switch (LOWORD(wParam)) {
                case SB_LINEUP: pos -= dpx(st, 48); break;
                case SB_LINEDOWN: pos += dpx(st, 48); break;
                case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                default: break;
            }
            st->scrollX = pos;
            clampScroll(st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_NCDESTROY:
            discardDeviceResources(st);
            SafeRelease(st->fmtCell);
            SafeRelease(st->fmtHeader);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        default: return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

// ---- public API ------------------------------------------------------------

bool registerSqlGridClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = SqlGridProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // painted entirely via Direct2D
    wc.lpszClassName = L"SqlD2DGrid";
    if (!RegisterClassExW(&wc)) return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return true;
}

void gridSetResult(HWND grid, const QueryResult& result) {
    GridState* st = state(grid);
    if (!st) return;
    cancelResize(st);  // a query result can be posted while a column drag is in progress
    ensureFormats(st);
    st->result = result;
    st->filterLower.clear();
    st->sortColumn = -1;
    st->sortAscending = true;
    applyFilterSort(st);  // identity order (no sort, no filter)
    st->selectedRow = -1;
    st->scrollX = st->scrollY = 0;
    autoSizeColumns(st);
    clampScroll(st);
    updateScrollbars(st);
    InvalidateRect(grid, nullptr, FALSE);
}

void gridClear(HWND grid) {
    GridState* st = state(grid);
    if (!st) return;
    cancelResize(st);
    st->result = QueryResult{};
    st->rowOrder.clear();
    st->colWidth.clear();
    st->filterLower.clear();
    st->sortColumn = -1;
    st->sortAscending = true;
    st->selectedRow = -1;
    st->ctxRow = st->ctxCol = -1;
    st->scrollX = st->scrollY = 0;
    updateScrollbars(st);
    InvalidateRect(grid, nullptr, FALSE);
}

void gridSetFilter(HWND grid, const std::wstring& text) {
    GridState* st = state(grid);
    if (!st) return;
    std::wstring lower = text;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    if (lower == st->filterLower) return;
    st->filterLower = std::move(lower);
    applyFilterSort(st);
    st->selectedRow = -1;
    st->scrollY = 0;  // show the top of the filtered set
    clampScroll(st);
    updateScrollbars(st);
    InvalidateRect(grid, nullptr, FALSE);
}

void gridGetCounts(HWND grid, int& shown, int& total) {
    GridState* st = state(grid);
    if (!st) {
        shown = total = 0;
        return;
    }
    shown = static_cast<int>(st->rowOrder.size());
    total = static_cast<int>(st->result.rows.size());
}

void gridApplyTheme(HWND grid) {
    GridState* st = state(grid);
    if (!st) return;
    SetWindowTheme(grid, currentTheme().dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    if (st->rt) {
        if (!createBrushes(st)) discardDeviceResources(st);
    }
    InvalidateRect(grid, nullptr, FALSE);
}

void gridUpdateDpi(HWND grid, UINT dpi) {
    GridState* st = state(grid);
    if (!st) return;
    const UINT old = st->dpi;
    st->dpi = dpi ? dpi : 96;
    if (old > 0 && old != st->dpi)
        for (int& w : st->colWidth) w = MulDiv(w, static_cast<int>(st->dpi), static_cast<int>(old));
    SafeRelease(st->fmtCell);
    SafeRelease(st->fmtHeader);
    ensureFormats(st);
    clampScroll(st);
    updateScrollbars(st);
    InvalidateRect(grid, nullptr, FALSE);
}

}  // namespace sqlterm
