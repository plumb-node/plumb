// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <consensus/validation.h>
#include <key.h>
#include <policy/policy.h>
#include <policy/settings.h>
#include <random.h>
#include <script/interpreter.h>
#include <script/sigcache.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <span.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <util/chaintype.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

struct Dersig100Setup : public TestChain100Setup {
    Dersig100Setup()
        : TestChain100Setup{ChainType::REGTEST, {.extra_args = {"-testactivationheight=dersig@102"}}} {}
};

bool CheckInputScripts(const CTransaction& tx, TxValidationState& state,
                       const CCoinsViewCache& inputs, unsigned int flags, bool cacheSigStore,
                       bool cacheFullScriptStore, PrecomputedTransactionData& txdata,
                       ValidationCache& validation_cache,
                       std::vector<CScriptCheck>* pvChecks,
                       const std::vector<unsigned int>& flags_per_input = {},
                       std::vector<VerifiedKeys>* verified_keys = nullptr
) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

BOOST_AUTO_TEST_SUITE(txvalidationcache_tests)

BOOST_FIXTURE_TEST_CASE(tx_mempool_block_doublespend, Dersig100Setup)
{
    // Make sure skipping validation of transactions that were
    // validated going into the memory pool does not allow
    // double-spends in blocks to pass validation when they should not.

    CScript scriptPubKey = CScript() <<  ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    const auto ToMemPool = [this](const CMutableTransaction& tx) {
        LOCK(cs_main);

        const MempoolAcceptResult result = m_node.chainman->ProcessTransaction(MakeTransactionRef(tx));
        return result.m_result_type == MempoolAcceptResult::ResultType::VALID;
    };

    // Create a double-spend of mature coinbase txn:
    std::vector<CMutableTransaction> spends;
    spends.resize(2);
    for (int i = 0; i < 2; i++)
    {
        spends[i].version = 1;
        spends[i].vin.resize(1);
        spends[i].vin[0].prevout.hash = m_coinbase_txns[0]->GetHash();
        spends[i].vin[0].prevout.n = 0;
        spends[i].vout.resize(1);
        spends[i].vout[0].nValue = 11*CENT;
        spends[i].vout[0].scriptPubKey = GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()));

        // Sign:
        std::vector<unsigned char> vchSig;
        uint256 hash = SignatureHash(scriptPubKey, spends[i], 0, SIGHASH_ALL, 0, SigVersion::BASE);
        BOOST_CHECK(coinbaseKey.Sign(hash, vchSig));
        vchSig.push_back((unsigned char)SIGHASH_ALL);
        spends[i].vin[0].scriptSig << vchSig;
    }

    CBlock block;

    // Test 1: block with both of those transactions should be rejected.
    block = CreateAndProcessBlock(spends, scriptPubKey);
    {
        LOCK(cs_main);
        BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() != block.GetHash());
    }

    // Test 2: ... and should be rejected if spend1 is in the memory pool
    BOOST_CHECK(ToMemPool(spends[0]));
    block = CreateAndProcessBlock(spends, scriptPubKey);
    {
        LOCK(cs_main);
        BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() != block.GetHash());
    }
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 1U);
    WITH_LOCK(m_node.mempool->cs, m_node.mempool->removeRecursive(CTransaction{spends[0]}, MemPoolRemovalReason::CONFLICT));
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);

    // Test 3: ... and should be rejected if spend2 is in the memory pool
    BOOST_CHECK(ToMemPool(spends[1]));
    block = CreateAndProcessBlock(spends, scriptPubKey);
    {
        LOCK(cs_main);
        BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() != block.GetHash());
    }
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 1U);
    WITH_LOCK(m_node.mempool->cs, m_node.mempool->removeRecursive(CTransaction{spends[1]}, MemPoolRemovalReason::CONFLICT));
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);

    // Final sanity test: first spend in *m_node.mempool, second in block, that's OK:
    std::vector<CMutableTransaction> oneSpend;
    oneSpend.push_back(spends[0]);
    BOOST_CHECK(ToMemPool(spends[1]));
    block = CreateAndProcessBlock(oneSpend, scriptPubKey);
    {
        LOCK(cs_main);
        BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() == block.GetHash());
    }
    // spends[1] should have been removed from the mempool when the
    // block with spends[0] is accepted:
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);
}

// Run CheckInputScripts (using CoinsTip()) on the given transaction, for all script
// flags.  Test that CheckInputScripts passes for all flags that don't overlap with
// the failing_flags argument, but otherwise fails.
// CHECKLOCKTIMEVERIFY and CHECKSEQUENCEVERIFY (and future NOP codes that may
// get reassigned) have an interaction with DISCOURAGE_UPGRADABLE_NOPS: if
// the script flags used contain DISCOURAGE_UPGRADABLE_NOPS but don't contain
// CHECKLOCKTIMEVERIFY (or CHECKSEQUENCEVERIFY), but the script does contain
// OP_CHECKLOCKTIMEVERIFY (or OP_CHECKSEQUENCEVERIFY), then script execution
// should fail.
// Capture this interaction with the upgraded_nop argument: set it when evaluating
// any script flag that is implemented as an upgraded NOP code.
static void ValidateCheckInputsForAllFlags(const CTransaction &tx, uint32_t failing_flags, bool add_to_cache, CCoinsViewCache& active_coins_tip, ValidationCache& validation_cache) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    PrecomputedTransactionData txdata;

    FastRandomContext insecure_rand(true);

    for (int count = 0; count < 10000; ++count) {
        TxValidationState state;

        // Randomly selects flag combinations
        uint32_t test_flags = (uint32_t) insecure_rand.randrange((SCRIPT_VERIFY_END_MARKER - 1) << 1);

        // SCRIPT_VERIFY_UNIFIED_SIGHASH stays in the sweep. Nothing here opted
        // in, so the flag must make no difference to any of it, and sweeping it
        // is what holds that: a change letting the new rules reach a signature
        // that did not ask for them would fail these.

        // Filter out incompatible flag choices
        if ((test_flags & SCRIPT_VERIFY_CLEANSTACK)) {
            // CLEANSTACK requires P2SH and WITNESS, see VerifyScript() in
            // script/interpreter.cpp
            test_flags |= SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS;
        }
        if ((test_flags & SCRIPT_VERIFY_WITNESS)) {
            // WITNESS requires P2SH
            test_flags |= SCRIPT_VERIFY_P2SH;
        }
        bool ret = CheckInputScripts(tx, state, &active_coins_tip, test_flags, true, add_to_cache, txdata, validation_cache, nullptr);
        // CheckInputScripts should succeed iff test_flags doesn't intersect with
        // failing_flags
        bool expected_return_value = !(test_flags & failing_flags);
        BOOST_CHECK_EQUAL(ret, expected_return_value);

        // Test the caching
        if (ret && add_to_cache) {
            // Check that we get a cache hit if the tx was valid
            std::vector<CScriptCheck> scriptchecks;
            BOOST_CHECK(CheckInputScripts(tx, state, &active_coins_tip, test_flags, true, add_to_cache, txdata, validation_cache, &scriptchecks));
            BOOST_CHECK(scriptchecks.empty());
        } else {
            // Check that we get script executions to check, if the transaction
            // was invalid, or we didn't add to cache.
            std::vector<CScriptCheck> scriptchecks;
            BOOST_CHECK(CheckInputScripts(tx, state, &active_coins_tip, test_flags, true, add_to_cache, txdata, validation_cache, &scriptchecks));
            BOOST_CHECK_EQUAL(scriptchecks.size(), tx.vin.size());
        }
    }
}

