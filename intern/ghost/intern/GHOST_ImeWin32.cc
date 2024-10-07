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

/* ISO_639-1 2-Letter Abbreviations. */
#  define IMELANG_ENGLISH "en"
#  define IMELANG_CHINESE "zh"
#  define IMELANG_JAPANESE "ja"
#  define IMELANG_KOREAN "ko"

GHOST_ImeWin32::GHOST_ImeWin32()
    : m_caret_rect(0, 0, 0, 0),
      m_exclude_rect(0, 0, 0, 0),
      m_hwnd(nullptr),
      m_is_first(true),
      m_language(IMELANG_ENGLISH),
      m_invoker(GHOST_IMEInvokerNone),
      m_is_paused(false),
      m_is_composing(false)
{
}

GHOST_ImeWin32::~GHOST_ImeWin32() {}

HWND GHOST_ImeWin32::GetHwnd()
{
  return m_hwnd;
}

void GHOST_ImeWin32::SetHwnd(HWND window_handle)
{
  if (window_handle && !m_hwnd) {
    printx(CCFY "GHOST_ImeWin32::SetHwnd: %p", window_handle);
    m_hwnd = window_handle;
  }
}

void GHOST_ImeWin32::CheckFirst()
{
  if (m_is_first) {
    m_is_first = false;

    printx(CCFY "GHOST_ImeWin32::CheckFirst");

    /* Ensure the states record by ourself is up to date. */
    UpdateInputLanguage();

    /* The IME is enabled by default, but we want it disabled at default,
     * because we application is not a Text Process Program.
     */
    EndIME();
  }
}

