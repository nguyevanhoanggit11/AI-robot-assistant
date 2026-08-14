#pragma once

/**
 * @brief Khởi tạo phần cứng màn hình và giao diện LVGL
 */
void display_init(void);

/**
 * @brief Cập nhật trạng thái biểu cảm và phụ đề từ kết quả phản hồi của server
 * @param face Chuỗi biểu cảm ("happy", "sad", "angry", "listening", "thinking")
 * @param speech Nội dung phụ đề thoại
 */
void display_update(const char *face, const char *speech);

/**
 * @brief Chuyển giao diện sang trạng thái đang lắng nghe (Wake word detected)
 */
void display_set_listening(void);

/**
 * @brief Chuyển giao diện sang trạng thái đang xử lý suy nghĩ
 */
void display_set_thinking(void);

/**
 * @brief Đặt lại giao diện về trạng thái chờ mặc định (Idle)
 */
void display_reset_to_idle(void);