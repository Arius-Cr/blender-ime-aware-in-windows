# Introduction

Hello everyone, I found that Blender still lacks support for IME on the Windows platform, so I researched improvement plans to enhance Blender's support for IME.

Input method editor (IME) is a program that assists users in inputting Chinese, Japanese, Korean, and other characters.

For specific introductions, please refer to Microsoft's official documentation:

[Input Method Editors (IME)](https://learn.microsoft.com/en-us/globalization/input/input-method-editors)
[Input Method Manager (IMM)](https://learn.microsoft.com/en-us/windows/win32/intl/input-method-manager)

Current support for IME in Blender (only consider Windows):

- The text field supports IME, but there are issues:

    - After activating the text field, simply exit the text field and press the shortcut key, such as "G", to move an object. The IME intercepts the key and pops up a candidate window, causing the user to be unable to use the program's shortcut keys normally.

        Reason for the problem: The prerequisite for calling `ui_textedit_ime_end` in `ui_textedit_end` is `win->ime_data != nullptr`, and `win->ime_data` will only not be null when the user triggers the composition process of the IME at least once. Therefore, when the user activates the text field and the program enables the IME, if the user exits the text field directly, the program will not disable the IME. This may result in the user's subsequent keystrokes being intercepted by the IME.

    - When certain keys are pressed, characters cannot be entered.

        Reason for the problem: Blender only processes key messages in `WM_INPUT`, but it cannot determine whether the key is processed by the IME in `WM_INPUT` (only `wParam == VK_PROCESSKEY` can be used to determine whether the key is processed by the IME in `WM_KEYDOWN`, etc.). The current solution is that the program assumes that certain keys must be processed by the IME, thus skipping the processing of these keys in `WM_INPUT`.

        If the program believes that a key will definitely be processed by the IME, but the IME does not process the key, then the key will not be processed by any program.

        That is to say, as long as the IME's processing logic for the key is different from that in `GHOST_ImeWin32::IsImeKeyEvent`, it will definitely result in the key being unable to input characters.

        Unfortunately, the processing logic of IMEs for keys is complex, and the processing logic of different IMEs in the same language is also different.

        Here are some specific examples of this issue, using Microsoft Pinyin IME and in Chinese conversion mode:

        - CAPS LOCK on, press the alpha key, unable to input characters.

        - Press the "/" key near the Shift key, unable to input characters.

    - The number field unexpectedly supports IME. This can cause some IMEs to interfere with user input. According to the current function of the number field, users do not need to use complex character in the number field, so disabling the IME in the number field can avoid unexpected interference from the input method.

        Reason for the problem: Blender itself has a design of "not enable IME in number field" (see `ui_textedit_begin`), but unexpectedly enabled IME in `widget_draw_text` through `ui_but_ime_reposition`.

    - Microsoft Korean IME is not supported.

        Reason for the problem: Blender copies the composition information to the `ime_data` of `wmWindow` to pass the composition information to other components, but the composition message sent by Microsoft Korean IME is too compact, causing the composition information of the latter composition message to overwrite the previous one, ultimately resulting in abnormal text input.

- Text Edit mode in 3D Viewport, Text Editor, Console does not support IME.

- When using "Type to Search", the first alpha key cannot be captured by the IME.

After improvement: 

- Text field support IME, while number field do not need to support IME.

- Text Edit mode in 3D Viewport, Text Editor, Console support IME.

- When using "Type to Search", the first alpha key can be captured by the IME.

- Good support for IME, that is, implementing the key requirements in the "Windows IME-aware Application Design Guide" (see later), including:

    - Correctly enable/disable IME.

    - Correctly handle key messages.

    - Correctly handle composition messages.

    - Correctly position the candidate window.

The following video shows the situation before and after the modification:

[ime-aware-in-windows-before-after-comparison](./files/ime-aware-in-windows.mp4)

# Modification

In order to make things as simple as possible, I have written a simplified IME-aware example program based on Microsoft's official example:

[https://github.com/Arius-Cr/wire_app_ime_aware_example](https://github.com/Arius-Cr/wire_app_ime_aware_example)

It includes a self written "Windows IME-aware Application Design Guide" based on IMM.

This guide introduces programmers to the tasks that need to be completed and followed to implement a program that supports IME.

The following text assumes that you have already read the example and the guide.

Now I will list the changes included in this modification.

The following figure lists the relationship between related functions:

![relation](./files/relation.png)

Note: All changes made to the source code this time are only for the Windows platform, so the code related to IME shared with other platforms will remain unchanged. So you will see this source code:

```
#if defined(WITH_INPUT_IME) && !defined(WIN32)
// ... The original code remains unchanged on non Windows platforms.
#endif

#if defined(WITH_INPUT_IME) && defined(WIN32)
// ... The new code on Windows platforms.
#endif
```

## 1. Enable and Disable IME

The UI of Blender belongs to the "Direct UI" category in the design guidelines, so when the keyboard focus tranfers between various controls, it is necessary to promptly switch the enable/disable state of the IME.

The `wm_window_IME_begin` function can enable the IME for the current window, while the `wm_window_IME_end` function can disable the IME for the current window.

For text field:

- When the text field enters editing mode (`ui_textedit_ime_begin`), enable IME.

- When the text field exits the editing state (`ui_textedit_ime_end`), disable IME.

The text field does not include number field (i.e. `UI_BTYPE_NUM` and `UI_BTYPE_NUM_SLIDER`).

For editor:

I have added a callback function `on_activation_changed` in `ARegionType`. When the active region (`screen->active_region`) changed, it will first call the `on_activation_changed` callback function of the region that has already lost focus, and then call the `on_activation_changed` callback function of the region that has already gained focus.

At present, only the main region of 3D Viewport, Text Editor, and Consoles will listen to the change of active region.

- When the main region of the editor gains focus, if the editor currently allows users to input text, then enable IME.

- When the editor's state changed, and the focus is on the main region, and the editor currently allows users to input text, then enable IME. Otherwise, disable IME.

Conditions for enabling IME for 3D viewport:

- In Text Edit mode.

Conditions for enabling IME for Text Editor:

- Related Text object exists.

Conditions for enabling IME for Console:

- Unrestricted, that is, as long as the main region has focus, the IME will be enabled.

Key functions:
- `ui_textedit_ime_begin`
- `ui_textedit_ime_end`
- `view3d_enable_ime`
- `view3d_disable_ime`
- `text_enable_ime`
- `text_disable_ime`
- `console_enable_ime`
- `console_disable_ime`

## 2. Process key messages

This modification has modified the key message processing flow of Blender:

- When processing the `WM_INPUT` message, if the IME is already enabled, the most of key messages are ignored and only special keys such as the OS key are processed.

- When processing messages such as `WM_KEYDOWN`, if the IME is already enabled, then:

    - If the key is processed by the IME (`wParam == VK_PROCESSKEY`), ignore it.

    - Otherwise, the key message will be processed according to the original logic of `WM_INPUT`.

Simply put, when the IME is enabled, most of the key events will be processed in `WM_KEYDOWN`, `WM_KEYUP` ..., not `WM_INPUT`.

The following diagram can more clearly show the message processing flow before and after:

![Old Key Processing With IME](./files/old_key_processing_with_ime.png)

![New Key Processing With IME](./files/new_key_processing_with_ime.png)

After this modification, Blender can accurately determine whether a certain key has been processed by IME.

Key functions:
- `GHOST_SystemWin32::s_wndProc`

## 3. Process composition messages

Under Blender's original processing logic, the following changes have been made:

- Adjust the calculation rules for the target (`target_start` and `target_end`).

    The original calculation rules need to consider the category of the input language. Actually, there is no need to consider the input language.

    The modified rule is as follows:

    - Firstly, check if there is an `ATTR_TARGET_CONVERTED` or `ATTR_TARGET_NOTCONVERTED` attribute, and if so, use it as the target.

        Suitable for Japanese IMEs and some Traditional Chinese IMEs.

    - Otherwise, check if there are clauses, and if so, use the clause where the composition cursor is located as target.

        Suitable for some Chinese IMEs.

    - Otherwise, use the entire composition string as the target.

        Suitable for Korean and other IMEs.

    The "Target" is mainly used to locate candidate window positions, but in Blender it is also used to draw composition string underline.

    However, in reality, the "Target" is not suitable for drawing composite string underline because its logic is different from the conventional one (please refer to the relevant code of the example program), but the result is also ok, so this modification does not modify the logic of underline drawing.

- Record the composition information in `GHOST_Event` and `wmEvent`.

    The original behavior was to record the composition information in the `ime_data` field of the `wmWindow`. But there is a problem with this behavior. Because Blender does not process messages individually, but in batches (if I understand correctly). When composition messages are triggered in a very compact manner, the composition information of the next composition message will overwrite the composition information of the previous composition message. When the final event handler processes the event, there may be data synchronization issues.

    This issue can be triggered through the new version of the Microsoft Korean IME, which will send the following composition message sequence:

    1. WM_IME_COMPOSITION (with GCS_RESULTSTR)
    2. WM_IME_ENDCOMPOSITION
    3. WM_IME_STARTCOMPOSITION
    4. WM_IME_COMPOSITION (with GCS_COMPSTR)

    When the first message arrives, the composition information is written to `wmWindow.ime_data`, and when the fourth message arrives, its composition information is written to `wmWindow.ime_data`, covering the composition information of the first message. However, the event handler only received the first message after the fourth message, and the composition information it obtained was from the fourth message instead of the first, resulting in information confusion.

    To avoid this issue, it is necessary to record the composition information to the `GHOST_Event` and `wmEvent` objects.

    After modification, in the Windows platform, the `ime_data` field of `wmWindow` will no longer be used at all, but instead:

    - `m_data` of `GHOST_Event`

    -  `customdata` of `wmEvent`

    - `ime_data` of `uiBut` (used for `widget_draw_text`)

- Use new operations and key bindings to make the editor (3D view, text editor, console) support IME.

    The new operations include：

    - XXX_OT_ime_input series:
        - `FONT_OT_ime_input`
        - `TEXT_OT_ime_input`
        - `CONSOLE_OT_ime_input`

    - XXX_OT_ime_insert series:
        - `FONT_OT_ime_insert`
        - `TEXT_OT_ime_insert`
        - `CONSOLE_OT_ime_insert`

    XXX_OT_ime_input will be bound to the `WM_IME_COMPOSITE_START` event。

    XXX_OT_ime_insert will be called by XXX_OT_ime_input to insert the composition result string，in order to provide Undo/Redo functionality.

    The events such as `WM_IME_COMPOSITE_START` already exist, but they have not been exported to BPY. In order to enable operations to bind and receive these events, I have exported them to BPY.

    The working process of XXX_OT_ime_input is as follows:

    - When the `WM_IME_COMPOSITE_START` event is triggered, base on key bindings, the program will trigger XXX_OT_ime_input to run in modal mode.

    - The operator listen for the subsequent `WM_IME_COMPOSITE_EVENT` and `WM_IME_COMPOSITE_END` events, and end running when `WM_IME_COMPOSITE_END` occurs.

    - During the running of operator, the result string will be inserted into the text to be edited through XXX_OT_ime_insert, while the composition string will be temporarily inserted into the text to be edited.

    - When the composition string is updated, the last inserted composition string will be deleted and a new composition string will be inserted.

    - When the operator is end, it will ensure that all temporarily inserted composition string is cleared.

    XXX_OT_ime_input will also be bound to the `WM_IME_COMPOSITE_EVENT` event. This is only done to support Microsoft Korean IME (compatibility mode). The composition message generated by this IME has a defect, as it will send a `WM_IME_COMPOSITION` message outside of the range of `WM_IME_STARTCOMPOSITION` to `WM_IME_ENDCOMPOSITION`. Therefore, it is necessary to bind the XXX_OT_ime_input to the `WM_IME_COMPOSITION` events in order to capture this escaping message. This escaping message only contains the result string.

Key functions:
- `GHOST_ImeWin32::GetCaret`
- `GHOST_SystemWin32::processImeEvent`
- `wm_event_add_ghostevent`
- `ui_do_but_textedit`
- `TEXT_OT_ime_input`
- `CONSOLE_OT_ime_input`
- `FONT_OT_ime_input`

## 4. Position the candidate window

The original behavior of Blender was to follow the position of the text cursor. This behavior will cause the candidate window to always move, and most IME users do not want the candidate window to move unnecessarily.

The modified rule is as follows:

- When there is no composition：

    - If selection exists, position the candidate window at the start of selection, which is closer to the beginning of the text.

    - Otherwise, position at the cursor in the screen.

- During composition, locate the candidate window at the position of `sel_start` in the screen.

- When locating candidate window, always exclude the rectangular area occupied by the line to avoid the candidate window covering the composition string.

Reposition timing for candidate window:

- Update candidate window positions when enabling IME;

- For text field, reposition candidate window after redraw.

- For editor：

    - When there is no composition, reposition candidate window after redraw.

    - During composition, reposition candidate window by the draw handler add by XXX_OT_ime_input according to the target (`sel_start`).

We must correctly locate the candidate window before the start of composition (`WM_IME_STARTCOMPOSITION`). Because:

- If the positioning is carried out after the composition starts, there is a certain probability that the candidate window will appear in the previous position first and then move to a new position, which will cause the window to flash and reduce the user experience.

- Some IMEs ignore candidate window reposition requests until the end of composition.

Key functions:
- `ui_but_ime_reposition`
- `text_reposition_ime_window`
- `console_reposition_ime_window`
- `ED_curve_editfont_reposition_ime_window`

## 5. Make "Type to Search" to support IME

Before modification, when using the "Type to Search" function, the character corresponding to the first pressed alpha key will be directly inserted into the search field.

After modification, the program simulates user input by sending the same alpha key through the Windows API `SendInput`. If the IME processes that key, it will enter the text composition process normally, otherwise it will produce the same character input as before.

Key functions:
- `wm_search_menu_invoke`

# The problem of NUM LOCK

Blender ignores the number lock state when processing keys on the numpad, so when editing text, the numpad cannot be used in a NUM LOCK off state.

Here are the relevant issues: https://projects.blender.org/blender/blender/issues/121998

A solution was mentioned in issues: "is it possible that we add a state switch in GHOST (like when handling IME) that in text editors we switch to a kind of mapping that does take care of numlock state?".

For this issue, if limited to the Windows platform, it can be implemented based on the current modification.

Because the current modification has implemented the following functions:

- Enter text editing state -> wm_window_IME_begin() -> GHOST_BeginIME() -> ...

- Exit text editing state -> wm_window_IME_end() -> GHOST_EndIME() -> ...

Just make some modifications to achieve:

- Enter text editing state -> wm_window_text_edit_begin() -> GHOST_BeginTextEdit() -> ...

- Exit text editing state -> wm_window_text_edit_end() -> GHOST_EndTextEdit() -> ...

So the problem mentioned in the issues can be fixed based on the current modification (limited to Windows platforms only).

This modification currently does not include any fixes to that issues.
