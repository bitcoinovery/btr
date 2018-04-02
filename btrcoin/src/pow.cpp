// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pow.h"

#include "arith_uint256.h"
#include "chain.h"
#include "chainparams.h"
#include "primitives/block.h"
#include "uint256.h"
#include "util.h"

#include <math.h>

unsigned int static KimotoGravityWell(const CBlockIndex* pindexLast, const Consensus::Params& params) {
    const CBlockIndex *BlockLastSolved = pindexLast;
    const CBlockIndex *BlockReading = pindexLast;
    uint64_t PastBlocksMass = 0;
    int64_t PastRateActualSeconds = 0;
    int64_t PastRateTargetSeconds = 0;
    double PastRateAdjustmentRatio = double(1);
    arith_uint256 PastDifficultyAverage;
    arith_uint256 PastDifficultyAveragePrev;
    double EventHorizonDeviation;
    double EventHorizonDeviationFast;
    double EventHorizonDeviationSlow;

    uint64_t pastSecondsMin = params.nPowTargetTimespan * 0.025;
    uint64_t pastSecondsMax = params.nPowTargetTimespan * 7;
    uint64_t PastBlocksMin = pastSecondsMin / params.nPowTargetSpacing;
    uint64_t PastBlocksMax = pastSecondsMax / params.nPowTargetSpacing;

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || (uint64_t)BlockLastSolved->nHeight < PastBlocksMin) { return UintToArith256(params.powLimit).GetCompact(); }

    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
        PastBlocksMass++;

        PastDifficultyAverage.SetCompact(BlockReading->nBits);
        if (i > 1) {
            // handle negative arith_uint256
            if(PastDifficultyAverage >= PastDifficultyAveragePrev)
                PastDifficultyAverage = ((PastDifficultyAverage - PastDifficultyAveragePrev) / i) + PastDifficultyAveragePrev;
            else
                PastDifficultyAverage = PastDifficultyAveragePrev - ((PastDifficultyAveragePrev - PastDifficultyAverage) / i);
        }
        PastDifficultyAveragePrev = PastDifficultyAverage;

        PastRateActualSeconds = BlockLastSolved->GetBlockTime() - BlockReading->GetBlockTime();
        PastRateTargetSeconds = params.nPowTargetSpacing * PastBlocksMass;
        PastRateAdjustmentRatio = double(1);
        if (PastRateActualSeconds < 0) { PastRateActualSeconds = 0; }
        if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
            PastRateAdjustmentRatio = double(PastRateTargetSeconds) / double(PastRateActualSeconds);
        }
        EventHorizonDeviation = 1 + (0.7084 * pow((double(PastBlocksMass)/double(28.2)), -1.228));
        EventHorizonDeviationFast = EventHorizonDeviation;
        EventHorizonDeviationSlow = 1 / EventHorizonDeviation;

        if (PastBlocksMass >= PastBlocksMin) {
                if ((PastRateAdjustmentRatio <= EventHorizonDeviationSlow) || (PastRateAdjustmentRatio >= EventHorizonDeviationFast))
                { assert(BlockReading); break; }
        }
        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    arith_uint256 bnNew(PastDifficultyAverage);
    if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
        bnNew *= PastRateActualSeconds;
        bnNew /= PastRateTargetSeconds;
    }

    if (bnNew > UintToArith256(params.powLimit)) {
        bnNew = UintToArith256(params.powLimit);
    }

    return bnNew.GetCompact();
}

unsigned int static DarkGravityWave(const CBlockIndex* pindexLast, const Consensus::Params& params) {
    /* current difficulty formula, btr - DarkGravity v3, written by Evan Duffield - evan@btr.org */
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    int64_t nPastBlocks = 24;

    // make sure we have at least (nPastBlocks + 1) blocks, otherwise just return powLimit
    if (!pindexLast || pindexLast->nHeight < nPastBlocks) {
        return bnPowLimit.GetCompact();
    }

    const CBlockIndex *pindex = pindexLast;
    arith_uint256 bnPastTargetAvg;

    for (unsigned int nCountBlocks = 1; nCountBlocks <= nPastBlocks; nCountBlocks++) {
        arith_uint256 bnTarget = arith_uint256().SetCompact(pindex->nBits);
        if (nCountBlocks == 1) {
            bnPastTargetAvg = bnTarget;
        } else {
            // NOTE: that's not an average really...
            bnPastTargetAvg = (bnPastTargetAvg * nCountBlocks + bnTarget) / (nCountBlocks + 1);
        }

        if(nCountBlocks != nPastBlocks) {
            assert(pindex->pprev); // should never fail
            pindex = pindex->pprev;
        }
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

    if (bnNew > bnPowLimit) {
        bnNew = bnPowLimit;
    }

    return bnNew.GetCompact();
}

unsigned int GetNextWorkRequiredBTC(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Genesis block
    if (pindexLast == NULL)
        return nProofOfWorkLimit;

    // Only change once per interval

    int iRound = 4;
    if ((pindexLast->nHeight+1) % iRound != 0)
    {
       return pindexLast->nBits;
    }


    // Go back by what we want to be 1 day worth of blocks
    int nHeightFirst = pindexLast->nHeight - (iRound/*params.DifficultyAdjustmentInterval()*/-1);
    assert(nHeightFirst >= 0);
    const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
    assert(pindexFirst);

   return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);

}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    // Most recent algo first
   /* if (pindexLast->nHeight + 1 >= params.nPowDGWHeight) {
        return DarkGravityWave(pindexLast, params);
    }
    else  if (pindexLast->nHeight + 1 >= params.nPowKGWHeight) {
        return KimotoGravityWell(pindexLast, params);
    }*/

    if (pindexLast->nHeight + 1 >= params.nPowDGWHeight) {
        return DarkGravityWave(pindexLast, params);
    }
    else{
        return GetNextWorkRequiredBTC(pindexLast, pblock, params);
    }
}

