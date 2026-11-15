#pragma once

#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <cstring>
#include <cerrno>

namespace tcp_tunnel {

static const char *const TAG = "tcp_tunnel";
static const char *const VPS_IP = "180.93.36.132";
static const uint16_t VPS_PORT = 51821;
static const uint16_t LOCAL_API_PORT = 6053;

inline int connect_to_host(const char *ip, uint16_t port, int timeout_sec) {
  int sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (sock < 0) return -1;

  struct timeval tv;
  tv.tv_sec = timeout_sec;
  tv.tv_usec = 0;
  lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  int nodelay = 1;
  lwip_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_aton(ip, &addr.sin_addr);

  if (lwip_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    lwip_close(sock);
    return -1;
  }
  return sock;
}

inline void bridge_worker_task(void *pvParameters) {
  ESP_LOGI(TAG, "Opening data tunnel to VPS %s:%d...", VPS_IP, VPS_PORT);
  int vps_sock = connect_to_host(VPS_IP, VPS_PORT, 10);
  if (vps_sock < 0) {
    ESP_LOGE(TAG, "Failed to connect data socket to VPS");
    vTaskDelete(nullptr);
    return;
  }

  // Send DAT\n header
  if (lwip_send(vps_sock, "DAT\n", 4, 0) != 4) {
    lwip_close(vps_sock);
    vTaskDelete(nullptr);
    return;
  }

  // Connect to local ESPHome API server (127.0.0.1:6053, fallback 192.168.1.200:6053)
  int api_sock = connect_to_host("127.0.0.1", LOCAL_API_PORT, 5);
  if (api_sock < 0) {
    api_sock = connect_to_host("192.168.1.200", LOCAL_API_PORT, 5);
  }
  if (api_sock < 0) {
    ESP_LOGE(TAG, "Failed to connect to local API server on port %d", LOCAL_API_PORT);
    lwip_close(vps_sock);
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "Data tunnel active (VPS <-> ESPHome API 6053)");

  uint8_t buf[1024];
  int max_fd = (vps_sock > api_sock ? vps_sock : api_sock) + 1;

  while (true) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(vps_sock, &read_fds);
    FD_SET(api_sock, &read_fds);

    struct timeval tv;
    tv.tv_sec = 60;
    tv.tv_usec = 0;

    int ret = lwip_select(max_fd, &read_fds, nullptr, nullptr, &tv);
    if (ret < 0) break;
    if (ret == 0) continue;

    if (FD_ISSET(vps_sock, &read_fds)) {
      int n = lwip_recv(vps_sock, buf, sizeof(buf), 0);
      if (n <= 0) break;
      int sent = 0;
      while (sent < n) {
        int s = lwip_send(api_sock, buf + sent, n - sent, 0);
        if (s <= 0) break;
        sent += s;
      }
      if (sent < n) break;
    }

    if (FD_ISSET(api_sock, &read_fds)) {
      int n = lwip_recv(api_sock, buf, sizeof(buf), 0);
      if (n <= 0) break;
      int sent = 0;
      while (sent < n) {
        int s = lwip_send(vps_sock, buf + sent, n - sent, 0);
        if (s <= 0) break;
        sent += s;
      }
      if (sent < n) break;
    }
  }

  ESP_LOGI(TAG, "Data tunnel closed");
  lwip_close(api_sock);
  lwip_close(vps_sock);
  vTaskDelete(nullptr);
}

inline void control_tunnel_task(void *pvParameters) {
  vTaskDelay(pdMS_TO_TICKS(5000));
  while (true) {
    int ctl_sock = connect_to_host(VPS_IP, VPS_PORT, 15);
    if (ctl_sock < 0) {
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    if (lwip_send(ctl_sock, "CTL\n", 4, 0) != 4) {
      lwip_close(ctl_sock);
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    ESP_LOGI(TAG, "Connected control tunnel to VPS %s:%d", VPS_IP, VPS_PORT);

    // 25s recv timeout; send keepalive 'P' if idle
    struct timeval tv;
    tv.tv_sec = 25;
    tv.tv_usec = 0;
    lwip_setsockopt(ctl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char cmd = 0;
    while (true) {
      int n = lwip_recv(ctl_sock, &cmd, 1, 0);
      if (n == 1) {
        if (cmd == 'N') {
          xTaskCreate(bridge_worker_task, "tcp_tun_dat", 6144, nullptr, 5, nullptr);
        }
      } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Send heartbeat ping
        if (lwip_send(ctl_sock, "P", 1, 0) != 1) break;
      } else {
        break;
      }
    }

    ESP_LOGW(TAG, "Control tunnel disconnected, reconnecting in 2s...");
    lwip_close(ctl_sock);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

inline void start_tcp_tunnel() {
  static bool started = false;
  if (started) return;
  started = true;
  xTaskCreate(control_tunnel_task, "tcp_tun_ctl", 4096, nullptr, 5, nullptr);
}

}  // namespace tcp_tunnel
