#include "file_tree.h"
#include "layout.h"
#include "struct.h"
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ANSI_RED "\033[31m"
#define ANSI_PURPLE "\033[35m"
#define ANSI_RESET "\033[0m"
#define BTRFS_SUBVOL_ROOT_INODE 256
#define PRINT_CACHE_CAPACITY 4096
#define PRINT_CACHE_MAX_ENTRIES 2048

struct tree_child_entry {
  struct inode_instance_id id;
  uint8_t key_type;
  uint32_t generation;
  uint8_t type;
};

struct move_cache_entry {
  struct move_cache move;
  struct move_cache_entry *next;
};

struct print_cache_entry {
  struct inode_instance_id id;
  uint8_t key_type;
  uint8_t used;
  uint8_t node_loaded;
  uint8_t node_ok;
  uint8_t name_loaded;
  uint8_t moves_loaded;
  struct total_file_tree node;
  char *name;
  struct move_cache_entry *moves;
};

struct print_cache {
  struct print_cache_entry *entries;
  size_t capacity;
  size_t count;
};

static struct print_cache print_cache;
static FILE *print_inode_cache_file;
static int print_inode_cache_opened;

static uint64_t print_cache_hash(const struct inode_instance_id *id,
                                 uint8_t key_type) {
  uint64_t h = id->root_dirid ^ (id->objectid + 0x9e3779b97f4a7c15ULL);
  h ^= id->inode_generation + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  h ^= ((uint64_t)key_type + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  return h;
}

static int print_cache_key_equal(const struct print_cache_entry *entry,
                                 const struct inode_instance_id *id,
                                 uint8_t key_type) {
  return entry->used && inode_instance_id_equal(&entry->id, id) &&
         entry->key_type == key_type;
}

static int print_generation_is_allowed(uint32_t generation) {
  uint64_t limit = current_fs_generation_limit();
  return limit == 0 || generation == 0 || generation <= limit;
}

static void build_child_prefix(char *out, size_t out_size, const char *prefix,
                               int is_last) {
  const char *branch_gap = is_last ? "    " : "│   ";
  size_t prefix_len = strlen(prefix);

  if (out_size == 0)
    return;
  if (prefix_len >= out_size)
    prefix_len = out_size - 1;

  memcpy(out, prefix, prefix_len);
  out[prefix_len] = '\0';
  strncat(out, branch_gap, out_size - prefix_len - 1);
}

static void print_cache_clear_entries(void) {
  for (size_t i = 0; i < print_cache.capacity; i++) {
    struct print_cache_entry *entry = &print_cache.entries[i];
    if (!entry->used)
      continue;
    free(entry->name);
    struct move_cache_entry *move = entry->moves;
    while (move) {
      struct move_cache_entry *next = move->next;
      free(move);
      move = next;
    }
  }
  if (print_cache.entries)
    memset(print_cache.entries, 0,
           print_cache.capacity * sizeof(struct print_cache_entry));
  print_cache.count = 0;
}

static void print_cache_clear(void) {
  print_cache_clear_entries();
  free(print_cache.entries);
  memset(&print_cache, 0, sizeof(print_cache));
}

static int print_cache_init(void) {
  if (print_cache.entries)
    return 0;

  print_cache.entries =
      calloc(PRINT_CACHE_CAPACITY, sizeof(struct print_cache_entry));
  if (!print_cache.entries)
    return -1;
  print_cache.capacity = PRINT_CACHE_CAPACITY;
  return 0;
}

static struct print_cache_entry *
print_cache_get_entry(const struct inode_instance_id *id, uint8_t key_type,
                      int create) {
  if (!id)
    return NULL;
  if (print_cache_init() != 0)
    return NULL;
  if (create && print_cache.count >= PRINT_CACHE_MAX_ENTRIES)
    print_cache_clear_entries();

  uint64_t h = print_cache_hash(id, key_type);
  for (;;) {
    size_t pos = (size_t)(h & (print_cache.capacity - 1));
    struct print_cache_entry *entry = &print_cache.entries[pos];
    if (print_cache_key_equal(entry, id, key_type))
      return entry;
    if (!entry->used) {
      if (!create)
        return NULL;
      memset(entry, 0, sizeof(*entry));
      entry->used = 1;
      entry->id = *id;
      entry->key_type = key_type;
      print_cache.count++;
      return entry;
    }
    h++;
  }
}

static char *duplicate_string(const char *value) {
  size_t len = strlen(value) + 1;
  char *copy = malloc(len);
  if (copy)
    memcpy(copy, value, len);
  return copy;
}

static char *print_cache_path(const struct inode_instance_id *id,
                              uint8_t key_type, int cache_type) {
  if (!id)
    return NULL;
  if (key_type == INODE_ITEM)
    return file_tree_inode_cache_path(id, cache_type);
  return file_tree_cache_path_typed(id->objectid, id->root_dirid, key_type,
                                    cache_type);
}

static int read_total_file_tree(const struct inode_instance_id *id,
                                uint8_t key_type,
                                struct total_file_tree *node) {
  struct print_cache_entry *entry =
      print_cache_get_entry(id, key_type, 1);
  if (!entry)
    return -1;
  if (entry->node_loaded) {
    if (!entry->node_ok)
      return -1;
    *node = entry->node;
    return 0;
  }

  char *path = print_cache_path(id, key_type, FILE_TREE_NODE_CACHE);
  if (!path)
    return -1;

  FILE *file = fopen(path, "rb");
  free(path);
  if (!file) {
    entry->node_loaded = 1;
    entry->node_ok = 0;
    return -1;
  }

  if (fread(node, sizeof(*node), 1, file) != 1 ||
      node->objectid != id->objectid || node->key_type != key_type ||
      (key_type == INODE_ITEM &&
       node->inode_generation != id->inode_generation)) {
    fclose(file);
    entry->node_loaded = 1;
    entry->node_ok = 0;
    return -1;
  }
  fclose(file);
  entry->node = *node;
  entry->node_loaded = 1;
  entry->node_ok = 1;
  return 0;
}

static char *read_latest_name(const struct inode_instance_id *id,
                              uint8_t key_type) {
  struct print_cache_entry *cache_entry =
      print_cache_get_entry(id, key_type, 1);
  if (!cache_entry)
    return NULL;
  if (cache_entry->name_loaded)
    return cache_entry->name ? duplicate_string(cache_entry->name) : NULL;

  char *path = print_cache_path(id, key_type, FILE_TREE_NODE_NAME_CACHE);
  if (!path)
    return NULL;

  FILE *file = fopen(path, "rb");
  free(path);
  if (!file) {
    cache_entry->name_loaded = 1;
    return NULL;
  }

  struct name_cache entry;
  char *name = NULL;

  while (fread(&entry, NAME_CACHE_FIX_SIZE, 1, file) == 1) {
    char *next = realloc(name, entry.name_len + 1);
    if (!next) {
      free(name);
      fclose(file);
      return NULL;
    }
    name = next;
    if (fread(name, entry.name_len, 1, file) != 1) {
      free(name);
      fclose(file);
      return NULL;
    }
    name[entry.name_len] = '\0';
  }
  fclose(file);

  if (name) {
    cache_entry->name = name;
    cache_entry->name_loaded = 1;
    return duplicate_string(cache_entry->name);
  }

  name = malloc(24);
  if (name)
    snprintf(name, 24, "0x%" PRIx64, id->objectid);
  cache_entry->name = name;
  cache_entry->name_loaded = 1;
  return cache_entry->name ? duplicate_string(cache_entry->name) : NULL;
}

static int read_move_cache(const struct inode_instance_id *id,
                           uint8_t key_type, uint32_t move_gen,
                           struct move_cache *out) {
  struct print_cache_entry *cache_entry =
      print_cache_get_entry(id, key_type, 1);
  if (!cache_entry)
    return -1;
  for (struct move_cache_entry *move = cache_entry->moves; move;
       move = move->next) {
    if (move->move.move_gen == move_gen) {
      *out = move->move;
      return 0;
    }
  }
  if (cache_entry->moves_loaded)
    return -1;

  char *path = print_cache_path(id, key_type, FILE_TREE_NODE_MOVE_CACHE);
  if (!path)
    return -1;

  FILE *file = fopen(path, "rb");
  free(path);
  if (!file) {
    cache_entry->moves_loaded = 1;
    return -1;
  }

  struct move_cache entry;
  while (fread(&entry, sizeof(entry), 1, file) == 1) {
    struct move_cache_entry *cached_move = malloc(sizeof(*cached_move));
    if (!cached_move) {
      fclose(file);
      return -1;
    }
    cached_move->move = entry;
    cached_move->next = cache_entry->moves;
    cache_entry->moves = cached_move;
    if (entry.move_gen == move_gen) {
      *out = entry;
    }
  }
  fclose(file);
  cache_entry->moves_loaded = 1;
  for (struct move_cache_entry *move = cache_entry->moves; move;
       move = move->next) {
    if (move->move.move_gen == move_gen) {
      *out = move->move;
      return 0;
    }
  }
  return -1;
}

static void print_parent_ref(uint64_t root_dirid, uint64_t parent_objectid,
                             uint64_t parent_inode_generation) {
  if (parent_objectid == INVALID_OBJECTID) {
    printf("?");
    return;
  }

  const struct inode_instance_id parent_id = {
      .root_dirid = root_dirid,
      .objectid = parent_objectid,
      .inode_generation = parent_inode_generation,
  };
  char *parent_name = read_latest_name(&parent_id, INODE_ITEM);
  if (parent_name) {
    printf("%s (0x%" PRIx64 ")", parent_name, parent_objectid);
    free(parent_name);
  } else {
    printf("0x%" PRIx64, parent_objectid);
  }
}

static void close_print_inode_cache(void) {
  if (print_inode_cache_file)
    fclose(print_inode_cache_file);
  print_inode_cache_file = NULL;
  print_inode_cache_opened = 0;
}

static FILE *open_print_inode_cache(void) {
  if (print_inode_cache_opened)
    return print_inode_cache_file;

  char *path = item_cache_path(INODE_ITEM_INDEX);
  print_inode_cache_file = path ? fopen(path, "rb") : NULL;
  free(path);
  print_inode_cache_opened = 1;
  return print_inode_cache_file;
}

static int read_inode_index_for_print(uint32_t inode_item_index,
                                      struct inode_item *inode) {
  if (inode_item_index == INVALID_INDEX)
    return -1;

  FILE *file = open_print_inode_cache();
  if (!file)
    return -1;

  if (fseek(file, inode_item_index, SEEK_SET) != 0)
    return -1;
  return fread(inode, sizeof(*inode), 1, file) == 1 ? 0 : -1;
}

static int add_inode_time_index(uint32_t **indices, size_t *count, size_t *cap,
                                uint32_t index) {
  if (index == INVALID_INDEX)
    return 0;

  for (size_t i = 0; i < *count; i++) {
    if ((*indices)[i] == index)
      return 0;
  }

  if (*count >= *cap) {
    size_t next_cap = *cap ? *cap * 2 : 4;
    uint32_t *next = realloc(*indices, next_cap * sizeof(uint32_t));
    if (!next)
      return -1;
    *indices = next;
    *cap = next_cap;
  }

  (*indices)[(*count)++] = index;
  return 0;
}

static int collect_inode_time_indices(const struct total_file_tree *node,
                                      const struct inode_instance_id *id,
                                      uint32_t **indices, size_t *count) {
  size_t cap = 0;

  *indices = NULL;
  *count = 0;

  if (!node)
    return -1;

  if (node->modify_count == 0) {
    if (add_inode_time_index(indices, count, &cap, node->inode_item_index) != 0)
      return -1;
    return 0;
  }

  char *path = print_cache_path(id, node->key_type,
                                FILE_TREE_NODE_MODIFY_CACHE);
  FILE *file = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!file) {
    if (add_inode_time_index(indices, count, &cap, node->inode_item_index) != 0)
      return -1;
    return 0;
  }

  struct modify_cache modify;
  while (fread(&modify, sizeof(modify), 1, file) == 1) {
    if (add_inode_time_index(indices, count, &cap,
                             modify.prev_inode_item_index) != 0 ||
        add_inode_time_index(indices, count, &cap,
                             modify.current_inode_item_index) != 0) {
      fclose(file);
      free(*indices);
      *indices = NULL;
      *count = 0;
      return -1;
    }
  }

  fclose(file);
  if (add_inode_time_index(indices, count, &cap, node->inode_item_index) != 0) {
    free(*indices);
    *indices = NULL;
    *count = 0;
    return -1;
  }
  return 0;
}

