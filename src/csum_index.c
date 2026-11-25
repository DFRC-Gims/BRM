#define _POSIX_C_SOURCE 200809L

#include "csum_index.h"
#include "checksum.h"

#include "layout.h"
#include "struct.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CSUM_RANGE_INDEX_MAGIC "BTRCSI1"
#define CSUM_RANGE_INDEX_VERSION 1U
#define CSUM_RANGE_INDEX_HEADER_SIZE 48U
#define CSUM_RANGE_INDEX_NODE_SIZE 52U
#define CSUM_RANGE_INDEX_INVALID_CHILD UINT32_MAX
#define CRC32C_SIZE 4U

struct csum_range_index_header {
  char magic[8];
  uint32_t version;
  uint32_t header_size;
  uint64_t raw_cache_size;
  uint32_t sectorsize;
  uint16_t csum_type;
  uint16_t csum_size;
  uint64_t node_count;
  uint32_t root_index;
  uint32_t node_size;
};

struct csum_range_node {
  uint64_t logical_start;
  uint64_t logical_end;
  uint64_t payload_offset;
  uint64_t source_order;
  uint64_t subtree_max_end;
  uint32_t payload_size;
  uint32_t left_index;
  uint32_t right_index;
};

struct csum_range_index_handle {
  struct csum_range_node *nodes;
  size_t node_count;
  uint32_t root_index;
  uint32_t sectorsize;
  uint16_t csum_type;
  uint16_t csum_size;
  uint64_t raw_cache_size;
  int raw_fd;
};

struct match_candidate {
  uint32_t node_index;
  uint64_t source_order;
};

static struct csum_range_index_handle current_index = {
    .raw_fd = -1,
    .root_index = CSUM_RANGE_INDEX_INVALID_CHILD,
};

static void encode_le16(uint16_t value, uint8_t out[2]) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
}

static void encode_le32(uint32_t value, uint8_t out[4]) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
  out[2] = (uint8_t)(value >> 16);
  out[3] = (uint8_t)(value >> 24);
}

static void encode_le64(uint64_t value, uint8_t out[8]) {
  for (size_t i = 0; i < 8; i++)
    out[i] = (uint8_t)(value >> (i * 8));
}

static uint16_t decode_le16(const uint8_t in[2]) {
  return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t decode_le32(const uint8_t in[4]) {
  return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
         ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static uint64_t decode_le64(const uint8_t in[8]) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; i++)
    value |= (uint64_t)in[i] << (i * 8);
  return value;
}

static int read_exact_at(int fd, void *buffer, size_t size, uint64_t offset) {
  uint8_t *bytes = buffer;
  size_t done = 0;

  if (offset > (uint64_t)INT64_MAX)
    return -1;
  off_t initial = (off_t)offset;
  if ((uint64_t)initial != offset)
    return -1;

  while (done < size) {
    if (offset > UINT64_MAX - done)
      return -1;
    off_t current = (off_t)(offset + done);
    if ((uint64_t)current != offset + done)
      return -1;
    ssize_t count = pread(fd, bytes + done, size - done, current);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return -1;
    done += (size_t)count;
  }
  return 0;
}

static int write_all(FILE *file, const void *buffer, size_t size) {
  return fwrite(buffer, 1, size, file) == size ? 0 : -1;
}

static int range_node_compare(const void *lhs_ptr, const void *rhs_ptr) {
  const struct csum_range_node *lhs = lhs_ptr;
  const struct csum_range_node *rhs = rhs_ptr;
  if (lhs->logical_start < rhs->logical_start)
    return -1;
  if (lhs->logical_start > rhs->logical_start)
    return 1;
  if (lhs->source_order < rhs->source_order)
    return -1;
  if (lhs->source_order > rhs->source_order)
    return 1;
  return 0;
}

