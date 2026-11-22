#!/usr/bin/env python3
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a signature pushed in a witness script never passes for a proven key.

The unproven-key count (knots#470's recount, Plumb's -rejectfakemultisig) credits a pushed key only
when a signature verified against those bytes. It takes any 33 or 65 byte push for a key, and a 32
byte push right before a signature check. Plumb also records every signature that verified, for
-rejectunverifiedwitness. A P2WSH script can push an ECDSA signature of one of those sizes and check
it against a key the witness gives, recovered from the signature and the signature hash (BIP143 does
not commit to a key taken from the stack). Such a push is no key and must be counted as Knots counts
it, though Plumb has its signature on record. A key pushed in the same script and checked against a
real signature is still credited.

Each script ends in three checks that take key and signature from the witness, so the static count,
capped at the signatures the witness shows, finds nothing and only the recount can. Every node runs
Plumb's defaults past the BLAKE2b fork, as on mainnet, at -datacarriersize=32 or 33, which pins a
charge: node 0 (32) and node 1 (33) differ in the limit only; node 2 (32) turns
-rejectunverifiedwitness off, which must not change the count; node 3 (32) turns -rejectfakemultisig
off, the rule that counts the pushes, and every witness item here is a key or a signature that
verified, so it accepts every spend.

A tapscript cannot take this path: a BIP340 signature commits to its key and the signature hash to
the leaf, so no signature can be pushed in the leaf it signs. The tapscript case checks that pushed
x-only keys signed for are still credited there.
"""
from random import randbytes, randrange

from test_framework.crypto.secp256k1 import G, GE
from test_framework.key import (
    ECKey,
    compute_xonly_pubkey,
    generate_privkey,
    sign_schnorr,
)
from test_framework.messages import (
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
)
from test_framework.script import (
    CScript,
    OP_CHECKSIG,
    OP_CHECKSIGVERIFY,
    OP_ENDIF,
    OP_IF,
    OP_SWAP,
    OP_TRUE,
    SIGHASH_ALL,
    SegwitV0SignatureHash,
    TaprootSignatureHash,
    taproot_construct,
)
from test_framework.script_util import (
    script_to_p2sh_script,
    script_to_p2wsh_script,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.wallet import MiniWallet

FUNDING = 100_000
REJECT = "txn-datacarrier-exceeded"
FORK_HEIGHT = 100
NODE_ARGS = ["-corepolicy=0", f"-testactivationheight=blake2b@{FORK_HEIGHT}", "-rdtsexpiry=2000000000", "-acceptnonstdtxn=0"]
LIMITS = [32, 33, 32, 32]
ALLOWANCE = 330  # ten compressed keys per script go uncounted
WARMUP_BLOCKS = 310
N = GE.ORDER


def der(r, s, size):
    """A strict DER signature with SIGHASH_ALL, checked to be size bytes long."""
    def integer(v):
        b = v.to_bytes((v.bit_length() + 7) // 8, "big")
        assert not b[0] & 0x80
        return b"\x02" + bytes([len(b)]) + b
    body = integer(r) + integer(s)
    sig = b"\x30" + bytes([len(body)]) + body + bytes([SIGHASH_ALL])
    assert len(sig) == size, len(sig)
    return sig


def unsigned_int(nbytes):
    """A number that DER writes in exactly nbytes: top bit clear, top byte not zero."""
    return int.from_bytes(bytes([randrange(1, 128)]) + randbytes(nbytes - 1), "big")


def charged(unproven_bytes):
    return max(0, unproven_bytes - ALLOWANCE)


class SignaturePushesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.extra_args = [
            NODE_ARGS + [f"-datacarriersize={LIMITS[0]}"],
            NODE_ARGS + [f"-datacarriersize={LIMITS[1]}"],
            NODE_ARGS + [f"-datacarriersize={LIMITS[2]}", "-rejectunverifiedwitness=0"],
            NODE_ARGS + [f"-datacarriersize={LIMITS[3]}", "-rejectfakemultisig=0"],
        ]

    def newkey(self):
        k = ECKey()
        k.generate()
        return k

    def fund(self, spk):
        f = self.wallet.send_to(from_node=self.nodes[0], scriptPubKey=spk, amount=FUNDING)
        self.generate(self.wallet, 1)
        return COutPoint(int(f['txid'], 16), f['sent_vout'])

    def unsigned_spend(self, outpoint, script_sig=b''):
        tx = CTransaction()
        tx.version = 2
        tx.vin = [CTxIn(outpoint, scriptSig=script_sig)]
        tx.vout = [CTxOut(FUNDING - 10_000, script_to_p2wsh_script(CScript([OP_TRUE])))]
        tx.wit.vtxinwit = [CTxInWitness()]
        return tx

    def p2wsh(self, script, stack, wrap=False):
        """Fund script as P2WSH (in P2SH when wrap) and spend it; stack(z) gives the witness below the
        script for the signature hash z of the spend."""
        program = script_to_p2wsh_script(script)
        spk = script_to_p2sh_script(program) if wrap else program
        tx = self.unsigned_spend(self.fund(spk), CScript([bytes(program)]) if wrap else b'')
        z = SegwitV0SignatureHash(script, tx, 0, SIGHASH_ALL, FUNDING)
        tx.wit.vtxinwit[0].scriptWitness.stack = stack(z) + [bytes(script)]
        return tx

    @staticmethod
    def sign(key, z):
        return key.sign_ecdsa(z) + bytes([SIGHASH_ALL])

    def signature_pushes(self, size, n, wrap=False, dead_copies=False, script_keys=0):
        """n ECDSA signatures of size bytes pushed and each checked, with OP_SWAP OP_CHECKSIGVERIFY,
        against a key recovered from it, then three checks from the witness. With dead_copies each
        push is also named right before an OP_CHECKSIG behind a branch the spend skips, where the
        count reads a 32 byte push as a key. script_keys real keys are pushed and checked first."""
        r_bytes = 1 if size <= 33 else 29
        s_bytes = size - 7 - r_bytes
        while True:
            r = unsigned_int(r_bytes)
            if GE.is_valid_x(r):
                break
        R = GE.lift_x(r)
        ss = [unsigned_int(s_bytes) for _ in range(n)]
        pushes = [der(r, s, size) for s in ss]
        keys = [self.newkey() for _ in range(script_keys)]
        tail = [self.newkey() for _ in range(3)]
        ops = []
        for k in keys:
            ops += [k.get_pubkey().get_bytes(), OP_CHECKSIGVERIFY]
        if dead_copies:
            ops += [OP_IF]
            for p in pushes:
                ops += [p, OP_CHECKSIG]
            ops += [OP_ENDIF]
        for p in pushes:
            ops += [p, OP_SWAP, OP_CHECKSIGVERIFY]
        ops += [OP_CHECKSIGVERIFY, OP_CHECKSIGVERIFY, OP_CHECKSIG]
        script = CScript(ops)

        def stack(z):
            zn = int.from_bytes(z, "big") % N
            rinv = pow(r, -1, N)
            recovered = [((rinv * s % N) * R + (-(rinv * zn) % N) * G).to_bytes_compressed() for s in ss]
            items = []
            for k in reversed(tail):  # the last check takes the bottom pair
                items += [self.sign(k, z), k.get_pubkey().get_bytes()]
            items += list(reversed(recovered))  # the first push's key on top of them
            if dead_copies:
                items.append(b'')  # skip the branch
            items += [self.sign(k, z) for k in reversed(keys)]
            return items
        return self.p2wsh(script, stack, wrap)

    def tapscript_keys(self, n):
        """n x-only keys pushed in a tapscript leaf, each checked against a real signature."""
        privs = [generate_privkey() for _ in range(n)]
        ops = []
        for i, priv in enumerate(privs):
            ops += [compute_xonly_pubkey(priv)[0], OP_CHECKSIG if i == n - 1 else OP_CHECKSIGVERIFY]
        leaf = CScript(ops)
        tap = taproot_construct(compute_xonly_pubkey(generate_privkey())[0], [("leaf", leaf)])
        info = tap.leaves["leaf"]
        control = bytes([info.version | tap.negflag]) + tap.internal_pubkey + info.merklebranch
        tx = self.unsigned_spend(self.fund(tap.scriptPubKey))
        h = TaprootSignatureHash(tx, [CTxOut(FUNDING, tap.scriptPubKey)], 0, 0, scriptpath=True, leaf_script=leaf)
        tx.wit.vtxinwit[0].scriptWitness.stack = [sign_schnorr(p, h) for p in reversed(privs)] + [bytes(leaf), control]
        return tx

    def check(self, tx, charge, what):
        """Nodes 0 to 2 accept the spend exactly when their limit covers the charge; node 3 always does."""
        raw = tx.serialize().hex()
        for n, limit in enumerate(LIMITS):
            res = self.nodes[n].testmempoolaccept([raw])[0]
            if n < 3 and charge > limit:
                assert not res['allowed'] and res['reject-reason'] == REJECT, f"{what}: node {n}: {res}"
            else:
                assert res['allowed'], f"{what}: node {n}: {res}"
        self.log.info(f"  ok  {what}: {charge} bytes counted")

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0])
        self.generate(self.wallet, WARMUP_BLOCKS)

        self.log.info("Keys pushed and checked against real signatures are credited")
        key = self.newkey()
        self.check(self.p2wsh(CScript([key.get_pubkey().get_bytes(), OP_CHECKSIG]), lambda z: [self.sign(key, z)]), 0, "one key, one signature")
        keys = [self.newkey() for _ in range(13)]
        real = CScript(sum([[k.get_pubkey().get_bytes(), OP_CHECKSIGVERIFY] for k in keys[:-1]], []) + [keys[-1].get_pubkey().get_bytes(), OP_CHECKSIG])
        self.check(self.p2wsh(real, lambda z: [self.sign(k, z) for k in reversed(keys)]), 0, "13 keys, 13 signatures (unproven: 99)")
        self.check(self.tapscript_keys(13), 0, "tapscript: 13 x-only keys, 13 signatures (unproven: 86)")

        self.log.info("Signatures pushed and checked against recovered keys count as unproven keys")
        self.check(self.signature_pushes(33, 11), charged(11 * 33), "P2WSH: 11 signatures of 33 bytes")
        self.check(self.signature_pushes(33, 11, wrap=True), charged(11 * 33), "P2SH-P2WSH: 11 signatures of 33 bytes")
        self.check(self.signature_pushes(33, 13), charged(13 * 33), "P2WSH: 13 signatures of 33 bytes")
        self.check(self.signature_pushes(65, 6), charged(6 * 65), "P2WSH: 6 signatures of 65 bytes")
        self.check(self.signature_pushes(32, 12, dead_copies=True), charged(12 * 32), "P2WSH: 12 signatures of 32 bytes, named before OP_CHECKSIG in a skipped branch")
        self.check(self.signature_pushes(33, 10), 0, "P2WSH: 10 signatures of 33 bytes, within the allowance")

        self.log.info("Beside them, keys checked against real signatures are still credited")
        self.check(self.signature_pushes(33, 11, script_keys=3), charged(11 * 33), "P2WSH: 3 signed keys and 11 signatures of 33 bytes (keys uncredited: 132)")


if __name__ == '__main__':
    SignaturePushesTest(__file__).main()
