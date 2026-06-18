
#pragma once

#include <string>
#include "lvgl.h"
#include "esp_brookesia.hpp"

class AppTerminal : public ESP_Brookesia_PhoneApp {
public:
    AppTerminal();
    ~AppTerminal();

    bool run(void);
    bool back(void);
    bool close(void);

    bool init(void) override;
    bool pause(void) override;
    bool resume(void) override;

    static void dispatchCommand(const std::string &line, std::string &out);

private:

    void buildUi(lv_obj_t *parent);
    void appendOutput(const std::string &text);
    void appendOutputPrompt(void);

    static void onUiInputReadyCallback(lv_event_t *e);
    static void onUiKeyboardCallback(lv_event_t *e);

    lv_obj_t *_screen;
    lv_obj_t *_output;
    lv_obj_t *_input;
    lv_obj_t *_keyboard;

    static void telnetServerTask(void *arg);
    static bool _telnet_started;
};
