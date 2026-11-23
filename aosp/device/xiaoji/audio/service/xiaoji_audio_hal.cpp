/*
 * XiaoJi audio HAL service. The stock hardware/interfaces/audio service main aborts in this
 * tree: it calls ProcessState::startThreadPool() on the vndbinder before
 * ABinderProcess_setThreadPoolMaxThreadCount(1), and libbinder fatals on a shrink after the
 * pool started (the probe in docs/boot.md, B12). This service has no vndbinder (every HAL it
 * serves is HIDL, on hwbinder), so that path is not there. It registers the same passthrough
 * interfaces as the stock service, in the same order, and joins the hwbinder pool.
 *
 * Copyright (C) 2026 XiaoJi
 * Licensed under the Apache License, Version 2.0.
 */

#define LOG_TAG "xiaoji_audio_hal"

#include <signal.h>
#include <string>
#include <vector>

#include <hidl/HidlTransportSupport.h>
#include <hidl/LegacySupport.h>
#include <log/log.h>

using namespace android::hardware;

using InterfacesList = std::vector<std::string>;

/** Registers the first interface in the list that has an implementation; true if one did. */
template <class Iter>
static bool registerPassthroughServiceImplementations(Iter first, Iter last) {
    for (; first != last; ++first) {
        if (registerPassthroughServiceImplementation(*first) == android::OK) {
            return true;
        }
    }
    return false;
}

int main(int /* argc */, char* /* argv */ []) {
    signal(SIGPIPE, SIG_IGN);

    configureRpcThreadpool(16, true /*callerWillJoin*/);

    // The mandatory families are the stock service's, without the ones this product does not use
    // (soundtrigger, Bluetooth): a family here with no implementation stops the service.
    const std::vector<InterfacesList> mandatoryInterfaces = {
        {
            "Audio Core API",
            "android.hardware.audio@7.1::IDevicesFactory",
            "android.hardware.audio@7.0::IDevicesFactory",
        },
        {
            "Audio Effect API",
            "android.hardware.audio.effect@7.0::IEffectsFactory",
        },
    };

    // Soundtrigger (the ranchu passthrough library, as the stock service registers it). The framework's
    // SoundTriggerMiddlewareService connects to it on the system server's main thread at boot, and waits
    // without a timeout when no instance is registered (a watchdog kill, docs/boot.md B13).
    const std::vector<InterfacesList> optionalInterfaces = {
        {
            "Soundtrigger API",
            "android.hardware.soundtrigger@2.3::ISoundTriggerHw",
            "android.hardware.soundtrigger@2.2::ISoundTriggerHw",
            "android.hardware.soundtrigger@2.1::ISoundTriggerHw",
            "android.hardware.soundtrigger@2.0::ISoundTriggerHw",
        },
    };

    for (const auto& listIter : mandatoryInterfaces) {
        auto iter = listIter.begin();
        const std::string& interfaceFamilyName = *iter++;
        LOG_ALWAYS_FATAL_IF(!registerPassthroughServiceImplementations(iter, listIter.end()),
                            "Could not register %s", interfaceFamilyName.c_str());
    }

    for (const auto& listIter : optionalInterfaces) {
        auto iter = listIter.begin();
        const std::string& interfaceFamilyName = *iter++;
        ALOGW_IF(!registerPassthroughServiceImplementations(iter, listIter.end()),
                 "Could not register %s", interfaceFamilyName.c_str());
    }

    joinRpcThreadpool();
    return 0;
}
