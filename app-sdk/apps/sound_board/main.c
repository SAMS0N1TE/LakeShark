#include <lakeshark_app.h>
#include <stdio.h>
static lap_caps_t caps;
static int mic = -1, recording = -1, sample = -1, result;
static uint32_t captured, dropped;
static bool configured;
static int16_t tone[320];
static void enter(void) {
    lap_caps(&caps, sizeof(caps));
    mic = recording = sample = -1;
    configured = false;
    for (int i = 0; i < 320; i++)
        tone[i] = (i % 32 < 16) ? 5000 : -5000;
}
static void leave(void) {
    if (recording >= 0)
        lap_audio_close(recording);
    if (sample >= 0)
        lap_audio_close(sample);
    if (mic >= 0)
        lap_mic_close(mic);
    for (int i = 0; i < 3; i++)
        lap_mixer_clear(i);
}
static bool key(lap_key_t k, char ch) {
    if (k != LAP_KEY_CHAR)
        return false;
    if (ch >= '1' && ch <= '3')
        result = lap_mixer_write(ch - '1', tone, 320);
    else if (ch == 'm' && (caps.hardware & LAP_HW_MIC) && mic < 0)
        mic = lap_mic_open();
    else if (ch == 'r' && mic >= 0 && (caps.hardware & LAP_HW_SD)) {
        if (recording >= 0)
            lap_audio_close(recording);
        recording = lap_audio_record_start(mic, "recording.wav", 5000);
    } else if (ch == 's') {
        if (recording >= 0)
            result = lap_audio_record_stop(recording);
        if (mic >= 0)
            lap_mic_close(mic);
        mic = -1;
    } else if (ch == 'p' && (caps.hardware & LAP_HW_SPEAKER) && (caps.hardware & LAP_HW_SD)) {
        if (sample >= 0)
            lap_audio_close(sample);
        sample = lap_sample_play(LAP_MIX_EFFECTS, "recording.wav");
    } else
        return false;
    return true;
}
static void draw(void) {
    if (!configured && (caps.hardware & LAP_HW_SPEAKER)) {
        result = lap_mixer_config(LAP_MIX_MUSIC, 60, 20);
        if (!result) {
            int effects=lap_mixer_config(LAP_MIX_EFFECTS,80,50);
            int voice=lap_mixer_config(LAP_MIX_VOICE,100,100);
            configured = !effects&&!voice;
        }
    }
    if (mic >= 0) {
        int16_t pcm[1024];
        lap_operation_t s;
        int n = lap_mic_read(mic, pcm, 1024, &s);
        if (n >= 0) {
            captured = s.transferred;
            dropped = s.dropped;
        }
    }
    lap_operation_t state;
    if (caps.hardware & LAP_HW_SPEAKER)
        lap_mixer_status(LAP_MIX_MUSIC, &state);
    if (recording >= 0 && !lap_audio_operation(recording, &state))
        result = state.error;
    if (sample >= 0 && !lap_audio_operation(sample, &state))
        result = state.error;
    char line[90];
    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 1, "SOUND BOARD / RECORDER", LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK));
    snprintf(line, sizeof(line), "Speaker %s  Mic %s  SD %s",
             caps.hardware & LAP_HW_SPEAKER ? "yes" : "absent", caps.hardware & LAP_HW_MIC ? "yes" : "absent",
             caps.hardware & LAP_HW_SD ? "yes" : "absent");
    lap_put_str(1, 3, line, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    snprintf(line, sizeof(line), "Captured %lu dropped %lu result %d", (unsigned long)captured,
             (unsigned long)dropped, result);
    lap_put_str(1, 5, line, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 7, "1 music 2 effects 3 voice (ducks music)", LAP_ATTR(LAP_YELLOW, LAP_BLACK));
    lap_put_str(1, 8, "M mic R record 5s S stop P play recording", LAP_ATTR(LAP_YELLOW, LAP_BLACK));
}
LAP_APP(.id = "sound_board", .name = "SOUND BOARD", .sub = "mixer and WAV recorder", .version = "1.0",
        .author = "LakeShark", .flags = LAP_CAP_MIC, .icon = LAP_ICON_TOWER, .color = LAP_CYAN,
        .enter = enter, .leave = leave, .draw = draw, .key = key);
