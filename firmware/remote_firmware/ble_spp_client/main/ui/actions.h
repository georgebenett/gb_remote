#ifndef EEZ_LVGL_UI_EVENTS_H
#define EEZ_LVGL_UI_EVENTS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void action_call_skate_config_screen(lv_event_t * e);
extern void action_call_controller_configure_screen(lv_event_t * e);
extern void action_call_wheel_pulley_screen(lv_event_t * e);
extern void action_call_wheel_size_screen(lv_event_t * e);
extern void action_call_motor_pulley_screen(lv_event_t * e);
extern void action_call_motor_poles_screen(lv_event_t * e);
extern void action_call_save(lv_event_t * e);
extern void action_call_set_pole_pairs(lv_event_t * e);
extern void action_call_set_motor_pulley(lv_event_t * e);
extern void action_call_set_wheel_size(lv_event_t * e);
extern void action_call_set_wheel_pulley(lv_event_t * e);
extern void action_call_invert_throttle(lv_event_t * e);
extern void action_call_disable_hand_sensor(lv_event_t * e);
extern void action_call_calibrate_hand_sensor(lv_event_t * e);
extern void action_call_reset_to_defaults(lv_event_t * e);
extern void action_call_set_brightness(lv_event_t * e);
extern void action_call_about_screen(lv_event_t * e);
extern void action_call_calibrate_throttle(lv_event_t * e);
extern void action_call_reset_odometer(lv_event_t * e);
extern void action_call_settings_menu_screen(lv_event_t * e);
extern void action_call_hand_configure_screen(lv_event_t * e);
extern void action_call_odometer_screen(lv_event_t * e);
extern void action_call_reset_screen(lv_event_t * e);


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_EVENTS_H*/