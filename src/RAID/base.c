#include "../../hdr/init.h"
#include "../../hdr/compression.h"
#include "../../hdr/csum_index.h"
#include "../../hdr/checksum.h"
#include "../../hdr/layout.h"
#include "../../hdr/raid.h"
#include "../../hdr/type.h"
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/btrfs_tree.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define BTRFS_SUBVOL_ROOT_INODE 256ULL
#define EXTRACT_COMPONENT_MAX 255U
#define EXTRACT_GENERATED_NAME_PREFIX "[btrfs-recovered]-"

struct extract_context;
struct extract_visit_set;

struct extract_ancestor {
  struct inode_instance_id id;
  uint8_t key_type;
  const struct extract_ancestor *parent;
};

struct extract_task {
  uint64_t objectid;
  uint64_t root_dirid;
  uint64_t inode_generation;
  uint8_t key_type;
  char *parent_path;
  struct extract_ancestor *ancestors;
  unsigned depth;
  struct extract_task *next;
};

struct extract_context {
  pthread_mutex_t mutex;
  pthread_cond_t work_ready;
  pthread_cond_t all_done;
  struct extract_task *head;
  struct extract_task *tail;
  size_t pending;
  int stopping;
  int errors;
  int include_moved;
  struct extraction_options options;
  struct extract_visit_set *visited;
};

#define EXTRACT_VISIT_INITIAL_CAPACITY 16U

struct extract_visit_entry {
  struct inode_instance_id id;
  uint8_t key_type;
  uint8_t occupied;
  uint8_t successful;
};

struct extract_visit_set {
  pthread_mutex_t mutex;
  struct extract_visit_entry *entries;
  size_t capacity;
  size_t count;
};

