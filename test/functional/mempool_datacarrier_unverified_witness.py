#!/usr/bin/env python3
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test Plumb's -rejectunverifiedwitness.

After the policy script check, the bytes of witness items that no verified signature check used count
as data carrier bytes, past 42 bytes per input. The script and a tapscript's control block are not
items. Data dropped from the stack, or handed to a signature check behind a branch that never runs,
counts; signatures, the keys they verified against, preimages and selectors fit in the allowance.

Node 0 runs with the rule on and -datacarriersize=42, Plumb's default, node 1 the same with the rule
off, node 2 -corepolicy. The chain is past the BLAKE2b fork, as on mainnet. The dropped items are constant filler.
"""
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
    sha256,
)
from test_framework.script import (
    CScript,
    OP_2,
    OP_2DROP,
    OP_3,
    OP_CHECKMULTISIG,
    OP_CHECKSIG,
    OP_DROP,
    OP_DUP,
    OP_ENDIF,
    OP_EQUAL,
    OP_EQUALVERIFY,
    OP_FROMALTSTACK,
    OP_HASH160,
    OP_NOTIF,
    OP_SHA256,
    OP_SIZE,
    OP_TOALTSTACK,
    OP_TRUE,
    SIGHASH_ALL,
    SegwitV0SignatureHash,
    TaprootSignatureHash,
    hash160,
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
ALLOWANCE = 42
LIMIT = 42  # -datacarriersize on nodes 0 and 1, Plumb's default
FORK_HEIGHT = 100
NODE_ARGS = ["-corepolicy=0", f"-datacarriersize={LIMIT}", f"-testactivationheight=blake2b@{FORK_HEIGHT}", "-rdtsexpiry=2000000000", "-acceptnonstdtxn=0"]
WARMUP_BLOCKS = 310


def filler(total, size=64):
    """Constant items of at most size bytes, adding up to total."""
    items = []
    while total:
        n = min(size, total)
        items.append(b'\x01' * n)
        total -= n
    return items


class UnverifiedWitnessTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            NODE_ARGS,
            NODE_ARGS + ["-rejectunverifiedwitness=0"],
            [f"-testactivationheight=blake2b@{FORK_HEIGHT}", "-rdtsexpiry=2000000000"],  # the framework's -corepolicy
        ]

    def newkey(self):
        k = ECKey()
        k.generate()
        return k

    def fund(self, spk):
        f = self.wallet.send_to(from_node=self.nodes[0], scriptPubKey=spk, amount=FUNDING)
        return COutPoint(int(f['txid'], 16), f['sent_vout'])

    def spend(self, spk, stack, script_sig=b''):
        """Fund spk, mine it, and return an unmined tx spending it with the witness stack(tx) gives."""
        outpoint = self.fund(spk)
        self.generate(self.wallet, 1, sync_fun=self.sync_blocks)
        tx = CTransaction()
        tx.version = 2
        tx.vin = [CTxIn(outpoint, scriptSig=script_sig)]
        tx.vout = [CTxOut(FUNDING - 10_000, script_to_p2wsh_script(CScript([OP_TRUE])))]
        tx.wit.vtxinwit = [CTxInWitness()]
        tx.wit.vtxinwit[0].scriptWitness.stack = stack(tx)
        return tx

    def p2wsh(self, script, items, wrap=False):
        """items(tx, sig) gives the witness below the script; sig is a signature of tx by self.key."""
        program = script_to_p2wsh_script(script)
        spk = script_to_p2sh_script(program) if wrap else program
        script_sig = CScript([bytes(program)]) if wrap else b''

        def stack(tx):
            sig = self.key.sign_ecdsa(SegwitV0SignatureHash(script, tx, 0, SIGHASH_ALL, FUNDING)) + bytes([SIGHASH_ALL])
            return items(tx, sig) + [bytes(script)]
        return self.spend(spk, stack, script_sig)

    def tapscript(self, leaf_ops, items):
        """A tapscript leaf <leaf_ops> <xonly> OP_CHECKSIG; items(sig) gives the witness below the leaf."""
        priv = generate_privkey()
        xonly = compute_xonly_pubkey(priv)[0]
        leaf = CScript(leaf_ops + [xonly, OP_CHECKSIG])
        tap = taproot_construct(compute_xonly_pubkey(generate_privkey())[0], [("leaf", leaf)])
        info = tap.leaves["leaf"]
        control = bytes([info.version | tap.negflag]) + tap.internal_pubkey + info.merklebranch

        def stack(tx):
            h = TaprootSignatureHash(tx, [CTxOut(FUNDING, tap.scriptPubKey)], 0, 0, scriptpath=True, leaf_script=leaf)
            return items(sign_schnorr(priv, h)) + [bytes(leaf), control]
        return self.spend(tap.scriptPubKey, stack)

    def check(self, tx, refused, what):
        """Refused by node 0 for data carrier bytes when refused, accepted otherwise; node 1 accepts
        everything (the rule is off there), and so does node 2 (-corepolicy)."""
        raw = tx.serialize().hex()
        res = self.nodes[0].testmempoolaccept([raw])[0]
        if refused:
            assert not res['allowed'] and res['reject-reason'] == REJECT, f"{what}: {res}"
        else:
            assert res['allowed'], f"{what}: {res}"
        for n in (1, 2):
            res = self.nodes[n].testmempoolaccept([raw])[0]
            assert res['allowed'], f"{what}: node {n}: {res}"
        self.log.info(f"  ok  {what}: {'refused' if refused else 'accepted'}")

    def drops(self, total, what, wrap=False):
        """Items adding up to total bytes, dropped before a single-key check."""
        items = filler(total)
        script = CScript([OP_2DROP] * (len(items) // 2) + [OP_DROP] * (len(items) % 2) + [self.pub, OP_CHECKSIG])
        # The signature at the bottom, the items above it for the script to drop
        tx = self.p2wsh(script, lambda tx, sig: [sig] + items, wrap)
        self.check(tx, total - ALLOWANCE > LIMIT, what)

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0])
        self.generate(self.wallet, WARMUP_BLOCKS)
        self.key = self.newkey()
        self.pub = self.key.get_pubkey().get_bytes()
        edge = ALLOWANCE + LIMIT

        self.log.info("Dropped items count past the allowance")
        self.drops(edge, f"P2WSH dropping {edge} bytes")
        self.drops(edge + 1, f"P2WSH dropping {edge + 1} bytes")
        self.drops(edge + 1, f"P2SH-P2WSH dropping {edge + 1} bytes", wrap=True)

        self.log.info("Signature-sized items behind a signature check that never runs")
        # The padded reveal from blocks 981910 on: the real check first, then 13 checks behind
        # OP_SIZE 64 OP_EQUAL OP_NOTIF, which a 64-byte item on top skips, then 13 items dropped
        guarded = CScript([self.pub, OP_CHECKSIG, OP_TOALTSTACK, OP_SIZE, 64, OP_EQUAL, OP_NOTIF] + [OP_CHECKSIG] * 13 +
                          [OP_ENDIF] + [OP_2DROP] * 6 + [OP_DROP, OP_FROMALTSTACK])
        self.check(self.p2wsh(guarded, lambda tx, sig: filler(13 * 64) + [sig]), True, "13 x 64 bytes behind dead checks")

        self.log.info("A tapscript leaf dropping items, as at block 981837")
        sizes = [61] + [80] * 6 + [14]
        self.check(self.tapscript([OP_DROP] * len(sizes), lambda sig: [sig] + [b'\x01' * n for n in sizes]), True,
                   "tapscript dropping 555 bytes")
        self.check(self.tapscript([OP_DROP], lambda sig: [sig, b'\x01' * 80]), False, "tapscript dropping 80 bytes")

        self.log.info("Ordinary spends fit in the allowance")
        # LND's to_local revocation leaf, <local_delayedpubkey> OP_DROP <revocationkey> OP_CHECKSIG, spent
        # by the justice transaction: only the signature is an item, and the 34 bytes the leaf drops count
        # toward -datacarriersize as before (knots#487)
        delayed = compute_xonly_pubkey(generate_privkey())[0]
        self.check(self.tapscript([delayed, OP_DROP], lambda sig: [sig]), False, "LND revocation leaf justice spend")
        k2, k3 = self.newkey(), self.newkey()
        msig = CScript([OP_2, self.pub, k2.get_pubkey().get_bytes(), k3.get_pubkey().get_bytes(), OP_3, OP_CHECKMULTISIG])

        def two_of_three(tx, sig):
            sig2 = k2.sign_ecdsa(SegwitV0SignatureHash(msig, tx, 0, SIGHASH_ALL, FUNDING)) + bytes([SIGHASH_ALL])
            return [b'', sig, sig2]
        self.check(self.p2wsh(msig, two_of_three), False, "2-of-3 multisig")
        preimage = b'\x07' * 32
        htlc = CScript([OP_SHA256, sha256(preimage), OP_EQUALVERIFY, self.pub, OP_CHECKSIG])
        self.check(self.p2wsh(htlc, lambda tx, sig: [sig, preimage]), False, "hash lock: signature and preimage")
        # A key checked only by its hash beside the one that signs: the 4,882-input shape since the fork
        other = k2.get_pubkey().get_bytes()
        hashed = CScript([OP_HASH160, hash160(other), OP_EQUALVERIFY, OP_DUP, OP_HASH160, hash160(self.pub), OP_EQUALVERIFY, OP_CHECKSIG])
        self.check(self.p2wsh(hashed, lambda tx, sig: [sig, self.pub, other]), False, "signing key and a key checked only by hash")
        # Two keys checked only by hash: 66 bytes, 24 past the allowance, under -datacarriersize alone
        third = self.newkey().get_pubkey().get_bytes()
        two_hashed = CScript([OP_HASH160, hash160(third), OP_EQUALVERIFY, OP_HASH160, hash160(other), OP_EQUALVERIFY,
                              OP_DUP, OP_HASH160, hash160(self.pub), OP_EQUALVERIFY, OP_CHECKSIG])
        self.check(self.p2wsh(two_hashed, lambda tx, sig: [sig, self.pub, other, third]), False, "signing key and two keys checked only by hash")
        # A copy of the verified key elsewhere in the witness is credited like the key itself
        copies = CScript([OP_2DROP, OP_2DROP, self.pub, OP_CHECKSIG])
        self.check(self.p2wsh(copies, lambda tx, sig: [sig, self.pub, self.pub, self.pub, self.pub]), False, "four copies of the verified key")


if __name__ == '__main__':
    UnverifiedWitnessTest(__file__).main()
