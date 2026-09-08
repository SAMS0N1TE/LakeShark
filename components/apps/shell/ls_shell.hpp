#pragma once

#include "lvgl.h"
#include "shell/ls_statusbar.hpp"
#include "shell/ls_transition.h"

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

    /*LS-019*/
    LsApp *current(void) { return _current; }
    bool transitioning(void) const { return _transition.phase != LS_TRANS_IDLE; }

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
    /*LS-791*/
    static void pressedCb(lv_event_t *e);
    static lv_coord_t _press_y;
    /*LS-606*/
    static void themeCb(void *ud);
    static void transitionCb(lv_timer_t *timer);
    static uint32_t transitionStop(void *ctx);
    static int transitionStopped(void *ctx, uint32_t token);
    static bool transitionUnload(void *ctx, int idx);
    static bool transitionBuild(void *ctx, int idx);
    void showTransition(void);

    lv_obj_t *_root     = nullptr;
    lv_obj_t *_content  = nullptr;
    lv_obj_t *_rail     = nullptr;
    LsApp    *_current  = nullptr;
    ls_transition_t _transition = {};
    lv_timer_t *_transition_timer = nullptr;
    lv_timer_t *_entry_timer = nullptr;
    lv_obj_t *_loading = nullptr;
    lv_obj_t *_loading_text = nullptr;
    char _loading_caption[96] = {};
    const char *_transition_error = nullptr;

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