static uint64_t extract_visit_hash_value(uint64_t value) {
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

static size_t extract_visit_hash(const struct inode_instance_id *id,
                                 uint8_t key_type) {
  uint64_t hash = extract_visit_hash_value(id->root_dirid);
  hash ^= extract_visit_hash_value(id->objectid + UINT64_C(0x9e3779b97f4a7c15));
  hash ^= extract_visit_hash_value(id->inode_generation +
                                   UINT64_C(0x517cc1b727220a95));
  hash ^= extract_visit_hash_value((uint64_t)key_type +
                                   UINT64_C(0x6eed0e9da4d94a4f));
  return (size_t)hash;
}

static int extract_visit_entry_matches(
    const struct extract_visit_entry *entry,
    const struct inode_instance_id *id, uint8_t key_type) {
  return entry->occupied && entry->key_type == key_type &&
         inode_instance_id_equal(&entry->id, id);
}

static int extract_visit_set_resize_locked(struct extract_visit_set *set,
                                           size_t new_capacity) {
  if (new_capacity < set->capacity ||
      new_capacity > SIZE_MAX / sizeof(*set->entries))
    return -1;

  struct extract_visit_entry *entries =
      calloc(new_capacity, sizeof(*entries));
  if (!entries)
    return -1;

  for (size_t i = 0; i < set->capacity; i++) {
    const struct extract_visit_entry *old = &set->entries[i];
    if (!old->occupied)
      continue;
    size_t slot = extract_visit_hash(&old->id, old->key_type) &
                  (new_capacity - 1);
    while (entries[slot].occupied)
      slot = (slot + 1) & (new_capacity - 1);
    entries[slot] = *old;
  }

  free(set->entries);
  set->entries = entries;
  set->capacity = new_capacity;
  return 0;
}

static int extract_visit_set_init(struct extract_visit_set *set) {
  memset(set, 0, sizeof(*set));
  set->entries =
      calloc(EXTRACT_VISIT_INITIAL_CAPACITY, sizeof(*set->entries));
  if (!set->entries)
    return -1;
  set->capacity = EXTRACT_VISIT_INITIAL_CAPACITY;
  if (pthread_mutex_init(&set->mutex, NULL) != 0) {
    free(set->entries);
    memset(set, 0, sizeof(*set));
    return -1;
  }
  return 0;
}

static void extract_visit_set_destroy(struct extract_visit_set *set) {
  pthread_mutex_destroy(&set->mutex);
  free(set->entries);
  memset(set, 0, sizeof(*set));
}

static int extract_visit_set_contains(struct extract_visit_set *set,
                                      const struct inode_instance_id *id,
                                      uint8_t key_type) {
  if (!set || !id)
    return 0;

  pthread_mutex_lock(&set->mutex);
  size_t slot = extract_visit_hash(id, key_type) & (set->capacity - 1);
  while (set->entries[slot].occupied) {
    if (extract_visit_entry_matches(&set->entries[slot], id, key_type)) {
      int successful = set->entries[slot].successful;
      pthread_mutex_unlock(&set->mutex);
      return successful;
    }
    slot = (slot + 1) & (set->capacity - 1);
  }
  pthread_mutex_unlock(&set->mutex);
  return 0;
}

static int extract_visit_set_set_success(struct extract_visit_set *set,
                                         const struct inode_instance_id *id,
                                         uint8_t key_type, int successful) {
  if (!set)
    return 0;
  if (!id)
    return -1;

  pthread_mutex_lock(&set->mutex);
  size_t slot = extract_visit_hash(id, key_type) & (set->capacity - 1);
  while (set->entries[slot].occupied) {
    if (extract_visit_entry_matches(&set->entries[slot], id, key_type)) {
      set->entries[slot].successful = successful != 0;
      pthread_mutex_unlock(&set->mutex);
      return 0;
    }
    slot = (slot + 1) & (set->capacity - 1);
  }
  pthread_mutex_unlock(&set->mutex);
  return -1;
}

static int extract_visit_set_mark(struct extract_visit_set *set,
                                  const struct inode_instance_id *id,
                                  uint8_t key_type) {
  if (!set)
    return 0;
  if (!id)
    return -1;

  pthread_mutex_lock(&set->mutex);
  if ((set->count + 1) * 10 >= set->capacity * 7 &&
      extract_visit_set_resize_locked(set, set->capacity * 2) != 0) {
    pthread_mutex_unlock(&set->mutex);
    return -1;
  }

  size_t slot = extract_visit_hash(id, key_type) & (set->capacity - 1);
  while (set->entries[slot].occupied) {
    if (extract_visit_entry_matches(&set->entries[slot], id, key_type)) {
      pthread_mutex_unlock(&set->mutex);
      return 0;
    }
    slot = (slot + 1) & (set->capacity - 1);
  }

  set->entries[slot].id = *id;
  set->entries[slot].key_type = key_type;
  set->entries[slot].occupied = 1;
  set->count++;
  pthread_mutex_unlock(&set->mutex);
  return 0;
}

static int extract_visit_set_merge_successful(
    struct extract_visit_set *destination,
    const struct extract_visit_set *source) {
  for (size_t i = 0; i < source->capacity; i++) {
    const struct extract_visit_entry *entry = &source->entries[i];
    if (!entry->occupied || !entry->successful)
      continue;
    if (extract_visit_set_mark(destination, &entry->id, entry->key_type) != 0 ||
        extract_visit_set_set_success(destination, &entry->id,
                                      entry->key_type, 1) != 0)
      return -1;
  }
  return 0;
}

#define VERIFIED_WRITE_BUCKET_COUNT 64U
#define CSUM_LOOKUP_BATCH_SECTORS 16U

struct verified_write_region {
  uint64_t start;
  uint64_t end;
};

struct verified_file_regions {
  uint64_t name_hash;
  char *name;
  struct verified_write_region *regions;
  size_t region_count;
  size_t region_capacity;
  struct verified_file_regions *next;
};

struct verified_write_bucket {
  pthread_mutex_t mutex;
  struct verified_file_regions *files;
};

static struct verified_write_bucket
    verified_write_buckets[VERIFIED_WRITE_BUCKET_COUNT];
static pthread_once_t verified_write_once = PTHREAD_ONCE_INIT;
static int verified_write_buckets_ready;

static char *copy_region_name(const uint8_t *name) {
  if (!name)
    return NULL;
  size_t len = strlen((const char *)name) + 1;
  char *copy = malloc(len);
  if (copy)
    memcpy(copy, name, len);
  return copy;
}

static int ranges_overlap(uint64_t start, uint64_t end, uint64_t other_start,
                          uint64_t other_end) {
  return start < other_end && other_start < end;
}

static uint64_t verified_name_hash(const uint8_t *name) {
  uint64_t hash = UINT64_C(14695981039346656037);
  while (*name) {
    hash ^= *name++;
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static void initialize_verified_write_buckets(void) {
  size_t initialized = 0;
  for (; initialized < VERIFIED_WRITE_BUCKET_COUNT; initialized++) {
    if (pthread_mutex_init(&verified_write_buckets[initialized].mutex, NULL) !=
        0)
      break;
  }
  if (initialized == VERIFIED_WRITE_BUCKET_COUNT) {
    verified_write_buckets_ready = 1;
    return;
  }
  while (initialized > 0) {
    initialized--;
    pthread_mutex_destroy(&verified_write_buckets[initialized].mutex);
  }
}

static struct verified_write_bucket *
verified_bucket_for_name(const uint8_t *name, uint64_t *name_hash) {
  if (!name || pthread_once(&verified_write_once,
                            initialize_verified_write_buckets) != 0 ||
      !verified_write_buckets_ready)
    return NULL;

  *name_hash = verified_name_hash(name);
  return &verified_write_buckets[*name_hash % VERIFIED_WRITE_BUCKET_COUNT];
}

static struct verified_file_regions *
find_verified_file(struct verified_write_bucket *bucket, const uint8_t *name,
                   uint64_t name_hash) {
  for (struct verified_file_regions *file = bucket->files; file;
       file = file->next) {
    if (file->name_hash == name_hash &&
        strcmp(file->name, (const char *)name) == 0)
      return file;
  }
  return NULL;
}

static int reserve_verified_regions(struct verified_file_regions *file,
                                    size_t needed) {
  if (needed <= file->region_capacity)
    return 0;

  size_t capacity = file->region_capacity ? file->region_capacity : 4;
  while (capacity < needed) {
    if (capacity > SIZE_MAX / 2)
      return -1;
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(*file->regions))
    return -1;

  struct verified_write_region *regions =
      realloc(file->regions, capacity * sizeof(*regions));
  if (!regions)
    return -1;
  file->regions = regions;
  file->region_capacity = capacity;
  return 0;
}

static int find_verified_overlap(const uint8_t *name, uint64_t start,
                                 uint64_t end, uint64_t *overlap_start,
                                 uint64_t *overlap_end) {
  if (!overlap_start || !overlap_end || start >= end)
    return -1;

  uint64_t name_hash = 0;
  struct verified_write_bucket *bucket =
      verified_bucket_for_name(name, &name_hash);
  if (!bucket || pthread_mutex_lock(&bucket->mutex) != 0)
    return -1;

  int found = 0;
  struct verified_file_regions *file =
      find_verified_file(bucket, name, name_hash);
  if (file) {
    size_t low = 0;
    size_t high = file->region_count;
    while (low < high) {
      size_t middle = low + (high - low) / 2;
      if (file->regions[middle].end <= start)
        low = middle + 1;
      else
        high = middle;
    }
    if (low < file->region_count && file->regions[low].start < end) {
      *overlap_start = file->regions[low].start;
      *overlap_end = file->regions[low].end;
      found = 1;
    }
  }
  return pthread_mutex_unlock(&bucket->mutex) == 0 ? found : -1;
}

static int mark_verified_region(const uint8_t *name, uint64_t start,
                                uint64_t end) {
  if (!name || start >= end)
    return name ? 0 : -1;

  uint64_t name_hash = 0;
  struct verified_write_bucket *bucket =
      verified_bucket_for_name(name, &name_hash);
  if (!bucket || pthread_mutex_lock(&bucket->mutex) != 0)
    return -1;

  struct verified_file_regions *file =
      find_verified_file(bucket, name, name_hash);
  if (!file) {
    file = calloc(1, sizeof(*file));
    if (!file)
      goto fail;
    file->name = copy_region_name(name);
    if (!file->name || reserve_verified_regions(file, 1) != 0) {
      free(file->name);
      free(file->regions);
      free(file);
      goto fail;
    }
    file->name_hash = name_hash;
    file->next = bucket->files;
    bucket->files = file;
  }

  size_t first = 0;
  size_t high = file->region_count;
  while (first < high) {
    size_t middle = first + (high - first) / 2;
    if (file->regions[middle].end < start)
      first = middle + 1;
    else
      high = middle;
  }

  uint64_t merged_start = start;
  uint64_t merged_end = end;
  size_t last = first;
  while (last < file->region_count &&
         file->regions[last].start <= merged_end) {
    if (file->regions[last].start < merged_start)
      merged_start = file->regions[last].start;
    if (file->regions[last].end > merged_end)
      merged_end = file->regions[last].end;
    last++;
  }

  if (last == first) {
    if (reserve_verified_regions(file, file->region_count + 1) != 0)
      goto fail;
    memmove(&file->regions[first + 1], &file->regions[first],
            (file->region_count - first) * sizeof(*file->regions));
    file->region_count++;
  } else if (last > first + 1) {
    memmove(&file->regions[first + 1], &file->regions[last],
            (file->region_count - last) * sizeof(*file->regions));
    file->region_count -= last - first - 1;
  }
  file->regions[first].start = merged_start;
  file->regions[first].end = merged_end;

  if (pthread_mutex_unlock(&bucket->mutex) != 0)
    return -1;
  return 0;

fail:
  pthread_mutex_unlock(&bucket->mutex);
  return -1;
}

static int clear_verified_regions_for_name(const uint8_t *name) {
  if (!name)
    return -1;

  uint64_t name_hash = 0;
  struct verified_write_bucket *bucket =
      verified_bucket_for_name(name, &name_hash);
  if (!bucket || pthread_mutex_lock(&bucket->mutex) != 0)
    return -1;

  struct verified_file_regions **file = &bucket->files;
  struct verified_file_regions *remove = NULL;
  while (*file) {
    if ((*file)->name_hash == name_hash &&
        strcmp((*file)->name, (const char *)name) == 0) {
      remove = *file;
      *file = remove->next;
      break;
    }
    file = &(*file)->next;
  }
  if (pthread_mutex_unlock(&bucket->mutex) != 0)
    return -1;

  if (remove) {
    free(remove->regions);
    free(remove->name);
    free(remove);
  }
  return 0;
}

static int seek_file_offset(FILE *file, uint64_t offset) {
  off_t file_offset = (off_t)offset;
  if (!file || file_offset < 0 || (uint64_t)file_offset != offset)
    return -1;
  return fseeko(file, file_offset, SEEK_SET) == 0 ? 0 : -1;
}

static int write_output_data(FILE *file, uint64_t offset, const uint8_t *data,
                             uint64_t size) {
  size_t write_size = (size_t)size;
  if (!data || (uint64_t)write_size != size ||
      seek_file_offset(file, offset) != 0)
    return -1;
  if (write_size == 0)
    return 0;
  return fwrite(data, 1, write_size, file) == write_size ? 0 : -1;
}

int read_device_extent_data(FILE *file, uint64_t offset, void *data,
                            uint64_t size) {
  size_t read_size = (size_t)size;
  if (!data || (uint64_t)read_size != size ||
      seek_file_offset(file, offset) != 0)
    return -1;
  if (read_size == 0)
    return 0;
  return fread(data, 1, read_size, file) == read_size ? 0 : -1;
}

static int write_tentative_extent_data(FILE *extract_file,
                                       const uint8_t *name,
                                       uint64_t offset_in_file,
                                       uint64_t file_size, const uint8_t *data,
                                       uint64_t size) {
  if (!extract_file || !data || !name)
    return -1;
  if (offset_in_file >= file_size || size == 0)
    return 0;
  if (file_size - offset_in_file < size)
    size = file_size - offset_in_file;

  uint64_t pos = 0;
  while (pos < size) {
    uint64_t start = offset_in_file + pos;
    uint64_t end = offset_in_file + size;
    uint64_t overlap_start = 0;
    uint64_t overlap_end = 0;

    int overlap = find_verified_overlap(name, start, end, &overlap_start,
                                        &overlap_end);
    if (overlap < 0)
      return -1;
    if (overlap == 0) {
      return write_output_data(extract_file, start, data + pos, end - start);
    }

    if (start < overlap_start) {
      uint64_t write_end = overlap_start < end ? overlap_start : end;
      if (write_output_data(extract_file, start, data + pos,
                            write_end - start) != 0)
        return -1;
      pos += write_end - start;
      continue;
    }

    if (overlap_end <= start)
      pos++;
    else
      pos += overlap_end - start;
  }
  return 0;
}

int extent_source_logical_address(
    const struct extent_data_reg_prealloc *reg_prealloc,
    uint64_t *logical_address) {
  if (!reg_prealloc || !logical_address ||
      reg_prealloc->logical_address == 0 ||
      reg_prealloc->logical_address == INVALID_OBJECTID ||
      reg_prealloc->num_bytes == 0 ||
      reg_prealloc->offset > reg_prealloc->size_extent ||
      reg_prealloc->num_bytes >
          reg_prealloc->size_extent - reg_prealloc->offset ||
      reg_prealloc->offset > UINT64_MAX - reg_prealloc->logical_address)
    return -1;

  uint64_t start = reg_prealloc->logical_address + reg_prealloc->offset;
  if (reg_prealloc->num_bytes > UINT64_MAX - start)
    return -1;

  *logical_address = start;
  return 0;
}

uint64_t extract_read_limit_for_file(uint64_t requested_size,
                                     uint64_t logical_address,
                                     uint64_t offset_in_file,
                                     uint64_t file_size, int verify_checksum) {
  if (requested_size == 0 || offset_in_file >= file_size)
    return 0;

  uint64_t valid_size = file_size - offset_in_file;
  if (valid_size > requested_size)
    valid_size = requested_size;

  if (verify_checksum != EXTRACT_WRITE_VERIFY ||
      logical_address == INVALID_OBJECTID)
    return valid_size;

  uint32_t sectorsize = current_fs_sectorsize();
  if (sectorsize == 0)
    return valid_size;

  if (UINT64_MAX - logical_address < valid_size)
    return valid_size;
  uint64_t logical_end = logical_address + valid_size;
  uint64_t sector_tail = logical_end % sectorsize;
  if (sector_tail == 0)
    return valid_size;

  uint64_t padding = sectorsize - sector_tail;
  if (UINT64_MAX - valid_size < padding)
    return valid_size;

  uint64_t padded_size = valid_size + padding;
  return padded_size < requested_size ? padded_size : requested_size;
}

int extent_sector_csum_status(uint64_t logical_address,
                              const uint8_t *data, uint64_t size) {
  uint32_t sectorsize = current_fs_sectorsize();
  uint16_t csum_type = current_fs_csum_type();
  if (!data || sectorsize == 0 || size != sectorsize || size > SIZE_MAX ||
      logical_address % sectorsize != 0)
    return -1;

  uint8_t actual[CHECKSUM_MAX_SIZE];
  if (checksum_compute(csum_type, data, (size_t)size, actual) != 0)
    return -1;
  return csum_range_index_match(logical_address, csum_type, actual);
}

int write_verified_extent_data(FILE *extract_file, uint64_t logical_address,
                               uint64_t offset_in_file, uint64_t file_size,
                               const void *data, uint64_t size, uint8_t *name,
                               int verify_checksum) {
  uint32_t sectorsize = current_fs_sectorsize();
  const uint8_t *bytes = data;
  if (!extract_file || !data)
    return -1;

  if (verify_checksum == EXTRACT_WRITE_DIRECT) {
    if (offset_in_file >= file_size || size == 0)
      return 0;
    return write_output_data(extract_file, offset_in_file, bytes,
                             MIN(size, file_size - offset_in_file));
  }
  if (verify_checksum != EXTRACT_WRITE_VERIFY || sectorsize == 0 ||
      logical_address == INVALID_OBJECTID) {
    return write_tentative_extent_data(extract_file, name, offset_in_file,
                                       file_size, bytes, size);
  }

  for (uint64_t pos = 0; pos < size;) {
    uint64_t logical = logical_address + pos;
    uint64_t file_offset = offset_in_file + pos;
    uint64_t sector_offset = logical % sectorsize;
    uint64_t chunk = MIN(size - pos, sectorsize - sector_offset);

    if (sector_offset == 0 && chunk == sectorsize) {
      uint8_t actual[CSUM_LOOKUP_BATCH_SECTORS * CHECKSUM_MAX_SIZE];
      int statuses[CSUM_LOOKUP_BATCH_SECTORS];
      uint64_t available_sector_count = (size - pos) / sectorsize;
      size_t sector_count = available_sector_count > CSUM_LOOKUP_BATCH_SECTORS
                                ? CSUM_LOOKUP_BATCH_SECTORS
                                : (size_t)available_sector_count;
      int batch_result = -1;
      uint16_t csum_type = current_fs_csum_type();
      size_t csum_size = checksum_size(csum_type);
      if (csum_size != 0) {
        size_t i;
        for (i = 0; i < sector_count; i++) {
          if (checksum_compute(csum_type,
                               bytes + pos + (uint64_t)i * sectorsize,
                               sectorsize, actual + i * csum_size) != 0)
            break;
        }
        if (i == sector_count)
          batch_result = csum_range_index_match_batch(
              logical, csum_type, actual, sector_count, statuses);
      }
      if (batch_result != 0) {
        for (size_t i = 0; i < sector_count; i++)
          statuses[i] = extent_sector_csum_status(
              logical + (uint64_t)i * sectorsize,
              bytes + pos + (uint64_t)i * sectorsize, sectorsize);
      }

      for (size_t i = 0; i < sector_count;) {
        uint64_t sector_pos = pos + (uint64_t)i * sectorsize;
        uint64_t sector_file_offset = offset_in_file + sector_pos;
        if (statuses[i] == 1) {
          size_t run_end = i + 1;
          while (run_end < sector_count && statuses[run_end] == 1)
            run_end++;

          uint64_t write_size = (uint64_t)(run_end - i) * sectorsize;
          if (sector_file_offset >= file_size)
            write_size = 0;
          else if (file_size - sector_file_offset < write_size)
            write_size = file_size - sector_file_offset;
          if (write_size > 0 &&
              (write_output_data(extract_file, sector_file_offset,
                                 bytes + sector_pos, write_size) != 0 ||
               mark_verified_region(name, sector_file_offset,
                                    sector_file_offset + write_size) != 0))
            return -1;
          i = run_end;
        } else {
          if (write_tentative_extent_data(
                  extract_file, name, sector_file_offset, file_size,
                  bytes + sector_pos, sectorsize) != 0)
            return -1;
          i++;
        }
      }
      pos += (uint64_t)sector_count * sectorsize;
      continue;
    } else {
      if (write_tentative_extent_data(extract_file, name, file_offset,
                                      file_size, bytes + pos, chunk) != 0)
        return -1;
    }

    pos += chunk;
  }
  return 0;
}

FILE **open_item_caches(void) {
  FILE **caches = calloc(ITEM_CACHE_COUNT, sizeof(FILE *));
  char *cache_path;
  for (int i = 0; i < ITEM_CACHE_COUNT; i++) {
    cache_path = item_cache_path(i);
    caches[i] = fopen(cache_path, "ab");
    if (caches[i] == NULL) {
      perror("fopen");
      free(caches[i]);
      caches[i] = NULL;
      return NULL;
    }
    free(cache_path);
  }
  return caches;
}

struct btrfs_item_cache *parse_item(char *buf, struct parse_btrfs_key *key,
                                    uint32_t size, FILE **item_caches) {
  struct btrfs_item_cache *item_cache =
      calloc(1, sizeof(struct btrfs_item_cache));
  item_cache->objectid = key->objectid;
  item_cache->offset = key->offset;
  item_cache->type = key->type;
  item_cache->size = size;
  switch (key->type) {
  case DEV_ITEM: {
    struct dev_item *dev_item = calloc(1, size);
    memcpy(dev_item, buf, size);
    item_cache->index = ftell(item_caches[0]);
    fwrite(dev_item, size, 1, item_caches[0]);
    free(dev_item);
    break;
  }
  case CHUNK_ITEM: {
    struct chunk_item *chunk_item = calloc(1, sizeof(struct chunk_item));
    struct stripe stripe_data;
    size_t pnt = sizeof(struct chunk_item);
    memcpy(chunk_item, buf, sizeof(struct chunk_item));
    item_cache->index = ftell(item_caches[1]);
    fwrite(chunk_item, sizeof(struct chunk_item), 1, item_caches[1]);
    for (int i = 0; i < chunk_item->num_stripes; i++) {
      memcpy(&stripe_data, buf + pnt, sizeof(struct stripe));
      pnt += sizeof(struct stripe) + 0x10; // 0x10 is the length of the UUID
      fwrite(&stripe_data, sizeof(struct stripe), 1, item_caches[1]);
    }
    free(chunk_item);
    break;
  }
  case INODE_ITEM: {
    struct inode_item *inode_item = calloc(1, size);
    memcpy(inode_item, buf, size);
    item_cache->index = ftell(item_caches[2]);
    fwrite(inode_item, size, 1, item_caches[2]);
    free(inode_item);
    break;
  }
  case INODE_REF: {
    struct inode_ref *inode_ref = calloc(1, size);
    memcpy(inode_ref, buf, size);
    item_cache->index = ftell(item_caches[3]);
    fwrite(inode_ref, size, 1, item_caches[3]);
    free(inode_ref);
    break;
  }
  case INODE_EXTREF: {
    struct inode_extref *inode_extref = calloc(1, size);
    memcpy(inode_extref, buf, size);
    item_cache->index = ftell(item_caches[4]);
    fwrite(inode_extref, size, 1, item_caches[4]);
    free(inode_extref);
    break;
  }
  case DIR_ITEM: {
    struct dir_item_index *dir_item = calloc(1, size);
    memcpy(dir_item, buf, size);
    item_cache->index = ftell(item_caches[5]);
    fwrite(dir_item, size, 1, item_caches[5]);
    free(dir_item);
    break;
  }
  case DIR_INDEX: {
    struct dir_item_index *dir_index = calloc(1, size);
    memcpy(dir_index, buf, size);
    item_cache->index = ftell(item_caches[6]);
    fwrite(dir_index, size, 1, item_caches[6]);
    free(dir_index);
    break;
  }
  case EXTENT_DATA: {
    if (size < sizeof(struct extent_data_item)) {
      free(item_cache);
      return NULL;
    }
    struct extent_data_item *extent_data_item = calloc(1, size);
    if (!extent_data_item) {
      perror("calloc");
      free(item_cache);
      return NULL;
    }
    memcpy(extent_data_item, buf, sizeof(struct extent_data_item));
    item_cache->index = ftell(item_caches[7]);
    fwrite(extent_data_item, sizeof(struct extent_data_item), 1,
           item_caches[7]);
    if (extent_data_item->extent_type == EXTENT_INLINE) {
      size_t inline_size = size - sizeof(struct extent_data_item);
      if (inline_size > 0) {
        uint8_t *data = calloc(1, inline_size);
        if (!data) {
          perror("calloc");
          free(extent_data_item);
          free(item_cache);
          return NULL;
        }
        memcpy(data, buf + sizeof(struct extent_data_item), inline_size);
        fwrite(data, inline_size, 1, item_caches[7]);
        free(data);
      }
    } else {
      if (size < sizeof(struct extent_data_item) +
                     sizeof(struct extent_data_reg_prealloc)) {
        free(extent_data_item);
        free(item_cache);
        return NULL;
      }
      struct extent_data_reg_prealloc *extent_data_reg_prealloc =
          calloc(1, sizeof(struct extent_data_reg_prealloc));
      if (!extent_data_reg_prealloc) {
        perror("calloc");
        free(extent_data_item);
        free(item_cache);
        return NULL;
      }
      memcpy(extent_data_reg_prealloc, buf + sizeof(struct extent_data_item),
             sizeof(struct extent_data_reg_prealloc));
      fwrite(extent_data_reg_prealloc, sizeof(struct extent_data_reg_prealloc),
             1, item_caches[7]);
      free(extent_data_reg_prealloc);
    }

    free(extent_data_item);
    break;
  }
  case ROOT_ITEM: {
    struct root_item *root_item = calloc(1, size);
    memcpy(root_item, buf, size);
    item_cache->index = ftell(item_caches[8]);
    fwrite(root_item, size, 1, item_caches[8]);
    free(root_item);
    break;
  }
  case EXTENT_ITEM: {
    struct extent_item *extent_item = calloc(1, size);
    memcpy(extent_item, buf, size);
    item_cache->index = ftell(item_caches[9]);
    fwrite(extent_item, size, 1, item_caches[9]);
    free(extent_item);
    break;
  }
  case METADATA_ITEM: {
    struct metadata_item *metadata_item = calloc(1, size);
    memcpy(metadata_item, buf, size);
    item_cache->index = ftell(item_caches[10]);
    fwrite(metadata_item, size, 1, item_caches[10]);
    free(metadata_item);
    break;
  }
  case BLOCK_GROUP_ITEM: {
    struct block_group_item *block_group_item = calloc(1, size);
    memcpy(block_group_item, buf, size);
    item_cache->index = ftell(item_caches[11]);
    fwrite(block_group_item, size, 1, item_caches[11]);
    free(block_group_item);
    break;
  }
  case EXTENT_CSUM: {
    struct csum_item_cache csum_item = {
        .logical_address = key->objectid == PROJECT_BTRFS_EXTENT_CSUM_OBJECTID
                               ? key->offset
                               : key->objectid,
        .size = size,
    };
    item_cache->index = ftell(item_caches[EXTENT_CSUM_INDEX]);
    fwrite(&csum_item, sizeof(csum_item), 1, item_caches[EXTENT_CSUM_INDEX]);
    fwrite(buf, size, 1, item_caches[EXTENT_CSUM_INDEX]);
    break;
  }
  default: {
    break;
  }
  }
  return item_cache;
}

int check_node(char *buf) {
  const char magic[] = {'_', 'B', 'H', 'R', 'f', 'S', '_', 'M'};
  if (memcmp(buf + 0x40, magic, 8) == 0)
    return 1;
  for (int i = 0; i < 0x4; i++)
    if (buf[i] != 0)
      return 0;
  return 1;
}

void parse_node(char *buf, FILE **item_caches, FILE *map_pnt) {
  if (check_node(buf)) {
    return;
  }
  struct parse_btrfs_header *header =
      calloc(1, sizeof(struct parse_btrfs_header));
  int idx = 0;
  if (!header) {
    perror("calloc");
    return;
  }
  memcpy(header, buf, sizeof(struct parse_btrfs_header));
  idx += 0x65;
  if (header->num_items >= 1000) {
    free(header);
    return;
  }
  if (header->level == 0) {
    struct btrfs_item_cache **item_cache_idxs =
        calloc(header->num_items, sizeof(struct btrfs_item_cache *));
    if (!item_cache_idxs) {
      perror("calloc");
      free(header);
      return;
    }
    struct parse_btrfs_key *key = calloc(1, sizeof(struct parse_btrfs_key));
    struct parse_btrfs_item *item = calloc(1, sizeof(struct parse_btrfs_item));
    struct btrfs_leaf_node_cache *leaf_node_cache =
        calloc(1, sizeof(struct btrfs_leaf_node_cache));
    int cache_type;
    if (header->owner == 0x03) {
      cache_type = TOTAL_CHUNK_TREE_CACHE;
    } else {
      cache_type = LEAF_NODE_CACHE;
    }
    char *leaf_node_cache_path = cache_path(cache_type);
    FILE *leaf_node_cache_file = fopen(leaf_node_cache_path, "ab");
    free(leaf_node_cache_path);
    if (!key || !item || !leaf_node_cache || !leaf_node_cache_file) {
      perror("calloc");
      free(header);
      free(key);
      free(item);
      free(leaf_node_cache);
      free(item_cache_idxs);
      if (leaf_node_cache_file)
        fclose(leaf_node_cache_file);
      return;
    }
    for (uint32_t i = 0; i < header->num_items; i++) {
      memcpy(key, buf + idx, sizeof(struct parse_btrfs_key));
      idx += sizeof(struct parse_btrfs_key);
      memcpy(item, buf + idx, sizeof(struct parse_btrfs_item));
      idx += sizeof(struct parse_btrfs_item);
      item_cache_idxs[i] =
          parse_item(buf + 0x65 + item->offset, key, item->size, item_caches);
    }
    struct address_map_cache address_map_cache = {
        .logical_address = header->logical_address_of_this_node,
        .cache_type = cache_type,
        .index = ftell(leaf_node_cache_file),
        .generation = header->generation,
        .owner = header->owner,
    };
    fwrite(&address_map_cache, sizeof(struct address_map_cache), 1, map_pnt);
    fwrite(header, sizeof(struct parse_btrfs_header), 1, leaf_node_cache_file);
    for (uint32_t i = 0; i < header->num_items; i++) {
      struct btrfs_item_cache empty_item = {0};
      fwrite(item_cache_idxs[i] ? item_cache_idxs[i] : &empty_item,
             sizeof(struct btrfs_item_cache), 1, leaf_node_cache_file);
      free(item_cache_idxs[i]);
    }
    fclose(leaf_node_cache_file);
    free(item_cache_idxs);
    free(key);
    free(item);
    free(leaf_node_cache);
  } else {
    char *internal_node_cache_path = cache_path(INTERNAL_NODE_CACHE);
    FILE *internal_node_cache_file = fopen(internal_node_cache_path, "ab");
    free(internal_node_cache_path);
    if (!internal_node_cache_file) {
      perror("fopen");
      free(header);
      return;
    }
    struct block_ptr **block_ptr =
        calloc(header->num_items, sizeof(struct block_ptr *));
    if (!block_ptr) {
      perror("calloc");
      fclose(internal_node_cache_file);
      free(header);
      return;
    }
    for (uint32_t i = 0; i < header->num_items; i++) {
      block_ptr[i] = calloc(1, sizeof(struct block_ptr));
      if (!block_ptr[i]) {
        perror("calloc");
        for (uint32_t j = 0; j < i; j++)
          free(block_ptr[j]);
        free(block_ptr);
        fclose(internal_node_cache_file);
        free(header);
        return;
      }
      memcpy(block_ptr[i], buf + idx, sizeof(struct block_ptr));
      idx += sizeof(struct block_ptr);
    }
    struct address_map_cache address_map_cache = {
        .logical_address = header->logical_address_of_this_node,
        .cache_type = INTERNAL_NODE_CACHE,
        .index = ftell(internal_node_cache_file),
        .generation = header->generation,
        .owner = header->owner,
    };
    fwrite(&address_map_cache, sizeof(struct address_map_cache), 1, map_pnt);
    fwrite(header, sizeof(struct parse_btrfs_header), 1,
           internal_node_cache_file);
    for (uint32_t i = 0; i < header->num_items; i++) {
      fwrite(block_ptr[i], sizeof(struct block_ptr), 1,
             internal_node_cache_file);
    }
    fclose(internal_node_cache_file);
    for (uint32_t i = 0; i < header->num_items; i++) {
      free(block_ptr[i]);
    }
    free(block_ptr);
  }
  free(header);
}

void extract_raid(uint64_t logical_address_start, uint64_t decoded_size,
                  uint64_t offset_in_file, uint64_t file_size,
                  struct extent_data_reg_prealloc reg_prealloc,
                  struct chunk_item chunk_item, struct stripe *stripe_data,
                  uint8_t *name) {
  int ret = check_chunk_raid_type(chunk_item.type);
  switch (ret) {
  case SINGLE_CHUNK_TYPE: {
    extract_single(logical_address_start, decoded_size, offset_in_file,
                   file_size, reg_prealloc, stripe_data, name, 0);
    break;
  }
  case DUP_CHUNK_TYPE: {
    extract_dup(logical_address_start, decoded_size, offset_in_file, file_size,
                reg_prealloc, chunk_item, stripe_data, name, 0);
    break;
  }
  case RAID0_CHUNK_TYPE: {
    extract_raid0(logical_address_start, decoded_size, offset_in_file,
                  file_size, reg_prealloc, chunk_item, stripe_data, name, 0);
    break;
  }
  case RAID1_CHUNK_TYPE: {
    extract_raid1(logical_address_start, decoded_size, offset_in_file,
                  file_size, reg_prealloc, chunk_item, stripe_data, name, 0);
    break;
  }
  case RAID1C3_CHUNK_TYPE: {
    extract_raid1(logical_address_start, decoded_size, offset_in_file,
                  file_size, reg_prealloc, chunk_item, stripe_data, name, 0);
    break;
  }
  case RAID1C4_CHUNK_TYPE: {
    extract_raid1(logical_address_start, decoded_size, offset_in_file,
                  file_size, reg_prealloc, chunk_item, stripe_data, name, 0);
    break;
  }
  case RAID10_CHUNK_TYPE: {
    extract_raid10(logical_address_start, decoded_size, offset_in_file,
                   file_size, reg_prealloc, chunk_item, stripe_data, name, 0);
    break;
  }
  case RAID5_CHUNK_TYPE: {
    printf("RAID5_NOT_SUPPORTED\n");
    exit(1);
    break;
  }
  case RAID6_CHUNK_TYPE: {
    printf("RAID6_NOT_SUPPORTED\n");
    exit(1);
    break;
  }
  default: {
    break;
  }
  }
}

static int
load_current_chunk_mapping_for_logical(uint64_t logical_address,
                                       struct parse_btrfs_chunk *chunk,
                                       struct stripe **stripe_data) {
  char *chunk_path = chunk_tree_cache_path(CHUNK_TREE_CACHE);
  char *stripe_path = chunk_tree_cache_path(CHUNK_TREE_STRIPE_CACHE);
  FILE *chunk_file = chunk_path ? fopen(chunk_path, "rb") : NULL;
  FILE *stripe_file = stripe_path ? fopen(stripe_path, "rb") : NULL;
  free(chunk_path);
  free(stripe_path);

  if (!chunk_file || !stripe_file) {
    perror("fopen");
    if (chunk_file)
      fclose(chunk_file);
    if (stripe_file)
      fclose(stripe_file);
    return -1;
  }

  int found = 0;
  while (fread(chunk, sizeof(*chunk), 1, chunk_file) == 1) {
    uint64_t chunk_end = chunk->logical_address + chunk->length;
    if (chunk_end < chunk->logical_address)
      chunk_end = UINT64_MAX;
    if (logical_address >= chunk->logical_address &&
        logical_address < chunk_end) {
      found = 1;
      break;
    }
  }

  if (!found) {
    fclose(chunk_file);
    fclose(stripe_file);
    return -1;
  }

  struct stripe *stripes = calloc(chunk->num_stripes, sizeof(*stripes));
  if (!stripes) {
    perror("calloc");
    fclose(chunk_file);
    fclose(stripe_file);
    return -1;
  }

  if (fseek(stripe_file, (long)(chunk->stripe_index * sizeof(struct stripe)),
            SEEK_SET) != 0) {
    perror("fseek");
    free(stripes);
    fclose(chunk_file);
    fclose(stripe_file);
    return -1;
  }

  for (uint16_t i = 0; i < chunk->num_stripes; i++) {
    if (fread(&stripes[i], sizeof(*stripes), 1, stripe_file) != 1) {
      free(stripes);
      fclose(chunk_file);
      fclose(stripe_file);
      return -1;
    }
  }

  fclose(chunk_file);
  fclose(stripe_file);
  *stripe_data = stripes;
  return 0;
}

static int mapping_generation_is_allowed(uint32_t generation) {
  uint64_t limit = current_fs_generation_limit();
  return limit == 0 || generation <= limit;
}

static uint64_t mapping_generation_score(uint32_t mapping_generation,
                                         uint64_t extent_generation) {
  if (extent_generation == 0)
    return mapping_generation;
  if (mapping_generation >= extent_generation)
    return mapping_generation - extent_generation;
  return extent_generation - mapping_generation + (UINT64_MAX / 2);
}

static int read_historical_chunk_item(FILE *chunk_item_file,
                                      const struct btrfs_item_cache *item,
                                      struct parse_btrfs_chunk *chunk,
                                      struct stripe **stripe_data) {
  struct chunk_item chunk_item;

  if (fseek(chunk_item_file, item->index, SEEK_SET) != 0 ||
      fread(&chunk_item, sizeof(chunk_item), 1, chunk_item_file) != 1)
    return -1;

  struct stripe *stripes =
      calloc(chunk_item.num_stripes, sizeof(struct stripe));
  if (!stripes) {
    perror("calloc");
    return -1;
  }

  for (uint16_t i = 0; i < chunk_item.num_stripes; i++) {
    if (fread(&stripes[i], sizeof(struct stripe), 1, chunk_item_file) != 1) {
      free(stripes);
      return -1;
    }
  }

  *chunk = (struct parse_btrfs_chunk){
      .logical_address = item->offset,
      .length = chunk_item.length,
      .raid_type = check_chunk_raid_type(chunk_item.type),
      .num_stripes = chunk_item.num_stripes,
      .sub_stripes = chunk_item.sub_stripes,
      .stripe_length = chunk_item.stripe_len,
  };
  *stripe_data = stripes;
  return 0;
}

struct historical_chunk_mapping_entry {
  struct parse_btrfs_chunk chunk;
  struct stripe *stripes;
  uint64_t logical_end;
  uint64_t prefix_max_end;
  uint64_t source_order;
  uint32_t generation;
  uint8_t fs_uuid[BTRFS_UUID_SIZE];
};

struct historical_chunk_mapping_index {
  struct historical_chunk_mapping_entry *entries;
  size_t count;
  size_t capacity;
  char *cache_base;
  int initialized;
  int build_status;
};

static struct historical_chunk_mapping_index historical_chunk_index;
static pthread_rwlock_t historical_chunk_index_lock =
    PTHREAD_RWLOCK_INITIALIZER;

static void free_historical_chunk_mapping_index(
    struct historical_chunk_mapping_index *index) {
  if (!index)
    return;
  for (size_t i = 0; i < index->count; i++)
    free(index->entries[i].stripes);
  free(index->entries);
  free(index->cache_base);
  memset(index, 0, sizeof(*index));
}

static int append_historical_chunk_mapping(
    struct historical_chunk_mapping_index *index,
    const struct historical_chunk_mapping_entry *entry) {
  if (index->count == index->capacity) {
    size_t capacity = index->capacity ? index->capacity * 2 : 64;
    if (capacity < index->capacity ||
        capacity > SIZE_MAX / sizeof(*index->entries))
      return -1;
    void *entries = realloc(index->entries,
                            capacity * sizeof(*index->entries));
    if (!entries)
      return -1;
    index->entries = entries;
    index->capacity = capacity;
  }
  index->entries[index->count++] = *entry;
  return 0;
}

static int historical_chunk_mapping_compare(const void *lhs_ptr,
                                            const void *rhs_ptr) {
  const struct historical_chunk_mapping_entry *lhs = lhs_ptr;
  const struct historical_chunk_mapping_entry *rhs = rhs_ptr;
  if (lhs->chunk.logical_address < rhs->chunk.logical_address)
    return -1;
  if (lhs->chunk.logical_address > rhs->chunk.logical_address)
    return 1;
  if (lhs->source_order < rhs->source_order)
    return -1;
  if (lhs->source_order > rhs->source_order)
    return 1;
  return 0;
}

static int build_historical_chunk_mapping_index(
    const char *cache_base, struct historical_chunk_mapping_index *built) {
  char *map_path = NULL;
  char *chunk_leaf_path = NULL;
  char *chunk_item_path = NULL;
  FILE *map_file = NULL;
  FILE *chunk_leaf_file = NULL;
  FILE *chunk_item_file = NULL;
  int result = -1;

  memset(built, 0, sizeof(*built));
  built->cache_base = copy_region_name((const uint8_t *)cache_base);
  if (!built->cache_base)
    goto out;

  map_path = cache_path(MAP_CACHE);
  chunk_leaf_path = chunk_tree_cache_path(TOTAL_CHUNK_TREE_CACHE);
  chunk_item_path = item_cache_path(CHUNK_ITEM_INDEX);
  if (!map_path || !chunk_leaf_path || !chunk_item_path)
    goto out;

  map_file = fopen(map_path, "rb");
  chunk_leaf_file = fopen(chunk_leaf_path, "rb");
  chunk_item_file = fopen(chunk_item_path, "rb");
  if (!map_file || !chunk_leaf_file || !chunk_item_file)
    goto out;

  struct address_map_cache rec;
  uint64_t source_order = 0;
  while (fread(&rec, sizeof(rec), 1, map_file) == 1) {
    if (rec.owner != 0x3 || rec.cache_type != TOTAL_CHUNK_TREE_CACHE)
      continue;
    if (fseek(chunk_leaf_file, rec.index, SEEK_SET) != 0)
      continue;

    struct parse_btrfs_header header;
    if (fread(&header, sizeof(header), 1, chunk_leaf_file) != 1 ||
        header.level != 0)
      continue;

    for (uint32_t i = 0; i < header.num_items; i++) {
      struct btrfs_item_cache item;
      if (fread(&item, sizeof(item), 1, chunk_leaf_file) != 1)
        break;
      if (item.type != CHUNK_ITEM)
        continue;

      struct historical_chunk_mapping_entry entry = {0};
      if (read_historical_chunk_item(chunk_item_file, &item, &entry.chunk,
                                     &entry.stripes) != 0)
        continue;
      entry.logical_end = entry.chunk.logical_address + entry.chunk.length;
      if (entry.logical_end < entry.chunk.logical_address)
        entry.logical_end = UINT64_MAX;
      entry.source_order = source_order++;
      entry.generation = rec.generation;
      memcpy(entry.fs_uuid, header.fs_uuid, sizeof(entry.fs_uuid));
      if (append_historical_chunk_mapping(built, &entry) != 0) {
        free(entry.stripes);
        goto out;
      }
    }
  }

  if (built->count > 1)
    qsort(built->entries, built->count, sizeof(*built->entries),
          historical_chunk_mapping_compare);
  uint64_t prefix_max_end = 0;
  for (size_t i = 0; i < built->count; i++) {
    if (built->entries[i].logical_end > prefix_max_end)
      prefix_max_end = built->entries[i].logical_end;
    built->entries[i].prefix_max_end = prefix_max_end;
  }
  result = 0;

out:
  if (map_file)
    fclose(map_file);
  if (chunk_leaf_file)
    fclose(chunk_leaf_file);
  if (chunk_item_file)
    fclose(chunk_item_file);
  free(chunk_item_path);
  free(chunk_leaf_path);
  free(map_path);
  if (result != 0)
    free_historical_chunk_mapping_index(built);
  return result;
}

static int ensure_historical_chunk_mapping_index(void) {
  const char *cache_base = current_cache_base_path();
  if (!cache_base)
    return -1;

  if (pthread_rwlock_rdlock(&historical_chunk_index_lock) != 0)
    return -1;
  int current = historical_chunk_index.initialized &&
                historical_chunk_index.cache_base &&
                strcmp(historical_chunk_index.cache_base, cache_base) == 0;
  int status = historical_chunk_index.build_status;
  pthread_rwlock_unlock(&historical_chunk_index_lock);
  if (current)
    return status;

  if (pthread_rwlock_wrlock(&historical_chunk_index_lock) != 0)
    return -1;
  current = historical_chunk_index.initialized &&
            historical_chunk_index.cache_base &&
            strcmp(historical_chunk_index.cache_base, cache_base) == 0;
  if (!current) {
    struct historical_chunk_mapping_index built;
    int build_status =
        build_historical_chunk_mapping_index(cache_base, &built);
    free_historical_chunk_mapping_index(&historical_chunk_index);
    if (build_status == 0) {
      historical_chunk_index = built;
    } else {
      memset(&historical_chunk_index, 0, sizeof(historical_chunk_index));
      historical_chunk_index.cache_base =
          copy_region_name((const uint8_t *)cache_base);
    }
    historical_chunk_index.initialized = 1;
    historical_chunk_index.build_status = build_status;
  }
  status = historical_chunk_index.build_status;
  pthread_rwlock_unlock(&historical_chunk_index_lock);
  return status;
}

static int load_historical_chunk_mapping_by_scan(
    uint64_t logical_address, uint64_t extent_generation,
    struct parse_btrfs_chunk *chunk, struct stripe **stripe_data) {
  char *map_path = cache_path(MAP_CACHE);
  char *chunk_leaf_path = chunk_tree_cache_path(TOTAL_CHUNK_TREE_CACHE);
  char *chunk_item_path = item_cache_path(CHUNK_ITEM_INDEX);
  FILE *map_file = map_path ? fopen(map_path, "rb") : NULL;
  FILE *chunk_leaf_file = chunk_leaf_path ? fopen(chunk_leaf_path, "rb") : NULL;
  FILE *chunk_item_file = chunk_item_path ? fopen(chunk_item_path, "rb") : NULL;
  free(map_path);
  free(chunk_leaf_path);
  free(chunk_item_path);

  if (!map_file || !chunk_leaf_file || !chunk_item_file) {
    if (map_file)
      fclose(map_file);
    if (chunk_leaf_file)
      fclose(chunk_leaf_file);
    if (chunk_item_file)
      fclose(chunk_item_file);
    return -1;
  }

  int found = 0;
  uint64_t best_score = UINT64_MAX;
  struct parse_btrfs_chunk best_chunk;
  struct stripe *best_stripes = NULL;
  struct address_map_cache rec;

  while (fread(&rec, sizeof(rec), 1, map_file) == 1) {
    if (rec.owner != 0x3 || rec.cache_type != TOTAL_CHUNK_TREE_CACHE)
      continue;
    if (!mapping_generation_is_allowed(rec.generation))
      continue;

    if (fseek(chunk_leaf_file, rec.index, SEEK_SET) != 0)
      continue;

    struct parse_btrfs_header header;
    if (fread(&header, sizeof(header), 1, chunk_leaf_file) != 1)
      continue;
    if (header.level != 0 || !should_parse_fs_uuid(header.fs_uuid))
      continue;

    for (uint32_t i = 0; i < header.num_items; i++) {
      struct btrfs_item_cache item;
      if (fread(&item, sizeof(item), 1, chunk_leaf_file) != 1)
        break;
      if (item.type != CHUNK_ITEM)
        continue;

      struct parse_btrfs_chunk candidate;
      struct stripe *candidate_stripes = NULL;
      if (read_historical_chunk_item(chunk_item_file, &item, &candidate,
                                     &candidate_stripes) != 0)
        continue;

      uint64_t chunk_end = candidate.logical_address + candidate.length;
      if (chunk_end < candidate.logical_address)
        chunk_end = UINT64_MAX;
      if (logical_address < candidate.logical_address ||
          logical_address >= chunk_end) {
        free(candidate_stripes);
        continue;
      }

      uint64_t score =
          mapping_generation_score(rec.generation, extent_generation);
      if (!found || score < best_score) {
        free(best_stripes);
        best_chunk = candidate;
        best_stripes = candidate_stripes;
        best_score = score;
        found = 1;
      } else {
        free(candidate_stripes);
      }
    }
  }

  fclose(map_file);
  fclose(chunk_leaf_file);
  fclose(chunk_item_file);

  if (!found)
    return -1;

  *chunk = best_chunk;
  *stripe_data = best_stripes;
  return 0;
}

static int load_historical_chunk_mapping_for_logical(
    uint64_t logical_address, uint64_t extent_generation,
    struct parse_btrfs_chunk *chunk, struct stripe **stripe_data) {
  if (ensure_historical_chunk_mapping_index() != 0)
    return load_historical_chunk_mapping_by_scan(
        logical_address, extent_generation, chunk, stripe_data);
  if (pthread_rwlock_rdlock(&historical_chunk_index_lock) != 0)
    return load_historical_chunk_mapping_by_scan(
        logical_address, extent_generation, chunk, stripe_data);

  size_t low = 0;
  size_t high = historical_chunk_index.count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (historical_chunk_index.entries[middle].chunk.logical_address <=
        logical_address)
      low = middle + 1;
    else
      high = middle;
  }

  const struct historical_chunk_mapping_entry *best = NULL;
  uint64_t best_score = UINT64_MAX;
  while (low > 0) {
    size_t index = --low;
    const struct historical_chunk_mapping_entry *entry =
        &historical_chunk_index.entries[index];
    if (entry->prefix_max_end <= logical_address)
      break;
    if (logical_address < entry->chunk.logical_address ||
        logical_address >= entry->logical_end ||
        !mapping_generation_is_allowed(entry->generation) ||
        !should_parse_fs_uuid(entry->fs_uuid))
      continue;

    uint64_t score =
        mapping_generation_score(entry->generation, extent_generation);
    if (!best || score < best_score ||
        (score == best_score && entry->source_order < best->source_order)) {
      best = entry;
      best_score = score;
    }
  }

  int result = -1;
  int copy_failed = 0;
  if (best) {
    size_t stripe_bytes =
        (size_t)best->chunk.num_stripes * sizeof(struct stripe);
    struct stripe *stripes =
        calloc(best->chunk.num_stripes, sizeof(struct stripe));
    if (stripes) {
      memcpy(stripes, best->stripes, stripe_bytes);
      *chunk = best->chunk;
      *stripe_data = stripes;
      result = 0;
    } else
      copy_failed = 1;
  }
  pthread_rwlock_unlock(&historical_chunk_index_lock);
  if (copy_failed)
    return load_historical_chunk_mapping_by_scan(
        logical_address, extent_generation, chunk, stripe_data);
  return result;
}

static int load_chunk_mapping_for_logical(uint64_t logical_address,
                                          uint64_t extent_generation,
                                          struct parse_btrfs_chunk *chunk,
                                          struct stripe **stripe_data) {
  if (load_historical_chunk_mapping_for_logical(
          logical_address, extent_generation, chunk, stripe_data) == 0)
    return 0;

  return load_current_chunk_mapping_for_logical(logical_address, chunk,
                                                stripe_data);
}

static struct chunk_item
chunk_item_from_mapping(const struct parse_btrfs_chunk *chunk) {
  return (struct chunk_item){
      .length = chunk->length,
      .stripe_len = chunk->stripe_length,
      .num_stripes = chunk->num_stripes,
      .sub_stripes = chunk->sub_stripes,
  };
}

static int extract_raid_from_mapping(const struct parse_btrfs_chunk *chunk,
                                     uint64_t decoded_size,
                                     uint64_t offset_in_file,
                                     uint64_t file_size,
                                     struct extent_data_reg_prealloc reg,
                                     struct stripe *stripe_data, uint8_t *name,
                                     int verify_checksum) {
  struct chunk_item chunk_item = chunk_item_from_mapping(chunk);

  switch (chunk->raid_type) {
  case SINGLE_CHUNK_TYPE:
    return extract_single(chunk->logical_address, decoded_size, offset_in_file,
                          file_size, reg, stripe_data, name, verify_checksum);
  case DUP_CHUNK_TYPE:
    return extract_dup(chunk->logical_address, decoded_size, offset_in_file,
                       file_size, reg, chunk_item, stripe_data, name,
                       verify_checksum);
  case RAID0_CHUNK_TYPE:
    return extract_raid0(chunk->logical_address, decoded_size, offset_in_file,
                         file_size, reg, chunk_item, stripe_data, name,
                         verify_checksum);
  case RAID1_CHUNK_TYPE:
  case RAID1C3_CHUNK_TYPE:
  case RAID1C4_CHUNK_TYPE:
    return extract_raid1(chunk->logical_address, decoded_size, offset_in_file,
                         file_size, reg, chunk_item, stripe_data, name,
                         verify_checksum);
  case RAID10_CHUNK_TYPE:
    return extract_raid10(chunk->logical_address, decoded_size, offset_in_file,
                          file_size, reg, chunk_item, stripe_data, name,
                          verify_checksum);
  case RAID5_CHUNK_TYPE:
  case RAID6_CHUNK_TYPE:
    fprintf(stderr, "RAID5/6 extraction is not supported\n");
    return -1;
  default:
    fprintf(stderr, "Unknown chunk RAID type: %" PRIu32 "\n", chunk->raid_type);
    return -1;
  }
}

/* Compressed extents must be assembled and validated before any decoded bytes
 * reach the output. An extent offset addresses the decoded stream, never the
 * compressed device bytes. */
static int compressed_extent_valid(const struct extent_data_item *item,
                                    const struct extent_data_reg_prealloc *reg) {
  uint32_t sector = current_fs_sectorsize();
  return item && reg && item->extent_type == EXTENT_REG &&
         item->compression >= 1 && item->compression <= 3 &&
         item->decoded_size &&
         item->decoded_size <= RECOVERY_MAX_COMPRESSED_EXTENT &&
         reg->size_extent && reg->size_extent <= RECOVERY_MAX_COMPRESSED_EXTENT &&
         sector >= 4096 && sector <= 65536 && !(sector & (sector - 1)) &&
         reg->logical_address && reg->logical_address != INVALID_OBJECTID &&
         reg->logical_address % sector == 0 && reg->size_extent % sector == 0 &&
         reg->size_extent <= UINT64_MAX - reg->logical_address &&
         reg->offset <= item->decoded_size && reg->num_bytes &&
         reg->num_bytes <= item->decoded_size - reg->offset;
}

static int compressed_mapping_geometry(const struct parse_btrfs_chunk *chunk,
                                        uint16_t *mirrors, uint16_t *groups) {
  if (!chunk || !chunk->num_stripes) return -1;
  *mirrors = 1;
  *groups = 1;
  switch (chunk->raid_type) {
  case SINGLE_CHUNK_TYPE:
    return chunk->num_stripes == 1 ? 0 : -1;
  case DUP_CHUNK_TYPE:
  case RAID1_CHUNK_TYPE:
  case RAID1C3_CHUNK_TYPE:
  case RAID1C4_CHUNK_TYPE:
    *mirrors = chunk->num_stripes;
    return 0;
  case RAID0_CHUNK_TYPE:
    *groups = chunk->num_stripes;
    break;
  case RAID10_CHUNK_TYPE:
    if (!chunk->sub_stripes || chunk->num_stripes % chunk->sub_stripes)
      return -1;
    *mirrors = chunk->sub_stripes;
    *groups = chunk->num_stripes / chunk->sub_stripes;
    break;
  default:
    return -1;
  }
  uint32_t sector = current_fs_sectorsize();
  return sector && chunk->stripe_length && chunk->stripe_length % sector == 0
             ? 0 : -1;
}

#define COMPRESSED_MAX_SECTORS (RECOVERY_MAX_COMPRESSED_EXTENT / 4096U)

static void compressed_checksum_statuses(uint64_t logical, const uint8_t *data,
                                          size_t count, int *statuses) {
  uint32_t sector = current_fs_sectorsize();
  uint16_t type = current_fs_csum_type();
  size_t digest_size = checksum_size(type);
  uint8_t actual[COMPRESSED_MAX_SECTORS * CHECKSUM_MAX_SIZE];
  int hashed = digest_size != 0;
  for (size_t i = 0; hashed && i < count; i++) {
    if (checksum_compute(type, data + i * sector, sector,
                         actual + i * digest_size) != 0)
      hashed = 0;
  }
  if (hashed && csum_range_index_match_batch(logical, type, actual, count,
                                             statuses) == 0)
    return;
  /* A failed batch lookup may still have readable individual checksums. */
  for (size_t i = 0; i < count; i++)
    statuses[i] = extent_sector_csum_status(logical + i * sector,
                                            data + i * sector, sector);
}

static void select_compressed_sector(uint8_t *output, const uint8_t *candidate,
                                      size_t size, int status, int *selected) {
  /* Prefer a verified mirror; otherwise retain the first readable unknown
   * candidate. Known mismatches never enter the decompressor. */
  if (*selected != 1 && (status == 1 || (status == -1 && *selected == 0))) {
    memcpy(output, candidate, size);
    *selected = status;
  }
}

static int read_compressed_extent(const struct parse_btrfs_chunk *chunk,
                                   const struct stripe *stripes,
                                   FILE **devices, uint64_t logical,
                                   uint8_t *data, size_t size,
                                   uint16_t preferred_mirror, int *verified,
                                   const struct extraction_options *options) {
  uint16_t mirrors, groups;
  uint32_t sector = current_fs_sectorsize();
  if (compressed_mapping_geometry(chunk, &mirrors, &groups) ||
      sector < 4096 || sector > 65536 || (sector & (sector - 1)) ||
      !size || size > RECOVERY_MAX_COMPRESSED_EXTENT || size % sector ||
      logical % sector || chunk->logical_address % sector ||
      logical < chunk->logical_address ||
      logical - chunk->logical_address > chunk->length ||
      size > chunk->length - (logical - chunk->logical_address))
    return -1;
  uint8_t *candidate = malloc(size);
  if (!candidate) return -1;
  *verified = !options->no_data_checksum;
  int result = -1;
  for (size_t done = 0; done < size;) {
    uint64_t relative = logical - chunk->logical_address + done;
    uint64_t physical = relative;
    uint16_t group = 0;
    size_t length = size - done;
    if (chunk->raid_type == RAID0_CHUNK_TYPE || chunk->raid_type == RAID10_CHUNK_TYPE) {
      uint64_t stripe = relative / chunk->stripe_length;
      group = stripe % groups;
      physical = (stripe / groups) * chunk->stripe_length + relative % chunk->stripe_length;
      uint64_t stripe_left = chunk->stripe_length - relative % chunk->stripe_length;
      if (length > stripe_left) length = stripe_left;
    }
    size_t count = length / sector;
    int selected[COMPRESSED_MAX_SECTORS] = {0};
    size_t matched = 0;
    for (uint16_t m = 0; m < mirrors && matched < count; m++) {
      uint16_t mirror = ((uint32_t)preferred_mirror + m) % mirrors;
      uint16_t index = group * mirrors + mirror;
      if (!devices[index] || physical > UINT64_MAX - stripes[index].physical_address)
        continue;
      uint64_t device_offset = stripes[index].physical_address + physical;
      if (length > UINT64_MAX - device_offset) continue;
      clearerr(devices[index]);
      if (read_device_extent_data(devices[index], device_offset, candidate, length) == 0) {
        int statuses[COMPRESSED_MAX_SECTORS];
        if (options->no_data_checksum) {
          /* Finalize the first readable copy without claiming verification. */
          for (size_t i = 0; i < count; i++) statuses[i] = 1;
        } else {
          compressed_checksum_statuses(logical + done, candidate, count, statuses);
        }
        for (size_t i = 0; i < count; i++) {
          if (selected[i] != 1 && statuses[i] == 1) matched++;
          select_compressed_sector(data + done + i * sector, candidate + i * sector,
                                     sector, statuses[i], &selected[i]);
        }
      } else {
        /* A bad sector must not discard its mirror's other readable sectors. */
        for (size_t i = 0; i < count; i++) {
          if (selected[i] == 1) continue;
          clearerr(devices[index]);
          if (read_device_extent_data(devices[index], device_offset + i * sector,
                                      candidate, sector) != 0)
            continue;
          int status = options->no_data_checksum ? 1 :
              extent_sector_csum_status(logical + done + i * sector,
                                         candidate, sector);
          if (status == 1) matched++;
          select_compressed_sector(data + done + i * sector, candidate, sector,
                                     status, &selected[i]);
        }
      }
    }
    for (size_t i = 0; i < count; i++) {
      if (selected[i] == 0) goto out;
      if (selected[i] != 1) *verified = 0;
    }
    done += length;
  }
  result = 0;
out:
  free(candidate);
  return result;
}

static int write_decoded_extent(const uint8_t *name, uint64_t offset,
                                 uint64_t file_size, const uint8_t *data,
                                 uint64_t size, int verified,
                                 const struct extraction_options *options) {
  if (offset >= file_size || !size) return 0;
  if (size > file_size - offset) size = file_size - offset;
  FILE *file = open_extract_file(name, "r+b");
  if (!file) return -1;
  int result;
  if (options->no_data_checksum) {
    result = write_output_data(file, offset, data, size);
  } else if (verified) {
    result = write_output_data(file, offset, data, size);
    if (!result) result = mark_verified_region(name, offset, offset + size);
  } else {
    result = write_tentative_extent_data(file, name, offset, file_size, data, size);
  }
  if (fclose(file)) result = -1;
  return result;
}

static int extract_compressed_extent(const struct extent_data_item *item,
                                      const struct extent_data_reg_prealloc *reg,
                                      const struct parse_btrfs_chunk *chunk,
                                      const struct stripe *stripes,
                                      uint64_t offset, uint64_t file_size,
                                      const uint8_t *name,
                                      const struct extraction_options *options) {
  uint16_t mirrors, groups;
  if (!compressed_extent_valid(item, reg) ||
      compressed_mapping_geometry(chunk, &mirrors, &groups)) return -1;
  uint8_t *encoded = malloc(reg->size_extent);
  uint8_t *decoded = malloc(item->decoded_size);
  FILE **devices = calloc(chunk->num_stripes, sizeof(*devices));
  int result = -1;
  if (!encoded || !decoded || !devices) goto out;
  for (uint16_t i = 0; i < chunk->num_stripes; i++)
    devices[i] = open_cached_device(stripes[i].devid);
  /* Without checksums, retry other mirrors after a decoder failure. */
  for (uint16_t mirror = 0; mirror < mirrors; mirror++) {
    int verified = 0;
    size_t decoded_size = 0;
    uint64_t needed = offset < file_size ? MIN(reg->num_bytes, file_size - offset) : 0;
    if (read_compressed_extent(chunk, stripes, devices, reg->logical_address,
                                encoded, reg->size_extent, mirror, &verified,
                                options) ||
        btrfs_decompress_extent(item->compression, current_fs_sectorsize(), encoded,
                                 reg->size_extent, decoded, item->decoded_size, 1,
                                 &decoded_size) ||
        reg->offset > decoded_size || needed > decoded_size - reg->offset)
      continue;
    result = write_decoded_extent(name, offset, file_size, decoded + reg->offset,
                                   reg->num_bytes, verified, options);
    break;
  }
out:
  if (devices) {
    for (uint16_t i = 0; i < chunk->num_stripes; i++)
      if (devices[i]) fclose(devices[i]);
  }
  free(devices);
  free(encoded);
  free(decoded);
  return result;
}

static int extract_inline_extent(FILE *items, const struct extent_data_item *item,
                                  const struct extent_cache *cached,
                                  uint64_t file_size, const uint8_t *name,
                                  const struct extraction_options *options) {
  if (!item->decoded_size || item->decoded_size > RECOVERY_MAX_COMPRESSED_EXTENT)
    return -1;
  uint64_t encoded_size = item->decoded_size;
  if (cached->extent_item_size) {
    if (cached->extent_item_size <= sizeof(*item)) return -1;
    encoded_size = cached->extent_item_size - sizeof(*item);
  } else if (item->compression) {
    /* Compressed inline payloads need the exact item boundary. */
    return -1;
  }
  if (!encoded_size || encoded_size > RECOVERY_MAX_COMPRESSED_EXTENT ||
      (!item->compression && encoded_size != item->decoded_size)) return -1;
  uint8_t *encoded = malloc(encoded_size);
  uint8_t *decoded = item->compression ? malloc(item->decoded_size) : NULL;
  int result = -1;
  if (!encoded || (item->compression && !decoded) ||
      fread(encoded, 1, encoded_size, items) != encoded_size) goto out;
  if (item->compression &&
      btrfs_decompress_extent(item->compression, current_fs_sectorsize(), encoded,
                               encoded_size, decoded, item->decoded_size, 0, NULL)) goto out;
  result = write_decoded_extent(name, cached->offset_in_file, file_size,
                                 item->compression ? decoded : encoded,
                                 item->decoded_size, 0, options);
out:
  free(encoded);
  free(decoded);
  return result;
}

static char *extract_cache_path(const struct inode_instance_id *id,
                                uint8_t key_type, int cache_type) {
  if (!id)
    return NULL;
  if (key_type == INODE_ITEM)
    return file_tree_inode_cache_path(id, cache_type);
  return file_tree_cache_path_typed(id->objectid, id->root_dirid, key_type,
                                    cache_type);
}

static uint64_t extract_name_digest(const uint8_t *name, size_t name_len) {
  uint64_t hash = UINT64_C(14695981039346656037);
  uint64_t encoded_len = name_len;
  for (size_t i = 0; i < sizeof(encoded_len); i++) {
    hash ^= (uint8_t)(encoded_len & 0xffU);
    hash *= UINT64_C(1099511628211);
    encoded_len >>= 8;
  }
  for (size_t i = 0; i < name_len; i++) {
    hash ^= name[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static char *generated_extract_component(
    const struct inode_instance_id *id, uint8_t key_type,
    const uint8_t *raw_name, size_t raw_name_len) {
  uint64_t digest = extract_name_digest(raw_name, raw_name_len);
  int component_len = snprintf(
      NULL, 0,
      EXTRACT_GENERATED_NAME_PREFIX "r0x%" PRIx64 "-o0x%" PRIx64
                                    "-g0x%" PRIx64 "-t0x%02x-h%016" PRIx64,
      id->root_dirid, id->objectid, id->inode_generation,
      (unsigned)key_type, digest);
  if (component_len < 0 || (size_t)component_len > EXTRACT_COMPONENT_MAX)
    return NULL;

  char *component = malloc((size_t)component_len + 1);
  if (!component)
    return NULL;
  snprintf(component, (size_t)component_len + 1,
           EXTRACT_GENERATED_NAME_PREFIX "r0x%" PRIx64 "-o0x%" PRIx64
                                         "-g0x%" PRIx64
                                         "-t0x%02x-h%016" PRIx64,
           id->root_dirid, id->objectid, id->inode_generation,
           (unsigned)key_type, digest);
  return component;
}

static char *generated_extract_identity_directory(
    const struct inode_instance_id *id, uint8_t key_type,
    const char *marker) {
  int component_len =
      marker ? snprintf(NULL, 0,
                        EXTRACT_GENERATED_NAME_PREFIX
                        "r0x%" PRIx64 "-o0x%" PRIx64 "-g0x%" PRIx64
                        "-t0x%02x-%s",
                        id->root_dirid, id->objectid,
                        id->inode_generation, (unsigned)key_type, marker)
             : snprintf(NULL, 0,
                        EXTRACT_GENERATED_NAME_PREFIX
                        "r0x%" PRIx64 "-o0x%" PRIx64 "-g0x%" PRIx64
                        "-t0x%02x",
                        id->root_dirid, id->objectid,
                        id->inode_generation, (unsigned)key_type);
  if (component_len < 0 || (size_t)component_len > EXTRACT_COMPONENT_MAX)
    return NULL;

  char *component = malloc((size_t)component_len + 1);
  if (!component)
    return NULL;
  if (marker)
    snprintf(component, (size_t)component_len + 1,
             EXTRACT_GENERATED_NAME_PREFIX
             "r0x%" PRIx64 "-o0x%" PRIx64 "-g0x%" PRIx64
             "-t0x%02x-%s",
             id->root_dirid, id->objectid, id->inode_generation,
             (unsigned)key_type, marker);
  else
    snprintf(component, (size_t)component_len + 1,
             EXTRACT_GENERATED_NAME_PREFIX
             "r0x%" PRIx64 "-o0x%" PRIx64 "-g0x%" PRIx64 "-t0x%02x",
             id->root_dirid, id->objectid, id->inode_generation,
             (unsigned)key_type);
  return component;
}

static int read_total_file_tree_for_extract(const struct inode_instance_id *id,
                                            uint8_t key_type,
                                            struct total_file_tree *node) {
  char *path = extract_cache_path(id, key_type, FILE_TREE_NODE_CACHE);
  FILE *file = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!file)
    return -1;

  int ret = -1;
  if (fread(node, sizeof(*node), 1, file) == 1 &&
      node->objectid == id->objectid && node->key_type == key_type &&
      (key_type != INODE_ITEM ||
       node->inode_generation == id->inode_generation))
    ret = 0;
  fclose(file);
  return ret;
}

enum extract_child_location {
  EXTRACT_CHILD_LOCATION_UNKNOWN = 0,
  EXTRACT_CHILD_LOCATION_CURRENT,
  EXTRACT_CHILD_LOCATION_STALE,
};

/*
 * File-tree parsing keeps the latest observed parent on each inode node.
 * Compare a directory edge with that parent before following it in
 * current-location extraction.  Unknown is deliberately distinct from stale:
 * incomplete recovery caches keep their existing best-effort traversal.
 */
static enum extract_child_location extract_child_location_for_parent(
    const struct inode_instance_id *parent_id, uint8_t parent_key_type,
    const struct child_cache *child) {
  if (!parent_id || !child)
    return EXTRACT_CHILD_LOCATION_UNKNOWN;
  if (child->type != CHILD)
    return EXTRACT_CHILD_LOCATION_STALE;
  if (parent_key_type != INODE_ITEM)
    return EXTRACT_CHILD_LOCATION_UNKNOWN;

  const struct inode_instance_id child_id = {
      .root_dirid = parent_id->root_dirid,
      .objectid = child->objectid,
      .inode_generation = child->inode_generation,
  };
  struct total_file_tree child_node;
  if (read_total_file_tree_for_extract(&child_id, child->key_type,
                                       &child_node) != 0 ||
      child_node.parent_objectid == 0 ||
      child_node.parent_objectid == INVALID_OBJECTID)
    return EXTRACT_CHILD_LOCATION_UNKNOWN;

  return child_node.parent_objectid == parent_id->objectid &&
                 child_node.parent_inode_generation ==
                     parent_id->inode_generation
             ? EXTRACT_CHILD_LOCATION_CURRENT
             : EXTRACT_CHILD_LOCATION_STALE;
}

static char *read_latest_name_for_extract(const struct inode_instance_id *id,
                                          uint8_t key_type) {
  char *path = extract_cache_path(id, key_type, FILE_TREE_NODE_NAME_CACHE);
  FILE *file = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!file)
    return NULL;

  struct name_cache name_cache;
  char *name = NULL;
  uint16_t name_len = 0;
  while (fread(&name_cache, NAME_CACHE_FIX_SIZE, 1, file) == 1) {
    char *next = realloc(name, (size_t)name_cache.name_len + 1);
    if (!next) {
      free(name);
      fclose(file);
      return NULL;
    }
    name = next;
    if (name_cache.name_len != 0 &&
        fread(name, name_cache.name_len, 1, file) != 1) {
      free(name);
      fclose(file);
      return NULL;
    }
    name[name_cache.name_len] = '\0';
    name_len = name_cache.name_len;
  }
  fclose(file);

  const size_t generated_prefix_len =
      sizeof(EXTRACT_GENERATED_NAME_PREFIX) - 1;
  int safe_name = name && name_len > 0 && name_len <= EXTRACT_COMPONENT_MAX &&
                  memchr(name, '\0', name_len) == NULL &&
                  memchr(name, '/', name_len) == NULL &&
                  !(name_len == 1 && name[0] == '.') &&
                  !(name_len == 2 && name[0] == '.' && name[1] == '.') &&
                  (name_len < generated_prefix_len ||
                   memcmp(name, EXTRACT_GENERATED_NAME_PREFIX,
                          generated_prefix_len) != 0);
  if (safe_name)
    return name;

  char *generated = generated_extract_component(
      id, key_type, (const uint8_t *)name, name_len);
  free(name);
  if (generated) {
    fprintf(stderr,
            "Unsafe, reserved, or empty recovered name for 0x%" PRIx64
            ", root 0x%" PRIx64 ", type 0x%x; using %s\n",
            id->objectid, id->root_dirid, key_type, generated);
  }
  return generated;
}

static char *join_extract_relative_path(const char *parent, const char *name) {
  size_t name_len = name ? strlen(name) : 0;
  if (name_len == 0 || name_len > EXTRACT_COMPONENT_MAX ||
      strchr(name, '/') != NULL || strcmp(name, ".") == 0 ||
      strcmp(name, "..") == 0)
    return NULL;

  if (!parent || parent[0] == '\0') {
    size_t len = name_len + 1;
    char *path = malloc(len);
    if (path)
      memcpy(path, name, len);
    return path;
  }

  size_t parent_len = strlen(parent);
  char *path = malloc(parent_len + 1 + name_len + 1);
  if (!path)
    return NULL;
  memcpy(path, parent, parent_len);
  path[parent_len] = '/';
  memcpy(path + parent_len + 1, name, name_len + 1);
  return path;
}

static char *prefixed_extract_file_name(uint64_t root_dirid, uint64_t objectid,
                                        const char *marker, const char *name,
                                        int *used_original_name) {
  if (!used_original_name)
    return NULL;
  *used_original_name = 0;

  const char *suffix = marker ? marker : "";
  int prefix_len = marker ? snprintf(NULL, 0, "0x%" PRIx64 "-0x%" PRIx64 "-%s-",
                                     root_dirid, objectid, suffix)
                          : snprintf(NULL, 0, "0x%" PRIx64 "-0x%" PRIx64 "-",
                                     root_dirid, objectid);
  if (prefix_len < 0)
    return NULL;

  size_t name_len = strlen(name);
  size_t full_len = (size_t)prefix_len + name_len;
  if (full_len > EXTRACT_COMPONENT_MAX) {
    char *original = malloc(name_len + 1);
    if (!original)
      return NULL;
    memcpy(original, name, name_len + 1);
    *used_original_name = 1;
    return original;
  }

  char *prefixed = malloc(full_len + 1);
  if (!prefixed)
    return NULL;

  if (marker)
    snprintf(prefixed, (size_t)prefix_len + 1,
             "0x%" PRIx64 "-0x%" PRIx64 "-%s-", root_dirid, objectid, suffix);
  else
    snprintf(prefixed, (size_t)prefix_len + 1, "0x%" PRIx64 "-0x%" PRIx64 "-",
             root_dirid, objectid);
  memcpy(prefixed + prefix_len, name, name_len + 1);
  return prefixed;
}

static char *duplicate_extract_string(const char *value) {
  if (!value)
    return NULL;
  size_t len = strlen(value) + 1;
  char *copy = malloc(len);
  if (copy)
    memcpy(copy, value, len);
  return copy;
}

static int extract_ancestor_matches(const struct extract_ancestor *ancestor,
                                    const struct inode_instance_id *id,
                                    uint8_t key_type) {
  return ancestor->key_type == key_type &&
         inode_instance_id_equal(&ancestor->id, id);
}

static int extract_ancestor_contains(const struct extract_ancestor *ancestors,
                                     const struct inode_instance_id *id,
                                     uint8_t key_type) {
  for (const struct extract_ancestor *entry = ancestors; entry;
       entry = entry->parent) {
    if (extract_ancestor_matches(entry, id, key_type))
      return 1;
  }
  return 0;
}

static int extract_ancestor_clone(const struct extract_ancestor *source,
                                  struct extract_ancestor **copy_out) {
  if (!copy_out)
    return -1;
  *copy_out = NULL;

  size_t count = 0;
  for (const struct extract_ancestor *entry = source; entry;
       entry = entry->parent) {
    if (count == SIZE_MAX)
      return -1;
    count++;
  }
  if (count == 0)
    return 0;
  if (count > SIZE_MAX / sizeof(struct extract_ancestor))
    return -1;

  struct extract_ancestor *copy = malloc(count * sizeof(*copy));
  if (!copy)
    return -1;

  const struct extract_ancestor *entry = source;
  for (size_t i = 0; i < count; i++) {
    copy[i].id = entry->id;
    copy[i].key_type = entry->key_type;
    copy[i].parent = i + 1 < count ? &copy[i + 1] : NULL;
    entry = entry->parent;
  }
  *copy_out = copy;
  return 0;
}

static int enqueue_extract_task(struct extract_context *ctx,
                                const struct inode_instance_id *id,
                                uint8_t key_type,
                                const char *parent_path, unsigned depth,
                                const struct extract_ancestor *ancestors) {
  if (!id)
    return -1;
  struct extract_task *task = calloc(1, sizeof(*task));
  if (!task)
    return -1;

  task->objectid = id->objectid;
  task->root_dirid = id->root_dirid;
  task->inode_generation = id->inode_generation;
  task->key_type = key_type;
  task->depth = depth;
  task->parent_path = duplicate_extract_string(parent_path);
  if ((parent_path && !task->parent_path) ||
      extract_ancestor_clone(ancestors, &task->ancestors) != 0) {
    free(task->parent_path);
    free(task);
    return -1;
  }

  pthread_mutex_lock(&ctx->mutex);
  if (ctx->tail)
    ctx->tail->next = task;
  else
    ctx->head = task;
  ctx->tail = task;
  ctx->pending++;
  pthread_cond_signal(&ctx->work_ready);
  pthread_mutex_unlock(&ctx->mutex);
  return 0;
}

static struct extract_task *dequeue_extract_task(struct extract_context *ctx) {
  pthread_mutex_lock(&ctx->mutex);
  while (!ctx->head && !ctx->stopping)
    pthread_cond_wait(&ctx->work_ready, &ctx->mutex);

  struct extract_task *task = NULL;
  if (ctx->head) {
    task = ctx->head;
    ctx->head = task->next;
    if (!ctx->head)
      ctx->tail = NULL;
  }
  pthread_mutex_unlock(&ctx->mutex);
  return task;
}

static void complete_extract_task(struct extract_context *ctx, int failed) {
  pthread_mutex_lock(&ctx->mutex);
  if (failed)
    ctx->errors = 1;
  if (ctx->pending > 0)
    ctx->pending--;
  if (ctx->pending == 0)
    pthread_cond_signal(&ctx->all_done);
  pthread_mutex_unlock(&ctx->mutex);
}

static int create_extract_dir(const char *relative_path) {
  int ret = ensure_extract_directory(relative_path);
  if (ret != 0)
    perror("mkdir");
  return ret;
}

static int create_empty_extract_file(const char *relative_path) {
  FILE *file = open_extract_file((const uint8_t *)relative_path, "wb");
  if (!file) {
    perror("fopen");
    return -1;
  }
  if (fclose(file) != 0)
    return -1;
  return 0;
}

static int read_inode_for_extract(const struct total_file_tree *file_node,
                                  const struct inode_instance_id *id,
                                  struct inode_item *inode) {
  if (!file_node || !id || !inode ||
      file_node->inode_item_index == INVALID_INDEX ||
      file_node->objectid != id->objectid ||
      file_node->inode_generation != id->inode_generation)
    return -1;

  char *path = item_cache_path(INODE_ITEM_INDEX);
  FILE *file = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!file)
    return -1;

  int ret = -1;
  if (fseek(file, file_node->inode_item_index, SEEK_SET) == 0 &&
      fread(inode, sizeof(*inode), 1, file) == 1 &&
      inode->generation == id->inode_generation)
    ret = 0;
  if (fclose(file) != 0)
    ret = -1;
  return ret;
}

static int file_node_has_salvaged_extents(const struct total_file_tree *node,
                                          const struct inode_instance_id *id) {
  if (node->extent_item_count == 0)
    return 0;

  char *path = extract_cache_path(id, node->key_type,
                                  FILE_TREE_NODE_EXTENT_CACHE);
  FILE *file = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!file)
    return 0;

  struct extent_cache extent;
  int has_salvaged = 0;
  for (uint32_t i = 0; i < node->extent_item_count; i++) {
    if (fread(&extent, sizeof(extent), 1, file) != 1)
      break;
    if (extent.source == EXTENT_SOURCE_SALVAGED) {
      has_salvaged = 1;
      break;
    }
  }
  fclose(file);
  return has_salvaged;
}

static int file_node_has_complete_extents(const struct total_file_tree *node,
                                          const struct inode_instance_id *id) {
  if (node->extent_item_count == 0)
    return 0;

  char *path = extract_cache_path(id, node->key_type,
                                  FILE_TREE_NODE_EXTENT_CACHE);
  FILE *file = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!file)
    return 0;

  struct extent_cache extent;
  int has_complete = 0;
  for (uint32_t i = 0; i < node->extent_item_count; i++) {
    if (fread(&extent, sizeof(extent), 1, file) != 1)
      break;
    if (extent.source == EXTENT_SOURCE_COMPLETE) {
      has_complete = 1;
      break;
    }
  }
  fclose(file);
  return has_complete;
}

