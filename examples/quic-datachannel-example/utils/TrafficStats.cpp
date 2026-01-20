#include "TrafficStats.hpp"
#include <algorithm>

TrafficStats::TrafficStats() {}

void TrafficStats::reset() {
    std::lock_guard<std::mutex> lock(mMutex);
    mTotalBytes = 0;
    mTotalMessages = 0;
    mActive = false;
    mTestBytes = 0;
    mTestMessages = 0;
}

void TrafficStats::addBytes(size_t bytes) {
    std::lock_guard<std::mutex> lock(mMutex);
    mTotalBytes += bytes;
    mTotalMessages++;
    
    if (mActive) {
        mTestBytes += bytes;
        mTestMessages++;
    }
}

void TrafficStats::startTest(const std::string& name, uint64_t seq) {
    std::lock_guard<std::mutex> lock(mMutex);
    mActive = true;
    mTestName = name;
    mTestSeq = seq;
    mTestBytes = 0;
    mTestMessages = 0;
    mStartTime = std::chrono::steady_clock::now();
}

void TrafficStats::endTest() {
    std::lock_guard<std::mutex> lock(mMutex);
    mActive = false;
}

bool TrafficStats::isActive() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mActive;
}

std::string TrafficStats::getTestName() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mTestName;
}

uint64_t TrafficStats::getTestSeq() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mTestSeq;
}

uint64_t TrafficStats::getTestBytes() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mTestBytes;
}

uint64_t TrafficStats::getTestMessages() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mTestMessages;
}

uint64_t TrafficStats::getDurationMs() const {
    std::lock_guard<std::mutex> lock(mMutex);
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - mStartTime).count();
}

uint64_t TrafficStats::getTotalBytes() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mTotalBytes;
}

uint64_t TrafficStats::getTotalMessages() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mTotalMessages;
}

std::string TrafficStats::humanSizeLabel(int bytes) {
    if (bytes % (1024 * 1024) == 0) return std::to_string(bytes / (1024 * 1024)) + "MB";
    if (bytes % 1024 == 0) return std::to_string(bytes / 1024) + "KB";
    return std::to_string(bytes) + "B";
}

std::vector<int> TrafficStats::parseCsvInts(const std::string &csv) {
    std::vector<int> out;
    std::string cur;
    auto pushOne = [&](const std::string &s) {
        if (s.empty()) return;
        try {
            int v = std::stoi(s);
            if (v > 0) out.push_back(v);
        } catch (...) {
            // ignore bad token
        }
    };
    for (char ch : csv) {
        if (ch == ',' || ch == ';' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            pushOne(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    pushOne(cur);
    if (out.empty())
        out.push_back(1024);
    return out;
}
