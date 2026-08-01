/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spseq
 */

#include <cstddef>

#include "DNA_sequence_types.h"

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "BKE_context.hh"
#include "BKE_scene.hh"

#include "SEQ_effects.hh"
#include "SEQ_relations.hh"
#include "SEQ_select.hh"
#include "SEQ_transform.hh"

#include "WM_api.hh"

#include "RNA_define.hh"

#include "UI_view2d.hh"

#include "ED_screen.hh"

#if defined(WITH_INPUT_IME) && defined(WIN32)
#  include "BKE_screen.hh"
#  include "ED_space_api.hh"
#  include "GPU_immediate.hh"
#  include "GPU_state.hh"
#  include "UI_resources.hh"
#  include "UI_view2d.hh"
#  include "wm_window.hh"
#endif

#include "printx.h"

#include "sequencer_intern.hh"

namespace blender::ed::vse {

/* -------------------------------------------------------------------- */
/** \name Text Edit Polls
 * \{ */

static bool sequencer_text_editing_poll(bContext *C)
{
  if (!sequencer_editing_initialized_and_active(C)) {
    return false;
  }

  if (ED_screen_animation_no_scrub(CTX_wm_manager(C))) {
    return false;
  }

  const Scene *scene = CTX_data_sequencer_scene(C);
  const Strip *strip = seq::select_active_get(scene);
  if (strip == nullptr || strip->type != STRIP_TYPE_TEXT ||
      !strip->intersects_frame(scene, BKE_scene_frame_get(scene)))
  {
    return false;
  }

  const TextVars *data = static_cast<TextVars *>(strip->effectdata);
  if (data == nullptr || data->runtime == nullptr || !seq::effects_can_render_text(strip)) {
    return false;
  }

  return true;
}

bool sequencer_text_editing_active_poll(bContext *C)
{
  if (!sequencer_text_editing_poll(C)) {
    return false;
  }

  const Scene *scene = CTX_data_sequencer_scene(C);
  const Strip *strip = seq::select_active_get(scene);

  return (strip->flag & SEQ_FLAG_TEXT_EDITING_ACTIVE) != 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Edit Utilities
 * \{ */

int2 strip_text_cursor_offset_to_position(const seq::TextVarsRuntime *runtime, int cursor_offset)
{
  cursor_offset = std::clamp(cursor_offset, 0, runtime->character_count);

  int2 cursor_position{0, 0};
  for (const seq::LineInfo &line : runtime->lines) {
    if (cursor_offset < line.characters.size()) {
      cursor_position.x = cursor_offset;
      break;
    }
    cursor_offset -= line.characters.size();
    cursor_position.y += 1;
  }

  cursor_position.y = std::clamp(cursor_position.y, 0, int(runtime->lines.size() - 1));
  cursor_position.x = std::clamp(
      cursor_position.x, 0, int(runtime->lines[cursor_position.y].characters.size() - 1));

  return cursor_position;
}

static const seq::CharInfo &character_at_cursor_pos_get(const seq::TextVarsRuntime *runtime,
                                                        const int2 cursor_pos)
{
  return runtime->lines[cursor_pos.y].characters[cursor_pos.x];
}

static const seq::CharInfo &character_at_cursor_offset_get(const seq::TextVarsRuntime *runtime,
                                                           const int cursor_offset)
{
  const int2 cursor_pos = strip_text_cursor_offset_to_position(runtime, cursor_offset);
  return character_at_cursor_pos_get(runtime, cursor_pos);
}

static int cursor_position_to_offset(const seq::TextVarsRuntime *runtime, int2 cursor_position)
{
  return character_at_cursor_pos_get(runtime, cursor_position).index;
}

static void text_selection_cancel(TextVars *data)
{
  data->selection_start_offset = 0;
  data->selection_end_offset = 0;
}

IndexRange strip_text_selection_range_get(const TextVars *data)
{
  /* Ensure, that selection start < selection end. */
  int sel_start_offset = data->selection_start_offset;
  int sel_end_offset = data->selection_end_offset;
  if (sel_start_offset > sel_end_offset) {
    std::swap(sel_start_offset, sel_end_offset);
  }

  return IndexRange(sel_start_offset, sel_end_offset - sel_start_offset);
}

static bool text_has_selection(const TextVars *data)
{
  return !strip_text_selection_range_get(data).is_empty();
}

static void delete_selected_text(TextVars *data)
{
  if (!text_has_selection(data)) {
    return;
  }

  seq::TextVarsRuntime *runtime = data->runtime;
  IndexRange sel_range = strip_text_selection_range_get(data);

  seq::CharInfo char_start = character_at_cursor_offset_get(runtime, sel_range.first());
  seq::CharInfo char_end = character_at_cursor_offset_get(runtime, sel_range.last());

  const int offset_start = char_start.offset;
  const int offset_end = char_end.offset + char_end.byte_length;
  BLI_assert(offset_start >= 0 && offset_end <= data->text_len_bytes);
  BLI_assert(offset_end >= 0 && offset_end <= data->text_len_bytes);
  BLI_assert(offset_start <= offset_end);
  const int remaining = data->text_len_bytes - offset_end;

  std::memmove(data->text_ptr + offset_start, data->text_ptr + offset_end, remaining + 1);
  data->text_len_bytes = offset_start + remaining;

  const int2 sel_start = strip_text_cursor_offset_to_position(runtime, sel_range.first());
  data->cursor_offset = cursor_position_to_offset(runtime, sel_start);
  text_selection_cancel(data);
}

static void text_editing_update(const bContext *C)
{
  Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  seq::relations_invalidate_cache_raw(CTX_data_sequencer_scene(C), strip);
  WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER, CTX_data_sequencer_scene(C));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select/Deselect All Text
 * \{ */

static wmOperatorStatus sequencer_text_select_all_exec(bContext *C, wmOperator * /*op*/)
{
  const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  TextVars *data = static_cast<TextVars *>(strip->effectdata);
  data->selection_start_offset = 0;
  data->selection_end_offset = data->runtime->character_count;
  text_editing_update(C);
  return OPERATOR_FINISHED;
}

void SEQUENCER_OT_text_select_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select All";
  ot->description = "Select all characters";
  ot->idname = "SEQUENCER_OT_text_select_all";

  /* API callbacks. */
  ot->exec = sequencer_text_select_all_exec;
  ot->poll = sequencer_text_editing_active_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO;
}

static wmOperatorStatus sequencer_text_deselect_all_exec(bContext *C, wmOperator * /*op*/)
{
  Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  TextVars *data = static_cast<TextVars *>(strip->effectdata);

  if (!text_has_selection(data)) {
    /* Exit edit mode, so text can be translated by mouse. */
    strip->flag &= ~SEQ_FLAG_TEXT_EDITING_ACTIVE;
  }
  else {
    text_selection_cancel(data);
  }

  text_editing_update(C);
  return OPERATOR_FINISHED;
}

void SEQUENCER_OT_text_deselect_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Deselect All";
  ot->description = "Deselect all characters";
  ot->idname = "SEQUENCER_OT_text_deselect_all";

  /* API callbacks. */
  ot->exec = sequencer_text_deselect_all_exec;
  ot->poll = sequencer_text_editing_active_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Copy Text
 * \{ */

static void text_edit_copy(const TextVars *data)
{
  const seq::TextVarsRuntime *runtime = data->runtime;
  const IndexRange selection_range = strip_text_selection_range_get(data);
  const seq::CharInfo start = character_at_cursor_offset_get(runtime, selection_range.first());
  const seq::CharInfo end = character_at_cursor_offset_get(runtime, selection_range.last());

  const int offset_start = start.offset;
  const int offset_end = end.offset + end.byte_length;
  BLI_assert(offset_start >= 0 && offset_start <= data->text_len_bytes);
  BLI_assert(offset_end >= 0 && offset_end <= data->text_len_bytes);
  BLI_assert(offset_start <= offset_end);

  const size_t len = offset_end - offset_start;
  char *buf = MEM_new_array_uninitialized<char>(len + 1, "text clipboard");
  memcpy(buf, data->text_ptr + offset_start, len);
  buf[len] = 0;
  WM_clipboard_text_set(buf, false);
  MEM_delete(buf);
}

static wmOperatorStatus sequencer_text_edit_copy_exec(bContext *C, wmOperator * /*op*/)
{
  const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  const TextVars *data = static_cast<TextVars *>(strip->effectdata);

  if (!text_has_selection(data)) {
    return OPERATOR_CANCELLED;
  }

  text_edit_copy(data);

  return OPERATOR_FINISHED;
}

void SEQUENCER_OT_text_edit_copy(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Copy Text";
  ot->description = "Copy text to clipboard";
  ot->idname = "SEQUENCER_OT_text_edit_copy";

  /* API callbacks. */
  ot->exec = sequencer_text_edit_copy_exec;
  ot->poll = sequencer_text_editing_active_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cut Text
 * \{ */

static wmOperatorStatus sequencer_text_edit_cut_exec(bContext *C, wmOperator * /*op*/)
{
  const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  TextVars *data = static_cast<TextVars *>(strip->effectdata);

  if (!text_has_selection(data)) {
    return OPERATOR_CANCELLED;
  }

  text_edit_copy(data);
  delete_selected_text(data);

  text_editing_update(C);
  return OPERATOR_FINISHED;
}

void SEQUENCER_OT_text_edit_cut(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Cut Text";
  ot->description = "Cut text to clipboard";
  ot->idname = "SEQUENCER_OT_text_edit_cut";

  /* API callbacks. */
  ot->exec = sequencer_text_edit_cut_exec;
  ot->poll = sequencer_text_editing_active_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paste Text
 * \{ */

static wmOperatorStatus sequencer_text_edit_paste_exec(bContext *C, wmOperator * /*op*/)
{
  const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  TextVars *data = static_cast<TextVars *>(strip->effectdata);
  const seq::TextVarsRuntime *runtime = data->runtime;

  int buf_len;
  char *buf = WM_clipboard_text_get(false, true, &buf_len);

  if (buf_len == 0) {
    return OPERATOR_CANCELLED;
  }

  delete_selected_text(data);
  size_t needed_size = data->text_len_bytes + buf_len + 1;
  char *new_text = MEM_new_array_uninitialized<char>(needed_size, "text");

  const seq::CharInfo cur_char = character_at_cursor_offset_get(runtime, data->cursor_offset);
  BLI_assert(cur_char.offset >= 0 && cur_char.offset <= data->text_len_bytes);
  std::memcpy(new_text, data->text_ptr, cur_char.offset);
  std::memcpy(new_text + cur_char.offset, buf, buf_len);
  std::memcpy(new_text + cur_char.offset + buf_len,
              data->text_ptr + cur_char.offset,
              data->text_len_bytes - cur_char.offset + 1);
  data->text_len_bytes += buf_len;
  MEM_delete(data->text_ptr);
  data->text_ptr = new_text;

  data->cursor_offset += BLI_strlen_utf8(buf);

  MEM_delete(buf);
  text_editing_update(C);
  return OPERATOR_FINISHED;
}

void SEQUENCER_OT_text_edit_paste(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Paste Text";
  ot->description = "Paste text from clipboard";
  ot->idname = "SEQUENCER_OT_text_edit_paste";

  /* API callbacks. */
  ot->exec = sequencer_text_edit_paste_exec;
  ot->poll = sequencer_text_editing_active_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Move Text Cursor
 * \{ */

enum {
  LINE_BEGIN,
  LINE_END,
  TEXT_BEGIN,
  TEXT_END,
  PREV_CHAR,
  NEXT_CHAR,
  PREV_WORD,
  NEXT_WORD,
  PREV_LINE,
  NEXT_LINE,
};

static const EnumPropertyItem move_type_items[] = {
    {LINE_BEGIN, "LINE_BEGIN", 0, "Line Begin", ""},
    {LINE_END, "LINE_END", 0, "Line End", ""},
    {TEXT_BEGIN, "TEXT_BEGIN", 0, "Text Begin", ""},
    {TEXT_END, "TEXT_END", 0, "Text End", ""},
    {PREV_CHAR, "PREVIOUS_CHARACTER", 0, "Previous Character", ""},
    {NEXT_CHAR, "NEXT_CHARACTER", 0, "Next Character", ""},
    {PREV_WORD, "PREVIOUS_WORD", 0, "Previous Word", ""},
    {NEXT_WORD, "NEXT_WORD", 0, "Next Word", ""},
    {PREV_LINE, "PREVIOUS_LINE", 0, "Previous Line", ""},
    {NEXT_LINE, "NEXT_LINE", 0, "Next Line", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static int2 cursor_move_by_character(int2 cursor_position,
                                     const seq::TextVarsRuntime *runtime,
                                     int offset)
{
  const seq::LineInfo &cur_line = runtime->lines[cursor_position.y];
  /* Move to next line. */
  if (cursor_position.x + offset > cur_line.characters.size() - 1 &&
      cursor_position.y < runtime->lines.size() - 1)
  {
    cursor_position.x = 0;
    cursor_position.y++;
  }
  /* Move to previous line. */
  else if (cursor_position.x + offset < 0 && cursor_position.y > 0) {
    cursor_position.y--;
    cursor_position.x = runtime->lines[cursor_position.y].characters.size() - 1;
  }
  else {
    cursor_position.x += offset;
    const int position_max = runtime->lines[cursor_position.y].characters.size() - 1;
    cursor_position.x = std::clamp(cursor_position.x, 0, position_max);
  }
  return cursor_position;
}

static int2 cursor_move_by_line(int2 cursor_position,
                                const seq::TextVarsRuntime *runtime,
                                int offset)
{
  const seq::LineInfo &cur_line = runtime->lines[cursor_position.y];
  const int cur_pos_x = cur_line.characters[cursor_position.x].position.x;

  const int line_max = runtime->lines.size() - 1;
  const int new_line_index = std::clamp(cursor_position.y + offset, 0, line_max);
  const seq::LineInfo &new_line = runtime->lines[new_line_index];

  if (cursor_position.y == new_line_index) {
    return cursor_position;
  }

  /* Find character in another line closest to current position. */
  int best_distance = std::numeric_limits<int>::max();
  int best_character_index = 0;

  for (int i : new_line.characters.index_range()) {
    seq::CharInfo character = new_line.characters[i];
    const int distance = std::abs(character.position.x - cur_pos_x);
    if (distance < best_distance) {
      best_distance = distance;
      best_character_index = i;
    }
  }

  cursor_position.x = best_character_index;
  cursor_position.y = new_line_index;
  return cursor_position;
}

static int2 cursor_move_line_end(int2 cursor_position, const seq::TextVarsRuntime *runtime)
{
  const seq::LineInfo &cur_line = runtime->lines[cursor_position.y];
  cursor_position.x = cur_line.characters.size() - 1;
  return cursor_position;
}

static bool is_whitespace_transition(char chr1, char chr2)
{
  return ELEM(chr1, ' ', '\t', '\n') && !ELEM(chr2, ' ', '\t', '\n');
}

static int2 cursor_move_prev_word(int2 cursor_position,
                                  const seq::TextVarsRuntime *runtime,
                                  const char *text_ptr)
{
  cursor_position = cursor_move_by_character(cursor_position, runtime, -1);

  while (cursor_position.x > 0 || cursor_position.y > 0) {
    const seq::CharInfo character = character_at_cursor_pos_get(runtime, cursor_position);
    const int2 prev_cursor_pos = cursor_move_by_character(cursor_position, runtime, -1);
    const seq::CharInfo prev_character = character_at_cursor_pos_get(runtime, prev_cursor_pos);

    if (is_whitespace_transition(text_ptr[prev_character.offset], text_ptr[character.offset])) {
      break;
    }
    cursor_position = prev_cursor_pos;
  }
  return cursor_position;
}

static int2 cursor_move_next_word(int2 cursor_position,
                                  const seq::TextVarsRuntime *runtime,
                                  const char *text_ptr)
{
  const int maxline = runtime->lines.size() - 1;
  const int maxchar = runtime->lines.last().characters.size() - 1;

  while ((cursor_position.x < maxchar) || (cursor_position.y < maxline)) {
    const seq::CharInfo character = character_at_cursor_pos_get(runtime, cursor_position);
    cursor_position = cursor_move_by_character(cursor_position, runtime, 1);
    const seq::CharInfo next_character = character_at_cursor_pos_get(runtime, cursor_position);

    if (is_whitespace_transition(text_ptr[next_character.offset], text_ptr[character.offset])) {
      break;
    }
  }
  return cursor_position;
}

static wmOperatorStatus sequencer_text_cursor_move_exec(bContext *C, wmOperator *op)
{
  const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  TextVars *data = static_cast<TextVars *>(strip->effectdata);
  const seq::TextVarsRuntime *runtime = data->runtime;

  if (RNA_boolean_get(op->ptr, "select_text") && !text_has_selection(data)) {
    data->selection_start_offset = data->cursor_offset;
  }

  int2 cursor_position = strip_text_cursor_offset_to_position(runtime, data->cursor_offset);

  switch (RNA_enum_get(op->ptr, "type")) {
    case PREV_CHAR:
      cursor_position = cursor_move_by_character(cursor_position, runtime, -1);
      break;
    case NEXT_CHAR:
      cursor_position = cursor_move_by_character(cursor_position, runtime, 1);
      break;
    case PREV_LINE:
      cursor_position = cursor_move_by_line(cursor_position, runtime, -1);
      break;
    case NEXT_LINE:
      cursor_position = cursor_move_by_line(cursor_position, runtime, 1);
      break;
    case LINE_BEGIN:
      cursor_position.x = 0;
      break;
    case LINE_END:
      cursor_position = cursor_move_line_end(cursor_position, runtime);
      break;
    case TEXT_BEGIN:
      cursor_position = {0, 0};
      break;
    case TEXT_END:
      cursor_position.y = runtime->lines.size() - 1;
      cursor_position = cursor_move_line_end(cursor_position, runtime);
      break;
    case PREV_WORD:
      cursor_position = cursor_move_prev_word(cursor_position, runtime, data->text_ptr);
      break;
    case NEXT_WORD:
      cursor_position = cursor_move_next_word(cursor_position, runtime, data->text_ptr);
      break;
  }

  data->cursor_offset = cursor_position_to_offset(runtime, cursor_position);
  if (RNA_boolean_get(op->ptr, "select_text")) {
    data->selection_end_offset = data->cursor_offset;
  }

  if (!RNA_boolean_get(op->ptr, "select_text") ||
      data->cursor_offset == data->selection_start_offset)
  {
    text_selection_cancel(data);
  }

  WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER, CTX_data_sequencer_scene(C));
  return OPERATOR_FINISHED;
}

void SEQUENCER_OT_text_cursor_move(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Move Cursor";
  ot->description = "Move cursor in text";
  ot->idname = "SEQUENCER_OT_text_cursor_move";

  /* API callbacks. */
  ot->exec = sequencer_text_cursor_move_exec;
  ot->poll = sequencer_text_editing_active_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_enum(ot->srna,
               "type",
               move_type_items,
               LINE_BEGIN,
               "Type",
               "Where to move cursor to, to make a selection");

  PropertyRNA *prop = RNA_def_boolean(
      ot->srna, "select_text", false, "Select Text", "Select text while moving cursor");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete Text
 * \{ */

enum { DEL_NEXT_SEL, DEL_PREV_SEL };
static const EnumPropertyItem delete_type_items[] = {
    {DEL_NEXT_SEL, "NEXT_OR_SELECTION", 0, "Next or Selection", ""},
    {DEL_PREV_SEL, "PREVIOUS_OR_SELECTION", 0, "Previous or Selection", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static void delete_character(const seq::CharInfo character, TextVars *data)
{
  const int offset_start = character.offset;
  const int offset_end = character.offset + character.byte_length;
  BLI_assert(offset_start >= 0 && offset_start <= data->text_len_bytes);
  BLI_assert(offset_end >= 0 && offset_end <= data->text_len_bytes);
  const int remaining = data->text_len_bytes - offset_end + 1;
  std::memmove(data->text_ptr + offset_start, data->text_ptr + offset_end, remaining);
  data->text_len_bytes -= character.byte_length;
  BLI_assert(data->text_len_bytes >= 0);
}

static wmOperatorStatus sequencer_text_delete_exec(bContext *C, wmOperator *op)
{
  const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  TextVars *data = static_cast<TextVars *>(strip->effectdata);
  const seq::TextVarsRuntime *runtime = data->runtime;
  const int type = RNA_enum_get(op->ptr, "type");

  if (text_has_selection(data)) {
    delete_selected_text(data);
    text_editing_update(C);
    return OPERATOR_FINISHED;
  }

  if (type == DEL_NEXT_SEL) {
    if (data->cursor_offset >= runtime->character_count) {
      return OPERATOR_CANCELLED;
    }

    delete_character(character_at_cursor_offset_get(runtime, data->cursor_offset), data);
  }
  if (type == DEL_PREV_SEL) {
    if (data->cursor_offset == 0) {
      return OPERATOR_CANCELLED;
    }

    delete_character(character_at_cursor_offset_get(runtime, data->cursor_offset - 1), data);
    data->cursor_offset -= 1;
  }

  text_editing_update(C);
  return OPERATOR_FINISHED;
}

void SEQUENCER_OT_text_delete(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Delete Text";
  ot->description = "Delete text at cursor position";
  ot->idname = "SEQUENCER_OT_text_delete";

  /* API callbacks. */
  ot->exec = sequencer_text_delete_exec;
  ot->poll = sequencer_text_editing_active_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO;

  /* properties */
  RNA_def_enum(ot->srna,
               "type",
               delete_type_items,
               DEL_NEXT_SEL,
               "Type",
               "Which part of the text to delete");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Insert Text
 * \{ */

static bool text_insert(TextVars *data, const char *buf, const size_t buf_len)
{
  BLI_assert(strlen(buf) == buf_len);
  const seq::TextVarsRuntime *runtime = data->runtime;

  delete_selected_text(data);

  size_t needed_size = data->text_len_bytes + buf_len + 1;
  char *new_text = MEM_new_array_uninitialized<char>(needed_size, "text");

  const seq::CharInfo cur_char = character_at_cursor_offset_get(runtime, data->cursor_offset);
  BLI_assert(cur_char.offset >= 0 && cur_char.offset <= data->text_len_bytes);
  std::memcpy(new_text, data->text_ptr, cur_char.offset);
  std::memcpy(new_text + cur_char.offset, buf, buf_len);
  std::memcpy(new_text + cur_char.offset + buf_len,
              data->text_ptr + cur_char.offset,
              data->text_len_bytes - cur_char.offset + 1);
  data->text_len_bytes += buf_len;
  MEM_delete(data->text_ptr);
  data->text_ptr = new_text;

  data->cursor_offset += 1;
  return true;
}

static wmOperatorStatus sequencer_text_insert_exec(bContext *C, wmOperator *op)
{
  const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  TextVars *data = static_cast<TextVars *>(strip->effectdata);

  char str[512];
  RNA_string_get(op->ptr, "string", str);

  const size_t in_buf_len = STRNLEN(str);
  if (in_buf_len == 0) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  if (!text_insert(data, str, in_buf_len)) {
    return OPERATOR_CANCELLED;
  }

  text_editing_update(C);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus sequencer_text_insert_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  char str[6];
  BLI_strncpy_utf8(str, event->utf8_buf, BLI_str_utf8_size_safe(event->utf8_buf) + 1);
  RNA_string_set(op->ptr, "string", str);
  return sequencer_text_insert_exec(C, op);
}

void SEQUENCER_OT_text_insert(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Insert Text";
  ot->description = "Insert text at cursor position";
  ot->idname = "SEQUENCER_OT_text_insert";

  /* API callbacks. */
  ot->exec = sequencer_text_insert_exec;
  ot->invoke = sequencer_text_insert_invoke;
  ot->poll = sequencer_text_editing_active_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO;

  /* properties */
  RNA_def_string(
      ot->srna, "string", nullptr, 512, "String", "String to be inserted at cursor position");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Insert Line Break
 * \{ */

static wmOperatorStatus sequencer_text_line_break_exec(bContext *C, wmOperator * /*op*/)
{
  const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  TextVars *data = static_cast<TextVars *>(strip->effectdata);

  if (!text_insert(data, "\n", 1)) {
    return OPERATOR_CANCELLED;
  }

  text_editing_update(C);
  return OPERATOR_FINISHED;
}

void SEQUENCER_OT_text_line_break(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Insert Line Break";
  ot->description = "Insert line break at cursor position";
  ot->idname = "SEQUENCER_OT_text_line_break";

  /* API callbacks. */
  ot->exec = sequencer_text_line_break_exec;
  ot->poll = sequencer_text_editing_active_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Text Cursor
 * \{ */

static int find_closest_cursor_offset(const TextVars *data, float2 mouse_loc)
{
  const seq::TextVarsRuntime *runtime = data->runtime;
  int best_cursor_offset = 0;
  float best_distance = std::numeric_limits<float>::max();

  for (const seq::LineInfo &line : runtime->lines) {
    for (const seq::CharInfo &character : line.characters) {
      const float distance = math::distance(mouse_loc, character.position);
      if (distance < best_distance) {
        best_distance = distance;
        best_cursor_offset = character.index;
      }
    }
  }

  return best_cursor_offset;
}

static void cursor_set_by_mouse_position(const bContext *C, const wmEvent *event)
{
  const Scene *scene = CTX_data_sequencer_scene(C);
  const Strip *strip = seq::select_active_get(scene);
  TextVars *data = static_cast<TextVars *>(strip->effectdata);
  const View2D *v2d = ui::view2d_fromcontext(C);

  int2 mval_region;
  WM_event_drag_start_mval(event, CTX_wm_region(C), mval_region);
  float2 mouse_loc;
  ui::view2d_region_to_view(v2d, mval_region.x, mval_region.y, &mouse_loc.x, &mouse_loc.y);

  /* Convert cursor coordinates to domain of CharInfo::position. */
  const float2 view_offs{-scene->r.xsch / 2.0f, -scene->r.ysch / 2.0f};
  const float view_aspect = scene->r.xasp / scene->r.yasp;
  float3x3 transform_mat = seq::image_transform_matrix_get(CTX_data_sequencer_scene(C), strip);
  // MSVC 2019 can't decide here for some reason, pick the template for it.
  transform_mat = math::invert<float, 3>(transform_mat);

  mouse_loc.x /= view_aspect;
  mouse_loc = math::transform_point(transform_mat, mouse_loc);
  mouse_loc -= view_offs;
  data->cursor_offset = find_closest_cursor_offset(data, float2(mouse_loc));
}

static wmOperatorStatus sequencer_text_cursor_set_modal(bContext *C,
                                                        wmOperator * /*op*/,
                                                        const wmEvent *event)
{
  const Scene *scene = CTX_data_sequencer_scene(C);
  const Strip *strip = seq::select_active_get(scene);
  TextVars *data = static_cast<TextVars *>(strip->effectdata);
  bool make_selection = false;

  switch (event->type) {
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        cursor_set_by_mouse_position(C, event);
        if (make_selection) {
          data->selection_end_offset = data->cursor_offset;
        }
        return OPERATOR_FINISHED;
      }
      break;
    case MIDDLEMOUSE:
    case RIGHTMOUSE:
      return OPERATOR_FINISHED;
    case MOUSEMOVE:
      make_selection = true;
      if (!text_has_selection(data)) {
        data->selection_start_offset = data->cursor_offset;
      }
      cursor_set_by_mouse_position(C, event);
      data->selection_end_offset = data->cursor_offset;
      break;
    default: {
      break;
    }
  }

  WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER, CTX_data_sequencer_scene(C));
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus sequencer_text_cursor_set_invoke(bContext *C,
                                                         wmOperator *op,
                                                         const wmEvent *event)
{
  const Scene *scene = CTX_data_sequencer_scene(C);
  Strip *strip = seq::select_active_get(scene);
  TextVars *data = static_cast<TextVars *>(strip->effectdata);
  const View2D *v2d = ui::view2d_fromcontext(C);

  int2 mval_region;
  WM_event_drag_start_mval(event, CTX_wm_region(C), mval_region);
  float2 mouse_loc;
  ui::view2d_region_to_view(v2d, mval_region.x, mval_region.y, &mouse_loc.x, &mouse_loc.y);

  if (!strip_point_image_isect(scene, strip, mouse_loc)) {
    strip->flag &= ~SEQ_FLAG_TEXT_EDITING_ACTIVE;
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  text_selection_cancel(data);
  cursor_set_by_mouse_position(C, event);

  WM_event_add_modal_handler(C, op);
  WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER, CTX_data_sequencer_scene(C));
  return OPERATOR_RUNNING_MODAL;
}

void SEQUENCER_OT_text_cursor_set(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Set Cursor";
  ot->description = "Set cursor position in text";
  ot->idname = "SEQUENCER_OT_text_cursor_set";

  /* API callbacks. */
  ot->invoke = sequencer_text_cursor_set_invoke;
  ot->modal = sequencer_text_cursor_set_modal;
  ot->poll = sequencer_text_editing_active_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */

  PropertyRNA *prop = RNA_def_boolean(
      ot->srna, "select_text", false, "Select Text", "Select text while moving cursor");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Edit Mode Toggle
 * \{ */

static wmOperatorStatus sequencer_text_edit_mode_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  if ((strip->flag & SEQ_FLAG_TEXT_EDITING_ACTIVE) != 0) {
    strip->flag &= ~SEQ_FLAG_TEXT_EDITING_ACTIVE;
  }
  else {
    strip->flag |= SEQ_FLAG_TEXT_EDITING_ACTIVE;
  }

  WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER, CTX_data_sequencer_scene(C));
  return OPERATOR_FINISHED;
}

void SEQUENCER_OT_text_edit_mode_toggle(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Edit Text";
  ot->description = "Toggle text editing";
  ot->idname = "SEQUENCER_OT_text_edit_mode_toggle";

  /* API callbacks. */
  ot->exec = sequencer_text_edit_mode_toggle_exec;
  ot->poll = sequencer_text_editing_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO;
}

/** \} */

#if defined(WITH_INPUT_IME) && defined(WIN32)

/* Note: Please check `TEXT_OT_ime_input` and `TEXT_OT_ime_insert` for more information. */

struct ImeInputData {
  /**
   * Example: aaaccttccbbb ('ccttcc' is the composite string, 'tt' is the composite target string)
   *     start_idx: 3
   *     end_idx: 9 (3 + 6)
   *     target_start_idx: 5
   *     target_end_idx: 7 (5 + 2)
   */

  /* The character index of the start of composite string in text */
  int start_idx;
  /* The character index of the end of composite string in text */
  int end_idx;
  /* The character index of the start of composite target string in text */
  int target_start_idx;
  /* The character index of the end of composite target string in text */
  int target_end_idx;
  ARegion *region;
  void *draw_handle;
};

void sequencer_text_edit_reposition_ime_window(
    const bContext *C, wmWindow *win, ScrArea * /*area*/, ARegion *region, void *ime_input_data)
{
  const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  const TextVars *strip_data = static_cast<TextVars *>(strip->effectdata);
  const TextVarsRuntime *text = strip_data->runtime;
  const Scene *scene = CTX_data_scene(C);
  const View2D *v2d = &region->v2d;

  int line_height = text->line_height;
  /* Add a little space to make the candidate window not too close to string. */
  int margin = 10 * UI_SCALE_FAC;

  int curl = 0;
  int curc = 0;

  int creat_l;
  int creat_b;
  int creat_h = line_height;
  int exclude_t;
  int exclude_b;

  if (ime_input_data == nullptr) {
    /**
     * If not compositing:
     * - If selection exists, locate the candidate window to the start of the selection,
     *   which is closer to the beginning of the text.
     * - Otherwise, locate to the cursor.
     */

    if (text_has_selection(strip_data)) {
      blender::int2 selection_start = strip_text_cursor_offset_to_position(
          text, strip_data->selection_start_offset);
      curl = selection_start.y;
      curc = selection_start.x;
    }
    else {
      blender::int2 selection_start = strip_text_cursor_offset_to_position(
          text, strip_data->cursor_offset);
      curl = selection_start.y;
      curc = selection_start.x;
    }
  }
  else {
    /**
     * If compositing:
     * - If target exists, locate the candidate window to the start of the target.
     * - Otherwise, locate to the start of the composite string.
     */

    ImeInputData *data = static_cast<ImeInputData *>(ime_input_data);

    if (data->target_start_idx != -1) {
      blender::int2 selection_start = strip_text_cursor_offset_to_position(text,
                                                                           data->target_start_idx);
      curl = selection_start.y;
      curc = selection_start.x;
    }
    else {
      blender::int2 selection_start = strip_text_cursor_offset_to_position(text, data->start_idx);
      curl = selection_start.y;
      curc = selection_start.x;
    }
  }

  blender::float3 creat_pos{text->lines[curl].characters[curc].position.x,
                            text->lines[curl].characters[curc].position.y,
                            0.0f};

  const blender::float3 view_offs{-scene->r.xsch / 2.0f, -scene->r.ysch / 2.0f, 0.0f};
  const float view_aspect = scene->r.xasp / scene->r.yasp;
  blender::float3x3 transform_mat = seq::image_transform_matrix_get(scene, strip);

  creat_pos += view_offs;
  creat_pos = blender::math::transform_point(transform_mat, creat_pos);
  creat_pos.x *= view_aspect;

  creat_l = creat_pos.x - v2d->cur.xmin;
  creat_b = creat_pos.y - v2d->cur.ymin;

  creat_b -= margin;
  creat_h += 2 * margin;

  exclude_t = creat_pos.y - v2d->cur.ymin + line_height;
  exclude_b = creat_pos.y - v2d->cur.ymin;
  exclude_t += margin;
  exclude_b -= margin;

  float ratio = (region->winx) / (v2d->cur.xmax - v2d->cur.xmin);
  creat_l *= ratio;
  creat_b *= ratio;
  creat_h *= ratio;
  exclude_t *= ratio;
  exclude_b *= ratio;

  creat_l += region->winrct.xmin;
  creat_b += region->winrct.ymin;
  exclude_t += region->winrct.ymin;
  exclude_b += region->winrct.ymin;

  debug_ime(CCFA "creat: %d, %d, %d, %d", creat_l, creat_b, 0, creat_h);
  debug_ime(CCFA "exclude: %d, %d, %d, %d",
            region->winrct.xmin,
            exclude_b,
            region->winrct.xmax - region->winrct.xmin,
            exclude_t - exclude_b);

  wm_window_IME_move_with_exclude(win,
                                  creat_l,
                                  creat_b,
                                  0,
                                  creat_h,
                                  region->winrct.xmin,
                                  exclude_b,
                                  region->winrct.xmax - region->winrct.xmin,
                                  exclude_t - exclude_b);
}

static bool text_insert_utf8(TextVars *data, const char *buf, int buf_len)
{
  /**
   * Copy from `sequencer_text_edit_paste_exec`.
   * Return `false` if not all characters can be inserted.
   */

  const TextVarsRuntime *text = data->runtime;

  if (buf_len == 0) {
    return true;
  }

  delete_selected_text(data);
  size_t needed_size = data->text_len_bytes + buf_len + 1;
  char *new_text = MEM_malloc_arrayN<char>(needed_size, "text");

  const seq::CharInfo cur_char = character_at_cursor_offset_get(text, data->cursor_offset);
  BLI_assert(cur_char.offset >= 0 && cur_char.offset <= data->text_len_bytes);
  std::memcpy(new_text, data->text_ptr, cur_char.offset);
  std::memcpy(new_text + cur_char.offset, buf, buf_len);
  std::memcpy(new_text + cur_char.offset + buf_len,
              data->text_ptr + cur_char.offset,
              data->text_len_bytes - cur_char.offset + 1);
  data->text_len_bytes += buf_len;
  MEM_freeN(data->text_ptr);
  data->text_ptr = new_text;

  data->cursor_offset += BLI_strlen_utf8(buf);

  return true;
}

/* -------------------------------------------------------------------- */
/** \name Handle IME Composition Events Operator
 * \{ */

/* Note: Please check `TEXT_OT_ime_input` for more information. */

static void ime_input_draw_underline(
    const bContext *C, const Strip *strip, int start_idx, int end_idx, float uheight, uint pos)
{
  /* Copy from `text_selection_draw`. */

  const TextVars *data = static_cast<TextVars *>(strip->effectdata);
  const TextVarsRuntime *text = data->runtime;
  const Scene *scene = CTX_data_scene(C);

  if (start_idx != -1 && end_idx != start_idx) {
    debug_ime(CCFA "start_idx, end_idx: %d, %d", start_idx, end_idx);

    // Use (start_idx, end_idx - 1) as `sel_range`
    const blender::IndexRange sel_range = blender::IndexRange(start_idx, end_idx - start_idx);
    const blender::int2 selection_start = strip_text_cursor_offset_to_position(text,
                                                                               sel_range.first());
    const blender::int2 selection_end = strip_text_cursor_offset_to_position(text,
                                                                             sel_range.last());
    const int line_start = selection_start.y;
    const int line_end = selection_end.y;

    debug_ime(CCFA "first, last: %lld, %lld", sel_range.first(), sel_range.last());

    debug_ime(CCFA "selection_start: %d, %d", selection_start.x, selection_start.y);
    debug_ime(CCFA "selection_end: %d, %d", selection_end.x, selection_end.y);

    for (int line_index = line_start; line_index <= line_end; line_index++) {
      const blender::seq::LineInfo line = text->lines[line_index];
      blender::seq::CharInfo character_start = line.characters.first();
      blender::seq::CharInfo character_end = line.characters.last();

      if (line_index == selection_start.y) {
        character_start = line.characters[selection_start.x];
      }
      if (line_index == selection_end.y) {
        character_end = line.characters[selection_end.x];
      }

      debug_ime(
          CCFA "character_start: %f, %f", character_start.position.x, character_start.position.y);
      debug_ime(CCFA "character_end: %f, %f", character_end.position.x, character_end.position.y);

      const float line_y = character_start.position.y + text->font_descender;

      const blender::float2 view_offs{-scene->r.xsch / 2.0f, -scene->r.ysch / 2.0f};
      const float view_aspect = scene->r.xasp / scene->r.yasp;
      blender::float3x3 transform_mat = seq::image_transform_matrix_get(scene, strip);
      blender::float4x2 selection_quad{
          {character_start.position.x, line_y},
          {character_start.position.x, line_y + text->line_height},
          {character_end.position.x + character_end.advance_x, line_y + text->line_height},
          {character_end.position.x + character_end.advance_x, line_y},
      };

      debug_ime(CCFA "view_offs: %f, %f", view_offs[0], view_offs[1]);
      debug_ime(CCFA "view_aspect: %f", view_aspect);

      for (int i : blender::IndexRange(0, 4)) {
        selection_quad[i] += view_offs;
        selection_quad[i] = blender::math::transform_point(transform_mat, selection_quad[i]);
        selection_quad[i].x *= view_aspect;
      }

      debug_ime(CCFA "selection_quad[0]: %f, %f", selection_quad[0][0], selection_quad[0][1]);
      debug_ime(CCFA "selection_quad[1]: %f, %f", selection_quad[1][0], selection_quad[1][1]);
      debug_ime(CCFA "selection_quad[2]: %f, %f", selection_quad[2][0], selection_quad[2][1]);
      debug_ime(CCFA "selection_quad[3]: %f, %f", selection_quad[3][0], selection_quad[3][1]);

      // Convert to region coordinates

      const ARegion *region = CTX_wm_region(C);
      const View2D *v2d = &region->v2d;

      debug_ime(CCFA "v2d->tot: %f, %f, %f, %f",
                v2d->tot.xmin,
                v2d->tot.ymin,
                v2d->tot.xmax,
                v2d->tot.ymax);
      debug_ime(CCFA "v2d->cur: %f, %f, %f, %f",
                v2d->cur.xmin,
                v2d->cur.ymin,
                v2d->cur.xmax,
                v2d->cur.ymax);
      debug_ime(CCFA "v2d->mask: %d, %d, %d, %d",
                v2d->mask.xmin,
                v2d->mask.ymin,
                v2d->mask.xmax,
                v2d->mask.ymax);

      float ratio = (region->winx) / (v2d->cur.xmax - v2d->cur.xmin);
      float start_x = (selection_quad[0][0] - v2d->cur.xmin) * ratio;
      float start_y = (selection_quad[0][1] - v2d->cur.ymin) * ratio;
      float end_x = (selection_quad[3][0] - v2d->cur.xmin) * ratio;
      float end_y = (selection_quad[3][1] - v2d->cur.ymin) * ratio;
      float ascent = U.pixelsize * ratio;  // Border width of text box, see `text_edit_draw_box`.

      debug_ime(CCFA "ratio: %f", ratio);
      debug_ime(CCFA "start_x: %f -> %f", selection_quad[0][0] - v2d->cur.xmin, start_x);
      debug_ime(CCFA "start_y: %f -> %f", selection_quad[0][1] - v2d->cur.ymin, start_y);
      debug_ime(CCFA "end_x: %f -> %f", selection_quad[3][0] - v2d->cur.xmin, end_x);
      debug_ime(CCFA "end_y: %f -> %f", selection_quad[3][1] - v2d->cur.ymin, end_y);
      debug_ime(CCFA "text->line_height: %d -> %f", text->line_height, text->line_height * ratio);
      debug_ime(CCFA "uheight: %f -> %f", uheight, uheight * ratio);

      immRectf(pos, start_x, start_y + ascent, end_x, end_y + ascent + uheight * ratio);
    }
  }
}

static void ime_input_draw(const bContext *C, ARegion *region, void *customdata)
{
  /** Note: `ime_input_draw` will call for all SpaceSequencer, not only the one we focusing on. */

  ImeInputData *data = static_cast<ImeInputData *>(customdata);

  if (region != data->region) {
    return;
  }

  const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  TextVars *strip_data = static_cast<TextVars *>(strip->effectdata);

  debug_ime(CCBP "SpaceText Redraw [comp]: Enable & Reposition IME");
  sequencer_text_edit_reposition_ime_window(C, CTX_wm_window(C), CTX_wm_area(C), region, data);

  uchar color[4] = {255, 255, 255, 255};
  blender::ui::theme::get_color_4ubv(TH_TEXT, color);

  GPU_blend(GPU_BLEND_ALPHA);

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  immUniformColor4ubv(color);

  debug_ime(CCFR "start_idx: %d, %d", data->start_idx, data->end_idx);
  debug_ime(CCFR "target_start_idx: %d, %d", data->target_start_idx, data->target_end_idx);
  debug_ime(CCFR "line_height: %d", strip_data->runtime->line_height);

  ime_input_draw_underline(C,
                           strip,
                           data->start_idx,
                           data->end_idx,
                           max_ff(1, strip_data->runtime->line_height * 0.04),
                           pos);

  ime_input_draw_underline(C,
                           strip,
                           data->target_start_idx,
                           data->target_end_idx,
                           max_ff(2, strip_data->runtime->line_height * 0.08),
                           pos);

  immUnbindProgram();

  GPU_blend(GPU_BLEND_NONE);
}

static void ime_input_clean(bContext * /*C*/, wmOperator *op)
{
  ImeInputData *data = static_cast<ImeInputData *>(op->customdata);
  if (data->draw_handle) {
    ED_region_draw_cb_exit(data->region->runtime->type, data->draw_handle);
  }

  MEM_freeN(data);

  op->customdata = nullptr;
}

static wmOperatorStatus ime_input_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region;

  if (event->type == WM_IME_COMPOSITE_START) {
    debug_ime("SEQUENCER_OT_ime_input: start\n");

    region = CTX_wm_region(C);

    const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
    TextVars *strip_data = static_cast<TextVars *>(strip->effectdata);

    /* Delete selection. */

    delete_selected_text(strip_data);

    /* Initialize IME input data. */

    ImeInputData *data = static_cast<ImeInputData *>(MEM_callocN(sizeof(ImeInputData), __func__));
    op->customdata = data;
    data->start_idx = strip_data->cursor_offset;
    data->end_idx = data->start_idx;
    data->target_start_idx = -1;
    data->target_end_idx = -1;

    data->region = region;
    data->draw_handle = ED_region_draw_cb_activate(
        region->runtime->type, ime_input_draw, data, REGION_DRAW_POST_PIXEL);

    text_editing_update(C);

    WM_event_add_modal_handler(C, op);
    return OPERATOR_RUNNING_MODAL;
  }
  else if (event->type == WM_IME_COMPOSITE_EVENT) {
    /* Capture the WM_IME_COMPOSITE_EVENT event that not between START and END,
     * and then insert the result string carried by the event.
     * This isolated event can occur when using the old (i.e. compatibility mode)
     * Microsoft Korean IME.
     */
    WM_operator_name_call(C, "SEQUENCER_OT_ime_insert", blender::wm::OpCallContext::InvokeRegionWin, nullptr, event);
  }

  return OPERATOR_CANCELLED;
}

static wmOperatorStatus ime_input_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  wmWindow *win;
  const wmIMEData *ime_data;
  ImeInputData *data;

  const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
  TextVars *strip_data = static_cast<TextVars *>(strip->effectdata);

  bool changed = false;

  if (ELEM(event->type, WM_IME_COMPOSITE_EVENT, WM_IME_COMPOSITE_END)) {
    win = CTX_wm_window(C);
    if (event->type == WM_IME_COMPOSITE_EVENT) {
      ime_data = static_cast<const wmIMEData *>(event->customdata);
    }
    data = static_cast<ImeInputData *>(op->customdata);

    /* Delete previous composite string. */

    if (data->end_idx != data->start_idx) {
      debug_ime(CCFG "SEQUENCER_OT_ime_input: delete previous composite string");

      strip_data->selection_start_offset = data->start_idx;
      strip_data->selection_end_offset = data->end_idx;
      strip_data->cursor_offset = data->end_idx;
      delete_selected_text(strip_data);

      data->end_idx = data->start_idx;
      data->target_start_idx = -1;
      data->target_end_idx = -1;

      changed = true;
    }
  }

  if (event->type == WM_IME_COMPOSITE_EVENT) {

    /* Insert result string. */

    if (ime_data->result.size() != 0) {
      debug_ime(CCFG "SEQUENCER_OT_ime_input: insert result string");
      debug_ime(CCFG "  result_len: %zu", ime_data->result.size());

      WM_operator_name_call(C, "SEQUENCER_OT_ime_insert", blender::wm::OpCallContext::InvokeRegionWin, nullptr, event);

      /* Reinitialize IME input data. */

      data->start_idx = strip_data->cursor_offset;
      data->end_idx = data->start_idx;
      data->target_start_idx = -1;
      data->target_end_idx = -1;
    }

    /* Insert composite string. */

    if (ime_data->composite.size() != 0) {
      debug_ime(CCFG "SEQUENCER_OT_ime_input: insert composite string");
      debug_ime(CCFG "  composite_len: %zu", ime_data->composite.size());

      bool all_insterd = text_insert_utf8(
          strip_data, ime_data->composite.c_str(), ime_data->composite.size());

      if (all_insterd) {
        data->end_idx = strip_data->cursor_offset;
        if (ime_data->sel_start != -1 && ime_data->sel_end != -1) {
          data->target_start_idx = data->start_idx +
                                   BLI_str_utf8_offset_to_index(ime_data->composite.c_str(),
                                                                ime_data->composite.size(),
                                                                ime_data->sel_start);
          data->target_end_idx = data->start_idx +
                                 BLI_str_utf8_offset_to_index(ime_data->composite.c_str(),
                                                              ime_data->composite.size(),
                                                              ime_data->sel_end);
        }
        else {
          data->target_start_idx = -1;
          data->target_end_idx = -1;
        }

        strip_data->cursor_offset = data->start_idx +
                                    BLI_str_utf8_offset_to_index(ime_data->composite.c_str(),
                                                                 ime_data->composite.size(),
                                                                 ime_data->cursor_pos);
      }
      else {
        /* Ignore target if not all characters can be inserted. */
        data->end_idx = strip_data->cursor_offset;
        data->target_start_idx = -1;
        data->target_end_idx = -1;
      }

      changed = true;
    }

    if (changed) {
      text_editing_update(C);
    }
  }

  else if (event->type == WM_IME_COMPOSITE_END) {
    debug_ime(CCFG "SEQUENCER_OT_ime_input: end");

    ime_input_clean(C, op);

    return OPERATOR_FINISHED;
  }

  else if (ISMOUSE_BUTTON(event->type)) {
    debug_ime(CCFG "SEQUENCER_OT_ime_input: MOUSE COMPLETE COMPOSITE");

    wm_window_IME_complete(CTX_wm_window(C));
  }

  return OPERATOR_RUNNING_MODAL;
}

void SEQUENCER_OT_ime_input(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "IME Input";
  ot->idname = "SEQUENCER_OT_ime_input";
  ot->description = "Handle IME composition events (Windows only)";

  /* api callbacks */
  ot->invoke = ime_input_invoke;
  ot->modal = ime_input_modal;
  ot->cancel = ime_input_clean;
  ot->poll = sequencer_text_editing_active_poll;

  /* flags */
  ot->flag = OPTYPE_INTERNAL;
}

/* -------------------------------------------------------------------- */
/** \name Insert IME Result String Operator
 * \{ */

/* Note: Please check `TEXT_OT_ime_insert` for more information. */

static wmOperatorStatus ime_insert_invoke(bContext *C, wmOperator * /*op*/, const wmEvent *event)
{
  wmWindow *win;
  const wmIMEData *ime_data;

  Object *obedit;

  if (event->type == WM_IME_COMPOSITE_EVENT) {
    debug_ime(CCFG "SEQUENCER_OT_ime_insert");

    win = CTX_wm_window(C);
    ime_data = static_cast<const wmIMEData *>(event->customdata);

    obedit = CTX_data_edit_object(C);

    if (ime_data->result.size() != 0) {

      const Strip *strip = seq::select_active_get(CTX_data_sequencer_scene(C));
      TextVars *data = static_cast<TextVars *>(strip->effectdata);

      text_insert_utf8(data, ime_data->result.c_str(), ime_data->result.size());

      text_editing_update(C);

      return OPERATOR_FINISHED;
    }
  }

  return OPERATOR_CANCELLED;
}

void SEQUENCER_OT_ime_insert(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Insert (IME)";
  ot->idname = "SEQUENCER_OT_ime_insert";
  ot->description = "Insert IME result string. (Windows only)";

  /* api callbacks */
  ot->invoke = ime_insert_invoke;
  ot->poll = sequencer_text_editing_active_poll;

  /* flags */
  ot->flag = OPTYPE_INTERNAL | OPTYPE_UNDO;
}

/** \} */

#endif /* WITH_INPUT_IME && WIN32 */

}  // namespace blender::ed::vse
