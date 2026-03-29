#!/bin/bash

# ----------------------------
# makevideo.sh
# Usage: ./makevideo.sh --length <seconds> <input_dir> <output.mp4>
# ----------------------------

# Argument check
if [ "$#" -ne 4 ]; then
    echo "Usage: $0 --length <seconds> <input_dir> <output.mp4>"
    exit 1
fi

if [ "$1" != "--length" ]; then
    echo "First argument must be --length"
    exit 1
fi

LENGTH="$2"
INPUT_DIR="$3"
OUTPUT="$4"

# Verify input directory
if [ ! -d "$INPUT_DIR" ]; then
    echo "Input directory does not exist: $INPUT_DIR"
    exit 1
fi

# Get numeric list of images
FILES=($(ls "$INPUT_DIR"/out_*.png 2>/dev/null | sort -V))

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "No images found in $INPUT_DIR"
    exit 1
fi

# Calculate frame rate to match total video length
NUM_IMAGES=${#FILES[@]}
FPS=$(awk "BEGIN {print $NUM_IMAGES / $LENGTH}")

# Create temporary directory for sequentially numbered images
TMPDIR=$(mktemp -d)
i=0
for f in "${FILES[@]}"; do
    cp "$f" "$TMPDIR/$(printf "%05d.png" $i)"
    ((i++))
done

# Encode video from sequence
ffmpeg -y -framerate "$FPS" -i "$TMPDIR/%05d.png" \
    -vf "scale=trunc(iw/2)*2:trunc(ih/2)*2" \
    -pix_fmt yuv420p "$OUTPUT"

# Cleanup
rm -rf "$TMPDIR"

echo "Video created: $OUTPUT"