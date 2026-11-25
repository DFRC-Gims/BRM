#include "root_tree.h"
#include "layout.h"
#include "raid.h"
#include "struct.h"
#include <dirent.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  uint8_t fs_uuid[BTRFS_UUID_SIZE];
  uint64_t root_dirid;
  uint64_t objectid;
  uint64_t inode_generation;
  uint8_t key_type;
} seen_entry;

static seen_entry *g_seen_entries = NULL;
static size_t g_seen_count = 0;
static size_t g_seen_cap = 0;

struct inode_generation_binding {
  uint8_t fs_uuid[BTRFS_UUID_SIZE];
  uint64_t root_dirid;
  uint64_t objectid;
  uint64_t inode_generation;
};

static struct inode_generation_binding *g_inode_generation_bindings = NULL;
static size_t g_inode_generation_binding_count = 0;
static size_t g_inode_generation_binding_cap = 0;

struct inode_generation_catalog_entry {
  uint8_t fs_uuid[BTRFS_UUID_SIZE];
  struct inode_instance_id id;
};

static struct inode_generation_catalog_entry *g_inode_generation_catalog =
    NULL;
static size_t g_inode_generation_catalog_count = 0;
static size_t g_inode_generation_catalog_cap = 0;

enum pending_relation_kind {
  PENDING_DIR_ITEM,
  PENDING_DIR_INDEX,
  PENDING_INODE_REF,
};

struct pending_inode_relation {
  uint8_t fs_uuid[BTRFS_UUID_SIZE];
  enum pending_relation_kind kind;
  struct btrfs_item_cache item_cache;
  uint32_t observation_generation;
  uint64_t root_dirid;
  uint64_t wait_objectid;
  struct pending_inode_relation *next;
};

static struct pending_inode_relation **g_pending_inode_relation_buckets = NULL;
static struct pending_inode_relation **g_pending_inode_relation_bucket_tails =
    NULL;
static size_t g_pending_inode_relation_bucket_count = 0;
static size_t g_pending_inode_relation_count = 0;

static uint8_t (*g_generation_fs_uuids)[BTRFS_UUID_SIZE] = NULL;
static size_t g_generation_fs_uuid_count = 0;
static size_t g_generation_fs_uuid_cap = 0;

static int parse_dir_item(struct btrfs_item_cache item_cache,
                          uint32_t generation, int item_type,
                          uint64_t root_dirid);
static int parse_inode_ref(struct btrfs_item_cache item_cache,
                           uint32_t generation, uint64_t root_dirid);
static int pending_inode_relations_flush(const struct inode_instance_id *id);

static uint64_t normalize_root_dirid(uint64_t root_dirid) {
  if (root_dirid == INVALID_OBJECTID)
    return 0;
  return root_dirid;
}

static void active_namespace_uuid(uint8_t fs_uuid[BTRFS_UUID_SIZE]) {
  const uint8_t *active_uuid = active_file_tree_fs_uuid();
  memset(fs_uuid, 0, BTRFS_UUID_SIZE);
  if (active_uuid)
    memcpy(fs_uuid, active_uuid, BTRFS_UUID_SIZE);
}

static int generation_fs_uuid_add(
    const uint8_t fs_uuid[BTRFS_UUID_SIZE]) {
  if (!fs_uuid)
    return -1;
  for (size_t i = 0; i < g_generation_fs_uuid_count; i++) {
    if (memcmp(g_generation_fs_uuids[i], fs_uuid, BTRFS_UUID_SIZE) == 0)
      return 0;
  }
  if (g_generation_fs_uuid_count == g_generation_fs_uuid_cap) {
    size_t ncap = g_generation_fs_uuid_cap ? g_generation_fs_uuid_cap * 2 : 4;
    if (ncap > SIZE_MAX / sizeof(*g_generation_fs_uuids))
      return -1;
    void *next =
        realloc(g_generation_fs_uuids,
                ncap * sizeof(*g_generation_fs_uuids));
    if (!next)
      return -1;
    g_generation_fs_uuids = next;
    g_generation_fs_uuid_cap = ncap;
  }
  memcpy(g_generation_fs_uuids[g_generation_fs_uuid_count++], fs_uuid,
         BTRFS_UUID_SIZE);
  return 0;
}

static void seen_reset(void) { g_seen_count = 0; }

static void inode_generation_bindings_reset(void) {
  g_inode_generation_binding_count = 0;
}

static int inode_generation_binding_put(const struct inode_instance_id *id) {
  if (!id)
    return -1;

  uint8_t fs_uuid[BTRFS_UUID_SIZE];
  active_namespace_uuid(fs_uuid);
  uint64_t root_dirid = normalize_root_dirid(id->root_dirid);
  for (size_t i = 0; i < g_inode_generation_binding_count; i++) {
    if (memcmp(g_inode_generation_bindings[i].fs_uuid, fs_uuid,
               BTRFS_UUID_SIZE) == 0 &&
        g_inode_generation_bindings[i].root_dirid == root_dirid &&
        g_inode_generation_bindings[i].objectid == id->objectid) {
      g_inode_generation_bindings[i].inode_generation = id->inode_generation;
      return 0;
    }
  }

  if (g_inode_generation_binding_count == g_inode_generation_binding_cap) {
    size_t ncap = g_inode_generation_binding_cap
                      ? g_inode_generation_binding_cap * 2
                      : 256;
    void *next = realloc(g_inode_generation_bindings,
                         ncap * sizeof(*g_inode_generation_bindings));
    if (!next)
      return -1;
    g_inode_generation_bindings = next;
    g_inode_generation_binding_cap = ncap;
  }

  g_inode_generation_bindings[g_inode_generation_binding_count++] =
      (struct inode_generation_binding){
          .root_dirid = root_dirid,
          .objectid = id->objectid,
          .inode_generation = id->inode_generation,
      };
  memcpy(g_inode_generation_bindings[g_inode_generation_binding_count - 1]
             .fs_uuid,
         fs_uuid, BTRFS_UUID_SIZE);
  return 0;
}

static int inode_generation_binding_get(uint64_t root_dirid, uint64_t objectid,
                                        struct inode_instance_id *id) {
  uint8_t fs_uuid[BTRFS_UUID_SIZE];
  active_namespace_uuid(fs_uuid);
  root_dirid = normalize_root_dirid(root_dirid);
  for (size_t i = 0; i < g_inode_generation_binding_count; i++) {
    if (memcmp(g_inode_generation_bindings[i].fs_uuid, fs_uuid,
               BTRFS_UUID_SIZE) == 0 &&
        g_inode_generation_bindings[i].root_dirid == root_dirid &&
        g_inode_generation_bindings[i].objectid == objectid) {
      if (id) {
        *id = (struct inode_instance_id){
            .root_dirid = root_dirid,
            .objectid = objectid,
            .inode_generation =
                g_inode_generation_bindings[i].inode_generation,
        };
      }
      return 0;
    }
  }
  return -1;
}

static int inode_generation_catalog_add(const struct inode_instance_id *id) {
  if (!id)
    return -1;
  uint8_t fs_uuid[BTRFS_UUID_SIZE];
  active_namespace_uuid(fs_uuid);
  struct inode_instance_id normalized = *id;
  normalized.root_dirid = normalize_root_dirid(normalized.root_dirid);

  for (size_t i = 0; i < g_inode_generation_catalog_count; i++) {
    if (memcmp(g_inode_generation_catalog[i].fs_uuid, fs_uuid,
               BTRFS_UUID_SIZE) == 0 &&
        inode_instance_id_equal(&g_inode_generation_catalog[i].id,
                                &normalized))
      return 0;
  }

  if (g_inode_generation_catalog_count == g_inode_generation_catalog_cap) {
    size_t ncap =
        g_inode_generation_catalog_cap ? g_inode_generation_catalog_cap * 2
                                       : 256;
    void *next = realloc(g_inode_generation_catalog,
                         ncap * sizeof(*g_inode_generation_catalog));
    if (!next)
      return -1;
    g_inode_generation_catalog = next;
    g_inode_generation_catalog_cap = ncap;
  }
  struct inode_generation_catalog_entry *entry =
      &g_inode_generation_catalog[g_inode_generation_catalog_count++];
  memcpy(entry->fs_uuid, fs_uuid, BTRFS_UUID_SIZE);
  entry->id = normalized;
  return 0;
}

static int inode_generation_catalog_find_unique(
    uint64_t root_dirid, uint64_t objectid, struct inode_instance_id *id) {
  const struct inode_instance_id *match = NULL;
  uint8_t fs_uuid[BTRFS_UUID_SIZE];
  active_namespace_uuid(fs_uuid);
  root_dirid = normalize_root_dirid(root_dirid);
  for (size_t i = 0; i < g_inode_generation_catalog_count; i++) {
    if (memcmp(g_inode_generation_catalog[i].fs_uuid, fs_uuid,
               BTRFS_UUID_SIZE) != 0 ||
        g_inode_generation_catalog[i].id.root_dirid != root_dirid ||
        g_inode_generation_catalog[i].id.objectid != objectid)
      continue;
    if (match && match->inode_generation !=
                     g_inode_generation_catalog[i].id.inode_generation)
      return -1;
    match = &g_inode_generation_catalog[i].id;
  }
  if (!match)
    return -1;
  if (id)
    *id = *match;
  return 0;
}

static void inode_generation_catalog_clear(void) {
  free(g_inode_generation_catalog);
  g_inode_generation_catalog = NULL;
  g_inode_generation_catalog_count = 0;
  g_inode_generation_catalog_cap = 0;
}

static size_t pending_inode_relation_bucket(
    const uint8_t fs_uuid[BTRFS_UUID_SIZE], uint64_t root_dirid,
    uint64_t wait_objectid, size_t bucket_count) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < BTRFS_UUID_SIZE; i++) {
    hash ^= fs_uuid[i];
    hash *= UINT64_C(1099511628211);
  }
  hash ^= root_dirid;
  hash *= UINT64_C(1099511628211);
  hash ^= wait_objectid;
  hash *= UINT64_C(1099511628211);
  return (size_t)(hash & (bucket_count - 1));
}

