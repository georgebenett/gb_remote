#include "snake.h"
#include "battery.h"
#include "ble.h"
#include "button.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "power.h"
#include "target_config.h"
#include "throttle.h"
#include "ui_updater.h"
#include "vesc_config.h"
#include "viber.h"

#define TAG "SNAKE"
#define SNAKE_RENDER_MS 16

// Difficulty-based speed multipliers
static const uint32_t DIFFICULTY_SPEED_MS[SNAKE_DIFFICULTY_TIERS] = {
    175, // EASY: slower base tick
    145, // NORMAL: default
    100, // HARD: faster base tick
};

static snake_t snake = {0};

// External state from ui_updater
extern volatile bool splash_transition_active;
extern lv_timer_t *splash_transition_timer;

// Forward declarations
static void snake_rebuild_polyline(void);
static void snake_spawn_food(void);
static void snake_finish_game(void);
static void snake_begin_round_locked(void);
static void snake_update_menu_selection(void);
static void snake_update_difficulty_selection(void);
static void snake_show_difficulty_menu(void);
static void snake_hide_difficulty_menu(void);
static void snake_show_pause_menu(void);
static void snake_hide_pause_menu(void);
static void snake_update_pause_selection(void);
static void snake_show_game_over_menu(void);
static void snake_update_score_labels(void);
static void snake_update_hud_labels(void);
static void snake_set_hud_visible(bool visible);
static void snake_input_poll_cb(lv_timer_t *timer);
static void snake_render_cb(lv_timer_t *timer);

static int8_t snake_read_turn_input(void) {
  uint32_t input = adc_get_latest_value();
  if (input > (VESC_NEUTRAL_VALUE + SNAKE_TURN_DEADBAND)) {
    return 1;
  }
  if (input + SNAKE_TURN_DEADBAND < VESC_NEUTRAL_VALUE) {
    return -1;
  }
  return 0;
}

static void snake_menu_input_timer_cb(lv_timer_t *timer) {
  (void)timer;

  if (!snake.running) {
    return;
  }

  // Only handle input during menus that need throttle/brake navigation
  if (snake.state != SNAKE_STATE_GAME_OVER &&
      snake.state != SNAKE_STATE_DIFFICULTY_SELECT &&
      snake.state != SNAKE_STATE_PAUSED) {
    return;
  }

  int8_t turn_input = snake_read_turn_input();
  if (turn_input != 0 && snake.last_menu_turn_input == 0) {
    if (snake.state == SNAKE_STATE_DIFFICULTY_SELECT) {
      // 4-way toggle for difficulty: 0=EASY, 1=NORMAL, 2=HARD, 3=EXIT
      if (turn_input > 0) {
        if (snake.menu_selected < 3)
          snake.menu_selected++;
      } else {
        if (snake.menu_selected > 0)
          snake.menu_selected--;
      }
      snake_update_difficulty_selection();
    } else if (snake.state == SNAKE_STATE_PAUSED) {
      // 2-way toggle for pause: 0=RESUME, 1=QUIT
      snake.menu_selected = (snake.menu_selected == 0) ? 1 : 0;
      snake_update_pause_selection();
    } else {
      // 2-way toggle for game-over: 0=RETRY, 1=EXIT
      snake.menu_selected = (snake.menu_selected == 0) ? 1 : 0;
      snake_update_menu_selection();
    }
  }
  snake.last_menu_turn_input = turn_input;
}

