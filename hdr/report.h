#ifndef REPORT_H
#define REPORT_H

#include <stddef.h>

struct run_report {
  const char *input_path;
  const char *output_path;
  const char *cache_dir;
  const char *manifest_path;
  int image_mode;
  int skip_parse;
  int threads;
  int no_data_checksum;
  int deleted_only;
  long chunk_parse_seconds;
  long tree_parse_seconds;
  long root_tree_seconds;
  size_t candidate_count;
  size_t selected_deleted;
  size_t skipped_active;
  size_t skipped_unknown;
  size_t skipped_non_regular;
  int extract_failures;
};

int write_run_report_json(const char *path, const struct run_report *report);

#endif
