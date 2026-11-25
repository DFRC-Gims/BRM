#include "../../hdr/init.h"
#include "../../hdr/layout.h"
#include "../../hdr/raid.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static void close_device_files(FILE **files, size_t count) {
  if (!files)
    return;
  for (size_t i = 0; i < count; i++)
    if (files[i])
      fclose(files[i]);
}

static int valid_geometry(uint16_t count, uint16_t mirrors,
                          uint64_t stripe_len) {
  if (!count || !mirrors || count % mirrors || !stripe_len) {
    errno = EINVAL;
    return 0;
  }
  return 1;
}

static FILE **open_device_files(const struct stripe *stripes, uint16_t count) {
  FILE **files = calloc(count, sizeof(*files));
  if (files)
    for (uint16_t i = 0; i < count; i++)
      files[i] = open_cached_device(stripes[i].devid);
  return files;
}

/* Adjacent sub_stripes entries are mirrors of one data stripe. Equal bytes
 * never identify mirror membership: different data stripes may be identical. */
static int read_logical_range(FILE **files, const struct stripe *stripes,
                               uint16_t count, uint16_t mirrors,
                               uint64_t stripe_len, uint64_t offset,
                               void *buffer, size_t size) {
  if (size > UINT64_MAX - offset) {
    errno = EOVERFLOW;
    return -1;
  }
  uint16_t groups = count / mirrors;
  for (size_t done = 0; done < size;) {
    uint64_t logical = offset + done;
    uint16_t group = (logical / stripe_len) % groups;
    uint64_t physical = (logical / stripe_len / groups) * stripe_len +
                        logical % stripe_len;
    size_t length = size - done;
    if (length > stripe_len - logical % stripe_len)
      length = stripe_len - logical % stripe_len;

    int read_ok = 0;
    for (uint16_t mirror = 0; mirror < mirrors; mirror++) {
      uint16_t i = group * mirrors + mirror;
      if (!files[i] || physical > UINT64_MAX - stripes[i].physical_address)
        continue;
      clearerr(files[i]);
      if (read_device_extent_data(files[i], stripes[i].physical_address + physical,
                                  (char *)buffer + done, length) == 0) {
        read_ok = 1;
        break;
      }
    }
    if (!read_ok) {
      errno = EIO;
      return -1;
    }
    done += length;
  }
  return 0;
}

int read_tree_raid10(struct parse_btrfs_chunk *chunk, struct stripe *stripes) {
  char read_buf[0x4000];
  if (!chunk || !stripes ||
      !valid_geometry(chunk->num_stripes, chunk->sub_stripes,
                       chunk->stripe_length) ||
      chunk->length % sizeof(read_buf))
    return 1;

  FILE **devices = open_device_files(stripes, chunk->num_stripes);
  FILE **items = NULL;
  FILE *map = NULL;
  int result = 1;
  if (!devices || ensure_address_mapping_leaf_internal_cache_dirs() != 0)
    goto out;
  char *map_path = cache_path(MAP_CACHE);
  map = map_path ? fopen(map_path, "ab") : NULL;
  free(map_path);
  if (!map || !(items = open_item_caches()))
    goto out;

  for (uint64_t offset = 0; offset < chunk->length; offset += sizeof(read_buf)) {
    if (read_logical_range(devices, stripes, chunk->num_stripes,
                            chunk->sub_stripes, chunk->stripe_length, offset,
                            read_buf, sizeof(read_buf)) != 0)
      goto out;
    parse_node(read_buf, items, map);
  }
  result = 0;
out:
  if (items) {
    for (int i = 0; i < ITEM_CACHE_COUNT; i++)
      if (fclose(items[i]) != 0)
        result = 1;
    free(items);
  }
  if (map && fclose(map) != 0)
    result = 1;
  close_device_files(devices, chunk->num_stripes);
  free(devices);
  return result;
}

int extract_raid10(uint64_t logical_address_start, uint64_t remain_size,
                   uint64_t offset_in_file, uint64_t file_size,
                   struct extent_data_reg_prealloc reg_prealloc,
                   struct chunk_item chunk_item, struct stripe *stripe_data,
                   uint8_t *name, int verify_checksum) {
  uint64_t source_logical_address;
  if (!stripe_data ||
      !valid_geometry(chunk_item.num_stripes, chunk_item.sub_stripes,
                       chunk_item.stripe_len) ||
      extent_source_logical_address(&reg_prealloc, &source_logical_address) != 0 ||
      source_logical_address < logical_address_start)
    return -1;
  remain_size = extract_read_limit_for_file(
      remain_size, source_logical_address, offset_in_file, file_size,
      verify_checksum);
  if (remain_size > UINT64_MAX - source_logical_address) {
    errno = EOVERFLOW;
    return -1;
  }

  FILE **devices = open_device_files(stripe_data, chunk_item.num_stripes);
  char *read_buf = malloc(65536);
  FILE *output = NULL;
  int result = -1;
  if (!devices || !read_buf)
    goto out;
  output = open_extract_file(name, "r+b");
  if (!output)
    goto out;

  for (uint64_t done = 0; done < remain_size;) {
    uint64_t logical = source_logical_address - logical_address_start + done;
    size_t length = MIN(remain_size - done, 65536);
    /* Commit each readable stripe before a later group can fail. */
    uint64_t stripe_left = chunk_item.stripe_len - logical % chunk_item.stripe_len;
    if (length > stripe_left)
      length = stripe_left;
    if (read_logical_range(devices, stripe_data, chunk_item.num_stripes,
                            chunk_item.sub_stripes, chunk_item.stripe_len,
                            logical, read_buf, length) != 0 ||
        write_verified_extent_data(output, source_logical_address + done,
                                    offset_in_file + done, file_size, read_buf,
                                    length, name, verify_checksum) != 0)
      goto out;
    done += length;
  }
  result = 0;
out:
  if (output && fclose(output) != 0)
    result = -1;
  close_device_files(devices, chunk_item.num_stripes);
  free(devices);
  free(read_buf);
  return result;
}
