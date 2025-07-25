#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-types.h"
#include <lib/json/json.h>
#include <memory>
#include <optional>
#include <string>

namespace asio
{
}
namespace medida
{
class MetricsRegistry;
}

namespace stellar
{

class VirtualClock;
class TmpDirManager;
class LedgerManager;
class BucketManager;
class LedgerApplyManager;
class HistoryArchiveManager;
class HistoryManager;
class Maintainer;
class ProcessManager;
class Herder;
class HerderPersistence;
class InvariantManager;
class OverlayManager;
class Database;
class PersistentState;
class CommandHandler;
class WorkScheduler;
class BanManager;
class StatusManager;
class AbstractLedgerTxnParent;
class BasicWork;
enum class LoadGenMode;
struct GeneratedLoadConfig;
class AppConnector;

#ifdef BUILD_TESTS
class LoadGenerator;
class TestAccount;
#endif

class Application;
void validateNetworkPassphrase(std::shared_ptr<Application> app);

/*
 * State of a single instance of the stellar-core application.
 *
 * Multiple instances may exist in the same process, eg. for the sake of testing
 * by simulating a network of Applications.
 *
 *
 * Clocks, time and events
 * -----------------------
 *
 * An Application is connected to a VirtualClock, that both manages the
 * Application's view of time and also owns an IO event loop that dispatches
 * events for the main thread. See VirtualClock for details.
 *
 * In order to advance an Application's view of time, as well as dispatch any IO
 * events, timers or callbacks, the associated VirtualClock must be cranked. See
 * VirtualClock::crank().
 *
 * All Applications coordinating on a simulation should be bound to the same
 * VirtualClock, so that their view of time advances from event to event within
 * the collective simulation.
 *
 *
 * Configuration
 * -------------
 *
 * Each Application owns a Config object, which describes any user-controllable
 * configuration variables, including cryptographic keys, network ports, log
 * files, directories and the like. A local copy of the Config object is made on
 * construction of each Application, after which the local copy cannot be
 * further altered; the Application should be destroyed and recreated if any
 * change to configuration is desired.
 *
 *
 * Subsystems
 * ----------
 *
 * Each Application owns a collection of subsystem "manager" objects, typically
 * one per subdirectory in the source tree. For example, the LedgerManager, the
 * OverlayManager, the HistoryManager, etc. Instances of these subsystem objects
 * are generally created in 1:1 correspondence with their owning Application;
 * each Application creates a new LedgerManager for itself, for example.
 *
 * Each subsystem object contains a reference back to its owning Application,
 *and
 * uses this reference to retrieve its Application's associated instance of the
 * other subsystems. So for example an Application's LedgerManager can access
 * that Application's HistoryManager in order to run catchup. Subsystems access
 * one another through virtual interfaces, to afford some degree of support for
 * testing and information hiding.
 *
 *
 * Threading
 * ---------
 *
 * In general, Application expects to run on a single thread -- the main thread
 * -- and most subsystems perform no locking, are not multi-thread
 * safe. Operations with high IO latency are broken into steps and executed
 * piecewise through the VirtualClock's asio::io_context; those with high CPU
 * latency are run on a "worker" thread pool.
 *
 * Each Application owns a secondary "worker" asio::io_context, that queues and
 * dispatches CPU-bound work to a number of worker threads (one per core); these
 * serve only to offload self-contained CPU-bound tasks like hashing from the
 * main thread, and do not generally call back into the Application's owned
 * sub-objects (with a couple exceptions, in the BucketManager and BucketList
 * objects).
 *
 * Completed "worker" tasks typically post their results back to the main
 * thread's io_context (held in the VirtualClock), or else deliver their results
 * to the Application through std::futures or similar standard
 * thread-synchronization primitives.
 *
 */

class Application
{
  public:
    typedef std::shared_ptr<Application> pointer;

    // Running state of an application; different values inhibit or enable
    // certain subsystem responses to IO events, timers etc.
    enum State
    {
        // Application created, but not started
        APP_CREATED_STATE,

