/*
 * XiaoJi null-sink PCM backend. Replaces the ranchu talsa (tinyalsa over the goldfish audio
 * device). The same functions, with no hardware behind them: output is paced in real time
 * and discarded, input is paced and reads as silence. See docs/boot.md, B13.
 *
 * Copyright (C) 2026 XiaoJi
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <memory>

namespace android {
namespace hardware {
namespace audio {
namespace CPP_VERSION {
namespace implementation {
namespace talsa {

constexpr unsigned int kPcmDevice = 0;
constexpr unsigned int kPcmCard = 0;

typedef struct xiaoji_pcm pcm_t;
struct PcmDeleter { void operator()(pcm_t *x) const; };
typedef std::unique_ptr<pcm_t, PcmDeleter> PcmPtr;
PcmPtr pcmOpen(unsigned int dev, unsigned int card, unsigned int nChannels, size_t sampleRateHz, size_t frameCount, bool isOut);
bool pcmPrepare(pcm_t *pcm);
bool pcmStart(pcm_t *pcm);
bool pcmStop(pcm_t *pcm);
bool pcmRead(pcm_t *pcm, void *data, unsigned int count);
bool pcmWrite(pcm_t *pcm, const void *data, unsigned int count);

/* No mixer: the null sink has no controls. The object is always "open". */
class Mixer {
public:
    Mixer(unsigned card);
    ~Mixer();

    operator bool() const { return true; }

    Mixer(const Mixer &) = delete;
    Mixer &operator=(const Mixer &) = delete;
    Mixer(Mixer &&) = delete;
    Mixer &operator=(Mixer &&) = delete;
};

}  // namespace talsa
}  // namespace implementation
}  // namespace CPP_VERSION
}  // namespace audio
}  // namespace hardware
}  // namespace android
