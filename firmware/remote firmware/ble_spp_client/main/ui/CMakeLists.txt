SET(SOURCES screens/ui_home_screen.c
    screens/ui_shutdown_screen.c
    screens/ui_splash_screen.c
    ui_theme_manager.c
    ui_themes.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_battery_icon_png.c
    images/ui_img_no_connection_png.c
    images/ui_img_gb_png.c
    fonts/ui_font_bebas150.c
    fonts/ui_font_bebas15.c
    fonts/ui_font_bebas35.c
    fonts/ui_font_bebas20.c)

add_library(ui ${SOURCES})