static void format_btrfs_time(const struct parse_btrfs_timespec *time_value,
                              char *out, size_t out_size) {
  if (out_size == 0)
    return;

  time_t seconds = (time_t)time_value->sec;
  struct tm local_time;
  size_t len = 0;

  if ((uint64_t)seconds == time_value->sec &&
      localtime_r(&seconds, &local_time))
    len = strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &local_time);

  if (len == 0) {
    snprintf(out, out_size, "%" PRIu64 ".%09" PRIu32, time_value->sec,
             time_value->nsec);
    return;
  }

  snprintf(out + len, out_size - len, ".%09" PRIu32, time_value->nsec);
}

static void print_inode_time_suffix(const struct total_file_tree *node,
                                    const struct inode_instance_id *id) {
  struct inode_item inode;
  char otime[64];
  char mtime[64];
  uint32_t *indices = NULL;
  size_t count = 0;

  if (collect_inode_time_indices(node, id, &indices, &count) != 0 ||
      count == 0) {
    free(indices);
    return;
  }

  if (count == 1) {
    if (read_inode_index_for_print(indices[0], &inode) == 0) {
      format_btrfs_time(&inode.otime, otime, sizeof(otime));
      format_btrfs_time(&inode.mtime, mtime, sizeof(mtime));
      printf(" [otime: %s, mtime: %s]", otime, mtime);
    }
    free(indices);
    return;
  }

  int printed = 0;
  for (size_t i = 0; i < count; i++) {
    if (read_inode_index_for_print(indices[i], &inode) != 0)
      continue;

    format_btrfs_time(&inode.otime, otime, sizeof(otime));
    format_btrfs_time(&inode.mtime, mtime, sizeof(mtime));
    if (!printed)
      printf(" [inode_times: ");
    printf("%sindex:%" PRIu32 " otime:%s mtime:%s", printed ? "; " : "",
           indices[i], otime, mtime);
    printed = 1;
  }
  if (printed)
    printf("]");
  free(indices);
}

