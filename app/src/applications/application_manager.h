#pragma once

#include <lvgl.h>

typedef void(*application_start_fn)(lv_obj_t *root, lv_group_t *group);
typedef void(*application_stop_fn)(void);

typedef void(*on_application_manager_cb_fn)(void);


typedef struct application_t {
    application_start_fn    start_func;
    application_stop_fn     stop_func;
    char                   *name;
    const lv_img_dsc_t     *icon;
} application_t;

void application_manager_show(on_application_manager_cb_fn close_cb, lv_obj_t *root, lv_group_t *group);

void application_manager_delete(void);

void application_manager_add_application(application_t *app);

void application_manager_app_close_request(application_t *app);

void application_manager_exit_app(void);