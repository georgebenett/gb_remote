#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _objects_t {
  lv_obj_t *charging_screen;
  lv_obj_t *splash_screen;
  lv_obj_t *home_screen;
  lv_obj_t *shutdown_screen;
  lv_obj_t *low_battery_screen;
  lv_obj_t *charging_screen_percentage;
  lv_obj_t *charging_arc;
  lv_obj_t *firmware_version;
  lv_obj_t *firmware_text;
  lv_obj_t *skate_battery;
  lv_obj_t *skate_battery_text;
  lv_obj_t *controller_battery;
  lv_obj_t *controller_battery_text;
  lv_obj_t *connection_icon;
  lv_obj_t *static_speed;
  lv_obj_t *speedlabel;
  lv_obj_t *odometer;
  lv_obj_t *aux_output;
  lv_obj_t *power_lock;
  lv_obj_t *obj0;
  lv_obj_t *shutting_down_bar;
  lv_obj_t *obj1;
  lv_obj_t *obj2;
  lv_obj_t *batt_charging_main_1;
} objects_t;

extern objects_t objects;

enum ScreensEnum {
  SCREEN_ID_CHARGING_SCREEN = 1,
  SCREEN_ID_SPLASH_SCREEN = 2,
  SCREEN_ID_HOME_SCREEN = 3,
  SCREEN_ID_SHUTDOWN_SCREEN = 4,
  SCREEN_ID_LOW_BATTERY_SCREEN = 5,
};

void create_screen_charging_screen();
void tick_screen_charging_screen();

void create_screen_splash_screen();
void tick_screen_splash_screen();

void create_screen_home_screen();
void tick_screen_home_screen();

void create_screen_shutdown_screen();
void tick_screen_shutdown_screen();

void create_screen_low_battery_screen();
void tick_screen_low_battery_screen();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/