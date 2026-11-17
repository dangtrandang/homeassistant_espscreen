# Tessera (ESP Screens) for Freenove ESP32-S3 2.8" IPS Capacitive Touch (FNK0104)

Bản tùy biến nâng cao của [MaxGramser/homeassistant_espscreen](https://github.com/MaxGramser/homeassistant_espscreen) dành riêng cho bo mạch **Freenove ESP32-S3 2.8" Capacitive Touch (FNK0104 - ILI9341 IPS 240x320 + FT6336)**, hỗ trợ kết nối từ xa về **Home Assistant trên Cloud VPS** xuyên qua CGNAT/Firewall và bổ sung nhiều cải tiến thực tế cho thiết bị tại Việt Nam.

---

## Tính năng & Các bản vá đã tích hợp (Custom Patches)

1. **Hỗ trợ phần cứng Freenove ESP32-S3 2.8" (FNK0104)**:
   - Vi xử lý ESP32-S3 với **16MB Flash** và **8MB Octal PSRAM (80MHz)**, nâng giới hạn lên **128 tiles / 24 pages**.
   - Cấu hình màn hình **ILI9341 IPS 240x320** qua SPI (`CLK: GPIO12`, `MOSI: GPIO11`, `CS: GPIO10`, `DC: GPIO46`, `BL: GPIO45`, chân Reset dùng chung mạch `EN` của board để giải phóng `GPIO4` cho xung nhịp `I2S_MCLK`) với `invert_colors: true` để hiển thị đúng dải màu trắng/đen của tấm nền IPS.
   - Cảm ứng điện dung **FT6336 (ft63x6)** qua I2C (`SDA: GPIO16`, `SCL: GPIO15`, `INT: GPIO17`, `RST: GPIO18`).

2. **Tích hợp Audio Codec ES8311 & Plugin Voice Assistant (`voice_assist` v0.4.0) chính thức của Tessera**:
   - Kích hoạt chip giải mã âm thanh **ES8311 (`0x18`)** trên bus I2C `touch_bus` (`SDA: GPIO16`, `SCL: GPIO15`), kết nối cả Microphone analog (`use_microphone: false`, `mic_gain: 36DB`, `DIN: GPIO6`) và Loa (`DOUT: GPIO8`) qua I2S (`MCLK: GPIO4`, `BCLK: GPIO5`, `LRCLK: GPIO7`) ở tần số `16000Hz`, cùng chân điều khiển Amply loa `AP_ENABLE` (`GPIO1`, tích cực mức `LOW` / `inverted: true`).
   - Tích hợp plugin **`voice_assist`** của Tessera hỗ trợ nhận diện **Wake Word cục bộ ngay trên chip ESP32-S3 (`micro_wake_word`)** với 3 từ khóa: *"Okay Nabu"*, *"Hey Jarvis"*, *"Hey Mycroft"*.
   - **Truyền nhận Audio TTS trực tiếp qua ESPHome API (`speaker: ts_speaker`)**: Âm thanh phản hồi từ Home Assistant được truyền dạng PCM stream qua API tunnel cổng 6053 thay vì tải file qua HTTP port 8123, hoạt động ổn định và xuyên suốt ngay cả khi port 8123 bị đóng hoặc VPN WireGuard offline.
   - **Tối ưu hóa âm lượng và độ nhạy Micro**:
     - Nâng độ nhạy micro `mic_gain: 36DB` (tăng gấp 4 lần so với mức mặc định 24dB) giúp bắt wake word chuẩn xác từ khoảng cách xa.
     - Mở rộng dải âm lượng phần cứng ES8311 (`AUDIO_VOLUME_DB_LOW: "-25"`, `AUDIO_VOLUME_DB_HIGH: "20"`), khuếch đại gain phần cứng lên đến `+20dB`.
     - Tự động thiết lập âm lượng khởi động ở mức `85%`.
     - Chuẩn hóa tần số lấy mẫu file âm thanh hệ thống `wake.wav` và `thinking.wav` về đúng chuẩn `16000Hz` đồng thời boost biên độ cực đại (~98% full-scale), triệt tiêu hoàn toàn hiện tượng lệch sample rate làm TTS nói nhanh/méo tiếng.
   - Hỗ trợ đầy đủ âm báo khi bắt đầu nghe (`wake.wav`), âm báo chờ phản hồi (`thinking.wav`), giao diện **Popup sóng âm + chạy chữ câu hỏi/trả lời**, **Tile "Assistant"** và thanh trượt chỉnh âm lượng **Media Player** / công tắc **Microphone** trong mục *Settings → Extras*.
   - **Lưu ý quan trọng khi cấu hình**: Trong khối `packages:`, `core: !include packages/core.yaml` bắt buộc phải đặt ở dòng đầu tiên trước `audio:` để không ghi đè mất tập lệnh khởi tạo `BOOT_AUDIO`.

3. **Reverse TCP Tunnel xuyên CGNAT/Firewall (`tcp_tunnel.h`)**:
   - Giải quyết triệt để vấn đề nhà mạng chặn gói tin UDP VPN (WireGuard) hoặc mạng LAN sau NAT không mở được port.
   - Mạch ESP32 chủ động mở kết nối Reverse TCP (`CTL` / `DAT`) lên Cloud VPS (`port 51821`), tự động nối cầu (bridge) cổng `127.0.0.1:6053` trên VPS về ESPHome API (`port 6053`) trên mạch.

4. **Hỗ trợ đầy đủ Font chữ Tiếng Việt có dấu**:
   - Tích hợp trọn bộ ký tự Tiếng Việt có dấu (`SCRIPT_GLYPHS`) vào tất cả các cấp độ font chữ (`headline`, `watch_value`, `label`, `sublabel`, `sublabel_big`).

5. **Sửa lỗi bật/tắt công tắc Tuya không phản hồi trạng thái (`components/smart_display/optimistic.h`)**:
   - Nhiều thiết bị công tắc Tuya trên Home Assistant nhận lệnh `switch.turn_off` / `switch.turn_on` và thực thi thành công nhưng không đẩy sự kiện `state_changed` ngược về Home Assistant, khiến bản gốc tự động revert trạng thái nút về `'on'` sau 2 giây (dẫn đến chỉ tắt được mà không bật lại được).
   - Bản vá trong `optimistic::answer()` và `optimistic::tick()` giữ nguyên trạng thái mới ngay khi Home Assistant xác nhận dịch vụ thực thi thành công (`ok == true`), loại bỏ hiện tượng treo `Updating...` hoặc tự trả về trạng thái cũ.

6. **Hạ thời gian chờ (Standby After) xuống tối thiểu `3 giây` & Chỉnh từng `1 giây`**:
   - Mở khóa giới hạn cứng `60s` của bản gốc xuống **`3s`** (`min_value: 3`, `step: 1`) trong `screen_settings.h`, `settings_screen.h`, `packages/core.yaml`, `screen_manager/app/core.py` và `web/src/model/settings.ts`.
   - Hỗ trợ tăng/giảm từng `1 giây` (`3s, 4s, 5s...`) ngay trong menu cài đặt trên màn hình hoặc từ Home Assistant.

7. **Cuộn trang xoay vòng vô hạn (`1 → 2 → 3 → 1`) (`components/smart_display/page_protocol.h`)**:
   - Cập nhật hàm `Pages::step()` xoay vòng theo modulo tổng số trang, giúp 2 nút điều hướng `<` và `>` luôn hiển thị ở mọi trang: từ trang cuối bấm `>` về thẳng trang 1 và từ trang 1 bấm `<` sang thẳng trang cuối.

8. **Đồng bộ thời gian chuẩn Việt Nam (`ICT-7`) qua Google NTP**:
   - Tách riêng đồng hồ hệ thống theo máy chủ SNTP (`216.239.35.0`), ngăn `homeassistant_time` ghi đè giờ sai lệch múi giờ từ VPS.

9. **Kiến trúc Pipeline Voice Assistant Tiếng Việt Siêu Tốc (Tổng độ trễ phản hồi kỷ lục ~0.54 giây)**:
   - **Tầng 1 - STT (Speech-to-Text)**: **GroqCloud Whisper** (`whisper-large-v3` qua HACS `ha-openai-whisper-stt-api` / *Whisper Cloud*) – nhận diện tiếng Việt chính xác 100% trên chip LPU chuyên dụng với thời gian chỉ **`0.31 giây (314ms)`**.
   - **Tầng 2 - Intent & Semantic Resolution**: Home Assistant Assist bật **Ưu tiên xử lý cục bộ (Prefer local intents)** – đối chiếu tên thiết bị và bí danh (Aliases: *Đèn sân, Đèn sông, Đèn xong...*) để thực thi bật/tắt thiết bị tức thì trong **`0.22 giây (222ms)`** mà không cần gọi cloud. Đồng thời kết nối **Google Gemini (Flash Lite)** làm bộ não dự phòng phía sau với System Prompt thông minh để xử lý các câu hội thoại phức tạp và sửa lỗi phát âm.
   - **Tầng 3 - TTS (Text-to-Speech)**: **Wyoming-Piper** chạy container Docker nội bộ trên VPS (`rhasspy/wyoming-piper` với voice `vi_VN-vais1000-medium`) – sinh âm thanh phản hồi tức thì trong **`0.002 giây (2ms)`**, loại bỏ hoàn toàn hiện tượng chập chờn, timeout hoặc bị Google chặn IP (429 Too Many Requests).
   - **Tổng thời gian từ lúc dứt câu nói đến khi đèn bật và loa phát tiếng**: Đạt mức kỷ lục **`0.54 giây`**!

---

## Cấu trúc thư mục quan trọng

| File / Thư mục | Mô tả |
| :--- | :--- |
| `tessera_freenove_28.yaml` | File cấu hình ESPHome hoàn chỉnh cho bo mạch Freenove ESP32-S3 2.8" (FNK0104) kèm Audio ES8311 & Voice Assistant. |
| `tessera-plugins/voice_assist/` | Plugin Voice Assistant chính thức của Tessera (`micro_wake_word`, giao diện Popup sóng âm, `wake.wav` / `thinking.wav`). |
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

## Cấu hình Docker Compose trên Cloud VPS (`esp-tunnel` & `wyoming-piper`)

Để kết nối màn hình Freenove xuyên qua CGNAT/Firewall và chạy bộ đọc tiếng Việt Piper siêu tốc (2ms) không phụ thuộc cloud, sử dụng file `compose.yaml` (hoặc cấu hình qua Dockge):

```yaml
services:
  esp-tunnel:
    image: python:3-alpine
    container_name: esp-tunnel
    restart: unless-stopped
    network_mode: host
    command:
      - python3
      - -u
      - -c
      - |
        import asyncio

        ctl_writer = None
        dat_queue = asyncio.Queue()

        async def pipe(r, w):
            try:
                while True:
                    d = await r.read(4096)
                    if not d: break
                    w.write(d)
                    await w.drain()
            except Exception: pass
            finally:
                try: w.close()
                except Exception: pass

        async def on_esp(r, w):
            global ctl_writer
            try:
                hdr = await asyncio.wait_for(r.readexactly(4), 10)
                if hdr == b'CTL\n':
                    print("ESP32 Control Tunnel Connected!")
                    ctl_writer = w
                    while await r.read(64): pass
                elif hdr == b'DAT\n':
                    await dat_queue.put((r, w))
            except Exception:
                try: w.close()
                except Exception: pass

        async def on_ha(hr, hw):
            global ctl_writer
            if not ctl_writer:
                hw.close(); return
            try:
                while not dat_queue.empty(): dat_queue.get_nowait()
                ctl_writer.write(b'N')
                await ctl_writer.drain()
                er, ew = await asyncio.wait_for(dat_queue.get(), 10)
                await asyncio.gather(pipe(hr, ew), pipe(er, hw))
            except Exception:
                hw.close()

        async def on_ui(ur, uw):
            try:
                tr, tw = await asyncio.open_connection('127.0.0.1', 8099)
                await asyncio.gather(pipe(ur, tw), pipe(tr, uw))
            except Exception:
                uw.close()

        async def main():
            await asyncio.start_server(on_esp, '0.0.0.0', 51821)
            await asyncio.start_server(on_ha, '127.0.0.1', 6053)
            await asyncio.start_server(on_ui, '0.0.0.0', 4500)
            print("Relay ready: ESP=51821, HA=127.0.0.1:6053, TesseraUI=4500")
            await asyncio.Future()

        asyncio.run(main())

  wyoming-piper:
    image: rhasspy/wyoming-piper:latest
    container_name: wyoming-piper
    restart: unless-stopped
    network_mode: host
    volumes:
      - ./piper-data:/data
    command:
      - --voice
      - vi_VN-vais1000-medium
```

---

## Mẹo sử dụng trên màn hình

- **Mở nhanh menu Cài đặt trên màn hình (On-screen Settings)**: Nhấn giữ vào thanh tiêu đề phía trên cùng (khoảng giữa biểu tượng góc trái và đồng hồ góc phải) trong **1 giây**.
  - Vào **Brightness** → **Standby after** để chỉnh thời gian tắt/chờ màn hình (`3s, 4s, 5s...`).
  - Vào **Brightness** → **Dark mode** để bật/tắt giao diện nền tối.
- **Nhảy nhanh về Trang 1**: Bấm vào biểu tượng góc trên cùng bên trái hoặc vuốt từ cạnh dưới màn hình lên.
- **Mở bảng chọn trang nhanh (Dock)**: Bấm vào số trang `1 / 3` ở chính giữa cạnh dưới cùng.
