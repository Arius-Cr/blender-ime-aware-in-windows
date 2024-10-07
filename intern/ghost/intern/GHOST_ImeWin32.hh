/* SPDX-FileCopyrightText: 2010 The Chromium Authors. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#pragma once

#ifdef WITH_INPUT_IME

#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <string>

#  include "GHOST_Event.hh"
#  include "GHOST_Rect.hh"
#  include <vector>

/* MSDN LOCALE_SISO639LANGNAME states maximum length of 9, including terminating null. */
#  define W32_ISO639_LEN 9

class GHOST_EventIME : public GHOST_Event {
 public:
  /**
   * Constructor.
   * \param msec: The time this event was generated.
   * \param type: The type of key event.
   * \param key: The key code of the key.
   */
  GHOST_EventIME(uint64_t msec, GHOST_TEventType type, GHOST_IWindow *window, void *customdata)
      : GHOST_Event(msec, type, window)
  {
    this->m_data = customdata;
  }
};

/**
 * This header file defines a struct and a class used for encapsulating IMM32
 * APIs, controls IMEs attached to a window, and enables the 'on-the-spot'
 * input without deep knowledge about the APIs, i.e. knowledge about the
 * language-specific and IME-specific behaviors.
 * The following items enumerates the simplest steps for an (window)
 * application to control its IMEs with the struct and the class defined
 * this file.
 * 1. Add an instance of the GHOST_ImeWin32 class to its window class.
 *    (The GHOST_ImeWin32 class needs a window handle.)
 * 2. Add messages handlers listed in the following subsections, follow the
 *    instructions written in each subsection, and use the GHOST_ImeWin32 class.
 * 2.1. WM_IME_SETCONTEXT (0x0281)
 *      Call the functions listed below:
 *      - GHOST_ImeWin32::CreateImeWindow();
 *      - GHOST_ImeWin32::CleanupComposition(), and;
 *      - GHOST_ImeWin32::SetImeWindowStyle().
 *      An application MUST prevent from calling ::DefWindowProc().
 * 2.2. WM_IME_STARTCOMPOSITION (0x010D)
 *      Call the functions listed below:
 *      - GHOST_ImeWin32::CreateImeWindow(), and;
 *      - GHOST_ImeWin32::ResetComposition().
 *      An application MUST prevent from calling ::DefWindowProc().
 * 2.3. WM_IME_COMPOSITION (0x010F)
 *      Call the functions listed below:
 *      - GHOST_ImeWin32::UpdateImeWindow();
 *      - GHOST_ImeWin32::GetResult();
 *      - GHOST_ImeWin32::GetComposition(), and;
 *      - GHOST_ImeWin32::ResetComposition() (optional).
 *      An application MUST prevent from calling ::DefWindowProc().
 * 2.4. WM_IME_ENDCOMPOSITION (0x010E)
 *      Call the functions listed below:
 *      - GHOST_ImeWin32::ResetComposition(), and;
 *      - GHOST_ImeWin32::DestroyImeWindow().
 *      An application CAN call ::DefWindowProc().
 * 2.5. WM_INPUTLANGCHANGE (0x0051)
 *      Call the functions listed below:
 *      - GHOST_ImeWin32::UpdateInputLanguage().
 *      An application CAN call ::DefWindowProc().
 */

/* This struct represents the status of an ongoing composition. */
struct ImeComposition {
  /* Represents the cursor position in the IME composition. */
  int cursor_position;

  /* Represents the position of the beginning of the selection */
  int target_start;

  /* Represents the position of the end of the selection */
  int target_end;

  /**
   * Represents the type of the string in the 'ime_string' parameter.
   * Its possible values and description are listed below:
   *   Value         Description
   *   0             The parameter is not used.
   *   GCS_RESULTSTR The parameter represents a result string.
   *   GCS_COMPSTR   The parameter represents a composition string.
   */
  int string_type;

  /* Represents the string retrieved from IME (Input Method Editor) */
  std::wstring ime_string;
  std::vector<char> utf8_buf;
  std::vector<unsigned char> format;
};

/**
 * This class controls the IMM (Input Method Manager) through IMM32 APIs and
 * enables it to retrieve the string being controlled by the IMM. (I wrote
 * a note to describe the reason why I do not use 'IME' but 'IMM' below.)
 * NOTE(hbono):
 *   Fortunately or unfortunately, TSF (Text Service Framework) and
 *   CUAS (Cicero Unaware Application Support) allows IMM32 APIs for
 *   retrieving not only the inputs from IMEs (Input Method Editors), used
 *   only for inputting East-Asian language texts, but also the ones from
 *   tablets (on Windows XP Tablet PC Edition and Windows Vista), voice
 *   recognizers (e.g. ViaVoice and Microsoft Office), etc.
 *   We can disable TSF and CUAS in Windows XP Tablet PC Edition. On the other
 *   hand, we can NEVER disable either TSF or CUAS in Windows Vista, i.e.
 *   THIS CLASS IS NOT ONLY USED ON THE INPUT CONTEXTS OF EAST-ASIAN
 *   LANGUAGES BUT ALSO USED ON THE INPUT CONTEXTS OF ALL LANGUAGES.
 */