static int collect_children(const struct inode_instance_id *id,
                            uint8_t key_type,
                            struct tree_child_entry **children, size_t *count) {
  struct total_file_tree node;
  if (read_total_file_tree(id, key_type, &node) != 0)
    return -1;

  *children = NULL;
  *count = 0;
  if (node.child_count == 0)
    return 0;

  char *path = print_cache_path(id, key_type, FILE_TREE_NODE_CHILD_CACHE);
  if (!path)
    return -1;

  FILE *file = fopen(path, "rb");
  free(path);
  if (!file)
    return -1;

  size_t cap = 0;
  struct child_cache child;
  for (uint32_t i = 0; i < node.child_count; i++) {
    if (fread(&child, sizeof(child), 1, file) != 1)
      break;
    if (child.type != CHILD && child.type != MOVED && child.type != DELETED)
      continue;
    if (!print_generation_is_allowed(child.generation))
      continue;

    if (*count >= cap) {
      cap = cap ? cap * 2 : 8;
      struct tree_child_entry *next =
          realloc(*children, cap * sizeof(struct tree_child_entry));
      if (!next) {
        free(*children);
        *children = NULL;
        *count = 0;
        fclose(file);
        return -1;
      }
      *children = next;
    }
    (*children)[*count].id = (struct inode_instance_id){
        .root_dirid = id->root_dirid,
        .objectid = child.objectid,
        .inode_generation = child.inode_generation,
    };
    (*children)[*count].key_type = child.key_type;
    (*children)[*count].generation = child.generation;
    (*children)[*count].type = child.type;
    (*count)++;
  }
  fclose(file);
  return 0;
}

