#!/usr/bin/env bash
# publish-rootfs-release.sh — upload the SPLIT 64-bit rootfs to a GitHub Release
# so the Pages deploy workflow (.github/workflows/deploy-pages.yml) can pull it.
#
# The rootfs zips are gitignored Docker build artifacts (wine64.zip is ~196 MB,
# built by build-wine64-zip.sh) — too big for git and over Pages' 100 MB/file
# limit. So we build them once locally, split into <100 MB parts
# (split-rootfs.sh), and park the parts + manifests on a Release tagged
# `rootfs-pages`. The CI workflow DOWNLOADS them and lays them onto Pages
# same-origin (never rebuilding the heavy rootfs). Re-run this only when the
# rootfs content changes.
#
# Note the Release here is just durable storage that CI reads — the browser never
# fetches from it at runtime (that would fail COEP require-corp; it fetches the
# same-origin copies served by Pages). So the Release host's missing CORP header
# doesn't matter.
#
# Requires: gh CLI authenticated with write access to the repo (gh auth status).
# Usage: tools/rootfs64/publish-rootfs-release.sh [TAG]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG="${1:-rootfs-pages}"
DIST="$HERE/dist"
PAGES="$DIST/pages"

# Ensure the split assets exist and are current.
echo "Splitting rootfs zips (<100 MB parts) -> $PAGES ..."
"$HERE/split-rootfs.sh" "$DIST" "$PAGES"

# Collect every asset the workflow expects: whole small zips, big-zip parts,
# and every manifest.
shopt -s nullglob
assets=( "$PAGES"/*.zip "$PAGES"/*.zip.part* "$PAGES"/*.manifest.json )
if [ "${#assets[@]}" -eq 0 ]; then
  echo "ERROR: no split assets found in $PAGES — did split-rootfs.sh run?" >&2
  exit 1
fi
echo "Uploading ${#assets[@]} assets to release '$TAG':"
printf '  %s\n' "${assets[@]##*/}"

# Create the release if it doesn't exist (a non-source-code, prerelease tag), then
# upload, clobbering any stale assets of the same name.
if ! gh release view "$TAG" >/dev/null 2>&1; then
  gh release create "$TAG" \
    --title "wine64 rootfs (split for Pages)" \
    --notes "Split <100 MB rootfs parts consumed by .github/workflows/deploy-pages.yml. Built by tools/rootfs64/publish-rootfs-release.sh. Not for direct browser fetch." \
    --prerelease
fi

gh release upload "$TAG" "${assets[@]}" --clobber
echo "Done. Release '$TAG' now holds the split rootfs."
echo "Trigger a deploy with: gh workflow run deploy-pages.yml  (or push to master)"
