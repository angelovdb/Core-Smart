// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/wallet.h"

#include "base58.h"
#include "checkpoints.h"
#include "chain.h"
#include "coincontrol.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "key.h"
#include "keystore.h"
#include "validation.h"
#include "net.h"
#include "policy/policy.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "random.h"
#include "script/script.h"
#include "script/sign.h"
#include "smartnode/instantx.h"
#include "smartnode/smartnode.h"
#include "smarthive/hive.h"
#include "timedata.h"
#include "txmempool.h"
#include "util.h"
#include "ui_interface.h"
#include "utilmoneystr.h"

#include <assert.h>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>

using namespace std;

/** Transaction fee set by the user */
CFeeRate payTxFee(DEFAULT_TRANSACTION_FEE);
unsigned int nTxConfirmTarget = DEFAULT_TX_CONFIRM_TARGET;
bool bSpendZeroConfChange = DEFAULT_SPEND_ZEROCONF_CHANGE;
bool fSendFreeTransactions = DEFAULT_SEND_FREE_TRANSACTIONS;

const char *DEFAULT_WALLET_DAT = "wallet.dat";

/**
 * Fees smaller than this (in satoshi) are considered zero fee (for transaction creation)
 * Override with -mintxfee
 */
CFeeRate CWallet::minTxFee = CFeeRate(DEFAULT_TRANSACTION_MINFEE);
/**
 * If fee estimation does not have enough data to provide estimates, use this fee instead.
 * Has no effect if not using fee estimation
 * Override with -fallbackfee
 */
CFeeRate CWallet::fallbackFee = CFeeRate(DEFAULT_FALLBACK_FEE);

const uint256 CMerkleTx::ABANDON_HASH(uint256S("0000000000000000000000000000000000000000000000000000000000000001"));

/** @defgroup mapWallet
 *
 * @{
 */

enum LockTimeFormat {
  UNSET = 0,
  BLOCKTIME,
  TIMESTAMP
};

struct CompareValueOnly {
    bool operator()(const pair <CAmount, pair<const CWalletTx *, unsigned int>> &t1,
                    const pair <CAmount, pair<const CWalletTx *, unsigned int>> &t2) const {
        return t1.first < t2.first;
    }
};

std::string COutput::ToString() const {
    return strprintf("COutput(%s, %d, %d) [%s]", tx->GetHash().ToString(), i, nDepth, FormatMoney(tx->vout[i].nValue));
}

inline CAmount CalculateInputFee(int64_t nInputs){
    CAmount feeCalc = (((nInputs * 148) + (2 * 34) + 10 + 9) / 1024.0) * 100000;
    feeCalc = std::floor((feeCalc / 100000.0) + 0.5) * 100000;
    return std::max(feeCalc, static_cast<CAmount>(100000));
}

const CWalletTx *CWallet::GetWalletTx(const uint256 &hash) const {
    LOCK(cs_wallet);
    std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(hash);
    if (it == mapWallet.end())
        return NULL;
    return &(it->second);
}

CPubKey CWallet::GenerateNewKey(uint32_t nAccountIndex, bool fInternal) {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    bool fCompressed = CanSupportFeature(FEATURE_COMPRPUBKEY); // default to compressed public keys if we want 0.6.0 wallets

    CKey secret;

    // Create new metadata
    int64_t nCreationTime = GetTime();
    CKeyMetadata metadata(nCreationTime);

    CPubKey pubkey;
    // use HD key derivation if HD was enabled during wallet creation
    if (IsHDEnabled()) {
        DeriveNewChildKey(metadata, secret, nAccountIndex, fInternal);
        pubkey = secret.GetPubKey();
    } else {
        secret.MakeNewKey(fCompressed);

        // Compressed public keys were introduced in version 0.6.0
        if (fCompressed)
            SetMinVersion(FEATURE_COMPRPUBKEY);

        pubkey = secret.GetPubKey();
        assert(secret.VerifyPubKey(pubkey));

        // Create new metadata
        mapKeyMetadata[pubkey.GetID()] = metadata;
        if (!nTimeFirstKey || nCreationTime < nTimeFirstKey)
            nTimeFirstKey = nCreationTime;

        if (!AddKeyPubKey(secret, pubkey))
            throw std::runtime_error(std::string(__func__) + ": AddKey failed");
    }
    return pubkey;
}

void CWallet::DeriveNewChildKey(const CKeyMetadata& metadata, CKey& secretRet, uint32_t nAccountIndex, bool fInternal)
{
    CHDChain hdChainTmp;
    if (!GetHDChain(hdChainTmp)) {
        throw std::runtime_error(std::string(__func__) + ": GetHDChain failed");
    }

    if (!DecryptHDChain(hdChainTmp))
        throw std::runtime_error(std::string(__func__) + ": DecryptHDChainSeed failed");
    // make sure seed matches this chain
    if (hdChainTmp.GetID() != hdChainTmp.GetSeedHash())
        throw std::runtime_error(std::string(__func__) + ": Wrong HD chain!");

    CHDAccount acc;
    if (!hdChainTmp.GetAccount(nAccountIndex, acc))
        throw std::runtime_error(std::string(__func__) + ": Wrong HD account!");

    // derive child key at next index, skip keys already known to the wallet
    CExtKey childKey;
    uint32_t nChildIndex = fInternal ? acc.nInternalChainCounter : acc.nExternalChainCounter;
    do {
        hdChainTmp.DeriveChildExtKey(nAccountIndex, fInternal, nChildIndex, childKey);
        // increment childkey index
        nChildIndex++;
    } while (HaveKey(childKey.key.GetPubKey().GetID()));
    secretRet = childKey.key;

    CPubKey pubkey = secretRet.GetPubKey();
    assert(secretRet.VerifyPubKey(pubkey));

    // store metadata
    mapKeyMetadata[pubkey.GetID()] = metadata;
    if (!nTimeFirstKey || metadata.nCreateTime < nTimeFirstKey)
        nTimeFirstKey = metadata.nCreateTime;

    // update the chain model in the database
    CHDChain hdChainCurrent;
    GetHDChain(hdChainCurrent);

    if (fInternal) {
        acc.nInternalChainCounter = nChildIndex;
    }
    else {
        acc.nExternalChainCounter = nChildIndex;
    }

    if (!hdChainCurrent.SetAccount(nAccountIndex, acc))
        throw std::runtime_error(std::string(__func__) + ": SetAccount failed");

    if (IsCrypted()) {
        if (!SetCryptedHDChain(hdChainCurrent, false))
            throw std::runtime_error(std::string(__func__) + ": SetCryptedHDChain failed");
    }
    else {
        if (!SetHDChain(hdChainCurrent, false))
            throw std::runtime_error(std::string(__func__) + ": SetHDChain failed");
    }

    if (!AddHDPubKey(childKey.Neuter(), fInternal))
        throw std::runtime_error(std::string(__func__) + ": AddHDPubKey failed");
}

bool CWallet::GetPubKey(const CKeyID &address, CPubKey& vchPubKeyOut) const
{
    LOCK(cs_wallet);
    std::map<CKeyID, CHDPubKey>::const_iterator mi = mapHdPubKeys.find(address);
    if (mi != mapHdPubKeys.end())
    {
        const CHDPubKey &hdPubKey = (*mi).second;
        vchPubKeyOut = hdPubKey.extPubKey.pubkey;
        return true;
    }
    else
        return CCryptoKeyStore::GetPubKey(address, vchPubKeyOut);
}

bool CWallet::GetKey(const CKeyID &address, CKey& keyOut) const
{
    LOCK(cs_wallet);
    std::map<CKeyID, CHDPubKey>::const_iterator mi = mapHdPubKeys.find(address);
    if (mi != mapHdPubKeys.end())
    {
        // if the key has been found in mapHdPubKeys, derive it on the fly
        const CHDPubKey &hdPubKey = (*mi).second;
        CHDChain hdChainCurrent;
        if (!GetHDChain(hdChainCurrent))
            throw std::runtime_error(std::string(__func__) + ": GetHDChain failed");
        if (!DecryptHDChain(hdChainCurrent))
            throw std::runtime_error(std::string(__func__) + ": DecryptHDChainSeed failed");
        // make sure seed matches this chain
        if (hdChainCurrent.GetID() != hdChainCurrent.GetSeedHash())
            throw std::runtime_error(std::string(__func__) + ": Wrong HD chain!");

        CExtKey extkey;
        hdChainCurrent.DeriveChildExtKey(hdPubKey.nAccountIndex, hdPubKey.nChangeIndex != 0, hdPubKey.extPubKey.nChild, extkey);
        keyOut = extkey.key;

        return true;
    }
    else {
        return CCryptoKeyStore::GetKey(address, keyOut);
    }
}

bool CWallet::HaveKey(const CKeyID &address) const
{
    LOCK(cs_wallet);
    if (mapHdPubKeys.count(address) > 0)
        return true;
    return CCryptoKeyStore::HaveKey(address);
}

bool CWallet::LoadHDPubKey(const CHDPubKey &hdPubKey)
{
    AssertLockHeld(cs_wallet);

    mapHdPubKeys[hdPubKey.extPubKey.pubkey.GetID()] = hdPubKey;
    return true;
}

bool CWallet::AddHDPubKey(const CExtPubKey &extPubKey, bool fInternal)
{
    AssertLockHeld(cs_wallet);

    CHDChain hdChainCurrent;
    GetHDChain(hdChainCurrent);

    CHDPubKey hdPubKey;
    hdPubKey.extPubKey = extPubKey;
    hdPubKey.hdchainID = hdChainCurrent.GetID();
    hdPubKey.nChangeIndex = fInternal ? 1 : 0;
    mapHdPubKeys[extPubKey.pubkey.GetID()] = hdPubKey;

    // check if we need to remove from watch-only
    CScript script;
    script = GetScriptForDestination(extPubKey.pubkey.GetID());
    if (HaveWatchOnly(script))
        RemoveWatchOnly(script);
    script = GetScriptForRawPubKey(extPubKey.pubkey);
    if (HaveWatchOnly(script))
        RemoveWatchOnly(script);

    if (!fFileBacked)
        return true;

    return CWalletDB(strWalletFile).WriteHDPubKey(hdPubKey, mapKeyMetadata[extPubKey.pubkey.GetID()]);
}

bool CWallet::AddKeyPubKey(const CKey &secret, const CPubKey &pubkey) {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    if (!CCryptoKeyStore::AddKeyPubKey(secret, pubkey))
        return false;

    // check if we need to remove from watch-only
    CScript script;
    script = GetScriptForDestination(pubkey.GetID());
    if (HaveWatchOnly(script))
        RemoveWatchOnly(script);
    script = GetScriptForRawPubKey(pubkey);
    if (HaveWatchOnly(script))
        RemoveWatchOnly(script);

    if (!fFileBacked)
        return true;
    if (!IsCrypted()) {
        return CWalletDB(strWalletFile).WriteKey(pubkey,
                                                 secret.GetPrivKey(),
                                                 mapKeyMetadata[pubkey.GetID()]);
    }
    return true;
}

bool CWallet::AddCryptedKey(const CPubKey &vchPubKey,
                            const vector<unsigned char> &vchCryptedSecret) {
    if (!CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret))
        return false;
    if (!fFileBacked)
        return true;
    {
        LOCK(cs_wallet);
        if (pwalletdbEncryption)
            return pwalletdbEncryption->WriteCryptedKey(vchPubKey,
                                                        vchCryptedSecret,
                                                        mapKeyMetadata[vchPubKey.GetID()]);
        else
            return CWalletDB(strWalletFile).WriteCryptedKey(vchPubKey,
                                                            vchCryptedSecret,
                                                            mapKeyMetadata[vchPubKey.GetID()]);
    }
    return false;
}

bool CWallet::LoadKeyMetadata(const CPubKey &pubkey, const CKeyMetadata &meta) {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    if (meta.nCreateTime && (!nTimeFirstKey || meta.nCreateTime < nTimeFirstKey))
        nTimeFirstKey = meta.nCreateTime;

    mapKeyMetadata[pubkey.GetID()] = meta;

    return true;
}

bool CWallet::UpdateKeyMetadata(const CPubKey &vchPubKey) {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    return CWalletDB(strWalletFile).UpdateKeyMeta(vchPubKey, mapKeyMetadata[vchPubKey.GetID()]);
}

bool CWallet::LoadCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret) {
    return CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret);
}

bool CWallet::AddCScript(const CScript &redeemScript) {
    if (!CCryptoKeyStore::AddCScript(redeemScript))
        return false;
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteCScript(Hash160(redeemScript), redeemScript);
}

bool CWallet::LoadCScript(const CScript &redeemScript) {
    /* A sanity check was added in pull #3843 to avoid adding redeemScripts
     * that never can be redeemed. However, old wallets may still contain
     * these. Do not add them to the wallet and warn. */
    if (redeemScript.size() > MAX_SCRIPT_ELEMENT_SIZE) {
        std::string strAddr = CBitcoinAddress(CScriptID(redeemScript)).ToString();
        LogPrintf(
                "%s: Warning: This wallet contains a redeemScript of size %i which exceeds maximum size %i thus can never be redeemed. Do not use address %s.\n",
                __func__, redeemScript.size(), MAX_SCRIPT_ELEMENT_SIZE, strAddr);
        return true;
    }

    return CCryptoKeyStore::AddCScript(redeemScript);
}

bool CWallet::AddWatchOnly(const CScript &dest) {
    if (!CCryptoKeyStore::AddWatchOnly(dest))
        return false;
    nTimeFirstKey = 1; // No birthday information for watch-only keys.
    NotifyWatchonlyChanged(true);
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteWatchOnly(dest);
}

bool CWallet::RemoveWatchOnly(const CScript &dest) {
    AssertLockHeld(cs_wallet);
    if (!CCryptoKeyStore::RemoveWatchOnly(dest))
        return false;
    if (!HaveWatchOnly())
        NotifyWatchonlyChanged(false);
    if (fFileBacked)
        if (!CWalletDB(strWalletFile).EraseWatchOnly(dest))
            return false;

    return true;
}

bool CWallet::LoadWatchOnly(const CScript &dest) {
    return CCryptoKeyStore::AddWatchOnly(dest);
}

bool CWallet::HaveVotingKey(const CKeyID &address) const
{
    LOCK(cs_wallet);
    return CCryptoKeyStore::HaveVotingKey(address);
}

bool CWallet::GetVotingPubKey(const CKeyID &address, CPubKey& vchPubKeyOut) const
{
    LOCK(cs_wallet);
    return CCryptoKeyStore::GetVotingPubKey(address, vchPubKeyOut);
}

bool CWallet::GetVotingKey(const CKeyID &address, CKey& keyOut) const
{
    LOCK(cs_wallet);
    return CCryptoKeyStore::GetVotingKey(address, keyOut);
}

bool CWallet::AddVotingKeyPubKey(const CKey &secret, const CPubKey &pubkey) {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    if (!CCryptoKeyStore::AddVotingKeyPubKey(secret, pubkey))
        return false;

    if (!fFileBacked)
        return true;
    if (!IsVotingCrypted()) {
        return CWalletDB(strWalletFile).WriteVotingKey(pubkey,
                                                 secret.GetPrivKey(),
                                                 mapVotingKeyMetadata[pubkey.GetID()]);
    }
    return true;
}

bool CWallet::AddCryptedVotingKey(const CPubKey &vchPubKey,
                            const vector<unsigned char> &vchCryptedSecret) {
    if (!CCryptoKeyStore::AddCryptedVotingKey(vchPubKey, vchCryptedSecret))
        return false;
    if (!fFileBacked)
        return true;
    {
        LOCK(cs_wallet);
        if (pvotingdbEncryption)
            return pvotingdbEncryption->WriteCryptedVotingKey(vchPubKey,
                                                        vchCryptedSecret,
                                                        mapVotingKeyMetadata[vchPubKey.GetID()]);
        else
            return CWalletDB(strWalletFile).WriteCryptedVotingKey(vchPubKey,
                                                            vchCryptedSecret,
                                                            mapVotingKeyMetadata[vchPubKey.GetID()]);
    }
    return false;
}

bool CWallet::LoadVotingKeyMetadata(const CKeyID &keyId, const CVotingKeyMetadata &meta) {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    mapVotingKeyMetadata[keyId] = meta;
    return true;
}

bool CWallet::UpdateVotingKeyMetadata(const CKeyID &keyId) {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    return CWalletDB(strWalletFile).UpdateVotingKeyMeta(keyId, mapVotingKeyMetadata[keyId]);
}

bool CWallet::LoadVotingKeyRegistration(const CKeyID &keyId, const uint256 &txhash) {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    mapVotingKeyRegistrations[keyId] = txhash;
    return true;
}

bool CWallet::UpdateVotingKeyRegistration(const CKeyID &keyId) {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    return CWalletDB(strWalletFile).UpdateVotingKeyRegistration(keyId, mapVotingKeyRegistrations[keyId]);
}

bool CWallet::LoadCryptedVotingKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret) {
    return CCryptoKeyStore::AddCryptedVotingKey(vchPubKey, vchCryptedSecret);
}

bool CWallet::Unlock(const SecureString &strWalletPassphrase) {
    CCrypter crypter;
    CKeyingMaterial vMasterKey;

    {
        LOCK(cs_wallet);
        BOOST_FOREACH(const MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                continue; // try another master key
            if (CCryptoKeyStore::Unlock(vMasterKey)) {
                if(nWalletBackups == -2) {
                    TopUpKeyPool();
                    LogPrintf("Keypool replenished, re-initializing automatic backups.\n");
                    nWalletBackups = GetArg("-createwalletbackups", 10);
                }
                return true;
            }
        }
    }
    return false;
}

