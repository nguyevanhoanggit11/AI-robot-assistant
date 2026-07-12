#include "face_eyes.h"

#include <string.h>
#include <stdlib.h>
#include "esp_random.h"

// Màu nền màn hình (phải khớp với màu set cho screen trong display.c)
// Dùng để "mask" (che) một nửa hình tròn, tạo hiệu ứng bán nguyệt.
#define FACE_BG_COLOR_HEX 0x1A1A2E

typedef enum {
    MASK_NONE = 0,   // không che -> hình tròn đầy đủ
    MASK_TOP,        // che nửa TRÊN -> còn lại nửa DƯỚI tròn, bụng cong xuống (SAD)
    MASK_BOTTOM,     // che nửa DƯỚI -> còn lại nửa TRÊN tròn, bụng cong lên (HAPPY)
} mask_mode_t;

// =======================
// Bảng tham số hình học từng biểu cảm
// =======================
typedef struct {
    int16_t     eye_d;         // đường kính hình tròn gốc của mắt (px)
    int16_t     eye_y;         // offset dọc của cả cụm mắt so với tâm container
    int16_t     gap_x;         // khoảng cách từ tâm mặt tới tâm mỗi mắt
    mask_mode_t mask;          // che nửa nào để tạo hình bán nguyệt (happy/sad)
    bool        show_qmark;    // hiện dấu "?" cạnh mắt phải (chỉ dùng cho listening)
    lv_color_t  color;
} emotion_preset_t;

static const emotion_preset_t k_presets[FACE_EMOTION_COUNT] = {
    [FACE_NORMAL] = {
        .eye_d = 86, .eye_y = 0, .gap_x = 65,
        .mask = MASK_NONE, .show_qmark = false,
        .color = LV_COLOR_MAKE(0x66, 0xE0, 0xFF),
    },
    [FACE_HAPPY] = {
        .eye_d = 92, .eye_y = -6, .gap_x = 65,
        .mask = MASK_BOTTOM, .show_qmark = false,   // bụng cong lên
        .color = LV_COLOR_MAKE(0xFF, 0xD7, 0x00),
    },
    [FACE_SAD] = {
        .eye_d = 78, .eye_y = 12, .gap_x = 62,
        .mask = MASK_TOP, .show_qmark = false,      // bụng cong xuống
        .color = LV_COLOR_MAKE(0x66, 0x99, 0xFF),
    },
    [FACE_ANGRY] = {
        .eye_d = 74, .eye_y = 4, .gap_x = 65,
        .mask = MASK_NONE, .show_qmark = false,
        .color = LV_COLOR_MAKE(0xFF, 0x44, 0x44),
    },
    [FACE_LISTENING] = {
        .eye_d = 104, .eye_y = 0, .gap_x = 70,
        .mask = MASK_NONE, .show_qmark = true,
        .color = LV_COLOR_MAKE(0x00, 0xFF, 0x88),
    },
    [FACE_THINKING] = {
        .eye_d = 72, .eye_y = -18, .gap_x = 60,
        .mask = MASK_NONE, .show_qmark = false,
        .color = LV_COLOR_MAKE(0xBB, 0x88, 0xFF),
    },
};

static lv_obj_t *s_face_cont    = NULL;
static lv_obj_t *s_left_grp     = NULL;   // container di chuyển cả cụm mắt trái theo trục Y
static lv_obj_t *s_right_grp    = NULL;
static lv_obj_t *s_left_shape   = NULL;   // hình tròn mắt trái
static lv_obj_t *s_right_shape  = NULL;
static lv_obj_t *s_left_mask    = NULL;   // khối che (cùng màu nền) để tạo bán nguyệt
static lv_obj_t *s_right_mask   = NULL;
static lv_obj_t *s_qmark        = NULL;   // dấu "?" cho trạng thái listening
static lv_timer_t *s_blink_timer = NULL;

static face_emotion_t s_current_emotion = FACE_NORMAL;
static bool s_blinking = false;

