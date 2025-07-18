// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "invariant/AccountSubEntriesCountIsValid.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "invariant/test/InvariantTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/util/stdrandom.h"
#include "main/Application.h"
#include "test/Catch2.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/Math.h"
#include <random>
#include <xdrpp/autocheck.h>

using namespace stellar;
using namespace stellar::InvariantTestUtils;

static LedgerEntry
generateRandomAccountWithNoSubEntries(uint32_t ledgerSeq)
{
    LedgerEntry le;
    le.lastModifiedLedgerSeq = ledgerSeq;
    le.data.type(ACCOUNT);
    le.data.account() = LedgerTestUtils::generateValidAccountEntry(5);
    auto& ae = le.data.account();

    ae.signers.clear();
    if (hasAccountEntryExtV2(ae))
    {
        ae.ext.v1().ext.v2().signerSponsoringIDs.clear();
    }

    ae.numSubEntries = 0;
    return le;
}

static LedgerEntry
generateRandomSubEntry(LedgerEntry const& acc)
{
    static auto validAccountIDGenerator =
        autocheck::map([](AccountID&& id, size_t s) { return std::move(id); },
                       autocheck::generator<AccountID>());
    static auto validDataNameGenerator = autocheck::map(
        [](string64&& dn, size_t s) {
            LedgerTestUtils::replaceControlCharacters(dn, 64);
            return std::move(dn);
        },
        autocheck::generator<string64>());

    LedgerEntry le;
    do
    {
        le = LedgerTestUtils::generateValidLedgerEntry(5);
    } while (le.data.type() == ACCOUNT || le.data.type() == CLAIMABLE_BALANCE ||
             le.data.type() == LIQUIDITY_POOL ||
             le.data.type() == CONFIG_SETTING ||
             le.data.type() == CONTRACT_DATA ||
             le.data.type() == CONTRACT_CODE || le.data.type() == TTL);
    le.lastModifiedLedgerSeq = acc.lastModifiedLedgerSeq;

    switch (le.data.type())
    {
    case OFFER:
        le.data.offer().sellerID = acc.data.account().accountID;
        break;
    case TRUSTLINE:
        le.data.trustLine().accountID = acc.data.account().accountID;
        switch (le.data.trustLine().asset.type())
        {
        case ASSET_TYPE_CREDIT_ALPHANUM4:
            le.data.trustLine().asset.alphaNum4().issuer =
                validAccountIDGenerator();
            break;
        case ASSET_TYPE_CREDIT_ALPHANUM12:
            le.data.trustLine().asset.alphaNum12().issuer =
                validAccountIDGenerator();
            break;
        default:
            break;
        }
        break;
    case DATA:
        le.data.data().accountID = acc.data.account().accountID;
        le.data.data().dataName = validDataNameGenerator(64);
        break;
    case CLAIMABLE_BALANCE:
    case ACCOUNT:
    case LIQUIDITY_POOL:
    case CONFIG_SETTING:
    case CONTRACT_DATA:
    case CONTRACT_CODE:
    case TTL:
    default:
        abort();
    }
    return le;
}

static LedgerEntry
generateRandomModifiedSubEntry(LedgerEntry const& acc, LedgerEntry const& se)
{
    LedgerEntry res;
    do
    {
        res = generateRandomSubEntry(acc);
    } while (res.data.type() != se.data.type());

    switch (se.data.type())
    {
    case ACCOUNT:
    case CLAIMABLE_BALANCE:
    case LIQUIDITY_POOL:
    case CONFIG_SETTING:
    case CONTRACT_DATA:
    case CONTRACT_CODE:
    case TTL:
        break;
    case OFFER:
        res.data.offer().offerID = se.data.offer().offerID;
        break;
    case TRUSTLINE:
        res.data.trustLine().accountID = se.data.trustLine().accountID;
        res.data.trustLine().asset = se.data.trustLine().asset;
        break;
    case DATA:
        res.data.data().dataName = se.data.data().dataName;
        break;
    default:
        abort();
    }
    return res;
}

static auto validSignerGenerator = autocheck::map(
    [](Signer&& signer, size_t s) {
        signer.weight = signer.weight & UINT8_MAX;
        if (signer.weight == 0)
        {
            signer.weight = 100;
        }
        return std::move(signer);
    },
    autocheck::generator<Signer>());

static void
updateAccountSubEntries(Application& app, LedgerEntry& leCurr,
                        LedgerEntry lePrev, int32_t deltaNumSubEntries,
                        UpdateList const& updatesBase)
{
    if (deltaNumSubEntries != 0)
    {
        auto updates = updatesBase;
        auto currPtr = std::make_shared<LedgerEntry>(leCurr);
        auto prevPtr = std::make_shared<LedgerEntry>(lePrev);
        updates.push_back(std::make_tuple(currPtr, prevPtr));
        LedgerTxn ltx(app.getLedgerTxnRoot());
        REQUIRE(!store(app, updates, &ltx));
    }
    {
        leCurr.data.account().numSubEntries += deltaNumSubEntries;
        auto updates = updatesBase;
        auto currPtr = std::make_shared<LedgerEntry>(leCurr);
        auto prevPtr = std::make_shared<LedgerEntry>(lePrev);
        updates.push_back(std::make_tuple(currPtr, prevPtr));
        REQUIRE(store(app, updates));
    }
}

