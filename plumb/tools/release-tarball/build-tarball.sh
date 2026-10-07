#!/bin/bash
# Builds the x86_64 Linux tarball for a Plumb release tag in a Debian 11 container.
#
#   plumb/tools/release-tarball/build-tarball.sh <tag> [work dir]
#
# Needs Docker. The source comes from `git archive` of the tag, so the working tree
# does not matter; the tag must carry a good signature from SIGNER below, the Plumb
# tag key that README.md names. Output in <work dir>/out
# (default ~/scratch/plumb-release-<tag>): the tarball, SHA256SUMS (unsigned),
# build-info.txt and the full log. Nothing here signs or uploads anything.
#
# Environment: JOBS (default 2), CPUS (default 4), MEMORY (default 4000m): what the
# container may use. Two jobs fit in 4 GB; raise both together on a bigger machine.
set -euo pipefail
TAG=${1:?tag, for example v29.4.2.knots20260508.plumb5}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(git -C "$HERE" rev-parse --show-toplevel)
W=${2:-$HOME/scratch/plumb-release-$TAG}
IMAGE=plumb-release-builder:bullseye-gcc12
SIGNER=89F0E41D72CE523F4AA1CDB692CDFFB7C40CD1BA
JOBS=${JOBS:-2} CPUS=${CPUS:-4} MEMORY=${MEMORY:-4000m}

# gpg's status lines (--raw) are the fixed format meant for programs, so the check does
# not depend on the language gpg prints in. Field 12 of VALIDSIG is the primary key. Keep
# the exit status check: gpg prints VALIDSIG for expired and revoked keys too.
if ! raw=$(git -C "$REPO" verify-tag --raw "$TAG" 2>&1) \
   || ! awk -v k="$SIGNER" '$1 == "[GNUPG:]" && $2 == "VALIDSIG" && $12 == k "" { ok = 1 } END { exit !ok }' <<<"$raw"; then
  printf '%s\n' "$raw"
  echo "tag is not signed by the Plumb key $SIGNER, stopping"; exit 1
fi
tree=$(git -C "$REPO" rev-parse "$TAG^{tree}")
export SOURCE_DATE_EPOCH=$(git -C "$REPO" log -1 --format=%ct "$TAG^{commit}")

echo "== source tree from the tag ($tree)"
rm -rf "$W/src"
mkdir -p "$W/src" "$W/out" "$W/deps"
git -C "$REPO" archive --format=tar "$TAG" | tar -x -C "$W/src"

echo "== build image"
docker build -q -t "$IMAGE" "$HERE" >/dev/null
docker image inspect "$IMAGE" --format 'image {{.Id}}'

echo "== build (jobs=$JOBS, cpus=$CPUS, memory=$MEMORY)"
docker run --rm --name plumb-release-build \
  --cpus="$CPUS" --memory="$MEMORY" --memory-swap="$MEMORY" \
  --user "$(id -u):$(id -g)" -e HOME=/tmp -e JOBS="$JOBS" -e SOURCE_DATE_EPOCH \
  -v "$W/src:/src" -v "$W/out:/out" -v "$W/deps:/deps" -v "$HERE/inside.sh:/inside.sh:ro" \
  "$IMAGE" bash /inside.sh 2>&1 | tee "$W/out/build.log"
[ "${PIPESTATUS[0]}" = 0 ] || { echo "BUILD FAILED, see $W/out/build.log"; exit 1; }

cd "$W/out"
tarball=$(ls bitcoin-*-x86_64-linux-gnu.tar.gz)
sha256sum "$tarball" > SHA256SUMS
echo "== done"
cat SHA256SUMS
echo "tag commit $(git -C "$REPO" rev-parse "$TAG^{commit}"), tree $tree, built $(date -u +%FT%TZ)"
echo "next: gpg --detach-sign --armor SHA256SUMS, then upload $tarball, SHA256SUMS and SHA256SUMS.asc to the release"
