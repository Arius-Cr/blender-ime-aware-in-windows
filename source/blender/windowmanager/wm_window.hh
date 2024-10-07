/* SPDX-FileCopyrightText: 2007 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#pragma once

#include "BLI_compiler_attrs.h"

struct bContext;
struct Main;
struct PointerRNA;
struct wmOperator;
struct wmOperatorType;
struct wmWindow;
struct wmWindowManager;

/* *************** internal api ************** */

/**
 * \note #bContext can be null in background mode because we don't
 * need to event handling.
 */
void wm_ghost_init(bContext *C);
void wm_ghost_init_background();
void wm_ghost_exit();

void wm_clipboard_free();

/**
 * This one should correctly check for apple top header...
 * done for Cocoa: returns window contents (and not frame) max size.
 * \return true on success.
 */
bool wm_get_screensize(int r_size[2]) ATTR_NONNULL(1) ATTR_WARN_UNUSED_RESULT;
/**
 * Size of all screens (desktop), useful since the mouse is bound by this.
 * \return true on success.
 */
bool wm_get_desktopsize(int r_size[2]) ATTR_NONNULL(1) ATTR_WARN_UNUSED_RESULT;

/**
 * Don't change context itself.
 */
wmWindow *wm_window_new(const Main *bmain, wmWindowManager *wm, wmWindow *parent, bool dialog);
/**
 * Part of `wm_window.cc` API.
 */
wmWindow *wm_window_copy(
    Main *bmain, wmWindowManager *wm, wmWindow *win_src, bool duplicate_layout, bool child);
/**
 * A higher level version of copy that tests the new window can be added.
 * (called from the operator directly).
 */
wmWindow *wm_window_copy_test(bContext *C, wmWindow *win_src, bool duplicate_layout, bool child);
/**
 * Including window itself.
 * \param C: can be NULL.
 * \note #ED_screen_exit should have been called.
 */
void wm_window_free(bContext *C, wmWindowManager *wm, wmWindow *win);
/**
 * This is event from ghost, or exit-Blender operator.
 */
void wm_window_close(bContext *C, wmWindowManager *wm, wmWindow *win);

/**
 * Initialize #wmWindow without `ghostwin`, open these and clear.
 *
 * Window size is read from window, if 0 it uses prefsize
 * called in #WM_check, also initialize stuff after file read.
 *
 * \warning After running, `win->ghostwin` can be NULL in rare cases
 * (where OpenGL driver fails to create a context for eg).
 * We could remove them with #wm_window_ghostwindows_remove_invalid
 * but better not since caller may continue to use.
 * Instead, caller needs to handle the error case and cleanup.
 */
void wm_window_ghostwindows_ensure(wmWindowManager *wm);
/**
 * Call after #wm_window_ghostwindows_ensure or #WM_check
 * (after loading a new file) in the unlikely event a window couldn't be created.
 */
void wm_window_ghostwindows_remove_invalid(bContext *C, wmWindowManager *wm);
void wm_window_events_process(const bContext *C);

void wm_window_clear_drawable(wmWindowManager *wm);
void wm_window_make_drawable(wmWindowManager *wm, wmWindow *win);
/**
 * Reset active the current window gpu drawing context.
 */
void wm_window_reset_drawable();

void wm_window_raise(wmWindow *win);
void wm_window_lower(wmWindow *win);
void wm_window_set_size(wmWindow *win, int width, int height);
/**
 * \brief Push rendered buffer to the screen.
 */
void wm_window_swap_buffers(wmWindow *win);
void wm_window_set_swap_interval(wmWindow *win, int interval);
bool wm_window_get_swap_interval(wmWindow *win, int *r_interval);

bool wm_cursor_position_get(wmWindow *win, int *r_x, int *r_y) ATTR_WARN_UNUSED_RESULT;
void wm_cursor_position_from_ghost_screen_coords(wmWindow *win, int *x, int *y);
void wm_cursor_position_to_ghost_screen_coords(wmWindow *win, int *x, int *y);

void wm_cursor_position_from_ghost_client_coords(wmWindow *win, int *x, int *y);
void wm_cursor_position_to_ghost_client_coords(wmWindow *win, int *x, int *y);

#if defined(WITH_INPUT_IME) && !defined(WIN32)
void wm_window_IME_begin(wmWindow *win, int x, int y, int w, int h, bool complete);
void wm_window_IME_end(wmWindow *win);
#endif

#if defined(WITH_INPUT_IME) && defined(WIN32)
void wm_window_IME_begin(wmWindow *win, wmIMEInvoker invoker);
void wm_window_IME_end(wmWindow *win);
bool wm_window_IME_is_enable(wmWindow *win);
wmIMEInvoker wm_window_IME_get_invoker(wmWindow *win);
bool wm_window_IME_is_composing(wmWindow *win);
void wm_window_IME_complete(wmWindow *win);
void wm_window_IME_cancel(wmWindow *win);
/**
 * Move the IME (Conversion) Candidate Window.
 * \param windowhandle: The wmWindow of the caller.
 * \param c_l: The left of the caret in wmWindow coordinates.
 * \param c_b: The bottom of the caret in wmWindow coordinates.
 * \param c_w: The width of the caret.
 * \param c_h: The height of the caret.
 */
void wm_window_IME_move(wmWindow *win, int c_l, int c_b, int c_w, int c_h);
/**
 * Move the IME (Conversion) Candidate Window.
 * \param windowhandle: The wmWindow of the caller.
 * \param c_l: The left of the caret in wmWindow coordinates.
 * \param c_b: The top of the caret in wmWindow coordinates.
 * \param c_w: The width of the caret.
 * \param c_h: The height of the caret.
 * \param e_l: The left of the exclude rectangle in wmWindow coordinates.
 * \param e_t: The top of the exclude rectangle in wmWindow coordinates.
 * \param e_w: The width of the exclude rectangle.
 * \param e_h: The height of the exclude rectangle.
 */
void wm_window_IME_move_with_exclude(
    wmWindow *win, int c_l, int c_b, int c_w, int c_h, int e_l, int e_b, int e_w, int e_h);
void wm_window_IME_start_composition_by_char(wmWindow *win, char c);
#endif

/**
 * Effectively remove timers from the list and delete them. Calling this should only be done by
 * internal WM management code, from specific, safe places.
 */
void wm_window_timers_delete_removed(wmWindowManager *wm);

/* *************** window operators ************** */

int wm_window_close_exec(bContext *C, wmOperator *op);
/**
 * Full-screen operator callback.
 */
int wm_window_fullscreen_toggle_exec(bContext *C, wmOperator *op);
/**
 * Call the quit confirmation prompt or exit directly if needed. The use can
 * still cancel via the confirmation popup. Also, this may not quit Blender
 * immediately, but rather schedule the closing.
 *
 * \param win: The window to show the confirmation popup/window in.
 */
void wm_quit_with_optional_confirmation_prompt(bContext *C, wmWindow *win) ATTR_NONNULL();

int wm_window_new_exec(bContext *C, wmOperator *op);
int wm_window_new_main_exec(bContext *C, wmOperator *op);

void wm_test_autorun_revert_action_set(wmOperatorType *ot, PointerRNA *ptr);
void wm_test_autorun_warning(bContext *C);