static int append_node(struct csum_range_node **nodes, size_t *count,
                       size_t *capacity,
                       const struct csum_range_node *node) {
  if (*count >= UINT32_MAX)
    return -1;
  if (*count == *capacity) {
    size_t new_capacity = *capacity == 0 ? 16 : *capacity * 2;
    if (new_capacity < *capacity || new_capacity > UINT32_MAX)
      new_capacity = UINT32_MAX;
    if (new_capacity <= *count ||
        new_capacity > SIZE_MAX / sizeof(**nodes))
      return -1;
    void *new_nodes = realloc(*nodes, new_capacity * sizeof(**nodes));
    if (!new_nodes)
      return -1;
    *nodes = new_nodes;
    *capacity = new_capacity;
  }
  (*nodes)[*count] = *node;
  (*count)++;
  return 0;
}

static uint32_t build_balanced_tree(struct csum_range_node *nodes,
                                    size_t start, size_t end,
                                    uint64_t *subtree_max_end) {
  if (start == end) {
    *subtree_max_end = 0;
    return CSUM_RANGE_INDEX_INVALID_CHILD;
  }

  size_t middle = start + (end - start) / 2;
  uint64_t left_max;
  uint64_t right_max;
  uint32_t left = build_balanced_tree(nodes, start, middle, &left_max);
  uint32_t right = build_balanced_tree(nodes, middle + 1, end, &right_max);
  uint64_t maximum = nodes[middle].logical_end;
  if (left != CSUM_RANGE_INDEX_INVALID_CHILD && left_max > maximum)
    maximum = left_max;
  if (right != CSUM_RANGE_INDEX_INVALID_CHILD && right_max > maximum)
    maximum = right_max;

  nodes[middle].left_index = left;
  nodes[middle].right_index = right;
  nodes[middle].subtree_max_end = maximum;
  *subtree_max_end = maximum;
  return (uint32_t)middle;
}

static void encode_header(const struct csum_range_index_header *header,
                          uint8_t out[CSUM_RANGE_INDEX_HEADER_SIZE]) {
  memset(out, 0, CSUM_RANGE_INDEX_HEADER_SIZE);
  memcpy(out, header->magic, sizeof(header->magic));
  encode_le32(header->version, out + 8);
  encode_le32(header->header_size, out + 12);
  encode_le64(header->raw_cache_size, out + 16);
  encode_le32(header->sectorsize, out + 24);
  encode_le16(header->csum_type, out + 28);
  encode_le16(header->csum_size, out + 30);
  encode_le64(header->node_count, out + 32);
  encode_le32(header->root_index, out + 40);
  encode_le32(header->node_size, out + 44);
}

static void decode_header(const uint8_t in[CSUM_RANGE_INDEX_HEADER_SIZE],
                          struct csum_range_index_header *header) {
  memcpy(header->magic, in, sizeof(header->magic));
  header->version = decode_le32(in + 8);
  header->header_size = decode_le32(in + 12);
  header->raw_cache_size = decode_le64(in + 16);
  header->sectorsize = decode_le32(in + 24);
  header->csum_type = decode_le16(in + 28);
  header->csum_size = decode_le16(in + 30);
  header->node_count = decode_le64(in + 32);
  header->root_index = decode_le32(in + 40);
  header->node_size = decode_le32(in + 44);
}

static void encode_node(const struct csum_range_node *node,
                        uint8_t out[CSUM_RANGE_INDEX_NODE_SIZE]) {
  encode_le64(node->logical_start, out);
  encode_le64(node->logical_end, out + 8);
  encode_le64(node->payload_offset, out + 16);
  encode_le64(node->source_order, out + 24);
  encode_le64(node->subtree_max_end, out + 32);
  encode_le32(node->payload_size, out + 40);
  encode_le32(node->left_index, out + 44);
  encode_le32(node->right_index, out + 48);
}

static void decode_node(const uint8_t in[CSUM_RANGE_INDEX_NODE_SIZE],
                        struct csum_range_node *node) {
  node->logical_start = decode_le64(in);
  node->logical_end = decode_le64(in + 8);
  node->payload_offset = decode_le64(in + 16);
  node->source_order = decode_le64(in + 24);
  node->subtree_max_end = decode_le64(in + 32);
  node->payload_size = decode_le32(in + 40);
  node->left_index = decode_le32(in + 44);
  node->right_index = decode_le32(in + 48);
}

