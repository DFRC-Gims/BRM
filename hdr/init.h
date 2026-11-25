#ifndef INIT_H
#define INIT_H

#include "struct.h"
#include "type.h"
#include <stdint.h>
#include <stdio.h>

int parse_btrfs_current_chunk_tree(void);
int parse_btrfs_current_chunk_tree_from_images(const char *image_dir,
                                               uint8_t fs_uuid[16]);
int read_btrfs_image_identity(const char *image_dir, uint8_t fs_uuid[16],
                              uint64_t *generation);
int parse_btrfs_trees(void);
struct dev_entry *get_dev_entry(uint64_t devid);
FILE *open_cached_device(uint64_t devid);
int check_chunk_raid_type(uint64_t type);

#endif
