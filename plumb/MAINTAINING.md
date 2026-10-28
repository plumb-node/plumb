Maintaining Plumb
=================

Branch layout
-------------

`29.x-plumb` is the release branch. Its history is always:

1. a Bitcoin Knots release tag (today `v29.4.3.knots20260508`),
2. one merge commit per filter, `Merge knots#N: <title>`, merging the exact
   pull request commit listed in `plumb/filters.json`; where Knots has
   merged the rule and Plumb keeps a switch for it, the branch holds only
   the Plumb commit that adds the switch; where Plumb keeps a switch over
   part of a pull request (`-rejecttokenmessages` over knots#489), the
   branch holds the pull request commit and the switch on top; then one
   merge per other Knots pull request Plumb carries (see below),
3. the Plumb commits on top: version suffix, user agent, startup banner,
   README, assets, and the revert of Knots commit 6323b42e07 that starts
   `-datacarriersize` at 42 instead of 83.

Releases are signed tags on that branch:
`v<knots version>.plumb<N>`, with `N` counting up by one with every release,
across Knots bases (plumb8 was the first on 29.4.3).
The same `N` goes in `CLIENT_VERSION_SUFFIX` in `CMakeLists.txt`, which is
what the version string and the `/Plumb:N/` user agent field come from.

Adding a filter
---------------

The bar is in the README. A filter comes in as a pull request here, as a
Knots pull request, or both. A filter from our own research is opened as a
pull request here like any other and gets the same review and ACK before it
merges. Its `source` in `plumb/filters.json` is `plumb#N`, its `url` is that
pull request, and `related` can say which Knots issue or pull request asked
for it.

1. Review it: read the code, run the suites, measure it against the chain
   since the fork, and run it on a mainnet node. Post the ACK on the pull
   request (and on the Knots one if there is one).
2. Push the ACKed commit to its own branch:
   `git push origin <commit>:refs/heads/filter/<option without the dash>`.
3. Check it merges cleanly onto `29.x-plumb` and against every filter already
   in, then merge the branch with
   `git merge -S --no-ff filter/<name> -m "Merge knots#N: <title>"`
   (or `#N` for a pull request here).
4. Add it to `PLUMB_FILTERS` in `src/init.cpp` and to `plumb/filters.json`:
   option, a short name, source, url, upstream status, branch, commit, a
   one-line summary, the release it first ships in, what it rejects, what it
   leaves alone, and an example transaction from the chain.
5. Run `plumb/tools/gen-assets.py`. It fails if `src/init.cpp` and
   `plumb/filters.json` disagree, and it rewrites `plumb/FILTERS.md`, the
   filter list in the README and the badge. Never edit those by hand.
6. Build with `-Werror`, run `ctest --test-dir build` and the full functional
   suite (`build/test/functional/test_runner.py`). Nothing goes out with a
   failure.

When a filter's pull request gets new commits, review the new head before
the next release, move `filter/<name>` to it, re-merge, and update `commit`
in `plumb/filters.json`.

When Knots closes a filter's pull request, set `upstream` to `closed` and
keep shipping it. The `filter/<name>` branch is the copy we maintain from
then on.

When Knots merges a filter, it arrives with the next Knots release. Drop it
from `PLUMB_FILTERS` and `plumb/filters.json` on that rebase, rerun
`plumb/tools/gen-assets.py`, and delete its `filter/` branch.

A pull request that changes the rule of a filter already in (knots#470 for
`-rejectfakemultisig`) is merged right after that filter, goes on that
filter's branch, and is listed under `amended_by` in its entry in
`plumb/filters.json`, with its own source, upstream status, commit and
release. It is not a filter of its own.

When a filter's commits are replaced rather than added to, for example
when it moves to another pull request, rebuild `29.x-plumb` from the Knots
tag as for a new Knots release below, keep the filter stack linear so the
merges never conflict, and replace the branch.

Other Knots pull requests
-------------------------

A Knots pull request that fixes something other than spam can go in ahead
of Knots (knots#487 and knots#488 today). It gets the same review, sits on
top of the filter stack, is merged with
`git merge -S --no-ff <commit> -m "Merge knots#N: <title>"`, and is listed
in the README. It gets no `filter/` branch and no entry in `PLUMB_FILTERS`
or `plumb/filters.json`. A policy change gets its own option in Knots'
style, default on, so a node can go back to what Knots does
(`-permitlnrevokeleaf` for knots#487). Drop it on the rebase onto the Knots
release that ships it.

Moving to a new Knots release
-----------------------------

1. Start a fresh branch at the new Knots tag.
2. Rebase each `filter/` branch onto the new tag if it does not merge
   cleanly (review the rebase like any other change), then re-merge them in
   the same order.
3. Cherry-pick the Plumb commits. Set the suffix to
   `.knots<date>.plumb<N>`, the new base's date and the next release
   number. Check that the new Knots release still defaults
   `-datacarriersize` to 83 through
   `MAX_OP_RETURN_RELAY` in `src/policy/policy.h`, and that `-corepolicy` in
   `src/init.cpp` does not set it; if Knots moved either, redo the 42 change
   by hand instead of cherry-picking the revert.
4. Build and run the full suites as above.
5. Replace `29.x-plumb` with the new branch (`git push --force-with-lease`).
   The old line stays reachable through its tags.

Releasing
---------

1. Bump `plumb<N>` in `CMakeLists.txt` if this is not the first release on
   the base, and commit.
2. `git tag -s v<knots version>.plumb<N> -m "Plumb v<knots version>.plumb<N>"`
3. `git verify-tag` it, push the branch and the tag.
4. Create the GitHub release from the tag. The notes say which Knots release
   is underneath, which filters are in (with commits), and what changed since
   the last Plumb release.
5. Build the x86_64 and aarch64 Linux tarballs with
   `plumb/tools/release-tarball/build-tarball.sh <tag>` and
   `ARCH=aarch64 plumb/tools/release-tarball/build-tarball.sh <tag>` (needs
   Docker, and QEMU for aarch64; see that folder's README), sign one
   `SHA256SUMS` listing both, and upload both tarballs, `SHA256SUMS` and
   `SHA256SUMS.asc` to the release.
6. Point knots-datum-node at the release: in that repository,
   `tools/bump-plumb.sh <knots version>.plumb<N>` checks the release and
   changes the version in its three places. Commit, tag and release it.
7. Once cesspool.lol grades with the new release, copy its
   `/check/plumb.conf` and `/check/knots.conf` over `plumb/conf/` and to
   plumbnode.org.

GitHub settings
---------------

- Social preview: upload `plumb/assets/png/social-preview.png` under the
  repository settings.
- Organization avatar: `plumb/assets/png/avatar-512.png`.
- Private vulnerability reporting: enabled, since SECURITY.md points at it.
- Assets are regenerated with `plumb/tools/gen-assets.py`; see its header for
  the fonts it needs.
