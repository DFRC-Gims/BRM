#include "candidates.h"

#include "layout.h"
#include "struct.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int is_ascii_digit(char value) { return value >= '0' && value <= '9'; }

static int has_rejected_suffix(const char *filename) {
  return strstr(filename, "_name") || strstr(filename, "_child") ||
         strstr(filename, "_extent") || strstr(filename, "_modify") ||
         strstr(filename, "_move");
}

static int parse_u64_component(const char *start, char **end,
                               uint64_t *value) {
  unsigned long long parsed;
  const char *cursor = start;

  if (!is_ascii_digit(*start))
    return -1;

  while (is_ascii_digit(*cursor))
    cursor++;

  if (*cursor != '_' && *cursor != '\0')
    return -1;

  errno = 0;
  parsed = strtoull(start, end, 10);
  if (*end == start || errno != 0)
    return -1;

  *value = (uint64_t)parsed;
  return 0;
}

int parse_candidate_filename(const char *filename,
                             struct recoverable_candidate *candidate) {
  char *end = NULL;
  uint64_t root_dirid = 0;
  uint64_t objectid = 0;
  uint64_t inode_generation = 0;

  if (!filename || !candidate || has_rejected_suffix(filename))
    return -1;

  if (parse_u64_component(filename, &end, &root_dirid) != 0 || *end != '_')
    return -1;

  end++;
  if (parse_u64_component(end, &end, &objectid) != 0 || *end != '_' ||
      end[1] != 'g')
    return -1;
  end += 2;
  if (parse_u64_component(end, &end, &inode_generation) != 0)
    return -1;

  if (*end != '\0')
    return -1;

  candidate->id.root_dirid = root_dirid;
  candidate->id.objectid = objectid;
  candidate->id.inode_generation = inode_generation;
  return 0;
}

static int candidate_compare(const void *lhs, const void *rhs) {
  const struct recoverable_candidate *a = lhs;
  const struct recoverable_candidate *b = rhs;

  if (a->id.root_dirid < b->id.root_dirid)
    return -1;
  if (a->id.root_dirid > b->id.root_dirid)
    return 1;
  if (a->id.objectid < b->id.objectid)
    return -1;
  if (a->id.objectid > b->id.objectid)
    return 1;
  if (a->id.inode_generation < b->id.inode_generation)
    return -1;
  if (a->id.inode_generation > b->id.inode_generation)
    return 1;
  return 0;
}

static int append_candidate(struct recoverable_candidate_list *list,
                            const struct recoverable_candidate *candidate,
                            size_t *capacity) {
  if (list->count < *capacity) {
    list->items[list->count++] = *candidate;
    return 0;
  }

  size_t next_capacity = *capacity ? *capacity * 2 : 64;
  if (next_capacity < *capacity ||
      next_capacity > SIZE_MAX / sizeof(*list->items))
    return -1;
  struct recoverable_candidate *items =
      realloc(list->items, next_capacity * sizeof(*list->items));
  if (!items)
    return -1;

  list->items = items;
  *capacity = next_capacity;
  list->items[list->count++] = *candidate;
  return 0;
}

int candidate_list_load(struct recoverable_candidate_list *list) {
  char *cache_dir = NULL;
  DIR *dir = NULL;
  struct dirent *entry;
  size_t capacity = 0;

  if (!list)
    return -1;

  list->items = NULL;
  list->count = 0;

  cache_dir = file_tree_cache_dir_all_path();
  if (!cache_dir)
    return -1;

  dir = opendir(cache_dir);
  free(cache_dir);
  if (!dir)
    return -1;

  for (;;) {
    struct recoverable_candidate candidate;

    errno = 0;
    entry = readdir(dir);
    if (!entry)
      break;

    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    if (parse_candidate_filename(entry->d_name, &candidate) != 0)
      continue;

    if (append_candidate(list, &candidate, &capacity) != 0) {
      closedir(dir);
      candidate_list_free(list);
      return -1;
    }
  }
  if (errno != 0) {
    closedir(dir);
    candidate_list_free(list);
    return -1;
  }

  closedir(dir);
  qsort(list->items, list->count, sizeof(*list->items), candidate_compare);
  return 0;
}

