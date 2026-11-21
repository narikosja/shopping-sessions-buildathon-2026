#!/bin/sh
set -eu
input=$(realpath "$1")
mkdir -p "$2"
output=$(realpath "$2")
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
docker run --rm --platform linux/amd64 --network none --read-only \
    --user 1000:1000 --pids-limit 4096 --cpus 8 --memory 24g --memory-swap 24g \
    --tmpfs /tmp:rw,size=64m --env DATA_DIR=/data --env OUTPUT_DIR=/output \
    --env SCRATCH_DIR=/scratch --env HOME=/scratch --env NCPU=8 --env MEM_GB=24 \
    --mount "type=bind,source=$input,target=/data,readonly" \
    --mount "type=bind,source=$output,target=/output" \
    --mount "type=bind,source=$scratch,target=/scratch" \
    "${IMAGE:-shopping-sessions}"
