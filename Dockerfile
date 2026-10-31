# Plumb is Bitcoin Knots with extra spam filters in its relay policy:
# https://github.com/plumb-node/plumb. This stage takes bitcoind and
# bitcoin-cli from the signed Plumb release tarball for x86_64.
#
# Trust: the keyring must hold exactly the one pinned Plumb release key,
# SHA256SUMS must match PLUMB_SHA256SUMS, pinned in the manifest, and carry a
# good signature from that key, and the tarball must match SHA256SUMS.
# Pinned by digest so a later pack builds on the image the package was tested on.
FROM debian:stable-slim@sha256:a717a33469731894e77e912ea7d714e27c2dc38ee6554fd1a3102e92999cfc66 AS builder

ARG PLUMB_VERSION
ARG PLUMB_SHA256SUMS
ARG TARGETPLATFORM

ENV PLUMB_SIGNER=89F0E41D72CE523F4AA1CDB692CDFFB7C40CD1BA

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg \
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
    TARBALL="bitcoin-${PLUMB_VERSION}-x86_64-linux-gnu.tar.gz"; \
    BASE="https://github.com/plumb-node/plumb/releases/download/v${PLUMB_VERSION}"; \
    curl -fsSL --fail-early -o SHA256SUMS "${BASE}/SHA256SUMS" -o SHA256SUMS.asc "${BASE}/SHA256SUMS.asc" -o "${TARBALL}" "${BASE}/${TARBALL}"; \
    if ! echo "${PLUMB_SHA256SUMS}  SHA256SUMS" | sha256sum -c --status -; then \
        echo "SHA256SUMS DOES NOT MATCH THE PINNED HASH ${PLUMB_SHA256SUMS}"; exit 1; \
    fi; \
    gpg --status-fd 1 --verify SHA256SUMS.asc SHA256SUMS > /tmp/verify-sums 2>/dev/null || true; \
    if ! grep -q '^\[GNUPG:\] GOODSIG ' /tmp/verify-sums || \
       ! grep -q "^\[GNUPG:\] VALIDSIG .* ${PLUMB_SIGNER}\$" /tmp/verify-sums; then \
        cat /tmp/verify-sums; echo "SHA256SUMS IS NOT SIGNED BY THE PINNED PLUMB KEY"; exit 1; \
    fi; \
    if ! grep " ${TARBALL}\$" SHA256SUMS | sha256sum -c --status -; then \
        echo "${TARBALL} DOES NOT MATCH SHA256SUMS"; exit 1; \
    fi; \
    mkdir -p /build/bin; \
    tar -xzf "${TARBALL}" -C /build/bin --strip-components=2 \
        "bitcoin-${PLUMB_VERSION}/bin/bitcoind" "bitcoin-${PLUMB_VERSION}/bin/bitcoin-cli"; \
    if [ "$(/build/bin/bitcoind -version | head -1)" != "Bitcoin Knots daemon version v${PLUMB_VERSION}" ]; then \
        echo "THE BITCOIND IN ${TARBALL} IS NOT v${PLUMB_VERSION}"; exit 1; \
    fi; \
    echo "Plumb ${PLUMB_VERSION} from ${TARBALL}, SHA256SUMS ${PLUMB_SHA256SUMS} signed by ${PLUMB_SIGNER}"; \
    sha256sum /build/bin/bitcoind /build/bin/bitcoin-cli

# The Debian packages that own the shared libraries the binaries load, for the
# runtime image below. dpkg knows some of them under /usr only.
RUN for lib in $(ldd /build/bin/bitcoind /build/bin/bitcoin-cli | awk '/=> \//{print $3}' | sort -u); do \
        dpkg -S "$lib" 2>/dev/null || dpkg -S "/usr$lib"; \
    done | cut -d: -f1 | sort -u | grep -vx 'libc6\|libgcc-s1\|libstdc++6' > /runtime-packages; \
    cat /runtime-packages

# Final image
FROM debian:stable-slim@sha256:a717a33469731894e77e912ea7d714e27c2dc38ee6554fd1a3102e92999cfc66

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