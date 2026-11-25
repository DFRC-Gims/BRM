#ifndef BTRFS_RECOVERY_COMPRESSION_H
#define BTRFS_RECOVERY_COMPRESSION_H
#include <stddef.h>
#include <stdint.h>

/* Btrfs compressed extents are bounded to 128 KiB on disk and in memory. */
#define RECOVERY_MAX_COMPRESSED_EXTENT (128U * 1024U)

/* Decode at most output_size bytes, returning the actual size via decoded_size.
 * For regular (padded) extents only, the final decoded sector may be partial;
 * the caller must reject requested file bytes beyond decoded_size.
 * Padded input must consume every disk
 * sector, allowing only the final sector's trailing bytes after the stream.
 * No output may be used on failure. Codec IDs are Btrfs: zlib=1, LZO=2, Zstd=3. */
int btrfs_decompress_extent(uint8_t compression, uint32_t sectorsize,
                            const void *input, size_t input_size,
                            void *output, size_t output_size, int padded,
                            size_t *decoded_size);
#endif
