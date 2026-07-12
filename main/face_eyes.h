#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FACE_NORMAL = 0,
    FACE_HAPPY,
    FACE_SAD,
    FACE_ANGRY,
    FACE_LISTENING,
    FACE_THINKING,
    FACE_EMOTION_COUNT,
} face_emotion_t;

// Tạo 2 mắt procedural (2 hình chữ nhật bo tròn) + 2 lông mày bên trong `parent`.
// Gọi 1 lần trong display_init(), sau khi đã có screen active và đang giữ lock LVGL.
void face_eyes_create(lv_obj_t *parent);

// Chuyển sang biểu cảm mới, animate mượt trong transition_ms (vd: 250-350ms).
// Phải gọi trong lúc đang giữ esp_lv_adapter_lock().
void face_eyes_set_emotion(face_emotion_t emotion, uint32_t transition_ms);

// Kích hoạt 1 lần chớp mắt thủ công. Bình thường không cần gọi tay,
// module tự chớp random mỗi 2.5-6s qua lv_timer nội bộ.
void face_eyes_blink(void);

// Map chuỗi "happy"/"sad"/"angry"/"listening"/"thinking" (từ JSON server) sang enum.
// Chuỗi không khớp hoặc NULL -> FACE_NORMAL.
face_emotion_t face_eyes_parse_emotion(const char *face_str);

#ifdef __cplusplus
}
#endif