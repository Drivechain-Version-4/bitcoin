// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "coinbasecache.h"

#include "clientversion.h"
#include "streams.h"
#include "util.h"

CoinbaseCache::CoinbaseCache()
{

}

bool CoinbaseCache::ProcessNewCoinbase(const uint256& hashBlock, const CTransactionRef& tx)
{
    if (vCoinbase.size() >= nCoinbaseToCache)
        vCoinbase.erase(vCoinbase.begin());

    vCoinbase.push_back(std::make_pair(hashBlock, tx));

    return true;
}

bool CoinbaseCache::Write(CAutoFile& fileout) const
{
    try {
        fileout << 149900; // version required to read: 0.14.99 or later
        fileout << CLIENT_VERSION; // version that wrote the file
        fileout << vCoinbase;
    }
    catch (const std::exception&) {
        LogPrintf("CoinbaseCache::Write(): unable to write coinbase cache (non-fatal)\n");
        return false;
    }
    return true;
}

bool CoinbaseCache::Read(CAutoFile& filein)
{
    try {
        int nVersionRequired, nVersionThatWrote;
        filein >> nVersionRequired >> nVersionThatWrote;
        if (nVersionRequired > CLIENT_VERSION)
            return error("CoinbaseCache::Read: up-version (%d) coinbase cache", nVersionRequired);
        filein >> vCoinbase;
    }
    catch (const std::exception&) {
        LogPrintf("CoinbaseCache::Read(): unable to read coinbase cache (non-fatal)\n");
        return false;
    }
    return true;
}