// =======================
// Exec callback cho lv_anim (chữ ký bắt buộc: void*, int32_t)
// =======================
static void anim_set_width_cb(void *obj, int32_t v)  { lv_obj_set_width((lv_obj_t *)obj, v); }
static void anim_set_height_cb(void *obj, int32_t v) { lv_obj_set_height((lv_obj_t *)obj, v); }
static void anim_set_y_cb(void *obj, int32_t v)      { lv_obj_set_y((lv_obj_t *)obj, v); }
static void anim_set_opa_cb(void *obj, int32_t v)    { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }

static void animate_prop(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t time_ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_start(&a);
}

// =======================
// Đặt/ẩn khối "mask" để tạo hình bán nguyệt cho 1 mắt
// =======================
static void apply_mask(lv_obj_t *mask, lv_obj_t *shape, mask_mode_t mode, int32_t d, uint32_t transition_ms)
{
    if (mode == MASK_NONE) {
        animate_prop(mask, anim_set_opa_cb, lv_obj_get_style_opa(mask, 0), LV_OPA_TRANSP, transition_ms);
        return;
    }

    // Đặt lại kích thước mask luôn là hình tròn to hơn mắt một chút để che viền
    lv_obj_set_size(mask, d + 10, d + 10);

    // Tính toán offset. 
    // Chia cho 2 (d / 2) sẽ tạo hình lưỡi liềm khá nét. 
    // Nếu muốn lưỡi liềm dày hơn, giảm mẫu số (ví dụ: d / 1.5). 
    // Nếu muốn mỏng hơn, tăng mẫu số (ví dụ: d / 2.5).
    int32_t offset_y = (mode == MASK_BOTTOM) ? (d / 2) : -(d / 2);
    
    lv_obj_align_to(mask, shape, LV_ALIGN_CENTER, 0, offset_y);

    animate_prop(mask, anim_set_opa_cb, lv_obj_get_style_opa(mask, 0), LV_OPA_COVER, transition_ms);
}
// =======================
// Chớp mắt
// =======================
static void blink_ready_cb(lv_anim_t *a)
{
    (void)a;
    s_blinking = false;
}

void face_eyes_blink(void)
{
    if (s_blinking || s_left_shape == NULL || s_right_shape == NULL) {
        return;
    }
    s_blinking = true;

    const emotion_preset_t *p = &k_presets[s_current_emotion];
    bool crescent = (p->mask != MASK_NONE); // happy/sad -> mắt đã là bán nguyệt cố định

    if (crescent) {
        // Không nhắm được vì hình cố định -> rung nhẹ theo trục Y tại chỗ
        const int32_t base_y = p->eye_y;
        const int32_t jitter = 5;

        lv_anim_t a1;
        lv_anim_init(&a1);
        lv_anim_set_var(&a1, s_left_grp);
        lv_anim_set_values(&a1, base_y, base_y - jitter);
        lv_anim_set_time(&a1, 15);
        lv_anim_set_playback_time(&a1, 15);
        lv_anim_set_path_cb(&a1, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a1, anim_set_y_cb);
        lv_anim_start(&a1);

        lv_anim_t a2; 
        lv_anim_init(&a2);
        lv_anim_set_var(&a2, s_right_grp);
        lv_anim_set_values(&a2, base_y, base_y - jitter);
        lv_anim_set_time(&a2, 15);
        lv_anim_set_playback_time(&a2, 15);
        lv_anim_set_path_cb(&a2, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a2, anim_set_y_cb);
        lv_anim_set_ready_cb(&a2, blink_ready_cb);
        lv_anim_start(&a2);
    } else {
        // Nhắm mắt bình thường: ép chiều cao hình tròn xuống gần 0 rồi phồng lại
        const int32_t open_d = p->eye_d;
        const int32_t closed_d = 4;

        lv_anim_t a1;
        lv_anim_init(&a1);
        lv_anim_set_var(&a1, s_left_shape);
        lv_anim_set_values(&a1, open_d, closed_d);
        lv_anim_set_time(&a1, 90);
        lv_anim_set_playback_time(&a1, 90);
        lv_anim_set_path_cb(&a1, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a1, anim_set_height_cb);
        lv_anim_start(&a1);

        lv_anim_t a2;
        lv_anim_init(&a2);
        lv_anim_set_var(&a2, s_right_shape);
        lv_anim_set_values(&a2, open_d, closed_d);
        lv_anim_set_time(&a2, 90);
        lv_anim_set_playback_time(&a2, 90);
        lv_anim_set_path_cb(&a2, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a2, anim_set_height_cb);
        lv_anim_set_ready_cb(&a2, blink_ready_cb);
        lv_anim_start(&a2);
    }
}

