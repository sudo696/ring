// Copyright (c) 2018-2019 The Ring Developers
// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <miner.h>

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <hash.h>
#include <net.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <timedata.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <validationinterface.h>
#include <wallet/wallet.h>          // Ring-fork: In-wallet miner
#include <wallet/rpcwallet.h>       // Ring-fork: In-wallet miner
#include <rpc/server.h>             // Ring-fork: In-wallet miner
#include <ui_interface.h>           // Ring-fork: In-wallet miner
#include <sync.h>                   // Ring-fork: Hive
#include <key_io.h>                 // Ring-fork: Hive
#include <boost/thread.hpp>         // Ring-fork: Hive: Mining optimisations

static CCriticalSection cs_solution_vars;
std::atomic<bool> solutionFound;    // Ring-fork: Hive: Mining optimisations: Thread-safe atomic flag to signal solution found (saves a slow mutex)
std::atomic<bool> earlyAbort;       // Ring-fork: Hive: Mining optimisations: Thread-safe atomic flag to signal early abort needed
CDwarfRange solvingRange;           // Ring-fork: Hive: Mining optimisations: The solving range (protected by mutex)
uint32_t solvingDwarf;              // Ring-fork: Hive: Mining optimisations: The solving dwarf (protected by mutex)

#include <algorithm>
#include <queue>
#include <utility>
#include <boost/thread/thread.hpp>  // Ring-fork: In-wallet miner

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    // Ring-fork: Don't do this, it's ugly
    /*
    if (consensusParams.fPowAllowMinDifficultyBlocks)
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);
    */

    return nNewTime - nOldTime;
}

BlockAssembler::Options::Options() {
    blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    nBlockMaxWeight = DEFAULT_BLOCK_MAX_WEIGHT;
}

BlockAssembler::BlockAssembler(const CChainParams& params, const Options& options) : chainparams(params)
{
    blockMinFeeRate = options.blockMinFeeRate;
    // Limit weight to between 4K and MAX_BLOCK_WEIGHT-4K for sanity:
    nBlockMaxWeight = std::max<size_t>(4000, std::min<size_t>(MAX_BLOCK_WEIGHT - 4000, options.nBlockMaxWeight));
}

static BlockAssembler::Options DefaultOptions()
{
    // Block resource limits
    // If -blockmaxweight is not given, limit to DEFAULT_BLOCK_MAX_WEIGHT
    BlockAssembler::Options options;
    options.nBlockMaxWeight = gArgs.GetArg("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT);
    CAmount n = 0;
    if (gArgs.IsArgSet("-blockmintxfee") && ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n)) {
        options.blockMinFeeRate = CFeeRate(n);
    } else {
        options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    }
    return options;
}

BlockAssembler::BlockAssembler(const CChainParams& params) : BlockAssembler(params, DefaultOptions()) {}

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockWeight = 4000;
    nBlockSigOpsCost = 400;
    fIncludeWitness = false;
    fIncludeDCTs = true;    // Ring-fork: Hive

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}

Optional<int64_t> BlockAssembler::m_last_block_num_txs{nullopt};
Optional<int64_t> BlockAssembler::m_last_block_weight{nullopt};

