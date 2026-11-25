# BRM

**BRM (Btrfs Reconstruction from residual Metadata)** is a Linux command-line tool for recovering files from Btrfs. It scans system and metadata chunks for surviving tree blocks, including older blocks left behind by copy-on-write updates. If a directory entry has disappeared from the live filesystem, those blocks may still identify the file, its parent directory, and the extents that hold its contents.

BRM builds a tree of recovery candidates from a mounted Btrfs filesystem or a set of unmounted device images. You can inspect the tree, extract a single file or directory subtree, or recover candidates in a batch. BRM writes the metadata cache and recovered files to a separate output location.

## Build

Use Linux with a C11 compiler, Linux Btrfs userspace headers, POSIX threads, and development libraries for xxHash, libsodium, zlib, LZO2, and Zstandard. The GCC preset below requires Ninja and CMake 3.25 or newer. On Debian or Ubuntu, install the build dependencies with the following command.

```sh
sudo apt install build-essential cmake ninja-build linux-libc-dev \
  libxxhash-dev libsodium-dev zlib1g-dev liblzo2-dev libzstd-dev
```

Configure and build from the repository root.

```sh
cmake --preset gcc-debug
cmake --build --preset gcc-debug
```

The current build produces **`build/gcc-debug/c_demo`**, the BRM executable used below. For CMake 3.20 through 3.24, configure without a preset using `cmake -S . -B build/gcc-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug`, then run `cmake --build build/gcc-debug`.

## Recover from device images

1. **List candidates.** Put the raw Btrfs device image files for one filesystem in a single directory. Each image must start at the Btrfs device boundary and have a readable primary superblock at 64 KiB. Include all member images for a multi-device filesystem. Set the paths below to your image directory and a new output directory on separate storage.

   ```sh
   images=/path/to/device-images
   out=/path/to/new-recovery-output
   ./build/gcc-debug/c_demo --image-dir "$images" --list "$out"
   ```

   The first run builds the cache in `$out/cache/`. Each candidate has a root ID, an object ID, and an inode generation. This example identifies one inode instance. The generation distinguishes files that reused the same object ID.

   ```text
   root=0x5 object=0x101 inode_generation=0x7
   ```

2. **Extract one candidate.** Copy the three values from your own listing, in that order. A directory or subvolume root triggers recursive extraction. To browse names and parent directories before choosing, omit `--extract` and its argument to open the interactive tree.

   ```sh
   ./build/gcc-debug/c_demo --image-dir "$images" --skip-parse \
     --extract 0x5,0x101,0x7 "$out"
   ```

   Look in `$out/extract/` for the recovered files. BRM prefixes names with root and object IDs. If a file has both complete and salvaged extent records, the `[Complete]` variant uses the complete records alone and the `[salvaged]` variant also incorporates the salvaged records. Check either variant against a known hash when one is available.

3. **Extract a batch.** To reuse the same cache and keep the batch output separate, pass the original output directory as the cache base. Choose a new destination with room for all recovered files.

   ```sh
   batch=/path/to/new-batch-output
   ./build/gcc-debug/c_demo --image-dir "$images" --cache-dir "$out" --skip-parse \
     --extract-all --threads 4 --report-json "$batch/run.json" "$batch"
   ```

   The batch includes disconnected candidates as well as files reachable through the reconstructed tree. To select only regular files whose surviving parent records indicate deletion, add `--deleted-only`. Missing or conflicting records and multiple hard links can prevent a deleted file from being selected. An empty selection is a successful run.

## Recover from a mounted filesystem

Pass the mount point as the first positional argument and a destination on another filesystem as the second. BRM queries the mount using Linux Btrfs ioctls and reads data from its backing devices. This may require elevated privileges.

```sh
sudo ./build/gcc-debug/c_demo --list /mnt/btrfs /recovery/mounted-case
```

The same extraction actions work with mounted input. For a repeatable view of the source, use unchanged device images.

## Cache and run options

A fresh parse clears and rebuilds the contents of `<cache-base>/cache/`. The cache base defaults to the output directory. With `--skip-parse`, use the same unchanged source at the same paths and a compatible cache (current format 3). Extraction still reads file data from the source devices or images. A new output directory keeps earlier extracted files out of the results.