bool CWallet::ChangeWalletPassphrase(const SecureString &strOldWalletPassphrase,
                                     const SecureString &strNewWalletPassphrase) {
    bool fWasLocked = IsLocked();

    {
        LOCK(cs_wallet);
        Lock();

        CCrypter crypter;
        CKeyingMaterial vMasterKey;
        BOOST_FOREACH(MasterKeyMap::value_type & pMasterKey, mapMasterKeys)
        {
            if (!crypter.SetKeyFromPassphrase(strOldWalletPassphrase, pMasterKey.second.vchSalt,
                                              pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;
            if (CCryptoKeyStore::Unlock(vMasterKey)) {
                int64_t nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt,
                                             pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations =
                        pMasterKey.second.nDeriveIterations * (100 / ((double) (GetTimeMillis() - nStartTime)));

                nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt,
                                             pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = (pMasterKey.second.nDeriveIterations +
                                                       pMasterKey.second.nDeriveIterations * 100 /
                                                       ((double) (GetTimeMillis() - nStartTime))) / 2;

                if (pMasterKey.second.nDeriveIterations < 25000)
                    pMasterKey.second.nDeriveIterations = 25000;

                LogPrintf("Wallet passphrase changed to an nDeriveIterations of %i\n",
                          pMasterKey.second.nDeriveIterations);

                if (!crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt,
                                                  pMasterKey.second.nDeriveIterations,
                                                  pMasterKey.second.nDerivationMethod))
                    return false;
                if (!crypter.Encrypt(vMasterKey, pMasterKey.second.vchCryptedKey))
                    return false;
                CWalletDB(strWalletFile).WriteMasterKey(pMasterKey.first, pMasterKey.second);
                if (fWasLocked)
                    Lock();
                return true;
            }
        }
    }

    return false;
}

bool CWallet::EncryptWallet(const SecureString& strWalletPassphrase)
{
    if (IsCrypted())
        return false;

    CKeyingMaterial vMasterKey;

    vMasterKey.resize(WALLET_CRYPTO_KEY_SIZE);
    GetStrongRandBytes(&vMasterKey[0], WALLET_CRYPTO_KEY_SIZE);

    CMasterKey kMasterKey;

    kMasterKey.vchSalt.resize(WALLET_CRYPTO_SALT_SIZE);
    GetStrongRandBytes(&kMasterKey.vchSalt[0], WALLET_CRYPTO_SALT_SIZE);

    CCrypter crypter;
    int64_t nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, 25000, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = 2500000 / ((double)(GetTimeMillis() - nStartTime));

    nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = (kMasterKey.nDeriveIterations + kMasterKey.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

    if (kMasterKey.nDeriveIterations < 25000)
        kMasterKey.nDeriveIterations = 25000;

    LogPrintf("Encrypting Wallet with an nDeriveIterations of %i\n", kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod))
        return false;
    if (!crypter.Encrypt(vMasterKey, kMasterKey.vchCryptedKey))
        return false;

    {
        LOCK(cs_wallet);
        mapMasterKeys[++nMasterKeyMaxID] = kMasterKey;
        if (fFileBacked)
        {
            assert(!pwalletdbEncryption);
            pwalletdbEncryption = new CWalletDB(strWalletFile);
            if (!pwalletdbEncryption->TxnBegin()) {
                delete pwalletdbEncryption;
                pwalletdbEncryption = NULL;
                return false;
            }
            pwalletdbEncryption->WriteMasterKey(nMasterKeyMaxID, kMasterKey);
        }

        // must get current HD chain before EncryptKeys
        CHDChain hdChainCurrent;
        GetHDChain(hdChainCurrent);

        if (!EncryptKeys(vMasterKey))
        {
            if (fFileBacked) {
                pwalletdbEncryption->TxnAbort();
                delete pwalletdbEncryption;
            }
            // We now probably have half of our keys encrypted in memory, and half not...
            // die and let the user reload the unencrypted wallet.
            assert(false);
        }

        if (!hdChainCurrent.IsNull()) {
            assert(EncryptHDChain(vMasterKey));

            CHDChain hdChainCrypted;
            assert(GetHDChain(hdChainCrypted));

            DBG(
                printf("EncryptWallet -- current seed: '%s'\n", HexStr(hdChainCurrent.GetSeed()).c_str());
                printf("EncryptWallet -- crypted seed: '%s'\n", HexStr(hdChainCrypted.GetSeed()).c_str());
            );

            // ids should match, seed hashes should not
            assert(hdChainCurrent.GetID() == hdChainCrypted.GetID());
            assert(hdChainCurrent.GetSeedHash() != hdChainCrypted.GetSeedHash());

            assert(SetCryptedHDChain(hdChainCrypted, false));
        }

        // Encryption was introduced in version 0.4.0
        SetMinVersion(FEATURE_WALLETCRYPT, pwalletdbEncryption, true);

        if (fFileBacked)
        {
            if (!pwalletdbEncryption->TxnCommit()) {
                delete pwalletdbEncryption;
                // We now have keys encrypted in memory, but not on disk...
                // die to avoid confusion and let the user reload the unencrypted wallet.
                assert(false);
            }

            delete pwalletdbEncryption;
            pwalletdbEncryption = NULL;
        }

        Lock();
        Unlock(strWalletPassphrase);

        // if we are not using HD, generate new keypool
        if(IsHDEnabled()) {
            TopUpKeyPool();
        }
        else {
            NewKeyPool();
        }

        Lock();

        // Need to completely rewrite the wallet file; if we don't, bdb might keep
        // bits of the unencrypted private key in slack space in the database file.
        CDB::Rewrite(strWalletFile);

    }
    NotifyStatusChanged(this);

    return true;
}

bool CWallet::UnlockVoting(const SecureString &strWalletPassphrase) {
    CCrypter crypter;
    CKeyingMaterial vMasterKey;

    {
        LOCK(cs_wallet);
        BOOST_FOREACH(const MasterKeyMap::value_type& pMasterKey, mapVotingMasterKeys)
        {
            if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                continue; // try another master key
            if (CCryptoKeyStore::UnlockVoting(vMasterKey)) {
                return true;
            }
        }
    }
    return false;
}

bool CWallet::ChangeVotingPassphrase(const SecureString &strOldWalletPassphrase,
                                     const SecureString &strNewWalletPassphrase) {
    bool fWasLocked = IsVotingLocked();

    {
        LOCK(cs_wallet);
        LockVoting();

        CCrypter crypter;
        CKeyingMaterial vMasterKey;
        BOOST_FOREACH(MasterKeyMap::value_type & pMasterKey, mapVotingMasterKeys)
        {
            if (!crypter.SetKeyFromPassphrase(strOldWalletPassphrase, pMasterKey.second.vchSalt,
                                              pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;
            if (CCryptoKeyStore::UnlockVoting(vMasterKey)) {
                int64_t nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt,
                                             pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations =
                        pMasterKey.second.nDeriveIterations * (100 / ((double) (GetTimeMillis() - nStartTime)));

                nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt,
                                             pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = (pMasterKey.second.nDeriveIterations +
                                                       pMasterKey.second.nDeriveIterations * 100 /
                                                       ((double) (GetTimeMillis() - nStartTime))) / 2;

                if (pMasterKey.second.nDeriveIterations < 25000)
                    pMasterKey.second.nDeriveIterations = 25000;

                LogPrintf("Voting passphrase changed to an nDeriveIterations of %i\n",
                          pMasterKey.second.nDeriveIterations);

                if (!crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt,
                                                  pMasterKey.second.nDeriveIterations,
                                                  pMasterKey.second.nDerivationMethod))
                    return false;
                if (!crypter.Encrypt(vMasterKey, pMasterKey.second.vchCryptedKey))
                    return false;
                CWalletDB(strWalletFile).WriteVotingMasterKey(pMasterKey.first, pMasterKey.second);
                if (fWasLocked)
                    LockVoting();
                return true;
            }
        }
    }

    return false;
}

bool CWallet::EncryptVoting(const SecureString& strWalletPassphrase)
{
    if (IsVotingCrypted())
        return false;

    CKeyingMaterial vMasterKey;

    vMasterKey.resize(WALLET_CRYPTO_KEY_SIZE);
    GetStrongRandBytes(&vMasterKey[0], WALLET_CRYPTO_KEY_SIZE);

    CMasterKey kMasterKey;

    kMasterKey.vchSalt.resize(WALLET_CRYPTO_SALT_SIZE);
    GetStrongRandBytes(&kMasterKey.vchSalt[0], WALLET_CRYPTO_SALT_SIZE);

    CCrypter crypter;
    int64_t nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, 25000, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = 2500000 / ((double)(GetTimeMillis() - nStartTime));

    nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = (kMasterKey.nDeriveIterations + kMasterKey.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

    if (kMasterKey.nDeriveIterations < 25000)
        kMasterKey.nDeriveIterations = 25000;

    LogPrintf("Encrypting Voting with an nDeriveIterations of %i\n", kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod))
        return false;
    if (!crypter.Encrypt(vMasterKey, kMasterKey.vchCryptedKey))
        return false;

    {
        LOCK(cs_wallet);
        mapVotingMasterKeys[++nVotingMasterKeyMaxID] = kMasterKey;
        if (fFileBacked)
        {
            assert(!pvotingdbEncryption);
            pvotingdbEncryption = new CWalletDB(strWalletFile);
            if (!pvotingdbEncryption->TxnBegin()) {
                delete pvotingdbEncryption;
                pvotingdbEncryption = NULL;
                return false;
            }
            pvotingdbEncryption->WriteVotingMasterKey(nVotingMasterKeyMaxID, kMasterKey);
        }

        if (!EncryptVotingKeys(vMasterKey))
        {
            if (fFileBacked) {
                pvotingdbEncryption->TxnAbort();
                delete pvotingdbEncryption;
            }
            // We now probably have half of our keys encrypted in memory, and half not...
            // die and let the user reload the unencrypted wallet.
            assert(false);
        }

        // Encryption was introduced in version 0.4.0
        SetMinVersion(FEATURE_VOTINGCRYPT, pvotingdbEncryption, true);

        if (fFileBacked)
        {
            if (!pvotingdbEncryption->TxnCommit()) {
                delete pvotingdbEncryption;
                // We now have keys encrypted in memory, but not on disk...
                // die to avoid confusion and let the user reload the unencrypted wallet.
                assert(false);
            }

            delete pvotingdbEncryption;
            pvotingdbEncryption = NULL;
        }

        LockVoting();

        // Need to completely rewrite the wallet file; if we don't, bdb might keep
        // bits of the unencrypted private key in slack space in the database file.
        CDB::Rewrite(strWalletFile);

    }
    NotifyStatusChanged(this);

    return true;
}

void CWallet::SetBestChain(const CBlockLocator &loc) {
    CWalletDB walletdb(strWalletFile);
    walletdb.WriteBestBlock(loc);
}

bool CWallet::SetMinVersion(enum WalletFeature nVersion, CWalletDB *pwalletdbIn, bool fExplicit) {
    LOCK(cs_wallet); // nWalletVersion
    if (nWalletVersion >= nVersion)
        return true;

    // when doing an explicit upgrade, if we pass the max version permitted, upgrade all the way
    if (fExplicit && nVersion > nWalletMaxVersion)
        nVersion = FEATURE_LATEST;

    nWalletVersion = nVersion;

    if (nVersion > nWalletMaxVersion)
        nWalletMaxVersion = nVersion;

    if (fFileBacked) {
        CWalletDB *pwalletdb = pwalletdbIn ? pwalletdbIn : new CWalletDB(strWalletFile);
        if (nWalletVersion > 40000)
            pwalletdb->WriteMinVersion(nWalletVersion);
        if (!pwalletdbIn)
            delete pwalletdb;
    }

    return true;
}

bool CWallet::SetMaxVersion(int nVersion) {
    LOCK(cs_wallet); // nWalletVersion, nWalletMaxVersion
    // cannot downgrade below current version
    if (nWalletVersion > nVersion)
        return false;

    nWalletMaxVersion = nVersion;

    return true;
}

set <uint256> CWallet::GetConflicts(const uint256 &txid) const {
    set <uint256> result;
    AssertLockHeld(cs_wallet);

    std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(txid);
    if (it == mapWallet.end())
        return result;
    const CWalletTx &wtx = it->second;

    std::pair <TxSpends::const_iterator, TxSpends::const_iterator> range;

    BOOST_FOREACH(
    const CTxIn &txin, wtx.vin)
    {
        if (mapTxSpends.count(txin.prevout) <= 1)
            continue;  // No conflict if zero or one spends
        range = mapTxSpends.equal_range(txin.prevout);
        for (TxSpends::const_iterator it = range.first; it != range.second; ++it)
            result.insert(it->second);
    }
    return result;
}

void CWallet::Flush(bool shutdown) {
    bitdb.Flush(shutdown);
}

bool CWallet::Verify(const string& walletFile, string& warningString, string& errorString)
{
    if (!bitdb.Open(GetDataDir()))
    {
        // try moving the database env out of the way
        boost::filesystem::path pathDatabase = GetDataDir() / "database";
        boost::filesystem::path pathDatabaseBak = GetDataDir() / strprintf("database.%d.bak", GetTime());
        try {
            boost::filesystem::rename(pathDatabase, pathDatabaseBak);
            LogPrintf("Moved old %s to %s. Retrying.\n", pathDatabase.string(), pathDatabaseBak.string());
        } catch (const boost::filesystem::filesystem_error&) {
            // failure is ok (well, not really, but it's not worse than what we started with)
        }

        // try again
        if (!bitdb.Open(GetDataDir())) {
            // if it still fails, it probably means we can't even create the database env
            string msg = strprintf(_("Error initializing wallet database environment %s!"), GetDataDir());
            errorString += msg;
            return true;
        }
    }

    if (GetBoolArg("-salvagewallet", false))
    {
        // Recover readable keypairs:
        if (!CWalletDB::Recover(bitdb, walletFile, true))
            return false;
    }

    if (boost::filesystem::exists(GetDataDir() / walletFile))
    {
        CDBEnv::VerifyResult r = bitdb.Verify(walletFile, CWalletDB::Recover);
        if (r == CDBEnv::RECOVER_OK)
        {
            warningString += strprintf(_("Warning: wallet.dat corrupt, data salvaged!"
                                     " Original wallet.dat saved as wallet.{timestamp}.bak in %s; if"
                                     " your balance or transactions are incorrect you should"
                                     " restore from a backup."), GetDataDir());
        }
        if (r == CDBEnv::RECOVER_FAIL)
            errorString += _("wallet.dat corrupt, salvage failed");
    }

    return true;
}

void CWallet::SyncMetaData(pair <TxSpends::iterator, TxSpends::iterator> range) {
    // We want all the wallet transactions in range to have the same metadata as
    // the oldest (smallest nOrderPos).
    // So: find smallest nOrderPos:

    int nMinOrderPos = std::numeric_limits<int>::max();
    const CWalletTx *copyFrom = NULL;
    for (TxSpends::iterator it = range.first; it != range.second; ++it) {
        const uint256 &hash = it->second;
        int n = mapWallet[hash].nOrderPos;
        if (n < nMinOrderPos) {
            nMinOrderPos = n;
            copyFrom = &mapWallet[hash];
        }
    }
    // Now copy data from copyFrom to rest:
    for (TxSpends::iterator it = range.first; it != range.second; ++it) {
        const uint256 &hash = it->second;
        CWalletTx *copyTo = &mapWallet[hash];
        if (copyFrom == copyTo) continue;
        if (!copyFrom->IsEquivalentTo(*copyTo)) continue;
        copyTo->mapValue = copyFrom->mapValue;
        copyTo->vOrderForm = copyFrom->vOrderForm;
        // fTimeReceivedIsTxTime not copied on purpose
        // nTimeReceived not copied on purpose
        copyTo->nTimeSmart = copyFrom->nTimeSmart;
        copyTo->fFromMe = copyFrom->fFromMe;
        copyTo->strFromAccount = copyFrom->strFromAccount;
        // nOrderPos not copied on purpose
        // cached members not copied on purpose
    }
}

/**
 * Outpoint is spent if any non-conflicted transaction
 * spends it:
 */
bool CWallet::IsSpent(const uint256 &hash, unsigned int n) const {
    const COutPoint outpoint(hash, n);
    pair <TxSpends::const_iterator, TxSpends::const_iterator> range;
    range = mapTxSpends.equal_range(outpoint);

    for (TxSpends::const_iterator it = range.first; it != range.second; ++it) {
        const uint256 &wtxid = it->second;
        std::map<uint256, CWalletTx>::const_iterator mit = mapWallet.find(wtxid);
        if (mit != mapWallet.end()) {
            int depth = mit->second.GetDepthInMainChain();
            if (depth > 0 || (depth == 0 && !mit->second.isAbandoned()))
                return true; // Spent
        }
    }
    return false;
}

void CWallet::AddToSpends(const COutPoint &outpoint, const uint256 &wtxid) {
    mapTxSpends.insert(make_pair(outpoint, wtxid));

    pair <TxSpends::iterator, TxSpends::iterator> range;
    range = mapTxSpends.equal_range(outpoint);
    SyncMetaData(range);
}


void CWallet::AddToSpends(const uint256 &wtxid) {
    assert(mapWallet.count(wtxid));
    CWalletTx &thisTx = mapWallet[wtxid];
    if (thisTx.IsCoinBase() || thisTx.IsZerocoinSpend()) // Coinbases don't spend anything!
        return;

    BOOST_FOREACH(
    const CTxIn &txin, thisTx.vin)
    AddToSpends(txin.prevout, wtxid);
}


int64_t CWallet::IncOrderPosNext(CWalletDB *pwalletdb) {
    AssertLockHeld(cs_wallet); // nOrderPosNext
    int64_t nRet = nOrderPosNext++;
    if (pwalletdb) {
        pwalletdb->WriteOrderPosNext(nOrderPosNext);
    } else {
        CWalletDB(strWalletFile).WriteOrderPosNext(nOrderPosNext);
    }
    return nRet;
}

bool CWallet::AccountMove(std::string strFrom, std::string strTo, CAmount nAmount, std::string strComment) {
    CWalletDB walletdb(strWalletFile);
    if (!walletdb.TxnBegin())
        return false;

    int64_t nNow = GetAdjustedTime();

    // Debit
    CAccountingEntry debit;
    debit.nOrderPos = IncOrderPosNext(&walletdb);
    debit.strAccount = strFrom;
    debit.nCreditDebit = -nAmount;
    debit.nTime = nNow;
    debit.strOtherAccount = strTo;
    debit.strComment = strComment;
    AddAccountingEntry(debit, walletdb);

    // Credit
    CAccountingEntry credit;
    credit.nOrderPos = IncOrderPosNext(&walletdb);
    credit.strAccount = strTo;
    credit.nCreditDebit = nAmount;
    credit.nTime = nNow;
    credit.strOtherAccount = strFrom;
    credit.strComment = strComment;
    AddAccountingEntry(credit, walletdb);

    if (!walletdb.TxnCommit())
        return false;

    return true;
}

// bool CWallet::GetAccountPubkey(CPubKey &pubKey, std::string strAccount, bool bForceNew) {
//     CWalletDB walletdb(strWalletFile);

//     CAccount account;
//     walletdb.ReadAccount(strAccount, account);

//     if (!bForceNew) {
//         if (!account.vchPubKey.IsValid())
//             bForceNew = true;
//         else {
//             // Check if the current key has been used
//             CScript scriptPubKey = GetScriptForDestination(account.vchPubKey.GetID());
//             for (map<uint256, CWalletTx>::iterator it = mapWallet.begin();
//                  it != mapWallet.end() && account.vchPubKey.IsValid();
//                  ++it)
//                 BOOST_FOREACH(
//             const CTxOut &txout, (*it).second.vout)
//             if (txout.scriptPubKey == scriptPubKey) {
//                 bForceNew = true;
//                 break;
//             }
//         }
//     }

//     // Generate a new key
//     if (bForceNew) {
//         if (!GetKeyFromPool(account.vchPubKey))
//             return false;

//         SetAddressBook(account.vchPubKey.GetID(), strAccount, "receive");
//         walletdb.WriteAccount(strAccount, account);
//     }

//     pubKey = account.vchPubKey;

//     return true;
// }

void CWallet::MarkDirty() {
    {
        LOCK(cs_wallet);
        BOOST_FOREACH(PAIRTYPE(
        const uint256, CWalletTx)&item, mapWallet)
        item.second.MarkDirty();
    }
}

bool CWallet::AddToWallet(const CWalletTx &wtxIn, bool fFromLoadWallet, CWalletDB *pwalletdb) {
    LogPrint("selectcoins", "CWallet::AddToWallet\n");
    uint256 hash = wtxIn.GetHash();
    LogPrint("selectcoins","hash=%s\n", hash.ToString());
    if (fFromLoadWallet) {
        mapWallet[hash] = wtxIn;
        CWalletTx &wtx = mapWallet[hash];
        wtx.BindWallet(this);
//        if (!wtx.IsZerocoinSpend()) {
        wtxOrdered.insert(make_pair(wtx.nOrderPos, TxPair(&wtx, (CAccountingEntry *) 0)));
        AddToSpends(hash);
//            BOOST_FOREACH(const CTxIn &txin, wtx.vin) {
//                LogPrintf("txin.prevout.hash=%s\n", txin.prevout.hash.ToString());
//                if (mapWallet.count(txin.prevout.hash)) {
//                    CWalletTx &prevtx = mapWallet[txin.prevout.hash];
//                    if (prevtx.nIndex == -1 && !prevtx.hashUnset()) {
//                        LogPrintf("Enter\n");
//                        MarkConflicted(prevtx.hashBlock, wtx.GetHash());
//                        LogPrintf("Out\n");
//                    }
//                }
//            }
//        }
    } else {
        LOCK(cs_wallet);
        // Inserts only if not already there, returns tx inserted or tx found
        pair<map<uint256, CWalletTx>::iterator, bool> ret = mapWallet.insert(make_pair(hash, wtxIn));
        CWalletTx &wtx = (*ret.first).second;
        wtx.BindWallet(this);
        bool fInsertedNew = ret.second;
        if (fInsertedNew) {
            wtx.nTimeReceived = GetAdjustedTime();
            wtx.nOrderPos = IncOrderPosNext(pwalletdb);
            wtxOrdered.insert(make_pair(wtx.nOrderPos, TxPair(&wtx, (CAccountingEntry *) 0)));
            wtx.nTimeSmart = wtx.nTimeReceived;
            if (!wtxIn.hashUnset()) {
                if (mapBlockIndex.count(wtxIn.hashBlock)) {
                    int64_t latestNow = wtx.nTimeReceived;
                    int64_t latestEntry = 0;
                    {
                        // Tolerate times up to the last timestamp in the wallet not more than 5 minutes into the future
                        int64_t latestTolerated = latestNow + 300;
                        const TxItems &txOrdered = wtxOrdered;
                        for (TxItems::const_reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it) {
                            CWalletTx *const pwtx = (*it).second.first;
                            if (pwtx == &wtx)
                                continue;
                            CAccountingEntry *const pacentry = (*it).second.second;
                            int64_t nSmartTime;
                            if (pwtx) {
                                nSmartTime = pwtx->nTimeSmart;
                                if (!nSmartTime)
                                    nSmartTime = pwtx->nTimeReceived;
                            } else
                                nSmartTime = pacentry->nTime;
                            if (nSmartTime <= latestTolerated) {
                                latestEntry = nSmartTime;
                                if (nSmartTime > latestNow)
                                    latestNow = nSmartTime;
                                break;
                            }
                        }
                    }
                    int64_t blocktime = mapBlockIndex[wtxIn.hashBlock]->GetBlockTime();
                    wtx.nTimeSmart = std::max(latestEntry, std::min(blocktime, latestNow));
                } else
                    LogPrint("selectcoins", "AddToWallet(): found %s in block %s not in index\n",
                              wtxIn.GetHash().ToString(),
                              wtxIn.hashBlock.ToString());
            }
            AddToSpends(hash);
        }
        bool fUpdated = false;
        if (!fInsertedNew) {
            // Merge
            if (!wtxIn.hashUnset() && wtxIn.hashBlock != wtx.hashBlock) {
                wtx.hashBlock = wtxIn.hashBlock;
                fUpdated = true;
            }
            // If no longer abandoned, update
            if (wtxIn.hashBlock.IsNull() && wtx.isAbandoned()) {
                wtx.hashBlock = wtxIn.hashBlock;
                fUpdated = true;
            }
            if (wtxIn.nIndex != -1 && (wtxIn.nIndex != wtx.nIndex)) {
                wtx.nIndex = wtxIn.nIndex;
                fUpdated = true;
            }
            if (wtxIn.fFromMe && wtxIn.fFromMe != wtx.fFromMe) {
                wtx.fFromMe = wtxIn.fFromMe;
                fUpdated = true;
            }
        }

        //// debug print
        LogPrint("selectcoins", "AddToWallet %s  %s%s\n", wtxIn.GetHash().ToString(), (fInsertedNew ? "new" : ""),
                  (fUpdated ? "update" : ""));

        // Write to disk
        if (fInsertedNew || fUpdated)
            if (!wtx.WriteToDisk(pwalletdb))
                return false;

        // Break debit/credit balance caches:
        wtx.MarkDirty();

        // Notify UI of new or updated transaction
        NotifyTransactionChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

        // notify an external script when a wallet transaction comes in or is updated
        std::string strCmd = GetArg("-walletnotify", "");

        if (!strCmd.empty()) {
            boost::replace_all(strCmd, "%s", wtxIn.GetHash().GetHex());
            boost::thread t(runCommand, strCmd); // thread runs free
        }

    }
    LogPrint("selectcoins", "CWallet::AddToWallet -> ok\n");
    return true;
}

/**
 * Add a transaction to the wallet, or update it.
 * pblock is optional, but should be provided if the transaction is known to be in a block.
 * If fUpdate is true, existing transactions will be updated.
 */
bool CWallet::AddToWalletIfInvolvingMe(const CTransaction &tx, const CBlock *pblock, bool fUpdate) {
    {
//        LogPrintf("CWallet::AddToWalletIfInvolvingMe, tx=%s\n", tx.GetHash().ToString());
        AssertLockHeld(cs_wallet);

        bool fExisted = mapWallet.count(tx.GetHash()) != 0;
        if (fExisted && !fUpdate) return false;
        if (fExisted || IsMine(tx) || IsFromMe(tx)) {
            CWalletTx wtx(this, tx);

            // Get merkle branch if transaction was found in a block
            if (pblock)
                wtx.SetMerkleBranch(*pblock);

            // Do not flush the wallet here for performance reasons
            // this is safe, as in case of a crash, we rescan the necessary blocks on startup through our SetBestChain-mechanism
            CWalletDB walletdb(strWalletFile, "r+", false);

            return AddToWallet(wtx, false, &walletdb);
        }
    }
//    LogPrintf("CWallet::AddToWalletIfInvolvingMe -> out false!\n");
    return false;
}

bool CWallet::AbandonTransaction(const uint256 &hashTx) {
    LOCK2(cs_main, cs_wallet);

    // Do not flush the wallet here for performance reasons
    CWalletDB walletdb(strWalletFile, "r+", false);

    std::set <uint256> todo;
    std::set <uint256> done;

    // Can't mark abandoned if confirmed or in mempool
    assert(mapWallet.count(hashTx));
    CWalletTx &origtx = mapWallet[hashTx];
    if (origtx.GetDepthInMainChain() > 0 || origtx.InMempool()) {
        return false;
    }

    todo.insert(hashTx);

    while (!todo.empty()) {
        uint256 now = *todo.begin();
        todo.erase(now);
        done.insert(now);
        assert(mapWallet.count(now));
        CWalletTx &wtx = mapWallet[now];
        int currentconfirm = wtx.GetDepthInMainChain();
        // If the orig tx was not in block, none of its spends can be
        assert(currentconfirm <= 0);
        // if (currentconfirm < 0) {Tx and spends are already conflicted, no need to abandon}
        if (currentconfirm == 0 && !wtx.isAbandoned()) {
            // If the orig tx was not in block/mempool, none of its spends can be in mempool
            assert(!wtx.InMempool());
            wtx.nIndex = -1;
            wtx.setAbandoned();
            wtx.MarkDirty();
            wtx.WriteToDisk(&walletdb);
            NotifyTransactionChanged(this, wtx.GetHash(), CT_UPDATED);
            // Iterate over all its outputs, and mark transactions in the wallet that spend them abandoned too
            TxSpends::const_iterator iter = mapTxSpends.lower_bound(COutPoint(hashTx, 0));
            while (iter != mapTxSpends.end() && iter->first.hash == now) {
                if (!done.count(iter->second)) {
                    todo.insert(iter->second);
                }
                iter++;
            }
            // If a transaction changes 'conflicted' state, that changes the balance
            // available of the outputs it spends. So force those to be recomputed
            BOOST_FOREACH(
            const CTxIn &txin, wtx.vin)
            {
                if (mapWallet.count(txin.prevout.hash))
                    mapWallet[txin.prevout.hash].MarkDirty();
            }
        }
    }

    return true;
}

void CWallet::MarkConflicted(const uint256 &hashBlock, const uint256 &hashTx) {
    LOCK2(cs_main, cs_wallet);

    int conflictconfirms = 0;
    if (mapBlockIndex.count(hashBlock)) {
        CBlockIndex *pindex = mapBlockIndex[hashBlock];
        if (chainActive.Contains(pindex)) {
            conflictconfirms = -(chainActive.Height() - pindex->nHeight + 1);
        }
    }
    // If number of conflict confirms cannot be determined, this means
    // that the block is still unknown or not yet part of the main chain,
    // for example when loading the wallet during a reindex. Do nothing in that
    // case.
    if (conflictconfirms >= 0)
        return;

    // Do not flush the wallet here for performance reasons
    CWalletDB walletdb(strWalletFile, "r+", false);

    std::set <uint256> todo;
    std::set <uint256> done;

    todo.insert(hashTx);

    while (!todo.empty()) {
        uint256 now = *todo.begin();
        todo.erase(now);
        done.insert(now);
        assert(mapWallet.count(now));
        CWalletTx &wtx = mapWallet[now];
        int currentconfirm = wtx.GetDepthInMainChain();
        if (conflictconfirms < currentconfirm) {
            // Block is 'more conflicted' than current confirm; update.
            // Mark transaction as conflicted with this block.
            wtx.nIndex = -1;
            wtx.hashBlock = hashBlock;
            wtx.MarkDirty();
            wtx.WriteToDisk(&walletdb);
            // Iterate over all its outputs, and mark transactions in the wallet that spend them conflicted too
            TxSpends::const_iterator iter = mapTxSpends.lower_bound(COutPoint(now, 0));
            while (iter != mapTxSpends.end() && iter->first.hash == now) {
                if (!done.count(iter->second)) {
                    todo.insert(iter->second);
                }
                iter++;
            }
            // If a transaction changes 'conflicted' state, that changes the balance
            // available of the outputs it spends. So force those to be recomputed
            BOOST_FOREACH(const CTxIn &txin, wtx.vin)
            {
                if (mapWallet.count(txin.prevout.hash))
                    mapWallet[txin.prevout.hash].MarkDirty();
            }
        }
    }
}

void CWallet::SyncTransaction(const CTransaction& tx, const CBlock* pblock)
{
    LOCK2(cs_main, cs_wallet);

    if (!AddToWalletIfInvolvingMe(tx, pblock, true))
        return; // Not one of ours

    // If a transaction changes 'conflicted' state, that changes the balance
    // available of the outputs it spends. So force those to be
    // recomputed, also:
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        if (mapWallet.count(txin.prevout.hash))
            mapWallet[txin.prevout.hash].MarkDirty();
    }

    fAnonymizableTallyCached = false;
    fAnonymizableTallyCachedNonDenom = false;
}


isminetype CWallet::IsMine(const CTxIn &txin) const {
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end()) {
            const CWalletTx &prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                return IsMine(prev.vout[txin.prevout.n]);
        }
    }
    return ISMINE_NO;
}

