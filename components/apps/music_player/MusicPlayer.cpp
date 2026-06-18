/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"

#include "gui_music/lv_demo_music.h"
#include "gui_music/lv_demo_music_main.h"
#include "MusicPlayer.hpp"
#include "lakeshark_backend.h"

#define MUSIC_DIR   BSP_SPIFFS_MOUNT_POINT "/music"

using namespace std;

LV_IMG_DECLARE(img_app_music_player);

static const char *TAG = "MusicPlayer";

MusicPlayer::MusicPlayer():
    ESP_Brookesia_PhoneApp("Music Player", &img_app_music_player, true), // auto_resize_visual_area
    _file_iterator(NULL)
{
}

MusicPlayer::~MusicPlayer()
{
}

bool MusicPlayer::run(void)
{
    /* Hard guarantee clean audio: stop the RTL-SDR stream + decode task
     * (prio 5, core 1) before playback so they can't starve the I2S/codec
     * DMA. The radio apps already park on background, but a stale-running
     * radio (e.g. opened straight from the launcher) is killed here too. */
    lakeshark_radio_park();

    lv_demo_music(lv_scr_act(), _file_iterator);

    return true;
}

bool MusicPlayer::pause(void)
{
    _lv_demo_music_exit_pause();

    return true;
}

bool MusicPlayer::back(void)
{
    notifyCoreClosed();

    return true;
}

bool MusicPlayer::close(void)
{
    if (audio_player_pause() != ESP_OK) {
        ESP_LOGE(TAG, "audio_player_pause failed");
        return false;
    }

    return true;
}

bool MusicPlayer::init(void)
{
    if (bsp_extra_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "Play init with SPIFFS failed");
        return false;
    }

    if (bsp_extra_file_instance_init(MUSIC_DIR, &_file_iterator) != ESP_OK) {
        ESP_LOGE(TAG, "bsp_extra_file_instance_init failed");
        return false;
    }

    return true;
}
