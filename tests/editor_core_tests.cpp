// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for editor::EditorModel — the pure text model behind the Direct2D
// SQL editor. The model is the build-blind confidence backbone: everything that
// can be verified without a screen (offset math, edits, navigation, word
// boundaries, surrogate pairs, undo/redo) is pinned here.
#include <string>

#include "editor/EditorModel.h"
#include "test_harness.h"

using editor::EditorModel;
using editor::Selection;
using editor::normalizeNewlines;
using std::wstring;

// ---- dbg() overloads (used by CHECK_EQ to print mismatches) -----------------

[[maybe_unused]] static std::string dbg(bool b) { return b ? "true" : "false"; }
[[maybe_unused]] static std::string dbg(unsigned long long v) { return std::to_string(v); }
[[maybe_unused]] static std::string dbg(const wstring& w) { return "\"" + th::narrow(w) + "\""; }
[[maybe_unused]] static std::string dbg(const Selection& s) {
    return "{anchor=" + std::to_string(s.anchor) + ", caret=" + std::to_string(s.caret) + "}";
}

// Shorthand: the model's text after a sequence of ops.
static const wstring& T(const EditorModel& m) { return m.text(); }

// ===== newline normalization ================================================

TEST(normalize_crlf) { CHECK_EQ(normalizeNewlines(L"a\r\nb"), wstring(L"a\nb")); }
TEST(normalize_lone_cr) { CHECK_EQ(normalizeNewlines(L"a\rb"), wstring(L"a\nb")); }
TEST(normalize_lf_unchanged) { CHECK_EQ(normalizeNewlines(L"a\nb"), wstring(L"a\nb")); }
TEST(normalize_double_crlf) { CHECK_EQ(normalizeNewlines(L"\r\n\r\n"), wstring(L"\n\n")); }
TEST(normalize_mixed) {
    CHECK_EQ(normalizeNewlines(L"a\r\nb\rc\nd"), wstring(L"a\nb\nc\nd"));
}
TEST(normalize_empty) { CHECK_EQ(normalizeNewlines(L""), wstring(L"")); }

TEST(set_text_normalizes_and_caret_to_end) {
    EditorModel m;
    m.setText(L"a\r\nb");
    CHECK_EQ(T(m), wstring(L"a\nb"));
    CHECK_EQ(m.length(), 3ull);
    CHECK_EQ(m.selection(), (Selection{3, 3}));
}

TEST(set_text_clears_undo) {
    EditorModel m;
    m.insertText(L"x");
    CHECK(m.canUndo());
    m.setText(L"zzz");
    CHECK(!m.canUndo());
    CHECK(!m.canRedo());
}

// ===== basic insertion / caret math =========================================

TEST(insert_into_empty) {
    EditorModel m;
    m.insertText(L"abc");
    CHECK_EQ(T(m), wstring(L"abc"));
    CHECK_EQ(m.selection(), (Selection{3, 3}));
}

TEST(insert_at_offset) {
    EditorModel m;
    m.setText(L"ac");
    m.setCaret(1, false);
    m.insertText(L"b");
    CHECK_EQ(T(m), wstring(L"abc"));
    CHECK_EQ(m.caret(), 2ull);
}

TEST(set_caret_clamps) {
    EditorModel m;
    m.setText(L"abc");
    m.setCaret(999, false);
    CHECK_EQ(m.caret(), 3ull);
}

TEST(insert_empty_is_noop) {
    EditorModel m;
    m.setText(L"abc");
    m.setCaret(1, false);
    m.insertText(L"");
    CHECK_EQ(T(m), wstring(L"abc"));
    CHECK_EQ(m.caret(), 1ull);
}

// ===== edits with selection =================================================

TEST(insert_replaces_selection) {
    EditorModel m;
    m.setText(L"abcd");
    m.setSelection(1, 3);  // "bc"
    m.insertText(L"XY");
    CHECK_EQ(T(m), wstring(L"aXYd"));
    CHECK_EQ(m.caret(), 3ull);
    CHECK(m.selection().empty());
}

TEST(backspace_at_start_is_noop) {
    EditorModel m;
    m.setText(L"abc");
    m.setCaret(0, false);
    m.backspace();
    CHECK_EQ(T(m), wstring(L"abc"));
}

TEST(backspace_deletes_char) {
    EditorModel m;
    m.setText(L"abc");
    m.setCaret(3, false);
    m.backspace();
    CHECK_EQ(T(m), wstring(L"ab"));
    CHECK_EQ(m.caret(), 2ull);
}

TEST(backspace_deletes_selection) {
    EditorModel m;
    m.setText(L"abcd");
    m.setSelection(1, 3);
    m.backspace();
    CHECK_EQ(T(m), wstring(L"ad"));
    CHECK_EQ(m.caret(), 1ull);
}

