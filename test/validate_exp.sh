#!/bin/bash

set -euo pipefail

# --- begin runfiles.bash initialization --- {{{
# Copy-pasted from Bazel's Bash runfiles library https://github.com/bazelbuild/bazel/blob/defd737761be2b154908646121de47c30434ed51/tools/bash/runfiles/runfiles.bash
if [[ ! -d "${RUNFILES_DIR:-/dev/null}" && ! -f "${RUNFILES_MANIFEST_FILE:-/dev/null}" ]]; then
  if [[ -f "$0.runfiles_manifest" ]]; then
    export RUNFILES_MANIFEST_FILE="$0.runfiles_manifest"
  elif [[ -f "$0.runfiles/MANIFEST" ]]; then
    export RUNFILES_MANIFEST_FILE="$0.runfiles/MANIFEST"
  elif [[ -f "$0.runfiles/bazel_tools/tools/bash/runfiles/runfiles.bash" ]]; then
    export RUNFILES_DIR="$0.runfiles"
  fi
fi
if [[ -f "${RUNFILES_DIR:-/dev/null}/bazel_tools/tools/bash/runfiles/runfiles.bash" ]]; then
  # shellcheck disable=SC1090,SC1091
  source "${RUNFILES_DIR}/bazel_tools/tools/bash/runfiles/runfiles.bash"
elif [[ -f "${RUNFILES_MANIFEST_FILE:-/dev/null}" ]]; then
  # shellcheck disable=SC1090
  source "$(grep -m1 "^bazel_tools/tools/bash/runfiles/runfiles.bash " \
            "$RUNFILES_MANIFEST_FILE" | cut -d ' ' -f 2-)"
else
  echo >&2 "ERROR: cannot find @bazel_tools//tools/bash/runfiles:runfiles.bash"
  exit 1
fi
# --- end runfiles.bash initialization --- }}}

# This script is always run from the repo root, so `logging.sh` doesn't exist
# where shellcheck thinks it does.
# shellcheck disable=SC1091
source "test/logging.sh"

# Argument Parsing #############################################################

# Positional arguments
build_dir=${1/--build_dir=/}
shift 1

# sources make up the remaining argumenets
rb=( "$@" )

# Environment Setup ############################################################

root=$PWD

llvm_diff_path="$root/external/llvm_toolchain_12_0_0/bin/llvm-diff"

ruby="$(rlocation sorbet_ruby_2_7/ruby)"
diff_diff="$(rlocation com_stripe_ruby_typer/test/diff-diff.rb)"

diff_dir="$(mktemp -d)"

cleanup() {
  rm -rf "$diff_dir"
}
trap cleanup EXIT

# Main #########################################################################

info "Checking Build:"
pushd "$build_dir/" > /dev/null

exts=("llo")
for ext in "${exts[@]}"; do
  exp="$root/${rb[0]%.rb}.$ext.exp"
  if [ -f "$exp" ]; then
    actual="${rb[0]}.$ext"
    if [ ! -f "$actual" ]; then
      fatal "No LLVMIR found at" "$actual"
    fi
    if [[ "$OSTYPE" != "darwin"* ]]; then
      diff_out="${diff_dir}/${ext}.diff"

      # NOTE: because of https://bugs.llvm.org/show_bug.cgi?id=48137, we pipe
      # the output of llvm-diff through the diff-diff.rb script to clean out
      # spurious errors from its output. This will also take over returning the
      # correct exit code if it discovers that all of the differences were
      # related to the bug.
      if ($llvm_diff_path "$exp" "$actual" 2>&1 || true) | "$ruby" "$diff_diff" > "$diff_out" ; then
        if grep "exists only in" "$diff_out" > /dev/null ; then
          cat "$diff_out"
          fatal "* $(basename "$exp")"
        else
          success "* $(basename "$exp")"
        fi
      else
        cat "$diff_out"
        fatal "* $(basename "$exp")"
      fi
    fi
  fi
done
popd > /dev/null

success "Test passed"
