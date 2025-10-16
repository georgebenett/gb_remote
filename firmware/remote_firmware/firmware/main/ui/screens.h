#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _objects_t {
    lv_obj_t *splash_screen;
    lv_obj_t *home_screen;
    lv_obj_t *shutdown_screen;
    lv_obj_t *splash_arc;
    lv_obj_t *connection_icon;
    lv_obj_t *controller_battery;
    lv_obj_t *light;
    lv_obj_t *controller_battery_text;
    lv_obj_t *static_speed;
    lv_obj_t *skate_battery;
    lv_obj_t *skate_battery_text;
    lv_obj_t *speedlabel;
    lv_obj_t *odometer;
    lv_obj_t *obj0;
    lv_obj_t *splash_arc_1;
} objects_t;

extern objects_t objects;

enum ScreensEnum {
    SCREEN_ID_SPLASH_SCREEN = 1,
    SCREEN_ID_HOME_SCREEN = 2,
    SCREEN_ID_SHUTDOWN_SCREEN = 3,
};

void create_screen_splash_screen();
void tick_screen_splash_screen();

void create_screen_home_screen();
void tick_screen_home_screen();

void create_screen_shutdown_screen();
void tick_screen_shutdown_screen();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();




#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/