#define _DEFAULT_SOURCE

#include "../hdr/init.h"
#include "../hdr/layout.h"
#include "../hdr/type.h"
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <linux/btrfs.h>
#include <linux/btrfs_tree.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define BTRFS_SUPER_INFO_OFFSET 0x10000ULL
#define BTRFS_SUPER_MAGIC 0x4D5F53665248425FULL
#define MAX_IMAGE_TREE_DEPTH 16

struct image_device {
  uint64_t devid;
  char *path;
  struct btrfs_super_block super;
};

struct image_chunk_mapping {
  struct parse_btrfs_chunk chunk;
  struct stripe *stripes;
};

struct image_read_location {
  uint64_t devid;
  uint64_t physical_address;
  uint64_t logical_start;
  uint64_t logical_length;
  uint32_t raid_type;
};

static int get_btrfs_fs_info(int fd, uint32_t *nodesize, uint32_t *sectorsize,
                             uint64_t *max_id) {
  struct btrfs_ioctl_fs_info_args fi;

  memset(&fi, 0, sizeof(fi));
  if (ioctl(fd, BTRFS_IOC_FS_INFO, &fi) < 0) {
    perror("BTRFS_IOC_FS_INFO");
    return -1;
  }

  *nodesize = fi.nodesize;
  *sectorsize = fi.sectorsize;
  *max_id = fi.max_id;
  return 0;
}

static int
get_btrfs_device_info_by_devid(int fs_fd, uint64_t devid,
                               struct btrfs_ioctl_dev_info_args *out) {
  memset(out, 0, sizeof(*out));

  out->devid = devid;

  if (ioctl(fs_fd, BTRFS_IOC_DEV_INFO, out) < 0)
    return -1;

  out->path[BTRFS_DEVICE_PATH_NAME_MAX - 1] = '\0';

  return 0;
}

static void uuid_to_string(const uint8_t uuid[16], char out[37]) {
  char *p = out;

  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      *p++ = '-';

    sprintf(p, "%02x", uuid[i]);
    p += 2;
  }

  *p = '\0';
}

static int write_dev_cache(uint64_t devid, const char *path) {
  if (!path)
    return -1;
  if (ensure_cache_dir(DEVICE_CACHE_DIR) != 0)
    return -1;

  char *dev_cache_path = device_cache_path(devid);
  FILE *dev_cache = dev_cache_path ? fopen(dev_cache_path, "wb") : NULL;
  free(dev_cache_path);
  if (!dev_cache)
    return -1;

  int ok = 0;
  if (fwrite(&devid, sizeof(uint64_t), 1, dev_cache) != 1)
    ok = -1;
  if (ok == 0 && fwrite(path, strlen(path) + 1, 1, dev_cache) != 1)
    ok = -1;

  if (fclose(dev_cache) != 0)
    ok = -1;
  return ok;
}

static void parse_btrfs_device_info(int fs_fd, uint64_t devid) {
  struct btrfs_ioctl_dev_info_args dev;
  char uuid[37];

  if (get_btrfs_device_info_by_devid(fs_fd, devid, &dev) < 0) {
    fprintf(stderr, "  device devid=%" PRIu64 " lookup failed: %s\n", devid,
            strerror(errno));
    return;
  }

  uuid_to_string(dev.uuid, uuid);
  if (write_dev_cache((uint64_t)dev.devid, (const char *)dev.path) != 0)
    perror("write_dev_cache");
}

struct dev_entry *get_dev_entry(uint64_t devid) {
  char *dev_cache_path = device_cache_path(devid);
  FILE *dev_cache = dev_cache_path ? fopen(dev_cache_path, "rb") : NULL;
  if (!dev_cache) {
    perror("fopen");
    free(dev_cache_path);
    return NULL;
  }
  if (fseek(dev_cache, 0, SEEK_END) != 0) {
    fclose(dev_cache);
    free(dev_cache_path);
    return NULL;
  }
  long cache_size = ftell(dev_cache);
  if (cache_size <= (long)sizeof(uint64_t) ||
      fseek(dev_cache, 0, SEEK_SET) != 0) {
    fclose(dev_cache);
    free(dev_cache_path);
    return NULL;
  }
  size_t size = (size_t)cache_size;
  size_t path_size = size - sizeof(uint64_t);
  if (path_size > SIZE_MAX - sizeof(struct dev_entry) - 1) {
    fclose(dev_cache);
    free(dev_cache_path);
    return NULL;
  }
  struct dev_entry *dev_entry =
      calloc(1, sizeof(struct dev_entry) + path_size + 1);
  if (!dev_entry) {
    perror("calloc");
    fclose(dev_cache);
    free(dev_cache_path);
    return NULL;
  }
  dev_entry->path = (char *)(dev_entry + 1);
  uint64_t cached_devid = 0;
  if (fread(&cached_devid, sizeof(cached_devid), 1, dev_cache) != 1 ||
      fread(dev_entry->path, path_size, 1, dev_cache) != 1 ||
      cached_devid != devid ||
      memchr(dev_entry->path, '\0', path_size) == NULL) {
    free(dev_entry);
    dev_entry = NULL;
  } else {
    dev_entry->devid = cached_devid;
  }
  fclose(dev_cache);
  free(dev_cache_path);
  return dev_entry;
}

