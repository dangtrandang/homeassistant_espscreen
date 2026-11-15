# Tessera (ESP Screens) for Freenove ESP32-S3 2.8" IPS Capacitive Touch (FNK0104)

Bản tùy biến nâng cao của [MaxGramser/homeassistant_espscreen](https://github.com/MaxGramser/homeassistant_espscreen) dành riêng cho bo mạch **Freenove ESP32-S3 2.8" Capacitive Touch (FNK0104 - ILI9341 IPS 240x320 + FT6336)**, hỗ trợ kết nối từ xa về **Home Assistant trên Cloud VPS** xuyên qua CGNAT/Firewall và bổ sung nhiều cải tiến thực tế cho thiết bị tại Việt Nam.

---

## Tính năng & Các bản vá đã tích hợp (Custom Patches)

1. **Hỗ trợ phần cứng Freenove ESP32-S3 2.8" (FNK0104)**:
   - Vi xử lý ESP32-S3 với **16MB Flash** và **8MB Octal PSRAM (80MHz)**, nâng giới hạn lên **128 tiles / 24 pages**.
   - Cấu hình màn hình **ILI9341 IPS 240x320** qua SPI (`CLK: GPIO12`, `MOSI: GPIO11`, `CS: GPIO10`, `DC: GPIO46`, `RST: GPIO4`, `BL: GPIO45`) với `invert_colors: true` để hiển thị đúng dải màu trắng/đen của tấm nền IPS.
   - Cảm ứng điện dung **FT6336 (ft63x6)** qua I2C (`SDA: GPIO16`, `SCL: GPIO15`, `INT: GPIO17`, `RST: GPIO18`).

2. **Reverse TCP Tunnel xuyên CGNAT/Firewall (`tcp_tunnel.h`)**:
   - Giải quyết triệt để vấn đề nhà mạng chặn gói tin UDP VPN (WireGuard) hoặc mạng LAN sau NAT không mở được port.
   - Mạch ESP32 chủ động mở kết nối Reverse TCP (`CTL` / `DAT`) lên Cloud VPS (`port 51821`), tự động nối cầu (bridge) cổng `127.0.0.1:6053` trên VPS về ESPHome API (`port 6053`) trên mạch.

3. **Hỗ trợ đầy đủ Font chữ Tiếng Việt có dấu**:
   - Tích hợp trọn bộ ký tự Tiếng Việt có dấu (`SCRIPT_GLYPHS`) vào tất cả các cấp độ font chữ (`headline`, `watch_value`, `label`, `sublabel`, `sublabel_big`).

4. **Sửa lỗi bật/tắt công tắc Tuya không phản hồi trạng thái (`components/smart_display/optimistic.h`)**:
   - Nhiều thiết bị công tắc Tuya trên Home Assistant nhận lệnh `switch.turn_off` / `switch.turn_on` và thực thi thành công nhưng không đẩy sự kiện `state_changed` ngược về Home Assistant, khiến bản gốc tự động revert trạng thái nút về `'on'` sau 2 giây (dẫn đến chỉ tắt được mà không bật lại được).
   - Bản vá trong `optimistic::answer()` và `optimistic::tick()` giữ nguyên trạng thái mới ngay khi Home Assistant xác nhận dịch vụ thực thi thành công (`ok == true`), loại bỏ hiện tượng treo `Updating...` hoặc tự trả về trạng thái cũ.

5. **Hạ thời gian chờ (Standby After) xuống tối thiểu `3 giây` & Chỉnh từng `1 giây`**:
   - Mở khóa giới hạn cứng `60s` của bản gốc xuống **`3s`** (`min_value: 3`, `step: 1`) trong `screen_settings.h`, `settings_screen.h`, `packages/core.yaml`, `screen_manager/app/core.py` và `web/src/model/settings.ts`.
   - Hỗ trợ tăng/giảm từng `1 giây` (`3s, 4s, 5s...`) ngay trong menu cài đặt trên màn hình hoặc từ Home Assistant.

6. **Cuộn trang xoay vòng vô hạn (`1 → 2 → 3 → 1`) (`components/smart_display/page_protocol.h`)**:
   - Cập nhật hàm `Pages::step()` xoay vòng theo modulo tổng số trang, giúp 2 nút điều hướng `<` và `>` luôn hiển thị ở mọi trang: từ trang cuối bấm `>` về thẳng trang 1 và từ trang 1 bấm `<` sang thẳng trang cuối.

7. **Đồng bộ thời gian chuẩn Việt Nam (`ICT-7`) qua Google NTP**:
   - Tách riêng đồng hồ hệ thống theo máy chủ SNTP (`216.239.35.0`), ngăn `homeassistant_time` ghi đè giờ sai lệch múi giờ từ VPS.

---

## Cấu trúc thư mục quan trọng

| File / Thư mục | Mô tả |
| :--- | :--- |
| `tessera_freenove_28.yaml` | File cấu hình ESPHome hoàn chỉnh cho bo mạch Freenove ESP32-S3 2.8" (FNK0104). |
| `tcp_tunnel.h` | Thư viện C++ Reverse TCP Tunnel chạy ngầm trên FreeRTOS kết nối ESP32 với VPS. |
| `docker-compose-tessera.yaml` | File Docker Compose để chạy dịch vụ quản lý màn hình Tessera (`esp-screens`) trên VPS. |
| `components/smart_display/optimistic.h` | Xử lý trạng thái Optimistic UI khi bật/tắt thiết bị (đã vá lỗi công tắc Tuya). |
| `components/smart_display/page_protocol.h` | Điều hướng trang xoay vòng vô hạn (`1 → 2 → 3 → 1`). |
| `components/smart_display/settings_screen.h` | Giao diện cài đặt trực tiếp trên màn hình cảm ứng (đã mở giới hạn Standby từ `3s`, bước nhảy `1s`). |
| `components/smart_display/screen_settings.h` | Cấu trúc lưu trữ cài đặt vào bộ nhớ Flash NVS (chấp nhận `standby_seconds >= 3`). |
| `packages/core.yaml` | Cấu hình lõi LVGL, Font chữ, Entity đồng bộ với Home Assistant (`min_value: 3`, `step: 1`). |

---

## Hướng dẫn Biên dịch & Nạp Firmware (ESPHome)

### 1. Chuẩn bị `secrets.yaml`
Tạo file `secrets.yaml` cùng thư mục với `tessera_freenove_28.yaml`:

```yaml
wifi_ssid: "Ten_WiFi_Cua_Ban"
wifi_password: "Mat_Khau_WiFi_Cua_Ban"
```

*(Hoặc điền trực tiếp thông tin Wi-Fi và IP tĩnh trong `tessera_freenove_28.yaml`).*

### 2. Biên dịch và nạp qua cáp USB (Lần đầu)
```bash
esphome compile tessera_freenove_28.yaml
esphome upload tessera_freenove_28.yaml --device COM8
```

### 3. Nạp cập nhật từ xa qua mạng Wi-Fi (OTA)
```bash
esphome compile tessera_freenove_28.yaml
esphome upload tessera_freenove_28.yaml --device 192.168.1.200
```

---

## Mẹo sử dụng trên màn hình

- **Mở nhanh menu Cài đặt trên màn hình (On-screen Settings)**: Nhấn giữ vào thanh tiêu đề phía trên cùng (khoảng giữa biểu tượng góc trái và đồng hồ góc phải) trong **1 giây**.
  - Vào **Brightness** → **Standby after** để chỉnh thời gian tắt/chờ màn hình (`3s, 4s, 5s...`).
  - Vào **Brightness** → **Dark mode** để bật/tắt giao diện nền tối.
- **Nhảy nhanh về Trang 1**: Bấm vào biểu tượng góc trên cùng bên trái hoặc vuốt từ cạnh dưới màn hình lên.
- **Mở bảng chọn trang nhanh (Dock)**: Bấm vào số trang `1 / 3` ở chính giữa cạnh dưới cùng.