BOOST_FIXTURE_TEST_CASE(checkinputs_test, Dersig100Setup)
{
    // Test that passing CheckInputScripts with one set of script flags doesn't imply
    // that we would pass again with a different set of flags.
    CScript p2pk_scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    CScript p2sh_scriptPubKey = GetScriptForDestination(ScriptHash(p2pk_scriptPubKey));
    CScript p2pkh_scriptPubKey = GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()));
    CScript p2wpkh_scriptPubKey = GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey()));

    FillableSigningProvider keystore;
    BOOST_CHECK(keystore.AddKey(coinbaseKey));
    BOOST_CHECK(keystore.AddCScript(p2pk_scriptPubKey));

    // flags to test: SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY, SCRIPT_VERIFY_CHECKSEQUENCE_VERIFY, SCRIPT_VERIFY_NULLDUMMY, uncompressed pubkey thing

    // Create 2 outputs that match the three scripts above, spending the first
    // coinbase tx.
    CMutableTransaction spend_tx;

    spend_tx.version = 1;
    spend_tx.vin.resize(1);
    spend_tx.vin[0].prevout.hash = m_coinbase_txns[0]->GetHash();
    spend_tx.vin[0].prevout.n = 0;
    spend_tx.vout.resize(4);
    spend_tx.vout[0].nValue = 11*CENT;
    spend_tx.vout[0].scriptPubKey = p2sh_scriptPubKey;
    spend_tx.vout[1].nValue = 11*CENT;
    spend_tx.vout[1].scriptPubKey = p2wpkh_scriptPubKey;
    spend_tx.vout[2].nValue = 11*CENT;
    spend_tx.vout[2].scriptPubKey = CScript() << OP_CHECKLOCKTIMEVERIFY << OP_DROP << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    spend_tx.vout[3].nValue = 11*CENT;
    spend_tx.vout[3].scriptPubKey = CScript() << OP_CHECKSEQUENCEVERIFY << OP_DROP << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    // Sign, with a non-DER signature
    {
        std::vector<unsigned char> vchSig;
        uint256 hash = SignatureHash(p2pk_scriptPubKey, spend_tx, 0, SIGHASH_ALL, 0, SigVersion::BASE);
        BOOST_CHECK(coinbaseKey.Sign(hash, vchSig));
        vchSig.push_back((unsigned char) 0); // padding byte makes this non-DER
        vchSig.push_back((unsigned char)SIGHASH_ALL);
        spend_tx.vin[0].scriptSig << vchSig;
    }

    // Test that invalidity under a set of flags doesn't preclude validity
    // under other (eg consensus) flags.
    // spend_tx is invalid according to DERSIG
    {
        LOCK(cs_main);

        TxValidationState state;
        PrecomputedTransactionData ptd_spend_tx;

        BOOST_CHECK(!CheckInputScripts(CTransaction(spend_tx), state, &m_node.chainman->ActiveChainstate().CoinsTip(), SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_DERSIG, true, true, ptd_spend_tx, m_node.chainman->m_validation_cache, nullptr));

        // If we call again asking for scriptchecks (as happens in
        // ConnectBlock), we should add a script check object for this -- we're
        // not caching invalidity (if that changes, delete this test case).
        std::vector<CScriptCheck> scriptchecks;
        BOOST_CHECK(CheckInputScripts(CTransaction(spend_tx), state, &m_node.chainman->ActiveChainstate().CoinsTip(), SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_DERSIG, true, true, ptd_spend_tx, m_node.chainman->m_validation_cache, &scriptchecks));
        BOOST_CHECK_EQUAL(scriptchecks.size(), 1U);

        // Test that CheckInputScripts returns true iff DERSIG-enforcing flags are
        // not present.  Don't add these checks to the cache, so that we can
        // test later that block validation works fine in the absence of cached
        // successes.
        ValidateCheckInputsForAllFlags(CTransaction(spend_tx), SCRIPT_VERIFY_DERSIG | SCRIPT_VERIFY_LOW_S | SCRIPT_VERIFY_STRICTENC, false, m_node.chainman->ActiveChainstate().CoinsTip(), m_node.chainman->m_validation_cache);
    }

    // And if we produce a block with this tx, it should be valid (DERSIG not
    // enabled yet), even though there's no cache entry.
    CBlock block;

    block = CreateAndProcessBlock({spend_tx}, p2pk_scriptPubKey);
    LOCK(cs_main);
    BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() == block.GetHash());
    BOOST_CHECK(m_node.chainman->ActiveChainstate().CoinsTip().GetBestBlock() == block.GetHash());

    // Test P2SH: construct a transaction that is valid without P2SH, and
    // then test validity with P2SH.
    {
        CMutableTransaction invalid_under_p2sh_tx;
        invalid_under_p2sh_tx.version = 1;
        invalid_under_p2sh_tx.vin.resize(1);
        invalid_under_p2sh_tx.vin[0].prevout.hash = spend_tx.GetHash();
        invalid_under_p2sh_tx.vin[0].prevout.n = 0;
        invalid_under_p2sh_tx.vout.resize(1);
        invalid_under_p2sh_tx.vout[0].nValue = 11*CENT;
        invalid_under_p2sh_tx.vout[0].scriptPubKey = p2pk_scriptPubKey;
        std::vector<unsigned char> vchSig2(p2pk_scriptPubKey.begin(), p2pk_scriptPubKey.end());
        invalid_under_p2sh_tx.vin[0].scriptSig << vchSig2;

        ValidateCheckInputsForAllFlags(CTransaction(invalid_under_p2sh_tx), SCRIPT_VERIFY_P2SH, true, m_node.chainman->ActiveChainstate().CoinsTip(), m_node.chainman->m_validation_cache);
    }

    // Test CHECKLOCKTIMEVERIFY
    {
        CMutableTransaction invalid_with_cltv_tx;
        invalid_with_cltv_tx.version = 1;
        invalid_with_cltv_tx.nLockTime = 100;
        invalid_with_cltv_tx.vin.resize(1);
        invalid_with_cltv_tx.vin[0].prevout.hash = spend_tx.GetHash();
        invalid_with_cltv_tx.vin[0].prevout.n = 2;
        invalid_with_cltv_tx.vin[0].nSequence = 0;
        invalid_with_cltv_tx.vout.resize(1);
        invalid_with_cltv_tx.vout[0].nValue = 11*CENT;
        invalid_with_cltv_tx.vout[0].scriptPubKey = p2pk_scriptPubKey;

        // Sign
        std::vector<unsigned char> vchSig;
        uint256 hash = SignatureHash(spend_tx.vout[2].scriptPubKey, invalid_with_cltv_tx, 0, SIGHASH_ALL, 0, SigVersion::BASE);
        BOOST_CHECK(coinbaseKey.Sign(hash, vchSig));
        vchSig.push_back((unsigned char)SIGHASH_ALL);
        invalid_with_cltv_tx.vin[0].scriptSig = CScript() << vchSig << 101;

        ValidateCheckInputsForAllFlags(CTransaction(invalid_with_cltv_tx), SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY, true, m_node.chainman->ActiveChainstate().CoinsTip(), m_node.chainman->m_validation_cache);

        // Make it valid, and check again
        invalid_with_cltv_tx.vin[0].scriptSig = CScript() << vchSig << 100;
        TxValidationState state;
        PrecomputedTransactionData txdata;
        BOOST_CHECK(CheckInputScripts(CTransaction(invalid_with_cltv_tx), state, m_node.chainman->ActiveChainstate().CoinsTip(), SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY, true, true, txdata, m_node.chainman->m_validation_cache, nullptr));
    }

    // TEST CHECKSEQUENCEVERIFY
    {
        CMutableTransaction invalid_with_csv_tx;
        invalid_with_csv_tx.version = 2;
        invalid_with_csv_tx.vin.resize(1);
        invalid_with_csv_tx.vin[0].prevout.hash = spend_tx.GetHash();
        invalid_with_csv_tx.vin[0].prevout.n = 3;
        invalid_with_csv_tx.vin[0].nSequence = 100;
        invalid_with_csv_tx.vout.resize(1);
        invalid_with_csv_tx.vout[0].nValue = 11*CENT;
        invalid_with_csv_tx.vout[0].scriptPubKey = p2pk_scriptPubKey;

        // Sign
        std::vector<unsigned char> vchSig;
        uint256 hash = SignatureHash(spend_tx.vout[3].scriptPubKey, invalid_with_csv_tx, 0, SIGHASH_ALL, 0, SigVersion::BASE);
        BOOST_CHECK(coinbaseKey.Sign(hash, vchSig));
        vchSig.push_back((unsigned char)SIGHASH_ALL);
        invalid_with_csv_tx.vin[0].scriptSig = CScript() << vchSig << 101;

        ValidateCheckInputsForAllFlags(CTransaction(invalid_with_csv_tx), SCRIPT_VERIFY_CHECKSEQUENCEVERIFY, true, m_node.chainman->ActiveChainstate().CoinsTip(), m_node.chainman->m_validation_cache);

        // Make it valid, and check again
        invalid_with_csv_tx.vin[0].scriptSig = CScript() << vchSig << 100;
        TxValidationState state;
        PrecomputedTransactionData txdata;
        BOOST_CHECK(CheckInputScripts(CTransaction(invalid_with_csv_tx), state, &m_node.chainman->ActiveChainstate().CoinsTip(), SCRIPT_VERIFY_CHECKSEQUENCEVERIFY, true, true, txdata, m_node.chainman->m_validation_cache, nullptr));
    }

    // TODO: add tests for remaining script flags

    // Test that passing CheckInputScripts with a valid witness doesn't imply success
    // for the same tx with a different witness.
    {
        CMutableTransaction valid_with_witness_tx;
        valid_with_witness_tx.version = 1;
        valid_with_witness_tx.vin.resize(1);
        valid_with_witness_tx.vin[0].prevout.hash = spend_tx.GetHash();
        valid_with_witness_tx.vin[0].prevout.n = 1;
        valid_with_witness_tx.vout.resize(1);
        valid_with_witness_tx.vout[0].nValue = 11*CENT;
        valid_with_witness_tx.vout[0].scriptPubKey = p2pk_scriptPubKey;

        // Sign
        SignatureData sigdata;
        BOOST_CHECK(ProduceSignature(keystore, MutableTransactionSignatureCreator(valid_with_witness_tx, 0, 11 * CENT, SIGHASH_ALL), spend_tx.vout[1].scriptPubKey, sigdata));
        UpdateInput(valid_with_witness_tx.vin[0], sigdata);

        // This should be valid under all script flags.
        ValidateCheckInputsForAllFlags(CTransaction(valid_with_witness_tx), 0, true, m_node.chainman->ActiveChainstate().CoinsTip(), m_node.chainman->m_validation_cache);

        // Remove the witness, and check that it is now invalid.
        valid_with_witness_tx.vin[0].scriptWitness.SetNull();
        ValidateCheckInputsForAllFlags(CTransaction(valid_with_witness_tx), SCRIPT_VERIFY_WITNESS, true, m_node.chainman->ActiveChainstate().CoinsTip(), m_node.chainman->m_validation_cache);
    }

    {
        // Test a transaction with multiple inputs.
        CMutableTransaction tx;

        tx.version = 1;
        tx.vin.resize(2);
        tx.vin[0].prevout.hash = spend_tx.GetHash();
        tx.vin[0].prevout.n = 0;
        tx.vin[1].prevout.hash = spend_tx.GetHash();
        tx.vin[1].prevout.n = 1;
        tx.vout.resize(1);
        tx.vout[0].nValue = 22*CENT;
        tx.vout[0].scriptPubKey = p2pk_scriptPubKey;

        // Sign
        for (int i = 0; i < 2; ++i) {
            SignatureData sigdata;
            BOOST_CHECK(ProduceSignature(keystore, MutableTransactionSignatureCreator(tx, i, 11 * CENT, SIGHASH_ALL), spend_tx.vout[i].scriptPubKey, sigdata));
            UpdateInput(tx.vin[i], sigdata);
        }

        // This should be valid under all script flags
        ValidateCheckInputsForAllFlags(CTransaction(tx), 0, true, m_node.chainman->ActiveChainstate().CoinsTip(), m_node.chainman->m_validation_cache);

        // Check that if the second input is invalid, but the first input is
        // valid, the transaction is not cached.
        // Invalidate vin[1]
        tx.vin[1].scriptWitness.SetNull();

        TxValidationState state;
        PrecomputedTransactionData txdata;
        // This transaction is now invalid under segwit, because of the second input.
        BOOST_CHECK(!CheckInputScripts(CTransaction(tx), state, &m_node.chainman->ActiveChainstate().CoinsTip(), SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS, true, true, txdata, m_node.chainman->m_validation_cache, nullptr));

        std::vector<CScriptCheck> scriptchecks;
        // Make sure this transaction was not cached (ie because the first
        // input was valid)
        BOOST_CHECK(CheckInputScripts(CTransaction(tx), state, &m_node.chainman->ActiveChainstate().CoinsTip(), SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS, true, true, txdata, m_node.chainman->m_validation_cache, &scriptchecks));
        // Should get 2 script checks back -- caching is on a whole-transaction basis.
        BOOST_CHECK_EQUAL(scriptchecks.size(), 2U);
    }
}

