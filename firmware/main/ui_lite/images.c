#include "images.h"

const ext_img_desc_t images[11] = {
    {"splash", &img_splash},
    {"battery", &img_battery},
    {"battery charging", &img_battery_charging},
    {"33 connection", &img_33_connection},
    {"66 connection", &img_66_connection},
    {"100 connection", &img_100_connection},
    {"connection_0", &img_connection_0},
    {"aux_output", &img_aux_output},
    {"lightning", &img_lightning},
    {"empty_battery", &img_empty_battery},
    {"power lock", &img_power_lock},
};
