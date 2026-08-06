#include "display.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_lv_adapter.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_host_ll.h"
#include "hal/mipi_dsi_types.h"
#include "lvgl.h"
#include "face_eyes.h"

static const char *TAG = "DISPLAY";

// =======================
// Waveshare 4.3" DSI Cap Touch rev2.2 (cầu TC358762 + ATTINY PMIC @0x45)
// Ghép qua cáp FFC vào ESP32-P4-Module-DEV-KIT
// =======================
#define LCD_H_RES              800
#define LCD_V_RES              480
#define LCD_DPI_CLK_MHZ        26
#define LCD_HSW                1
// --- ĐÃ SỬA: Điều chỉnh Timings phần cứng để căn giữa màn hình ---
#define LCD_HBP                26   // Giảm từ 46 xuống 26 để dịch màn hình sang trái
#define LCD_HFP                230  // Tăng từ 210 lên 230 để giữ nguyên H_Total
// -----------------------------------------------------------------
#define LCD_VSW                3
#define LCD_VBP                29
#define LCD_VFP                13
#define LCD_PIXEL_BYTES        4
#define LCD_MIPI_DSI_LANE_NUM  1
#define LCD_MIPI_DSI_LANE_MBPS 800
#define LCD_LDO_CHAN           3
#define LCD_LDO_VOLTAGE_MV     2500
#define LV_TEAR_MODE           ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL
#define LV_ROTATION            ESP_LV_ADAPTER_ROTATE_0


#define LCD_I2C_PORT           I2C_NUM_1
#define LCD_I2C_SDA            7   
#define LCD_I2C_SCL            8   
#define LCD_BL_I2C_ADDR        0x45
#define PMIC_I2C_HZ            100000
#define PMIC_I2C_TIMEOUT_MS    100

#define PMIC_REG_ID            0x80
#define PMIC_REG_PORTA         0x81
#define PMIC_REG_PORTB         0x82
#define PMIC_REG_PORTC         0x83
#define PMIC_REG_PWM           0x86
#define PMIC_REG_ADDR_L        0x8C
#define PMIC_REG_ADDR_H        0x8D
#define PMIC_REG_WRITE_DATA_H  0x90
#define PMIC_REG_WRITE_DATA_L  0x91
#define PMIC_PA_LCD_LR         0x04
#define PMIC_PB_LCD_MAIN        0x80
#define PMIC_PC_LED_EN          0x01
#define PMIC_PC_RST_LCD_N       0x04
#define PMIC_PC_RST_BRIDGE_N    0x08
#define PMIC_PORTC_ACTIVE      (PMIC_PC_LED_EN | PMIC_PC_RST_LCD_N | PMIC_PC_RST_BRIDGE_N)

#define TC358762_DSI_LANEENABLE        0x0210
#define TC358762_PPI_D0S_CLRSIPOCOUNT  0x0164
#define TC358762_PPI_D1S_CLRSIPOCOUNT  0x0168
#define TC358762_PPI_D0S_ATMR          0x0144
#define TC358762_PPI_D1S_ATMR          0x0148
#define TC358762_PPI_LPTXTIMECNT       0x0114
#define TC358762_SPICMR                0x0450
#define TC358762_LCDCTRL               0x0420
#define TC358762_SYSCTRL               0x0464
#define TC358762_LCD_HS_HBP            0x0424
#define TC358762_LCD_HDISP_HFP         0x0428
#define TC358762_LCD_VS_VBP            0x042C
#define TC358762_LCD_VDISP_VFP         0x0430
#define TC358762_PPI_STARTPPI          0x0104
#define TC358762_DSI_STARTDSI          0x0204
#define TC358762_LCDCTRL_VAL           0x00100150
#define TC358762_LANE_CLK_D0           0x03

typedef struct {
    int bus_id;
    mipi_dsi_hal_context_t hal;
} lcd_dsi_bus_access_t;

static esp_lcd_panel_handle_t s_panel;
static bool s_pmic_power_on;
static lv_display_t *lvgl_display = NULL;
static bool s_i2c_initialized = false;

// =======================
// LVGL UI Objects
// =======================
static lv_obj_t *ui_speech_label = NULL;
static lv_obj_t *ui_state_label  = NULL;