class GHOST_ImeWin32 {
 public:
  GHOST_ImeWin32();
  ~GHOST_ImeWin32();

  HWND GetHwnd();
  /** GHOST_WindowWin32 call this function to bind the window handle to GHOST_ImeWin32. */
  void SetHwnd(HWND window_handle);

  void CheckFirst();

  /* Retrieve the input language from Windows and store it. */
  void UpdateInputLanguage();

  BOOL IsLanguage(const char name[W32_ISO639_LEN]);

  /**
   * \return Wheater message is handled (DefWindowProcW is called).
   */
  bool SetImeWindowStyle(UINT message, WPARAM wparam, LPARAM lparam);

  bool IsIgnoreKey(USHORT key);

  void OnWindowActivated();

  void OnWindowDeactivated();

  void BeginIME(GHOST_IMEInvoker invoker);

  void EndIME();

  bool IsEnabled();

  GHOST_IMEInvoker GetIMEInvoker();

  void PauseIME();

  void ResumeIME();

  bool IsPaused();

  void MoveIME();

  void MoveIME(const GHOST_Rect &caret_rect, const GHOST_Rect &exclude_rect);

  /**
   * Call this function on WM_IME_STARTCOMPOSITION message.
   */
  void OnCompositionStart();

  /**
   * Call this function on WM_IME_COMPOSITION message.
   * Update the composite info (include result string, composite string,
   * cursor positon ...).
   */
  void OnCompositionUpdate(LPARAM lParam);

  /**
   * Call this function on WM_IME_ENDCOMPOSITION message.
   */
  void OnCompositionEnd();

  /* Retrieves whether or not there is an ongoing composition. */
  bool IsComposing();

  /**
   * Force complete the ongoing composition.
   */
  void CompleteComposition();

  /**
   * Force cancel the ongoing composition.
   */
  void CancelComposition();

  /**
   * Manually start the IME composition.
   * \param c: character of a-Z to start the composition.
   */
  void StartIMEComplsitionByChar(char c);

  ImeComposition resultInfo, compInfo;

  GHOST_TEventImeData eventImeData;

  /* The rectangle of the input caret retrieved from a renderer process. */
  GHOST_Rect m_caret_rect;

  /**
   * The exclude rectangle of the IME Window.
   */
  GHOST_Rect m_exclude_rect;

 protected:
  /**
   * Update the composite info according the lParam (e.g. GCS_RESULTSTR,
   * GCS_COMPSTR).
   * \param lParam: lParam of WM_IME_COMPOSITION.
   */
  void UpdateInfo(LPARAM lParam);
  /**
   * Get the result string from IME if exits.
   * \param lParam: lParam of WM_IME_COMPOSITION.
   */
  bool GetResult(LPARAM lparam, ImeComposition *composition);
  /**
   * Get the composition string from IME if exits.
   * \param lParam: lParam of WM_IME_COMPOSITION.
   */
  bool GetComposition(LPARAM lparam, ImeComposition *composition);
  /**
   * Get the result/composition string from IME.
   */
  bool GetString(HIMC imm_context, WPARAM lparam, int type, ImeComposition *composition);
  /**
   * Get the target range (selection range) of the composition string from IME.
   */
  void GetCaret(HIMC imm_context, LPARAM lparam, ImeComposition *composition);

  /* Determines whether or not the given attribute represents a target (a.k.a. a selection). */
  bool IsTargetAttribute(char attribute) const
  {
    return (attribute == ATTR_TARGET_CONVERTED || attribute == ATTR_TARGET_NOTCONVERTED);
  }

  /* Owner window. */
  HWND m_hwnd;

  /* Indicate whether the data has been initialized. */
  bool m_is_first;

  /* Abbreviated ISO 639-1 name of the input language, such as "en" for English. */
  char m_language[W32_ISO639_LEN];

  GHOST_IMEInvoker m_invoker;

  /**
   * The IME is enable but temporary disable.
   *
   * Most Chinese IMEs use the Shift key to switch the conversion mode.
   * So in 3DView with IME on, the view navigation shortcuts like Shift + MMB
   * will cause IME switch conversion mode.
   *
   * It is a know issue in many word processor (like notepad, Microsoft Office Word)
   * with Chinese IMEs. When use the Shift + LMB to selection text, the conversion
   * mode will change. They usual ignore this issue, but here try to fix it.
   *
   * The main logic is, if Shift + Mouse Button down, "pause" IME,
   * if Shift key up, and IME was paused, "resume" IME.
   */
  bool m_is_paused;

  /**
   * Represents whether or not there is an ongoing composition in a browser
   * process, i.e. whether or not a browser process is composing a text.
   */
  bool m_is_composing;
};

#endif /* WITH_INPUT_IME */
