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
    virtual bool close(void)             { return true; }
    virtual bool pause(void)             { return true; }
    virtual bool resume(void)            { return true; }

    virtual void switchTab(int delta)    { (void)delta; }

    const char *name(void) const { return _name; }
    const char *icon(void) const { return _icon; }

protected:
    bool exitToLauncher(void);

private:
    const char *_name;
    const char *_icon;
};
