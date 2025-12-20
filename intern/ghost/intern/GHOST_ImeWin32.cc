/* SPDX-FileCopyrightText: 2010 The Chromium Authors. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#ifdef WITH_INPUT_IME

#  include "GHOST_ImeWin32.hh"
#  include "GHOST_C-api.h"
#  include "GHOST_WindowWin32.hh"
#  include "utfconv.hh"

#  include "printx.h"

GHOST_ImeWin32::GHOST_ImeWin32()
    : caret_rect_(0, 0, 0, 0),
      exclude_rect_(0, 0, 0, 0),
      h_wnd_(nullptr),
      is_first_(true),
      is_enabled_(true),
      is_composing_(false)
{
}

GHOST_ImeWin32::~GHOST_ImeWin32() {}

void GHOST_ImeWin32::CheckFirst()
{
  if (is_first_) {
    is_first_ = false;

    debug_ime(CCFY "GHOST_ImeWin32::CheckFirst");

    /**
     * The IME is enabled by default, but we want it disabled at default,
     * because we application is not a Text Process Program.
     */
    EndIME();
  }
}

void GHOST_ImeWin32::OnWindowActivated()
{
  debug_ime(CCFY "GHOST_ImeWin32::OnWindowActivated");

  /* Ensure the system caret. */
  MoveIME();
}

void GHOST_ImeWin32::OnWindowDeactivated()
{
  debug_ime(CCFY "GHOST_ImeWin32::OnWindowDeactivated");

  /* WIN32 ignores this call if the system caret is not created.  */
  ::DestroyCaret();
}

LRESULT GHOST_ImeWin32::OnSetContext(UINT message, WPARAM wparam, LPARAM lparam)
{
  /* Sync IME state(enable/disable). */

  HIMC himc = ::ImmGetContext(h_wnd_);
  if (himc) {
    ::ImmReleaseContext(h_wnd_, himc);
    is_enabled_ = true;
  }
  else {
    is_enabled_ = false;
  }

  /**
   * To prevent the IMM (Input Method Manager) from displaying the IME
   * composition window, Update the styles of the IME windows and EXPLICITLY
   * call ::DefWindowProc() here.
   *
   * NOTE:
   *   It seems that the above-mentioned behavior has become invalid now.
   *   According to testing, if the return of WM_IME_STARTCOMPOSITION message is 0,
   *   the IME composition window will be hidden, otherwise, it will always display.
   *   To avoid potential issues, we keep this code here.
   */

  return ::DefWindowProcW(h_wnd_, message, wparam, lparam & ~ISC_SHOWUICOMPOSITIONWINDOW);
}

bool GHOST_ImeWin32::IsIgnoreKey(USHORT key)
{
  switch (key) {
    case VK_LWIN:
    case VK_RWIN:
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
      return true;
    default:
      return false;
  }
}

void GHOST_ImeWin32::BeginIME()
{
  debug_ime(CCFY "GHOST_ImeWin32::BeginIME");

  debug_ime(CCFY "  HWND: %p", h_wnd_);

  /**
   * Load the default IME context.
   *
   * NOTE:
   *   IMM ignores this call if the IME context is loaded. Therefore, we do
   *   not have to check whether or not the IME context is loaded.
   */
  ::ImmAssociateContextEx(h_wnd_, nullptr, IACE_DEFAULT);
}

void GHOST_ImeWin32::EndIME()
{
  debug_ime(CCFY "GHOST_ImeWin32::EndIME");

  HIMC himc = ::ImmGetContext(h_wnd_);

  if (himc) {
    ::ImmReleaseContext(h_wnd_, himc);

    debug_ime(CCFY "  HWND, HIMC: %p, %p", h_wnd_, himc);

    /**
     * Clean up the composition BEFORE DISABLING THE IME.
     *
     * IMM ignores this call if there is no ongoing composition.
     */
    CompleteComposition();

    ::ImmAssociateContextEx(h_wnd_, nullptr, 0);
  }

  /* Windows ignores this call if there is no system caret.  */
  ::DestroyCaret();
}

bool GHOST_ImeWin32::IsEnabled()
{
  debug_ime(CCFA "GHOST_ImeWin32::IsEnabled");
  return is_enabled_;
}

void GHOST_ImeWin32::OnCompositionStart()
{
  debug_ime(CCFY "GHOST_ImeWin32::OnCompositionStart");
  is_composing_ = true;
}

void GHOST_ImeWin32::OnCompositionUpdate(LPARAM lparam)
{
  debug_ime(CCFY "GHOST_ImeWin32::OnCompositionUpdate");
  UpdateInfo(lparam);
}