TEST(delete_forward_at_end_is_noop) {
    EditorModel m;
    m.setText(L"abc");
    m.setCaret(3, false);
    m.deleteForward();
    CHECK_EQ(T(m), wstring(L"abc"));
}

TEST(delete_forward_deletes_char) {
    EditorModel m;
    m.setText(L"abc");
    m.setCaret(0, false);
    m.deleteForward();
    CHECK_EQ(T(m), wstring(L"bc"));
    CHECK_EQ(m.caret(), 0ull);
}

TEST(delete_forward_deletes_selection) {
    EditorModel m;
    m.setText(L"abcd");
    m.setSelection(1, 3);
    m.deleteForward();
    CHECK_EQ(T(m), wstring(L"ad"));
    CHECK_EQ(m.caret(), 1ull);
}

// ===== logical navigation ===================================================

TEST(line_home_and_end) {
    EditorModel m;
    m.setText(L"ab\ncd\nef");  // a0 b1 \n2 c3 d4 \n5 e6 f7
    m.setCaret(4, false);
    m.moveLineHome(false);
    CHECK_EQ(m.caret(), 3ull);
    m.moveLineEnd(false);
    CHECK_EQ(m.caret(), 5ull);
}

TEST(line_home_idempotent_at_start) {
    EditorModel m;
    m.setText(L"abc");
    m.setCaret(0, false);
    m.moveLineHome(false);
    CHECK_EQ(m.caret(), 0ull);
}

TEST(doc_start_and_end) {
    EditorModel m;
    m.setText(L"ab\ncd");
    m.setCaret(3, false);
    m.moveDocStart(false);
    CHECK_EQ(m.caret(), 0ull);
    m.moveDocEnd(false);
    CHECK_EQ(m.caret(), 5ull);
}

TEST(line_end_on_last_line_without_newline) {
    EditorModel m;
    m.setText(L"ab\ncd");
    m.setCaret(3, false);
    m.moveLineEnd(false);
    CHECK_EQ(m.caret(), 5ull);  // end of "cd" == doc end
}

TEST(move_left_right_by_codepoint) {
    EditorModel m;
    m.setText(L"abc");
    m.setCaret(2, false);
    m.moveLeft(false, false);
    CHECK_EQ(m.caret(), 1ull);
    m.moveRight(false, false);
    CHECK_EQ(m.caret(), 2ull);
}

// ===== word navigation (must match sqlcore::text::isWordChar) ================

TEST(word_right_lands_at_next_word_start) {
    EditorModel m;
    m.setText(L"SELECT foo_bar");
    CHECK_EQ(m.wordRight(0), 7ull);  // past "SELECT" + the space
}

TEST(word_left_lands_at_word_start) {
    EditorModel m;
    m.setText(L"SELECT foo_bar");
    CHECK_EQ(m.wordLeft(14), 7ull);  // start of "foo_bar"
}

TEST(word_nav_treats_punctuation_as_its_own_class) {
    EditorModel m;
    m.setText(L"a = b");  // a0 sp1 =2 sp3 b4
    CHECK_EQ(m.wordRight(0), 2ull);  // "a" then ws -> at "="
    CHECK_EQ(m.wordLeft(5), 4ull);   // start of "b"
    CHECK_EQ(m.wordLeft(4), 2ull);   // from before "b": skip ws -> start of "="
}

TEST(move_by_word_extends_selection) {
    EditorModel m;
    m.setText(L"SELECT foo");
    m.setCaret(0, false);
    m.moveRight(true, true);  // Ctrl+Shift+Right
    CHECK_EQ(m.selection(), (Selection{0, 7}));
}

TEST(delete_word_left) {
    EditorModel m;
    m.setText(L"foo bar baz");
    m.setCaret(11, false);
    m.deleteWordLeft();
    CHECK_EQ(T(m), wstring(L"foo bar "));
    CHECK_EQ(m.caret(), 8ull);
}

TEST(delete_word_right) {
    EditorModel m;
    m.setText(L"foo bar baz");
    m.setCaret(0, false);
    m.deleteWordRight();
    CHECK_EQ(T(m), wstring(L"bar baz"));
    CHECK_EQ(m.caret(), 0ull);
}

TEST(delete_word_left_deletes_existing_selection) {
    EditorModel m;
    m.setText(L"foo bar");
    m.setSelection(0, 3);
    m.deleteWordLeft();  // selection present -> just delete it
    CHECK_EQ(T(m), wstring(L" bar"));
}

// ===== surrogate pairs (emoji = one codepoint, two UTF-16 units) ============
// L"a\U0001F600b": a(0) high(1) low(2) b(3), length 4.

TEST(surrogate_step_right_skips_pair) {
    EditorModel m;
    m.setText(L"a\U0001F600b");
    CHECK_EQ(m.length(), 4ull);
    CHECK_EQ(m.stepCodepoint(1, +1), 3ull);
}

