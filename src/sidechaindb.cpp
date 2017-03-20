// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sidechaindb.h"

#include "base58.h"
#include "chain.h"
#include "core_io.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "sidechain.h"
#include "uint256.h"
#include "utilstrencodings.h"
#include "wallet/wallet.h"

#include <map>
#include <sstream>

SidechainDB::SidechainDB()
{
    // TODO Read state from historical blocks
    SCDB.resize(ValidSidechains.size());
}

void SidechainDB::AddDeposit(const CTransaction& tx)
{
    // Create sidechain deposit objects from transaction outputs
    std::vector<SidechainDeposit> vDepositAdd;
    for (size_t i = 0; i < tx.vout.size(); i++) {
        const CScript &script = tx.vout[i].scriptPubKey;
        if (script.size() < 3)
            continue;
        if (script.front() != OP_RETURN)
            continue;

        uint8_t nSidechain = (unsigned int)script[1];
        if (!SidechainNumberValid(nSidechain))
            continue;

        std::vector<unsigned char> vch;
        opcodetype opcode;
        CScript::const_iterator pkey = script.begin() + 2;
        if (!script.GetOp2(pkey, opcode, &vch))
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

        vDepositAdd.push_back(deposit);
    }

    // Add deposits to cache
    for (const SidechainDeposit& d : vDepositAdd) {
        if (!HaveDepositCached(d))
            vDepositCache.push_back(d);
    }
}

