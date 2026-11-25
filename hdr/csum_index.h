#ifndef CSUM_INDEX_H
#define CSUM_INDEX_H

#include <stddef.h>
#include <stdint.h>

int csum_range_index_build(void);
int csum_range_index_open(void);
void csum_range_index_close(void);
/* Digests are packed in Btrfs on-disk order, checksum_size(type) bytes each.
 * Match statuses: 1 = match, 0 = known mismatch, -1 = absent/unreadable.
 * Lookups are thread-safe while the opened index and FS profile are stable.
 * Build/open/close and profile changes must happen outside lookup workers. */
int csum_range_index_match(uint64_t logical_address, uint16_t csum_type,
                           const uint8_t *actual);
int csum_range_index_match_batch(uint64_t logical_address, uint16_t csum_type,
                                 const uint8_t *actual, size_t sector_count,
                                 int *statuses);

/* Compatibility helpers encode host-order CRC32C values as little endian. */
int csum_range_index_match_crc32c(uint64_t logical_address,
                                  uint32_t actual_crc32c);
int csum_range_index_match_crc32c_batch(uint64_t logical_address,
                                        const uint32_t *actual_crc32c,
                                        size_t sector_count, int *statuses);

#endif