FILE *open_cached_device(uint64_t devid) {
  struct dev_entry *dev_entry = get_dev_entry(devid);
  if (!dev_entry)
    return NULL;

  FILE *file = fopen(dev_entry->path, "rb");
  free(dev_entry);
  return file;
}

static int dev_bitmap_test(const uint64_t *bmp, uint64_t max_dev_id,
                           uint64_t devid) {
  if (!bmp || max_dev_id == 0 || devid == 0 || devid > max_dev_id)
    return 0;
  uint64_t b = devid - 1;
  size_t w = (size_t)(b / 64);
  unsigned sh = (unsigned)(b % 64);
  return (int)((bmp[w] >> sh) & 1);
}

static void dev_bitmap_set(uint64_t *bmp, uint64_t max_dev_id, uint64_t devid) {
  if (!bmp || max_dev_id == 0 || devid == 0 || devid > max_dev_id)
    return;
  uint64_t b = devid - 1;
  size_t w = (size_t)(b / 64);
  unsigned sh = (unsigned)(b % 64);
  bmp[w] |= (uint64_t)1 << sh;
}

static int check_chunk_data_type(uint64_t type) {
  if (type & BTRFS_BLOCK_GROUP_SYSTEM)
    return SYSTEM_CHUNK_TYPE;
  if (type & BTRFS_BLOCK_GROUP_DATA)
    return DATA_CHUNK_TYPE;
  if (type & BTRFS_BLOCK_GROUP_METADATA)
    return METADATA_CHUNK_TYPE;
  return UNKNOWN_CHUNK_DATA_TYPE;
}

int check_chunk_raid_type(uint64_t type) {
  uint64_t profile = type & BTRFS_BLOCK_GROUP_PROFILE_MASK;

  if (profile == 0)
    return SINGLE_CHUNK_TYPE;
  if (profile & BTRFS_BLOCK_GROUP_DUP)
    return DUP_CHUNK_TYPE;
  if (profile & BTRFS_BLOCK_GROUP_RAID0)
    return RAID0_CHUNK_TYPE;
  if (profile & BTRFS_BLOCK_GROUP_RAID1)
    return RAID1_CHUNK_TYPE;
  if (profile & BTRFS_BLOCK_GROUP_RAID10)
    return RAID10_CHUNK_TYPE;
  if (profile & BTRFS_BLOCK_GROUP_RAID5)
    return RAID5_CHUNK_TYPE;
  if (profile & BTRFS_BLOCK_GROUP_RAID6)
    return RAID6_CHUNK_TYPE;
  if (profile & BTRFS_BLOCK_GROUP_RAID1C3)
    return RAID1C3_CHUNK_TYPE;
  if (profile & BTRFS_BLOCK_GROUP_RAID1C4)
    return RAID1C4_CHUNK_TYPE;
  return UNKNOWN_CHUNK_RAID_TYPE;
}

