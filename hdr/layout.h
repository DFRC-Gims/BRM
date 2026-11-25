#ifndef LAYOUT_H
#define LAYOUT_H

#include "inode_identity.h"

#include <stdio.h>
#include <stdint.h>
#define INVALID_OBJECTID ((uint64_t)-1)
#define BTRFS_UUID_SIZE 16
extern char *output_path;
extern char *disk_path;
extern int worker_thread_count;
extern int include_other_fs_uuid;
#define CHUNK_TREE_CACHE 1
#define CHUNK_TREE_STRIPE_CACHE 2
#define CHUNK_TREE_INDEX_CACHE 3
#define TOTAL_CHUNK_TREE_CACHE 4

#define MAP_CACHE 1
#define LEAF_NODE_CACHE 2
#define INTERNAL_NODE_CACHE 3

#define FILE_TREE_NODE_CACHE 1
#define FILE_TREE_NODE_NAME_CACHE 2
#define FILE_TREE_NODE_CHILD_CACHE 3
#define FILE_TREE_NODE_EXTENT_CACHE 4
#define FILE_TREE_NODE_MODIFY_CACHE 5
#define FILE_TREE_NODE_MOVE_CACHE 6

#define FILE_TREE_NODE_NAME_CACHE_NAME "_name"
#define FILE_TREE_NODE_CHILD_CACHE_NAME "_child"
#define FILE_TREE_NODE_EXTENT_CACHE_NAME "_extent"
#define FILE_TREE_NODE_MODIFY_CACHE_NAME "_modify"
#define FILE_TREE_NODE_MOVE_CACHE_NAME "_move"

#define CACHE_DIR "/cache"
#define EXTRACT_DIR "/extract/"
#define FILE_TREE_CACHE_DIR CACHE_DIR "/file_tree_cache/"

#define CHUNK_TREE_CACHE_DIR CACHE_DIR "/chunk_tree_cache"
#define CURRENT_CHUNK_TREE_CACHE_NAME                                          \
  CHUNK_TREE_CACHE_DIR "/current_chunk_tree_cache.bin"
#define CURRENT_CHUNK_TREE_STRIPE_CACHE_NAME                                   \
  CHUNK_TREE_CACHE_DIR "/current_chunk_tree_stripe_cache.bin"
#define TOTAL_CHUNK_TREE_CACHE_NAME                                            \
  CHUNK_TREE_CACHE_DIR "/total_chunk_tree_cache.bin"
#define DEVICE_CACHE_DIR CACHE_DIR "/device_cache/"
#define CHUNK_TREE_INDEX_CACHE_NAME CHUNK_TREE_CACHE_DIR "/chunk_tree_index.bin"
#define ITEM_CACHE_DIR CACHE_DIR "/item_cache"
#define PROJECT_CACHE_FORMAT_VERSION 3U

#define LEAF_NODE_CACHE_DIR CACHE_DIR "/leaf_node_cache"
#define INTERNAL_NODE_CACHE_DIR CACHE_DIR "/internal_node_cache"
#define LEAF_NODE_CACHE_NAME LEAF_NODE_CACHE_DIR "/leaf_node_cache.bin"
#define INTERNAL_NODE_CACHE_NAME                                               \
  INTERNAL_NODE_CACHE_DIR "/internal_node_cache.bin"

#define ADDRESS_MAPPING_CACHE_DIR CACHE_DIR "/address_mapping_cache"
#define ADDRESS_MAPPING_CACHE_NAME                                             \
  ADDRESS_MAPPING_CACHE_DIR "/address_mapping_cache.bin"

#define DEV_ITEM_INDEX 0
#define CHUNK_ITEM_INDEX 1
#define INODE_ITEM_INDEX 2
#define INODE_REF_INDEX 3
#define INODE_EXTREF_INDEX 4
#define DIR_ITEM_INDEX 5
#define DIR_INDEX_INDEX 6
#define EXTENT_DATA_INDEX 7
#define ROOT_ITEM_INDEX 8
#define EXTENT_ITEM_INDEX 9
#define METADATA_ITEM_INDEX 10
#define BLOCK_GROUP_ITEM_INDEX 11
#define EXTENT_CSUM_INDEX 12
#define ITEM_CACHE_COUNT 13

int ensure_cache_dir(const char *cache_dir);
int ensure_address_mapping_leaf_internal_cache_dirs(void);
int remove_cache_files(const char *path);
void set_worker_thread_count(int count);
void set_include_other_fs_uuid(int include);
void set_cache_base_path(const char *path);
const char *current_cache_base_path(void);
void set_current_fs_generation(uint64_t generation);
uint64_t current_fs_generation_limit(void);
void set_current_fs_profile(uint32_t sectorsize, uint16_t csum_type);
uint32_t current_fs_sectorsize(void);
uint16_t current_fs_csum_type(void);
int should_parse_fs_uuid(const uint8_t fs_uuid[BTRFS_UUID_SIZE]);
void set_current_fs_uuid(const uint8_t fs_uuid[BTRFS_UUID_SIZE]);
void select_file_tree_fs_uuid(const uint8_t fs_uuid[BTRFS_UUID_SIZE]);
void select_current_file_tree_fs_uuid(void);
const uint8_t *active_file_tree_fs_uuid(void);
char *chunk_tree_cache_path(int type);
char *device_cache_path(uint64_t devid);
char *item_cache_path(int type);
char *cache_path(int type);
char *cache_dir_path(void);
char *file_tree_cache_dir_path(void);
char *file_tree_cache_path(uint64_t objectid, uint64_t root_dirid, int type);
char *file_tree_cache_path_typed(uint64_t objectid, uint64_t root_dirid,
                                 uint8_t key_type, int type);
char *file_tree_inode_cache_path(const struct inode_instance_id *id, int type);
char *file_tree_cache_dir_all_path(void);
char *extent_csum_range_index_path(void);
int cache_format_publish(void);
int cache_format_validate(void);
char *extract_path(uint8_t *name);
int ensure_extract_directory(const char *relative_path);
FILE *open_extract_file(const uint8_t *relative_path, const char *mode);

#endif