void GHOST_ImeWin32::OnCompositionEnd()
{
  debug_ime(CCFY "GHOST_ImeWin32::OnCompositionEnd");
  is_composing_ = false;
}

bool GHOST_ImeWin32::IsComposing()
{
  return is_composing_;
}

void GHOST_ImeWin32::CompleteComposition()
{
  if (is_composing_) {
    debug_ime(CCFY "CompleteComposition");
    HIMC himc = ::ImmGetContext(h_wnd_);
    if (himc) {
      ::ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
      ::ImmReleaseContext(h_wnd_, himc);
    }
  }
}

void GHOST_ImeWin32::CancelComposition()
{
  if (is_composing_) {
    debug_ime(CCFY "CancelComposition");
    HIMC himc = ::ImmGetContext(h_wnd_);
    if (himc) {
      ::ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
      ::ImmReleaseContext(h_wnd_, himc);
    }
  }
}

void GHOST_ImeWin32::MoveIME()
{
  debug_ime(CCFY "GHOST_ImeWin32::MoveIME()");

  MoveIME(caret_rect_, exclude_rect_);
}

void GHOST_ImeWin32::MoveIME(const GHOST_Rect &caret_rect, const GHOST_Rect &exclude_rect)
{
  debug_ime(CCFY "GHOST_ImeWin32::MoveIME(caret_rect, exclude_rect)");

  HIMC himc = ::ImmGetContext(h_wnd_);

  if (himc) {
    caret_rect_ = caret_rect;
    exclude_rect_ = exclude_rect;

    debug_ime(CCFY "  1: (%d, %d, %d, %d), (%d, %d, %d, %d)",
              caret_rect.l_,
              caret_rect.t_,
              caret_rect.getWidth(),
              caret_rect.getHeight(),
              exclude_rect.l_,
              exclude_rect.t_,
              exclude_rect.getWidth(),
              exclude_rect.getHeight());

    /**
     * c_l, c_t, c_w, c_h is the rect of text caret,
     * e_l, e_t, e_w, e_h is the rect of exclude area.
     *
     * NOTE: CANDIDATEFORM.ptCurrentPos containing the coordinates of the upper left corner
     * of the candidate window or the caret position, depending on the value of dwStyle.
     * CFS_CANDIDATEPOS - ptCurrentPos is the upper left corner of the candidate window.
     * CFS_EXCLUDE - ptCurrentPos is the upper left corner of the text caret.
     *
     * Here we always use CFS_EXCLUDE:
     * - It can simply treat as the text caret.
     * - when the downward has not enought space, the candidate window will display to upperward,
     * if we use CFS_CANDIDATEPOS, the candidate window may overlay the composing string,
     * because IME don't know the height of the composing string.
     *
     * NOTE: If the height of system caret less than 2,
     * some IMEs will ignore the position of system caret.
     */
    int c_l = caret_rect.l_;
    int c_t = caret_rect.t_;
    int c_w = max(0, caret_rect.getWidth());
    int c_h = max(2, caret_rect.getHeight());
    int e_l = exclude_rect.l_;
    int e_t = exclude_rect.t_;
    int e_w = max(0, exclude_rect.getWidth());
    int e_h = max(2, exclude_rect.getHeight());

    caret_rect_.l_ = c_l;
    caret_rect_.t_ = c_t;
    caret_rect_.r_ = c_l + c_w;
    caret_rect_.b_ = c_t + c_h;
    exclude_rect_.l_ = e_l;
    exclude_rect_.t_ = e_t;
    exclude_rect_.r_ = e_l + e_w;
    exclude_rect_.b_ = e_t + e_h;

    debug_ime(
        CCFY "  2: (%d, %d, %d, %d), (%d, %d, %d, %d)", c_l, c_t, c_w, c_h, e_l, e_t, e_w, e_h);

    CANDIDATEFORM candidate_position = {
        0, CFS_EXCLUDE, {c_l, c_t}, {e_l, e_t, e_l + e_w, e_t + e_h}};
    ::ImmSetCandidateWindow(himc, &candidate_position);

    /**
     * Some Chinese IMEs ignore function calls to ::ImmSetCandidateWindow()
     * when a user disables TSF (Text Service Framework) and CUAS (Cicero
     * Unaware Application Support).
     * On the other hand, when a user enables TSF and CUAS, Chinese IMEs
     * ignore the position of the current system caret and uses the
     * parameters given to ::ImmSetCandidateWindow() with its 'dwStyle'
     * parameter CFS_CANDIDATEPOS.
     * Therefore, we do not only call ::ImmSetCandidateWindow() but also
     * set the positions of the temporary system caret if it exists.
     */

    ::DestroyCaret();
    ::CreateCaret(h_wnd_, NULL, c_w, c_h);
    ::SetCaretPos(c_l, c_t);

#  if defined(_DEBUG) || FORCE_DEBUG
    ::ShowCaret(h_wnd_);
#  endif

    ::ImmReleaseContext(h_wnd_, himc);
  }
  else {
    debug_ime(CCFY "  !HIMC");
  }
}

