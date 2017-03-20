// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sidechain.h"

bool SidechainNumberValid(uint8_t nSidechain)
{
    if (nSidechain > ValidSidechains.size())
        return false;

    // Check that number coresponds to a valid sidechain
    switch (nSidechain) {
    case SIDECHAIN_TEST:
    case SIDECHAIN_HIVEMIND:
    case SIDECHAIN_WIMBLE:
        return true;
    default:
        return false;
    }
}