static int publish_index(const char *path,
                         const struct csum_range_index_header *header,
                         const struct csum_range_node *nodes, size_t count) {
  size_t path_length = strlen(path);
  char *temporary_path = NULL;
  FILE *file = NULL;
  int result = -1;

  if (path_length > SIZE_MAX - sizeof(".tmp"))
    return -1;
  temporary_path = malloc(path_length + sizeof(".tmp"));
  if (!temporary_path)
    return -1;
  memcpy(temporary_path, path, path_length);
  memcpy(temporary_path + path_length, ".tmp", sizeof(".tmp"));

  file = fopen(temporary_path, "wb");
  if (!file)
    goto out;

  uint8_t encoded_header[CSUM_RANGE_INDEX_HEADER_SIZE];
  encode_header(header, encoded_header);
  if (write_all(file, encoded_header, sizeof(encoded_header)) != 0)
    goto out;
  for (size_t i = 0; i < count; i++) {
    uint8_t encoded_node[CSUM_RANGE_INDEX_NODE_SIZE];
    encode_node(&nodes[i], encoded_node);
    if (write_all(file, encoded_node, sizeof(encoded_node)) != 0)
      goto out;
  }
  if (fflush(file) != 0 || fsync(fileno(file)) != 0)
    goto out;
  if (fclose(file) != 0) {
    file = NULL;
    goto out;
  }
  file = NULL;
  if (rename(temporary_path, path) != 0)
    goto out;
  result = 0;

out:
  if (file)
    fclose(file);
  if (result != 0)
    remove(temporary_path);
  free(temporary_path);
  return result;
}

int csum_range_index_build(void) {
  char *raw_path = NULL;
  char *index_path = NULL;
  int raw_fd = -1;
  struct stat raw_stat;
  struct csum_range_node *nodes = NULL;
  size_t node_count = 0;
  size_t node_capacity = 0;
  uint32_t sectorsize = current_fs_sectorsize();
  uint16_t csum_type = current_fs_csum_type();
  size_t csum_size = checksum_size(csum_type);
  uint64_t raw_size;
  uint64_t offset = 0;
  int result = -1;

  if (sectorsize == 0 || csum_size == 0)
    return -1;
  raw_path = item_cache_path(EXTENT_CSUM_INDEX);
  index_path = extent_csum_range_index_path();
  if (!raw_path || !index_path)
    goto out;
  raw_fd = open(raw_path, O_RDONLY);
  if (raw_fd < 0 || fstat(raw_fd, &raw_stat) != 0 || raw_stat.st_size < 0)
    goto out;
  raw_size = (uint64_t)raw_stat.st_size;

  while (offset < raw_size) {
    struct csum_item_cache item;
    struct csum_range_node node;
    uint64_t payload_offset;
    uint64_t sector_count;
    uint64_t logical_span;

    if (raw_size - offset < sizeof(item) ||
        read_exact_at(raw_fd, &item, sizeof(item), offset) != 0)
      goto out;
    if (offset > UINT64_MAX - sizeof(item))
      goto out;
    payload_offset = offset + sizeof(item);
    if (item.size == 0 || item.size % csum_size != 0 ||
        item.logical_address % sectorsize != 0 ||
        payload_offset > raw_size || item.size > raw_size - payload_offset)
      goto out;

    sector_count = item.size / csum_size;
    if (sector_count > UINT64_MAX / sectorsize)
      goto out;
    logical_span = sector_count * sectorsize;
    if (item.logical_address > UINT64_MAX - logical_span)
      goto out;

    memset(&node, 0, sizeof(node));
    node.logical_start = item.logical_address;
    node.logical_end = item.logical_address + logical_span;
    node.payload_offset = payload_offset;
    node.source_order = node_count;
    node.payload_size = item.size;
    node.left_index = CSUM_RANGE_INDEX_INVALID_CHILD;
    node.right_index = CSUM_RANGE_INDEX_INVALID_CHILD;
    if (append_node(&nodes, &node_count, &node_capacity, &node) != 0)
      goto out;

    offset = payload_offset + item.size;
  }

  if (node_count > 1)
    qsort(nodes, node_count, sizeof(*nodes), range_node_compare);
  uint64_t subtree_max_end;
  uint32_t root_index =
      build_balanced_tree(nodes, 0, node_count, &subtree_max_end);
  (void)subtree_max_end;
  struct csum_range_index_header header = {
      .magic = CSUM_RANGE_INDEX_MAGIC,
      .version = CSUM_RANGE_INDEX_VERSION,
      .header_size = CSUM_RANGE_INDEX_HEADER_SIZE,
      .raw_cache_size = raw_size,
      .sectorsize = sectorsize,
      .csum_type = csum_type,
      .csum_size = (uint16_t)csum_size,
      .node_count = node_count,
      .root_index = root_index,
      .node_size = CSUM_RANGE_INDEX_NODE_SIZE,
  };
  result = publish_index(index_path, &header, nodes, node_count);

out:
  if (raw_fd >= 0)
    close(raw_fd);
  free(nodes);
  free(index_path);
  free(raw_path);
  return result;
}

