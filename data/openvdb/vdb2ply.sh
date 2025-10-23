#!/usr/bin/env bash

# vdb2ply.sh
# Converts a .vdb file to .ply
# Usage: ./vdb2ply.sh input.vdb output.ply [iso_value]

if [ "$#" -lt 2 ]; then
  echo "Usage: $0 <input.vdb> <output.ply> [iso_value]"
  exit 1
fi

INPUT="$1"
OUTPUT="$2"
ISO="${3:-0.0}"  # default iso‐value 0.0 if not provided

# Example command: convert volume to mesh and export to PLY
# Replace `vdb_to_mesh_tool` with the actual tool/command you have
vdb_to_mesh_tool --input "${INPUT}" --iso-value "${ISO}" --output-mesh "${OUTPUT}" --format ply

if [ $? -ne 0 ]; then
  echo "Error: conversion failed."
  exit 2
fi

echo "Successfully converted ${INPUT} → ${OUTPUT} (iso=${ISO})"
exit 0