static int read_extent_file_range(FILE *extent_data_item,
                                  const struct extent_cache *extent,
                                  uint64_t *start, uint64_t *end) {
  struct extent_data_item extent_data;
  struct extent_data_reg_prealloc reg_prealloc;
  uint64_t size;

  if (!extent_data_item || !extent || !start || !end)
    return -1;

  if (fseek(extent_data_item, extent->extent_item_index, SEEK_SET) != 0 ||
      fread(&extent_data, sizeof(extent_data), 1, extent_data_item) != 1)
    return -1;

  if (extent_data.extent_type == EXTENT_INLINE) {
    size = extent_data.decoded_size;
  } else if (extent_data.extent_type == EXTENT_REG ||
             extent_data.extent_type == EXTENT_PREALLOC) {
    if (fread(&reg_prealloc, sizeof(reg_prealloc), 1, extent_data_item) != 1)
      return -1;
    size = reg_prealloc.num_bytes;
    if (size == 0 || size > extent_data.decoded_size)
      size = extent_data.decoded_size;
  } else {
    return -1;
  }

  if (UINT64_MAX - extent->offset_in_file < size)
    return -1;

  *start = extent->offset_in_file;
  *end = extent->offset_in_file + size;
  return *start < *end ? 0 : -1;
}

static uint64_t
read_extent_source_logical_address(FILE *extent_data_item,
                                   const struct extent_cache *extent) {
  struct extent_data_item extent_data;
  struct extent_data_reg_prealloc reg_prealloc;
  uint64_t logical_address = INVALID_OBJECTID;

  if (!extent_data_item || !extent ||
      fseek(extent_data_item, extent->extent_item_index, SEEK_SET) != 0 ||
      fread(&extent_data, sizeof(extent_data), 1, extent_data_item) != 1 ||
      (extent_data.extent_type != EXTENT_REG &&
       extent_data.extent_type != EXTENT_PREALLOC) ||
      fread(&reg_prealloc, sizeof(reg_prealloc), 1, extent_data_item) != 1)
    return INVALID_OBJECTID;

  if (extent_data.compression)
    return compressed_extent_valid(&extent_data, &reg_prealloc)
               ? reg_prealloc.logical_address : INVALID_OBJECTID;
  if (extent_source_logical_address(&reg_prealloc, &logical_address) != 0)
    return INVALID_OBJECTID;
  return logical_address;
}