| Option | Effect |
| --- | --- |
| `--cache-dir BASE` | Store parsed metadata in `BASE/cache/` and write recovered files under the positional output directory. |
| `--threads N` | Set the number of extraction workers. The default uses the number of online processors. |
| `--print-times` | Include available inode creation and modification times in the interactive tree. |
| `--include-other-fs-uuid` | Also parse surviving tree blocks carrying a different filesystem UUID. Image discovery still selects one filesystem. |
| `--no-data-checksum` | Disable data checksum validation during extraction. Overlapping extent records are replayed in cache order, so later writes can replace earlier bytes. Compressed data must still decode successfully. |
| `--no-interactive` | Require an explicit action in scripts. Choose one of `--list`, `--extract`, or `--extract-all`. |
| `--report-json PATH` | Write input and output paths, mode, parsing times, candidate counts, and extraction failures to a JSON file. |
| `--manifest PATH` | Record a manifest path in the BRM run report. |

## Format support and recovery limits

| Feature | Coverage |
| --- | --- |
| Chunk profiles | SINGLE, DUP, RAID0, RAID1, RAID10, RAID1C3, and RAID1C4. RAID5 and RAID6 are unsupported. |
| File extents | Uncompressed inline and regular extents, plus preallocated extent records (see below). Inline and regular extents can also use zlib, LZO, or Zstandard compression. Encrypted extents are unsupported. |
| Data checksums | CRC32C, xxHash64, SHA-256, and BLAKE2b-256. BRM uses available checksum records for compressed regular extents and to resolve overlaps in uncompressed data. |
| Metadata layout | The residual scan reads 16 KiB tree blocks in system and metadata chunks. It skips mixed data/metadata chunks. |

For preallocated extents, BRM reads the mapped storage bytes, which may differ from the logical contents of unwritten ranges.

The inode generation selects an inode instance. Extents recorded for that instance can span several updates, so the result may combine data from different points in time. Overwritten data, missing mappings, and lost directory records can leave files incomplete or disconnected from their original paths.

A successful extraction can contain unverified bytes. Uncontested uncompressed extents may be copied without a data checksum check, inline data has no separate extent checksum, and BRM does not verify tree-block checksums.

## Reproduce a recovery experiment

The Python scripts require Python 3.10 or later. The scenario also uses GNU `cp` and `sync`. Run the commands from the repository root. Use a dedicated, writable Btrfs test filesystem for the workload, and keep the source corpus, ground truth, recovered files, and reports on separate storage from that filesystem.

1. **Generate source files.** Choose one of the two commands below and start with a fresh source directory. This small example covers all scenario types.

   ```sh
   work=/path/to/experiment-storage
   python3 script/generate_btrfs_eval_sources.py "$work/source" \
     --count-per-class 1 --chain2-under-to-inline-count 1 --chain2-inline-to-under-count 1 \
     --chain2-over-to-under-count 1 --chain2-under-to-over-count 1 \
     --inline-size 512 --under-size 64KiB --over-size 1MiB
   ```

   To generate the paper's 2,300 source files, use the default counts and size ranges with the study seed `20260905`. Allow about 223 GiB of free space for this step.

   ```sh
   work=/path/to/experiment-storage
   python3 script/generate_btrfs_eval_sources.py "$work/source" --seed 20260905
   ```

   `--count-per-class` sets the cases per size class for each of the six general scenarios (default 100). Each of the four chain2 count options shown above sets the count for one size transition (default 50 each). `--seed` selects the dataset seed (default `20260811`). Supply all three fixed sizes together, or set ranges with `--inline-min-size` / `--inline-max-size` and their `--under-` and `--over-` counterparts. Sizes accept bytes or units such as `KiB` and `MiB`. Use a new output path by default. `--force` deletes and recreates an existing output directory, and `--skip-space-check` bypasses the free-space check.

2. **Run the file operations.** Set `target` to the mount point of your Btrfs test filesystem. The runner creates, modifies, and deletes experiment files there and writes the ground truth under `$work/truth/$run_id/`. Choose a fresh run ID for each workload.

   ```sh
   target=/mnt/btrfs-experiment
   run_id=evaluation-single
   python3 script/run_btrfs_eval_scenario.py "$work/source" "$target" \
     --results-dir "$work/truth" --run-id "$run_id"
   ```

   `--run-id` names the workload and its ground-truth directory (default `evaluation-001`). Without `--results-dir`, the runner creates a sibling of the source directory named `<source-name>_results`. `--pause-seconds` sets the delay after each stage sync (default 0.05 seconds). A value of `0` keeps the sync and skips the delay.

3. **Recover and verify.** After the workload finishes, unmount the test filesystem and use its device images as described above. Pass the BRM output directory and the ground truth from the same run to the verifier. It compares recovered bytes with each object's last contents before deletion or replacement, and writes `recovery_verification_summary.csv` under the report directory.

   ```sh
   images=/path/to/device-images-from-this-run
   recovered="$work/recovered/$run_id"
   ./build/gcc-debug/c_demo --image-dir "$images" --extract-all "$recovered"
   python3 script/verify_recovered_files_tools.py "$recovered" "$work/truth/$run_id" \
     --tool proposed --output-dir "$work/report/$run_id"
   ```

