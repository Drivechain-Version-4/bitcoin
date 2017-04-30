// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAINDB_H
#define BITCOIN_SIDECHAINDB_H

#include "uint256.h"

#include <vector>

class CScript;
class CTransaction;

struct Sidechain;
struct SidechainDeposit;
struct SidechainWTJoinState;

class SidechainDB
{
public:
    SidechainDB();

    /** Add deposit to cache */
    void AddDeposits(const std::vector<CTransaction>& vtx);

    /** Add a new WT^ to the database */
    bool AddWTJoin(uint8_t nSidechain, const CTransaction& tx);

    /** Return true if the deposit is cached */
    bool HaveDepositCached(const SidechainDeposit& deposit) const;

    /** Return true if the full WT^ CTransaction is cached */
    bool HaveWTJoinCached(uint256 wtxid) const;

    /** Look for state update scripts in coinbase transaction */
    bool Update(const CTransaction& tx);

    /** Update the DB state (public for unit tests) */
    bool Update(uint8_t nSidechain, uint16_t nBlocks, uint16_t nScore, uint256 wtxid, bool fJustCheck = false);

    /** Return vector of deposits this tau for nSidechain. */
    std::vector<SidechainDeposit> GetDeposits(uint8_t nSidechain) const;

    /** Return B-WT^ for sidechain if one has been verified */
    CTransaction GetWTJoinTx(uint8_t nSidechain, int nHeight) const;

    /** Create a script with OP_RETURN data representing the DB state */
    CScript CreateStateScript(int nHeight) const;

    /** Return serialization hash of SCDB latest verification(s) */
    uint256 CreateSCDBHash() const;

    /** Check SCDB WT^ verification status */
    bool CheckWorkScore(const uint8_t& nSidechain, const uint256& wtxid) const;

    /** Print SCDB WT^ verification status */
    std::string ToString() const;

private:
    /** Sidechain state database */
    std::vector<std::vector<SidechainWTJoinState>> SCDB;

    /** The DB vector stores verifications, which contain as one member
     *  the hash / txid of the WT^ being verified. This vector stores the
     *  full transaction(s) so that they can be looked up as needed */
    std::vector<CTransaction> vWTJoinCache;

    /** Track deposits created during this tau */
    std::vector<SidechainDeposit> vDepositCache;

    /** Is there anything being tracked by the SCDB? */
    bool HasState() const;

    /** Return height of the end of the previous / begining of this tau */
    int GetLastTauHeight(const Sidechain &sidechain, int nHeight) const;

    /** Get the latest scores for nSidechain's WT^(s) */
    std::vector<SidechainWTJoinState> GetState(uint8_t nSidechain) const;

    /** Read state script and update SCDB */
    bool ApplyStateScript(const CScript& state, const std::vector<std::vector<SidechainWTJoinState>>& vScores, bool fJustCheck = false);
};

#endif // BITCOIN_SIDECHAINDB_H