BOOST_FIXTURE_TEST_CASE(checkinputs_flags_per_input_cache_safety, Dersig100Setup)
{
    // Reproducer for cache poisoning via per-input flag relaxation.
    //
    // A 300-byte witness push passes only when SCRIPT_VERIFY_REDUCED_ELEMENT_SIZE is
    // relaxed (as it would be for pre-activation UTXOs). The cache key is
    // computed from the strict global flags, so if the result is cached after
    // passing with relaxed per-input flags, a subsequent strict-flags lookup
    // will incorrectly return a cache hit.

    const auto& coinbase_script{m_coinbase_txns[0]->vout[0].scriptPubKey};
    const unsigned int strict_flags{SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_REDUCED_ELEMENT_SIZE};
    const unsigned int relaxed_flags{SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS};

    // P2WSH witness script: OP_DROP OP_TRUE (accepts any single witness element)
    const CScript witness_script = CScript() << OP_DROP << OP_TRUE;
    const std::vector<unsigned char> big_witness_elem(300, 0x42);
    const CScript p2wsh_script = GetScriptForDestination(WitnessV0ScriptHash(witness_script));

    // Mine a funding tx that pays to the P2WSH output.
    const auto mine_funding_tx{[&]
    {
        CMutableTransaction tx;
        tx.vin = {CTxIn{m_coinbase_txns[0]->GetHash(), 0}};
        tx.vout = {CTxOut{11 * CENT, p2wsh_script}};

        std::vector<unsigned char> vchSig;
        const uint256 hash = SignatureHash(coinbase_script, tx, 0, SIGHASH_ALL, 0, SigVersion::BASE);
        BOOST_CHECK(coinbaseKey.Sign(hash, vchSig));
        vchSig.push_back(SIGHASH_ALL);
        tx.vin[0].scriptSig << vchSig;

        const CBlock block = CreateAndProcessBlock({tx}, coinbase_script);
        LOCK(cs_main);
        BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() == block.GetHash());
        return CTransaction{tx};
    }};
    const CTransaction funding_tx{mine_funding_tx()};

    // Build spending tx with a 300-byte witness element (violates REDUCED_DATA).
    CMutableTransaction spend_tx;
    spend_tx.vin = {CTxIn{funding_tx.GetHash(), 0}};
    spend_tx.vout = {CTxOut{10 * CENT, GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()))}};
    spend_tx.vin[0].scriptWitness.stack = {big_witness_elem, {witness_script.begin(), witness_script.end()}};
    const CTransaction spend{spend_tx};
    BOOST_CHECK_EQUAL(spend.vin[0].scriptWitness.stack[0].size(), 300U);

    LOCK(cs_main);
    auto& coins_tip = m_node.chainman->ActiveChainstate().CoinsTip();

    // Use a fresh validation cache to isolate this test.
    ValidationCache validation_cache{/*script_execution_cache_bytes=*/1 << 20, /*signature_cache_bytes=*/1 << 20};

    const auto run_check{[&](const std::vector<unsigned int>& flags_per_input) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        TxValidationState state;
        PrecomputedTransactionData txdata;
        return CheckInputScripts(spend, state, &coins_tip, strict_flags,
                                 /*cacheSigStore=*/true, /*cacheFullScriptStore=*/true,
                                 txdata, validation_cache, /*pvChecks=*/nullptr, flags_per_input);
    }};

    // Step 1: strict validation (REDUCED_DATA enforced) must fail.
    BOOST_CHECK(!run_check({}));

    // Step 2: relaxed per-input flags (no REDUCED_DATA) must pass.
    BOOST_CHECK(run_check({relaxed_flags}));

    // Step 3: strict validation must STILL fail.
    // If the cache was poisoned in step 2, this incorrectly passes.
    BOOST_CHECK(!run_check({}));
}

