﻿// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sidechaindb.h"

#include "base58.h"
#include "chain.h"
#include "core_io.h"
#include "script/script.h"
#include "sidechain.h"
#include "uint256.h"
#include "util.h"
#include "utilstrencodings.h"
#include "wallet/wallet.h"

SidechainDB::SidechainDB()
{
    SCDB.resize(ARRAYLEN(ValidSidechains));
}

void SidechainDB::AddDeposits(const std::vector<CTransaction>& vtx)
{
    std::vector<SidechainDeposit> vDeposit;
    for (const CTransaction& tx : vtx) {
        // Create sidechain deposit objects from transaction outputs
        for (const CTxOut& out : tx.vout) {
            const CScript &scriptPubKey = out.scriptPubKey;

            // scriptPubKey must contain keyID
            if (scriptPubKey.size() < sizeof(uint160))
                continue;
            if (scriptPubKey.front() != OP_RETURN)
                continue;

            uint8_t nSidechain = (unsigned int)scriptPubKey[1];
            if (!SidechainNumberValid(nSidechain))
                continue;

            std::vector<unsigned char> vch;
            opcodetype opcode;
            CScript::const_iterator pkey = scriptPubKey.begin() + 2;
            if (!scriptPubKey.GetOp2(pkey, opcode, &vch))
                continue;
            if (vch.size() != sizeof(uint160))
                continue;

            CKeyID keyID = CKeyID(uint160(vch));
            if (keyID.IsNull())
                continue;

            SidechainDeposit deposit;
            deposit.hex = EncodeHexTx(tx);
            deposit.keyID = keyID;
            deposit.nSidechain = nSidechain;

            vDeposit.push_back(deposit);
        }
    }

    // Add deposits to cache
    for (const SidechainDeposit& d : vDeposit) {
        if (!HaveDepositCached(d))
            vDepositCache.push_back(d);
    }
}

bool SidechainDB::AddWTJoin(uint8_t nSidechain, const CTransaction& tx)
{
    if (vWTJoinCache.size() >= SIDECHAIN_MAX_WT)
        return false;
    if (!SidechainNumberValid(nSidechain))
        return false;
    if (HaveWTJoinCached(tx.GetHash()))
        return false;

    const Sidechain& s = ValidSidechains[nSidechain];
    if (Update(nSidechain, s.GetTau(), 0, tx.GetHash())) {
        vWTJoinCache.push_back(tx);
        return true;
    }
    return false;
}

bool SidechainDB::HaveDepositCached(const SidechainDeposit &deposit) const
{
    for (const SidechainDeposit& d : vDepositCache) {
        if (d == deposit)
            return true;
    }
    return false;
}

bool SidechainDB::HaveWTJoinCached(const uint256& wtxid) const
{
    for (const CTransaction& tx : vWTJoinCache) {
        if (tx.GetHash() == wtxid)
            return true;
    }
    return false;
}

std::vector<SidechainDeposit> SidechainDB::GetDeposits(uint8_t nSidechain) const
{
    std::vector<SidechainDeposit> vSidechainDeposit;
    for (size_t i = 0; i < vDepositCache.size(); i++) {
        if (vDepositCache[i].nSidechain == nSidechain)
            vSidechainDeposit.push_back(vDepositCache[i]);
    }
    return vSidechainDeposit;
}