static int write_chunk_item(uint64_t logical_address,
                            const struct btrfs_chunk *chunk,
                            uint32_t *stripe_index, int fs_fd,
                            uint64_t *dev_bitmap, uint64_t max_dev_id) {
  struct parse_btrfs_chunk chunk_data;
  memset(&chunk_data, 0, sizeof(chunk_data));

  char *cache_path = chunk_tree_cache_path(CHUNK_TREE_CACHE);
  char *stripe_path = chunk_tree_cache_path(CHUNK_TREE_STRIPE_CACHE);

  FILE *chunk_tree_cache = cache_path ? fopen(cache_path, "ab") : NULL;
  FILE *chunk_tree_stripe_cache = stripe_path ? fopen(stripe_path, "ab") : NULL;

  free(cache_path);
  free(stripe_path);

  if (!chunk_tree_cache || !chunk_tree_stripe_cache) {
    perror("fopen");
    if (chunk_tree_cache)
      fclose(chunk_tree_cache);
    if (chunk_tree_stripe_cache)
      fclose(chunk_tree_stripe_cache);
    return -1;
  }

  chunk_data.logical_address = logical_address;
  chunk_data.length = le64toh(chunk->length);
  chunk_data.data_type = check_chunk_data_type(le64toh(chunk->type));
  chunk_data.raid_type = check_chunk_raid_type(le64toh(chunk->type));
  chunk_data.stripe_length = le64toh(chunk->stripe_len);
  chunk_data.num_stripes = le16toh(chunk->num_stripes);
  chunk_data.sub_stripes = le16toh(chunk->sub_stripes);
  chunk_data.stripe_index = *stripe_index;
  *stripe_index += chunk_data.num_stripes;

  if (fwrite(&chunk_data, sizeof(chunk_data), 1, chunk_tree_cache) != 1) {
    perror("fwrite");
    fclose(chunk_tree_cache);
    fclose(chunk_tree_stripe_cache);
    return -1;
  }
  fclose(chunk_tree_cache);

  struct stripe *stripes = calloc(chunk_data.num_stripes, sizeof(*stripes));
  if (!stripes) {
    perror("calloc");
    fclose(chunk_tree_stripe_cache);
    return -1;
  }

  for (uint16_t i = 0; i < chunk_data.num_stripes; i++) {
    struct btrfs_stripe *stripe =
        (struct btrfs_stripe *)((char *)chunk +
                                offsetof(struct btrfs_chunk, stripe) +
                                (size_t)i * sizeof(struct btrfs_stripe));
    stripes[i].devid = le64toh(stripe->devid);
    stripes[i].physical_address = le64toh(stripe->offset);
    if (fwrite(&stripes[i], sizeof(struct stripe), 1,
               chunk_tree_stripe_cache) != 1) {
      perror("fwrite");
      free(stripes);
      fclose(chunk_tree_stripe_cache);
      return -1;
    }
    if (ensure_cache_dir(DEVICE_CACHE_DIR) != 0) {
      perror("ensure_cache_dir");
      fclose(chunk_tree_stripe_cache);
      free(stripes);
      return -1;
    }

    uint64_t did = stripes[i].devid;
    if (fs_fd >= 0 && dev_bitmap && max_dev_id > 0 && did > 0 &&
        did <= max_dev_id) {
      if (!dev_bitmap_test(dev_bitmap, max_dev_id, did)) {
        parse_btrfs_device_info(fs_fd, did);
        dev_bitmap_set(dev_bitmap, max_dev_id, did);
      }
    }
  }

  fclose(chunk_tree_stripe_cache);
  free(stripes);
  return 0;
}

static void parse_chunk_item(const struct btrfs_ioctl_search_header *sh,
                             const void *data, uint32_t *stripe_index,
                             int fs_fd, uint64_t *dev_bitmap,
                             uint64_t max_dev_id) {
  const struct btrfs_chunk *chunk = data;
  (void)write_chunk_item(sh->offset, chunk, stripe_index, fs_fd, dev_bitmap,
                         max_dev_id);
}

static char *duplicate_string(const char *value) {
  size_t len = strlen(value) + 1;
  char *copy = malloc(len);
  if (copy)
    memcpy(copy, value, len);
  return copy;
}

static char *join_path(const char *dir, const char *name) {
  size_t dir_len = strlen(dir);
  size_t name_len = strlen(name);
  int need_slash = dir_len > 0 && dir[dir_len - 1] != '/';
  char *path = malloc(dir_len + (size_t)need_slash + name_len + 1);
  if (!path)
    return NULL;

  memcpy(path, dir, dir_len);
  if (need_slash)
    path[dir_len++] = '/';
  memcpy(path + dir_len, name, name_len + 1);
  return path;
}

static void free_image_devices(struct image_device *devices, size_t count) {
  if (!devices)
    return;
  for (size_t i = 0; i < count; i++)
    free(devices[i].path);
  free(devices);
}

static void free_image_mappings(struct image_chunk_mapping *mappings,
                                size_t count) {
  if (!mappings)
    return;
  for (size_t i = 0; i < count; i++)
    free(mappings[i].stripes);
  free(mappings);
}

