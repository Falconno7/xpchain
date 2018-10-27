#include <kernel.h>
#include <db.h>
#include <uint256.h>
#include <chainparams.h>
#include <util.h>
#include <wallet/wallet.h>
#include <init.h>
#include <timedata.h>
#include <txdb.h>
#include <validation.h>
#include <index/txindex.h>

using namespace std;

struct CDiskIndex;


// ppcoin kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.offset + txPrev.nTime + txPrev.vout.n + nTime) < bnTarget * nCoinDayWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: 
//       (v0.3) scrambles computation to make it very difficult to precompute
//              future proof-of-stake at the time of the coin's confirmation
//       (v0.2) nBits (deprecated): encodes all past block timestamps
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.offset: offset of txPrev inside block, to reduce the chance of 
//                  nodes generating coinstake at the same time
//   txPrev.nTime: reduce the chance of nodes generating coinstake at the same
//                 time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset, const CTxOut& txOutPrev, const COutPoint& prevout, uint32_t nTimeTx, uint256& hashProofOfStake)
{
    uint32_t nTimeBlockFrom = blockFrom.GetBlockTime();
    if (nTimeBlockFrom + Params().GetConsensus().nStakeMinAge > nTimeTx) // Min age requirement
        return error("CheckStakeKernelHash() : min age violation");

    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);
    // v0.3 protocol kernel hash weight starts from 0 at the 30-day min age
    // this change increases active coins participating the hash and helps
    // to secure the network when proof-of-stake difficulty is low
    int64_t nTimeWeight = min((int64_t)nTimeTx - nTimeBlockFrom, Params().GetConsensus().nStakeMaxAge) - Params().GetConsensus().nStakeMinAge;
    arith_uint256 bnCoinDayWeight = arith_uint256(txOutPrev.nValue) * nTimeWeight / COIN / (24 * 60 * 60);
    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);

    ss << nBits << nTimeBlockFrom << nTxPrevOffset << nTimeBlockFrom << prevout.n << nTimeTx;

    hashProofOfStake = Hash(ss.begin(), ss.end());

    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) > bnCoinDayWeight * bnTargetPerCoinDay)
        return false;

    return true;
}

bool CheckProofOfStake(CValidationState& state, const CTransactionRef& tx, unsigned int nBits, uint256& hashProofOfStake, unsigned int nBlockTime)
{
    const CTxIn& txin = tx->vin[0];

    CTransactionRef txTmp;
    uint256 hashBlock;

    if (!GetTransaction(txin.prevout.hash, txTmp, Params().GetConsensus(), hashBlock))
        return false;
        //return state.DoS(1, error("CheckProofOfStake() : txPrev not found")); // previous transaction not in main chain, may occur during initial download

    // Verify signature
    PrecomputedTransactionData txdata(*tx);
    if (!CScriptCheck(txTmp->vout[tx->vin[0].prevout.n], *tx, 0, 0, true, &txdata)())
        return false;
        //return state.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx->GetHash().ToString()));

    // Get transaction index for the previous transaction
    CDiskTxPos postx;
    CBlockHeader header;
    CTransactionRef txPrev;
    if(!g_txindex->FindTx(txin.prevout.hash,postx,header,txPrev))
        return false;

    if (!CheckStakeKernelHash(nBits, header, postx.nTxOffset + sizeof(CBlockHeader), txPrev->vout[txin.prevout.n], txin.prevout, nBlockTime, hashProofOfStake))
        return false;
        //return state.DoS(1, error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx->GetHash().ToString(), hashProofOfStake.ToString())); // may occur during initial download or if behind on block chain sync

    return true;
    
}