static void snake_show_game_over_menu(void) {
  if (snake.status_label != NULL) {
    uint32_t percent_of_best = 0;
    if (snake.high_score > 0) {
      percent_of_best = (snake.score * 100) / snake.high_score;
    }

    uint32_t secs = snake.play_time_ms / 1000;
    uint32_t mins = secs / 60;
    secs = secs % 60;

    lv_label_set_text_fmt(
        snake.status_label, "Score %u\nBest %u (%u%%)\nTime %02u:%02u",
        snake.score, snake.high_score, (unsigned int)percent_of_best,
        (unsigned int)mins, (unsigned int)secs);
    lv_obj_clear_flag(snake.status_label, LV_OBJ_FLAG_HIDDEN);
  }

  snake.menu_selected = 0;
  snake.last_menu_turn_input = 0;
  snake_update_menu_selection();

  if (snake.menu_panel != NULL) {
    lv_obj_clear_flag(snake.menu_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(snake.menu_panel);
  }

  snake_set_hud_visible(false);

  if (snake.menu_input_timer != NULL) {
    lv_timer_resume(snake.menu_input_timer);
  }
}

static void snake_hide_game_over_menu(void) {
  if (snake.menu_input_timer != NULL) {
    lv_timer_pause(snake.menu_input_timer);
  }
  if (snake.status_label != NULL) {
    lv_obj_add_flag(snake.status_label, LV_OBJ_FLAG_HIDDEN);
  }
  if (snake.menu_panel != NULL) {
    lv_obj_add_flag(snake.menu_panel, LV_OBJ_FLAG_HIDDEN);
  }
  snake_set_hud_visible(true);
}

static void snake_update_menu_selection(void) {
  if (snake.menu_retry_label == NULL || snake.menu_exit_label == NULL) {
    return;
  }

  if (snake.menu_selected == 0) {
    lv_label_set_text(snake.menu_retry_label, "> RETRY");
    lv_obj_set_style_text_color(snake.menu_retry_label, lv_color_hex(0x7BFF7A),
                                0);
    lv_label_set_text(snake.menu_exit_label, "  EXIT");
    lv_obj_set_style_text_color(snake.menu_exit_label, lv_color_hex(0xE2E2E2),
                                0);
  } else {
    lv_label_set_text(snake.menu_retry_label, "  RETRY");
    lv_obj_set_style_text_color(snake.menu_retry_label, lv_color_hex(0xE2E2E2),
                                0);
    lv_label_set_text(snake.menu_exit_label, "> EXIT");
    lv_obj_set_style_text_color(snake.menu_exit_label, lv_color_hex(0x7BFF7A),
                                0);
  }
}

static void snake_load_high_score(void) {
  nvs_handle_t nvs_handle;
  uint16_t stored_high_score = 0;

  if (nvs_open(SNAKE_NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
    if (nvs_get_u16(nvs_handle, SNAKE_NVS_HIGH_SCORE_KEY, &stored_high_score) ==
        ESP_OK) {
      snake.high_score = stored_high_score;
    }
    nvs_close(nvs_handle);
  }
}

static void snake_save_high_score(void) {
  nvs_handle_t nvs_handle;

  if (nvs_open(SNAKE_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
    return;
  }

  if (nvs_set_u16(nvs_handle, SNAKE_NVS_HIGH_SCORE_KEY, snake.high_score) ==
      ESP_OK) {
    nvs_commit(nvs_handle);
  }
  nvs_close(nvs_handle);
}

static void snake_update_score_labels(void) {
  if (snake.score_label != NULL) {
    lv_label_set_text_fmt(snake.score_label, "SCORE %u", snake.score);
  }
  if (snake.best_label != NULL) {
    lv_label_set_text_fmt(snake.best_label, "Best: %u", snake.high_score);
  }
}

static void snake_set_hud_visible(bool visible) {
  lv_obj_t *hud[] = {
      snake.score_label,
  };
  for (int i = 0; i < (int)(sizeof(hud) / sizeof(hud[0])); i++) {
    if (hud[i] == NULL)
      continue;
    if (visible) {
      lv_obj_clear_flag(hud[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(hud[i]);
    } else {
      lv_obj_add_flag(hud[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void snake_update_hud_labels(void) {
  if (snake.speed_label != NULL) {
    uint32_t base_speed = DIFFICULTY_SPEED_MS[snake.difficulty];
    uint32_t current_speed = base_speed - snake.score * 2;
    if (current_speed < SNAKE_TICK_MS_MIN) {
      current_speed = SNAKE_TICK_MS_MIN;
    }
    lv_label_set_text_fmt(snake.speed_label, "SPD %ums",
                          (unsigned int)current_speed);
  }

  if (snake.time_label != NULL) {
    uint32_t secs = snake.play_time_ms / 1000;
    uint32_t mins = secs / 60;
    secs = secs % 60;
    lv_label_set_text_fmt(snake.time_label, "%02u:%02u", (unsigned int)mins,
                          (unsigned int)secs);
  }

  if (snake.combo_label != NULL && snake.combo_counter > 0) {
    lv_label_set_text_fmt(snake.combo_label, "Combo x%u", snake.combo_counter);
  }
}

static bool snake_cell_hits_body(int16_t x, int16_t y, uint16_t body_count) {
  for (uint16_t i = 0; i < body_count; i++) {
    if (snake.cells[i].x == x && snake.cells[i].y == y) {
      return true;
    }
  }
  return false;
}

// Check if a grid cell's center falls inside a clipped rounded corner.
// Returns true if the cell is outside the visible rounded-rect area.
static bool snake_cell_in_corner(int16_t gx, int16_t gy) {
  // Playfield pixel dimensions
  int16_t fw = (int16_t)snake.grid_w * snake.cell_w;
  int16_t fh = (int16_t)snake.grid_h * snake.cell_h;
  int16_t r = 35; // must match playfield radius

  // Cell center in pixel coords
  int16_t cx = gx * snake.cell_w + snake.cell_w / 2;
  int16_t cy = gy * snake.cell_h + snake.cell_h / 2;

  // Only cells near a corner need the circle test
  bool near_left = (cx < r);
  bool near_right = (cx > fw - r);
  bool near_top = (cy < r);
  bool near_bot = (cy > fh - r);

  if (!(near_left || near_right) || !(near_top || near_bot)) {
    return false; // not in any corner zone
  }

  // Find the circle center for this corner
  int16_t ox = near_left ? r : (fw - r);
  int16_t oy = near_top ? r : (fh - r);

  int32_t dx = cx - ox;
  int32_t dy = cy - oy;
  return (dx * dx + dy * dy) > ((int32_t)r * r);
}

static void snake_spawn_food(void) {
  if (snake.length >= SNAKE_MAX_SEGMENTS) {
    snake_finish_game();
    return;
  }

  while (1) {
    int16_t food_x = (int16_t)(esp_random() % snake.grid_w);
    int16_t food_y = (int16_t)(esp_random() % snake.grid_h);
    if (!snake_cell_hits_body(food_x, food_y, snake.length) &&
        !snake_cell_in_corner(food_x, food_y)) {
      snake.food.x = food_x;
      snake.food.y = food_y;
      break;
    }
  }

  // 10% chance for bonus food
  snake.food_quality =
      (esp_random() % 10 == 0) ? SNAKE_FOOD_BONUS : SNAKE_FOOD_NORMAL;
  snake.food_pulse_offset = 0;
  snake.food_pulse_growing = true;

  if (snake.food_obj != NULL) {
    lv_obj_clear_flag(snake.food_obj, LV_OBJ_FLAG_HIDDEN);
    int16_t dot_size = LV_MIN(snake.cell_w, snake.cell_h) - 2;
    if (dot_size < 2) {
      dot_size = 2;
    }
    lv_obj_set_size(snake.food_obj, dot_size, dot_size);
    lv_obj_set_pos(snake.food_obj,
                   snake.food.x * snake.cell_w + (snake.cell_w - dot_size) / 2,
                   snake.food.y * snake.cell_h + (snake.cell_h - dot_size) / 2);

    // Bonus food is gold, normal is red
    if (snake.food_quality == SNAKE_FOOD_BONUS) {
      lv_obj_set_style_bg_color(snake.food_obj, lv_color_hex(0xFFD700), 0);
    } else {
      lv_obj_set_style_bg_color(snake.food_obj, lv_color_hex(0xFF5C5C), 0);
    }
  }
}

static void snake_rebuild_polyline(void) {
  for (uint16_t i = 0; i < snake.length; i++) {
    snake.points[i].x = snake.cells[i].x * snake.cell_w + snake.cell_w / 2;
    snake.points[i].y = snake.cells[i].y * snake.cell_h + snake.cell_h / 2;
  }
}

static void snake_turn_relative(int8_t turn) {
  if (turn < 0) {
    switch (snake.direction) {
    case SNAKE_DIR_UP:
      snake.direction = SNAKE_DIR_LEFT;
      break;
    case SNAKE_DIR_LEFT:
      snake.direction = SNAKE_DIR_DOWN;
      break;
    case SNAKE_DIR_DOWN:
      snake.direction = SNAKE_DIR_RIGHT;
      break;
    case SNAKE_DIR_RIGHT:
      snake.direction = SNAKE_DIR_UP;
      break;
    }
  } else if (turn > 0) {
    switch (snake.direction) {
    case SNAKE_DIR_UP:
      snake.direction = SNAKE_DIR_RIGHT;
      break;
    case SNAKE_DIR_RIGHT:
      snake.direction = SNAKE_DIR_DOWN;
      break;
    case SNAKE_DIR_DOWN:
      snake.direction = SNAKE_DIR_LEFT;
      break;
    case SNAKE_DIR_LEFT:
      snake.direction = SNAKE_DIR_UP;
      break;
    }
  }
}

static void snake_finish_game(void) {
  snake.game_over = true;
  snake.state = SNAKE_STATE_GAME_OVER;

  if (snake.tick_timer != NULL) {
    lv_timer_pause(snake.tick_timer);
  }
  if (snake.time_update_timer != NULL) {
    lv_timer_pause(snake.time_update_timer);
  }
  if (snake.food_pulse_timer != NULL) {
    lv_timer_pause(snake.food_pulse_timer);
  }
  if (snake.input_poll_timer != NULL) {
    lv_timer_pause(snake.input_poll_timer);
  }
  if (snake.render_timer != NULL) {
    lv_timer_pause(snake.render_timer);
  }

  if (snake.score > snake.high_score) {
    snake.high_score = snake.score;
  }
  snake_save_high_score();
  snake_update_score_labels();
  snake_show_game_over_menu();

  // Haptic feedback on loss — non-blocking pattern
  viber_play_pattern(VIBER_PATTERN_ERROR);
}

static void snake_time_update_cb(lv_timer_t *timer) {
  (void)timer;
  if (!snake.running || snake.paused || snake.game_over) {
    return;
  }

  uint64_t now_ms = esp_timer_get_time() / 1000ULL;
  snake.play_time_ms = (uint32_t)(now_ms - snake.play_start_time);
  snake_update_hud_labels();
}

static void snake_food_pulse_cb(lv_timer_t *timer) {
  (void)timer;
  if (!snake.running || snake.food_obj == NULL) {
    return;
  }

  int16_t base_size = LV_MIN(snake.cell_w, snake.cell_h) - 2;
  if (base_size < 2) {
    base_size = 2;
  }

  if (snake.food_pulse_growing) {
    snake.food_pulse_offset += 2;
    if (snake.food_pulse_offset >= 4) {
      snake.food_pulse_growing = false;
    }
  } else {
    snake.food_pulse_offset -= 2;
    if (snake.food_pulse_offset <= 0) {
      snake.food_pulse_offset = 0;
      snake.food_pulse_growing = true;
    }
  }

  int16_t new_size = base_size + snake.food_pulse_offset;
  if (new_size < 2) {
    new_size = 2;
  }
  lv_obj_set_size(snake.food_obj, new_size, new_size);

  // Center food with new size
  int16_t offset = (snake.cell_w - new_size) / 2;
  lv_obj_set_x(snake.food_obj, snake.food.x * snake.cell_w + offset);
  lv_obj_set_y(snake.food_obj, snake.food.y * snake.cell_h + offset);
}

static void snake_input_poll_cb(lv_timer_t *timer) {
  (void)timer;
  if (!snake.running || snake.game_over || snake.paused) {
    return;
  }

  int8_t turn_input = snake_read_turn_input();
  if (turn_input != 0 && snake.last_turn_input == 0) {
    if (snake.turn_queue_len < SNAKE_TURN_QUEUE_SIZE) {
      snake.turn_queue[snake.turn_queue_len++] = turn_input;
    }
  }
  snake.last_turn_input = turn_input;
}

static void snake_render_cb(lv_timer_t *timer) {
  (void)timer;
  if (!snake.running || snake.game_over || snake.paused) {
    return;
  }
  if (snake.snake_line == NULL || snake.length == 0) {
    return;
  }

  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
  uint32_t elapsed = now_ms - snake.last_tick_wall_ms;
  uint32_t period =
      snake.tick_period_ms > 0 ? snake.tick_period_ms : SNAKE_TICK_MS_BASE;
  if (elapsed > period) {
    elapsed = period;
  }

  for (uint16_t i = 0; i < snake.length; i++) {
    snake.interp_pts[i] = snake.points[i];
  }

  // Slide head forward from previous position to current
  snake.interp_pts[0].x =
      (int16_t)(snake.prev_head_px.x +
                (int32_t)(snake.points[0].x - snake.prev_head_px.x) *
                    (int32_t)elapsed / (int32_t)period);
  snake.interp_pts[0].y =
      (int16_t)(snake.prev_head_px.y +
                (int32_t)(snake.points[0].y - snake.prev_head_px.y) *
                    (int32_t)elapsed / (int32_t)period);

  // Retract tail by adding an extra point beyond the current tail.
  // interp_pts[length-1] stays at the corner (points[length-1]) so any
  // 90-degree bend there is preserved. The extra interp_pts[length] slides from
  // prev_tail_px toward that corner, retracting along the correct axis.
  uint16_t render_len = snake.length;
  if (!snake.last_move_grew && snake.length > 1 &&
      snake.length < SNAKE_MAX_SEGMENTS) {
    snake.interp_pts[snake.length].x =
        (int16_t)(snake.prev_tail_px.x +
                  (int32_t)(snake.points[snake.length - 1].x -
                            snake.prev_tail_px.x) *
                      (int32_t)elapsed / (int32_t)period);
    snake.interp_pts[snake.length].y =
        (int16_t)(snake.prev_tail_px.y +
                  (int32_t)(snake.points[snake.length - 1].y -
                            snake.prev_tail_px.y) *
                      (int32_t)elapsed / (int32_t)period);
    render_len = snake.length + 1;
  }

  lv_line_set_points(snake.snake_line, snake.interp_pts, render_len);
}

static void snake_tick_cb(lv_timer_t *timer) {
  (void)timer;

  if (!snake.running || snake.game_over) {
    return;
  }

  // Keep the HUD score visibly "live" while the game is running.
  snake_update_score_labels();

  // Dequeue one buffered turn per tick
  if (snake.turn_queue_len > 0) {
    snake_turn_relative(snake.turn_queue[0]);
    // Shift remaining entries
    for (uint8_t i = 1; i < snake.turn_queue_len; i++) {
      snake.turn_queue[i - 1] = snake.turn_queue[i];
    }
    snake.turn_queue_len--;
  }

  int16_t next_x = snake.cells[0].x;
  int16_t next_y = snake.cells[0].y;

  switch (snake.direction) {
  case SNAKE_DIR_UP:
    next_y--;
    break;
  case SNAKE_DIR_RIGHT:
    next_x++;
    break;
  case SNAKE_DIR_DOWN:
    next_y++;
    break;
  case SNAKE_DIR_LEFT:
    next_x--;
    break;
  }

  if (next_x < 0 || next_x >= (int16_t)snake.grid_w || next_y < 0 ||
      next_y >= (int16_t)snake.grid_h || snake_cell_in_corner(next_x, next_y)) {
    snake_finish_game();
    return;
  }

  bool grows = (next_x == snake.food.x && next_y == snake.food.y);
  uint16_t body_to_check = snake.length - (grows ? 0 : 1);

  if (snake_cell_hits_body(next_x, next_y, body_to_check)) {
    snake_finish_game();
    return;
  }

  // Save positions for smooth interpolation before cells shift
  snake.prev_head_px = snake.points[0];
  if (!grows && snake.length > 1) {
    snake.prev_tail_px = snake.points[snake.length - 1];
  }
  snake.last_move_grew = grows;
  snake.last_tick_wall_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

  uint16_t move_length = snake.length;
  if (grows && snake.length < SNAKE_MAX_SEGMENTS) {
    snake.length++;
    move_length = snake.length;
  }

  for (uint16_t i = move_length - 1; i > 0; i--) {
    snake.cells[i] = snake.cells[i - 1];
  }
  snake.cells[0].x = next_x;
  snake.cells[0].y = next_y;

  if (grows) {
    uint16_t points = (snake.food_quality == SNAKE_FOOD_BONUS) ? 2 : 1;
    snake.score += points;
    snake.combo_counter++;

    snake_update_score_labels();
    snake_spawn_food();

    // Haptic feedback on food eaten — non-blocking pattern
    if (snake.food_quality == SNAKE_FOOD_BONUS) {
      viber_play_pattern(VIBER_PATTERN_DOUBLE_SHORT);
    } else {
      viber_play_pattern(VIBER_PATTERN_VERY_SHORT);
    }

    if (snake.tick_timer != NULL) {
      uint32_t base_speed = DIFFICULTY_SPEED_MS[snake.difficulty];
      uint32_t next_period = base_speed - snake.score * 2;
      if (next_period < SNAKE_TICK_MS_MIN) {
        next_period = SNAKE_TICK_MS_MIN;
      }
      lv_timer_set_period(snake.tick_timer, next_period);
      snake.tick_period_ms = next_period;
    }
  }

  snake_rebuild_polyline();
}

static void snake_update_difficulty_selection(void) {
  if (snake.diff_easy_label == NULL || snake.diff_normal_label == NULL ||
      snake.diff_hard_label == NULL || snake.diff_exit_label == NULL) {
    return;
  }

  static const char *labels[] = {"EASY", "NORMAL", "HARD", "EXIT"};
  lv_obj_t *label_objs[] = {
      snake.diff_easy_label,
      snake.diff_normal_label,
      snake.diff_hard_label,
      snake.diff_exit_label,
  };

  for (int i = 0; i < 4; i++) {
    if (i == snake.menu_selected) {
      lv_label_set_text_fmt(label_objs[i], "> %s", labels[i]);
      lv_color_t color =
          (i == 3) ? lv_color_hex(0xFF6B6B) : lv_color_hex(0x7BFF7A);
      lv_obj_set_style_text_color(label_objs[i], color, 0);
    } else {
      lv_label_set_text_fmt(label_objs[i], "  %s", labels[i]);
      lv_obj_set_style_text_color(label_objs[i], lv_color_hex(0xE2E2E2), 0);
    }
  }
}

static void snake_show_difficulty_menu(void) {
  if (snake.difficulty_panel == NULL) {
    snake.difficulty_panel = lv_obj_create(snake.screen);
    lv_obj_set_size(snake.difficulty_panel, LV_PCT(78), LV_PCT(60));
    lv_obj_align(snake.difficulty_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(snake.difficulty_panel, lv_color_hex(0x111111),
                              0);
    lv_obj_set_style_bg_opa(snake.difficulty_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(snake.difficulty_panel, 2, 0);
    lv_obj_set_style_border_color(snake.difficulty_panel,
                                  lv_color_hex(0x7BFF7A), 0);
    lv_obj_set_style_radius(snake.difficulty_panel, 6, 0);
    lv_obj_set_style_pad_all(snake.difficulty_panel, 8, 0);
    lv_obj_clear_flag(snake.difficulty_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(snake.difficulty_panel, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title = lv_label_create(snake.difficulty_panel);
    lv_label_set_text(title, "SELECT DIFFICULTY");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    snake.diff_easy_label = lv_label_create(snake.difficulty_panel);
    lv_obj_align(snake.diff_easy_label, LV_ALIGN_LEFT_MID, 14, -24);

    snake.diff_normal_label = lv_label_create(snake.difficulty_panel);
    lv_obj_align(snake.diff_normal_label, LV_ALIGN_LEFT_MID, 14, -4);

    snake.diff_hard_label = lv_label_create(snake.difficulty_panel);
    lv_obj_align(snake.diff_hard_label, LV_ALIGN_LEFT_MID, 14, 16);

    snake.diff_exit_label = lv_label_create(snake.difficulty_panel);
    lv_obj_align(snake.diff_exit_label, LV_ALIGN_LEFT_MID, 14, 36);

    lv_obj_t *hint = lv_label_create(snake.difficulty_panel);
    lv_label_set_text(hint, "Throttle/Brake: Select  Power: Confirm");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xCFCFCF), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
  }

  snake.state = SNAKE_STATE_DIFFICULTY_SELECT;
  snake.menu_selected = 1; // DEFAULT: NORMAL
  snake.last_menu_turn_input = 0;

  lv_obj_clear_flag(snake.difficulty_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(snake.difficulty_panel);
  snake_update_difficulty_selection();

  snake_set_hud_visible(false);

  if (snake.menu_input_timer != NULL) {
    lv_timer_resume(snake.menu_input_timer);
  }
}

static void snake_hide_difficulty_menu(void) {
  if (snake.difficulty_panel != NULL) {
    lv_obj_add_flag(snake.difficulty_panel, LV_OBJ_FLAG_HIDDEN);
  }
  if (snake.menu_input_timer != NULL) {
    lv_timer_pause(snake.menu_input_timer);
  }
  snake_set_hud_visible(true);
}

static void snake_show_pause_menu(void) {
  snake.paused = true;
  snake.state = SNAKE_STATE_PAUSED;

  if (snake.pause_panel == NULL) {
    snake.pause_panel = lv_obj_create(snake.screen);
    lv_obj_set_size(snake.pause_panel, LV_PCT(78), LV_PCT(35));
    lv_obj_align(snake.pause_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(snake.pause_panel, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(snake.pause_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(snake.pause_panel, 2, 0);
    lv_obj_set_style_border_color(snake.pause_panel, lv_color_hex(0x7BFF7A), 0);
    lv_obj_set_style_radius(snake.pause_panel, 6, 0);
    lv_obj_set_style_pad_all(snake.pause_panel, 8, 0);
    lv_obj_clear_flag(snake.pause_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(snake.pause_panel);
    lv_label_set_text(title, "PAUSED");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    snake.pause_resume_label = lv_label_create(snake.pause_panel);
    lv_obj_set_style_text_color(snake.pause_resume_label,
                                lv_color_hex(0x7BFF7A), 0);
    lv_obj_align(snake.pause_resume_label, LV_ALIGN_BOTTOM_MID, 0, -16);

    snake.pause_quit_label = lv_label_create(snake.pause_panel);
    lv_obj_set_style_text_color(snake.pause_quit_label, lv_color_hex(0xE2E2E2),
                                0);
    lv_obj_align(snake.pause_quit_label, LV_ALIGN_BOTTOM_MID, 0, -2);
  }

  lv_obj_clear_flag(snake.pause_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(snake.pause_panel);

  snake.menu_selected = 0;
  snake.last_menu_turn_input = 0;
  snake_update_pause_selection();

  if (snake.render_timer != NULL) {
    lv_timer_pause(snake.render_timer);
  }
  snake_set_hud_visible(false);

  if (snake.menu_input_timer != NULL) {
    lv_timer_resume(snake.menu_input_timer);
  }
}

static void snake_hide_pause_menu(void) {
  if (snake.pause_panel != NULL) {
    lv_obj_add_flag(snake.pause_panel, LV_OBJ_FLAG_HIDDEN);
  }
  if (snake.menu_input_timer != NULL) {
    lv_timer_pause(snake.menu_input_timer);
  }
  snake.paused = false;
  snake.state = SNAKE_STATE_PLAYING;
  snake_set_hud_visible(true);

  // Resume game timers
  if (snake.tick_timer != NULL) {
    lv_timer_resume(snake.tick_timer);
  }
  if (snake.time_update_timer != NULL) {
    lv_timer_resume(snake.time_update_timer);
  }
  if (snake.food_pulse_timer != NULL) {
    lv_timer_resume(snake.food_pulse_timer);
  }
  if (snake.input_poll_timer != NULL) {
    lv_timer_resume(snake.input_poll_timer);
  }
  snake.last_tick_wall_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
  if (snake.render_timer != NULL) {
    lv_timer_resume(snake.render_timer);
  }
}

static void snake_update_pause_selection(void) {
  if (snake.pause_resume_label == NULL || snake.pause_quit_label == NULL) {
    return;
  }

  if (snake.menu_selected == 0) {
    lv_label_set_text(snake.pause_resume_label, "> RESUME");
    lv_obj_set_style_text_color(snake.pause_resume_label,
                                lv_color_hex(0x7BFF7A), 0);
    lv_label_set_text(snake.pause_quit_label, "  QUIT");
    lv_obj_set_style_text_color(snake.pause_quit_label, lv_color_hex(0xE2E2E2),
                                0);
  } else {
    lv_label_set_text(snake.pause_resume_label, "  RESUME");
    lv_obj_set_style_text_color(snake.pause_resume_label,
                                lv_color_hex(0xE2E2E2), 0);
    lv_label_set_text(snake.pause_quit_label, "> QUIT");
    lv_obj_set_style_text_color(snake.pause_quit_label, lv_color_hex(0x7BFF7A),
                                0);
  }
}

static void snake_build_screen_if_needed(void) {
  if (snake.screen != NULL) {
    return;
  }

  snake.screen = lv_obj_create(NULL);
  lv_obj_set_pos(snake.screen, 0, 0);
  lv_obj_set_size(snake.screen, lv_obj_get_width(ui_get_home_screen()),
                  lv_obj_get_height(ui_get_home_screen()));
  lv_obj_set_style_bg_color(snake.screen, lv_color_hex(0x0A0A0A), 0);
  lv_obj_set_style_bg_opa(snake.screen, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(snake.screen, 0, 0);
  lv_obj_set_style_pad_all(snake.screen, 0, 0);
  lv_obj_clear_flag(snake.screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(snake.screen, LV_SCROLLBAR_MODE_OFF);

  snake.score_label = lv_label_create(snake.screen);
  lv_obj_set_style_text_color(snake.score_label, lv_color_hex(0x7BFF7A), 0);
  lv_obj_set_style_text_font(snake.score_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(snake.score_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(snake.score_label, LV_ALIGN_TOP_MID, 0, 32);

  snake.playfield = lv_obj_create(snake.screen);
  lv_obj_set_style_bg_color(snake.playfield, lv_color_hex(0x111111), 0);
  lv_obj_set_style_bg_opa(snake.playfield, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(snake.playfield, 2, 0);
  lv_obj_set_style_border_color(snake.playfield, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_opa(snake.playfield, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(snake.playfield, 35, 0);
  lv_obj_set_style_pad_all(snake.playfield, 0, 0);
  lv_obj_clear_flag(snake.playfield, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(snake.playfield, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_clip_corner(snake.playfield, true, 0);
  lv_obj_set_pos(snake.playfield, 0, SNAKE_BORDER_INSET_PX);
  lv_obj_set_size(snake.playfield, lv_obj_get_width(snake.screen),
                  lv_obj_get_height(snake.screen) -
                      (2 * SNAKE_BORDER_INSET_PX));

  snake.snake_line = lv_line_create(snake.playfield);
  lv_obj_set_style_line_color(snake.snake_line, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_line_opa(snake.snake_line, LV_OPA_COVER, 0);
  lv_obj_set_style_line_rounded(snake.snake_line, true, 0);

  snake.food_obj = lv_obj_create(snake.playfield);
  lv_obj_set_style_bg_color(snake.food_obj, lv_color_hex(0xFF5C5C), 0);
  lv_obj_set_style_bg_opa(snake.food_obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(snake.food_obj, 0, 0);
  lv_obj_set_style_radius(snake.food_obj, 2, 0);
  lv_obj_clear_flag(snake.food_obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(snake.food_obj, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_flag(snake.snake_line, LV_OBJ_FLAG_HIDDEN);

  snake.status_label = lv_label_create(snake.screen);
  lv_obj_set_style_text_color(snake.status_label, lv_color_hex(0xFFFFFF), 0);
  lv_label_set_long_mode(snake.status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(snake.status_label, LV_PCT(90));
  lv_obj_set_style_text_align(snake.status_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(snake.status_label, LV_ALIGN_TOP_MID, 0, 28);
  lv_obj_add_flag(snake.status_label, LV_OBJ_FLAG_HIDDEN);

  snake.menu_panel = lv_obj_create(snake.screen);
  lv_obj_set_size(snake.menu_panel, LV_PCT(78), LV_PCT(55));
  lv_obj_align(snake.menu_panel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(snake.menu_panel, lv_color_hex(0x111111), 0);
  lv_obj_set_style_bg_opa(snake.menu_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(snake.menu_panel, 2, 0);
  lv_obj_set_style_border_color(snake.menu_panel, lv_color_hex(0x7BFF7A), 0);
  lv_obj_set_style_radius(snake.menu_panel, 6, 0);
  lv_obj_set_style_pad_all(snake.menu_panel, 8, 0);
  lv_obj_clear_flag(snake.menu_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(snake.menu_panel, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(snake.menu_panel, LV_OBJ_FLAG_HIDDEN);

  lv_obj_set_parent(snake.status_label, snake.menu_panel);

  lv_obj_t *title = lv_label_create(snake.menu_panel);
  lv_label_set_text(title, "GAME OVER");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  snake.menu_retry_label = lv_label_create(snake.menu_panel);
  lv_obj_align(snake.menu_retry_label, LV_ALIGN_LEFT_MID, 14, 8);

  snake.menu_exit_label = lv_label_create(snake.menu_panel);
  lv_obj_align(snake.menu_exit_label, LV_ALIGN_LEFT_MID, 14, 34);

  snake.menu_hint_label = lv_label_create(snake.menu_panel);
  lv_label_set_text(snake.menu_hint_label,
                    "Throttle/Brake: Select\nPower: Enter");
  lv_obj_set_style_text_color(snake.menu_hint_label, lv_color_hex(0xCFCFCF), 0);
  lv_obj_set_style_text_align(snake.menu_hint_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(snake.menu_hint_label, LV_PCT(100));
  lv_obj_align(snake.menu_hint_label, LV_ALIGN_BOTTOM_MID, 0, -2);

  lv_obj_update_layout(snake.screen);
  int16_t field_w = lv_obj_get_content_width(snake.playfield);
  int16_t field_h = lv_obj_get_content_height(snake.playfield);

  snake.grid_w = field_w / SNAKE_CELL_TARGET_PX;
  snake.grid_h = field_h / SNAKE_CELL_TARGET_PX;
  if (snake.grid_w < SNAKE_GRID_MIN_W) {
    snake.grid_w = SNAKE_GRID_MIN_W;
  }
  if (snake.grid_h < SNAKE_GRID_MIN_H) {
    snake.grid_h = SNAKE_GRID_MIN_H;
  }

  while ((uint32_t)snake.grid_w * (uint32_t)snake.grid_h > SNAKE_MAX_SEGMENTS) {
    if (snake.grid_h > snake.grid_w) {
      snake.grid_h--;
    } else {
      snake.grid_w--;
    }
  }

  if (snake.grid_w < 2) {
    snake.grid_w = 2;
  }
  if (snake.grid_h < 2) {
    snake.grid_h = 2;
  }

  snake.cell_w = field_w / (int16_t)snake.grid_w;
  snake.cell_h = field_h / (int16_t)snake.grid_h;
  if (snake.cell_w < 2) {
    snake.cell_w = 2;
  }
  if (snake.cell_h < 2) {
    snake.cell_h = 2;
  }

  lv_obj_set_size(snake.snake_line, field_w, field_h);
  lv_obj_set_style_line_width(
      snake.snake_line, LV_MAX(2, LV_MIN(snake.cell_w, snake.cell_h) - 2), 0);

  lv_obj_move_foreground(snake.score_label);

  if (snake.menu_input_timer == NULL) {
    snake.menu_input_timer = lv_timer_create(snake_menu_input_timer_cb,
                                             SNAKE_MENU_INPUT_POLL_MS, NULL);
  }
  lv_timer_pause(snake.menu_input_timer);

  if (snake.tick_timer == NULL) {
    snake.tick_timer = lv_timer_create(snake_tick_cb, SNAKE_TICK_MS_BASE, NULL);
  }
  lv_timer_pause(snake.tick_timer);

  if (snake.time_update_timer == NULL) {
    snake.time_update_timer =
        lv_timer_create(snake_time_update_cb, SNAKE_TIME_UPDATE_MS, NULL);
  }
  lv_timer_pause(snake.time_update_timer);

  if (snake.food_pulse_timer == NULL) {
    snake.food_pulse_timer =
        lv_timer_create(snake_food_pulse_cb, SNAKE_FOOD_PULSE_FREQ_MS, NULL);
  }
  lv_timer_pause(snake.food_pulse_timer);

  if (snake.input_poll_timer == NULL) {
    snake.input_poll_timer =
        lv_timer_create(snake_input_poll_cb, SNAKE_INPUT_POLL_MS, NULL);
  }
  lv_timer_pause(snake.input_poll_timer);

  if (snake.render_timer == NULL) {
    snake.render_timer =
        lv_timer_create(snake_render_cb, SNAKE_RENDER_MS, NULL);
  }
  lv_timer_pause(snake.render_timer);
}

static void snake_begin_round_locked(void) {
  snake.game_over = false;
  snake.paused = false;
  snake.state = SNAKE_STATE_PLAYING;
  snake.length = SNAKE_INITIAL_LENGTH;
  snake.score = 0;
  snake.combo_counter = 0;
  snake.play_time_ms = 0;
  snake.play_start_time = esp_timer_get_time() / 1000ULL;
  snake.direction = SNAKE_DIR_RIGHT;
  snake.last_turn_input = 0;
  snake.last_menu_turn_input = 0;
  snake.menu_selected = 0;
  snake.turn_queue_len = 0;

  int16_t start_x = (int16_t)(snake.grid_w / 2);
  int16_t start_y = (int16_t)(snake.grid_h / 2);

  for (uint16_t i = 0; i < snake.length; i++) {
    snake.cells[i].x = start_x - i;
    snake.cells[i].y = start_y;
  }

  snake_update_score_labels();
  snake_hide_game_over_menu();
  snake_hide_pause_menu();
  snake_hide_difficulty_menu();
  lv_obj_add_flag(snake.status_label, LV_OBJ_FLAG_HIDDEN);

  // Show game objects now that a round is starting
  if (snake.snake_line != NULL) {
    lv_obj_clear_flag(snake.snake_line, LV_OBJ_FLAG_HIDDEN);
  }

  snake_spawn_food();
  snake_rebuild_polyline();
  snake_update_hud_labels();

  uint32_t base_speed = DIFFICULTY_SPEED_MS[snake.difficulty];

  // Init interpolation state so render timer starts at rest
  snake.tick_period_ms = base_speed;
  snake.last_tick_wall_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
  snake.last_move_grew = false;
  if (snake.length > 0) {
    snake.prev_head_px = snake.points[0];
    snake.prev_tail_px = snake.points[snake.length - 1];
  }

  if (snake.tick_timer != NULL) {
    lv_timer_set_period(snake.tick_timer, base_speed);
    lv_timer_resume(snake.tick_timer);
  }

  if (snake.time_update_timer != NULL) {
    lv_timer_resume(snake.time_update_timer);
  }

  if (snake.food_pulse_timer != NULL) {
    lv_timer_resume(snake.food_pulse_timer);
  }

  if (snake.input_poll_timer != NULL) {
    lv_timer_resume(snake.input_poll_timer);
  }

  if (snake.render_timer != NULL) {
    lv_timer_resume(snake.render_timer);
  }
}

static void snake_exit_to_home_locked(void) {
  snake.running = false;
  snake.game_over = false;
  snake.paused = false;
  snake.state = SNAKE_STATE_MODE_SELECT;
  snake.length = 0;
  snake.score = 0;
  snake.last_turn_input = 0;
  snake.last_menu_turn_input = 0;

  if (snake.tick_timer != NULL) {
    lv_timer_pause(snake.tick_timer);
  }
  if (snake.time_update_timer != NULL) {
    lv_timer_pause(snake.time_update_timer);
  }
  if (snake.food_pulse_timer != NULL) {
    lv_timer_pause(snake.food_pulse_timer);
  }
  if (snake.input_poll_timer != NULL) {
    lv_timer_pause(snake.input_poll_timer);
  }
  if (snake.menu_input_timer != NULL) {
    lv_timer_pause(snake.menu_input_timer);
  }
  if (snake.render_timer != NULL) {
    lv_timer_pause(snake.render_timer);
  }

  snake_hide_game_over_menu();
  snake_hide_pause_menu();
  snake_hide_difficulty_menu();

  lv_obj_t *home = ui_get_home_screen();
  lv_disp_load_scr(home);
  if (!throttle_is_calibrated()) {
    ui_show_throttle_not_calibrated_text();
  }
  lv_obj_invalidate(home);

  // Back on home screen — resume BLE
  ble_resume();
}

// Public API
bool snake_is_running(void) { return snake.running; }

bool ui_handle_easter_egg_button_event(int event_raw) {
  button_event_t event = (button_event_t)event_raw;
  static uint8_t splash_combo_count = 0;
  static uint64_t splash_combo_last_press_ms = 0;

  if (snake.high_score == 0) {
    snake_load_high_score();
  }

  // Handle difficulty selection menu
  if (snake.state == SNAKE_STATE_DIFFICULTY_SELECT) {
    if (event == BUTTON_EVENT_PRESSED) {
      if (take_lvgl_mutex()) {
        if (snake.menu_selected == 3) {
          snake_exit_to_home_locked();
        } else {
          snake.difficulty = (snake_difficulty_t)snake.menu_selected;
          snake_hide_difficulty_menu();
          snake.game_over = false;
          snake_begin_round_locked();
        }
        give_lvgl_mutex();
      }
    }
    return true;
  }

  // Handle pause menu
  if (snake.state == SNAKE_STATE_PAUSED) {
    if (event == BUTTON_EVENT_PRESSED) {
      if (take_lvgl_mutex()) {
        if (snake.menu_selected == 0) {
          snake_hide_pause_menu();
        } else {
          snake_exit_to_home_locked();
        }
        give_lvgl_mutex();
      }
    }
    return true;
  }

  // Handle active game
  if (snake.running) {
    if (snake.game_over && event == BUTTON_EVENT_PRESSED) {
      if (take_lvgl_mutex()) {
        if (snake.menu_selected == 0) {
          snake_begin_round_locked();
        } else {
          snake_exit_to_home_locked();
        }
        give_lvgl_mutex();
      }
    } else if (!snake.game_over && event == BUTTON_EVENT_LONG_PRESS) {
      // Pause game on long-press
      if (take_lvgl_mutex()) {
        snake_show_pause_menu();

        if (snake.tick_timer != NULL) {
          lv_timer_pause(snake.tick_timer);
        }
        if (snake.time_update_timer != NULL) {
          lv_timer_pause(snake.time_update_timer);
        }
        if (snake.food_pulse_timer != NULL) {
          lv_timer_pause(snake.food_pulse_timer);
        }
        if (snake.input_poll_timer != NULL) {
          lv_timer_pause(snake.input_poll_timer);
        }

        give_lvgl_mutex();
      }
    }
    return true;
  }

  // Handle splash screen combo
  if (!splash_transition_active) {
    splash_combo_count = 0;
    splash_combo_last_press_ms = 0;
    return false;
  }

  if (event == BUTTON_EVENT_PRESSED) {
    uint64_t now_ms = esp_timer_get_time() / 1000ULL;
    if (splash_combo_last_press_ms != 0 &&
        (now_ms - splash_combo_last_press_ms) <= SNAKE_COMBO_MAX_GAP_MS) {
      splash_combo_count++;
    } else {
      splash_combo_count = 1;
    }
    splash_combo_last_press_ms = now_ms;

    if (splash_combo_count >= 3) {
      if (take_lvgl_mutex()) {
        // Suspend BLE — leaving home/splash for snake game
        ble_suspend();

        snake_build_screen_if_needed();
        snake.running = true;
        lv_disp_load_scr(snake.screen);
        lv_obj_invalidate(snake.screen);
        snake_show_difficulty_menu();

        // Cancel splash transition
        if (splash_transition_timer != NULL) {
          lv_timer_del(splash_transition_timer);
          splash_transition_timer = NULL;
        }
        splash_transition_active = false;

        give_lvgl_mutex();
      }
      splash_combo_count = 0;
      splash_combo_last_press_ms = 0;
    }
  }

  // Consume all button events while splash is active
  return true;
}