enum deleted_candidate_kind {
  CANDIDATE_UNKNOWN,
  CANDIDATE_NON_REGULAR,
  CANDIDATE_REGULAR,
  CANDIDATE_SELECTED
};

struct deleted_candidate_state {
  struct recoverable_candidate candidate;
  size_t original_index;
  uint64_t parent_objectid;
  uint64_t parent_inode_generation;
  enum deleted_candidate_kind kind;
  unsigned active : 1;
  unsigned deleted_at_parent : 1;
  unsigned moved_at_parent : 1;
  unsigned ambiguous : 1;
  unsigned root_incomplete : 1;
};

static int deleted_state_compare(const void *lhs, const void *rhs) {
  const struct deleted_candidate_state *a = lhs, *b = rhs;
  return candidate_compare(&a->candidate, &b->candidate);
}

static int deleted_state_order_compare(const void *lhs, const void *rhs) {
  const struct deleted_candidate_state *a = lhs, *b = rhs;
  return (a->original_index > b->original_index) -
         (a->original_index < b->original_index);
}

static size_t deleted_state_lower_bound(
    const struct deleted_candidate_state *states, size_t count,
    const struct inode_instance_id *id) {
  struct recoverable_candidate key = {.id = *id};
  size_t first = 0;
  while (first < count) {
    size_t middle = first + (count - first) / 2;
    if (candidate_compare(&states[middle].candidate, &key) < 0)
      first = middle + 1;
    else
      count = middle;
  }
  return first;
}

static FILE *open_regular_cache_file(int parent_fd, const char *name) {
  int fd = openat(parent_fd, name, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    return NULL;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return NULL;
  }
  FILE *file = fdopen(fd, "rb");
  if (!file)
    close(fd);
  return file;
}

/* Node metadata alone cannot distinguish regular files from directories. */
static int deleted_candidate_read(struct deleted_candidate_state *state,
                                  FILE *inode_file) {
  const struct inode_instance_id *id = &state->candidate.id;
  struct total_file_tree node;
  struct inode_item inode;
  char *path = file_tree_inode_cache_path(id, FILE_TREE_NODE_CACHE);
  if (!path)
    return -1;
  FILE *file = open_regular_cache_file(AT_FDCWD, path);
  free(path);
  if (!file)
    return 0;
  int valid = fread(&node, sizeof(node), 1, file) == 1 &&
              fgetc(file) == EOF && !ferror(file);
  if (fclose(file) != 0)
    valid = 0;
  if (!valid || node.objectid != id->objectid ||
      node.inode_generation != id->inode_generation ||
      id->objectid == INVALID_OBJECTID || id->root_dirid == INVALID_OBJECTID ||
      id->root_dirid == 0 || id->inode_generation == 0)
    return 0;
  if (node.key_type == ROOT_ITEM) {
    state->kind = CANDIDATE_NON_REGULAR;
    return 0;
  }
  if (node.key_type != INODE_ITEM || node.inode_item_index == INVALID_INDEX ||
      !inode_file)
    return 0;
  clearerr(inode_file);
  if (fseeko(inode_file, (off_t)node.inode_item_index, SEEK_SET) != 0 ||
      fread(&inode, sizeof(inode), 1, inode_file) != 1 ||
      inode.generation != id->inode_generation ||
      (inode.mode & ~((uint32_t)S_IFMT | 07777U)) != 0)
    return 0;

  if (!S_ISREG(inode.mode)) {
    if (S_ISDIR(inode.mode) || S_ISLNK(inode.mode) || S_ISCHR(inode.mode) ||
        S_ISBLK(inode.mode) || S_ISFIFO(inode.mode) || S_ISSOCK(inode.mode))
      state->kind = CANDIDATE_NON_REGULAR;
    return 0;
  }
  state->kind = CANDIDATE_REGULAR;
  state->parent_objectid = node.parent_objectid;
  state->parent_inode_generation = node.parent_inode_generation;
  /* Child caches collapse same-directory hardlink names into one record. A
   * historical link count can therefore veto proof, but never supply it. */
  state->ambiguous = inode.nlink > 1;
  return 0;
}

