#!/usr/bin/env bash
# Upload the supported IMSA disc image to the private repository that
# .github/workflows/release-build.yml downloads it from at build time.
#
# The image is never added to this repository or to a release package. This
# script only seeds the separate, private game-data repository.
set -euo pipefail

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: tools/seed_game_data_release.sh [--check] /path/to/imsa.iso [owner/repo]

Uploads the supported IMSA disc image to a private GitHub release. The
repository defaults to <your-login>/imsa-wcr-m2-recomp-data and the release
and asset default to "game-data" and "imsa.iso". With --check, verify the
image against cmake/ImsaGameData.cmake and exit without uploading.

Environment overrides:
  GAME_DATA_RELEASE  Release tag to create or update (default: game-data)
  GAME_DATA_ASSET    Asset file name (default: imsa.iso)

Uploading requires an authenticated GitHub CLI (gh auth login) with permission
to create the private repository and upload release assets. --check does not
need gh.
EOF
}

check_only=false
if [ "${1:-}" = "--check" ]; then
    check_only=true
    shift
fi

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    usage
    exit 0
fi

iso="${1:-}"
if [ -z "$iso" ]; then
    usage >&2
    exit 2
fi
[ -f "$iso" ] || die "not a file: $iso"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
release="${GAME_DATA_RELEASE:-game-data}"
asset="${GAME_DATA_ASSET:-imsa.iso}"
case "$asset" in
    ""|*/*) die "GAME_DATA_ASSET must be a file name, got: $asset" ;;
esac

# Keep the checks in step with CMake, which verifies the same values again.
expected_size="$(sed -n 's/^set(IMSA_ISO_SIZE \([0-9][0-9]*\))$/\1/p' "$root/cmake/ImsaGameData.cmake")"
expected_sha="$(sed -n 's/^set(IMSA_ISO_SHA256 "\([0-9a-f][0-9a-f]*\)")$/\1/p' "$root/cmake/ImsaGameData.cmake")"
[ -n "$expected_size" ] || die "could not read IMSA_ISO_SIZE from cmake/ImsaGameData.cmake"
[ -n "$expected_sha" ] || die "could not read IMSA_ISO_SHA256 from cmake/ImsaGameData.cmake"

size="$(wc -c < "$iso" | tr -d '[:space:]')"
if command -v sha256sum >/dev/null 2>&1; then
    sha="$(sha256sum "$iso" | awk '{print $1}')"
else
    sha="$(shasum -a 256 "$iso" | awk '{print $1}')"
fi
[ "$size" = "$expected_size" ] || die "$iso is ${size} bytes; expected ${expected_size}"
[ "$sha" = "$expected_sha" ] || die "$iso has SHA-256 ${sha}; expected ${expected_sha}"

if $check_only; then
    echo "$iso matches the supported disc (${size} bytes, ${sha})"
    exit 0
fi

command -v gh >/dev/null 2>&1 || die "the GitHub CLI (gh) is required"
gh auth status >/dev/null 2>&1 || die "run 'gh auth login' first"

login="$(gh api user --jq .login)"
repo="${2:-$login/imsa-wcr-m2-recomp-data}"
case "$repo" in
    */*/*) die "repository must be owner/name: $repo" ;;
    */*) ;;
    *) die "repository must be owner/name: $repo" ;;
esac

if gh repo view "$repo" >/dev/null 2>&1; then
    visibility="$(gh repo view "$repo" --json visibility --jq .visibility)"
    [ "$visibility" = "PRIVATE" ] || die "$repo is $visibility; refusing to upload the disc image there"
    echo "Using existing private repository $repo"
else
    echo "Creating private repository $repo"
    gh repo create "$repo" --private --add-readme \
        --description "Private IMSA disc image for CI release builds; do not make public" >/dev/null
fi

if gh release view "$release" --repo "$repo" >/dev/null 2>&1; then
    is_draft="$(gh release view "$release" --repo "$repo" --json isDraft --jq .isDraft)"
    [ "$is_draft" = "false" ] || die "release $release in $repo is a draft; publish it first"
    echo "Using existing release $release in $repo"
else
    if [ "$(gh api "repos/$repo/commits?per_page=1" --jq 'length')" = "0" ]; then
        die "$repo has no commits; add an initial commit before uploading a release asset"
    fi
    echo "Creating release $release in $repo"
    gh release create "$release" --repo "$repo" \
        --title "IMSA disc image" \
        --notes "Private copy of the supported IMSA disc image for CI release builds. Do not publish this repository or release." >/dev/null
fi

# gh names the asset after the uploaded path, so stage a differently named
# local image under the name the workflow expects.
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
if [ "$(basename "$iso")" = "$asset" ]; then
    upload="$iso"
else
    upload="$staging/$asset"
    cp "$iso" "$upload"
fi

echo "Uploading $(basename "$iso") as $asset to $repo/$release (this can take a while)"
gh release upload "$release" "$upload" --repo "$repo" --clobber

gh release view "$release" --repo "$repo" --json assets \
    --jq ".assets[] | select(.name == \"$asset\") | \"Uploaded \(.name) (\(.size) bytes)\""

public_repo="$(cd "$root" && gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null || true)"
public_repo="${public_repo:-$login/imsa-wcr-m2-recomp}"

cat <<EOF

Next steps:
1. Create a fine-grained personal access token for only $repo with the
   repository permission "Contents: Read-only":
   https://github.com/settings/personal-access-tokens/new

2. Store it as the release environment secret (the first command creates the
   environment only if it does not exist yet):
   gh api repos/$public_repo/environments/release >/dev/null 2>&1 || \
       gh api --method PUT repos/$public_repo/environments/release
   gh secret set GAME_DATA_TOKEN --env release --repo $public_repo

3. Optionally protect the release environment (Settings > Environments >
   release): add required reviewers and restrict deployments to v* tags
   and main.

4. Run the Release workflow from Actions, or push a tag:
   git tag v<version> && git push origin v<version>
EOF

if [ "$repo" != "$login/imsa-wcr-m2-recomp-data" ]; then
    cat <<EOF

The repository name is not the workflow default. Set:
   gh variable set GAME_DATA_REPO --repo $public_repo --body "$repo"
EOF
fi
if [ "$release" != "game-data" ]; then
    cat <<EOF

The release name is not the workflow default. Set:
   gh variable set GAME_DATA_RELEASE --repo $public_repo --body "$release"
EOF
fi
if [ "$asset" != "imsa.iso" ]; then
    cat <<EOF

The asset name is not the workflow default. Set:
   gh variable set GAME_DATA_ASSET --repo $public_repo --body "$asset"
EOF
fi