CAmount CWallet::GetDebit(const CTxIn &txin, const isminefilter &filter) const {
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end()) {
            const CWalletTx &prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                if (IsMine(prev.vout[txin.prevout.n]) & filter)
                    return prev.vout[txin.prevout.n].nValue;
        }
    }
    return 0;
}

isminetype CWallet::IsMine(const CTxOut &txout) const {
    return ::IsMine(*this, txout.scriptPubKey);
}

CAmount CWallet::GetCredit(const CTxOut &txout, const isminefilter &filter) const {
    if (!MoneyRange(txout.nValue))
        throw std::runtime_error(std::string(__func__) + ": value out of range");
    return ((IsMine(txout) & filter) ? txout.nValue : 0);
}

bool CWallet::IsChange(const CTxOut &txout) const {
    // TODO: fix handling of 'change' outputs. The assumption is that any
    // payment to a script that is ours, but is not in the address book
    // is change. That assumption is likely to break when we implement multisignature
    // wallets that return change back into a multi-signature-protected address;
    // a better way of identifying which outputs are 'the send' and which are
    // 'the change' will need to be implemented (maybe extend CWalletTx to remember
    // which output, if any, was change).
    if (::IsMine(*this, txout.scriptPubKey)) {
        CTxDestination address;
        if (!ExtractDestination(txout.scriptPubKey, address))
            return true;

        LOCK(cs_wallet);
        if (!mapAddressBook.count(address))
            return true;
    }
    return false;
}

CAmount CWallet::GetChange(const CTxOut &txout) const {
    if (!MoneyRange(txout.nValue))
        throw std::runtime_error(std::string(__func__) + ": value out of range");
    return (IsChange(txout) ? txout.nValue : 0);
}

void CWallet::GenerateNewHDChain()
{
    CHDChain newHdChain;

    std::string strSeed = GetArg("-hdseed", "not hex");

    if(mapArgs.count("-hdseed") && IsHex(strSeed)) {
        std::vector<unsigned char> vchSeed = ParseHex(strSeed);
        if (!newHdChain.SetSeed(SecureVector(vchSeed.begin(), vchSeed.end()), true))
            throw std::runtime_error(std::string(__func__) + ": SetSeed failed");
    }
    else {
        if (mapArgs.count("-hdseed") && !IsHex(strSeed))
            LogPrintf("CWallet::GenerateNewHDChain -- Incorrect seed, generating random one instead\n");

        // NOTE: empty mnemonic means "generate a new one for me"
        std::string strMnemonic = GetArg("-mnemonic", "");
        // NOTE: default mnemonic passphrase is an empty string
        std::string strMnemonicPassphrase = GetArg("-mnemonicpassphrase", "");

        SecureVector vchMnemonic(strMnemonic.begin(), strMnemonic.end());
        SecureVector vchMnemonicPassphrase(strMnemonicPassphrase.begin(), strMnemonicPassphrase.end());

        if (!newHdChain.SetMnemonic(vchMnemonic, vchMnemonicPassphrase, true))
            throw std::runtime_error(std::string(__func__) + ": SetMnemonic failed");
    }
    newHdChain.Debug(__func__);

    if (!SetHDChain(newHdChain, false))
        throw std::runtime_error(std::string(__func__) + ": SetHDChain failed");

    // clean up
    mapArgs.erase("-hdseed");
    mapArgs.erase("-mnemonic");
    mapArgs.erase("-mnemonicpassphrase");
}

bool CWallet::SetHDChain(const CHDChain& chain, bool memonly)
{
    LOCK(cs_wallet);

    if (!CCryptoKeyStore::SetHDChain(chain))
        return false;

    if (!memonly && !CWalletDB(strWalletFile).WriteHDChain(chain))
        throw std::runtime_error(std::string(__func__) + ": WriteHDChain failed");

    return true;
}

bool CWallet::SetCryptedHDChain(const CHDChain& chain, bool memonly)
{
    LOCK(cs_wallet);

    if (!CCryptoKeyStore::SetCryptedHDChain(chain))
        return false;

    if (!memonly) {
        if (!fFileBacked)
            return false;
        if (pwalletdbEncryption) {
            if (!pwalletdbEncryption->WriteCryptedHDChain(chain))
                throw std::runtime_error(std::string(__func__) + ": WriteCryptedHDChain failed");
        } else {
            if (!CWalletDB(strWalletFile).WriteCryptedHDChain(chain))
                throw std::runtime_error(std::string(__func__) + ": WriteCryptedHDChain failed");
        }
    }

    return true;
}

bool CWallet::GetDecryptedHDChain(CHDChain& hdChainRet)
{
    LOCK(cs_wallet);

    CHDChain hdChainTmp;
    if (!GetHDChain(hdChainTmp)) {
        return false;
    }

    if (!DecryptHDChain(hdChainTmp))
        return false;

    // make sure seed matches this chain
    if (hdChainTmp.GetID() != hdChainTmp.GetSeedHash())
        return false;

    hdChainRet = hdChainTmp;

    return true;
}

bool CWallet::IsHDEnabled()
{
    CHDChain hdChainCurrent;
    return GetHDChain(hdChainCurrent);
}

bool CWallet::IsMine(const CTransaction &tx) const {
    BOOST_FOREACH(const CTxOut &txout, tx.vout)
    if (IsMine(txout) && txout.nValue >= nMinimumInputValue)
        return true;
    return false;
}

bool CWallet::IsFromMe(const CTransaction &tx) const {
    return (GetDebit(tx, ISMINE_ALL) > 0);
}

CAmount CWallet::GetDebit(const CTransaction &tx, const isminefilter &filter) const {
    CAmount nDebit = 0;
    BOOST_FOREACH(
    const CTxIn &txin, tx.vin)
    {
        nDebit += GetDebit(txin, filter);
        if (!MoneyRange(nDebit))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
    }
    return nDebit;
}

CAmount CWallet::GetCredit(const CTransaction &tx, const isminefilter &filter) const {
    CAmount nCredit = 0;
    BOOST_FOREACH(
    const CTxOut &txout, tx.vout)
    {
        nCredit += GetCredit(txout, filter);
        if (!MoneyRange(nCredit))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
    }
    return nCredit;
}

CAmount CWallet::GetChange(const CTransaction &tx) const {
    CAmount nChange = 0;
    BOOST_FOREACH(
    const CTxOut &txout, tx.vout)
    {
        nChange += GetChange(txout);
        if (!MoneyRange(nChange))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
    }
    return nChange;
}

int64_t CWalletTx::GetTxTime() const {
    int64_t n = nTimeSmart;
    return n ? n : nTimeReceived;
}

int CWalletTx::GetRequestCount() const {
    // Returns -1 if it wasn't being tracked
    int nRequests = -1;
    {
        LOCK(pwallet->cs_wallet);
        if (IsCoinBase()) {
            // Generated block
            if (!hashUnset()) {
                map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                if (mi != pwallet->mapRequestCount.end())
                    nRequests = (*mi).second;
            }
        } else {
            // Did anyone request this transaction?
            map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(GetHash());
            if (mi != pwallet->mapRequestCount.end()) {
                nRequests = (*mi).second;

                // How about the block it's in?
                if (nRequests == 0 && !hashUnset()) {
                    map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                    if (mi != pwallet->mapRequestCount.end())
                        nRequests = (*mi).second;
                    else
                        nRequests = 1; // If it's in someone else's block it must have got out
                }
            }
        }
    }
    return nRequests;
}

void CWalletTx::GetAmounts(list <COutputEntry> &listReceived,
                           list <COutputEntry> &listSent, CAmount &nFee, string &strSentAccount,
                           const isminefilter &filter) const {
    nFee = 0;
    listReceived.clear();
    listSent.clear();
    strSentAccount = strFromAccount;

    // Compute fee:
    CAmount nDebit = GetDebit(filter);
    if (nDebit > 0) // debit>0 means we signed/sent this transaction
    {
        CAmount nValueOut = GetValueOut();
        nFee = nDebit - nValueOut;
    }

    // Sent/received.
    for (unsigned int i = 0; i < vout.size(); ++i) {
        const CTxOut &txout = vout[i];
        isminetype fIsMine = pwallet->IsMine(txout);
        // Only need to handle txouts if AT LEAST one of these is true:
        //   1) they debit from us (sent)
        //   2) the output is to us (received)
        if (nDebit > 0) {
            // Don't report 'change' txouts
            if (pwallet->IsChange(txout))
                continue;
        } else if (!(fIsMine & filter))
            continue;

        // In either case, we need to get the destination address
        CTxDestination address;

        if (!ExtractDestination(txout.scriptPubKey, address) && !txout.scriptPubKey.IsUnspendable()) {
            LogPrintf("CWalletTx::GetAmounts: Unknown transaction type found, txid %s\n",
                      this->GetHash().ToString());
            address = CNoDestination();
        }

        COutputEntry output = {address, txout.nValue, (int) i};

        // If we are debited by the transaction, add the output as a "sent" entry
        if (nDebit > 0)
            listSent.push_back(output);

        // If we are receiving the output, add it as a "received" entry
        if (fIsMine & filter)
            listReceived.push_back(output);
    }

}

void CWalletTx::GetAccountAmounts(const string &strAccount, CAmount &nReceived,
                                  CAmount &nSent, CAmount &nFee, const isminefilter &filter) const {
    nReceived = nSent = nFee = 0;

    CAmount allFee;
    string strSentAccount;
    list <COutputEntry> listReceived;
    list <COutputEntry> listSent;
    GetAmounts(listReceived, listSent, allFee, strSentAccount, filter);

    if (strAccount == strSentAccount) {
        BOOST_FOREACH(
        const COutputEntry &s, listSent)
        nSent += s.amount;
        nFee = allFee;
    }
    {
        LOCK(pwallet->cs_wallet);
        BOOST_FOREACH(
        const COutputEntry &r, listReceived)
        {
            if (pwallet->mapAddressBook.count(r.destination)) {
                map<CTxDestination, CAddressBookData>::const_iterator mi = pwallet->mapAddressBook.find(r.destination);
                if (mi != pwallet->mapAddressBook.end() && (*mi).second.name == strAccount)
                    nReceived += r.amount;
            } else if (strAccount.empty()) {
                nReceived += r.amount;
            }
        }
    }
}

bool CWalletTx::WriteToDisk(CWalletDB *pwalletdb)
{
    return pwalletdb->WriteTx(GetHash(), *this);
}

/**
 * Scan the block chain (starting in pindexStart) for transactions
 * from or to us. If fUpdate is true, found transactions that already
 * exist in the wallet will be updated.
 */
