// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sys/types.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace huawei::wifi::tofu {

constexpr size_t kMaxCertificateSize = 65536;
constexpr uint32_t kErrorDepth = UINT32_MAX;

class Fd {
   public:
    explicit Fd(int fd = -1) : mFd(fd) {}
    ~Fd();
    Fd(Fd&& other) noexcept : mFd(other.release()) {}
    Fd& operator=(Fd&& other) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return mFd; }
    int release();

   private:
    int mFd;
};

enum class ParseResult { Ignored, Certificate, Invalid };
ParseResult parseCertificateEvent(const std::string& event, uint32_t* depth,
                                  std::vector<uint8_t>* certificate);

// A connected datagram socket. Every reply/event must carry the same kernel PID/UID.
class ControlSocket {
   public:
    static ControlSocket open(const std::string& path, uid_t wifiUid = 1010);
    bool valid() const { return mFd.get() >= 0; }
    int fd() const { return mFd.get(); }
    bool command(const std::string& command, std::string* reply);
    bool receive(std::string* event);

   private:
    Fd mFd;
    uid_t mWifiUid = 1010;
    pid_t mPeerPid = 0;
    uid_t mPeerUid = 0;
};

ControlSocket openAcquisitionMonitor(const std::string& path, int networkId, uid_t wifiUid = 1010);

// Single owner joins before closing descriptors; no close/read descriptor-reuse race.
class MonitorSession {
   public:
    MonitorSession(ControlSocket control, Fd stream);
    ~MonitorSession();
    void start();

   private:
    void run();
    bool sendFrame(uint32_t depth, const std::vector<uint8_t>& certificate);
    ControlSocket mControl;
    Fd mStream;
    std::atomic<bool> mStop{false};
    std::thread mThread;
};
}  // namespace huawei::wifi::tofu