void GHOST_ImeWin32::StartIMEComplsitionByChar(char c)
{
  WORD key = LOBYTE(::VkKeyScan(c));
  WORD scan = MapVirtualKey(key, MAPVK_VK_TO_VSC);

  INPUT playback_key_events[2];

  playback_key_events[0].type = INPUT_KEYBOARD;
  playback_key_events[0].ki.wVk = key;
  playback_key_events[0].ki.wScan = scan;
  playback_key_events[0].ki.dwFlags = 0;
  playback_key_events[0].ki.dwExtraInfo = 0;

  playback_key_events[1].type = INPUT_KEYBOARD;
  playback_key_events[1].ki.wVk = key;
  playback_key_events[1].ki.wScan = scan;
  playback_key_events[1].ki.dwFlags = KEYEVENTF_KEYUP;
  playback_key_events[1].ki.dwExtraInfo = 0;

  ::SendInput(2, (PINPUT)&playback_key_events, sizeof(INPUT));
}

static void convert_utf16_to_utf8_len(std::wstring s, int &len)
{
  if (len >= 0 && len <= s.size()) {
    len = count_utf_8_from_16(s.substr(0, len).c_str()) - 1;
  }
  else {
    len = -1;
  }
}

static size_t updateUtf8Buf(ImeComposition &info)
{
  size_t len = count_utf_8_from_16(info.ime_string.c_str());
  info.utf8_buf.resize(len);
  conv_utf_16_to_8(info.ime_string.c_str(), &info.utf8_buf[0], len);
  convert_utf16_to_utf8_len(info.ime_string, info.cursor_position);
  convert_utf16_to_utf8_len(info.ime_string, info.target_start);
  convert_utf16_to_utf8_len(info.ime_string, info.target_end);
  return len - 1;
}

void GHOST_ImeWin32::UpdateInfo(LPARAM lparam)
{
  int res = this->GetResult(lparam, &resultInfo);
  debug_ime("GHOST_ImeWin32 result str len: %d", res);
  int comp = this->GetComposition(lparam, &compInfo);
  debug_ime("GHOST_ImeWin32 composition str len: %d", comp);
  /* convert wchar to utf8 */
  if (res) {
    updateUtf8Buf(resultInfo);
    eventImeData.result = std::string(&resultInfo.utf8_buf[0]);
  }
  else {
    eventImeData.result = "";
  }
  if (comp) {
    updateUtf8Buf(compInfo);
    eventImeData.composite = std::string(&compInfo.utf8_buf[0]);
    eventImeData.cursor_position = compInfo.cursor_position;
    eventImeData.target_start = compInfo.target_start;
    eventImeData.target_end = compInfo.target_end;
  }
  else {
    eventImeData.composite = "";
    eventImeData.cursor_position = -1;
    eventImeData.target_start = -1;
    eventImeData.target_end = -1;
  }
}

bool GHOST_ImeWin32::GetResult(LPARAM lparam, ImeComposition *composition)
{
  bool result = false;
  HIMC himc = ::ImmGetContext(h_wnd_);
  if (himc) {
    /* Copy the result string to the ImeComposition object. */
    result = GetString(himc, lparam, GCS_RESULTSTR, composition);

    ::ImmReleaseContext(h_wnd_, himc);
  }
  return result;
}

bool GHOST_ImeWin32::GetComposition(LPARAM lparam, ImeComposition *composition)
{
  bool result = false;
  HIMC himc = ::ImmGetContext(h_wnd_);
  if (himc) {
    /* Copy the composition string to the ImeComposition object. */
    result = GetString(himc, lparam, GCS_COMPSTR, composition);

    if (result) {
      /* Retrieve the cursor position in the IME composition. */
      int cursor_position = ::ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
      composition->cursor_position = cursor_position;

      /* Retrieve the target selection and Update the ImeComposition object. */
      GetCaret(himc, lparam, composition);
    }

    ::ImmReleaseContext(h_wnd_, himc);
  }
  return result;
}