int CWallet::ScanForWalletTransactions(CBlockIndex *pindexStart, bool fUpdate) {
    int ret = 0;
    int64_t nNow = GetTime();
    const CChainParams &chainParams = Params();

    CBlockIndex *pindex = pindexStart;
    {
        LOCK2(cs_main, cs_wallet);

        // no need to read and scan block, if block was created before
        // our wallet birthday (as adjusted for block time variability)
        while (pindex && nTimeFirstKey && (pindex->GetBlockTime() < (nTimeFirstKey - 7200)))
            pindex = chainActive.Next(pindex);

        ShowProgress(_("Rescanning..."),
                     0); // show rescan progress in GUI as dialog or on splashscreen, if -rescan on startup
        double dProgressStart = Checkpoints::GuessVerificationProgress(chainParams.Checkpoints(), pindex, false);
        double dProgressTip = Checkpoints::GuessVerificationProgress(chainParams.Checkpoints(), chainActive.Tip(),
                                                                     false);
        while (pindex) {
            if (pindex->nHeight % 100 == 0 && dProgressTip - dProgressStart > 0.0)
                ShowProgress(_("Rescanning..."), std::max(1, std::min(99,
                                                                      (int) ((Checkpoints::GuessVerificationProgress(
                                                                              chainParams.Checkpoints(), pindex,
                                                                              false) - dProgressStart) /
                                                                             (dProgressTip - dProgressStart) * 100))));

            CBlock block;
            ReadBlockFromDisk(block, pindex, Params().GetConsensus());
            BOOST_FOREACH(CTransaction & tx, block.vtx)
            {
                if (AddToWalletIfInvolvingMe(tx, &block, fUpdate))
                    ret++;
            }
            pindex = chainActive.Next(pindex);
            if (GetTime() >= nNow + 60) {
                nNow = GetTime();
                LogPrintf("Still rescanning. At block %d. Progress=%f\n", pindex->nHeight,
                          Checkpoints::GuessVerificationProgress(chainParams.Checkpoints(), pindex));
            }
        }
        ShowProgress(_("Rescanning..."), 100); // hide progress dialog in GUI
    }
    return ret;
}

void CWallet::ReacceptWalletTransactions() {
    LogPrintf("CWallet::ReacceptWalletTransactions()\n");
    // If transactions aren't being broadcasted, don't let them into local mempool either
    if (!fBroadcastTransactions)
        return;
    LOCK2(cs_main, cs_wallet);
    std::map < int64_t, CWalletTx * > mapSorted;

    // Sort pending wallet transactions based on their initial wallet insertion order
    BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)&item, mapWallet)
    {
        const uint256 &wtxid = item.first;
        CWalletTx &wtx = item.second;
        assert(wtx.GetHash() == wtxid);

        int nDepth = wtx.GetDepthInMainChain();

        if ((wtx.IsCoinBase() ||  wtx.IsZerocoinSpend()) && (nDepth == 0 && !wtx.isAbandoned()))
            continue;

        if (nDepth == 0 && !wtx.isAbandoned()) {
            mapSorted.insert(std::make_pair(wtx.nOrderPos, &wtx));
        }
    }

    // Try to add wallet transactions to memory pool
    BOOST_FOREACH(PAIRTYPE(const int64_t, CWalletTx*)& item, mapSorted)
    {
        CWalletTx& wtx = *(item.second);

        LOCK(mempool.cs);
        wtx.AcceptToMemoryPool(false);
    }
}

bool CWalletTx::RelayWalletTransaction(CConnman* connman, std::string strCommand)
{
    assert(pwallet->GetBroadcastTransactions());
    if (!IsCoinBase())
    {
        if (GetDepthInMainChain() == 0 && !isAbandoned() && InMempool()) {
            uint256 hash = GetHash();
            LogPrintf("Relaying wtx %s\n", hash.ToString());

            if(strCommand == NetMsgType::TXLOCKREQUEST) {
                instantsend.ProcessTxLockRequest(((CTxLockRequest)*this), *connman);
            }
            if (connman) {
                connman->RelayTransaction((CTransaction)*this);
                return true;
            }
        }
    }
    return false;
}

set <uint256> CWalletTx::GetConflicts() const {
    set <uint256> result;
    if (pwallet != NULL) {
        uint256 myHash = GetHash();
        result = pwallet->GetConflicts(myHash);
        result.erase(myHash);
    }
    return result;
}

CAmount CWalletTx::GetDebit(const isminefilter &filter) const {
    if (vin.empty())
        return 0;

    CAmount debit = 0;
    if (filter & ISMINE_SPENDABLE) {
        if (fDebitCached)
            debit += nDebitCached;
        else {
            nDebitCached = pwallet->GetDebit(*this, ISMINE_SPENDABLE);
            fDebitCached = true;
            debit += nDebitCached;
        }
    }
    if (filter & ISMINE_WATCH_ONLY) {
        if (fWatchDebitCached)
            debit += nWatchDebitCached;
        else {
            nWatchDebitCached = pwallet->GetDebit(*this, ISMINE_WATCH_ONLY);
            fWatchDebitCached = true;
            debit += nWatchDebitCached;
        }
    }
    return debit;
}

CAmount CWalletTx::GetCredit(const isminefilter &filter) const {
    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if (IsCoinBase() && GetBlocksToMaturity() > 0)
        return 0;

    int64_t credit = 0;
    if (filter & ISMINE_SPENDABLE) {
        // GetBalance can assume transactions in mapWallet won't change
        if (fCreditCached)
            credit += nCreditCached;
        else {
            nCreditCached = pwallet->GetCredit(*this, ISMINE_SPENDABLE);
            fCreditCached = true;
            credit += nCreditCached;
        }
    }
    if (filter & ISMINE_WATCH_ONLY) {
        if (fWatchCreditCached)
            credit += nWatchCreditCached;
        else {
            nWatchCreditCached = pwallet->GetCredit(*this, ISMINE_WATCH_ONLY);
            fWatchCreditCached = true;
            credit += nWatchCreditCached;
        }
    }
    return credit;
}

CAmount CWalletTx::GetImmatureCredit(bool fUseCache) const {
    if (IsCoinBase() && GetBlocksToMaturity() > 0 && IsInMainChain()) {
        if (fUseCache && fImmatureCreditCached)
            return nImmatureCreditCached;
        nImmatureCreditCached = pwallet->GetCredit(*this, ISMINE_SPENDABLE);
        fImmatureCreditCached = true;
        return nImmatureCreditCached;
    }

    return 0;
}

CAmount CWalletTx::GetAvailableCredit(bool fUseCache) const {
    if (pwallet == 0)
        return 0;

    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if (IsCoinBase() && GetBlocksToMaturity() > 0)
        return 0;

    if (fUseCache && fAvailableCreditCached)
        return nAvailableCreditCached;

    CAmount nCredit = 0;
    uint256 hashTx = GetHash();
    for (unsigned int i = 0; i < vout.size(); i++) {
        if (!pwallet->IsSpent(hashTx, i)) {
            const CTxOut &txout = vout[i];
            nCredit += pwallet->GetCredit(txout, ISMINE_SPENDABLE);
            if (!MoneyRange(nCredit))
                throw std::runtime_error("CWalletTx::GetAvailableCredit() : value out of range");
        }
    }

    nAvailableCreditCached = nCredit;
    fAvailableCreditCached = true;
    return nCredit;
}

CAmount CWalletTx::GetImmatureWatchOnlyCredit(const bool &fUseCache) const {
    if (IsCoinBase() && GetBlocksToMaturity() > 0 && IsInMainChain()) {
        if (fUseCache && fImmatureWatchCreditCached)
            return nImmatureWatchCreditCached;
        nImmatureWatchCreditCached = pwallet->GetCredit(*this, ISMINE_WATCH_ONLY);
        fImmatureWatchCreditCached = true;
        return nImmatureWatchCreditCached;
    }

    return 0;
}

CAmount CWalletTx::GetAvailableWatchOnlyCredit(const bool &fUseCache) const {
    if (pwallet == 0)
        return 0;

    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if (IsCoinBase() && GetBlocksToMaturity() > 0)
        return 0;

    if (fUseCache && fAvailableWatchCreditCached)
        return nAvailableWatchCreditCached;

    CAmount nCredit = 0;
    for (unsigned int i = 0; i < vout.size(); i++) {
        if (!pwallet->IsSpent(GetHash(), i)) {
            const CTxOut &txout = vout[i];
            nCredit += pwallet->GetCredit(txout, ISMINE_WATCH_ONLY);
            if (!MoneyRange(nCredit))
                throw std::runtime_error("CWalletTx::GetAvailableCredit() : value out of range");
        }
    }

    nAvailableWatchCreditCached = nCredit;
    fAvailableWatchCreditCached = true;
    return nCredit;
}

CAmount CWalletTx::GetChange() const {
    if (fChangeCached)
        return nChangeCached;
    nChangeCached = pwallet->GetChange(*this);
    fChangeCached = true;
    return nChangeCached;
}

bool CWalletTx::InMempool() const {
    LOCK(mempool.cs);
    if (mempool.exists(GetHash())) {
        return true;
    }
    return false;
}

bool CWalletTx::IsTrusted() const {
    // Quick answer in most cases
    if (!CheckFinalTx(*this))
        return false;
    int nDepth = GetDepthInMainChain();
    if (nDepth >= 1)
        return true;
    if (nDepth < 0)
        return false;
    if (!bSpendZeroConfChange || !IsFromMe(ISMINE_ALL)) // using wtx's cached debit
        return false;

    // Don't trust unconfirmed transactions from us unless they are in the mempool.
    if (!InMempool())
        return false;

    // Trusted if all inputs are from us and are in the mempool:
    BOOST_FOREACH(
    const CTxIn &txin, vin)
    {
        // Transactions not sent by us: not trusted
        const CWalletTx *parent = pwallet->GetWalletTx(txin.prevout.hash);
        if (parent == NULL)
            return false;
        const CTxOut &parentOut = parent->vout[txin.prevout.n];
        if (pwallet->IsMine(parentOut) != ISMINE_SPENDABLE)
            return false;
    }
    return true;
}

bool CWalletTx::IsEquivalentTo(const CWalletTx &tx) const {
    CMutableTransaction tx1 = *this;
    CMutableTransaction tx2 = tx;
    for (unsigned int i = 0; i < tx1.vin.size(); i++) tx1.vin[i].scriptSig = CScript();
    for (unsigned int i = 0; i < tx2.vin.size(); i++) tx2.vin[i].scriptSig = CScript();
    return CTransaction(tx1) == CTransaction(tx2);
}

std::vector<uint256> CWallet::ResendWalletTransactionsBefore(int64_t nTime, CConnman* connman)
{
    std::vector<uint256> result;

    LOCK(cs_wallet);
    // Sort them in chronological order
    multimap<unsigned int, CWalletTx*> mapSorted;
    BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet)
    {
        CWalletTx& wtx = item.second;
        // Don't rebroadcast if newer than nTime:
        if (wtx.nTimeReceived > nTime)
            continue;
        mapSorted.insert(make_pair(wtx.nTimeReceived, &wtx));
    }
    BOOST_FOREACH(PAIRTYPE(const unsigned int, CWalletTx*)& item, mapSorted)
    {
        CWalletTx& wtx = *item.second;
        if (wtx.RelayWalletTransaction(connman))
            result.push_back(wtx.GetHash());
    }
    return result;
}

void CWallet::ResendWalletTransactions(int64_t nBestBlockTime, CConnman* connman)
{
    // Do this infrequently and randomly to avoid giving away
    // that these are our transactions.
    if (GetTime() < nNextResend || !fBroadcastTransactions)
        return;
    bool fFirst = (nNextResend == 0);
    nNextResend = GetTime() + GetRand(30 * 60);
    if (fFirst)
        return;

    // Only do it if there's been a new block since last time
    if (nBestBlockTime < nLastResend)
        return;
    nLastResend = GetTime();

    // Rebroadcast unconfirmed txes older than 5 minutes before the last
    // block was found:
    std::vector<uint256> relayed = ResendWalletTransactionsBefore(nBestBlockTime-5*60, connman);
    if (!relayed.empty())
        LogPrintf("%s: rebroadcast %u unconfirmed transactions\n", __func__, relayed.size());
}

/** @} */ // end of mapWallet




/** @defgroup Actions
 *
 * @{
 */


CAmount CWallet::GetBalance() const {
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx *pcoin = &(*it).second;
            if (pcoin->IsTrusted())
                nTotal += pcoin->GetAvailableCredit();
        }
    }

    return nTotal;
}

// CAmount CWallet::GetAnonymizableBalance(bool fSkipDenominated, bool fSkipUnconfirmed) const
// {
//     if(fLiteMode) return 0;

//     std::vector<CompactTallyItem> vecTally;
//     if(!SelectCoinsGrouppedByAddresses(vecTally, fSkipDenominated, true, fSkipUnconfirmed)) return 0;

//     CAmount nTotal = 0;

//     const CAmount nSmallestDenom = CPrivateSend::GetSmallestDenomination();
//     const CAmount nMixingCollateral = CPrivateSend::GetCollateralAmount();
//     BOOST_FOREACH(CompactTallyItem& item, vecTally) {
//         bool fIsDenominated = CPrivateSend::IsDenominatedAmount(item.nAmount);
//         if(fSkipDenominated && fIsDenominated) continue;
//         // assume that the fee to create denoms should be mixing collateral at max
//         if(item.nAmount >= nSmallestDenom + (fIsDenominated ? 0 : nMixingCollateral))
//             nTotal += item.nAmount;
//     }

//     return nTotal;
// }

// CAmount CWallet::GetAnonymizedBalance() const
// {
//     if(fLiteMode) return 0;

//     CAmount nTotal = 0;

//     LOCK2(cs_main, cs_wallet);

//     std::set<uint256> setWalletTxesCounted;
//     for (auto& outpoint : setWalletUTXO) {

//         if (setWalletTxesCounted.find(outpoint.hash) != setWalletTxesCounted.end()) continue;
//         setWalletTxesCounted.insert(outpoint.hash);

//         for (map<uint256, CWalletTx>::const_iterator it = mapWallet.find(outpoint.hash); it != mapWallet.end() && it->first == outpoint.hash; ++it) {
//             if (it->second.IsTrusted())
//                 nTotal += it->second.GetAnonymizedCredit();
//         }
//     }

//     return nTotal;
// }

// CAmount CWalletTx::GetAnonymizedCredit(bool fUseCache) const
// {
//     if (pwallet == 0)
//         return 0;

//     // Must wait until coinbase is safely deep enough in the chain before valuing it
//     if (IsCoinBase() && GetBlocksToMaturity() > 0)
//         return 0;

// //    if (fUseCache && fAnonymizedCreditCached)
// //        return nAnonymizedCreditCached;

//     CAmount nCredit = 0;
//     uint256 hashTx = GetHash();
//     for (unsigned int i = 0; i < vout.size(); i++)
//     {
//         const CTxOut &txout = vout[i];
//         const CTxIn txin = CTxIn(hashTx, i);

//         if(pwallet->IsSpent(hashTx, i) || !pwallet->IsDenominated(txin)) continue;

// //        const int nRounds = pwallet->GetInputPrivateSendRounds(txin);
//         const int nRounds = 0;
//         if(nRounds >= nPrivateSendRounds){
//             nCredit += pwallet->GetCredit(txout, ISMINE_SPENDABLE);
//             if (!MoneyRange(nCredit))
//                 throw std::runtime_error("CWalletTx::GetAnonymizedCredit() : value out of range");
//         }
//     }

// //    nAnonymizedCreditCached = nCredit;
// //    fAnonymizedCreditCached = true;
//     return nCredit;
// }


// CAmount CWallet::GetNeedsToBeAnonymizedBalance(CAmount nMinBalance) const
// {
//     if(fLiteMode) return 0;

//     CAmount nAnonymizedBalance = GetAnonymizedBalance();
//     CAmount nNeedsToAnonymizeBalance = nPrivateSendAmount*COIN - nAnonymizedBalance;

//     // try to overshoot target DS balance up to nMinBalance
//     nNeedsToAnonymizeBalance += nMinBalance;

//     CAmount nAnonymizableBalance = GetAnonymizableBalance();

//     // anonymizable balance is way too small
//     if(nAnonymizableBalance < nMinBalance) return 0;

//     // not enough funds to anonymze amount we want, try the max we can
//     if(nNeedsToAnonymizeBalance > nAnonymizableBalance) nNeedsToAnonymizeBalance = nAnonymizableBalance;

//     // we should never exceed the pool max
//     if (nNeedsToAnonymizeBalance > PRIVATESEND_POOL_MAX) nNeedsToAnonymizeBalance = PRIVATESEND_POOL_MAX;

//     return nNeedsToAnonymizeBalance;
// }

// CAmount CWallet::GetDenominatedBalance(bool unconfirmed) const
// {
//     if(fLiteMode) return 0;

//     CAmount nTotal = 0;
// //     {
// //         LOCK2(cs_main, cs_wallet);
// //         for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
// //         {
// //             const CWalletTx* pcoin = &(*it).second;

// // //            nTotal += pcoin->GetDenominatedCredit(unconfirmed);
// //         }
// //     }

//     return nTotal;
// }

CAmount CWallet::GetUnconfirmedBalance() const {
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx *pcoin = &(*it).second;
            if (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0 && pcoin->InMempool())
                nTotal += pcoin->GetAvailableCredit();
        }
    }
    return nTotal;
}

CAmount CWallet::GetImmatureBalance() const {
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx *pcoin = &(*it).second;
            nTotal += pcoin->GetImmatureCredit();
        }
    }
    return nTotal;
}

CAmount CWallet::GetWatchOnlyBalance() const {
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx *pcoin = &(*it).second;
            if (pcoin->IsTrusted())
                nTotal += pcoin->GetAvailableWatchOnlyCredit();
        }
    }

    return nTotal;
}

CAmount CWallet::GetUnconfirmedWatchOnlyBalance() const {
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx *pcoin = &(*it).second;
            if (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0 && pcoin->InMempool())
                nTotal += pcoin->GetAvailableWatchOnlyCredit();
        }
    }
    return nTotal;
}

// bool CWallet::IsDenominated(const CTxIn &txin) const
// {
//     LOCK(cs_wallet);

//     map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
//     if (mi != mapWallet.end()) {
//         const CWalletTx& prev = (*mi).second;
//         if (txin.prevout.n < prev.vout.size()) {
//             return IsDenominatedAmount(prev.vout[txin.prevout.n].nValue);
//         }
//     }

//     return false;
// }

// bool CWallet::IsDenominatedAmount(CAmount nInputAmount) const
// {
// //    BOOST_FOREACH(CAmount d, vecPrivateSendDenominations)
// //    if(nInputAmount == d)
// //        return true;
//     return false;
// }

// bool CWallet::HasCollateralInputs(bool fOnlyConfirmed) const
// {
//     vector<COutput> vCoins;
//     AvailableCoins(vCoins, fOnlyConfirmed, NULL, false, ONLY_PRIVATESEND_COLLATERAL);

//     return !vCoins.empty();
// }


// bool CWallet::IsCollateralAmount(CAmount nInputAmount) const
// {
//     // collateral inputs should always be a 2x..4x of PRIVATESEND_COLLATERAL
//     return  nInputAmount >= PRIVATESEND_COLLATERAL * 2 &&
//             nInputAmount <= PRIVATESEND_COLLATERAL * 4 &&
//             nInputAmount %  PRIVATESEND_COLLATERAL == 0;
// }

CAmount CWallet::GetImmatureWatchOnlyBalance() const {
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx *pcoin = &(*it).second;
            nTotal += pcoin->GetImmatureWatchOnlyCredit();
        }
    }
    return nTotal;
}

