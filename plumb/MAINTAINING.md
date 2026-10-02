Maintaining Plumb
=================

Branch layout
-------------

`29.x-plumb` is the release branch. Its history is always:

1. a Bitcoin Knots release tag (today `v29.4.2.knots20260508`),
2. one merge commit per filter, `Merge knots#N: <title>`, merging the exact
   pull request commit listed in `plumb/filters.json`,
3. the Plumb commits on top: version suffix, user agent, startup banner,
   README, assets.

Releases are signed tags on that branch:
`v<knots version>.plumb<N>`, with `N` counting up from 1 on each Knots base.
The same `N` goes in `CLIENT_VERSION_SUFFIX` in `CMakeLists.txt`, which is
what the version string and the `/Plumb:N/` user agent field come from.

Adding a filter
---------------

The bar is in the README. A filter comes in as a pull request here, as a
Knots pull request, or both.

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

Moving to a new Knots release
-----------------------------

1. Start a fresh branch at the new Knots tag.
2. Rebase each `filter/` branch onto the new tag if it does not merge
   cleanly (review the rebase like any other change), then re-merge them in
   the same order.
3. Cherry-pick the Plumb commits. Reset the suffix to
   `.knots<date>.plumb1` to match the new base.
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

GitHub settings
---------------

- Social preview: upload `plumb/assets/png/social-preview.png` under the
  repository settings.
- Organization avatar: `plumb/assets/png/avatar-512.png`.
- Private vulnerability reporting: enabled, since SECURITY.md points at it.
- Assets are regenerated with `plumb/tools/gen-assets.py`; see its header for
  the fonts it needs.