static int extent_has_file_range_conflict(FILE *extent_data_item,
                                          const struct extent_cache *extents,
                                          uint32_t extent_count,
                                          uint32_t extent_index,
                                          uint64_t target_size,
                                          int include_salvaged) {
  const struct extent_cache *target = &extents[extent_index];
  uint64_t target_start = target->offset_in_file;
  uint64_t target_end;

  if (!include_salvaged && target->source == EXTENT_SOURCE_SALVAGED)
    return 0;
  if (target->logical_address == INVALID_OBJECTID || target_size == 0 ||
      UINT64_MAX - target_start < target_size)
    return 0;

  target_end = target_start + target_size;

  for (uint32_t i = 0; i < extent_count; i++) {
    uint64_t start = 0;
    uint64_t end = 0;

    if (i == extent_index)
      continue;
    if (!include_salvaged && extents[i].source == EXTENT_SOURCE_SALVAGED)
      continue;
    if (extents[i].logical_address == INVALID_OBJECTID ||
        extents[i].logical_address == target->logical_address)
      continue;
    if (read_extent_file_range(extent_data_item, &extents[i], &start, &end) !=
        0)
      continue;
    if (ranges_overlap(target_start, target_end, start, end))
      return 1;
  }

  return 0;
}

static int extract_file_node_impl(const struct total_file_tree *file_node,
                                  const struct inode_instance_id *id,
                                  const char *relative_path,
                                  int include_salvaged,
                                  const struct extraction_options *options) {
  struct extent_cache *extent_caches = NULL;
  FILE *extent_data_item = NULL;
  struct inode_item inode;
  char *path = NULL;

  if (create_empty_extract_file(relative_path) != 0)
    return -1;
  if (!options->no_data_checksum &&
      clear_verified_regions_for_name((const uint8_t *)relative_path) != 0)
    return -1;

  if (file_node->extent_item_count == 0) {
    fprintf(stderr,
            "No extent data cached for 0x%" PRIx64 ", root 0x%" PRIx64 "\n",
            file_node->objectid, id->root_dirid);
    return 0;
  }

  if (read_inode_for_extract(file_node, id, &inode) != 0) {
    fprintf(stderr, "Failed to read inode for 0x%" PRIx64 "\n",
            file_node->objectid);
    return -1;
  }

  extent_caches =
      calloc(file_node->extent_item_count, sizeof(struct extent_cache));
  if (!extent_caches) {
    perror("calloc");
    return -1;
  }

  path = extract_cache_path(id, file_node->key_type,
                            FILE_TREE_NODE_EXTENT_CACHE);
  FILE *file = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!file) {
    perror("fopen");
    free(extent_caches);
    return -1;
  }

  for (uint32_t i = 0; i < file_node->extent_item_count; i++) {
    if (fread(&extent_caches[i], sizeof(struct extent_cache), 1, file) != 1) {
      fclose(file);
      free(extent_caches);
      return -1;
    }
  }
  fclose(file);

  path = item_cache_path(EXTENT_DATA_INDEX);
  extent_data_item = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!extent_data_item) {
    perror("fopen");
    free(extent_caches);
    return -1;
  }

  for (uint32_t i = 0; i < file_node->extent_item_count; i++)
    extent_caches[i].logical_address =
        read_extent_source_logical_address(extent_data_item, &extent_caches[i]);

  int failed = 0;
  for (uint32_t i = 0; i < file_node->extent_item_count; i++) {
    struct extent_data_item extent_data;
    struct extent_data_reg_prealloc reg_prealloc;

    if (!include_salvaged && extent_caches[i].source == EXTENT_SOURCE_SALVAGED)
      continue;

    if (fseek(extent_data_item, extent_caches[i].extent_item_index, SEEK_SET) !=
            0 ||
        fread(&extent_data, sizeof(struct extent_data_item), 1,
              extent_data_item) != 1) {
      failed = 1;
      continue;
    }

    if (extent_data.compression > 3 || extent_data.encryption != 0 ||
        extent_data.other_encoding != 0) {
      fprintf(stderr,
              "Unsupported encoded extent at 0x%" PRIx64
              " (compression=%u, encryption=%u, other=%u)\n",
              file_node->objectid, extent_data.compression,
              extent_data.encryption, extent_data.other_encoding);
      failed = 1;
      continue;
    }

    if (extent_data.extent_type == EXTENT_INLINE) {
      if (extract_inline_extent(extent_data_item, &extent_data, &extent_caches[i],
                                  inode.size, (const uint8_t *)relative_path,
                                  options)) {
        fprintf(stderr, "Failed to extract inline extent for 0x%" PRIx64 "\n",
                file_node->objectid);
        failed = 1;
      }
      continue;
    }
    if ((extent_data.extent_type != EXTENT_REG &&
         extent_data.extent_type != EXTENT_PREALLOC) ||
        (extent_data.compression && extent_data.extent_type != EXTENT_REG)) {
      failed = 1;
      continue;
    }

    if (extent_caches[i].extent_item_size &&
        extent_caches[i].extent_item_size !=
            sizeof(extent_data) + sizeof(reg_prealloc)) {
      failed = 1;
      continue;
    }
    if (fread(&reg_prealloc, sizeof(struct extent_data_reg_prealloc), 1,
              extent_data_item) != 1) {
      failed = 1;
      continue;
    }

    if (!extent_data.compression &&
        (reg_prealloc.logical_address == 0 || reg_prealloc.num_bytes == 0))
      continue;

    uint64_t bytes_to_extract = reg_prealloc.num_bytes;
    if (bytes_to_extract == 0 || bytes_to_extract > extent_data.decoded_size)
      bytes_to_extract = extent_data.decoded_size;

    uint64_t source_logical_address = 0;
    if (extent_data.compression) {
      if (!compressed_extent_valid(&extent_data, &reg_prealloc)) {
        fprintf(stderr, "Invalid compressed extent for 0x%" PRIx64 "\n",
                file_node->objectid);
        failed = 1;
        continue;
      }
      source_logical_address = reg_prealloc.logical_address;
    } else if (extent_source_logical_address(&reg_prealloc,
                                             &source_logical_address) != 0) {
      fprintf(stderr, "Invalid extent source range for 0x%" PRIx64 "\n",
              file_node->objectid);
      failed = 1;
      continue;
    }
    int verify_checksum = options->no_data_checksum ? EXTRACT_WRITE_DIRECT :
        (!extent_data.compression && extent_has_file_range_conflict(
            extent_data_item, extent_caches, file_node->extent_item_count, i,
            bytes_to_extract, include_salvaged));

    struct parse_btrfs_chunk chunk;
    struct stripe *stripe_data = NULL;
    if (load_chunk_mapping_for_logical(source_logical_address,
                                       extent_data.generation, &chunk,
                                       &stripe_data) != 0) {
      fprintf(stderr,
              "No chunk mapping for extent logical 0x%" PRIx64 " in 0x%" PRIx64
              "\n",
              source_logical_address, file_node->objectid);
      failed = 1;
      continue;
    }

    uint64_t chunk_offset = source_logical_address - chunk.logical_address;
    uint64_t disk_read_size = extent_data.compression ? reg_prealloc.size_extent
                                                    : bytes_to_extract;
    if (source_logical_address < chunk.logical_address ||
        chunk_offset > chunk.length ||
        disk_read_size > chunk.length - chunk_offset) {
      fprintf(stderr,
              "Extent source range crosses chunk boundary for 0x%" PRIx64
              "\n",
              file_node->objectid);
      free(stripe_data);
      failed = 1;
      continue;
    }

    int extraction_result = extent_data.compression
        ? extract_compressed_extent(&extent_data, &reg_prealloc, &chunk, stripe_data,
                                      extent_caches[i].offset_in_file, inode.size,
                                      (const uint8_t *)relative_path, options)
        : extract_raid_from_mapping(&chunk, bytes_to_extract,
                                      extent_caches[i].offset_in_file, inode.size,
                                      reg_prealloc, stripe_data,
                                      (uint8_t *)relative_path, verify_checksum);
    if (extraction_result != 0) {
      fprintf(stderr, "Failed to extract extent for 0x%" PRIx64 "\n",
              file_node->objectid);
      failed = 1;
    }
    free(stripe_data);
  }

  fclose(extent_data_item);
  free(extent_caches);
  return failed ? -1 : 0;
}