void CWallet::AvailableCoins(vector <COutput> &vCoins, bool fOnlyConfirmed, const CCoinControl *coinControl,
                             bool fIncludeZeroValue, AvailableCoinsType nCoinType, bool fUseInstantSend) const {
    vCoins.clear();

    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const uint256 &wtxid = it->first;
            const CWalletTx *pcoin = &(*it).second;

            if (!CheckFinalTx(*pcoin))
                continue;

            if (fOnlyConfirmed && !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain(false);
            // do not use IX for inputs that have less then INSTANTSEND_CONFIRMATIONS_REQUIRED blockchain confirmations
            if (fUseInstantSend && nDepth < INSTANTSEND_CONFIRMATIONS_REQUIRED)
                continue;

            // We should not consider coins which aren't at least in our mempool
            // It's possible for these to be conflicted via ancestors which we may never be able to detect
            if (nDepth == 0 && !pcoin->InMempool())
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
                bool found = false;
                if(nCoinType == ONLY_DENOMINATED) {
                    //found = CPrivateSend::IsDenominatedAmount(pcoin->vout[i].nValue);
                } else if(nCoinType == ONLY_NONDENOMINATED) {
                    //if (CPrivateSend::IsCollateralAmount(pcoin->vout[i].nValue)) continue; // do not use collateral amounts
                    //found = !CPrivateSend::IsDenominatedAmount(pcoin->vout[i].nValue);
                } else if(nCoinType == ONLY_10000) {
                    found = pcoin->vout[i].nValue == 100000*COIN;
                } else if(nCoinType == ONLY_PRIVATESEND_COLLATERAL) {
                    //found = CPrivateSend::IsCollateralAmount(pcoin->vout[i].nValue);
                } else {
                    found = true;
                }
                if(!found) continue;

                isminetype mine = IsMine(pcoin->vout[i]);
                if (!(IsSpent(wtxid, i)) && mine != ISMINE_NO &&
                    (!IsLockedCoin((*it).first, i) || nCoinType == ONLY_10000) &&
                    (pcoin->vout[i].nValue > 0 || fIncludeZeroValue) &&
                    (!coinControl || !coinControl->HasSelected() || coinControl->fAllowOtherInputs || coinControl->IsSelected(COutPoint((*it).first, i)))){

                        vCoins.push_back(COutput(pcoin, i, nDepth,
                                                 ((mine & ISMINE_SPENDABLE) != ISMINE_NO) ||
                                                  (coinControl && coinControl->fAllowWatchOnly && (mine & ISMINE_WATCH_SOLVABLE) != ISMINE_NO),
                                                 (mine & (ISMINE_SPENDABLE | ISMINE_WATCH_SOLVABLE)) != ISMINE_NO, pcoin->vout[i].GetLockTime()));
                }

            }
        }
    }
}

void CWallet::AvailableCoins(vector <COutput> &vCoins, const CSmartAddress& address) const {
    vCoins.clear();

    {
        LOCK2(cs_main, cs_wallet);

        CScript addressScript = address.GetScript();

        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const uint256 &wtxid = it->first;
            const CWalletTx *pcoin = &(*it).second;

            if (!CheckFinalTx(*pcoin))
                continue;

            if (!pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain(false);

            // We should not consider coins which aren't at least in our mempool
            // It's possible for these to be conflicted via ancestors which we may never be able to detect
            if (nDepth == 0 && !pcoin->InMempool())
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++) {

                if( pcoin->vout[i].scriptPubKey != addressScript)
                    continue;

                isminetype mine = IsMine(pcoin->vout[i]);
                if (!(IsSpent(wtxid, i)) && mine != ISMINE_NO &&
                    !IsLockedCoin((*it).first, i) &&
                    pcoin->vout[i].nValue > 0){                        
                        vCoins.push_back(COutput(pcoin, i, nDepth,
                                                 ((mine & ISMINE_SPENDABLE) != ISMINE_NO) ||
                                                  false,
                                                 (mine & (ISMINE_SPENDABLE | ISMINE_WATCH_SOLVABLE)) != ISMINE_NO,
                                                 pcoin->vout[i].GetLockTime()));
                }

            }
        }
    }
}

bool CWallet::SelectCoinsDark(CAmount nValueMin, CAmount nValueMax, std::vector<CTxIn>& vecTxInRet, CAmount& nValueRet, int nPrivateSendRoundsMin, int nPrivateSendRoundsMax) const
{
    //CCoinControl *coinControl=NULL;

    vecTxInRet.clear();
    nValueRet = 0;

    vector<COutput> vCoins;
    //AvailableCoins(vCoins, true, coinControl, false, nPrivateSendRoundsMin < 0 ? ONLY_NONDENOMINATED_NOT1000IFMN : ONLY_DENOMINATED);

    //order the array so largest nondenom are first, then denominations, then very small inputs.
//    sort(vCoins.rbegin(), vCoins.rend(), CompareByPriority());

    BOOST_FOREACH(const COutput& out, vCoins)
    {
        //do not allow inputs less than 1/10th of minimum value
        if(out.tx->vout[out.i].nValue < nValueMin/10) continue;
        //do not allow collaterals to be selected
        if(IsCollateralAmount(out.tx->vout[out.i].nValue)) continue;
        if(fSmartNode && out.tx->vout[out.i].nValue == 100000*COIN) continue; //smartnode input

        if(nValueRet + out.tx->vout[out.i].nValue <= nValueMax){
            CTxIn txin = CTxIn(out.tx->GetHash(),out.i);

//            int nRounds = GetInputPrivateSendRounds(txin);
            int nRounds = 0;
            if(nRounds >= nPrivateSendRoundsMax) continue;
            if(nRounds < nPrivateSendRoundsMin) continue;

            txin.prevPubKey = out.tx->vout[out.i].scriptPubKey; // the inputs PubKey
            nValueRet += out.tx->vout[out.i].nValue;
            vecTxInRet.push_back(txin);
        }
    }

    return nValueRet >= nValueMin;
}

bool CWallet::GetSmartnodeOutpointAndKeys(COutPoint& outpointRet, CPubKey& pubKeyRet, CKey& keyRet, std::string strTxHash, std::string strOutputIndex)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    // Find possible candidates
    std::vector<COutput> vPossibleCoins;
    AvailableCoins(vPossibleCoins, true, NULL, false, ONLY_10000);
    if(vPossibleCoins.empty()) {
        LogPrintf("CWallet::GetSmartnodeOutpointAndKeys -- Could not locate any valid smartnode vin\n");
        return false;
    }

    if(strTxHash.empty()) // No output specified, select the first one
        return GetOutpointAndKeysFromOutput(vPossibleCoins[0], outpointRet, pubKeyRet, keyRet);

    // Find specific vin
    uint256 txHash = uint256S(strTxHash);
    int nOutputIndex = atoi(strOutputIndex.c_str());

    BOOST_FOREACH(COutput& out, vPossibleCoins)
        if(out.tx->GetHash() == txHash && out.i == nOutputIndex) // found it!
            return GetOutpointAndKeysFromOutput(out, outpointRet, pubKeyRet, keyRet);

    LogPrintf("CWallet::GetSmartnodeOutpointAndKeys -- Could not locate specified smartnode vin\n");
    return false;
}

// bool CWallet::GetVinAndKeysFromOutput(COutput out, CTxIn& txinRet, CPubKey& pubKeyRet, CKey& keyRet)
// {
//     // wait for reindex and/or import to finish
//     if (fImporting || fReindex) return false;

//     CScript pubScript;

//     txinRet = CTxIn(out.tx->GetHash(), out.i);
//     pubScript = out.tx->vout[out.i].scriptPubKey; // the inputs PubKey

//     CTxDestination address1;
//     ExtractDestination(pubScript, address1);
//     CBitcoinAddress address2(address1);

//     CKeyID keyID;
//     if (!address2.GetKeyID(keyID)) {
//         LogPrintf("CWallet::GetVinAndKeysFromOutput -- Address does not refer to a key\n");
//         return false;
//     }

//     if (!GetKey(keyID, keyRet)) {
//         LogPrintf ("CWallet::GetVinAndKeysFromOutput -- Private key for address is not known\n");
//         return false;
//     }

//     pubKeyRet = keyRet.GetPubKey();
//     return true;
// }

static void ApproximateBestSubset(vector<pair<CAmount, pair<const CWalletTx*,unsigned int> > >vValue, const CAmount& nTotalLower, const CAmount& nTargetValue,
                                  vector<char>& vfBest, CAmount& nBest, int iterations = 1000, bool fUseInstantSend = false)
{
    vector<char> vfIncluded;

    vfBest.assign(vValue.size(), true);
    nBest = nTotalLower;

    FastRandomContext insecure_rand;

    for (int nRep = 0; nRep < iterations && nBest != nTargetValue; nRep++)
    {
        vfIncluded.assign(vValue.size(), false);
        CAmount nTotal = 0;
        bool fReachedTarget = false;
        for (int nPass = 0; nPass < 2 && !fReachedTarget; nPass++)
        {
            for (unsigned int i = 0; i < vValue.size(); i++)
            {
                if (fUseInstantSend && nTotal + vValue[i].first > sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)*COIN) {
                    continue;
                }
                //The solver here uses a randomized algorithm,
                //the randomness serves no real security purpose but is just
                //needed to prevent degenerate behavior and it is important
                //that the rng is fast. We do not use a constant random sequence,
                //because there may be some privacy improvement by making
                //the selection random.
                if (nPass == 0 ? insecure_rand.rand32()&1 : !vfIncluded[i])
                {
                    nTotal += vValue[i].first;
                    vfIncluded[i] = true;
                    if (nTotal >= nTargetValue)
                    {
                        fReachedTarget = true;
                        if (nTotal < nBest)
                        {
                            nBest = nTotal;
                            vfBest = vfIncluded;
                        }
                        nTotal -= vValue[i].first;
                        vfIncluded[i] = false;
                    }
                }
            }
        }
    }

    //Reduces the approximate best subset by removing any inputs that are smaller than the surplus of nTotal beyond nTargetValue.
    for (unsigned int i = 0; i < vValue.size(); i++)
    {
        if (vfBest[i] && (nBest - vValue[i].first) >= nTargetValue )
        {
            vfBest[i] = false;
            nBest -= vValue[i].first;
        }
    }
}

bool less_then_denom (const COutput& out1, const COutput& out2)
{
    // const CWalletTx *pcoin1 = out1.tx;
    // const CWalletTx *pcoin2 = out2.tx;

    // bool found1 = false;
    // bool found2 = false;
    // BOOST_FOREACH(CAmount d, CPrivateSend::GetStandardDenominations()) // loop through predefined denoms
    // {
    //     if(pcoin1->vout[out1.i].nValue == d) found1 = true;
    //     if(pcoin2->vout[out2.i].nValue == d) found2 = true;
    // }
    // return (!found1 && found2);
    return false;
}

bool CWallet::SelectCoinsMinConf(const CAmount& nTargetValue, int nConfMine, int nConfTheirs, const uint64_t nMaxAncestors, vector<COutput> vCoins,
                                 set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, CAmount& nValueRet, bool fUseInstantSend) const
{
        setCoinsRet.clear();
    nValueRet = 0;

    // List of values less than target
    pair<CAmount, pair<const CWalletTx*,unsigned int> > coinLowestLarger;
    coinLowestLarger.first = fUseInstantSend
                                        ? sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)*COIN
                                        : std::numeric_limits<CAmount>::max();
    coinLowestLarger.second.first = NULL;
    vector<pair<CAmount, pair<const CWalletTx*,unsigned int> > > vValue;
    CAmount nTotalLower = 0;

    random_shuffle(vCoins.begin(), vCoins.end(), GetRandInt);

    // move denoms down on the list
    sort(vCoins.begin(), vCoins.end(), less_then_denom);

    // try to find nondenom first to prevent unneeded spending of mixed coins
    for (unsigned int tryDenom = 0; tryDenom < 2; tryDenom++)
    {
        LogPrint("selectcoins", "tryDenom: %d\n", tryDenom);
        vValue.clear();
        nTotalLower = 0;
        BOOST_FOREACH(const COutput &output, vCoins)
        {
            if (!output.fSpendable)
                continue;

            const CWalletTx *pcoin = output.tx;

//            if (fDebug) LogPrint("selectcoins", "value %s confirms %d\n", FormatMoney(pcoin->vout[output.i].nValue), output.nDepth);
            if (output.nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? nConfMine : nConfTheirs))
                continue;

            int i = output.i;
            CAmount n = pcoin->vout[i].nValue;
            //if (tryDenom == 0 && CPrivateSend::IsDenominatedAmount(n)) continue; // we don't want denom values on first run

            pair<CAmount,pair<const CWalletTx*,unsigned int> > coin = make_pair(n,make_pair(pcoin, i));

            if (n == nTargetValue)
            {
                setCoinsRet.insert(coin.second);
                nValueRet += coin.first;
                return true;
            }
            else if (n < nTargetValue + MIN_CHANGE)
            {
                vValue.push_back(coin);
                nTotalLower += n;
            }
            else if (n < coinLowestLarger.first)
            {
                coinLowestLarger = coin;
            }
        }

        if (nTotalLower == nTargetValue)
        {
            for (unsigned int i = 0; i < vValue.size(); ++i)
            {
                setCoinsRet.insert(vValue[i].second);
                nValueRet += vValue[i].first;
            }
            return true;
        }

        if (nTotalLower < nTargetValue)
        {
            if (coinLowestLarger.second.first == NULL) // there is no input larger than nTargetValue
            {
                if (tryDenom == 0)
                    // we didn't look at denom yet, let's do it
                    continue;
                else
                    // we looked at everything possible and didn't find anything, no luck
                    return false;
            }
            setCoinsRet.insert(coinLowestLarger.second);
            nValueRet += coinLowestLarger.first;
            return true;
        }

        // nTotalLower > nTargetValue
        break;

    }

    // Solve subset sum by stochastic approximation
    sort(vValue.rbegin(), vValue.rend(), CompareValueOnly());
    vector<char> vfBest;
    CAmount nBest;

    ApproximateBestSubset(vValue, nTotalLower, nTargetValue, vfBest, nBest, fUseInstantSend);
    if (nBest != nTargetValue && nTotalLower >= nTargetValue + MIN_CHANGE)
        ApproximateBestSubset(vValue, nTotalLower, nTargetValue + MIN_CHANGE, vfBest, nBest, fUseInstantSend);

    // If we have a bigger coin and (either the stochastic approximation didn't find a good solution,
    //                                   or the next bigger coin is closer), return the bigger coin
    if (coinLowestLarger.second.first &&
        ((nBest != nTargetValue && nBest < nTargetValue + MIN_CHANGE) || coinLowestLarger.first <= nBest))
    {
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
    }
    else {
        string s = "CWallet::SelectCoinsMinConf best subset: ";
        for (unsigned int i = 0; i < vValue.size(); i++)
        {
            if (vfBest[i])
            {
                setCoinsRet.insert(vValue[i].second);
                nValueRet += vValue[i].first;
                s += FormatMoney(vValue[i].first) + " ";
            }
        }
        LogPrint("selectcoins", "%s - total %s\n", s, FormatMoney(nBest));
    }

    return true;
}

bool CWallet::SelectCoins(const CAmount& nTargetValue, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, CAmount& nValueRet, const CCoinControl* coinControl, AvailableCoinsType nCoinType, bool fUseInstantSend) const
{
    vector<COutput> vCoins;
    AvailableCoins(vCoins, true, coinControl, false, nCoinType, fUseInstantSend);

    // coin control -> return all selected outputs (we want all selected to go into the transaction for sure)
    if (coinControl && coinControl->HasSelected() && !coinControl->fAllowOtherInputs) {
        BOOST_FOREACH(
        const COutput &out, vCoins)
        {
            if (!out.fSpendable)
                continue;
            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.insert(make_pair(out.tx, out.i));
        }
        return (nValueRet >= nTargetValue);
    }

    // calculate value from preset inputs and store them
    set <pair<const CWalletTx *, uint32_t>> setPresetCoins;
    CAmount nValueFromPresetInputs = 0;

    std::vector <COutPoint> vPresetInputs;
    if (coinControl)
        coinControl->ListSelected(vPresetInputs);
    BOOST_FOREACH(
    const COutPoint &outpoint, vPresetInputs)
    {
        map<uint256, CWalletTx>::const_iterator it = mapWallet.find(outpoint.hash);
        if (it != mapWallet.end()) {
            const CWalletTx *pcoin = &it->second;
            // Clearly invalid input, fail
            if (pcoin->vout.size() <= outpoint.n)
                return false;
            nValueFromPresetInputs += pcoin->vout[outpoint.n].nValue;
            setPresetCoins.insert(make_pair(pcoin, outpoint.n));
        } else
            return false; // TODO: Allow non-wallet inputs
    }

    // remove preset inputs from vCoins
    for (vector<COutput>::iterator it = vCoins.begin();
         it != vCoins.end() && coinControl && coinControl->HasSelected();) {
        if (setPresetCoins.count(make_pair(it->tx, it->i)))
            it = vCoins.erase(it);
        else
            ++it;
    }

    size_t nMaxChainLength = std::min(GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT),
                                      GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT));
    bool fRejectLongChains = GetBoolArg("-walletrejectlongchains", DEFAULT_WALLET_REJECT_LONG_CHAINS);

    bool res = nTargetValue <= nValueFromPresetInputs ||
               SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 1, 6, 0, vCoins, setCoinsRet, nValueRet, fUseInstantSend) ||
               SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 1, 1, 0, vCoins, setCoinsRet, nValueRet, fUseInstantSend) ||
               (bSpendZeroConfChange &&
                SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 0, 1, 2, vCoins, setCoinsRet, nValueRet, fUseInstantSend)) ||
               (bSpendZeroConfChange && SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 0, 1,
                                                           std::min((size_t) 4, nMaxChainLength / 3), vCoins,
                                                           setCoinsRet, nValueRet, fUseInstantSend)) ||
               (bSpendZeroConfChange &&
                SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 0, 1, nMaxChainLength / 2, vCoins,
                                   setCoinsRet, nValueRet, fUseInstantSend)) ||
               (bSpendZeroConfChange &&
                SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 0, 1, nMaxChainLength, vCoins, setCoinsRet,
                                   nValueRet, fUseInstantSend)) ||
               (bSpendZeroConfChange && !fRejectLongChains &&
                SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 0, 1, std::numeric_limits<uint64_t>::max(),
                                   vCoins, setCoinsRet, nValueRet, fUseInstantSend));

    // because SelectCoinsMinConf clears the setCoinsRet, we now add the possible inputs to the coinset
    setCoinsRet.insert(setPresetCoins.begin(), setPresetCoins.end());

    // add preset inputs to the total value selected
    nValueRet += nValueFromPresetInputs;

    return res;
}

bool CWallet::FundTransaction(CMutableTransaction &tx, CAmount &nFeeRet, bool overrideEstimatedFeeRate,
                              const CFeeRate &specificFeeRate, int &nChangePosInOut, std::string &strFailReason,
                              bool includeWatching, bool lockUnspents, const CTxDestination &destChange) {
    vector <CRecipient> vecSend;

    // Turn the txout set into a CRecipient vector
    BOOST_FOREACH(
    const CTxOut &txOut, tx.vout)
    {
        CRecipient recipient = {txOut.scriptPubKey, txOut.nValue, false};
        vecSend.push_back(recipient);
    }

    CCoinControl coinControl;
    coinControl.destChange = destChange;
    coinControl.fAllowOtherInputs = true;
    coinControl.fAllowWatchOnly = includeWatching;
    coinControl.fOverrideFeeRate = overrideEstimatedFeeRate;
    coinControl.nFeeRate = specificFeeRate;

    BOOST_FOREACH(
    const CTxIn &txin, tx.vin)
    coinControl.Select(txin.prevout);

    CReserveKey reservekey(this);
    CWalletTx wtx;
    if (!CreateTransaction(vecSend, wtx, reservekey, nFeeRet, nChangePosInOut, strFailReason, &coinControl, false))
        return false;

    if (nChangePosInOut != -1)
        tx.vout.insert(tx.vout.begin() + nChangePosInOut, wtx.vout[nChangePosInOut]);

    // Add new txins (keeping original txin scriptSig/order)
    BOOST_FOREACH(
    const CTxIn &txin, wtx.vin)
    {
        if (!coinControl.IsSelected(txin.prevout)) {
            tx.vin.push_back(txin);

            if (lockUnspents) {
                LOCK2(cs_main, cs_wallet);
                LockCoin(txin.prevout);
            }
        }
    }

    return true;
}

