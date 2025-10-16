#if defined(EEZ_FOR_LVGL)
#include <eez/core/vars.h>
#endif

#include "ui.h"
#include "screens.h"
#include "images.h"
#include "actions.h"
#include "vars.h"
#include "ui_updater.h"







/*CUSTOM FUNCTION DECLARATIONS:
this navigates between the screens with swipe motion*/ 
void ui_event_home_screen(lv_event_t * e);
void ui_event_shutdown_screen(lv_event_t * e);
void ui_event_menu_screen(lv_event_t * e);
void ui_event_skate_configure(lv_event_t * e);
void ui_event_settings_menu(lv_event_t * e);
void ui_event_about_screen(lv_event_t * e);


/*this returns from the skate configure screens back to main menu*/
void ui_event_wheel_pulley_screen(lv_event_t * e);
void ui_event_wheel_size_screen(lv_event_t * e);
void ui_event_motor_pulley_screen(lv_event_t * e);
void ui_event_motor_poles_screen(lv_event_t * e);
void ui_event_throttle_configure(lv_event_t *e);
void ui_event_hand_configure(lv_event_t *e);
void ui_event_odometer_configure(lv_event_t *e);
void ui_event_reset_menu(lv_event_t *e);

#if defined(EEZ_FOR_LVGL)

void ui_init() {
    eez_flow_init(assets, sizeof(assets), (lv_obj_t **)&objects, sizeof(objects), images, sizeof(images), actions);
}

void ui_tick() {
    eez_flow_tick();
    tick_screen(g_currentScreen);
}

#else

#include <string.h>

static int16_t currentScreen = -1;

static lv_obj_t *getLvglObjectFromIndex(int32_t index) {
    if (index == -1) {
        return 0;
    }
    return ((lv_obj_t **)&objects)[index];
}

void loadScreen(enum ScreensEnum screenId) {
    currentScreen = screenId - 1;
    lv_obj_t *screen = getLvglObjectFromIndex(currentScreen);
    lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_OUT, 500, 0, false);
}

void ui_init() {
    create_screens();
    
    /*CUSTOM CODE HERE:
    this is responsible for navigating the screens with swipe motion
    edit this only in EEZ STUDIO*/
    lv_obj_add_event_cb(objects.home_screen, ui_event_home_screen, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.shutdown_screen, ui_event_shutdown_screen, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.menu_screen, ui_event_menu_screen, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.skate_configure, ui_event_skate_configure, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.about_screen, ui_event_about_screen, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.settings_menu, ui_event_settings_menu, LV_EVENT_ALL, NULL);

    
    lv_obj_add_event_cb(objects.wheel_pulley_menu, ui_event_wheel_pulley_screen, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.wheel_size_menu, ui_event_wheel_size_screen, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.motor_pulley_menu, ui_event_motor_pulley_screen, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.motor_poles_menu, ui_event_motor_poles_screen, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.throttle_configure, ui_event_throttle_configure, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.hand_configure, ui_event_hand_configure, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.odometer_configure, ui_event_odometer_configure, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.reset_menu, ui_event_reset_menu, LV_EVENT_ALL, NULL);

    



    loadScreen(SCREEN_ID_SPLASH_SCREEN);

}

void ui_tick() {
    tick_screen(currentScreen);
}

#endif
