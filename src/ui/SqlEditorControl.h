// SPDX-License-Identifier: GPL-3.0-or-later
// SqlEditorControl — a GPU-accelerated (Direct2D + DirectWrite) SQL editor that
// replaces the RichEdit 4.1 control. It is a real child window (custom WNDCLASS
// "SqlD2DEditor") so the existing layout()/SetFocus/DeferWindowPos wiring in
// MainWindow.cpp is unchanged, and it implements WM_SETTEXT/WM_GETTEXT so
// SetWindowTextW()/GetWindowTextW() keep working from the existing call sites.
//
// The text/edit logic lives in the pure, unit-tested editor::EditorModel (see
// src/editor/); this file is the thin Win32 + Direct2D view: rendering, input,
// caret/selection geometry (via DirectWrite hit-testing), scrolling, and live
// syntax coloring (reusing sqlcore::SqlSyntaxHighlighter). Colors come from
// Theme.h, so a light/dark toggle is just a brush rebuild + repaint.
//
// Rendering uses an ID2D1HwndRenderTarget (no DirectComposition / swap chain):
// GPU-accelerated, robust on a constantly-resized child window, and the
// resize-reflow lag that motivated this work is gone because we own the
// DirectWrite layout and only re-wrap when the width actually changes.
#pragma once

#include <windows.h>

#include <string>

namespace sqlterm {

// Register the "SqlD2DEditor" window class once (call in place of loading
// Msftedit.dll). Returns false only if RegisterClassEx fails.
bool registerSqlEditorClass(HINSTANCE hInst);

// Full editor text, always with '\n' line breaks (UTF-16 code units, aligned
// with SqlCore). Replaces the old RichEdit-based editorText() helper.
std::wstring editorText(HWND edit);

// Caret offset = start (cpMin) of the current selection, as a UTF-16 offset.
LONG caretOffset(HWND edit);

// Re-read currentTheme(): rebuild brushes + scrollbar theme and repaint. Call
// from reapplyTheme() on a light/dark toggle.
void editorApplyTheme(HWND edit);

// Rebuild the DirectWrite text format at the new DPI and repaint. Call from the
// main window's WM_DPICHANGED handler.
void editorUpdateDpi(HWND edit, UINT dpi);

}  // namespace sqlterm