struct CompareByAmount
{
    bool operator()(const CompactTallyItem& t1,
                    const CompactTallyItem& t2) const
    {
        return t1.nAmount > t2.nAmount;
    }
};

bool CWallet::GetOutpointAndKeysFromOutput(const COutput& out, COutPoint& outpointRet, CPubKey& pubKeyRet, CKey& keyRet)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    CScript pubScript;

    outpointRet = COutPoint(out.tx->GetHash(), out.i);
    pubScript = out.tx->vout[out.i].scriptPubKey; // the inputs PubKey

    CTxDestination address1;
    ExtractDestination(pubScript, address1);
    CBitcoinAddress address2(address1);

    CKeyID keyID;
    if (!address2.GetKeyID(keyID)) {
        LogPrintf("CWallet::GetOutpointAndKeysFromOutput -- Address does not refer to a key\n");
        return false;
    }

    if (!GetKey(keyID, keyRet)) {
        LogPrintf ("CWallet::GetOutpointAndKeysFromOutput -- Private key for address is not known\n");
        return false;
    }

    pubKeyRet = keyRet.GetPubKey();
    return true;
}

int CWallet::CountInputsWithAmount(CAmount nInputAmount)
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted()){
                int nDepth = pcoin->GetDepthInMainChain(false);

                for (unsigned int i = 0; i < pcoin->vout.size(); i++) {

                    uint32_t nLockTime = pcoin->vout[i].GetLockTime();

                    if( nLockTime ){
                        LogPrintf("LOGTIME FOUND %d\n", nLockTime);
                    }

                    COutput out = COutput(pcoin, i, nDepth, true, true, nLockTime);
                    //COutPoint outpoint = COutPoint(out.tx->GetHash(), out.i);

                    if(out.tx->vout[out.i].nValue != nInputAmount) continue;
                    //if(!CPrivateSend::IsDenominatedAmount(pcoin->vout[i].nValue)) continue;
                    if(IsSpent(out.tx->GetHash(), i) || IsMine(pcoin->vout[i]) != ISMINE_SPENDABLE) continue;

                    nTotal++;
                }
            }
        }
    }

    return nTotal;
}

bool CWallet::HasCollateralInputs(bool fOnlyConfirmed) const
{
    vector<COutput> vCoins;
    AvailableCoins(vCoins, fOnlyConfirmed, NULL, false, ONLY_PRIVATESEND_COLLATERAL);

    return !vCoins.empty();
}

bool CWallet::GetProposalFeeTX(CWalletTx& tx, const CSmartAddress& fromAddress, uint256 proposalHash, CAmount nAmount)
{
    // make our change address
    CReserveKey reservekey(this);

    std::vector<COutput> vecCoins;

    AvailableCoins(vecCoins, fromAddress);

    CScript scriptData;
    scriptData << OP_RETURN << ToByteVector(proposalHash);

    CAmount nAmountIn = 0, nFeeRet = 0;
    int nChangePosRet = -1;
    std::string strFail = "";
    CRecipient dataRecipient = {scriptData, 0, false};
    vector< CRecipient > vecSend;
    vecSend.push_back(dataRecipient);

    CCoinControl coinControl;

    coinControl.destChange = fromAddress.Get();

    int64_t nInputs = 0;
    CAmount nFee = CalculateInputFee(0);

    for( auto coin : vecCoins ){

        COutPoint out(coin.tx->GetHash(), coin.i);

        coinControl.Select(out);

        nAmountIn += coin.tx->vout[coin.i].nValue;

        nFee = CalculateInputFee(++nInputs);

        if( nAmountIn > (nAmount + nFee) )
            break;

    }

    bool success = CreateTransaction(vecSend, tx, reservekey, nFeeRet, nChangePosRet, strFail, &coinControl, true, ALL_COINS, false);
    if(!success){
        LogPrintf("CWallet::GetBudgetSystemCollateralTX -- Error: %s\n", strFail);
        return false;
    }

    return true;
}

bool CWallet::ConvertList(std::vector<CTxIn> vecTxIn, std::vector<CAmount>& vecAmounts)
{
    BOOST_FOREACH(CTxIn txin, vecTxIn) {
        if (mapWallet.count(txin.prevout.hash)) {
            CWalletTx& wtx = mapWallet[txin.prevout.hash];
            if(txin.prevout.n < wtx.vout.size()){
                vecAmounts.push_back(wtx.vout[txin.prevout.n].nValue);
            }
        } else {
            LogPrintf("CWallet::ConvertList -- Couldn't find transaction\n");
        }
    }
    return true;
}


bool CWallet::CreateTransaction(const vector<CRecipient>& vecSend, CWalletTx& wtxNew, CReserveKey& reservekey, CAmount& nFeeRet, int &nChangePosInOut,
                                std::string& strFailReason, const CCoinControl* coinControl, bool sign, AvailableCoinsType nCoinType, bool fUseInstantSend)
{
    LogPrintf("CreateTransaction()\n");
    CAmount nValue = 0;
    int nChangePosRequest = nChangePosInOut;
    unsigned int nSubtractFeeFromAmount = 0;
    BOOST_FOREACH(const CRecipient &recipient, vecSend)
    {
        if (nValue < 0 || recipient.nAmount < 0) {
            strFailReason = _("Transaction amounts must be positive");
            return false;
        }
        nValue += recipient.nAmount;

        if (recipient.fSubtractFeeFromAmount)
            nSubtractFeeFromAmount++;
    }
    if (vecSend.empty() || nValue < 0) {
        strFailReason = _("Transaction amounts must be positive");
        return false;
    }

    wtxNew.fTimeReceivedIsTxTime = true;
    wtxNew.BindWallet(this);
    CMutableTransaction txNew;

    {
        LOCK2(cs_main, cs_wallet);
        {
            std::vector <COutput> vAvailableCoins;
            AvailableCoins(vAvailableCoins, true, coinControl);

            nFeeRet = payTxFee.GetFeePerK();
            // Start with no fee and loop until there is enough fee
            while (true) {
                nChangePosInOut = nChangePosRequest;
                txNew.vin.clear();
                txNew.vout.clear();
                txNew.wit.SetNull();
                wtxNew.fFromMe = true;
                bool fFirst = true;

                CAmount nValueToSelect = nValue;
                if (nSubtractFeeFromAmount == 0)
                    nValueToSelect += nFeeRet;
                double dPriority = 0;
                // vouts to the payees
                BOOST_FOREACH(const CRecipient &recipient, vecSend)
                {
                    CTxOut txout(recipient.nAmount, recipient.scriptPubKey);

                   if (recipient.fSubtractFeeFromAmount) {
                       txout.nValue -=
                               nFeeRet / nSubtractFeeFromAmount; // Subtract fee equally from each selected recipient

                       if (fFirst) // first receiver pays the remainder not divisible by output count
                       {
                           fFirst = false;
                           txout.nValue -= nFeeRet % nSubtractFeeFromAmount;
                       }
                   }

                    if (txout.IsDust(::minRelayTxFee)) {
                        if (recipient.fSubtractFeeFromAmount && nFeeRet > 0) {
                            if (txout.nValue < 0)
                                strFailReason = _("The transaction amount is too small to pay the fee");
                            else
                                strFailReason = _(
                                        "The transaction amount is too small to send after the fee has been deducted");
                        } else
                            strFailReason = _("Transaction amount too small");
                        return false;
                    }
                    txNew.vout.push_back(txout);
                }

                // Choose coins to use
                set <pair<const CWalletTx *, unsigned int>> setCoins;
                CAmount nValueIn = 0;
                if (!SelectCoins(nValueToSelect, setCoins, nValueIn, coinControl, nCoinType, fUseInstantSend))
                {
                    if (nValueIn < nValueToSelect) {
                        strFailReason = _("Insufficient funds.");
                        if (fUseInstantSend) {
                            // could be not true but most likely that's the reason
                            strFailReason += " " + strprintf(_("InstantSend requires inputs with at least %d confirmations, you might need to wait a few minutes and try again."), INSTANTSEND_CONFIRMATIONS_REQUIRED);
                        }
                    }
                    return false;
                }

                if (fUseInstantSend && nValueIn > sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)*COIN) {
                    strFailReason += " " + strprintf(_("InstantSend doesn't support sending values that high yet. Transactions are currently limited to %1 SMART."), sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE));
                    return false;
                }

                enum LockTimeFormat lockTimeFmt = UNSET;
                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
                {
                    CAmount nCredit = pcoin.first->vout[pcoin.second].nValue;
                    //The coin age after the next block (depth+1) is used instead of the current,
                    //reflecting an assumption the user would accept a bit more delay for
                    //a chance at a free transaction.
                    //But mempool inputs might still be in the mempool, so their age stays 0
                    int age = pcoin.first->GetDepthInMainChain();
                    assert(age >= 0);
                    if (age != 0)
                        age += 1;
                    dPriority += (double) nCredit * age;

                    // Figure out if the input is a CLTV script and the lockTime format it uses
                    const CTxOut &txout = pcoin.first->vout[pcoin.second];
                    int lockTime = txout.GetLockTime();
                    if (!lockTime && txout.scriptPubKey.IsPayToScriptHash()) {
                       // Need to parse redeem script
                       CTxDestination outputAddress;
                       if (ExtractDestination(txout.scriptPubKey, outputAddress)) {
                          const CScriptID& hash = boost::get<CScriptID>(outputAddress);
                           CScript redeemScript;
                           if (pwalletMain->GetCScript(hash, redeemScript)) {
                                int nLockTimeLength = redeemScript[0];
                                std::vector<unsigned char> lockTimeVch(redeemScript.begin() + 1,
                                        redeemScript.begin() + 1 + nLockTimeLength);
                                lockTime = CScriptNum(lockTimeVch, false).getint();
                           }
                       }
                    }

                    if (lockTime > LOCKTIME_THRESHOLD) {
                      if (lockTimeFmt == BLOCKTIME) {
                        strFailReason = _("Cannot mix Timestamp and block based time-locked inputs in the same transaction. Consider using coin control to select inputs manually.");
                        return false;
                      } else {
                        lockTimeFmt = TIMESTAMP;
                      }
                    } else if (lockTime > 0) {
                      if (lockTimeFmt == TIMESTAMP) {
                        strFailReason = _("Cannot mix Timestamp and block based time-locked inputs in the same transaction. Consider using coin control to select inputs manually.");
                        return false;
                      } else {
                        lockTimeFmt = BLOCKTIME;
                      }
                    }
                }

                if (lockTimeFmt == TIMESTAMP) {
                    // Use one second less than median time of past block as required by BIP113
                    txNew.nLockTime = chainActive.Tip()->GetMedianTimePast() - 1;

                    // Randomize until 3h back
                    if (GetRandInt(10) == 0)
                        txNew.nLockTime = std::max(0, (int) txNew.nLockTime - GetRandInt(3 * 3600));

                    assert(txNew.nLockTime <= (unsigned int)GetTime());
                    assert(txNew.nLockTime > LOCKTIME_THRESHOLD);
                } else {
                    // Discourage fee sniping.
                    //
                    // For a large miner the value of the transactions in the best block and
                    // the mempool can exceed the cost of deliberately attempting to mine two
                    // blocks to orphan the current best block. By setting nLockTime such that
                    // only the next block can include the transaction, we discourage this
                    // practice as the height restricted and limited blocksize gives miners
                    // considering fee sniping fewer options for pulling off this attack.
                    //
                    // A simple way to think about this is from the wallet's point of view we
                    // always want the blockchain to move forward. By setting nLockTime this
                    // way we're basically making the statement that we only want this
                    // transaction to appear in the next block; we don't want to potentially
                    // encourage reorgs by allowing transactions to appear at lower heights
                    // than the next block in forks of the best chain.
                    //
                    // Of course, the subsidy is high enough, and transaction volume low
                    // enough, that fee sniping isn't a problem yet, but by implementing a fix
                    // now we ensure code won't be written that makes assumptions about
                    // nLockTime that preclude a fix later.
                    txNew.nLockTime = chainActive.Height();

                    // Secondly occasionally randomly pick a nLockTime even further back, so
                    // that transactions that are delayed after signing for whatever reason,
                    // e.g. high-latency mix networks and some CoinJoin implementations, have
                    // better privacy.
                    if (GetRandInt(10) == 0)
                        txNew.nLockTime = std::max(0, (int) txNew.nLockTime - GetRandInt(100));

                    assert(txNew.nLockTime <= (unsigned int) chainActive.Height());
                    assert(txNew.nLockTime < LOCKTIME_THRESHOLD);
                }

                const CAmount nChange = nValueIn - nValueToSelect;
                if (nChange > 0) {
                    // Fill a vout to ourself
                    // TODO: pass in scriptChange instead of reservekey so
                    // change transaction isn't always pay-to-bitcoin-address
                    CScript scriptChange;

                    // coin control: send change to custom address
                    if (coinControl && !boost::get<CNoDestination>(&coinControl->destChange))
                        scriptChange = GetScriptForDestination(coinControl->destChange);

                        // no coin control: send change to newly generated address
                    else {
                        // Note: We use a new key here to keep it from being obvious which side is the change.
                        //  The drawback is that by not reusing a previous key, the change may be lost if a
                        //  backup is restored, if the backup doesn't have the new private key for the change.
                        //  If we reused the old key, it would be possible to add code to look for and
                        //  rediscover unknown transactions that were written with keys of ours to recover
                        //  post-backup change.

                        // Reserve a new key pair from key pool
                        CPubKey vchPubKey;
                        bool ret;
                        ret = reservekey.GetReservedKey(vchPubKey,true);
                        if (!ret) {
                            strFailReason = _("Keypool ran out, please call keypoolrefill first");
                            return false;
                        }

                        scriptChange = GetScriptForDestination(vchPubKey.GetID());
                    }

                    CTxOut newTxOut(nChange, scriptChange);

                    // We do not move dust-change to fees, because the sender would end up paying more than requested.
                    // This would be against the purpose of the all-inclusive feature.
                    // So instead we raise the change and deduct from the recipient.
                    if (nSubtractFeeFromAmount > 0 && newTxOut.IsDust(::minRelayTxFee)) {
                        CAmount nDust = newTxOut.GetDustThreshold(::minRelayTxFee) - newTxOut.nValue;
                        newTxOut.nValue += nDust; // raise change until no more dust
                        for (unsigned int i = 0; i < vecSend.size(); i++) // subtract from first recipient
                        {
                            if (vecSend[i].fSubtractFeeFromAmount) {
                                txNew.vout[i].nValue -= nDust;
                                if (txNew.vout[i].IsDust(::minRelayTxFee)) {
                                    strFailReason = _(
                                            "The transaction amount is too small to send after the fee has been deducted");
                                    return false;
                                }
                                break;
                            }
                        }
                    }

                    // Never create dust outputs; if we would, just
                    // add the dust to the fee.
                    if (newTxOut.IsDust(::minRelayTxFee)) {
                        nChangePosInOut = -1;
                        nFeeRet += nChange;
                        reservekey.ReturnKey();
                    } else {
                        if (nChangePosInOut == -1) {
                            // Insert change txn at random position:
                            nChangePosInOut = GetRandInt(txNew.vout.size() + 1);
                        } else if ((unsigned int) nChangePosInOut > txNew.vout.size()) {
                            strFailReason = _("Change index out of range");
                            return false;
                        }

                        vector<CTxOut>::iterator position = txNew.vout.begin() + nChangePosInOut;
                        txNew.vout.insert(position, newTxOut);
                    }
                } else
                    reservekey.ReturnKey();

                // Fill vin
                //
                // Note how the sequence number is set to max()-1 so that the
                // nLockTime set above actually works.
                BOOST_FOREACH(const PAIRTYPE(const CWalletTx *, unsigned int) &coin, setCoins)
                    txNew.vin.push_back(CTxIn(coin.first->GetHash(), coin.second, CScript(), std::numeric_limits < unsigned int > ::max() - 1));

                // Sign
                int nIn = 0;
                CTransaction txNewConst(txNew);
                BOOST_FOREACH(const PAIRTYPE(const CWalletTx *, unsigned int) &coin, setCoins)
                {
                    bool signSuccess;
                    const CScript &scriptPubKey = coin.first->vout[coin.second].scriptPubKey;
                    CScript& scriptSigRes = txNew.vin[nIn].scriptSig;
                    if (sign)
                        signSuccess = ProduceSignature(TransactionSignatureCreator(this, &txNewConst, nIn, SIGHASH_ALL), scriptPubKey, scriptSigRes);
                    else
                        signSuccess = ProduceSignature(DummySignatureCreator(this), scriptPubKey, scriptSigRes);

                    if (!signSuccess)
                    {
                        strFailReason = _("Signing transaction failed");
                        return false;
                    }
                    nIn++;
                }

                unsigned int nBytes = ::GetSerializeSize(txNew, SER_NETWORK, PROTOCOL_VERSION);

                // Remove scriptSigs if we used dummy signatures for fee calculation
                if (!sign) {
                    BOOST_FOREACH(CTxIn & vin, txNew.vin)
                    vin.scriptSig = CScript();
                    txNew.wit.SetNull();
                }

                // Embed the constructed transaction data in wtxNew.
                *static_cast<CTransaction *>(&wtxNew) = CTransaction(txNew);

                // Limit size
                if (GetTransactionWeight(txNew) >= MAX_STANDARD_TX_WEIGHT) {
                    strFailReason = _("Transaction too large");
                    return false;
                }

                dPriority = wtxNew.ComputePriority(dPriority, nBytes);

                // Can we complete this as a free transaction?
                // Note: InstantSend transaction can't be a free one
                if (!fUseInstantSend && fSendFreeTransactions && nBytes <= MAX_FREE_TRANSACTION_CREATE_SIZE)
                {
                    // Not enough fee: enough priority?
                    double dPriorityNeeded = mempool.estimateSmartPriority(nTxConfirmTarget);
                    // Require at least hard-coded AllowFree.
                    if (dPriority >= dPriorityNeeded && AllowFree(dPriority))
                        break;

                    // Small enough, and priority high enough, to send for free
//                    if (dPriorityNeeded > 0 && dPriority >= dPriorityNeeded)
//                        break;
                }
                int64_t nPayFee = payTxFee.GetFeePerK() * (1 + (int64_t) GetTransactionWeight(txNew) / 1000);
                bool fAllowFree = AllowFree(dPriority);// No free TXs in SMART
                LogPrintf("CreateTransaction: fAllowFree=%s\n", fAllowFree);
                int64_t nMinFee = wtxNew.GetMinFee(1, fAllowFree, GMF_SEND);

                int64_t nFeeNeeded = nPayFee;
                if (nFeeNeeded < nMinFee) {
                    nFeeNeeded = nMinFee;
                }

                if (nFeeRet >= nFeeNeeded)
                    break; // Done, enough fee included.

                // Include more fee and try again.
                nFeeRet = nFeeNeeded;
                continue;
            }
        }
    }

    if(fUseInstantSend){
      nFeeRet += CTxLockRequest().GetMinFee();
    }

    if (GetBoolArg("-walletrejectlongchains", DEFAULT_WALLET_REJECT_LONG_CHAINS)) {
        // Lastly, ensure this tx will pass the mempool's chain limits
        LockPoints lp;
        CTxMemPoolEntry entry(txNew, 0, 0, 0, 0, false, 0, false, 0, lp);
        CTxMemPool::setEntries setAncestors;
        size_t nLimitAncestors = GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT);
        size_t nLimitAncestorSize = GetArg("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT) * 1000;
        size_t nLimitDescendants = GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT);
        size_t nLimitDescendantSize = GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT) * 1000;
        std::string errString;
        if (!mempool.CalculateMemPoolAncestors(entry, setAncestors, nLimitAncestors, nLimitAncestorSize,
                                               nLimitDescendants, nLimitDescendantSize, errString)) {
            strFailReason = _("Transaction has too long of a mempool chain");
            return false;
        }
    }
    return true;
}

