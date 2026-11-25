#ifndef CLI_H
#define CLI_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "inode_identity.h"

enum app_mode {
  APP_MODE_INTERACTIVE = 0,
  APP_MODE_LIST,
  APP_MODE_EXTRACT_ONE,
  APP_MODE_EXTRACT_ALL
};

struct app_options {
  int skip_parse;
  int image_mode;
  int include_other_fs_uuid;
  int print_times;
  int no_interactive;
  int no_data_checksum;
  int deleted_only;
  int threads;
  enum app_mode mode;
  const char *input_path;
  const char *output_path;
  const char *extract_target;
  const char *report_json_path;
  const char *manifest_path;
  const char *cache_dir;
};

void app_options_init(struct app_options *opts);
int parse_app_options(int argc, char **argv, struct app_options *opts,
                      char *err, size_t err_size);
int parse_extract_target(const char *value, struct inode_instance_id *target,
                        char *err, size_t err_size);
void print_usage(FILE *stream, const char *argv0);

#endif
