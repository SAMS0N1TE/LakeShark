
#pragma once

#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include "lvgl.h"
#include "esp_brookesia.hpp"

class AppControlPanel : public ESP_Brookesia_PhoneApp {
public:
    AppControlPanel();
    ~AppControlPanel();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;
    bool pause(void) override;
    bool resume(void) override;

    enum class WidgetType { Button, Toggle, Slider, Label, Unknown };
    using Value = std::variant<bool, double, std::string>;

    struct Widget {
        std::string id;
        std::string title;
        WidgetType  type;
        int col, row, w, h;
        double min, max;
        std::string webhook;
        Value value;
        lv_obj_t *lv_obj;
    };

    struct Panel {
        std::string name;
        std::string title;
        int cols, rows;
        std::vector<std::unique_ptr<Widget>> widgets;
        std::map<std::string, Widget *> by_id;
        lv_obj_t *tab_page;
    };

    bool setWidgetValue(const std::string &panel, const std::string &id, const std::string &json_value);

    std::string snapshotPanelList(void) const;
    std::string snapshotPanelWidgets(const std::string &panel) const;

    std::string readConfig(const std::string &name) const;
    bool saveConfig(const std::string &name, const std::string &json_text);
    bool deleteConfig(const std::string &name);

    static bool isValidPanelName(const std::string &name);

    static AppControlPanel *_instance;

private:
    bool loadAllPanels(void);
    bool loadPanelFile(const std::string &full_path);
    void buildUi(lv_obj_t *parent);
    void renderPanel(Panel &p, lv_obj_t *tab_page);
    void renderWidget(lv_obj_t *grid_parent, Widget &w, int cell_w_px, int cell_h_px);
    void applyValueToWidget(Widget &w);

    static void fireWebhook(const std::string &url);

    void startHttpServer(void);
    void stopHttpServer(void);

    Panel *findPanel(const std::string &name);
    const Panel *findPanel(const std::string &name) const;

    static void onUiButtonClicked(lv_event_t *e);
    static void onUiToggleChanged(lv_event_t *e);
    static void onUiSliderChanged(lv_event_t *e);

    std::vector<std::unique_ptr<Panel>> _panels;
    lv_obj_t *_tabview;
    void *_httpd;
};