static int pending_inode_relations_resize(size_t bucket_count) {
  struct pending_inode_relation **buckets =
      calloc(bucket_count, sizeof(*buckets));
  struct pending_inode_relation **tails =
      calloc(bucket_count, sizeof(*tails));
  if (!buckets || !tails) {
    free(buckets);
    free(tails);
    return -1;
  }

  for (size_t i = 0; i < g_pending_inode_relation_bucket_count; i++) {
    struct pending_inode_relation *relation =
        g_pending_inode_relation_buckets[i];
    while (relation) {
      struct pending_inode_relation *next = relation->next;
      size_t bucket = pending_inode_relation_bucket(
          relation->fs_uuid, relation->root_dirid, relation->wait_objectid,
          bucket_count);
      relation->next = NULL;
      if (tails[bucket])
        tails[bucket]->next = relation;
      else
        buckets[bucket] = relation;
      tails[bucket] = relation;
      relation = next;
    }
  }

  free(g_pending_inode_relation_buckets);
  free(g_pending_inode_relation_bucket_tails);
  g_pending_inode_relation_buckets = buckets;
  g_pending_inode_relation_bucket_tails = tails;
  g_pending_inode_relation_bucket_count = bucket_count;
  return 0;
}

static int pending_inode_relations_reserve(void) {
  if (g_pending_inode_relation_bucket_count == 0)
    return pending_inode_relations_resize(64);
  if (g_pending_inode_relation_count == SIZE_MAX)
    return -1;
  size_t max_load = g_pending_inode_relation_bucket_count -
                    g_pending_inode_relation_bucket_count / 4;
  if (g_pending_inode_relation_count + 1 <= max_load)
    return 0;
  if (g_pending_inode_relation_bucket_count > SIZE_MAX / 2)
    return -1;
  return pending_inode_relations_resize(
      g_pending_inode_relation_bucket_count * 2);
}

static int pending_inode_relation_add(
    enum pending_relation_kind kind,
    const struct btrfs_item_cache *item_cache,
    uint32_t observation_generation, uint64_t root_dirid,
    uint64_t wait_objectid) {
  if (!item_cache)
    return -1;
  uint8_t fs_uuid[BTRFS_UUID_SIZE];
  active_namespace_uuid(fs_uuid);
  root_dirid = normalize_root_dirid(root_dirid);
  if (pending_inode_relations_reserve() != 0)
    return -1;
  size_t bucket = pending_inode_relation_bucket(
      fs_uuid, root_dirid, wait_objectid,
      g_pending_inode_relation_bucket_count);
  for (struct pending_inode_relation *current =
           g_pending_inode_relation_buckets[bucket];
       current; current = current->next) {
    if (memcmp(current->fs_uuid, fs_uuid, BTRFS_UUID_SIZE) == 0 &&
        current->kind == kind && current->item_cache.index == item_cache->index &&
        current->item_cache.objectid == item_cache->objectid &&
        current->observation_generation == observation_generation &&
        current->root_dirid == root_dirid &&
        current->wait_objectid == wait_objectid)
      return 0;
  }

  struct pending_inode_relation *relation = malloc(sizeof(*relation));
  if (!relation)
    return -1;
  *relation = (struct pending_inode_relation){
      .kind = kind,
      .item_cache = *item_cache,
      .observation_generation = observation_generation,
      .root_dirid = root_dirid,
      .wait_objectid = wait_objectid,
  };
  memcpy(relation->fs_uuid, fs_uuid, BTRFS_UUID_SIZE);
  relation->next = NULL;
  if (g_pending_inode_relation_bucket_tails[bucket])
    g_pending_inode_relation_bucket_tails[bucket]->next = relation;
  else
    g_pending_inode_relation_buckets[bucket] = relation;
  g_pending_inode_relation_bucket_tails[bucket] = relation;
  g_pending_inode_relation_count++;
  return 0;
}

static void pending_inode_relations_clear(void) {
  for (size_t i = 0; i < g_pending_inode_relation_bucket_count; i++) {
    struct pending_inode_relation *relation =
        g_pending_inode_relation_buckets[i];
    while (relation) {
      struct pending_inode_relation *next = relation->next;
      free(relation);
      relation = next;
    }
  }
  free(g_pending_inode_relation_buckets);
  free(g_pending_inode_relation_bucket_tails);
  g_pending_inode_relation_buckets = NULL;
  g_pending_inode_relation_bucket_tails = NULL;
  g_pending_inode_relation_bucket_count = 0;
  g_pending_inode_relation_count = 0;
}

void root_tree_generation_state_reset(void) {
  inode_generation_bindings_reset();
  pending_inode_relations_clear();
  g_generation_fs_uuid_count = 0;
}

static int seen_mark_typed(const struct inode_instance_id *id,
                           uint8_t key_type) {
  const uint8_t *fs_uuid = active_file_tree_fs_uuid();
  if (!id)
    return -1;
  uint64_t root_dirid = normalize_root_dirid(id->root_dirid);
  for (size_t i = 0; i < g_seen_count; i++) {
    if ((!fs_uuid ||
         memcmp(g_seen_entries[i].fs_uuid, fs_uuid, BTRFS_UUID_SIZE) == 0) &&
        g_seen_entries[i].root_dirid == root_dirid &&
        g_seen_entries[i].objectid == id->objectid &&
        g_seen_entries[i].inode_generation == id->inode_generation &&
        g_seen_entries[i].key_type == key_type)
      return 0;
  }
  if (g_seen_count == g_seen_cap) {
    size_t ncap = g_seen_cap ? g_seen_cap * 2 : 256;
    seen_entry *nq = realloc(g_seen_entries, ncap * sizeof(*g_seen_entries));
    if (!nq)
      return -1;
    g_seen_entries = nq;
    g_seen_cap = ncap;
  }
  seen_entry entry = {.root_dirid = root_dirid,
                      .objectid = id->objectid,
                      .inode_generation = id->inode_generation,
                      .key_type = key_type};
  if (fs_uuid)
    memcpy(entry.fs_uuid, fs_uuid, BTRFS_UUID_SIZE);
  g_seen_entries[g_seen_count++] = entry;
  return 0;
}

static int seen_mark(const struct inode_instance_id *id) {
  return seen_mark_typed(id, INODE_ITEM);
}

static int seen_contains_typed(const struct inode_instance_id *id,
                               uint8_t key_type) {
  const uint8_t *fs_uuid = active_file_tree_fs_uuid();
  if (!id)
    return 0;
  uint64_t root_dirid = normalize_root_dirid(id->root_dirid);
  for (size_t i = 0; i < g_seen_count; i++) {
    if ((!fs_uuid ||
         memcmp(g_seen_entries[i].fs_uuid, fs_uuid, BTRFS_UUID_SIZE) == 0) &&
        g_seen_entries[i].root_dirid == root_dirid &&
        g_seen_entries[i].objectid == id->objectid &&
        g_seen_entries[i].inode_generation == id->inode_generation &&
        g_seen_entries[i].key_type == key_type)
      return 1;
  }
  return 0;
}

static char *inode_or_typed_cache_path(const struct inode_instance_id *id,
                                       uint8_t key_type, int cache_type) {
  if (!id)
    return NULL;
  if (key_type == INODE_ITEM)
    return file_tree_inode_cache_path(id, cache_type);
  return file_tree_cache_path_typed(id->objectid, id->root_dirid, key_type,
                                    cache_type);
}

static int extent_data_logical_address(uint32_t extent_item_index,
                                       uint64_t *logical_address) {
  if (!logical_address)
    return -1;
  *logical_address = INVALID_OBJECTID;
  char *path = item_cache_path(EXTENT_DATA_INDEX);
  FILE *file = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!file)
    return -1;

  struct extent_data_item extent_data;
  int result = 0;
  if (fseek(file, extent_item_index, SEEK_SET) != 0 ||
      fread(&extent_data, sizeof(extent_data), 1, file) != 1) {
    result = -1;
  } else if (extent_data.extent_type == EXTENT_REG ||
             extent_data.extent_type == EXTENT_PREALLOC) {
    struct extent_data_reg_prealloc reg_prealloc;
    if (fread(&reg_prealloc, sizeof(reg_prealloc), 1, file) != 1)
      result = -1;
    else if (extent_data.compression && reg_prealloc.logical_address &&
             reg_prealloc.logical_address != INVALID_OBJECTID)
      *logical_address = reg_prealloc.logical_address;
    else
      (void)extent_source_logical_address(&reg_prealloc, logical_address);
  }

  if (fclose(file) != 0)
    result = -1;
  return result;
}

static int
select_file_tree_cache_for_header(const struct parse_btrfs_header *header) {
  char *path = NULL;

  if (!header)
    return -1;

  if (!should_parse_fs_uuid(header->fs_uuid))
    return 0;

  if (generation_fs_uuid_add(header->fs_uuid) != 0)
    return -1;

  select_file_tree_fs_uuid(header->fs_uuid);
  path = file_tree_cache_dir_path();
  if (!path)
    return -1;
  if (ensure_cache_dir(path) != 0) {
    perror("ensure_file_tree_cache_dir");
    free(path);
    return -1;
  }
  free(path);
  return 1;
}

static int is_node_cache_filename(const char *name) {
  if (!name || !name[0])
    return 0;
  if (strstr(name, FILE_TREE_NODE_NAME_CACHE_NAME) ||
      strstr(name, FILE_TREE_NODE_CHILD_CACHE_NAME) ||
      strstr(name, FILE_TREE_NODE_EXTENT_CACHE_NAME) ||
      strstr(name, FILE_TREE_NODE_MODIFY_CACHE_NAME) ||
      strstr(name, FILE_TREE_NODE_MOVE_CACHE_NAME))
    return 0;
  return strchr(name, '_') != NULL;
}

