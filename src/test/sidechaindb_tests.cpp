// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <stdlib.h>

#include "chainparams.h"
#include "consensus/validation.h"
#include "core_io.h"
#include "main.h"
#include "miner.h"
#include "script/sigcache.h"
#include "sidechaindb.h"
#include "uint256.h"
#include "utilstrencodings.h"

#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>

//! KeyID for testing
// mx3PT9t2kzCFgAURR9HeK6B5wN8egReUxY
// cN5CqwXiaNWhNhx3oBQtA8iLjThSKxyZjfmieTsyMpG6NnHBzR7J
static const std::string testkey = "b5437dc6a4e5da5597548cf87db009237d286636";

BOOST_FIXTURE_TEST_SUITE(sidechaindb_tests, TestChain100Setup)

std::vector<CMutableTransaction> CreateDepositTransactions(SidechainNumber sidechain, int count)
{
    std::vector<CMutableTransaction> depositTransactions;
    depositTransactions.resize(count);
    for (int i = 0; i < count; i++) {
        // Random enough output value
        int rand = (std::rand() % 50) + i;

        depositTransactions[i].vout.resize(1);
        depositTransactions[i].vout[0].nValue = rand*CENT;
        depositTransactions[i].vout[0].scriptPubKey = CScript() << sidechain << ToByteVector(testkey) << OP_NOP4;
    }
    return depositTransactions;
}

CBlock CreateBlock(const std::vector<CMutableTransaction>& txns, const CScript& scriptPubKey)
{
    const CChainParams& chainparams = Params();
    std::unique_ptr<CBlockTemplate> pblocktemplate = BlockAssembler(chainparams).CreateNewBlock(scriptPubKey);
    CBlock& block = pblocktemplate->block;

    // Replace mempool-selected txns with just coinbase plus passed-in txns:
    block.vtx.resize(1);
    for (const CMutableTransaction& tx : txns) {
        block.vtx.push_back(MakeTransactionRef(tx));
    }

    // IncrementExtraNonce creates a valid coinbase and merkleRoot
    unsigned int extraNonce = 0;
    IncrementExtraNonce(&block, chainActive.Tip(), extraNonce);

    while (!CheckProofOfWork(block.GetHash(), block.nBits, chainparams.GetConsensus()))
        ++block.nNonce;

    CBlock result = block;
    return result;
}

bool ProcessBlock(const CBlock &block)
{
    const CChainParams& chainparams = Params();
    return ProcessNewBlock(chainparams, &block, true, NULL, NULL);
}

// TODO Make unit test
//// Check that a block will be invalid if it tries to spend WT^ with 0 workscore
//std::vector<CMutableTransaction> vWT;
//vWT.push_back(wtx);
//CBlock spendBlock = CreateBlock(vWT, scriptPubKey);
//int heightPreSpend = chainActive.Height();
//ProcessBlock(spendBlock);
//BOOST_CHECK(chainActive.Height() == heightPreSpend);