/**
 * Call after CreateTransaction unless you want to abort
 */
bool CWallet::CommitTransaction(CWalletTx& wtxNew, CReserveKey& reservekey, CConnman* connman, std::string strCommand)
{
    {
        LOCK2(cs_main, cs_wallet);
        LogPrintf("CommitTransaction:\n%s", wtxNew.ToString());
        {
            // This is only to keep the database open to defeat the auto-flush for the
            // duration of this scope.  This is the only place where this optimization
            // maybe makes sense; please don't do it anywhere else.
            CWalletDB* pwalletdb = fFileBacked ? new CWalletDB(strWalletFile,"r+") : NULL;

            // Take key pair from key pool so it won't be used again
            reservekey.KeepKey();

            // Add tx to wallet, because if it has change it's also ours,
            // otherwise just for transaction history.
            AddToWallet(wtxNew, false, pwalletdb);

            // Notify that old coins are spent
            set<uint256> updated_hahes;
            BOOST_FOREACH(const CTxIn& txin, wtxNew.vin)
            {
                // notify only once
                if(updated_hahes.find(txin.prevout.hash) != updated_hahes.end()) continue;

                CWalletTx &coin = mapWallet[txin.prevout.hash];
                coin.BindWallet(this);
                NotifyTransactionChanged(this, txin.prevout.hash, CT_UPDATED);
                updated_hahes.insert(txin.prevout.hash);
            }
            if (fFileBacked)
                delete pwalletdb;
        }

        // Track how many getdata requests our transaction gets
        mapRequestCount[wtxNew.GetHash()] = 0;

        if (fBroadcastTransactions)
        {
            // Broadcast
            if (!wtxNew.AcceptToMemoryPool(false))
            {
                // This must not fail. The transaction has already been signed and recorded.
                LogPrintf("CommitTransaction(): Error: Transaction not valid\n");
                return false;
            }
            wtxNew.RelayWalletTransaction(connman, strCommand);
        }
    }
    return true;
}

bool CWallet::EraseFromWallet(uint256 hash)
{
    if (!fFileBacked)
        return false;
    {
        LOCK(cs_wallet);
        if (mapWallet.erase(hash))
            CWalletDB(strWalletFile).EraseTx(hash);
    }
    return true;
}

bool CWallet::AddAccountingEntry(const CAccountingEntry &acentry, CWalletDB &pwalletdb) {
    if (!pwalletdb.WriteAccountingEntry_Backend(acentry))
        return false;

    laccentries.push_back(acentry);
    CAccountingEntry &entry = laccentries.back();
    wtxOrdered.insert(make_pair(entry.nOrderPos, TxPair((CWalletTx *) 0, &entry)));

    return true;
}

CAmount CWallet::GetRequiredFee(unsigned int nTxBytes) {
    return std::max(minTxFee.GetFee(nTxBytes), ::minRelayTxFee.GetFee(nTxBytes));
}

CAmount CWallet::GetMinimumFee(unsigned int nTxBytes, unsigned int nConfirmTarget, const CTxMemPool &pool) {
    // payTxFee is user-set "I want to pay this much"
    CAmount nFeeNeeded = payTxFee.GetFee(nTxBytes);
    // User didn't set: use -txconfirmtarget to estimate...
    if (nFeeNeeded == 0) {
        int estimateFoundTarget = nConfirmTarget;
        nFeeNeeded = pool.estimateSmartFee(nConfirmTarget, &estimateFoundTarget).GetFee(nTxBytes);
        // ... unless we don't have enough mempool data for estimatefee, then use fallbackFee
        if (nFeeNeeded == 0)
            nFeeNeeded = fallbackFee.GetFee(nTxBytes);
    }
    // prevent user from paying a fee below minRelayTxFee or minTxFee
    nFeeNeeded = std::max(nFeeNeeded, GetRequiredFee(nTxBytes));
    // But always obey the maximum
    if (nFeeNeeded > maxTxFee)
        nFeeNeeded = maxTxFee;
    return nFeeNeeded;
}


DBErrors CWallet::LoadWallet(bool& fFirstRunRet)
{
    if (!fFileBacked)
        return DB_LOAD_OK;
    fFirstRunRet = false;
    DBErrors nLoadWalletRet = CWalletDB(strWalletFile,"cr+").LoadWallet(this);
    if (nLoadWalletRet == DB_NEED_REWRITE)
    {
        if (CDB::Rewrite(strWalletFile, "\x04pool"))
        {
            LOCK(cs_wallet);
            setInternalKeyPool.clear();
            setExternalKeyPool.clear();
            nKeysLeftSinceAutoBackup = 0;
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // that requires a new key.
        }
    }

    {
        LOCK2(cs_main, cs_wallet);
        for (auto& pair : mapWallet) {
            for(unsigned int i = 0; i < pair.second.vout.size(); ++i) {
                if (IsMine(pair.second.vout[i]) && !IsSpent(pair.first, i)) {
                    setWalletUTXO.insert(COutPoint(pair.first, i));
                }
            }
        }
    }

    if (nLoadWalletRet != DB_LOAD_OK)
        return nLoadWalletRet;
    fFirstRunRet = !vchDefaultKey.IsValid();

    uiInterface.LoadWallet(this);

    return DB_LOAD_OK;
}

DBErrors CWallet::ZapWalletTx(std::vector <CWalletTx> &vWtx) {
    if (!fFileBacked)
        return DB_LOAD_OK;
    DBErrors nZapWalletTxRet = CWalletDB(strWalletFile, "cr+").ZapWalletTx(this, vWtx);
    if (nZapWalletTxRet == DB_NEED_REWRITE) {
        if (CDB::Rewrite(strWalletFile, "\x04pool")) {
            LOCK(cs_wallet);
            setInternalKeyPool.clear();
            setExternalKeyPool.clear();
            nKeysLeftSinceAutoBackup = 0;
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // that requires a new key.
        }
    }

    if (nZapWalletTxRet != DB_LOAD_OK)
        return nZapWalletTxRet;

    return DB_LOAD_OK;
}


bool CWallet::SetAddressBook(const CTxDestination &address, const string &strName, const string &strPurpose) {
    bool fUpdated = false;
    {
        LOCK(cs_wallet); // mapAddressBook
        std::map<CTxDestination, CAddressBookData>::iterator mi = mapAddressBook.find(address);
        fUpdated = mi != mapAddressBook.end();
        mapAddressBook[address].name = strName;
        if (!strPurpose.empty()) /* update purpose only if requested */
            mapAddressBook[address].purpose = strPurpose;
    }
    NotifyAddressBookChanged(this, address, strName, ::IsMine(*this, address) != ISMINE_NO,
                             strPurpose, (fUpdated ? CT_UPDATED : CT_NEW));
    if (!fFileBacked)
        return false;
    if (!strPurpose.empty() && !CWalletDB(strWalletFile).WritePurpose(CBitcoinAddress(address).ToString(), strPurpose))
        return false;
    return CWalletDB(strWalletFile).WriteName(CBitcoinAddress(address).ToString(), strName);
}

bool CWallet::DelAddressBook(const CTxDestination &address) {
    {
        LOCK(cs_wallet); // mapAddressBook

        if (fFileBacked) {
            // Delete destdata tuples associated with address
            std::string strAddress = CBitcoinAddress(address).ToString();
            BOOST_FOREACH(
            const PAIRTYPE(string, string) &item, mapAddressBook[address].destdata)
            {
                CWalletDB(strWalletFile).EraseDestData(strAddress, item.first);
            }
        }
        mapAddressBook.erase(address);
    }

    NotifyAddressBookChanged(this, address, "", ::IsMine(*this, address) != ISMINE_NO, "", CT_DELETED);

    if (!fFileBacked)
        return false;
    CWalletDB(strWalletFile).ErasePurpose(CBitcoinAddress(address).ToString());
    return CWalletDB(strWalletFile).EraseName(CBitcoinAddress(address).ToString());
}

bool CWallet::SetDefaultKey(const CPubKey &vchPubKey) {
    if (fFileBacked) {
        if (!CWalletDB(strWalletFile).WriteDefaultKey(vchPubKey))
            return false;
    }
    vchDefaultKey = vchPubKey;
    return true;
}

/**
 * Mark old keypool keys as used,
 * and generate all new keys
 */
bool CWallet::NewKeyPool()
{
    {
        LOCK(cs_wallet);
        CWalletDB walletdb(strWalletFile);
        BOOST_FOREACH(int64_t nIndex, setInternalKeyPool) {
            walletdb.ErasePool(nIndex);
        }
        setInternalKeyPool.clear();
        BOOST_FOREACH(int64_t nIndex, setExternalKeyPool) {
            walletdb.ErasePool(nIndex);
        }
        setExternalKeyPool.clear();
        //privateSendClient.fEnablePrivateSend = false;
        nKeysLeftSinceAutoBackup = 0;

        if (!TopUpKeyPool())
            return false;

        LogPrintf("CWallet::NewKeyPool rewrote keypool\n");
    }
    return true;
}

size_t CWallet::KeypoolCountExternalKeys()
{
    AssertLockHeld(cs_wallet); // setExternalKeyPool
    return setExternalKeyPool.size();
}

size_t CWallet::KeypoolCountInternalKeys()
{
    AssertLockHeld(cs_wallet); // setInternalKeyPool
    return setInternalKeyPool.size();
}

bool CWallet::TopUpKeyPool(unsigned int kpSize) {
    {
        LOCK(cs_wallet);

        if (IsLocked(true))
            return false;

        // Top up key pool
        unsigned int nTargetSize;
        if (kpSize > 0)
            nTargetSize = kpSize;
        else
            nTargetSize = max(GetArg("-keypool", DEFAULT_KEYPOOL_SIZE), (int64_t) 0);

        // count amount of available keys (internal, external)
        // make sure the keypool of external and internal keys fits the user selected target (-keypool)
        int64_t amountExternal = setExternalKeyPool.size();
        int64_t amountInternal = setInternalKeyPool.size();
        int64_t missingExternal = std::max(std::max((int64_t) nTargetSize/2, (int64_t) 1) - amountExternal, (int64_t) 0);
        int64_t missingInternal = std::max(std::max((int64_t) nTargetSize/2, (int64_t) 1) - amountInternal, (int64_t) 0);

        if (!IsHDEnabled())
        {
            // don't create extra internal keys
            missingInternal = 0;
        }

        bool fInternal = false;
        CWalletDB walletdb(strWalletFile);
        for (int64_t i = missingInternal + missingExternal; i--;)
        {
            int64_t nEnd = 1;
            if (i < missingInternal) {
                fInternal = true;
            }
            if (!setInternalKeyPool.empty()) {
                nEnd = *(--setInternalKeyPool.end()) + 1;
            }
            if (!setExternalKeyPool.empty()) {
                nEnd = std::max(nEnd, *(--setExternalKeyPool.end()) + 1);
            }
            // TODO: implement keypools for all accounts?
            if (!walletdb.WritePool(nEnd, CKeyPool(GenerateNewKey(0, fInternal), fInternal)))
                throw runtime_error("TopUpKeyPool(): writing generated key failed");

            if (fInternal) {
                setInternalKeyPool.insert(nEnd);
            } else {
                setExternalKeyPool.insert(nEnd);
            }
            LogPrintf("keypool added key %d, size=%u, internal=%d\n", nEnd, setInternalKeyPool.size() + setExternalKeyPool.size(), fInternal);

            double dProgress = 100.f * nEnd / (nTargetSize + 1);
            std::string strMsg = strprintf(_("Loading wallet... (%3.2f %%)"), dProgress);
            uiInterface.InitMessage(strMsg);
        }
    }
    return true;
}

void CWallet::ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool, bool fInternal)
{
    nIndex = -1;
    keypool.vchPubKey = CPubKey();
    {
        LOCK(cs_wallet);

        if (!IsLocked(true))
            TopUpKeyPool();

        fInternal = fInternal && IsHDEnabled();
        std::set<int64_t>& setKeyPool = fInternal ? setInternalKeyPool : setExternalKeyPool;

        // Get the oldest key
        if(setKeyPool.empty())
            return;

        CWalletDB walletdb(strWalletFile);

        nIndex = *setKeyPool.begin();
        setKeyPool.erase(nIndex);
        if (!walletdb.ReadPool(nIndex, keypool)) {
            throw std::runtime_error(std::string(__func__) + ": read failed");
        }
        if (!HaveKey(keypool.vchPubKey.GetID())) {
            throw std::runtime_error(std::string(__func__) + ": unknown key in key pool");
        }
        if (keypool.fInternal != fInternal) {
            throw std::runtime_error(std::string(__func__) + ": keypool entry misclassified");
        }

        assert(keypool.vchPubKey.IsValid());
        LogPrintf("keypool reserve %d\n", nIndex);
    }
}

void CWallet::KeepKey(int64_t nIndex)
{
    // Remove from key pool
    if (fFileBacked)
    {
        CWalletDB walletdb(strWalletFile);
        walletdb.ErasePool(nIndex);
        nKeysLeftSinceAutoBackup = nWalletBackups ? nKeysLeftSinceAutoBackup - 1 : 0;
    }
    LogPrintf("keypool keep %d\n", nIndex);
}

void CWallet::ReturnKey(int64_t nIndex, bool fInternal)
{
    // Return to key pool
    {
        LOCK(cs_wallet);
        if (fInternal) {
            setInternalKeyPool.insert(nIndex);
        } else {
            setExternalKeyPool.insert(nIndex);
        }
    }
    LogPrintf("keypool return %d\n", nIndex);
}

bool CWallet::GetKeyFromPool(CPubKey& result, bool fInternal)
{
    int64_t nIndex = 0;
    CKeyPool keypool;
    {
        LOCK(cs_wallet);
        ReserveKeyFromKeyPool(nIndex, keypool, fInternal);
        if (nIndex == -1)
        {
            if (IsLocked(true)) return false;
            // TODO: implement keypool for all accouts?
            result = GenerateNewKey(0, fInternal);
            return true;
        }
        KeepKey(nIndex);
        result = keypool.vchPubKey;
    }
    return true;
}

static int64_t GetOldestKeyInPool(const std::set<int64_t>& setKeyPool, CWalletDB& walletdb) {
    CKeyPool keypool;
    int64_t nIndex = *(setKeyPool.begin());
    if (!walletdb.ReadPool(nIndex, keypool)) {
        throw std::runtime_error(std::string(__func__) + ": read oldest key in keypool failed");
    }
    assert(keypool.vchPubKey.IsValid());
    return keypool.nTime;
}

int64_t CWallet::GetOldestKeyPoolTime()
{
    LOCK(cs_wallet);

    // if the keypool is empty, return <NOW>
    if (setExternalKeyPool.empty() && setInternalKeyPool.empty())
        return GetTime();

    CWalletDB walletdb(strWalletFile);
    int64_t oldestKey = -1;

    // load oldest key from keypool, get time and return
    if (!setInternalKeyPool.empty()) {
        oldestKey = std::max(GetOldestKeyInPool(setInternalKeyPool, walletdb), oldestKey);
    }
    if (!setExternalKeyPool.empty()) {
        oldestKey = std::max(GetOldestKeyInPool(setExternalKeyPool, walletdb), oldestKey);
    }
    return oldestKey;
}

std::map <CTxDestination, CAmount> CWallet::GetAddressBalances() {
    map <CTxDestination, CAmount> balances;

    {
        LOCK(cs_wallet);
        BOOST_FOREACH(PAIRTYPE(uint256, CWalletTx)
        walletEntry, mapWallet)
        {
            CWalletTx *pcoin = &walletEntry.second;

            if (!CheckFinalTx(*pcoin) || !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? 0 : 1))
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
                CTxDestination addr;
                if (!IsMine(pcoin->vout[i]))
                    continue;
                if (!ExtractDestination(pcoin->vout[i].scriptPubKey, addr))
                    continue;

                CAmount n = IsSpent(walletEntry.first, i) ? 0 : pcoin->vout[i].nValue;

                if (!balances.count(addr))
                    balances[addr] = 0;
                balances[addr] += n;
            }
        }
    }

    return balances;
}