static int parse_child_cache_filename(const char *filename,
                                      struct inode_instance_id *parent) {
  size_t len = strlen(filename);
  size_t suffix_len = strlen(FILE_TREE_NODE_CHILD_CACHE_NAME);
  char node_name[3 * 20 + 4];
  struct recoverable_candidate candidate;
  if (len <= suffix_len ||
      strcmp(filename + len - suffix_len, FILE_TREE_NODE_CHILD_CACHE_NAME) != 0 ||
      len - suffix_len >= sizeof(node_name))
    return -1;
  memcpy(node_name, filename, len - suffix_len);
  node_name[len - suffix_len] = '\0';
  if (parse_candidate_filename(node_name, &candidate) != 0)
    return -1;
  *parent = candidate.id;
  return 0;
}


/* A sidecar may be missing or truncated exactly at a record boundary. The
 * cached parent count still proves that the relation index is incomplete. */
static int deleted_candidate_check_parent(
    DIR *dir, const char *name, const struct inode_instance_id *parent,
    unsigned *incomplete) {
  struct total_file_tree node;
  FILE *file = open_regular_cache_file(dirfd(dir), name);
  if (!file)
    return 0;
  int valid = fread(&node, sizeof(node), 1, file) == 1 &&
              fgetc(file) == EOF && !ferror(file);
  if (fclose(file) != 0)
    valid = 0;
  if (!valid || node.key_type != INODE_ITEM ||
      node.objectid != parent->objectid ||
      node.inode_generation != parent->inode_generation || !node.child_count)
    return 0;
  size_t len = strlen(name) + sizeof(FILE_TREE_NODE_CHILD_CACHE_NAME);
  char *child_name = malloc(len);
  if (!child_name)
    return -1;
  snprintf(child_name, len, "%s%s", name, FILE_TREE_NODE_CHILD_CACHE_NAME);
  struct stat st;
  if (fstatat(dirfd(dir), child_name, &st, 0) != 0 ||
      !S_ISREG(st.st_mode) || st.st_size < 0 ||
      (uint64_t)st.st_size <
          (uint64_t)node.child_count * sizeof(struct child_cache) ||
      (uint64_t)st.st_size % sizeof(struct child_cache) != 0)
    *incomplete = 1;
  free(child_name);
  return 0;
}

/* Read every relation file once, including parents absent from the candidate
 * list. A surviving link in any parent of this root vetoes deletion. */
static int deleted_candidate_scan_relations(
    struct deleted_candidate_state *states, size_t count) {
  char *directory = file_tree_cache_dir_all_path();
  if (!directory)
    return -1;
  DIR *dir = opendir(directory);
  free(directory);
  if (!dir)
    return -1;
  int result = 0;
  for (;;) {
    struct inode_instance_id parent;
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      if (errno != 0)
        result = -1;
      break;
    }
    struct recoverable_candidate parent_candidate;
    int is_parent_node =
        parse_candidate_filename(entry->d_name, &parent_candidate) == 0;
    if (is_parent_node)
      parent = parent_candidate.id;
    else if (parse_child_cache_filename(entry->d_name, &parent) != 0)
      continue;
    const struct inode_instance_id root_start = {parent.root_dirid, 0, 0};
    size_t root_index = deleted_state_lower_bound(states, count, &root_start);
    if (root_index == count ||
        states[root_index].candidate.id.root_dirid != parent.root_dirid)
      continue;
    if (is_parent_node) {
      unsigned incomplete = 0;
      if (deleted_candidate_check_parent(dir, entry->d_name, &parent,
                                        &incomplete) != 0) {
        result = -1;
        break;
      }
      if (incomplete)
        states[root_index].root_incomplete = 1;
      continue;
    }
    FILE *file = open_regular_cache_file(dirfd(dir), entry->d_name);
    if (!file) {
      states[root_index].root_incomplete = 1;
      continue;
    }
    struct child_cache edge;
    size_t bytes;
    while ((bytes = fread(&edge, 1, sizeof(edge), file)) == sizeof(edge)) {
      if (edge.key_type != INODE_ITEM)
        continue;
      const struct inode_instance_id child = {
          parent.root_dirid, edge.objectid, edge.inode_generation};
      size_t index = deleted_state_lower_bound(states, count, &child);
      for (; index < count &&
             inode_instance_id_equal(&states[index].candidate.id, &child);
           index++) {
        struct deleted_candidate_state *state = &states[index];
        int latest_parent = state->parent_objectid == parent.objectid &&
                            state->parent_inode_generation ==
                                parent.inode_generation &&
                            parent.objectid != INVALID_OBJECTID &&
                            parent.objectid != 0 &&
                            parent.inode_generation != 0;
        if (edge.type == CHILD)
          state->active = 1;
        else if (edge.type == DELETED && latest_parent)
          state->deleted_at_parent = 1;
        else if (edge.type == MOVED && latest_parent)
          state->moved_at_parent = 1;
        else if (edge.type != DELETED && edge.type != MOVED)
          state->ambiguous = 1;
      }
    }
    /* An unreadable tail could contain a surviving link to any inode in this
     * root. Do not turn partial relation data into evidence of deletion. */
    if (bytes != 0 || ferror(file))
      states[root_index].root_incomplete = 1;
    if (fclose(file) != 0)
      states[root_index].root_incomplete = 1;
  }
  if (closedir(dir) != 0)
    result = -1;
  return result;
}

