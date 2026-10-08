#!/bin/bash
# Builds the x86_64 or aarch64 Linux tarball for a Plumb release tag in a Debian 11
# container.
#
#   plumb/tools/release-tarball/build-tarball.sh <tag> [work dir]
#   ARCH=aarch64 plumb/tools/release-tarball/build-tarball.sh <tag> [work dir]
#
# Needs Docker. The source comes from `git archive` of the tag, so the working tree
# does not matter; the tag must carry a good signature from SIGNER below, the Plumb
# tag key that README.md names. Output in <work dir>/out
# (default ~/scratch/plumb-release-<tag>): the tarball, SHA256SUMS (unsigned),
# build-info.txt and the full log. Nothing here signs or uploads anything.
#
# Environment: JOBS (default 2), CPUS (default 4), MEMORY (default 4000m): what the
# container may use. Two jobs fit in 4 GB; raise them together on a bigger machine.
# ARCH (default x86_64) is x86_64 or aarch64. aarch64 on an x86_64 machine runs the
# whole build in an arm64 container under QEMU, which needs QEMU registered with the
# kernel (on Debian or Ubuntu: sudo apt-get install qemu-user-static) and takes
# much longer. Its work dir defaults to ~/scratch/plumb-release-<tag>-aarch64.
set -euo pipefail
TAG=${1:?tag, for example v29.4.2.knots20260508.plumb5}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(git -C "$HERE" rev-parse --show-toplevel)
SIGNER=89F0E41D72CE523F4AA1CDB692CDFFB7C40CD1BA
JOBS=${JOBS:-2} CPUS=${CPUS:-4} MEMORY=${MEMORY:-4000m} ARCH=${ARCH:-x86_64}
# PLATFORM: the container's; HOST: what the depends system calls this machine, so the
# build is native inside the container; TRIPLET: the tarball's name, as Knots names it.
case "$ARCH" in
  x86_64) PLATFORM=linux/amd64 HOST=x86_64-pc-linux-gnu TRIPLET=x86_64-linux-gnu
          W=${2:-$HOME/scratch/plumb-release-$TAG} IMAGE=plumb-release-builder:bullseye-gcc12 ;;
  aarch64) PLATFORM=linux/arm64 HOST=aarch64-unknown-linux-gnu TRIPLET=aarch64-linux-gnu
          W=${2:-$HOME/scratch/plumb-release-$TAG-aarch64} IMAGE=plumb-release-builder:bullseye-gcc12-aarch64 ;;
  *) echo "ARCH must be x86_64 or aarch64"; exit 1 ;;
esac
if [ "$(uname -m)" != "$ARCH" ] && [ ! -e "/proc/sys/fs/binfmt_misc/qemu-$ARCH" ]; then
  echo "building for $ARCH on $(uname -m) needs QEMU registered (sudo apt-get install qemu-user-static), stopping"; exit 1
fi

# gpg's status lines (--raw) are the fixed format meant for programs, so the check does
# not depend on the language gpg prints in. Field 12 of VALIDSIG is the primary key. Keep
# the exit status check: gpg prints VALIDSIG for expired and revoked keys too.
if ! raw=$(git -C "$REPO" verify-tag --raw "$TAG" 2>&1) \
   || ! awk -v k="$SIGNER" '$1 == "[GNUPG:]" && $2 == "VALIDSIG" && $12 == k "" { ok = 1 } END { exit !ok }' <<<"$raw"; then
  printf '%s\n' "$raw"
  echo "tag is not signed by the Plumb key $SIGNER, stopping"; exit 1
fi
# A genuine older tag stored under a newer name still has a good signature. The signed tag
# object records the name it was made with, so compare that.
obj=$(git -C "$REPO" cat-file tag "$TAG")
[ "$(awk '/^$/ { exit } $1 == "tag" { print $2 }' <<<"$obj")" = "$TAG" ] \
  || { echo "the signed tag object is not named $TAG, stopping"; exit 1; }
tree=$(git -C "$REPO" rev-parse "$TAG^{tree}")
export SOURCE_DATE_EPOCH=$(git -C "$REPO" log -1 --format=%ct "$TAG^{commit}")

echo "== source tree from the tag ($tree)"
rm -rf "$W/src"
mkdir -p "$W/src" "$W/out" "$W/deps"
git -C "$REPO" archive --format=tar "$TAG" | tar -x -C "$W/src"

echo "== build image"
docker build -q --platform "$PLATFORM" -t "$IMAGE" "$HERE" >/dev/null
docker image inspect "$IMAGE" --format 'image {{.Id}}'

echo "== build $ARCH (jobs=$JOBS, cpus=$CPUS, memory=$MEMORY)"
docker run --rm --name plumb-release-build --platform "$PLATFORM" \
  --cpus="$CPUS" --memory="$MEMORY" --memory-swap="$MEMORY" \
  --user "$(id -u):$(id -g)" -e HOME=/tmp -e JOBS="$JOBS" -e SOURCE_DATE_EPOCH -e HOST="$HOST" -e TRIPLET="$TRIPLET" \
  -v "$W/src:/src" -v "$W/out:/out" -v "$W/deps:/deps" -v "$HERE/inside.sh:/inside.sh:ro" \
  "$IMAGE" bash /inside.sh 2>&1 | tee "$W/out/build.log" \
  || { echo "BUILD FAILED, see $W/out/build.log"; exit 1; }

cd "$W/out"
tarball=$(ls bitcoin-*-"$TRIPLET".tar.gz)
sha256sum "$tarball" > SHA256SUMS
echo "== done"
cat SHA256SUMS
echo "tag commit $(git -C "$REPO" rev-parse "$TAG^{commit}"), tree $tree, built $(date -u +%FT%TZ)"
echo "next: gpg --detach-sign --armor SHA256SUMS, then upload $tarball, SHA256SUMS and SHA256SUMS.asc to the release"
echo "(with both tarballs on one release, sign one SHA256SUMS that has both lines: see README.md)"
