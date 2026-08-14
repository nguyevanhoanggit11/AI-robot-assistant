// ui_siri_anim.c
#include "ui.h"
#include "ui_siri_anim.h"
#include <math.h>
#include <stdatomic.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define CYCLE_MS        1000.0f   // 1 chu kỳ "thở" đầy đủ
#define TICK_MS         35        // ~50fps
#define SMOOTH_FACTOR   0.15f     // làm mượt khi audio level đổi đột ngột

static _Atomic float g_level_raw = 0.0f;
static float g_level_smoothed = 0.0f;
static lv_timer_t * g_timer = NULL;

void ui_siri_anim_set_level(float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    atomic_store(&g_level_raw, level);
}

static inline float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// pulse: 0 -> 1 -> 0 trong 1 chu kỳ, đỉnh tại local_t = 0.5 (dùng cho "phồng lên rồi co lại")
static inline float pulse(float local_t)
{
    return (1.0f - cosf(2.0f * (float)M_PI * local_t)) * 0.5f;
}

// wave: dao động 2 chiều -1 -> +1 -> -1 (dùng cho "trượt trái rồi sang phải")
static inline float wave(float local_t)
{
    return sinf(2.0f * (float)M_PI * local_t);
}

// t: pha hiện tại (0-1), delay_ms: độ trễ pha của layer này -> trả về local_t đã lệch pha, luôn trong [0,1)
static inline float phase(float t, float delay_ms)
{
    float lt = t - delay_ms / CYCLE_MS;
    lt = fmodf(lt, 1.0f);
    if (lt < 0.0f) lt += 1.0f;
    return lt;
}

