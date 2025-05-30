// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantManagerImpl.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketList.h"
#include "crypto/Hex.h"
#include "invariant/Invariant.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManagerImpl.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDRCereal.h"
#include <fmt/format.h>

#include "medida/counter.h"
#include "medida/metrics_registry.h"

#include <memory>
#include <numeric>
#include <regex>

namespace stellar
{

std::unique_ptr<InvariantManager>
InvariantManager::create(Application& app)
{
    return std::make_unique<InvariantManagerImpl>(app.getMetrics());
}

InvariantManagerImpl::InvariantManagerImpl(medida::MetricsRegistry& registry)
    : mInvariantFailureCount(
          registry.NewCounter({"ledger", "invariant", "failure"}))
{
}

Json::Value
InvariantManagerImpl::getJsonInfo()
{
    Json::Value failures;

    for (auto const& fi : mFailureInformation)
    {
        auto& fail = failures[fi.first];
        auto& info = fi.second;
        fail["last_failed_on_ledger"] = info.lastFailedOnLedger;
        fail["last_failed_with_message"] = info.lastFailedWithMessage;
    }
    if (!failures.empty())
    {
        failures["count"] = (Json::Int64)mInvariantFailureCount.count();
    }
    return failures;
}

std::vector<std::string>
InvariantManagerImpl::getEnabledInvariants() const
{
    std::vector<std::string> res;
    for (auto const& p : mEnabled)
    {
        res.emplace_back(p->getName());
    }
    return res;
}

bool
InvariantManagerImpl::isBucketApplyInvariantEnabled() const
{
    return std::any_of(mEnabled.begin(), mEnabled.end(), [](auto const& inv) {
        return inv->getName() == "BucketListIsConsistentWithDatabase";
    });
}

void
InvariantManagerImpl::checkOnBucketApply(
    std::shared_ptr<LiveBucket const> bucket, uint32_t ledger, uint32_t level,
    bool isCurr, std::unordered_set<LedgerKey> const& shadowedKeys)
{
    uint32_t oldestLedger =
        isCurr ? LiveBucketList::oldestLedgerInCurr(ledger, level)
               : LiveBucketList::oldestLedgerInSnap(ledger, level);
    uint32_t newestLedger =
        oldestLedger - 1 +
        (isCurr ? LiveBucketList::sizeOfCurr(ledger, level)
                : LiveBucketList::sizeOfSnap(ledger, level));
    for (auto invariant : mEnabled)
    {
        auto result = invariant->checkOnBucketApply(bucket, oldestLedger,
                                                    newestLedger, shadowedKeys);
        if (result.empty())
        {
            continue;
        }

        auto message = fmt::format(
            FMT_STRING(
                R"(invariant "{}" does not hold on bucket {}[{}] = {}: {})"),
            invariant->getName(), isCurr ? "Curr" : "Snap", level,
            binToHex(bucket->getHash()), result);
        onInvariantFailure(invariant, message, ledger);
    }
}

void
InvariantManagerImpl::checkAfterAssumeState(uint32_t newestLedger)
{
    for (auto invariant : mEnabled)
    {
        auto result = invariant->checkAfterAssumeState(newestLedger);
        if (result.empty())
        {
            continue;
        }

        auto message = fmt::format(
            FMT_STRING(
                R"(invariant "{}" does not hold after assume state: {})"),
            invariant->getName(), result);
        onInvariantFailure(invariant, message, 0);
    }
}

void
InvariantManagerImpl::checkOnOperationApply(
    Operation const& operation, OperationResult const& opres,
    LedgerTxnDelta const& ltxDelta, std::vector<ContractEvent> const& events)
{
    for (auto invariant : mEnabled)
    {
        if (protocolVersionIsBefore(ltxDelta.header.current.ledgerVersion,
                                    ProtocolVersion::V_8) &&
            invariant->getName() != "EventsAreConsistentWithEntryDiffs")
        {
            continue;
        }

        auto result = invariant->checkOnOperationApply(operation, opres,
                                                       ltxDelta, events);
        if (result.empty())
        {
            continue;
        }

        auto message = fmt::format(
            FMT_STRING(R"(Invariant "{}" does not hold on operation: {}{}{})"),
            invariant->getName(), result, "\n",
            xdrToCerealString(operation, "Operation"));
        onInvariantFailure(invariant, message,
                           ltxDelta.header.current.ledgerSeq);
    }
}

void
InvariantManagerImpl::registerInvariant(std::shared_ptr<Invariant> invariant)
{
    auto name = invariant->getName();
    auto iter = mInvariants.find(name);
    if (iter == mInvariants.end())
    {
        mInvariants[name] = invariant;
    }
    else
    {
        throw std::runtime_error{"Invariant " + invariant->getName() +
                                 " already registered"};
    }
}

void
InvariantManagerImpl::enableInvariant(std::string const& invPattern)
{
    if (invPattern.empty())
    {
        throw std::invalid_argument("Invariant pattern must be non empty");
    }

    std::regex r;
    try
    {
        r = std::regex(invPattern, std::regex::ECMAScript | std::regex::icase);
    }
    catch (std::regex_error& e)
    {
        throw std::invalid_argument(
            fmt::format(FMT_STRING("Invalid invariant pattern '{}': {}"),
                        invPattern, e.what()));
    }

    bool enabledSome = false;
    for (auto const& inv : mInvariants)
    {
        auto const& name = inv.first;
        if (std::regex_match(name, r, std::regex_constants::match_not_null))
        {
            auto iter = std::find(mEnabled.begin(), mEnabled.end(), inv.second);
            if (iter == mEnabled.end())
            {
                enabledSome = true;
                mEnabled.push_back(inv.second);
                CLOG_INFO(Invariant, "Enabled invariant '{}'", name);
            }
            else
            {
                throw std::runtime_error{"Invariant " + name +
                                         " already enabled"};
            }
        }
    }
    if (!enabledSome)
    {
        std::string message = fmt::format(
            FMT_STRING("Invariant pattern '{}' did not match any invariants."),
            invPattern);
        if (mInvariants.size() > 0)
        {
            using value_type = decltype(mInvariants)::value_type;
            std::string registered = std::accumulate(
                std::next(mInvariants.cbegin()), mInvariants.cend(),
                mInvariants.cbegin()->first,
                [](std::string const& lhs, value_type const& rhs) {
                    return lhs + ", " + rhs.first;
                });
            message += " Registered invariants are: " + registered;
        }
        else
        {
            message += " There are no registered invariants";
        }
        throw std::runtime_error{message};
    }
}

void
InvariantManagerImpl::onInvariantFailure(std::shared_ptr<Invariant> invariant,
                                         std::string const& message,
                                         uint32_t ledger)
{
    mInvariantFailureCount.inc();
    mFailureInformation[invariant->getName()] = {ledger, message};
    handleInvariantFailure(invariant, message);
}

void
InvariantManagerImpl::handleInvariantFailure(
    std::shared_ptr<Invariant> invariant, std::string const& message) const
{
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    abort();
#endif
    if (invariant->isStrict())
    {
        CLOG_FATAL(Invariant, "{}", message);
        CLOG_FATAL(Invariant, "{}", REPORT_INTERNAL_BUG);
        throw InvariantDoesNotHold{message};
    }
    else
    {
        CLOG_ERROR(Invariant, "{}", message);
        CLOG_ERROR(Invariant, "{}", REPORT_INTERNAL_BUG);
    }
}

#ifdef BUILD_TESTS
void
InvariantManagerImpl::snapshotForFuzzer()
{
    for (auto const& invariant : mEnabled)
    {
        invariant->snapshotForFuzzer();
    }
}

void
InvariantManagerImpl::resetForFuzzer()
{
    for (auto const& invariant : mEnabled)
    {
        invariant->resetForFuzzer();
    }
}
#endif // BUILD_TESTS
}