static int parse_node_cache_filename(const char *name, uint64_t *root_dirid,
                                     uint64_t *objectid,
                                     uint64_t *inode_generation,
                                     uint8_t *key_type) {
  unsigned parsed_key_type = INODE_ITEM;
  int consumed = 0;

  if (!is_node_cache_filename(name))
    return -1;

  if (sscanf(name, "%" SCNu64 "_%" SCNu64 "_g%" SCNu64 "%n", root_dirid,
             objectid, inode_generation, &consumed) == 3 &&
      name[consumed] == '\0') {
    *key_type = INODE_ITEM;
    return 0;
  }

  if (sscanf(name, "%" SCNu64 "_%" SCNu64 "_%u%n", root_dirid, objectid,
             &parsed_key_type, &consumed) == 3 &&
      name[consumed] == '\0' && parsed_key_type <= UINT8_MAX) {
    *inode_generation = 0;
    *key_type = (uint8_t)parsed_key_type;
    return 0;
  }

  consumed = 0;
  if (sscanf(name, "%" SCNu64 "_%" SCNu64 "%n", root_dirid, objectid,
             &consumed) == 2 &&
      name[consumed] == '\0') {
    *inode_generation = 0;
    *key_type = INODE_ITEM;
    return 0;
  }

  return -1;
}

static int child_delete(const struct inode_instance_id *parent_id,
                        const struct inode_instance_id *child_id,
                        uint8_t child_key_type, uint32_t generation) {
  char *path = NULL;
  FILE *file = NULL;
  struct stat st;
  struct child_cache child;
  int found = 0;

  if (!parent_id || !child_id || parent_id->objectid == INVALID_OBJECTID ||
      child_id->objectid == parent_id->objectid)
    return parent_id && child_id ? 0 : -1;

  path = file_tree_inode_cache_path(parent_id, FILE_TREE_NODE_CHILD_CACHE);
  if (!path)
    return -1;

  if (stat(path, &st) != 0) {
    file = fopen(path, "w+b");
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    child = (struct child_cache){.objectid = child_id->objectid,
                                 .inode_generation =
                                     child_id->inode_generation,
                                 .key_type = child_key_type,
                                 .generation = generation,
                                 .type = DELETED};
  } else {
    file = fopen(path, "r+b");
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    while (fread(&child, sizeof(struct child_cache), 1, file) == 1) {
      if (child.objectid == child_id->objectid &&
          child.inode_generation == child_id->inode_generation &&
          child.key_type == child_key_type) {
        if (fseek(file, -(long)sizeof(struct child_cache), SEEK_CUR) != 0) {
          fclose(file);
          free(path);
          return -1;
        }
        found = 1;
        break;
      }
    }
    if (ferror(file)) {
      fclose(file);
      free(path);
      return -1;
    }
    if (!found) {
      child = (struct child_cache){.objectid = child_id->objectid,
                                   .inode_generation =
                                       child_id->inode_generation,
                                   .key_type = child_key_type,
                                   .generation = generation,
                                   .type = DELETED};
      if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        free(path);
        return -1;
      }
    } else {
      child.generation = generation;
      child.type = DELETED;
    }
  }
  int result =
      fwrite(&child, sizeof(struct child_cache), 1, file) == 1 ? 0 : -1;
  if (fclose(file) != 0)
    result = -1;
  free(path);
  return result;
}

static int mark_deletions_in_active_namespace(uint32_t generation) {
  char *dir_path = file_tree_cache_dir_all_path();
  DIR *dir = NULL;
  struct dirent *entry = NULL;
  int result = 0;

  if (!dir_path)
    return -1;

  dir = opendir(dir_path);
  if (!dir) {
    perror("opendir");
    free(dir_path);
    return -1;
  }

  while ((entry = readdir(dir)) != NULL) {
    uint64_t root_dirid = 0;
    uint64_t objectid = 0;
    uint64_t inode_generation = 0;
    uint8_t key_type = INODE_ITEM;
    char *node_path = NULL;
    FILE *node_file = NULL;
    struct total_file_tree node;

    if (parse_node_cache_filename(entry->d_name, &root_dirid, &objectid,
                                  &inode_generation, &key_type) != 0)
      continue;
    struct inode_instance_id id = {
        .root_dirid = root_dirid,
        .objectid = objectid,
        .inode_generation = inode_generation,
    };
    if (seen_contains_typed(&id, key_type))
      continue;

    node_path = inode_or_typed_cache_path(&id, key_type, FILE_TREE_NODE_CACHE);
    if (!node_path) {
      result = -1;
      break;
    }

    node_file = fopen(node_path, "rb");
    free(node_path);
    if (!node_file) {
      result = -1;
      break;
    }
    if (fread(&node, sizeof(node), 1, node_file) != 1) {
      fclose(node_file);
      result = -1;
      break;
    }
    if (fclose(node_file) != 0) {
      result = -1;
      break;
    }

    if (node.recent_gen >= generation)
      continue;

    struct inode_instance_id parent_id = {
        .root_dirid = root_dirid,
        .objectid = node.parent_objectid,
        .inode_generation = node.parent_inode_generation,
    };
    id.inode_generation = node.inode_generation;
    if (child_delete(&parent_id, &id, key_type, generation) != 0) {
      result = -1;
      break;
    }
  }

  if (closedir(dir) != 0)
    result = -1;
  free(dir_path);
  return result;
}

static int mark_deletions(uint32_t generation) {
  uint8_t active_uuid[BTRFS_UUID_SIZE];
  int result = 0;
  active_namespace_uuid(active_uuid);
  for (size_t i = 0; i < g_generation_fs_uuid_count; i++) {
    select_file_tree_fs_uuid(g_generation_fs_uuids[i]);
    if (mark_deletions_in_active_namespace(generation) != 0)
      result = -1;
  }
  select_file_tree_fs_uuid(active_uuid);
  return result;
}

static void seen_cleanup(void) {
  free(g_seen_entries);
  g_seen_entries = NULL;
  g_seen_count = 0;
  g_seen_cap = 0;
}

static int cmp_address_map_by_generation_asc(const void *a, const void *b) {
  const struct address_map_cache *pa = a;
  const struct address_map_cache *pb = b;

  if (pa->generation < pb->generation)
    return -1;
  if (pa->generation > pb->generation)
    return 1;
  if (pa->logical_address < pb->logical_address)
    return -1;
  if (pa->logical_address > pb->logical_address)
    return 1;
  return 0;
}

static int is_file_tree_owner(uint64_t owner) {
  switch (owner) {
  case 0:
  case 1:
  case 2:
  case 3:
  case 4:
  case 7:
  case 8:
  case 9:
  case 10:
  case 0xFFFFFFFFFFFFFFFBULL:
  case 0xFFFFFFFFFFFFFFFAULL:
  case 0xFFFFFFFFFFFFFFF9ULL:
  case 0xFFFFFFFFFFFFFFF8ULL:
  case 0xFFFFFFFFFFFFFFF7ULL:
  case 0xFFFFFFFFFFFFFFF6ULL:
  case 0xFFFFFFFFFFFFFF00ULL:
    return 0;
  default:
    return owner == 5 || owner >= 256;
  }
}

static int
address_map_generation_is_allowed(const struct address_map_cache *rec) {
  uint64_t limit = current_fs_generation_limit();
  return limit == 0 || rec->generation <= limit;
}

static int address_map_matches_block(const struct address_map_cache *rec,
                                     uint64_t logical_address,
                                     uint64_t generation) {
  if (!address_map_generation_is_allowed(rec))
    return 0;
  if (rec->logical_address != logical_address)
    return 0;
  return generation == 0 || rec->generation == generation;
}

static int extent_source_should_replace(uint8_t existing_source,
                                        uint8_t new_source) {
  if (existing_source == EXTENT_SOURCE_COMPLETE &&
      new_source == EXTENT_SOURCE_SALVAGED)
    return 0;
  return 1;
}

static int parse_inode_item(struct btrfs_item_cache item_cache,
                            uint32_t generation, uint64_t root_dirid) {
  struct inode_item inode;
  char *inode_item_path = item_cache_path(INODE_ITEM_INDEX);
  FILE *inode_item_file = inode_item_path ? fopen(inode_item_path, "rb") : NULL;
  free(inode_item_path);
  if (!inode_item_file)
    return -1;
  if (fseek(inode_item_file, item_cache.index, SEEK_SET) != 0 ||
      fread(&inode, sizeof(inode), 1, inode_item_file) != 1) {
    fclose(inode_item_file);
    return -1;
  }
  if (fclose(inode_item_file) != 0)
    return -1;

  struct inode_instance_id id = {
      .root_dirid = normalize_root_dirid(root_dirid),
      .objectid = item_cache.objectid,
      .inode_generation = inode.generation,
  };
  if (inode_generation_binding_put(&id) != 0 ||
      inode_generation_catalog_add(&id) != 0)
    return -1;

  char *path = file_tree_inode_cache_path(&id, FILE_TREE_NODE_CACHE);
  FILE *file = NULL;
  struct total_file_tree file_node;
  struct stat st;
  if (!path)
    return -1;
  if (stat(path, &st) != 0) {
    file = fopen(path, "w+b");
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    file_node = (struct total_file_tree){.objectid = item_cache.objectid,
                                         .inode_generation =
                                             id.inode_generation,
                                         .key_type = INODE_ITEM,
                                         .inode_item_index = item_cache.index,
                                         .extent_item_count = 0,
                                         .parent_objectid = INVALID_OBJECTID,
                                         .parent_inode_generation = 0,
                                         .child_count = 0,
                                         .name_count = 0,
                                         .create_gen = generation,
                                         .recent_gen = generation,
                                         .modify_count = 0,
                                         .move_count = 0};
  } else {
    file = fopen(path, "r+b");
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    if (fread(&file_node, sizeof(struct total_file_tree), 1, file) != 1 ||
        file_node.objectid != id.objectid ||
        file_node.inode_generation != id.inode_generation ||
        file_node.key_type != INODE_ITEM) {
      fclose(file);
      free(path);
      return -1;
    }
    if (file_node.inode_item_index == INVALID_INDEX) {
      file_node.inode_item_index = item_cache.index;
    } else if (file_node.inode_item_index != item_cache.index) {
      if (file_node.modify_count == UINT32_MAX) {
        fclose(file);
        free(path);
        return -1;
      }
      file_node.modify_count += 1;
      char *modify_cache_path =
          file_tree_inode_cache_path(&id, FILE_TREE_NODE_MODIFY_CACHE);
      FILE *modify_cache =
          modify_cache_path ? fopen(modify_cache_path, "ab") : NULL;
      if (!modify_cache) {
        perror("fopen");
        free(modify_cache_path);
        fclose(file);
        free(path);
        return -1;
      }
      struct modify_cache modify_cache_data = {
          .modify_gen = generation,
          .prev_inode_item_index = file_node.inode_item_index,
          .current_inode_item_index = item_cache.index};
      int modify_result =
          fwrite(&modify_cache_data, sizeof(struct modify_cache), 1,
                 modify_cache) == 1
              ? 0
              : -1;
      if (fclose(modify_cache) != 0)
        modify_result = -1;
      if (modify_result != 0) {
        free(modify_cache_path);
        fclose(file);
        free(path);
        return -1;
      }
      free(modify_cache_path);
      file_node.inode_item_index = item_cache.index;
    }
    file_node.recent_gen = generation;
    if (fseek(file, 0, SEEK_SET) != 0) {
      fclose(file);
      free(path);
      return -1;
    }
  }
  int result =
      fwrite(&file_node, sizeof(struct total_file_tree), 1, file) == 1 ? 0 : -1;
  if (seen_mark(&id) != 0)
    result = -1;
  if (fclose(file) != 0)
    result = -1;
  if (result != 0) {
    free(path);
    return -1;
  }
  free(path);
  return pending_inode_relations_flush(&id);
}

