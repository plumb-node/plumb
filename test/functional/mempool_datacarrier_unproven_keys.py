#!/usr/bin/env python3
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test Plumb's -rejectunprovenkeys.

Each input's script leaves unproven the multisig keys no signature in the spend proves. Past 7 a script
they add up over the transaction's inputs, and past 16 in all each one counts 33 data carrier bytes.

Node 0 runs the rule with -datacarriersize=42, Plumb's default, node 1 the same with -datacarriersize=32,
node 2 with the rule off, node 3 -corepolicy. The chain is past the BLAKE2b fork, as on mainnet. Every key
is a real key, and one signature signs each 1-of-N input.
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
)
from test_framework.script import (
    CScript,
    OP_0,
    OP_CHECKMULTISIG,
    OP_CHECKSIG,
    OP_CHECKSIGADD,
    OP_NUMEQUAL,
    OP_TRUE,
    SIGHASH_ALL,
    LegacySignatureHash,
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
FLOOR = 7        # UNPROVEN_KEYS_PER_SCRIPT
ALLOWANCE = 16   # MAX_UNPROVEN_KEYS_PER_TX
KEY_BYTES = 33
LIMITS = [42, 32]  # -datacarriersize on nodes 0 and 1
FORK_HEIGHT = 100
NODE_ARGS = ["-corepolicy=0", f"-testactivationheight=blake2b@{FORK_HEIGHT}", "-rdtsexpiry=2000000000", "-acceptnonstdtxn=0"]
WARMUP_BLOCKS = 310


class UnprovenKeysTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.extra_args = [
            NODE_ARGS + [f"-datacarriersize={LIMITS[0]}"],
            NODE_ARGS + [f"-datacarriersize={LIMITS[1]}"],
            NODE_ARGS + [f"-datacarriersize={LIMITS[0]}", "-rejectunprovenkeys=0"],
            [f"-testactivationheight=blake2b@{FORK_HEIGHT}", "-rdtsexpiry=2000000000"],  # the framework's -corepolicy
        ]

    def newkey(self):
        k = ECKey()
        k.generate()
        return k

    def spend(self, specs):
        """Fund one output for each spec, mine them, and return an unmined tx spending all of them.

        A spec is a dict with 'spk', and 'stack' (a function of the tx and the input index giving the
        witness) or 'script_sig' (the same, giving the scriptSig), or both.
        """
        outpoints = []
        for s in specs:
            f = self.wallet.send_to(from_node=self.nodes[0], scriptPubKey=s['spk'], amount=FUNDING)
            outpoints.append(COutPoint(int(f['txid'], 16), f['sent_vout']))
        self.generate(self.wallet, 1, sync_fun=self.sync_blocks)
        tx = CTransaction()
        tx.version = 2
        tx.vin = [CTxIn(op) for op in outpoints]
        tx.vout = [CTxOut(FUNDING * len(specs) - 10_000 * len(specs), script_to_p2wsh_script(CScript([OP_TRUE])))]
        tx.wit.vtxinwit = [CTxInWitness() for _ in specs]
        # A taproot signature commits to every input's prevout
        self.prevouts = [CTxOut(FUNDING, s['spk']) for s in specs]
        for i, s in enumerate(specs):
            if 'script_sig' in s:
                tx.vin[i].scriptSig = s['script_sig'](tx, i)
            if 'stack' in s:
                tx.wit.vtxinwit[i].scriptWitness.stack = s['stack'](tx, i)
        return tx

    def multisig(self, k, n, kind='p2wsh'):
        """A k-of-n multisig input of fresh keys, kind 'p2wsh', 'p2sh-p2wsh' or 'p2sh', signed by the last k."""
        keys = [self.newkey() for _ in range(n)]
        script = CScript([k] + [key.get_pubkey().get_bytes() for key in keys] + [n, OP_CHECKMULTISIG])
        signers = keys[n - k:]
        if kind == 'p2sh':
            def script_sig(tx, i):
                h = LegacySignatureHash(script, tx, i, SIGHASH_ALL)[0]
                return CScript([OP_0] + [key.sign_ecdsa(h) + bytes([SIGHASH_ALL]) for key in signers] + [bytes(script)])
            return {'spk': script_to_p2sh_script(script), 'script_sig': script_sig}
        program = script_to_p2wsh_script(script)

        def stack(tx, i):
            h = SegwitV0SignatureHash(script, tx, i, SIGHASH_ALL, FUNDING)
            return [b''] + [key.sign_ecdsa(h) + bytes([SIGHASH_ALL]) for key in signers] + [bytes(script)]
        spec = {'spk': program, 'stack': stack}
        if kind == 'p2sh-p2wsh':
            spec['spk'] = script_to_p2sh_script(program)
            spec['script_sig'] = lambda tx, i: CScript([bytes(program)])
        return spec

    def multi_a(self, k, n):
        """A tapscript k-of-n leaf of fresh keys, signed by the first k."""
        privs = [generate_privkey() for _ in range(n)]
        xs = [compute_xonly_pubkey(p)[0] for p in privs]
        ops = [xs[0], OP_CHECKSIG]
        for x in xs[1:]:
            ops += [x, OP_CHECKSIGADD]
        leaf = CScript(ops + [k, OP_NUMEQUAL])
        tap = taproot_construct(compute_xonly_pubkey(generate_privkey())[0], [("leaf", leaf)])
        info = tap.leaves["leaf"]
        control = bytes([info.version | tap.negflag]) + tap.internal_pubkey + info.merklebranch
        spk = tap.scriptPubKey

        def stack(tx, i):
            h = TaprootSignatureHash(tx, self.prevouts, 0, i, scriptpath=True, leaf_script=leaf)
            # The first OP_CHECKSIG takes the top of the stack, so the signatures go in reverse key order
            return [sign_schnorr(privs[j], h) if j < k else b'' for j in reversed(range(n))] + [bytes(leaf), control]
        return {'spk': spk, 'stack': stack}

    def check(self, specs, keys_past_floor, what):
        """Node 0 and node 1 count the keys past the transaction's allowance, 33 bytes each, against their
        -datacarriersize; nodes 2 (rule off) and 3 (-corepolicy) accept everything."""
        tx = self.spend(specs)
        counted = max(0, keys_past_floor - ALLOWANCE) * KEY_BYTES
        raw = tx.serialize().hex()
        for n, limit in enumerate(LIMITS):
            res = self.nodes[n].testmempoolaccept([raw])[0]
            if counted > limit:
                assert not res['allowed'] and res['reject-reason'] == REJECT, f"{what}: node {n}: {res}"
            else:
                assert res['allowed'], f"{what}: node {n}: {res}"
        for n in (2, 3):
            res = self.nodes[n].testmempoolaccept([raw])[0]
            assert res['allowed'], f"{what}: node {n}: {res}"
        self.log.info(f"  ok  {what}: {keys_past_floor} past the floor, {counted} bytes counted")
        return tx

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0])
        self.generate(self.wallet, WARMUP_BLOCKS)

        self.log.info("Ordinary multisig adds nothing, however many inputs")
        self.check([self.multisig(2, 3) for _ in range(40)], 0, "40 inputs of 2-of-3")
        self.check([self.multisig(11, 15) for _ in range(4)], 0, "4 inputs of 11-of-15")
        self.check([self.multisig(2, 3, 'p2sh') for _ in range(20)], 0, "20 P2SH inputs of 2-of-3")

        self.log.info("1-of-11 leaves 10 keys unproven, 3 past the floor, in every input")
        self.check([self.multisig(1, 11) for _ in range(5)], 15, "5 P2WSH inputs of 1-of-11")
        self.check([self.multisig(1, 11) for _ in range(6)], 18, "6 P2WSH inputs of 1-of-11")
        self.check([self.multisig(1, 11, 'p2sh-p2wsh') for _ in range(6)], 18, "6 P2SH-P2WSH inputs of 1-of-11")
        self.check([self.multisig(1, 11, 'p2sh') for _ in range(6)], 18, "6 P2SH inputs of 1-of-11")
        self.check([self.multi_a(1, 11) for _ in range(6)], 18, "6 tapscript inputs of 1-of-11")

        self.log.info("The transaction's allowance, however the inputs share it")
        self.check([self.multisig(1, 11) for _ in range(5)] + [self.multisig(1, 9)], 16, "5 inputs of 1-of-11 and one of 1-of-9")
        # One more key past the allowance is 33 bytes: under 42, over 32
        self.check([self.multisig(1, 11) for _ in range(5)] + [self.multisig(1, 9) for _ in range(2)], 17, "5 inputs of 1-of-11 and two of 1-of-9")
        self.check([self.multisig(1, 9) for _ in range(17)], 17, "17 inputs of 1-of-9")


if __name__ == '__main__':
    UnprovenKeysTest(__file__).main()
