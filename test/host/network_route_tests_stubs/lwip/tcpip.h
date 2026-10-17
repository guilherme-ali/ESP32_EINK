#pragma once
#include "dns.h"
namespace RouteTest { inline bool rejectDnsSchedule = false; }
inline err_t tcpip_try_callback(void (*callback)(void *), void *arg) {
  if (RouteTest::rejectDnsSchedule) return -1;
  callback(arg); return ERR_OK;
}
