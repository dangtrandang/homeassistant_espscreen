#pragma once

#include "esphome/core/log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace wol {

static const char *const TAG = "wol";
static constexpr uint16_t WOL_PORT = 9;
static constexpr size_t MAGIC_PACKET_LEN = 102;  // 6x 0xFF + 16x 6-byte MAC

inline int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Parse MAC address in formats: "AA:BB:CC:DD:EE:FF", "AA-BB-CC-DD-EE-FF", or "AABBCCDDEEFF"
inline bool parse_mac(const std::string &mac_str, uint8_t out[6]) {
  size_t idx = 0;
  int byte_idx = 0;
  const size_t n = mac_str.size();

  // Trim leading/trailing whitespace
  size_t start = 0;
  while (start < n && (mac_str[start] == ' ' || mac_str[start] == '\t' || mac_str[start] == '\r' || mac_str[start] == '\n')) {
    start++;
  }
  size_t end = n;
  while (end > start && (mac_str[end - 1] == ' ' || mac_str[end - 1] == '\t' || mac_str[end - 1] == '\r' || mac_str[end - 1] == '\n')) {
    end--;
  }

  idx = start;
  char sep = 0;
  while (idx < end && byte_idx < 6) {
    if (idx + 1 >= end) return false;
    int hi = hex_val(mac_str[idx]);
    int lo = hex_val(mac_str[idx + 1]);
    if (hi < 0 || lo < 0) return false;
    out[byte_idx++] = static_cast<uint8_t>((hi << 4) | lo);
    idx += 2;

    if (byte_idx < 6) {
      if (idx >= end) return false;
      char c = mac_str[idx];
      if (c == ':' || c == '-') {
        if (sep == 0) {
          sep = c;
        } else if (sep != c) {
          return false;
        }
        idx++;
      } else if (sep != 0) {
        return false;
      }
    }
  }

  return byte_idx == 6 && idx == end;
}

inline bool is_valid_mac(const std::string &mac_str) {
  uint8_t mac[6];
  return parse_mac(mac_str, mac);
}

inline bool send_once(const std::string &mac_str, uint32_t attempt = 0) {
  uint8_t mac[6];
  if (!parse_mac(mac_str, mac)) {
    ESP_LOGW(TAG, "Invalid MAC address: '%s'", mac_str.c_str());
    return false;
  }

  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta == nullptr) {
    ESP_LOGE(TAG, "WIFI_STA_DEF netif not found");
    return false;
  }

  esp_netif_ip_info_t ip_info{};
  if (esp_netif_get_ip_info(sta, &ip_info) != ESP_OK || ip_info.ip.addr == 0 || ip_info.netmask.addr == 0) {
    ESP_LOGE(TAG, "Wi-Fi STA has no valid IPv4/netmask");
    return false;
  }

  // Directed subnet broadcast: (ip & netmask) | ~netmask
  uint32_t bcast_addr = (ip_info.ip.addr & ip_info.netmask.addr) | (~ip_info.netmask.addr);

  // Build 102-byte Magic Packet
  uint8_t packet[MAGIC_PACKET_LEN];
  std::memset(packet, 0xFF, 6);
  for (int i = 0; i < 16; i++) {
    std::memcpy(packet + 6 + (i * 6), mac, 6);
  }

  int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Failed to create UDP socket (errno=%d)", errno);
    return false;
  }

  int broadcast = 1;
  if (lwip_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
    ESP_LOGE(TAG, "Failed to enable SO_BROADCAST (errno=%d)", errno);
    lwip_close(sock);
    return false;
  }

  // Optional interface binding if supported by LwIP config
  char ifname[8] = "";
  bool bound_if = false;
  if (esp_netif_get_netif_impl_name(sta, ifname) == ESP_OK && ifname[0] != '\0') {
#if defined(SO_BINDTODEVICE) && defined(LWIP_SO_BINDTODEVICE) && LWIP_SO_BINDTODEVICE
    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    if (lwip_setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) == 0) {
      bound_if = true;
    } else if (attempt == 0) {
      ESP_LOGD(TAG, "SO_BINDTODEVICE(%s) not applied (errno=%d), relying on IP bind + directed broadcast", ifname, errno);
    }
#endif
  }

  // Bind source address to Wi-Fi STA IP
  struct sockaddr_in src_addr{};
  src_addr.sin_family = AF_INET;
  src_addr.sin_port = htons(0);
  src_addr.sin_addr.s_addr = ip_info.ip.addr;
  if (lwip_bind(sock, reinterpret_cast<struct sockaddr *>(&src_addr), sizeof(src_addr)) < 0) {
    ESP_LOGW(TAG, "Failed to bind UDP socket to Wi-Fi STA IP (errno=%d)", errno);
  }

  struct sockaddr_in dst_addr{};
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(WOL_PORT);
  dst_addr.sin_addr.s_addr = bcast_addr;

  int sent = lwip_sendto(sock, packet, sizeof(packet), MSG_DONTWAIT,
                         reinterpret_cast<struct sockaddr *>(&dst_addr), sizeof(dst_addr));
  int send_errno = errno;
  lwip_close(sock);

  if (sent != static_cast<int>(sizeof(packet))) {
    ESP_LOGE(TAG, "WoL send #%u failed (sent=%d, errno=%d)", (unsigned)(attempt + 1), sent, send_errno);
    return false;
  }

  if (attempt == 0) {
    char src_str[INET_ADDRSTRLEN] = "";
    char bcast_str[INET_ADDRSTRLEN] = "";
    inet_ntoa_r(src_addr.sin_addr, src_str, sizeof(src_str));
    inet_ntoa_r(dst_addr.sin_addr, bcast_str, sizeof(bcast_str));
    ESP_LOGI(TAG, "WoL Magic Packet sent to %02X:%02X:%02X:%02X:%02X:%02X via %s (%s -> %s:%u, ifbind=%s)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             ifname[0] ? ifname : "WIFI_STA_DEF", src_str, bcast_str, WOL_PORT,
             bound_if ? "yes" : "no");
  } else {
    ESP_LOGD(TAG, "WoL burst #%u sent (%d bytes)", (unsigned)(attempt + 1), sent);
  }

  return true;
}

}  // namespace wol