static void blink_timer_cb(lv_timer_t *timer)
{
    face_eyes_blink();
    uint32_t next_ms = 2500 + (esp_random() % 3500); // random 2.5s - 6s
    lv_timer_set_period(timer, next_ms);
}

// =======================
// Chuyển biểu cảm
// =======================
void face_eyes_set_emotion(face_emotion_t emotion, uint32_t transition_ms)
{
    if (s_left_shape == NULL || s_right_shape == NULL) {
        return;
    }
    if (emotion >= FACE_EMOTION_COUNT || emotion == s_current_emotion) {
        return;
    }

    const emotion_preset_t *p = &k_presets[emotion];

    animate_prop(s_left_shape,  anim_set_width_cb,  lv_obj_get_width(s_left_shape),   p->eye_d, transition_ms);
    animate_prop(s_left_shape,  anim_set_height_cb, lv_obj_get_height(s_left_shape),  p->eye_d, transition_ms);
    animate_prop(s_right_shape, anim_set_width_cb,  lv_obj_get_width(s_right_shape),  p->eye_d, transition_ms);
    animate_prop(s_right_shape, anim_set_height_cb, lv_obj_get_height(s_right_shape), p->eye_d, transition_ms);

    animate_prop(s_left_grp,  anim_set_y_cb, lv_obj_get_y(s_left_grp),  p->eye_y, transition_ms);
    animate_prop(s_right_grp, anim_set_y_cb, lv_obj_get_y(s_right_grp), p->eye_y, transition_ms);

    lv_obj_set_style_bg_color(s_left_shape,  p->color, 0);
    lv_obj_set_style_bg_color(s_right_shape, p->color, 0);

    // BỔ SUNG: Chuyển đổi trạng thái hiển thị giữa Hình Tròn Đặc và Hình Vòng Cung (Dấu Ngoặc Đơn)
    if (p->mask != MASK_NONE) {
        // Khi biểu cảm yêu cầu hình bán nguyệt (HAPPY / SAD) -> Chuyển thành hình nhẫn rỗng
        lv_obj_set_style_bg_opa(s_left_shape, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(s_right_shape, LV_OPA_TRANSP, 0);
        
        lv_obj_set_style_border_color(s_left_shape, p->color, 0);
        lv_obj_set_style_border_color(s_right_shape, p->color, 0);
        
        // Thay đổi con số 12 này để tăng/giảm độ dày nét vẽ của dấu ngoặc đơn
        lv_obj_set_style_border_width(s_left_shape, 12, 0);
        lv_obj_set_style_border_width(s_right_shape, 12, 0);
    } else {
        // Khi quay lại các biểu cảm dạng tròn (NORMAL, ANGRY...) -> Phục hồi thành hình tròn đặc
        lv_obj_set_style_bg_opa(s_left_shape, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(s_right_shape, LV_OPA_COVER, 0);
        
        lv_obj_set_style_border_width(s_left_shape, 0, 0);
        lv_obj_set_style_border_width(s_right_shape, 0, 0);
    }

    apply_mask(s_left_mask,  s_left_shape,  p->mask, p->eye_d, transition_ms);
    apply_mask(s_right_mask, s_right_shape, p->mask, p->eye_d, transition_ms);

    int32_t qmark_target = p->show_qmark ? LV_OPA_COVER : LV_OPA_TRANSP;
    animate_prop(s_qmark, anim_set_opa_cb, lv_obj_get_style_opa(s_qmark, 0), qmark_target, transition_ms);

    s_current_emotion = emotion;
}

// =======================
// Khởi tạo
// =======================
static lv_obj_t *create_eye_group(lv_obj_t *parent, int16_t gap_x, int16_t eye_y,
                                   int16_t eye_d, lv_color_t color,
                                   lv_obj_t **out_shape, lv_obj_t **out_mask, bool mirror)
{
    lv_obj_t *grp = lv_obj_create(parent);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, 130, 130);
    lv_obj_set_style_bg_opa(grp, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(grp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(grp, LV_ALIGN_CENTER, mirror ? gap_x : -gap_x, eye_y);

    lv_obj_t *shape = lv_obj_create(grp);
    lv_obj_remove_style_all(shape);
    lv_obj_set_size(shape, eye_d, eye_d);
    lv_obj_set_style_radius(shape, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(shape, color, 0);
    lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
    lv_obj_align(shape, LV_ALIGN_CENTER, 0, 0);
lv_obj_t *mask = lv_obj_create(grp);
    lv_obj_remove_style_all(mask);
    
    // Đặt kích thước ban đầu là hình vuông để bo tròn thành hình tròn
    lv_obj_set_size(mask, eye_d + 10, eye_d + 10); 
    lv_obj_set_style_radius(mask, LV_RADIUS_CIRCLE, 0); // Thêm dòng này
    
    lv_obj_set_style_bg_color(mask, lv_color_hex(FACE_BG_COLOR_HEX), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(mask, LV_OPA_TRANSP, 0); 
    lv_obj_align(mask, LV_ALIGN_CENTER, 0, 0);

    *out_shape = shape;
    *out_mask  = mask;
    return grp;
}

void face_eyes_create(lv_obj_t *parent)
{
    const emotion_preset_t *p = &k_presets[FACE_NORMAL];

    s_face_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(s_face_cont);
    lv_obj_set_size(s_face_cont, 300, 180);
    lv_obj_align(s_face_cont, LV_ALIGN_CENTER, -100, -60);
    lv_obj_remove_flag(s_face_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_face_cont, LV_OPA_TRANSP, 0);

    s_left_grp  = create_eye_group(s_face_cont, p->gap_x, p->eye_y, p->eye_d, p->color,
                                    &s_left_shape, &s_left_mask, false);
    s_right_grp = create_eye_group(s_face_cont, p->gap_x, p->eye_y, p->eye_d, p->color,
                                    &s_right_shape, &s_right_mask, true);

    // Dấu "?" cho trạng thái listening, đặt chếch lên-phải mắt phải (hướng ~2 giờ)
    s_qmark = lv_label_create(s_face_cont);
    lv_obj_set_style_text_font(s_qmark, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_qmark, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_opa(s_qmark, LV_OPA_TRANSP, 0);
    lv_label_set_text(s_qmark, "?");
    lv_obj_align_to(s_qmark, s_right_grp, LV_ALIGN_OUT_TOP_RIGHT, 4, 4);

    s_current_emotion = FACE_NORMAL;
    s_blinking = false;

    s_blink_timer = lv_timer_create(blink_timer_cb, 4000, NULL);
}

// =======================
// Parse chuỗi "face" từ JSON server
// =======================
face_emotion_t face_eyes_parse_emotion(const char *face_str)
{
    if (face_str == NULL) return FACE_NORMAL;
    if (strcmp(face_str, "happy")     == 0) return FACE_HAPPY;
    if (strcmp(face_str, "sad")       == 0) return FACE_SAD;
    if (strcmp(face_str, "angry")     == 0) return FACE_ANGRY;
    if (strcmp(face_str, "listening") == 0) return FACE_LISTENING;
    if (strcmp(face_str, "thinking")  == 0) return FACE_THINKING;
    return FACE_NORMAL;
}