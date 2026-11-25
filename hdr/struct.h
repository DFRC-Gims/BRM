#ifndef STRUCT_H
#define STRUCT_H

#include <stdint.h>

#define DEV_ITEM 0xD8
#define CHUNK_ITEM 0xE4
#define INODE_ITEM 0x01
#define INODE_REF 0x0C
#define INODE_EXTREF 0x0D
#define DIR_ITEM 0x54
#define DIR_INDEX 0x60
#define EXTENT_DATA 0x6C
#define EXTENT_CSUM 0x80
#define ROOT_ITEM 0x84
#define EXTENT_ITEM 0xA8
#define METADATA_ITEM 0xA9
#define BLOCK_GROUP_ITEM 0xC0

struct dev_entry {
  uint64_t devid;
  char *path;
} __attribute__((packed));

struct stripe {
  uint64_t devid;
  uint64_t physical_address;
} __attribute__((packed));

struct address_map_cache {
  uint64_t logical_address;
  uint8_t cache_type;
  uint32_t generation;
  uint32_t owner;
  uint32_t index;
} __attribute__((packed));

struct parse_btrfs_chunk {
  uint64_t logical_address;
  uint64_t length;
  uint32_t data_type;
  uint32_t raid_type;
  uint16_t num_stripes;
  uint16_t sub_stripes;
  uint64_t stripe_length;
  uint32_t stripe_index;
} __attribute__((packed));

struct parse_btrfs_header {
  uint8_t csum[0x20];
  uint8_t fs_uuid[0x10];
  uint64_t logical_address_of_this_node;
  uint8_t flags[0x7];
  uint8_t backref;
  uint8_t chunk_tree_uuid[0x10];
  uint64_t generation;
  uint64_t owner;
  uint32_t num_items;
  uint8_t level;
} __attribute__((packed));

struct btrfs_item_cache {
  uint64_t objectid;
  uint8_t type;
  uint64_t offset;
  uint32_t index;
  uint32_t size;
} __attribute__((packed));

struct btrfs_leaf_node_cache {
  uint8_t csum[0x20];
  uint8_t fs_uuid[0x10];
  uint64_t logical_address_of_this_node;
  uint8_t flags[0x7];
  uint8_t backref;
  uint8_t chunk_tree_uuid[0x10];
  uint64_t generation;
  uint64_t owner;
  uint32_t num_items;
  uint8_t level;
  struct btrfs_item_cache **item_caches;
} __attribute__((packed));

struct btrfs_internal_node_cache {
  uint8_t csum[0x20];
  uint8_t fs_uuid[0x10];
  uint64_t logical_address_of_this_node;
  uint8_t flags[0x7];
  uint8_t backref;
  uint8_t chunk_tree_uuid[0x10];
  uint64_t generation;
  uint64_t owner;
  uint32_t num_items;
  uint8_t level;
  struct block_ptr **ptr_caches;
} __attribute__((packed));

struct parse_btrfs_key {
  uint64_t objectid;
  uint8_t type;
  uint64_t offset;
} __attribute__((packed));

struct block_ptr {
  uint64_t objectid;
  uint8_t type;
  uint64_t offset;
  uint64_t block_offset;
  uint64_t generation;
} __attribute__((packed));

struct dev_item {
  uint64_t devid;
  uint64_t total_bytes;
  uint64_t bytes_used;
  uint32_t io_align;
  uint32_t io_width;
  uint32_t sector_size;
  uint64_t type;
  uint64_t generation;
  uint64_t start_offset;
  uint32_t dev_group;
  uint8_t seek_speed;
  uint8_t bandwidth;
  uint8_t uuid[0x10];
  uint8_t fsid[0x10];
} __attribute__((packed));

struct parse_btrfs_item {
  uint32_t offset;
  uint32_t size;
} __attribute__((packed));

struct chunk_item {
  uint64_t length;
  uint64_t owner;
  uint64_t stripe_len;
  uint64_t type;
  uint32_t io_align;
  uint32_t io_width;
  uint32_t sector_size;
  uint16_t num_stripes;
  uint16_t sub_stripes;
} __attribute__((packed));

struct parse_btrfs_timespec {
  uint64_t sec;
  uint32_t nsec;
} __attribute__((__packed__));

struct inode_item {
  uint64_t generation;
  uint64_t transid;
  uint64_t size;
  uint64_t nbytes;
  uint64_t block_group;
  uint32_t nlink;
  uint32_t uid;
  uint32_t gid;
  uint32_t mode;
  uint64_t rdev;
  uint64_t flags;
  uint64_t sequence;
  uint64_t reserved[4];
  struct parse_btrfs_timespec atime;
  struct parse_btrfs_timespec ctime;
  struct parse_btrfs_timespec mtime;
  struct parse_btrfs_timespec otime;
} __attribute__((packed));

#define INODE_REF_INDEX_FIX_SIZE 0xA
struct inode_ref {
  uint64_t index;
  uint16_t name_len;
  uint8_t *name;
} __attribute__((packed));

struct inode_extref {
  uint64_t directory_objectid;
  uint64_t index;
  uint16_t name_len;
  uint8_t *name;
} __attribute__((packed));

