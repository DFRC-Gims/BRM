#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../hdr/layout.h"
#include "../hdr/struct.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mkdir_one(const char *path, mode_t mode) {
  if (mkdir(path, mode) == 0)
    return 0;
  if (errno == EEXIST && is_dir(path))
    return 0;
  return -1;
}

int worker_thread_count = 1;
int include_other_fs_uuid = 0;
static const char *cache_base_path = NULL;
static uint8_t current_fs_uuid[BTRFS_UUID_SIZE];
static uint8_t selected_fs_uuid[BTRFS_UUID_SIZE];
static uint64_t current_fs_generation;
static uint32_t current_sectorsize = 4096;
static uint16_t current_csum_type = 0;
static int current_fs_uuid_set;
static int selected_fs_uuid_set;

void set_worker_thread_count(int count) {
  if (count < 1)
    count = 1;
  worker_thread_count = count;
}

void set_include_other_fs_uuid(int include) {
  include_other_fs_uuid = include ? 1 : 0;
}

void set_cache_base_path(const char *path) { cache_base_path = path; }

const char *current_cache_base_path(void) {
  return cache_base_path ? cache_base_path : output_path;
}

void set_current_fs_generation(uint64_t generation) {
  current_fs_generation = generation;
}

uint64_t current_fs_generation_limit(void) { return current_fs_generation; }

void set_current_fs_profile(uint32_t sectorsize, uint16_t csum_type) {
  if (sectorsize != 0)
    current_sectorsize = sectorsize;
  current_csum_type = csum_type;
}

uint32_t current_fs_sectorsize(void) { return current_sectorsize; }

uint16_t current_fs_csum_type(void) { return current_csum_type; }

static int uuid_equal(const uint8_t lhs[BTRFS_UUID_SIZE],
                      const uint8_t rhs[BTRFS_UUID_SIZE]) {
  return memcmp(lhs, rhs, BTRFS_UUID_SIZE) == 0;
}

int should_parse_fs_uuid(const uint8_t fs_uuid[BTRFS_UUID_SIZE]) {
  if (!fs_uuid || !current_fs_uuid_set)
    return 1;
  if (uuid_equal(fs_uuid, current_fs_uuid))
    return 1;
  return include_other_fs_uuid;
}

static void uuid_to_string(const uint8_t uuid[BTRFS_UUID_SIZE], char out[37]) {
  static const char hex[] = "0123456789abcdef";
  size_t pos = 0;

  for (size_t i = 0; i < BTRFS_UUID_SIZE; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      out[pos++] = '-';
    out[pos++] = hex[(uuid[i] >> 4) & 0x0f];
    out[pos++] = hex[uuid[i] & 0x0f];
  }
  out[pos] = '\0';
}

static int selected_uuid_is_current(void) {
  if (!selected_fs_uuid_set || !current_fs_uuid_set)
    return 1;
  return uuid_equal(selected_fs_uuid, current_fs_uuid);
}

void set_current_fs_uuid(const uint8_t fs_uuid[BTRFS_UUID_SIZE]) {
  if (!fs_uuid)
    return;

  memcpy(current_fs_uuid, fs_uuid, BTRFS_UUID_SIZE);
  current_fs_uuid_set = 1;
  if (!selected_fs_uuid_set)
    select_current_file_tree_fs_uuid();
}

void select_file_tree_fs_uuid(const uint8_t fs_uuid[BTRFS_UUID_SIZE]) {
  if (!fs_uuid) {
    select_current_file_tree_fs_uuid();
    return;
  }

  memcpy(selected_fs_uuid, fs_uuid, BTRFS_UUID_SIZE);
  selected_fs_uuid_set = 1;
}

void select_current_file_tree_fs_uuid(void) {
  if (current_fs_uuid_set) {
    memcpy(selected_fs_uuid, current_fs_uuid, BTRFS_UUID_SIZE);
    selected_fs_uuid_set = 1;
  } else {
    selected_fs_uuid_set = 0;
  }
}

const uint8_t *active_file_tree_fs_uuid(void) {
  if (selected_fs_uuid_set)
    return selected_fs_uuid;
  if (current_fs_uuid_set)
    return current_fs_uuid;
  return NULL;
}