static void set_scale(lv_obj_t * obj, float scale_pct)
{
    // Làm tròn về bước 1% để tăng khả năng trùng cache-key giữa các frame
    float rounded = roundf(scale_pct);
    int16_t s = (int16_t)(256.0f * rounded / 100.0f);
    lv_obj_set_style_transform_scale_x(obj, s, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(obj, s, LV_PART_MAIN);
}
static void set_pivot_center(lv_obj_t * obj)
{
    lv_obj_set_style_transform_pivot_x(obj, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(obj, lv_pct(50), LV_PART_MAIN);
}

static void siri_anim_tick_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);

    // làm mượt audio level
    float target = atomic_load(&g_level_raw);
    g_level_smoothed += (target - g_level_smoothed) * SMOOTH_FACTOR;
    float lvl = g_level_smoothed;

    float t = fmodf(lv_tick_get() / CYCLE_MS, 1.0f);

    // ---- icon-bg: phồng 100% -> ~112% ----
    {
        float lt = phase(t, 0);
        float amp = lerp(3.0f, 15.0f, lvl);           // idle 103% .. loud 115%
        set_scale(ui_image__screen1__iconbg, 100.0f + amp * pulse(lt));
        lv_opa_t opa = (lv_opa_t)lerp(200.0f, 255.0f, pulse(lt)); 
        lv_obj_set_style_opa(ui_image__screen1__iconbg, opa, LV_PART_MAIN);

    }

    // ---- shadow: lan rộng, trễ 40ms so với icon-bg ----
    {
        float lt = phase(t, 40);
        float amp = lerp(4.0f, 22.0f, lvl);
        set_scale(ui_image__screen1__shaow, 100.0f + amp * pulse(lt));
    }

    // ---- blue-right: trái -> phải, trễ 80ms ----
    {
        float lt = phase(t, 80);
        float range = lerp(2.0f, 18.0f, lvl);
        float rot   = lerp(1.0f, 6.0f, lvl);
        lv_obj_set_style_translate_x(ui_image__screen1__blueright, (int32_t)(range * wave(lt)), LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(ui_image__screen1__blueright, (int32_t)(rot * wave(lt) * 10), LV_PART_MAIN);
        set_scale(ui_image__screen1__blueright, 100.0f + lerp(0.0f, 5.0f, lvl) * pulse(lt));
    }

    // ---- blue-middle: ngược hướng blue-right, trễ 120ms ----
    {
        float lt = phase(t, 120);
        float range = lerp(2.0f, 18.0f, lvl);
        lv_obj_set_style_translate_x(ui_image__screen1__bluemid, (int32_t)(-range * wave(lt)), LV_PART_MAIN);
    }

    // ---- green-left / green-left1: lệch pha ngược nhau, trễ 160ms ----
    {
        float lt = phase(t, 160);
        float range = lerp(2.0f, 14.0f, lvl);
        float rot   = lerp(1.0f, 8.0f, lvl);

        lv_obj_set_style_translate_y(ui_image__screen1__greenleft, (int32_t)(-range * wave(lt)), LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(ui_image__screen1__greenleft, (int32_t)(-rot * wave(lt) * 10), LV_PART_MAIN);

        lv_obj_set_style_translate_y(ui_image__screen1__greenleft1, (int32_t)(range * wave(lt)), LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(ui_image__screen1__greenleft1, (int32_t)(rot * wave(lt) * 10), LV_PART_MAIN);
    }

    // ---- pink-left: fade + scale, trễ 200ms ----
    {
        float lt = phase(t, 200);
        float p = pulse(lt);
        lv_opa_t opa = (lv_opa_t)lerp(120.0f, 255.0f, p * lerp(0.4f, 1.0f, lvl));
        lv_obj_set_style_opa(ui_image__screen1__pinkleft, opa, LV_PART_MAIN);
        set_scale(ui_image__screen1__pinkleft, 100.0f + lerp(1.0f, 8.0f, lvl) * p);
    }

    // ---- pink-top / bottom-pink: ngược pha nhau, trễ 220ms ----
    {
        float lt = phase(t, 220);
        float range = lerp(2.0f, 16.0f, lvl);
        float p = pulse(lt);
        lv_obj_set_style_translate_y(ui_image__screen1__pinktop, (int32_t)(-range * p), LV_PART_MAIN);
        lv_obj_set_style_translate_y(ui_image__screen1__bottompink, (int32_t)(range * p), LV_PART_MAIN);
    }

    // ---- highlight: quét trái -> phải liên tục, sáng nhất giữa chu kỳ ----
    {
        float range = 30.0f;
        lv_obj_set_style_translate_x(ui_image__screen1__hightlight, (int32_t)lerp(-range, range, t), LV_PART_MAIN);
        lv_opa_t opa = (lv_opa_t)lerp(140.0f, 255.0f, pulse(t)) ;
        opa = (lv_opa_t)(opa + lerp(0.0f, 20.0f, lvl));
        if (opa > 255) opa = 255;
        lv_obj_set_style_opa(ui_image__screen1__hightlight, opa, LV_PART_MAIN);
    }

    // ---- intersect: gần như tĩnh, chỉ nhích nhẹ theo icon-bg ----
    {
        float lt = phase(t, 0);
        set_scale(ui_image__screen1__intersect, 100.0f + (1.0f + lvl) * pulse(lt));
    }
}

void ui_siri_anim_init(void)
{
    lv_obj_t * layers[] = {
        ui_image__screen1__iconbg, ui_image__screen1__shaow,
        ui_image__screen1__blueright, ui_image__screen1__bluemid,
        ui_image__screen1__greenleft, ui_image__screen1__greenleft1,
        ui_image__screen1__pinkleft, ui_image__screen1__pinktop,
        ui_image__screen1__bottompink, ui_image__screen1__hightlight,
        ui_image__screen1__intersect
    };
    for (size_t i = 0; i < sizeof(layers)/sizeof(layers[0]); i++) {
        set_pivot_center(layers[i]);
    }

    if (g_timer == NULL) {
        g_timer = lv_timer_create(siri_anim_tick_cb, TICK_MS, NULL);
    }
}