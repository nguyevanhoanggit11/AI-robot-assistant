# Hướng dẫn Cấu hình và Xử lý Lỗi (ESP32 / ESP-IDF)
# Trong quá trình làm tuyệt đối không được bấm vào nút chọn lại chip vì sẽ phải set menuconfig lại từ đầu
Tài liệu này tổng hợp các cấu hình bắt buộc và cách xử lý một số lỗi thường gặp trong quá trình phát triển dự án nhúng với ESP32 trên môi trường ESP-IDF.

## 1. Các Cấu Hình Bắt Buộc Trong Menuconfig
Để đảm bảo hệ thống hoạt động ổn định và tránh lỗi phát sinh, cần bật/điều chỉnh các thông số sau trong giao diện cấu hình (`idf.py menuconfig`):

*   **Bộ nhớ:**
    *   Thiết lập Flash size: **16MB flash**.
    *   Kích hoạt PSRAM: Bật **Enable PS RAM support**.
*   **Kết nối mạng:**
    *   Wifi: Bật **SDIO WiFi**.
    *   LWIP: Điều hướng đến `Component config` → `lwip` → Bật **Enable PPP support**.
*   **Cấu hình hệ thống khác:**
    *   Sử dụng bảng phân vùng tùy chỉnh: Chọn **Custom partitions**.
    *   Cấu hình dao động/tần số: Đặt **48 MHz** (48 monesat).

## 2. Các Lỗi Thường Gặp Và Cách Xử Lý

### 2.1. Lỗi Màn Hình (Tràn bộ nhớ Task)
*   **Hiện tượng:** Quá trình chạy bị crash, lỗi khởi tạo giao diện màn hình hoặc hệ thống báo lỗi tràn stack (Stack overflow).
*   **Cách xử lý:** Cần tăng kích thước bộ nhớ cấp phát cho Main task.
    1. Vào `Component config` → `ESP System Settings`
    2. Chỉnh thông số `Main task stack size` lên mức **12288**.

### 2.2. Lỗi Version Không Thích Hợp (Phần cứng ESP32-P4)
*   **Hiện tượng:** Quá trình build báo lỗi không tương thích phiên bản chip (incompatible version).
*   **Cách xử lý:** Giảm mức yêu cầu revision phần cứng tối thiểu.
    1. Vào `Component config` → `Hardware Settings`
    2. Tìm mục `Minimum Supported ESP32-P4 Revision` và thiết lập về giá trị **0.0**.

## 3. Cấu Hình File `idf_component.yml` (Quan trọng)
Quản lý dependency trong ESP-IDF rất dễ phát sinh xung đột phiên bản, đặc biệt là với các thư viện đồ họa và mạng. Dưới đây là nội dung chuẩn cho file `idf_component.yml` đã được kiểm chứng hoạt động ổn định. Khi gặp lỗi build liên quan đến thư viện, hãy đối chiếu lại với cấu hình này:

```yaml
dependencies:
  idf:
    version: '>=5.0.0'

  espressif/es8311: ^1.0
  espressif/esp_lvgl_port:
    version: '>=2.5.0'
  espressif/esp_lvgl_adapter: ^0.5.1
  espressif/esp-sr: '*'
  espressif/esp_websocket_client: '>=1.0.0'
  
  espressif/esp_wifi_remote:
    matches:
    - if: idf_version >= 6.0
      version: '>=1.6,<2.0'
    - if: idf_version < 6.0
      version: 0.14.*

  espressif/esp_hosted:
    matches:
    - if: idf_version >= 6.0
      version: '>=2.12,<3.0'
    - if: idf_version < 6.0
      version: 1.4.*
      
  trombik/esp_wireguard: ^0.9.0
  lvgl/lvgl: "~9.2.0"
  espressif/esp_lv_adapter: "*"
```
