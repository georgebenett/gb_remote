#ifndef SNAKE_H
#define SNAKE_H

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

#define SNAKE_CELL_TARGET_PX 8
#define SNAKE_GRID_MIN_W 12
#define SNAKE_GRID_MIN_H 20
#define SNAKE_MAX_SEGMENTS 1400
#define SNAKE_INITIAL_LENGTH 5
#define SNAKE_TICK_MS_BASE 145
#define SNAKE_TICK_MS_MIN 75
#define SNAKE_TURN_DEADBAND 10
#define SNAKE_COMBO_MAX_GAP_MS 450
#define SNAKE_MENU_INPUT_POLL_MS 80
#define SNAKE_BORDER_INSET_PX 25
#define SNAKE_FOOD_PULSE_FREQ_MS 200
#define SNAKE_TIME_UPDATE_MS 500
#define SNAKE_DIFFICULTY_TIERS 3
#define SNAKE_INPUT_POLL_MS 25
#define SNAKE_TURN_QUEUE_SIZE 3

#define SNAKE_NVS_NAMESPACE "snake"
#define SNAKE_NVS_HIGH_SCORE_KEY "high_score"

typedef enum {
  SNAKE_DIFFICULTY_EASY = 0,
  SNAKE_DIFFICULTY_NORMAL = 1,
  SNAKE_DIFFICULTY_HARD = 2,
} snake_difficulty_t;

typedef enum {
  SNAKE_FOOD_NORMAL = 0,
  SNAKE_FOOD_BONUS = 1,
} snake_food_quality_t;

typedef enum {
  SNAKE_STATE_MODE_SELECT,
  SNAKE_STATE_DIFFICULTY_SELECT,
  SNAKE_STATE_PLAYING,
  SNAKE_STATE_PAUSED,
  SNAKE_STATE_GAME_OVER,
} snake_state_t;

typedef enum {
  SNAKE_DIR_UP = 0,
  SNAKE_DIR_RIGHT,
  SNAKE_DIR_DOWN,
  SNAKE_DIR_LEFT,
} snake_dir_t;

typedef struct {
  int16_t x;
  int16_t y;
} snake_cell_t;

typedef struct {
  bool running;
  bool game_over;
  bool paused;
  uint16_t length;
  uint16_t score;
  uint16_t high_score;
  uint16_t grid_w;
  uint16_t grid_h;
  snake_dir_t direction;
  int8_t last_turn_input;
  int8_t last_menu_turn_input;
  uint8_t menu_selected;
  snake_difficulty_t difficulty;
  snake_state_t state;
  uint16_t combo_counter;
  uint32_t play_time_ms;
  uint64_t play_start_time;
  uint16_t food_pulse_offset;
  bool food_pulse_growing;
  snake_food_quality_t food_quality;
  
  snake_cell_t cells[SNAKE_MAX_SEGMENTS];
  lv_point_t points[SNAKE_MAX_SEGMENTS];
  snake_cell_t food;

  lv_obj_t *screen;
  lv_obj_t *playfield;
  lv_obj_t *snake_line;
  lv_obj_t *food_obj;
  lv_obj_t *score_label;
  lv_obj_t *best_label;
  lv_obj_t *status_label;
  lv_obj_t *menu_panel;
  lv_obj_t *menu_retry_label;
  lv_obj_t *menu_exit_label;
  lv_obj_t *menu_hint_label;
  lv_obj_t *speed_label;
  lv_obj_t *time_label;
  lv_obj_t *combo_label;
  lv_obj_t *difficulty_panel;
  lv_obj_t *diff_easy_label;
  lv_obj_t *diff_normal_label;
  lv_obj_t *diff_hard_label;
  lv_obj_t *pause_panel;
  lv_obj_t *pause_resume_label;
  lv_obj_t *pause_quit_label;
  lv_timer_t *tick_timer;
  lv_timer_t *menu_input_timer;
  lv_timer_t *time_update_timer;
  lv_timer_t *food_pulse_timer;
  lv_timer_t *input_poll_timer;

  int8_t turn_queue[SNAKE_TURN_QUEUE_SIZE];
  uint8_t turn_queue_len;

  int16_t cell_w;
  int16_t cell_h;
} snake_t;

// Public API
bool ui_handle_easter_egg_button_event(int event_raw);
bool snake_is_running(void);

#endif // SNAKE_H