        // Out of sync with SCP peers
        APP_ACQUIRING_CONSENSUS_STATE,

        // Connected to other SCP peers

        // in sync with network but ledger subsystem still booting up
        APP_CONNECTED_STANDBY_STATE,

        // some work required to catchup to the consensus ledger
        // ie: downloading from history, applying buckets and replaying
        // transactions
        APP_CATCHING_UP_STATE,

        // In sync with SCP peers, applying transactions. SCP is active,
        APP_SYNCED_STATE,

        // application is shutting down
        APP_STOPPING_STATE,

        APP_NUM_STATE
    };

    // Types of threads that may be running
    enum class ThreadType
    {
        MAIN,
        WORKER,
        EVICTION,
        OVERLAY,
        APPLY
    };

    virtual ~Application(){};

    virtual void initialize(bool createNewDB, bool forceRebuild) = 0;

    // reset the ledger state entirely
    // (to be used before applying buckets)
    virtual void resetLedgerState() = 0;

    // Return the time in seconds since the POSIX epoch, according to the
    // VirtualClock this Application is bound to. Convenience method.
    virtual uint64_t timeNow() = 0;

    // Return a reference to the Application-local copy of the Config object
    // that the Application was constructed with.
    virtual Config const& getConfig() = 0;

    // Gets the current execution-state of the Application
    // (derived from the state of other modules
    virtual State getState() const = 0;
    virtual std::string getStateHuman() const = 0;
    virtual bool isStopping() const = 0;

    // Get the external VirtualClock to which this Application is bound.
    virtual VirtualClock& getClock() = 0;

    // Get the registry of metrics owned by this application. Metrics are
    // reported through the administrative HTTP interface, see CommandHandler.
    virtual medida::MetricsRegistry& getMetrics() = 0;

    // Ensure any App-local metrics that are "current state" gauge-like counters
    // reflect the current reality as best as possible.
    virtual void syncOwnMetrics() = 0;

    // Call syncOwnMetrics on self and syncMetrics all objects owned by App.
    virtual void syncAllMetrics() = 0;

    // Clear all metrics
    virtual void clearMetrics(std::string const& domain) = 0;

    // Get references to each of the "subsystem" objects.
    virtual TmpDirManager& getTmpDirManager() = 0;
    virtual LedgerManager& getLedgerManager() = 0;
    virtual BucketManager& getBucketManager() = 0;
    virtual LedgerApplyManager& getLedgerApplyManager() = 0;
    virtual HistoryArchiveManager& getHistoryArchiveManager() = 0;
    virtual HistoryManager& getHistoryManager() = 0;
    virtual Maintainer& getMaintainer() = 0;
    virtual ProcessManager& getProcessManager() = 0;
    virtual Herder& getHerder() = 0;
    virtual HerderPersistence& getHerderPersistence() = 0;
    virtual InvariantManager& getInvariantManager() = 0;
    virtual OverlayManager& getOverlayManager() = 0;
    virtual Database& getDatabase() const = 0;
    virtual PersistentState& getPersistentState() = 0;
    virtual CommandHandler& getCommandHandler() = 0;
    virtual WorkScheduler& getWorkScheduler() = 0;
    virtual BanManager& getBanManager() = 0;
    virtual StatusManager& getStatusManager() = 0;

    // Get the worker IO service, served by background threads. Work posted to
    // this io_context will execute in parallel with the calling thread, so use
    // with caution.
    virtual asio::io_context& getWorkerIOContext() = 0;
    virtual asio::io_context& getEvictionIOContext() = 0;
    virtual asio::io_context& getOverlayIOContext() = 0;
    virtual asio::io_context& getLedgerCloseIOContext() = 0;

    virtual void postOnMainThread(
        std::function<void()>&& f, std::string&& name,
        Scheduler::ActionType type = Scheduler::ActionType::NORMAL_ACTION) = 0;

