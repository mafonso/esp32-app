#pragma once

#include <stdint.h>

// Captive-portal DNS for the setup hotspot: answers every A query from a hotspot
// client with the board's hotspot address, so phones open the setup page. Both
// addresses are in network byte order. The task starts on first use and stays.
void captive_dns_start(uint32_t ip, uint32_t netmask);
void captive_dns_stop(void);