static void print_tree_branch(const char *prefix, int is_last,
                              const struct inode_instance_id *id,
                              const char *name, uint8_t child_type,
                              uint32_t create_gen, uint32_t event_gen,
                              const struct move_cache *move,
                              const struct total_file_tree *node,
                              int print_times) {
  const char *branch = is_last ? "└──" : "├──";

  if (child_type == DELETED) {
    printf("%s%s " ANSI_RED "%s (0x%" PRIx64 ", root 0x%" PRIx64
           ", inode_generation=0x%" PRIx64
           ") [created: %" PRIu32 ", deleted: %" PRIu32 "]",
           prefix, branch, name, id->objectid, id->root_dirid,
           id->inode_generation, create_gen, event_gen);
    if (print_times)
      print_inode_time_suffix(node, id);
    printf(ANSI_RESET "\n");
  } else if (child_type == MOVED) {
    printf("%s%s " ANSI_PURPLE "%s (0x%" PRIx64 ", root 0x%" PRIx64
           ", inode_generation=0x%" PRIx64
           ") [created: %" PRIu32 ", moved: %" PRIu32,
           prefix, branch, name, id->objectid, id->root_dirid,
           id->inode_generation, create_gen, event_gen);
    if (move) {
      printf(" | ");
      print_parent_ref(id->root_dirid, move->prev_parent_objectid,
                       move->prev_parent_inode_generation);
      printf(" -> ");
      print_parent_ref(id->root_dirid, move->current_parent_objectid,
                       move->current_parent_inode_generation);
    }
    printf("]");
    if (print_times)
      print_inode_time_suffix(node, id);
    printf(ANSI_RESET "\n");
  } else {
    printf("%s%s %s (0x%" PRIx64 ", root 0x%" PRIx64
           ", inode_generation=0x%" PRIx64 ") [created: %" PRIu32 "]",
           prefix, branch, name, id->objectid, id->root_dirid,
           id->inode_generation, create_gen);
    if (print_times)
      print_inode_time_suffix(node, id);
    printf("\n");
  }
}