static int parse_extent_data(struct btrfs_item_cache item_cache,
                             uint32_t generation, uint64_t root_dirid,
                             uint8_t source) {
  struct inode_instance_id id;
  int identity_result =
      source == EXTENT_SOURCE_SALVAGED
          ? inode_generation_catalog_find_unique(root_dirid,
                                                 item_cache.objectid, &id)
          : inode_generation_binding_get(root_dirid, item_cache.objectid, &id);
  if (identity_result != 0)
    return 0;

  char *path = file_tree_inode_cache_path(&id, FILE_TREE_NODE_CACHE);
  char *extent_data_cache_path =
      file_tree_inode_cache_path(&id, FILE_TREE_NODE_EXTENT_CACHE);
  FILE *extent_data_cache_file = NULL;
  struct stat st;
  struct extent_cache extent_data_cache;
  uint64_t logical_address;
  if (extent_data_logical_address(item_cache.index, &logical_address) != 0) {
    free(path);
    free(extent_data_cache_path);
    return -1;
  }
  struct extent_cache new_extent_data_cache = {
      .extent_item_index = item_cache.index,
      .extent_item_size = item_cache.size,
      .logical_address = logical_address,
      .offset_in_file = item_cache.offset,
      .generation = generation,
      .source = source,
  };
  int flag = 0;
  if (!path || !extent_data_cache_path) {
    free(path);
    free(extent_data_cache_path);
    return -1;
  }
  if (stat(extent_data_cache_path, &st) != 0) {
    extent_data_cache_file = fopen(extent_data_cache_path, "w+b");
    if (!extent_data_cache_file) {
      perror("fopen");
      free(path);
      free(extent_data_cache_path);
      return -1;
    }
    if (fwrite(&new_extent_data_cache, sizeof(struct extent_cache), 1,
               extent_data_cache_file) != 1) {
      fclose(extent_data_cache_file);
      free(path);
      free(extent_data_cache_path);
      return -1;
    }
  } else {
    extent_data_cache_file = fopen(extent_data_cache_path, "r+b");
    if (!extent_data_cache_file) {
      perror("fopen");
      free(path);
      free(extent_data_cache_path);
      return -1;
    }

    if (fseek(extent_data_cache_file, 0, SEEK_END) != 0) {
      fclose(extent_data_cache_file);
      free(path);
      free(extent_data_cache_path);
      return -1;
    }
    long extent_cache_size = ftell(extent_data_cache_file);
    if (extent_cache_size < 0 ||
        (size_t)extent_cache_size % sizeof(struct extent_cache) != 0 ||
        fseek(extent_data_cache_file, 0, SEEK_SET) != 0) {
      fclose(extent_data_cache_file);
      free(path);
      free(extent_data_cache_path);
      return -1;
    }

    while (fread(&extent_data_cache, sizeof(struct extent_cache), 1,
                 extent_data_cache_file) == 1) {
      if (extent_data_cache.extent_item_index == item_cache.index ||
          (extent_data_cache.offset_in_file == item_cache.offset &&
           extent_data_cache.logical_address == logical_address)) {
        flag = 1;
        if (extent_source_should_replace(extent_data_cache.source, source)) {
          if (fseek(extent_data_cache_file,
                    -(long)sizeof(struct extent_cache), SEEK_CUR) != 0 ||
              fwrite(&new_extent_data_cache, sizeof(struct extent_cache), 1,
                     extent_data_cache_file) != 1) {
            fclose(extent_data_cache_file);
            free(path);
            free(extent_data_cache_path);
            return -1;
          }
        }
        break;
      }
    }
    if (ferror(extent_data_cache_file)) {
      fclose(extent_data_cache_file);
      free(path);
      free(extent_data_cache_path);
      return -1;
    }
    if (flag == 0) {
      if (fseek(extent_data_cache_file, 0, SEEK_END) != 0 ||
          fwrite(&new_extent_data_cache, sizeof(struct extent_cache), 1,
                 extent_data_cache_file) != 1) {
        fclose(extent_data_cache_file);
        free(path);
        free(extent_data_cache_path);
        return -1;
      }
    }
  }
  free(extent_data_cache_path);
  if (fclose(extent_data_cache_file) != 0) {
    free(path);
    return -1;
  }
  FILE *file = NULL;
  struct total_file_tree file_node;

  if (stat(path, &st) != 0) {
    file = fopen(path, "w+b");
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    file_node = (struct total_file_tree){.objectid = item_cache.objectid,
                                         .inode_generation =
                                             id.inode_generation,
                                         .key_type = INODE_ITEM,
                                         .inode_item_index = INVALID_INDEX,
                                         .extent_item_count = 1,
                                         .parent_objectid = INVALID_OBJECTID,
                                         .parent_inode_generation = 0,
                                         .child_count = 0,
                                         .name_count = 0,
                                         .create_gen = generation,
                                         .recent_gen = generation,
                                         .modify_count = 0,
                                         .move_count = 0};
  } else {
    file = fopen(path, "r+b");
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    if (fread(&file_node, sizeof(struct total_file_tree), 1, file) != 1 ||
        file_node.objectid != id.objectid ||
        file_node.inode_generation != id.inode_generation ||
        file_node.key_type != INODE_ITEM ||
        (!flag && file_node.extent_item_count == UINT32_MAX)) {
      fclose(file);
      free(path);
      return -1;
    }
    if (!flag)
      file_node.extent_item_count++;
    file_node.recent_gen = generation;
    if (fseek(file, 0, SEEK_SET) != 0) {
      fclose(file);
      free(path);
      return -1;
    }
  }
  int result =
      fwrite(&file_node, sizeof(struct total_file_tree), 1, file) == 1 ? 0 : -1;
  if (seen_mark(&id) != 0)
    result = -1;
  if (fclose(file) != 0)
    result = -1;
  free(path);
  return result;
}

static int child_move(const struct inode_instance_id *parent_id,
                      const struct inode_instance_id *child_id,
                      uint8_t child_key_type, uint32_t generation) {
  if (!parent_id || !child_id)
    return -1;
  char *path =
      file_tree_inode_cache_path(parent_id, FILE_TREE_NODE_CHILD_CACHE);
  FILE *file = NULL;
  struct stat st;
  struct child_cache child;
  int found = 0;
  if (!path)
    return -1;
  if (stat(path, &st) != 0) {
    file = fopen(path, "w+b");
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    child = (struct child_cache){.objectid = child_id->objectid,
                                 .inode_generation =
                                     child_id->inode_generation,
                                 .key_type = child_key_type,
                                 .generation = generation,
                                 .type = MOVED};
  } else {
    file = fopen(path, "r+b");
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    while (fread(&child, sizeof(struct child_cache), 1, file) == 1) {
      if (child.objectid == child_id->objectid &&
          child.inode_generation == child_id->inode_generation &&
          child.key_type == child_key_type) {
        if (fseek(file, -(long)sizeof(struct child_cache), SEEK_CUR) != 0) {
          fclose(file);
          free(path);
          return -1;
        }
        found = 1;
        break;
      }
    }
    if (ferror(file)) {
      fclose(file);
      free(path);
      return -1;
    }
    if (!found) {
      child = (struct child_cache){.objectid = child_id->objectid,
                                   .inode_generation =
                                       child_id->inode_generation,
                                   .key_type = child_key_type,
                                   .generation = generation,
                                   .type = MOVED};
      if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        free(path);
        return -1;
      }
    } else {
      child.generation = generation;
      child.type = MOVED;
    }
  }
  int result =
      fwrite(&child, sizeof(struct child_cache), 1, file) == 1 ? 0 : -1;
  if (fclose(file) != 0)
    result = -1;
  free(path);
  return result;
}

