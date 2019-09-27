// Copyright (c) 2017 - 2019 - The SmartCash Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "smartrewards/rewardsdb.h"

#include "chainparams.h"
#include "hash.h"
#include "pow.h"
#include "uint256.h"
#include "ui_interface.h"
#include "init.h"
#include "rewards.h"
#include "rewardsdb.h"

#include <stdint.h>

#include <boost/thread.hpp>
#include "leveldb/include/leveldb/db.h"

using namespace std;

static const char DB_ROUND_CURRENT = 'R';
static const char DB_ROUND = 'r';
static const char DB_ROUND_SNAPSHOT = 's';

static const char DB_REWARD_ENTRY = 'E';
static const char DB_BLOCK = 'B';
static const char DB_BLOCK_LAST = 'b';
static const char DB_TX_HASH = 't';

static const char DB_VERSION = 'V';

CSmartRewardsDB::CSmartRewardsDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "rewards", nCacheSize, fMemory, fWipe) {

    if( !Exists(DB_VERSION) ){
        Write(DB_VERSION, REWARDS_DB_VERSION);
    }

}

bool CSmartRewardsDB::Verify(int& lastBlockHeight)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());
    CSmartRewardBlock last;
    uint8_t dbVersion;

    lastBlockHeight = 0;

    if( !Read(DB_VERSION, dbVersion) ){
        LogPrintf("CSmartRewards::Verify() Could't read DB_VERSION\n");
        return false;
    }

    if( dbVersion < REWARDS_DB_VERSION ){
        LogPrintf("CSmartRewards::Verify() DB_VERSION too old.\n");
        return false;
    }

    if(!ReadLastBlock(last)){
        LogPrintf("CSmartRewards::Verify() No block here yet\n");
        return true;
    }

    lastBlockHeight = last.nHeight;

    return true;
}

bool CSmartRewardsDB::ReadBlock(const int nHeight, CSmartRewardBlock &block)
{
    return Read(make_pair(DB_BLOCK,nHeight), block);
}

bool CSmartRewardsDB::ReadLastBlock(CSmartRewardBlock &block)
{
    return Read(DB_BLOCK_LAST, block);
}

bool CSmartRewardsDB::ReadTransaction(const uint256 hash, CSmartRewardTransaction &transaction)
{
    return Read(make_pair(DB_TX_HASH,hash), transaction);
}

bool CSmartRewardsDB::ReadRound(const int16_t number, CSmartRewardRound &round)
{
    return Read(make_pair(DB_ROUND,number), round);
}

bool CSmartRewardsDB::ReadRounds(CSmartRewardRoundList &vect)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(DB_ROUND);

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char,int16_t> key;
        if (pcursor->GetKey(key) && key.first == DB_ROUND) {
            CSmartRewardRound nValue;
            if (pcursor->GetValue(nValue)) {
                vect.push_back(nValue);
                pcursor->Next();
            } else {
                return error("failed to get reward round");
            }
        } else {
            break;
        }
    }

    return true;
}

bool CSmartRewardsDB::ReadCurrentRound(CSmartRewardRound &round)
{
    return Read(DB_ROUND_CURRENT, round);
}

bool CSmartRewardsDB::ReadRewardEntry(const CSmartAddress &id, CSmartRewardEntry &entry)
{
    return Read(make_pair(DB_REWARD_ENTRY,id), entry);
}