// Ring-fork: Hive: If hiveProofScript is passed, create a Hive block instead of a PoW block
// Ring-fork: Pop: If hiveProofScript is null and popProofScript is passed, create a Pop block
std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn, const CScript* hiveProofScript, const CScript* popProofScript)
{
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if(!pblocktemplate.get())
        return nullptr;
    pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

    LOCK2(cs_main, mempool.cs);
    CBlockIndex* pindexPrev = chainActive.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = gArgs.GetArg("-blockversion", pblock->nVersion);

    pblock->nTime = GetAdjustedTime();
    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                       ? nMedianTimePast
                       : pblock->GetBlockTime();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization).
    // Note that the mempool would accept transactions with witness data before
    // IsWitnessEnabled, but we would only ever mine blocks after IsWitnessEnabled
    // unless there is a massive block reorganization with the witness softfork
    // not activated.
    // TODO: replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = IsWitnessEnabled(pindexPrev, chainparams.GetConsensus());

    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;

    // Ring-fork: Hive: Don't include DCTs in hivemined blocks
    // Ring-fork: Pop: Don't include DCTs in pop blocks
    if (hiveProofScript || popProofScript)
        fIncludeDCTs = false;

    addPackageTxs(nPackagesSelected, nDescendantsUpdated);

    int64_t nTime1 = GetTimeMicros();

    m_last_block_num_txs = nBlockTx;
    m_last_block_weight = nBlockWeight;

    // Create coinbase transaction.
    // Ring-fork: Hive: Create appropriate coinbase tx for pow or Hive block
    // Ring-fork: Pop: Handle pop blocks too
    if (hiveProofScript) {
        CMutableTransaction coinbaseTx;

        // 1 vin with empty prevout
        coinbaseTx.vin.resize(1);
        coinbaseTx.vin[0].prevout.SetNull();
        coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;

        // vout[0]: Hive proof
        coinbaseTx.vout.resize(2);
        coinbaseTx.vout[0].scriptPubKey = *hiveProofScript;
        coinbaseTx.vout[0].nValue = 0;

        // vout[1]: Reward :)
        coinbaseTx.vout[1].scriptPubKey = scriptPubKeyIn;
        coinbaseTx.vout[1].nValue = nFees + GetBlockSubsidyHive(chainparams.GetConsensus());

        // vout[2]: Coinbase commitment
        pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
        pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus());
        pblocktemplate->vTxFees[0] = -nFees;
    } else if (popProofScript) {
        CMutableTransaction coinbaseTx;

        // 1 vin with empty prevout
        coinbaseTx.vin.resize(1);
        coinbaseTx.vin[0].prevout.SetNull();
        coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;

        // vout[0]: Hive proof
        coinbaseTx.vout.resize(2);
        coinbaseTx.vout[0].scriptPubKey = *popProofScript;
        coinbaseTx.vout[0].nValue = 0;

        // vout[1]: Reward :)
        coinbaseTx.vout[1].scriptPubKey = scriptPubKeyIn;

        // Ring-fork: Pop
        bool isPrivate = coinbaseTx.vout[0].scriptPubKey[36] == OP_TRUE;
        CAmount subsidy = isPrivate ? GetBlockSubsidyPopPrivate(chainparams.GetConsensus()) : GetBlockSubsidyPopPublic(chainparams.GetConsensus());
        coinbaseTx.vout[1].nValue = nFees + subsidy;

        // vout[2]: Coinbase commitment
        pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
        pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus());
        pblocktemplate->vTxFees[0] = -nFees;
    } else {    
        CMutableTransaction coinbaseTx;
        coinbaseTx.vin.resize(1);
        coinbaseTx.vin[0].prevout.SetNull();
        coinbaseTx.vout.resize(1);
        coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
        coinbaseTx.vout[0].nValue = nFees + GetBlockSubsidyPow(nHeight, chainparams.GetConsensus());
        coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
        pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
        pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus());
        pblocktemplate->vTxFees[0] = -nFees;
    }

    LogPrintf("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, nFees, nBlockSigOpsCost);

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);

    // Ring-fork: Hive: Choose correct nBits depending on whether a Hive block is requested
    // Ring-fork: Pop: Handle pop blocks too
    if (hiveProofScript)
        pblock->nBits = GetNextHiveWorkRequired(pindexPrev, chainparams.GetConsensus());
    else if (popProofScript)
        pblock->nBits = UintToArith256(chainparams.GetConsensus().powLimit).GetCompact();
    else
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());

    // Ring-fork: Hive: Set nonce marker for hivemined blocks
    // Ring-fork: Pop: Handle pop blocks too
    pblock->nNonce = hiveProofScript ? chainparams.GetConsensus().hiveNonceMarker : popProofScript ? chainparams.GetConsensus().popNonceMarker : 0;

    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);

    CValidationState state;
    if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
        // Ring-fork: Pop: Don't throw here -- we may be in an event handler thread, and we can provide nicer messages anyway.
        if (popProofScript)
            return nullptr;
        else
            throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
    }
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCH, "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)\n", 0.001 * (nTime1 - nTimeStart), nPackagesSelected, nDescendantsUpdated, 0.001 * (nTime2 - nTime1), 0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        }
        else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= nBlockMaxWeight)
        return false;
    if (nBlockSigOpsCost + packageSigOpsCost >= MAX_BLOCK_SIGOPS_COST)
        return false;
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - premature witness (in case segwit transactions are added to mempool before
//   segwit activation)
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries& package)
{
    const Consensus::Params& consensusParams = Params().GetConsensus(); // Ring-fork: Hive

    for (CTxMemPool::txiter it : package) {
        if (!IsFinalTx(it->GetTx(), nHeight, nLockTimeCutoff))
            return false;
        if (!fIncludeWitness && it->GetTx().HasWitness())
            return false;
        // Ring-fork: Hive: Inhibit DCTs if required
        if (!fIncludeDCTs && it->GetTx().IsDCT(consensusParams, GetScriptForDestination(DecodeDestination(consensusParams.dwarfCreationAddress))))
            return false;
    }
    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("fee %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

int BlockAssembler::UpdatePackagesForAdded(const CTxMemPool::setEntries& alreadyAdded,
        indexed_modified_transaction_set &mapModifiedTx)
{
    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc))
                continue;
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCostWithAncestors -= it->GetSigOpCost();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }
    return nDescendantsUpdated;
}