bool SidechainDB::AddWTJoin(uint8_t nSidechain, CTransaction wtx)
{
    if (vWTJoinCache.size() >= SIDECHAIN_MAX_WT)
        return false;
    if (!SidechainNumberValid(nSidechain))
        return false;
    if (HaveWTJoinCached(wtx.GetHash()))
        return false;

    const Sidechain& s = ValidSidechains[nSidechain];
    uint16_t nTau = s.nWaitPeriod + s.nVerificationPeriod;

    if (Update(nSidechain, nTau, 0, wtx.GetHash())) {
        vWTJoinCache.push_back(wtx);
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

bool SidechainDB::HaveWTJoinCached(uint256 wtxid) const
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

    uint16_t nTau = (sidechain.nWaitPeriod + sidechain.nVerificationPeriod);
    if (nHeight % nTau != 0)
        return CTransaction();

    // Select the highest scoring B-WT^ for sidechain this tau
    uint256 hashBest = uint256();
    uint16_t scoreBest = 0;
    std::vector<SidechainVerification> vScores = GetLastVerifications(nSidechain);
    for (const SidechainVerification& v : vScores) {
        if (v.nWorkScore > scoreBest || scoreBest == 0) {
            hashBest = v.wtxid;
            scoreBest = v.nWorkScore;
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
    std::vector<std::vector<SidechainVerification>> vScores;
    for (const Sidechain& s : ValidSidechains) {
        const std::vector<SidechainVerification>& vSidechainLatest = GetLastVerifications(s.nSidechain);
        vScores.push_back(vSidechainLatest);
    }

    for (size_t x = 0; x < vScores.size(); x++) {
        SidechainVerification mostVerified;
        for (size_t y = 0; y < vScores[x].size(); y++) {
            const SidechainVerification& v = vScores[x][y];
            if (y == 0)
                mostVerified = v;
            if (v.nWorkScore > mostVerified.nWorkScore)
                mostVerified = v;
        }

        for (size_t y = 0; y < vScores[x].size(); y++) {
            const Sidechain& s = ValidSidechains[x];
            const SidechainVerification& v = vScores[x][y];

            int nTauLast = GetLastTauHeight(s, nHeight);
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

bool SidechainDB::Update(const CTransaction& tx)
{
    /*
     * Only one state script of the current version is valid.
     * State scripts with invalid version numbers will be ignored.
     * If there are multiple state scripts with valid version numbers
     * the entire coinbase will be ignored by SCDB and a default
     * ignore vote will be cast. If there isn't a state update in
     * the transaction outputs, a default ignore vote will be cast.
     */
    if (!HasState())
        return false;

    // Collect potentially valid state scripts
    std::vector<CScript> vStateScript;
    for (size_t i = 0; i < tx.vout.size(); i++) {
        const CScript& scriptPubKey = tx.vout[i].scriptPubKey;
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
    if (vStateScript.size() != 1) {
        // Collect WT^(s) that need to be updated
        std::vector<SidechainVerification> vNeedUpdate;
        for (const Sidechain& s : ValidSidechains) {
            const std::vector<SidechainVerification> vScores = GetLastVerifications(s.nSidechain);
            for (const SidechainVerification& v : vScores)
                vNeedUpdate.push_back(v);
        }

        // Check that the updates can be applied
        for (const SidechainVerification& v : vNeedUpdate) {
            if (!Update(v.nSidechain, v.nBlocksLeft - 1, v.nWorkScore, v.wtxid, true))
                return false;
        }
        // Apply the updates
        for (const SidechainVerification& v : vNeedUpdate)
            Update(v.nSidechain, v.nBlocksLeft - 1, v.nWorkScore, v.wtxid);
        return true;
    }

    // Collect scores that need updating
    std::vector<std::vector<SidechainVerification>> vScores;
    for (const Sidechain& s : ValidSidechains) {
        const std::vector<SidechainVerification>& vSidechainLatest = GetLastVerifications(s.nSidechain);
        vScores.push_back(vSidechainLatest);
    }

    // Second case: potentially valid update script, attempt to update SCDB
    const CScript& state = vStateScript.front();
    if (ApplyStateScript(state, vScores, true))
        return ApplyStateScript(state, vScores);

    return false;
}

bool SidechainDB::Update(uint8_t nSidechain, uint16_t nBlocks, uint16_t nScore, uint256 wtxid, bool fJustCheck)
{
    if (!SidechainNumberValid(nSidechain))
        return false;

    SidechainVerification v;
    v.nBlocksLeft = nBlocks;
    v.nSidechain = nSidechain;
    v.nWorkScore = nScore;
    v.wtxid = wtxid;

    if (!fJustCheck)
        SCDB[nSidechain].push_back(v);
    return true;
}

bool SidechainDB::HasState() const
{
    if (SCDB.size() < ARRAYLEN(ValidSidechains))
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

int SidechainDB::GetLastTauHeight(const Sidechain& sidechain, int nHeight) const
{
    uint16_t nTau = (sidechain.nWaitPeriod + sidechain.nVerificationPeriod);
    for (;;) {
        if (nHeight % nTau == 0) break;
        if (nHeight == 0) break;
        nHeight--;
    }
    return nHeight;
}

std::vector<SidechainVerification> SidechainDB::GetLastVerifications(uint8_t nSidechain) const
{
    if (!HasState() || !SidechainNumberValid(nSidechain))
        return std::vector<SidechainVerification>();

    // Go through SCDB and find newest verification for each WT^
    std::map<uint256, SidechainVerification> mapScores;
    // This vector tracks the order in which verifications entered the SCDB
    std::vector<SidechainVerification> vLastVerification;
    for (size_t x = 0; x < SCDB[nSidechain].size(); x++) {
        const SidechainVerification &v = SCDB[nSidechain][x];

        std::map<uint256, SidechainVerification>::iterator it = mapScores.find(v.wtxid);
        if (it != mapScores.end()) {
            // We already found the latest verification
            if (it->second.nWorkScore < v.nWorkScore)
                it->second = v;
        } else {
            // Add latest verification for undiscoverd WT^
            mapScores[v.wtxid] = v;
            vLastVerification.push_back(v);
        }
    }
    // Update properly sorted list of verifications with found scores
    for (size_t i = 0; i < vLastVerification.size(); i++) {
        std::map<uint256, SidechainVerification>::iterator it = mapScores.find(vLastVerification[i].wtxid);
        if (it != mapScores.end())
            vLastVerification[i] = it->second;
    }
    return vLastVerification;
}

bool SidechainDB::ApplyStateScript(const CScript& script, const std::vector<std::vector<SidechainVerification>>& vScores, bool fJustCheck)
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

        if (nSidechainIndex > vScores.size())
            return false;
        if (nWTIndex > vScores[nSidechainIndex].size())
            return false;

        const SidechainVerification& old = vScores[nSidechainIndex][nWTIndex];

        uint16_t nBlocksLeft = old.nBlocksLeft - 1; // TODO limits
        if (vote == SCOP_REJECT) {
            if (!Update(old.nSidechain, nBlocksLeft, old.nWorkScore - 1, old.wtxid, fJustCheck) && fJustCheck)
                return false;
        }
        else
        if (vote == SCOP_VERIFY) {
            if (!Update(old.nSidechain, nBlocksLeft, old.nWorkScore + 1, old.wtxid, fJustCheck) && fJustCheck)
                return false;
        }
        else
        if (vote == SCOP_IGNORE) {
            if (!Update(old.nSidechain, nBlocksLeft, old.nWorkScore, old.wtxid, fJustCheck) && fJustCheck)
                return false;
        }
    }
    return true;
}

std::string Sidechain::GetSidechainName() const
{
    // Check that number coresponds to a valid sidechain
    switch (nSidechain) {
    case SIDECHAIN_TEST:
        return "SIDECHAIN_TEST";
    case SIDECHAIN_HIVEMIND:
        return "SIDECHAIN_HIVEMIND";
    case SIDECHAIN_WIMBLE:
        return "SIDECHAIN_WIMBLE";
    default:
        break;
    }
    return "SIDECHAIN_UNKNOWN";
}

bool SidechainDeposit::operator==(const SidechainDeposit& a) const
{
    return (a.nSidechain == nSidechain &&
            a.keyID == keyID &&
            a.hex == hex);
}

std::string Sidechain::ToString() const
{
    std::stringstream ss;
    ss << "nSidechain=" << (unsigned int)nSidechain << std::endl;
    ss << "nWaitPeriod=" << nWaitPeriod << std::endl;
    ss << "nVerificationPeriod=" << nVerificationPeriod << std::endl;
    ss << "nMinWorkScore=" << nMinWorkScore << std::endl;
    return ss.str();
}

std::string SidechainDeposit::ToString() const
{
    std::stringstream ss;
    ss << "nSidechain=" << (unsigned int)nSidechain << std::endl;
    ss << "keyID=" << keyID.ToString() << std::endl;
    ss << "hex=" << hex << std::endl;
    return ss.str();
}

std::string SidechainVerification::ToString() const
{
    std::stringstream ss;
    ss << "nSidechain=" << (unsigned int)nSidechain << std::endl;
    ss << "nBlocksLeft=" << (unsigned int)nBlocksLeft << std::endl;
    ss << "nWorkScore=" << (unsigned int)nWorkScore << std::endl;
    ss << "wtxid=" << wtxid.ToString() << std::endl;
    return ss.str();
}
