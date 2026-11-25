#include "compression.h"
#include <lzo/lzo1x.h>
#include <zlib.h>
#include <zstd.h>

static uint32_t get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int consumed_input(size_t used, size_t size, uint32_t sector,
                           int padded) {
  if (used == size) return 0;
  return padded && used < size && size % sector == 0 && size - used < sector
             ? 0 : -1;
}

/* Btrfs LZO: total length, then sector-sized LZO1X segments with le32
 * lengths. A segment header never straddles a sector; skip up to 3 pad bytes.
 * See fs/btrfs/lzo.c in the Linux kernel for the on-disk framing. */
static int complete_output(size_t actual, size_t expected, uint32_t sector,
                            int padded) {
  return actual == expected ||
         (padded && actual && expected % sector == 0 &&
          actual < expected && expected - actual < sector);
}

static int decompress_lzo(const uint8_t *input, size_t size, uint8_t *output,
                           size_t expected, uint32_t sector, int padded, size_t *decoded_size) {
  if (size < 8) return -1;
  size_t total = get_le32(input);
  if (total < 8 || total > size || consumed_input(total, size, sector, padded))
    return -1;
  size_t in = 4, out = 0;
  while (in < total) {
    if (total - in < 4 || out >= expected) return -1;
    size_t length = get_le32(input + in);
    in += 4;
    if (!length || length > sector + sector / 16 + 67 || length > total - in)
      return -1;
    lzo_uint decoded = expected - out < sector ? expected - out : sector;
    if (lzo1x_decompress_safe(input + in, length, output + out, &decoded, NULL) != LZO_E_OK || !decoded)
      return -1;
    in += length;
    out += decoded;
    size_t left = sector - in % sector;
    if (left < 4 && in < total) {
      if (left > total - in) return -1;
      in += left;
    }
    if (in < total && decoded != sector) return -1;
  }
  if (!complete_output(out, expected, sector, padded)) return -1;
  if (decoded_size) *decoded_size = out;
  return 0;
}

int btrfs_decompress_extent(uint8_t compression, uint32_t sectorsize,
                            const void *input, size_t input_size,
                            void *output, size_t output_size, int padded,
                            size_t *decoded_size) {
  if (decoded_size) *decoded_size = 0;
  if (!input || !output || !input_size || !output_size ||
      input_size > RECOVERY_MAX_COMPRESSED_EXTENT ||
      output_size > RECOVERY_MAX_COMPRESSED_EXTENT ||
      sectorsize < 4096 || sectorsize > 65536 ||
      (sectorsize & (sectorsize - 1)) ||
      (padded && input_size % sectorsize))
    return -1;
  if (compression == 1) {
    z_stream stream = {0};
    if (inflateInit(&stream) != Z_OK) return -1;
    stream.next_in = (Bytef *)input;
    stream.avail_in = input_size;
    stream.next_out = output;
    stream.avail_out = output_size;
    int status = inflate(&stream, Z_FINISH);
    int result = status == Z_STREAM_END &&
                 complete_output(stream.total_out, output_size, sectorsize, padded) &&
                 consumed_input(stream.total_in, input_size, sectorsize, padded) == 0 ? 0 : -1;
    if (!result && decoded_size) *decoded_size = stream.total_out;
    inflateEnd(&stream);
    return result;
  }
  if (compression == 2)
    return decompress_lzo(input, input_size, output, output_size, sectorsize, padded, decoded_size);
  if (compression == 3) {
    ZSTD_DStream *stream = ZSTD_createDStream();
    if (!stream) return -1;
    ZSTD_inBuffer in = {input, input_size, 0};
    ZSTD_outBuffer out = {output, output_size, 0};
    int result = -1;
    if (!ZSTD_isError(ZSTD_DCtx_setParameter(stream, ZSTD_d_windowLogMax, 17))) {
      size_t status = ZSTD_decompressStream(stream, &out, &in);
      if (status == 0 && complete_output(out.pos, output_size, sectorsize, padded) &&
          consumed_input(in.pos, input_size, sectorsize, padded) == 0)
        result = 0;
    }
    if (!result && decoded_size) *decoded_size = out.pos;
    ZSTD_freeDStream(stream);
    return result;
  }
  return -1;
}
