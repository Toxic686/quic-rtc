#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include <mutex>
#include <vector>
#include <iostream>

class TrafficStats {
public:
    TrafficStats();
    
    void reset();
    void addBytes(size_t bytes);
    
    // Test session management
    void startTest(const std::string& name, uint64_t seq);
    void endTest();
    
    // Getters
    bool isActive() const;
    std::string getTestName() const;
    uint64_t getTestSeq() const;
    uint64_t getTestBytes() const;
    uint64_t getTestMessages() const;
    uint64_t getDurationMs() const;
    
    // Global stats
    uint64_t getTotalBytes() const;
    uint64_t getTotalMessages() const;

    // Static helpers
    static std::string humanSizeLabel(int bytes);
    static std::vector<int> parseCsvInts(const std::string &csv);
    
private:
    mutable std::mutex mMutex;
    uint64_t mTotalBytes = 0;
    uint64_t mTotalMessages = 0;
    
    // Current test state
    bool mActive = false;
    std::string mTestName;
    uint64_t mTestSeq = 0;
    uint64_t mTestBytes = 0;
    uint64_t mTestMessages = 0;
    std::chrono::steady_clock::time_point mStartTime;
};