void csum_range_index_close(void) {
  if (current_index.raw_fd >= 0)
    close(current_index.raw_fd);
  free(current_index.nodes);
  memset(&current_index, 0, sizeof(current_index));
  current_index.raw_fd = -1;
  current_index.root_index = CSUM_RANGE_INDEX_INVALID_CHILD;
}

static int key_is_before(const struct csum_range_node *lhs,
                         const struct csum_range_node *rhs) {
  return lhs->logical_start < rhs->logical_start ||
         (lhs->logical_start == rhs->logical_start &&
          lhs->source_order < rhs->source_order);
}

static int validate_balanced_subtree(const struct csum_range_node *nodes,
                                     size_t start, size_t end,
                                     uint32_t *root_index,
                                     uint64_t *subtree_max_end) {
  if (start == end) {
    *root_index = CSUM_RANGE_INDEX_INVALID_CHILD;
    *subtree_max_end = 0;
    return 0;
  }

  size_t middle = start + (end - start) / 2;
  uint64_t left_max;
  uint64_t right_max;
  uint32_t expected_left;
  uint32_t expected_right;
  if (validate_balanced_subtree(nodes, start, middle, &expected_left,
                                &left_max) != 0 ||
      validate_balanced_subtree(nodes, middle + 1, end, &expected_right,
                                &right_max) != 0)
    return -1;
  if (nodes[middle].left_index != expected_left ||
      nodes[middle].right_index != expected_right)
    return -1;

  uint64_t maximum = nodes[middle].logical_end;
  if (expected_left != CSUM_RANGE_INDEX_INVALID_CHILD && left_max > maximum)
    maximum = left_max;
  if (expected_right != CSUM_RANGE_INDEX_INVALID_CHILD && right_max > maximum)
    maximum = right_max;
  if (nodes[middle].subtree_max_end != maximum)
    return -1;
  *root_index = (uint32_t)middle;
  *subtree_max_end = maximum;
  return 0;
}

