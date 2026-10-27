#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Knots developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test token message filtering (-rejecttokenmessages)."""

from test_framework.messages import CTxOut
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import TestNode
from test_framework.util import assert_raises_rpc_error
from test_framework.wallet import MiniWallet

# Payloads as written on mainnet: a JSON mint (715863e04b618a6818eaec826b8ea900a75a43a5c22859c6b408c68246c4ebcb),
# an Omni Layer simple send (7134e618c5970bc3d7346cb4dce83aaf081213c122601caa8e64ff5e32296f38) and random
# data that begins with '{' (444c708b2b6e41d170d37ad9dbbdd2e7d2e942072be27f398b347b830c9a2255).
JSON_MINT = b'{"p":"ico-20","op":"mint","tick":"LEAF"}'
OMNI_SEND = bytes.fromhex("6f6d6e690000000000000003000000000000009d")
BRACE_DATA = bytes.fromhex("7ba0149edc5646e8b3385eb62c9d2dbe")


class RejectTokenMessagesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        # The framework starts every node with -corepolicy, which turns the filter off; nodes 0
        # and 1 set it explicitly, node 2 takes the default without -corepolicy, node 3 keeps it.
        self.extra_args = [
            ["-rejecttokenmessages=1"],
            ["-rejecttokenmessages=0"],
            ["-corepolicy=0"],
            [],
        ]

    def send(self, node: TestNode, payload: bytes, reason: str | None) -> None:
        tx = self.wallet.create_self_transfer(fee_rate=0, confirmed_only=True)["tx"]
        tx.vout.append(CTxOut(nValue=0, scriptPubKey=CScript([OP_RETURN, payload])))
        tx.vout[0].nValue -= 10 * tx.get_vsize()
        tx_hex = tx.serialize().hex()
        if reason is None:
            self.wallet.sendrawtransaction(from_node=node, tx_hex=tx_hex)
            assert tx.rehash() in node.getrawmempool(), f'{tx_hex} not in mempool'
        else:
            assert_raises_rpc_error(-26, reason, self.wallet.sendrawtransaction, from_node=node, tx_hex=tx_hex)

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0])

        self.log.info("A JSON token mint is refused with -rejecttokenmessages=1 and accepted with =0.")
        self.send(self.nodes[0], JSON_MINT, "tokens-json")
        self.send(self.nodes[1], JSON_MINT, None)

        self.log.info("An Omni Layer message is refused with -rejecttokenmessages=1 and accepted with =0.")
        self.send(self.nodes[0], OMNI_SEND, "tokens-omni")
        self.send(self.nodes[1], OMNI_SEND, None)

        self.log.info("Random data that begins with '{' is not a token message.")
        self.send(self.nodes[0], BRACE_DATA, None)

        self.log.info("The filter is on by default and -corepolicy turns it off.")
        self.send(self.nodes[2], JSON_MINT, "tokens-json")
        self.send(self.nodes[2], OMNI_SEND, "tokens-omni")
        self.send(self.nodes[3], JSON_MINT, None)


if __name__ == '__main__':
    RejectTokenMessagesTest(__file__).main()
