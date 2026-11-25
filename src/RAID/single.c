#include "../../hdr/init.h"
#include "../../hdr/layout.h"
#include "../../hdr/raid.h"
#include <limits.h>
#include <stddef.h>
#include <linux/btrfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int read_tree_single(struct parse_btrfs_chunk *chunk, struct stripe *stripe) {
  FILE *dev_pnt = NULL;
  if (ensure_address_mapping_leaf_internal_cache_dirs() != 0) {
    perror("ensure_address_mapping_leaf_internal_cache_dirs");
    return 1;
  }
  char *map_path = cache_path(MAP_CACHE);
  FILE *map_pnt = map_path ? fopen(map_path, "ab") : NULL;
  free(map_path);
  if (!map_pnt) {
    perror("calloc");
    return 1;
  }

  dev_pnt = open_cached_device(stripe->devid);
  if (dev_pnt == NULL) {
    perror("fopen");
    fclose(map_pnt);
    return 1;
  }
  fseek(dev_pnt, stripe->physical_address, SEEK_SET);

  char read_buf[0x4000];
  uint64_t read_size = 0;
  uint64_t chunk_size = chunk->length;
  FILE **item_caches = open_item_caches();

  while (chunk_size > 0) {
    fread(read_buf, 0x4000, 1, dev_pnt);
    read_size += 0x4000;
    chunk_size -= 0x4000;
    parse_node(read_buf, item_caches, map_pnt);
    if (read_size >= chunk->stripe_length) {
      read_size = 0;
    }
  }

  for (int i = 0; i < ITEM_CACHE_COUNT; i++) {
    fclose(item_caches[i]);
  }
  free(item_caches);
  fclose(dev_pnt);
  fclose(map_pnt);
  return 0;
}

int extract_single(uint64_t logical_address_start, uint64_t remain_size,
                   uint64_t offset_in_file, uint64_t file_size,
                   struct extent_data_reg_prealloc reg_prealloc,
                   struct stripe *stripe_data, uint8_t *name,
                   int verify_checksum) {
  uint64_t source_logical_address = 0;
  if (!stripe_data ||
      extent_source_logical_address(&reg_prealloc, &source_logical_address) !=
          0 ||
      source_logical_address < logical_address_start)
    return -1;
  uint64_t logical_gap = source_logical_address - logical_address_start;
  remain_size = extract_read_limit_for_file(
      remain_size, source_logical_address, offset_in_file, file_size,
      verify_checksum);

  FILE *all_dev_pnt = NULL;
  uint64_t all_dev_offset = stripe_data->physical_address + logical_gap;
  all_dev_pnt = open_cached_device(stripe_data->devid);
  if (!all_dev_pnt) {
    perror("fopen");
    return -1;
  }

  uint64_t read_size = 0;
  char *read_buf = calloc(1, 0x10000);
  FILE *extract_file = open_extract_file(name, "r+b");
  if (!read_buf || !extract_file) {
    perror(read_buf ? "fopen" : "calloc");
    if (extract_file)
      fclose(extract_file);
    free(read_buf);
    fclose(all_dev_pnt);
    return -1;
  }

  uint64_t extracted_size = 0;
  int result = 0;
  while (remain_size > 0) {
    read_size = MIN(remain_size, 0x10000);
    if (read_device_extent_data(all_dev_pnt, all_dev_offset, read_buf,
                                read_size) != 0 ||
        write_verified_extent_data(
            extract_file, source_logical_address + extracted_size,
            offset_in_file + extracted_size, file_size, read_buf, read_size,
            name, verify_checksum) != 0) {
      result = -1;
      break;
    }
    remain_size -= read_size;
    all_dev_offset += read_size;
    extracted_size += read_size;
  }
  free(read_buf);
  if (fclose(extract_file) != 0)
    result = -1;
  fclose(all_dev_pnt);
  return result;
}