static int validate_nodes(const struct csum_range_index_header *header,
                          const struct csum_range_node *nodes) {
  size_t count = (size_t)header->node_count;
  uint8_t *seen_source_orders = NULL;
  const struct csum_range_node **nodes_by_source = NULL;
  int result = -1;

  if (count == 0)
    return header->root_index == CSUM_RANGE_INDEX_INVALID_CHILD &&
                   header->raw_cache_size == 0
               ? 0
               : -1;
  if (header->root_index != (uint32_t)(count / 2))
    return -1;
  if (count > SIZE_MAX / sizeof(*nodes_by_source))
    return -1;
  seen_source_orders = calloc(count, 1);
  nodes_by_source = malloc(count * sizeof(*nodes_by_source));
  if (!seen_source_orders || !nodes_by_source)
    goto out;

  for (size_t i = 0; i < count; i++) {
    const struct csum_range_node *node = &nodes[i];
    uint64_t sector_count;
    uint64_t logical_span;

    if (node->logical_start >= node->logical_end ||
        node->logical_start % header->sectorsize != 0 ||
        node->payload_size == 0 || node->payload_size % header->csum_size != 0)
      goto out;
    sector_count = node->payload_size / header->csum_size;
    if (sector_count > UINT64_MAX / header->sectorsize)
      goto out;
    logical_span = sector_count * header->sectorsize;
    if (node->logical_start > UINT64_MAX - logical_span ||
        node->logical_end != node->logical_start + logical_span)
      goto out;
    if (node->payload_offset < sizeof(struct csum_item_cache) ||
        node->payload_offset > header->raw_cache_size ||
        node->payload_size > header->raw_cache_size - node->payload_offset)
      goto out;
    if (node->left_index != CSUM_RANGE_INDEX_INVALID_CHILD &&
        node->left_index >= count)
      goto out;
    if (node->right_index != CSUM_RANGE_INDEX_INVALID_CHILD &&
        node->right_index >= count)
      goto out;
    if (node->source_order >= count ||
        seen_source_orders[(size_t)node->source_order])
      goto out;
    seen_source_orders[(size_t)node->source_order] = 1;
    nodes_by_source[(size_t)node->source_order] = node;
    if (i > 0 && !key_is_before(&nodes[i - 1], node))
      goto out;
  }

  uint64_t record_offset = 0;
  for (size_t i = 0; i < count; i++) {
    const struct csum_range_node *node = nodes_by_source[i];
    if (record_offset > UINT64_MAX - sizeof(struct csum_item_cache))
      goto out;
    uint64_t expected_payload_offset =
        record_offset + sizeof(struct csum_item_cache);
    if (node->payload_offset != expected_payload_offset ||
        expected_payload_offset > UINT64_MAX - node->payload_size)
      goto out;
    record_offset = expected_payload_offset + node->payload_size;
  }
  if (record_offset != header->raw_cache_size)
    goto out;

  uint64_t subtree_max_end;
  uint32_t root;
  if (validate_balanced_subtree(nodes, 0, count, &root, &subtree_max_end) != 0)
    goto out;
  (void)subtree_max_end;
  if (root != header->root_index)
    goto out;
  result = 0;

out:
  free(nodes_by_source);
  free(seen_source_orders);
  return result;
}

int csum_range_index_open(void) {
  char *index_path = NULL;
  char *raw_path = NULL;
  int index_fd = -1;
  int raw_fd = -1;
  struct stat index_stat;
  struct stat raw_stat;
  struct csum_range_index_header header;
  struct csum_range_node *nodes = NULL;
  uint8_t encoded_header[CSUM_RANGE_INDEX_HEADER_SIZE];
  uint32_t sectorsize = current_fs_sectorsize();
  uint16_t csum_type = current_fs_csum_type();
  size_t csum_size = checksum_size(csum_type);
  int result = -1;

  csum_range_index_close();
  if (sectorsize == 0 || csum_size == 0)
    return -1;
  index_path = extent_csum_range_index_path();
  raw_path = item_cache_path(EXTENT_CSUM_INDEX);
  if (!index_path || !raw_path)
    goto out;
  index_fd = open(index_path, O_RDONLY);
  raw_fd = open(raw_path, O_RDONLY);
  if (index_fd < 0 || raw_fd < 0 || fstat(index_fd, &index_stat) != 0 ||
      fstat(raw_fd, &raw_stat) != 0 || index_stat.st_size < 0 ||
      raw_stat.st_size < 0)
    goto out;
  if ((uint64_t)index_stat.st_size < CSUM_RANGE_INDEX_HEADER_SIZE ||
      read_exact_at(index_fd, encoded_header, sizeof(encoded_header), 0) != 0)
    goto out;
  decode_header(encoded_header, &header);

  if (memcmp(header.magic, CSUM_RANGE_INDEX_MAGIC, sizeof(header.magic)) != 0 ||
      header.version != CSUM_RANGE_INDEX_VERSION ||
      header.header_size != CSUM_RANGE_INDEX_HEADER_SIZE ||
      header.node_size != CSUM_RANGE_INDEX_NODE_SIZE ||
      header.sectorsize != sectorsize || header.csum_type != csum_type ||
      header.csum_size != csum_size ||
      header.raw_cache_size != (uint64_t)raw_stat.st_size ||
      header.node_count > UINT32_MAX ||
      header.node_count > SIZE_MAX / sizeof(*nodes) ||
      header.node_count >
          (UINT64_MAX - CSUM_RANGE_INDEX_HEADER_SIZE) /
              CSUM_RANGE_INDEX_NODE_SIZE)
    goto out;
  uint64_t expected_index_size =
      CSUM_RANGE_INDEX_HEADER_SIZE +
      header.node_count * CSUM_RANGE_INDEX_NODE_SIZE;
  if (expected_index_size != (uint64_t)index_stat.st_size)
    goto out;

  if (header.node_count != 0) {
    nodes = malloc((size_t)header.node_count * sizeof(*nodes));
    if (!nodes)
      goto out;
  }
  for (size_t i = 0; i < (size_t)header.node_count; i++) {
    uint8_t encoded_node[CSUM_RANGE_INDEX_NODE_SIZE];
    uint64_t offset = CSUM_RANGE_INDEX_HEADER_SIZE +
                      (uint64_t)i * CSUM_RANGE_INDEX_NODE_SIZE;
    if (read_exact_at(index_fd, encoded_node, sizeof(encoded_node), offset) != 0)
      goto out;
    decode_node(encoded_node, &nodes[i]);
  }
  if (validate_nodes(&header, nodes) != 0)
    goto out;

  current_index.nodes = nodes;
  current_index.node_count = (size_t)header.node_count;
  current_index.root_index = header.root_index;
  current_index.sectorsize = header.sectorsize;
  current_index.csum_type = header.csum_type;
  current_index.csum_size = header.csum_size;
  current_index.raw_cache_size = header.raw_cache_size;
  current_index.raw_fd = raw_fd;
  nodes = NULL;
  raw_fd = -1;
  result = 0;

out:
  if (index_fd >= 0)
    close(index_fd);
  if (raw_fd >= 0)
    close(raw_fd);
  free(nodes);
  free(raw_path);
  free(index_path);
  return result;
}

