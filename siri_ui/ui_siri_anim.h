// ui_siri_anim.h
#ifndef UI_SIRI_ANIM_H
#define UI_SIRI_ANIM_H

#ifdef __cplusplus
extern "C" {
#endif

// Gọi 1 lần sau khi ui_init() đã tạo xong các object của ui_screen__screen1
void ui_siri_anim_init(void);

// Gọi từ audio task mỗi khi có mức âm lượng mới, level trong khoảng 0.0 (im lặng) - 1.0 (rất to)
void ui_siri_anim_set_level(float level);

#ifdef __cplusplus
}
#endif

#endif