int candidate_list_filter_deleted(struct recoverable_candidate_list *list,
                                  struct deleted_candidate_stats *stats) {
  if (!list || !stats || (list->count && !list->items))
    return -1;
  memset(stats, 0, sizeof(*stats));
  if (!list->count)
    return 0;
  if (list->count > SIZE_MAX / sizeof(struct deleted_candidate_state))
    return -1;
  struct deleted_candidate_state *states = calloc(list->count, sizeof(*states));
  if (!states)
    return -1;
  char *inode_path = item_cache_path(INODE_ITEM_INDEX);
  if (!inode_path) {
    free(states);
    return -1;
  }
  FILE *inode_file = open_regular_cache_file(AT_FDCWD, inode_path);
  free(inode_path);
  int result = 0;
  for (size_t i = 0; i < list->count; i++) {
    states[i].candidate = list->items[i];
    states[i].original_index = i;
    if (deleted_candidate_read(&states[i], inode_file) != 0) {
      result = -1;
      break;
    }
  }
  if (inode_file && fclose(inode_file) != 0)
    result = -1;
  if (result == 0) {
    qsort(states, list->count, sizeof(*states), deleted_state_compare);
    result = deleted_candidate_scan_relations(states, list->count);
  }
  if (result != 0) {
    free(states);
    return -1;
  }

  int root_incomplete = 0;
  for (size_t i = 0; i < list->count; i++) {
    struct deleted_candidate_state *state = &states[i];
    if (i == 0 || states[i - 1].candidate.id.root_dirid !=
                      state->candidate.id.root_dirid)
      root_incomplete = state->root_incomplete;
    if (state->kind == CANDIDATE_NON_REGULAR)
      stats->skipped_non_regular++;
    else if (state->kind != CANDIDATE_REGULAR)
      stats->skipped_unknown++;
    else if (state->active)
      stats->skipped_active++;
    else if (root_incomplete || state->ambiguous || state->moved_at_parent ||
             !state->deleted_at_parent)
      stats->skipped_unknown++;
    else {
      state->kind = CANDIDATE_SELECTED;
      stats->selected_deleted++;
    }
  }
  qsort(states, list->count, sizeof(*states), deleted_state_order_compare);
  size_t retained = 0;
  for (size_t i = 0; i < list->count; i++) {
    if (states[i].kind == CANDIDATE_SELECTED)
      list->items[retained++] = states[i].candidate;
  }
  list->count = retained;
  free(states);
  return 0;
}

void candidate_list_free(struct recoverable_candidate_list *list) {
  if (!list)
    return;

  free(list->items);
  list->items = NULL;
  list->count = 0;
}

void candidate_list_print(FILE *stream,
                          const struct recoverable_candidate_list *list) {
  if (!stream || !list)
    return;

  for (size_t i = 0; i < list->count; i++) {
    fprintf(stream,
            "root=0x%" PRIx64 " object=0x%" PRIx64
            " inode_generation=0x%" PRIx64 "\n",
            list->items[i].id.root_dirid, list->items[i].id.objectid,
            list->items[i].id.inode_generation);
  }
}
