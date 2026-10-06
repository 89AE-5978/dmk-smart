#include "DmkLink.h"
struct RxLayout { uint8_t type; uint8_t via; uint8_t mac[6]; IPAddress ip; uint16_t port; const uint8_t *data; uint8_t len; };
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
void CAT(SIDE_PREFIX, onFrame_impl)(const RxLayout &);
void CAT(SIDE_PREFIX, onFrame)(const DmkLink::Rx &rx) {
  RxLayout r{rx.type, (uint8_t)rx.via, {}, rx.ip, rx.port, rx.data, rx.len};
  memcpy(r.mac, rx.mac, 6);
  CAT(SIDE_PREFIX, onFrame_impl)(r);
}
