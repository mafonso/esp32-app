#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "captive_dns.h"

static const char *TAG = "dns";

#define DNS_PORT 53
#define DNS_MAX_LEN 512
#define DNS_HDR_LEN 12
#define TYPE_A 1
#define CLASS_IN 1

static volatile uint32_t s_ip, s_mask;
static volatile bool s_on;
static TaskHandle_t s_task;

// Turns the query in buf (len bytes) into the reply; returns its length, or 0 to drop it.
static int answer(uint8_t *buf, int len, uint32_t ip)
{
    if (len < DNS_HDR_LEN || (buf[2] & 0x80) || ((buf[2] >> 3) & 0x0f) != 0) {
        return 0;  // a reply, or not a standard query
    }
    if (buf[4] != 0 || buf[5] != 1) {
        return 0;  // exactly one question, as every resolver sends
    }
    int p = DNS_HDR_LEN;
    while (p < len && buf[p] != 0) {
        if (buf[p] & 0xc0) {
            return 0;  // no compression in a question
        }
        p += buf[p] + 1;
    }
    p += 1;  // the root label
    if (p + 4 > len) {
        return 0;
    }
    uint16_t qtype = buf[p] << 8 | buf[p + 1], qclass = buf[p + 2] << 8 | buf[p + 3];
    p += 4;
    bool give = qtype == TYPE_A && (qclass & 0x7fff) == CLASS_IN;

    buf[2] = 0x84 | (buf[2] & 0x01);  // response, authoritative, keep RD
    buf[3] = 0;                       // no recursion, NOERROR (empty answer for AAAA etc.)
    buf[6] = 0;
    buf[7] = give;                    // ANCOUNT
    memset(buf + 8, 0, 4);            // drop authority/additional (EDNS) records
    if (!give) {
        return p;
    }
    if (p + 16 > DNS_MAX_LEN) {
        return 0;
    }
    static const uint8_t rr[] = {
        0xc0, DNS_HDR_LEN,  // name: pointer to the question
        0, TYPE_A, 0, CLASS_IN,
        0, 0, 0, 10,        // TTL 10 s: don't let phones keep it after setup
        0, 4,
    };
    memcpy(buf + p, rr, sizeof(rr));
    memcpy(buf + p + sizeof(rr), &ip, 4);
    return p + sizeof(rr) + 4;
}

static void dns_task(void *arg)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(DNS_PORT), .sin_addr.s_addr = INADDR_ANY};
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "no socket for the captive DNS (%d)", errno);
        if (fd >= 0) {
            close(fd);
        }
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    static uint8_t buf[DNS_MAX_LEN];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        uint32_t ip = s_ip, mask = s_mask;
        // Only hotspot clients, and only while the hotspot is up: never answer the LAN.
        if (n <= 0 || !s_on || (from.sin_addr.s_addr & mask) != (ip & mask)) {
            continue;
        }
        int out = answer(buf, n, ip);
        if (out > 0) {
            sendto(fd, buf, out, 0, (struct sockaddr *)&from, flen);
        }
    }
}

void captive_dns_start(uint32_t ip, uint32_t netmask)
{
    s_ip = ip;
    s_mask = netmask;
    s_on = true;
    if (!s_task && xTaskCreate(dns_task, "captive_dns", 3072, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "no memory for the captive DNS task");
    }
}

void captive_dns_stop(void)
{
    s_on = false;
}
