#ifndef ROOT_TREE_H
#define ROOT_TREE_H

struct address_map_cache;
struct parse_btrfs_header;
struct btrfs_item_cache;
struct block_ptr;
#include <stddef.h>
#include <stdint.h>

int parse_root_tree(uint32_t **generations, size_t *generation_count);
void root_tree_generation_state_reset(void);
int parse_root_tree_node(struct address_map_cache **root_tree_nodes,
                         int same_root);
int parse_leaf_node(struct parse_btrfs_header *header,
                    struct btrfs_item_cache *item_caches, uint32_t generation,
                    uint64_t root_dirid);
int parse_internal_node(struct parse_btrfs_header *top_item,
                        struct block_ptr *block_ptr, uint32_t generation,
                        uint64_t root_dirid);
#endif