static int collect_root_inode_instances(uint64_t root_dirid,
                                        struct inode_instance_id **ids,
                                        size_t *count);

static void print_subtree(const struct inode_instance_id *id,
                          const char *prefix, int is_last, int is_root,
                          int skip_self, uint8_t self_type, uint8_t key_type,
                          int print_times) {
  char *name = NULL;

  if (!skip_self) {
    struct total_file_tree node;
    int node_loaded = 0;
    uint32_t generation = 0;

    name = read_latest_name(id, key_type);
    if (!name) {
      name = malloc(32);
      if (!name)
        return;
      snprintf(name, 32, "0x%" PRIx64, id->objectid);
    }

    if (read_total_file_tree(id, key_type, &node) == 0) {
      if (!print_generation_is_allowed(node.create_gen)) {
        free(name);
        return;
      }
      node_loaded = 1;
      generation = node.create_gen;
    }

    if (is_root) {
      printf("%s (0x%" PRIx64 ", root 0x%" PRIx64
             ", inode_generation=0x%" PRIx64 ") [created: %" PRIu32 "]",
             name, id->objectid, id->root_dirid, id->inode_generation,
             generation);
      if (print_times)
        print_inode_time_suffix(node_loaded ? &node : NULL, id);
      printf("\n");
    } else {
      print_tree_branch(prefix, is_last, id, name, self_type,
                        generation, 0, NULL, node_loaded ? &node : NULL,
                        print_times);
    }
    free(name);
  }

  struct tree_child_entry *children = NULL;
  size_t child_count = 0;
  if (collect_children(id, key_type, &children, &child_count) != 0) {
    free(children);
    return;
  }

  char child_prefix[4096];
  if (is_root || skip_self)
    snprintf(child_prefix, sizeof(child_prefix), "%s", prefix);
  else
    build_child_prefix(child_prefix, sizeof(child_prefix), prefix, is_last);

  for (size_t i = 0; i < child_count; i++) {
    int child_is_last = i + 1 == child_count;

    if (children[i].type == MOVED) {
      struct total_file_tree node;
      struct move_cache move;
      const struct move_cache *move_ptr = NULL;
      int node_loaded = 0;
      uint32_t create_gen = 0;
      char *child_name = read_latest_name(&children[i].id, children[i].key_type);
      if (!child_name) {
        fprintf(stderr, "Failed to read name_cache: %" PRIu64 "\n",
                children[i].id.objectid);
        continue;
      }
      if (read_total_file_tree(&children[i].id, children[i].key_type, &node) ==
          0) {
        node_loaded = 1;
        create_gen = node.create_gen;
      }
      if (read_move_cache(&children[i].id, children[i].key_type,
                          children[i].generation,
                          &move) == 0)
        move_ptr = &move;
      print_tree_branch(child_prefix, child_is_last, &children[i].id,
                        child_name, MOVED, create_gen,
                        children[i].generation, move_ptr,
                        node_loaded ? &node : NULL, print_times);
      free(child_name);
      continue;
    }

    if (children[i].type == DELETED) {
      struct total_file_tree node;
      int node_loaded = 0;
      uint32_t create_gen = 0;
      char *child_name = read_latest_name(&children[i].id, children[i].key_type);
      if (!child_name) {
        fprintf(stderr, "Failed to read name_cache: %" PRIu64 "\n",
                children[i].id.objectid);
        continue;
      }
      if (read_total_file_tree(&children[i].id, children[i].key_type, &node) ==
          0) {
        node_loaded = 1;
        create_gen = node.create_gen;
      }
      print_tree_branch(child_prefix, child_is_last, &children[i].id,
                        child_name, DELETED, create_gen,
                        children[i].generation, NULL,
                        node_loaded ? &node : NULL, print_times);
      free(child_name);

      char deleted_child_prefix[4096];
      build_child_prefix(deleted_child_prefix, sizeof(deleted_child_prefix),
                         child_prefix, child_is_last);
      print_subtree(&children[i].id, deleted_child_prefix, child_is_last, 0, 1,
                    CHILD, children[i].key_type, print_times);
      continue;
    }

    if (children[i].key_type == ROOT_ITEM) {
      struct total_file_tree node;
      int node_loaded = 0;
      uint32_t create_gen = 0;
      char *child_name = read_latest_name(&children[i].id, children[i].key_type);
      if (!child_name) {
        child_name = malloc(32);
        if (!child_name)
          continue;
        snprintf(child_name, 32, "0x%" PRIx64, children[i].id.objectid);
      }
      if (read_total_file_tree(&children[i].id, children[i].key_type, &node) ==
          0) {
        node_loaded = 1;
        create_gen = node.create_gen;
      }
      print_tree_branch(child_prefix, child_is_last, &children[i].id,
                        child_name, CHILD, create_gen, 0,
                        NULL, node_loaded ? &node : NULL, print_times);
      free(child_name);

      char subvolume_prefix[4096];
      build_child_prefix(subvolume_prefix, sizeof(subvolume_prefix),
                         child_prefix, child_is_last);
      struct inode_instance_id *subvolume_roots = NULL;
      size_t subvolume_root_count = 0;
      if (collect_root_inode_instances(children[i].id.objectid,
                                       &subvolume_roots,
                                       &subvolume_root_count) == 0) {
        for (size_t j = 0; j < subvolume_root_count; j++)
          print_subtree(&subvolume_roots[j], subvolume_prefix,
                        j + 1 == subvolume_root_count, 0, 1, CHILD, INODE_ITEM,
                        print_times);
      }
      free(subvolume_roots);
      continue;
    }

    print_subtree(&children[i].id, child_prefix, child_is_last, 0, 0, CHILD,
                  children[i].key_type, print_times);
  }

  free(children);
}