static int read_image_super(const char *path, struct btrfs_super_block *super) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return -1;

  int ret = 0;
  if (fseeko(file, (off_t)BTRFS_SUPER_INFO_OFFSET, SEEK_SET) != 0 ||
      fread(super, sizeof(*super), 1, file) != 1)
    ret = -1;
  fclose(file);
  if (ret != 0)
    return -1;

  if (le64toh(super->magic) != BTRFS_SUPER_MAGIC)
    return 1;
  return 0;
}

static int image_devid_exists(const struct image_device *devices, size_t count,
                              uint64_t devid) {
  for (size_t i = 0; i < count; i++)
    if (devices[i].devid == devid)
      return 1;
  return 0;
}

static int add_image_device(struct image_device **devices, size_t *count,
                            const char *path,
                            const struct btrfs_super_block *super) {
  uint64_t devid = le64toh(super->dev_item.devid);
  if (devid == 0) {
    fprintf(stderr, "Invalid image device id in %s\n", path);
    return -1;
  }
  if (image_devid_exists(*devices, *count, devid)) {
    fprintf(stderr, "Duplicate image for devid=%" PRIu64 ": %s\n", devid, path);
    return -1;
  }

  struct image_device *tmp =
      realloc(*devices, (*count + 1) * sizeof(struct image_device));
  if (!tmp)
    return -1;
  *devices = tmp;

  struct image_device *dev = &(*devices)[*count];
  memset(dev, 0, sizeof(*dev));
  dev->devid = devid;
  dev->path = duplicate_string(path);
  if (!dev->path)
    return -1;
  memcpy(&dev->super, super, sizeof(*super));
  (*count)++;

  if (write_dev_cache(devid, path) != 0) {
    perror("write_dev_cache");
    return -1;
  }
  return 0;
}

static int scan_image_devices(const char *image_dir,
                              struct image_device **devices,
                              size_t *device_count,
                              struct btrfs_super_block *primary_super) {
  DIR *dir = opendir(image_dir);
  if (!dir) {
    perror("opendir");
    return -1;
  }

  uint8_t fsid[BTRFS_FSID_SIZE] = {0};
  int fsid_set = 0;
  *devices = NULL;
  *device_count = 0;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    char *path = join_path(image_dir, entry->d_name);
    if (!path) {
      closedir(dir);
      return -1;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
      free(path);
      continue;
    }

    struct btrfs_super_block super;
    int ret = read_image_super(path, &super);
    if (ret != 0) {
      free(path);
      continue;
    }

    if (!fsid_set) {
      memcpy(fsid, super.fsid, BTRFS_FSID_SIZE);
      memcpy(primary_super, &super, sizeof(super));
      fsid_set = 1;
    } else if (memcmp(fsid, super.fsid, BTRFS_FSID_SIZE) != 0) {
      fprintf(stderr, "Skipping image with different fs uuid: %s\n", path);
      free(path);
      continue;
    }

    if (add_image_device(devices, device_count, path, &super) != 0) {
      free(path);
      closedir(dir);
      return -1;
    }
    free(path);
  }

  closedir(dir);

  if (*device_count == 0) {
    fprintf(stderr, "No Btrfs image files found in %s\n", image_dir);
    return -1;
  }

  uint64_t expected_devices = le64toh(primary_super->num_devices);
  if (expected_devices > *device_count) {
    fprintf(stderr,
            "Image set is incomplete: superblock expects %" PRIu64
            " devices, found %zu image files\n",
            expected_devices, *device_count);
    return -1;
  }

  set_current_fs_uuid(primary_super->fsid);
  return 0;
}

static int add_image_chunk_mapping(struct image_chunk_mapping **mappings,
                                   size_t *mapping_count,
                                   uint64_t logical_address,
                                   const struct btrfs_chunk *chunk) {
  uint16_t num_stripes = le16toh(chunk->num_stripes);
  struct image_chunk_mapping *tmp = realloc(
      *mappings, (*mapping_count + 1) * sizeof(struct image_chunk_mapping));
  if (!tmp)
    return -1;
  *mappings = tmp;

  struct image_chunk_mapping *mapping = &(*mappings)[*mapping_count];
  memset(mapping, 0, sizeof(*mapping));
  mapping->chunk.logical_address = logical_address;
  mapping->chunk.length = le64toh(chunk->length);
  mapping->chunk.data_type = check_chunk_data_type(le64toh(chunk->type));
  mapping->chunk.raid_type = check_chunk_raid_type(le64toh(chunk->type));
  mapping->chunk.stripe_length = le64toh(chunk->stripe_len);
  mapping->chunk.num_stripes = num_stripes;
  mapping->chunk.sub_stripes = le16toh(chunk->sub_stripes);

  mapping->stripes = calloc(num_stripes, sizeof(struct stripe));
  if (!mapping->stripes)
    return -1;

  for (uint16_t i = 0; i < num_stripes; i++) {
    const struct btrfs_stripe *stripe =
        (const struct btrfs_stripe *)((const char *)chunk +
                                      offsetof(struct btrfs_chunk, stripe) +
                                      (size_t)i * sizeof(struct btrfs_stripe));
    mapping->stripes[i].devid = le64toh(stripe->devid);
    mapping->stripes[i].physical_address = le64toh(stripe->offset);
  }

  (*mapping_count)++;
  return 0;
}