static int child_add(const struct inode_instance_id *parent_id,
                     const struct inode_instance_id *child_id,
                     uint8_t child_key_type, uint32_t generation) {
  if (!parent_id || !child_id)
    return -1;
  char *path =
      file_tree_inode_cache_path(parent_id, FILE_TREE_NODE_CHILD_CACHE);
  FILE *file = NULL;
  uint32_t flag = 0;
  struct stat st;
  struct child_cache child;
  struct total_file_tree file_node;
  int found = 0;

  if (child_id->objectid == parent_id->objectid) {
    free(path);
    return 0;
  }

  if (!path) {
    perror("malloc");
    return -1;
  }

  if (stat(path, &st) != 0) {
    file = fopen(path, "w+b");
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    child = (struct child_cache){.objectid = child_id->objectid,
                                 .inode_generation =
                                     child_id->inode_generation,
                                 .key_type = child_key_type,
                                 .generation = generation,
                                 .type = CHILD};
    flag = 1;
  } else {
    file = fopen(path, "r+b");
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    while (fread(&child, sizeof(struct child_cache), 1, file) == 1) {
      if (child.objectid == child_id->objectid &&
          child.inode_generation == child_id->inode_generation &&
          child.key_type == child_key_type) {
        if (fseek(file, -(long)sizeof(struct child_cache), SEEK_CUR) != 0) {
          fclose(file);
          free(path);
          return -1;
        }
        found = 1;
        break;
      }
    }
    if (ferror(file)) {
      fclose(file);
      free(path);
      return -1;
    }
    if (!found) {
      child = (struct child_cache){.objectid = child_id->objectid,
                                   .inode_generation =
                                       child_id->inode_generation,
                                   .key_type = child_key_type,
                                   .generation = generation,
                                   .type = CHILD};
      if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        free(path);
        return -1;
      }
      flag = 1;
    } else {
      child.generation = generation;
      child.type = CHILD;
    }
  }
  if (fwrite(&child, sizeof(struct child_cache), 1, file) != 1) {
    fclose(file);
    free(path);
    return -1;
  }
  if (fclose(file) != 0) {
    free(path);
    return -1;
  }
  free(path);

  path = file_tree_inode_cache_path(parent_id, FILE_TREE_NODE_CACHE);
  if (!path) {
    perror("malloc");
    return -1;
  }
  if (stat(path, &st) != 0) {
    file = fopen(path, "w+b");
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    file_node = (struct total_file_tree){.objectid = parent_id->objectid,
                                         .inode_generation =
                                             parent_id->inode_generation,
                                         .key_type = INODE_ITEM,
                                         .inode_item_index = INVALID_INDEX,
                                         .extent_item_count = 0,
                                         .parent_objectid = INVALID_OBJECTID,
                                         .parent_inode_generation = 0,
                                         .child_count = 1,
                                         .name_count = 0,
                                         .create_gen = generation,
                                         .recent_gen = generation,
                                         .modify_count = 0,
                                         .move_count = 0};
  } else {
    file = fopen(path, "r+b");
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    if (fread(&file_node, sizeof(struct total_file_tree), 1, file) != 1 ||
        file_node.objectid != parent_id->objectid ||
        file_node.inode_generation != parent_id->inode_generation ||
        file_node.key_type != INODE_ITEM ||
        (flag && file_node.child_count == UINT32_MAX)) {
      fclose(file);
      free(path);
      return -1;
    }
    file_node.child_count += flag;
    file_node.recent_gen = generation;
    if (fseek(file, 0, SEEK_SET) != 0) {
      fclose(file);
      free(path);
      return -1;
    }
  }
  int result =
      fwrite(&file_node, sizeof(struct total_file_tree), 1, file) == 1 ? 0 : -1;
  if (seen_mark(parent_id) != 0)
    result = -1;
  if (fclose(file) != 0)
    result = -1;
  free(path);
  return result;
}

static int update_name_cache(const struct inode_instance_id *id,
                             uint8_t key_type, const uint8_t *name,
                             uint16_t name_len, uint32_t generation,
                             int refresh_existing, int *added) {
  if (!id || (name_len != 0 && !name) || !added)
    return -1;

  *added = 0;
  char *path = inode_or_typed_cache_path(id, key_type,
                                         FILE_TREE_NODE_NAME_CACHE);
  if (!path)
    return -1;

  struct stat st;
  int exists = stat(path, &st) == 0;
  FILE *file = fopen(path, exists ? "r+b" : "w+b");
  if (!file) {
    perror("fopen");
    free(path);
    return -1;
  }
  free(path);

  struct name_cache last = {0};
  long last_offset = -1;
  if (exists) {
    for (;;) {
      long offset = ftell(file);
      if (offset < 0) {
        free(last.name);
        fclose(file);
        return -1;
      }
      struct name_cache current = {0};
      size_t count = fread(&current, 1, NAME_CACHE_FIX_SIZE, file);
      if (count == 0) {
        if (ferror(file)) {
          free(last.name);
          fclose(file);
          return -1;
        }
        break;
      }
      if (count != NAME_CACHE_FIX_SIZE) {
        free(last.name);
        fclose(file);
        return -1;
      }
      current.name = current.name_len ? malloc(current.name_len) : NULL;
      if (current.name_len &&
          (!current.name ||
           fread(current.name, 1, current.name_len, file) !=
               current.name_len)) {
        free(current.name);
        free(last.name);
        fclose(file);
        return -1;
      }
      free(last.name);
      last = current;
      last_offset = offset;
    }
  }

  int same = last_offset >= 0 && last.name_len == name_len &&
             (name_len == 0 || memcmp(last.name, name, name_len) == 0);
  int result = 0;
  if (!same) {
    struct name_cache record = {
        .generation = generation, .name_len = name_len, .name = NULL};
    if (fseek(file, 0, SEEK_END) != 0 ||
        fwrite(&record, NAME_CACHE_FIX_SIZE, 1, file) != 1 ||
        (name_len && fwrite(name, name_len, 1, file) != 1)) {
      result = -1;
    } else {
      *added = 1;
    }
  } else if (refresh_existing) {
    last.generation = generation;
    uint8_t *last_name = last.name;
    last.name = NULL;
    if (fseek(file, last_offset, SEEK_SET) != 0 ||
        fwrite(&last, NAME_CACHE_FIX_SIZE, 1, file) != 1)
      result = -1;
    last.name = last_name;
  }

  free(last.name);
  if (fclose(file) != 0)
    result = -1;
  return result;
}

static int parse_dir_item(struct btrfs_item_cache item_cache,
                          uint32_t generation, int item_type,
                          uint64_t root_dirid) {
  FILE *file = NULL;
  struct stat st;
  struct total_file_tree file_node;
  struct dir_item_index dir_item;
  char *dir_item_path = item_cache_path(item_type);
  FILE *dir_item_file = dir_item_path ? fopen(dir_item_path, "rb") : NULL;
  free(dir_item_path);
  if (!dir_item_file) {
    perror("fopen");
    return -1;
  }
  if (fseek(dir_item_file, item_cache.index, SEEK_SET) != 0 ||
      fread(&dir_item, DIR_ITEM_INDEX_FIX_SIZE, 1, dir_item_file) != 1) {
    fclose(dir_item_file);
    return -1;
  }
  dir_item.name = calloc(dir_item.name_len, sizeof(uint8_t));
  if (dir_item.name_len &&
      (!dir_item.name ||
       fread(dir_item.name, dir_item.name_len, 1, dir_item_file) != 1)) {
    free(dir_item.name);
    fclose(dir_item_file);
    return -1;
  }
  if (fclose(dir_item_file) != 0) {
    free(dir_item.name);
    return -1;
  }

  uint8_t child_key_type = dir_item.item_type;
  struct inode_instance_id child_id = {
      .root_dirid = normalize_root_dirid(root_dirid),
      .objectid = dir_item.objectid,
      .inode_generation = 0,
  };
  if (child_key_type == INODE_ITEM &&
      inode_generation_binding_get(root_dirid, dir_item.objectid, &child_id) !=
          0) {
    enum pending_relation_kind kind = item_type == DIR_INDEX_INDEX
                                          ? PENDING_DIR_INDEX
                                          : PENDING_DIR_ITEM;
    int result = pending_inode_relation_add(kind, &item_cache, generation,
                                            root_dirid, dir_item.objectid);
    free(dir_item.name);
    return result;
  }

  struct inode_instance_id parent_id;
  int parent_is_bound =
      inode_generation_binding_get(root_dirid, item_cache.objectid,
                                   &parent_id) == 0;
  if (!parent_is_bound) {
    enum pending_relation_kind kind = item_type == DIR_INDEX_INDEX
                                          ? PENDING_DIR_INDEX
                                          : PENDING_DIR_ITEM;
    int result = pending_inode_relation_add(kind, &item_cache, generation,
                                            root_dirid, item_cache.objectid);
    free(dir_item.name);
    return result;
  }
  char *path = inode_or_typed_cache_path(&child_id, child_key_type,
                                         FILE_TREE_NODE_CACHE);
  if (!path) {
    free(dir_item.name);
    return -1;
  }

  if (stat(path, &st) != 0) {
    file = fopen(path, "w+b");
    if (!file) {
      perror("fopen");
      free(path);
      free(dir_item.name);
      return -1;
    }
    file_node = (struct total_file_tree){.objectid = dir_item.objectid,
                                         .inode_generation =
                                             child_id.inode_generation,
                                         .key_type = child_key_type,
                                         .inode_item_index = INVALID_INDEX,
                                         .extent_item_count = 0,
                                         .parent_objectid = item_cache.objectid,
                                         .parent_inode_generation =
                                             parent_id.inode_generation,
                                         .child_count = 0,
                                         .name_count = 1,
                                         .create_gen = generation,
                                         .recent_gen = generation,
                                         .modify_count = 0,
                                         .move_count = 0};
  } else {
    file = fopen(path, "r+b");
    if (!file) {
      perror("fopen");
      free(path);
      free(dir_item.name);
      return -1;
    }
    if (fread(&file_node, sizeof(struct total_file_tree), 1, file) != 1 ||
        file_node.objectid != child_id.objectid ||
        file_node.inode_generation != child_id.inode_generation ||
        file_node.key_type != child_key_type) {
      fclose(file);
      free(path);
      free(dir_item.name);
      return -1;
    }
    if (file_node.parent_objectid == INVALID_OBJECTID) {
      file_node.parent_objectid = item_cache.objectid;
      file_node.parent_inode_generation = parent_id.inode_generation;
    } else if (file_node.parent_objectid != item_cache.objectid ||
               file_node.parent_inode_generation !=
                   parent_id.inode_generation) {
      if (file_node.move_count == UINT32_MAX) {
        fclose(file);
        free(path);
        free(dir_item.name);
        return -1;
      }
      file_node.move_count += 1;
      char *move_cache_path = inode_or_typed_cache_path(
          &child_id, file_node.key_type, FILE_TREE_NODE_MOVE_CACHE);
      FILE *move_cache = move_cache_path ? fopen(move_cache_path, "ab") : NULL;
      if (!move_cache) {
        perror("fopen");
        free(move_cache_path);
        fclose(file);
        free(path);
        free(dir_item.name);
        return -1;
      }
      struct move_cache move_cache_data = {
          .move_gen = generation,
          .prev_parent_objectid = file_node.parent_objectid,
          .prev_parent_inode_generation =
              file_node.parent_inode_generation,
          .current_parent_objectid = item_cache.objectid,
          .current_parent_inode_generation = parent_id.inode_generation};
      int move_result =
          fwrite(&move_cache_data, sizeof(struct move_cache), 1, move_cache) ==
                  1
              ? 0
              : -1;
      if (fclose(move_cache) != 0)
        move_result = -1;
      free(move_cache_path);
      if (move_result != 0) {
        fclose(file);
        free(path);
        free(dir_item.name);
        return -1;
      }
      struct inode_instance_id previous_parent_id = {
          .root_dirid = child_id.root_dirid,
          .objectid = file_node.parent_objectid,
          .inode_generation = file_node.parent_inode_generation,
      };
      if (child_move(&previous_parent_id, &child_id, file_node.key_type,
                     generation) != 0) {
        fclose(file);
        free(path);
        free(dir_item.name);
        return -1;
      }
      file_node.parent_objectid = item_cache.objectid;
      file_node.parent_inode_generation = parent_id.inode_generation;
    }
    file_node.recent_gen = generation;
    if (fseek(file, 0, SEEK_SET) != 0) {
      fclose(file);
      free(path);
      free(dir_item.name);
      return -1;
    }
  }
  if (parent_is_bound &&
      child_add(&parent_id, &child_id, file_node.key_type, generation) != 0) {
    fclose(file);
    free(path);
    free(dir_item.name);
    return -1;
  }
  free(path);
  int added = 0;
  if (update_name_cache(&child_id, child_key_type, dir_item.name,
                        dir_item.name_len, generation, 0, &added) != 0 ||
      (added && file_node.name_count == UINT32_MAX)) {
    fclose(file);
    free(dir_item.name);
    return -1;
  }
  file_node.name_count += (uint32_t)added;
  int result =
      fwrite(&file_node, sizeof(struct total_file_tree), 1, file) == 1 ? 0 : -1;
  if (seen_mark_typed(&child_id, child_key_type) != 0)
    result = -1;
  if (fclose(file) != 0)
    result = -1;
  free(dir_item.name);
  return result;
}

