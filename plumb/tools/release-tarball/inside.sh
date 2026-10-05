#!/bin/bash
# Runs inside the build image (see Dockerfile). Builds bitcoind and bitcoin-cli
# for x86_64 Linux from the release tree mounted at /src, the way the Knots
# release builds do: every library from the depends system, libstdc++ linked
# in, glibc (2.31 here) the only thing taken from the system.
#
#   /src   the release tree, extracted from the signed tag (written to: depends and build/)
#   /out   results: the tarball's contents, logs, build-info.txt
#   /deps  depends download and build cache, kept between runs
set -euo pipefail
JOBS=${JOBS:-3}
HOST=x86_64-pc-linux-gnu
: "${SOURCE_DATE_EPOCH:?set to the commit time of the tag}"
export LC_ALL=C TZ=UTC

step() { printf '\n=== %s  (%s)\n' "$1" "$(date -u +%FT%TZ)"; }

step "toolchain"
gcc --version | head -1
ldd --version | head -1
cmake --version | head -1
python3 -c 'import lief; print("lief", lief.__version__)'

step "depends ($HOST, no Qt, no USDT)"
make -C /src/depends HOST="$HOST" NO_QT=1 NO_USDT=1 \
     SOURCES_PATH=/deps/sources BASE_CACHE=/deps/built \
     LDFLAGS="-static-libstdc++ -Wl,-O2 -Wl,--as-needed" \
     -j"$JOBS"

step "configure"
cmake -S /src -B /src/build \
      --toolchain "/src/depends/$HOST/toolchain.cmake" \
      -DBUILD_GUI=OFF -DWITH_CCACHE=OFF -DREDUCE_EXPORTS=ON \
      -DBUILD_BENCH=OFF -DBUILD_FUZZ_BINARY=OFF \
      -DCMAKE_INSTALL_PREFIX=/usr
grep -E '^(CMAKE_BUILD_TYPE|ENABLE_WALLET|WITH_SQLITE|WITH_ZMQ|ENABLE_HARDENING|REDUCE_EXPORTS|BUILD_TESTS|CMAKE_EXE_LINKER_FLAGS_INIT):' /src/build/CMakeCache.txt || true

step "build"
cmake --build /src/build -j"$JOBS"

step "unit tests on this build"
rc=0
/src/build/bin/test_bitcoin > /out/test_bitcoin.log 2>&1 || rc=$?
tail -2 /out/test_bitcoin.log
echo "test_bitcoin exit $rc"
[ "$rc" = 0 ] || { echo "UNIT TESTS FAILED, see /out/test_bitcoin.log"; exit 1; }

step "release checks (contrib/devtools), on the unstripped binaries as the tree's check-security and check-symbols targets do"
cd /src/build/bin
python3 /src/contrib/devtools/symbol-check.py ./bitcoind ./bitcoin-cli || { echo "SYMBOL CHECK FAILED"; exit 1; }
echo "symbol-check ok"
python3 /src/contrib/devtools/security-check.py ./bitcoind ./bitcoin-cli || { echo "SECURITY CHECK FAILED"; exit 1; }
echo "security-check ok"

step "install, stripped"
rm -rf /out/stage
cmake --install /src/build --strip --prefix /out/stage >/dev/null
ls -l /out/stage/bin

step "the stripped binaries"
cd /out/stage/bin
ldd ./bitcoind
./bitcoind -version | head -1
./bitcoin-cli -version | head -1

step "regtest smoke"
D=$(mktemp -d)
./bitcoind -regtest -datadir="$D" -daemonwait -listen=0 -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 >/dev/null
./bitcoin-cli -regtest -datadir="$D" getnetworkinfo | grep -E '"(subversion|version)"'
./bitcoin-cli -regtest -datadir="$D" getmempoolinfo | grep -E '"(maxdatacarriersize|loaded)"' || true
./bitcoin-cli -regtest -datadir="$D" stop >/dev/null
sleep 2
grep -E 'Plumb filter' "$D/regtest/debug.log" | sed 's/^[^ ]* //' | head -8
rm -rf "$D"

step "package"
VER=$(./bitcoind -version | sed -n '1s/.* version v//p')
[ -n "$VER" ]
DIST="bitcoin-$VER"
TARBALL="$DIST-x86_64-linux-gnu.tar.gz"
rm -rf "/out/$DIST" "/out/$TARBALL"
mkdir -p "/out/$DIST/bin"
cp ./bitcoind ./bitcoin-cli "/out/$DIST/bin/"
cd /out
tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner \
    --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
    -cf - "$DIST" | gzip -9n > "$TARBALL"
sha256sum "$TARBALL" "$DIST/bin/bitcoind" "$DIST/bin/bitcoin-cli" | tee /out/sha256.txt

GLIBC_VER=$(ldd --version | head -1 | awk '{print $NF}')
GCC_VER=$(gcc --version | head -1)
CMAKE_VER=$(cmake --version | head -1 | awk '{print $NF}')
GLIBC_NEEDED=$(objdump -T "$DIST/bin/bitcoind" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
{
  echo "built: $(date -u +%FT%TZ)"
  echo "version: v$VER"
  echo "host: $HOST"
  echo "image: gcc:12.3.0-bullseye (Debian 11, glibc $GLIBC_VER, $GCC_VER)"
  echo "cmake: $CMAKE_VER"
  echo "depends: NO_QT=1 NO_USDT=1 LDFLAGS=-static-libstdc++ -Wl,-O2 -Wl,--as-needed"
  echo "configure: -DBUILD_GUI=OFF -DWITH_CCACHE=OFF -DREDUCE_EXPORTS=ON -DBUILD_BENCH=OFF -DBUILD_FUZZ_BINARY=OFF"
  echo "jobs: $JOBS"
  echo "SOURCE_DATE_EPOCH: $SOURCE_DATE_EPOCH"
  echo "glibc symbols needed: $GLIBC_NEEDED"
} > /out/build-info.txt
cat /out/build-info.txt
step "done"