static int extract_file_node(const struct total_file_tree *file_node,
                             const struct inode_instance_id *id,
                             const char *relative_path,
                             int include_salvaged,
                             const struct extraction_options *options) {
  int ret = extract_file_node_impl(file_node, id, relative_path,
                                   include_salvaged, options);
  if (!options->no_data_checksum &&
      clear_verified_regions_for_name((const uint8_t *)relative_path) != 0)
    ret = -1;
  return ret;
}

static int extract_children(const struct inode_instance_id *id,
                            uint8_t key_type, const char *relative_path,
                            unsigned depth, int include_moved,
                            struct extract_context *ctx,
                            struct extract_visit_set *visited,
                            const struct extract_ancestor *ancestors,
                            const struct extraction_options *options);

static int extract_node_recursive(const struct inode_instance_id *id,
                                  uint8_t key_type, const char *parent_path,
                                  unsigned depth, int include_moved,
                                  struct extract_context *ctx,
                                  struct extract_visit_set *visited,
                                  const struct extract_ancestor *ancestors,
                                  const struct extraction_options *options);

static int extract_node_recursive_impl(
    const struct inode_instance_id *id, uint8_t key_type,
    const char *parent_path, unsigned depth, int include_moved,
    struct extract_context *ctx, struct extract_visit_set *visited,
    const struct extract_ancestor *ancestors,
    const struct extraction_options *options) {

  if (options->regular_files_only && key_type != INODE_ITEM)
    return -1;
  struct total_file_tree file_node;
  if (read_total_file_tree_for_extract(id, key_type, &file_node) != 0) {
    fprintf(stderr,
            "Failed to read file tree node: 0x%" PRIx64 ", root 0x%" PRIx64
            ", type 0x%x\n",
            id->objectid, id->root_dirid, key_type);
    return -1;
  }

  char *name = read_latest_name_for_extract(id, key_type);
  if (!name)
    return -1;

  if (key_type == ROOT_ITEM) {
    char *relative_path = join_extract_relative_path(parent_path, name);
    free(name);
    if (!relative_path)
      return -1;

    if (create_extract_dir(relative_path) != 0) {
      free(relative_path);
      return -1;
    }
    if (extract_visit_set_set_success(visited, id, key_type, 1) != 0) {
      free(relative_path);
      return -1;
    }

    char *cache_dir = file_tree_cache_dir_all_path();
    DIR *dir = cache_dir ? opendir(cache_dir) : NULL;
    free(cache_dir);
    if (!dir) {
      free(relative_path);
      return -1;
    }

    int ret = 0;
    int found = 0;
    for (;;) {
      errno = 0;
      struct dirent *entry = readdir(dir);
      if (!entry) {
        if (errno != 0)
          ret = -1;
        break;
      }
      uint64_t root_dirid = 0;
      uint64_t objectid = 0;
      uint64_t inode_generation = 0;
      int consumed = 0;
      if (sscanf(entry->d_name,
                 "%" SCNu64 "_%" SCNu64 "_g%" SCNu64 "%n", &root_dirid,
                 &objectid, &inode_generation, &consumed) != 3 ||
          entry->d_name[consumed] != '\0' || root_dirid != id->objectid ||
          objectid != BTRFS_SUBVOL_ROOT_INODE)
        continue;

      const struct inode_instance_id subvolume_root_id = {
          .root_dirid = root_dirid,
          .objectid = objectid,
          .inode_generation = inode_generation,
      };
      found = 1;
      int root_ret =
          ctx ? enqueue_extract_task(ctx, &subvolume_root_id, INODE_ITEM,
                                     relative_path, depth + 1, ancestors)
              : extract_node_recursive(&subvolume_root_id, INODE_ITEM,
                                       relative_path, depth + 1,
                                       include_moved, NULL, visited,
                                       ancestors, options);
      if (root_ret != 0)
        ret = -1;
    }
    closedir(dir);
    free(relative_path);
    return found ? ret : -1;
  }

  struct inode_item inode;
  int inode_read = read_inode_for_extract(&file_node, id, &inode);
  if (key_type == INODE_ITEM && inode_read != 0) {
    fprintf(stderr,
            "Failed to read matching raw inode for 0x%" PRIx64
            ", root 0x%" PRIx64 ", generation 0x%" PRIx64 "\n",
            id->objectid, id->root_dirid, id->inode_generation);
    free(name);
    return -1;
  }
  if (options->regular_files_only &&
      (inode_read != 0 || !S_ISREG(inode.mode))) {
    free(name);
    return -1;
  }
  int is_dir = !options->regular_files_only &&
      (file_node.child_count > 0 || (inode_read == 0 && S_ISDIR(inode.mode)));

  char *path_name = NULL;
  if (is_dir) {
    path_name = duplicate_extract_string(name);
    free(name);
    if (!path_name)
      return -1;

    char *relative_path = join_extract_relative_path(parent_path, path_name);
    free(path_name);
    if (!relative_path)
      return -1;

    if (create_extract_dir(relative_path) != 0) {
      free(relative_path);
      return -1;
    }
    if (extract_visit_set_set_success(visited, id, key_type, 1) != 0) {
      free(relative_path);
      return -1;
    }
    int ret = extract_children(id, key_type, relative_path, depth + 1,
                               include_moved, ctx, visited, ancestors, options);
    free(relative_path);
    return ret;
  }

  int has_complete = file_node_has_complete_extents(&file_node, id);
  int has_salvaged = file_node_has_salvaged_extents(&file_node, id);
  const char *markers[2] = {NULL, NULL};
  int include_salvaged[2] = {1, 0};
  int variants = 1;
  if (has_complete && has_salvaged) {
    markers[0] = "[Complete]";
    include_salvaged[0] = 0;
    markers[1] = "[salvaged]";
    include_salvaged[1] = 1;
    variants = 2;
  }

  int ret = 0;
  for (int i = 0; i < variants; i++) {
    int used_original_name = 0;
    path_name = prefixed_extract_file_name(id->root_dirid, id->objectid,
                                           markers[i], name,
                                           &used_original_name);
    if (!path_name) {
      ret = -1;
      continue;
    }

    char *generated_parent = NULL;
    const char *file_parent = parent_path;
    if (used_original_name) {
      char *identity_directory = generated_extract_identity_directory(
          id, key_type, markers[i]);
      if (!identity_directory) {
        free(path_name);
        ret = -1;
        continue;
      }
      generated_parent =
          join_extract_relative_path(parent_path, identity_directory);
      free(identity_directory);
      if (!generated_parent || create_extract_dir(generated_parent) != 0) {
        free(generated_parent);
        free(path_name);
        ret = -1;
        continue;
      }
      file_parent = generated_parent;
    }

    char *relative_path = join_extract_relative_path(file_parent, path_name);
    free(path_name);
    if (!relative_path) {
      free(generated_parent);
      ret = -1;
      continue;
    }

    if (extract_file_node(&file_node, id, relative_path, include_salvaged[i],
                           options) != 0)
      ret = -1;
    free(relative_path);
    free(generated_parent);
  }

  free(name);
  return ret;
}

