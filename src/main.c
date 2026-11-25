#include "cli.h"
#include "candidates.h"
#include "csum_index.h"
#include "file_tree.h"
#include "init.h"
#include "layout.h"
#include "raid.h"
#include "report.h"
#include "root_tree.h"
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <linux/btrfs.h>
#include <linux/btrfs_tree.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define BTRFS_SUPER_INFO_OFFSET 0x10000ULL
#define BTRFS_SUPER_MAGIC 0x4D5F53665248425FULL

char *output_path;
char *disk_path;

static int write_report_if_requested(const struct app_options *opts,
                                     const struct run_report *report) {
  if (opts->report_json_path &&
      write_run_report_json(opts->report_json_path, report) != 0) {
    fprintf(stderr, "Failed to write report JSON: %s\n",
            opts->report_json_path);
    return 1;
  }
  return 0;
}

static int read_super_identity(const char *path,
                               const uint8_t expected_uuid[BTRFS_UUID_SIZE],
                               uint8_t fs_uuid[BTRFS_UUID_SIZE],
                               uint64_t *generation) {
  FILE *disk = fopen(path, "rb");
  if (!disk)
    return -1;

  struct btrfs_super_block super;
  if (fseek(disk, BTRFS_SUPER_INFO_OFFSET, SEEK_SET) != 0 ||
      fread(&super, sizeof(super), 1, disk) != 1) {
    fclose(disk);
    return -1;
  }
  fclose(disk);

  if (le64toh(super.magic) != BTRFS_SUPER_MAGIC)
    return -1;
  if (expected_uuid && memcmp(super.fsid, expected_uuid, BTRFS_UUID_SIZE) != 0)
    return -1;

  if (fs_uuid)
    memcpy(fs_uuid, super.fsid, BTRFS_UUID_SIZE);
  if (generation)
    *generation = le64toh(super.generation);
  set_current_fs_profile(le32toh(super.sectorsize), le16toh(super.csum_type));
  return 0;
}

static int read_current_fs_identity(const char *path,
                                    uint8_t fs_uuid[BTRFS_UUID_SIZE],
                                    uint64_t *generation) {
  if (generation)
    *generation = 0;

  int fd = open(path, O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    struct btrfs_ioctl_fs_info_args fs_info;
    memset(&fs_info, 0, sizeof(fs_info));
    fs_info.flags = BTRFS_FS_INFO_FLAG_CSUM_INFO;
    if (ioctl(fd, BTRFS_IOC_FS_INFO, &fs_info) == 0) {
      memcpy(fs_uuid, fs_info.fsid, BTRFS_UUID_SIZE);
      int profile_loaded = (fs_info.flags & BTRFS_FS_INFO_FLAG_CSUM_INFO) != 0;
      if (profile_loaded)
        set_current_fs_profile(fs_info.sectorsize, fs_info.csum_type);

      uint64_t best_generation = 0;
      for (uint64_t devid = 1; devid <= fs_info.max_id; devid++) {
        struct btrfs_ioctl_dev_info_args dev;
        memset(&dev, 0, sizeof(dev));
        dev.devid = devid;
        if (ioctl(fd, BTRFS_IOC_DEV_INFO, &dev) != 0)
          continue;

        dev.path[BTRFS_DEVICE_PATH_NAME_MAX - 1] = '\0';
        if (dev.path[0] == '\0')
          continue;

        uint64_t dev_generation = 0;
        if (read_super_identity((const char *)dev.path, fs_uuid, NULL,
                                &dev_generation) == 0) {
          profile_loaded = 1;
          if (dev_generation > best_generation)
            best_generation = dev_generation;
        }
      }
      if (generation)
        *generation = best_generation;
      /* The mounted kernel may have selected a backup superblock. Its live
       * profile takes precedence over individual primary device copies. */
      if (fs_info.flags & BTRFS_FS_INFO_FLAG_CSUM_INFO)
        set_current_fs_profile(fs_info.sectorsize, fs_info.csum_type);
      close(fd);
      if (!profile_loaded) {
        errno = ENODATA;
        return -1;
      }
      return 0;
    }
    close(fd);
  }

  return read_super_identity(path, NULL, fs_uuid, generation);
}

