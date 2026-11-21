#!/bin/sh
set -eu
if [ "$#" -ne 2 ]; then
    echo "Usage: scripts/run-container.sh DATA_DIR OUTPUT_DIR" >&2
    exit 2
fi
image=${IMAGE:-shopping-sessions:latest}
cpus=${CPUS:-8}
mem=${MEM:-24g}
platform=${PLATFORM:-linux/amd64}
data=$(cd "$1" && pwd)
mkdir -p "$2"
output=$(cd "$2" && pwd)
scratch=$(mktemp -d)
chmod a+rwx "$output" "$scratch"
trap 'rmdir "$scratch" 2>/dev/null || true' EXIT

docker run --rm --platform "$platform" \
    --network none --read-only --tmpfs /tmp:rw,size=64m,mode=1777 \
    --cap-drop ALL --security-opt no-new-privileges --pids-limit 4096 \
    --user 1000:1000 --cpus "$cpus" --memory "$mem" --memory-swap "$mem" \
    -v "$data":/data:ro -v "$output":/output -v "$scratch":/scratch \
    -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch -e HOME=/scratch \
    -e NCPU="${cpus%.*}" -e MEM_GB="${mem%g}" -e PROFILE \
    "$image"