BOOST_AUTO_TEST_CASE(sidechaindb_blockchain)
{
    // Test SCDB with blockchain
    SidechainDB scdb;

    const Sidechain &sidechainTest = ValidSidechains[SIDECHAIN_TEST];
    CScript scriptPubKey = CScript() <<  ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    // Create deposit
    CMutableTransaction depositTX;
    depositTX.vin.resize(1);
    depositTX.vin[0].prevout.hash = coinbaseTxns[0].GetHash();
    depositTX.vin[0].prevout.n = 0;
    depositTX.vout.resize(1);
    depositTX.vout[0].nValue = 50 * CENT;
    // Payment to test key
    CKeyID depositKey;
    depositKey.SetHex(testkey);
    depositTX.vout[0].scriptPubKey = CScript() << SIDECHAIN_TEST << ToByteVector(depositKey) << OP_NOP4;

    std::vector<unsigned char> vchSig;
    uint256 hash = SignatureHash(scriptPubKey, depositTX, 0, SIGHASH_ALL, 0, SIGVERSION_BASE);
    coinbaseKey.Sign(hash, vchSig);
    vchSig.push_back((unsigned char)SIGHASH_ALL);
    depositTX.vin[0].scriptSig << vchSig;

    // Skip to the begining of the next tau
    for (int i = 0; i < 199; i++) {
        std::vector<CMutableTransaction> noTxns;
        CBlock b = CreateAndProcessBlock(noTxns, scriptPubKey);
        coinbaseTxns.push_back(*b.vtx[0]);
    }

    // Add deposit to blockchain
    std::vector<CMutableTransaction> vDeposit;
    vDeposit.push_back(depositTX);
    CBlock depositBlock = CreateBlock(vDeposit, scriptPubKey);
    int preDepositHeight = chainActive.Height();
    ProcessBlock(depositBlock);
    BOOST_CHECK(chainActive.Height() == (preDepositHeight + 1));

    // Create WT^ (try to spend the deposit created in the previous block)
    CMutableTransaction wtx;
    wtx.vin.resize(1);
    wtx.vin[0].prevout.hash = depositTX.GetHash();
    wtx.vin[0].prevout.n = 0;
    wtx.vout.resize(1);
    wtx.vout[0].nValue = 42 * CENT;
    wtx.vout[0].scriptPubKey = CScript() << OP_DUP << OP_HASH160 << ToByteVector(testkey) << OP_CHECKSIGVERIFY;

    // Manully submit WT^ to SCDB for tracking and verification
    BOOST_CHECK(scdb.AddWTJoin(SIDECHAIN_TEST, wtx));
    // Pass waiting period
    for (int i = 0; i < sidechainTest.nWaitPeriod; i++) {
        std::vector<CMutableTransaction> noTxns;
        CBlock b = CreateAndProcessBlock(noTxns, scriptPubKey);
        coinbaseTxns.push_back(*b.vtx[0]);
    }

    std::vector<SidechainDeposit> dep = scdb.GetDeposits(sidechainTest.nSidechain);

    // Pass verification period
    for (int i = 0; i < sidechainTest.nVerificationPeriod; i++) {
        std::vector<CMutableTransaction> noTxns;
        CBlock b = CreateAndProcessBlock(noTxns, scriptPubKey);
        coinbaseTxns.push_back(*b.vtx[0]);
    }

    // TODO BOOST_CHECK(scdb.CheckWorkScore(SIDECHAIN_TEST, wtx.GetHash()));
}

BOOST_AUTO_TEST_CASE(sidechaindb_isolated)
{
    std::vector<CMutableTransaction> vTestDeposit = CreateDepositTransactions(SIDECHAIN_TEST, 1);
    std::vector<CMutableTransaction> vHivemindDeposit = CreateDepositTransactions(SIDECHAIN_HIVEMIND, 1);
    std::vector<CMutableTransaction> vWimbleDeposit = CreateDepositTransactions(SIDECHAIN_WIMBLE, 1);

    // Test SidechainDB without blocks
    SidechainDB scdb;

    const Sidechain& test = ValidSidechains[SIDECHAIN_TEST];
    const Sidechain& hivemind = ValidSidechains[SIDECHAIN_HIVEMIND];
    const Sidechain& wimble = ValidSidechains[SIDECHAIN_WIMBLE];

    int blocksLeft0 = test.nWaitPeriod + test.nVerificationPeriod;
    int blocksLeft1 = hivemind.nWaitPeriod + hivemind.nVerificationPeriod;
    int blocksLeft2 = wimble.nWaitPeriod + wimble.nVerificationPeriod;

    int score0, score1;
    score0 = score1 = 0;
    for (int i = 0; i <= 300; i++) {
        scdb.Update(SIDECHAIN_TEST, blocksLeft0, score0, vTestDeposit[0].GetHash());
        scdb.Update(SIDECHAIN_HIVEMIND, blocksLeft1, score1, vHivemindDeposit[0].GetHash());
        scdb.Update(SIDECHAIN_WIMBLE, blocksLeft2, 0, vWimbleDeposit[0].GetHash());

        score0++;

        if (i % 2 == 0)
            score1++;

        blocksLeft0--;
        blocksLeft1--;
        blocksLeft2--;
    }

    // WT^ 0 should pass with valid workscore (200/100)
    BOOST_CHECK(scdb.CheckWorkScore(SIDECHAIN_TEST, vTestDeposit[0].GetHash()));
    // WT^ 1 should fail with unsatisfied workscore (100/200)
    BOOST_CHECK(!scdb.CheckWorkScore(SIDECHAIN_HIVEMIND, vHivemindDeposit[0].GetHash()));
    // WT^ 2 should fail with unsatisfied workscore (0/200)
    BOOST_CHECK(!scdb.CheckWorkScore(SIDECHAIN_WIMBLE, vWimbleDeposit[0].GetHash()));
}