static int pending_inode_relation_targets(
    const struct pending_inode_relation *relation,
    const struct inode_instance_id *id,
    const uint8_t fs_uuid[BTRFS_UUID_SIZE]) {
  return memcmp(relation->fs_uuid, fs_uuid, BTRFS_UUID_SIZE) == 0 &&
         normalize_root_dirid(relation->root_dirid) == id->root_dirid &&
         relation->wait_objectid == id->objectid;
}

static int pending_inode_relations_flush(const struct inode_instance_id *id) {
  uint8_t fs_uuid[BTRFS_UUID_SIZE];
  int result = 0;
  active_namespace_uuid(fs_uuid);
  if (g_pending_inode_relation_bucket_count == 0)
    return 0;
  size_t bucket = pending_inode_relation_bucket(
      fs_uuid, normalize_root_dirid(id->root_dirid), id->objectid,
      g_pending_inode_relation_bucket_count);
  struct pending_inode_relation *matches = NULL;
  struct pending_inode_relation **match_tail = &matches;
  struct pending_inode_relation **current =
      &g_pending_inode_relation_buckets[bucket];
  while (*current) {
    struct pending_inode_relation *relation = *current;
    if (!pending_inode_relation_targets(relation, id, fs_uuid)) {
      current = &relation->next;
      continue;
    }
    *current = relation->next;
    relation->next = NULL;
    *match_tail = relation;
    match_tail = &relation->next;
    g_pending_inode_relation_count--;
  }
  g_pending_inode_relation_bucket_tails[bucket] = NULL;
  for (struct pending_inode_relation *remaining =
           g_pending_inode_relation_buckets[bucket];
       remaining; remaining = remaining->next)
    g_pending_inode_relation_bucket_tails[bucket] = remaining;

  while (matches) {
    struct pending_inode_relation *relation = matches;
    matches = relation->next;
    select_file_tree_fs_uuid(relation->fs_uuid);
    if (relation->kind == PENDING_INODE_REF) {
      if (parse_inode_ref(relation->item_cache,
                          relation->observation_generation,
                          relation->root_dirid) != 0)
        result = -1;
    } else {
      int item_type = relation->kind == PENDING_DIR_INDEX ? DIR_INDEX_INDEX
                                                          : DIR_ITEM_INDEX;
      if (parse_dir_item(relation->item_cache,
                         relation->observation_generation, item_type,
                         relation->root_dirid) != 0)
        result = -1;
    }
    select_file_tree_fs_uuid(fs_uuid);
    free(relation);
  }
  return result;
}

static int parse_inode_ref(struct btrfs_item_cache item_cache,
                           uint32_t generation, uint64_t root_dirid) {
  struct inode_instance_id id;
  if (inode_generation_binding_get(root_dirid, item_cache.objectid, &id) != 0)
    return 0;
  struct inode_instance_id parent_id;
  if (inode_generation_binding_get(root_dirid, item_cache.offset, &parent_id) !=
      0)
    return pending_inode_relation_add(PENDING_INODE_REF, &item_cache,
                                      generation, root_dirid,
                                      item_cache.offset);

  char *path = file_tree_inode_cache_path(&id, FILE_TREE_NODE_CACHE);
  if (!path)
    return -1;

  struct inode_ref inode_ref;
  char *inode_ref_path = item_cache_path(INODE_REF_INDEX);
  FILE *inode_ref_file = inode_ref_path ? fopen(inode_ref_path, "rb") : NULL;
  free(inode_ref_path);
  if (!inode_ref_file) {
    perror("fopen");
    free(path);
    return -1;
  }
  if (fseek(inode_ref_file, item_cache.index, SEEK_SET) != 0 ||
      fread(&inode_ref, INODE_REF_INDEX_FIX_SIZE, 1, inode_ref_file) != 1) {
    fclose(inode_ref_file);
    free(path);
    return -1;
  }
  inode_ref.name = inode_ref.name_len ? malloc(inode_ref.name_len) : NULL;
  if (inode_ref.name_len &&
      (!inode_ref.name ||
       fread(inode_ref.name, inode_ref.name_len, 1, inode_ref_file) != 1)) {
    free(inode_ref.name);
    fclose(inode_ref_file);
    free(path);
    return -1;
  }
  if (fclose(inode_ref_file) != 0) {
    free(inode_ref.name);
    free(path);
    return -1;
  }

  struct stat st;
  FILE *file = NULL;
  struct total_file_tree file_node;
  if (stat(path, &st) != 0) {
    file = fopen(path, "w+b");
    if (!file) {
      perror("fopen");
      free(path);
      free(inode_ref.name);
      return -1;
    }
    file_node = (struct total_file_tree){.objectid = item_cache.objectid,
                                         .inode_generation =
                                             id.inode_generation,
                                         .key_type = INODE_ITEM,
                                         .inode_item_index = INVALID_INDEX,
                                         .extent_item_count = 0,
                                         .parent_objectid = item_cache.offset,
                                         .parent_inode_generation =
                                             parent_id.inode_generation,
                                         .child_count = 0,
                                         .name_count = 1,
                                         .create_gen = generation,
                                         .recent_gen = generation,
                                         .modify_count = 0,
                                         .move_count = 0};
  } else {
    file = fopen(path, "r+b");
    if (!file) {
      perror("fopen");
      free(path);
      free(inode_ref.name);
      return -1;
    }
    if (fread(&file_node, sizeof(file_node), 1, file) != 1 ||
        file_node.objectid != id.objectid ||
        file_node.inode_generation != id.inode_generation ||
        file_node.key_type != INODE_ITEM) {
      fclose(file);
      free(path);
      free(inode_ref.name);
      return -1;
    }
    if (file_node.parent_objectid == INVALID_OBJECTID) {
      file_node.parent_objectid = item_cache.offset;
      file_node.parent_inode_generation = parent_id.inode_generation;
    } else if (file_node.parent_objectid != item_cache.offset ||
               file_node.parent_inode_generation !=
                   parent_id.inode_generation) {
      if (file_node.move_count == UINT32_MAX) {
        fclose(file);
        free(path);
        free(inode_ref.name);
        return -1;
      }
      file_node.move_count++;
      char *move_path =
          file_tree_inode_cache_path(&id, FILE_TREE_NODE_MOVE_CACHE);
      FILE *move_file = move_path ? fopen(move_path, "ab") : NULL;
      if (!move_file) {
        perror("fopen");
        free(move_path);
        fclose(file);
        free(path);
        free(inode_ref.name);
        return -1;
      }
      struct move_cache move = {
          .move_gen = generation,
          .prev_parent_objectid = file_node.parent_objectid,
          .prev_parent_inode_generation = file_node.parent_inode_generation,
          .current_parent_objectid = item_cache.offset,
          .current_parent_inode_generation = parent_id.inode_generation};
      int move_result =
          fwrite(&move, sizeof(move), 1, move_file) == 1 ? 0 : -1;
      if (fclose(move_file) != 0)
        move_result = -1;
      free(move_path);
      struct inode_instance_id previous_parent_id = {
          .root_dirid = id.root_dirid,
          .objectid = file_node.parent_objectid,
          .inode_generation = file_node.parent_inode_generation,
      };
      if (move_result != 0 ||
          child_move(&previous_parent_id, &id, file_node.key_type,
                     generation) != 0) {
        fclose(file);
        free(path);
        free(inode_ref.name);
        return -1;
      }
      file_node.parent_objectid = item_cache.offset;
      file_node.parent_inode_generation = parent_id.inode_generation;
    }
    file_node.recent_gen = generation;
    if (fseek(file, 0, SEEK_SET) != 0) {
      fclose(file);
      free(path);
      free(inode_ref.name);
      return -1;
    }
  }

  if (child_add(&parent_id, &id, file_node.key_type, generation) != 0) {
    fclose(file);
    free(path);
    free(inode_ref.name);
    return -1;
  }
  free(path);

  int added = 0;
  if (update_name_cache(&id, INODE_ITEM, inode_ref.name, inode_ref.name_len,
                        generation, 1, &added) != 0 ||
      (added && file_node.name_count == UINT32_MAX)) {
    fclose(file);
    free(inode_ref.name);
    return -1;
  }
  file_node.name_count += (uint32_t)added;
  int result = fwrite(&file_node, sizeof(file_node), 1, file) == 1 ? 0 : -1;
  if (seen_mark(&id) != 0)
    result = -1;
  if (fclose(file) != 0)
    result = -1;
  free(inode_ref.name);
  return result;
}

