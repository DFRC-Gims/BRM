#include "cli.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void set_error(char *err, size_t err_size, const char *fmt, ...) {
  va_list ap;

  if (err == NULL || err_size == 0)
    return;

  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

static int parse_long_option_value(const char *name, const char *value,
                                   long *out, char *err, size_t err_size) {
  char *end = NULL;
  long parsed;

  if (value == NULL) {
    set_error(err, err_size, "missing value for %s", name);
    return -1;
  }

  errno = 0;
  parsed = strtol(value, &end, 10);
  if (end == value || errno != 0) {
    set_error(err, err_size, "invalid value for %s: %s", name, value);
    return -1;
  }
  while (*end != '\0') {
    if (!isspace((unsigned char)*end)) {
      set_error(err, err_size, "invalid trailing text for %s: %s", name,
                value);
      return -1;
    }
    end++;
  }

  *out = parsed;
  return 0;
}

static int require_value(int argc, char **argv, int *index, const char *option,
                         const char **value, char *err, size_t err_size) {
  if (*index + 1 >= argc) {
    set_error(err, err_size, "missing value for %s", option);
    return -1;
  }

  *index += 1;
  *value = argv[*index];
  return 0;
}

static int set_action_mode(struct app_options *opts, enum app_mode mode,
                           int *action_seen, char *err, size_t err_size) {
  if (*action_seen) {
    set_error(err, err_size, "multiple action modes specified");
    return -1;
  }

  opts->mode = mode;
  *action_seen = 1;
  return 0;
}

void app_options_init(struct app_options *opts) {
  long cpu_count;

  if (opts == NULL)
    return;

  memset(opts, 0, sizeof(*opts));
  opts->mode = APP_MODE_INTERACTIVE;

  cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
  if (cpu_count < 1)
    cpu_count = 1;
  opts->threads = (int)cpu_count;
}

int parse_extract_target(const char *value, struct inode_instance_id *target,
                        char *err, size_t err_size) {
  uint64_t parsed_root_dirid = 0;
  uint64_t parsed_objectid = 0;
  uint64_t parsed_inode_generation = 0;
  char *end = NULL;
  const char *parse_start;

  if (value == NULL || target == NULL) {
    set_error(err, err_size, "extract target requires root_dirid,objectid");
    return -1;
  }

  while (isspace((unsigned char)*value))
    value++;

  if (*value == '-') {
    set_error(err, err_size, "invalid negative root_dirid in extract target");
    return -1;
  }

  errno = 0;
  parsed_root_dirid = strtoull(value, &end, 0);
  if (end == value || errno != 0) {
    set_error(err, err_size, "invalid root_dirid in extract target: %s",
              value);
    return -1;
  }

  while (isspace((unsigned char)*end))
    end++;
  if (*end != ',') {
    set_error(err, err_size,
              "extract target must be root_dirid,objectid,inode_generation");
    return -1;
  }

  end++;
  while (isspace((unsigned char)*end))
    end++;

  if (*end == '-') {
    set_error(err, err_size, "invalid negative objectid in extract target");
    return -1;
  }

  parse_start = end;
  errno = 0;
  parsed_objectid = strtoull(end, &end, 0);
  if (end == parse_start || errno != 0) {
    set_error(err, err_size, "invalid objectid in extract target");
    return -1;
  }

  while (isspace((unsigned char)*end))
    end++;
  if (*end != ',') {
    set_error(err, err_size,
              "extract target must be root_dirid,objectid,inode_generation");
    return -1;
  }

  end++;
  while (isspace((unsigned char)*end))
    end++;

  if (*end == '-') {
    set_error(err, err_size, "invalid negative inode_generation in extract target");
    return -1;
  }

  parse_start = end;
  errno = 0;
  parsed_inode_generation = strtoull(end, &end, 0);
  if (end == parse_start || errno != 0) {
    set_error(err, err_size, "invalid inode_generation in extract target");
    return -1;
  }

  while (isspace((unsigned char)*end))
    end++;
  if (*end != '\0') {
    set_error(err, err_size, "invalid trailing text in extract target");
    return -1;
  }

  target->root_dirid = parsed_root_dirid;
  target->objectid = parsed_objectid;
  target->inode_generation = parsed_inode_generation;
  return 0;
}