BOOST_AUTO_TEST_CASE(sidechaindb_EmptyStateScript)
{
    // Test empty SCDB
    const Sidechain& sidechainTest = ValidSidechains[SIDECHAIN_TEST];
    CScript scriptEmptyExpected = CScript();
    SidechainDB scdbEmpty;
    BOOST_CHECK(scriptEmptyExpected == scdbEmpty.CreateStateScript(sidechainTest.GetTau() - 1));
}

BOOST_AUTO_TEST_CASE(sidechaindb_PopulatedStateScript)
{
    const Sidechain& sidechainTest = ValidSidechains[SIDECHAIN_TEST];
    const Sidechain& sidechainHivemind = ValidSidechains[SIDECHAIN_HIVEMIND];
    const Sidechain& sidechainWimble = ValidSidechains[SIDECHAIN_WIMBLE];

    std::vector<CMutableTransaction> vTestDeposit = CreateDepositTransactions(SIDECHAIN_TEST, 3);
    std::vector<CMutableTransaction> vHivemindDeposit = CreateDepositTransactions(SIDECHAIN_HIVEMIND, 3);
    std::vector<CMutableTransaction> vWimbleDeposit = CreateDepositTransactions(SIDECHAIN_WIMBLE, 3);

    // Test populated (but not full) SCDB test
    CScript scriptPopulatedExpected;
    scriptPopulatedExpected << OP_RETURN << SCOP_VERSION << SCOP_VERSION_DELIM
                    << SCOP_VERIFY << SCOP_SC_DELIM
                    << SCOP_VERIFY << SCOP_SC_DELIM
                    << SCOP_VERIFY;
    SidechainDB scdbPopulated;
    // Pass waiting period and then submit verification
    for (uint16_t i = 0; i < sidechainTest.nWaitPeriod; i++) {
        scdbPopulated.Update(SIDECHAIN_TEST, sidechainTest.GetTau() - i, 0, vTestDeposit[0].GetHash());
    }
    uint16_t nVoteHeight = sidechainTest.GetTau() - sidechainTest.nWaitPeriod;
    scdbPopulated.Update(SIDECHAIN_TEST, nVoteHeight, 1, vTestDeposit[0].GetHash());

    // Pass waiting period and then submit verification
    for (uint16_t i = 0; i < sidechainHivemind.nWaitPeriod; i++) {
        scdbPopulated.Update(SIDECHAIN_HIVEMIND, sidechainHivemind.GetTau() - i, 0, vHivemindDeposit[0].GetHash());
    }
    nVoteHeight = sidechainHivemind.GetTau() - sidechainHivemind.nWaitPeriod;
    scdbPopulated.Update(SIDECHAIN_HIVEMIND, nVoteHeight, 1, vHivemindDeposit[0].GetHash());

    // Pass waiting period and then submit verification
    for (uint16_t i = 0; i < sidechainWimble.nWaitPeriod; i++) {
        scdbPopulated.Update(SIDECHAIN_WIMBLE, sidechainWimble.GetTau() - i, 0, vWimbleDeposit[0].GetHash());
    }
    nVoteHeight = sidechainWimble.GetTau() - sidechainWimble.nWaitPeriod;
    scdbPopulated.Update(SIDECHAIN_WIMBLE, nVoteHeight, 1, vWimbleDeposit[0].GetHash());
    BOOST_CHECK(scriptPopulatedExpected == scdbPopulated.CreateStateScript(sidechainTest.GetTau() - 1));
}