// Skip entries in mapTx that are already in a block or are present
// in mapModifiedTx (which implies that the mapTx ancestor state is
// stale due to ancestor inclusion in the block)
// Also skip transactions that we've already failed to add. This can happen if
// we consider a transaction in mapModifiedTx and it fails: we can then
// potentially consider it again while walking mapTx.  It's currently
// guaranteed to fail again, but as a belt-and-suspenders check we put it in
// failedTx and avoid re-evaluation, since the re-evaluation would be using
// cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx, CTxMemPool::setEntries &failedTx)
{
    assert (it != mempool.mapTx.end());
    return mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it);
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs(int &nPackagesSelected, int &nDescendantsUpdated)
{
    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator mi = mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != mempool.mapTx.get<ancestor_score>().end() || !mapModifiedTx.empty())
    {
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != mempool.mapTx.get<ancestor_score>().end() &&
                SkipMapTxEntry(mempool.mapTx.project<0>(mi), mapModifiedTx, failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                    CompareTxMemPoolEntryByAncestorFee()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOpsCost = iter->GetSigOpCostWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOpsCost = modit->nSigOpCostWithAncestors;
        }

        if (packageFees < blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOpsCost)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight >
                    nBlockMaxWeight - 4000) {
                // Give up if we're close to full and haven't succeeded in a while
                break;
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final
        if (!TestPackageTransactions(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, sortedEntries);

        for (size_t i=0; i<sortedEntries.size(); ++i) {
            AddToBlock(sortedEntries[i]);
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
}

void IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}

// Ring-fork: In-wallet miner: Hashrate measurement vars
double dHashesPerSec = 0.0;
int64_t nHPSTimerStart = 0;

// Ring-fork: In-wallet miner: Scans nonces looking for a hash with at least some zero bits. The nonce is usually preserved between calls, but periodically or if the nonce is 0xffff0000 or above, the block is rebuilt and nNonce starts over at zero.
bool static ScanHash(CBlockHeader *pblock, uint32_t& nNonce, uint256 *phash) {
    uint256 hash;
    while (true) {
        nNonce++;

        pblock->nNonce = nNonce;
        hash = pblock->GetPowHash();                                // Ring-fork: Seperate block hash and pow hash

        if (hash.ByteAt(31) == 0 && hash.ByteAt(30) == 0) {         // Return the nonce if the hash has at least some zero bits, caller will check if it has enough to reach the target
            memcpy(phash, &hash, 32);
            return true;
        }

        if ((nNonce & 0xffff) == 0)                                 // If nothing found after trying for a while, return -1
            return false;

        if ((nNonce & 0xfff) == 0)                                  // Fire an interrupt to measure hashrate
            boost::this_thread::interruption_point();
    }
}

// Ring-fork: In-wallet miner: Single thread in the thread group
void static MinerThread(const CChainParams& chainparams) {
    LogPrintf("Miner: Thread started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("cpu-miner");

    unsigned int nExtraNonce = 0;
    try {
        // Check P2P exists
        if(!g_connman)
            throw std::runtime_error("P2P unavailable");

        // Get wallet
        JSONRPCRequest request;
        std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
        CWallet* const pwallet = wallet.get();

        if (!EnsureWalletIsAvailable(pwallet, true))
            throw std::runtime_error("Wallet unavailable");

        // Get coinbase script
        std::shared_ptr<CReserveScript> coinbaseScript;
        pwallet->GetScriptForMining(coinbaseScript);

        if (!coinbaseScript)
            throw std::runtime_error("Keypool ran out, please call keypoolrefill first");

        if (coinbaseScript->reserveScript.empty())
            throw std::runtime_error(" No coinbase script available");

        while (true) {
            // Wait for network unless on regtest
            if (!chainparams.MineBlocksOnDemand()) {
                do {
                    if (g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) > 0 && !IsInitialBlockDownload())
                        break;
                    if (IsInitialBlockDownload())
                        LogPrintf("Miner: Initial block download; sleeping for 10 seconds.\n");
                    else
                        LogPrintf("Miner: No peers; sleeping for 10 seconds.\n");
                    MilliSleep(10000);
                } while (true);
            }

            // Create a block
            unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
            CBlockIndex* pindexPrev = chainActive.Tip();

            // Check if this is an RNG block
            if(pindexPrev->nHeight % chainparams.GetConsensus().nRNGBlockSpacing == 0) {
                // Get valid burn transactions from 5 blocks ago
                std::vector<CTxBurn> vBurns; // Placeholder:  Needs implementation to fetch burn transactions
                CBlockIndex* pindexBurn = pindexPrev->GetAncestor(
                    pindexPrev->nHeight - chainparams.GetConsensus().nBurnBlockConfirmations
                );

                // Calculate total burn amount and votes
                CAmount totalBurned = 0;
                uint64_t totalVotes = 0;
                for(const auto& burn : vBurns) {
                    if(burn.amount >= chainparams.GetConsensus().nMinBurnAmount) {
                        totalBurned += burn.amount;
                        totalVotes += burn.amount / chainparams.GetConsensus().nBurnVoteRatio;
                    }
                }

                // Select winner using block hash as seed
                uint256 blockHash = pindexPrev->GetBlockHash();
                uint64_t rand = uint64_t(UintToArith256(blockHash));
                uint64_t winningVote = rand % totalVotes;

                // Find winning burn transaction
                uint64_t voteCount = 0;
                CTxBurn* winner = nullptr;
                for(auto& burn : vBurns) {
                    uint64_t votes = burn.amount / chainparams.GetConsensus().nBurnVoteRatio;
                    if(voteCount <= winningVote && winningVote < voteCount + votes) {
                        winner = &burn;
                        break;
                    }
                    voteCount += votes;
                }
            }

            std::unique_ptr<CBlockTemplate> pblocktemplate(BlockAssembler(Params()).CreateNewBlock(coinbaseScript->reserveScript));
            if (!pblocktemplate.get())
                throw std::runtime_error("Couldn't get block template. Probably keypool ran out; please call keypoolrefill before restarting the mining thread");

            CBlock *pblock = &pblocktemplate->block;
            {
                LOCK(cs_main);
                IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);
            }

            // Scan for a good nonce
            LogPrintf("Miner: Running (%u transactions in block)\n", pblock->vtx.size());
            int64_t nStart = GetTime();
            arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);
            uint256 hash;
            uint32_t nNonce = 0;
            uint32_t nOldNonce = 0;
            while (true) {
                bool fFound = ScanHash(pblock, nNonce, &hash);
                uint32_t nHashesDone = nNonce - nOldNonce;
                nOldNonce = nNonce;

                if (fFound) {                                       // Found a potential (has at least some zeroes)
                    if (UintToArith256(hash) <= hashTarget) {       // Found a good solution :)
                        pblock->nNonce = nNonce;
                        assert(hash == pblock->GetPowHash());       // Ring-fork: Seperate block hash and pow hash

                        SetThreadPriority(THREAD_PRIORITY_NORMAL);
                        LogPrintf("Miner: BLOCK FOUND.\nhash: %s\ntarget: %s\n", hash.GetHex(), hashTarget.GetHex());

                        // Make sure the new block's not stale
                        {
                            LOCK(cs_main);
                            if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash()) {
                                LogPrintf("Miner: WARNING: Generated block is stale.\n");
                                break;
                            }
                        }

                        // Process this block the same as if we had received it from another node
                        std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(*pblock);
                        if (!ProcessNewBlock(Params(), shared_pblock, true, nullptr)) {
                            LogPrintf("Miner: WARNING: Block was not accepted.\n");
                            break;
                        }

                        SetThreadPriority(THREAD_PRIORITY_LOWEST);
                        coinbaseScript->KeepScript();

                        uiInterface.NotifyBlockFound(); // Fire UI notification

                        // In regression test mode, stop mining after a block is found.
                        if (chainparams.MineBlocksOnDemand())
                            throw boost::thread_interrupted();

                        break;
                    }
                }

                // Meter hashes/sec
                static int64_t nHashCounter;
                if (nHPSTimerStart == 0) {
                    nHPSTimerStart = GetTimeMillis();
                    nHashCounter = 0;
                } else
                    nHashCounter += nHashesDone;
                if (GetTimeMillis() - nHPSTimerStart > 4000) {
                    static CCriticalSection cs;
                    {
                        LOCK(cs);
                        if (GetTimeMillis() - nHPSTimerStart > 4000) {
                            dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                            nHPSTimerStart = GetTimeMillis();
                            nHashCounter = 0;
                            static int64_t nLogTime;
                            if (GetTime() - nLogTime > 30 * 60) {
                                nLogTime = GetTime();
                                LogPrintf("Miner: Hashrate: %6.1f khash/s\n", dHashesPerSec/1000.0);
                            }
                        }
                    }
                }

                // Check whether to break or continue
                boost::this_thread::interruption_point();
                if (!chainparams.MineBlocksOnDemand() && g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0)   // No peers and not in regtest
                    break;
                if (nNonce >= 0xffff0000)                                                                           // Nonce space maxed out
                    break;
                if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)        // Transactions updated, or been trying a while
                    break;
                if (pindexPrev != chainActive.Tip())                                                                // Tip changed
                    break;
                if (UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev) < 0)                                 // Clock ran backwards
                    break;
                if (chainparams.GetConsensus().fPowAllowMinDifficultyBlocks)                                        // Changing pblock->nTime can change work required on testnet due to diff reset
                    hashTarget.SetCompact(pblock->nBits);
            }
        }
    }
    catch (const boost::thread_interrupted&) {
        LogPrintf("Miner: Thread terminated\n");
        throw;
    }
    catch (const std::runtime_error &e) {
        LogPrintf("Miner: Runtime error: %s\n", e.what());
        return;
    }
}

