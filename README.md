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
make build check-csv fixtures run-dev check-dev
make fixtures run-dev check-dev RUNNER=scripts/run-container.sh OUT=out/container
```

Before the container checks, build the image with `make docker`. Fixtures and expected rows are downloaded from the organizer and ignored by Git.

## Algorithm

- Memory-map the CSV parts and scan LF, commas, and quotes together with AVX2 when supported. A scalar parser handles other CPUs. Both validate quoting and column counts before deduplication.
- Store short nonzero ASCII IDs as reversible 64-bit keys. Preserve other IDs by reference and compare them byte for byte.
- Build the union-find identity graph concurrently with event parsing. Keep each component's byte-wise smallest ID as its name.
- Buffer event copies in fixed-size blocks and partition them by a hash of the exact event ID. Keep the minimum ingest timestamp even when the winning copy is invalid. Resolve the identities of surviving events and valid clicks before partitioning by person.
- Group each partition by compact person index and sort timestamps within each person, then collect sessions and user-day rows in one pass. Use a rolling click window with per-campaign counts to assign credit and unique assists.
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
line over 2 MiB and pre-epoch UTC dates. The streaming CSV parser, ID encoding, and timestamp fast path passed a randomized differential test
with 1,000,000 generated cases each.

The Dockerfile was built with Kaniko. The resulting image filesystem and entrypoint
were run in Linux namespaces with UID 1000, offline networking, a read-only root
and input, and eight pinned logical CPUs. Wall time includes launching that
sandbox. These are local measurements on an AMD EPYC 7763 host.

| Generated events | Clicks | Wall time |
| ---: | ---: | ---: |
| 1,000,000 | 250,000 | 0.33 s |
| 5,000,000 | 1,250,000 | 0.80 s |
| 20,000,000 | 5,000,000 | 1.90 s median (1.86, 1.90, 2.58 s) |

The 20M-event runs peaked at 3.54 GiB resident memory. Both output tables matched
the previous scored implementation on the 5M and 20M inputs. Median stage timings in seconds:

| Stage | Seconds |
| --- | ---: |
| Setup | 0.010 |
| CSV parsing and concurrent identity resolution | 0.693 |
| Event deduplication and person partitioning | 0.484 |
| Person indexing and sorting | 0.238 |
| Sessionization and user aggregation | 0.040 |
| Attribution and assists | 0.110 |
| Campaign aggregation | 0.009 |
| Parquet output | 0.116 |