int parse_app_options(int argc, char **argv, struct app_options *opts,
                      char *err, size_t err_size) {
  const char *positionals[3];
  int positional_count = 0;
  int action_seen = 0;

  if (opts == NULL) {
    set_error(err, err_size, "opts is required");
    return -1;
  }

  if (err != NULL && err_size > 0)
    err[0] = '\0';

  app_options_init(opts);

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    const char *value = NULL;

    if (strcmp(arg, "--skip-parse") == 0) {
      opts->skip_parse = 1;
    } else if (strcmp(arg, "--no-data-checksum") == 0) {
      opts->no_data_checksum = 1;
    } else if (strcmp(arg, "--deleted-only") == 0) {
      opts->deleted_only = 1;
    } else if (strcmp(arg, "--threads") == 0) {
      long parsed;
      if (require_value(argc, argv, &i, arg, &value, err, err_size) != 0)
        return -1;
      if (parse_long_option_value(arg, value, &parsed, err, err_size) != 0)
        return -1;
      if (parsed < 1) {
        set_error(err, err_size, "%s must be at least 1: %s", arg, value);
        return -1;
      }
      if (parsed > INT_MAX) {
        set_error(err, err_size, "%s exceeds INT_MAX: %s", arg, value);
        return -1;
      }
      opts->threads = (int)parsed;
    } else if (strcmp(arg, "--include-other-fs-uuid") == 0) {
      opts->include_other_fs_uuid = 1;
    } else if (strcmp(arg, "--print-times") == 0) {
      opts->print_times = 1;
    } else if (strcmp(arg, "--image-dir") == 0) {
      if (require_value(argc, argv, &i, arg, &value, err, err_size) != 0)
        return -1;
      opts->image_mode = 1;
      opts->input_path = value;
    } else if (strcmp(arg, "--list") == 0) {
      if (set_action_mode(opts, APP_MODE_LIST, &action_seen, err, err_size) !=
          0)
        return -1;
    } else if (strcmp(arg, "--extract") == 0) {
      if (require_value(argc, argv, &i, arg, &value, err, err_size) != 0)
        return -1;
      opts->extract_target = value;
      if (set_action_mode(opts, APP_MODE_EXTRACT_ONE, &action_seen, err,
                          err_size) != 0)
        return -1;
    } else if (strcmp(arg, "--extract-all") == 0) {
      if (set_action_mode(opts, APP_MODE_EXTRACT_ALL, &action_seen, err,
                          err_size) != 0)
        return -1;
    } else if (strcmp(arg, "--report-json") == 0) {
      if (require_value(argc, argv, &i, arg, &value, err, err_size) != 0)
        return -1;
      opts->report_json_path = value;
    } else if (strcmp(arg, "--manifest") == 0) {
      if (require_value(argc, argv, &i, arg, &value, err, err_size) != 0)
        return -1;
      opts->manifest_path = value;
    } else if (strcmp(arg, "--no-interactive") == 0) {
      opts->no_interactive = 1;
    } else if (strcmp(arg, "--cache-dir") == 0) {
      if (require_value(argc, argv, &i, arg, &value, err, err_size) != 0)
        return -1;
      opts->cache_dir = value;
    } else if (arg[0] == '-') {
      set_error(err, err_size, "unknown option: %s", arg);
      return -1;
    } else {
      if (positional_count == 2) {
        set_error(err, err_size, "too many positional arguments");
        return -1;
      }
      positionals[positional_count] = arg;
      positional_count++;
    }
  }

  if (opts->deleted_only && opts->mode != APP_MODE_EXTRACT_ALL) {
    set_error(err, err_size, "--deleted-only requires --extract-all");
    return -1;
  }
  if (opts->no_data_checksum && opts->mode == APP_MODE_LIST) {
    set_error(err, err_size, "--no-data-checksum cannot be used with --list");
    return -1;
  }

  if (opts->image_mode) {
    if (opts->input_path == NULL) {
      set_error(err, err_size, "missing input_path");
      return -1;
    }
    if (positional_count != 1) {
      set_error(err, err_size, "missing output_path");
      return -1;
    }
    opts->output_path = positionals[0];
  } else {
    if (positional_count != 2) {
      if (positional_count == 0)
        set_error(err, err_size, "missing input_path and output_path");
      else
        set_error(err, err_size, "missing output_path");
      return -1;
    }
    opts->input_path = positionals[0];
    opts->output_path = positionals[1];
  }

  return 0;
}

void print_usage(FILE *stream, const char *argv0) {
  if (stream == NULL)
    return;

  if (argv0 == NULL)
    argv0 = "c_demo";

  fprintf(stream,
          "Usage: %s [options] <input_path> <output_path>\n"
          "       %s [options] --image-dir <dir> <output_path>\n"
          "\n"
          "Options:\n"
          "  --skip-parse\n"
          "  --threads N\n"
          "  --include-other-fs-uuid\n"
          "  --print-times\n"
          "  --image-dir DIR\n"
          "  --list\n"
          "  --extract ROOT,OBJECT,INODE_GENERATION\n"
          "  --extract-all\n"
          "  --deleted-only (requires --extract-all)\n"
          "  --no-data-checksum (extraction only)\n"
          "  --report-json PATH\n"
          "  --manifest PATH\n"
          "  --no-interactive\n"
          "  --cache-dir PATH\n",
          argv0, argv0);
}
