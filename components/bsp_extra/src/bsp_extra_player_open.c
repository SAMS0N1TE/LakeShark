/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

#include "audio_player.h"
#include "file_iterator.h"
#include "bsp_extra_player_state.h"

/* The prototypes live in bsp_board_extra.h, but that header pulls in
   esp_codec_dev.h and the i2s/gpio drivers, which the bench does not shim.
   Redeclaring here keeps this translation unit compilable on the host
   without dragging the whole BSP in.  The linker still checks the
   signatures against every caller that does include bsp_board_extra.h. */
esp_err_t bsp_extra_file_instance_init(const char *path,
                                       file_iterator_instance_t **ret_instance);
esp_err_t bsp_extra_player_play_index(file_iterator_instance_t *instance,
                                      int index);
esp_err_t bsp_extra_player_play_file(const char *file_path);
bool      bsp_extra_player_is_playing_by_path(const char *file_path);

static const char *TAG = "bsp_extra_player_open";
static char audio_file_path[128];

esp_err_t bsp_extra_file_instance_init(const char *path,
                                       file_iterator_instance_t **ret_instance)
{
    ESP_RETURN_ON_FALSE(path, ESP_FAIL, TAG, "path is NULL");
    ESP_RETURN_ON_FALSE(ret_instance, ESP_FAIL, TAG, "ret_instance is NULL");

    file_iterator_instance_t *file_iterator = file_iterator_new(path);
    ESP_RETURN_ON_FALSE(file_iterator, ESP_FAIL, TAG,
                        "file_iterator_new failed, %s", path);

    *ret_instance = file_iterator;
    return ESP_OK;
}

esp_err_t bsp_extra_player_play_index(file_iterator_instance_t *instance, int index)
{
    ESP_RETURN_ON_FALSE(instance, ESP_FAIL, TAG, "instance is NULL");

    ESP_LOGI(TAG, "play_index(%d)", index);
    char filename[128];
    int retval = file_iterator_get_full_path_from_index(instance, index, filename,
                                                        sizeof(filename));
    ESP_RETURN_ON_FALSE(retval != 0, ESP_FAIL, TAG,
                        "file_iterator_get_full_path_from_index failed");

    ESP_LOGI(TAG, "opening file '%s'", filename);
    FILE *fp = fopen(filename, "rb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", filename);
    esp_err_t err = audio_player_play(fp);
    if (err != ESP_OK) {

        /* Enqueue failed - audio_player did not take the fp, so close it
           here or every failed tap leaks a fatfs handle. */
        ESP_LOGE(TAG, "audio_player_play failed (0x%x) - closing '%s'",
                 (unsigned)err, filename);
        fclose(fp);
        return err;
    }

    memcpy(audio_file_path, filename, sizeof(audio_file_path));

    bsp_extra_player_state_note_play_index(instance, index);
    return ESP_OK;
}

esp_err_t bsp_extra_player_play_file(const char *file_path)
{
    ESP_RETURN_ON_FALSE(file_path, ESP_FAIL, TAG, "file_path is NULL");

    ESP_LOGI(TAG, "opening file '%s'", file_path);
    FILE *fp = fopen(file_path, "rb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", file_path);
    esp_err_t err = audio_player_play(fp);
    if (err != ESP_OK) {

        ESP_LOGE(TAG, "audio_player_play failed (0x%x) - closing '%s'",
                 (unsigned)err, file_path);
        fclose(fp);
        return err;
    }

    memcpy(audio_file_path, file_path, sizeof(audio_file_path));

    bsp_extra_player_state_note_play_path(file_path);
    return ESP_OK;
}

bool bsp_extra_player_is_playing_by_path(const char *file_path)
{
    return (strcmp(audio_file_path, file_path) == 0);
}