CTransaction SidechainDB::GetWTJoinTx(uint8_t nSidechain, int nHeight) const
{
    if (!HasState())
        return CTransaction();
    if (!SidechainNumberValid(nSidechain))
        return CTransaction();

    const Sidechain& sidechain = ValidSidechains[nSidechain];

    if (nHeight % sidechain.GetTau() != 0)
        return CTransaction();

    // Select the highest scoring B-WT^ for sidechain this tau
    uint256 hashBest = uint256();
    uint16_t scoreBest = 0;
    std::vector<SidechainWTJoinState> vState = GetState(nSidechain);
    for (const SidechainWTJoinState& state : vState) {
        if (state.nWorkScore > scoreBest || scoreBest == 0) {
            hashBest = state.wtxid;
            scoreBest = state.nWorkScore;
        }
    }
    if (hashBest == uint256())
        return CTransaction();

    // Is the selected B-WT^ verified?
    if (scoreBest < sidechain.nMinWorkScore)
        return CTransaction();

    // Copy outputs from B-WT^
    CMutableTransaction mtx; // WT^
    for (const CTransaction& tx : vWTJoinCache) {
        if (tx.GetHash() == hashBest)
            for (const CTxOut& out : tx.vout)
                mtx.vout.push_back(out);
    }
    if (!mtx.vout.size())
        return CTransaction();

    // Calculate the amount to be withdrawn by WT^
    CAmount amtBWT = CAmount(0);
    for (const CTxOut& out : mtx.vout) {
        const CScript scriptPubKey = out.scriptPubKey;
        if (HexStr(scriptPubKey) != SIDECHAIN_TEST_SCRIPT_HEX) {
            amtBWT += out.nValue;
        }
    }

    // Format sidechain change return script
    CKeyID sidechainKey;
    sidechainKey.SetHex(SIDECHAIN_TEST_KEY);
    CScript sidechainScript;
    sidechainScript << OP_DUP << OP_HASH160 << ToByteVector(sidechainKey) << OP_EQUALVERIFY << OP_CHECKSIG;

    // Add placeholder change return as last output
    mtx.vout.push_back(CTxOut(0, sidechainScript));

    // Get SCUTXO(s)
    std::vector<COutput> vSidechainCoins;
    pwalletMain->AvailableSidechainCoins(vSidechainCoins, 0);
    if (!vSidechainCoins.size())
        return CTransaction();

    // Calculate amount returning to sidechain script
    CAmount returnAmount = CAmount(0);
    for (const COutput& output : vSidechainCoins) {
        mtx.vin.push_back(CTxIn(output.tx->GetHash(), output.i));
        returnAmount += output.tx->tx->vout[output.i].nValue;
        mtx.vout.back().nValue += returnAmount;
    }

    // Subtract payout amount from sidechain change return
    mtx.vout.back().nValue -= amtBWT;

    if (mtx.vout.back().nValue < 0)
        return CTransaction();
    if (!mtx.vin.size())
        return CTransaction();

    CBitcoinSecret vchSecret;
    bool fGood = vchSecret.SetString(SIDECHAIN_TEST_PRIV);
    if (!fGood)
        return CTransaction();

    CKey privKey = vchSecret.GetKey();
    if (!privKey.IsValid())
        return CTransaction();

    // Set up keystore with sidechain's private key
    CBasicKeyStore tempKeystore;
    tempKeystore.AddKey(privKey);
    const CKeyStore& keystoreConst = tempKeystore;

    // Sign WT^ SCUTXO input
    const CTransaction& txToSign = mtx;
    TransactionSignatureCreator creator(&keystoreConst, &txToSign, 0, returnAmount - amtBWT);
    SignatureData sigdata;
    bool sigCreated = ProduceSignature(creator, sidechainScript, sigdata);
    if (!sigCreated)
        return CTransaction();

    mtx.vin[0].scriptSig = sigdata.scriptSig;

    // Return completed WT^
    return mtx;
}