BOOST_AUTO_TEST_CASE(sidechaindb_FullStateScript)
{
    const Sidechain& sidechainTest = ValidSidechains[SIDECHAIN_TEST];
    const Sidechain& sidechainHivemind = ValidSidechains[SIDECHAIN_HIVEMIND];
    const Sidechain& sidechainWimble = ValidSidechains[SIDECHAIN_WIMBLE];

    std::vector<CMutableTransaction> vTestDeposit = CreateDepositTransactions(SIDECHAIN_TEST, 3);
    std::vector<CMutableTransaction> vHivemindDeposit = CreateDepositTransactions(SIDECHAIN_HIVEMIND, 3);
    std::vector<CMutableTransaction> vWimbleDeposit = CreateDepositTransactions(SIDECHAIN_WIMBLE, 3);
    // Test Full SCDB

    CScript scriptFullExpected;
    scriptFullExpected << OP_RETURN << SCOP_VERSION << SCOP_VERSION_DELIM
               << SCOP_VERIFY << SCOP_WT_DELIM << SCOP_REJECT << SCOP_WT_DELIM << SCOP_REJECT
               << SCOP_SC_DELIM
               << SCOP_VERIFY << SCOP_WT_DELIM << SCOP_REJECT << SCOP_WT_DELIM << SCOP_REJECT
               << SCOP_SC_DELIM
               << SCOP_VERIFY << SCOP_WT_DELIM << SCOP_REJECT << SCOP_WT_DELIM << SCOP_REJECT;

    SidechainDB scdbFull;
    // Pass waiting period and then submit verification
    for (uint16_t i = 0; i < sidechainTest.nWaitPeriod; i++) {
        scdbFull.Update(SIDECHAIN_TEST, sidechainTest.GetTau() - i, 0, vTestDeposit[0].GetHash());
    }
    int nVoteHeight = sidechainTest.GetTau() - sidechainTest.nWaitPeriod;
    scdbFull.Update(SIDECHAIN_TEST, nVoteHeight, 1, vTestDeposit[0].GetHash());
    scdbFull.Update(SIDECHAIN_TEST, nVoteHeight - 1, 0, vTestDeposit[1].GetHash());
    scdbFull.Update(SIDECHAIN_TEST, nVoteHeight - 2, 0, vTestDeposit[2].GetHash());

    // Pass waiting period and then submit verification
    for (uint16_t i = 0; i < sidechainHivemind.nWaitPeriod; i++) {
        scdbFull.Update(SIDECHAIN_HIVEMIND, sidechainHivemind.GetTau() - i, 0, vHivemindDeposit[0].GetHash());
    }
    nVoteHeight = sidechainHivemind.GetTau() - sidechainHivemind.nWaitPeriod;
    scdbFull.Update(SIDECHAIN_HIVEMIND, nVoteHeight, 1, vHivemindDeposit[0].GetHash());
    scdbFull.Update(SIDECHAIN_HIVEMIND, nVoteHeight - 1, 0, vHivemindDeposit[1].GetHash());
    scdbFull.Update(SIDECHAIN_HIVEMIND, nVoteHeight - 2, 0, vHivemindDeposit[2].GetHash());

    // Pass waiting period and then submit verification
    for (uint16_t i = 0; i < sidechainWimble.nWaitPeriod; i++) {
        scdbFull.Update(SIDECHAIN_WIMBLE, sidechainWimble.GetTau() - i, 0, vWimbleDeposit[0].GetHash());
    }
    nVoteHeight = sidechainWimble.GetTau() - sidechainWimble.nWaitPeriod;
    scdbFull.Update(SIDECHAIN_WIMBLE, nVoteHeight, 1, vWimbleDeposit[0].GetHash());
    scdbFull.Update(SIDECHAIN_WIMBLE, nVoteHeight - 1, 0, vWimbleDeposit[1].GetHash());
    scdbFull.Update(SIDECHAIN_WIMBLE, nVoteHeight - 2, 0, vWimbleDeposit[2].GetHash());
    BOOST_CHECK(scriptFullExpected == scdbFull.CreateStateScript(sidechainTest.GetTau() - 1));
}

