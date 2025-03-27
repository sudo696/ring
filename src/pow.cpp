// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>
#include <core_io.h>            // Ring-fork: Hive
#include <script/standard.h>    // Ring-fork: Hive
#include <base58.h>             // Ring-fork: Hive
#include <pubkey.h>             // Ring-fork: Hive
#include <hash.h>               // Ring-fork: Hive
#include <sync.h>               // Ring-fork: Hive
#include <validation.h>         // Ring-fork: Hive
#include <util/strencodings.h>  // Ring-fork: Hive
#include <logging.h>            // Ring-fork: Hive
#include <key_io.h>             // Ring-fork: Hive
#include <randomx.h> // Include RandomX header

DwarfPopGraphPoint dwarfPopGraph[1024*40];       // Ring-fork: Hive

// Ring-fork: Difficulty adjustment is based on Zawy's fixed DGW.
unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Allow extremely low difficulty up to the last initial distribution block
    if (pindexLast->nHeight < params.lastInitialDistributionHeight)
        return UintToArith256(params.powLimitInitialDistribution).GetCompact();

    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    int64_t nPastBlocks = 24;

    // Ring-fork: Allow minimum difficulty blocks if we haven't seen a block for ostensibly 10 blocks worth of time (testnet only)
    if (params.fPowAllowMinDifficultyBlocks && pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing * 10)
        return bnPowLimit.GetCompact();

    // Ring-fork: Hive: Skip over Hivemined blocks at tip
    while (pindexLast->GetBlockHeader().IsHiveMined(params)) {
        //LogPrintf("Skipping hivemined block at %i\n", pindex->nHeight);
        assert(pindexLast->pprev); // should never fail
        pindexLast = pindexLast->pprev;
    }

    const CBlockIndex *pindex = pindexLast;
    arith_uint256 bnPastTargetAvg;

    for (unsigned int i = 1; i <= nPastBlocks; i++) {
        // Ring-fork: Hive: Skip over Hivemined blocks; we only want to consider PoW blocks
        while (pindex->GetBlockHeader().IsHiveMined(params)) {
            //LogPrintf("Skipping popmined block at %i\n", pindex->nHeight);
            assert(pindex->pprev); // should never fail
            pindex = pindex->pprev;
        }

        arith_uint256 bnTarget = arith_uint256().SetCompact(pindex->nBits);
        bnPastTargetAvg += bnTarget/nPastBlocks;    // Simple moving average
        assert(pindex->pprev);
        pindex = pindex->pprev;
    }

    arith_uint256 bnNew(bnPastTargetAvg);

    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindex->GetBlockTime();
    // NOTE: is this accurate? nActualTimespan counts it for (nPastBlocks - 1) blocks only...
    int64_t nTargetTimespan = nPastBlocks * params.nPowTargetSpacing;

    if (nActualTimespan < nTargetTimespan/3)
        nActualTimespan = nTargetTimespan/3;
    if (nActualTimespan > nTargetTimespan*3)
        nActualTimespan = nTargetTimespan*3;

    // Retarget
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}


// Ring-fork: Hive: SMA Hive Difficulty Adjust
unsigned int GetNextHiveWorkRequired(const CBlockIndex* pindexLast, const Consensus::Params& params) {
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimitHive);

    arith_uint256 dwarfHashTarget = 0;
    int hiveBlockCount = 0;
    int totalBlockCount = 0;

    // Step back till we have found 24 hive blocks, or we ran out...
    while (hiveBlockCount < params.hiveDifficultyWindow && pindexLast->pprev && pindexLast->nHeight >= params.minHiveCheckBlock) {
        if (pindexLast->GetBlockHeader().IsHiveMined(params)) {
            dwarfHashTarget += arith_uint256().SetCompact(pindexLast->nBits);
            hiveBlockCount++;
        }
        totalBlockCount++;

        pindexLast = pindexLast->pprev;
    }

    if (hiveBlockCount == 0) {          // Should only happen when chain is starting
        if (LogAcceptCategory(BCLog::HIVE))
            LogPrintf("GetNextHiveWorkRequired: No previous hive blocks found.\n");
        return bnPowLimit.GetCompact();
    }

    dwarfHashTarget /= hiveBlockCount;    // Average the dwarf hash targets in window

    // Retarget based on totalBlockCount
    int targetTotalBlockCount = hiveBlockCount * params.hiveBlockSpacingTarget;
    dwarfHashTarget *= totalBlockCount;
    dwarfHashTarget /= targetTotalBlockCount;

    if (dwarfHashTarget > bnPowLimit)
        dwarfHashTarget = bnPowLimit;

    return dwarfHashTarget.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimitInitialDistribution))   // Ring-fork: Take the lower limit used for initial distribution as the early-out fail.
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}