static const char *image_path_for_devid(const struct image_device *devices,
                                        size_t device_count, uint64_t devid) {
  for (size_t i = 0; i < device_count; i++)
    if (devices[i].devid == devid)
      return devices[i].path;
  return NULL;
}

static int read_physical_from_image(const struct image_device *devices,
                                    size_t device_count, uint64_t devid,
                                    uint64_t physical, uint32_t size,
                                    void *buf) {
  const char *path = image_path_for_devid(devices, device_count, devid);
  if (!path)
    return -1;

  FILE *file = fopen(path, "rb");
  if (!file)
    return -1;
  int ok = 0;
  if (fseeko(file, (off_t)physical, SEEK_SET) == 0 &&
      fread(buf, size, 1, file) == 1)
    ok = 1;
  fclose(file);
  return ok ? 0 : -1;
}

static int calculate_striped_read(const struct image_chunk_mapping *mapping,
                                  uint64_t chunk_offset, uint32_t size,
                                  uint16_t data_stripes, uint16_t mirror_count,
                                  uint16_t *stripe_index,
                                  uint64_t *stripe_offset) {
  uint64_t stripe_len = mapping->chunk.stripe_length;
  if (stripe_len == 0 || data_stripes == 0 || mirror_count == 0)
    return -1;

  uint64_t offset_in_stripe = chunk_offset % stripe_len;
  if (offset_in_stripe + size > stripe_len) {
    fprintf(stderr,
            "Image bootstrap read crosses RAID stripe boundary; logical "
            "chunk offset=0x%" PRIx64 ", size=%" PRIu32 "\n",
            chunk_offset, size);
    return -1;
  }

  uint64_t stripe_nr = chunk_offset / stripe_len;
  uint16_t data_index = (uint16_t)(stripe_nr % data_stripes);
  *stripe_index = (uint16_t)(data_index * mirror_count);
  *stripe_offset = (stripe_nr / data_stripes) * stripe_len + offset_in_stripe;
  return 0;
}

static int read_logical_from_images(const struct image_chunk_mapping *mappings,
                                    size_t mapping_count,
                                    const struct image_device *devices,
                                    size_t device_count,
                                    uint64_t logical_address, uint32_t size,
                                    void *buf,
                                    struct image_read_location *location) {
  for (size_t i = 0; i < mapping_count; i++) {
    const struct image_chunk_mapping *mapping = &mappings[i];
    uint64_t chunk_start = mapping->chunk.logical_address;
    uint64_t chunk_end = chunk_start + mapping->chunk.length;
    if (logical_address < chunk_start || logical_address + size > chunk_end)
      continue;

    uint64_t chunk_offset = logical_address - chunk_start;
    uint16_t start_stripe = 0;
    uint16_t mirror_count = 1;
    uint64_t stripe_offset = chunk_offset;

    if (mapping->chunk.raid_type == RAID0_CHUNK_TYPE) {
      if (calculate_striped_read(mapping, chunk_offset, size,
                                 mapping->chunk.num_stripes, 1, &start_stripe,
                                 &stripe_offset) != 0)
        return -1;
    } else if (mapping->chunk.raid_type == RAID10_CHUNK_TYPE) {
      mirror_count = mapping->chunk.sub_stripes;
      uint16_t data_stripes =
          mirror_count ? mapping->chunk.num_stripes / mirror_count : 0;
      if (calculate_striped_read(mapping, chunk_offset, size, data_stripes,
                                 mirror_count, &start_stripe,
                                 &stripe_offset) != 0)
        return -1;
    } else if (mapping->chunk.raid_type == RAID5_CHUNK_TYPE ||
               mapping->chunk.raid_type == RAID6_CHUNK_TYPE) {
      fprintf(stderr, "Unsupported image bootstrap RAID profile: %" PRIu32 "\n",
              mapping->chunk.raid_type);
      return -1;
    } else if (mapping->chunk.raid_type == DUP_CHUNK_TYPE ||
               mapping->chunk.raid_type == RAID1_CHUNK_TYPE ||
               mapping->chunk.raid_type == RAID1C3_CHUNK_TYPE ||
               mapping->chunk.raid_type == RAID1C4_CHUNK_TYPE) {
      mirror_count = mapping->chunk.num_stripes;
    }

    for (uint16_t j = 0; j < mirror_count; j++) {
      uint16_t stripe_nr = (uint16_t)(start_stripe + j);
      if (stripe_nr >= mapping->chunk.num_stripes)
        break;
      const struct stripe *stripe = &mapping->stripes[stripe_nr];
      uint64_t physical = stripe->physical_address + stripe_offset;
      if (read_physical_from_image(devices, device_count, stripe->devid,
                                   physical, size, buf) == 0) {
        if (location) {
          location->devid = stripe->devid;
          location->physical_address = physical;
          location->logical_start = chunk_start;
          location->logical_length = mapping->chunk.length;
          location->raid_type = mapping->chunk.raid_type;
        }
        return 0;
      }
    }
  }

  fprintf(stderr, "Failed to map logical address 0x%" PRIx64 "\n",
          logical_address);
  return -1;
}