void GHOST_ImeWin32::UpdateInputLanguage()
{
  printx(CCFY "GHOST_ImeWin32::UpdateInputLanguage");

  /* Get the current input locale full name. */
  WCHAR locale[LOCALE_NAME_MAX_LENGTH];
  LCIDToLocaleName(
      MAKELCID(LOWORD(::GetKeyboardLayout(0)), SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0);
  /* Get the 2-letter ISO-63901 abbreviation of the input locale name. */
  WCHAR language_u16[W32_ISO639_LEN];
  GetLocaleInfoEx(locale, LOCALE_SISO639LANGNAME, language_u16, W32_ISO639_LEN);
  /* Store this as a UTF-8 string. */
  WideCharToMultiByte(
      CP_UTF8, 0, language_u16, W32_ISO639_LEN, m_language, W32_ISO639_LEN, nullptr, nullptr);
  printx(CCFY "  Language: %s", m_language);
}

BOOL GHOST_ImeWin32::IsLanguage(const char name[W32_ISO639_LEN])
{
  return (strcmp(name, m_language) == 0);
}

bool GHOST_ImeWin32::SetImeWindowStyle(UINT message, WPARAM wparam, LPARAM lparam)
{
  /**
   * To prevent the IMM (Input Method Manager) from displaying the IME
   * composition window, Update the styles of the IME windows and EXPLICITLY
   * call ::DefWindowProc() here.
   * NOTE(hbono): We can NEVER let WTL call ::DefWindowProc() when we update
   * the styles of IME windows because the 'lparam' variable is a local one
   * and all its updates disappear in returning from this function, i.e. WTL
   * does not call ::DefWindowProc() with our updated 'lparam' value but call
   * the function with its original value and over-writes our window styles.
   */
  lparam &= ~ISC_SHOWUICOMPOSITIONWINDOW;
  ::DefWindowProcW(m_hwnd, message, wparam, lparam);

  return true;
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

void GHOST_ImeWin32::OnWindowActivated()
{
  printx(CCFY "GHOST_ImeWin32::OnWindowActivated");

  /* Ensure the candidate window position, and system caret postion. */
  MoveIME();
}

void GHOST_ImeWin32::OnWindowDeactivated()
{
  printx(CCFY "GHOST_ImeWin32::OnWindowDeactivated");

  /* WIN32 ignores this call if the system caret is not created.  */
  ::DestroyCaret();
}

void GHOST_ImeWin32::BeginIME(GHOST_IMEInvoker invoker)
{
  printx(CCFY "GHOST_ImeWin32::BeginIME");

  BLI_assert(invoker != GHOST_IMEInvokerNone);

  m_invoker = invoker;

  printx(CCFY "  HWND: %p", m_hwnd);

  /**
   * Load the default IME context.
   * NOTE(hbono)
   *   IMM ignores this call if the IME context is loaded. Therefore, we do
   *   not have to check whether or not the IME context is loaded.
   */
  ::ImmAssociateContextEx(m_hwnd, nullptr, IACE_DEFAULT);
}

void GHOST_ImeWin32::EndIME()
{
  printx(CCFY "GHOST_ImeWin32::EndIME");

  m_invoker = GHOST_IMEInvokerNone;

  HIMC himc = ::ImmGetContext(m_hwnd);

  if (himc) {
    ::ImmReleaseContext(m_hwnd, himc);

    printx(CCFY "  HWND, HIMC: %p, %p", m_hwnd, himc);

    /**
     * A renderer process have moved its input focus to a password input
     * when there is an ongoing composition, e.g. a user has clicked a
     * mouse button and selected a password input while composing a text.
     * For this case, we have to complete the ongoing composition and
     * clean up the resources attached to this object BEFORE DISABLING THE IME.
     */
    CompleteComposition();

    ::ImmAssociateContextEx(m_hwnd, nullptr, 0);
  }

  ::DestroyCaret();
}

bool GHOST_ImeWin32::IsEnabled()
{
  HIMC himc = ::ImmGetContext(m_hwnd);
  if (himc) {
    ::ImmReleaseContext(m_hwnd, himc);
    return true;
  }
  else {
    return false;
  }
}

GHOST_IMEInvoker GHOST_ImeWin32::GetIMEInvoker()
{
  return m_invoker;
}

void GHOST_ImeWin32::PauseIME()
{
  printx(CCFY "GHOST_ImeWin32::PauseIME");
  m_is_paused = true;
  EndIME();
}

void GHOST_ImeWin32::ResumeIME()
{
  printx(CCFY "GHOST_ImeWin32::ResumeIME");
  m_is_paused = false;
  BeginIME(m_invoker);
}

bool GHOST_ImeWin32::IsPaused()
{
  return m_is_paused;
}

void GHOST_ImeWin32::MoveIME()
{
  printx(CCFY "GHOST_ImeWin32::MoveIME()");

  MoveIME(m_caret_rect, m_exclude_rect);
}

void GHOST_ImeWin32::MoveIME(const GHOST_Rect &caret_rect, const GHOST_Rect &exclude_rect)
{
  printx(CCFY "GHOST_ImeWin32::MoveIME(caret_rect, exclude_rect)");

  HIMC himc = ::ImmGetContext(m_hwnd);

  if (himc) {
    m_caret_rect = caret_rect;
    m_exclude_rect = exclude_rect;

    printx(CCFY "  1: (%d, %d, %d, %d), (%d, %d, %d, %d)",
           caret_rect.m_l,
           caret_rect.m_t,
           caret_rect.getWidth(),
           caret_rect.getHeight(),
           exclude_rect.m_l,
           exclude_rect.m_t,
           exclude_rect.getWidth(),
           exclude_rect.getHeight());

    /**
     * c_x, c_y, ... is the rect of system caret,
     * e_l, e_t, ... is the rect of exclude area.
     *
     * \note CANDIDATEFORM.ptCurrentPos containing the coordinates of the upper left corner
     * of the candidate window or the caret position, depending on the value of dwStyle.
     * CFS_CANDIDATEPOS - ptCurrentPos is the upper left corner of the candidate window.
     * CFS_EXCLUDE - ptCurrentPos is the upper left corner of the system caret.
     *
     * Here we always use CFS_EXCLUDE:
     * - It can simply treat as the system caret.
     * - when the downward has not enought space, the candidate window will display to upperward,
     * if we use CFS_CANDIDATEPOS, the candidate window will overlay the composiing string.
     * because IME don't know the height of the composing string.
     *
     *
     * \note If the height of system caret less than 2,
     * some IMEs will ignore the position of system caret.
     * ie. Baidu Pinyin
     */
    int c_x = caret_rect.m_l;
    int c_y = caret_rect.m_t;
    int c_w = max(0, caret_rect.getWidth());
    int c_h = max(2, caret_rect.getHeight());
    int e_l = exclude_rect.m_l;
    int e_t = exclude_rect.m_t;
    int e_w = max(0, exclude_rect.getWidth());
    int e_h = max(2, exclude_rect.getHeight());

    m_caret_rect.m_l = c_x;
    m_caret_rect.m_t = c_y;
    m_caret_rect.m_r = c_x + c_w;
    m_caret_rect.m_b = c_y + c_h;
    m_exclude_rect.m_l = e_l;
    m_exclude_rect.m_t = e_t;
    m_exclude_rect.m_r = e_l + e_w;
    m_exclude_rect.m_b = e_t + e_h;

    printx(CCFY "  2: (%d, %d, %d, %d), (%d, %d, %d, %d)", c_x, c_y, c_w, c_h, e_l, e_t, e_w, e_h);

    // 一些输入法指根据光标位置决定候选栏位置。（百度拼音输入法）
    // 光标高度不会也会被忽略。（百度拼音输入法）
    // 一些输入法同时支持两种模式，如果有后者则优先后者。一般后者会自动添加间距，各个输入法添加的间距都不同。
    // 如果使用光标位置，一般不会自动添加间距。

    /**
     * As written in a comment in GHOST_ImeWin32::CreateImeWindow(),
     * Chinese IMEs ignore function calls to ::ImmSetCandidateWindow()
     * when a user disables TSF (Text Service Framework) and CUAS (Cicero
     * Unaware Application Support).
     * On the other hand, when a user enables TSF and CUAS, Chinese IMEs
     * ignore the position of the current system caret and uses the
     * parameters given to ::ImmSetCandidateWindow() with its 'dwStyle'
     * parameter CFS_CANDIDATEPOS.
     * Therefore, we do not only call ::ImmSetCandidateWindow() but also
     * set the positions of the temporary system caret if it exists.
     */

    /**
     * Some IMEs ignore ImmSetCandidateWindow, or just ignore CFS_CANDIDATEPOS.
     * Some IMEs take ImmSetCandidateWindow, but also need the system caret to
     * location some special window(like conve mode switch state hit).
     * ... so no matter what, we need to set the system caret.
     */

    // ::DestroyCaret();
    // ::CreateCaret(m_hwnd, NULL, c_w, c_h);
    // ::SetCaretPos(c_x, c_y);

    ::DestroyCaret();
    ::CreateCaret(m_hwnd, NULL, c_w, c_h);
    ::SetCaretPos(0, 0);

    if (DEBUG_IME) {
      ::ShowCaret(m_hwnd);
    }

    CANDIDATEFORM candidate_position = {
        0, CFS_DEFAULT | CFS_EXCLUDE, {c_x, c_y}, {e_l, e_t, e_l + e_w, e_t + e_h}};
    ::ImmSetCandidateWindow(himc, &candidate_position);

    ::ImmReleaseContext(m_hwnd, himc);
  }
  else {
    printx(CCFY "  !HIMC");
  }
}

void GHOST_ImeWin32::OnCompositionStart()
{
  printx(CCFY "GHOST_ImeWin32::OnCompositionStart");
  m_is_composing = true;
}

void GHOST_ImeWin32::OnCompositionUpdate(LPARAM lparam)
{
  printx(CCFY "GHOST_ImeWin32::OnCompositionUpdate");
  UpdateInfo(lparam);
}

void GHOST_ImeWin32::OnCompositionEnd()
{
  printx(CCFY "GHOST_ImeWin32::OnCompositionEnd");
  m_is_composing = false;
}

bool GHOST_ImeWin32::IsComposing()
{
  return m_is_composing;
}

void GHOST_ImeWin32::CompleteComposition()
{
  if (m_is_composing) {
    printx(CCFY "CompleteComposition");
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
    printx(CCFY "CancelComposition");
    HIMC himc = ::ImmGetContext(m_hwnd);
    if (himc) {
      ::ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
      ::ImmReleaseContext(m_hwnd, himc);
    }
  }
}

void GHOST_ImeWin32::StartIMEComplsitionByChar(char c)
{
  WORD key = LOBYTE(::VkKeyScan(c));

  INPUT playback_key_events[1];
  playback_key_events[0].type = INPUT_KEYBOARD;
  playback_key_events[0].ki.wVk = key;
  playback_key_events[0].ki.wScan = MapVirtualKey(key, MAPVK_VK_TO_VSC);
  playback_key_events[0].ki.dwFlags = 0;
  playback_key_events[0].ki.dwExtraInfo = NULL;
  ::SendInput(1, (PINPUT)&playback_key_events, sizeof(INPUT));
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
  printx("GHOST_ImeWin32 result str len: %d", res);
  int comp = this->GetComposition(lparam, &compInfo);
  printx("GHOST_ImeWin32 composition str len: %d", comp);
  /* convert wchar to utf8 */
  if (res) {
    eventImeData.result_len = (GHOST_TUserDataPtr)updateUtf8Buf(resultInfo);
    eventImeData.result = &resultInfo.utf8_buf[0];
  }
  else {
    eventImeData.result = 0;
    eventImeData.result_len = 0;
  }
  if (comp) {
    eventImeData.composite_len = (GHOST_TUserDataPtr)updateUtf8Buf(compInfo);
    eventImeData.composite = &compInfo.utf8_buf[0];
    eventImeData.cursor_position = compInfo.cursor_position;
    eventImeData.target_start = compInfo.target_start;
    eventImeData.target_end = compInfo.target_end;
  }
  else {
    eventImeData.composite = 0;
    eventImeData.composite_len = 0;
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
    // lparam
    /* Copy the result string to the ImeComposition object. */
    result = GetString(himc, lparam, GCS_RESULTSTR, composition);
    /**
     * Reset all the other parameters because a result string does not
     * have composition attributes.
     */
    composition->cursor_position = -1;
    composition->target_start = -1;
    composition->target_end = -1;
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

    /* Retrieve the cursor position in the IME composition. */
    int cursor_position = ::ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
    composition->cursor_position = cursor_position;
    composition->target_start = -1;
    composition->target_end = -1;

    /* Retrieve the target selection and Update the ImeComposition object. */
    GetCaret(himc, lparam, composition);

    // /* Mark that there is an ongoing composition. */
    // m_is_composing = true;

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
  /**
   * This operation is optional and language-dependent because the caret
   * style is depended on the language, e.g.:
   *   * Korean IMEs: the caret is a blinking block,
   *     (It contains only one hangul character);
   *   * Chinese IMEs: the caret is a blinking line,
   *     (i.e. they do not need to retrieve the target selection);
   *   * Japanese IMEs: the caret is a selection (or underlined) block,
   *     (which can contain one or more Japanese characters).
   */
  int target_start = -1;
  int target_end = -1;
  if (IsLanguage(IMELANG_KOREAN)) {
    if (lparam & CS_NOMOVECARET) {
      target_start = 0;
      target_end = 1;
    }
  }
  else if (IsLanguage(IMELANG_CHINESE)) {
    printx(CCFY "GetCaret IMELANG_CHINESE");
    int clause_size = ImmGetCompositionStringW(himc, GCS_COMPCLAUSE, nullptr, 0);
    if (clause_size) {
      printx(CCFY "GetCaret IMELANG_CHINESE clause_size: %d", clause_size);
      static std::vector<ulong> clauses;
      clause_size = clause_size / sizeof(clauses[0]);
      clauses.resize(clause_size);
      ImmGetCompositionStringW(
          himc, GCS_COMPCLAUSE, &clauses[0], sizeof(clauses[0]) * clause_size);
      if (composition->cursor_position == composition->ime_string.size()) {
        target_start = clauses[clause_size - 2];
        target_end = clauses[clause_size - 1];
      }
      else {
        for (int i = 0; i < clause_size - 1; i++) {
          if (clauses[i] == composition->cursor_position) {
            target_start = clauses[i];
            target_end = clauses[i + 1];
            break;
          }
        }
      }
    }
    else {
      if (composition->cursor_position != -1) {
        target_start = composition->cursor_position;
        target_end = composition->ime_string.size();
      }
    }
  }
  else if (IsLanguage(IMELANG_JAPANESE)) {
    /**
     * For Japanese IMEs, the robustest way to retrieve the caret
     * is scanning the attribute of the latest composition string and
     * retrieving the beginning and the end of the target clause, i.e.
     * a clause being converted.
     */
    if (lparam & GCS_COMPATTR) {
      int attribute_size = ::ImmGetCompositionStringW(himc, GCS_COMPATTR, nullptr, 0);
      if (attribute_size > 0) {
        char *attribute_data = new char[attribute_size];
        if (attribute_data) {
          ::ImmGetCompositionStringW(himc, GCS_COMPATTR, attribute_data, attribute_size);
          for (target_start = 0; target_start < attribute_size; ++target_start) {
            if (IsTargetAttribute(attribute_data[target_start]))
              break;
          }
          for (target_end = target_start; target_end < attribute_size; ++target_end) {
            if (!IsTargetAttribute(attribute_data[target_end]))
              break;
          }
          if (target_start == attribute_size) {
            /**
             * This composition clause does not contain any target clauses,
             * i.e. this clauses is an input clause.
             * We treat whole this clause as a target clause.
             */
            target_end = target_start;
            target_start = 0;
          }
          if (target_start != -1 && target_start < attribute_size &&
              attribute_data[target_start] == ATTR_TARGET_NOTCONVERTED)
          {
            composition->cursor_position = target_start;
          }
        }
        delete[] attribute_data;
      }
    }
  }
  composition->target_start = target_start;
  composition->target_end = target_end;
}

#endif /* WITH_INPUT_IME */
