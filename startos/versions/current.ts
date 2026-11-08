import { IMPOSSIBLE, VersionInfo } from '@start9labs/start-sdk'
import { rm } from 'fs/promises'
import { bitcoinConfFile } from '../fileModels/bitcoin.conf'

export const current = VersionInfo.of({
  version: '#plumb:29.5.0.10:0',
  releaseNotes: {
    en_US: `Plumb v29.5.knots20260508.plumb10, from the signed release tarball, on the wrapper Leo Haf ships for Bitcoin Knots 29.5, which needs StartOS 0.4.1 or later. Mempool Settings gains Reject Dead Branches and Reject BPUB; each stays on unless set to Off. Switching back to Bitcoin Knots now takes his package for Knots 29.4.3 or later.`,
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
      // Switching back hands the data to Knots 29.4.3:1, the first package
      // whose Knots holds a coinbase mined from block 979919 until RDTS
      // expires for 25,920 blocks, as Plumb does. Knots 29.4.2 releases it
      // after 100, so it would accept blocks Plumb rejects. Later Knots
      // packages run their own migrations from 29.4.3:1.
      ['=#knots:29.4.3:1']: {
        down: async ({ effects }) => {},
      },
    },
  },
})
  // Dependents written for the #knots flavor (BLAKE2b Fulcrum, Datum) accept
  // this release as the Knots it is built on: Leo Haf's package for Knots
  // 29.5 is 29.5.0:3. 29.4.3:1 and 29.4.2:3 stay for dependents pinned to them.
  .satisfies('#knots:29.5.0:3')
  .satisfies('#knots:29.4.3:1')
  .satisfies('#knots:29.4.2:3')