static int extract_node_recursive(const struct inode_instance_id *id,
                                  uint8_t key_type, const char *parent_path,
                                  unsigned depth, int include_moved,
                                  struct extract_context *ctx,
                                  struct extract_visit_set *visited,
                                  const struct extract_ancestor *ancestors,
                                  const struct extraction_options *options) {
  if (!id)
    return -1;
  if (extract_ancestor_contains(ancestors, id, key_type)) {
    fprintf(stderr,
            "extract recursion cycle at 0x%" PRIx64 ", root 0x%" PRIx64
            ", generation 0x%" PRIx64 ", type 0x%x\n",
            id->objectid, id->root_dirid, id->inode_generation, key_type);
    return -1;
  }
  if (depth > 256) {
    fprintf(stderr, "extract recursion too deep at 0x%" PRIx64 "\n",
            id->objectid);
    return -1;
  }
  if (extract_visit_set_mark(visited, id, key_type) != 0)
    return -1;

  const struct extract_ancestor current = {
      .id = *id,
      .key_type = key_type,
      .parent = ancestors,
  };
  int ret = extract_node_recursive_impl(id, key_type, parent_path, depth,
                                        include_moved, ctx, visited, &current,
                                        options);
  if (ret == 0 &&
      extract_visit_set_set_success(visited, id, key_type, 1) != 0)
    return -1;
  return ret;
}