bool CSmartRewardsDB::SyncCached(const CSmartRewardsCache &cache)
{
    CDBBatch batch(*this);

    auto entry = cache.GetEntries()->begin();

    while( entry != cache.GetEntries()->end() ){

        if( entry->second->balance <= 0 ){
            batch.Erase(make_pair(DB_REWARD_ENTRY,entry->first));
        }else{
            batch.Write(make_pair(DB_REWARD_ENTRY,entry->first), *entry->second);
        }

        ++entry;
    }

    auto addTx = cache.GetAddedTransactions()->begin();

    while( addTx != cache.GetAddedTransactions()->end() ){
        batch.Write(make_pair(DB_TX_HASH, addTx->first), addTx->second);
        ++addTx;
    }

    auto removeTx = cache.GetRemovedTransactions()->begin();

    while( removeTx != cache.GetRemovedTransactions()->end() ){
        batch.Erase(make_pair(DB_TX_HASH, removeTx->first));
        ++removeTx;
    }

    auto round = cache.GetRounds()->begin();

    while( round != cache.GetRounds()->end() ){
        batch.Write(make_pair(DB_ROUND, round->number), *round);
        ++round;
    }

    batch.Write(DB_BLOCK_LAST, *cache.GetCurrentBlock());

    if( cache.GetCurrentRound()->number ){
        batch.Write(DB_ROUND_CURRENT, *cache.GetCurrentRound());
    }

    return WriteBatch(batch, true);
}

bool CSmartRewardsDB::StartFirstRound(const CSmartRewardRound &start, const CSmartRewardEntryList &entries)
{
    CDBBatch batch(*this);

    BOOST_FOREACH(const CSmartRewardEntry &e, entries) {
        batch.Write(make_pair(DB_REWARD_ENTRY,e.id), e);
    }

    batch.Write(DB_ROUND_CURRENT, start);

    return WriteBatch(batch);
}

bool CSmartRewardsDB::FinalizeRound(const CSmartRewardRound &current, const CSmartRewardRound &next, const CSmartRewardEntryList &entries, const CSmartRewardResultEntryList &results)
{
    CDBBatch batch(*this);

    BOOST_FOREACH(const CSmartRewardResultEntry &s, results) {
        batch.Write(make_pair(DB_ROUND_SNAPSHOT, make_pair(current.number, s.entry.id)), s);
    }

    BOOST_FOREACH(const CSmartRewardEntry &e, entries) {
        batch.Write(make_pair(DB_REWARD_ENTRY,e.id), e);
    }

    batch.Write(make_pair(DB_ROUND,current.number), current);
    batch.Write(DB_ROUND_CURRENT, next);

    return WriteBatch(batch);
}

bool CSmartRewardsDB::UndoFinalizeRound(const CSmartRewardRound &current, const CSmartRewardResultEntryList &results)
{
    CDBBatch batch(*this);

    BOOST_FOREACH(const CSmartRewardResultEntry &s, results) {
        batch.Erase(make_pair(DB_ROUND_SNAPSHOT, make_pair(current.number, s.entry.id)));
        batch.Write(make_pair(DB_REWARD_ENTRY,s.entry.id), s.entry);
    }

    batch.Erase(make_pair(DB_ROUND, current.number));
    batch.Write(DB_ROUND_CURRENT, current);

    return WriteBatch(batch);
}

bool CSmartRewardsDB::ReadRewardEntries(CSmartRewardEntryMap &entries) {

    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(DB_REWARD_ENTRY);

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char,CSmartAddress> key;
        if (pcursor->GetKey(key) && key.first == DB_REWARD_ENTRY) {
            CSmartRewardEntry entry;
            if (pcursor->GetValue(entry)) {
                entries.insert(std::make_pair(entry.id, new CSmartRewardEntry(entry) ));
                pcursor->Next();
            } else {
                return error("failed to get reward entry");
            }
        } else {
            break;
        }
    }

    return true;
}

bool CSmartRewardsDB::ReadRewardRoundResults(const int16_t round, CSmartRewardResultEntryList &results) {

    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_ROUND_SNAPSHOT,round));

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char,std::pair<int16_t, CSmartAddress>> key;
        if (pcursor->GetKey(key) && key.first == DB_ROUND_SNAPSHOT) {

            if( key.second.first != round ) break;

            CSmartRewardResultEntry nValue;
            if (pcursor->GetValue(nValue)) {
                results.push_back(nValue);
                pcursor->Next();
            } else {
                return error("failed to get reward entry");
            }
        } else {
            break;
        }
    }

    return true;
}