#define DIR_ITEM_INDEX_FIX_SIZE 0x1E
struct dir_item_index {
  uint64_t objectid;
  uint8_t item_type;
  uint64_t offset;
  uint64_t transid;
  uint16_t data_len;
  uint16_t name_len;
  uint8_t type;
  uint8_t *name;
} __attribute__((packed));

struct extent_data_item {
  uint64_t generation;
  uint64_t decoded_size;
  uint8_t compression;
  uint8_t encryption;
  uint16_t other_encoding;
  uint8_t extent_type;
} __attribute__((packed));

#define EXTENT_INLINE 0x00
#define EXTENT_REG 0x01
#define EXTENT_PREALLOC 0x02

struct extent_data_reg_prealloc {
  uint64_t logical_address;
  uint64_t size_extent;
  uint64_t offset;
  uint64_t num_bytes;
} __attribute__((packed));

struct root_item {
  struct inode_item inode;
  uint64_t generation;
  uint64_t root_dirid;
  uint64_t bytenr;
  uint64_t byte_limit;
  uint64_t bytes_used;
  uint64_t last_snapshot;
  uint64_t flags;
  uint32_t refs;
  struct parse_btrfs_key drop_progress;
  uint8_t drop_level;
  uint8_t level;
  uint64_t generation_v2;
  uint8_t uuid[0x10];
  uint8_t parent_uuid[0x10];
  uint8_t received_uuid[0x10];
  uint64_t ctransid;
  uint64_t otransid;
  uint64_t stransid;
  uint64_t rtransid;
  struct parse_btrfs_timespec ctime;
  struct parse_btrfs_timespec otime;
  struct parse_btrfs_timespec stime;
  struct parse_btrfs_timespec rtime;
  uint64_t reserved[8];
} __attribute__((packed));

struct extent_item {
  uint64_t refs;
  uint64_t generation;
  uint64_t flags;
  void **data;
} __attribute__((packed));

struct tree_block_info {
  struct parse_btrfs_key key;
  uint8_t level;
} __attribute__((packed));

struct extent_inline_ref {
  uint8_t type; // 0xB0, 0xB6
  uint64_t offset;
} __attribute__((packed));

struct extent_data_ref {
  uint8_t type; // 0xB2
  uint64_t root_objectid;
  uint64_t objectid;
  uint64_t byte_offset;
  uint32_t refs_count;
} __attribute__((packed));

struct shared_data_ref {
  uint8_t type; // 0xB8
  uint64_t offset;
  uint32_t count;
} __attribute__((packed));

struct metadata_item {
  uint64_t refs;
  uint64_t generation;
  uint64_t flags;
  void **data;
} __attribute__((packed));

struct block_group_item {
  uint64_t used_bytes;
  uint64_t chunk_objectid;
  uint64_t flags;
} __attribute__((packed));

struct csum_item_cache {
  uint64_t logical_address;
  uint32_t size;
} __attribute__((packed));

#define INVALID_OBJECTID ((uint64_t)-1)
#define INVALID_INDEX ((uint32_t)-1)
struct file_tree_node {
  uint64_t objectid;
  uint32_t inode_item_index;
  uint64_t parent_objectid;
  uint16_t name_len;
  uint32_t child_count;
  uint32_t extent_item_count;
} __attribute__((packed));

struct file_tree_map {
  uint64_t objectid;
  uint64_t index;
} __attribute__((packed));

struct total_file_tree {
  uint64_t objectid;
  uint64_t inode_generation;
  uint8_t key_type;
  uint32_t inode_item_index;
  uint64_t parent_objectid;
  uint64_t parent_inode_generation;
  uint32_t name_count;
  uint32_t child_count;
  uint32_t extent_item_count;
  uint32_t create_gen;
  uint32_t recent_gen;
  uint32_t modify_count;
  uint32_t move_count;
} __attribute__((packed));

struct modify_cache {
  uint32_t modify_gen;
  uint32_t prev_inode_item_index;
  uint32_t current_inode_item_index;
} __attribute__((packed));

struct move_cache {
  uint32_t move_gen;
  uint64_t prev_parent_objectid;
  uint64_t prev_parent_inode_generation;
  uint64_t current_parent_objectid;
  uint64_t current_parent_inode_generation;
} __attribute__((packed));

#define CHILD 0
#define MOVED 1
#define DELETED 2
struct child_cache {
  uint64_t objectid;
  uint64_t inode_generation;
  uint8_t key_type;
  uint32_t generation;
  uint8_t type;
} __attribute__((packed));

struct extent_cache {
  uint32_t extent_item_index;
  uint32_t extent_item_size;
  uint64_t logical_address;
  uint64_t offset_in_file;
  uint32_t generation;
  uint8_t source;
} __attribute__((packed));

#define EXTENT_SOURCE_COMPLETE 0
#define EXTENT_SOURCE_SALVAGED 1

#define PROJECT_BTRFS_CSUM_TREE_OBJECTID 7ULL
#define PROJECT_BTRFS_EXTENT_CSUM_OBJECTID ((uint64_t)-10)

#define NAME_CACHE_FIX_SIZE 0x6
struct name_cache {
  uint32_t generation;
  uint16_t name_len;
  uint8_t *name;
} __attribute__((packed));

#endif
