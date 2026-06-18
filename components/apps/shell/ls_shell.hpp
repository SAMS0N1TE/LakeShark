#pragma once

#include "lvgl.h"

class LsApp;

class LsShell {
public:
    static LsShell &instance(void);

    void begin(void);
    void registerApp(LsApp *app);
    void start(const char *prefer = nullptr);

    void launch(LsApp *app);
    bool launchByName(const char *name);
    void cycleNext(void);

    void home(void);
    void goBack(void);

    lv_obj_t *content(void) { return _content; }

private:
    LsShell() = default;

    void buildRail(void);
    void updateRail(void);

    void buildTabArrows(void);

    static void railBtnCb(lv_event_t *e);
    static void tabPrevCb(lv_event_t *e);
    static void tabNextCb(lv_event_t *e);

    lv_obj_t *_root     = nullptr;
    lv_obj_t *_content  = nullptr;
    lv_obj_t *_rail     = nullptr;
    lv_obj_t *_tab_prev = nullptr;
    lv_obj_t *_tab_next = nullptr;
    LsApp    *_current  = nullptr;

    static const int MAX_APPS = 16;
    LsApp     *_apps[MAX_APPS]     = {nullptr};
    lv_obj_t  *_rail_btn[MAX_APPS] = {nullptr};
    int        _app_count = 0;
};
