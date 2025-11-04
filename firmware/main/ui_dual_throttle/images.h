#ifndef EEZ_LVGL_UI_IMAGES_H
#define EEZ_LVGL_UI_IMAGES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t img_splash;
extern const lv_img_dsc_t img_arclock;
extern const lv_img_dsc_t img_battery;
extern const lv_img_dsc_t img_battery_charging;
extern const lv_img_dsc_t img_33_connection;
extern const lv_img_dsc_t img_66_connection;
extern const lv_img_dsc_t img_100_connection;
extern const lv_img_dsc_t img_connection_0;
extern const lv_img_dsc_t img_arcunlock;
extern const lv_img_dsc_t img_arrow;
extern const lv_img_dsc_t img_config;
extern const lv_img_dsc_t img_skateboard;
extern const lv_img_dsc_t img_info;
extern const lv_img_dsc_t img_about;
extern const lv_img_dsc_t img_light_yellow;
extern const lv_img_dsc_t img_light_white;
extern const lv_img_dsc_t img_toggle_on;
extern const lv_img_dsc_t img_toggle_off;
extern const lv_img_dsc_t img_hand_sensor;
extern const lv_img_dsc_t img_throttle;
extern const lv_img_dsc_t img_odometer;
extern const lv_img_dsc_t img_warning;

#ifndef EXT_IMG_DESC_T
#define EXT_IMG_DESC_T
typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;
#endif

extern const ext_img_desc_t images[22];


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_IMAGES_H*/