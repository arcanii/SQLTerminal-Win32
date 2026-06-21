// SPDX-License-Identifier: GPL-3.0-or-later
// EditorModel.h — the pure, platform-free text model behind the GPU SQL editor.
//
// This is the unit-tested core of the Direct2D/DirectWrite editor that replaces
// RichEdit (see src/ui/SqlEditorControl.*). It owns the text buffer, the caret
// and selection, all edit operations, *logical* (wrap-independent) navigation,
// and an undo/redo stack. It deliberately knows nothing about Windows, Direct2D,
// or visual layout, so the bulk of the editor's behavior can be verified by
// ctest (EditorCoreTests) without a screen.
//
// Index space: UTF-16 code units (std::wstring / wchar_t), matching SqlCore and
// the highlighter so the editor's offsets line up byte-for-byte with
// SqlSyntaxHighlighter / SqlStatementSplitter. The buffer always uses single
// '\n' line breaks (set/paste normalize '\r\n' and lone '\r' to '\n'), which is
// exactly what editorText() handed to SqlCore before.
//
// What lives elsewhere (the view): up/down/page-up/down navigation is *visual*
// (it depends on word wrap) and is computed by the control via DirectWrite
// hit-testing, then applied here through setCaret(offset, extend).
#pragma once

#include <string>
#include <vector>

namespace editor {

// A selection is a half-open range [min, max) in UTF-16 code units. `anchor` is
// where the selection started; `caret` is the moving end where the caret blinks.
// An empty selection (anchor == caret) is a bare caret.
struct Selection {
    size_t anchor = 0;
    size_t caret = 0;

    size_t min() const { return anchor < caret ? anchor : caret; }
    size_t max() const { return anchor < caret ? caret : anchor; }
    bool empty() const { return anchor == caret; }

    bool operator==(const Selection& o) const {
        return anchor == o.anchor && caret == o.caret;
    }
    bool operator!=(const Selection& o) const { return !(*this == o); }
};

// Convert '\r\n' and lone '\r' to '\n'. Free function so it can be unit-tested
// directly and reused by the control's paste path.
std::wstring normalizeNewlines(const std::wstring& s);

class EditorModel {
public:
    EditorModel() = default;

    // ---- text access -------------------------------------------------------
    // Always '\n' line breaks; safe to hand straight to the highlighter / DWrite.
    const std::wstring& text() const { return text_; }
    size_t length() const { return text_.size(); }

    // Hard reset (initial text, history/snippet/schema insert, clear-after-run).
    // Normalizes newlines, places the caret at the end, and clears undo history
    // (matches RichEdit's WM_SETTEXT, which discards the undo buffer).
    void setText(const std::wstring& s);

    // ---- selection / caret -------------------------------------------------
    Selection selection() const { return sel_; }
    size_t caret() const { return sel_.caret; }
    // Move the caret to `off` (clamped, snapped off surrogate boundaries). When
    // `extend` is true the anchor is kept (Shift+navigation); otherwise the
    // selection collapses to the caret.
    void setCaret(size_t off, bool extend);
    void setSelection(size_t anchor, size_t caret);
    void selectAll();

    // ---- editing -----------------------------------------------------------
    void insertText(const std::wstring& s);  // replaces the selection if any
    void backspace();                        // delete selection, else codepoint left
    void deleteForward();                    // delete selection, else codepoint right
    void deleteSelection();                  // no-op when empty
    void deleteWordLeft();                   // Ctrl+Backspace
    void deleteWordRight();                  // Ctrl+Delete

    // ---- logical navigation (wrap-independent; what the WNDPROC calls) ------
    void moveLeft(bool extend, bool byWord);
    void moveRight(bool extend, bool byWord);
    void moveLineHome(bool extend);
    void moveLineEnd(bool extend);
    void moveDocStart(bool extend);
    void moveDocEnd(bool extend);

    // ---- offset helpers (const; reused by the view) ------------------------
    size_t stepCodepoint(size_t off, int dir) const;  // ±1 codepoint, pair-aware
    size_t lineStart(size_t off) const;               // after the previous '\n'
    size_t lineEnd(size_t off) const;                 // at the next '\n' or end
    size_t wordLeft(size_t off) const;                // Ctrl+Left target
    size_t wordRight(size_t off) const;               // Ctrl+Right target
    size_t docStart() const { return 0; }
    size_t docEnd() const { return text_.size(); }

    // ---- undo / redo -------------------------------------------------------
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void undo();  // restores text AND selection
    void redo();

private:
    struct Snapshot {
        std::wstring text;
        Selection sel;
    };

    size_t clamp(size_t off) const { return off > text_.size() ? text_.size() : off; }
    size_t snap(size_t off) const;  // never land between a surrogate pair

    // Push the current state as an undo step before mutating. When `extendRun`
    // is true and a typing run is already open, the edit coalesces into the
    // current step (no new snapshot). Always clears the redo stack.
    void recordPreEdit(bool extendRun);

    std::wstring text_;
    Selection sel_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    bool typingRun_ = false;  // a run of coalescing single-char inserts is open
};

}  // namespace editor
