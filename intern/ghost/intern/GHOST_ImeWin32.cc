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
    : m_caret_rect(0, 0, 0, 0),
      m_exclude_rect(0, 0, 0, 0),
      m_hwnd(nullptr),
      m_is_first(true),
      m_is_enabled(true),
      m_is_composing(false)
{
}

GHOST_ImeWin32::~GHOST_ImeWin32() {}

void GHOST_ImeWin32::CheckFirst()
{
  if (m_is_first) {
    m_is_first = false;

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

  HIMC himc = ::ImmGetContext(m_hwnd);
  if (himc) {
    ::ImmReleaseContext(m_hwnd, himc);
    m_is_enabled = true;
  }
  else {
    m_is_enabled = false;
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

  return ::DefWindowProcW(m_hwnd, message, wparam, lparam & ~ISC_SHOWUICOMPOSITIONWINDOW);
}

bool GHOST_ImeWin32::IsIgnoreKey(USHORT key)
{
  switch (key) {
    case VK_LWIN:
    case VK_RWIN:
      return true;
    default:
      return false;
  }
}

void GHOST_ImeWin32::BeginIME()
{
  debug_ime(CCFY "GHOST_ImeWin32::BeginIME");

  debug_ime(CCFY "  HWND: %p", m_hwnd);

  /**
   * Load the default IME context.
   *
   * NOTE:
   *   IMM ignores this call if the IME context is loaded. Therefore, we do
   *   not have to check whether or not the IME context is loaded.
   */
  ::ImmAssociateContextEx(m_hwnd, nullptr, IACE_DEFAULT);
}

void GHOST_ImeWin32::EndIME()
{
  debug_ime(CCFY "GHOST_ImeWin32::EndIME");

  HIMC himc = ::ImmGetContext(m_hwnd);

  if (himc) {
    ::ImmReleaseContext(m_hwnd, himc);

    debug_ime(CCFY "  HWND, HIMC: %p, %p", m_hwnd, himc);

    /**
     * Clean up the composition BEFORE DISABLING THE IME.
     *
     * IMM ignores this call if there is no ongoing composition.
     */
    CompleteComposition();

    ::ImmAssociateContextEx(m_hwnd, nullptr, 0);
  }

  /* Windows ignores this call if there is no system caret.  */
  ::DestroyCaret();
}

bool GHOST_ImeWin32::IsEnabled()
{
  debug_ime(CCFA "GHOST_ImeWin32::IsEnabled");
  return m_is_enabled;
}

void GHOST_ImeWin32::OnCompositionStart()
{
  debug_ime(CCFY "GHOST_ImeWin32::OnCompositionStart");
  m_is_composing = true;
}

void GHOST_ImeWin32::OnCompositionUpdate(LPARAM lparam)
{
  debug_ime(CCFY "GHOST_ImeWin32::OnCompositionUpdate");
  UpdateInfo(lparam);
}

void GHOST_ImeWin32::OnCompositionEnd()
{
  debug_ime(CCFY "GHOST_ImeWin32::OnCompositionEnd");
  m_is_composing = false;
}

bool GHOST_ImeWin32::IsComposing()
{
  return m_is_composing;
}

void GHOST_ImeWin32::CompleteComposition()
{
  if (m_is_composing) {
    debug_ime(CCFY "CompleteComposition");
    HIMC himc = ::ImmGetContext(m_hwnd);
    if (himc) {
      ::ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
      ::ImmReleaseContext(m_hwnd, himc);
    }
  }
}

void GHOST_ImeWin32::CancelComposition()
{
  if (m_is_composing) {
    debug_ime(CCFY "CancelComposition");
    HIMC himc = ::ImmGetContext(m_hwnd);
    if (himc) {
      ::ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
      ::ImmReleaseContext(m_hwnd, himc);
    }
  }
}

void GHOST_ImeWin32::MoveIME()
{
  debug_ime(CCFY "GHOST_ImeWin32::MoveIME()");

  MoveIME(m_caret_rect, m_exclude_rect);
}

void GHOST_ImeWin32::MoveIME(const GHOST_Rect &caret_rect, const GHOST_Rect &exclude_rect)
{
  debug_ime(CCFY "GHOST_ImeWin32::MoveIME(caret_rect, exclude_rect)");

  HIMC himc = ::ImmGetContext(m_hwnd);

  if (himc) {
    m_caret_rect = caret_rect;
    m_exclude_rect = exclude_rect;

    debug_ime(CCFY "  1: (%d, %d, %d, %d), (%d, %d, %d, %d)",
              caret_rect.m_l,
              caret_rect.m_t,
              caret_rect.getWidth(),
              caret_rect.getHeight(),
              exclude_rect.m_l,
              exclude_rect.m_t,
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
    int c_l = caret_rect.m_l;
    int c_t = caret_rect.m_t;
    int c_w = max(0, caret_rect.getWidth());
    int c_h = max(2, caret_rect.getHeight());
    int e_l = exclude_rect.m_l;
    int e_t = exclude_rect.m_t;
    int e_w = max(0, exclude_rect.getWidth());
    int e_h = max(2, exclude_rect.getHeight());

    m_caret_rect.m_l = c_l;
    m_caret_rect.m_t = c_t;
    m_caret_rect.m_r = c_l + c_w;
    m_caret_rect.m_b = c_t + c_h;
    m_exclude_rect.m_l = e_l;
    m_exclude_rect.m_t = e_t;
    m_exclude_rect.m_r = e_l + e_w;
    m_exclude_rect.m_b = e_t + e_h;

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
    ::CreateCaret(m_hwnd, NULL, c_w, c_h);
    ::SetCaretPos(c_l, c_t);

#  if defined(_DEBUG) && !defined(_NDEBUG_IME)
    if (DEBUG_IME) {
      ::ShowCaret(m_hwnd);
    }
#  endif

    ::ImmReleaseContext(m_hwnd, himc);
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
  if (len >= 0 && len <= s.size())
    len = count_utf_8_from_16(s.substr(0, len).c_str()) - 1;
  else
    len = -1;
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
  HIMC himc = ::ImmGetContext(m_hwnd);
  if (himc) {
    /* Copy the result string to the ImeComposition object. */
    result = GetString(himc, lparam, GCS_RESULTSTR, composition);

    ::ImmReleaseContext(m_hwnd, himc);
  }
  return result;
}

bool GHOST_ImeWin32::GetComposition(LPARAM lparam, ImeComposition *composition)
{
  bool result = false;
  HIMC himc = ::ImmGetContext(m_hwnd);
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

    ::ImmReleaseContext(m_hwnd, himc);
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
