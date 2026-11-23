// xiaoji_netd: a minimal netd for the xiaoji engine.
//
// The stock netd needs bpf(2), the BPF maps and NETLINK_ROUTE/NETFILTER, none of which the engine
// has (docs/boot.md, B11). This binary registers the same "netd" binder service with the real
// android.net.INetd (V10) interface, and the "dnsresolver" service with android.net.IDnsResolver
// (V10, NDK), which ConnectivityService requires. The boot path gets the answers it needs; the
// other INetd methods are logged and return EX_UNSUPPORTED_OPERATION (see gen_stub.py).
#define LOG_TAG "xiaoji_netd"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <log/log.h>

#include "xiaoji_dnsresolver_gen.h"
#include "xiaoji_netd_gen.h"

using android::binder::Status;
using android::net::InterfaceConfigurationParcel;

namespace xiaoji {

namespace {

constexpr const char kProcSysNet[] = "/proc/sys/net";

// Only plain names: no path separators, no dot-files, no "..".
bool safeComponent(const std::string& s) {
    return !s.empty() && s[0] != '.' && s.find('/') == std::string::npos && s.size() < 256;
}

// The parameter directory for a (ipversion, which) pair, as the stock netd builds it.
bool procSysPath(int32_t ipversion, int32_t which, const std::string& ifname,
                 const std::string& parameter, std::string* path, Status* error) {
    const char* ip = nullptr;
    switch (ipversion) {
        case android::net::INetd::IPV4: ip = "ipv4"; break;
        case android::net::INetd::IPV6: ip = "ipv6"; break;
        default:
            *error = Status::fromServiceSpecificError(EAFNOSUPPORT, "Bad IP version");
            return false;
    }
    const char* category = nullptr;
    switch (which) {
        case android::net::INetd::CONF: category = "conf"; break;
        case android::net::INetd::NEIGH: category = "neigh"; break;
        default:
            *error = Status::fromServiceSpecificError(EINVAL, "Bad category");
            return false;
    }
    if (!safeComponent(ifname) || !safeComponent(parameter)) {
        *error = Status::fromServiceSpecificError(EINVAL, "Bad interface or parameter name");
        return false;
    }
    *path = std::string(kProcSysNet) + "/" + ip + "/" + category + "/" + ifname + "/" + parameter;
    return true;
}

// The engine has /proc/sys/net only when the product provides it. Without it, the
// parameters do not exist, and the framework's writes are accepted as no-ops.
bool procSysNetPresent() {
    struct stat st;
    return stat(kProcSysNet, &st) == 0;
}

}  // namespace

Status XiaojiNetd::isAlive(bool* _aidl_return) {
    *_aidl_return = true;
    return Status::ok();
}

Status XiaojiNetd::interfaceGetList(std::vector<std::string>* _aidl_return) {
    // The engine has the loopback only (no kernel network devices).
    *_aidl_return = {"lo"};
    return Status::ok();
}

Status XiaojiNetd::interfaceGetCfg(const std::string& ifName,
                                   InterfaceConfigurationParcel* _aidl_return) {
    if (ifName != "lo") {
        ALOGW("interfaceGetCfg: no such interface %s", ifName.c_str());
        return Status::fromServiceSpecificError(ENODEV, "No such interface");
    }
    InterfaceConfigurationParcel cfg;
    cfg.ifName = "lo";
    cfg.hwAddr = "00:00:00:00:00:00";
    cfg.ipv4Addr = "127.0.0.1";
    cfg.prefixLength = 8;
    cfg.flags = {"up", "loopback", "running"};
    *_aidl_return = cfg;
    return Status::ok();
}

Status XiaojiNetd::getProcSysNet(int32_t ipversion, int32_t which, const std::string& ifname,
                                 const std::string& parameter, std::string* _aidl_return) {
    std::string path;
    Status error = Status::ok();
    if (!procSysPath(ipversion, which, ifname, parameter, &path, &error)) {
        return error;
    }
    if (!procSysNetPresent()) {
        *_aidl_return = "";
        return Status::ok();
    }
    FILE* f = fopen(path.c_str(), "r");
    if (f == nullptr) {
        return Status::fromServiceSpecificError(errno, "getProcSysNet: open failed");
    }
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    *_aidl_return = buf;
    return Status::ok();
}

Status XiaojiNetd::setProcSysNet(int32_t ipversion, int32_t which, const std::string& ifname,
                                 const std::string& parameter, const std::string& value) {
    std::string path;
    Status error = Status::ok();
    if (!procSysPath(ipversion, which, ifname, parameter, &path, &error)) {
        return error;
    }
    if (!procSysNetPresent()) {
        ALOGI("setProcSysNet: no %s, ignored %s", kProcSysNet, path.c_str());
        return Status::ok();
    }
    FILE* f = fopen(path.c_str(), "w");
    if (f == nullptr) {
        return Status::fromServiceSpecificError(errno, "setProcSysNet: open failed");
    }
    size_t n = fwrite(value.data(), 1, value.size(), f);
    fclose(f);
    if (n != value.size()) {
        return Status::fromServiceSpecificError(EIO, "setProcSysNet: short write");
    }
    return Status::ok();
}

}  // namespace xiaoji

int main() {
    ALOGI("xiaoji_netd starting");
    android::ProcessState::self()->setThreadPoolMaxThreadCount(4);
    android::sp<xiaoji::XiaojiNetd> netd = new xiaoji::XiaojiNetd();
    android::status_t ret = android::defaultServiceManager()->addService(
            android::String16("netd"), netd);
    if (ret != android::OK) {
        ALOGE("addService(netd) failed: %d", ret);
        return 1;
    }
    ALOGI("registered netd");
    ret = android::defaultServiceManager()->addService(android::String16("dnsresolver"),
                                                       new xiaoji::XiaojiDnsResolver());
    if (ret != android::OK) {
        ALOGE("addService(dnsresolver) failed: %d", ret);
        return 1;
    }
    ALOGI("registered dnsresolver");
    android::ProcessState::self()->startThreadPool();
    android::IPCThreadState::self()->joinThreadPool();
    ALOGE("xiaoji_netd: binder thread pool exited");
    return 1;
}
