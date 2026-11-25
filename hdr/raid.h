#ifndef RAID_H
#define RAID_H

#include "inode_identity.h"
#include "struct.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct parse_btrfs_chunk;
struct stripe;

#define MIN(a, b) ((a) < (b) ? (a) : (b))
int read_tree_single(struct parse_btrfs_chunk *chunk, struct stripe *stripe);
int read_tree_dup(struct parse_btrfs_chunk *chunk, struct stripe *stripe);
int read_tree_raid10(struct parse_btrfs_chunk *chunk, struct stripe *stripe);
int read_tree_raid0(struct parse_btrfs_chunk *chunk, struct stripe *stripe);
int read_tree_raid1(struct parse_btrfs_chunk *chunk, struct stripe *stripe);

int check_node(char *buf);
void parse_node(char *buf, FILE **item_caches, FILE *map_pnt);
FILE **open_item_caches(void);

struct extraction_options {
  /* Write readable extents in cache order without data checksums or protection. */
  int no_data_checksum;
  /* Require raw regular inode mode and never follow cached child entries. */
  int regular_files_only;
};

enum extent_write_policy {
  EXTRACT_WRITE_TENTATIVE = 0,
  EXTRACT_WRITE_VERIFY = 1,
  /* Skip hashing and verified-region bookkeeping, including overlap checks. */
  EXTRACT_WRITE_DIRECT = 2,
};

/* NULL options preserve the behavior of the corresponding legacy entry point. */
int extract_one_with_options(const struct inode_instance_id *id,
                             const struct extraction_options *options);
int extract_all_with_options(const struct inode_instance_id *ids, size_t count,
                             size_t *failure_count,
                             const struct extraction_options *options);
int extract_one(const struct inode_instance_id *id);
int extract_one_without_moved(const struct inode_instance_id *id);
int extract_all_without_moved(const struct inode_instance_id *ids,
                              size_t count, size_t *failure_count);
void extract_raid(uint64_t logical_address_start, uint64_t decoded_size,
                  uint64_t offset_in_file, uint64_t file_size,
                  struct extent_data_reg_prealloc reg_prealloc,
                  struct chunk_item chunk_item, struct stripe *stripe_data,
                  uint8_t *name);

int extract_raid10(uint64_t logical_address_start, uint64_t decoded_size,
                   uint64_t offset_in_file, uint64_t file_size,
                   struct extent_data_reg_prealloc reg_prealloc,
                   struct chunk_item chunk_item, struct stripe *stripe_data,
                   uint8_t *name, int verify_checksum);
int extract_single(uint64_t logical_address_start, uint64_t remain_size,
                   uint64_t offset_in_file, uint64_t file_size,
                   struct extent_data_reg_prealloc reg_prealloc,
                   struct stripe *stripe_data, uint8_t *name,
                   int verify_checksum);
int extract_dup(uint64_t logical_address_start, uint64_t decoded_size,
                uint64_t offset_in_file, uint64_t file_size,
                struct extent_data_reg_prealloc reg_prealloc,
                struct chunk_item chunk_item, struct stripe *stripe_data,
                uint8_t *name, int verify_checksum);
int extract_raid0(uint64_t logical_address_start, uint64_t decoded_size,
                  uint64_t offset_in_file, uint64_t file_size,
                  struct extent_data_reg_prealloc reg_prealloc,
                  struct chunk_item chunk_item, struct stripe *stripe_data,
                  uint8_t *name, int verify_checksum);
int extract_raid1(uint64_t logical_address_start, uint64_t decoded_size,
                  uint64_t offset_in_file, uint64_t file_size,
                  struct extent_data_reg_prealloc reg_prealloc,
                  struct chunk_item chunk_item, struct stripe *stripe_data,
                  uint8_t *name, int verify_checksum);
int read_device_extent_data(FILE *file, uint64_t offset, void *data,
                            uint64_t size);
int extent_sector_csum_status(uint64_t logical_address,
                              const uint8_t *data, uint64_t size);
int write_verified_extent_data(FILE *extract_file, uint64_t logical_address,
                               uint64_t offset_in_file, uint64_t file_size,
                               const void *data, uint64_t size, uint8_t *name,
                               int verify_checksum);
int extent_source_logical_address(
    const struct extent_data_reg_prealloc *reg_prealloc,
    uint64_t *logical_address);
uint64_t extract_read_limit_for_file(uint64_t requested_size,
                                     uint64_t logical_address,
                                     uint64_t offset_in_file,
                                     uint64_t file_size,
                                     int verify_checksum);
#endif
