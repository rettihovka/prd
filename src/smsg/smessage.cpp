// Copyright (c) 2014-2016 The ShadowCoin developers
// Copyright (c) 2017 The Particl Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/*
Notes:
    Running with -debug could leave to and from address hashes and public keys in the log.

    Wallet Locked
        A copy of each incoming message is stored in bucket files ending in _wl.dat
        wl (wallet locked) bucket files are deleted if they expire, like normal buckets
        When the wallet is unlocked all the messages in wl files are scanned.

    Address Whitelist
        Owned Addresses are stored in smsgAddresses vector
        Saved to smsg.ini
        Modify options using the smsglocalkeys rpc command or edit the smsg.ini file (with client closed)

    TODO:
        For buckets older than current, only need to store no. messages and hash in memory

*/

#include "smessage.h"

#include <stdint.h>
#include <time.h>
#include <map>
#include <stdexcept>
#include <errno.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/thread.hpp>

#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <secp256k1_recovery.h>

#include "crypto/hmac_sha256.h"
#include "crypto/sha512.h"

#include "script/ismine.h"
#include "policy/policy.h"
#include "support/allocators/secure.h"

#include "base58.h"
#include "sync.h"
#include "random.h"
#include "chain.h"
#include "netmessagemaker.h"
#include "fs.h"

#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include "utilstrencodings.h"
#include "clientversion.h"


#include "lz4/lz4.c"

#include "xxhash/xxhash.h"
#include "xxhash/xxhash.c"

#include "smsg/crypter.h"
#include "smsg/db.h"


boost::thread_group threadGroupSmsg;

boost::signals2::signal<void (SecMsgStored& inboxHdr)>  NotifySecMsgInboxChanged;
boost::signals2::signal<void (SecMsgStored& outboxHdr)> NotifySecMsgOutboxChanged;
boost::signals2::signal<void ()> NotifySecMsgWalletUnlocked;

bool fSecMsgEnabled = false;

std::map<int64_t, SecMsgBucket> smsgBuckets;
std::vector<SecMsgAddress>      smsgAddresses;
SecMsgOptions                   smsgOptions;


CCriticalSection cs_smsg;
CCriticalSection cs_smsgThreads;

extern void Misbehaving(NodeId pnode, int howmuch);
extern bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex, const Consensus::Params& consensusParams);
extern CChain chainActive;
extern CCriticalSection cs_main;

secp256k1_context *secp256k1_context_smsg = nullptr;
CWallet *pwalletSmsg = nullptr;

typedef std::vector<unsigned char> valtype; // script/ismine.cpp


void SecMsgBucket::hashBucket()
{
    void *state = XXH32_init(1);

    std::set<SecMsgToken>::iterator it;
    for (it = setTokens.begin(); it != setTokens.end(); ++it)
    {
        XXH32_update(state, it->sample, 8);
    };

    uint32_t hash_new = XXH32_digest(state);

    if (hash != hash_new)
    {
        LogPrint(BCLog::SMSG, "Bucket hash updated from %u to %u.\n", hash, hash_new);

        hash = hash_new;
        timeChanged = GetTime();
    };

    LogPrint(BCLog::SMSG, "Hashed %u messages, hash %u\n", setTokens.size(), hash_new);
};


