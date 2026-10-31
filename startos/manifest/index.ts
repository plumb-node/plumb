import { setupManifest } from '@start9labs/start-sdk'
import { long, short } from './i18n'

export const manifest = setupManifest({
  id: 'bitcoind',
  title: 'Plumb',
  license: 'MIT',
  donationUrl: null,
  packageRepo: 'https://github.com/plumb-node/plumb/tree/startos',
  upstreamRepo: 'https://github.com/plumb-node/plumb',
  marketingUrl: 'https://github.com/plumb-node/plumb',
  description: { short, long },
  volumes: ['main', 'i2pd'],
  images: {
    bitcoind: {
      source: {
        dockerBuild: {
          buildArgs: {
            PLUMB_VERSION: '29.4.3.knots20260508.plumb9',
            PLUMB_SHA256SUMS:
              'c11c40bd45a773f438c3675695a8cdc3b412c1145b511ab6e68403ac5ae9107e',
          },
        },
      },
      arch: ['x86_64'],
      emulateMissing: false,
    },
    proxy: {
      source: {
        dockerTag: 'ghcr.io/start9labs/btc-rpc-proxy:v0.8.2',
      },
      arch: ['x86_64', 'aarch64', 'riscv64'],
      emulateMissing: false,
    },
    python: {
      source: {
        dockerTag: 'python:3.14.2-alpine',
      },
      arch: ['x86_64', 'aarch64', 'riscv64'],
      emulateMissing: false,
    },
    i2pd: {
      source: {
        dockerTag: 'purplei2p/i2pd:release-2.58.0',
      },
      arch: ['x86_64', 'aarch64'],
      emulateMissing: true,
    },
  },
})
