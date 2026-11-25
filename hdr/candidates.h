#ifndef CANDIDATES_H
#define CANDIDATES_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "inode_identity.h"

struct recoverable_candidate {
  struct inode_instance_id id;
};

struct recoverable_candidate_list {
  struct recoverable_candidate *items;
  size_t count;
};

struct deleted_candidate_stats {
  size_t selected_deleted;
  size_t skipped_active;
  size_t skipped_unknown;
  size_t skipped_non_regular;
};

int parse_candidate_filename(const char *filename,
                             struct recoverable_candidate *candidate);
int candidate_list_load(struct recoverable_candidate_list *list);
int candidate_list_filter_deleted(struct recoverable_candidate_list *list,
                                  struct deleted_candidate_stats *stats);
void candidate_list_free(struct recoverable_candidate_list *list);
void candidate_list_print(FILE *stream,
                          const struct recoverable_candidate_list *list);

#endif