static int extract_children(const struct inode_instance_id *id,
                            uint8_t key_type, const char *relative_path,
                            unsigned depth, int include_moved,
                            struct extract_context *ctx,
                            struct extract_visit_set *visited,
                            const struct extract_ancestor *ancestors,
                            const struct extraction_options *options) {
  struct total_file_tree node;
  if (read_total_file_tree_for_extract(id, key_type, &node) != 0)
    return -1;
  if (node.child_count == 0)
    return 0;

  char *path = extract_cache_path(id, key_type, FILE_TREE_NODE_CHILD_CACHE);
  FILE *file = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!file)
    return -1;

  int ret = 0;
  struct child_cache child;
  for (uint32_t i = 0; i < node.child_count; i++) {
    if (fread(&child, sizeof(child), 1, file) != 1) {
      ret = -1;
      break;
    }
    if (child.type != CHILD && child.type != MOVED && child.type != DELETED)
      continue;
    if (!include_moved && child.type != CHILD)
      continue;
    if (child.objectid == id->objectid && child.key_type == key_type &&
        child.inode_generation == id->inode_generation)
      continue;

    if (!include_moved &&
        extract_child_location_for_parent(id, key_type, &child) ==
            EXTRACT_CHILD_LOCATION_STALE)
      continue;

    const struct inode_instance_id child_id = {
        .root_dirid = id->root_dirid,
        .objectid = child.objectid,
        .inode_generation = child.inode_generation,
    };
    int child_ret =
        ctx ? enqueue_extract_task(ctx, &child_id, child.key_type,
                                   relative_path, depth, ancestors)
            : extract_node_recursive(&child_id, child.key_type, relative_path,
                                     depth, include_moved, NULL, visited,
                                     ancestors, options);
    if (child_ret != 0)
      ret = -1;
  }

  fclose(file);
  return ret;
}

static void *extract_worker_main(void *arg) {
  struct extract_context *ctx = arg;

  for (;;) {
    struct extract_task *task = dequeue_extract_task(ctx);
    if (!task)
      break;

    const struct inode_instance_id id = {
        .root_dirid = task->root_dirid,
        .objectid = task->objectid,
        .inode_generation = task->inode_generation,
    };
    int ret = extract_node_recursive(&id, task->key_type, task->parent_path,
                                     task->depth, ctx->include_moved, ctx,
                                     ctx->visited, task->ancestors,
                                     &ctx->options);
    free(task->parent_path);
    free(task->ancestors);
    free(task);
    complete_extract_task(ctx, ret != 0);
  }

  return NULL;
}

static int extract_parallel(const struct inode_instance_id *id,
                            int thread_count, int include_moved,
                            struct extract_visit_set *visited,
                            const struct extraction_options *options) {
  struct extract_context ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.include_moved = include_moved;
  ctx.options = *options;
  ctx.visited = visited;

  if (pthread_mutex_init(&ctx.mutex, NULL) != 0 ||
      pthread_cond_init(&ctx.work_ready, NULL) != 0 ||
      pthread_cond_init(&ctx.all_done, NULL) != 0)
    return -1;

  pthread_t *threads = calloc((size_t)thread_count, sizeof(*threads));
  if (!threads) {
    pthread_mutex_destroy(&ctx.mutex);
    pthread_cond_destroy(&ctx.work_ready);
    pthread_cond_destroy(&ctx.all_done);
    return -1;
  }

  int created = 0;
  for (int i = 0; i < thread_count; i++) {
    if (pthread_create(&threads[i], NULL, extract_worker_main, &ctx) != 0)
      break;
    created++;
  }

  if (created == 0 ||
      enqueue_extract_task(&ctx, id, INODE_ITEM, NULL, 0, NULL) != 0) {
    pthread_mutex_lock(&ctx.mutex);
    ctx.stopping = 1;
    pthread_cond_broadcast(&ctx.work_ready);
    pthread_mutex_unlock(&ctx.mutex);
    for (int i = 0; i < created; i++)
      pthread_join(threads[i], NULL);
    free(threads);
    pthread_mutex_destroy(&ctx.mutex);
    pthread_cond_destroy(&ctx.work_ready);
    pthread_cond_destroy(&ctx.all_done);
    return -1;
  }

  pthread_mutex_lock(&ctx.mutex);
  while (ctx.pending > 0)
    pthread_cond_wait(&ctx.all_done, &ctx.mutex);
  ctx.stopping = 1;
  pthread_cond_broadcast(&ctx.work_ready);
  pthread_mutex_unlock(&ctx.mutex);

  for (int i = 0; i < created; i++)
    pthread_join(threads[i], NULL);

  int ret = ctx.errors ? -1 : 0;
  free(threads);
  pthread_mutex_destroy(&ctx.mutex);
  pthread_cond_destroy(&ctx.work_ready);
  pthread_cond_destroy(&ctx.all_done);
  return ret;
}

static int extract_typed(const struct inode_instance_id *id,
                         uint8_t key_type, int include_moved,
                         struct extract_visit_set *visited,
                         const struct extraction_options *options) {
  if (!id)
    return -1;
  if (worker_thread_count > 1 && key_type == INODE_ITEM) {
    if (extract_parallel(id, worker_thread_count, include_moved, visited,
                          options) == 0)
      return 0;
    fprintf(stderr, "Parallel extract failed; retrying with one thread\n");
  }
  return extract_node_recursive(id, key_type, NULL, 0, include_moved, NULL,
                                visited, NULL, options);
}

int extract_one_with_options(const struct inode_instance_id *id,
                             const struct extraction_options *options) {
  const struct extraction_options resolved = options ? *options :
      (struct extraction_options){0};
  return extract_typed(id, INODE_ITEM, 1, NULL, &resolved);
}

int extract_one(const struct inode_instance_id *id) {
  return extract_one_with_options(id, NULL);
}

int extract_one_without_moved(const struct inode_instance_id *id) {
  const struct extraction_options options = {0};
  return extract_typed(id, INODE_ITEM, 0, NULL, &options);
}

static int extract_batch_is_file_tree_root(uint64_t root_dirid) {
  if (root_dirid == BTRFS_FS_TREE_OBJECTID)
    return 1;
  return root_dirid >= BTRFS_FIRST_FREE_OBJECTID &&
         root_dirid <= (uint64_t)BTRFS_LAST_FREE_OBJECTID;
}

#define EXTRACT_GRAPH_INITIAL_CAPACITY 16U

struct extract_candidate_edge {
  size_t parent;
  size_t child;
};

struct extract_candidate_vertex {
  struct inode_instance_id id;
  size_t next_root_candidate_plus_one;
  uint8_t key_type;
};

struct extract_candidate_index_entry {
  struct inode_instance_id id;
  size_t vertex_plus_one;
  uint8_t key_type;
};

struct extract_candidate_root_entry {
  uint64_t root_dirid;
  size_t head_plus_one;
  uint8_t occupied;
};

struct extract_candidate_graph {
  struct extract_candidate_vertex *vertices;
  size_t vertex_count;
  size_t vertex_capacity;
  size_t candidate_count;
  struct extract_candidate_edge *edges;
  size_t edge_count;
  size_t edge_capacity;
  struct extract_candidate_index_entry *index;
  size_t index_capacity;
  struct extract_candidate_root_entry *roots;
  size_t root_count;
  size_t root_capacity;
};

static int extract_inode_id_compare(const struct inode_instance_id *a,
                                    const struct inode_instance_id *b) {
  if (a->root_dirid != b->root_dirid)
    return a->root_dirid < b->root_dirid ? -1 : 1;
  if (a->objectid != b->objectid)
    return a->objectid < b->objectid ? -1 : 1;
  if (a->inode_generation != b->inode_generation)
    return a->inode_generation < b->inode_generation ? -1 : 1;
  return 0;
}

static int extract_inode_id_qsort_compare(const void *lhs, const void *rhs) {
  return extract_inode_id_compare(lhs, rhs);
}

static int extract_candidate_ids_canonicalize(
    const struct inode_instance_id *ids, size_t count,
    struct inode_instance_id **candidates_out, size_t *candidate_count_out) {
  *candidates_out = NULL;
  *candidate_count_out = 0;
  if (count == 0)
    return 0;
  if (count > SIZE_MAX / sizeof(**candidates_out))
    return -1;

  struct inode_instance_id *candidates =
      malloc(count * sizeof(*candidates));
  if (!candidates)
    return -1;

  size_t candidate_count = 0;
  for (size_t i = 0; i < count; i++) {
    if (extract_batch_is_file_tree_root(ids[i].root_dirid))
      candidates[candidate_count++] = ids[i];
  }
  qsort(candidates, candidate_count, sizeof(*candidates),
        extract_inode_id_qsort_compare);

  size_t unique_count = 0;
  for (size_t i = 0; i < candidate_count; i++) {
    if (unique_count != 0 &&
        inode_instance_id_equal(&candidates[unique_count - 1],
                                &candidates[i]))
      continue;
    candidates[unique_count++] = candidates[i];
  }

  if (unique_count == 0) {
    free(candidates);
    candidates = NULL;
  }
  *candidates_out = candidates;
  *candidate_count_out = unique_count;
  return 0;
}

static void
extract_candidate_graph_destroy(struct extract_candidate_graph *graph) {
  free(graph->vertices);
  free(graph->edges);
  free(graph->index);
  free(graph->roots);
  memset(graph, 0, sizeof(*graph));
}

static int extract_candidate_graph_index_resize(
    struct extract_candidate_graph *graph, size_t capacity) {
  if (capacity < graph->index_capacity ||
      capacity > SIZE_MAX / sizeof(*graph->index))
    return -1;

  struct extract_candidate_index_entry *index =
      calloc(capacity, sizeof(*index));
  if (!index)
    return -1;

  for (size_t i = 0; i < graph->index_capacity; i++) {
    const struct extract_candidate_index_entry *entry = &graph->index[i];
    if (entry->vertex_plus_one == 0)
      continue;
    size_t slot = extract_visit_hash(&entry->id, entry->key_type) &
                  (capacity - 1);
    while (index[slot].vertex_plus_one != 0)
      slot = (slot + 1) & (capacity - 1);
    index[slot] = *entry;
  }

  free(graph->index);
  graph->index = index;
  graph->index_capacity = capacity;
  return 0;
}

static int extract_candidate_graph_reserve_vertex(
    struct extract_candidate_graph *graph) {
  if (graph->vertex_count < graph->vertex_capacity)
    return 0;

  size_t capacity =
      graph->vertex_capacity ? graph->vertex_capacity * 2
                             : EXTRACT_GRAPH_INITIAL_CAPACITY;
  if (capacity < graph->vertex_capacity ||
      capacity > SIZE_MAX / sizeof(*graph->vertices))
    return -1;
  struct extract_candidate_vertex *vertices =
      realloc(graph->vertices, capacity * sizeof(*vertices));
  if (!vertices)
    return -1;
  graph->vertices = vertices;
  graph->vertex_capacity = capacity;
  return 0;
}

static int extract_candidate_graph_get_or_add(
    struct extract_candidate_graph *graph,
    const struct inode_instance_id *input_id, uint8_t key_type,
    size_t *vertex_out) {
  struct inode_instance_id id = *input_id;
  if (key_type != INODE_ITEM)
    id.inode_generation = 0;

  if (graph->index_capacity == 0 ||
      graph->vertex_count >= graph->index_capacity / 2) {
    size_t capacity =
        graph->index_capacity ? graph->index_capacity * 2
                              : EXTRACT_GRAPH_INITIAL_CAPACITY;
    if (capacity < graph->index_capacity ||
        extract_candidate_graph_index_resize(graph, capacity) != 0)
      return -1;
  }

  size_t slot =
      extract_visit_hash(&id, key_type) & (graph->index_capacity - 1);
  while (graph->index[slot].vertex_plus_one != 0) {
    if (graph->index[slot].key_type == key_type &&
        inode_instance_id_equal(&graph->index[slot].id, &id)) {
      *vertex_out = graph->index[slot].vertex_plus_one - 1;
      return 0;
    }
    slot = (slot + 1) & (graph->index_capacity - 1);
  }

  if (graph->vertex_count == SIZE_MAX ||
      extract_candidate_graph_reserve_vertex(graph) != 0)
    return -1;
  size_t vertex = graph->vertex_count++;
  graph->vertices[vertex] = (struct extract_candidate_vertex){
      .id = id,
      .key_type = key_type,
  };
  graph->index[slot] = (struct extract_candidate_index_entry){
      .id = id,
      .vertex_plus_one = vertex + 1,
      .key_type = key_type,
  };
  *vertex_out = vertex;
  return 0;
}

static int extract_candidate_graph_root_index_resize(
    struct extract_candidate_graph *graph, size_t capacity) {
  if (capacity < graph->root_capacity ||
      capacity > SIZE_MAX / sizeof(*graph->roots))
    return -1;

  struct extract_candidate_root_entry *roots =
      calloc(capacity, sizeof(*roots));
  if (!roots)
    return -1;

  for (size_t i = 0; i < graph->root_capacity; i++) {
    const struct extract_candidate_root_entry *entry = &graph->roots[i];
    if (!entry->occupied)
      continue;
    size_t slot = extract_visit_hash_value(entry->root_dirid) &
                  (capacity - 1);
    while (roots[slot].occupied)
      slot = (slot + 1) & (capacity - 1);
    roots[slot] = *entry;
  }

  free(graph->roots);
  graph->roots = roots;
  graph->root_capacity = capacity;
  return 0;
}

static struct extract_candidate_root_entry *
extract_candidate_graph_find_root(struct extract_candidate_graph *graph,
                                  uint64_t root_dirid) {
  if (graph->root_capacity == 0)
    return NULL;
  size_t slot =
      extract_visit_hash_value(root_dirid) & (graph->root_capacity - 1);
  while (graph->roots[slot].occupied) {
    if (graph->roots[slot].root_dirid == root_dirid)
      return &graph->roots[slot];
    slot = (slot + 1) & (graph->root_capacity - 1);
  }
  return NULL;
}

static int extract_candidate_graph_add_root_candidate(
    struct extract_candidate_graph *graph, size_t vertex) {
  if (graph->root_capacity == 0 ||
      graph->root_count >= graph->root_capacity / 2) {
    size_t capacity =
        graph->root_capacity ? graph->root_capacity * 2
                             : EXTRACT_GRAPH_INITIAL_CAPACITY;
    if (capacity < graph->root_capacity ||
        extract_candidate_graph_root_index_resize(graph, capacity) != 0)
      return -1;
  }

  uint64_t root_dirid = graph->vertices[vertex].id.root_dirid;
  size_t slot =
      extract_visit_hash_value(root_dirid) & (graph->root_capacity - 1);
  while (graph->roots[slot].occupied &&
         graph->roots[slot].root_dirid != root_dirid)
    slot = (slot + 1) & (graph->root_capacity - 1);
  if (!graph->roots[slot].occupied) {
    graph->roots[slot].root_dirid = root_dirid;
    graph->roots[slot].occupied = 1;
    graph->root_count++;
  }

  graph->vertices[vertex].next_root_candidate_plus_one =
      graph->roots[slot].head_plus_one;
  graph->roots[slot].head_plus_one = vertex + 1;
  return 0;
}

static int extract_candidate_graph_append(struct extract_candidate_graph *graph,
                                          size_t parent, size_t child) {
  if (parent == child)
    return 0;
  if (graph->edge_count == graph->edge_capacity) {
    size_t capacity = graph->edge_capacity ? graph->edge_capacity * 2 : 64;
    if (capacity < graph->edge_capacity ||
        capacity > SIZE_MAX / sizeof(*graph->edges))
      return -1;
    struct extract_candidate_edge *edges =
        realloc(graph->edges, capacity * sizeof(*edges));
    if (!edges)
      return -1;
    graph->edges = edges;
    graph->edge_capacity = capacity;
  }
  graph->edges[graph->edge_count++] = (struct extract_candidate_edge){
      .parent = parent,
      .child = child,
  };
  return 0;
}

/*
 * A node-local cache can be absent, malformed, or become unreadable without
 * preventing unrelated candidates from being attempted.  Only failures to
 * construct the cache path are treated as planner failures.
 */
static int extract_candidate_graph_read_node(
    const struct inode_instance_id *id, uint8_t key_type,
    struct total_file_tree *node) {
  char *path = extract_cache_path(id, key_type, FILE_TREE_NODE_CACHE);
  if (!path)
    return -1;

  for (int attempt = 0; attempt < 2; attempt++) {
    errno = 0;
    FILE *file = fopen(path, "rb");
    if (!file) {
      if (errno == ENOENT)
        break;
      continue;
    }

    int read_ok = fread(node, sizeof(*node), 1, file) == 1;
    fclose(file);
    if (read_ok) {
      free(path);
      return node->objectid == id->objectid && node->key_type == key_type &&
                     (key_type != INODE_ITEM ||
                      node->inode_generation == id->inode_generation)
                 ? 0
                 : 1;
    }
  }

  free(path);
  return 1;
}

static int extract_candidate_graph_open_children(
    const struct inode_instance_id *id, uint8_t key_type, FILE **file_out) {
  *file_out = NULL;
  char *path =
      extract_cache_path(id, key_type, FILE_TREE_NODE_CHILD_CACHE);
  if (!path)
    return -1;

  for (int attempt = 0; attempt < 2; attempt++) {
    errno = 0;
    *file_out = fopen(path, "rb");
    if (*file_out) {
      free(path);
      return 0;
    }
    if (errno == ENOENT)
      break;
  }

  free(path);
  return 1;
}