// Ring-fork: Hive: Get count of all live and gestating DCTs on the network
bool GetNetworkHiveInfo(int& immatureDwarves, int& immatureDCTs, int& matureDwarves, int& matureDCTs, CAmount& potentialLifespanRewards, const Consensus::Params& consensusParams, bool recalcGraph) {
    int totalDwarfLifespan = consensusParams.dwarfLifespanBlocks + consensusParams.dwarfGestationBlocks;
    immatureDwarves = immatureDCTs = matureDwarves = matureDCTs = 0;

    CBlockIndex* pindexPrev = chainActive.Tip();
    assert(pindexPrev != nullptr);
    int tipHeight = pindexPrev->nHeight;
    potentialLifespanRewards = (consensusParams.dwarfLifespanBlocks * GetBlockSubsidyHive(consensusParams)) / (consensusParams.hiveBlockSpacingTargetTypical + consensusParams.popBlocksPerHive);

    if (recalcGraph) {
        for (int i = 0; i < totalDwarfLifespan; i++) {
            dwarfPopGraph[i].immaturePop = 0;
            dwarfPopGraph[i].maturePop = 0;
        }
    }

    if (IsInitialBlockDownload())   // Refuse if we're downloading
        return false;

    // Count dwarves in next blockCount blocks
    CBlock block;
    CScript scriptPubKeyBCF = GetScriptForDestination(DecodeDestination(consensusParams.dwarfCreationAddress));
    CScript scriptPubKeyCF = GetScriptForDestination(DecodeDestination(consensusParams.hiveCommunityAddress));

    for (int i = 0; i < totalDwarfLifespan; i++) {
        // Don't keep checking before minHiveCheckBlock 
        if (pindexPrev->nHeight < consensusParams.minHiveCheckBlock)
            break;

        if (fHavePruned && !(pindexPrev->nStatus & BLOCK_HAVE_DATA) && pindexPrev->nTx > 0) {
            LogPrintf("! GetNetworkHiveInfo: Warn: Block not available (pruned data); can't calculate network dwarf count.");
            return false;
        }

        if (!pindexPrev->GetBlockHeader().IsHiveMined(consensusParams)      // Don't check Hivemined blocks (no DCTs will be found in them)
        ) {
            if (!ReadBlockFromDisk(block, pindexPrev, consensusParams, false)) {
                LogPrintf("! GetNetworkHiveInfo: Warn: Block not available (not found on disk); can't calculate network dwarf count.");
                return false;
            }
            int blockHeight = pindexPrev->nHeight;
            CAmount dwarfCost = GetDwarfCost(blockHeight, consensusParams);
            if (block.vtx.size() > 0) {
                for(const auto& tx : block.vtx) {
                    CAmount dwarfFeePaid;
                    if (tx->IsDCT(consensusParams, scriptPubKeyBCF, &dwarfFeePaid)) {               // If it's a DCT, total its dwarves
                        if (tx->vout.size() > 1 && tx->vout[1].scriptPubKey == scriptPubKeyCF) {    // If it has a community fund contrib...
                            CAmount donationAmount = tx->vout[1].nValue;
                            CAmount expectedDonationAmount = (dwarfFeePaid + donationAmount) / consensusParams.communityContribFactor;  // ...check for valid donation amount
                            if (donationAmount != expectedDonationAmount)
                                continue;
                            dwarfFeePaid += donationAmount;                                           // Add donation amount back to total paid
                        }
                        int dwarfCount = dwarfFeePaid / dwarfCost;
                        if (i < consensusParams.dwarfGestationBlocks) {
                            immatureDwarves += dwarfCount;
                            immatureDCTs++;
                        } else {
                            matureDwarves += dwarfCount; 
                            matureDCTs++;
                        }

                        // Add these dwarves to pop graph
                        if (recalcGraph) {
                            int dwarfBornBlock = blockHeight;
                            int dwarfMaturesBlock = dwarfBornBlock + consensusParams.dwarfGestationBlocks;
                            int dwarfDiesBlock = dwarfMaturesBlock + consensusParams.dwarfLifespanBlocks;
                            for (int j = dwarfBornBlock; j < dwarfDiesBlock; j++) {
                                int graphPos = j - tipHeight;
                                if (graphPos > 0 && graphPos < totalDwarfLifespan) {
                                    if (j < dwarfMaturesBlock)
                                        dwarfPopGraph[graphPos].immaturePop += dwarfCount;
                                    else
                                        dwarfPopGraph[graphPos].maturePop += dwarfCount;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!pindexPrev->pprev)     // Check we didn't run out of blocks
            return true;

        pindexPrev = pindexPrev->pprev;
    }

    return true;
}

// Ring-fork: Hive: Check the hive proof for given block
bool CheckHiveProof(const CBlock* pblock, const Consensus::Params& consensusParams) {
    bool verbose = LogAcceptCategory(BCLog::HIVE);

    if (verbose)
        LogPrintf("********************* Hive: CheckHiveProof *********************\n");

    // Get height (a CBlockIndex isn't always available when this func is called, eg in reads from disk)
    int blockHeight;
    CBlockIndex* pindexPrev;
    {
        LOCK(cs_main);
        pindexPrev = mapBlockIndex[pblock->hashPrevBlock];
        blockHeight = pindexPrev->nHeight + 1;
    }
    if (!pindexPrev) {
        LogPrintf("CheckHiveProof: Couldn't get previous block's CBlockIndex!\n");
        return false;
    }
    if (verbose)
        LogPrintf("CheckHiveProof: nHeight             = %i\n", blockHeight);

    // Check we're past the pow-only slowstart
    if (blockHeight < consensusParams.lastInitialDistributionHeight + consensusParams.slowStartBlocks) {
        LogPrintf("CheckHiveProof: No hive blocks accepted by network until after slowstart!\n");
        return false;
    }

    // Check that there aren't too many Hive blocks since the last Pow block
    int hiveBlocksSincePow = 0;
    CBlockIndex* pindexTemp = pindexPrev;
    while (pindexTemp->GetBlockHeader().IsHiveMined(consensusParams)) {
        hiveBlocksSincePow++;

        assert(pindexTemp->pprev);
        pindexTemp = pindexTemp->pprev;
    }
    if (hiveBlocksSincePow >= consensusParams.maxConsecutiveHiveBlocks) {
        LogPrintf("CheckHiveProof: Too many Hive blocks without a POW block.\n");
        return false;
    }

    // Block mustn't include any DCTs
    CScript scriptPubKeyBCF = GetScriptForDestination(DecodeDestination(consensusParams.dwarfCreationAddress));
    if (pblock->vtx.size() > 1)
        for (unsigned int i=1; i < pblock->vtx.size(); i++)
            if (pblock->vtx[i]->IsDCT(consensusParams, scriptPubKeyBCF)) {
                LogPrintf("CheckHiveProof: Hivemined block contains DCTs!\n");
                return false;
            }

    // Coinbase tx must be valid
    CTransactionRef txCoinbase = pblock->vtx[0];
    //LogPrintf("CheckHiveProof: Got coinbase tx: %s\n", txCoinbase->ToString());
    if (!txCoinbase->IsCoinBase()) {
        LogPrintf("CheckHiveProof: Coinbase tx isn't valid!\n");
        return false;
    }

    // Must have exactly 2 or 3 outputs
    if (txCoinbase->vout.size() < 2 || txCoinbase->vout.size() > 3) {
        LogPrintf("CheckHiveProof: Didn't expect %i vouts!\n", txCoinbase->vout.size());
        return false;
    }

    // vout[0] must be long enough to contain all encodings
    if (txCoinbase->vout[0].scriptPubKey.size() < 144) {
        LogPrintf("CheckHiveProof: vout[0].scriptPubKey isn't long enough to contain hive proof encodings\n");
        return false;
    }

    // vout[1] must start OP_RETURN OP_DWARF (bytes 0-1)
    if (txCoinbase->vout[0].scriptPubKey[0] != OP_RETURN || txCoinbase->vout[0].scriptPubKey[1] != OP_DWARF) {
        LogPrintf("CheckHiveProof: vout[0].scriptPubKey doesn't start OP_RETURN OP_DWARF\n");
        return false;
    }

    // Grab the dwarf nonce (bytes 3-6; byte 2 has value 04 as a size marker for this field)
    uint32_t dwarfNonce = ReadLE32(&txCoinbase->vout[0].scriptPubKey[3]);
    if (verbose)
        LogPrintf("CheckHiveProof: dwarfNonce          = %i\n", dwarfNonce);

    // Grab the dct height (bytes 8-11; byte 7 has value 04 as a size marker for this field)
    uint32_t dctClaimedHeight = ReadLE32(&txCoinbase->vout[0].scriptPubKey[8]);
    if (verbose)
        LogPrintf("CheckHiveProof: dctHeight           = %i\n", dctClaimedHeight);

    // Get community contrib flag (byte 12)
    bool communityContrib = txCoinbase->vout[0].scriptPubKey[12] == OP_TRUE;
    if (verbose)
        LogPrintf("CheckHiveProof: communityContrib    = %s\n", communityContrib ? "true" : "false");

    // Grab the txid (bytes 14-78; byte 13 has val 64 as size marker)
    std::vector<unsigned char> txid(&txCoinbase->vout[0].scriptPubKey[14], &txCoinbase->vout[0].scriptPubKey[14 + 64]);
    std::string txidStr = std::string(txid.begin(), txid.end());
    if (verbose)
        LogPrintf("CheckHiveProof: dctTxId             = %s\n", txidStr);

    // Check dwarf hash against target
    std::string deterministicRandString = GetDeterministicRandString(pindexPrev);
    if (verbose)
        LogPrintf("CheckHiveProof: detRandString       = %s\n", deterministicRandString);
    arith_uint256 dwarfHashTarget;
    dwarfHashTarget.SetCompact(GetNextHiveWorkRequired(pindexPrev, consensusParams));
    if (verbose)
        LogPrintf("CheckHiveProof: dwarfHashTarget     = %s\n", dwarfHashTarget.ToString());

    arith_uint256 dwarfHash(CBlockHeader::MinotaurHashArbitrary(std::string(deterministicRandString + txidStr + std::to_string(dwarfNonce)).c_str()).ToString());
    dwarfHash = arith_uint256(CBlockHeader::MinotaurHashArbitrary(dwarfHash.ToString().c_str()).ToString());

    if (verbose)
        LogPrintf("CheckHiveProof: dwarfHash           = %s\n", dwarfHash.ToString());
    if (dwarfHash >= dwarfHashTarget) {
        LogPrintf("CheckHiveProof: Dwarf does not meet hash target!\n");
        return false;
    }

    // Grab the message sig (bytes 79-end; byte 78 is size)
    std::vector<unsigned char> messageSig(&txCoinbase->vout[0].scriptPubKey[79], &txCoinbase->vout[0].scriptPubKey[79 + 65]);
    if (verbose)
        LogPrintf("CheckHiveProof: messageSig          = %s\n", HexStr(&messageSig[0], &messageSig[messageSig.size()]));

    // Grab the reward address from the reward vout
    CTxDestination rewardDestination;
    if (!ExtractDestination(txCoinbase->vout[1].scriptPubKey, rewardDestination)) {
        LogPrintf("CheckHiveProof: Couldn't extract reward address\n");
        return false;
    }
    if (!IsValidDestination(rewardDestination)) {
        LogPrintf("CheckHiveProof: Reward address is invalid\n");
        return false;
    }
    if (verbose)
        LogPrintf("CheckHiveProof: rewardAddress       = %s\n", EncodeDestination(rewardDestination));

    // Verify the message sig
    const CKeyID *keyID = boost::get<CKeyID>(&rewardDestination);
    if (!keyID) {
        LogPrintf("CheckHiveProof: Can't get pubkey for reward address\n");
        return false;
    }
    CHashWriter ss(SER_GETHASH, 0);
    ss << deterministicRandString;
    uint256 mhash = ss.GetHash();
    CPubKey pubkey;
    if (!pubkey.RecoverCompact(mhash, messageSig)) {
        LogPrintf("CheckHiveProof: Couldn't recover pubkey from hash\n");
        return false;
    }
    if (pubkey.GetID() != *keyID) {
        LogPrintf("CheckHiveProof: Signature mismatch! GetID() = %s, *keyID = %s\n", pubkey.GetID().ToString(), (*keyID).ToString());
        return false;
    }

    // Grab the DCT utxo
    bool deepDrill = false;
    uint32_t dctFoundHeight;
    CAmount dctValue;
    CScript dctScriptPubKey;
    {
        LOCK(cs_main);

        COutPoint outDwarfCreation(uint256S(txidStr), 0);
        COutPoint outCommFund(uint256S(txidStr), 1);
        Coin coin;
        CTransactionRef dct = nullptr;
        CBlockIndex foundAt;

        if (pcoinsTip && pcoinsTip->GetCoin(outDwarfCreation, coin)) {      // First try the UTXO set (this pathway will hit on incoming blocks)
            if (verbose)
                LogPrintf("CheckHiveProof: Using UTXO set for outDwarfCreation\n");
            dctValue = coin.out.nValue;
            dctScriptPubKey = coin.out.scriptPubKey;
            dctFoundHeight = coin.nHeight;
        } else {                                                            // UTXO set isn't available when eg reindexing, so drill into block db (not too bad, since Alice put her DCT height in the coinbase tx)
            if (verbose)
                LogPrintf("! CheckHiveProof: Warn: Using deep drill for outDwarfCreation\n");
            if (!GetTxByHashAndHeight(uint256S(txidStr), dctClaimedHeight, dct, foundAt, pindexPrev, consensusParams)) {
                LogPrintf("CheckHiveProof: Couldn't locate indicated DCT\n");
                return false;
            }
            deepDrill = true;
            dctFoundHeight = foundAt.nHeight;
            dctValue = dct->vout[0].nValue;
            dctScriptPubKey = dct->vout[0].scriptPubKey;
        }

        if (communityContrib) {
            CScript scriptPubKeyCF = GetScriptForDestination(DecodeDestination(consensusParams.hiveCommunityAddress));
            CAmount donationAmount;

            if(dct == nullptr) {                                                                // If we dont have a ref to the DCT
                if (pcoinsTip && pcoinsTip->GetCoin(outCommFund, coin)) {                       // First try UTXO set
                    if (verbose)
                        LogPrintf("CheckHiveProof: Using UTXO set for outCommFund\n");
                    if (coin.out.scriptPubKey != scriptPubKeyCF) {                              // If we find it, validate the scriptPubKey and store amount
                        LogPrintf("CheckHiveProof: Community contrib was indicated but not found\n");
                        return false;
                    }
                    donationAmount = coin.out.nValue;
                } else {                                                                        // Fallback if we couldn't use UTXO set
                    if (verbose)
                        LogPrintf("! CheckHiveProof: Warn: Using deep drill for outCommFund\n");
                    if (!GetTxByHashAndHeight(uint256S(txidStr), dctClaimedHeight, dct, foundAt, pindexPrev, consensusParams)) {
                        LogPrintf("CheckHiveProof: Couldn't locate indicated DCT\n");           // Still couldn't find it
                        return false;
                    }
                    deepDrill = true;
                }
            }
            if(dct != nullptr) {                                                                // We have the DCT either way now (either from first or second drill). If got from UTXO set dct == nullptr still.
                if (dct->vout.size() < 2 || dct->vout[1].scriptPubKey != scriptPubKeyCF) {      // So Validate the scriptPubKey and store amount
                    LogPrintf("CheckHiveProof: Community contrib was indicated but not found\n");
                    return false;
                }
                donationAmount = dct->vout[1].nValue;
            }

            // Check for valid donation amount
            CAmount expectedDonationAmount = (dctValue + donationAmount) / consensusParams.communityContribFactor;
            if (donationAmount != expectedDonationAmount) {
                LogPrintf("CheckHiveProof: DCT pays community fund incorrect amount %i (expected %i)\n", donationAmount, expectedDonationAmount);
                return false;
            }

            // Update amount paid
            dctValue += donationAmount;
        }
    }

    if (dctFoundHeight != dctClaimedHeight) {
        LogPrintf("CheckHiveProof: Claimed DCT height of %i conflicts with found height of %i\n", dctClaimedHeight, dctFoundHeight);
        return false;
    }

    // Check dwarf maturity
    int dctDepth = blockHeight - dctFoundHeight;
    if (dctDepth < consensusParams.dwarfGestationBlocks) {
        LogPrintf("CheckHiveProof: Indicated DCT is immature.\n");
        return false;
    }
    if (dctDepth > consensusParams.dwarfGestationBlocks + consensusParams.dwarfLifespanBlocks) {
        LogPrintf("CheckHiveProof: Indicated DCT is too old.\n");
        return false;
    }

    // Check for valid dwarf creation script and get reward scriptPubKey from DCT
    CScript scriptPubKeyReward;
    if (!CScript::IsDCTScript(dctScriptPubKey, scriptPubKeyBCF, &scriptPubKeyReward)) {
        LogPrintf("CheckHiveProof: Indicated utxo is not a valid DCT script\n");
        return false;
    }

    CTxDestination rewardDestinationDCT;
    if (!ExtractDestination(scriptPubKeyReward, rewardDestinationDCT)) {
        LogPrintf("CheckHiveProof: Couldn't extract reward address from DCT UTXO\n");
        return false;
    }

    // Check DCT's reward address actually matches the claimed reward address
    if (rewardDestination != rewardDestinationDCT) {
        LogPrintf("CheckHiveProof: DCT's reward address does not match claimed reward address!\n");
        return false;
    }

    // Find dwarf count
    if (dctValue < consensusParams.dwarfCost) {
        LogPrintf("CheckHiveProof: DCT fee is less than the cost for a single dwarf\n");
        return false;
    }
    unsigned int dwarfCount = dctValue / consensusParams.dwarfCost;
    if (verbose) {
        LogPrintf("CheckHiveProof: dctValue            = %i\n", dctValue);
        LogPrintf("CheckHiveProof: dwarfCount          = %i\n", dwarfCount);
    }

    // Check enough dwarves were bought to include claimed dwarfNonce
    if (dwarfNonce >= dwarfCount) {
        LogPrintf("CheckHiveProof: DCT did not create enough dwarves for claimed nonce!\n");
        return false;
    }

    if (verbose)
        LogPrintf("CheckHiveProof: Pass at %i%s\n", blockHeight, deepDrill ? " (used deepdrill)" : "");
    return true;
}

// POP validation removed

bool static ScanHash(CBlockHeader *pblock, uint32_t& nNonce, uint256 *phash) {
    uint256 hash;
    while (true) {
        nNonce++;

        pblock->nNonce = nNonce;

        // Generate block header data for hashing
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << *pblock;
        std::vector<unsigned char> blockData(ss.begin(), ss.end());

        // Calculate RandomX hash
        randomx_vm* vm = randomx_create_vm(RANDOMX_FLAG_DEFAULT, nullptr, nullptr);
        if (vm == nullptr) return false;

        uint64_t seed[4];
        memcpy(seed, blockData.data(), sizeof(seed));
        randomx_calculate_hash(vm, blockData.data(), blockData.size(), hash.begin());
        randomx_destroy_vm(vm);

        if (hash.ByteAt(31) == 0 && hash.ByteAt(30) == 0) {
            memcpy(phash, &hash, 32);
            return true;
        }

        if ((nNonce & 0xffff) == 0)
            return false;

        if ((nNonce & 0xfff) == 0)
            boost::this_thread::interruption_point();
    }
}