CScript SidechainDB::CreateStateScript(int nHeight) const
{
    /*
     * TODO use merged mining to decide what the new state is.
     * For now, just upvoting the current best WT^.
     */
    if (!HasState())
        return CScript();

    CScript script;
    script << OP_RETURN << SCOP_VERSION << SCOP_VERSION_DELIM;

    // Collect scores that need updating
    std::vector<std::vector<SidechainWTJoinState>> vScores;
    for (const Sidechain& s : ValidSidechains) {
        const std::vector<SidechainWTJoinState> vState = GetState(s.nSidechain);
        vScores.push_back(vState);
    }

    for (size_t x = 0; x < vScores.size(); x++) {
        SidechainWTJoinState mostVerified;
        for (size_t y = 0; y < vScores[x].size(); y++) {
            const SidechainWTJoinState& v = vScores[x][y];
            if (y == 0)
                mostVerified = v;
            if (v.nWorkScore > mostVerified.nWorkScore)
                mostVerified = v;
        }

        for (size_t y = 0; y < vScores[x].size(); y++) {
            const Sidechain& s = ValidSidechains[x];
            const SidechainWTJoinState& v = vScores[x][y];
            int nTauLast = s.GetLastTauHeight(nHeight);
            if (nHeight - nTauLast >= s.nWaitPeriod) {
                // Update state during verification period
                if (v.wtxid == mostVerified.wtxid)
                    script << SCOP_VERIFY;
                else
                    script << SCOP_REJECT;
            } else {
                // Ignore state during waiting period
                script << SCOP_IGNORE;
            }

            // Delimit WT^
            if (y != vScores[x].size() - 1)
                script << SCOP_WT_DELIM;
        }
        // Delimit sidechain
        if (x != vScores.size() - 1)
            script << SCOP_SC_DELIM;
    }
    return script;
}

uint256 SidechainDB::CreateSCDBHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    for (size_t i = 0; i < SCDB.size(); i++) {
        if (!SCDB[i].size())
            continue;
        ss << SCDB[i].back();
    }
    return ss.GetHash();
}

bool SidechainDB::ReadStateScript(const CTransactionRef& coinbase)
{
    /*
     * Only one state script of the current version is valid.
     * State scripts with invalid version numbers will be ignored.
     * If there are multiple state scripts with valid version numbers
     * the entire coinbase will be ignored by SCDB and a default
     * ignore vote will be cast. If there isn't a state update in
     * the transaction outputs, a default ignore vote will be cast.
     */
    if (!coinbase || coinbase->IsNull())
        return false;

    if (!HasState())
        return false;

    // Collect potentially valid state scripts
    std::vector<CScript> vStateScript;
    for (size_t i = 0; i < coinbase->vout.size(); i++) {
        const CScript& scriptPubKey = coinbase->vout[i].scriptPubKey;
        if (scriptPubKey.size() < 3)
            continue;
        // State script begins with OP_RETURN
        if (scriptPubKey[0] != OP_RETURN)
            continue;
        // Check state script version
        if (scriptPubKey[1] != SCOP_VERSION || scriptPubKey[2] != SCOP_VERSION_DELIM)
            continue;
        vStateScript.push_back(scriptPubKey);
    }

    // First case: Invalid update. Ignore state script, cast all ignore votes
    if (vStateScript.size() != 1)
        return ApplyDefaultUpdate();

    // Second case: potentially valid update script, attempt to update SCDB
    // Collect and combine the status of all sidechain WT^(s)
    std::vector<std::vector<SidechainWTJoinState>> vStateAll;
    for (const Sidechain& s : ValidSidechains) {
        const std::vector<SidechainWTJoinState> vState = GetState(s.nSidechain);
        vStateAll.push_back(vState);
    }

    const CScript& state = vStateScript.front();
    if (ApplyStateScript(state, vStateAll, true))
        return ApplyStateScript(state, vStateAll);

    // Invalid or no update script, apply default
    if (!ApplyDefaultUpdate())
        LogPrintf("SidechainDB::ReadStateScript: Invalid update & failed to apply default update!\n");

    return false;
}

bool SidechainDB::Update(uint8_t nSidechain, uint16_t nBlocks, uint16_t nScore, uint256 wtxid, bool fJustCheck)
{
    if (!SidechainNumberValid(nSidechain))
        return false;

    SidechainWTJoinState v;
    v.nBlocksLeft = nBlocks;
    v.nSidechain = nSidechain;
    v.nWorkScore = nScore;
    v.wtxid = wtxid;

    if (!fJustCheck)
        SCDB[nSidechain].push_back(v);

    return true;
}