// =======================
// PMIC (ATTINY) - cấp nguồn LCD + điều khiển backlight
// =======================
static esp_err_t pmic_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(LCD_I2C_PORT, LCD_BL_I2C_ADDR, buf, sizeof(buf), pdMS_TO_TICKS(PMIC_I2C_TIMEOUT_MS));
}

static esp_err_t pmic_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(LCD_I2C_PORT, LCD_BL_I2C_ADDR, &reg, 1, val, 1, pdMS_TO_TICKS(PMIC_I2C_TIMEOUT_MS));
}

static esp_err_t pmic_i2c_init(void)
{
    if (s_i2c_initialized) {
        return ESP_OK;
    }

    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = LCD_I2C_SDA,
        .scl_io_num = LCD_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = PMIC_I2C_HZ,
    };
    
    ESP_RETURN_ON_ERROR(i2c_param_config(LCD_I2C_PORT, &i2c_conf), TAG, "i2c param config failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(LCD_I2C_PORT, i2c_conf.mode, 0, 0, 0), TAG, "i2c driver install failed");
    
    s_i2c_initialized = true;
    return ESP_OK;
}

static esp_err_t pmic_probe(void)
{
    uint8_t id = 0;
    for (int i = 0; i < 20; i++) {
        if (pmic_read(PMIC_REG_ID, &id) == ESP_OK) {
            ESP_LOGI(TAG, "PMIC ID 0x%02X", id);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGE(TAG, "PMIC @ 0x45 không phản hồi — kiểm tra lại cáp FFC / chân GPIO I2C");
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t pmic_power_on_sequence(void)
{
    s_pmic_power_on = false;

    ESP_RETURN_ON_ERROR(pmic_write(PMIC_REG_PORTC, 0x00), TAG, "PORTC rst");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(pmic_write(PMIC_REG_PORTA, PMIC_PA_LCD_LR), TAG, "PORTA");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(pmic_write(PMIC_REG_PORTB, PMIC_PB_LCD_MAIN), TAG, "PORTB");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(pmic_write(PMIC_REG_PORTC, PMIC_PC_LED_EN), TAG, "LED_EN");
    vTaskDelay(pdMS_TO_TICKS(80));

    s_pmic_power_on = true;
    ESP_LOGI(TAG, "Nguồn LCD (qua ATTINY) đã bật (bridge vẫn đang reset)");
    return ESP_OK;
}

static esp_err_t pmic_bridge_reset_release(void)
{
    ESP_RETURN_ON_ERROR(pmic_write(PMIC_REG_PORTC, PMIC_PORTC_ACTIVE), TAG, "PORTC rel");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(pmic_write(PMIC_REG_ADDR_H, 0x04), TAG, "ADDR_H");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(pmic_write(PMIC_REG_ADDR_L, 0x7C), TAG, "ADDR_L");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(pmic_write(PMIC_REG_WRITE_DATA_H, 0x00), TAG, "DATA_H");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(pmic_write(PMIC_REG_WRITE_DATA_L, 0x00), TAG, "DATA_L");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Đã giải phóng reset TC358762");
    return ESP_OK;
}

static esp_err_t pmic_backlight_set(uint8_t brightness)
{
    if (!s_pmic_power_on) {
        return ESP_FAIL;
    }
    return pmic_write(PMIC_REG_PWM, brightness);
}

static esp_err_t lcd_phy_power_on(void)
{
    esp_ldo_channel_handle_t ldo = NULL;
    const esp_ldo_channel_config_t cfg = {
        .chan_id = LCD_LDO_CHAN,
        .voltage_mv = LCD_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&cfg, &ldo), TAG, "DSI PHY LDO");
    ESP_LOGI(TAG, "MIPI DSI PHY đã cấp nguồn");
    return ESP_OK;
}

// =======================
// Cầu TC358762 (DSI -> RGB)
// =======================
static void tc358762_reg_write(esp_lcd_dsi_bus_handle_t bus, uint16_t reg, uint32_t val)
{
    lcd_dsi_bus_access_t *dsi = (lcd_dsi_bus_access_t *)bus;
    const uint8_t payload[6] = {
        reg & 0xFF,
        reg >> 8,
        val & 0xFF,
        (val >> 8) & 0xFF,
        (val >> 16) & 0xFF,
        (val >> 24) & 0xFF,
    };
    mipi_dsi_hal_host_gen_write_long_packet(&dsi->hal, 0, MIPI_DSI_DT_GENERIC_LONG_WRITE,
                                            payload, sizeof(payload));
}

static esp_err_t lcd_tc358762_init(esp_lcd_dsi_bus_handle_t bus)
{
    const uint32_t hs_hbp = (LCD_HBP << 16) | LCD_HSW;
    const uint32_t hdisp_hfp = (LCD_HFP << 16) | LCD_H_RES;
    const uint32_t vs_vbp = (LCD_VBP << 16) | LCD_VSW;
    const uint32_t vdisp_vfp = (LCD_VFP << 16) | LCD_V_RES;

    ESP_LOGI(TAG, "Cấu hình TC358762");
    tc358762_reg_write(bus, TC358762_DSI_LANEENABLE, TC358762_LANE_CLK_D0);
    tc358762_reg_write(bus, TC358762_PPI_D0S_CLRSIPOCOUNT, 0x05);
    tc358762_reg_write(bus, TC358762_PPI_D1S_CLRSIPOCOUNT, 0x05);
    tc358762_reg_write(bus, TC358762_PPI_D0S_ATMR, 0x00);
    tc358762_reg_write(bus, TC358762_PPI_D1S_ATMR, 0x00);
    tc358762_reg_write(bus, TC358762_PPI_LPTXTIMECNT, 0x03);
    tc358762_reg_write(bus, TC358762_SPICMR, 0x00);
    tc358762_reg_write(bus, TC358762_LCDCTRL, TC358762_LCDCTRL_VAL);
    tc358762_reg_write(bus, TC358762_SYSCTRL, 0x040F);
    tc358762_reg_write(bus, TC358762_LCD_HS_HBP, hs_hbp);
    tc358762_reg_write(bus, TC358762_LCD_HDISP_HFP, hdisp_hfp);
    tc358762_reg_write(bus, TC358762_LCD_VS_VBP, vs_vbp);
    tc358762_reg_write(bus, TC358762_LCD_VDISP_VFP, vdisp_vfp);
    vTaskDelay(pdMS_TO_TICKS(100));
    tc358762_reg_write(bus, TC358762_PPI_STARTPPI, 0x01);
    tc358762_reg_write(bus, TC358762_DSI_STARTDSI, 0x01);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "TC358762 sẵn sàng");
    return ESP_OK;
}

static esp_err_t lcd_dsi_latch_lp_gen_writes(esp_lcd_dsi_bus_handle_t bus)
{
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &dbi_io), TAG, "DBI LP latch");
    return esp_lcd_panel_io_del(dbi_io);
}