set <set<CTxDestination>> CWallet::GetAddressGroupings() {
    AssertLockHeld(cs_wallet); // mapWallet
    set <set<CTxDestination>> groupings;
    set <CTxDestination> grouping;

    BOOST_FOREACH(PAIRTYPE(uint256, CWalletTx)
    walletEntry, mapWallet)
    {
        CWalletTx *pcoin = &walletEntry.second;

        if (pcoin->vin.size() > 0) {
            bool any_mine = false;
            // group all input addresses with each other
            BOOST_FOREACH(CTxIn
            txin, pcoin->vin)
            {
                CTxDestination address;
                if (!IsMine(txin)) /* If this input isn't mine, ignore it */
                    continue;
                if (!ExtractDestination(mapWallet[txin.prevout.hash].vout[txin.prevout.n].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                any_mine = true;
            }

            // group change with input addresses
            if (any_mine) {
                BOOST_FOREACH(CTxOut
                txout, pcoin->vout)
                if (IsChange(txout)) {
                    CTxDestination txoutAddr;
                    if (!ExtractDestination(txout.scriptPubKey, txoutAddr))
                        continue;
                    grouping.insert(txoutAddr);
                }
            }
            if (grouping.size() > 0) {
                groupings.insert(grouping);
                grouping.clear();
            }
        }

        // group lone addrs by themselves
        for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            if (IsMine(pcoin->vout[i])) {
                CTxDestination address;
                if (!ExtractDestination(pcoin->vout[i].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                groupings.insert(grouping);
                grouping.clear();
            }
    }

    set < set < CTxDestination > * > uniqueGroupings; // a set of pointers to groups of addresses
    map < CTxDestination, set < CTxDestination > * > setmap;  // map addresses to the unique group containing it
    BOOST_FOREACH(set < CTxDestination > grouping, groupings)
    {
        // make a set of all the groups hit by this new group
        set < set < CTxDestination > * > hits;
        map < CTxDestination, set < CTxDestination > * > ::iterator
        it;
        BOOST_FOREACH(CTxDestination
        address, grouping)
        if ((it = setmap.find(address)) != setmap.end())
            hits.insert((*it).second);

        // merge all hit groups into a new single group and delete old groups
        set <CTxDestination> *merged = new set<CTxDestination>(grouping);
        BOOST_FOREACH(set < CTxDestination > *hit, hits)
        {
            merged->insert(hit->begin(), hit->end());
            uniqueGroupings.erase(hit);
            delete hit;
        }
        uniqueGroupings.insert(merged);

        // update setmap
        BOOST_FOREACH(CTxDestination
        element, *merged)
        setmap[element] = merged;
    }

    set <set<CTxDestination>> ret;
    BOOST_FOREACH(set < CTxDestination > *uniqueGrouping, uniqueGroupings)
    {
        ret.insert(*uniqueGrouping);
        delete uniqueGrouping;
    }

    return ret;
}

std::set <CTxDestination> CWallet::GetAccountAddresses(const std::string &strAccount) const {
    LOCK(cs_wallet);
    set <CTxDestination> result;
    BOOST_FOREACH(
    const PAIRTYPE(CTxDestination, CAddressBookData)&item, mapAddressBook)
    {
        const CTxDestination &address = item.first;
        const string &strName = item.second.name;
        if (strName == strAccount)
            result.insert(address);
    }
    return result;
}

bool CReserveKey::GetReservedKey(CPubKey& pubkey, bool fInternalIn)
{
    if (nIndex == -1)
    {
        CKeyPool keypool;
        pwallet->ReserveKeyFromKeyPool(nIndex, keypool, fInternalIn);
        if (nIndex != -1) {
            vchPubKey = keypool.vchPubKey;
        }
        else {
            return false;
        }
        fInternal = keypool.fInternal;
    }
    assert(vchPubKey.IsValid());
    pubkey = vchPubKey;
    return true;
}

void CReserveKey::KeepKey()
{
    if (nIndex != -1) {
        pwallet->KeepKey(nIndex);
    }
    nIndex = -1;
    vchPubKey = CPubKey();
}

void CReserveKey::ReturnKey()
{
    if (nIndex != -1) {
        pwallet->ReturnKey(nIndex, fInternal);
    }
    nIndex = -1;
    vchPubKey = CPubKey();
}

static void LoadReserveKeysToSet(std::set<CKeyID>& setAddress, const std::set<int64_t>& setKeyPool, CWalletDB& walletdb)
{
    BOOST_FOREACH(const int64_t& id, setKeyPool)
    {
        CKeyPool keypool;
        if (!walletdb.ReadPool(id, keypool))
            throw runtime_error("GetAllReserveKeyHashes(): read failed");
        assert(keypool.vchPubKey.IsValid());
        CKeyID keyID = keypool.vchPubKey.GetID();
        setAddress.insert(keyID);
    }
}

void CWallet::GetAllReserveKeys(std::set<CKeyID>& setAddress) const
{
    setAddress.clear();

    CWalletDB walletdb(strWalletFile);

    LOCK2(cs_main, cs_wallet);
    LoadReserveKeysToSet(setAddress, setInternalKeyPool, walletdb);
    LoadReserveKeysToSet(setAddress, setExternalKeyPool, walletdb);

    BOOST_FOREACH (const CKeyID& keyID, setAddress) {
        if (!HaveKey(keyID)) {
            throw std::runtime_error(std::string(__func__) + ": unknown key in key pool");
        }
    }
}

bool CWallet::UpdatedTransaction(const uint256 &hashTx) {
    {
        LOCK(cs_wallet);
        // Only notify UI if this transaction is in this wallet
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(hashTx);
        if (mi != mapWallet.end()){
            NotifyTransactionChanged(this, hashTx, CT_UPDATED);
            return true;
        }
    }
    return false;
}

void CWallet::GetScriptForMining(boost::shared_ptr <CReserveScript> &script) {
    boost::shared_ptr <CReserveKey> rKey(new CReserveKey(this));
    CPubKey pubkey;
    if (!rKey->GetReservedKey(pubkey,true))
        return;

    script = rKey;
    script->reserveScript = CScript() << OP_DUP << OP_HASH160 << ToByteVector(pubkey.GetID()) << OP_EQUALVERIFY << OP_CHECKSIG;
}

void CWallet::LockCoin(const COutPoint &output) {
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.insert(output);
}

void CWallet::UnlockCoin(const COutPoint &output) {
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.erase(output);
}

void CWallet::UnlockAllCoins() {
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.clear();
}

bool CWallet::IsLockedCoin(uint256 hash, unsigned int n) const {
    AssertLockHeld(cs_wallet); // setLockedCoins
    COutPoint outpt(hash, n);

    return (setLockedCoins.count(outpt) > 0);
}

void CWallet::ListLockedCoins(std::vector <COutPoint> &vOutpts) {
    AssertLockHeld(cs_wallet); // setLockedCoins
    for (std::set<COutPoint>::iterator it = setLockedCoins.begin();
         it != setLockedCoins.end(); it++) {
        COutPoint outpt = (*it);
        vOutpts.push_back(outpt);
    }
}

/** @} */ // end of Actions

class CAffectedKeysVisitor : public boost::static_visitor<void> {
private:
    const CKeyStore &keystore;
    std::vector <CKeyID> &vKeys;

public:
    CAffectedKeysVisitor(const CKeyStore &keystoreIn, std::vector <CKeyID> &vKeysIn) : keystore(keystoreIn),
                                                                                       vKeys(vKeysIn) {}

    void Process(const CScript &script) {
        txnouttype type;
        std::vector <CTxDestination> vDest;
        int nRequired;
        if (ExtractDestinations(script, type, vDest, nRequired)) {
            BOOST_FOREACH(
            const CTxDestination &dest, vDest)
            boost::apply_visitor(*this, dest);
        }
    }

    void operator()(const CKeyID &keyId) {
        if (keystore.HaveKey(keyId))
            vKeys.push_back(keyId);
    }

    void operator()(const CScriptID &scriptId) {
        CScript script;
        if (keystore.GetCScript(scriptId, script))
            Process(script);
    }

    void operator()(const CNoDestination &none) {}
};

void CWallet::GetKeyBirthTimes(std::map <CKeyID, int64_t> &mapKeyBirth) const {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    mapKeyBirth.clear();

    // get birth times for keys with metadata
    for (std::map<CKeyID, CKeyMetadata>::const_iterator it = mapKeyMetadata.begin(); it != mapKeyMetadata.end(); it++)
        if (it->second.nCreateTime)
            mapKeyBirth[it->first] = it->second.nCreateTime;

    // map in which we'll infer heights of other keys
    CBlockIndex *pindexMax = chainActive[std::max(0, chainActive.Height() -  144)]; // the tip can be reorganized; use a 144-block safety margin
    std::map < CKeyID, CBlockIndex * > mapKeyFirstBlock;
    std::set <CKeyID> setKeys;
    GetKeys(setKeys);
    BOOST_FOREACH(
    const CKeyID &keyid, setKeys) {
        if (mapKeyBirth.count(keyid) == 0)
            mapKeyFirstBlock[keyid] = pindexMax;
    }
    setKeys.clear();

    // if there are no such keys, we're done
    if (mapKeyFirstBlock.empty())
        return;

    // find first block that affects those keys, if there are any left
    std::vector <CKeyID> vAffected;
    for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); it++) {
        // iterate over all wallet transactions...
        const CWalletTx &wtx = (*it).second;
        BlockMap::const_iterator blit = mapBlockIndex.find(wtx.hashBlock);
        if (blit != mapBlockIndex.end() && chainActive.Contains(blit->second)) {
            // ... which are already in a block
            int nHeight = blit->second->nHeight;
            BOOST_FOREACH(
            const CTxOut &txout, wtx.vout) {
                // iterate over all their outputs
                CAffectedKeysVisitor(*this, vAffected).Process(txout.scriptPubKey);
                BOOST_FOREACH(
                const CKeyID &keyid, vAffected) {
                    // ... and all their affected keys
                    std::map<CKeyID, CBlockIndex *>::iterator rit = mapKeyFirstBlock.find(keyid);
                    if (rit != mapKeyFirstBlock.end() && nHeight < rit->second->nHeight)
                        rit->second = blit->second;
                }
                vAffected.clear();
            }
        }
    }

    // Extract block timestamps for those keys
    for (std::map<CKeyID, CBlockIndex *>::const_iterator it = mapKeyFirstBlock.begin();
         it != mapKeyFirstBlock.end(); it++)
        mapKeyBirth[it->first] = it->second->GetBlockTime() - 7200; // block times can be 2h off
}

bool CWallet::AddDestData(const CTxDestination &dest, const std::string &key, const std::string &value) {
    if (boost::get<CNoDestination>(&dest))
        return false;

    mapAddressBook[dest].destdata.insert(std::make_pair(key, value));
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteDestData(CBitcoinAddress(dest).ToString(), key, value);
}

bool CWallet::EraseDestData(const CTxDestination &dest, const std::string &key) {
    if (!mapAddressBook[dest].destdata.erase(key))
        return false;
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).EraseDestData(CBitcoinAddress(dest).ToString(), key);
}

bool CWallet::LoadDestData(const CTxDestination &dest, const std::string &key, const std::string &value) {
    mapAddressBook[dest].destdata.insert(std::make_pair(key, value));
    return true;
}

bool CWallet::GetDestData(const CTxDestination &dest, const std::string &key, std::string *value) const {
    std::map<CTxDestination, CAddressBookData>::const_iterator i = mapAddressBook.find(dest);
    if (i != mapAddressBook.end()) {
        CAddressBookData::StringMap::const_iterator j = i->second.destdata.find(key);
        if (j != i->second.destdata.end()) {
            if (value)
                *value = j->second;
            return true;
        }
    }
    return false;
}

std::string CWallet::GetWalletHelpString(bool showDebug) {
    std::string strUsage = HelpMessageGroup(_("Wallet options:"));
    strUsage += HelpMessageOpt("-disablewallet", _("Do not load the wallet and disable wallet RPC calls"));
    strUsage += HelpMessageOpt("-keypool=<n>",
                               strprintf(_("Set key pool size to <n> (default: %u)"), DEFAULT_KEYPOOL_SIZE));
    strUsage += HelpMessageOpt("-fallbackfee=<amt>", strprintf(
            _("A fee rate (in %s/kB) that will be used when fee estimation has insufficient data (default: %s)"),
            CURRENCY_UNIT, FormatMoney(DEFAULT_FALLBACK_FEE)));
    strUsage += HelpMessageOpt("-mintxfee=<amt>", strprintf(
            _("Fees (in %s/kB) smaller than this are considered zero fee for transaction creation (default: %s)"),
            CURRENCY_UNIT, FormatMoney(DEFAULT_TRANSACTION_MINFEE)));
    strUsage += HelpMessageOpt("-paytxfee=<amt>",
                               strprintf(_("Fee (in %s/kB) to add to transactions you send (default: %s)"),
                                         CURRENCY_UNIT, FormatMoney(payTxFee.GetFeePerK())));
    strUsage += HelpMessageOpt("-rescan", _("Rescan the block chain for missing wallet transactions on startup"));
    strUsage += HelpMessageOpt("-salvagewallet", _("Attempt to recover private keys from a corrupt wallet on startup"));
    if (showDebug)
        strUsage += HelpMessageOpt("-sendfreetransactions",
                                   strprintf(_("Send transactions as zero-fee transactions if possible (default: %u)"),
                                             DEFAULT_SEND_FREE_TRANSACTIONS));
    strUsage += HelpMessageOpt("-spendzeroconfchange",
                               strprintf(_("Spend unconfirmed change when sending transactions (default: %u)"),
                                         DEFAULT_SPEND_ZEROCONF_CHANGE));
    strUsage += HelpMessageOpt("-txconfirmtarget=<n>", strprintf(
            _("If paytxfee is not set, include enough fee so transactions begin confirmation on average within n blocks (default: %u)"),
            DEFAULT_TX_CONFIRM_TARGET));
    strUsage += HelpMessageOpt("-usehd",
                               _("Use hierarchical deterministic key generation (HD) after BIP32. Only has effect during wallet creation/first start") +
                               " " + strprintf(_("(default: %u)"), DEFAULT_USE_HD_WALLET));
    strUsage += HelpMessageOpt("-upgradewallet", _("Upgrade wallet to latest format on startup"));
    strUsage += HelpMessageOpt("-wallet=<file>", _("Specify wallet file (within data directory)") + " " +
                                                 strprintf(_("(default: %s)"), DEFAULT_WALLET_DAT));
    strUsage += HelpMessageOpt("-walletbackupsdir=<path>", _("Specify a custom backup directory"));                                                     
    strUsage += HelpMessageOpt("-walletbroadcast", _("Make the wallet broadcast transactions") + " " +
                                                   strprintf(_("(default: %u)"), DEFAULT_WALLETBROADCAST));
    strUsage += HelpMessageOpt("-walletnotify=<cmd>",
                               _("Execute command when a wallet transaction changes (%s in cmd is replaced by TxID)"));
    strUsage += HelpMessageOpt("-zapwallettxes=<mode>",
                               _("Delete all wallet transactions and only recover those parts of the blockchain through -rescan on startup") +
                               " " +
                               _("(1 = keep tx meta data e.g. account owner and payment request information, 2 = drop tx meta data)"));

    if (showDebug) {
        strUsage += HelpMessageGroup(_("Wallet debugging/testing options:"));

        strUsage += HelpMessageOpt("-dblogsize=<n>", strprintf(
                "Flush wallet database activity from memory to disk log every <n> megabytes (default: %u)",
                DEFAULT_WALLET_DBLOGSIZE));
        strUsage += HelpMessageOpt("-flushwallet", strprintf("Run a thread to flush wallet periodically (default: %u)",
                                                             DEFAULT_FLUSHWALLET));
        strUsage += HelpMessageOpt("-privdb",
                                   strprintf("Sets the DB_PRIVATE flag in the wallet db environment (default: %u)",
                                             DEFAULT_WALLET_PRIVDB));
        strUsage += HelpMessageOpt("-walletrejectlongchains", strprintf(
                _("Wallet will not create transactions that violate mempool chain limits (default: %u"),
                DEFAULT_WALLET_REJECT_LONG_CHAINS));
    }

    return strUsage;
}

// bool CWallet::ParameterInteraction() {
//     if (mapArgs.count("-mintxfee")) {
//         CAmount n = 0;
//         if (ParseMoney(mapArgs["-mintxfee"], n) && n > 0)
//             CWallet::minTxFee = CFeeRate(n);
//         else
//             return InitError(AmountErrMsg("mintxfee", mapArgs["-mintxfee"]));
//     }
//     if (mapArgs.count("-fallbackfee")) {
//         CAmount nFeePerK = 0;
//         if (!ParseMoney(mapArgs["-fallbackfee"], nFeePerK))
//             return InitError(strprintf(_("Invalid amount for -fallbackfee=<amount>: '%s'"), mapArgs["-fallbackfee"]));
//         if (nFeePerK > HIGH_TX_FEE_PER_KB)
//             InitWarning(
//                     _("-fallbackfee is set very high! This is the transaction fee you may pay when fee estimates are not available."));
//         CWallet::fallbackFee = CFeeRate(nFeePerK);
//     }
//     if (mapArgs.count("-paytxfee")) {
//         CAmount nFeePerK = 0;
//         if (!ParseMoney(mapArgs["-paytxfee"], nFeePerK))
//             return InitError(AmountErrMsg("paytxfee", mapArgs["-paytxfee"]));
//         if (nFeePerK > HIGH_TX_FEE_PER_KB)
//             InitWarning(
//                     _("-paytxfee is set very high! This is the transaction fee you will pay if you send a transaction."));
//         payTxFee = CFeeRate(nFeePerK, 1000);
//         if (payTxFee < ::minRelayTxFee) {
//             return InitError(strprintf(_("Invalid amount for -paytxfee=<amount>: '%s' (must be at least %s)"),
//                                        mapArgs["-paytxfee"], ::minRelayTxFee.ToString()));
//         }
//     }
//     if (mapArgs.count("-maxtxfee")) {
//         CAmount nMaxFee = 0;
//         if (!ParseMoney(mapArgs["-maxtxfee"], nMaxFee))
//             return InitError(AmountErrMsg("maxtxfee", mapArgs["-maxtxfee"]));
//         if (nMaxFee > HIGH_MAX_TX_FEE)
//             InitWarning(_("-maxtxfee is set very high! Fees this large could be paid on a single transaction."));
//         maxTxFee = nMaxFee;
//         if (CFeeRate(maxTxFee, 1000) < ::minRelayTxFee) {
//             return InitError(strprintf(
//                     _("Invalid amount for -maxtxfee=<amount>: '%s' (must be at least the minrelay fee of %s to prevent stuck transactions)"),
//                     mapArgs["-maxtxfee"], ::minRelayTxFee.ToString()));
//         }
//     }

//     if (mapArgs.count("-mininput"))
//     {
//         if (!ParseMoney(mapArgs["-mininput"], nMinimumInputValue))
//             return InitError(strprintf(_("Invalid amount for -mininput=<amount>: '%s'"), mapArgs["-mininput"].c_str()));
//     }

//     nTxConfirmTarget = GetArg("-txconfirmtarget", DEFAULT_TX_CONFIRM_TARGET);
//     bSpendZeroConfChange = GetBoolArg("-spendzeroconfchange", DEFAULT_SPEND_ZEROCONF_CHANGE);
//     fSendFreeTransactions = GetBoolArg("-sendfreetransactions", DEFAULT_SEND_FREE_TRANSACTIONS);

//     return true;
// }

bool CWallet::BackupWallet(const std::string &strDest) {
    if (!fFileBacked)
        return false;
    while (true) {
        {
            LOCK(bitdb.cs_db);
            if (!bitdb.mapFileUseCount.count(strWalletFile) || bitdb.mapFileUseCount[strWalletFile] == 0) {
                // Flush log data to the dat file
                bitdb.CloseDb(strWalletFile);
                bitdb.CheckpointLSN(strWalletFile);
                bitdb.mapFileUseCount.erase(strWalletFile);

                // Copy wallet file
                boost::filesystem::path pathSrc = GetDataDir() / strWalletFile;
                boost::filesystem::path pathDest(strDest);
                if (boost::filesystem::is_directory(pathDest))
                    pathDest /= strWalletFile;

                try {
#if BOOST_VERSION >= 104000
                    boost::filesystem::copy_file(pathSrc, pathDest, boost::filesystem::copy_option::overwrite_if_exists);
#else
                    boost::filesystem::copy_file(pathSrc, pathDest);
#endif
                    LogPrintf("copied %s to %s\n", strWalletFile, pathDest.string());
                    return true;
                } catch (const boost::filesystem::filesystem_error &e) {
                    LogPrintf("error copying %s to %s - %s\n", strWalletFile, pathDest.string(), e.what());
                    return false;
                }
            }
        }
        MilliSleep(100);
    }
    return false;
}

CKeyPool::CKeyPool()
{
    nTime = GetTime();
    fInternal = false;
}

CKeyPool::CKeyPool(const CPubKey& vchPubKeyIn, bool fInternalIn)
{
    nTime = GetTime();
    vchPubKey = vchPubKeyIn;
    fInternal = fInternalIn;
}

CWalletKey::CWalletKey(int64_t nExpires) {
    nTimeCreated = (nExpires ? GetTime() : 0);
    nTimeExpires = nExpires;
}

int CMerkleTx::SetMerkleBranch(const CBlock &block) {
    AssertLockHeld(cs_main);
    CBlock blockTmp;

    // Update the tx's hashBlock
    hashBlock = block.GetHash();

    // Locate the transaction
    for (nIndex = 0; nIndex < (int) block.vtx.size(); nIndex++)
        if (block.vtx[nIndex] == *(CTransaction * )this)
    break;
    if (nIndex == (int) block.vtx.size()) {
        nIndex = -1;
        LogPrintf("ERROR: SetMerkleBranch(): couldn't find tx in block\n");
        return 0;
    }

    // Is the tx in a block that's in the main chain
    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    const CBlockIndex *pindex = (*mi).second;
    if (!pindex || !chainActive.Contains(pindex))
        return 0;

    return chainActive.Height() - pindex->nHeight + 1;
}

int CMerkleTx::GetDepthInMainChain(const CBlockIndex* &pindexRet, bool enableIX) const
{
    int nResult;

    if (hashUnset())
        nResult = 0;
    else {
        AssertLockHeld(cs_main);

        // Find the block it claims to be in
        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi == mapBlockIndex.end())
            nResult = 0;
        else {
            CBlockIndex* pindex = (*mi).second;
            if (!pindex || !chainActive.Contains(pindex))
                nResult = 0;
            else {
                pindexRet = pindex;
                nResult = ((nIndex == -1) ? (-1) : 1) * (chainActive.Height() - pindex->nHeight + 1);

                if (nResult == 0 && !mempool.exists(GetHash()))
                    return -1; // Not in chain, not in mempool
            }
        }
    }

    if(enableIX && nResult < 6 && instantsend.IsLockedInstantSendTransaction(GetHash()))
        return nInstantSendDepth + nResult;

    return nResult;
}


int CMerkleTx::GetDepthInMainChain(const CBlockIndex *&pindexRet) const {
    if (hashUnset())
        return 0;

    AssertLockHeld(cs_main);

    // Find the block it claims to be in
    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex *pindex = (*mi).second;
    if (!pindex || !chainActive.Contains(pindex))
        return 0;
    pindexRet = pindex;
    return ((nIndex == -1) ? (-1) : 1) * (chainActive.Height() - pindex->nHeight + 1);
}

int CMerkleTx::GetBlocksToMaturity() const {
    if (!IsCoinBase())
        return 0;
    return max(0, (COINBASE_MATURITY + 1) - GetDepthInMainChain());
}


bool CMerkleTx::AcceptToMemoryPool(bool fLimitFree, bool fRejectAbsurdFee)
{
    CValidationState state;
    return ::AcceptToMemoryPool(mempool, state, *this, fLimitFree, NULL, false, fRejectAbsurdFee);
}