static int parse_node_cache_filename(const char *filename, uint64_t *root_dirid,
                                     uint64_t *objectid,
                                     uint64_t *inode_generation,
                                     uint8_t *key_type) {
  unsigned parsed_key_type = INODE_ITEM;
  int consumed = 0;

  if (strstr(filename, FILE_TREE_NODE_NAME_CACHE_NAME) ||
      strstr(filename, FILE_TREE_NODE_CHILD_CACHE_NAME) ||
      strstr(filename, FILE_TREE_NODE_EXTENT_CACHE_NAME) ||
      strstr(filename, FILE_TREE_NODE_MODIFY_CACHE_NAME) ||
      strstr(filename, FILE_TREE_NODE_MOVE_CACHE_NAME))
    return -1;

  if (sscanf(filename, "%" SCNu64 "_%" SCNu64 "_g%" SCNu64 "%n", root_dirid,
             objectid, inode_generation, &consumed) == 3 &&
      filename[consumed] == '\0') {
    *key_type = INODE_ITEM;
    return 0;
  }

  consumed = 0;
  if (sscanf(filename, "%" SCNu64 "_%" SCNu64 "_%u%n", root_dirid, objectid,
             &parsed_key_type, &consumed) == 3 &&
      filename[consumed] == '\0' && parsed_key_type <= UINT8_MAX) {
    *inode_generation = 0;
    *key_type = (uint8_t)parsed_key_type;
    return 0;
  }

  return -1;
}

