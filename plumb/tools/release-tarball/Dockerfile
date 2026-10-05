# Build image for Plumb's x86_64 Linux release binaries.
#
# Debian 11 (glibc 2.31) with GCC 12.3: the same glibc floor the Bitcoin Knots
# and Bitcoin Core release binaries have, so the result runs on Debian 11,
# Ubuntu 20.04, RHEL 9 and anything newer. Every other library comes from the
# depends system in the source tree and is linked in statically.
FROM gcc:12.3.0-bullseye@sha256:5e1768ffd3230192358cfbde63443750f8f78f160a449d25007de9828c41976d

# Debian 11 left support in August 2026 and its package mirror no longer serves
# everything, so pip comes from PyPA's bootstrap instead of apt. Debian 11's
# cmake is 3.18 and the tree needs 3.22; lief is for the release symbol and
# security checks in contrib/devtools (0.13.2 is the version the tree's CI pins).
RUN curl -sSfL https://bootstrap.pypa.io/pip/3.9/get-pip.py -o /tmp/get-pip.py \
 && python3 /tmp/get-pip.py --quiet --root-user-action=ignore \
 && rm /tmp/get-pip.py \
 && pip3 install --quiet --no-cache-dir --root-user-action=ignore cmake==3.31.6 lief==0.13.2
