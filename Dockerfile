# Plumb is Bitcoin Knots with extra spam filters in its relay policy:
# https://github.com/plumb-node/plumb. Plumb publishes signed source tags and
# no binaries, so this stage builds bitcoind and bitcoin-cli from the tag.
#
# Trust: the tag must point at PLUMB_COMMIT, pinned in the manifest, and carry
# a good signature from the one pinned Plumb release key. The keyring holds
# that key and nothing else, so no other key can satisfy the check.
FROM debian:stable-slim AS builder

ARG PLUMB_REPO=https://github.com/plumb-node/plumb.git
ARG PLUMB_TAG
ARG PLUMB_COMMIT
# Parallel compile jobs. Kept low by default: the build machine may be running
# a node of its own.
ARG JOBS=4
ARG TARGETPLATFORM

ENV PLUMB_SIGNER=89F0E41D72CE523F4AA1CDB692CDFFB7C40CD1BA

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkgconf python3 git ca-certificates gnupg \
        libevent-dev libsqlite3-dev libboost-dev libzmq3-dev \
    && rm -rf /var/lib/apt/lists/*

COPY assets/plumb-keys/ /tmp/plumb-keys/

WORKDIR /src
RUN set -e; \
    export LC_ALL=C; \
    case "${TARGETPLATFORM}" in linux/amd64) ;; *) echo "Unsupported platform: ${TARGETPLATFORM}"; exit 1 ;; esac; \
    gpg --quiet --import /tmp/plumb-keys/*.asc; \
    rm -rf /tmp/plumb-keys; \
    if [ "$(gpg --with-colons --list-keys | grep -c '^pub:')" != 1 ] || \
       [ "$(gpg --with-colons --list-keys | awk -F: '/^fpr:/{print $10; exit}')" != "${PLUMB_SIGNER}" ]; then \
        echo "KEYRING IS NOT EXACTLY THE PINNED PLUMB KEY"; exit 1; \
    fi; \
    git init -q; \
    git remote add origin "${PLUMB_REPO}"; \
    git fetch -q --depth 1 origin "refs/tags/${PLUMB_TAG}:refs/tags/${PLUMB_TAG}"; \
    if [ "$(git rev-parse "${PLUMB_TAG}^{commit}")" != "${PLUMB_COMMIT}" ]; then \
        echo "TAG ${PLUMB_TAG} DOES NOT POINT AT ${PLUMB_COMMIT}"; exit 1; \
    fi; \
    git verify-tag --raw "${PLUMB_TAG}" 2> /tmp/verify-tag || true; \
    if ! grep -q '^\[GNUPG:\] GOODSIG ' /tmp/verify-tag || \
       ! grep -q "^\[GNUPG:\] VALIDSIG .* ${PLUMB_SIGNER}\$" /tmp/verify-tag; then \
        cat /tmp/verify-tag; echo "TAG SIGNATURE DOES NOT VERIFY AGAINST THE PINNED KEY"; exit 1; \
    fi; \
    git checkout -q "${PLUMB_COMMIT}"; \
    echo "Building ${PLUMB_TAG} at ${PLUMB_COMMIT}, tag signed by ${PLUMB_SIGNER}"

RUN cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_GUI=OFF \
        -DBUILD_TESTS=OFF \
        -DBUILD_BENCH=OFF \
        -DBUILD_FUZZ_BINARY=OFF \
        -DENABLE_WALLET=ON \
        -DWITH_ZMQ=ON \
 && cmake --build build -j"${JOBS}" --target bitcoind bitcoin-cli \
 && mkdir -p /build/bin \
 && cp build/bin/bitcoind build/bin/bitcoin-cli /build/bin/ \
 && strip /build/bin/bitcoind /build/bin/bitcoin-cli \
 && /build/bin/bitcoind -version | head -1

# The Debian packages that own the shared libraries the binaries load, for the
# runtime image below. dpkg knows some of them under /usr only.
RUN for lib in $(ldd /build/bin/bitcoind /build/bin/bitcoin-cli | awk '/=> \//{print $3}' | sort -u); do \
        dpkg -S "$lib" 2>/dev/null || dpkg -S "/usr$lib"; \
    done | cut -d: -f1 | sort -u | grep -vx 'libc6\|libgcc-s1\|libstdc++6' > /runtime-packages; \
    cat /runtime-packages

# Final image
FROM debian:stable-slim

ENV BITCOIN_DATA=/root/.bitcoin
ENV BITCOIN_PREFIX=/opt/bitcoin
ENV PATH=${BITCOIN_PREFIX}/bin:$PATH

# curl is load-bearing: the assumeutxo action shells out to it in this image
# to download the UTXO snapshot. The rest are the libraries the Plumb binaries
# load, listed by the build stage.
COPY --from=builder /runtime-packages /tmp/runtime-packages
RUN apt-get update && apt-get install -y curl e2fsprogs jq yq $(cat /tmp/runtime-packages) \
    && rm -rf /var/lib/apt/lists/* /tmp/runtime-packages

COPY --from=builder /build/bin/bitcoind ${BITCOIN_PREFIX}/bin/
COPY --from=builder /build/bin/bitcoin-cli ${BITCOIN_PREFIX}/bin/
RUN if ldd ${BITCOIN_PREFIX}/bin/bitcoind ${BITCOIN_PREFIX}/bin/bitcoin-cli | grep "not found"; then \
        echo "A SHARED LIBRARY IS MISSING FROM THE RUNTIME IMAGE"; exit 1; \
    fi

ARG ARCH

EXPOSE 8332 8333