static int parse_sys_chunk_array(const struct btrfs_super_block *super,
                                 struct image_chunk_mapping **mappings,
                                 size_t *mapping_count,
                                 uint32_t *stripe_index) {
  (void)stripe_index;
  const uint8_t *p = super->sys_chunk_array;
  const uint8_t *end = p + le32toh(super->sys_chunk_array_size);

  while (p + sizeof(struct btrfs_disk_key) <= end) {
    const struct btrfs_disk_key *key = (const struct btrfs_disk_key *)p;
    p += sizeof(*key);

    if (p + offsetof(struct btrfs_chunk, stripe) > end) {
      fprintf(stderr, "Truncated system chunk array\n");
      return -1;
    }

    const struct btrfs_chunk *chunk = (const struct btrfs_chunk *)p;
    uint16_t num_stripes = le16toh(chunk->num_stripes);
    size_t chunk_size = offsetof(struct btrfs_chunk, stripe) +
                        (size_t)num_stripes * sizeof(struct btrfs_stripe);
    if (num_stripes == 0 || p + chunk_size > end) {
      fprintf(stderr, "Invalid system chunk array entry\n");
      return -1;
    }

    if (key->type == BTRFS_CHUNK_ITEM_KEY) {
      uint64_t logical = le64toh(key->offset);
      if (add_image_chunk_mapping(mappings, mapping_count, logical, chunk) != 0)
        return -1;
    }

    p += chunk_size;
  }

  return 0;
}