// =======================
// Khởi tạo phần cứng LCD
// =======================
static esp_err_t lcd_hw_init(void)
{
    esp_lcd_dsi_bus_handle_t bus = NULL;

    ESP_LOGI(TAG, "Waveshare 4.3\" DSI: %dx%d @ %dMHz", LCD_H_RES, LCD_V_RES, LCD_DPI_CLK_MHZ);

    ESP_RETURN_ON_ERROR(pmic_i2c_init(), TAG, "I2C");
    ESP_RETURN_ON_ERROR(pmic_probe(), TAG, "PMIC probe");
    ESP_RETURN_ON_ERROR(lcd_phy_power_on(), TAG, "PHY");
    ESP_RETURN_ON_ERROR(pmic_power_on_sequence(), TAG, "PMIC power");

    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = LCD_MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = LCD_MIPI_DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "DSI bus");
    ESP_RETURN_ON_ERROR(lcd_dsi_latch_lp_gen_writes(bus), TAG, "LP latch");

    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_DPI_CLK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
        .in_color_format = LCD_COLOR_FMT_RGB888,
        .out_color_format = LCD_COLOR_FMT_RGB888,
        .num_fbs = esp_lv_adapter_get_required_frame_buffer_count(LV_TEAR_MODE, LV_ROTATION),
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES,
            .hsync_back_porch = LCD_HBP,
            .hsync_pulse_width = LCD_HSW,
            .hsync_front_porch = LCD_HFP,
            .vsync_back_porch = LCD_VBP,
            .vsync_pulse_width = LCD_VSW,
            .vsync_front_porch = LCD_VFP,
        },
        .flags = {
            .use_dma2d = true,
            .disable_lp = false,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(bus, &dpi_cfg, &s_panel), TAG, "DPI panel");

    lcd_dsi_bus_access_t *dsi = (lcd_dsi_bus_access_t *)bus;
    mipi_dsi_host_ll_dpi_set_video_burst_type(dsi->hal.host, MIPI_DSI_LL_VIDEO_NON_BURST_WITH_SYNC_PULSES);
    mipi_dsi_host_ll_dpi_enable_frame_ack(dsi->hal.host, false);

    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");

    mipi_dsi_host_ll_set_clock_lane_state(dsi->hal.host, MIPI_DSI_LL_CLOCK_LANE_STATE_HS);
    mipi_dsi_host_ll_enable_cmd_ack(dsi->hal.host, false);

    ESP_RETURN_ON_ERROR(pmic_bridge_reset_release(), TAG, "bridge reset");
    ESP_RETURN_ON_ERROR(lcd_tc358762_init(bus), TAG, "TC358762");
    ESP_RETURN_ON_ERROR(pmic_backlight_set(255), TAG, "backlight");

    ESP_LOGI(TAG, "LCD sẵn sàng");
    return ESP_OK;
}

