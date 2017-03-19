// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAINDB_H
#define BITCOIN_SIDECHAINDB_H

#include "pubkey.h"
#include "uint256.h"

#include <string>
#include <vector>

class CScript;
class CTransaction;

/**
 * Sidechain Keys
 */
//! KeyID for testing
// 4LQSw2aWn3EuC52va1JLzCDAHud2VaougL
static const char* const SIDECHAIN_TEST_KEY = "09c1fbf0ad3047fb825e0bc5911528596b7d7f49";
static const char* const SIDECHAIN_TEST_PRIV = "cQMQ99mA5Xi2Hm9YM3WmB2JcJai3tzGupuFb5b7HWiwNgTKoaFr5";
static const char* const SIDECHAIN_TEST_SCRIPT_HEX = "76a914497f7d6b59281591c50b5e82fb4730adf0fbc10988ac";

struct Sidechain {
    uint8_t nSidechain;
    uint16_t nWaitPeriod;
    uint16_t nVerificationPeriod;
    uint16_t nMinWorkScore;

    std::string ToString() const;
    std::string GetSidechainName() const;
    uint16_t GetTau() const;
};

struct SidechainDeposit {
    uint8_t nSidechain;
    CKeyID keyID;
    std::string hex;

    std::string ToString() const;
    bool operator==(const SidechainDeposit& a) const;
};

struct SidechainVerification {
    uint8_t nSidechain;
    uint16_t nBlocksLeft;
    uint16_t nWorkScore;
    uint256 wtxid;

    std::string ToString() const;
};

enum SidechainNumber {
    SIDECHAIN_TEST = 0,
    SIDECHAIN_HIVEMIND = 1,
    SIDECHAIN_WIMBLE = 2
};

static const Sidechain ValidSidechains[] =
{
    // {nSidechain, nWaitPeriod, nVerificationPeriod, nMinWorkScore}
    {SIDECHAIN_TEST, 100, 200, 100},
    {SIDECHAIN_HIVEMIND, 200, 400, 200},
    {SIDECHAIN_WIMBLE, 200, 400, 200},
};

//! Max number of WT^(s) per sidechain during tau
static const int SIDECHAIN_MAX_WT = 3;

bool SidechainNumberValid(uint8_t nSidechain);

class SidechainDB
{
public:
    SidechainDB();

    /** Add a new WT^ to the database */
    bool AddWTJoin(uint8_t nSidechain, CTransaction wtx);

    /** Add deposit to cache */
    void AddDeposit(const CTransaction &tx);

    /** Return verified WT^ for nSidechain if one exists */
    CTransaction GetWTJoinTx(uint8_t nSidechain, int nHeight) const;

    /** Create a script with OP_RETURN data representing the DB state */
    CScript CreateStateScript(int nHeight) const;

    /** Get all of the deposits this tau for nSidechain. */
    std::vector<SidechainDeposit> GetDeposits(uint8_t nSidechain) const;

    /** Return true if the deposit is cached */
    bool HaveDepositCached(const SidechainDeposit& deposit) const;

    /** Return true if the full WT^ CTransaction is cached */
    bool HaveWTJoinCached(uint256 wtxid) const;

    /** Update DB state with new state script */
    bool Update(const CTransaction& tx);

private:
    /** Sidechain state database */
    std::vector<std::vector<SidechainVerification>> SCDB;

    /** The DB vector stores verifications, which contain as one member
     *  the hash / txid of the WT^ being verified. This vector stores the
     *  full transaction(s) so that they can be looked up as needed */
    std::vector<CTransaction> vWTJoinCache;

    /** Track deposits created during this tau */
    std::vector<SidechainDeposit> vDepositCache;

    /** Is there anything being tracked by the SCDB? */
    bool HasState() const;

    /** Get the latest scores for nSidechain's WT^(s) */
    std::vector<SidechainVerification> GetLastVerifications(uint8_t nSidechain) const;

    /** Return height of the end of the previous / begining of this tau */
    int GetLastTauHeight(const Sidechain &sidechain, int nHeight) const;

    /** Update the DB state */
    bool Update(uint8_t nSidechain, uint16_t nBlocks, uint16_t nScore, uint256 wtxid, bool fJustCheck = false);

    /** Read state script and update SCDB */
    bool ApplyStateScript(const CScript& state, const std::vector<std::vector<SidechainVerification>>& vScores, bool fJustCheck = false);
};

#endif // BITCOIN_SIDECHAINDB_H

