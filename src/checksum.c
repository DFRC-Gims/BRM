#include "checksum.h"

#include <pthread.h>
#include <sodium.h>
#include <xxhash.h>

static uint32_t crc32c_table[256];
static pthread_once_t crc32c_once = PTHREAD_ONCE_INIT;
static pthread_once_t sodium_once = PTHREAD_ONCE_INIT;
static int sodium_status = -1;

static void init_crc32c(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t crc = i;
    for (unsigned bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ (0x82f63b78U & (uint32_t)-(int32_t)(crc & 1));
    crc32c_table[i] = crc;
  }
}

static void init_sodium(void) { sodium_status = sodium_init(); }

size_t checksum_size(uint16_t type) {
  static const size_t sizes[] = {4, 8, 32, 32};
  return type < sizeof(sizes) / sizeof(sizes[0]) ? sizes[type] : 0;
}

int checksum_compute(uint16_t type, const void *data, size_t size,
                     uint8_t out[CHECKSUM_MAX_SIZE]) {
  if (!out || (!data && size != 0) || checksum_size(type) == 0)
    return -1;
  const uint8_t *bytes = data ? data : (const uint8_t *)"";

  switch (type) {
  case 0: {
    if (pthread_once(&crc32c_once, init_crc32c) != 0)
      return -1;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; i++)
      crc = crc32c_table[(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);
    crc = ~crc;
    for (size_t i = 0; i < 4; i++)
      out[i] = (uint8_t)(crc >> (8 * i));
    return 0;
  }
  case 1: {
    uint64_t hash = XXH64(bytes, size, 0);
    for (size_t i = 0; i < 8; i++)
      out[i] = (uint8_t)(hash >> (8 * i));
    return 0;
  }
  case 2:
  case 3:
    if (pthread_once(&sodium_once, init_sodium) != 0 || sodium_status < 0)
      return -1;
    if (type == 2)
      return crypto_hash_sha256(out, bytes, size);
    /* BLAKE2's output length is part of its parameter block. Truncating
     * BLAKE2b-512 would produce a different, incompatible digest. */
    return crypto_generichash_blake2b(out, 32, bytes, size, NULL, 0);
  default:
    return -1;
  }
}