void ThreadSecureMsg()
{
    // Bucket management thread
    //SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL);

    uint32_t nLoop = 0;
    std::vector<std::pair<int64_t, NodeId> > vTimedOutLocks;
    while (fSecMsgEnabled)
    {
        nLoop++;
        int64_t now = GetTime();

        if (LogAcceptCategory(BCLog::SMSG) && nLoop % SMSG_THREAD_LOG_GAP == 0) // log every SMSG_THREAD_LOG_GAP instance, is useful source of timestamps
            LogPrintf("SecureMsgThread %d \n", now);

        vTimedOutLocks.resize(0);

        int64_t cutoffTime = now - SMSG_RETENTION;
        {
            LOCK(cs_smsg);
            for (std::map<int64_t, SecMsgBucket>::iterator it(smsgBuckets.begin()); it != smsgBuckets.end(); )
            {
                //if (fDebugSmsg)
                //    LogPrintf("Checking bucket %d, size %u \n", it->first, it->second.setTokens.size());

                if (it->first < cutoffTime)
                {
                    LogPrint(BCLog::SMSG, "Removing bucket %d \n", it->first);

                    std::string fileName = std::to_string(it->first);

                    fs::path fullPath = GetDataDir() / "smsgstore" / (fileName + "_01.dat");
                    if (fs::exists(fullPath))
                    {
                        try { fs::remove(fullPath);
                        } catch (const fs::filesystem_error &ex)
                        {
                            LogPrintf("Error removing bucket file %s.\n", ex.what());
                        };
                    } else
                    {
                        LogPrintf("Path %s does not exist \n", fullPath.string());
                    };

                    // Look for a wl file, it stores incoming messages when wallet is locked
                    fullPath = GetDataDir() / "smsgstore" / (fileName + "_01_wl.dat");
                    if (fs::exists(fullPath))
                    {
                        try { fs::remove(fullPath);
                        } catch (const fs::filesystem_error &ex)
                        {
                            LogPrintf("Error removing wallet locked file %s.\n", ex.what());
                        };
                    };

                    smsgBuckets.erase(it++);
                } else
                {
                    if (it->second.nLockCount > 0) // Tick down nLockCount, so will eventually expire if peer never sends data
                    {
                        it->second.nLockCount--;

                        if (it->second.nLockCount == 0)     // lock timed out
                        {
                            vTimedOutLocks.push_back(std::make_pair(it->first, it->second.nLockPeerId)); // g_connman->cs_vNodes

                            it->second.nLockPeerId = 0;
                        };
                    };
                    ++it;
                };
            };
        } // cs_smsg

        for (std::vector<std::pair<int64_t, NodeId> >::iterator it(vTimedOutLocks.begin()); it != vTimedOutLocks.end(); it++)
        {
            NodeId nPeerId = it->second;
            uint32_t fExists = 0;

            LogPrint(BCLog::SMSG, "Lock on bucket %d for peer %d timed out.\n", it->first, nPeerId);

            // Look through the nodes for the peer that locked this bucket

            {
                LOCK(g_connman->cs_vNodes);
                for (auto *pnode : g_connman->vNodes)
                {
                    if (pnode->GetId() != nPeerId)
                        continue;

                    fExists = 1; //found in g_connman->vNodes

                    LOCK(pnode->smsgData.cs_smsg_net);
                    int64_t ignoreUntil = GetTime() + SMSG_TIME_IGNORE;
                    pnode->smsgData.ignoreUntil = ignoreUntil;

                    // Alert peer that they are being ignored
                    std::vector<uint8_t> vchData;
                    vchData.resize(8);
                    memcpy(&vchData[0], &ignoreUntil, 8);
                    g_connman->PushMessage(pnode,
                        CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgIgnore", vchData));

                    LogPrint(BCLog::SMSG, "This node will ignore peer %d until %d.\n", nPeerId, ignoreUntil);
                    break;
                };
            } // g_connman->cs_vNodes

            LogPrint(BCLog::SMSG, "smsg-thread: ignoring - looked peer %d, status on search %u\n", nPeerId, fExists);
        };

        MilliSleep(SMSG_THREAD_DELAY * 1000); //  // check every SMSG_THREAD_DELAY seconds
    };
};

void ThreadSecureMsgPow()
{
    // Proof of work thread

    int rv;
    std::vector<uint8_t> vchKey;
    SecMsgStored smsgStored;

    std::string sPrefix("qm");
    uint8_t chKey[18];

    while (fSecMsgEnabled)
    {
        // Sleep at end, then fSecMsgEnabled is tested on wake

        SecMsgDB dbOutbox;
        leveldb::Iterator *it;
        {
            LOCK(cs_smsgDB);

            if (!dbOutbox.Open("cr+"))
                continue;

            // fifo (smallest key first)
            it = dbOutbox.pdb->NewIterator(leveldb::ReadOptions());
        }
        // Break up lock, SecureMsgSetHash will take long

        for (;;)
        {
            {
                LOCK(cs_smsgDB);
                if (!dbOutbox.NextSmesg(it, sPrefix, chKey, smsgStored))
                    break;
            }

            uint8_t *pHeader = &smsgStored.vchMessage[0];
            uint8_t *pPayload = &smsgStored.vchMessage[SMSG_HDR_LEN];
            SecureMessage* psmsg = (SecureMessage*) pHeader;

            // Do proof of work
            rv = SecureMsgSetHash(pHeader, pPayload, psmsg->nPayload);
            if (rv == 2)
                break; // leave message in db, if terminated due to shutdown

            // Message is removed here, no matter what
            {
                LOCK(cs_smsgDB);
                dbOutbox.EraseSmesg(chKey);
            }
            
            if (rv != 0)
            {
                LogPrintf("SecMsgPow: Could not get proof of work hash, message removed.\n");
                continue;
            };

            // Add to message store
            {
                LOCK(cs_smsg);
                if (SecureMsgStore(pHeader, pPayload, psmsg->nPayload, true) != 0)
                {
                    LogPrintf("SecMsgPow: Could not place message in buckets, message removed.\n");
                    continue;
                };
            }

            // Test if message was sent to self
            if (SecureMsgScanMessage(pHeader, pPayload, psmsg->nPayload, true) != 0)
            {
                // message recipient is not this node (or failed)
            };
        };

        delete it;

        // Shutdown thread waits 5 seconds, this should be less
        MilliSleep(2000); // seconds
    };
};

std::string SecureMsgGetHelpString(bool showDebug)
{
    std::string strUsage;

    strUsage += HelpMessageGroup(_("Secure messaging options:"));
    strUsage += HelpMessageOpt("-smsg", _("Enable secure messaging. (default: true)"));
    strUsage += HelpMessageOpt("-debugsmsg", _("Show extra debug messages. (default: false)"));
    strUsage += HelpMessageOpt("-smsgscanchain", _("Scan the block chain for public key addresses on startup. (default: false)"));
    strUsage += HelpMessageOpt("-smsgscanincoming", _("Scan incoming blocks for public key addresses. (default: false)"));
    strUsage += HelpMessageOpt("-smsgnotify=<cmd>", _("Execute command when a message is received. (%s in cmd is replaced by receiving address)"));

    return strUsage;
};

int SecureMsgBuildBucketSet()
{
    /*
        Build the bucket set by scanning the files in the smsgstore dir.

        smsgBuckets should be empty
    */

    LogPrint(BCLog::SMSG, "SecureMsgBuildBucketSet()\n");

    int64_t  now            = GetTime();
    uint32_t nFiles         = 0;
    uint32_t nMessages      = 0;

    fs::path pathSmsgDir = GetDataDir() / "smsgstore";
    fs::directory_iterator itend;

    if (!fs::exists(pathSmsgDir)
        || !fs::is_directory(pathSmsgDir))
    {
        LogPrintf("Message store directory does not exist.\n");
        return 0; // not an error
    };

    for (fs::directory_iterator itd(pathSmsgDir) ; itd != itend ; ++itd)
    {
        if (!fs::is_regular_file(itd->status()))
            continue;

        std::string fileType = (*itd).path().extension().string();

        if (fileType.compare(".dat") != 0)
            continue;

        std::string fileName = (*itd).path().filename().string();

        LogPrint(BCLog::SMSG, "Processing file: %s.\n", fileName);

        nFiles++;

        // TODO files must be split if > 2GB
        // time_noFile.dat
        size_t sep = fileName.find_first_of("_");
        if (sep == std::string::npos)
            continue;

        std::string stime = fileName.substr(0, sep);
        int64_t fileTime;
        if (!ParseInt64(stime, &fileTime))
        {
            LogPrintf("%s: ParseInt64 failed %s.\n", __func__, stime);
            continue;
        };

        if (fileTime < now - SMSG_RETENTION)
        {
            LogPrintf("Dropping file %s, expired.\n", fileName);
            try {
                fs::remove((*itd).path());
            } catch (const fs::filesystem_error &ex) {
                LogPrintf("Error removing bucket file %s, %s.\n", fileName, ex.what());
            };
            continue;
        };

        if (boost::algorithm::ends_with(fileName, "_wl.dat"))
        {
            LogPrint(BCLog::SMSG, "Skipping wallet locked file: %s.\n", fileName);
            continue;
        };

        size_t nTokenSetSize = 0;
        SecureMessage smsg;
        {
            LOCK(cs_smsg);

            std::set<SecMsgToken>& tokenSet = smsgBuckets[fileTime].setTokens;

            FILE *fp;
            if (!(fp = fopen((*itd).path().string().c_str(), "rb")))
            {
                LogPrintf("Error opening file: %s\n", strerror(errno));
                continue;
            };

            for (;;)
            {
                long int ofs = ftell(fp);
                SecMsgToken token;
                token.offset = ofs;
                errno = 0;
                if (fread(&smsg.hash[0], sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN)
                {
                    if (errno != 0)
                    {
                        LogPrintf("fread header failed: %s\n", strerror(errno));
                    } else
                    {
                        //LogPrintf("End of file.\n");
                    };
                    break;
                };
                token.timestamp = smsg.timestamp;

                if (smsg.nPayload < 8)
                    continue;

                if (fread(token.sample, sizeof(uint8_t), 8, fp) != 8)
                {
                    LogPrintf("fread data failed: %s\n", strerror(errno));
                    break;
                };

                if (fseek(fp, smsg.nPayload-8, SEEK_CUR) != 0)
                {
                    LogPrintf("fseek, strerror: %s.\n", strerror(errno));
                    break;
                };

                tokenSet.insert(token);
            };

            fclose(fp);

            smsgBuckets[fileTime].hashBucket();

            nTokenSetSize = tokenSet.size();
        } // cs_smsg

        nMessages += nTokenSetSize;
        LogPrint(BCLog::SMSG, "Bucket %d contains %u messages.\n", fileTime, nTokenSetSize);
    };

    LogPrintf("Processed %u files, loaded %u buckets containing %u messages.\n", nFiles, smsgBuckets.size(), nMessages);

    return 0;
};

/*
SecureMsgAddWalletAddresses
    Enumerates the AddressBook, filters out anon outputs and checks the "real addresses"
    Adds these to the vector smsgAddresses to be used for decryption

    Returns 0 on success
*/

int SecureMsgAddWalletAddresses()
{
    LogPrint(BCLog::SMSG, "SecureMsgAddWalletAddresses()\n");

#ifdef ENABLE_WALLET
    if (!pwalletSmsg)
        return errorN(1, "No wallet.");

    uint32_t nAdded = 0;

    for (const auto &entry : pwalletSmsg->mapAddressBook) // PAIRTYPE(CTxDestination, CAddressBookData)
    {
        if (!IsMine(*pwalletSmsg, entry.first))
            continue;

        // TODO: skip addresses for stealth transactions
        CKeyID keyID;
        CBitcoinAddress coinAddress(entry.first);
        if (!coinAddress.IsValid()
            || !coinAddress.GetKeyID(keyID))
            continue;

        bool fExists = 0;
        for (std::vector<SecMsgAddress>::iterator it = smsgAddresses.begin(); it != smsgAddresses.end(); ++it)
        {
            if (keyID != it->address)
                continue;
            fExists = 1;
            break;
        };

        if (fExists)
            continue;

        bool recvEnabled    = 1;
        bool recvAnon       = 1;

        smsgAddresses.push_back(SecMsgAddress(keyID, recvEnabled, recvAnon));
        nAdded++;
    };

    LogPrint(BCLog::SMSG, "Added %u addresses to whitelist.\n", nAdded);
#endif
    return 0;
};


int SecureMsgReadIni()
{
    if (!fSecMsgEnabled)
        return false;

    LogPrint(BCLog::SMSG, "SecureMsgReadIni()\n");

    fs::path fullpath = GetDataDir() / "smsg.ini";

    FILE *fp;
    errno = 0;
    if (!(fp = fopen(fullpath.string().c_str(), "r")))
    {
        LogPrintf("Error opening file: %s\n", strerror(errno));
        return 1;
    };

    char cLine[512];
    char *pName, *pValue;

    char cAddress[64];
    int addrRecv, addrRecvAnon;

    while (fgets(cLine, 512, fp))
    {
        cLine[strcspn(cLine, "\n")] = '\0';
        cLine[strcspn(cLine, "\r")] = '\0';
        cLine[511] = '\0'; // for safety

        // Check that line contains a name value pair and is not a comment, or section header
        if (cLine[0] == '#' || cLine[0] == '[' || strcspn(cLine, "=") < 1)
            continue;

        if (!(pName = strtok(cLine, "="))
            || !(pValue = strtok(nullptr, "=")))
            continue;

        if (strcmp(pName, "newAddressRecv") == 0)
        {
            smsgOptions.fNewAddressRecv = (strcmp(pValue, "true") == 0) ? true : false;
        } else
        if (strcmp(pName, "newAddressAnon") == 0)
        {
            smsgOptions.fNewAddressAnon = (strcmp(pValue, "true") == 0) ? true : false;
        } else
        if (strcmp(pName, "scanIncoming") == 0)
        {
            smsgOptions.fScanIncoming = (strcmp(pValue, "true") == 0) ? true : false;
        } else
        if (strcmp(pName, "key") == 0)
        {
            int rv = sscanf(pValue, "%64[^|]|%d|%d", cAddress, &addrRecv, &addrRecvAnon);
            if (rv == 3)
            {
                CKeyID k;
                CBitcoinAddress(cAddress).GetKeyID(k);
                
                if (k.IsNull())
                    LogPrintf("Could not parse key line %s, rv %d.\n", pValue, rv);
                else
                    smsgAddresses.push_back(SecMsgAddress(k, addrRecv, addrRecvAnon));
            } else
            {
                LogPrintf("Could not parse key line %s, rv %d.\n", pValue, rv);
            }
        } else
        {
            LogPrintf("Unknown setting name: '%s'.", pName);
        };
    };

    LogPrintf("Loaded %u addresses.\n", smsgAddresses.size());

    fclose(fp);

    return 0;
};

int SecureMsgWriteIni()
{
    if (!fSecMsgEnabled)
        return false;

    LogPrint(BCLog::SMSG, "SecureMsgWriteIni()\n");

    fs::path fullpath = GetDataDir() / "smsg.ini~";

    FILE *fp;
    errno = 0;
    if (!(fp = fopen(fullpath.string().c_str(), "w")))
    {
        LogPrintf("Error opening file: %s\n", strerror(errno));
        return 1;
    };

    if (fwrite("[Options]\n", sizeof(char), 10, fp) != 10)
    {
        LogPrintf("fwrite error: %s\n", strerror(errno));
        fclose(fp);
        return false;
    };

    if (fprintf(fp, "newAddressRecv=%s\n", smsgOptions.fNewAddressRecv ? "true" : "false") < 0
        || fprintf(fp, "newAddressAnon=%s\n", smsgOptions.fNewAddressAnon ? "true" : "false") < 0
        || fprintf(fp, "scanIncoming=%s\n", smsgOptions.fScanIncoming ? "true" : "false") < 0)
    {
        LogPrintf("fprintf error: %s\n", strerror(errno));
        fclose(fp);
        return false;
    };

    if (fwrite("\n[Keys]\n", sizeof(char), 8, fp) != 8)
    {
        LogPrintf("fwrite error: %s\n", strerror(errno));
        fclose(fp);
        return false;
    };
    
    for (std::vector<SecMsgAddress>::iterator it = smsgAddresses.begin(); it != smsgAddresses.end(); ++it)
    {
        errno = 0;
        
        CBitcoinAddress cAddress(it->address);
        
        if (!cAddress.IsValid())
        {
            LogPrintf("%s: Error saving address - invalid.", __func__);
            continue;
        };
        
        if (fprintf(fp, "key=%s|%d|%d\n", cAddress.ToString().c_str(), it->fReceiveEnabled, it->fReceiveAnon) < 0)
        {
            LogPrintf("fprintf error: %s\n", strerror(errno));
            continue;
        };
    };

    fclose(fp);

    try {
        fs::path finalpath = GetDataDir() / "smsg.ini";
        fs::rename(fullpath, finalpath);
    } catch (const fs::filesystem_error &ex) {
        LogPrintf("Error renaming file %s, %s.\n", fullpath.string(), ex.what());
    };
    return 0;
};

bool SecureMsgStart(CWallet *pwallet, bool fDontStart, bool fScanChain)
{
    if (fDontStart)
    {
        LogPrintf("Secure messaging not started.\n");
        return false;
    };
    
    LogPrintf("Secure messaging starting.\n");

    if (pwalletSmsg)
        return error("%s: pwalletSmsg is already set.", __func__);
    pwalletSmsg = pwallet;

    fSecMsgEnabled = true;
    g_connman->SetLocalServices(ServiceFlags(g_connman->GetLocalServices() | NODE_SMSG));

    if (SecureMsgReadIni() != 0)
        LogPrintf("Failed to read smsg.ini\n");

    if (smsgAddresses.size() < 1)
    {
        LogPrintf("No address keys loaded.\n");
        if (SecureMsgAddWalletAddresses() != 0)
            LogPrintf("Failed to load addresses from wallet.\n");
        else
            LogPrintf("Loaded addresses from wallet.\n");
    } else
    {
        LogPrintf("Loaded addresses from SMSG.ini\n");
    };

    if (secp256k1_context_smsg)
        return error("%s: secp256k1_context_smsg already exists.", __func__);

    if (!(secp256k1_context_smsg = secp256k1_context_create(SECP256K1_CONTEXT_SIGN)))
        return error("%s: secp256k1_context_create failed.", __func__);

    {
        // Pass in a random blinding seed to the secp256k1 context.
        std::vector<unsigned char, secure_allocator<unsigned char>> vseed(32);
        GetRandBytes(vseed.data(), 32);
        bool ret = secp256k1_context_randomize(secp256k1_context_smsg, vseed.data());
        assert(ret);
    }

    if (fScanChain)
    {
        SecureMsgScanBlockChain();
    };

    if (SecureMsgBuildBucketSet() != 0)
    {
        fSecMsgEnabled = false;
        return error("%s: Could not load bucket sets, secure messaging disabled.", __func__);
    };

    threadGroupSmsg.create_thread(boost::bind(&TraceThread<void (*)()>, "smsg", &ThreadSecureMsg));
    threadGroupSmsg.create_thread(boost::bind(&TraceThread<void (*)()>, "smsg-pow", &ThreadSecureMsgPow));

    return true;
};

bool SecureMsgShutdown()
{
    if (!fSecMsgEnabled)
        return false;

    LogPrintf("Stopping secure messaging.\n");

    if (SecureMsgWriteIni() != 0)
        LogPrintf("Failed to save smsg.ini\n");

    fSecMsgEnabled = false;
    g_connman->SetLocalServices(ServiceFlags(g_connman->GetLocalServices() & ~NODE_SMSG));

    threadGroupSmsg.interrupt_all();
    threadGroupSmsg.join_all();

    if (smsgDB)
    {
        LOCK(cs_smsgDB);
        delete smsgDB;
        smsgDB = nullptr;
    };

    if (secp256k1_context_smsg)
        secp256k1_context_destroy(secp256k1_context_smsg);
    secp256k1_context_smsg = nullptr;

    pwalletSmsg = nullptr;
    return true;
};

bool SecureMsgEnable(CWallet *pwallet)
{
    // Start secure messaging at runtime
    if (fSecMsgEnabled)
    {
        LogPrintf("SecureMsgEnable: secure messaging is already enabled.\n");
        return false;
    };

    {
        LOCK(cs_smsg);

        smsgAddresses.clear(); // should be empty already
        smsgBuckets.clear(); // should be empty already
        
        if (!SecureMsgStart(pwallet, false, false))
            return error("%s: SecureMsgStart failed.\n", __func__);
    } // cs_smsg

    // Ping each peer advertising smsg
    {
        LOCK(g_connman->cs_vNodes);
        for(auto *pnode : g_connman->vNodes)
        {
            if (!(pnode->GetLocalServices() & NODE_SMSG))
                continue;
            g_connman->PushMessage(pnode,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgPing"));
            g_connman->PushMessage(pnode,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgPong")); // Send pong as have missed initial ping sent by peer when it connected
        };
    } // g_connman->cs_vNodes

    LogPrintf("Secure messaging enabled.\n");
    return true;
};

bool SecureMsgDisable()
{
    // Stop secure messaging at runtime
    if (!fSecMsgEnabled)
    {
        LogPrintf("SecureMsgDisable: secure messaging is already disabled.\n");
        return false;
    };

    {
        LOCK(cs_smsg);

        if (!SecureMsgShutdown())
            return error("%s: SecureMsgShutdown failed.\n", __func__);

        // Clear smsgBuckets
        std::map<int64_t, SecMsgBucket>::iterator it;
        it = smsgBuckets.begin();
        for (it = smsgBuckets.begin(); it != smsgBuckets.end(); ++it)
            it->second.setTokens.clear();
        smsgBuckets.clear();
        smsgAddresses.clear();
    } // cs_smsg

    // Tell each smsg enabled peer that this node is disabling
    {
        LOCK(g_connman->cs_vNodes);
        for (auto *pnode : g_connman->vNodes)
        {
            if (!pnode->smsgData.fEnabled)
                continue;
            LOCK(pnode->smsgData.cs_smsg_net);
            g_connman->PushMessage(pnode,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgDisabled"));
            pnode->smsgData.fEnabled = false;
        };
    } // g_connman->cs_vNodes

    LogPrintf("Secure messaging disabled.\n");
    return true;
};


int SecureMsgReceiveData(CNode *pfrom, const std::string &strCommand, CDataStream &vRecv)
{
    /*
        Called from ProcessMessage
        Runs in ThreadMessageHandler2
    */

    /*
        return values:
            0: no error
            1: error
            2: unknown message
        
        TODO:
        Explain better and make use of better terminology such as
        Node A <-> Node B <-> Node C

        Commands
        + smsgInv =
            (1) received inventory of other node.
                (1.1) sanity checks
            (2) loop through buckets
                (2.1) sanity checks
                (2.2) check if bucket is locked to node C, if so continue but don't match. TODO: handle this properly, add critical section, lock on write. On read: nothing changes = no lock
                    (2.2.3) If our bucket is not locked to another node then add hash to buffer to be requested..
            (3) send smsgShow with list of hashes to request.

        + smsgShow =
            (1) received a list of requested bucket hashes which the other party does not have.
            (2) respond with smsgHave - contains all the message hashes within the requested buckets. 
        + smsgHave =
            (1) A list of all the message hashes which a node has in response to smsgShow.
        + smsgWant =
            (1) A list of the message hashes that a node does not have and wants to retrieve from the node who sent smsgHave
        + smsgMsg = 
            (1) In response to 
        + smsgPing = ping request
        + smsgPong = pong response
        + smsgMatch =
            Obsolete, it used tell a node up to which time their messages were synced in response to smsg, but this is overhead because we know exactly when we sent them
    */

    LogPrint(BCLog::SMSG, "SecureMsgReceiveData() %s %s.\n", pfrom->GetAddrName(), strCommand);

    if (!fSecMsgEnabled)
    {
        if (strCommand == "smsgPing") // ignore smsgPing
            return 0;
        return 2;
    };

    if (strCommand == "smsgInv")
    {
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        if (vchData.size() < 4)
        {
            Misbehaving(pfrom->GetId(), 1);
            return 1; // not enough data received to be a valid smsgInv
        };

        int64_t now = GetTime();

        {
            LOCK(pfrom->smsgData.cs_smsg_net);

            if (now < pfrom->smsgData.ignoreUntil)
            {
                LogPrint(BCLog::SMSG, "Node is ignoring peer %d until %d.\n", pfrom->GetId(), pfrom->smsgData.ignoreUntil);
                return 1;
            };
        }

        uint32_t nBuckets       = smsgBuckets.size();
        uint32_t nLocked        = 0;    // no. of locked buckets on this node
        uint32_t nInvBuckets;           // no. of bucket headers sent by peer in smsgInv
        memcpy(&nInvBuckets, &vchData[0], 4);
        LogPrint(BCLog::SMSG, "Remote node sent %d bucket headers, this has %d.\n", nInvBuckets, nBuckets);


        // Check no of buckets:
        if (nInvBuckets > (SMSG_RETENTION / SMSG_BUCKET_LEN) + 1) // +1 for some leeway
        {
            LogPrintf("Peer sent more bucket headers than possible %u, %u.\n", nInvBuckets, (SMSG_RETENTION / SMSG_BUCKET_LEN));
            Misbehaving(pfrom->GetId(), 1);
            return 1;
        };

        if (vchData.size() < 4 + nInvBuckets*16)
        {
            LogPrintf("Remote node did not send enough data.\n");
            Misbehaving(pfrom->GetId(), 1);
            return 1;
        };

        std::vector<uint8_t> vchDataOut;
        vchDataOut.reserve(4 + 8 * nInvBuckets); // reserve max possible size
        vchDataOut.resize(4);
        uint32_t nShowBuckets = 0;

        uint8_t *p = &vchData[4];
        for (uint32_t i = 0; i < nInvBuckets; ++i)
        {
            int64_t time;
            uint32_t ncontent, hash;
            memcpy(&time, p, 8);
            memcpy(&ncontent, p+8, 4);
            memcpy(&hash, p+12, 4);

            p += 16;

            // Check time valid:
            if (time < now - SMSG_RETENTION)
            {
                LogPrint(BCLog::SMSG, "Not interested in peer bucket %d, has expired.\n", time);

                if (time < now - SMSG_RETENTION - SMSG_TIME_LEEWAY)
                    Misbehaving(pfrom->GetId(), 1);
                continue;
            };
            if (time > now + SMSG_TIME_LEEWAY)
            {
                LogPrint(BCLog::SMSG, "Not interested in peer bucket %d, in the future.\n", time);
                Misbehaving(pfrom->GetId(), 1);
                continue;
            };

            if (ncontent < 1)
            {
                LogPrint(BCLog::SMSG, "Peer sent empty bucket, ignore %d %u %u.\n", time, ncontent, hash);
                continue;
            };

            if (LogAcceptCategory(BCLog::SMSG))
            {
                LogPrintf("peer bucket %d %u %u.\n", time, ncontent, hash);
                LogPrintf("this bucket %d %u %u.\n", time, smsgBuckets[time].setTokens.size(), smsgBuckets[time].hash);
            };
            {
                LOCK(cs_smsg);
                if (smsgBuckets[time].nLockCount > 0)
                {
                    LogPrint(BCLog::SMSG, "Bucket is locked %u, waiting for peer %u to send data.\n", smsgBuckets[time].nLockCount, smsgBuckets[time].nLockPeerId);
                    nLocked++;
                    continue;
                };

                // If this node has more than the peer node, peer node will pull from this
                //  if then peer node has more this node will pull fom peer
                if (smsgBuckets[time].setTokens.size() < ncontent
                    || (smsgBuckets[time].setTokens.size() == ncontent
                        && smsgBuckets[time].hash != hash)) // if same amount in buckets check hash
                {
                    LogPrint(BCLog::SMSG, "Requesting contents of bucket %d.\n", time);

                    uint32_t sz = vchDataOut.size();
                    vchDataOut.resize(sz + 8);
                    memcpy(&vchDataOut[sz], &time, 8);

                    nShowBuckets++;
                };
            } // cs_smsg
        };

        // TODO: should include hash?
        memcpy(&vchDataOut[0], &nShowBuckets, 4);
        if (vchDataOut.size() > 4)
        {
            g_connman->PushMessage(pfrom,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgShow", vchDataOut));
        } else
        if (nLocked < 1) // Don't report buckets as matched if any are locked
        {
            // Peer has no buckets we want, don't send them again until something changes
            //  peer will still request buckets from this node if needed (< ncontent)
            vchDataOut.resize(8);
            memcpy(&vchDataOut[0], &now, 8);
            g_connman->PushMessage(pfrom,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgMatch", vchDataOut));
            LogPrint(BCLog::SMSG, "Sending smsgMatch, no locked buckets, time= %d.\n", now);
        } else
        if (nLocked >= 1)
        {
            LogPrint(BCLog::SMSG, "%u buckets were locked, time= %d.\n", nLocked, now);
        };

    } else
    if (strCommand == "smsgShow")
    {
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        if (vchData.size() < 4)
            return 1;

        uint32_t nBuckets;
        memcpy(&nBuckets, &vchData[0], 4);

        if (vchData.size() < 4 + nBuckets * 8)
            return 1;

        LogPrint(BCLog::SMSG, "smsgShow: peer wants to see content of %u buckets.\n", nBuckets);

        std::map<int64_t, SecMsgBucket>::iterator itb;
        std::set<SecMsgToken>::iterator it;

        std::vector<uint8_t> vchDataOut;
        int64_t time;
        uint8_t* pIn = &vchData[4];
        for (uint32_t i = 0; i < nBuckets; ++i, pIn += 8)
        {
            memcpy(&time, pIn, 8);

            {
                LOCK(cs_smsg);
                itb = smsgBuckets.find(time);
                if (itb == smsgBuckets.end())
                {
                    LogPrint(BCLog::SMSG, "Don't have bucket %d.\n", time);
                    continue;
                };

                std::set<SecMsgToken>& tokenSet = (*itb).second.setTokens;

                try { vchDataOut.resize(8 + 16 * tokenSet.size());
                } catch (std::exception &e) {
                    LogPrintf("vchDataOut.resize %u threw: %s.\n", 8 + 16 * tokenSet.size(), e.what());
                    continue;
                };
                memcpy(&vchDataOut[0], &time, 8);

                uint8_t *p = &vchDataOut[8];
                for (it = tokenSet.begin(); it != tokenSet.end(); ++it)
                {
                    memcpy(p, &it->timestamp, 8);
                    memcpy(p+8, &it->sample, 8);

                    p += 16;
                };
            }
            g_connman->PushMessage(pfrom,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgHave", vchDataOut));
        };
    } else
    if (strCommand == "smsgHave")
    {
        // Peer has these messages in bucket
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        if (vchData.size() < 8)
            return 1;

        int n = (vchData.size() - 8) / 16;

        int64_t time;
        memcpy(&time, &vchData[0], 8);

        // Check time valid:
        int64_t now = GetTime();
        if (time < now - SMSG_RETENTION)
        {
            LogPrint(BCLog::SMSG, "Not interested in peer bucket %d, has expired.\n", time);
            return 1;
        };
        if (time > now + SMSG_TIME_LEEWAY)
        {
            LogPrint(BCLog::SMSG, "Not interested in peer bucket %d, in the future.\n", time);
            Misbehaving(pfrom->GetId(), 1);
            return 1;
        };

        std::vector<uint8_t> vchDataOut;

        {
            LOCK(cs_smsg);
            if (smsgBuckets[time].nLockCount > 0)
            {
                LogPrint(BCLog::SMSG, "Bucket %d lock count %u, waiting for message data from peer %u.\n", time, smsgBuckets[time].nLockCount, smsgBuckets[time].nLockPeerId);
                return 1;
            };

            LogPrint(BCLog::SMSG, "Sifting through bucket %d.\n", time);

            vchDataOut.resize(8);
            memcpy(&vchDataOut[0], &vchData[0], 8);

            std::set<SecMsgToken>& tokenSet = smsgBuckets[time].setTokens;
            std::set<SecMsgToken>::iterator it;
            SecMsgToken token;
            uint8_t *p = &vchData[8];

            for (int i = 0; i < n; ++i)
            {
                memcpy(&token.timestamp, p, 8);
                memcpy(&token.sample, p+8, 8);

                it = tokenSet.find(token);
                if (it == tokenSet.end())
                {
                    int nd = vchDataOut.size();
                    try {
                        vchDataOut.resize(nd + 16);
                    } catch (std::exception& e) {
                        LogPrintf("vchDataOut.resize %d threw: %s.\n", nd + 16, e.what());
                        continue;
                    };

                    memcpy(&vchDataOut[nd], p, 16);
                };

                p += 16;
            };
        } // cs_smsg

        if (vchDataOut.size() > 8)
        {
            if (LogAcceptCategory(BCLog::SMSG))
            {
                LogPrintf("Asking peer for %u messages.\n", (vchDataOut.size() - 8) / 16);
                LogPrintf("Locking bucket %u for peer %d.\n", time, pfrom->GetId());
            };
            {
                LOCK(cs_smsg);
                smsgBuckets[time].nLockCount   = 3; // lock this bucket for at most 3 * SMSG_THREAD_DELAY seconds, unset when peer sends smsgMsg
                smsgBuckets[time].nLockPeerId  = pfrom->GetId();
            }
            g_connman->PushMessage(pfrom,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgWant", vchDataOut));
        };
    } else
    if (strCommand == "smsgWant")
    {
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        if (vchData.size() < 8)
            return 1;

        std::vector<uint8_t> vchOne;
        std::vector<uint8_t> vchBunch;

        vchBunch.resize(4+8); // nmessages + bucketTime

        int n = (vchData.size() - 8) / 16;

        int64_t time;
        uint32_t nBunch = 0;
        memcpy(&time, &vchData[0], 8);


        std::map<int64_t, SecMsgBucket>::iterator itb;

        {
            LOCK(cs_smsg);
            itb = smsgBuckets.find(time);
            if (itb == smsgBuckets.end())
            {
                LogPrint(BCLog::SMSG, "Don't have bucket %d.\n", time);
                return 1;
            };

            std::set<SecMsgToken>& tokenSet = itb->second.setTokens;
            std::set<SecMsgToken>::iterator it;
            SecMsgToken token;
            uint8_t *p = &vchData[8];
            for (int i = 0; i < n; ++i)
            {
                memcpy(&token.timestamp, p, 8);
                memcpy(&token.sample, p+8, 8);

                it = tokenSet.find(token);
                if (it == tokenSet.end())
                {
                    LogPrint(BCLog::SMSG, "Don't have wanted message %d.\n", token.timestamp);
                } else
                {
                    //LogPrintf("Have message at %d.\n", it->offset); // DEBUG
                    token.offset = it->offset;

                    // Place in vchOne so if SecureMsgRetrieve fails it won't corrupt vchBunch
                    if (SecureMsgRetrieve(token, vchOne) == 0)
                    {
                        nBunch++;
                        vchBunch.insert(vchBunch.end(), vchOne.begin(), vchOne.end()); // append
                    } else
                    {
                        LogPrintf("SecureMsgRetrieve failed %d.\n", token.timestamp);
                    };

                    if (nBunch >= 500
                        || vchBunch.size() >= 96000)
                    {
                        LogPrint(BCLog::SMSG, "Break bunch %u, %u.\n", nBunch, vchBunch.size());
                        break; // end here, peer will send more want messages if needed.
                    };
                };
                p += 16;
            };
        } // cs_smsg

        if (nBunch > 0)
        {
            LogPrint(BCLog::SMSG, "Sending block of %u messages for bucket %d.\n", nBunch, time);

            memcpy(&vchBunch[0], &nBunch, 4);
            memcpy(&vchBunch[4], &time, 8);
            g_connman->PushMessage(pfrom,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgMsg", vchBunch));
        };
    } else
    if (strCommand == "smsgMsg")
    {
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        LogPrint(BCLog::SMSG, "smsgMsg vchData.size() %u.\n", vchData.size());

        SecureMsgReceive(pfrom, vchData);
    } else
    if (strCommand == "smsgMatch")
    {
        /*
        Basically all this code has to go..
        For now we can use it to punish nodes running the older version, not that it's really need because the overhead is small.
        TODO: remove this code.
        */
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        if (vchData.size() < 8)
        {
            LogPrintf("smsgMatch, not enough data %u.\n", vchData.size());
            Misbehaving(pfrom->GetId(), 1);
            return 1;
        };

        int64_t time;
        memcpy(&time, &vchData[0], 8);

        int64_t now = GetTime();
        if (time > now + SMSG_TIME_LEEWAY)
        {
            LogPrintf("Warning: Peer buckets matched in the future: %d.\nEither this node or the peer node has the incorrect time set.\n", time);
            LogPrint(BCLog::SMSG, "Peer match time set to now.\n");
            time = now;
        };
        /*
        {
            LOCK(pfrom->smsgData.cs_smsg_net);
            pfrom->smsgData.lastMatched = time;
        }*/
        LogPrint(BCLog::SMSG, "Peer buckets matched in smsgWant at %d.\n", time);
    } else
    if (strCommand == "smsgPing")
    {
        // smsgPing is the initial message, send reply
        g_connman->PushMessage(pfrom,
            CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgPong"));
    } else
    if (strCommand == "smsgPong")
    {
        LogPrint(BCLog::SMSG, "Peer replied, secure messaging enabled.\n");

        {
            LOCK(pfrom->smsgData.cs_smsg_net);
            pfrom->smsgData.fEnabled = true;
        }

    } else
    if (strCommand == "smsgDisabled")
    {
        // Peer has disabled secure messaging.

        {
            LOCK(pfrom->smsgData.cs_smsg_net);
            pfrom->smsgData.fEnabled = false;
        }

        LogPrint(BCLog::SMSG, "Peer %d has disabled secure messaging.\n", pfrom->GetId());
    } else
    if (strCommand == "smsgIgnore")
    {
        // Peer is reporting that it will ignore this node until time.
        //  Ignore peer too
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        if (vchData.size() < 8)
        {
            LogPrintf("smsgIgnore, not enough data %u.\n", vchData.size());
            Misbehaving(pfrom->GetId(), 1);
            return 1;
        };

        int64_t time;
        memcpy(&time, &vchData[0], 8);

        {
            LOCK(pfrom->smsgData.cs_smsg_net);
            pfrom->smsgData.ignoreUntil = time;
        }

        LogPrint(BCLog::SMSG, "Peer %d is ignoring this node until %d, ignore peer too.\n", pfrom->GetId(), time);
    } else
    {
        return 2; // Unknown message
    };

    return 0;
};

bool SecureMsgSendData(CNode *pto, bool fSendTrickle)
{
    /*
        Called from ProcessMessage
        Runs in ThreadMessageHandler2
    */

    LOCK(pto->smsgData.cs_smsg_net);

    //LogPrintf("SecureMsgSendData() %s.\n", pto->GetAddrName());

    int64_t now = GetTime();

    if (pto->smsgData.lastSeen == 0)
    {
        // First contact
        LogPrint(BCLog::SMSG, "SecureMsgSendData() new node %s, peer id %u.\n", pto->GetAddrName(), pto->GetId());
        // Send smsgPing once, do nothing until receive 1st smsgPong (then set fEnabled)
        g_connman->PushMessage(pto,
            CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgPing"));
        pto->smsgData.lastSeen = GetTime();
        return true;
    } else
    if (!pto->smsgData.fEnabled
        || now - pto->smsgData.lastSeen < SMSG_SEND_DELAY
        || now < pto->smsgData.ignoreUntil)
    {
        return true;
    };

    {
        LOCK(cs_smsg);
        std::map<int64_t, SecMsgBucket>::iterator it;

        uint32_t nBuckets = smsgBuckets.size();
        if (nBuckets > 0) // no need to send keep alive pkts, coin messages already do that
        {
            std::vector<uint8_t> vchData;
            // should reserve?
            vchData.reserve(4 + nBuckets*16); // timestamp + size + hash

            uint32_t nBucketsShown = 0;
            vchData.resize(4);

            /*
            Get time before loop and after looping through messages set nLastMatched to time before loop.
            This prevents scenario where:
                Loop()
                    message = locked and  thus skipped
                   message become free and nTimeChanged is updated
                End loop

                nLastMatched = GetTime()
                => bucket that became free in loop is now skipped :/

            Scenario 2:
                Same as one but time is updated before

                    bucket nTimeChanged is updated but not unlocked yet
                    now = GetTime()
                    Loop of buckets skips message

                But this is nanoseconds, very unlikely.

             */

            for (it = smsgBuckets.begin(); it != smsgBuckets.end(); ++it)
            {
                SecMsgBucket &bkt = it->second;

                uint32_t nMessages = bkt.setTokens.size();

                if (bkt.timeChanged < pto->smsgData.lastMatched     // peer was last sent all buckets at time of lastMatched. It should have this bucket
                    || nMessages < 1)                               // this bucket is empty
                    continue;

                uint32_t hash = bkt.hash;

                if (LogAcceptCategory(BCLog::SMSG))
                    LogPrintf("Preparing bucket with hash %d for transfer to node %u. timeChanged=%d > lastMatched=%d\n", hash, pto->GetId(), bkt.timeChanged, pto->smsgData.lastMatched);
                
                size_t sz = vchData.size();
                try { vchData.resize(vchData.size() + 16); } catch (std::exception& e)
                {
                    LogPrintf("vchData.resize %u threw: %s.\n", vchData.size() + 16, e.what());
                    continue;
                };
                
                uint8_t *p = &vchData[sz];
                memcpy(p, &it->first, 8);
                memcpy(p+8, &nMessages, 4);
                memcpy(p+12, &hash, 4);

                nBucketsShown++;
                //if (fDebug)
                //    LogPrintf("Sending bucket %d, size %d \n", it->first, it->second.size());
            };

            if (vchData.size() > 4)
            {
                memcpy(&vchData[0], &nBucketsShown, 4);
                LogPrint(BCLog::SMSG, "Sending %d bucket headers.\n", nBucketsShown);

                g_connman->PushMessage(pto,
                    CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgInv", vchData));
            };
        };
    } // cs_smsg

    pto->smsgData.lastSeen = now;
    pto->smsgData.lastMatched = now; //bug fix smsg 3

    return true;
};


static int SecureMsgInsertAddress(CKeyID &hashKey, CPubKey &pubKey, SecMsgDB &addrpkdb)
{
    /* Insert key hash and public key to addressdb.

        (+) Called when receiving a message, it will automatically add the public key of the sender to our database so we can reply.

        should have LOCK(cs_smsg) where db is opened

        returns
            0 success
            1 error
            4 address is already in db
    */

    if (addrpkdb.ExistsPK(hashKey))
    {
        //LogPrintf("DB already contains public key for address.\n");
        CPubKey cpkCheck;
        if (!addrpkdb.ReadPK(hashKey, cpkCheck))
        {
            LogPrintf("addrpkdb.Read failed.\n");
        } else
        {
            if (cpkCheck != pubKey)
                LogPrintf("DB already contains existing public key that does not match .\n");
        };
        return 4;
    };

    if (!addrpkdb.WritePK(hashKey, pubKey))
        return errorN(1, "%s: Write pair failed.", __func__);

    return 0;
};

int SecureMsgInsertAddress(CKeyID &hashKey, CPubKey &pubKey)
{
    LOCK(cs_smsgDB);
    SecMsgDB addrpkdb;

    if (!addrpkdb.Open("cr+"))
        return 1;

    return SecureMsgInsertAddress(hashKey, pubKey, addrpkdb);
};

static bool ScanBlock(const CBlock &block, SecMsgDB &addrpkdb,
    uint32_t &nTransactions, uint32_t &nElements, uint32_t &nPubkeys, uint32_t &nDuplicates)
{
    AssertLockHeld(cs_smsgDB);

    std::string reason;

    // Only scan inputs of standard txns and coinstakes
    for (const auto &tx : block.vtx)
    {
        // Harvest public keys from coinstake txns
        
        if (!tx->IsParticlVersion()) // skip legacy txns
            continue;
        
        for (const auto &txin : tx->vin)
        {
            if (txin.IsAnonInput())
                continue;
            
            if (txin.scriptWitness.stack.size() != 2)
                continue;
            
            if (txin.scriptWitness.stack[1].size() != 33)
                continue;
            
            CPubKey pubKey(txin.scriptWitness.stack[1]);
            
            if (!pubKey.IsValid()
                || !pubKey.IsCompressed())
            {
                LogPrintf("Public key is invalid %s.\n", HexStr(pubKey));
                continue;
            };

            CKeyID addrKey = pubKey.GetID();
            switch (SecureMsgInsertAddress(addrKey, pubKey, addrpkdb))
            {
                case 0: nPubkeys++; break;      // added key
                case 4: nDuplicates++; break;   // duplicate key
            };
            
            if (tx->IsCoinStake()) // coinstake inputs are always from the same address/pubkey
                break;
        };

        nTransactions++;

        if (nTransactions % 10000 == 0) // for ScanChainForPublicKeys
        {
            LogPrintf("Scanning transaction no. %u.\n", nTransactions);
        };
    };
    return true;
};


bool SecureMsgScanBlock(const CBlock &block)
{
    // - scan block for public key addresses

    if (!smsgOptions.fScanIncoming)
        return true;

    LogPrint(BCLog::SMSG, "%s.\n", __func__);

    uint32_t nTransactions  = 0;
    uint32_t nElements      = 0;
    uint32_t nPubkeys       = 0;
    uint32_t nDuplicates    = 0;

    {
        LOCK(cs_smsgDB);

        SecMsgDB addrpkdb;
        if (!addrpkdb.Open("cw")
            || !addrpkdb.TxnBegin())
            return false;

        ScanBlock(block, addrpkdb,
            nTransactions, nElements, nPubkeys, nDuplicates);

        addrpkdb.TxnCommit();
    } // cs_smsgDB

    LogPrint(BCLog::SMSG, "Found %u transactions, %u elements, %u new public keys, %u duplicates.\n", nTransactions, nElements, nPubkeys, nDuplicates);

    return true;
};

bool ScanChainForPublicKeys(CBlockIndex *pindexStart)
{
    LogPrintf("Scanning block chain for public keys.\n");
    int64_t nStart = GetTimeMillis();

    LogPrint(BCLog::SMSG, "From height %u.\n", pindexStart->nHeight);

    // Public keys are in txin.scriptSig
    //  matching addresses are in scriptPubKey of txin's referenced output

    uint32_t nBlocks        = 0;
    uint32_t nTransactions  = 0;
    uint32_t nInputs        = 0;
    uint32_t nPubkeys       = 0;
    uint32_t nDuplicates    = 0;

    {
        LOCK(cs_smsgDB);

        SecMsgDB addrpkdb;
        if (!addrpkdb.Open("cw")
            || !addrpkdb.TxnBegin())
            return false;

        CBlockIndex *pindex = pindexStart;
        while (pindex)
        {
            nBlocks++;
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus()))
                LogPrintf("%s: ReadBlockFromDisk failed.\n", __func__);
            else
                ScanBlock(block, addrpkdb,
                    nTransactions, nInputs, nPubkeys, nDuplicates);

            pindex = chainActive.Next(pindex);
        };

        addrpkdb.TxnCommit();
    } // cs_smsgDB

    LogPrintf("Scanned %u blocks, %u transactions, %u inputs\n", nBlocks, nTransactions, nInputs);
    LogPrintf("Found %u public keys, %u duplicates.\n", nPubkeys, nDuplicates);
    LogPrintf("Took %d ms\n", GetTimeMillis() - nStart);

    return true;
};

bool SecureMsgScanBlockChain()
{
    TRY_LOCK(cs_main, lockMain);
    if (lockMain)
    {
        CBlockIndex *pindexScan = chainActive.Genesis();
        if (pindexScan == nullptr)
        {
            LogPrintf("Error: pindexGenesisBlock not set.\n");
            return false;
        };

        try { // In try to catch errors opening db,
            if (!ScanChainForPublicKeys(pindexScan))
                return false;
        } catch (std::exception &e)
        {
            LogPrintf("ScanChainForPublicKeys() threw: %s.\n", e.what());
            return false;
        };
    } else
    {
        LogPrintf("ScanChainForPublicKeys() Could not lock main.\n");
        return false;
    };

    return true;
};

bool SecureMsgScanBuckets()
{
    LogPrint(BCLog::SMSG, "SecureMsgScanBuckets()\n");

#ifdef ENABLE_WALLET
    if (!fSecMsgEnabled
        || !pwalletSmsg
        || pwalletSmsg->IsLocked())
        return false;

    int64_t  mStart         = GetTimeMillis();
    int64_t  now            = GetTime();
    uint32_t nFiles         = 0;
    uint32_t nMessages      = 0;
    uint32_t nFoundMessages = 0;

    fs::path pathSmsgDir = GetDataDir() / "smsgstore";
    fs::directory_iterator itend;

    if (!fs::exists(pathSmsgDir)
        || !fs::is_directory(pathSmsgDir))
    {
        LogPrintf("Message store directory does not exist.\n");
        return 0; // not an error
    };

    SecureMessage smsg;
    std::vector<uint8_t> vchData;

    for (fs::directory_iterator itd(pathSmsgDir); itd != itend; ++itd)
    {
        if (!fs::is_regular_file(itd->status()))
            continue;

        std::string fileType = (*itd).path().extension().string();

        if (fileType.compare(".dat") != 0)
            continue;

        std::string fileName = (*itd).path().filename().string();


        LogPrint(BCLog::SMSG, "Processing file: %s.\n", fileName);

        nFiles++;

        // TODO files must be split if > 2GB
        // time_noFile.dat
        size_t sep = fileName.find_first_of("_");
        if (sep == std::string::npos)
            continue;

        std::string stime = fileName.substr(0, sep);
        int64_t fileTime;
        if (!ParseInt64(stime, &fileTime))
        {
            LogPrintf("%s: ParseInt64 failed %s.\n", __func__, stime);
            continue;
        };

        if (fileTime < now - SMSG_RETENTION)
        {
            LogPrintf("Dropping file %s, expired.\n", fileName);
            try {
                fs::remove((*itd).path());
            } catch (const fs::filesystem_error &ex)
            {
                LogPrintf("Error removing bucket file %s, %s.\n", fileName, ex.what());
            };
            continue;
        };

        if (boost::algorithm::ends_with(fileName, "_wl.dat"))
        {
            LogPrint(BCLog::SMSG, "Skipping wallet locked file: %s.\n", fileName);
            continue;
        };

        {
            LOCK(cs_smsg);
            FILE *fp;
            errno = 0;
            if (!(fp = fopen((*itd).path().string().c_str(), "rb")))
            {
                LogPrintf("Error opening file: %s\n", strerror(errno));
                continue;
            };

            for (;;)
            {
                errno = 0;
                if (fread(&smsg.hash[0], sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN)
                {
                    if (errno != 0)
                    {
                        LogPrintf("fread header failed: %s\n", strerror(errno));
                    } else
                    {
                        //LogPrintf("End of file.\n");
                    };
                    break;
                };

                try { vchData.resize(smsg.nPayload); } catch (std::exception &e)
                {
                    LogPrintf("SecureMsgWalletUnlocked(): Could not resize vchData, %u, %s\n", smsg.nPayload, e.what());
                    fclose(fp);
                    return 1;
                };

                if (fread(&vchData[0], sizeof(uint8_t), smsg.nPayload, fp) != smsg.nPayload)
                {
                    LogPrintf("fread data failed: %s\n", strerror(errno));
                    break;
                };

                // Don't report to gui,
                int rv = SecureMsgScanMessage(&smsg.hash[0], &vchData[0], smsg.nPayload, false);

                if (rv == 0)
                {
                    nFoundMessages++;
                } else
                if (rv != 0)
                {
                    // SecureMsgScanMessage failed
                };

                nMessages++;
            };

            fclose(fp);

            // Remove wl file when scanned
            try {
                fs::remove((*itd).path());
            } catch (const boost::filesystem::filesystem_error &ex)
            {
                LogPrintf("Error removing wl file %s - %s\n", fileName, ex.what());
                return 1;
            };
        } // cs_smsg
    };

    LogPrintf("Processed %u files, scanned %u messages, received %u messages.\n", nFiles, nMessages, nFoundMessages);
    LogPrintf("Took %d ms\n", GetTimeMillis() - mStart);
#endif
    return true;
}


int SecureMsgWalletUnlocked()
{
#ifdef ENABLE_WALLET
    /*
    When the wallet is unlocked, scan messages received while wallet was locked.
    */
    if (!fSecMsgEnabled || !pwalletSmsg)
        return 0;

    LogPrintf("SecureMsgWalletUnlocked()\n");

    if (pwalletSmsg->IsLocked())
    {
        LogPrintf("Error: Wallet is locked.\n");
        return 1;
    };

    int64_t  now            = GetTime();
    uint32_t nFiles         = 0;
    uint32_t nMessages      = 0;
    uint32_t nFoundMessages = 0;

    fs::path pathSmsgDir = GetDataDir() / "smsgstore";
    fs::directory_iterator itend;

    if (!fs::exists(pathSmsgDir)
        || !fs::is_directory(pathSmsgDir))
    {
        LogPrintf("Message store directory does not exist.\n");
        return 0; // not an error
    };

    SecureMessage smsg;
    std::vector<uint8_t> vchData;

    for (fs::directory_iterator itd(pathSmsgDir); itd != itend; ++itd)
    {
        if (!fs::is_regular_file(itd->status()))
            continue;

        std::string fileName = (*itd).path().filename().string();

        if (!boost::algorithm::ends_with(fileName, "_wl.dat"))
            continue;

        LogPrint(BCLog::SMSG, "Processing file: %s.\n", fileName);

        nFiles++;

        // TODO files must be split if > 2GB
        // time_noFile_wl.dat
        size_t sep = fileName.find_first_of("_");
        if (sep == std::string::npos)
            continue;

        std::string stime = fileName.substr(0, sep);
        int64_t fileTime;
        if (!ParseInt64(stime, &fileTime))
        {
            LogPrintf("%s: ParseInt64 failed %s.\n", __func__, stime);
            continue;
        };

        if (fileTime < now - SMSG_RETENTION)
        {
            LogPrintf("Dropping wallet locked file %s, expired.\n", fileName);
            try {
                fs::remove((*itd).path());
            } catch (const fs::filesystem_error &ex)
            {
                LogPrintf("Error removing wl file %s - %s\n", fileName, ex.what());
                return 1;
            };
            continue;
        };

        {
            LOCK(cs_smsg);
            FILE *fp;
            errno = 0;
            if (!(fp = fopen((*itd).path().string().c_str(), "rb")))
            {
                LogPrintf("Error opening file: %s\n", strerror(errno));
                continue;
            };

            for (;;)
            {
                errno = 0;
                if (fread(&smsg.hash[0], sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN)
                {
                    if (errno != 0)
                    {
                        LogPrintf("fread header failed: %s\n", strerror(errno));
                    } else
                    {
                        //LogPrintf("End of file.\n");
                    };
                    break;
                };

                try { vchData.resize(smsg.nPayload); } catch (std::exception &e)
                {
                    LogPrintf("%s: Could not resize vchData, %u, %s\n", __func__, smsg.nPayload, e.what());
                    fclose(fp);
                    return 1;
                };

                if (fread(&vchData[0], sizeof(uint8_t), smsg.nPayload, fp) != smsg.nPayload)
                {
                    LogPrintf("fread data failed: %s\n", strerror(errno));
                    break;
                };

                // Don't report to gui,
                int rv = SecureMsgScanMessage(&smsg.hash[0], &vchData[0], smsg.nPayload, false);

                if (rv == 0)
                {
                    nFoundMessages++;
                } else
                if (rv != 0)
                {
                    // SecureMsgScanMessage failed
                };

                nMessages++;
            };

            fclose(fp);

            // Remove wl file when scanned
            try {
                fs::remove((*itd).path());
            } catch (const fs::filesystem_error &ex)
            {
                LogPrintf("Error removing wl file %s - %s\n", fileName, ex.what());
                return 1;
            };
        } // cs_smsg
    };

    LogPrintf("Processed %u files, scanned %u messages, received %u messages.\n", nFiles, nMessages, nFoundMessages);

    // Notify gui
    NotifySecMsgWalletUnlocked();
#endif
    return 0;
};


int SecureMsgWalletKeyChanged(CKeyID &keyId, const std::string &sLabel, ChangeType mode)
{
    /*
    SecureMsgWalletKeyChanged():
    When a key changes in the wallet, this function should be called to update the smsgAddresses vector.

    mode:
        CT_NEW : a new key was added
        CT_DELETED : delete an existing key from vector.
    */

    if (!fSecMsgEnabled)
        return 0;

    LogPrintf("SecureMsgWalletKeyChanged()\n");

    // TODO: default recv and recvAnon

    {
        LOCK(cs_smsg);

        std::vector<SecMsgAddress>::iterator itFound = smsgAddresses.end();
        for (std::vector<SecMsgAddress>::iterator it = smsgAddresses.begin(); it != smsgAddresses.end(); ++it)
        {
            if (keyId != it->address)
                continue;
            itFound = it;
            break;
        };

        switch(mode)
        {
            case CT_NEW:
                if (itFound == smsgAddresses.end())
                    smsgAddresses.push_back(SecMsgAddress(keyId, smsgOptions.fNewAddressRecv, smsgOptions.fNewAddressAnon));
                else
                    LogPrint(BCLog::SMSG, "%s: Already have address: %s.\n", __func__, CBitcoinAddress(keyId).ToString());
                break;
            case CT_DELETED:
                if (itFound != smsgAddresses.end())
                {
                    smsgAddresses.erase(itFound);
                };
                break;
            default:
                break;
        };
    } // cs_smsg


    return 0;
};

int SecureMsgScanMessage(uint8_t *pHeader, uint8_t *pPayload, uint32_t nPayload, bool reportToGui)
{
#ifdef ENABLE_WALLET
    /*
    Check if message belongs to this node.
    If so add to inbox db.

    if !reportToGui don't fire NotifySecMsgInboxChanged
     - loads messages received when wallet locked in bulk.

    returns
        0 success,
        1 error
        2 no match
        3 wallet is locked - message stored for scanning later.
    */

    LogPrint(BCLog::SMSG, "SecureMsgScanMessage()\n");

    if (!pwalletSmsg)
    {
        LogPrint(BCLog::SMSG, "%s: Wallet is not set.\n", __func__);
        return 0;
    };

    if (pwalletSmsg->IsLocked())
    {
        LogPrint(BCLog::SMSG, "%s: Wallet is locked, storing message to scan later.\n", __func__);

        int rv;
        if ((rv = SecureMsgStoreUnscanned(pHeader, pPayload, nPayload)) != 0)
            return 1;

        return 3;
    };

    CKeyID addressTo;
    MessageData msg; // placeholder
    bool fOwnMessage = false;

    for (std::vector<SecMsgAddress>::iterator it = smsgAddresses.begin(); it != smsgAddresses.end(); ++it)
    {
        if (!it->fReceiveEnabled)
            continue;

        addressTo = it->address;

        if (!it->fReceiveAnon)
        {
            // Have to do full decrypt to see address from
            if (SecureMsgDecrypt(false, addressTo, pHeader, pPayload, nPayload, msg) == 0)
            {
                if (LogAcceptCategory(BCLog::SMSG))
                    LogPrintf("Decrypted message with %s.\n", CBitcoinAddress(addressTo).ToString());

                if (msg.sFromAddress.compare("anon") != 0)
                    fOwnMessage = true;
                break;
            };
        } else
        {
            if (SecureMsgDecrypt(true, addressTo, pHeader, pPayload, nPayload, msg) == 0)
            {
                if (LogAcceptCategory(BCLog::SMSG))
                    LogPrintf("Decrypted message with %s.\n", CBitcoinAddress(addressTo).ToString());

                fOwnMessage = true;
                break;
            };
        }
    };

    if (fOwnMessage)
    {
        // Save to inbox
        SecureMessage *psmsg = (SecureMessage*) pHeader;
        std::string sPrefix("im");
        uint8_t chKey[18];
        memcpy(&chKey[0],  sPrefix.data(),    2);
        memcpy(&chKey[2],  &psmsg->timestamp, 8);
        memcpy(&chKey[10], pPayload,          8);

        SecMsgStored smsgInbox;
        smsgInbox.timeReceived  = GetTime();
        smsgInbox.status        = (SMSG_MASK_UNREAD) & 0xFF;
        smsgInbox.addrTo        = addressTo;

        // Data may not be contiguous
        try { smsgInbox.vchMessage.resize(SMSG_HDR_LEN + nPayload); } catch (std::exception &e)
        {
            LogPrintf("SecureMsgScanMessage(): Could not resize vchData, %u, %s\n", SMSG_HDR_LEN + nPayload, e.what());
            return 1;
        };
        memcpy(&smsgInbox.vchMessage[0], pHeader, SMSG_HDR_LEN);
        memcpy(&smsgInbox.vchMessage[SMSG_HDR_LEN], pPayload, nPayload);

        {
            LOCK(cs_smsgDB);
            SecMsgDB dbInbox;

            if (dbInbox.Open("cw"))
            {
                if (dbInbox.ExistsSmesg(chKey))
                {
                    LogPrint(BCLog::SMSG, "Message already exists in inbox db.\n");
                } else
                {
                    dbInbox.WriteSmesg(chKey, smsgInbox);

                    if (reportToGui)
                        NotifySecMsgInboxChanged(smsgInbox);
                    LogPrintf("SecureMsg saved to inbox, received with %s.\n", CBitcoinAddress(addressTo).ToString());
                };
            };
        } // cs_smsgDB

        // notify an external script when a message comes in
        std::string strCmd = gArgs.GetArg("-smsgnotify", "");

        //TODO: Format message
        if (!strCmd.empty())
        {
            boost::replace_all(strCmd, "%s", CBitcoinAddress(addressTo).ToString());
            boost::thread t(runCommand, strCmd); // thread runs free
        };
    };
#endif
    return 0;
};

int SecureMsgGetLocalKey(CKeyID &ckid, CPubKey &cpkOut)
{
#ifdef ENABLE_WALLET
    LogPrint(BCLog::SMSG, "SecureMsgGetLocalKey()\n");
    
    if (!pwalletSmsg)
        return errorN(1, "%s: Wallet disabled.", __func__);

    if (!pwalletSmsg->GetPubKey(ckid, cpkOut))
        return 4;

    if (!cpkOut.IsValid()
        || !cpkOut.IsCompressed())
    {
        LogPrintf("Public key is invalid %s.\n", HexStr(cpkOut));
        return 1;
    };

    return 0;
#else
    return 1;
#endif
};

int SecureMsgGetLocalPublicKey(std::string &strAddress, std::string &strPublicKey)
{
    // TODO: pass in CkeyID?
    /* returns
        0 success,
        1 error
        2 invalid address
        3 address does not refer to a key
        4 address not in wallet
    */
    //if (fDebugSmsg)
    //   LogPrintf("SecureMsgGetLocalPublicKey().\n");

    CBitcoinAddress address;
    if (!address.SetString(strAddress))
        return 2; // Invalid coin address

    CKeyID keyID;
    if (!address.GetKeyID(keyID))
        return 3;

    int rv;
    CPubKey pubKey;
    if ((rv = SecureMsgGetLocalKey(keyID, pubKey)) != 0)
        return rv;

    strPublicKey = EncodeBase58(pubKey.begin(), pubKey.end());

    return 0;
};

int SecureMsgGetStoredKey(CKeyID &ckid, CPubKey &cpkOut)
{
    /* returns
        0 success,
        1 error
        2 public key not in database
    */
    LogPrint(BCLog::SMSG, "SecureMsgGetStoredKey().\n");

    {
        LOCK(cs_smsgDB);
        SecMsgDB addrpkdb;

        if (!addrpkdb.Open("r"))
            return 1;

        if (!addrpkdb.ReadPK(ckid, cpkOut))
        {
            //LogPrintf("addrpkdb.Read failed: %s.\n", coinAddress.ToString());
            return 2;
        };
    } // cs_smsgDB

    return 0;
};

int SecureMsgAddAddress(std::string &address, std::string &publicKey)
{
    /*
    Add address and matching public key to the database
    address and publicKey are in base58

    returns
        0 success
        1 error
        2 publicKey is invalid
        3 publicKey != address
        4 address is already in db
        5 address is invalid
    */

    CBitcoinAddress coinAddress(address);

    if (!coinAddress.IsValid())
        return errorN(5, "%s - Address is not valid: %s.", __func__, address);

    CKeyID hashKey;

    if (!coinAddress.GetKeyID(hashKey))
        return errorN(5, "%s - coinAddress.GetKeyID failed: %s.", __func__, coinAddress.ToString());

    std::vector<uint8_t> vchTest;
    DecodeBase58(publicKey, vchTest);
    CPubKey pubKey(vchTest);

    // Check that public key matches address hash
    CPubKey pubKeyT(pubKey);
    if (!pubKeyT.IsValid())
        return errorN(2, "%s - Invalid PubKey.", __func__);

    CKeyID keyIDT = pubKeyT.GetID();
    CBitcoinAddress addressT(keyIDT);

    if (addressT.ToString().compare(address) != 0)
        return errorN(3, "%s - Public key does not hash to address, addressT %s.", __func__, addressT.ToString());

    return SecureMsgInsertAddress(hashKey, pubKey);
};

int SecureMsgRetrieve(SecMsgToken &token, std::vector<uint8_t> &vchData)
{
    LogPrint(BCLog::SMSG, "SecureMsgRetrieve() %d.\n", token.timestamp);

    // Has cs_smsg lock from SecureMsgReceiveData

    fs::path pathSmsgDir = GetDataDir() / "smsgstore";

    //LogPrintf("token.offset %d.\n", token.offset); // DEBUG
    int64_t bucket = token.timestamp - (token.timestamp % SMSG_BUCKET_LEN);
    std::string fileName = std::to_string(bucket) + "_01.dat";
    fs::path fullpath = pathSmsgDir / fileName;

    //LogPrintf("bucket %d.\n", bucket);
    //LogPrintf("bucket d %d.\n", bucket);
    //LogPrintf("fileName %s.\n", fileName);

    FILE *fp;
    errno = 0;
    if (!(fp = fopen(fullpath.string().c_str(), "rb")))
        return errorN(1, "%s - Can't open file: %s\nPath %s.", __func__, strerror(errno), fullpath.string());

    errno = 0;
    if (fseek(fp, token.offset, SEEK_SET) != 0)
    {
        fclose(fp);
        return errorN(1, "%s - fseek, strerror: %s.", __func__, strerror(errno));
    };

    SecureMessage smsg;
    errno = 0;
    if (fread(&smsg.hash[0], sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN)
    {
        fclose(fp);
        return errorN(1, "%s - read header failed, strerror: %s.", __func__, strerror(errno));
    };

    try {vchData.resize(SMSG_HDR_LEN + smsg.nPayload);} catch (std::exception &e)
    {
        fclose(fp);
        return errorN(1, "%s - Could not resize vchData, %u, %s.", __func__, SMSG_HDR_LEN + smsg.nPayload, e.what());
    };

    memcpy(&vchData[0], &smsg.hash[0], SMSG_HDR_LEN);
    errno = 0;
    if (fread(&vchData[SMSG_HDR_LEN], sizeof(uint8_t), smsg.nPayload, fp) != smsg.nPayload)
    {
        fclose(fp);
        return errorN(1, "%s - fread data failed: %s. Wanted %u bytes.", __func__, strerror(errno), smsg.nPayload);
    };

    fclose(fp);

    return 0;
};

int SecureMsgReceive(CNode *pfrom, std::vector<uint8_t> &vchData)
{
    LogPrint(BCLog::SMSG, "SecureMsgReceive().\n");

    if (vchData.size() < 12) // nBunch4 + timestamp8
        return errorN(1, "%s - Not enough data.", __func__);

    uint32_t nBunch;
    int64_t bktTime;

    memcpy(&nBunch, &vchData[0], 4);
    memcpy(&bktTime, &vchData[4], 8);

    // Check bktTime ()
    //  Bucket may not exist yet - will be created when messages are added
    int64_t now = GetTime();
    if (bktTime > now + SMSG_TIME_LEEWAY)
    {
        LogPrint(BCLog::SMSG, "bktTime > now.\n");
        // misbehave?
        return 1;
    } else
    if (bktTime < now - SMSG_RETENTION)
    {
        LogPrint(BCLog::SMSG, "bktTime < now - SMSG_RETENTION.\n");
        // misbehave?
        return 1;
    };

    std::map<int64_t, SecMsgBucket>::iterator itb;

    if (nBunch == 0 || nBunch > 500)
    {
        LogPrintf("Error: Invalid no. messages received in bunch %u, for bucket %d.\n", nBunch, bktTime);
        Misbehaving(pfrom->GetId(), 1);

        {
            LOCK(cs_smsg);
            // Release lock on bucket if it exists
            itb = smsgBuckets.find(bktTime);
            if (itb != smsgBuckets.end())
                itb->second.nLockCount = 0;
        } // cs_smsg
        return 1;
    };

    uint32_t n = 12;

    for (uint32_t i = 0; i < nBunch; ++i)
    {
        if (vchData.size() - n < SMSG_HDR_LEN)
        {
            LogPrintf("Error: not enough data sent, n = %u.\n", n);
            break;
        };

        SecureMessage *psmsg = (SecureMessage*) &vchData[n];

        int rv;
        if ((rv = SecureMsgValidate(&vchData[n], &vchData[n + SMSG_HDR_LEN], psmsg->nPayload)) != 0)
        {
            // message dropped
            if (rv == 2) // invalid proof of work
            {
                Misbehaving(pfrom->GetId(), 10);
            } else
            {
                Misbehaving(pfrom->GetId(), 1);
            };
            continue;
        };

        {
            LOCK(cs_smsg);
            // Store message, but don't hash bucket
            if (SecureMsgStore(&vchData[n], &vchData[n + SMSG_HDR_LEN], psmsg->nPayload, false) != 0)
            {
                // message dropped
                break; // continue?
            };

            if (SecureMsgScanMessage(&vchData[n], &vchData[n + SMSG_HDR_LEN], psmsg->nPayload, true) != 0)
            {
                // message recipient is not this node (or failed)
            };
        } // cs_smsg

        n += SMSG_HDR_LEN + psmsg->nPayload;
    };

    {
        LOCK(cs_smsg);
        // If messages have been added, bucket must exist now
        itb = smsgBuckets.find(bktTime);
        if (itb == smsgBuckets.end())
        {
            LogPrint(BCLog::SMSG, "Don't have bucket %d.\n", bktTime);
            return 1;
        };

        itb->second.nLockCount  = 0; // This node has received data from peer, release lock
        itb->second.nLockPeerId = 0;
        itb->second.hashBucket();
    } // cs_smsg

    return 0;
};

int SecureMsgStoreUnscanned(uint8_t *pHeader, uint8_t *pPayload, uint32_t nPayload)
{
    /*
    When the wallet is locked a copy of each received message is stored to be scanned later if wallet is unlocked
    */

    LogPrint(BCLog::SMSG, "SecureMsgStoreUnscanned()\n");

    if (!pHeader
        || !pPayload)
    {
        return errorN(1, "%s - Null pointer to header or payload.", __func__);
    };

    SecureMessage* psmsg = (SecureMessage*) pHeader;

    fs::path pathSmsgDir;
    try {
        pathSmsgDir = GetDataDir() / "smsgstore";
        fs::create_directory(pathSmsgDir);
    } catch (const fs::filesystem_error& ex)
    {
        return errorN(1, "%s - Failed to create directory %s - %s.", __func__, pathSmsgDir.string(), ex.what());
    };

    int64_t now = GetTime();
    if (psmsg->timestamp > now + SMSG_TIME_LEEWAY)
        return errorN(1, "%s: Message > now.", __func__);
    if (psmsg->timestamp < now - SMSG_RETENTION)
        return errorN(1, "%s: Message < SMSG_RETENTION.", __func__);

    int64_t bucket = psmsg->timestamp - (psmsg->timestamp % SMSG_BUCKET_LEN);

    std::string fileName = std::to_string(bucket) + "_01_wl.dat";
    fs::path fullpath = pathSmsgDir / fileName;

    FILE *fp;
    errno = 0;
    if (!(fp = fopen(fullpath.string().c_str(), "ab")))
    {
        return errorN(1, "%s - Can't open file, strerror: %s.", __func__, strerror(errno));
    };

    if (fwrite(pHeader, sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN
        || fwrite(pPayload, sizeof(uint8_t), nPayload, fp) != nPayload)
    {
        fclose(fp);
        return errorN(1, "%s - fwrite failed, strerror: %s.", __func__, strerror(errno));
    };

    fclose(fp);

    return 0;
};


int SecureMsgStore(uint8_t *pHeader, uint8_t *pPayload, uint32_t nPayload, bool fUpdateBucket)
{
    if (LogAcceptCategory(BCLog::SMSG))
    {
        LogPrintf("SecureMsgStore()\n");
        AssertLockHeld(cs_smsg);
    };

    if (!pHeader
        || !pPayload)
    {
        return errorN(1, "Null pointer to header or payload.");
    };

    SecureMessage *psmsg = (SecureMessage*) pHeader;


    long int ofs;
    fs::path pathSmsgDir;
    try {
        pathSmsgDir = GetDataDir() / "smsgstore";
        fs::create_directory(pathSmsgDir);
    } catch (const fs::filesystem_error& ex)
    {
        return errorN(1, "Failed to create directory %s - %s.", pathSmsgDir.string(), ex.what());
    };

    int64_t now = GetTime();
    if (psmsg->timestamp > now + SMSG_TIME_LEEWAY)
        return errorN(1, "%s: Message > now.", __func__);
    if (psmsg->timestamp < now - SMSG_RETENTION)
        return errorN(1, "%s: Message < SMSG_RETENTION.", __func__);

    int64_t bucket = psmsg->timestamp - (psmsg->timestamp % SMSG_BUCKET_LEN);

    SecMsgToken token(psmsg->timestamp, pPayload, nPayload, 0);

    std::set<SecMsgToken>& tokenSet = smsgBuckets[bucket].setTokens;
    std::set<SecMsgToken>::iterator it;
    it = tokenSet.find(token);
    if (it != tokenSet.end())
    {
        LogPrintf("Already have message.\n");

        if (LogAcceptCategory(BCLog::SMSG))
        {
            LogPrintf("nPayload: %u\n", nPayload);
            LogPrintf("bucket: %d\n", bucket);

            LogPrintf("message ts: %d", token.timestamp);
            std::vector<uint8_t> vchShow;
            vchShow.resize(8);
            memcpy(&vchShow[0], token.sample, 8);
            LogPrintf(" sample %s\n", HexStr(vchShow));
            /*
            LogPrintf("\nmessages in bucket:\n");
            for (it = tokenSet.begin(); it != tokenSet.end(); ++it)
            {
                LogPrintf("message ts: %d", (*it).timestamp);
                vchShow.resize(8);
                memcpy(&vchShow[0], (*it).sample, 8);
                LogPrintf(" sample %s\n", HexStr(vchShow));
            };
            */
        };
        return 1;
    };

    std::string fileName = std::to_string(bucket) + "_01.dat";
    fs::path fullpath = pathSmsgDir / fileName;

    FILE *fp;
    errno = 0;
    if (!(fp = fopen(fullpath.string().c_str(), "ab")))
    {
        return errorN(1, "fopen failed: %s.", strerror(errno));
    };

    // On windows ftell will always return 0 after fopen(ab), call fseek to set.
    errno = 0;
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return errorN(1, "fseek failed: %s.", strerror(errno));
    };
    
    ofs = ftell(fp);

    if (fwrite(pHeader,  sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN
     || fwrite(pPayload, sizeof(uint8_t),     nPayload, fp) != nPayload)
    {
        fclose(fp);
        return errorN(1, "fwrite failed: %s.", strerror(errno));
    };

    fclose(fp);

    token.offset = ofs;

    //LogPrintf("token.offset: %d\n", token.offset); // DEBUG
    tokenSet.insert(token);

    if (fUpdateBucket)
        smsgBuckets[bucket].hashBucket();

    LogPrint(BCLog::SMSG, "SecureMsg added to bucket %d.\n", bucket);

    return 0;
};

int SecureMsgStore(SecureMessage& smsg, bool fUpdateBucket)
{
    return SecureMsgStore(&smsg.hash[0], smsg.pPayload, smsg.nPayload, fUpdateBucket);
};

int SecureMsgValidate(uint8_t *pHeader, uint8_t *pPayload, uint32_t nPayload)
{
    /*
    returns
        0 success
        1 error
        2 invalid hash
        3 checksum mismatch
        4 invalid version
        5 payload is too large
    */
    SecureMessage *psmsg = (SecureMessage*) pHeader;

    if (psmsg->version[0] != 2)
        return 4;

    if (nPayload > SMSG_MAX_MSG_WORST)
        return 5;

    uint8_t civ[32];
    uint8_t sha256Hash[32];
    int rv = 2; // invalid

    uint32_t nonce;
    memcpy(&nonce, &psmsg->nonce[0], 4);

    LogPrint(BCLog::SMSG, "%s: nonce %u.\n", __func__, nonce);

    for (int i = 0; i < 32; i+=4)
        memcpy(civ+i, &nonce, 4);

    CHMAC_SHA256 ctx(&civ[0], 32);
    ctx.Write((uint8_t*) pHeader+4, SMSG_HDR_LEN-4);
    ctx.Write((uint8_t*) pPayload, nPayload);
    ctx.Finalize(sha256Hash);

    if (sha256Hash[31] == 0
        && sha256Hash[30] == 0
        && (~(sha256Hash[29]) & ((1<<0) | (1<<1) | (1<<2)) ))
    {
        LogPrint(BCLog::SMSG, "Hash Valid.\n");
        rv = 0; // smsg is valid
    };

    if (part::memcmp_nta(psmsg->hash, sha256Hash, 4) != 0)
    {
        LogPrint(BCLog::SMSG, "Checksum mismatch.\n");
        rv = 3; // checksum mismatch
    };

    return rv;
};

int SecureMsgSetHash(uint8_t *pHeader, uint8_t *pPayload, uint32_t nPayload)
{
    /*  proof of work and checksum

        May run in a thread, if shutdown detected, return.

        returns:
            0 success
            1 error
            2 stopped due to node shutdown

    */

    SecureMessage* psmsg = (SecureMessage*) pHeader;

    int64_t nStart = GetTimeMillis();
    uint8_t civ[32];
    uint8_t sha256Hash[32];

    bool found = false;

    uint32_t nonce = 0;

    //CBigNum bnTarget(2);
    //bnTarget = bnTarget.pow(256 - 40);

    // Break for HMAC_CTX_cleanup
    for (;;)
    {
        if (!fSecMsgEnabled)
           break;

        //psmsg->timestamp = GetTime();
        //memcpy(&psmsg->timestamp, &now, 8);
        memcpy(&psmsg->nonce[0], &nonce, 4);

        for (int i = 0; i < 32; i+=4)
            memcpy(civ+i, &nonce, 4);

        CHMAC_SHA256 ctx(&civ[0], 32);
        ctx.Write((uint8_t*) pHeader+4, SMSG_HDR_LEN-4);
        ctx.Write((uint8_t*) pPayload, nPayload);
        ctx.Finalize(sha256Hash);

        /*
        if (CBigNum(vchHash) <= bnTarget)
        {
            found = true;
            if (fDebugSmsg)
                LogPrintf("Match %u\n", nonce);
            break;
        };
        */

        if (sha256Hash[31] == 0
            && sha256Hash[30] == 0
            && (~(sha256Hash[29]) & ((1<<0) | (1<<1) | (1<<2)) ))
        //    && sha256Hash[29] == 0)
        {
            found = true;
            //if (fDebugSmsg)
            //    LogPrintf("Match %u\n", nonce);
            break;
        };

        //if (nonce >= UINT32_MAX)
        if (nonce >= 4294967295U)
        {
            LogPrint(BCLog::SMSG, "No match %u\n", nonce);
            break;
            //return 1;
        };
        nonce++;
    };

    if (!fSecMsgEnabled)
    {
        LogPrint(BCLog::SMSG, "%s: Stopped, shutdown detected.\n", __func__);
        return 2;
    };

    if (!found)
    {
        LogPrint(BCLog::SMSG, "%s: Failed, took %d ms, nonce %u\n", __func__, GetTimeMillis() - nStart, nonce);
        return 1;
    };

    memcpy(psmsg->hash, sha256Hash, 4);

    LogPrint(BCLog::SMSG, "%s: Took %d ms, nonce %u\n", __func__, GetTimeMillis() - nStart, nonce);

    return 0;
};

int SecureMsgEncrypt(SecureMessage &smsg, const CKeyID &addressFrom, const CKeyID &addressTo, const std::string &message)
{
#ifdef ENABLE_WALLET
    /* Create a secure message

    Using similar method to bitmessage.
    If bitmessage is secure this should be too.
    https://bitmessage.org/wiki/Encryption

    Some differences:
    bitmessage seems to use curve sect283r1
    *coin addresses use secp256k1

    returns
        2       message is too long.
        3       addressFrom is invalid.
        4       addressTo is invalid.
        5       Could not get public key for addressTo.
        6       ECDH_compute_key failed
        7       Could not get private key for addressFrom.
        8       Could not allocate memory.
        9       Could not compress message data.
        10      Could not generate MAC.
        11      Encrypt failed.
    */

    bool fSendAnonymous = addressFrom.IsNull();

    if (LogAcceptCategory(BCLog::SMSG))
    {
        LogPrint(BCLog::SMSG, "SecureMsgEncrypt(%s, %s, ...)\n",
            fSendAnonymous ? "anon" : CBitcoinAddress(addressFrom).ToString(),
            CBitcoinAddress(addressTo).ToString());
    };

    if (message.size() > (fSendAnonymous ? SMSG_MAX_AMSG_BYTES : SMSG_MAX_MSG_BYTES))
        return errorN(2, "%s: Message is too long, %u.", __func__, message.size());

    smsg.version[0] = 2;
    smsg.version[1] = 1;
    smsg.timestamp = GetTime();

    CBitcoinAddress coinAddrFrom;
    CKeyID ckidFrom;
    CKey keyFrom;


    if (!fSendAnonymous)
    {
        if (!coinAddrFrom.Set(addressFrom))
            return errorN(3, "%s: addressFrom is not valid.", __func__);

        if (!coinAddrFrom.GetKeyID(ckidFrom))
            return errorN(4, "%s: coinAddrFrom.GetKeyID failed: %s.", __func__, coinAddrFrom.ToString());
    };

    CBitcoinAddress coinAddrDest;
    CKeyID ckidDest = addressTo;

    // Public key K is the destination address
    CPubKey cpkDestK;
    if (SecureMsgGetStoredKey(ckidDest, cpkDestK) != 0
        && SecureMsgGetLocalKey(ckidDest, cpkDestK) != 0) // maybe it's a local key (outbox?)
    {
        return errorN(5, "%s: Could not get public key for destination address.", __func__);
    };

    // Generate 16 random bytes as IV.
    GetStrongRandBytes(&smsg.iv[0], 16);

    // Generate a new random EC key pair with private key called r and public key called R.
    CKey keyR;
    keyR.MakeNewKey(true); // make compressed key
    
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_parse(secp256k1_context_smsg, &pubkey, cpkDestK.begin(), cpkDestK.size()))
        return errorN(4, "%s: secp256k1_ec_pubkey_parse failed: %s.", __func__, HexStr(cpkDestK));
    
    std::vector<uint8_t> vchP;
    vchP.resize(32);
    
    if (!secp256k1_ecdh(secp256k1_context_smsg, &vchP[0], &pubkey, keyR.begin()))
        return errorN(6, "%s: secp256k1_ecdh failed.", __func__);

    CPubKey cpkR = keyR.GetPubKey();
    if (!cpkR.IsValid()
        || !cpkR.IsCompressed())
    {
        return errorN(1, "%s: Could not get public key for key R.", __func__);
    };

    memcpy(smsg.cpkR, cpkR.begin(), 33);

    // Use public key P and calculate the SHA512 hash H.
    //   The first 32 bytes of H are called key_e and the last 32 bytes are called key_m.
    std::vector<uint8_t> vchHashed;
    vchHashed.resize(64); // 512
    memset(vchHashed.data(), 0, 64);
    CSHA512().Write(&vchP[0], vchP.size()).Finalize(&vchHashed[0]);
    std::vector<uint8_t> key_e(&vchHashed[0], &vchHashed[0]+32);
    std::vector<uint8_t> key_m(&vchHashed[32], &vchHashed[32]+32);

    std::vector<uint8_t> vchPayload;
    std::vector<uint8_t> vchCompressed;
    uint8_t *pMsgData;
    uint32_t lenMsgData;

    uint32_t lenMsg = message.size();
    if (lenMsg > 128)
    {
        // Only compress if over 128 bytes
        int worstCase = LZ4_compressBound(message.size());
        try { vchCompressed.resize(worstCase); } catch (std::exception &e)
        {
            return errorN(8, "%s: vchCompressed.resize %u threw: %s.", __func__, worstCase, e.what());
        };

        int lenComp = LZ4_compress((char*)message.c_str(), (char*)&vchCompressed[0], lenMsg);
        if (lenComp < 1)
        {
            return errorN(9, "%s: Could not compress message data.", __func__);
        };

        pMsgData = &vchCompressed[0];
        lenMsgData = lenComp;
    } else
    {
        // No compression
        pMsgData = (uint8_t*)message.c_str();
        lenMsgData = lenMsg;
    };

    if (fSendAnonymous)
    {
        try { vchPayload.resize(9 + lenMsgData); } catch (std::exception &e)
        {
            return errorN(8, "%s: vchPayload.resize %u threw: %s.", __func__, 9 + lenMsgData, e.what());
        };

        memcpy(&vchPayload[9], pMsgData, lenMsgData);

        vchPayload[0] = 250; // id as anonymous message
        // Next 4 bytes are unused - there to ensure encrypted payload always > 8 bytes
        memcpy(&vchPayload[5], &lenMsg, 4); // length of uncompressed plain text
    } else
    {
        try { vchPayload.resize(SMSG_PL_HDR_LEN + lenMsgData); } catch (std::exception &e)
        {
            return errorN(8, "%s: vchPayload.resize %u threw: %s.", __func__, SMSG_PL_HDR_LEN + lenMsgData, e.what());
        };

        memcpy(&vchPayload[SMSG_PL_HDR_LEN], pMsgData, lenMsgData);
        // Compact signature proves ownership of from address and allows the public key to be recovered, recipient can always reply.
        if (!pwalletSmsg->GetKey(ckidFrom, keyFrom))
        {
            return errorN(7, "%s: Could not get private key for addressFrom.", __func__);
        };

        // Sign the plaintext
        std::vector<uint8_t> vchSignature;
        vchSignature.resize(65);
        keyFrom.SignCompact(Hash(message.begin(), message.end()), vchSignature);

        // Save some bytes by sending address raw
        vchPayload[0] = (static_cast<CBitcoinAddress_B*>(&coinAddrFrom))->getVersion(); // vchPayload[0] = coinAddrDest.nVersion;
        memcpy(&vchPayload[1], ckidFrom.begin(), 20); // memcpy(&vchPayload[1], ckidDest.pn, 20);

        memcpy(&vchPayload[1+20], &vchSignature[0], vchSignature.size());
        memcpy(&vchPayload[1+20+65], &lenMsg, 4); // length of uncompressed plain text
    };

    SecMsgCrypter crypter;
    crypter.SetKey(key_e, smsg.iv);
    std::vector<uint8_t> vchCiphertext;

    if (!crypter.Encrypt(&vchPayload[0], vchPayload.size(), vchCiphertext))
        return errorN(11, "%s: Encrypt failed.", __func__);

    try { smsg.pPayload = new uint8_t[vchCiphertext.size()]; } catch (std::exception &e)
    {
        return errorN(8, "%s: Could not allocate pPayload, exception: %s.", __func__, e.what());
    };

    memcpy(smsg.pPayload, &vchCiphertext[0], vchCiphertext.size());
    smsg.nPayload = vchCiphertext.size();


    // Calculate a 32 byte MAC with HMACSHA256, using key_m as salt
    //  Message authentication code, (hash of timestamp + iv + destination + payload)

    CHMAC_SHA256 ctx(&key_m[0], 32);
    ctx.Write((uint8_t*) &smsg.timestamp, sizeof(smsg.timestamp));
    ctx.Write((uint8_t*) smsg.iv, sizeof(smsg.iv));
    ctx.Write((uint8_t*) &vchCiphertext[0], vchCiphertext.size());
    ctx.Finalize(smsg.mac);

#endif
    return 0;
};

int SecureMsgSend(CKeyID &addressFrom, CKeyID &addressTo, std::string &message, std::string &sError)
{
#ifdef ENABLE_WALLET
    /* Encrypt secure message, and place it on the network
        Make a copy of the message to sender's first address and place in send queue db
        proof of work thread will pick up messages from  send queue db
    */

    bool fSendAnonymous = (addressFrom.IsNull());

    if (LogAcceptCategory(BCLog::SMSG))
    {
        LogPrintf("SecureMsgSend(%s, %s, ...)\n", 
            fSendAnonymous ? "anon" : CBitcoinAddress(addressFrom).ToString(), CBitcoinAddress(addressTo).ToString());
    };
    
    if (!pwalletSmsg)
    {
        sError = "Wallet is not enabled.";
        return errorN(1, "%s: %s.", __func__, sError);
    };
    if (pwalletSmsg->IsLocked())
    {
        sError = "Wallet is locked, wallet must be unlocked to send and recieve messages.";
        return errorN(1, "%s: %s.", __func__, sError);
    };

    if (message.size() > (fSendAnonymous ? SMSG_MAX_AMSG_BYTES : SMSG_MAX_MSG_BYTES))
    {
        sError = strprintf("Message is too long, %d > %d", message.size(), fSendAnonymous ? SMSG_MAX_AMSG_BYTES : SMSG_MAX_MSG_BYTES);
        return errorN(1, "%s: %s.", __func__, sError);
    };

    int rv;
    SecureMessage smsg;

    if ((rv = SecureMsgEncrypt(smsg, addressFrom, addressTo, message)) != 0)
    {
        switch(rv)
        {
            case 2:  sError = "Message is too long.";                       break;
            case 3:  sError = "Invalid addressFrom.";                       break;
            case 4:  sError = "Invalid addressTo.";                         break;
            case 5:  sError = "Could not get public key for addressTo.";    break;
            case 6:  sError = "ECDH_compute_key failed.";                   break;
            case 7:  sError = "Could not get private key for addressFrom."; break;
            case 8:  sError = "Could not allocate memory.";                 break;
            case 9:  sError = "Could not compress message data.";           break;
            case 10: sError = "Could not generate MAC.";                    break;
            case 11: sError = "Encrypt failed.";                            break;
            default: sError = "Unspecified Error.";                         break;
        };
        
        return errorN(rv, "%s: %s.", __func__, sError);
    };


    // Place message in send queue, proof of work will happen in a thread.
    std::string sPrefix("qm");
    uint8_t chKey[18];
    memcpy(&chKey[0],  sPrefix.data(),  2);
    memcpy(&chKey[2],  &smsg.timestamp, 8);
    memcpy(&chKey[10], &smsg.pPayload,  8);

    SecMsgStored smsgSQ;

    smsgSQ.timeReceived  = GetTime();
    smsgSQ.addrTo        = addressTo;

    try { smsgSQ.vchMessage.resize(SMSG_HDR_LEN + smsg.nPayload);
    } catch (std::exception &e) {
        LogPrintf("smsgSQ.vchMessage.resize %u threw: %s.\n", SMSG_HDR_LEN + smsg.nPayload, e.what());
        sError = "Could not allocate memory.";
        return 8;
    };

    memcpy(&smsgSQ.vchMessage[0], &smsg.hash[0], SMSG_HDR_LEN);
    memcpy(&smsgSQ.vchMessage[SMSG_HDR_LEN], smsg.pPayload, smsg.nPayload);

    {
        LOCK(cs_smsgDB);
        SecMsgDB dbSendQueue;
        if (dbSendQueue.Open("cw"))
        {
            dbSendQueue.WriteSmesg(chKey, smsgSQ);
            //NotifySecMsgSendQueueChanged(smsgOutbox);
        };
    } // cs_smsgDB

    // TODO: only update outbox when proof of work thread is done.

    //  For outbox create a copy encrypted for owned address
    //   if the wallet is encrypted private key needed to decrypt will be unavailable

    LogPrint(BCLog::SMSG, "Encrypting message for outbox.\n");

    CKeyID addressOutbox;

    for (const auto &entry : pwalletSmsg->mapAddressBook) // PAIRTYPE(CTxDestination, CAddressBookData)
    {
        // Get first owned address
        if (!IsMine(*pwalletSmsg, entry.first))
            continue;

        const CBitcoinAddress &address = entry.first;

        if (!address.IsValid())
            continue;
        address.GetKeyID(addressOutbox);
        break;
    };

    if (addressOutbox.IsNull())
    {
        LogPrintf("Warning: SecureMsgSend() could not find an address to encrypt outbox message with.\n");
    } else
    {
        if (LogAcceptCategory(BCLog::SMSG))
            LogPrintf("Encrypting a copy for outbox, using address %s\n", CBitcoinAddress(addressOutbox).ToString());

        SecureMessage smsgForOutbox;
        if ((rv = SecureMsgEncrypt(smsgForOutbox, addressFrom, addressOutbox, message)) != 0)
        {
            LogPrintf("SecureMsgSend(), encrypt for outbox failed, %d.\n", rv);
        } else
        {
            // Save sent message to db
            std::string sPrefix("sm");
            uint8_t chKey[18];
            memcpy(&chKey[0],  sPrefix.data(),           2);
            memcpy(&chKey[2],  &smsgForOutbox.timestamp, 8);
            memcpy(&chKey[10], &smsgForOutbox.pPayload,  8);   // sample

            SecMsgStored smsgOutbox;

            smsgOutbox.timeReceived  = GetTime();
            smsgOutbox.addrTo        = addressTo;
            smsgOutbox.addrOutbox    = addressOutbox;

            try {
                smsgOutbox.vchMessage.resize(SMSG_HDR_LEN + smsgForOutbox.nPayload);
            } catch (std::exception &e) {
                LogPrintf("smsgOutbox.vchMessage.resize %u threw: %s.\n", SMSG_HDR_LEN + smsgForOutbox.nPayload, e.what());
                sError = "Could not allocate memory.";
                return 8;
            };
            memcpy(&smsgOutbox.vchMessage[0], &smsgForOutbox.hash[0], SMSG_HDR_LEN);
            memcpy(&smsgOutbox.vchMessage[SMSG_HDR_LEN], smsgForOutbox.pPayload, smsgForOutbox.nPayload);

            {
                LOCK(cs_smsgDB);
                SecMsgDB dbSent;

                if (dbSent.Open("cw"))
                {
                    dbSent.WriteSmesg(chKey, smsgOutbox);
                    NotifySecMsgOutboxChanged(smsgOutbox);
                };
            } // cs_smsgDB
        };
    };

    if (LogAcceptCategory(BCLog::SMSG))
        LogPrintf("Secure message queued for sending to %s.\n", CBitcoinAddress(addressTo).ToString());

#endif
    return 0;
};


int SecureMsgDecrypt(bool fTestOnly, CKeyID &address, uint8_t *pHeader, uint8_t *pPayload, uint32_t nPayload, MessageData &msg)
{
#ifdef ENABLE_WALLET
    /* Decrypt secure message

        address is the owned address to decrypt with.

        validate first in SecureMsgValidate

        returns
            1       Error
            2       Unknown version number
            3       Decrypt address is not valid.
            8       Could not allocate memory
    */

    if (LogAcceptCategory(BCLog::SMSG))
        LogPrintf("%s: using %s, testonly %d.\n", __func__, CBitcoinAddress(address).ToString(), fTestOnly);

    if (!pHeader
        || !pPayload)
    {
        return errorN(1, "%s: null pointer to header or payload.", __func__);
    };

    SecureMessage *psmsg = (SecureMessage*) pHeader;

    if (psmsg->version[0] != 2)
        return errorN(2, "%s: Unknown version number.", __func__);

    // Fetch private key k, used to decrypt
    CKey keyDest;
    
    if (!pwalletSmsg->GetKey(address, keyDest))
        return errorN(3, "%s: Could not get private key for addressDest.", __func__);

    secp256k1_pubkey R;
    if (!secp256k1_ec_pubkey_parse(secp256k1_context_smsg, &R, psmsg->cpkR, 33))
        return errorN(1, "%s: secp256k1_ec_pubkey_parse failed: %s.", __func__, HexStr(psmsg->cpkR, psmsg->cpkR+33));
    
    std::vector<uint8_t> vchP;
    vchP.resize(32);
    
    // Do an EC point multiply with private key k and public key R. This gives you public key P.
    if (!secp256k1_ecdh(secp256k1_context_smsg, &vchP[0], &R, keyDest.begin()))
        return errorN(1, "%s: secp256k1_ecdh failed.", __func__);

    // Use public key P to calculate the SHA512 hash H.
    //  The first 32 bytes of H are called key_e and the last 32 bytes are called key_m.
    std::vector<uint8_t> vchHashedDec;
    vchHashedDec.resize(64);    // 512 bits
    memset(vchHashedDec.data(), 0, 64); 
    CSHA512().Write(&vchP[0], vchP.size()).Finalize(&vchHashedDec[0]);
    std::vector<uint8_t> key_e(&vchHashedDec[0], &vchHashedDec[0]+32);
    std::vector<uint8_t> key_m(&vchHashedDec[32], &vchHashedDec[32]+32);

    // Message authentication code, (hash of timestamp + iv + destination + payload)
    uint8_t MAC[32];

    CHMAC_SHA256 ctx(&key_m[0], 32);
    ctx.Write((uint8_t*) &psmsg->timestamp, sizeof(psmsg->timestamp));
    ctx.Write((uint8_t*) psmsg->iv, sizeof(psmsg->iv));
    ctx.Write((uint8_t*) pPayload, nPayload);
    ctx.Finalize(MAC);

    if (part::memcmp_nta(MAC, psmsg->mac, 32) != 0)
    {
        LogPrint(BCLog::SMSG, "MAC does not match.\n"); // expected if message is not to address on node
        return 1;
    };

    if (fTestOnly)
        return 0;

    SecMsgCrypter crypter;
    crypter.SetKey(key_e, psmsg->iv);
    std::vector<uint8_t> vchPayload;
    if (!crypter.Decrypt(pPayload, nPayload, vchPayload))
        return errorN(1, "%s: Decrypt failed.", __func__);

    msg.timestamp = psmsg->timestamp;
    uint32_t lenData;
    uint32_t lenPlain;

    uint8_t *pMsgData;
    bool fFromAnonymous;
    if ((uint32_t)vchPayload[0] == 250)
    {
        fFromAnonymous = true;
        lenData = vchPayload.size() - (9);
        memcpy(&lenPlain, &vchPayload[5], 4);
        pMsgData = &vchPayload[9];
    } else
    {
        fFromAnonymous = false;
        lenData = vchPayload.size() - (SMSG_PL_HDR_LEN);
        memcpy(&lenPlain, &vchPayload[1+20+65], 4);
        pMsgData = &vchPayload[SMSG_PL_HDR_LEN];
    };

    try {
        msg.vchMessage.resize(lenPlain + 1);
    } catch (std::exception &e) {
        return errorN(8, "%s: msg.vchMessage.resize %u threw: %s.", __func__, lenPlain + 1, e.what());
    };


    if (lenPlain > 128)
    {
        // Decompress
        if (LZ4_decompress_safe((char*) pMsgData, (char*) &msg.vchMessage[0], lenData, lenPlain) != (int) lenPlain)
            return errorN(1, "%s: Could not decompress message data.", __func__);
    } else
    {
        // Plaintext
        memcpy(&msg.vchMessage[0], pMsgData, lenPlain);
    };

    msg.vchMessage[lenPlain] = '\0';

    if (fFromAnonymous)
    {
        // Anonymous sender
        msg.sFromAddress = "anon";
    } else
    {
        std::vector<uint8_t> vchUint160;
        vchUint160.resize(20);

        memcpy(&vchUint160[0], &vchPayload[1], 20);

        uint160 ui160(vchUint160);
        CKeyID ckidFrom(ui160);

        CBitcoinAddress coinAddrFrom;
        coinAddrFrom.Set(ckidFrom);
        if (!coinAddrFrom.IsValid())
            return errorN(1, "%s: From Address is invalid.", __func__);

        std::vector<uint8_t> vchSig;
        vchSig.resize(65);

        memcpy(&vchSig[0], &vchPayload[1+20], 65);

        CPubKey cpkFromSig;
        cpkFromSig.RecoverCompact(Hash(msg.vchMessage.begin(), msg.vchMessage.end()-1), vchSig);
        if (!cpkFromSig.IsValid())
            return errorN(1, "%s: Signature validation failed.", __func__);

        // Get address for the compressed public key
        CBitcoinAddress coinAddrFromSig;
        coinAddrFromSig.Set(cpkFromSig.GetID());

        if (!(coinAddrFrom == coinAddrFromSig))
            return errorN(1, "%s: Signature validation failed.", __func__);

        int rv = 5;
        try {
            rv = SecureMsgInsertAddress(ckidFrom, cpkFromSig);
        } catch (std::exception &e) {
            LogPrintf("SecureMsgInsertAddress(), exception: %s.\n", e.what());
            //return 1;
        };

        switch(rv)
        {
            case 0:
                LogPrintf("Sender public key added to db.\n");
                break;
            case 4:
                LogPrintf("Sender public key already in db.\n");
                break;
            default:
                LogPrintf("Error adding sender public key to db.\n");
                break;
        };

        msg.sFromAddress = coinAddrFrom.ToString();
    };

    if (LogAcceptCategory(BCLog::SMSG))
        LogPrintf("Decrypted message for %s.\n", CBitcoinAddress(address).ToString());

#endif
    return 0;
};

int SecureMsgDecrypt(bool fTestOnly, CKeyID &address, SecureMessage &smsg, MessageData &msg)
{
    return SecureMsgDecrypt(fTestOnly, address, &smsg.hash[0], smsg.pPayload, smsg.nPayload, msg);
};