bool CSmartRewardsDB::ReadRewardPayouts(const int16_t round, CSmartRewardResultEntryList &payouts) {

    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_ROUND_SNAPSHOT,round));

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char,std::pair<int16_t, CSmartAddress>> key;
        if (pcursor->GetKey(key) && key.first == DB_ROUND_SNAPSHOT) {

            if( key.second.first != round ) break;

            CSmartRewardResultEntry nValue;
            if (pcursor->GetValue(nValue)) {
                if( nValue.reward ) payouts.push_back(nValue);
                pcursor->Next();
            } else {
                return error("failed to get reward entry");
            }
        } else {
            break;
        }
    }

    return true;
}

bool CSmartRewardsDB::ReadRewardPayouts(const int16_t round, CSmartRewardResultEntryPtrList &payouts) {

    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_ROUND_SNAPSHOT,round));

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char,std::pair<int16_t, CSmartAddress>> key;
        if (pcursor->GetKey(key) && key.first == DB_ROUND_SNAPSHOT) {

            if( key.second.first != round ) break;

            CSmartRewardResultEntry nValue;
            if (pcursor->GetValue(nValue)) {
                if( nValue.reward ) payouts.push_back(new CSmartRewardResultEntry(nValue));
                pcursor->Next();
            } else {
                // Delete everything if something fails
                for( auto it : payouts ) delete it;
                payouts.clear();
                return error("failed to get reward entry");
            }
        } else {
            break;
        }
    }

    return true;
}

string CSmartRewardEntry::GetAddress() const
{
    return id.ToString();
}

void CSmartRewardEntry::SetNull()
{
    id = CSmartAddress();
    balance = 0;
    balanceAtStart = 0;
    balanceEligible = 0;
    disqualifyingTx.SetNull();
    fDisqualifyingTx = false;
    smartnodePaymentTx.SetNull();
    fSmartnodePaymentTx = false;
    voteProof.SetNull();
    fVoteProven = false;
}

string CSmartRewardEntry::ToString() const
{
    std::stringstream s;
    s << strprintf("CSmartRewardEntry(id=%s, balance=%d, balanceEligible=%d, isSmartNode=%b, voteProven=%b)\n",
        GetAddress(),
        balance,
        balanceEligible,
        fSmartnodePaymentTx,
        fVoteProven);
    return s.str();
}

bool CSmartRewardEntry::IsEligible()
{
    return fVoteProven && !fSmartnodePaymentTx && balanceEligible > 0 && !fDisqualifyingTx;
}

string CSmartRewardBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CSmartRewardBlock(height=%d, hash=%s, time=%d)\n",
        nHeight,
        nHash.ToString(),
        nTime);
    return s.str();
}

string CSmartRewardRound::ToString() const
{
    std::stringstream s;
    s << strprintf("CSmartRewardRound(number=%d, start(block)=%d, start(time)=%d, end(block)=%d, end(time)=%d\n"
                   "  Eligible addresses=%d\n  Eligible SMART=%d\n Percent=%f)\n",
        number,
        startBlockHeight,
        startBlockTime,
        endBlockHeight,
        endBlockTime,
        eligibleEntries,
        eligibleSmart,
        percent);
    return s.str();
}

string CSmartRewardResultEntry::GetAddress() const
{
    return entry.id.ToString();
}

string CSmartRewardResultEntry::ToString() const
{
    std::stringstream s;
    s << strprintf("CSmartRewardResultEntry(id=%d, balance=%d, reward=%d\n",
        GetAddress(),
        entry.balance,
        reward);
    return s.str();
}

arith_uint256 CSmartRewardResultEntry::CalculateScore(const uint256& blockHash)
{
    // Deterministically calculate a "score" for a CSmartRewardResultEntry based on any given (block)hash
    // Used to sort the payout list for 1.3 smartreward payouts
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << reward << entry.id << blockHash;
    return UintToArith256(ss.GetHash());
}

string CSmartRewardTransaction::ToString() const
{
    std::stringstream s;
    s << strprintf("CSmartRewardTransaction(hash=%s, blockHeight=%d\n",
        hash.ToString(),
        blockHeight);
    return s.str();
}