static int parse_image_chunk_tree_block(
    const struct image_chunk_mapping *mappings, size_t mapping_count,
    const struct image_device *devices, size_t device_count,
    uint64_t logical_address, uint32_t nodesize, uint32_t *stripe_index,
    int depth) {
  if (depth > MAX_IMAGE_TREE_DEPTH) {
    fprintf(stderr, "Chunk tree recursion is too deep at 0x%" PRIx64 "\n",
            logical_address);
    return -1;
  }

  uint8_t *buf = malloc(nodesize);
  if (!buf)
    return -1;

  struct image_read_location read_location;
  memset(&read_location, 0, sizeof(read_location));
  if (read_logical_from_images(mappings, mapping_count, devices, device_count,
                               logical_address, nodesize, buf,
                               &read_location) != 0) {
    free(buf);
    return -1;
  }

  const struct btrfs_header *header = (const struct btrfs_header *)buf;
  uint32_t nritems = le32toh(header->nritems);
  uint8_t level = header->level;
  uint64_t owner = le64toh(header->owner);
  uint64_t bytenr = le64toh(header->bytenr);

  if (owner != BTRFS_CHUNK_TREE_OBJECTID) {
    fprintf(stderr,
            "Unexpected tree owner at 0x%" PRIx64 ": expected %" PRIu64
            ", got %" PRIu64 " (read devid=%" PRIu64 ", physical=0x%" PRIx64
            ", map logical=0x%" PRIx64 "..0x%" PRIx64 ", raid=%" PRIu32
            ", header bytenr=0x%" PRIx64 ")\n",
            logical_address, (uint64_t)BTRFS_CHUNK_TREE_OBJECTID, owner,
            read_location.devid, read_location.physical_address,
            read_location.logical_start,
            read_location.logical_start + read_location.logical_length,
            read_location.raid_type, bytenr);
    free(buf);
    return -1;
  }

  if (level == 0) {
    size_t max_items =
        (nodesize - sizeof(struct btrfs_header)) / sizeof(struct btrfs_item);
    if (nritems > max_items) {
      fprintf(stderr, "Invalid chunk tree leaf item count: %" PRIu32 "\n",
              nritems);
      free(buf);
      return -1;
    }

    const struct btrfs_item *items =
        (const struct btrfs_item *)(buf + sizeof(struct btrfs_header));
    for (uint32_t i = 0; i < nritems; i++) {
      const struct btrfs_item *item = &items[i];
      uint64_t objectid = le64toh(item->key.objectid);
      uint8_t type = item->key.type;
      uint64_t offset = le64toh(item->key.offset);
      uint32_t item_offset = le32toh(item->offset);
      uint32_t item_size = le32toh(item->size);

      if ((uint64_t)sizeof(struct btrfs_header) + item_offset + item_size >
          nodesize) {
        fprintf(stderr, "Invalid chunk tree leaf item boundary\n");
        free(buf);
        return -1;
      }

      if (objectid == BTRFS_FIRST_CHUNK_TREE_OBJECTID &&
          type == BTRFS_CHUNK_ITEM_KEY) {
        const struct btrfs_chunk *chunk =
            (const struct btrfs_chunk *)(buf + sizeof(struct btrfs_header) +
                                         item_offset);
        if (write_chunk_item(offset, chunk, stripe_index, -1, NULL, 0) != 0) {
          free(buf);
          return -1;
        }
      }
    }
  } else {
    size_t max_ptrs =
        (nodesize - sizeof(struct btrfs_header)) / sizeof(struct btrfs_key_ptr);
    if (nritems > max_ptrs) {
      fprintf(stderr, "Invalid chunk tree node item count: %" PRIu32 "\n",
              nritems);
      free(buf);
      return -1;
    }

    const struct btrfs_key_ptr *ptrs =
        (const struct btrfs_key_ptr *)(buf + sizeof(struct btrfs_header));
    for (uint32_t i = 0; i < nritems; i++) {
      uint64_t blockptr = le64toh(ptrs[i].blockptr);
      if (parse_image_chunk_tree_block(mappings, mapping_count, devices,
                                       device_count, blockptr, nodesize,
                                       stripe_index, depth + 1) != 0) {
        free(buf);
        return -1;
      }
    }
  }

  free(buf);
  return 0;
}

static int reset_current_chunk_cache(void) {
  char *cache_dir = cache_dir_path();
  struct stat st;
  if (cache_dir && stat(cache_dir, &st) == 0)
    if (remove_cache_files(cache_dir) != 0) {
      perror("remove_cache_files");
      free(cache_dir);
      return -1;
    }
  free(cache_dir);

  if (ensure_cache_dir(CHUNK_TREE_CACHE_DIR) != 0) {
    perror("ensure_chunk_tree_cache_dir");
    return -1;
  }
  return 0;
}

int parse_btrfs_current_chunk_tree_from_images(const char *image_dir,
                                               uint8_t fs_uuid[16]) {
  if (reset_current_chunk_cache() != 0)
    return 1;

  struct image_device *devices = NULL;
  size_t device_count = 0;
  struct btrfs_super_block primary_super;
  memset(&primary_super, 0, sizeof(primary_super));

  if (scan_image_devices(image_dir, &devices, &device_count, &primary_super) !=
      0) {
    free_image_devices(devices, device_count);
    return 1;
  }

  if (fs_uuid)
    memcpy(fs_uuid, primary_super.fsid, BTRFS_FSID_SIZE);
  set_current_fs_generation(le64toh(primary_super.generation));
  set_current_fs_profile(le32toh(primary_super.sectorsize),
                         le16toh(primary_super.csum_type));

  uint32_t nodesize = le32toh(primary_super.nodesize);
  if (nodesize < sizeof(struct btrfs_header) || nodesize > 1024 * 1024) {
    fprintf(stderr, "Unsupported Btrfs nodesize: %" PRIu32 "\n", nodesize);
    free_image_devices(devices, device_count);
    return 1;
  }

  struct image_chunk_mapping *mappings = NULL;
  size_t mapping_count = 0;
  uint32_t stripe_index = 0;

  if (parse_sys_chunk_array(&primary_super, &mappings, &mapping_count,
                            &stripe_index) != 0 ||
      mapping_count == 0) {
    fprintf(stderr, "Failed to parse system chunk array\n");
    free_image_mappings(mappings, mapping_count);
    free_image_devices(devices, device_count);
    return 1;
  }

  uint64_t chunk_root = le64toh(primary_super.chunk_root);
  if (parse_image_chunk_tree_block(mappings, mapping_count, devices,
                                   device_count, chunk_root, nodesize,
                                   &stripe_index, 0) != 0) {
    free_image_mappings(mappings, mapping_count);
    free_image_devices(devices, device_count);
    return 1;
  }

  free_image_mappings(mappings, mapping_count);
  free_image_devices(devices, device_count);
  return 0;
}

