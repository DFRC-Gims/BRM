#include "../hdr/init.h"
#include "../hdr/layout.h"
#include "../hdr/raid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void parse_tree_raid(struct parse_btrfs_chunk *chunk, struct stripe *stripe) {
  switch (chunk->raid_type) {
  case SINGLE_CHUNK_TYPE:
    read_tree_single(chunk, stripe);
    break;
  case DUP_CHUNK_TYPE:
    read_tree_dup(chunk, stripe);
    break;
  case RAID0_CHUNK_TYPE:
    read_tree_raid0(chunk, stripe);
    break;
  case RAID1_CHUNK_TYPE:
    read_tree_raid1(chunk, stripe);
    break;
  case RAID10_CHUNK_TYPE:
    read_tree_raid10(chunk, stripe);
    break;
  case RAID5_CHUNK_TYPE:
    printf("RAID5_NOT_SUPPORTED\n");
    exit(1);
    break;
  case RAID6_CHUNK_TYPE:
    printf("RAID6_NOT_SUPPORTED\n");
    exit(1);
    break;
  case RAID1C3_CHUNK_TYPE:
    read_tree_raid1(chunk, stripe);
    break;
  case RAID1C4_CHUNK_TYPE:
    read_tree_raid1(chunk, stripe);
    break;
  case UNKNOWN_CHUNK_RAID_TYPE:
    printf("UNKNOWN_CHUNK_RAID_TYPE\n");
    exit(1);
    break;
  }
}

struct stripe *get_stripe(struct parse_btrfs_chunk *chunk,
                          FILE *chunk_tree_stripe_cache) {
  size_t stripe_size = sizeof(struct stripe);
  struct stripe *stripe = NULL;
  char buf[1024 * 1024];

  if (fseek(chunk_tree_stripe_cache, (long)(chunk->stripe_index * stripe_size),
            SEEK_SET) != 0) {
    perror("fseek");
    fclose(chunk_tree_stripe_cache);
    return NULL;
  }
  stripe = calloc(chunk->num_stripes, sizeof(struct stripe));
  if (!stripe) {
    perror("calloc");
    fclose(chunk_tree_stripe_cache);
    return NULL;
  }
  for (uint16_t i = 0; i < chunk->num_stripes; i++) {
    if (fread(buf, stripe_size, 1, chunk_tree_stripe_cache) != 1) {
      fprintf(stderr, "short read stripe %u\n", (unsigned)i);
      free(stripe);
      fclose(chunk_tree_stripe_cache);
      return NULL;
    }
    memcpy(&stripe[i], buf, stripe_size);
  }
  return stripe;
}

int parse_btrfs_trees(void) {

  char *cache_path = chunk_tree_cache_path(CHUNK_TREE_CACHE);
  char *stripe_path = chunk_tree_cache_path(CHUNK_TREE_STRIPE_CACHE);
  FILE *chunk_tree_cache = cache_path ? fopen(cache_path, "rb") : NULL;
  FILE *chunk_tree_stripe_cache = stripe_path ? fopen(stripe_path, "rb") : NULL;

  free(cache_path);
  free(stripe_path);

  struct parse_btrfs_chunk chunk;
  struct stripe *stripe = NULL;
  char buf[1024 * 1024];
  size_t chunk_size = sizeof(struct parse_btrfs_chunk);

  if (!chunk_tree_cache || !chunk_tree_stripe_cache) {
    perror("fopen");
    if (chunk_tree_cache)
      fclose(chunk_tree_cache);
    if (chunk_tree_stripe_cache)
      fclose(chunk_tree_stripe_cache);
    return 1;
  }
  while (fread(buf, chunk_size, 1, chunk_tree_cache) == 1) {
    memcpy(&chunk, buf, chunk_size);
    if (chunk.data_type == SYSTEM_CHUNK_TYPE ||
        chunk.data_type == METADATA_CHUNK_TYPE) {
      stripe = get_stripe(&chunk, chunk_tree_stripe_cache);
      if (!stripe) {
        perror("get_stripe");
        fclose(chunk_tree_cache);
        return 1;
      }
      parse_tree_raid(&chunk, stripe);
      free(stripe);
      stripe = NULL;
    }
  }
  fclose(chunk_tree_cache);
  fclose(chunk_tree_stripe_cache);
  return 0;
}