bool GHOST_ImeWin32::GetString(HIMC himc, WPARAM lparam, int type, ImeComposition *composition)
{
  bool result = false;
  if (lparam & type) {
    int string_size = ::ImmGetCompositionStringW(himc, type, nullptr, 0);
    if (string_size > 0) {
      int string_length = string_size / sizeof(wchar_t);
      wchar_t *string_data = new wchar_t[string_length + 1];
      string_data[string_length] = '\0';
      if (string_data) {
        /* Fill the given ImeComposition object. */
        ::ImmGetCompositionStringW(himc, type, string_data, string_size);
        composition->string_type = type;
        composition->ime_string = string_data;
        result = true;
      }
      delete[] string_data;
    }
  }
  return result;
}

void GHOST_ImeWin32::GetCaret(HIMC himc, LPARAM lparam, ImeComposition *composition)
{
  int target_start = -1;
  int target_end = -1;

  /**
   * The "Target" originally referred to the characters which have the ATTR_TARGET_NOTCONVERTED or
   * ATTR_TARGET_CONVERTED attribute. That characters are always continuous.
   *
   * Usually only Japanese and some Traditional Chinese IMEs generate that attribute.
   * So there will be no "Target" in other IMEs.
   *
   * It is OK, but in reality there is "Target" in other IMEs.
   * We can observe the "Target" through the position of the candidate window.
   * Usually their candidate window align to the start of "Target".
   *
   * To achieve the same effect, some rules have been added here to calculate
   * the "Target" of other IMEs:
   * 1. If IME generate `GCS_COMPCLAUSE`, use the clause that includes `GCS_CURSORPOS` as the
   * target. This mainly applies to some Chinese IMEs.
   * 2. Otherwire, we treat whole composition string as a target.
   */

  if (lparam & GCS_COMPATTR) {
    int attrs_len = ::ImmGetCompositionStringW(himc, GCS_COMPATTR, nullptr, 0);
    if (attrs_len > 0) {
      char *attrs = new char[attrs_len];
      if (attrs) {
        ::ImmGetCompositionStringW(himc, GCS_COMPATTR, attrs, attrs_len);
        for (target_start = 0; target_start < attrs_len; ++target_start) {
          if (IsTargetAttribute(attrs[target_start]))
            break;
        }
        for (target_end = target_start; target_end < attrs_len; ++target_end) {
          if (!IsTargetAttribute(attrs[target_end]))
            break;
        }
        /**
         * `attrs_len` is equal to `composition->ime_string.size()`.
         * If `target_start` equal to `attrs_len`, means `ATTR_TARGET_XXX` not exists.
         */
        if (target_start == attrs_len) {
          target_start = -1;
          target_end = -1;
        }
      }
      delete[] attrs;
    }
  }

  if (target_start == -1 && lparam & GCS_COMPCLAUSE) {
    /**
     * If `GCS_COMPCLAUSE` exists, use the clause that includes `GCS_CURSORPOS` as the target.
     *
     * About the Caluse:
     * https://learn.microsoft.com/en-us/windows/win32/intl/composition-string
     *
     * If the composition string has two clause, then:
     * clauses[0]: 0 - the start of clause-1 is 0, and the end is clause[1] - 1.
     * clauses[1]: 2 - the start of clause-2 is 2, and the end is clause[2] - 1.
     * clauses[2]: 5 - the length of the composition string is 5.
     */
    int clauses_buffer_len = ::ImmGetCompositionStringW(himc, GCS_COMPCLAUSE, nullptr, 0);
    if (clauses_buffer_len) {
      int clauses_len = clauses_buffer_len / sizeof(ulong);
      ulong *clauses = new ulong[clauses_len];
      if (clauses) {
        ::ImmGetCompositionStringW(himc, GCS_COMPCLAUSE, clauses, clauses_buffer_len);

        for (int i = 0; i < clauses_len; i++) {
          debug_ime(CCFY "\t[%d]: %d", i, clauses[i]);
        }

        if (composition->cursor_position == clauses[clauses_len - 1]) {
          target_start = clauses[clauses_len - 2];
          target_end = clauses[clauses_len - 1];
        }
        else {
          for (int i = 0; i < clauses_len - 1; i++) {
            if (clauses[i] <= composition->cursor_position &&
                composition->cursor_position < clauses[i + 1])
            {
              target_start = clauses[i];
              target_end = clauses[i + 1];
              break;
            }
          }
        }
      }
      delete[] clauses;
    }
  }

  if (target_start == -1) {
    /**
     * This composition string does not contain any target attribute or clauses,
     * i.e. this composition string is an input string.
     * We treat whole this string as a target.
     */
    target_start = 0;
    target_end = composition->ime_string.size();
  }

  composition->target_start = target_start;
  composition->target_end = target_end;
}

#endif /* WITH_INPUT_IME */