static size_t file_tree_base_len(int include_output_path) {
  size_t len = 1;

  if (include_output_path)
    len += strlen(current_cache_base_path());
  len += strlen(FILE_TREE_CACHE_DIR);
  if (!selected_uuid_is_current())
    len += 36 + 1;
  return len;
}

static void append_file_tree_base(char *path, size_t *pos,
                                  int include_output_path) {
  if (include_output_path) {
    const char *base_path = current_cache_base_path();
    size_t out_len = strlen(base_path);
    memcpy(path + *pos, base_path, out_len);
    *pos += out_len;
  }

  memcpy(path + *pos, FILE_TREE_CACHE_DIR, strlen(FILE_TREE_CACHE_DIR));
  *pos += strlen(FILE_TREE_CACHE_DIR);

  if (!selected_uuid_is_current()) {
    char uuid_string[37];
    uuid_to_string(selected_fs_uuid, uuid_string);
    memcpy(path + *pos, uuid_string, 36);
    *pos += 36;
    path[(*pos)++] = '/';
  }
}

static int mkdir_p(char *path, mode_t mode) {
  size_t len = strlen(path);
  if (len == 0) {
    errno = EINVAL;
    return -1;
  }

  for (size_t i = 1; i < len; i++) {
    if (path[i] != '/')
      continue;
    path[i] = '\0';
    if (mkdir_one(path, mode) != 0) {
      int e = errno;
      path[i] = '/';
      errno = e;
      return -1;
    }
    path[i] = '/';
  }

  if (mkdir_one(path, mode) != 0)
    return -1;
  return 0;
}

static const char *base_path_for_dir_suffix(const char *suffix) {
  if (strncmp(suffix, CACHE_DIR, strlen(CACHE_DIR)) == 0)
    return current_cache_base_path();
  return output_path;
}

int ensure_cache_dir(const char *cache_dir) {
  const char *base_path = base_path_for_dir_suffix(cache_dir);
  if (!base_path)
    return -1;

  size_t ol = strlen(base_path);
  while (ol > 0 && base_path[ol - 1] == '/')
    ol--;

  const char *suffix = cache_dir;
  size_t sl = strlen(suffix);
  char *full = malloc(ol + sl + 1);
  if (!full)
    return -1;

  memcpy(full, base_path, ol);
  memcpy(full + ol, suffix, sl + 1);

  int rc = mkdir_p(full, 0755);
  free(full);
  return rc;
}

int ensure_address_mapping_leaf_internal_cache_dirs(void) {
  if (ensure_cache_dir(ADDRESS_MAPPING_CACHE_DIR) != 0)
    return -1;
  if (ensure_cache_dir(LEAF_NODE_CACHE_DIR) != 0)
    return -1;
  if (ensure_cache_dir(INTERNAL_NODE_CACHE_DIR) != 0)
    return -1;
  if (ensure_cache_dir(ITEM_CACHE_DIR) != 0)
    return -1;
  return 0;
}

char *device_cache_path(uint64_t devid) {
  char devid_str[21];

  snprintf(devid_str, sizeof(devid_str), "%" PRIu64, devid);

  const char *base_path = current_cache_base_path();
  size_t out_len = strlen(base_path);
  size_t dir_len = strlen(DEVICE_CACHE_DIR);
  size_t id_len = strlen(devid_str);

  size_t len = out_len + dir_len + id_len + 1;
  char *path = malloc(len);
  if (!path)
    return NULL;

  memcpy(path, base_path, out_len);
  memcpy(path + out_len, DEVICE_CACHE_DIR, dir_len);
  memcpy(path + out_len + dir_len, devid_str, id_len + 1);

  return path;
}

