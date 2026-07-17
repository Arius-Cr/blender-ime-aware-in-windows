/* SPDX-FileCopyrightText: 2010 The Chromium Authors. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#pragma once

#ifdef WITH_INPUT_IME

#  include <windows.h>

#  include <string>

#  include "GHOST_Event.hh"
#  include "GHOST_Rect.hh"
#  include <vector>

class GHOST_EventIME : public GHOST_Event {
 public:
  /**
   * Constructor.
   * \param msec: The time this event was generated.
   * \param type: The type of key event.
   * \param customdata: custom-data, typically #GHOST_TEventImeData.
   */
  GHOST_EventIME(uint64_t msec,
                 GHOST_TEventType type,
                 GHOST_IWindow *window,
                 const void *customdata)
      : GHOST_Event(msec, type, window)
  {
    this->data_ = customdata;
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
 *    Call GHOST_ImeWin32::SetHwnd() to pass the window handle to GHOST_ImeWin32.
 * 2. Add messages handlers listed in the following subsections, follow the
 *    instructions written in each subsection, and use the GHOST_ImeWin32 class.
 * 2.1. WM_IME_NOTIFY (0x0282)
 *      Call the functions listed below:
 *      wParam == IMN_OPENSTATUSWINDOW:
 *      - GHOST_ImeWin32::CheckFirst()
 *      - GHOST_ImeWin32::OnWindowActivated()
 *      wParam == IMN_CLOSESTATUSWINDOW:
 *      - GHOST_ImeWin32::OnWindowDeactivated()
 *      Return ::DefWindowProc().
 * 2.2. WM_IME_SETCONTEXT (0x0281)
 *      Return GHOST_ImeWin32::OnSetContext().
 * 2.3. WM_IME_STARTCOMPOSITION (0x010D)
 *      Call the functions listed below:
 *      - GHOST_ImeWin32::OnCompositionStart()
 *      Return 0.
 * 2.4. WM_IME_COMPOSITION (0x010F)
 *      Call the functions listed below:
 *      - GHOST_ImeWin32::OnCompositionUpdate()
 *      Return 0.
 * 2.5. WM_IME_ENDCOMPOSITION (0x010E)
 *      Call the functions listed below:
 *      - GHOST_ImeWin32::OnCompositionEnd()
 *      Return 0.
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

  HWND GetHwnd()
  {
    return h_wnd_;
  }

  /**
   * GHOST_WindowWin32 call this function to bind the window handle to GHOST_ImeWin32.
   */
  void SetHwnd(HWND window_handle)
  {
    if (window_handle && !h_wnd_) {
      h_wnd_ = window_handle;
    }
  }

  void CheckFirst();

  void OnWindowActivated();

  void OnWindowDeactivated();

  LRESULT OnSetContext(UINT message, WPARAM wparam, LPARAM lparam);

  bool IsIgnoreKey(USHORT key);

  /**
   * Enable IME attached to the given window, i.e. allows user-input
   * events to be dispatched to the IME.
   */
  void BeginIME();

  /**
   * Disable the IME attached to the given window, i.e. prohibits any user-input
   * events from being dispatched to the IME.
   */
  void EndIME();

  /**
   * Check if IME is enabled.
   */
  bool IsEnabled();

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

  /**
   * Is there an ongoing composition.
   */
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
   * Move the candidate window using the previous caret_rect and exclude_rect.
   */
  void MoveIME();

  /**
   * Move the candidate window.
   * \param caret_rect: The top-left corner of the candidate window will align
   *     to the bottom-left corner of the caret.
   * \param exclude_rect: Indicate the window not to cover the specified area.
   */
  void MoveIME(const GHOST_Rect &caret_rect, const GHOST_Rect &exclude_rect);

  /**
   * Send an alphabetic key to start the IME composition.
   * \param c: The alphabetic character (a-Z).
   */
  void StartIMEComplsitionByChar(char c);

  ImeComposition resultInfo, compInfo;

  GHOST_TEventImeData eventImeData;

  /* The rectangle of the input caret retrieved from a renderer process. */
  GHOST_Rect caret_rect_;

  /* The exclude rectangle of the IME Window. */
  GHOST_Rect exclude_rect_;

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

  /**
   * Determines whether or not the given attribute represents a target (a.k.a. a selection).
   */
  bool IsTargetAttribute(char attribute) const
  {
    return (attribute == ATTR_TARGET_CONVERTED || attribute == ATTR_TARGET_NOTCONVERTED);
  }

  /* Owner window. */
  HWND h_wnd_;

  bool is_first_;

  bool is_enabled_;

  bool is_composing_;
};

#endif /* WITH_INPUT_IME */
