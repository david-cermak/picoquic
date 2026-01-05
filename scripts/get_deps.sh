#!/usr/bin/env bash
set -euo pipefail

# Downloads pinned GitHub dependencies into ./deps/<repo> (exact commit IDs).
#
# Pinned repositories:
# - https://github.com/david-cermak/esp-http3.git      @ 86a415a24313b8f24d1237e46909e921ba183684
# - https://github.com/private-octopus/picoquic.git    @ ba0bcac1c08c00f69eabb999aad4cccfc9e247b6
# - https://github.com/h2o/picotls.git                 @ f350eab60742138ac62b42ee444adf04c7898b0d
# - https://github.com/david-cermak/esp-protocols.git  @ e9c2d32b8e4a003f0a1b55286459a4c28e629968

usage() {
  cat <<'EOF'
Usage: scripts/get_deps.sh [--force] [--deps-dir DIR]

Options:
  --force         Re-download and overwrite existing deps/<repo> directories.
  --deps-dir DIR  Target directory for dependencies (default: <repo-root>/deps).
  -h, --help      Show this help.
EOF
}

FORCE=0
DEPS_DIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --force) FORCE=1; shift ;;
    --deps-dir)
      DEPS_DIR="${2:-}"
      if [[ -z "${DEPS_DIR}" ]]; then
        echo "error: --deps-dir requires a value" >&2
        exit 2
      fi
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
DEPS_DIR="${DEPS_DIR:-${REPO_ROOT}/deps}"

need_cmd() {
  local cmd="$1"
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "error: missing required command: ${cmd}" >&2
    exit 1
  fi
}

need_cmd tar
need_cmd mktemp
need_cmd rm
need_cmd mv
need_cmd mkdir

download_to() {
  local url="$1"
  local out="$2"

  if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 --retry-delay 1 -o "${out}" "${url}"
  elif command -v wget >/dev/null 2>&1; then
    wget -q --tries=3 -O "${out}" "${url}"
  else
    echo "error: need curl or wget to download dependencies" >&2
    exit 1
  fi
}

pin_marker_path() {
  local dest_dir="$1"
  echo "${dest_dir}/.pquic_pinned_dep"
}

write_pin_marker() {
  local dest_dir="$1"
  local repo_url="$2"
  local commit="$3"
  local marker
  marker="$(pin_marker_path "${dest_dir}")"
  cat >"${marker}" <<EOF
repo=${repo_url}
commit=${commit}
EOF
}

read_pin_marker_commit() {
  local dest_dir="$1"
  local marker
  marker="$(pin_marker_path "${dest_dir}")"
  if [[ ! -f "${marker}" ]]; then
    return 1
  fi
  # shellcheck disable=SC1090
  source "${marker}"
  if [[ -z "${commit:-}" ]]; then
    return 1
  fi
  echo "${commit}"
}

fetch_github_repo_at_commit() {
  local owner="$1"
  local repo="$2"
  local commit="$3"

  local repo_url="https://github.com/${owner}/${repo}.git"
  local dest_dir="${DEPS_DIR}/${repo}"
  local existing_commit=""

  if [[ -d "${dest_dir}" ]]; then
    if existing_commit="$(read_pin_marker_commit "${dest_dir}" 2>/dev/null)"; then
      if [[ "${existing_commit}" == "${commit}" ]]; then
        echo "ok: ${repo} already present at ${commit}"
        return 0
      fi
      if [[ "${FORCE}" -ne 1 ]]; then
        echo "error: ${dest_dir} exists but pinned to ${existing_commit} (wanted ${commit}). Re-run with --force to overwrite." >&2
        return 1
      fi
    else
      if [[ "${FORCE}" -ne 1 ]]; then
        echo "error: ${dest_dir} exists but has no pin marker. Re-run with --force to overwrite." >&2
        return 1
      fi
    fi
  fi

  mkdir -p "${DEPS_DIR}"

  local tmp_dir tmp_tar extract_dir extracted_root
  tmp_dir="$(mktemp -d)"
  tmp_tar="${tmp_dir}/${repo}-${commit}.tar.gz"
  extract_dir="${tmp_dir}/extract"
  mkdir -p "${extract_dir}"

  # Use GitHub's codeload endpoint; avoids redirects to HTML and is stable for tarballs.
  local tar_url="https://codeload.github.com/${owner}/${repo}/tar.gz/${commit}"
  echo "downloading: ${repo_url} @ ${commit}"
  download_to "${tar_url}" "${tmp_tar}"

  tar -xzf "${tmp_tar}" -C "${extract_dir}"

  # The tarball extracts as a single top-level directory (repo-<sha>).
  extracted_root="$(find "${extract_dir}" -mindepth 1 -maxdepth 1 -type d | head -n 1 || true)"
  if [[ -z "${extracted_root}" ]]; then
    echo "error: failed to extract ${repo} (${commit})" >&2
    rm -rf "${tmp_dir}"
    return 1
  fi

  if [[ -d "${dest_dir}" ]]; then
    rm -rf "${dest_dir}"
  fi

  mv "${extracted_root}" "${dest_dir}"
  write_pin_marker "${dest_dir}" "${repo_url}" "${commit}"

  rm -rf "${tmp_dir}"
  echo "ok: installed ${repo} -> ${dest_dir}"
}

fetch_all() {
  fetch_github_repo_at_commit "david-cermak" "esp-http3"     "86a415a24313b8f24d1237e46909e921ba183684"
  fetch_github_repo_at_commit "private-octopus" "picoquic"   "ba0bcac1c08c00f69eabb999aad4cccfc9e247b6"
  fetch_github_repo_at_commit "h2o" "picotls"                "f350eab60742138ac62b42ee444adf04c7898b0d"
  fetch_github_repo_at_commit "david-cermak" "esp-protocols" "e9c2d32b8e4a003f0a1b55286459a4c28e629968"
}

fetch_all