BOOST_FIXTURE_TEST_CASE(checkinputs_records_verified_keys, TestChain100Setup)
{
    // A pay-to-pubkey spend with a real signature
    const CScript spk{CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG};
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint{m_coinbase_txns[0]->GetHash(), 0};
    mtx.vout.emplace_back(CENT, spk);
    std::vector<unsigned char> sig;
    BOOST_REQUIRE(coinbaseKey.Sign(SignatureHash(spk, mtx, 0, SIGHASH_ALL, 0, SigVersion::BASE), sig));
    sig.push_back(SIGHASH_ALL);
    mtx.vin[0].scriptSig << sig;
    const CTransaction tx{mtx};

    LOCK(cs_main);
    CCoinsViewCache& coins{m_node.chainman->ActiveChainstate().CoinsTip()};
    ValidationCache& cache{m_node.chainman->m_validation_cache};
    const unsigned int flags{SCRIPT_VERIFY_P2SH};
    const auto key_bytes{ToByteVector(coinbaseKey.GetPubKey())};

    // The result is the same with and without a recorder, and the key that verified is noted.
    {
        TxValidationState state;
        PrecomputedTransactionData txdata;
        BOOST_CHECK(CheckInputScripts(tx, state, &coins, flags, true, false, txdata, cache, nullptr));
    }
    for (int round{0}; round < 2; ++round) {
        // The second round finds the signature in the signature cache, and still records it.
        TxValidationState state;
        PrecomputedTransactionData txdata;
        std::vector<VerifiedKeys> keys;
        BOOST_CHECK(CheckInputScripts(tx, state, &coins, flags, true, false, txdata, cache, nullptr, {}, &keys));
        BOOST_REQUIRE_EQUAL(keys.size(), 1U);
        // The key, and apart from it the signature that verified against it
        BOOST_CHECK(keys[0] == (VerifiedKeys{{key_bytes}, {sig}}));
    }

    // Queued checks are block validation's, which never records.
    {
        TxValidationState state;
        PrecomputedTransactionData txdata;
        std::vector<VerifiedKeys> keys;
        std::vector<CScriptCheck> checks;
        BOOST_CHECK(CheckInputScripts(tx, state, &coins, flags, true, false, txdata, cache, &checks, {}, &keys));
        BOOST_CHECK_EQUAL(checks.size(), 1U);
        BOOST_CHECK(keys.empty());
    }

    // Once the script execution cache has it, a check that records still runs the scripts and records,
    // and leaves the entry in place. A queued check that finds the entry queues nothing, which shows it is there.
    const auto queued_without_recorder{[&]() EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        TxValidationState state;
        PrecomputedTransactionData txdata;
        std::vector<CScriptCheck> checks;
        BOOST_CHECK(CheckInputScripts(tx, state, &coins, flags, true, true, txdata, cache, &checks));
        return checks.size();
    }};
    BOOST_CHECK_EQUAL(queued_without_recorder(), 1U);
    {
        TxValidationState state;
        PrecomputedTransactionData txdata;
        BOOST_CHECK(CheckInputScripts(tx, state, &coins, flags, true, true, txdata, cache, nullptr));
    }
    BOOST_CHECK_EQUAL(queued_without_recorder(), 0U);
    for (const bool cache_full_script_store : {true, false}) {
        TxValidationState state;
        PrecomputedTransactionData txdata;
        // stale content must not survive
        std::vector<VerifiedKeys> keys{VerifiedKeys{{std::vector<unsigned char>(33, 2)}, {}}, VerifiedKeys{}};
        BOOST_CHECK(CheckInputScripts(tx, state, &coins, flags, true, cache_full_script_store, txdata, cache, nullptr, {}, &keys));
        BOOST_REQUIRE_EQUAL(keys.size(), 1U);
        BOOST_CHECK(keys[0] == (VerifiedKeys{{key_bytes}, {sig}}));
        BOOST_CHECK_EQUAL(queued_without_recorder(), 0U);
    }

    // A failing script fails the same way, recorder or not.
    CMutableTransaction bad{mtx};
    bad.vin[0].scriptSig = CScript() << std::vector<unsigned char>(72, 1);
    {
        TxValidationState state;
        PrecomputedTransactionData txdata;
        std::vector<VerifiedKeys> keys;
        BOOST_CHECK(!CheckInputScripts(CTransaction{bad}, state, &coins, flags, true, false, txdata, cache, nullptr, {}, &keys));
    }

    // Under the unified signature hash rules a signature is recorded like any other, when the flags select
    // those rules. Without them the same signature does not verify, so nothing is recorded.
    {
        const CTxOut spent_out{coins.AccessCoin(mtx.vin[0].prevout).out};
        CMutableTransaction umtx{mtx};
        umtx.vin[0].scriptSig = CScript{};
        PrecomputedTransactionData sign_data;
        sign_data.Init(umtx, {spent_out}, /*force=*/true);
        const int hash_type{SIGHASH_ALL | SIGHASH_UNIFIED};
        uint256 hash;
        BOOST_REQUIRE(SignatureHashUnified(hash, spk, umtx, 0, hash_type, SigVersion::BASE, sign_data));
        std::vector<unsigned char> usig;
        BOOST_REQUIRE(coinbaseKey.Sign(hash, usig));
        usig.push_back(static_cast<unsigned char>(hash_type));
        umtx.vin[0].scriptSig << usig;
        const CTransaction utx{umtx};
        for (const bool unified : {true, false}) {
            TxValidationState state;
            PrecomputedTransactionData txdata;
            std::vector<VerifiedKeys> keys;
            const unsigned int uflags{unified ? flags | SCRIPT_VERIFY_UNIFIED_SIGHASH : flags};
            BOOST_CHECK_EQUAL(CheckInputScripts(utx, state, &coins, uflags, true, false, txdata, cache, nullptr, {}, &keys), unified);
            if (unified) {
                BOOST_REQUIRE_EQUAL(keys.size(), 1U);
                BOOST_CHECK(keys[0] == (VerifiedKeys{{key_bytes}, {usig}}));
            }
        }
    }
}