static int extract_candidate_graph_append_root_targets(
    struct extract_candidate_graph *graph, size_t parent,
    uint64_t target_root) {
  struct extract_candidate_root_entry *target =
      extract_candidate_graph_find_root(graph, target_root);
  if (!target)
    return 0;

  for (size_t link = target->head_plus_one; link != 0;
       link = graph->vertices[link - 1].next_root_candidate_plus_one) {
    if (extract_candidate_graph_append(graph, parent, link - 1) != 0)
      return -1;
  }
  return 0;
}

struct extract_candidate_staged_child {
  struct inode_instance_id id;
  uint8_t key_type;
};

static int extract_candidate_stage_append(
    struct extract_candidate_staged_child **children, size_t *count,
    size_t *capacity, const struct inode_instance_id *id, uint8_t key_type) {
  if (*count == *capacity) {
    size_t next_capacity = *capacity ? *capacity * 2 : 16;
    if (next_capacity < *capacity ||
        next_capacity > SIZE_MAX / sizeof(**children))
      return -1;
    struct extract_candidate_staged_child *next =
        realloc(*children, next_capacity * sizeof(*next));
    if (!next)
      return -1;
    *children = next;
    *capacity = next_capacity;
  }
  (*children)[*count] = (struct extract_candidate_staged_child){
      .id = *id,
      .key_type = key_type,
  };
  (*count)++;
  return 0;
}

static int extract_candidate_graph_scan_vertex(
    struct extract_candidate_graph *graph, size_t vertex) {
  /*
   * Adding descendants can realloc graph->vertices, so keep this vertex's
   * identity in a stable local copy for the duration of the scan.
   */
  const struct extract_candidate_vertex current = graph->vertices[vertex];
  if (current.key_type == ROOT_ITEM)
    return extract_candidate_graph_append_root_targets(
        graph, vertex, current.id.objectid);

  struct total_file_tree node;
  int node_status =
      extract_candidate_graph_read_node(&current.id, current.key_type, &node);
  if (node_status != 0)
    return node_status < 0 ? -1 : 0;
  if (node.child_count == 0)
    return 0;

  for (int attempt = 0; attempt < 2; attempt++) {
    FILE *file = NULL;
    int child_status = extract_candidate_graph_open_children(
        &current.id, current.key_type, &file);
    if (child_status != 0)
      return child_status < 0 ? -1 : 0;

    struct extract_candidate_staged_child *children = NULL;
    size_t child_count = 0;
    size_t child_capacity = 0;
    int read_error = 0;
    int stage_error = 0;
    struct child_cache child;
    for (uint32_t i = 0; i < node.child_count; i++) {
      if (fread(&child, sizeof(child), 1, file) != 1) {
        read_error = ferror(file) != 0;
        break;
      }
      if (child.type != CHILD)
        continue;
      if (extract_child_location_for_parent(&current.id, current.key_type,
                                            &child) ==
          EXTRACT_CHILD_LOCATION_STALE)
        continue;

      const struct inode_instance_id child_id = {
          .root_dirid = current.id.root_dirid,
          .objectid = child.objectid,
          .inode_generation = child.inode_generation,
      };
      if (extract_candidate_stage_append(
              &children, &child_count, &child_capacity, &child_id,
              child.key_type) != 0) {
        stage_error = 1;
        break;
      }
    }
    fclose(file);

    if (stage_error) {
      free(children);
      return -1;
    }
    if (read_error) {
      free(children);
      if (attempt == 0)
        continue;
      return 0;
    }

    int ret = 0;
    for (size_t i = 0; i < child_count; i++) {
      size_t child_vertex = 0;
      if (extract_candidate_graph_get_or_add(
              graph, &children[i].id, children[i].key_type,
              &child_vertex) != 0 ||
          extract_candidate_graph_append(graph, vertex, child_vertex) != 0) {
        ret = -1;
        break;
      }
    }
    free(children);
    return ret;
  }

  return 0;
}
static int extract_candidate_graph_load(
    struct extract_candidate_graph *graph,
    const struct inode_instance_id *candidates, size_t candidate_count) {
  memset(graph, 0, sizeof(*graph));

  for (size_t i = 0; i < candidate_count; i++) {
    size_t vertex = 0;
    if (extract_candidate_graph_get_or_add(
            graph, &candidates[i], INODE_ITEM, &vertex) != 0 ||
        vertex != i)
      goto fail;
    if (candidates[i].objectid == BTRFS_SUBVOL_ROOT_INODE &&
        extract_candidate_graph_add_root_candidate(graph, vertex) != 0)
      goto fail;
  }
  graph->candidate_count = candidate_count;

  /*
   * vertex_count grows as typed children are discovered.  The hash index
   * guarantees that every exact (identity, key type) vertex is scanned once.
   */
  for (size_t vertex = 0; vertex < graph->vertex_count; vertex++) {
    if (extract_candidate_graph_scan_vertex(graph, vertex) != 0)
      goto fail;
  }
  return 0;

fail:
  extract_candidate_graph_destroy(graph);
  return -1;
}

struct extract_candidate_order_state {
  const struct extract_candidate_graph *graph;
  const size_t *out_offsets;
  const size_t *out_children;
  size_t *indegree;
  size_t *helper_queue;
  size_t helper_head;
  size_t helper_tail;
  size_t *ready_roots;
  size_t ready_root_count;
  size_t *ready_other;
  size_t ready_other_count;
  uint8_t *candidate_queued;
  uint8_t *removed;
};

static int extract_candidate_heap_push(size_t *heap, size_t *heap_count,
                                       size_t capacity, size_t candidate) {
  if (*heap_count == capacity)
    return -1;

  size_t child = (*heap_count)++;
  while (child != 0) {
    size_t parent = (child - 1) / 2;
    if (heap[parent] <= candidate)
      break;
    heap[child] = heap[parent];
    child = parent;
  }
  heap[child] = candidate;
  return 0;
}

static size_t extract_candidate_heap_pop(size_t *heap, size_t *heap_count,
                                         const uint8_t *removed,
                                         size_t none) {
  while (*heap_count != 0) {
    size_t result = heap[0];
    size_t value = heap[--(*heap_count)];
    if (*heap_count != 0) {
      size_t parent = 0;
      for (;;) {
        size_t child = parent * 2 + 1;
        if (child >= *heap_count)
          break;
        if (child + 1 < *heap_count && heap[child + 1] < heap[child])
          child++;
        if (heap[child] >= value)
          break;
        heap[parent] = heap[child];
        parent = child;
      }
      heap[parent] = value;
    }
    if (!removed[result])
      return result;
  }
  return none;
}

static int extract_candidate_order_queue_candidate(
    struct extract_candidate_order_state *state, size_t candidate) {
  if (candidate >= state->graph->candidate_count ||
      state->removed[candidate] || state->candidate_queued[candidate] ||
      state->indegree[candidate] != 0)
    return 0;

  state->candidate_queued[candidate] = 1;
  size_t count = state->graph->candidate_count;
  if (state->graph->vertices[candidate].id.objectid ==
      BTRFS_SUBVOL_ROOT_INODE)
    return extract_candidate_heap_push(
        state->ready_roots, &state->ready_root_count, count, candidate);
  return extract_candidate_heap_push(
      state->ready_other, &state->ready_other_count, count, candidate);
}

static int extract_candidate_order_retire(
    struct extract_candidate_order_state *state, size_t vertex) {
  if (state->removed[vertex])
    return 0;
  state->removed[vertex] = 1;

  /*
   * Every stored edge contributes once to indegree and is retired once here,
   * so repeated physical references remain balanced without affecting order.
   */
  for (size_t edge = state->out_offsets[vertex];
       edge < state->out_offsets[vertex + 1]; edge++) {
    size_t child = state->out_children[edge];
    if (state->removed[child] || state->indegree[child] == 0)
      continue;
    state->indegree[child]--;
    if (state->indegree[child] != 0)
      continue;

    if (child < state->graph->candidate_count) {
      if (extract_candidate_order_queue_candidate(state, child) != 0)
        return -1;
    } else {
      if (state->helper_tail == state->graph->vertex_count)
        return -1;
      state->helper_queue[state->helper_tail++] = child;
    }
  }
  return 0;
}

static int extract_candidate_order_drain_helpers(
    struct extract_candidate_order_state *state) {
  while (state->helper_head < state->helper_tail) {
    size_t helper = state->helper_queue[state->helper_head++];
    if (extract_candidate_order_retire(state, helper) != 0)
      return -1;
  }
  return 0;
}

static size_t extract_candidate_order_next_cycle(
    const struct extract_candidate_graph *graph, const uint8_t *removed,
    size_t *cursor, int want_root) {
  while (*cursor < graph->candidate_count) {
    size_t candidate = (*cursor)++;
    int is_root =
        graph->vertices[candidate].id.objectid == BTRFS_SUBVOL_ROOT_INODE;
    if (!removed[candidate] && is_root == want_root)
      return candidate;
  }
  return graph->candidate_count;
}

static int extract_candidate_order_build(
    const struct extract_candidate_graph *graph, size_t **order_out,
    size_t *order_count_out) {
  *order_out = NULL;
  *order_count_out = 0;
  const size_t count = graph->candidate_count;
  const size_t vertex_count = graph->vertex_count;
  if (count == 0)
    return 0;
  if (vertex_count == SIZE_MAX ||
      vertex_count > SIZE_MAX / sizeof(size_t) ||
      graph->edge_count > SIZE_MAX / sizeof(size_t))
    return -1;

  size_t *order = malloc(count * sizeof(*order));
  size_t *indegree = calloc(vertex_count, sizeof(*indegree));
  size_t *out_offsets = calloc(vertex_count + 1, sizeof(*out_offsets));
  size_t *out_cursor = malloc(vertex_count * sizeof(*out_cursor));
  size_t *out_children =
      graph->edge_count
          ? malloc(graph->edge_count * sizeof(*out_children))
          : NULL;
  size_t *helper_queue = malloc(vertex_count * sizeof(*helper_queue));
  size_t *ready_roots = malloc(count * sizeof(*ready_roots));
  size_t *ready_other = malloc(count * sizeof(*ready_other));
  uint8_t *candidate_queued = calloc(count, 1);
  uint8_t *removed = calloc(vertex_count, 1);
  if (!order || !indegree || !out_offsets || !out_cursor ||
      (graph->edge_count && !out_children) || !helper_queue ||
      !ready_roots || !ready_other || !candidate_queued || !removed)
    goto fail;

  for (size_t i = 0; i < graph->edge_count; i++) {
    size_t parent = graph->edges[i].parent;
    size_t child = graph->edges[i].child;
    if (parent >= vertex_count || child >= vertex_count ||
        indegree[child] == SIZE_MAX ||
        out_offsets[parent + 1] == SIZE_MAX)
      goto fail;
    indegree[child]++;
    out_offsets[parent + 1]++;
  }
  for (size_t i = 1; i <= vertex_count; i++) {
    if (out_offsets[i] > SIZE_MAX - out_offsets[i - 1])
      goto fail;
    out_offsets[i] += out_offsets[i - 1];
  }
  memcpy(out_cursor, out_offsets, vertex_count * sizeof(*out_cursor));
  for (size_t i = 0; i < graph->edge_count; i++) {
    size_t parent = graph->edges[i].parent;
    out_children[out_cursor[parent]++] = graph->edges[i].child;
  }

  struct extract_candidate_order_state state = {
      .graph = graph,
      .out_offsets = out_offsets,
      .out_children = out_children,
      .indegree = indegree,
      .helper_queue = helper_queue,
      .ready_roots = ready_roots,
      .ready_other = ready_other,
      .candidate_queued = candidate_queued,
      .removed = removed,
  };
  for (size_t i = 0; i < vertex_count; i++) {
    if (indegree[i] != 0)
      continue;
    if (i < count) {
      if (extract_candidate_order_queue_candidate(&state, i) != 0)
        goto fail;
    } else {
      helper_queue[state.helper_tail++] = i;
    }
  }

  /*
   * Preserve the canonical FS tree root priority, even if corrupt dependency
   * data gives it an incoming edge.
   */
  for (size_t i = 0; i < count; i++) {
    const struct inode_instance_id *id = &graph->vertices[i].id;
    if (id->root_dirid != BTRFS_FS_TREE_OBJECTID ||
        id->objectid != BTRFS_SUBVOL_ROOT_INODE)
      continue;
    order[(*order_count_out)++] = i;
    if (extract_candidate_order_retire(&state, i) != 0)
      goto fail;
  }
  if (extract_candidate_order_drain_helpers(&state) != 0)
    goto fail;

  size_t cycle_root_cursor = 0;
  size_t cycle_other_cursor = 0;
  while (*order_count_out < count) {
    size_t selected = extract_candidate_heap_pop(
        ready_roots, &state.ready_root_count, removed, count);
    if (selected == count)
      selected = extract_candidate_heap_pop(
          ready_other, &state.ready_other_count, removed, count);
    if (selected == count)
      selected = extract_candidate_order_next_cycle(
          graph, removed, &cycle_root_cursor, 1);
    if (selected == count)
      selected = extract_candidate_order_next_cycle(
          graph, removed, &cycle_other_cursor, 0);
    if (selected == count)
      goto fail;

    order[(*order_count_out)++] = selected;
    if (extract_candidate_order_retire(&state, selected) != 0 ||
        extract_candidate_order_drain_helpers(&state) != 0)
      goto fail;
  }

  free(indegree);
  free(out_offsets);
  free(out_cursor);
  free(out_children);
  free(helper_queue);
  free(ready_roots);
  free(ready_other);
  free(candidate_queued);
  free(removed);
  *order_out = order;
  return 0;

fail:
  *order_count_out = 0;
  free(order);
  free(indegree);
  free(out_offsets);
  free(out_cursor);
  free(out_children);
  free(helper_queue);
  free(ready_roots);
  free(ready_other);
  free(candidate_queued);
  free(removed);
  return -1;
}
static int extract_batch_candidate(
    const struct inode_instance_id *id, struct extract_visit_set *visited,
    const struct extraction_options *options) {
  if (extract_visit_set_contains(visited, id, INODE_ITEM))
    return 0;

  for (int retry = 0; retry < 2; retry++) {
    struct extract_visit_set attempt;
    if (extract_visit_set_init(&attempt) != 0)
      return -1;
    int ret = extract_typed(id, INODE_ITEM, 0, &attempt, options) == 0 ? 0 : 1;
    if (extract_visit_set_merge_successful(visited, &attempt) != 0)
      ret = -1;
    extract_visit_set_destroy(&attempt);
    if (ret <= 0)
      return ret;
  }
  return 1;
}

int extract_all_with_options(const struct inode_instance_id *ids,
                             size_t count, size_t *failure_count,
                             const struct extraction_options *options) {
  const struct extraction_options resolved = options ? *options :
      (struct extraction_options){0};
  options = &resolved;
  if (!failure_count || (!ids && count != 0))
    return -1;
  *failure_count = 0;

  struct inode_instance_id *candidates = NULL;
  size_t candidate_count = 0;
  if (extract_candidate_ids_canonicalize(ids, count, &candidates,
                                         &candidate_count) != 0)
    return -1;

  struct extract_visit_set visited;
  if (extract_visit_set_init(&visited) != 0) {
    free(candidates);
    return -1;
  }

  if (options->regular_files_only) {
    int result = 0;
    for (size_t i = 0; i < candidate_count; i++) {
      int ret = extract_batch_candidate(&candidates[i], &visited, options);
      if (ret < 0) {
        result = -1;
        break;
      }
      if (ret > 0)
        (*failure_count)++;
    }
    extract_visit_set_destroy(&visited);
    free(candidates);
    return result;
  }

  struct extract_candidate_graph graph;
  if (extract_candidate_graph_load(&graph, candidates, candidate_count) != 0) {
    extract_visit_set_destroy(&visited);
    free(candidates);
    return -1;
  }

  size_t *order = NULL;
  size_t order_count = 0;
  if (extract_candidate_order_build(&graph, &order, &order_count) != 0) {
    extract_candidate_graph_destroy(&graph);
    extract_visit_set_destroy(&visited);
    free(candidates);
    return -1;
  }
  extract_candidate_graph_destroy(&graph);

  /*
   * Start at real file-tree roots instead of the root-tree directory alias.
   * Descendants reached by those roots are recorded by the shared set and are
   * therefore not extracted again when they later appear as standalone
   * candidates.  Candidate planning and recursive extraction validate known
   * child relationships against the latest parent cached on the child node.
   * Relationships whose latest parent cannot be established retain
   * best-effort traversal.
   */
  for (size_t i = 0; i < order_count; i++) {
    size_t candidate = order[i];
    if (extract_visit_set_contains(&visited, &candidates[candidate],
                                   INODE_ITEM))
      continue;

    int ret = extract_batch_candidate(&candidates[candidate], &visited, options);
    if (ret < 0) {
      free(order);
      extract_visit_set_destroy(&visited);
      free(candidates);
      return -1;
    }
    if (ret > 0)
      (*failure_count)++;
  }

  free(order);
  extract_visit_set_destroy(&visited);
  free(candidates);
  return 0;
}

int extract_all_without_moved(const struct inode_instance_id *ids,
                              size_t count, size_t *failure_count) {
  return extract_all_with_options(ids, count, failure_count, NULL);
}