### Ground truth from the paper

The [datasets_results/](datasets_results/) directory contains the ground truth recorded during the experiments on SINGLE, DUP, RAID0, RAID1, RAID10, RAID1C3 and RAID1C4. Each `evaluation-<profile>/` directory records the histories of 2,500 objects and identifies each object's last version before deletion or replacement. Local paths in the run configuration and recorded commands are relative in this copy.

For an export from an original experiment, pass the matching directory (for example, [datasets_results/evaluation-single/](datasets_results/evaluation-single/)) to the verifier. For a newly generated workload, use the ground truth written by that run in step 2. The original device images are not distributed.

### Verify exports from other recovery tools

Use `--tool hetman` for Hetman, UFS Explorer, btrfs restore, or btrfscue exports. This selects the verifier's shared name/path-based interface. The CSV `recovery_tool` field remains `hetman`, so identify the actual tool in your report directory and result labels.

Point `export_root` at the extracted files, preserving separate directories for different roots or candidates, and use the ground truth from the same scenario run. The paper's main comparison uses `all_candidates`.

```sh
tool_name=hetman  # Or ufs, btrfs_restore, or btrfscue.
export_root=/path/to/recovery-export
python3 script/verify_recovered_files_tools.py "$export_root" "$work/truth/$run_id" \
  --tool hetman --mode all_candidates --output-dir "$work/report/$tool_name/$run_id"
```

| Selection mode | Files considered |
| --- | --- |
| `no_suffix` | Only files without a numeric duplicate suffix. Given `file.bin`, `file (1).bin`, and `file (2).bin`, this selects `file.bin`. |
| `max_suffix` | One file per name group in each recovered directory. The same example selects `file (2).bin`. A group with no numbered copy keeps its unnumbered file. |
| `all_candidates` | All three files in the example are eligible for comparison. The verifier scores the best candidate for each target without combining bytes from different copies. |
| `all` (default for the `hetman` interface) | Run the three modes above and write `recovery_verification_summary_<mode>.csv` for each. Choose a single mode to produce only its CSV. |

### Other verifier options

- `--tool proposed` evaluates BRM output by object and writes `recovery_verification_summary.csv`. Omit `--mode` for BRM. The default `--tool auto` recognizes BRM under `~/output-Proposed` and HETMAN under `~/output`. For other locations, set the tool explicitly.
- `--batch` takes two parent directories. For example, it pairs `<recovery-root>/single/` with `<truth-root>/evaluation-single/` and combines the profile rows into one CSV per selected mode.
- `--output-dir DIR` selects the report directory. Use a path separate from both inputs. `--cache-db FILE` reuses measurements across runs through a SQLite file outside the inputs and report directory. The default uses memory only. `--no-cache` disables persistent reuse.
- The source generator, scenario runner and `verify_recovered_files_tools.py` accept `--help`, `--quiet`, and `--version`. The verifier exits with `0` on completed verification, `2` when some targets could not be verified or an inventory read failed, and `1` on a runtime error. Consult the CSV for recovery rates.

### Verify an mbkn-btrfs-rescue export

The paper evaluated mbkn-btrfs-rescue 0.4.0 on SINGLE and DUP using its best view with `--include all --include-stale`. The [mbkn adapter](script/evaluate_mbkn.py) reads the export's restore TSV and passes the listed regular files with actions `written`, `reconstructed` or `exists` to the same 3.0.2 verifier for content comparison and representative selection.

For a SINGLE export from the paper's experiment, set the two input paths to your export root and the actual TSV inside it. TSV entries are relative to the directory containing the TSV. Choose a new report directory outside the export and ground truth.

```sh
mbkn_export=/path/to/mbkn-export
restore_tsv="$mbkn_export/5/namespace/.mbkn-restore-YYYYMMDD-HHMMSS.tsv"
python3 script/evaluate_mbkn.py --verifier-root script \
  --ground-truth datasets_results/evaluation-single --profile single \
  --recovery-root "$mbkn_export" --restore-manifest "$restore_tsv" \
  --run-id mbkn-single --output-dir ./reports/mbkn-single
```

For DUP, use `datasets_results/evaluation-dup`, `--profile dup` and a fresh run label and output directory. Pass `--restore-manifest` once per TSV if the export has several. The adapter writes counts to `summary.csv` and per-object verdicts to `targets.csv`.