// =======================
// DISPLAY INIT
// =======================
LV_FONT_DECLARE(font_vietnamese_14);
void display_init(void)
{
    ESP_LOGI(TAG, "Khởi tạo màn hình Waveshare 4.3\" DSI...");

    // --- ĐÃ SỬA: Ghim tác vụ vẽ LVGL vào Core 0 ---
    esp_lv_adapter_config_t lv_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    //lv_cfg.task_affinity = 0; // Cực kỳ quan trọng để chống xung đột với âm thanh ở Core 1
    ESP_ERROR_CHECK(esp_lv_adapter_init(&lv_cfg));

    ESP_ERROR_CHECK(lcd_hw_init());

    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = NULL,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
            .rotation = LV_ROTATION,
            .hor_res = LCD_H_RES,
            .ver_res = LCD_V_RES,
            .buffer_height = LCD_V_RES,
            .use_psram = true,
            .enable_ppa_accel = false,
            .require_double_buffer = false,
        },
        .tear_avoid_mode = LV_TEAR_MODE,
    };

    lvgl_display = esp_lv_adapter_register_display(&disp_cfg);
    if (lvgl_display == NULL) {
        ESP_LOGE(TAG, "Lỗi tạo LVGL display!");
        return;
    }
    lv_display_set_physical_resolution(lvgl_display, LCD_H_RES, LCD_V_RES);
    
    lv_display_set_offset(lvgl_display, -400, 0);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    // --- Build UI ---
    esp_lv_adapter_lock(-1);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_pos(screen, 0, 0);
    lv_obj_set_size(screen, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x1A1A2E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // MASTER CONTAINER1
    lv_obj_t *ui_cont = lv_obj_create(screen);
    lv_obj_remove_style_all(ui_cont); 
    lv_obj_set_size(ui_cont, LCD_H_RES, LCD_V_RES); 
    lv_obj_remove_flag(ui_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(ui_cont, LV_ALIGN_CENTER, -100, 0); 

    // State label
    ui_state_label = lv_label_create(ui_cont); 
    lv_obj_set_style_text_font(ui_state_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_state_label, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_label_set_text(ui_state_label, "AI robot assistant");
    
    // --- ĐÃ SỬA: Đưa X về 0, giảm Y xuống 5 để không đè lên dấu "?" ---
    lv_obj_align(ui_state_label, LV_ALIGN_TOP_MID, 0, 5); 

    // Mắt
    face_eyes_create(ui_cont);

    // Speech label 
    ui_speech_label = lv_label_create(ui_cont);
    
    // --- ĐÃ SỬA: Box text giới hạn chu vi + Hiệu ứng tự cuộn (SCROLL) ---
    lv_obj_set_width(ui_speech_label, 600); 
    lv_obj_set_height(ui_speech_label, 130); // Đủ chỗ cho 2-3 dòng
    lv_label_set_long_mode(ui_speech_label, LV_LABEL_LONG_WRAP); 
    
    lv_obj_set_style_text_font(ui_speech_label, &font_vietnamese_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_speech_label, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_speech_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ui_speech_label, "Xin chào bạn, tôi là trợ lý AI robot, tôi có thể giúp gì cho bạn?");
    
    // Đưa X về 0 (vì phần cứng đã tự căn giữa), đẩy Y xuống dưới một chút
    lv_obj_align(ui_speech_label, LV_ALIGN_CENTER, 0, 120); 

    lv_refr_now(lvgl_display);
    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "Màn hình sẵn sàng.");
}

// =======================
// SUBTITLE QUEUE (hiển thị lời thoại theo từng đoạn, kiểu phụ đề)
// =======================
#define MAX_SUBTITLE_SEGMENTS 24
#define MAX_SUBTITLE_SEG_LEN  220
#define SUBTITLE_MS_PER_CHAR  40       // ~20 ký tự/giây — chỉnh lại nếu vẫn lệch so với giọng đọc thực tế
#define SUBTITLE_MIN_MS       1200

static char subtitle_segments[MAX_SUBTITLE_SEGMENTS][MAX_SUBTITLE_SEG_LEN];
static int  subtitle_segment_count  = 0;
static int  subtitle_current_index  = 0;
static lv_timer_t *subtitle_timer   = NULL;

// Đếm số ký tự UTF-8 (bỏ qua byte tiếp diễn) để ước lượng thời gian đọc
static int utf8_strlen(const char *s)
{
    int count = 0;
    while (*s) {
        if (((unsigned char)*s & 0xC0) != 0x80) count++;
        s++;
    }
    return count;
}

// Tách speech thành các đoạn theo dấu câu; nếu đoạn quá dài thì cắt tại
// khoảng trắng gần nhất (tránh đứt giữa ký tự UTF-8 nhiều byte)
static int split_into_subtitles(const char *speech)
{
    int seg_count = 0;
    int seg_len   = 0;
    int last_space_in_seg = -1;

    subtitle_segments[0][0] = '\0';

    for (const char *p = speech; *p != '\0'; p++) {
        if (seg_count >= MAX_SUBTITLE_SEGMENTS) break;

        if (seg_len < MAX_SUBTITLE_SEG_LEN - 1) {
            subtitle_segments[seg_count][seg_len++] = *p;
            subtitle_segments[seg_count][seg_len] = '\0';
        }

        if (*p == ' ') last_space_in_seg = seg_len - 1;

        bool is_punct    = (*p == '.' || *p == ',' || *p == '!' || *p == '?' || *p == ':' || *p == ';');
        bool force_split = (seg_len >= MAX_SUBTITLE_SEG_LEN - 1);

        if (is_punct || force_split) {
            const char *rest = NULL;
            if (force_split && last_space_in_seg > 0) {
                rest = &subtitle_segments[seg_count][last_space_in_seg + 1];
                subtitle_segments[seg_count][last_space_in_seg] = '\0';
            }

            seg_count++;
            if (seg_count >= MAX_SUBTITLE_SEGMENTS) break;

            seg_len = 0;
            last_space_in_seg = -1;
            subtitle_segments[seg_count][0] = '\0';

            if (rest) {
                seg_len = (int)strlen(rest);
                if (seg_len >= MAX_SUBTITLE_SEG_LEN) seg_len = MAX_SUBTITLE_SEG_LEN - 1;
                memcpy(subtitle_segments[seg_count], rest, seg_len);
                subtitle_segments[seg_count][seg_len] = '\0';
            }

            // Bỏ qua các khoảng trắng ngay sau dấu câu, tránh đoạn kế tiếp
            // bắt đầu bằng dấu cách (gây cảm giác hụt chữ đầu dòng)
            while (*(p + 1) == ' ') p++;
        }
    }

    if (seg_len > 0 && seg_count < MAX_SUBTITLE_SEGMENTS) seg_count++;

    return seg_count;
}

static void subtitle_show_segment(int idx)
{
    if (idx < 0 || idx >= subtitle_segment_count) return;
    lv_label_set_text(ui_speech_label, subtitle_segments[idx]);
}

// Callback timer LVGL: đã chạy sẵn trong context có lock của esp_lv_adapter,
// KHÔNG tự lock/unlock lại ở đây để tránh deadlock.
static void subtitle_timer_cb(lv_timer_t *timer)
{
    subtitle_current_index++;

    if (subtitle_current_index >= subtitle_segment_count) {
        lv_timer_del(subtitle_timer);
        subtitle_timer = NULL;
        return;
    }

    subtitle_show_segment(subtitle_current_index);

    int chars = utf8_strlen(subtitle_segments[subtitle_current_index]);
    uint32_t duration_ms = chars * SUBTITLE_MS_PER_CHAR;
    if (duration_ms < SUBTITLE_MIN_MS) duration_ms = SUBTITLE_MIN_MS;
    lv_timer_set_period(subtitle_timer, duration_ms);
}

// Bắt đầu hiển thị 1 câu nói mới kiểu phụ đề. Gọi từ display_update() (đã lock sẵn).
static void subtitle_start(const char *speech)
{
    if (subtitle_timer != NULL) {
        lv_timer_del(subtitle_timer);
        subtitle_timer = NULL;
    }

    subtitle_segment_count = split_into_subtitles(speech);
    subtitle_current_index = 0;

    if (subtitle_segment_count == 0) {
        lv_label_set_text(ui_speech_label, "");
        return;
    }

    subtitle_show_segment(0);

    if (subtitle_segment_count > 1) {
        int chars = utf8_strlen(subtitle_segments[0]);
        uint32_t duration_ms = chars * SUBTITLE_MS_PER_CHAR;
        if (duration_ms < SUBTITLE_MIN_MS) duration_ms = SUBTITLE_MIN_MS;
        subtitle_timer = lv_timer_create(subtitle_timer_cb, duration_ms, NULL);
    }
}

// =======================
// DISPLAY UPDATE
// =======================
void display_update(const char *face, const char *speech)
{
    if (ui_speech_label == NULL) return;

    esp_lv_adapter_lock(-1);

    face_eyes_set_emotion(face_eyes_parse_emotion(face), 300);

    lv_label_set_text(ui_state_label, "AI robot assistant");
    lv_obj_set_style_text_color(ui_state_label, lv_color_hex(0x00FF88), LV_PART_MAIN);

    subtitle_start(speech ? speech : "");

    esp_lv_adapter_unlock();
}

// =======================
// Reset màn hình về trạng thái chờ ban đầu (gọi khi phát xong 1 lượt hội thoại)
// =======================
void display_reset_to_idle(void)
{
    if (ui_speech_label == NULL || ui_state_label == NULL) return;

    esp_lv_adapter_lock(-1);

    if (subtitle_timer != NULL) {
        lv_timer_del(subtitle_timer);
        subtitle_timer = NULL;
    }
    subtitle_segment_count  = 0;
    subtitle_current_index  = 0;

    face_eyes_set_emotion(FACE_NORMAL, 300);

    lv_label_set_text(ui_state_label, "AI robot assistant");
    lv_obj_set_style_text_color(ui_state_label, lv_color_hex(0x888888), LV_PART_MAIN);

    lv_label_set_text(ui_speech_label, "Xin chào bạn, tôi là trợ lý AI robot, tôi có thể giúp gì cho bạn?");

    esp_lv_adapter_unlock();
}

// =======================
// Gọi khi wake word detected
// =======================
void display_set_listening(void)
{
    if (ui_state_label == NULL) return;
    esp_lv_adapter_lock(-1);
    lv_label_set_text(ui_state_label, "LISTENING...");
    lv_obj_set_style_text_color(ui_state_label, lv_color_hex(0xFF4444), LV_PART_MAIN);
    face_eyes_set_emotion(FACE_LISTENING, 250);
    lv_label_set_text(ui_speech_label, "Dang nghe...");
    esp_lv_adapter_unlock();
}

// =======================
// Gọi khi đang chờ LLM xử lý
// =======================
void display_set_thinking(void)
{
    if (ui_state_label == NULL) return;
    esp_lv_adapter_lock(-1);
    lv_label_set_text(ui_state_label, "THINKING...");
    lv_obj_set_style_text_color(ui_state_label, lv_color_hex(0xFFAA00), LV_PART_MAIN);
    face_eyes_set_emotion(FACE_THINKING, 250);
    lv_label_set_text(ui_speech_label, "Dang xu ly...");
    esp_lv_adapter_unlock();
}