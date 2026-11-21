/* Host test for the block CRC-16 and its on-air serialization.
 *
 *   cd test_host && gcc -Wall -Wextra -I stub -I ../main \
 *       test_crc16.c ../main/afsk_protocol.c -o /tmp/test_crc16 && /tmp/test_crc16
 */
#include <stdio.h>
#include <string.h>
#include "afsk_protocol.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* Reads one UART-style byte (START=0, 8 data bits LSB first, STOP=1). */
static int read_byte(const volatile bool *bits, uint32_t *pos, uint32_t end, uint8_t *out) {
    if (*pos + 10 > end || bits[*pos] != 0 || bits[*pos + 9] != 1) return 0;
    uint8_t v = 0;
    for (int b = 0; b < 8; b++) v |= (uint8_t)(bits[*pos + 1 + b] << b);
    *out = v;
    *pos += 10;
    return 1;
}

static void test_known_answer(void) {
    CHECK(afsk_crc16((const uint8_t *)"123456789", 9) == 0x29B1);
    CHECK(afsk_crc16((const uint8_t *)"", 0) == 0xFFFF);
}

static void test_single_bit_errors_detected(void) {
    uint8_t buf[AFSK_HDR_LEN + MAX_BLOCK_LEN];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 37 + 11);
    uint16_t ref = afsk_crc16(buf, sizeof buf);
    for (size_t i = 0; i < sizeof buf; i++)
        for (int b = 0; b < 8; b++) {
            buf[i] ^= (uint8_t)(1 << b);
            CHECK(afsk_crc16(buf, sizeof buf) != ref);
            buf[i] ^= (uint8_t)(1 << b);
        }
}

static void test_on_air_layout(void) {
    static volatile bool bits[4096];
    volatile uint32_t w = 0;
    uint8_t frame[AFSK_HDR_LEN + MAX_BLOCK_LEN];
    afsk_block_hdr_t hdr = { .seq = 7, .block_no = 0, .total = 1 };
    const char *text = "Hello, \xD0\x9C\xD0\xB8\xD1\x80"; /* "Hello, Мир" */
    size_t flen = afsk_pack_block(frame, sizeof frame, &hdr,
                                  (const uint8_t *)text, strlen(text));
    CHECK(flen == AFSK_HDR_LEN + strlen(text));

    CHECK(afsk_serialize_block_ring(frame, flen, bits, &w, NULL, 4096) == 0);

    /* preamble + bytes + CRC bytes + 10 trailing marks */
    CHECK(w == PREAMBLE_BITS + (flen + AFSK_CRC_LEN) * 10 + 10);

    uint32_t pos = 0;
    for (int i = 0; i < PREAMBLE_BITS; i++) CHECK(bits[pos++] == 1);

    uint8_t rx[AFSK_HDR_LEN + MAX_BLOCK_LEN + AFSK_CRC_LEN];
    size_t n = 0;
    while (n < sizeof rx && read_byte(bits, &pos, w, &rx[n])) n++;
    CHECK(n == flen + AFSK_CRC_LEN);
    CHECK(memcmp(rx, frame, flen) == 0);

    uint16_t sent = (uint16_t)(rx[flen] | (rx[flen + 1] << 8)); /* low byte first */
    CHECK(sent == afsk_crc16(frame, flen));
    for (uint32_t i = pos; i < w; i++) CHECK(bits[i] == 1);
}

static void test_full_block_fits_timing(void) {
    /* Longest block: the derived constants must match what is serialized. */
    static volatile bool bits[4096];
    volatile uint32_t w = 0;
    uint8_t frame[AFSK_HDR_LEN + MAX_BLOCK_LEN] = {0};
    CHECK(afsk_serialize_block_ring(frame, sizeof frame, bits, &w, NULL, 4096) == 0);
    CHECK(w == BLOCK_BITS);
}

int main(void) {
    test_known_answer();
    test_single_bit_errors_detected();
    test_on_air_layout();
    test_full_block_fits_timing();
    printf(failures ? "FAILED: %d\n" : "OK\n", failures);
    return failures != 0;
}