static int append_candidate(struct match_candidate **candidates, size_t *count,
                            size_t *capacity, uint32_t node_index,
                            uint64_t source_order) {
  if (*count == *capacity) {
    size_t new_capacity = *capacity == 0 ? 8 : *capacity * 2;
    if (new_capacity < *capacity ||
        new_capacity > SIZE_MAX / sizeof(**candidates))
      return -1;
    void *new_candidates =
        realloc(*candidates, new_capacity * sizeof(**candidates));
    if (!new_candidates)
      return -1;
    *candidates = new_candidates;
    *capacity = new_capacity;
  }
  (*candidates)[*count].node_index = node_index;
  (*candidates)[*count].source_order = source_order;
  (*count)++;
  return 0;
}

static int collect_overlapping_nodes(uint32_t node_index,
                                     uint64_t logical_start,
                                     uint64_t logical_end,
                                     struct match_candidate **candidates,
                                     size_t *count, size_t *capacity) {
  if (node_index == CSUM_RANGE_INDEX_INVALID_CHILD)
    return 0;

  const struct csum_range_node *node = &current_index.nodes[node_index];
  if (node->subtree_max_end <= logical_start)
    return 0;

  if (node->left_index != CSUM_RANGE_INDEX_INVALID_CHILD &&
      current_index.nodes[node->left_index].subtree_max_end > logical_start &&
      collect_overlapping_nodes(node->left_index, logical_start, logical_end,
                                candidates, count, capacity) != 0)
    return -1;
  if (node->logical_start < logical_end &&
      logical_start < node->logical_end &&
      append_candidate(candidates, count, capacity, node_index,
                       node->source_order) != 0)
    return -1;
  if (node->logical_start < logical_end &&
      node->right_index != CSUM_RANGE_INDEX_INVALID_CHILD &&
      current_index.nodes[node->right_index].subtree_max_end > logical_start &&
      collect_overlapping_nodes(node->right_index, logical_start, logical_end,
                                candidates, count, capacity) != 0)
    return -1;
  return 0;
}