char *chunk_tree_cache_path(int type) {
  const char *base_path = current_cache_base_path();
  size_t out_len = strlen(base_path);
  size_t len = out_len + 1;
  char *path = NULL;
  switch (type) {
  case CHUNK_TREE_CACHE:
    len += strlen(CURRENT_CHUNK_TREE_CACHE_NAME);
    path = malloc(len);
    memcpy(path, base_path, out_len);
    memcpy(path + out_len, CURRENT_CHUNK_TREE_CACHE_NAME,
           strlen(CURRENT_CHUNK_TREE_CACHE_NAME) + 1);
    return path;
  case CHUNK_TREE_STRIPE_CACHE:
    len += strlen(CURRENT_CHUNK_TREE_STRIPE_CACHE_NAME);
    path = malloc(len);
    memcpy(path, base_path, out_len);
    memcpy(path + out_len, CURRENT_CHUNK_TREE_STRIPE_CACHE_NAME,
           strlen(CURRENT_CHUNK_TREE_STRIPE_CACHE_NAME) + 1);
    return path;
  case TOTAL_CHUNK_TREE_CACHE:
    len += strlen(TOTAL_CHUNK_TREE_CACHE_NAME);
    path = malloc(len);
    memcpy(path, base_path, out_len);
    memcpy(path + out_len, TOTAL_CHUNK_TREE_CACHE_NAME,
           strlen(TOTAL_CHUNK_TREE_CACHE_NAME) + 1);
    return path;
  default:
    return NULL;
  }
}

char *cache_dir_path(void) {
  const char *base_path = current_cache_base_path();
  size_t out_len = strlen(base_path);
  size_t len = out_len + 1;
  char *path = NULL;
  len += strlen(CACHE_DIR);
  path = malloc(len);
  if (!path)
    return NULL;
  memcpy(path, base_path, out_len);
  memcpy(path + out_len, CACHE_DIR, strlen(CACHE_DIR) + 1);
  return path;
}

int remove_cache_files(const char *path) {
  DIR *dir = opendir(path);
  struct dirent *entry;
  char filepath[1024];

  if (!dir)
    return -1;

  while ((entry = readdir(dir)) != NULL) {

    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);

    struct stat st;
    stat(filepath, &st);

    if (S_ISDIR(st.st_mode)) {
      remove_cache_files(filepath);
    } else {
      remove(filepath);
    }
  }

  closedir(dir);

  return rmdir(path);
}

static const char *const item_cache_names[] = {
    "/dev_item.bin",    "/chunk_item.bin",    "/inode_item.bin",
    "/inode_ref.bin",   "/inode_extref.bin",  "/dir_item.bin",
    "/dir_index.bin",   "/extent_data.bin",   "/root_item.bin",
    "/extent_item.bin", "/metadata_item.bin", "/block_group.bin",
    "/extent_csum.bin",
};

static const char cache_format_marker[] = "BTRFS_RECOVERY_CACHE=3\n";

static char *cache_format_path(void) {
  const char *base_path = current_cache_base_path();
  const char suffix[] = CACHE_DIR "/cache_format";
  size_t base_len;
  char *path;

  if (!base_path)
    return NULL;
  base_len = strlen(base_path);
  path = malloc(base_len + sizeof(suffix));
  if (!path)
    return NULL;
  memcpy(path, base_path, base_len);
  memcpy(path + base_len, suffix, sizeof(suffix));
  return path;
}

char *extent_csum_range_index_path(void) {
  const char *base_path = current_cache_base_path();
  const char suffix[] = ITEM_CACHE_DIR "/extent_csum_range_index.bin";
  size_t base_len;
  char *path;

  if (!base_path)
    return NULL;
  base_len = strlen(base_path);
  path = malloc(base_len + sizeof(suffix));
  if (!path)
    return NULL;
  memcpy(path, base_path, base_len);
  memcpy(path + base_len, suffix, sizeof(suffix));
  return path;
}

int cache_format_publish(void) {
  char *path = NULL;
  char *tmp_path = NULL;
  FILE *file = NULL;
  size_t path_len;
  int result = -1;

  if (ensure_cache_dir(CACHE_DIR) != 0)
    return -1;

  path = cache_format_path();
  if (!path)
    return -1;
  path_len = strlen(path);
  tmp_path = malloc(path_len + sizeof(".tmp"));
  if (!tmp_path)
    goto out;
  memcpy(tmp_path, path, path_len);
  memcpy(tmp_path + path_len, ".tmp", sizeof(".tmp"));

  file = fopen(tmp_path, "wb");
  if (!file)
    goto out;
  if (fwrite(cache_format_marker, 1, sizeof(cache_format_marker) - 1, file) !=
      sizeof(cache_format_marker) - 1)
    goto out;
  if (fclose(file) != 0) {
    file = NULL;
    goto out;
  }
  file = NULL;
  if (rename(tmp_path, path) != 0)
    goto out;
  result = 0;

out:
  if (file)
    fclose(file);
  if (result != 0 && tmp_path)
    remove(tmp_path);
  free(tmp_path);
  free(path);
  return result;
}

