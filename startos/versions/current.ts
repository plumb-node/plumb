import { IMPOSSIBLE, VersionInfo } from '@start9labs/start-sdk'
import { rm } from 'fs/promises'
import { bitcoinConfFile } from '../fileModels/bitcoin.conf'

export const current = VersionInfo.of({
  version: '#plumb:29.4.3.8:1',
  releaseNotes: {
    en_US: `Plumb v29.4.3.knots20260508.plumb8, built from the signed source tag, on the wrapper Retropex ships for Bitcoin Knots 29.4.3`,
  },
  migrations: {
    up: async ({ effects }) => {},
    down: async ({ effects }) => {},
    other: {
      // `#knotsrdts` (the "Bitcoin Knots plus BIP-110" build) is being
      // retired. Users on it can move here; preserve their RDTS acceptance
      // so the consensusrules critical-task gate doesn't re-fire. No
      // `down` — `#knotsrdts` is being de-listed, so the inverse path
      // can't be selected by a user.
      ['^#knotsrdts:29.3']: {
        up: async ({ effects }) => {},
      },
      // Bitcoin Knots on the BLAKE2b chain (Retropex's #knots flavor). Same
      // chain, same data directory and bitcoin.conf, so nothing to migrate.
      // 29.4.1:1 is the first BLAKE2b build; #knots:29.4 builds before it
      // had the RDTS consent gate and may hold the legacy chain.
      ['>=#knots:29.4.1:1 <#knots:30.0:0']: {
        up: async ({ effects }) => {},
      },
      // Switching back hands the data to the Knots release this one is built
      // on, so the Knots package still runs its own migrations from there.
      ['=#knots:29.4.2:3']: {
        down: async ({ effects }) => {},
      },
    },
  },
})
  // Dependents written for the #knots flavor (BLAKE2b Fulcrum, Datum) accept
  // this release as the Knots it is built on: Retropex's package for Knots
  // 29.4.3 is 29.4.3:1, and 29.4.2:3 stays for a dependent still pinned to it.
  .satisfies('#knots:29.4.3:1')
  .satisfies('#knots:29.4.2:3')