BOOST_FIXTURE_TEST_CASE(plumb_rejectfakemultisig_keeps_script_execution_cache, TestChain100Setup)
{
    // knots#470 records the keys a signature verified against for its recount, and a check that records
    // them skips the script execution cache. Plumb's -rejectfakemultisig=0, with -rejectunverifiedwitness=0
    // and -rejectunprovenkeys=0, turns the recount off, so the policy script check records nothing and reads
    // the cache. A witness spend that breaks only a policy flag (MINIMALIF), with an entry put in the cache
    // for its policy flags, tells the two apart: a check that reads the cache accepts it, one that records
    // keys runs the scripts and refuses it.
    const CScript p2pk{CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG};
    const CScript witness_script{CScript() << OP_IF << OP_TRUE << OP_ENDIF};

    CMutableTransaction fund;
    fund.version = 2;
    fund.vin.emplace_back(COutPoint{m_coinbase_txns[0]->GetHash(), 0});
    fund.vout.emplace_back(49 * COIN, GetScriptForDestination(WitnessV0ScriptHash(witness_script)));
    std::vector<unsigned char> sig;
    BOOST_REQUIRE(coinbaseKey.Sign(SignatureHash(p2pk, fund, 0, SIGHASH_ALL, 0, SigVersion::BASE), sig));
    sig.push_back(SIGHASH_ALL);
    fund.vin[0].scriptSig << sig;
    BOOST_REQUIRE_EQUAL(CreateAndProcessBlock({fund}, p2pk).vtx.size(), 2U);

    CMutableTransaction spend;
    spend.version = 2;
    spend.vin.emplace_back(COutPoint{fund.GetHash(), 0});
    // 2 is true but not minimal: valid under the block flags, refused under MINIMALIF
    spend.vin[0].scriptWitness.stack = {{2}, {witness_script.begin(), witness_script.end()}};
    spend.vout.emplace_back(48 * COIN, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey())));
    const CTransactionRef tx{MakeTransactionRef(spend)};

    LOCK(cs_main);
    BOOST_REQUIRE(!m_node.chainman->ActiveChainstate().CoinsTip().AccessCoin(spend.vin[0].prevout).IsSpent());
    ValidationCache& validation_cache{m_node.chainman->m_validation_cache};
    // The policy flags, with or without the unified sighash flag of the BLAKE2b fork
    for (const unsigned int flags : {STANDARD_SCRIPT_VERIFY_FLAGS, STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_UNIFIED_SIGHASH}) {
        uint256 entry;
        validation_cache.ScriptExecutionCacheHasher().Write(UCharCast(tx->GetWitnessHash().begin()), 32).Write((unsigned char*)&flags, sizeof(flags)).Finalize(entry.begin());
        validation_cache.m_script_execution_cache.insert(entry);
    }
    // -rejectunverifiedwitness and -rejectunprovenkeys read the same recording, so the recount runs while any of
    // the three switches is on
    const bool saved{::g_reject_fake_multisig}, saved_witness{::g_reject_unverified_witness}, saved_keys{::g_reject_unproven_keys};
    for (const int on : {7, 6, 5, 4, 3, 2, 1, 0}) {
        ::g_reject_fake_multisig = on & 1;
        ::g_reject_unverified_witness = on & 2;
        ::g_reject_unproven_keys = on & 4;
        const MempoolAcceptResult res{AcceptToMemoryPool(m_node.chainman->ActiveChainstate(), tx, GetTime(), /*bypass_limits=*/false, /*test_accept=*/true)};
        if (on) {
            // On: keys are recorded, so the scripts run and MINIMALIF refuses the spend
            BOOST_CHECK(res.m_result_type == MempoolAcceptResult::ResultType::INVALID);
            BOOST_CHECK_EQUAL(res.m_state.GetRejectReason(), "mempool-script-verify-flag-failed (OP_IF/NOTIF argument must be minimal)");
        } else {
            // Off: nothing is recorded, the cache entry stands in for the policy check, and the block flags pass
            BOOST_CHECK_MESSAGE(res.m_result_type == MempoolAcceptResult::ResultType::VALID, res.m_state.ToString());
        }
    }
    ::g_reject_fake_multisig = saved;
    ::g_reject_unverified_witness = saved_witness;
    ::g_reject_unproven_keys = saved_keys;
}