static int parse_root_item(struct btrfs_item_cache item_cache,
                           uint32_t generation) {
  uint64_t unparse_tree[] = {0x2,
                             0x3,
                             0x4,
                             0x8,
                             0x9,
                             0xA,
                             0xFFFFFFFFFFFFFFFBULL,
                             0xFFFFFFFFFFFFFFFAULL,
                             0xFFFFFFFFFFFFFFF9ULL,
                             0xFFFFFFFFFFFFFFF8ULL,
                             0xFFFFFFFFFFFFFFF7ULL,
                             0xFFFFFFFFFFFFFFF6ULL,
                             0xFFFFFFFFFFFFFF00ULL};
  struct root_item node;
  struct parse_btrfs_header header;
  struct btrfs_item_cache *item_caches = NULL;
  struct block_ptr *block_ptr = NULL;
  struct address_map_cache rec;
  char *path = NULL;
  FILE *file = NULL;
  int result = 0;
  path = cache_path(MAP_CACHE);
  FILE *map_cache_file = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!map_cache_file) {
    perror("fopen");
    return -1;
  }
  for (int i = 0; i < 13; i++) {
    if (unparse_tree[i] == item_cache.objectid) {
      fclose(map_cache_file);
      return 0;
    }
  }
  path = item_cache_path(ROOT_ITEM_INDEX);
  file = path ? fopen(path, "rb") : NULL;
  if (!file) {
    perror("fopen");
    free(path);
    fclose(map_cache_file);
    return -1;
  }

  if (fseek(file, item_cache.index, SEEK_SET) != 0 ||
      fread(&node, sizeof(struct root_item), 1, file) != 1) {
    fclose(file);
    free(path);
    fclose(map_cache_file);
    return -1;
  }
  fclose(file);
  free(path);
  int found = 0;
  while (fread(&rec, sizeof(rec), 1, map_cache_file) == 1) {
    if (address_map_matches_block(&rec, node.bytenr, node.generation)) {
      found = 1;
      break;
    }
  }

  if (!found) {
    fclose(map_cache_file);
    return 0;
  }
  path = cache_path(rec.cache_type);
  file = path ? fopen(path, "rb") : NULL;
  if (!file) {
    perror("fopen");
    free(path);
    fclose(map_cache_file);
    return -1;
  }
  free(path);
  if (fseek(file, rec.index, SEEK_SET) != 0 ||
      fread(&header, sizeof(struct parse_btrfs_header), 1, file) != 1) {
    fclose(file);
    fclose(map_cache_file);
    return -1;
  }
  if (!should_parse_fs_uuid(header.fs_uuid)) {
    fclose(file);
    fclose(map_cache_file);
    return 0;
  }

  if (node.level != header.level) {
    fclose(file);
    fclose(map_cache_file);
    return -1;
  }

  if (node.level == 0) {
    item_caches = header.num_items
                      ? calloc(header.num_items, sizeof(struct btrfs_item_cache))
                      : NULL;
    if (header.num_items != 0 && !item_caches) {
      perror("calloc");
      fclose(file);
      fclose(map_cache_file);
      return -1;
    }
    for (uint32_t j = 0; j < header.num_items; j++) {
      if (fread(&item_caches[j], sizeof(struct btrfs_item_cache), 1, file) !=
          1) {
        free(item_caches);
        fclose(file);
        fclose(map_cache_file);
        return -1;
      }
    }
    fclose(file);
    result =
        parse_leaf_node(&header, item_caches, generation, item_cache.objectid);
    free(item_caches);
  } else {
    block_ptr = calloc(header.num_items, sizeof(struct block_ptr));
    if (!block_ptr) {
      perror("calloc");
      fclose(file);
      fclose(map_cache_file);
      return -1;
    }
    for (uint32_t j = 0; j < header.num_items; j++) {
      if (fread(&block_ptr[j], sizeof(struct block_ptr), 1, file) != 1) {
        free(block_ptr);
        fclose(file);
        fclose(map_cache_file);
        return -1;
      }
    }
    fclose(file);
    if (header.owner != 0xA)
      result = parse_internal_node(&header, block_ptr, generation,
                                   item_cache.objectid);
    free(block_ptr);
  }
  fclose(map_cache_file);
  return result;
}

int parse_leaf_node(struct parse_btrfs_header *header,
                    struct btrfs_item_cache *item_caches, uint32_t generation,
                    uint64_t root_dirid) {
  if (!header || (header->num_items != 0 && !item_caches))
    return -1;
  if (header->owner == PROJECT_BTRFS_CSUM_TREE_OBJECTID)
    return 0;

  int selection = select_file_tree_cache_for_header(header);
  if (selection <= 0)
    return selection;

  for (uint32_t i = 0; i < header->num_items; i++) {
    switch (item_caches[i].type) {
    case INODE_ITEM:
      if (parse_inode_item(item_caches[i], generation, root_dirid) != 0)
        return -1;
      break;
    case EXTENT_DATA:
      if (header->owner != 0x2)
        if (parse_extent_data(item_caches[i], generation, root_dirid,
                              EXTENT_SOURCE_COMPLETE) != 0)
          return -1;
      break;
    case ROOT_ITEM:
      if (parse_root_item(item_caches[i], generation) != 0)
        return -1;
      break;
    case DIR_ITEM:
      if (parse_dir_item(item_caches[i], generation, DIR_ITEM_INDEX,
                         root_dirid) != 0)
        return -1;
      break;
    case DIR_INDEX:
      if (parse_dir_item(item_caches[i], generation, DIR_INDEX_INDEX,
                         root_dirid) != 0)
        return -1;
      break;
    case INODE_REF:
      if (parse_inode_ref(item_caches[i], generation, root_dirid) != 0)
        return -1;
      break;
    default:
      break;
    }
  }
  return 0;
}

static int
parse_extent_data_only_leaf(struct parse_btrfs_header *header,
                            struct btrfs_item_cache *item_caches) {
  if (!header || (header->num_items != 0 && !item_caches))
    return -1;
  if (!is_file_tree_owner(header->owner))
    return 0;
  int selection = select_file_tree_cache_for_header(header);
  if (selection <= 0)
    return selection;

  for (uint32_t i = 0; i < header->num_items; i++) {
    if (item_caches[i].type != EXTENT_DATA)
      continue;
    if (parse_extent_data(item_caches[i], header->generation, header->owner,
                          EXTENT_SOURCE_SALVAGED) != 0)
      return -1;
  }
  return 0;
}

int parse_internal_node(struct parse_btrfs_header *top_item,
                        struct block_ptr *block_ptr, uint32_t generation,
                        uint64_t root_dirid) {
  if (!top_item || (top_item->num_items != 0 && !block_ptr))
    return -1;
  struct address_map_cache rec;
  struct parse_btrfs_header header;
  struct block_ptr *block_ptr_cache = NULL;
  struct btrfs_item_cache *item_caches = NULL;
  char *path = NULL;
  path = cache_path(MAP_CACHE);
  FILE *map_cache_file = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!map_cache_file) {
    perror("fopen");
    return -1;
  }
  for (uint32_t i = 0; i < top_item->num_items; i++) {
    int found = 0;
    fseek(map_cache_file, 0, SEEK_SET);
    while (fread(&rec, sizeof(rec), 1, map_cache_file) == 1) {
      if (address_map_matches_block(&rec, block_ptr[i].block_offset,
                                    block_ptr[i].generation)) {
        found = 1;
        break;
      }
    }
    if (!found) {
      if (top_item->owner != 0x2) {
        continue;
      }
      fclose(map_cache_file);
      return -1;
    }
    path = cache_path(rec.cache_type);
    FILE *cache_file = path ? fopen(path, "rb") : NULL;
    free(path);
    if (!cache_file) {
      perror("fopen");
      fclose(map_cache_file);
      return -1;
    }
    if (fseek(cache_file, rec.index, SEEK_SET) != 0 ||
        fread(&header, sizeof(struct parse_btrfs_header), 1, cache_file) != 1) {
      fclose(cache_file);
      fclose(map_cache_file);
      return -1;
    }
    if (!should_parse_fs_uuid(header.fs_uuid)) {
      fclose(cache_file);
      continue;
    }
    if (header.level == 0) {
      item_caches = header.num_items
                        ? calloc(header.num_items,
                                 sizeof(struct btrfs_item_cache))
                        : NULL;
      if (header.num_items != 0 && !item_caches) {
        perror("calloc");
        fclose(cache_file);
        fclose(map_cache_file);
        return -1;
      }
      for (uint32_t j = 0; j < header.num_items; j++) {
        if (fread(&item_caches[j], sizeof(struct btrfs_item_cache), 1,
                  cache_file) != 1) {
          free(item_caches);
          fclose(cache_file);
          fclose(map_cache_file);
          return -1;
        }
      }
      int parse_result =
          parse_leaf_node(&header, item_caches, generation, root_dirid);
      free(item_caches);
      item_caches = NULL;
      if (parse_result != 0) {
        fclose(cache_file);
        fclose(map_cache_file);
        return -1;
      }
    } else {
      block_ptr_cache = calloc(header.num_items, sizeof(struct block_ptr));
      if (!block_ptr_cache) {
        perror("calloc");
        fclose(cache_file);
        fclose(map_cache_file);
        return -1;
      }
      for (uint32_t j = 0; j < header.num_items; j++) {
        if (fread(&block_ptr_cache[j], sizeof(struct block_ptr), 1,
                  cache_file) != 1) {
          free(block_ptr_cache);
          fclose(cache_file);
          fclose(map_cache_file);
          return -1;
        }
      }
      int parse_result =
          parse_internal_node(&header, block_ptr_cache, generation, root_dirid);
      free(block_ptr_cache);
      block_ptr_cache = NULL;
      if (parse_result != 0) {
        fclose(cache_file);
        fclose(map_cache_file);
        return -1;
      }
    }
    fclose(cache_file);
  }
  fclose(map_cache_file);
  return 0;
}

