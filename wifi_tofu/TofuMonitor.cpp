// SPDX-License-Identifier: Apache-2.0
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "TofuMonitor.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

namespace huawei::wifi::tofu {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxEventSize = 2 * kMaxCertificateSize + 2048;
constexpr char kCertificatePrefix[] = "CTRL-EVENT-EAP-PEER-CERT depth=";
std::atomic<unsigned int> gSocketNumber{0};

bool stripPriority(std::string* event) {
    if (event->empty() || event->front() != '<') return true;
    auto end = event->find('>');
    if (end != 2 || (*event)[1] < '0' || (*event)[1] > '7') return false;
    event->erase(0, end + 1);
    return true;
}
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

Fd::~Fd() {
    if (mFd >= 0) close(mFd);
}
int Fd::release() { return std::exchange(mFd, -1); }
Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        if (mFd >= 0) close(mFd);
        mFd = other.release();
    }
    return *this;
}

ParseResult parseCertificateEvent(const std::string& input, uint32_t* depth,
                                  std::vector<uint8_t>* certificate) {
    std::string event = input;
    if (!stripPriority(&event)) return ParseResult::Invalid;
    if (event.compare(0, sizeof(kCertificatePrefix) - 1, kCertificatePrefix)) {
        return ParseResult::Ignored;
    }
    size_t position = sizeof(kCertificatePrefix) - 1;
    size_t start = position;
    uint32_t parsedDepth = 0;
    while (position < event.size() && event[position] >= '0' && event[position] <= '9') {
        parsedDepth = parsedDepth * 10 + event[position++] - '0';
        if (parsedDepth > 100) return ParseResult::Invalid;
    }
    if (position == start || event.compare(position, 10, " subject='")) {
        return ParseResult::Invalid;
    }
    auto marker = event.rfind("' cert=");
    // The separate subject/hash notification is not a complete certificate event.
    if (marker == std::string::npos) return ParseResult::Ignored;
    if (marker < position + 10) return ParseResult::Invalid;
    start = marker + 7;
    size_t length = event.size() - start;
    if (!length || length % 2 || length / 2 > kMaxCertificateSize) {
        return ParseResult::Invalid;
    }
    std::vector<uint8_t> bytes(length / 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        int high = hexDigit(event[start + 2 * i]);
        int low = hexDigit(event[start + 2 * i + 1]);
        if (high < 0 || low < 0) return ParseResult::Invalid;
        bytes[i] = (high << 4) | low;
    }
    *depth = parsedDepth;
    *certificate = std::move(bytes);
    return ParseResult::Certificate;
}

ControlSocket ControlSocket::open(const std::string& path, uid_t wifiUid) {
    ControlSocket result;
    result.mWifiUid = wifiUid;
    sockaddr_un remote{};
    remote.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(remote.sun_path)) return result;
    memcpy(remote.sun_path, path.c_str(), path.size() + 1);
    Fd fd(socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (fd.get() < 0) return result;
    int one = 1;
    int bufferSize = 512 * 1024;
    if (setsockopt(fd.get(), SOL_SOCKET, SO_PASSCRED, &one, sizeof(one))) return result;
    if (setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize))) {
        return result;
    }
    sockaddr_un local{};
    local.sun_family = AF_UNIX;
    std::string name =
        "huawei-wifi-tofu-" + std::to_string(getpid()) + "-" + std::to_string(++gSocketNumber);
    memcpy(local.sun_path + 1, name.data(), name.size());
    if (bind(fd.get(), reinterpret_cast<sockaddr*>(&local),
             offsetof(sockaddr_un, sun_path) + 1 + name.size()))
        return result;
    if (connect(fd.get(), reinterpret_cast<sockaddr*>(&remote), sizeof(remote))) return result;
    result.mFd = std::move(fd);
    std::string reply;
    if (!result.command("PING", &reply) || reply != "PONG\n") return ControlSocket();
    return result;
}

