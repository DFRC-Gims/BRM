#ifndef INODE_IDENTITY_H
#define INODE_IDENTITY_H

#include <stdint.h>

struct inode_instance_id {
  uint64_t root_dirid;
  uint64_t objectid;
  uint64_t inode_generation;
};

static inline int
inode_instance_id_equal(const struct inode_instance_id *a,
                       const struct inode_instance_id *b) {
  return a && b && a->root_dirid == b->root_dirid &&
         a->objectid == b->objectid &&
         a->inode_generation == b->inode_generation;
}

#endif
