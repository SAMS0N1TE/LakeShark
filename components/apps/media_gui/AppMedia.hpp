#pragma once

/**/
/* Music player. Registered HIDDEN - it is reached from FILES, not the rail,
   because the rail is already tight and a media player is not a radio
   function. See main.cpp: registerApp(app, false). */

#include "lvgl.h"
#include "shell/ls_app.hpp"

/**/
/* Ask the player to open a specific file on its next launch. */
extern "C" void ls_media_play_path(const char *path);

class AppMedia : public LsApp {
public:
    AppMedia();
    ~AppMedia();

    bool run(lv_obj_t *parent) override;
    bool back(void) override;
    bool close(void) override;
    bool pause(void) override;
    bool resume(void) override;
    bool background(void) override;

private:
    lv_obj_t *_screen_readout = nullptr;
    lv_obj_t *_screen_lamp = nullptr;
    void rescan(void);
    void refreshList(void);
    void updateNow(void);

    static void timerCb(lv_timer_t *t);
    static void playCb(lv_event_t *e);
    static void stopCb(lv_event_t *e);
    static void prevCb(lv_event_t *e);
    static void nextCb(lv_event_t *e);
    static void srcCb(lv_event_t *e);
    static void listCb(lv_event_t *e);

    lv_timer_t *_timer  = nullptr;
    lv_obj_t   *_list   = nullptr;
    lv_obj_t   *_now    = nullptr;
    lv_obj_t   *_srclbl = nullptr;
    lv_obj_t   *_playlbl = nullptr;

    int  _sel     = 0;
    int  _playing = -1;
    int  _src     = 0;      /* 0 = SD, 1 = internal spiffs */
};
