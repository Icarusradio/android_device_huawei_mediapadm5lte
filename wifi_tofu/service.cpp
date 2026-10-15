// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "HuaweiWifiTofu"
#include <aidl/vendor/lineage/wifi/tofu/BnWifiTofuBridge.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>
#include <sys/socket.h>

#include <memory>
#include <mutex>

#include "TofuMonitor.h"

namespace {
using huawei::wifi::tofu::ControlSocket;
using huawei::wifi::tofu::Fd;
using huawei::wifi::tofu::MonitorSession;
using ndk::ScopedAStatus;

class Bridge : public aidl::vendor::lineage::wifi::tofu::BnWifiTofuBridge {
   public:
    ScopedAStatus isSupported(const std::string& iface, bool* supported) override {
        *supported = false;
        if (AIBinder_getCallingUid() != 1000) return ScopedAStatus::fromExceptionCode(EX_SECURITY);
        if (iface != "wlan0") return ScopedAStatus::ok();
        auto control = ControlSocket::open(kControlPath);
        std::string reply;
        *supported = control.valid() && control.command("ATTACH", &reply) && reply == "OK\n";
        if (*supported) control.command("DETACH", &reply);
        return ScopedAStatus::ok();
    }

    ScopedAStatus openMonitor(const std::string& iface, int32_t networkId,
                              ndk::ScopedFileDescriptor* stream) override {
        if (AIBinder_getCallingUid() != 1000) return ScopedAStatus::fromExceptionCode(EX_SECURITY);
        if (iface != "wlan0" || networkId < 0)
            return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        std::lock_guard lock(mMutex);
        mSession.reset();
        // Only a freshly added, disabled EAP network can be used for certificate acquisition.
        auto control = huawei::wifi::tofu::openAcquisitionMonitor(kControlPath, networkId);
        if (!control.valid()) {
            ALOGE("Cannot prepare credential-free certificate monitor");
            return ScopedAStatus::fromServiceSpecificError(1);
        }
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) {
            return ScopedAStatus::fromServiceSpecificError(2);
        }
        mSession = std::make_unique<MonitorSession>(std::move(control), Fd(sockets[1]));
        mSession->start();
        *stream = ndk::ScopedFileDescriptor(sockets[0]);
        return ScopedAStatus::ok();
    }

   private:
    static constexpr char kControlPath[] = "/data/vendor/wifi/wpa/sockets/wlan0";
    std::mutex mMutex;
    std::unique_ptr<MonitorSession> mSession;
};
}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    auto service = ndk::SharedRefBase::make<Bridge>();
    const std::string instance = std::string(Bridge::descriptor) + "/default";
    if (AServiceManager_addService(service->asBinder().get(), instance.c_str()) != STATUS_OK) {
        ALOGE("Cannot register certificate adapter");
        return 1;
    }
    ALOGI("Huawei HIDL certificate adapter ready");
    ABinderProcess_joinThreadPool();
    return 1;
}