static int read_checksum_slice(int fd, uint8_t *encoded_checksums,
                                uint8_t *readable, size_t sector_count,
                                size_t csum_size, uint64_t offset) {
  size_t size = sector_count * csum_size;
  size_t complete_sectors = 0;
  ssize_t count;

  memset(readable, 0, sector_count);
  if (offset > (uint64_t)INT64_MAX)
    return -1;
  off_t file_offset = (off_t)offset;
  if ((uint64_t)file_offset != offset)
    return -1;

  do {
    count = pread(fd, encoded_checksums, size, file_offset);
  } while (count < 0 && errno == EINTR);
  if (count < 0)
    return -1;
  if (count > 0) {
    complete_sectors = (size_t)count / csum_size;
    if (complete_sectors > sector_count)
      complete_sectors = sector_count;
    memset(readable, 1, complete_sectors);
  }
  if (count == (ssize_t)size)
    return 0;

  for (size_t i = complete_sectors; i < sector_count; i++) {
    uint64_t checksum_offset = (uint64_t)i * csum_size;
    if (offset <= UINT64_MAX - checksum_offset &&
        read_exact_at(fd, encoded_checksums + i * csum_size, csum_size,
                      offset + checksum_offset) == 0)
      readable[i] = 1;
  }
  return 0;
}

static int candidate_compare(const void *lhs_ptr, const void *rhs_ptr) {
  const struct match_candidate *lhs = lhs_ptr;
  const struct match_candidate *rhs = rhs_ptr;
  if (lhs->source_order < rhs->source_order)
    return -1;
  if (lhs->source_order > rhs->source_order)
    return 1;
  return 0;
}

