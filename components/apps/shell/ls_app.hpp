#pragma once

#include "lvgl.h"

class LsApp {
public:
    LsApp(const char *name, const char *icon)
        : _name(name), _icon(icon) {}
    virtual ~LsApp() = default;

    virtual bool init(void)              { return true; }
    virtual bool run(lv_obj_t *parent)   = 0;
    virtual bool back(void);
    /* LS-746: close releases timers, external dialogs/subscriptions and
     * non-LVGL buffers; the shell then destroys the container. run() must
     * support reconstruction on this same lightweight app descriptor. */
    virtual bool close(void)             { return true; }
    virtual bool pause(void)             { return true; }
    /* App-owned workers must retire before the replacement consumes their RAM. */
    virtual bool stopped(void) const     { return true; }
    virtual bool resume(void)            { return true; }

    /*LS-604*/
    virtual bool background(void)        { return pause(); }
    /*LS-604*/
    virtual bool passive(void) const     { return false; }

    virtual void switchTab(int delta)    { (void)delta; }

    const char *name(void) const { return _name; }
    const char *icon(void) const { return _icon; }

protected:
    bool exitToLauncher(void);

private:
    const char *_name;
    const char *_icon;
};