static int add_root_inode_instance(struct inode_instance_id **ids,
                                   size_t *count,
                                   const struct inode_instance_id *id) {
  for (size_t i = 0; i < *count; i++) {
    if (inode_instance_id_equal(&(*ids)[i], id))
      return 0;
  }

  struct inode_instance_id *next =
      realloc(*ids, (*count + 1) * sizeof(**ids));
  if (!next)
    return -1;

  *ids = next;
  (*ids)[(*count)++] = *id;
  return 0;
}

static int collect_root_inode_instances(uint64_t root_dirid,
                                        struct inode_instance_id **ids,
                                        size_t *count) {
  char *dir_path = file_tree_cache_dir_all_path();
  if (!dir_path)
    return -1;

  DIR *dir = opendir(dir_path);
  free(dir_path);
  if (!dir)
    return -1;

  *ids = NULL;
  *count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    uint64_t parsed_root_dirid = 0;
    uint64_t objectid = 0;
    uint64_t inode_generation = 0;
    uint8_t key_type = INODE_ITEM;
    if (parse_node_cache_filename(entry->d_name, &parsed_root_dirid, &objectid,
                                  &inode_generation, &key_type) != 0 ||
        key_type != INODE_ITEM || objectid != BTRFS_SUBVOL_ROOT_INODE ||
        (root_dirid != INVALID_OBJECTID && parsed_root_dirid != root_dirid))
      continue;

    const struct inode_instance_id id = {
        .root_dirid = parsed_root_dirid,
        .objectid = objectid,
        .inode_generation = inode_generation,
    };
    if (add_root_inode_instance(ids, count, &id) != 0) {
      closedir(dir);
      free(*ids);
      *ids = NULL;
      *count = 0;
      return -1;
    }
  }
  closedir(dir);
  return 0;
}

int print_file_tree(const uint8_t fs_uuid[16], int print_times) {
  print_cache_clear();
  close_print_inode_cache();

  if (fs_uuid)
    select_file_tree_fs_uuid(fs_uuid);
  else
    select_current_file_tree_fs_uuid();

  struct inode_instance_id *root_ids = NULL;
  size_t root_count = 0;
  if (collect_root_inode_instances(INVALID_OBJECTID, &root_ids, &root_count) !=
      0) {
    fprintf(stderr, "Failed to open file tree cache directory\n");
    close_print_inode_cache();
    print_cache_clear();
    return -1;
  }

  for (size_t i = 0; i < root_count; i++) {
    print_subtree(&root_ids[i], "", 1, 1, 0, CHILD, INODE_ITEM, print_times);
    if (i + 1 < root_count)
      printf("\n");
  }
  free(root_ids);
  close_print_inode_cache();
  print_cache_clear();
  return 0;
}
