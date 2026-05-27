#!/bin/bash

set -e

if [ $# -lt 1 ]; then
  echo "Usage: ./shiftStart <workDir> --number <count>"
  exit 1
fi

WORK_DIR="$1"
shift

NUMBER=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --number)
      NUMBER="$2"
      shift 2
      ;;
    *)
      echo "Unknown argument: $1"
      exit 1
      ;;
  esac
done

if ! [[ "$NUMBER" =~ ^[0-9]+$ ]]; then
  echo "--number must be an integer"
  exit 1
fi

cd "$WORK_DIR"

echo "Deleting first $NUMBER images..."

for ((i=1; i<=NUMBER; i++)); do
  FILE="out_${i}.png"

  if [ -f "$FILE" ]; then
    rm "$FILE"
  fi
done

echo "Shifting remaining files..."

find . -maxdepth 1 -type f -name 'out_*.png' | while read -r FILE; do
  BASENAME=$(basename "$FILE")

  NUM=$(echo "$BASENAME" | sed -E 's/out_([0-9]+)\.png/\1/')

  if [[ "$NUM" =~ ^[0-9]+$ ]]; then
    NEW_NUM=$((NUM - NUMBER))

    if [ "$NEW_NUM" -ge 1 ]; then
      mv "$BASENAME" "tmp_${NEW_NUM}.png"
    fi
  fi
done

for FILE in tmp_*.png; do
  [ -e "$FILE" ] || continue

  FINAL_NAME=$(echo "$FILE" | sed 's/^tmp_/out_/')

  mv "$FILE" "$FINAL_NAME"
done

echo "Done."