BOOST_FIXTURE_TEST_CASE(plumb_unverified_witness_count, BasicTestingSetup)
{
    // -rejectunverifiedwitness: the recount adds the bytes of witness items that no verified signature
    // check used, past UNVERIFIED_WITNESS_ALLOWANCE per input. The verified sets are given here directly;
    // plumb_unverified_witness_mempool below takes them from real signatures.
    const std::vector<unsigned char> key(33, 2), sig(72, 0x30);
    const std::vector<unsigned char> xonly(32, 3), control(33, 0xc0), annex{ANNEX_TAG, 1};
    const CScript wscript{CScript() << OP_2DROP << OP_2DROP << OP_2DROP << key << OP_CHECKSIG};
    const CScript tscript{CScript() << OP_2DROP << OP_2DROP << OP_2DROP << xonly << OP_CHECKSIG};
    const auto junk{[](size_t bytes) {
        // Constant filler in items of at most 64 bytes
        std::vector<std::vector<unsigned char>> items;
        for (; bytes; bytes -= std::min<size_t>(bytes, 64)) items.emplace_back(std::min<size_t>(bytes, 64), 0x01);
        return items;
    }};
    enum class Kind { P2WSH, P2SH_P2WSH, TAPSCRIPT, TAPSCRIPT_ANNEX, P2WPKH };
    // The bytes the recount adds over the static count, for one input of the given kind
    const auto added{[&](Kind kind, std::vector<std::vector<unsigned char>> items, const VerifiedKeys& verified, bool with_keys = true) {
        CMutableTransaction mtx;
        mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
        mtx.vout.emplace_back(1000, CScript() << OP_TRUE);
        CScript spk;
        auto& stack{mtx.vin[0].scriptWitness.stack};
        stack = {sig};
        stack.insert(stack.end(), items.begin(), items.end());
        switch (kind) {
        case Kind::P2WSH:
            spk = GetScriptForDestination(WitnessV0ScriptHash(wscript));
            stack.emplace_back(wscript.begin(), wscript.end());
            break;
        case Kind::P2SH_P2WSH: {
            const CScript inner{GetScriptForDestination(WitnessV0ScriptHash(wscript))};
            spk = GetScriptForDestination(ScriptHash(inner));
            mtx.vin[0].scriptSig = CScript() << ToByteVector(inner);
            stack.emplace_back(wscript.begin(), wscript.end());
            break;
        }
        case Kind::TAPSCRIPT: case Kind::TAPSCRIPT_ANNEX:
            spk = CScript() << OP_1 << xonly;
            stack.emplace_back(tscript.begin(), tscript.end());
            stack.push_back(control);
            if (kind == Kind::TAPSCRIPT_ANNEX) stack.push_back(annex);
            break;
        case Kind::P2WPKH:
            spk = GetScriptForDestination(WitnessV0KeyHash(uint160{}));
            break;
        }
        CCoinsView dummy;
        CCoinsViewCache view{&dummy};
        view.AddCoin(mtx.vin[0].prevout, Coin{CTxOut{2000, spk}, 1, false}, true);
        const CTransaction tx{mtx};
        const std::vector<size_t> data_outputs(tx.vout.size(), 0);
        const std::vector<VerifiedKeys> keys{verified};
        const auto st{DatacarrierBytes(tx, view, data_outputs)};
        const auto rc{DatacarrierBytes(tx, view, data_outputs, with_keys ? &keys : nullptr)};
        BOOST_CHECK_EQUAL(rc.second, st.second);
        return rc.first - st.first;
    }};
    const VerifiedKeys verified{{key}, {sig}};
    const size_t allowance{UNVERIFIED_WITNESS_ALLOWANCE};
    const bool saved{::g_reject_unverified_witness};
    ::g_reject_unverified_witness = true;
    for (const Kind kind : {Kind::P2WSH, Kind::P2SH_P2WSH, Kind::TAPSCRIPT, Kind::TAPSCRIPT_ANNEX}) {
        // Up to the allowance nothing is added, and every byte past it is
        BOOST_CHECK_EQUAL(added(kind, junk(allowance), verified), 0U);
        BOOST_CHECK_EQUAL(added(kind, junk(allowance + 1), verified), 1U);
        BOOST_CHECK_EQUAL(added(kind, junk(13 * 64), verified), 13 * 64 - allowance);
        // A signature no check verified is an item like any other
        BOOST_CHECK_EQUAL(added(kind, junk(allowance), VerifiedKeys{{key}, {}}), sig.size());
        // A copy of a verified key is used, wherever it sits
        BOOST_CHECK_EQUAL(added(kind, {key, key, key, key}, verified), 0U);
        // Empty items and small numbers fit in the allowance
        BOOST_CHECK_EQUAL(added(kind, {{}, {1}, {0x10, 0x27}}, verified), 0U);
        // Static count: no keys observed, nothing added
        BOOST_CHECK_EQUAL(added(kind, junk(13 * 64), verified, /*with_keys=*/false), 0U);
    }
    // A P2WPKH spend has no witness script, so it is never recounted
    BOOST_CHECK_EQUAL(added(Kind::P2WPKH, {}, VerifiedKeys{}), 0U);
    // Switched off, nothing is added
    ::g_reject_unverified_witness = false;
    BOOST_CHECK_EQUAL(added(Kind::P2WSH, junk(13 * 64), verified), 0U);
    ::g_reject_unverified_witness = saved;
}

