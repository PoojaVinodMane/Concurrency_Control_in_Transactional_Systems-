# SSI Algorithm Benchmark

## 📌 Overview
This project implements and benchmarks the **Serializable Snapshot Isolation (SSI)** algorithm.  
The program simulates concurrent transactions with configurable parameters such as number of variables, number of threads, transaction types, and workload distribution.  

The benchmark evaluates:
- Transaction throughput  
- Abort rates due to conflicts  
- Average commit delay  
- Performance under different workload configurations  

---

## 🖥️ Running the Program

### 1. Compile
```bash
g++ Source-CS22BTECH11046_CS22BTECH11035.cpp
// Configurable parameters
int M               = 1000;  // Number of shared variables
int totTrans        = 1000;  // Total number of transactions
int constVal        = 1000;  // Maximum write value
int lambdaVal       = 20;    // For exponential sleep (inter-arrival delay)
int numIters        = 20;    // Fixed operations per transaction
int numThreads      = 16;    // Number of worker threads (N)
int readOnlyPercent = 80;    // Percentage of read-only transactions
=== SSI Benchmark Results(example) ===
Total transactions:       1000
Read-only %:             80%
Average commit delay:    2.00902 ms
Abort rate:              24.1641%
Throughput:              497.756 txns/sec

