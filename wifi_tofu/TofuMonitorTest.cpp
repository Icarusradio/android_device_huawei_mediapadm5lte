// SPDX-License-Identifier: Apache-2.0
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <mutex>

#include "TofuMonitor.h"

namespace huawei::wifi::tofu {
namespace {
class FakeSupplicant {
   public:
    explicit FakeSupplicant(bool disabled = true) {
        mDisabled = disabled;
        char directory[] = "/tmp/huawei-tofu-test-XXXXXX";
        const char* result = mkdtemp(directory);
        if (!result) return;
        mDirectory = result;
        path = mDirectory + "/wlan0";
        mSocket = Fd(socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (bind(mSocket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address))) return;
        mThread = std::thread([this] {
            while (!mStop) {
                pollfd descriptor{mSocket.get(), POLLIN, 0};
                if (poll(&descriptor, 1, 50) <= 0) continue;
                char buffer[256];
                sockaddr_un peer{};
                socklen_t size = sizeof(peer);
                ssize_t count = recvfrom(mSocket.get(), buffer, sizeof(buffer), 0,
                                         reinterpret_cast<sockaddr*>(&peer), &size);
                if (count <= 0) continue;
                {
                    std::lock_guard lock(mMutex);
                    mPeer = peer;
                    mPeerSize = size;
                }
                std::string command(buffer, count);
                std::string reply = command == "PING" ? "PONG\n" : "OK\n";
                if (command == "GET_NETWORK 7 disabled") reply = mDisabled ? "1" : "0";
                if (command == "GET_NETWORK 7 key_mgmt") reply = "WPA-EAP";
                sendto(mSocket.get(), reply.data(), reply.size(), 0,
                       reinterpret_cast<sockaddr*>(&peer), size);
            }
        });
    }
    ~FakeSupplicant() {
        mStop = true;
        if (mThread.joinable()) mThread.join();
        unlink(path.c_str());
        rmdir(mDirectory.c_str());
    }
    ssize_t sendEvent(const std::string& event) {
        std::lock_guard lock(mMutex);
        return sendto(mSocket.get(), event.data(), event.size(), 0,
                      reinterpret_cast<sockaddr*>(&mPeer), mPeerSize);
    }
    bool sendFromDifferentProcess(const std::string& event) {
        sockaddr_un peer;
        socklen_t peerSize;
        {
            std::lock_guard lock(mMutex);
            peer = mPeer;
            peerSize = mPeerSize;
        }
        pid_t child = fork();
        if (child < 0) return false;
        if (!child) {
            ssize_t count = sendto(mSocket.get(), event.data(), event.size(), 0,
                                   reinterpret_cast<sockaddr*>(&peer), peerSize);
            _exit(count == static_cast<ssize_t>(event.size()) ? 0 : 1);
        }
        int status;
        return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    std::string path;

   private:
    std::string mDirectory;
    Fd mSocket;
    sockaddr_un mPeer{};
    socklen_t mPeerSize = 0;
    std::mutex mMutex;
    std::atomic<bool> mStop{false};
    std::thread mThread;
    bool mDisabled;
};

bool readBytes(int fd, void* buffer, size_t size) {
    size_t total = 0;
    while (total < size) {
        pollfd descriptor{fd, POLLIN, 0};
        if (poll(&descriptor, 1, 1500) <= 0) return false;
        ssize_t count = read(fd, static_cast<char*>(buffer) + total, size - total);
        if (count <= 0) return false;
        total += count;
    }
    return true;
}

TEST(CertificateParser, SeparatesMetadataAndPreservesExactDer) {
    uint32_t depth;
    std::vector<uint8_t> certificate;
    EXPECT_EQ(
        ParseResult::Ignored,
        parseCertificateEvent("<3>CTRL-EVENT-EAP-PEER-CERT depth=0 subject='CN=server' hash=1234",
                              &depth, &certificate));
    EXPECT_EQ(ParseResult::Certificate,
              parseCertificateEvent(
                  "<3>CTRL-EVENT-EAP-PEER-CERT depth=2 subject='CN=O'Reilly' cert=00aB19", &depth,
                  &certificate));
    EXPECT_EQ(2u, depth);
    EXPECT_EQ((std::vector<uint8_t>{0, 0xab, 0x19}), certificate);
    EXPECT_EQ(ParseResult::Ignored,
              parseCertificateEvent("<3>CTRL-EVENT-EAP-FAILURE", &depth, &certificate));
}

TEST(CertificateParser, RejectsMalformedAndUnboundedInput) {
    uint32_t depth;
    std::vector<uint8_t> certificate;
    for (const std::string suffix :
         {"-1 subject='x' cert=30", "101 subject='x' cert=30",
          "99999999999999999999 subject='x' cert=30", "0 subject='x' cert=3",
          "0 subject='x' cert=xy", "0 subject='x' cert=", "0 subject='x' cert=30\n"}) {
        EXPECT_EQ(ParseResult::Invalid,
                  parseCertificateEvent("<3>CTRL-EVENT-EAP-PEER-CERT depth=" + suffix, &depth,
                                        &certificate));
    }
    EXPECT_EQ(ParseResult::Invalid,
              parseCertificateEvent("<3>CTRL-EVENT-EAP-PEER-CERT depth=0 subject='x' cert=" +
                                        std::string(2 * kMaxCertificateSize + 2, '0'),
                                    &depth, &certificate));
}

TEST(ControlSocket, AuthenticatesPeerAndRejectsChangedPid) {
    FakeSupplicant daemon;
    auto control = ControlSocket::open(daemon.path, getuid());
    ASSERT_TRUE(control.valid());
    ASSERT_TRUE(daemon.sendFromDifferentProcess(
        "<3>CTRL-EVENT-EAP-PEER-CERT depth=0 subject='x' cert=3000"));
    std::string event;
    EXPECT_FALSE(control.receive(&event));
}

TEST(ControlSocket, RejectsTruncatedDatagram) {
    FakeSupplicant daemon;
    auto control = ControlSocket::open(daemon.path, getuid());
    ASSERT_TRUE(control.valid());
    std::string oversized(2 * kMaxCertificateSize + 4096, 'a');
    ASSERT_EQ(static_cast<ssize_t>(oversized.size()), daemon.sendEvent(oversized));
    std::string event;
    EXPECT_FALSE(control.receive(&event));
}

TEST(ControlSocket, AcquisitionRequiresDisabledEapNetwork) {
    FakeSupplicant disabled;
    EXPECT_TRUE(openAcquisitionMonitor(disabled.path, 7, getuid()).valid());
    FakeSupplicant enabled(false);
    EXPECT_FALSE(openAcquisitionMonitor(enabled.path, 7, getuid()).valid());
    EXPECT_FALSE(openAcquisitionMonitor(disabled.path, -1, getuid()).valid());
}

TEST(MonitorSession, TransfersCertificatesInOrderAndHandlesLargeEvents) {
    FakeSupplicant daemon;
    auto control = ControlSocket::open(daemon.path, getuid());
    ASSERT_TRUE(control.valid());
    int pair[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair));
    Fd client(pair[0]);
    MonitorSession session(std::move(control), Fd(pair[1]));
    session.start();
    for (uint32_t depth : {2, 1, 0}) {
        const size_t size = depth ? 32 : 16384;
        std::string event = "<3>CTRL-EVENT-EAP-PEER-CERT depth=" + std::to_string(depth) +
                            " subject='CN=test' cert=" + std::string(2 * size, 'a');
        ASSERT_EQ(static_cast<ssize_t>(event.size()), daemon.sendEvent(event));
        uint32_t header[2];
        ASSERT_TRUE(readBytes(client.get(), header, sizeof(header)));
        ASSERT_EQ(depth, ntohl(header[0]));
        ASSERT_EQ(size, ntohl(header[1]));
        std::vector<uint8_t> bytes(size);
        ASSERT_TRUE(readBytes(client.get(), bytes.data(), bytes.size()));
        EXPECT_EQ(std::vector<uint8_t>(size, 0xaa), bytes);
    }
}

TEST(MonitorSession, MalformedCertificateFailsClosed) {
    FakeSupplicant daemon;
    auto control = ControlSocket::open(daemon.path, getuid());
    ASSERT_TRUE(control.valid());
    int pair[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair));
    Fd client(pair[0]);
    MonitorSession session(std::move(control), Fd(pair[1]));
    session.start();
    ASSERT_GT(daemon.sendEvent("<3>CTRL-EVENT-EAP-PEER-CERT depth=0 subject='x' cert=broken"), 0);
    uint32_t header[2];
    ASSERT_TRUE(readBytes(client.get(), header, sizeof(header)));
    EXPECT_EQ(kErrorDepth, ntohl(header[0]));
    EXPECT_EQ(0u, ntohl(header[1]));
    char byte;
    EXPECT_EQ(0, read(client.get(), &byte, 1));
}

TEST(MonitorSession, ClientCancellationDoesNotWaitForServer) {
    FakeSupplicant daemon;
    auto control = ControlSocket::open(daemon.path, getuid());
    ASSERT_TRUE(control.valid());
    int pair[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair));
    auto session = std::make_unique<MonitorSession>(std::move(control), Fd(pair[1]));
    session->start();
    close(pair[0]);
    auto start = std::chrono::steady_clock::now();
    session.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

TEST(MonitorSession, ChangedCertificateAtSameDepthFailsClosed) {
    FakeSupplicant daemon;
    auto control = ControlSocket::open(daemon.path, getuid());
    ASSERT_TRUE(control.valid());
    int pair[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair));
    Fd client(pair[0]);
    MonitorSession session(std::move(control), Fd(pair[1]));
    session.start();
    for (const char* der : {"3000", "3000", "3001"}) {
        ASSERT_GT(daemon.sendEvent(
                      std::string("<3>CTRL-EVENT-EAP-PEER-CERT depth=1 subject='x' cert=") + der),
                  0);
    }
    uint32_t header[2];
    ASSERT_TRUE(readBytes(client.get(), header, sizeof(header)));
    ASSERT_EQ(2u, ntohl(header[1]));
    uint8_t der[2];
    ASSERT_TRUE(readBytes(client.get(), der, sizeof(der)));
    ASSERT_TRUE(readBytes(client.get(), header, sizeof(header)));
    EXPECT_EQ(kErrorDepth, ntohl(header[0]));
    EXPECT_EQ(0u, ntohl(header[1]));
}
}  // namespace
}  // namespace huawei::wifi::tofu
