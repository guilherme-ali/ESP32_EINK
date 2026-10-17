#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
using err_t = int;
constexpr err_t ERR_OK = 0, ERR_INPROGRESS = -5;
constexpr int LWIP_DNS_ADDRTYPE_IPV4 = 0;
struct ip_addr_t { uint32_t value = 0x01020304; };
inline bool IP_IS_V4(const ip_addr_t *) { return true; }
inline const ip_addr_t *ip_2_ip4(const ip_addr_t *address) { return address; }
inline uint32_t ip4_addr_get_u32(const ip_addr_t *address) { return address->value; }
using dns_found_callback = void (*)(const char *, const ip_addr_t *, void *);
namespace RouteTest {
struct PendingDns { dns_found_callback callback; void *arg; };
inline std::deque<PendingDns> pendingDns;
inline unsigned dnsCalls = 0;
inline bool dnsWait = false, dnsFail = false;
inline void completeDns() {
  ip_addr_t address;
  while (!pendingDns.empty()) {
    auto pending = pendingDns.front(); pendingDns.pop_front();
    pending.callback(nullptr, dnsFail ? nullptr : &address, pending.arg);
  }
}
}
inline err_t dns_gethostbyname_addrtype(const char *, ip_addr_t *address,
                                      dns_found_callback callback, void *arg, int) {
  ++RouteTest::dnsCalls;
  if (RouteTest::dnsWait) { RouteTest::pendingDns.push_back({callback, arg}); return ERR_INPROGRESS; }
  if (RouteTest::dnsFail) return -1;
  address->value = 0x01020304; return ERR_OK;
}
