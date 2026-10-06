#include "Arduino.h"
#include "Preferences.h"
#include "WiFi.h"
#include "WiFiUdp.h"
#include "esp_wifi.h"
#include <deque>
#include <vector>
#include <functional>

uint32_t gNow = 1000;
uint32_t millis() { return gNow; }
void delay(uint32_t ms) { gNow += ms; }
SerialMock Serial;
WiFiMock WiFi;
int gCur = 0;
int SerialMock::curNode() { return gCur; }
std::map<std::string, std::string> &prefStore() { static std::map<std::string, std::string> m; return m; }

struct Pkt { std::vector<uint8_t> d; IPAddress from; };
struct Node {
  uint8_t mac[6];
  uint8_t ch = 1;
  bool wifi = false;
  IPAddress ip;
  esp_now_recv_cb_t cb = nullptr;
  std::deque<Pkt> udpIn;
  std::vector<uint8_t> udpOut;
  IPAddress udpDest;
};
Node gNodes[2];
std::function<bool(int, std::vector<uint8_t> &)> gTap;  // pode alterar/derrubar pacotes ESP-NOW
std::vector<std::vector<uint8_t>> gCaptured;
int gDelivered = 0;

void WiFiMock::macAddress(uint8_t *m) { memcpy(m, gNodes[gCur].mac, 6); }
int WiFiMock::status() { return gNodes[gCur].wifi ? WL_CONNECTED : WL_DISCONNECTED; }
IPAddress WiFiMock::localIP() { return gNodes[gCur].ip; }

esp_err_t esp_now_init() { return ESP_OK; }
esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb) { gNodes[gCur].cb = cb; return ESP_OK; }
bool esp_now_is_peer_exist(const uint8_t *) { return true; }
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *) { return ESP_OK; }
esp_err_t esp_now_del_peer(const uint8_t *) { return ESP_OK; }
esp_err_t esp_now_fetch_peer(bool, esp_now_peer_info_t *) { return 1; }
esp_err_t esp_now_send(const uint8_t *dest, const uint8_t *data, size_t len) {
  int me = gCur, other = 1 - me;
  static const uint8_t bc[6] = {255, 255, 255, 255, 255, 255};
  std::vector<uint8_t> pkt(data, data + len);
  gCaptured.push_back(pkt);
  if (memcmp(dest, bc, 6) != 0 && memcmp(dest, gNodes[other].mac, 6) != 0) return ESP_OK;
  if (gNodes[me].ch != gNodes[other].ch) return ESP_OK;  // canal diferente: nao chega
  if (gTap && !gTap(me, pkt)) return ESP_OK;
  esp_now_recv_info_t info{gNodes[me].mac, (uint8_t *)dest, nullptr};
  gCur = other;
  if (gNodes[other].cb) gNodes[other].cb(&info, pkt.data(), (int)pkt.size());
  gCur = me;
  gDelivered++;
  return ESP_OK;
}
esp_err_t esp_wifi_get_channel(uint8_t *p, wifi_second_chan_t *) { *p = gNodes[gCur].ch; return ESP_OK; }
esp_err_t esp_wifi_set_channel(uint8_t p, wifi_second_chan_t) { gNodes[gCur].ch = p; return ESP_OK; }

uint8_t WiFiUDP::begin(uint16_t) { return 1; }
int WiFiUDP::beginPacket(IPAddress ip, uint16_t) { gNodes[gCur].udpDest = ip; gNodes[gCur].udpOut.clear(); return 1; }
size_t WiFiUDP::write(const uint8_t *d, size_t n) { gNodes[gCur].udpOut.insert(gNodes[gCur].udpOut.end(), d, d + n); return n; }
int WiFiUDP::endPacket() {
  int me = gCur, other = 1 - me;
  IPAddress dst = gNodes[me].udpDest;
  if (gNodes[other].wifi && (dst == gNodes[other].ip || dst == IPAddress(192, 168, 0, 255)))
    gNodes[other].udpIn.push_back({gNodes[me].udpOut, gNodes[me].ip});
  return 1;
}
static Pkt gCurPkt;
int WiFiUDP::parsePacket() {
  auto &q = gNodes[gCur].udpIn;
  if (q.empty()) return 0;
  gCurPkt = q.front(); q.pop_front();
  return (int)gCurPkt.d.size();
}
int WiFiUDP::read(uint8_t *b, size_t n) { size_t k = std::min(n, gCurPkt.d.size()); memcpy(b, gCurPkt.d.data(), k); return (int)k; }
IPAddress WiFiUDP::remoteIP() { return gCurPkt.from; }
void setNode(int i, const uint8_t mac[6], uint8_t ch, bool wifi, IPAddress ip) { memcpy(gNodes[i].mac, mac, 6); gNodes[i].ch = ch; gNodes[i].wifi = wifi; gNodes[i].ip = ip; }
uint8_t nodeCh(int i) { return gNodes[i].ch; }
void injectTo(int node, const uint8_t *from, std::vector<uint8_t> pkt) {
  esp_now_recv_info_t info{(uint8_t *)from, nullptr, nullptr};
  int me = gCur; gCur = node; gNodes[node].cb(&info, pkt.data(), (int)pkt.size()); gCur = me;
}