    // While both are lower priority than the main thread, eviction threads have
    // more priority than regular worker background threads
    virtual void postOnBackgroundThread(std::function<void()>&& f,
                                        std::string jobName) = 0;
    virtual void postOnEvictionBackgroundThread(std::function<void()>&& f,
                                                std::string jobName) = 0;
    virtual void postOnOverlayThread(std::function<void()>&& f,
                                     std::string jobName) = 0;
    virtual void postOnLedgerCloseThread(std::function<void()>&& f,
                                         std::string jobName) = 0;

    // Perform actions necessary to transition from BOOTING_STATE to other
    // states. In particular: either reload or reinitialize the database, and
    // either restart or begin reacquiring SCP consensus (as instructed by
    // Config).
    virtual void start() = 0;

    // Stop the io_contexts, which should cause the threads to exit once they
    // finish running any work-in-progress.
    virtual void gracefulStop() = 0;

    // Wait-on and join all the threads this application started; should only
    // return when there is no more work to do or someone has force-stopped the
    // worker io_context. Application can be safely destroyed after this
    // returns.
    virtual void joinAllThreads() = 0;

    // If config.MANUAL_CLOSE=true, force the current ledger to close and return
    // true. Otherwise return false. This method exists only for testing.
    //
    // Non-default parameters may be specified only if additionally
    // config.RUN_STANDALONE=true.
    virtual std::string
    manualClose(std::optional<uint32_t> const& manualLedgerSeq,
                std::optional<TimePoint> const& manualCloseTime) = 0;

#ifdef BUILD_TESTS
    // If config.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING=true, generate some load
    // against the current application.
    virtual void generateLoad(GeneratedLoadConfig cfg) = 0;

    // Access the load generator for manual operation.
    virtual LoadGenerator& getLoadGenerator() = 0;

    virtual std::shared_ptr<TestAccount> getRoot() = 0;

    // Access the runtime overlay-only mode flag for testing
    virtual bool getRunInOverlayOnlyMode() const = 0;
    virtual void setRunInOverlayOnlyMode(bool mode) = 0;
#endif

    // Execute any administrative commands written in the Config.COMMANDS
    // variable of the config file. This permits scripting certain actions to
    // occur automatically at startup.
    virtual void applyCfgCommands() = 0;

    // Report, via standard logging, the current state any metrics defined in
    // the Config.REPORT_METRICS (or passed on the command line with --metric)
    virtual void reportCfgMetrics() = 0;

    // Get information about the instance as JSON object
    virtual Json::Value getJsonInfo(bool verbose) = 0;

    // Report information about the instance to standard logging
    virtual void reportInfo(bool verbose) = 0;

    // Schedule background work to do some (basic, online) self-checks.
    // Returns a WorkSequence that can be monitored for completion.
    virtual std::shared_ptr<BasicWork>
    scheduleSelfCheck(bool waitUntilNextCheckpoint) = 0;

    // Returns the hash of the passphrase, used to separate various network
    // instances
    virtual Hash const& getNetworkID() const = 0;

    virtual AbstractLedgerTxnParent& getLedgerTxnRoot() = 0;

    virtual void validateAndLogConfig() = 0;

    // Factory: create a new Application object bound to `clock`, with a local
    // copy made of `cfg`
    static pointer create(VirtualClock& clock, Config const& cfg,
                          bool newDB = true, bool forceRebuild = false);
    template <typename T, typename... Args>
    static std::shared_ptr<T>
    create(VirtualClock& clock, Config const& cfg, Args&&... args,
           bool newDB = true, bool forceRebuild = false)
    {
        auto ret = std::make_shared<T>(clock, cfg, std::forward<Args>(args)...);
        ret->initialize(newDB, forceRebuild);
        validateNetworkPassphrase(ret);
        ret->validateAndLogConfig();

        return ret;
    }

    // Returns true iff the calling thread has the same type as `type`
    virtual bool threadIsType(ThreadType type) const = 0;

    virtual AppConnector& getAppConnector() = 0;

  protected:
    Application()
    {
    }
};
}