TEST(surrogate_step_left_skips_pair) {
    EditorModel m;
    m.setText(L"a\U0001F600b");
    CHECK_EQ(m.stepCodepoint(3, -1), 1ull);
}

TEST(surrogate_backspace_removes_whole_pair) {
    EditorModel m;
    m.setText(L"a\U0001F600b");
    m.setCaret(3, false);
    m.backspace();
    CHECK_EQ(T(m), wstring(L"ab"));
    CHECK_EQ(m.caret(), 1ull);
}

TEST(surrogate_delete_forward_removes_whole_pair) {
    EditorModel m;
    m.setText(L"a\U0001F600b");
    m.setCaret(1, false);
    m.deleteForward();
    CHECK_EQ(T(m), wstring(L"ab"));
    CHECK_EQ(m.caret(), 1ull);
}

TEST(surrogate_set_caret_snaps_off_midpair) {
    EditorModel m;
    m.setText(L"a\U0001F600b");
    m.setCaret(2, false);  // mid-pair -> floor to pair start
    CHECK_EQ(m.caret(), 1ull);
}

// ===== selection semantics ==================================================

TEST(shift_right_builds_selection) {
    EditorModel m;
    m.setText(L"abcdef");
    m.setCaret(0, false);
    m.moveRight(true, false);
    m.moveRight(true, false);
    m.moveRight(true, false);
    CHECK_EQ(m.selection(), (Selection{0, 3}));
}

TEST(unshifted_left_collapses_to_min) {
    EditorModel m;
    m.setText(L"abcdef");
    m.setSelection(0, 3);
    m.moveLeft(false, false);
    CHECK_EQ(m.selection(), (Selection{0, 0}));
}

TEST(unshifted_right_collapses_to_max) {
    EditorModel m;
    m.setText(L"abcdef");
    m.setSelection(1, 4);
    m.moveRight(false, false);
    CHECK_EQ(m.selection(), (Selection{4, 4}));
}

TEST(select_all) {
    EditorModel m;
    m.setText(L"abc");
    m.selectAll();
    CHECK_EQ(m.selection(), (Selection{0, 3}));
}

// ===== undo / redo ==========================================================

TEST(undo_redo_typing_coalesces) {
    EditorModel m;
    m.insertText(L"a");
    m.insertText(L"b");
    m.insertText(L"c");
    CHECK_EQ(T(m), wstring(L"abc"));
    m.undo();
    CHECK_EQ(T(m), wstring(L""));  // one coalesced step
    m.redo();
    CHECK_EQ(T(m), wstring(L"abc"));
}

TEST(caret_move_breaks_typing_run) {
    EditorModel m;
    m.insertText(L"a");
    m.insertText(L"b");
    m.setCaret(0, false);
    m.insertText(L"c");  // "cab"
    CHECK_EQ(T(m), wstring(L"cab"));
    m.undo();
    CHECK_EQ(T(m), wstring(L"ab"));  // first the "c" step
    m.undo();
    CHECK_EQ(T(m), wstring(L""));  // then the "ab" step
}

TEST(undo_restores_selection) {
    EditorModel m;
    m.setText(L"hello");
    m.setSelection(1, 4);
    m.insertText(L"X");  // "hXo"
    CHECK_EQ(T(m), wstring(L"hXo"));
    m.undo();
    CHECK_EQ(T(m), wstring(L"hello"));
    CHECK_EQ(m.selection(), (Selection{1, 4}));
}

TEST(redo_cleared_after_new_edit) {
    EditorModel m;
    m.insertText(L"a");
    m.undo();
    CHECK(m.canRedo());
    m.insertText(L"b");
    CHECK(!m.canRedo());
    CHECK_EQ(T(m), wstring(L"b"));
}

TEST(undo_redo_flags_at_boundaries) {
    EditorModel m;
    CHECK(!m.canUndo());
    CHECK(!m.canRedo());
    m.undo();  // no-op
    CHECK(!m.canUndo());
    m.insertText(L"x");
    CHECK(m.canUndo());
}

TEST(delete_is_separate_undo_step_from_typing) {
    EditorModel m;
    m.insertText(L"ab");  // one typing step
    m.backspace();        // separate step -> "a"
    CHECK_EQ(T(m), wstring(L"a"));
    m.undo();
    CHECK_EQ(T(m), wstring(L"ab"));
    m.undo();
    CHECK_EQ(T(m), wstring(L""));
}

// ===== runner ================================================================

int main() {
    for (const auto& tc : th::registry()) {
        th::g_currentTest = tc.name;
        tc.fn();
    }
    const int passed = th::g_checks - th::g_failures;
    std::cout << "\n"
              << (th::g_failures == 0 ? "PASSED" : "FAILED") << ": " << passed << "/"
              << th::g_checks << " checks across " << th::registry().size() << " tests";
    if (th::g_failures) std::cout << "  (" << th::g_failures << " failed)";
    std::cout << "\n";
    return th::g_failures == 0 ? 0 : 1;
}
