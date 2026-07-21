#pragma once

// Khởi tạo màn hình
void display_init(void);

// Cập nhật sau khi nhận JSON từ server
// face: "happy", "sad", "angry", "surprised", "neutral"
void display_update(const char *face, const char *speech);

// Gọi khi wake word detected
void display_set_listening(void);

// Gọi khi đang chờ server xử lý
void display_set_thinking(void);