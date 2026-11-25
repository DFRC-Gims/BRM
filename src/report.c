#include "report.h"

#include <stdio.h>

static int write_raw(FILE *file, const char *value) {
  return fprintf(file, "%s", value) < 0 ? -1 : 0;
}

static int write_json_string(FILE *file, const char *value) {
  if (!value) {
    return write_raw(file, "null");
  }

  if (fputc('"', file) == EOF)
    return -1;
  for (const char *cursor = value; *cursor; cursor++) {
    unsigned char ch = (unsigned char)*cursor;
    switch (ch) {
    case '\\':
      if (write_raw(file, "\\\\") != 0)
        return -1;
      break;
    case '"':
      if (write_raw(file, "\\\"") != 0)
        return -1;
      break;
    case '\n':
      if (write_raw(file, "\\n") != 0)
        return -1;
      break;
    case '\r':
      if (write_raw(file, "\\r") != 0)
        return -1;
      break;
    case '\t':
      if (write_raw(file, "\\t") != 0)
        return -1;
      break;
    default:
      if (ch < 0x20) {
        if (fprintf(file, "\\u00%02X", ch) < 0)
          return -1;
      } else if (fputc(ch, file) == EOF) {
        return -1;
      }
      break;
    }
  }
  if (fputc('"', file) == EOF)
    return -1;
  return 0;
}

int write_run_report_json(const char *path, const struct run_report *report) {
  if (!path || !report)
    return -1;

  FILE *file = fopen(path, "w");
  if (!file)
    return -1;

  int failed =
      write_raw(file, "{\n") != 0 ||
      write_raw(file, "  \"input_path\": ") != 0 ||
      write_json_string(file, report->input_path) != 0 ||
      write_raw(file, ",\n  \"output_path\": ") != 0 ||
      write_json_string(file, report->output_path) != 0 ||
      write_raw(file, ",\n  \"cache_dir\": ") != 0 ||
      write_json_string(file, report->cache_dir) != 0 ||
      write_raw(file, ",\n  \"manifest_path\": ") != 0 ||
      write_json_string(file, report->manifest_path) != 0 ||
      fprintf(file, ",\n  \"image_mode\": %d", report->image_mode) < 0 ||
      fprintf(file, ",\n  \"skip_parse\": %d", report->skip_parse) < 0 ||
      fprintf(file, ",\n  \"threads\": %d", report->threads) < 0 ||
      fprintf(file, ",\n  \"data_checksum\": \"%s\"",
              report->no_data_checksum ? "disabled" : "enabled") < 0 ||
      fprintf(file, ",\n  \"deleted_only\": %s",
              report->deleted_only ? "true" : "false") < 0 ||
      fprintf(file, ",\n  \"chunk_parse_seconds\": %ld",
              report->chunk_parse_seconds) < 0 ||
      fprintf(file, ",\n  \"tree_parse_seconds\": %ld",
              report->tree_parse_seconds) < 0 ||
      fprintf(file, ",\n  \"root_tree_seconds\": %ld",
              report->root_tree_seconds) < 0 ||
      fprintf(file, ",\n  \"candidate_count\": %zu",
              report->candidate_count) < 0 ||
      fprintf(file, ",\n  \"selected_deleted\": %zu",
              report->selected_deleted) < 0 ||
      fprintf(file, ",\n  \"skipped_active\": %zu",
              report->skipped_active) < 0 ||
      fprintf(file, ",\n  \"skipped_unknown\": %zu",
              report->skipped_unknown) < 0 ||
      fprintf(file, ",\n  \"skipped_non_regular\": %zu",
              report->skipped_non_regular) < 0 ||
      fprintf(file, ",\n  \"extract_failures\": %d",
              report->extract_failures) < 0 ||
      write_raw(file, "\n}\n") != 0 || ferror(file);

  if (fclose(file) != 0 || failed)
    return -1;
  return 0;
}
