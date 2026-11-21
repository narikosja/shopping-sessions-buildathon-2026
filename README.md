# Shopping Sessions

A multithreaded C++20 solution for the [Databricks Shopping Sessions challenge](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). The executable reads `DATA_DIR` (default `/data`) and writes `user_daily/` and `campaign_daily/` under `OUTPUT_DIR` (default `/output`). `NCPU` sets the worker count. No runtime dependencies are downloaded and the pipeline does not require spill storage.

## Build and run

```sh
make docker
scripts/run-container.sh /path/to/data /path/to/output
```

The target is Linux AMD64. The scoring image starts the compiled program directly as UID/GID 1000. Use an output directory writable by that UID. Stage timings are printed to stderr.

## Verify against the official data

Install Python 3.12, C++20 (GCC 12 or newer), and Rust 1.85 or newer for local validation. Run:

```sh
python3 -m pip install -r requirements.txt -r requirements-test.txt
make official generator dev
make build fixtures run-dev check-dev
make fixtures run-dev check-dev RUNNER=scripts/run-container.sh OUT=out/container
```

Before the container checks, build the image with `make docker`. Fixtures and expected rows are downloaded from the organizer and ignored by Git.

## Algorithm

- Memory-map the CSV parts, scan only LF-delimited lines, and validate quoting and column counts before deduplication.
- Use union-find with path compression and union by size. Keep each component's byte-wise smallest ID as its name.
- Partition event copies by a hash of the exact event ID. Keep the minimum ingest timestamp even when the winning copy is invalid. Partition survivors and valid clicks by resolved person.
- Sort each partition by local person index and timestamp, then collect sessions and user-day rows in one pass. Use a rolling click window with per-campaign counts to assign credit and unique assists.
- Write standard Parquet date, integer, and UTF-8 columns directly with Arrow's C++ Parquet writer.

## Practice benchmark

```sh
official/generator/target/release/generate \
  --seed 271828 --events 20000000 --clicks 5000000 --output bench/practice
time scripts/run-container.sh bench/practice bench/out
```

The generated practice input uses a public generator seed. It is suitable for local timing and regression checks.

## Local verification results

The image built from this Dockerfile passed all 36 official fixtures and matched all
117,038 `user_daily` and 27,400 `campaign_daily` rows in the published dev answer.
A one-worker native run produced the same logical rows as the eight-worker run.
Two additional fixtures passed: empty input and a 2,000-link chain with a CSV
line over 2 MiB and pre-epoch UTC dates.

The Dockerfile was built with Kaniko. The resulting image filesystem and entrypoint
were run in Linux namespaces with UID 1000, offline networking, a read-only root
and input, and eight pinned logical CPUs. Wall time includes launching that
sandbox. These are local measurements on an AMD EPYC 7763 host.

| Generated events | Clicks | Wall time |
| ---: | ---: | ---: |
| 1,000,000 | 250,000 | 0.51 s |
| 5,000,000 | 1,250,000 | 1.76 s |
| 20,000,000 | 5,000,000 | 4.69 s median (4.08, 4.69, 6.70 s) |

The 20M-event runs peaked at 4.11 GiB resident memory. Both output tables matched
the verified baseline on that input. Median stage timings in seconds:

| Stage | Seconds |
| --- | ---: |
| Identity resolution and lookups | 0.436 |
| CSV parsing | 1.929 |
| Event deduplication | 0.756 |
| Person indexing and sorting | 0.937 |
| Sessionization and user aggregation | 0.039 |
| Attribution and assists | 0.106 |
| Campaign aggregation | 0.009 |
| Parquet output | 0.128 |