bool SidechainDB::Update(int nHeight, const uint256& hashBlock, const CTransactionRef& coinbase)
{
    if (!coinbase || coinbase->IsNull())
        return false;
    // If a sidechain's tau period ended, reset WT^ verification status
    for (const Sidechain& s : ValidSidechains)
        if (nHeight > 0 && (nHeight % s.GetTau()) == 0)
            SCDB[s.nSidechain].clear();

    // Apply state script
    if (!ReadStateScript(coinbase))
        LogPrintf("SidechainDB::Update: failed to read state script\n");

    // Scan for h*(s) in coinbase outputs
    for (const CTxOut& out : coinbase->vout) {
        const CScript& scriptPubKey = out.scriptPubKey;

        // Must at least contain the h*
        if (scriptPubKey.size() < sizeof(uint256))
            continue;
        if (scriptPubKey[0] != OP_RETURN)
            continue;

        CScript::const_iterator phash = scriptPubKey.begin() + 1;
        std::vector<unsigned char> vch;
        opcodetype opcode;
        if (!scriptPubKey.GetOp2(phash, opcode, &vch))
            continue;
        if (vch.size() != sizeof(uint256))
            continue;

        uint256 hashCritical = uint256(vch);

        // Update BMM linking data
        // Add new linking data
        mapBMMLD.emplace(hashCritical, nHeight);
        queueBMMLD.push(hashCritical);

        // Remove old linking data if we need to
        if (mapBMMLD.size() > SIDECHAIN_MAX_LD) {
            uint256 hashRemove = queueBMMLD.front();
            std::multimap<uint256, int>::const_iterator it = mapBMMLD.lower_bound(hashRemove);
            if (it->first == hashRemove) {
                mapBMMLD.erase(it);
                queueBMMLD.pop();
            }
        }
    }
    hashBlockLastSeen = hashBlock;
    return true;
}

uint256 SidechainDB::GetHashBlockLastSeen()
{
    return hashBlockLastSeen;
}

std::multimap<uint256, int> SidechainDB::GetLinkingData() const
{
    return mapBMMLD;
}

bool SidechainDB::HasState() const
{
    if (SCDB.size() != ARRAYLEN(ValidSidechains))
        return false;

    if (!SCDB[SIDECHAIN_TEST].empty())
        return true;
    else
    if (!SCDB[SIDECHAIN_HIVEMIND].empty())
        return true;
    else
    if (!SCDB[SIDECHAIN_WIMBLE].empty())
        return true;

    return false;
}

std::vector<SidechainWTJoinState> SidechainDB::GetState(uint8_t nSidechain) const
{
    if (!HasState() || !SidechainNumberValid(nSidechain))
        return std::vector<SidechainWTJoinState>();

    // Go through SCDB and find newest verification for each WT^
    std::map<uint256, SidechainWTJoinState> mapScores;
    // This vector tracks the order in which verifications entered the SCDB
    std::vector<SidechainWTJoinState> vLastVerification;
    for (size_t x = 0; x < SCDB[nSidechain].size(); x++) {
        const SidechainWTJoinState &v = SCDB[nSidechain][x];

        std::map<uint256, SidechainWTJoinState>::iterator it = mapScores.find(v.wtxid);
        if (it != mapScores.end()) {
            // We already found the latest verification
            if (it->second.nWorkScore < v.nWorkScore)
                it->second = v;
        } else {
            // Add latest verification for undiscovered WT^
            mapScores[v.wtxid] = v;
            vLastVerification.push_back(v);
        }
    }
    // Update properly sorted list of verifications with found scores
    for (size_t i = 0; i < vLastVerification.size(); i++) {
        std::map<uint256, SidechainWTJoinState>::iterator it = mapScores.find(vLastVerification[i].wtxid);
        if (it != mapScores.end())
            vLastVerification[i] = it->second;
    }
    return vLastVerification;
}

