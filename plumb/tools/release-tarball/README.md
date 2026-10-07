Release tarball for x86_64 Linux
================================

How the pre-built `bitcoind` and `bitcoin-cli` on a release page are made.
It needs Docker on an x86_64 Linux machine, which can be a faster PC than
your node: copy the tarball over when it is done.

    plumb/tools/release-tarball/build-tarball.sh <tag> [work dir]

The tag must carry a good signature from the Plumb tag key,
`89F0 E41D 72CE 523F 4AA1  CDB6 92CD FFB7 C40C D1BA`, so import that key first
(the top-level README says where to get it). The work dir defaults to
`~/scratch/plumb-release-<tag>` and the results land in its `out/`. `JOBS`
(default 2), `CPUS` (default 4) and `MEMORY` (default `4000m`) set what the
container may use. Two jobs fit in 4 GB, so raise them together on a bigger
machine:

    JOBS=8 CPUS=8 MEMORY=16g plumb/tools/release-tarball/build-tarball.sh <tag> ~/plumb-build

- `Dockerfile`: Debian 11 with GCC 12.3 (the official `gcc:12.3.0-bullseye`
  image, pinned by digest), plus a current CMake and `lief` from PyPI. Debian
  11 gives glibc 2.31, the same floor the Bitcoin Knots and Bitcoin Core
  release binaries have, so the result runs on Debian 11, Ubuntu 20.04,
  RHEL 9 and everything later.
- `inside.sh`: runs in the container. Builds every library with the tree's
  depends system (`NO_QT=1 NO_USDT=1`, libstdc++ linked in), configures with
  `-DBUILD_GUI=OFF -DREDUCE_EXPORTS=ON`, builds, runs `test_bitcoin`, runs
  `contrib/devtools/symbol-check.py` and `security-check.py` on the build
  binaries before they are stripped (the security check looks up `main`,
  which stripping removes), starts a regtest node and stops it, then packs
  `bitcoin-<version>/bin/{bitcoind,bitcoin-cli}` into
  `bitcoin-<version>-x86_64-linux-gnu.tar.gz` with fixed owners and the tag's
  commit time as every mtime.
- `build-tarball.sh`: verifies the tag's signature, extracts it with
  `git archive`, builds the image, runs `inside.sh`, writes `SHA256SUMS`.

Signing and uploading are by hand: `gpg --detach-sign --armor SHA256SUMS`,
then the tarball, `SHA256SUMS` and `SHA256SUMS.asc` go on the release.
`build-info.txt` next to them records the toolchain, flags and the highest
glibc symbol version the binary needs.

This is one person's build on one machine, not a reproducible build. Anyone
can rerun it. Two runs on the same machine gave the same bytes; nothing
guarantees that on another one.
