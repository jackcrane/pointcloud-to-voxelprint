#!/usr/bin/env bash

set -e

# --- Parse args ---
NUMBER=0

while [[ "$1" == --* ]]; do
  case "$1" in
    --number)
      NUMBER="$2"
      shift 2
      ;;
    *)
      echo "Unknown option: $1"
      exit 1
      ;;
  esac
done

INPUT_DIR="$1"
OUTPUT_DIR="$2"

if [[ -z "$NUMBER" || -z "$INPUT_DIR" || -z "$OUTPUT_DIR" ]]; then
  echo "Usage: ./pad.sh --number N input_dir output_dir"
  exit 1
fi

# --- Copy input -> output ---
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"
cp "$INPUT_DIR"/out_*.png "$OUTPUT_DIR"/

cd "$OUTPUT_DIR"

# --- Get sorted list of files numerically ---
mapfile -t FILES < <(ls out_*.png | sort -V)

# --- Rename from highest -> lowest (avoid overwrite) ---
for (( i=${#FILES[@]}-1; i>=0; i-- )); do
  FILE="${FILES[$i]}"
  N=$(echo "$FILE" | sed -E 's/out_([0-9]+)\.png/\1/')
  NEW_N=$((N + NUMBER))
  mv "$FILE" "out_${NEW_N}.png"
done

# --- Find first image (original smallest n, now shifted) ---
FIRST_SHIFTED="out_${NUMBER}.png"

if [[ ! -f "$FIRST_SHIFTED" ]]; then
  echo "Error: expected $FIRST_SHIFTED not found"
  exit 1
fi

# --- Copy first image down to out_0.png ---
for (( i=0; i<NUMBER; i++ )); do
  cp "$FIRST_SHIFTED" "out_${i}.png"
done

echo "Done."