BOOST_AUTO_TEST_CASE(sidechaindb_CountStateScript)
{
    const Sidechain& sidechainTest = ValidSidechains[SIDECHAIN_TEST];
    const Sidechain& sidechainHivemind = ValidSidechains[SIDECHAIN_HIVEMIND];
    const Sidechain& sidechainWimble = ValidSidechains[SIDECHAIN_WIMBLE];

    std::vector<CMutableTransaction> vTestDeposit = CreateDepositTransactions(SIDECHAIN_TEST, 3);
    std::vector<CMutableTransaction> vHivemindDeposit = CreateDepositTransactions(SIDECHAIN_HIVEMIND, 3);
    std::vector<CMutableTransaction> vWimbleDeposit = CreateDepositTransactions(SIDECHAIN_WIMBLE, 3);

    // Test with different number of WT^s per sidechain
    CScript scriptWTCountExpected;
    scriptWTCountExpected << OP_RETURN << SCOP_VERSION << SCOP_VERSION_DELIM
                          << SCOP_VERIFY
                          << SCOP_SC_DELIM
                          << SCOP_REJECT << SCOP_WT_DELIM << SCOP_VERIFY
                          << SCOP_SC_DELIM
                          << SCOP_REJECT << SCOP_WT_DELIM << SCOP_VERIFY << SCOP_WT_DELIM << SCOP_REJECT;
    SidechainDB scdbCount;
    // Pass waiting period and then submit verification
    for (uint16_t i = 0; i < sidechainTest.nWaitPeriod; i++) {
        scdbCount.Update(SIDECHAIN_TEST, sidechainTest.GetTau() - i, 0, vTestDeposit[0].GetHash());
    }
    int nVoteHeight = sidechainTest.GetTau() - sidechainTest.nWaitPeriod;
    scdbCount.Update(SIDECHAIN_TEST, nVoteHeight, 1, vTestDeposit[0].GetHash());

    // Pass waiting period and then submit verification
    for (uint16_t i = 0; i < sidechainHivemind.nWaitPeriod; i++) {
        scdbCount.Update(SIDECHAIN_HIVEMIND, sidechainHivemind.GetTau() - i, 0, vHivemindDeposit[0].GetHash());
    }
    nVoteHeight = sidechainHivemind.GetTau() - sidechainHivemind.nWaitPeriod;
    scdbCount.Update(SIDECHAIN_HIVEMIND, nVoteHeight, 0, vHivemindDeposit[0].GetHash());
    scdbCount.Update(SIDECHAIN_HIVEMIND, nVoteHeight - 1, 1, vHivemindDeposit[1].GetHash());


    // Pass waiting period and then submit verification
    for (uint16_t i = 0; i < sidechainWimble.nWaitPeriod; i++) {
        scdbCount.Update(SIDECHAIN_WIMBLE, sidechainWimble.GetTau() - i, 0, vWimbleDeposit[0].GetHash());
    }
    nVoteHeight = sidechainWimble.GetTau() - sidechainWimble.nWaitPeriod;
    scdbCount.Update(SIDECHAIN_WIMBLE, nVoteHeight, 0, vWimbleDeposit[0].GetHash());
    scdbCount.Update(SIDECHAIN_WIMBLE, nVoteHeight, 1, vWimbleDeposit[1].GetHash());
    scdbCount.Update(SIDECHAIN_WIMBLE, nVoteHeight, 0, vWimbleDeposit[2].GetHash());
    BOOST_CHECK(scriptWTCountExpected == scdbCount.CreateStateScript(sidechainTest.GetTau() - 1));
}

