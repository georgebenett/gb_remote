#ifndef EEZ_LVGL_UI_IMAGES_H
#define EEZ_LVGL_UI_IMAGES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t img_splash;
extern const lv_img_dsc_t img_battery;
extern const lv_img_dsc_t img_battery_charging;
extern const lv_img_dsc_t img_33_connection;
extern const lv_img_dsc_t img_66_connection;
extern const lv_img_dsc_t img_100_connection;
extern const lv_img_dsc_t img_connection_0;
extern const lv_img_dsc_t img_aux_output;
extern const lv_img_dsc_t img_lightning;
extern const lv_img_dsc_t img_empty_battery;
extern const lv_img_dsc_t img_power_lock;

#ifndef EXT_IMG_DESC_T
#define EXT_IMG_DESC_T
typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;
#endif

extern const ext_img_desc_t images[11];


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_IMAGES_H*/