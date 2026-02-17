#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
VERSION_FILE="${REPO_ROOT}/VERSION"

NO_COMMIT=0
NO_TAG=0
ALLOW_DIRTY=0
SKIP_BUILD_CHECK=0

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/release.sh patch [--no-commit] [--no-tag] [--allow-dirty] [--skip-build-check]
  ./scripts/release.sh minor [--no-commit] [--no-tag] [--allow-dirty] [--skip-build-check]
  ./scripts/release.sh major [--no-commit] [--no-tag] [--allow-dirty] [--skip-build-check]
  ./scripts/release.sh set <x.y.z> [--no-commit] [--no-tag] [--allow-dirty] [--skip-build-check]

Behavior:
  - Updates VERSION (single source of truth)
  - Runs build validation before writing VERSION (can be skipped)
  - Optionally creates git commit and annotated tag vX.Y.Z
USAGE
}

if [[ $# -lt 1 ]]; then
    usage
    exit 1
fi

ACTION="$1"
shift || true

TARGET_VERSION=""
if [[ "$ACTION" == "set" ]]; then
    if [[ $# -lt 1 ]]; then
        echo "Error: set requires an explicit version." >&2
        usage
        exit 1
    fi
    TARGET_VERSION="$1"
    shift || true
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-commit)
            NO_COMMIT=1
            ;;
        --no-tag)
            NO_TAG=1
            ;;
        --allow-dirty)
            ALLOW_DIRTY=1
            ;;
        --skip-build-check)
            SKIP_BUILD_CHECK=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown argument '$1'" >&2
            usage
            exit 1
            ;;
    esac
    shift || true
done

if [[ ! -f "$VERSION_FILE" ]]; then
    echo "Error: VERSION file not found at ${VERSION_FILE}" >&2
    exit 1
fi

current_version="$(tr -d '[:space:]' < "$VERSION_FILE")"
if [[ ! "$current_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: VERSION must follow semantic format x.y.z (current: '$current_version')." >&2
    exit 1
fi

IFS='.' read -r major minor patch <<< "$current_version"

case "$ACTION" in
    patch)
        patch=$((patch + 1))
        ;;
    minor)
        minor=$((minor + 1))
        patch=0
        ;;
    major)
        major=$((major + 1))
        minor=0
        patch=0
        ;;
    set)
        if [[ ! "$TARGET_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
            echo "Error: explicit version must follow x.y.z (received: '$TARGET_VERSION')." >&2
            exit 1
        fi
        IFS='.' read -r major minor patch <<< "$TARGET_VERSION"
        ;;
    *)
        echo "Error: action must be one of patch|minor|major|set" >&2
        usage
        exit 1
        ;;
esac

next_version="${major}.${minor}.${patch}"

if [[ "$ALLOW_DIRTY" != "1" ]]; then
    if [[ -n "$(git -C "$REPO_ROOT" status --porcelain)" ]]; then
        echo "Error: git working tree is dirty. Commit/stash changes or use --allow-dirty." >&2
        exit 1
    fi
fi

if [[ "$NO_COMMIT" == "1" && "$NO_TAG" != "1" ]]; then
    echo "Error: cannot create tag without commit. Use --no-tag or allow commit." >&2
    exit 1
fi

if [[ "$SKIP_BUILD_CHECK" != "1" ]]; then
    build_script="${REPO_ROOT}/scripts/build_flash_rp2350.sh"
    if [[ ! -x "$build_script" ]]; then
        echo "Error: build script not found or not executable: ${build_script}" >&2
        exit 1
    fi

    echo "Running build validation for version ${next_version}"
    SKIP_FLASH=1 FIRMWARE_VERSION_OVERRIDE="${next_version}" "$build_script"
fi

echo "$next_version" > "$VERSION_FILE"

echo "Version updated: ${current_version} -> ${next_version}"

if [[ "$NO_COMMIT" != "1" ]]; then
    git -C "$REPO_ROOT" add VERSION
    git -C "$REPO_ROOT" commit -m "chore(release): v${next_version}"
    echo "Commit created: chore(release): v${next_version}"
fi

if [[ "$NO_TAG" != "1" ]]; then
    git -C "$REPO_ROOT" tag -a "v${next_version}" -m "Release v${next_version}"
    echo "Tag created: v${next_version}"
fi

echo "Done."
if [[ "$NO_TAG" == "1" ]]; then
    echo "Tip: create tag manually with: git tag -a v${next_version} -m 'Release v${next_version}'"
fi

echo "Next: push with 'git push --follow-tags'"