BOOST_AUTO_TEST_CASE(sidechaindb_PositionStateScript)
{
    // Verify that state scripts created based on known SidechainDB
    // state examples are formatted as expected
    const Sidechain& sidechainTest = ValidSidechains[SIDECHAIN_TEST];
    const Sidechain& sidechainHivemind = ValidSidechains[SIDECHAIN_HIVEMIND];
    const Sidechain& sidechainWimble = ValidSidechains[SIDECHAIN_WIMBLE];

    std::vector<CMutableTransaction> vTestDeposit = CreateDepositTransactions(SIDECHAIN_TEST, 3);
    std::vector<CMutableTransaction> vHivemindDeposit = CreateDepositTransactions(SIDECHAIN_HIVEMIND, 3);
    std::vector<CMutableTransaction> vWimbleDeposit = CreateDepositTransactions(SIDECHAIN_WIMBLE, 3);

    // Test WT^ in different position for each sidechain
    CScript scriptWTPositionExpected;
    scriptWTPositionExpected << OP_RETURN << SCOP_VERSION << SCOP_VERSION_DELIM
                             << SCOP_VERIFY << SCOP_WT_DELIM << SCOP_REJECT << SCOP_WT_DELIM << SCOP_REJECT
                             << SCOP_SC_DELIM
                             << SCOP_REJECT << SCOP_WT_DELIM << SCOP_VERIFY << SCOP_WT_DELIM << SCOP_REJECT
                             << SCOP_SC_DELIM
                             << SCOP_REJECT << SCOP_WT_DELIM << SCOP_REJECT << SCOP_WT_DELIM << SCOP_VERIFY;
    SidechainDB scdbPosition;
    // Pass waiting period and then submit verification
    for (uint16_t i = 0; i < sidechainTest.nWaitPeriod; i++) {
        scdbPosition.Update(SIDECHAIN_TEST, sidechainTest.GetTau() - i, 0, vTestDeposit[0].GetHash());
    }
    int nVoteHeight = sidechainTest.GetTau() - sidechainTest.nWaitPeriod;
    scdbPosition.Update(SIDECHAIN_TEST, nVoteHeight, 1, vTestDeposit[0].GetHash());
    scdbPosition.Update(SIDECHAIN_TEST, nVoteHeight - 1, 0, vTestDeposit[1].GetHash());
    scdbPosition.Update(SIDECHAIN_TEST, nVoteHeight - 2, 0, vTestDeposit[2].GetHash());

    // Pass waiting period and then submit verification
    for (uint16_t i = 0; i < sidechainHivemind.nWaitPeriod; i++) {
        scdbPosition.Update(SIDECHAIN_HIVEMIND, sidechainHivemind.GetTau() - i, 0, vHivemindDeposit[0].GetHash());
    }
    nVoteHeight = sidechainHivemind.GetTau() - sidechainHivemind.nWaitPeriod;
    scdbPosition.Update(SIDECHAIN_HIVEMIND, nVoteHeight, 0, vHivemindDeposit[0].GetHash());
    scdbPosition.Update(SIDECHAIN_HIVEMIND, nVoteHeight - 1, 1, vHivemindDeposit[1].GetHash());
    scdbPosition.Update(SIDECHAIN_HIVEMIND, nVoteHeight - 2, 0, vHivemindDeposit[2].GetHash());

    // Pass waiting period and then submit verification
    for (uint16_t i = 0; i < sidechainWimble.nWaitPeriod; i++) {
        scdbPosition.Update(SIDECHAIN_WIMBLE, sidechainWimble.GetTau() - i, 0, vWimbleDeposit[0].GetHash());
    }
    nVoteHeight = sidechainWimble.GetTau() - sidechainWimble.nWaitPeriod;
    scdbPosition.Update(SIDECHAIN_WIMBLE, nVoteHeight, 0, vWimbleDeposit[0].GetHash());
    scdbPosition.Update(SIDECHAIN_WIMBLE, nVoteHeight - 1, 0, vWimbleDeposit[1].GetHash());
    scdbPosition.Update(SIDECHAIN_WIMBLE, nVoteHeight - 2, 1, vWimbleDeposit[2].GetHash());
    BOOST_CHECK(scriptWTPositionExpected == scdbPosition.CreateStateScript(sidechainTest.GetTau() - 1));
}

//BOOST_AUTO_TEST_CASE(sidechaindb_Update)
//{
//    // TODO

//    // Valid update (upvotes), should work

//    // Invalid increment, should be rejected

//    // Invalid sidechain, should be rejected

//    // Valid update (downvotes), should work
//}

BOOST_AUTO_TEST_SUITE_END()
