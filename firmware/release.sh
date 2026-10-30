#!/bin/bash
# Tag the version in main/version.h and push the tag. The release workflow
# (.github/workflows/release.yml) builds both variants and publishes them.
set -e
cd "$(dirname "$0")"

VERSION=$(sed -nE 's/^#define FW_VERSION "([^"]+)".*/\1/p' main/version.h)
[ -n "$VERSION" ] || { echo "No FW_VERSION in main/version.h"; exit 1; }

# The tag must point at a commit whose source is what gets built.
if ! git diff --quiet HEAD -- || [ -n "$(git ls-files --others --exclude-standard)" ]; then
    echo "Working tree is dirty; commit the version bump first."
    exit 1
fi
if [ "$(ls receiver_fw/gb_receiver_v*.bin 2>/dev/null | wc -l)" -ne 1 ]; then
    echo "Need exactly one receiver_fw/gb_receiver_v*.bin (gb_receiver's release bundles it)."
    exit 1
fi
if ! git diff --quiet "@{upstream}"..HEAD; then
    echo "Push your commits first: the tag must reference a pushed commit."
    exit 1
fi
if git rev-parse -q --verify "refs/tags/v$VERSION" >/dev/null; then
    echo "Tag v$VERSION already exists."
    exit 1
fi

git tag -a "v$VERSION" -m "Release v$VERSION"
git push origin "v$VERSION"
echo "Pushed v$VERSION; the release workflow builds and publishes it."
