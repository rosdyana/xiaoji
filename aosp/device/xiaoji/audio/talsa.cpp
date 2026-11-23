/*
 * XiaoJi null-sink PCM backend (see talsa.h). Writes are paced to the sample rate and
 * discarded; reads are paced and return silence. Output is discarded (the engine does not
 * play it yet); the pacing keeps AudioFlinger's mixer thread on a real-time schedule.
 *
 * Copyright (C) 2026 XiaoJi
 * Licensed under the Apache License, Version 2.0.
 */

#include <chrono>
#include <cstring>
#include <thread>

#include <log/log.h>
#include "talsa.h"

namespace android {
namespace hardware {
namespace audio {
namespace CPP_VERSION {
namespace implementation {
namespace talsa {

/* One open stream. Sixteen-bit samples, interleaved, as the HAL configures them. */
struct xiaoji_pcm {
    bool isOut;
    unsigned int channels;
    size_t rate;
    size_t frameCount;
};

namespace {

/* Sleeps for the time count bytes of audio take at the stream's rate. */
void pace(const pcm_t *pcm, unsigned int count) {
    const size_t bytesPerFrame = pcm->channels * sizeof(int16_t);
    const size_t frames = count / bytesPerFrame;
    const auto ns = std::chrono::nanoseconds(static_cast<int64_t>(frames) * 1000000000LL / pcm->rate);
    std::this_thread::sleep_for(ns);
}

}  // namespace

void PcmDeleter::operator()(pcm_t *x) const { delete x; }

std::unique_ptr<pcm_t, PcmDeleter> pcmOpen(const unsigned int dev,
                                           const unsigned int card,
                                           const unsigned int nChannels,
                                           const size_t sampleRateHz,
                                           const size_t frameCount,
                                           const bool isOut) {
    (void)dev;
    (void)card;
    if (nChannels == 0 || sampleRateHz == 0 || frameCount == 0) {
        ALOGE("%s:%d null sink: bad configuration nChannels=%u sampleRateHz=%zu frameCount=%zu",
              __func__, __LINE__, nChannels, sampleRateHz, frameCount);
        return PcmPtr(nullptr);
    }
    return PcmPtr(new pcm_t{isOut, nChannels, sampleRateHz, frameCount});
}

bool pcmPrepare(pcm_t *pcm) { return pcm != nullptr; }

bool pcmStart(pcm_t *pcm) { return pcm != nullptr; }

bool pcmStop(pcm_t *pcm) { return pcm != nullptr; }

bool pcmRead(pcm_t *pcm, void *data, unsigned int count) {
    if (!pcm || pcm->isOut || !data) {
        return false;
    }
    std::memset(data, 0, count);
    pace(pcm, count);
    return true;
}

bool pcmWrite(pcm_t *pcm, const void *data, unsigned int count) {
    if (!pcm || !pcm->isOut || !data) {
        return false;
    }
    pace(pcm, count);
    return true;
}

Mixer::Mixer(unsigned card) { (void)card; }

Mixer::~Mixer() {}

}  // namespace talsa
}  // namespace implementation
}  // namespace CPP_VERSION
}  // namespace audio
}  // namespace hardware
}  // namespace android
