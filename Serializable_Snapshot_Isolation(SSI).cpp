#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <random>
#include <chrono>
#include <algorithm>
#include <climits>
#include <thread>

using namespace std;
using namespace chrono;

// Utility functions
int getRand(int max) {
    static mt19937 gen(random_device{}());
    return uniform_int_distribution<>(0, max-1)(gen);
}

int getExpRand(double lambda) {
    static mt19937 gen(random_device{}());
    exponential_distribution<> dist(lambda);
    return (int)dist(gen);
}

long getSysTime() {
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Versioned data structure
struct VersionedValue {
    int value;
    int version;
};

struct SharedEntry {
    vector<VersionedValue> versions;
    int current_version = 0;
    int value = 0; // Current value for simplified access
    int version = 0; // Current version for simplified access
};

// Transaction structure
struct Transaction {
    int snapshot_version;
    unordered_set<int> read_locks;
    unordered_map<int, int> write_buffer;
    unordered_set<Transaction*> in_conflicts;
    unordered_set<Transaction*> out_conflicts;
    bool read_only;
    int64_t start_time;
    bool committed = false;

    Transaction(int snapshot, bool ro) 
        : snapshot_version(snapshot), read_only(ro), start_time(getSysTime()) {}
};

// Global data structures
vector<SharedEntry> shared;
mutex sharedMutex;

// SSI lock manager
unordered_map<int, unordered_set<Transaction*>> SIREAD_locks;
mutex lockManagerMutex;

// Transaction tracking
unordered_set<Transaction*> activeTransactions;
unordered_set<Transaction*> committedButUnsafeTxns;
mutex activeTransactionsMutex;

mutex statsLock;
long totalCommitTime = 0;
long totalAborts = 0;
long committedTxns = 0;

// Configurable parameters
int M               = 1000;   // Number of shared variables
int totTrans        = 1000;  // Total number of transactions
int constVal        = 1000;   // max write value
int lambdaVal       = 20;     // for exponential sleep
int numIters        = 20;     // fixed ops per txn
int numThreads      = 16;     // N
int readOnlyPercent = 80;     // % read-only txns

// ---- SSI Functions ----

Transaction* beginTransaction(bool read_only = false) {
    lock_guard<mutex> slock(sharedMutex);
    int max_version = 0;
    for (const auto& entry : shared) {
        if (entry.version > max_version) max_version = entry.version;
    }

    Transaction* txn = new Transaction(max_version, read_only);

    lock_guard<mutex> alock(activeTransactionsMutex);
    activeTransactions.insert(txn);
    return txn;
}

void read(Transaction* txn, int key, int& value) {
    // 1. Check transaction's own write buffer first
    auto write_it = txn->write_buffer.find(key);
    if (write_it != txn->write_buffer.end()) {
        value = write_it->second;
        return;
    }

    // 2. Read from version history using MVCC
    {
        lock_guard<mutex> slock(sharedMutex);
        int target_version = txn->snapshot_version;
        
        // Search backwards for most recent visible version
        for (auto it = shared[key].versions.rbegin(); it != shared[key].versions.rend(); ++it) {
            if (it->version <= target_version) {
                value = it->value;
                break;
            }
        }
        // If no version found, value remains 0 (default)
    }

    // 3. Acquire SIREAD lock for SSI conflict detection
    {
        lock_guard<mutex> llock(lockManagerMutex);
        SIREAD_locks[key].insert(txn);
        txn->read_locks.insert(key);
    }
}

void write(Transaction* txn, int key, int value) {
    // 1. Buffer the write locally
    txn->write_buffer[key] = value;

    // 2. Check for SIREAD lock conflicts (must lock sharedMutex first to prevent deadlocks)
    {
        lock_guard<mutex> slock(sharedMutex);
        lock_guard<mutex> llock(lockManagerMutex);
        
        auto it = SIREAD_locks.find(key);
        if (it != SIREAD_locks.end()) {
            for (Transaction* other : it->second) {
                if (other == txn) continue;

                // Check if other transaction is concurrent
                bool concurrent = !other->committed || 
                                (other->committed && other->start_time < txn->start_time);
                
                if (concurrent) {
                    // Record rw-antidependency (other read, we're writing)
                    txn->in_conflicts.insert(other);
                    other->out_conflicts.insert(txn);
                }
            }
        }
    }

    // 3. For MVCC: Create new version (applied on commit)
    {
        lock_guard<mutex> slock(sharedMutex);
        shared[key].current_version++;
        shared[key].versions.push_back({value, shared[key].current_version});
    }
}

void cleanupTransaction(Transaction* txn) {
    lock_guard<mutex> llock(lockManagerMutex);
    for (int key : txn->read_locks) {
        SIREAD_locks[key].erase(txn);
        if (SIREAD_locks[key].empty()) {
            SIREAD_locks.erase(key);
        }
    }
    delete txn;
}

void garbageCollectOldVersions() {
    lock_guard<mutex> slock(sharedMutex);
    lock_guard<mutex> alock(activeTransactionsMutex);
    
    int oldest_active_snapshot = INT_MAX;
    for (Transaction* txn : activeTransactions) {
        if (txn->snapshot_version < oldest_active_snapshot) {
            oldest_active_snapshot = txn->snapshot_version;
        }
    }

    for (auto& entry : shared) {
        // Remove versions older than the oldest active snapshot
        auto& versions = entry.versions;
        versions.erase(
            remove_if(versions.begin(), versions.end(),
                [oldest_active_snapshot](const VersionedValue& v) {
                    return v.version < oldest_active_snapshot;
                }),
            versions.end());
    }
}

void cleanupCommittedTxns() {
    lock_guard<mutex> alock(activeTransactionsMutex);
    for (auto it = committedButUnsafeTxns.begin(); it != committedButUnsafeTxns.end(); ) {
        Transaction* txn = *it;
        bool safe_to_cleanup = true;
        for (Transaction* active : activeTransactions) {
            if (active->start_time < txn->start_time) {
                safe_to_cleanup = false;
                break;
            }
        }
        if (safe_to_cleanup) {
            cleanupTransaction(txn);
            it = committedButUnsafeTxns.erase(it);
        } else {
            ++it;
        }
    }
}

bool tryCommit(Transaction* txn) {
    // SSI Danger Check: Two adjacent rw-antidependencies
    bool has_in = !txn->in_conflicts.empty();
    bool has_out = !txn->out_conflicts.empty();
    if (has_in && has_out) {
        // Abort: Part of a dangerous structure
        lock_guard<mutex> alock(activeTransactionsMutex);
        activeTransactions.erase(txn);
        cleanupTransaction(txn);
        return false;
    }

    // Commit: Apply writes and update versions
    {
        lock_guard<mutex> slock(sharedMutex);
        for (const auto& [key, value] : txn->write_buffer) {
            shared[key].value = value;
            shared[key].version = shared[key].current_version;
        }
    }

    txn->committed = true;
    
    // Retain locks if concurrent transactions exist
    {
        lock_guard<mutex> alock(activeTransactionsMutex);
        if (!activeTransactions.empty()) {
            committedButUnsafeTxns.insert(txn);
            activeTransactions.erase(txn);
            return true;
        }
    }

    // Safe to clean up if no concurrent transactions
    cleanupTransaction(txn);
    return true;
}

// ---- Benchmark Code ----
void updtMem(int transactionsPerThread) {
    long localCommitTime = 0;
    int localAborts = 0;
    int localCommitted = 0;

    for(int t = 0; t < transactionsPerThread; ++t) {
        bool committed = false;
        int retries = 0;
        long startTs = getSysTime();
        bool read_only = (getRand(100) < readOnlyPercent);

        do {
            Transaction* txn = beginTransaction(read_only);

            // perform fixed numIters operations
            for(int i = 0; i < numIters; ++i) {
                int key = getRand(M);
                int val;
                read(txn, key, val);
                if (!txn->read_only) {
                    val += getRand(constVal);
                    write(txn, key, val);
                }
                this_thread::sleep_for(milliseconds(getExpRand(lambdaVal)));
            }

            committed = tryCommit(txn);
            if (!committed) {
                ++localAborts;
                ++retries;
            }
        } while(!committed && retries < 10);

        if (committed) {
            localCommitted++;
            localCommitTime += (getSysTime() - startTs);
        }
        // occasionally clean up
        if (t % 10 == 0) {
            garbageCollectOldVersions();
            cleanupCommittedTxns();
        }
    }

    // aggregate into global stats
    lock_guard<mutex> lk(statsLock);
    totalCommitTime += localCommitTime;
    totalAborts     += localAborts;
    committedTxns   += localCommitted;
}

int main(){
    // initialize shared variables
    shared.resize(M);
    for(int i=0;i<M;i++) shared[i].versions.push_back({0,0});

    int perThread = totTrans / numThreads;
    int extra     = totTrans % numThreads;
    vector<thread> th;
    for(int i=0;i<numThreads;i++){
        int cnt = perThread + (i<extra ? 1:0);
        th.emplace_back(updtMem, cnt);
    }
    for(auto &t: th) t.join();

    cout << "=== SSI Benchmark Results ===\n";
    cout << "Total transactions:       " << totTrans << "\n";
    cout << "Read-only %:             " << readOnlyPercent << "%\n";
    cout << "Average commit delay:    " 
         << (double)totalCommitTime/committedTxns << " ms\n";
    cout << "Abort rate:              " 
         << (double)totalAborts/(totalAborts+committedTxns)*100 << "%\n";
    cout << "Throughput:              " 
         << (double)committedTxns/(totalCommitTime/1000.0) << " txns/sec\n";
    return 0;
}