bool SidechainDB::ApplyStateScript(const CScript& script, const std::vector<std::vector<SidechainWTJoinState>>& vState, bool fJustCheck)
{
    if (script.size() < 4)
        return false;

    uint8_t nSidechainIndex = 0;
    size_t nWTIndex = 0;
    for (size_t i = 3; i < script.size(); i++) {
        if (!SidechainNumberValid(nSidechainIndex))
            return false;

        // Move on to this sidechain's next WT^
        if (script[i] == SCOP_WT_DELIM) {
            nWTIndex++;
            continue;
        }

        // Move on to the next sidechain
        if (script[i] == SCOP_SC_DELIM) {
            nWTIndex = 0;
            nSidechainIndex++;
            continue;
        }

        // Check for valid vote type
        const unsigned char& vote = script[i];
        if (vote != SCOP_REJECT && vote != SCOP_VERIFY && vote != SCOP_IGNORE)
            continue;

        if (nSidechainIndex > vState.size())
            return false;
        if (nWTIndex > vState[nSidechainIndex].size())
            return false;

        const SidechainWTJoinState& old = vState[nSidechainIndex][nWTIndex];

        uint16_t nBlocksLeft = old.nBlocksLeft;
        if (nBlocksLeft > 0)
            nBlocksLeft--;

        uint16_t nWorkScore = old.nWorkScore;
        if (vote == SCOP_REJECT) {
            if (nWorkScore > 0)
                nWorkScore--;
        }
        else
        if (vote == SCOP_VERIFY) {
            nWorkScore++;
        }

        if (!Update(old.nSidechain, nBlocksLeft, nWorkScore, old.wtxid, fJustCheck) && fJustCheck)
            return false;
    }
    return true;
}

bool SidechainDB::ApplyDefaultUpdate()
{
    if (!HasState())
        return true;

    // Collect WT^(s) that need to be updated
    std::vector<SidechainWTJoinState> vNeedUpdate;
    for (const Sidechain& s : ValidSidechains) {
        const std::vector<SidechainWTJoinState> vState = GetState(s.nSidechain);
        for (const SidechainWTJoinState& state : vState)
            vNeedUpdate.push_back(state);
    }

    // Check that the updates can be applied
    for (const SidechainWTJoinState& v : vNeedUpdate) {
        if (!Update(v.nSidechain, v.nBlocksLeft - 1, v.nWorkScore, v.wtxid, true))
            return false;
    }
    // Apply the updates
    for (const SidechainWTJoinState& v : vNeedUpdate)
        Update(v.nSidechain, v.nBlocksLeft - 1, v.nWorkScore, v.wtxid);

    return true;
}

bool SidechainDB::CheckWorkScore(const uint8_t &nSidechain, const uint256& wtxid) const
{
    if (!SidechainNumberValid(nSidechain))
        return false;

    std::vector<SidechainWTJoinState> vState = GetState(nSidechain);
    for (const SidechainWTJoinState& state : vState) {
        if (state.wtxid == wtxid) {
            if (state.nWorkScore >= ValidSidechains[nSidechain].nMinWorkScore)
                return true;
            else
                return false;
        }
    }
    return false;
}

std::string SidechainDB::ToString() const
{
    std::string str;
    str += "SidechainDB:\n";
    for (const Sidechain& s : ValidSidechains) {
        // Print sidechain name
        str += "Sidechain: " + s.GetSidechainName() + "\n";
        // Print sidechain WT^ workscore(s)
        std::vector<SidechainWTJoinState> vState = GetState(s.nSidechain);
        for (const SidechainWTJoinState& state : vState) {
            str += "WT^: " + state.wtxid.ToString() + "\n";
            str += "workscore: " + std::to_string(state.nWorkScore) + "\n";
        }
        str += "\n";
    }
    return str;
}