// Ring-fork: In-wallet miner: Mining thread controller
void MineCoins(bool fGenerate, int nThreads, const CChainParams& chainparams) {
    static boost::thread_group* minerThreads = NULL;

    if (nThreads < 0)                           // Use all cores if -1 specified
        nThreads = GetNumCores();

    if (minerThreads != NULL) {                 // Kill any existing miner threads
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    uiInterface.NotifyGenerateChanged();        // Fire UI notification

    dHashesPerSec = 0;

    if (nThreads == 0 || !fGenerate)
        return;

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++)          // Start threads
        minerThreads->create_thread(boost::bind(&MinerThread, boost::cref(chainparams)));
}

// Ring-fork: Hive: Dwarf management thread
void DwarfMaster(const CChainParams& chainparams) {
    const Consensus::Params& consensusParams = chainparams.GetConsensus();

    LogPrintf("DwarfMaster: Thread started\n");
    RenameThread("hive-dwarfmaster");

    int height;
    {
        LOCK(cs_main);
        height = chainActive.Tip()->nHeight;
    }

    try {
        while (true) {
            // Ring-fork: Hive: Mining optimisations: Parameterised sleep time
            int sleepTime = std::max((int64_t) 1, gArgs.GetArg("-hivecheckdelay", DEFAULT_HIVE_CHECK_DELAY));
            MilliSleep(sleepTime);
            int newHeight;
            {
                LOCK(cs_main);
                newHeight = chainActive.Tip()->nHeight;
            }
            if (newHeight != height) {
                // Height changed; release the dwarves!
                height = newHeight;
                try {
                    BusyDwarves(consensusParams, height);
                } catch (const std::runtime_error &e) {
                    LogPrintf("! DwarfMaster: Error: %s\n", e.what());
                }
            }
        }
    } catch (const boost::thread_interrupted&) {
        LogPrintf("!!! DwarfMaster: FATAL: Thread interrupted\n");
        throw;
    }
}

