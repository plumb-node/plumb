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
            PLUMB_VERSION: '29.4.3.knots20260508.plumb8',
            PLUMB_SHA256SUMS:
              '0df3b788650b53658b6fd0570b2302e6887ccae72e3fc01fc0d7ddfa73dc395b',
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
