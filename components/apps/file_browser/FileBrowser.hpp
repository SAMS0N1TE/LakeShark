
#pragma once

#include <string>
#include <vector>
#include "lvgl.h"
#include "shell/ls_app.hpp"

class AppFileBrowser : public LsApp {
public:
    AppFileBrowser();
    ~AppFileBrowser();

    bool run(lv_obj_t *parent) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;
    bool pause(void) override;
    bool resume(void) override;

private:
    struct Entry {
        std::string name;
        bool is_dir;
        size_t size;
    };

    void buildUi(lv_obj_t *parent);
    void loadDirectory(const std::string &path);
    void openEntry(const Entry &e);
    void showFileDialog(const std::string &full_path, size_t size);

    static void onListRowClicked(lv_event_t *e);
    static void onBackButtonClicked(lv_event_t *e);
    static void onDialogCloseClicked(lv_event_t *e);
    static void onDialogDeleteClicked(lv_event_t *e);

    lv_obj_t *_path_label;
    lv_obj_t *_list;
    std::string _cwd;
    std::vector<Entry> _entries;
    lv_obj_t *_dialog;
    std::string _dialog_path;
};