BOOST_FIXTURE_TEST_CASE(plumb_unverified_witness_mempool, TestChain100Setup)
{
    // Real signatures through the mempool: P2WSH spends whose scripts drop witness items, refused once
    // the dropped bytes pass the allowance plus -datacarriersize, and accepted with the switch off.
    const CScript p2pk{CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG};
    const size_t limit{m_node.mempool->m_opts.max_datacarrier_bytes.value_or(0)};
    BOOST_REQUIRE(m_node.mempool->m_opts.datacarrier_fullcount);

    // drops: items of 64 bytes and one remainder, dropped before a single-key check
    // guarded: the script checks its key, then puts 13 more signature checks behind OP_SIZE 64 OP_EQUAL
    // OP_NOTIF, which a 64-byte item on top skips, and drops 13 items of 64 bytes
    const auto drops_script{[&](size_t items) {
        CScript s;
        for (size_t i{0}; i < items / 2; ++i) s << OP_2DROP;
        if (items % 2) s << OP_DROP;
        return s << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    }};
    CScript guarded{CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG << OP_TOALTSTACK << OP_SIZE << 64 << OP_EQUAL << OP_NOTIF};
    for (int i{0}; i < 13; ++i) guarded << OP_CHECKSIG;
    guarded << OP_ENDIF;
    for (int i{0}; i < 6; ++i) guarded << OP_2DROP;
    guarded << OP_DROP << OP_FROMALTSTACK;

    struct Case {
        CScript script;
        std::vector<std::vector<unsigned char>> items;
        bool sig_on_top; //!< guarded checks the signature first, so it sits above the items
        bool refused;
    };
    const auto items_of{[](size_t bytes) {
        std::vector<std::vector<unsigned char>> items;
        for (; bytes; bytes -= std::min<size_t>(bytes, 64)) items.emplace_back(std::min<size_t>(bytes, 64), 0x01);
        return items;
    }};
    const size_t edge{UNVERIFIED_WITNESS_ALLOWANCE + limit};
    std::vector<Case> cases;
    for (const size_t bytes : {edge, edge + 1}) {
        auto items{items_of(bytes)};
        cases.push_back({drops_script(items.size()), items, /*sig_on_top=*/false, bytes > edge});
    }
    cases.push_back({guarded, items_of(13 * 64), /*sig_on_top=*/true, true});

    // Fund one P2WSH output per case
    CMutableTransaction fund;
    fund.version = 2;
    fund.vin.emplace_back(COutPoint{m_coinbase_txns[0]->GetHash(), 0});
    for (const Case& c : cases) fund.vout.emplace_back(10 * COIN, GetScriptForDestination(WitnessV0ScriptHash(c.script)));
    std::vector<unsigned char> fund_sig;
    BOOST_REQUIRE(coinbaseKey.Sign(SignatureHash(p2pk, fund, 0, SIGHASH_ALL, 0, SigVersion::BASE), fund_sig));
    fund_sig.push_back(SIGHASH_ALL);
    fund.vin[0].scriptSig << fund_sig;
    BOOST_REQUIRE_EQUAL(CreateAndProcessBlock({fund}, p2pk).vtx.size(), 2U);

    const bool saved{::g_reject_unverified_witness};
    for (size_t n{0}; n < cases.size(); ++n) {
        const Case& c{cases[n]};
        CMutableTransaction spend;
        spend.version = 2;
        spend.vin.emplace_back(COutPoint{fund.GetHash(), uint32_t(n)});
        spend.vout.emplace_back(9 * COIN, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey())));
        std::vector<unsigned char> sig;
        BOOST_REQUIRE(coinbaseKey.Sign(SignatureHash(c.script, spend, 0, SIGHASH_ALL, 10 * COIN, SigVersion::WITNESS_V0), sig));
        sig.push_back(SIGHASH_ALL);
        // The signature below the items for drops, on top of them for guarded
        auto& stack{spend.vin[0].scriptWitness.stack};
        stack = c.items;
        stack.insert(c.sig_on_top ? stack.end() : stack.begin(), sig);
        stack.emplace_back(c.script.begin(), c.script.end());
        const CTransactionRef tx{MakeTransactionRef(spend)};
        for (const bool on : {true, false}) {
            ::g_reject_unverified_witness = on;
            LOCK(cs_main);
            const MempoolAcceptResult res{AcceptToMemoryPool(m_node.chainman->ActiveChainstate(), tx, GetTime(), /*bypass_limits=*/false, /*test_accept=*/true)};
            if (on && c.refused) {
                BOOST_CHECK_MESSAGE(res.m_result_type == MempoolAcceptResult::ResultType::INVALID, "case " << n);
                BOOST_CHECK_EQUAL(res.m_state.GetRejectReason(), "txn-datacarrier-exceeded");
            } else {
                BOOST_CHECK_MESSAGE(res.m_result_type == MempoolAcceptResult::ResultType::VALID, "case " << n << " on=" << on << " " << res.m_state.ToString());
            }
        }
    }
    ::g_reject_unverified_witness = saved;
}

