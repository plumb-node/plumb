Plumb filters
=============

Generated from `plumb/filters.json` by `plumb/tools/gen-assets.py`; edit
those, not this file.

Each filter names where it came from. Most are Knots pull requests, merged
here at the commit we reviewed. A filter marked Plumb's own came from our own
research and has no Knots pull request; it is reviewed and ACKed on its pull
request here like any other.

Most filters count bytes they recognize as data. Knots then applies its data
carrier rules to the count: with the default `-acceptnonstddatacarrier=0`,
any data outside an `OP_RETURN` output means the node does not relay or mine
the transaction, and `-datacarriersize` (83 bytes by default) caps the total.
`-rejecttokenmessages` instead refuses a token message it recognizes, the way
Knots' `-rejecttokens` refuses Runes and Counterparty. The filters never touch
block validity. A block that contains one of these transactions is still
valid and your node still accepts it.

Every filter is on by default. To turn one off, add its line with `=0` to
`bitcoin.conf` (or pass it on the command line) and restart the node.
`-corepolicy` turns all of them off along with the rest of the Knots policy.
To see what is active, check the startup lines in `debug.log`:

```
grep "Plumb filter" ~/.bitcoin/debug.log
```


Fake output hashes and keys
---------------------------

Option `-rejectfakeoutputs`, default on, from [knots#389](https://github.com/bitcoinknots/bitcoin/pull/389) (upstream: open), in Plumb since `v29.4.2.knots20260508.plumb1`.

**What it rejects.** Data hidden in the hashes and keys of outputs. A real hash or x-only key looks random, so one byte value eight times in a program, a run of six identical bytes, or a taproot key that is not on the curve marks it as data, along with its same-type, same-value siblings. Three or more P2WSH outputs sharing one value of 546 sat or less also count: file encoders pay every chunk the same dust amount.

**What it leaves alone.** Payments to every address type, batched payments and pool payouts. The all-zero burn hash used by Stacks block commits is exempt.

**Example.** `f93ee9d2a8d6fc0dce7889732d8e288ca64dacfe4c94c6cd76eb85037daea09c` at block 965224: an ACME file, a run of P2WSH outputs at the 330 sat dust floor whose programs hold a compressed HTML file.

**Turn it off.** In `bitcoin.conf`:

```
rejectfakeoutputs=0
```

or `-rejectfakeoutputs=0` on the command line. The code is on the [`filter/rejectfakeoutputs`](https://github.com/plumb-node/plumb/tree/filter/rejectfakeoutputs) branch and in [knots#389](https://github.com/bitcoinknots/bitcoin/pull/389).


Dead conditional branches
-------------------------

Option `-rejectdeadbranches`, default on, from [knots#400](https://github.com/bitcoinknots/bitcoin/pull/400) (upstream: open), in Plumb since `v29.4.2.knots20260508.plumb1`.

**What it rejects.** Data placed in a branch of a script that can never run because a constant decides the condition, such as `OP_1 OP_NOTIF <data> OP_ENDIF` or the dead side of an `OP_ELSE`. Knots already counts the `OP_FALSE OP_IF` envelope; this catches the other constant-decided forms.

**What it leaves alone.** Branches decided by a signature or a witness value, which is how real spending conditions branch: Lightning, timelocks, multisig recovery paths. Those are never counted.

**Example.** `23e8f9466cd895c21a041155dba377f1a77a46cec13dcb14c430fa9c75f8786a` at block 969961: a 37 KB JPEG XL image in an `OP_1 OP_NOTIF` branch.

**Turn it off.** In `bitcoin.conf`:

```
rejectdeadbranches=0
```

or `-rejectdeadbranches=0` on the command line. The code is on the [`filter/rejectdeadbranches`](https://github.com/plumb-node/plumb/tree/filter/rejectdeadbranches) branch and in [knots#400](https://github.com/bitcoinknots/bitcoin/pull/400).


Bare data envelopes
-------------------

Option `-rejectbareenvelopes`, default on, from [knots#319](https://github.com/bitcoinknots/bitcoin/pull/319) (upstream: open), in Plumb since `v29.4.2.knots20260508.plumb2`.

**What it rejects.** A run of data pushes dropped again with `OP_DROP` or `OP_2DROP`, the envelope shape inscriptions use when `OP_IF` is not available. The whole run counts as data, not only the last push.

**What it leaves alone.** Scripts that drop the result of an opcode, such as `<n> OP_CHECKSEQUENCEVERIFY OP_DROP` in timelocks. Only pushes followed by a drop count.

**Example.** `19782e5ae1ea872b2231f92400311e7e2f6d8259c4c0ecf18f056b977279d9eb` at block 963670: an 87 KB JPEG inscription in an ord envelope ended by `OP_2DROP`.

**Turn it off.** In `bitcoin.conf`:

```
rejectbareenvelopes=0
```

or `-rejectbareenvelopes=0` on the command line. The code is on the [`filter/rejectbareenvelopes`](https://github.com/plumb-node/plumb/tree/filter/rejectbareenvelopes) branch and in [knots#319](https://github.com/bitcoinknots/bitcoin/pull/319).


Fake multisig keys
------------------

Option `-rejectfakemultisig`, default on, from [knots#422](https://github.com/bitcoinknots/bitcoin/pull/422) (upstream: open), in Plumb since `v29.4.2.knots20260508.plumb3`.

**What it rejects.** File chunks dressed as public keys: a 1-of-15 multisig where one key signs and the other 14 each carry 31 bytes of a file. Keys that no signature in the spend can prove, beyond ten per script, count as data. Covers P2WSH, P2SH and tapscript `multi_a`.

**What it leaves alone.** Real multisig and vaults, which since the fork leave at most ten keys unsigned in a script (a 2-of-3 vault with timelocked recovery branches; a plain 2-of-3 leaves one), including federations spending 11-of-15.

**Known cost.** A Liquid federation spend through its 2-of-3 emergency branch leaves 16 keys unsigned and is refused. There has been one since the fork, `97f7d78b0b9ad4d47e150659c3091d905c3565fe7e6fd8977e4326b2b7af2270` at block 963410. Other nodes and miners still carry it.

**Example.** `7690b398fe92a6aacfe65b639f14f274733df205d61e06f9424e7744c313a6e5` at block 974241: a file in 475 inputs at 546 sat each, 474 of them a 1-of-15 multisig whose 14 unused keys are file chunks.

**Turn it off.** In `bitcoin.conf`:

```
rejectfakemultisig=0
```

or `-rejectfakemultisig=0` on the command line. The code is on the [`filter/rejectfakemultisig`](https://github.com/plumb-node/plumb/tree/filter/rejectfakemultisig) branch and in [knots#422](https://github.com/bitcoinknots/bitcoin/pull/422).


Token messages
--------------

Option `-rejecttokenmessages`, default on, Plumb's own filter from [plumb#2](https://github.com/plumb-node/plumb/pull/2); there is no Knots pull request. [knots#64](https://github.com/bitcoinknots/bitcoin/issues/64) asked for filters like it. In Plumb since `v29.4.2.knots20260508.plumb4`.

**What it rejects.** Token operations written into an `OP_RETURN` that `-rejecttokens` does not match. A JSON object whose `"p"` field names the protocol is the BRC-20 format, which ico-20, crc-20 and others copy: `{"p":"ico-20","op":"mint","tick":"LEAF"}`. Omni Layer messages begin with `omni`. Most JSON mints on this chain are 41 or 42 bytes, under even a 42-byte `-datacarriersize`.

**What it leaves alone.** Every other `OP_RETURN`: swap and bridge memos, Stacks block commits, Babylon staking, and random data that happens to start with `{`. The payload has to start as a JSON object with a top-level `"p"` member whose value is a string.

**Example.** `715863e04b618a6818eaec826b8ea900a75a43a5c22859c6b408c68246c4ebcb` at block 971661: the first ico-20 mint since the fork, `{"p":"ico-20","op":"mint","tick":"LEAF"}` in a 42-byte `OP_RETURN`.

**Turn it off.** In `bitcoin.conf`:

```
rejecttokenmessages=0
```

or `-rejecttokenmessages=0` on the command line. The code is on the [`filter/rejecttokenmessages`](https://github.com/plumb-node/plumb/tree/filter/rejecttokenmessages) branch and in [plumb#2](https://github.com/plumb-node/plumb/pull/2).
