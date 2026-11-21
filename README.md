# Databricks Shopping Sessions

C++20 implementation of the [official rules](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/rules.md). It produces `user_daily` and `campaign_daily` as Parquet, including on inputs where either table is empty.

The final performance target is **0.34 seconds on 60 million events**, from container start through CSV reads, processing, both Parquet writes, and container exit.

## Build and run

```sh
docker build --platform linux/amd64 -t shopping-sessions:latest .
scripts/run-container.sh /path/to/data /path/to/fresh-output
```

The build pins the base image and the Arrow 21.0.0 wheel by digest. For a build behind a proxy that uses a local CA, pass `--secret id=build_ca,src=/path/to/ca-bundle.pem` to Docker. The runtime uses UID 1000, requires no network, and reads `DATA_DIR`, `OUTPUT_DIR`, and `NCPU`. It also accepts explicit data and output paths when built as a local binary. The runner grants the container write access to its output and scratch directories, including under rootless Docker.

The pipeline memory-maps CSV splits, uses SIMD to validate delimiters and quotes, resolves links with union-find, and partitions events by exact event ID for deduplication. It stores timestamps relative to the window and packs purchase values and category IDs when the input fits those fields. The earliest well-formed copy determines whether an event survives validation. Person partitions are sorted and processed independently. A sliding window tracks distinct campaigns and the most recent eligible click. Arrow writes native Parquet dates and nullable categories.

## Validate with official assets

Unpack the DuckDB starter, generator, and expected development CSVs from the [challenge page](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). The starter supplies independent output comparison scripts. Build the generator with `cargo build --release --locked` in its own directory.

```sh
python3 -m pip install polars==1.35.2
export STARTER=/absolute/path/to/starter-duckdb
export GENERATE=/absolute/path/to/generator/target/release/generate
export DEV_ANSWER=/absolute/path/to/dev-answer

python3 "$STARTER/scripts/run_fixtures.py" "$STARTER/fixtures" -- scripts/run-container.sh
"$GENERATE" --seed 1 --events 1000000 --clicks 250000 --output dev
scripts/run-container.sh dev out/dev
python3 "$STARTER/scripts/compare.py" out/dev "$DEV_ANSWER"
```

## Benchmark

```sh
"$GENERATE" --seed 27183 --events 20000000 --clicks 5000000 --output bench/practice
time PROFILE=1 scripts/run-container.sh bench/practice out/practice
"$GENERATE" --seed 27183 --events 60000000 --clicks 15000000 --output bench/final
time PROFILE=1 scripts/run-container.sh bench/final out/final
```

Repeat the run with `PROFILE` unset to measure total time alone. The profile reports time for catalog and link loading, event parsing, deduplication and identity resolution, click parsing and identity resolution, sorting, sessionization with attribution and aggregation, and Parquet writing. Because each session is attributed and aggregated in the same pass, the profile reports those three operations together.

The local Docker measurements used two vCPUs and 8 GiB of memory. Times are medians of three runs using Docker's container start and exit timestamps.

| Workload | Median container run | Validation |
| --- | ---: | --- |
| Official development dataset, 1 million events | 0.348 s | Both tables match the official development answers |
| Generator seed 27183, 20 million events | 4.221 s | Both tables match the baseline implementation |
| Generator seed 27183, 60 million events | 18.932 s | Both tables match the baseline implementation |

The practice-size median was 5.667 seconds before this optimization. The 60-million-event input was 5.66 GB. One CSV shard was mounted from RAM to fit the dataset and Docker writable layers in the local workspace. The official scorer uses hidden workloads and eight pinned vCPUs.