int main(int argc, char **argv) {
  struct app_options opts;
  char option_error[256];
  if (parse_app_options(argc, argv, &opts, option_error,
                        sizeof(option_error)) != 0) {
    fprintf(stderr, "%s\n", option_error);
    print_usage(stderr, argv[0]);
    return 1;
  }

  struct run_report report = {
      .input_path = opts.input_path,
      .output_path = opts.output_path,
      .cache_dir = opts.cache_dir,
      .manifest_path = opts.manifest_path,
      .image_mode = opts.image_mode,
      .skip_parse = opts.skip_parse,
      .threads = opts.threads,
      .no_data_checksum = opts.no_data_checksum,
      .deleted_only = opts.deleted_only,
  };
  const struct extraction_options extraction_options = {
      .no_data_checksum = opts.no_data_checksum,
      .regular_files_only = opts.deleted_only,
  };

  struct inode_instance_id target_id = {0};
  if (opts.mode == APP_MODE_EXTRACT_ONE) {
    char target_error[128];
    if (parse_extract_target(opts.extract_target, &target_id, target_error,
                             sizeof(target_error)) != 0) {
      fprintf(stderr, "Invalid --extract target: %s\n", target_error);
      return 1;
    }
  }

  output_path = (char *)opts.output_path;
  disk_path = (char *)opts.input_path;
  if (opts.cache_dir)
    set_cache_base_path(opts.cache_dir);
  else
    set_cache_base_path(NULL);
  set_worker_thread_count(opts.threads);
  set_include_other_fs_uuid(opts.include_other_fs_uuid);

  uint8_t fs_uuid[BTRFS_UUID_SIZE] = {0};
  uint64_t fs_generation = 0;
  int fs_uuid_loaded = 0;
  if (!opts.image_mode &&
      read_current_fs_identity(disk_path, fs_uuid, &fs_generation) == 0) {
    set_current_fs_uuid(fs_uuid);
    if (fs_generation != 0)
      set_current_fs_generation(fs_generation);
    fs_uuid_loaded = 1;
  } else if (opts.image_mode &&
             read_btrfs_image_identity(opts.input_path, fs_uuid,
                                       &fs_generation) == 0) {
    set_current_fs_uuid(fs_uuid);
    if (fs_generation != 0)
      set_current_fs_generation(fs_generation);
    fs_uuid_loaded = 1;
  } else if (!opts.image_mode) {
    fprintf(stderr, "Failed to read current filesystem uuid: %s\n",
            strerror(errno));
  }

  if (opts.skip_parse && cache_format_validate() != 0) {
    fprintf(stderr,
            "Cache is missing or incompatible; rerun without --skip-parse\n");
    return 1;
  }

  if (!opts.image_mode && !fs_uuid_loaded)
    return 1;

  if (!opts.skip_parse) {
    time_t start_time = time(NULL);
    int ret = opts.image_mode
                  ? parse_btrfs_current_chunk_tree_from_images(opts.input_path,
                                                               fs_uuid)
                  : parse_btrfs_current_chunk_tree();
    if (ret != 0) {
      fprintf(stderr, "Failed to parse btrfs chunk tree\n");
      return 1;
    }
    if (opts.image_mode)
      fs_uuid_loaded = 1;
    time_t end_time = time(NULL);
    report.chunk_parse_seconds = (long)(end_time - start_time);
    printf("Time taken to parse btrfs chunk tree: %ld seconds\n",
           report.chunk_parse_seconds);
    start_time = time(NULL);
    ret = parse_btrfs_trees();
    end_time = time(NULL);
    report.tree_parse_seconds = (long)(end_time - start_time);
    printf("Time taken to parse all btrfs chunk trees: %ld seconds\n",
           report.tree_parse_seconds);
    if (ret != 0) {
      fprintf(stderr, "Failed to parse all btrfs chunk trees\n");
      return 1;
    }
    if (!opts.no_data_checksum && csum_range_index_build() != 0) {
      fprintf(stderr,
              "Failed to build checksum range index; check the cache path and "
              "rerun\n");
      return 1;
    }
    uint32_t *generations = NULL;
    size_t generation_count = 0;
    start_time = time(NULL);
    if (parse_root_tree(&generations, &generation_count) != 0) {
      fprintf(stderr, "Failed to parse root tree\n");
      return 1;
    }
    end_time = time(NULL);
    report.root_tree_seconds = (long)(end_time - start_time);
    printf("Time taken to parse root tree and reconstruct file tree: %ld seconds\n",
           report.root_tree_seconds);
    free(generations);
    if (cache_format_publish() != 0) {
      fprintf(stderr,
              "Failed to publish cache format marker; check the cache path "
              "and rerun\n");
      return 1;
    }
  }

  select_current_file_tree_fs_uuid();

  if (opts.mode == APP_MODE_LIST) {
    struct recoverable_candidate_list candidates;
    if (candidate_list_load(&candidates) != 0) {
      fprintf(stderr, "Failed to load recoverable candidates\n");
      return 1;
    }
    report.candidate_count = candidates.count;
    candidate_list_print(stdout, &candidates);
    candidate_list_free(&candidates);
    if (write_report_if_requested(&opts, &report) != 0)
      return 1;
    return 0;
  }

  ensure_cache_dir(EXTRACT_DIR);

  if (opts.mode == APP_MODE_EXTRACT_ALL) {
    if (!opts.no_data_checksum && csum_range_index_open() != 0) {
      fprintf(stderr,
              "Failed to open checksum range index; rerun without "
              "--skip-parse\n");
      return 1;
    }
    struct recoverable_candidate_list candidates;
    if (candidate_list_load(&candidates) != 0) {
      fprintf(stderr, "Failed to load recoverable candidates\n");
      if (!opts.no_data_checksum)
        csum_range_index_close();
      return 1;
    }
    report.candidate_count = candidates.count;

    if (opts.deleted_only) {
      struct deleted_candidate_stats stats;
      if (candidate_list_filter_deleted(&candidates, &stats) != 0) {
        fprintf(stderr, "Failed to classify deleted candidates\n");
        candidate_list_free(&candidates);
        if (!opts.no_data_checksum)
          csum_range_index_close();
        return 1;
      }
      report.selected_deleted = stats.selected_deleted;
      report.skipped_active = stats.skipped_active;
      report.skipped_unknown = stats.skipped_unknown;
      report.skipped_non_regular = stats.skipped_non_regular;
      printf("Deleted candidates: selected %zu, skipped active %zu, "
             "unknown %zu, non-regular %zu\n",
             stats.selected_deleted, stats.skipped_active,
             stats.skipped_unknown, stats.skipped_non_regular);
    }

    struct inode_instance_id *candidate_ids =
        candidates.count
            ? malloc(candidates.count * sizeof(*candidate_ids))
            : NULL;
    if (candidates.count && !candidate_ids) {
      fprintf(stderr, "Failed to allocate extract-all candidate list\n");
      candidate_list_free(&candidates);
      if (!opts.no_data_checksum)
        csum_range_index_close();
      return 1;
    }
    for (size_t i = 0; i < candidates.count; i++)
      candidate_ids[i] = candidates.items[i].id;
    size_t candidate_count = candidates.count;
    candidate_list_free(&candidates);

    size_t failed = 0;
    time_t start_time = time(NULL);
    int batch_result = extract_all_with_options(
        candidate_ids, candidate_count, &failed, &extraction_options);
    time_t end_time = time(NULL);
    printf("Time taken to extract all files: %ld seconds\n",
           (long)(end_time - start_time));
    free(candidate_ids);
    report.extract_failures =
        failed > (size_t)INT_MAX ? INT_MAX : (int)failed;
    if (!opts.no_data_checksum)
      csum_range_index_close();
    if (batch_result != 0) {
      report.extract_failures = -1;
      fprintf(stderr, "Failed to run extract-all batch\n");
      if (write_report_if_requested(&opts, &report) != 0)
        return 1;
      return 1;
    }
    if (failed != 0) {
      fprintf(stderr, "Failed to extract %zu candidates\n", failed);
      if (write_report_if_requested(&opts, &report) != 0)
        return 1;
      return 1;
    }
    if (write_report_if_requested(&opts, &report) != 0)
      return 1;
    return 0;
  }

  if (opts.mode == APP_MODE_EXTRACT_ONE) {
    if (!opts.no_data_checksum && csum_range_index_open() != 0) {
      fprintf(stderr,
              "Failed to open checksum range index; rerun without "
              "--skip-parse\n");
      return 1;
    }
    time_t start_time = time(NULL);
    int ret = extract_one_with_options(&target_id, &extraction_options);
    time_t end_time = time(NULL);
    printf("Time taken to extract file: %ld seconds\n",
           (long)(end_time - start_time));
    if (!opts.no_data_checksum)
      csum_range_index_close();
    if (ret != 0)
      report.extract_failures = 1;
    if (write_report_if_requested(&opts, &report) != 0)
      return 1;
    if (ret != 0)
      return 1;
    return 0;
  }

  if (opts.no_interactive) {
    fprintf(stderr, "No action selected in --no-interactive mode\n");
    return 1;
  }

  if (print_file_tree(fs_uuid_loaded ? fs_uuid : NULL, opts.print_times) != 0) {
    fprintf(stderr, "Failed to print recoverable file tree\n");
    return 1;
  }

  printf("Input root_dirid, objectid, inode_generation: ");
  fflush(stdout);
  char target_text[256];
  if (!fgets(target_text, sizeof(target_text), stdin)) {
    fprintf(stderr, "Failed to read extract target\n");
    return 1;
  }
  char target_error[128];
  if (parse_extract_target(target_text, &target_id, target_error,
                           sizeof(target_error)) != 0) {
    fprintf(stderr, "Invalid extract target: %s\n", target_error);
    return 1;
  }
  if (!opts.no_data_checksum && csum_range_index_open() != 0) {
    fprintf(stderr,
            "Failed to open checksum range index; rerun without "
            "--skip-parse\n");
    return 1;
  }
  time_t start_time = time(NULL);
  int ret = extract_one_with_options(&target_id, &extraction_options);
  time_t end_time = time(NULL);
  printf("Time taken to extract file: %ld seconds\n",
         (long)(end_time - start_time));
  if (!opts.no_data_checksum)
    csum_range_index_close();
  if (ret != 0)
    report.extract_failures = 1;
  if (write_report_if_requested(&opts, &report) != 0)
    return 1;
  if (ret != 0)
    return 1;
  return 0;
}
