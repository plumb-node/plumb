#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Knots developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test Plumb's -rejecttokenmessages switch over the JSON and Omni part of -rejecttokens."""

from test_framework.messages import CTxOut
from test_framework.script import CScript, OP_13, OP_FALSE, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import TestNode
from test_framework.util import assert_raises_rpc_error
from test_framework.wallet import MiniWallet

# Payloads as written on mainnet: a JSON mint (715863e04b618a6818eaec826b8ea900a75a43a5c22859c6b408c68246c4ebcb)
# and an Omni Layer simple send (7134e618c5970bc3d7346cb4dce83aaf081213c122601caa8e64ff5e32296f38).
JSON_MINT = CScript([OP_RETURN, b'{"p":"ico-20","op":"mint","tick":"LEAF"}'])
OMNI_SEND = CScript([OP_RETURN, bytes.fromhex("6f6d6e690000000000000003000000000000009d")])
RUNES = CScript([OP_RETURN, OP_13, OP_FALSE])


class RejectTokenMessagesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 5
        # The framework starts every node with -corepolicy, which turns both options off; nodes 0,
        # 1 and 4 set them explicitly, node 2 takes the defaults without -corepolicy, node 3 keeps it.
        self.extra_args = [
            ["-rejecttokens=1", "-rejecttokenmessages=1"],
            ["-rejecttokens=1", "-rejecttokenmessages=0"],
            ["-corepolicy=0"],
            [],
            ["-rejecttokens=0", "-rejecttokenmessages=1"],
        ]

    def send(self, node: TestNode, script: CScript, reason: str | None) -> None:
        tx = self.wallet.create_self_transfer(fee_rate=0, confirmed_only=True)["tx"]
        tx.vout.append(CTxOut(nValue=0, scriptPubKey=script))
        tx.vout[0].nValue -= 10 * tx.get_vsize()
        tx_hex = tx.serialize().hex()
        if reason is None:
            self.wallet.sendrawtransaction(from_node=node, tx_hex=tx_hex)
            assert tx.rehash() in node.getrawmempool(), f'{tx_hex} not in mempool'
        else:
            assert_raises_rpc_error(-26, reason, self.wallet.sendrawtransaction, from_node=node, tx_hex=tx_hex)

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0])

        self.log.info("With both on, a JSON mint and an Omni message are refused.")
        self.send(self.nodes[0], JSON_MINT, "tokens-json")
        self.send(self.nodes[0], OMNI_SEND, "tokens-omni")

        self.log.info("-rejecttokenmessages=0 relays both while -rejecttokens still refuses Runes.")
        self.send(self.nodes[1], JSON_MINT, None)
        self.send(self.nodes[1], OMNI_SEND, None)
        self.send(self.nodes[1], RUNES, "tokens-runes")

        self.log.info("Both are refused at the defaults, and -corepolicy turns them off.")
        self.send(self.nodes[2], JSON_MINT, "tokens-json")
        self.send(self.nodes[2], OMNI_SEND, "tokens-omni")
        self.send(self.nodes[3], JSON_MINT, None)
        self.send(self.nodes[3], OMNI_SEND, None)

        self.log.info("-rejecttokens=0 turns them off even with -rejecttokenmessages=1.")
        self.send(self.nodes[4], JSON_MINT, None)
        self.send(self.nodes[4], OMNI_SEND, None)


if __name__ == '__main__':
    RejectTokenMessagesTest(__file__).main()