int cache_format_validate(void) {
  char *path = cache_format_path();
  char marker[sizeof(cache_format_marker)];
  FILE *file;
  int result = -1;

  if (!path)
    return -1;
  file = fopen(path, "rb");
  free(path);
  if (!file)
    return -1;
  if (fread(marker, 1, sizeof(cache_format_marker) - 1, file) ==
          sizeof(cache_format_marker) - 1 &&
      memcmp(marker, cache_format_marker, sizeof(cache_format_marker) - 1) ==
          0 &&
      fgetc(file) == EOF && ferror(file) == 0)
    result = 0;
  fclose(file);
  return result;
}

char *item_cache_path(int type) {
  const char *name = item_cache_names[type];
  const char *base_path = current_cache_base_path();
  size_t out_len = strlen(base_path);
  size_t len = out_len + 1;
  char *path = NULL;
  len += strlen(ITEM_CACHE_DIR);
  len += strlen(name);
  path = malloc(len);
  if (!path)
    return NULL;
  memcpy(path, base_path, out_len);
  memcpy(path + out_len, ITEM_CACHE_DIR, strlen(ITEM_CACHE_DIR));
  memcpy(path + out_len + strlen(ITEM_CACHE_DIR), name, strlen(name) + 1);
  return path;
}

char *cache_path(int type) {
  const char *base_path = current_cache_base_path();
  size_t out_len = strlen(base_path);
  size_t len = out_len + 1;
  char *path = NULL;
  switch (type) {
  case MAP_CACHE:
    len += strlen(ADDRESS_MAPPING_CACHE_NAME);
    path = malloc(len);
    memcpy(path, base_path, out_len);
    memcpy(path + out_len, ADDRESS_MAPPING_CACHE_NAME,
           strlen(ADDRESS_MAPPING_CACHE_NAME) + 1);
    return path;
  case LEAF_NODE_CACHE:
    len += strlen(LEAF_NODE_CACHE_NAME);
    path = malloc(len);
    memcpy(path, base_path, out_len);
    memcpy(path + out_len, LEAF_NODE_CACHE_NAME,
           strlen(LEAF_NODE_CACHE_NAME) + 1);
    return path;
  case INTERNAL_NODE_CACHE:
    len += strlen(INTERNAL_NODE_CACHE_NAME);
    path = malloc(len);
    memcpy(path, base_path, out_len);
    memcpy(path + out_len, INTERNAL_NODE_CACHE_NAME,
           strlen(INTERNAL_NODE_CACHE_NAME) + 1);
    return path;
  case TOTAL_CHUNK_TREE_CACHE:
    len += strlen(TOTAL_CHUNK_TREE_CACHE_NAME);
    path = malloc(len);
    memcpy(path, base_path, out_len);
    memcpy(path + out_len, TOTAL_CHUNK_TREE_CACHE_NAME,
           strlen(TOTAL_CHUNK_TREE_CACHE_NAME) + 1);
    return path;
  default:
    return NULL;
  }
}

static int extract_relative_path_is_safe(const char *path) {
  if (!path || path[0] == '\0' || path[0] == '/')
    return 0;

  const char *component = path;
  for (const char *cursor = path;; cursor++) {
    if (*cursor != '/' && *cursor != '\0')
      continue;

    size_t component_len = (size_t)(cursor - component);
    if (component_len == 0 ||
        (component_len == 1 && component[0] == '.') ||
        (component_len == 2 && component[0] == '.' && component[1] == '.'))
      return 0;
    if (*cursor == '\0')
      return 1;
    component = cursor + 1;
  }
}

