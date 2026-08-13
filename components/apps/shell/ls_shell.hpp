#pragma once

#include "lvgl.h"
#include "shell/ls_statusbar.hpp"

class LsApp;

class LsShell {
public:
    static LsShell &instance(void);

    void begin(void);
    void registerApp(LsApp *app, bool show_in_rail = true);
    void start(const char *prefer = nullptr);

    void launch(LsApp *app);
    bool launchByName(const char *name);
    void cycleNext(void);
    /*LS-604*/
    void cycleApp(int delta);
    /*LS-600*/
    void closeAll(void);

    void home(void);
    void goBack(void);

    lv_obj_t *content(void) { return _content; }

private:
    LsShell() = default;

    void buildRail(void);
    void updateRail(void);

    /*LS-600*/
    int  indexOf(LsApp *app) const;
    lv_obj_t *containerFor(int idx);

    static void railBtnCb(lv_event_t *e);
    /*LS-905*/
    static void gestureCb(lv_event_t *e);
    /*LS-603*/
    static void statusTapCb(lv_event_t *e);

    lv_obj_t *_root     = nullptr;
    lv_obj_t *_content  = nullptr;
    lv_obj_t *_rail     = nullptr;
    LsApp    *_current  = nullptr;

    /*LS-603*/
    LsStatusBar _status;

    static const int MAX_APPS = 16;
    LsApp     *_apps[MAX_APPS]     = {nullptr};
    lv_obj_t  *_rail_btn[MAX_APPS] = {nullptr};
    /*LS-600*/
    lv_obj_t  *_app_cont[MAX_APPS] = {nullptr};
    bool       _app_built[MAX_APPS] = {false};
    bool       _rail_hidden[MAX_APPS] = {false};
    int        _app_count = 0;
};