int read_btrfs_image_identity(const char *image_dir, uint8_t fs_uuid[16],
                              uint64_t *generation) {
  struct image_device *devices = NULL;
  size_t device_count = 0;
  struct btrfs_super_block primary_super;
  memset(&primary_super, 0, sizeof(primary_super));

  if (generation)
    *generation = 0;

  if (scan_image_devices(image_dir, &devices, &device_count, &primary_super) !=
      0) {
    free_image_devices(devices, device_count);
    return -1;
  }

  if (fs_uuid)
    memcpy(fs_uuid, primary_super.fsid, BTRFS_FSID_SIZE);
  if (generation)
    *generation = le64toh(primary_super.generation);
  set_current_fs_profile(le32toh(primary_super.sectorsize),
                         le16toh(primary_super.csum_type));

  free_image_devices(devices, device_count);
  return 0;
}

int parse_btrfs_current_chunk_tree(void) {

  if (reset_current_chunk_cache() != 0)
    return 1;

  int fd = open(disk_path, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    perror("open");
    return 1;
  }

  const size_t buf_size = 1024 * 1024;
  struct btrfs_ioctl_search_args_v2 *args = calloc(1, sizeof(*args) + buf_size);

  if (!args) {
    perror("calloc");
    close(fd);
    return 1;
  }

  uint32_t fs_nodesize = 0;
  uint32_t fs_sectorsize = 0;
  uint64_t max_dev_id = 0;

  if (get_btrfs_fs_info(fd, &fs_nodesize, &fs_sectorsize, &max_dev_id) < 0) {
    free(args);
    close(fd);
    return 1;
  }

  size_t bmp_words =
      max_dev_id ? (size_t)((max_dev_id + 64 - 1) / 64) : (size_t)0;
  uint64_t *dev_bitmap = bmp_words ? calloc(bmp_words, sizeof(uint64_t)) : NULL;
  if (bmp_words != 0 && !dev_bitmap) {
    perror("calloc");
    free(args);
    close(fd);
    return 1;
  }

  args->buf_size = buf_size;

  args->key.tree_id = BTRFS_CHUNK_TREE_OBJECTID;

  args->key.min_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
  args->key.max_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;

  args->key.min_type = BTRFS_CHUNK_ITEM_KEY;
  args->key.max_type = BTRFS_CHUNK_ITEM_KEY;

  args->key.min_offset = 0;
  args->key.max_offset = UINT64_MAX;

  args->key.min_transid = 0;
  args->key.max_transid = UINT64_MAX;

  for (;;) {
    args->key.nr_items = UINT32_MAX;
    args->buf_size = buf_size;

    if (ioctl(fd, BTRFS_IOC_TREE_SEARCH_V2, args) < 0) {
      perror("BTRFS_IOC_TREE_SEARCH_V2");
      free(dev_bitmap);
      free(args);
      close(fd);
      return 1;
    }

    if (args->key.nr_items == 0)
      break;

    char *p = (char *)args->buf;
    struct btrfs_ioctl_search_header last = {0};
    uint32_t stripe_index = 0;

    for (uint32_t i = 0; i < args->key.nr_items; i++) {
      struct btrfs_ioctl_search_header *sh =
          (struct btrfs_ioctl_search_header *)p;

      p += sizeof(*sh);

      if (sh->type == BTRFS_CHUNK_ITEM_KEY) {
        parse_chunk_item(sh, p, &stripe_index, fd, dev_bitmap, max_dev_id);
      }

      last = *sh;
      p += sh->len;
    }

    if (last.offset == UINT64_MAX)
      break;

    args->key.min_offset = last.offset + 1;
  }

  free(dev_bitmap);
  free(args);
  close(fd);
  return 0;
}