static int open_extract_root_directory(void) {
  if (!output_path) {
    errno = EINVAL;
    return -1;
  }

  int output_fd = open(output_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (output_fd < 0)
    return -1;
  int extract_fd = openat(output_fd, "extract",
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (extract_fd < 0) {
    int saved_errno = errno;
    close(output_fd);
    errno = saved_errno;
    return -1;
  }
  close(output_fd);
  return extract_fd;
}

static int open_extract_directories(int root_fd, char *relative_path) {
  int current_fd = root_fd;
  char *component = relative_path;

  for (;;) {
    char *slash = strchr(component, '/');
    if (slash)
      *slash = '\0';

    if (mkdirat(current_fd, component, 0755) != 0 && errno != EEXIST) {
      int saved_errno = errno;
      close(current_fd);
      errno = saved_errno;
      return -1;
    }
    int next_fd = openat(current_fd, component,
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next_fd < 0) {
      int saved_errno = errno;
      close(current_fd);
      errno = saved_errno;
      return -1;
    }
    close(current_fd);
    current_fd = next_fd;

    if (!slash)
      return current_fd;
    component = slash + 1;
  }
}

int ensure_extract_directory(const char *relative_path) {
  if (!extract_relative_path_is_safe(relative_path)) {
    errno = EINVAL;
    return -1;
  }

  char *path = malloc(strlen(relative_path) + 1);
  if (!path)
    return -1;
  strcpy(path, relative_path);

  int root_fd = open_extract_root_directory();
  if (root_fd < 0) {
    int saved_errno = errno;
    free(path);
    errno = saved_errno;
    return -1;
  }

  int directory_fd = open_extract_directories(root_fd, path);
  if (directory_fd < 0) {
    int saved_errno = errno;
    free(path);
    errno = saved_errno;
    return -1;
  }

  close(directory_fd);
  free(path);
  return 0;
}

FILE *open_extract_file(const uint8_t *relative_path_bytes, const char *mode) {
  const char *relative_path = (const char *)relative_path_bytes;
  if (!extract_relative_path_is_safe(relative_path) || !mode ||
      (strcmp(mode, "wb") != 0 && strcmp(mode, "r+b") != 0)) {
    errno = EINVAL;
    return NULL;
  }

  char *path = malloc(strlen(relative_path) + 1);
  if (!path)
    return NULL;
  strcpy(path, relative_path);

  char *leaf = strrchr(path, '/');
  int parent_fd = open_extract_root_directory();
  if (parent_fd < 0) {
    int saved_errno = errno;
    free(path);
    errno = saved_errno;
    return NULL;
  }
  if (leaf) {
    *leaf = '\0';
    leaf++;
    parent_fd = open_extract_directories(parent_fd, path);
    if (parent_fd < 0) {
      int saved_errno = errno;
      free(path);
      errno = saved_errno;
      return NULL;
    }
  } else {
    leaf = path;
  }

  int write_mode = strcmp(mode, "wb") == 0;
  int fd;
  if (write_mode) {
    fd = openat(parent_fd, leaf,
                O_WRONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0 && errno == ENOENT)
      fd = openat(parent_fd, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
                                       O_CLOEXEC | O_NONBLOCK,
                  0644);
  } else {
    fd = openat(parent_fd, leaf,
                O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  }
  int saved_errno = fd < 0 ? errno : 0;
  close(parent_fd);
  free(path);
  if (fd < 0) {
    errno = saved_errno;
    return NULL;
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return NULL;
  }
  if (!S_ISREG(st.st_mode)) {
    close(fd);
    errno = EINVAL;
    return NULL;
  }
  if (st.st_nlink != 1) {
    close(fd);
    errno = EMLINK;
    return NULL;
  }
  if (write_mode && ftruncate(fd, 0) != 0) {
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return NULL;
  }

  int status_flags = fcntl(fd, F_GETFL);
  if (status_flags < 0 ||
      fcntl(fd, F_SETFL, status_flags & ~O_NONBLOCK) != 0) {
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return NULL;
  }

  FILE *file = fdopen(fd, mode);
  if (!file) {
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
  }
  return file;
}

char *extract_path(uint8_t *name) {
  const char *name_str = (const char *)name;
  if (!output_path || !extract_relative_path_is_safe(name_str)) {
    errno = EINVAL;
    return NULL;
  }
  size_t out_len = strlen(output_path);
  size_t len = out_len + 1;
  char *path = NULL;
  len += strlen(EXTRACT_DIR);
  len += strlen(name_str);
  path = malloc(len);
  if (!path)
    return NULL;
  memcpy(path, output_path, out_len);
  memcpy(path + out_len, EXTRACT_DIR, strlen(EXTRACT_DIR));
  memcpy(path + out_len + strlen(EXTRACT_DIR), name_str, strlen(name_str) + 1);
  return path;
}

char *file_tree_cache_dir_all_path(void) {
  size_t len = file_tree_base_len(1);
  size_t total_len = 0;
  char *path = NULL;

  path = malloc(len);
  if (!path)
    return NULL;
  append_file_tree_base(path, &total_len, 1);
  path[total_len] = '\0';
  return path;
}

char *file_tree_cache_dir_path(void) {
  size_t len = file_tree_base_len(0);
  size_t total_len = 0;
  char *path = NULL;

  path = malloc(len);
  if (!path)
    return NULL;
  append_file_tree_base(path, &total_len, 0);
  path[total_len] = '\0';
  return path;
}

char *file_tree_cache_path_typed(uint64_t objectid, uint64_t root_dirid,
                                 uint8_t key_type, int type) {
  char objectid_str[21];
  char root_dirid_str[21];
  char key_type_str[4];
  if (root_dirid == INVALID_OBJECTID) {
    root_dirid = 0;
  }
  snprintf(objectid_str, sizeof(objectid_str), "%" PRIu64, objectid);
  snprintf(root_dirid_str, sizeof(root_dirid_str), "%" PRIu64, root_dirid);
  snprintf(key_type_str, sizeof(key_type_str), "%u", (unsigned)key_type);
  size_t len = file_tree_base_len(1);
  char *path = NULL;
  len += strlen(root_dirid_str) + 1;
  len += strlen(objectid_str);
  if (key_type != INODE_ITEM)
    len += 1 + strlen(key_type_str);
  switch (type) {
  case FILE_TREE_NODE_NAME_CACHE:
    len += strlen(FILE_TREE_NODE_NAME_CACHE_NAME);
    break;
  case FILE_TREE_NODE_CHILD_CACHE:
    len += strlen(FILE_TREE_NODE_CHILD_CACHE_NAME);
    break;
  case FILE_TREE_NODE_EXTENT_CACHE:
    len += strlen(FILE_TREE_NODE_EXTENT_CACHE_NAME);
    break;
  case FILE_TREE_NODE_MODIFY_CACHE:
    len += strlen(FILE_TREE_NODE_MODIFY_CACHE_NAME);
    break;
  case FILE_TREE_NODE_MOVE_CACHE:
    len += strlen(FILE_TREE_NODE_MOVE_CACHE_NAME);
    break;
  }
  path = malloc(len);
  if (!path)
    return NULL;
  size_t total_len = 0;
  append_file_tree_base(path, &total_len, 1);
  memcpy(path + total_len, root_dirid_str, strlen(root_dirid_str));
  total_len += strlen(root_dirid_str);
  memcpy(path + total_len, "_", 1);
  total_len += 1;
  memcpy(path + total_len, objectid_str, strlen(objectid_str));
  total_len += strlen(objectid_str);
  if (key_type != INODE_ITEM) {
    memcpy(path + total_len, "_", 1);
    total_len += 1;
    memcpy(path + total_len, key_type_str, strlen(key_type_str));
    total_len += strlen(key_type_str);
  }
  switch (type) {
  case FILE_TREE_NODE_CACHE:
    path[total_len] = '\0';
    break;
  case FILE_TREE_NODE_NAME_CACHE:
    memcpy(path + total_len, FILE_TREE_NODE_NAME_CACHE_NAME,
           strlen(FILE_TREE_NODE_NAME_CACHE_NAME) + 1);
    total_len += strlen(FILE_TREE_NODE_NAME_CACHE_NAME) + 1;
    break;
  case FILE_TREE_NODE_CHILD_CACHE:
    memcpy(path + total_len, FILE_TREE_NODE_CHILD_CACHE_NAME,
           strlen(FILE_TREE_NODE_CHILD_CACHE_NAME) + 1);
    total_len += strlen(FILE_TREE_NODE_CHILD_CACHE_NAME) + 1;
    break;
  case FILE_TREE_NODE_EXTENT_CACHE:
    memcpy(path + total_len, FILE_TREE_NODE_EXTENT_CACHE_NAME,
           strlen(FILE_TREE_NODE_EXTENT_CACHE_NAME) + 1);
    total_len += strlen(FILE_TREE_NODE_EXTENT_CACHE_NAME) + 1;
    break;
  case FILE_TREE_NODE_MODIFY_CACHE:
    memcpy(path + total_len, FILE_TREE_NODE_MODIFY_CACHE_NAME,
           strlen(FILE_TREE_NODE_MODIFY_CACHE_NAME) + 1);
    total_len += strlen(FILE_TREE_NODE_MODIFY_CACHE_NAME) + 1;
    break;
  case FILE_TREE_NODE_MOVE_CACHE:
    memcpy(path + total_len, FILE_TREE_NODE_MOVE_CACHE_NAME,
           strlen(FILE_TREE_NODE_MOVE_CACHE_NAME) + 1);
    total_len += strlen(FILE_TREE_NODE_MOVE_CACHE_NAME) + 1;
    break;
  }
  return path;
}

char *file_tree_cache_path(uint64_t objectid, uint64_t root_dirid, int type) {
  return file_tree_cache_path_typed(objectid, root_dirid, INODE_ITEM, type);
}

char *file_tree_inode_cache_path(const struct inode_instance_id *id, int type) {
  char root_dirid_str[21];
  char objectid_str[21];
  char inode_generation_str[21];
  size_t len;
  size_t total_len = 0;
  char *path;

  if (!id)
    return NULL;

  snprintf(root_dirid_str, sizeof(root_dirid_str), "%" PRIu64,
           id->root_dirid);
  snprintf(objectid_str, sizeof(objectid_str), "%" PRIu64, id->objectid);
  snprintf(inode_generation_str, sizeof(inode_generation_str), "%" PRIu64,
           id->inode_generation);
  len = file_tree_base_len(1) + strlen(root_dirid_str) + 1 +
        strlen(objectid_str) + 2 + strlen(inode_generation_str);
  switch (type) {
  case FILE_TREE_NODE_NAME_CACHE:
    len += strlen(FILE_TREE_NODE_NAME_CACHE_NAME);
    break;
  case FILE_TREE_NODE_CHILD_CACHE:
    len += strlen(FILE_TREE_NODE_CHILD_CACHE_NAME);
    break;
  case FILE_TREE_NODE_EXTENT_CACHE:
    len += strlen(FILE_TREE_NODE_EXTENT_CACHE_NAME);
    break;
  case FILE_TREE_NODE_MODIFY_CACHE:
    len += strlen(FILE_TREE_NODE_MODIFY_CACHE_NAME);
    break;
  case FILE_TREE_NODE_MOVE_CACHE:
    len += strlen(FILE_TREE_NODE_MOVE_CACHE_NAME);
    break;
  }

  path = malloc(len);
  if (!path)
    return NULL;
  append_file_tree_base(path, &total_len, 1);
  total_len += sprintf(path + total_len, "%s_%s_g%s", root_dirid_str,
                       objectid_str, inode_generation_str);
  switch (type) {
  case FILE_TREE_NODE_CACHE:
    path[total_len] = '\0';
    break;
  case FILE_TREE_NODE_NAME_CACHE:
    strcpy(path + total_len, FILE_TREE_NODE_NAME_CACHE_NAME);
    break;
  case FILE_TREE_NODE_CHILD_CACHE:
    strcpy(path + total_len, FILE_TREE_NODE_CHILD_CACHE_NAME);
    break;
  case FILE_TREE_NODE_EXTENT_CACHE:
    strcpy(path + total_len, FILE_TREE_NODE_EXTENT_CACHE_NAME);
    break;
  case FILE_TREE_NODE_MODIFY_CACHE:
    strcpy(path + total_len, FILE_TREE_NODE_MODIFY_CACHE_NAME);
    break;
  case FILE_TREE_NODE_MOVE_CACHE:
    strcpy(path + total_len, FILE_TREE_NODE_MOVE_CACHE_NAME);
    break;
  default:
    path[total_len] = '\0';
    break;
  }
  return path;
}