// Ring-fork: Hive: Mining optimisations: Thread to signal abort on new block
void AbortWatchThread(int height) {
    // Loop until any exit condition
    while (true) {
        // Yield to OS
        MilliSleep(1);

        // Check pre-existing abort conditions
        if (solutionFound.load() || earlyAbort.load())
            return;

        // Get tip height, keeping lock scope as short as possible
        int newHeight;
        {
            LOCK(cs_main);
            newHeight = chainActive.Tip()->nHeight;
        }

        // Check for abort from tip height change
        if (newHeight != height) {
            //LogPrintf("*** ABORT FIRE\n");
            earlyAbort.store(true);
            return;
        }
    }
}

// Ring-fork: Hive: Mining optimisations: Thread to check a single bin
void CheckBin(int threadID, std::vector<CDwarfRange> bin, std::string deterministicRandString, arith_uint256 dwarfHashTarget) {
    // Iterate over ranges in this bin
    int checkCount = 0;
    for (std::vector<CDwarfRange>::const_iterator it = bin.begin(); it != bin.end(); it++) {
        CDwarfRange dwarfRange = *it;
        //LogPrintf("THREAD #%i: Checking %i-%i in %s\n", threadID, dwarfRange.offset, dwarfRange.offset + dwarfRange.count - 1, dwarfRange.txid);
        // Iterate over dwarves in this range
        for (int i = dwarfRange.offset; i < dwarfRange.offset + dwarfRange.count; i++) {
            // Check abort conditions (Only every N dwarves. The atomic load is expensive, but much cheaper than a mutex - esp on Windows, see https://www.arangodb.com/2015/02/comparing-atomic-mutex-rwlocks/)
            if(checkCount++ % 1000 == 0) {
                if (solutionFound.load() || earlyAbort.load()) {
                    //LogPrintf("THREAD #%i: Solution found elsewhere or early abort requested, ending early\n", threadID);
                    return;
                }
            }
            // Hash the dwarf
            arith_uint256 dwarfHash(CBlockHeader::MinotaurHashArbitrary(std::string(deterministicRandString + dwarfRange.txid + std::to_string(i)).c_str()).ToString());
            dwarfHash = arith_uint256(CBlockHeader::MinotaurHashArbitrary(dwarfHash.ToString().c_str()).ToString());

            // Compare to target and write out result if successful
            if (dwarfHash < dwarfHashTarget) {
                //LogPrintf("THREAD #%i: Solution found, returning\n", threadID);
                LOCK(cs_solution_vars);                                 // Expensive mutex only happens at write-out
                solutionFound.store(true);
                solvingRange = dwarfRange;
                solvingDwarf = i;
                return;
            }
        }
    }
    //LogPrintf("THREAD #%i: Out of tasks\n", threadID);
}

