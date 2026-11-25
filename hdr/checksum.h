#ifndef BTRFS_RECOVERY_CHECKSUM_H
#define BTRFS_RECOVERY_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

#define CHECKSUM_MAX_SIZE 32U

/* Btrfs on-disk checksum type: CRC32C, xxHash64, SHA256, BLAKE2b-256. */
size_t checksum_size(uint16_t type);

/* Writes checksum_size(type) bytes in Btrfs on-disk order. Thread-safe.
 * Returns -1 for unsupported types or invalid arguments. */
int checksum_compute(uint16_t type, const void *data, size_t size,
                     uint8_t out[CHECKSUM_MAX_SIZE]);

#endif
