<p align="center">
  <img src="icon.png" alt="Plumb Logo" width="21%">
</p>

# Plumb on StartOS

[Plumb](https://github.com/plumb-node/plumb) is Bitcoin Knots plus the spam-policy filters the Plumb maintainers have reviewed, each on by default and each its own option. It follows the same chain as the Knots release it is built on.

- **Upstream repo:** <https://github.com/plumb-node/plumb>
- **Wrapper source:** branch [`startos`](https://github.com/plumb-node/plumb/tree/startos) of the Plumb repo, based on [Retropex/knots-startos](https://github.com/Retropex/knots-startos) (branch `POW`)

This package shares the `bitcoind` package ID with the Bitcoin Knots package for the BLAKE2b chain, so you can switch between the two while keeping the blockchain data and dependent service connections.

The image takes `bitcoind` and `bitcoin-cli` from the x86_64 tarball of the Plumb release named in the manifest. The build stops unless the release's `SHA256SUMS` has the sha256 pinned in the manifest and a good signature from the pinned Plumb release key in `assets/plumb-keys/`, and the tarball matches `SHA256SUMS`.