// Ring-fork: Hive: Attempt to mint the next block
bool BusyDwarves(const Consensus::Params& consensusParams, int height) {
    bool verbose = LogAcceptCategory(BCLog::HIVE);

    CBlockIndex* pindexPrev = chainActive.Tip();
    assert(pindexPrev != nullptr);

    // Sanity checks
    if(!g_connman) {
        LogPrint(BCLog::HIVE, "BusyDwarves: Skipping hive check: Peer-to-peer functionality missing or disabled\n");
        return false;
    }
    if (g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0) {
        LogPrint(BCLog::HIVE, "BusyDwarves: Skipping hive check (not connected)\n");
        return false;
    }
    if (IsInitialBlockDownload()) {
        LogPrint(BCLog::HIVE, "BusyDwarves: Skipping hive check (in initial block download)\n");
        return false;
    }
    if (height < consensusParams.lastInitialDistributionHeight + consensusParams.slowStartBlocks) {
        LogPrint(BCLog::HIVE, "BusyDwarves: Skipping hive check (slow start has not finished)\n");
        return false;
    }

    // Check that there aren't too many Hive blocks since the last Pow block
    int hiveBlocksSincePow = 0;
    CBlockIndex* pindexTemp = pindexPrev;
    while (pindexTemp->GetBlockHeader().IsPopMined(consensusParams) || pindexTemp->GetBlockHeader().IsHiveMined(consensusParams)) {
        if (pindexTemp->GetBlockHeader().IsHiveMined(consensusParams))
            hiveBlocksSincePow++;

        assert(pindexTemp->pprev);
        pindexTemp = pindexTemp->pprev;
    }
    if (hiveBlocksSincePow >= consensusParams.maxConsecutiveHiveBlocks) {
        LogPrintf("BusyDwarves: Skipping hive check (max Hive blocks without a POW block reached)\n");
        return false;
    }

    // Get wallet
    JSONRPCRequest request;
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, true)) {
        LogPrint(BCLog::HIVE, "BusyDwarves: Skipping hive check (wallet unavailable)\n");
        return false;
    }
    if (pwallet->IsLocked()) {
        LogPrint(BCLog::HIVE, "BusyDwarves: Skipping hive check, wallet is locked\n");
        return false;
    }

    LogPrintf("********************* Hive: Dwarves at work *********************\n");

    // Find deterministicRandString
    std::string deterministicRandString = GetDeterministicRandString(pindexPrev);
    if (verbose) LogPrintf("BusyDwarves: deterministicRandString   = %s\n", deterministicRandString);

    // Find dwarfHashTarget
    arith_uint256 dwarfHashTarget;
    dwarfHashTarget.SetCompact(GetNextHiveWorkRequired(pindexPrev, consensusParams));
    if (verbose) LogPrintf("BusyDwarves: dwarfHashTarget             = %s\n", dwarfHashTarget.ToString());

    // Find bin size
    std::vector<CDwarfCreationTransactionInfo> potentialDcts = pwallet->GetDCTs(false, false, consensusParams);
    std::vector<CDwarfCreationTransactionInfo> dcts;
    int totalDwarves = 0;
    for (std::vector<CDwarfCreationTransactionInfo>::const_iterator it = potentialDcts.begin(); it != potentialDcts.end(); it++) {
        CDwarfCreationTransactionInfo dct = *it;
        if (dct.dwarfStatus != "mature")
            continue;
        dcts.push_back(dct);
        totalDwarves += dct.dwarfCount;
    }

    if (totalDwarves == 0) {
        LogPrint(BCLog::HIVE, "BusyDwarves: No mature dwarves found\n");
        return false;
    }

    int coreCount = GetNumCores();
    int threadCount = gArgs.GetArg("-hivecheckthreads", DEFAULT_HIVE_THREADS);
    if (threadCount == -2)
        threadCount = std::max(1, coreCount - 1);
    else if (threadCount < 0 || threadCount > coreCount)
        threadCount = coreCount;
    else if (threadCount == 0)
        threadCount = 1;

    int dwarvesPerBin = ceil(totalDwarves / (float)threadCount);  // We want to check this many dwarves per thread

    // Bin the dwarves according to desired thead count
    if (verbose) LogPrint(BCLog::HIVE, "BusyDwarves: Binning %i dwarves in %i bins (%i dwarves per bin)\n", totalDwarves, threadCount, dwarvesPerBin);
    std::vector<CDwarfCreationTransactionInfo>::const_iterator dctIterator = dcts.begin();
    CDwarfCreationTransactionInfo dct = *dctIterator;
    std::vector<std::vector<CDwarfRange>> dwarfBins;
    int dwarfOffset = 0;                                    // Track offset in current DCT
    while(dctIterator != dcts.end()) {                      // Until we're out of DCTs
        std::vector<CDwarfRange> currentBin;                // Create a new bin
        int dwarvesInBin = 0;
        while (dctIterator != dcts.end()) {                 // Keep filling it until full
            int spaceLeft = dwarvesPerBin - dwarvesInBin;
            if (dct.dwarfCount - dwarfOffset <= spaceLeft) {  // If there's room, add all the dwarves from this DCT...
                CDwarfRange range = {dct.txid, dct.rewardAddress, dct.communityContrib, dwarfOffset, dct.dwarfCount - dwarfOffset};
                currentBin.push_back(range);

                dwarvesInBin += dct.dwarfCount - dwarfOffset;
                dwarfOffset = 0;

                do {                                        // ... and iterate to next DCT
                    dctIterator++;
                    if (dctIterator == dcts.end())
                        break;
                    dct = *dctIterator;
                } while (dct.dwarfStatus != "mature");
            } else {                                        // Can't fit the whole thing to current bin; add what we can fit and let the rest go in next bin
                CDwarfRange range = {dct.txid, dct.rewardAddress, dct.communityContrib, dwarfOffset, spaceLeft};
                currentBin.push_back(range);
                dwarfOffset += spaceLeft;
                break;
            }
        }
        dwarfBins.push_back(currentBin);
    }

    // Create a worker thread for each bin
    if (verbose) LogPrintf("BusyDwarves: Running bins\n");
    solutionFound.store(false);
    earlyAbort.store(false);
    std::vector<std::vector<CDwarfRange>>::const_iterator dwarfBinIterator = dwarfBins.begin();
    std::vector<boost::thread> binThreads;
    int64_t checkTime = GetTimeMillis();
    int binID = 0;
    while (dwarfBinIterator != dwarfBins.end()) {
        std::vector<CDwarfRange> dwarfBin = *dwarfBinIterator;

        if (verbose) {
            LogPrintf("BusyDwarves: Bin #%i\n", binID);
            std::vector<CDwarfRange>::const_iterator dwarfRangeIterator = dwarfBin.begin();
            while (dwarfRangeIterator != dwarfBin.end()) {
                CDwarfRange dwarfRange = *dwarfRangeIterator;
                LogPrintf("offset = %i, count = %i, txid = %s\n", dwarfRange.offset, dwarfRange.count, dwarfRange.txid);
                dwarfRangeIterator++;
            }
        }
        binThreads.push_back(boost::thread(CheckBin, binID++, dwarfBin, deterministicRandString, dwarfHashTarget));   // Spawn the thread

        dwarfBinIterator++;
    }

    // Add an extra thread to watch external abort conditions (eg new incoming block)
    bool useEarlyAbortThread = gArgs.GetBoolArg("-hiveearlyout", DEFAULT_HIVE_EARLY_OUT);
    if (verbose && useEarlyAbortThread)
        LogPrintf("BusyDwarves: Will use early-abort thread\n");

    boost::thread* earlyAbortThread;
    if (useEarlyAbortThread)
        earlyAbortThread = new boost::thread(AbortWatchThread, height);

    // Wait for bin worker threads to find a solution or abort (in which case the others will all stop), or to run out of dwarves
    for(auto& t:binThreads)
        t.join();

    checkTime = GetTimeMillis() - checkTime;

    // Handle early aborts
    if (useEarlyAbortThread) {
        if (earlyAbort.load()) {
            LogPrintf("BusyDwarves: Chain state changed (check aborted after %ims)\n", checkTime);
            return false;
        } else {
            // We didn't abort; stop abort thread now
            earlyAbort.store(true);
            earlyAbortThread->join();
        }
    }

    // Check if a solution was found
    if (!solutionFound.load()) {
        LogPrintf("BusyDwarves: No dwarf meets hash target (%i dwarves checked with %i threads in %ims)\n", totalDwarves, threadCount, checkTime);
        return false;
    }
    LogPrintf("BusyDwarves: Dwarf meets hash target (check aborted after %ims). Solution with dwarf #%i from BCT %s. Honey address is %s.\n", checkTime, solvingDwarf, solvingRange.txid, solvingRange.rewardAddress);

    // Assemble the Hive proof script
    std::vector<unsigned char> messageProofVec;
    std::vector<unsigned char> txidVec(solvingRange.txid.begin(), solvingRange.txid.end());
    CScript hiveProofScript;
    uint32_t dctHeight;
    {   // Don't lock longer than needed
        LOCK2(cs_main, pwallet->cs_wallet);

        CTxDestination dest = DecodeDestination(solvingRange.rewardAddress);
        if (!IsValidDestination(dest)) {
            LogPrintf("BusyDwarves: Honey destination invalid\n");
            return false;
        }

        const CKeyID *keyID = boost::get<CKeyID>(&dest);
        if (!keyID) {
            LogPrintf("BusyDwarves: Wallet doesn't have privkey for honey destination\n");
            return false;
        }

        CKey key;
        if (!pwallet->GetKey(*keyID, key)) {
            LogPrintf("BusyDwarves: Privkey unavailable\n");
            return false;
        }

        CHashWriter ss(SER_GETHASH, 0);
        ss << deterministicRandString;
        uint256 mhash = ss.GetHash();
        if (!key.SignCompact(mhash, messageProofVec)) {
            LogPrintf("BusyDwarves: Couldn't sign the dwarf proof!\n");
            return false;
        }
        if (verbose) LogPrintf("BusyDwarves: messageSig                = %s\n", HexStr(&messageProofVec[0], &messageProofVec[messageProofVec.size()]));

        COutPoint out(uint256S(solvingRange.txid), 0);
        Coin coin;
        if (!pcoinsTip || !pcoinsTip->GetCoin(out, coin)) {
            LogPrintf("BusyDwarves: Couldn't get the bct utxo!\n");
            return false;
        }
        dctHeight = coin.nHeight;
    }

    unsigned char dwarfNonceEncoded[4];
    WriteLE32(dwarfNonceEncoded, solvingDwarf);
    std::vector<unsigned char> dwarfNonceVec(dwarfNonceEncoded, dwarfNonceEncoded + 4);

    unsigned char dctHeightEncoded[4];
    WriteLE32(dctHeightEncoded, dctHeight);
    std::vector<unsigned char> dctHeightVec(dctHeightEncoded, dctHeightEncoded + 4);

    opcodetype communityContribFlag = solvingRange.communityContrib ? OP_TRUE : OP_FALSE;
    hiveProofScript << OP_RETURN << OP_DWARF << dwarfNonceVec << dctHeightVec << communityContribFlag << txidVec << messageProofVec;

    // Create honey script from honey address
    CScript rewardScript = GetScriptForDestination(DecodeDestination(solvingRange.rewardAddress));

    // Create a Hive block
    std::unique_ptr<CBlockTemplate> pblocktemplate(BlockAssembler(Params()).CreateNewBlock(rewardScript, &hiveProofScript));
    if (!pblocktemplate.get()) {
        LogPrintf("BusyDwarves: Couldn't create block\n");
        return false;
    }
    CBlock *pblock = &pblocktemplate->block;
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);  // Calc the merkle root

    // Make sure the new block's not stale
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash()) {
            LogPrintf("BusyDwarves: Generated block is stale.\n");
            return false;
        }
    }

    if (verbose) {
        LogPrintf("BusyDwarves: Block created:\n");
        LogPrintf("%s",pblock->ToString());
    }

    // Commit and propagate the block
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(*pblock);
    if (!ProcessNewBlock(Params(), shared_pblock, true, nullptr)) {
        LogPrintf("BusyDwarves: Block wasn't accepted\n");
        return false;
    }

    LogPrintf("BusyDwarves: ** Block mined\n");
    return true;
}