BOOST_FIXTURE_TEST_CASE(plumb_signature_push_is_no_key, TestChain100Setup)
{
    // A P2WSH script pushes thirteen 33 byte ECDSA signatures and checks each against a key from the
    // witness, recovered from the signature and the signature hash (BIP143 does not commit to a key
    // taken from the stack). Three checks with key and signature from the witness follow, which cap
    // the static count at three provable keys, so only the recount can see the pushes. Knots records
    // only keys and counts the thirteen pushes as unproven keys: 13 x 33 - 330 = 99 bytes. Plumb must
    // count the same, though it records the signatures for -rejectunverifiedwitness.
    const CScript p2pk{CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG};
    constexpr int PUSHES{13};
    // A strict DER signature of 33 bytes with its sighash byte: r one byte, s 25 bytes and low
    const auto der{[](unsigned char r, const std::vector<unsigned char>& s) {
        std::vector<unsigned char> sig{0x30, 30, 0x02, 1, r, 0x02, 25};
        sig.insert(sig.end(), s.begin(), s.end());
        sig.push_back(SIGHASH_ALL);
        return sig;
    }};
    std::vector<std::vector<unsigned char>> s_values;
    for (int i{0}; i < PUSHES; ++i) {
        std::vector<unsigned char> s(25, static_cast<unsigned char>(0x11 * (i + 1)));
        s[0] = 0x10 + i;
        s_values.push_back(s);
    }
    // The smallest r that is the x coordinate of a point, found by recovering a key with it
    unsigned char r{0};
    for (unsigned char x{1}; x < 0x80 && !r; ++x) {
        std::vector<unsigned char> compact(CPubKey::COMPACT_SIGNATURE_SIZE, 0);
        compact[0] = 27 + 4;
        compact[32] = x;
        compact[64] = 1;
        CPubKey q;
        if (q.RecoverCompact(uint256::ONE, compact)) r = x;
    }
    BOOST_REQUIRE(r);
    std::vector<std::vector<unsigned char>> pushes;
    CScript sigkey;
    for (const auto& s : s_values) {
        pushes.push_back(der(r, s));
        BOOST_REQUIRE_EQUAL(pushes.back().size(), 33U);
        sigkey << pushes.back() << OP_SWAP << OP_CHECKSIGVERIFY;
    }
    sigkey << OP_CHECKSIGVERIFY << OP_CHECKSIGVERIFY << OP_CHECKSIG;
    std::vector<CKey> tail_keys, real_keys;
    for (int i{0}; i < 3; ++i) tail_keys.push_back(GenerateRandomKey());
    // Its neighbor with real keys: thirteen keys pushed, each checked against a real signature
    CScript real;
    for (int i{0}; i < PUSHES; ++i) {
        real_keys.push_back(GenerateRandomKey());
        real << ToByteVector(real_keys.back().GetPubKey()) << (i + 1 < PUSHES ? OP_CHECKSIGVERIFY : OP_CHECKSIG);
    }

    const std::vector<CScript> scripts{p2pk, real, sigkey};
    CMutableTransaction fund;
    fund.version = 2;
    fund.vin.emplace_back(COutPoint{m_coinbase_txns[0]->GetHash(), 0});
    for (const CScript& script : scripts) fund.vout.emplace_back(10 * COIN, GetScriptForDestination(WitnessV0ScriptHash(script)));
    std::vector<unsigned char> fund_sig;
    BOOST_REQUIRE(coinbaseKey.Sign(SignatureHash(p2pk, fund, 0, SIGHASH_ALL, 0, SigVersion::BASE), fund_sig));
    fund_sig.push_back(SIGHASH_ALL);
    fund.vin[0].scriptSig << fund_sig;
    BOOST_REQUIRE_EQUAL(CreateAndProcessBlock({fund}, p2pk).vtx.size(), 2U);

    std::vector<CTransactionRef> spends;
    std::vector<std::vector<unsigned char>> recovered;
    for (size_t n{0}; n < scripts.size(); ++n) {
        const CScript& script{scripts[n]};
        CMutableTransaction spend;
        spend.version = 2;
        spend.vin.emplace_back(COutPoint{fund.GetHash(), uint32_t(n)});
        spend.vout.emplace_back(9 * COIN, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey())));
        const uint256 hash{SignatureHash(script, spend, 0, SIGHASH_ALL, 10 * COIN, SigVersion::WITNESS_V0)};
        const auto sign{[&](const CKey& key) {
            std::vector<unsigned char> sig;
            BOOST_REQUIRE(key.Sign(hash, sig));
            sig.push_back(SIGHASH_ALL);
            return sig;
        }};
        auto& stack{spend.vin[0].scriptWitness.stack};
        if (n == 0) {
            stack.push_back(sign(coinbaseKey));
        } else if (n == 1) {
            for (int i{PUSHES - 1}; i >= 0; --i) stack.push_back(sign(real_keys[i]));
        } else {
            // The bottom pair is the last check's
            for (int i{2}; i >= 0; --i) {
                stack.push_back(sign(tail_keys[i]));
                stack.push_back(ToByteVector(tail_keys[i].GetPubKey()));
            }
            // The key for each pushed signature, the first one's on top
            for (int i{PUSHES - 1}; i >= 0; --i) {
                std::vector<unsigned char> compact(CPubKey::COMPACT_SIGNATURE_SIZE, 0);
                compact[0] = 27 + 4;
                compact[32] = r;
                std::copy(s_values[i].begin(), s_values[i].end(), compact.end() - 25);
                CPubKey q;
                BOOST_REQUIRE(q.RecoverCompact(hash, compact));
                BOOST_REQUIRE(q.Verify(hash, std::vector<unsigned char>(pushes[i].begin(), pushes[i].end() - 1)));
                stack.push_back(ToByteVector(q));
                recovered.push_back(stack.back());
            }
        }
        stack.emplace_back(script.begin(), script.end());
        spends.push_back(MakeTransactionRef(spend));
    }

    LOCK(cs_main);
    // The recorder keeps the pushed signatures apart from the keys they verified against
    {
        TxValidationState state;
        PrecomputedTransactionData txdata;
        std::vector<VerifiedKeys> keys;
        BOOST_REQUIRE(CheckInputScripts(*spends[2], state, m_node.chainman->ActiveChainstate().CoinsTip(), STANDARD_SCRIPT_VERIFY_FLAGS, true, false, txdata, m_node.chainman->m_validation_cache, nullptr, {}, &keys));
        BOOST_REQUIRE_EQUAL(keys.size(), 1U);
        BOOST_CHECK_EQUAL(keys[0].keys.size(), size_t{PUSHES + 3});
        BOOST_CHECK_EQUAL(keys[0].sigs.size(), size_t{PUSHES + 3});
        for (const auto& q : recovered) BOOST_CHECK(keys[0].keys.contains(q));
        for (const auto& push : pushes) {
            BOOST_CHECK(keys[0].sigs.contains(push));
            BOOST_CHECK(!keys[0].keys.contains(push));
        }
    }

    // Refused for the unproven-key rule wherever it runs, whatever -rejectunverifiedwitness is: every
    // witness item is a key or a signature that verified. The neighbors are accepted.
    const bool saved{::g_reject_fake_multisig}, saved_witness{::g_reject_unverified_witness};
    for (const auto& [fake_multisig, unverified_witness] : {std::pair{true, true}, {true, false}, {false, true}, {false, false}}) {
        ::g_reject_fake_multisig = fake_multisig;
        ::g_reject_unverified_witness = unverified_witness;
        for (size_t n{0}; n < spends.size(); ++n) {
            const MempoolAcceptResult res{AcceptToMemoryPool(m_node.chainman->ActiveChainstate(), spends[n], GetTime(), /*bypass_limits=*/false, /*test_accept=*/true)};
            if (n == 2 && fake_multisig) {
                BOOST_CHECK_MESSAGE(res.m_result_type == MempoolAcceptResult::ResultType::INVALID, "case " << n << " " << fake_multisig << unverified_witness);
                BOOST_CHECK_EQUAL(res.m_state.GetRejectReason(), "txn-datacarrier-exceeded");
            } else {
                BOOST_CHECK_MESSAGE(res.m_result_type == MempoolAcceptResult::ResultType::VALID, "case " << n << " " << fake_multisig << unverified_witness << " " << res.m_state.ToString());
            }
        }
    }
    ::g_reject_fake_multisig = saved;
    ::g_reject_unverified_witness = saved_witness;
}

BOOST_AUTO_TEST_SUITE_END()
