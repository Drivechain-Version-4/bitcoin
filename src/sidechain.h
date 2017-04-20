// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAIN_H
#define BITCOIN_SIDECHAIN_H

#include "pubkey.h"

#include <vector>

/**
 * Sidechain Keys
 */
//! KeyID for testing
// 4LQSw2aWn3EuC52va1JLzCDAHud2VaougL
static const char* const SIDECHAIN_TEST_KEY = "09c1fbf0ad3047fb825e0bc5911528596b7d7f49";
static const char* const SIDECHAIN_TEST_PRIV = "cQMQ99mA5Xi2Hm9YM3WmB2JcJai3tzGupuFb5b7HWiwNgTKoaFr5";
static const char* const SIDECHAIN_TEST_SCRIPT_HEX = "76a914497f7d6b59281591c50b5e82fb4730adf0fbc10988ac";

//! Max number of WT^(s) per sidechain during tau
static const int SIDECHAIN_MAX_WT = 3;

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

struct SidechainWTJoinState {
    uint8_t nSidechain;
    uint16_t nBlocksLeft;
    uint16_t nWorkScore;
    uint256 wtxid;

    std::string ToString() const;

    // For hash calculation
    ADD_SERIALIZE_METHODS

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(nSidechain);
        READWRITE(nBlocksLeft);
        READWRITE(nWorkScore);
        READWRITE(wtxid);
    }
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

bool SidechainNumberValid(uint8_t nSidechain);

#endif // BITCOIN_SIDECHAIN_H