// for DIFF_BTC only!
unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    LogPrintf("  nActualTimespan = %d  before bounds\n", nActualTimespan);
    do
    {
        if(nActualTimespan < params.nPowTargetTimespan/50)
        {
            nActualTimespan = params.nPowTargetTimespan/16;
            break;
        }
//        if(nActualTimespan < params.nPowTargetTimespan/40)
//        {
//            nActualTimespan = params.nPowTargetTimespan/18;
//            break;
//        }
//        if(nActualTimespan < params.nPowTargetTimespan/30)
//        {
//            nActualTimespan = params.nPowTargetTimespan/17;
//            break;
//        }
//        if(nActualTimespan < params.nPowTargetTimespan/20)
//        {
//            nActualTimespan = params.nPowTargetTimespan/16;
//            break;
//        }
//        if(nActualTimespan < params.nPowTargetTimespan/15)
//        {
//            nActualTimespan = params.nPowTargetTimespan/10;
//            break;
//        }
//        if(nActualTimespan < params.nPowTargetTimespan/10)
//        {
//            nActualTimespan = params.nPowTargetTimespan/8;
//            break;
//        }
//        if(nActualTimespan < params.nPowTargetTimespan/6)
//        {
//            nActualTimespan = params.nPowTargetTimespan/6;
//            break;
//        }
        if (nActualTimespan < params.nPowTargetTimespan/4)
        {
            nActualTimespan = params.nPowTargetTimespan/4;
            break;
        }

        if (nActualTimespan < params.nPowTargetTimespan/3)
        {
            nActualTimespan = params.nPowTargetTimespan/3;
            break;
        }

        if (nActualTimespan < params.nPowTargetTimespan/2)
        {
            nActualTimespan = params.nPowTargetTimespan/2;
            break;
        }

        if (nActualTimespan > params.nPowTargetTimespan*50)
         {
            nActualTimespan = params.nPowTargetTimespan*16;
            break;
        }
//        if (nActualTimespan > params.nPowTargetTimespan*40)
//         {
//            nActualTimespan = params.nPowTargetTimespan*18;
//            break;
//        }
//        if (nActualTimespan > params.nPowTargetTimespan*30)
//         {
//            nActualTimespan = params.nPowTargetTimespan*17;
//            break;
//        }
//        if (nActualTimespan > params.nPowTargetTimespan*20)
//         {
//            nActualTimespan = params.nPowTargetTimespan*16;
//            break;
//        }
//        if (nActualTimespan > params.nPowTargetTimespan*15)
//         {
//            nActualTimespan = params.nPowTargetTimespan*10;
//            break;
//         }
//        if (nActualTimespan > params.nPowTargetTimespan*10)
//         {
//             nActualTimespan = params.nPowTargetTimespan*8;
//             break;
//         }
//        if (nActualTimespan > params.nPowTargetTimespan*6)
//        {
//            nActualTimespan = params.nPowTargetTimespan*6;
//            break;
//        }
        if (nActualTimespan > params.nPowTargetTimespan*4)
        {
            nActualTimespan = params.nPowTargetTimespan*4;
            break;
        }
        if (nActualTimespan > params.nPowTargetTimespan*3)
        {
            nActualTimespan = params.nPowTargetTimespan*3;
            break;
        }
        if (nActualTimespan > params.nPowTargetTimespan*2)
        {
            nActualTimespan = params.nPowTargetTimespan*2;
            break;
        }

        break;
    }while(1);
    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    arith_uint256 bnOld;
    bnNew.SetCompact(pindexLast->nBits);
    bnOld = bnNew;
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    /// debug print
    LogPrint("bench","GetNextWorkRequired RETARGET\n");
    LogPrint("bench","params.nPowTargetTimespan = %d    nActualTimespan = %d\n", params.nPowTargetTimespan, nActualTimespan);
    LogPrint("bench","Before: %08x  %s\n", pindexLast->nBits, bnOld.ToString());
    LogPrint("bench","After:  %08x  %s\n", bnNew.GetCompact(), bnNew.ToString());

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return error("CheckProofOfWork(): nBits below minimum work");

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return error("CheckProofOfWork(): hash doesn't match nBits");

    return true;
}

arith_uint256 GetBlockProof(const CBlockIndex& block)
{
    arith_uint256 bnTarget;
    bool fNegative;
    bool fOverflow;
    bnTarget.SetCompact(block.nBits, &fNegative, &fOverflow);
    if (fNegative || fOverflow || bnTarget == 0)
        return 0;
    // We need to compute 2**256 / (bnTarget+1), but we can't represent 2**256
    // as it's too large for a arith_uint256. However, as 2**256 is at least as large
    // as bnTarget+1, it is equal to ((2**256 - bnTarget - 1) / (bnTarget+1)) + 1,
    // or ~bnTarget / (nTarget+1) + 1.
    return (~bnTarget / (bnTarget + 1)) + 1;
}

int64_t GetBlockProofEquivalentTime(const CBlockIndex& to, const CBlockIndex& from, const CBlockIndex& tip, const Consensus::Params& params)
{
    arith_uint256 r;
    int sign = 1;
    if (to.nChainWork > from.nChainWork) {
        r = to.nChainWork - from.nChainWork;
    } else {
        r = from.nChainWork - to.nChainWork;
        sign = -1;
    }
    r = r * arith_uint256(params.nPowTargetSpacing) / GetBlockProof(tip);
    if (r.bits() > 63) {
        return sign * std::numeric_limits<int64_t>::max();
    }
    return sign * r.GetLow64();
}