static void
addRandomSubEntryToAccount(Application& app, LedgerEntry& le,
                           std::vector<LedgerEntry>& subentries)
{
    auto lePrev = le;
    auto& acc = le.data.account();

    bool addSigner = false;
    if (acc.signers.size() < acc.signers.max_size())
    {
        auto letGen = autocheck::generator<LedgerEntryType>();
        addSigner = (letGen(5) == ACCOUNT);
    }

    ++le.lastModifiedLedgerSeq;
    if (addSigner)
    {
        acc.signers.push_back(validSignerGenerator());
        if (hasAccountEntryExtV2(acc))
        {
            acc.ext.v1().ext.v2().signerSponsoringIDs.push_back(
                autocheck::generator<SponsorshipDescriptor>()(5));
        }

        updateAccountSubEntries(app, le, lePrev, 1, {});
    }
    else
    {
        auto se = generateRandomSubEntry(le);
        subentries.push_back(se);
        updateAccountSubEntries(app, le, lePrev,
                                testutil::computeMultiplier(se),
                                makeUpdateList({se}, nullptr));
    }
}

static void
modifyRandomSubEntryFromAccount(Application& app, LedgerEntry& le,
                                std::vector<LedgerEntry>& subentries)
{
    auto lePrev = le;
    auto& acc = le.data.account();
    REQUIRE(le.data.account().numSubEntries > 0);

    bool modifySigner = false;
    if (acc.signers.size() > 0)
    {
        auto letGen = autocheck::generator<LedgerEntryType>();
        modifySigner = subentries.empty() || (letGen(5) == ACCOUNT);
    }

    ++le.lastModifiedLedgerSeq;
    if (modifySigner)
    {
        stellar::uniform_int_distribution<uint32_t> dist(
            0, uint32_t(acc.signers.size()) - 1);
        acc.signers.at(dist(gRandomEngine)) = validSignerGenerator();
        updateAccountSubEntries(app, le, lePrev, 0, {});
    }
    else
    {
        stellar::uniform_int_distribution<uint32_t> dist(
            0, uint32_t(subentries.size()) - 1);
        auto index = dist(gRandomEngine);
        auto se = subentries.at(index);
        auto se2 = generateRandomModifiedSubEntry(le, se);
        subentries.at(index) = se2;
        updateAccountSubEntries(app, le, lePrev, 0,
                                makeUpdateList({se2}, {se}));
    }
}

static void
deleteRandomSubEntryFromAccount(Application& app, LedgerEntry& le,
                                std::vector<LedgerEntry>& subentries)
{
    auto lePrev = le;
    auto& acc = le.data.account();
    REQUIRE(le.data.account().numSubEntries > 0);

    bool deleteSigner = false;
    if (acc.signers.size() > 0)
    {
        auto letGen = autocheck::generator<LedgerEntryType>();
        deleteSigner = subentries.empty() || (letGen(5) == ACCOUNT);
    }

    ++le.lastModifiedLedgerSeq;
    if (deleteSigner)
    {
        stellar::uniform_int_distribution<uint32_t> dist(
            0, uint32_t(acc.signers.size()) - 1);

        auto pos = dist(gRandomEngine);
        acc.signers.erase(acc.signers.begin() + pos);
        if (hasAccountEntryExtV2(acc))
        {
            auto& sponsoringIDs = acc.ext.v1().ext.v2().signerSponsoringIDs;
            sponsoringIDs.erase(sponsoringIDs.begin() + pos);
        }

        updateAccountSubEntries(app, le, lePrev, -1, {});
    }
    else
    {
        stellar::uniform_int_distribution<uint32_t> dist(
            0, uint32_t(subentries.size()) - 1);
        auto index = dist(gRandomEngine);
        auto se = subentries.at(index);
        subentries.erase(subentries.begin() + index);
        updateAccountSubEntries(app, le, lePrev,
                                -testutil::computeMultiplier(se),
                                makeUpdateList(nullptr, {se}));
    }
}

TEST_CASE("Create account with no subentries",
          "[invariant][accountsubentriescount]")
{
    Config cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);
    cfg.INVARIANT_CHECKS = {"AccountSubEntriesCountIsValid"};
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    for (uint32_t i = 0; i < 100; ++i)
    {
        auto le = generateRandomAccountWithNoSubEntries(2);
        REQUIRE(store(*app, makeUpdateList({le}, nullptr)));
        REQUIRE(store(*app, makeUpdateList(nullptr, {le})));
    }
}

TEST_CASE("Create account then add signers and subentries",
          "[invariant][accountsubentriescount]")
{
    stellar::uniform_int_distribution<int32_t> changesDist(-1, 2);
    Config cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);
    cfg.INVARIANT_CHECKS = {"AccountSubEntriesCountIsValid"};

    for (uint32_t i = 0; i < 50; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le = generateRandomAccountWithNoSubEntries(2);
        REQUIRE(store(*app, makeUpdateList({le}, nullptr)));

        std::vector<LedgerEntry> subentries;
        for (uint32_t j = 0; j < 50; ++j)
        {
            auto change = changesDist(gRandomEngine);
            if (change > 0 || le.data.account().numSubEntries == 0)
            {
                addRandomSubEntryToAccount(*app, le, subentries);
            }
            else if (change == 0)
            {
                modifyRandomSubEntryFromAccount(*app, le, subentries);
            }
            else if (change < 0)
            {
                deleteRandomSubEntryFromAccount(*app, le, subentries);
            }
        }

        if (le.data.account().numSubEntries != le.data.account().signers.size())
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(!store(*app, makeUpdateList(nullptr, {le}), &ltx));
        }
        {
            UpdateList apply(makeUpdateList(nullptr, {le}));
            for (auto const& se : subentries)
            {
                auto sePtr = std::make_shared<LedgerEntry>(se);
                apply.push_back(std::make_tuple(nullptr, sePtr));
            }
            REQUIRE(store(*app, apply));
        }
    }
}