bool ControlSocket::receive(std::string* event) {
    std::vector<char> data(kMaxEventSize);
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(ucred))]{};
    iovec vector{data.data(), data.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = ancillary;
    message.msg_controllen = sizeof(ancillary);
    ssize_t size = recvmsg(mFd.get(), &message, MSG_DONTWAIT | MSG_TRUNC);
    if (size <= 0 || static_cast<size_t>(size) > data.size() ||
        (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
        return false;
    const ucred* credentials = nullptr;
    for (auto* item = CMSG_FIRSTHDR(&message); item; item = CMSG_NXTHDR(&message, item)) {
        if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_CREDENTIALS &&
            item->cmsg_len == CMSG_LEN(sizeof(ucred))) {
            credentials = reinterpret_cast<const ucred*>(CMSG_DATA(item));
        }
    }
    if (!credentials || credentials->pid <= 0 ||
        (credentials->uid != 0 && credentials->uid != mWifiUid))
        return false;
    if (mPeerPid && (credentials->pid != mPeerPid || credentials->uid != mPeerUid)) {
        return false;
    }
    mPeerPid = credentials->pid;
    mPeerUid = credentials->uid;
    event->assign(data.data(), size);
    return event->find('\0') == std::string::npos;
}

bool ControlSocket::command(const std::string& command, std::string* reply) {
    if (send(mFd.get(), command.data(), command.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(command.size()))
        return false;
    auto deadline = Clock::now() + std::chrono::milliseconds(500);
    while (Clock::now() < deadline) {
        pollfd descriptor{mFd.get(), POLLIN, 0};
        int remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        int ready = poll(&descriptor, 1, remaining > 0 ? remaining : 1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || !(descriptor.revents & POLLIN) || !receive(reply)) return false;
        if (reply->empty() || reply->front() != '<') return true;
    }
    return false;
}

ControlSocket openAcquisitionMonitor(const std::string& path, int networkId, uid_t wifiUid) {
    if (networkId < 0) return ControlSocket();
    auto control = ControlSocket::open(path, wifiUid);
    std::string reply;
    std::string network = "GET_NETWORK " + std::to_string(networkId);
    if (!control.valid() || !control.command(network + " disabled", &reply) ||
        (reply != "1" && reply != "1\n") || !control.command(network + " key_mgmt", &reply) ||
        reply.find("WPA-EAP") == std::string::npos || !control.command("DISCONNECT", &reply) ||
        reply != "OK\n" || !control.command("PMKSA_FLUSH", &reply) || reply != "OK\n" ||
        !control.command("ATTACH", &reply) || reply != "OK\n")
        return ControlSocket();
    return control;
}

MonitorSession::MonitorSession(ControlSocket control, Fd stream)
    : mControl(std::move(control)), mStream(std::move(stream)) {}
MonitorSession::~MonitorSession() {
    mStop = true;
    if (mThread.joinable()) mThread.join();
}
void MonitorSession::start() { mThread = std::thread(&MonitorSession::run, this); }

bool MonitorSession::sendFrame(uint32_t depth, const std::vector<uint8_t>& certificate) {
    uint32_t header[] = {htonl(depth), htonl(certificate.size())};
    std::vector<uint8_t> frame(sizeof(header) + certificate.size());
    memcpy(frame.data(), header, sizeof(header));
    if (!certificate.empty())
        memcpy(frame.data() + sizeof(header), certificate.data(), certificate.size());
    size_t sent = 0;
    auto deadline = Clock::now() + std::chrono::seconds(1);
    while (!mStop && sent < frame.size() && Clock::now() < deadline) {
        ssize_t count = send(mStream.get(), frame.data() + sent, frame.size() - sent,
                             MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count > 0) {
            sent += count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{mStream.get(), POLLOUT, 0};
            if (poll(&descriptor, 1, 100) >= 0) continue;
        }
        return false;
    }
    return sent == frame.size();
}

void MonitorSession::run() {
    auto deadline = Clock::now() + std::chrono::seconds(30);
    std::array<std::vector<uint8_t>, 101> certificates;
    size_t totalBytes = 0;
    while (!mStop && Clock::now() < deadline) {
        pollfd descriptors[] = {{mControl.fd(), POLLIN, 0}, {mStream.get(), POLLIN, 0}};
        int ready = poll(descriptors, 2, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || descriptors[1].revents) break;
        if (!ready) continue;
        if (descriptors[0].revents != POLLIN) break;
        std::string event;
        if (!mControl.receive(&event)) break;
        if (event.find("CTRL-EVENT-TERMINATING") != std::string::npos) break;
        uint32_t depth;
        std::vector<uint8_t> certificate;
        ParseResult parsed = parseCertificateEvent(event, &depth, &certificate);
        if (parsed == ParseResult::Invalid) break;
        if (parsed == ParseResult::Certificate) {
            // Bound memory and queued framework events even for a malicious authentication server.
            if (!certificates[depth].empty()) {
                if (certificates[depth] != certificate) break;
                continue;
            }
            totalBytes += certificate.size();
            if (totalBytes > 1024 * 1024) break;
            certificates[depth] = certificate;
            if (!sendFrame(depth, certificate)) break;
        }
    }
    if (!mStop) sendFrame(kErrorDepth, {});
    shutdown(mStream.get(), SHUT_RDWR);
    std::string reply;
    mControl.command("DETACH", &reply);
    mControl = ControlSocket();
    mStream = Fd();
}
}  // namespace huawei::wifi::tofu
