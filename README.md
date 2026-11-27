# Tessera (ESP Screens) for Freenove ESP32-S3 2.8" IPS Capacitive Touch (FNK0104)

Bản tùy biến nâng cao của [MaxGramser/homeassistant_espscreen](https://github.com/MaxGramser/homeassistant_espscreen) dành riêng cho bo mạch **Freenove ESP32-S3 2.8" Capacitive Touch (FNK0104 - ILI9341 IPS 240x320 + FT6336)**, hỗ trợ kết nối từ xa về **Home Assistant trên Cloud VPS** xuyên qua CGNAT/Firewall và bổ sung nhiều cải tiến thực tế cho thiết bị tại Việt Nam.

---

## Tính năng & Các bản vá đã tích hợp (Custom Patches)

1. **Hỗ trợ phần cứng Freenove ESP32-S3 2.8" (FNK0104)**:
   - Vi xử lý ESP32-S3 với **16MB Flash** và **8MB Octal PSRAM (80MHz)**, nâng giới hạn lên **128 tiles / 24 pages**.
   - Cấu hình màn hình **ILI9341 IPS 240x320** qua SPI (`CLK: GPIO12`, `MOSI: GPIO11`, `CS: GPIO10`, `DC: GPIO46`, `BL: GPIO45`, chân Reset dùng chung mạch `EN` của board để giải phóng `GPIO4` cho xung nhịp `I2S_MCLK`) với `invert_colors: true` để hiển thị đúng dải màu trắng/đen của tấm nền IPS.
   - Cảm ứng điện dung **FT6336 (ft63x6)** qua I2C (`SDA: GPIO16`, `SCL: GPIO15`, `INT: GPIO17`, `RST: GPIO18`).

2. **Tích hợp Audio Codec ES8311 & Trợ lý giọng nói Tiếng Việt ("Trợ lý Meow")**:
   - Kích hoạt chip giải mã âm thanh **ES8311 (`0x18`)** trên bus I2C `touch_bus` (`SDA: GPIO16`, `SCL: GPIO15`, `frequency: 100kHz`), kết nối cả Microphone analog (`use_microphone: false`, `mic_gain: 30DB` mặc định — có thể đổi live `18–36 dB`, `DIN: GPIO6`) và Loa (`DOUT: GPIO8`) qua I2S (`MCLK: GPIO4`, `BCLK: GPIO5`, `LRCLK: GPIO7`) ở tần số `16000Hz`, cùng chân điều khiển Amply loa `AP_ENABLE` (`GPIO1`, tích cực mức `LOW` / `inverted: true`).
   - Tích hợp plugin **`voice_assist`** của Tessera hỗ trợ nhận diện **Wake Word cục bộ ngay trên chip ESP32-S3 (`micro_wake_word`)** với 3 từ khóa: *"Okay Nabu"*, *"Hey Jarvis"*, *"Hey Mycroft"*.
   - **Kích hoạt UI tức thì không độ trễ (Zero-Latency Local Trigger)**: Khi gọi Wake Word, hàm `on_wake()` lập tức đánh thức màn hình, phát âm chime và bật popup giao diện chỉ sau **~50–70ms**, hoàn toàn độc lập và không phải chờ phản hồi mạng từ Home Assistant.
   - **Triệt tiêu hiện tượng nháy & quét màn hình khi mở/đóng popup (Seamless Partial Refresh)**:
     - **Tối ưu Scrim trong suốt (`LV_OPA_TRANSP`)**: Lớp phủ nền `open->scrim` trên `lv_layer_top()` được đặt trong suốt (`LV_OPA_TRANSP`) thay vì phủ mờ 60% (`LV_OPA_60`). Nhờ đó, LVGL không bị ép đánh dấu bẩn (dirty) và render lại toàn bộ màn hình 320x240 (vốn làm bộ đệm 12% phải chia thành 8 dải quét liên tiếp trên bus SPI). Giờ đây, chỉ duy nhất vùng popup card (~258x178) được vẽ lại qua cơ chế Partial Update (CASET/PASET) của driver ILI9341, triệt tiêu hoàn toàn dải quét từ trên xuống dưới trong khi vẫn giữ nguyên trọn vẹn tính năng chạm ra ngoài vùng card (`popup_beside`) để đóng popup.
     - **Loại bỏ làm mới Home không cần thiết**: Gỡ bỏ lời gọi `tessera::refresh()` trong `on_wake()` / `ask()`, ngăn chặn hệ thống ép vẽ lại toàn bộ các tile Home bên dưới khi popup chỉ phủ ở lớp trên (`lv_layer_top()`).
     - **Đảm bảo tính Idempotent cho Voice Session**: Bổ sung cờ `session_holds_` và kiểm tra chuyển trạng thái `phase(LISTENING)`, ngăn chặn việc `on_wake()` cục bộ và sự kiện `on_listening` từ Home Assistant gọi kích hoạt popup 2 lần liên tiếp gây giật hình.
   - **Tối ưu độ nhạy Wake Word, chống kích hoạt nhầm & Cân chỉnh Live (Lifecycle & Live Tuning)**:
     - Bật bộ lọc `VAD` (`probability_cutoff: 35%`) kết hợp `sliding_window_size: 5` và ngưỡng mặc định `86%` (`Okay Nabu` / `Hey Jarvis`) / `85%` (`Hey Mycroft`), triệt tiêu tình trạng tự wake nhầm trong hội thoại bình thường nhưng vẫn bắt tốt từ khoảng cách ~3m.
     - Tối ưu luồng âm thanh gửi lên STT tiếng Việt: `noise_suppression_level: 2`, `auto_gain: 0dBFS`, `volume_multiplier: 1.0` (tránh khuếch đại nhiễu nền gây kích hoạt nhầm VAD), kèm `delay: 300ms` sau `on_wake()` trước khi gọi `voice_assistant.start` để tiếng `wake.wav` không lọt vào đầu phiên thu âm.
     - **Tự ngắt phiên nghe khi im lặng (`5 giây`)**: Nếu quá `5000ms` kể từ lúc wake mà Home Assistant VAD chưa phát hiện tiếng nói (`on_stt_vad_start`), `VoiceAssist` tự gọi `stop()` để hủy phiên nghe sạch sẽ, tránh gửi 15 giây im lặng lên Whisper gây ảo giác (hallucination); đồng thời đảm bảo dừng `thinking_sound_` trong `ended()` nếu pipeline kết thúc sớm.
     - **Chỉnh độ nhạy trực tiếp trong Cài đặt (không cần build/OTA lại)**: Bổ sung 2 mục chỉnh thời gian thực trong **Cài đặt → Trợ lý Meow** (hiện trên cả màn hình cảm ứng lẫn Home Assistant, tự lưu vào Flash NVS):
       - **Ngưỡng đánh thức (`75%` – `99%`, bước `1%`)**: gọi trực tiếp `set_probability_cutoff()` cho các model `micro_wake_word`, áp dụng ngay lập tức.
       - **Khuếch đại micro (`18 dB` / `24 dB` / `30 dB` / `36 dB`)**: ghi trực tiếp thanh ghi ADC gain (`0x16`) của chip `ES8311` qua I2C theo thời gian thực.
     - **Giải phóng CPU**: Khi bắt đầu phiên hội thoại, mô hình nhận diện Wake Word được tự động tạm dừng hoàn toàn (`STOPPED`) để giải phóng tài nguyên CPU/RAM cho việc thu âm microphone, truyền Wi-Fi và vẽ giao diện LVGL mượt mà.
     - **Tự động khôi phục tức thì**: Ngay khi phản hồi xong (`RESPONSE_FINISHED` / IDLE), hệ thống tự động đưa Wake Word trở lại trạng thái `DETECTING_WAKE_WORD` trong vài mili-giây mà không bị kẹt hay mất tác vụ.
   - **Việt hóa toàn diện giao diện & cài đặt ("Trợ lý Meow")**:
     - Đổi tên hiển thị chính thức thành **"Trợ lý Meow"**.
     - Việt hóa toàn bộ trạng thái (*"Đang nghe…"*, *"Đang xử lý…"*, *"Đang trả lời…"*, *"Không thể thực hiện"*, *"Chạm để hỏi"*, *"Home Assistant vắng mặt"*) và các menu cài đặt (*Từ đánh thức, Cụm từ đánh thức, Ngưỡng đánh thức, Khuếch đại micro, Âm thanh đánh thức, Âm thanh đang xử lý, Hiện popup, Hỏi ngay*).
     - Giao diện Popup với hiệu ứng **Pulsing Glow Core** cho biểu tượng đĩa xoay trợ lý khi đang nghe và xử lý câu lệnh.
   - **Truyền nhận Audio TTS trực tiếp qua ESPHome API (`speaker: ts_speaker`)**: Âm thanh phản hồi từ Home Assistant được truyền dạng PCM stream qua API tunnel cổng 6053 thay vì tải file qua HTTP port 8123, hoạt động ổn định và xuyên suốt ngay cả khi port 8123 bị đóng hoặc VPN WireGuard offline.
   - **Tối ưu hóa âm lượng, ghi nhớ trạng thái & chống giật tiếng**:
     - Đặt độ nhạy micro mặc định `mic_gain: 30DB` (cân bằng giữa thu xa 3m và tránh vỡ tiếng/clipping), có thể đổi trực tiếp `18 / 24 / 30 / 36 dB` ngay trong Cài đặt.
     - Mở rộng dải âm lượng phần cứng ES8311 (`AUDIO_VOLUME_DB_LOW: "-25"`, `AUDIO_VOLUME_DB_HIGH: "20"`), khuếch đại gain phần cứng lên đến `+20dB`.
     - **Tự động lưu và khôi phục âm lượng**: Lưu mức âm lượng vào bộ nhớ flash NVS mỗi khi người dùng thay đổi, tự khôi phục mức cũ hoặc mặc định `60%` khi khởi động lại.
     - Nâng bộ đệm phát âm thanh `buffer_duration: 500ms`, triệt tiêu hoàn toàn hiện tượng ngắt quãng tiếng khi phát TTS.
     - Chuẩn hóa tần số lấy mẫu file âm thanh hệ thống `wake.wav` và `thinking.wav` về đúng chuẩn `16000Hz` đồng thời boost biên độ cực đại (~98% full-scale), triệt tiêu hoàn toàn hiện tượng lệch sample rate làm TTS nói nhanh/méo tiếng.
   - **Lưu ý quan trọng khi cấu hình**: Trong khối `packages:`, `core: !include packages/core.yaml` bắt buộc phải đặt ở dòng đầu tiên trước `audio:` để không ghi đè mất tập lệnh khởi tạo `BOOT_AUDIO`.

3. **Reverse TCP Tunnel xuyên CGNAT/Firewall (`tcp_tunnel.h`)**:
   - Giải quyết triệt để vấn đề nhà mạng chặn gói tin UDP VPN (WireGuard) hoặc mạng LAN sau NAT không mở được port.
   - Mạch ESP32 chủ động mở kết nối Reverse TCP (`CTL` / `DAT`) lên Cloud VPS (`port 51821`), tự động nối cầu (bridge) cổng `127.0.0.1:6053` trên VPS về ESPHome API (`port 6053`) trên mạch.

4. **Hỗ trợ đầy đủ Font chữ Tiếng Việt có dấu (Bao gồm vần Ư và chữ hoa/thường)**:
   - Tích hợp trọn bộ ký tự Tiếng Việt có dấu đầy đủ (`SCRIPT_GLYPHS` bao gồm cả các ký tự `Ư, Ứ, Ừ, Ử, Ữ, Ự`, `ư, ứ, ừ, ử, ữ, ự` và các nguyên âm có dấu) vào tất cả các cấp độ font chữ (`headline`, `watch_value`, `label`, `sublabel`, `sublabel_big`), loại bỏ hoàn toàn lỗi hiển thị ô vuông hoặc mất ký tự.

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
   - **Tầng 1 - STT (Speech-to-Text)**: **GroqCloud Whisper** (`whisper-large-v3` / `whisper-large-v3-turbo` qua HACS `ha-openai-whisper-stt-api` / *Whisper Cloud*) – nhận diện tiếng Việt nhanh trên chip LPU chuyên dụng với thời gian chỉ **`~0.31 giây (314ms)`**. *Lưu ý:* Nên để trống `Prompt (Optional)` (tránh nhồi `{{ assist_entity_names }}`) để hạn chế hiện tượng Whisper đọc lặp lại tên thiết bị (hallucination) khi gặp đoạn audio ngắn hoặc nhiễu nền.
   - **Tầng 2 - Intent & Semantic Resolution**: Home Assistant Assist bật **Ưu tiên xử lý cục bộ (Prefer local intents)** – đối chiếu tên thiết bị và bí danh (Aliases: *Đèn sân, Đèn sông, Đèn xong...*) để thực thi bật/tắt thiết bị tức thì trong **`0.22 giây (222ms)`** mà không cần gọi cloud. Đồng thời kết nối **Google Gemini (Flash Lite)** làm bộ não dự phòng phía sau với System Prompt thông minh để xử lý các câu hội thoại phức tạp và sửa lỗi phát âm.
   - **Tầng 3 - TTS (Text-to-Speech)**: **Wyoming-Piper** chạy container Docker nội bộ trên VPS (`rhasspy/wyoming-piper` với voice `vi_VN-vais1000-medium`) – sinh âm thanh phản hồi tức thì trong **`0.002 giây (2ms)`**, loại bỏ hoàn toàn hiện tượng chập chờn, timeout hoặc bị Google chặn IP (429 Too Many Requests).
   - **Tổng thời gian từ lúc dứt câu nói đến khi đèn bật và loa phát tiếng**: Đạt mức kỷ lục **`0.54 – 0.58 giây`**!

10. **Tích hợp Wake-on-LAN (WoL) cục bộ qua ESPHome Native API (`wol.h`)**:
    - **Giải quyết bài toán Cloud VPS ↔ LAN nhà**: Home Assistant trên VPS không thể tự broadcast gói Layer 2 về mạng LAN nhà, nên ESP32-S3 (`192.168.1.200`) đảm nhiệm vai trò gửi **Magic Packet (102 bytes)** trực tiếp trong mạng LAN qua Wi-Fi STA (`st1`), thay thế hoàn toàn mạch ESP32-C3 Mini và không cần MQTT.
    - **Kiến trúc Stateless trên ESP32-S3**: Firmware chỉ cung cấp duy nhất ESPHome API action `esphome.freenove_28_screen_send_wol(mac)` (hỗ trợ định dạng `AA:BB:CC:DD:EE:FF`, `AA-BB-CC-DD-EE-FF`, `AABBCCDDEEFF`), không lưu MAC trong Flash. Toàn bộ danh sách MAC PC/NAS và kịch bản được quản lý linh hoạt trên Home Assistant (`input_text.pc_mac` + `script.wake_pc`) — thêm/sửa máy không cần build hay OTA lại firmware.
    - **An toàn định tuyến & Non-blocking**: Tự động lấy IP và netmask thực tế của interface `WIFI_STA_DEF` (`esp_netif_get_ip_info`), bind IP nguồn Wi-Fi STA và gửi **directed subnet broadcast** (`192.168.1.255:9`) nên hoạt động ổn định ngay cả khi WireGuard đang bật. Gửi lặp 3 gói liên tiếp bằng `MSG_DONTWAIT` + `delay: 30ms` của ESPHome scheduler (không tạo FreeRTOS task riêng, không chặn LVGL hay Audio/Wake Word).
    - **Điều khiển đa kênh**: Bật PC từ xa qua app/Dashboard Home Assistant (1 chạm), chạm trực tiếp trên màn hình cảm ứng Tessera, hoặc ra lệnh giọng nói qua **Trợ lý Meow** (*"Okay Nabu, bật máy tính"*).

11. **Báo thức cục bộ Local-First & Đồng bộ Home Assistant 2 chiều (`tessera-plugins/alarm_clock/`)**:
    - **Kiến trúc Local-First độc lập mạng/VPS**: Lõi Alarm Engine chạy trực tiếp theo đồng hồ SNTP (`ICT-7`) trên ESP32-S3, lưu tối đa **10 báo thức** vào bộ nhớ Flash NVS bằng khóa riêng `CLK1` (`0x434C4B31`, tách biệt với `ALM1` của `alarm_panel`). Khi mất Wi-Fi hoặc HA VPS ngoại tuyến, báo thức vẫn tự kêu đúng giờ, chống kích hoạt lặp trong cùng 1 phút (`last_fired_minute`) và hỗ trợ cửa sổ kêu bù `≤ 2 phút` sau khi đồng bộ giờ.
    - **Chuông WAV cục bộ + Software Fade-in không ảnh hưởng âm lượng hệ thống**: Nhúng sẵn file chuông `alarm.wav` (`16kHz`, 16-bit mono PCM) trong Flash, phát lặp mượt trên `ts_speaker` với hiệu ứng tăng dần âm lượng bằng phần mềm (25% → mức trần cấu hình trong 12 giây) nên không làm thay đổi `saved_volume` của `ts_media_player`.
    - **Chiếm & trả quyền Audio sạch sẽ (`audio_owner`)**: Khi `RINGING`, plugin tự claim `Side::BOTH` ở mức `Role::DOORBELL` (`id = 100`) để tạm dừng `micro_wake_word` và nhạc nền; ngay khi người dùng bấm **Báo lại (Snooze)** hoặc **Tắt (Dismiss)**, quyền micro được trả lại tức thì để **Trợ lý Meow** tự động khôi phục nhận diện Wake Word (`DETECTING_WAKE_WORD`).
    - **Popup báo thức chống tắt nhầm & giữ sáng màn hình**: Khi đến giờ, tự gọi `tessera::wake()` đánh thức màn hình từ standby/Sleep, giữ sáng liên tục không bị `AUTO_DIM_TIMEOUT` (`3s`) tắt qua hook trong `packages/core.yaml`, hiển thị giờ + tên báo thức + 2 nút lớn (**Báo lại** / **Tắt**). Chạm ra ngoài hoặc bấm nút `X` sẽ tự mở lại popup thay vì vô tình tắt chuông.
    - **Quản lý trực tiếp bằng cảm ứng (Cài đặt → Báo thức)**: Tích hợp `AlarmManagerCard` full-screen tối ưu cho màn 2.8" hỗ trợ xem danh sách cuộn dọc (tự refresh khi có thay đổi từ Popup/HA), bật/tắt trực tiếp, thêm/sửa giờ-phút-ngày lặp (`T2–CN` hoặc 1 lần), chọn nhanh tên mẫu, xóa xác nhận 2 chạm, kèm cài đặt thời gian Snooze (`1–30 phút`) và âm lượng chuông (`10–100%`).
    - **Đồng bộ Home Assistant 2 chiều (Per-ID LWW + ACK + Snapshot)**: Cung cấp ESPHome API actions (`alarm_upsert`, `alarm_delete`, `alarm_toggle`, `alarm_sync_request`, `alarm_snooze`, `alarm_dismiss`) kèm cơ chế Last-Write-Wins theo từng `alarm_id` + tombstone xóa. ESP32 chỉ phát event `esphome.alarm_ack` (kèm `req_id` và `saved_alarm` thực tế) sau khi ghi NVS thành công, đồng thời đẩy `esphome.alarm_snapshot` mỗi khi thay đổi hoặc khi API kết nối lại.

---

## Cấu trúc thư mục quan trọng

| File / Thư mục | Mô tả |
| :--- | :--- |
| `tessera_freenove_28.yaml` | File cấu hình ESPHome hoàn chỉnh cho bo mạch Freenove ESP32-S3 2.8" (FNK0104) kèm Audio ES8311 & Voice Assistant. |
| `tessera-plugins/voice_assist/` | Plugin Voice Assistant chính thức của Tessera (`micro_wake_word`, giao diện Popup sóng âm, `wake.wav` / `thinking.wav`). |
| `tessera-plugins/alarm_clock/` | Plugin Báo thức Local-First + HA Sync (`alarm_clock.h/.cpp`, `alarm_popup.h`, `alarm_card.h`, `alarm.wav`, NVS `CLK1`, LWW + ACK). |
| `tcp_tunnel.h` | Thư viện C++ Reverse TCP Tunnel chạy ngầm trên FreeRTOS kết nối ESP32 với VPS. |
| `wol.h` | Thư viện C++ gửi Wake-on-LAN Magic Packet (102 bytes) qua LwIP UDP directed broadcast non-blocking trên Wi-Fi STA. |
| `docker-compose-tessera.yaml` | File Docker Compose để chạy dịch vụ quản lý màn hình Tessera (`esp-screens`) trên VPS. |
| `components/smart_display/plugin_host.cpp` | Quản lý vòng đời plugin và popup (`popup_show`, `popup_hide`, tối ưu scrim trong suốt triệt tiêu quét màn hình). |
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
  - Vào **Trợ lý Meow** → **Ngưỡng đánh thức** (`75%–99%`) và **Khuếch đại micro** (`18–36 dB`) để tinh chỉnh độ nhạy wake word ngay lập tức mà không cần nạp lại firmware.
- **Nhảy nhanh về Trang 1**: Bấm vào biểu tượng góc trên cùng bên trái hoặc vuốt từ cạnh dưới màn hình lên.
- **Mở bảng chọn trang nhanh (Dock)**: Bấm vào số trang `1 / 3` ở chính giữa cạnh dưới cùng.