static int backfill_extent_data_from_file_tree_leaves(void) {
  char *map_path = cache_path(MAP_CACHE);
  char *leaf_path = cache_path(LEAF_NODE_CACHE);
  FILE *map_file = map_path ? fopen(map_path, "rb") : NULL;
  FILE *leaf_file = leaf_path ? fopen(leaf_path, "rb") : NULL;
  free(map_path);
  free(leaf_path);

  if (!map_file || !leaf_file) {
    if (map_file)
      fclose(map_file);
    if (leaf_file)
      fclose(leaf_file);
    return -1;
  }

  int result = 0;
  struct address_map_cache rec;
  while (fread(&rec, sizeof(rec), 1, map_file) == 1) {
    if (rec.cache_type != LEAF_NODE_CACHE)
      continue;
    if (!is_file_tree_owner(rec.owner))
      continue;
    if (!address_map_generation_is_allowed(&rec))
      continue;
    if (fseek(leaf_file, rec.index, SEEK_SET) != 0) {
      result = -1;
      break;
    }

    struct parse_btrfs_header header;
    if (fread(&header, sizeof(header), 1, leaf_file) != 1) {
      result = -1;
      break;
    }
    if (header.level != 0 || !is_file_tree_owner(header.owner))
      continue;

    struct btrfs_item_cache *item_caches =
        header.num_items ? calloc(header.num_items, sizeof(*item_caches))
                         : NULL;
    if (header.num_items != 0 && !item_caches) {
      perror("calloc");
      result = -1;
      break;
    }

    int ok = 1;
    for (uint32_t i = 0; i < header.num_items; i++) {
      if (fread(&item_caches[i], sizeof(*item_caches), 1, leaf_file) != 1) {
        ok = 0;
        break;
      }
    }

    if (ok) {
      if (parse_extent_data_only_leaf(&header, item_caches) != 0)
        result = -1;
    } else {
      result = -1;
    }
    free(item_caches);
    if (result != 0)
      break;
  }
  if (ferror(map_file))
    result = -1;

  fclose(map_file);
  fclose(leaf_file);
  return result;
}

int parse_root_tree_node(struct address_map_cache **root_tree_nodes,
                         int same_root) {
  if (!root_tree_nodes || same_root <= 0)
    return -1;
  uint8_t top_level = 0;
  int flag = 0;
  char *path = NULL;
  FILE *file = NULL;
  struct parse_btrfs_header current_item;
  struct parse_btrfs_header top_item;
  struct block_ptr *block_ptr = NULL;
  for (int i = 0; i < same_root; i++) {
    if (root_tree_nodes[i]->cache_type == INTERNAL_NODE_CACHE) {
      if (!file) {
        path = cache_path(INTERNAL_NODE_CACHE);
        file = path ? fopen(path, "rb") : NULL;
        free(path);
        if (!file) {
          perror("fopen");
          return -1;
        }
      }
      if (fseek(file, root_tree_nodes[i]->index, SEEK_SET) != 0 ||
          fread(&current_item, sizeof(struct parse_btrfs_header), 1, file) !=
              1) {
        fclose(file);
        free(block_ptr);
        return -1;
      }
      if (!should_parse_fs_uuid(current_item.fs_uuid))
        continue;
      if (current_item.level == 0) {
        struct btrfs_item_cache *leaf_items =
            current_item.num_items
                ? calloc(current_item.num_items, sizeof(*leaf_items))
                : NULL;
        if (current_item.num_items != 0 && !leaf_items) {
          fclose(file);
          free(block_ptr);
          return -1;
        }
        for (uint32_t j = 0; j < current_item.num_items; j++) {
          if (fread(&leaf_items[j], sizeof(*leaf_items), 1, file) != 1) {
            free(leaf_items);
            fclose(file);
            free(block_ptr);
            return -1;
          }
        }
        int leaf_result =
            parse_leaf_node(&current_item, leaf_items,
                            current_item.generation, INVALID_OBJECTID);
        free(leaf_items);
        if (leaf_result != 0) {
          fclose(file);
          free(block_ptr);
          return -1;
        }
        if (flag == 0)
          flag = 2;
        continue;
      }
      flag = 1;
      if (current_item.level > top_level) {
        top_level = current_item.level;
        top_item = current_item;
        size_t block_count = top_item.num_items;
        if (block_count == 0 ||
            block_count > SIZE_MAX / sizeof(struct block_ptr)) {
          fclose(file);
          free(block_ptr);
          return -1;
        }
        void *new_block_ptr =
            realloc(block_ptr, block_count * sizeof(struct block_ptr));
        if (!new_block_ptr) {
          perror("realloc");
          fclose(file);
          free(block_ptr);
          return -1;
        }
        block_ptr = new_block_ptr;
        for (uint32_t j = 0; j < top_item.num_items; j++) {
          if (fread(&block_ptr[j], sizeof(struct block_ptr), 1, file) != 1) {
            fclose(file);
            free(block_ptr);
            return -1;
          }
        }
      }
    }
  }
  path = file_tree_cache_dir_path();
  if (!path || ensure_cache_dir(path) != 0) {
    perror("ensure_file_tree_cache_dir");
    free(path);
    if (file)
      fclose(file);
    free(block_ptr);
    return -1;
  }
  free(path);
  if (flag == 0) {
    if (file)
      fclose(file);
    path = cache_path(LEAF_NODE_CACHE);
    file = path ? fopen(path, "rb") : NULL;
    if (!file) {
      perror("fopen");
      free(path);
      return -1;
    }
    free(path);
    struct btrfs_item_cache *item_caches = NULL;
    for (int i = 0; i < same_root; i++) {
      if (root_tree_nodes[i]->cache_type != LEAF_NODE_CACHE)
        continue;
      if (fseek(file, root_tree_nodes[i]->index, SEEK_SET) != 0 ||
          fread(&current_item, sizeof(struct parse_btrfs_header), 1, file) !=
              1) {
        free(item_caches);
        fclose(file);
        return -1;
      }
      if (!should_parse_fs_uuid(current_item.fs_uuid))
        continue;
      free(item_caches);
      item_caches = current_item.num_items
                        ? calloc(current_item.num_items,
                                 sizeof(struct btrfs_item_cache))
                        : NULL;
      if (current_item.num_items != 0 && !item_caches) {
        perror("calloc");
        fclose(file);
        return -1;
      }
      for (uint32_t j = 0; j < current_item.num_items; j++) {
        if (fread(&item_caches[j], sizeof(struct btrfs_item_cache), 1, file) !=
            1) {
          free(item_caches);
          fclose(file);
          return -1;
        }
      }
      if (parse_leaf_node(&current_item, item_caches, current_item.generation,
                          INVALID_OBJECTID) != 0) {
        free(item_caches);
        fclose(file);
        return -1;
      }
    }
    fclose(file);
    free(item_caches);
  } else if (flag == 1) {
    int result = parse_internal_node(&top_item, block_ptr, top_item.generation,
                                     INVALID_OBJECTID);
    fclose(file);
    free(block_ptr);
    return result;
  } else {
    fclose(file);
    free(block_ptr);
  }
  return 0;
}

int parse_root_tree(uint32_t **generations, size_t *generation_count) {
  struct address_map_cache *items = NULL;
  size_t n = 0;
  size_t cap = 0;
  uint32_t *gens = NULL;
  size_t gen_n = 0;
  size_t gen_cap = 0;

  if (generations)
    *generations = NULL;
  if (generation_count)
    *generation_count = 0;

  root_tree_generation_state_reset();
  inode_generation_catalog_clear();

  char *map_cache_path = cache_path(MAP_CACHE);
  FILE *map_cache_file = fopen(map_cache_path, "rb");
  free(map_cache_path);
  if (!map_cache_file) {
    perror("fopen");
    return -1;
  }

  struct address_map_cache rec;
  while (fread(&rec, sizeof(rec), 1, map_cache_file) == 1) {
    if (rec.owner != 0x1)
      continue;
    if (!address_map_generation_is_allowed(&rec))
      continue;

    if (n == cap) {
      size_t ncap = cap ? cap * 2 : 8;
      void *nq = realloc(items, ncap * sizeof(*items));
      if (!nq) {
        perror("realloc");
        fclose(map_cache_file);
        free(items);
        return -1;
      }
      items = nq;
      cap = ncap;
    }
    items[n++] = rec;
  }
  fclose(map_cache_file);

  if (n == 0) {
    free(items);
    return 0;
  }

  if (n > 1)
    qsort(items, n, sizeof(*items), cmp_address_map_by_generation_asc);

  size_t run_start = 0;
  for (size_t i = 1; i <= n; i++) {
    int run_end =
        (i == n) || items[i].generation != items[run_start].generation;
    if (!run_end)
      continue;

    if (gen_n == gen_cap) {
      size_t ncap = gen_cap ? gen_cap * 2 : 8;
      uint32_t *nq = realloc(gens, ncap * sizeof(*gens));
      if (!nq) {
        perror("realloc");
        free(gens);
        free(items);
        return -1;
      }
      gens = nq;
      gen_cap = ncap;
    }
    gens[gen_n++] = items[run_start].generation;

    size_t count = i - run_start;
    struct address_map_cache **ptrs = malloc(count * sizeof(*ptrs));
    if (!ptrs) {
      perror("malloc");
      free(gens);
      free(items);
      return -1;
    }
    for (size_t k = 0; k < count; k++)
      ptrs[k] = &items[run_start + k];

    if (count > (size_t)INT_MAX) {
      fprintf(stderr, "parse_root_tree: group too large for int\n");
      free(ptrs);
      free(gens);
      free(items);
      return -1;
    }

    seen_reset();
    root_tree_generation_state_reset();
    if (parse_root_tree_node(ptrs, (int)count) != 0) {
      free(ptrs);
      free(gens);
      free(items);
      seen_cleanup();
      root_tree_generation_state_reset();
      inode_generation_catalog_clear();
      return -1;
    }
    if (mark_deletions(items[run_start].generation) != 0) {
      free(ptrs);
      free(gens);
      free(items);
      seen_cleanup();
      root_tree_generation_state_reset();
      inode_generation_catalog_clear();
      return -1;
    }
    free(ptrs);
    run_start = i;
  }

  free(items);
  seen_cleanup();
  root_tree_generation_state_reset();
  if (backfill_extent_data_from_file_tree_leaves() != 0) {
    free(gens);
    inode_generation_catalog_clear();
    return -1;
  }
  inode_generation_catalog_clear();

  if (generations && generation_count) {
    *generations = gens;
    *generation_count = gen_n;
  } else {
    free(gens);
  }

  select_current_file_tree_fs_uuid();
  return 0;
}
