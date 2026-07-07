#!/usr/bin/env bash
# Regenerates docs/abi-ground-truth.txt from the sibling tt-kmd checkout.
# Run after every upstream tt-kmd rebase (see maintenance guide).
set -euo pipefail
cd "$(dirname "$0")"
gcc -Wall -Werror -o /tmp/abi_ground_truth abi_ground_truth.c
/tmp/abi_ground_truth > ../docs/abi-ground-truth.txt
rm -f /tmp/abi_ground_truth
echo "Wrote docs/abi-ground-truth.txt ($(wc -l < ../docs/abi-ground-truth.txt) lines)"