int csum_range_index_match_batch(uint64_t logical_address, uint16_t csum_type,
                                 const uint8_t *actual, size_t sector_count,
                                 int *statuses) {
  struct match_candidate *candidates = NULL;
  size_t candidate_count = 0;
  size_t candidate_capacity = 0;
  uint8_t *encoded_checksums = NULL;
  uint8_t *candidate_readable = NULL;
  uint8_t *saw_readable_candidate = NULL;
  uint8_t *saw_unreadable_candidate = NULL;
  uint64_t logical_span;
  uint64_t logical_end;
  size_t matched_count = 0;
  int result = -1;

  size_t csum_size = checksum_size(csum_type);

  if (sector_count == 0)
    return 0;
  if (!actual || !statuses || csum_size == 0 ||
      sector_count > SIZE_MAX / sizeof(*statuses) ||
      sector_count > SIZE_MAX / csum_size ||
      sector_count > (size_t)SSIZE_MAX / csum_size)
    return -1;
  for (size_t i = 0; i < sector_count; i++)
    statuses[i] = -1;

  if (current_index.raw_fd < 0 || current_index.sectorsize == 0 ||
      current_index.csum_type != csum_type ||
      current_index.csum_size != csum_size ||
      current_fs_csum_type() != csum_type ||
      current_fs_sectorsize() != current_index.sectorsize ||
      logical_address % current_index.sectorsize != 0 ||
      sector_count > UINT64_MAX / current_index.sectorsize)
    return -1;
  logical_span = (uint64_t)sector_count * current_index.sectorsize;
  if (logical_address > UINT64_MAX - logical_span)
    return -1;
  logical_end = logical_address + logical_span;

  if (collect_overlapping_nodes(current_index.root_index, logical_address,
                                logical_end, &candidates, &candidate_count,
                                &candidate_capacity) != 0)
    goto out;
  if (candidate_count == 0) {
    result = 0;
    goto out;
  }

  encoded_checksums = malloc(sector_count * csum_size);
  candidate_readable = malloc(sector_count);
  saw_readable_candidate = calloc(sector_count, 1);
  saw_unreadable_candidate = calloc(sector_count, 1);
  if (!encoded_checksums || !candidate_readable || !saw_readable_candidate ||
      !saw_unreadable_candidate)
    goto out;

  qsort(candidates, candidate_count, sizeof(*candidates), candidate_compare);
  for (size_t i = 0; i < candidate_count; i++) {
    const struct csum_range_node *node =
        &current_index.nodes[candidates[i].node_index];
    uint64_t overlap_start = node->logical_start > logical_address
                                 ? node->logical_start
                                 : logical_address;
    uint64_t overlap_end =
        node->logical_end < logical_end ? node->logical_end : logical_end;
    size_t first_batch_sector =
        (size_t)((overlap_start - logical_address) /
                 current_index.sectorsize);
    uint64_t first_node_sector =
        (overlap_start - node->logical_start) / current_index.sectorsize;
    size_t overlap_sector_count =
        (size_t)((overlap_end - overlap_start) / current_index.sectorsize);
    uint64_t checksum_offset;
    uint64_t expected_offset;
    size_t expected_size = overlap_sector_count * csum_size;
    int needs_candidate = 0;

    for (size_t sector = 0; sector < overlap_sector_count; sector++) {
      if (statuses[first_batch_sector + sector] != 1) {
        needs_candidate = 1;
        break;
      }
    }
    if (!needs_candidate)
      continue;

    if (first_node_sector > UINT64_MAX / csum_size) {
      memset(saw_unreadable_candidate + first_batch_sector, 1,
             overlap_sector_count);
      continue;
    }
    checksum_offset = first_node_sector * csum_size;
    if (node->payload_offset > UINT64_MAX - checksum_offset) {
      memset(saw_unreadable_candidate + first_batch_sector, 1,
             overlap_sector_count);
      continue;
    }
    expected_offset = node->payload_offset + checksum_offset;
    if (expected_offset > current_index.raw_cache_size ||
        expected_size > current_index.raw_cache_size - expected_offset) {
      memset(saw_unreadable_candidate + first_batch_sector, 1,
             overlap_sector_count);
      continue;
    }

    if (read_checksum_slice(current_index.raw_fd, encoded_checksums,
                             candidate_readable, overlap_sector_count,
                             csum_size, expected_offset) != 0) {
      if (overlap_sector_count > 1)
        goto out;
      saw_unreadable_candidate[first_batch_sector] = 1;
      continue;
    }

    for (size_t sector = 0; sector < overlap_sector_count; sector++) {
      size_t batch_sector = first_batch_sector + sector;
      if (!candidate_readable[sector]) {
        saw_unreadable_candidate[batch_sector] = 1;
        continue;
      }
      saw_readable_candidate[batch_sector] = 1;
      if (memcmp(encoded_checksums + sector * csum_size,
                 actual + batch_sector * csum_size, csum_size) == 0 &&
          statuses[batch_sector] != 1) {
        statuses[batch_sector] = 1;
        matched_count++;
      }
    }
    if (matched_count == sector_count)
      break;
  }

  for (size_t i = 0; i < sector_count; i++) {
    if (statuses[i] == 1)
      continue;
    statuses[i] = saw_unreadable_candidate[i]
                      ? -1
                      : (saw_readable_candidate[i] ? 0 : -1);
  }
  result = 0;

out:
  free(saw_unreadable_candidate);
  free(saw_readable_candidate);
  free(candidate_readable);
  free(encoded_checksums);
  free(candidates);
  return result;
}

int csum_range_index_match(uint64_t logical_address, uint16_t csum_type,
                           const uint8_t *actual) {
  int status = -1;
  if (csum_range_index_match_batch(logical_address, csum_type, actual, 1,
                                   &status) != 0)
    return -1;
  return status;
}

int csum_range_index_match_crc32c_batch(uint64_t logical_address,
                                        const uint32_t *actual_crc32c,
                                        size_t sector_count, int *statuses) {
  if (sector_count == 0)
    return 0;
  if (!actual_crc32c || !statuses || sector_count > SIZE_MAX / CRC32C_SIZE)
    return -1;
  uint8_t *encoded = malloc(sector_count * CRC32C_SIZE);
  if (!encoded)
    return -1;
  for (size_t i = 0; i < sector_count; i++)
    encode_le32(actual_crc32c[i], encoded + i * CRC32C_SIZE);
  int result = csum_range_index_match_batch(logical_address, 0, encoded,
                                            sector_count, statuses);
  free(encoded);
  return result;
}

int csum_range_index_match_crc32c(uint64_t logical_address,
                                  uint32_t actual_crc32c) {
  uint8_t encoded[CRC32C_SIZE];
  encode_le32(actual_crc32c, encoded);
  return csum_range_index_match(logical_address, 0, encoded);
}
