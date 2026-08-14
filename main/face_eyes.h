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

/**
 * @brief Khởi tạo đối tượng mắt và hiệu ứng đồ họa bên trong đối tượng cha (parent)
 * @param parent Container chứa cụm mắt
 * @note Cần gọi khi đang giữ lock LVGL
 */
void face_eyes_create(lv_obj_t *parent);

/**
 * @brief Chuyển đổi biểu cảm mắt với hiệu ứng chuyển động mượt mà
 * @param emotion Biểu cảm mục tiêu
 * @param transition_ms Thời gian thực hiện chuyển động (ms)
 * @note Phải gọi trong lúc đang giữ lock LVGL
 */
void face_eyes_set_emotion(face_emotion_t emotion, uint32_t transition_ms);

/**
 * @brief Kích hoạt một chu kỳ chớp mắt thủ công
 */
void face_eyes_blink(void);

/**
 * @brief Phân tích chuỗi tên cảm xúc từ server thành enum face_emotion_t
 * @param face_str Chuỗi tên cảm xúc ("happy", "sad", "angry", "listening", "thinking")
 * @return face_emotion_t Giá trị enum tương ứng (mặc định FACE_NORMAL nếu không khớp)
 */
face_emotion_t face_eyes_parse_emotion(const char *face_str);

#ifdef __cplusplus
}
#endif