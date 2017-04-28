// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_COINBASECACHE_H
#define BITCOIN_COINBASECACHE_H

#include "primitives/transaction.h"
#include "uint256.h"

#include <vector>

class CAutoFile;

const unsigned int nCoinbaseToCache = 2600;

class CoinbaseCache
{
public:
    CoinbaseCache();

    /** Add new coinbase to the cache */
    bool ProcessNewCoinbase(const uint256 &hashBlock, const CTransactionRef& tx);

    /** Write coinbase cache to a file */
    bool Write(CAutoFile& fileout) const;

    /** Read coinbase cache from a file */
    bool Read(CAutoFile& filein);

private:
    std::vector<std::pair<uint256, CTransactionRef>> vCoinbase;
};

#endif // BITCOIN_COINBASECACHE_H

