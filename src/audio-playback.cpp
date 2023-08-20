// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-process.hpp"

#include <algorithm>

static void balanceDevicePlaybackSpeed(DeviceAudio* const dev,
                                       VResampler* const resampler,
                                       const snd_pcm_sframes_t avail)
{
    const uint32_t frame = dev->frame;
    const uint16_t bufferSize = dev->bufferSize;
    const uint32_t sampleRate = dev->sampleRate;

    DeviceAudio::Balance& bal(dev->balance);

    const uint16_t kSpeedTarget = static_cast<double>(sampleRate) / bufferSize * 8;
    const uint16_t kMaxTarget = bufferSize * 2;
    const uint16_t kMinTarget = bufferSize * 1.25;

    if (avail > kMaxTarget)
    {
        if (bal.slowingDown == 0)
        {
            switch (bal.mode)
            {
            case kBalanceNormal:
            case kBalanceSlowingDown:
                bal.ratio *= 1.000005;
                break;
            case kBalanceSpeedingUp:
                bal.ratio = 1.0;
                break;
            }
            bal.mode = kBalanceSlowingDown;
            bal.slowingDown = 1;
            bal.speedingUp = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
            if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | playback | %.9f = %.9f * %.9f | %ld | avail > kMaxTarget slowing down...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            }
        }
        else if (++bal.slowingDown == kSpeedTarget)
        {
            bal.slowingDown = 1;
            if (d_isNotEqual(bal.ratio, 1.005))
            {
                bal.ratio = (bal.ratio * 3 + 1.005) / 4;
                DEBUGPRINT("%08u | playback | %.9f = %.9f * %.9f | %ld | avail > kMaxTarget | slowing down even more...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            }
        }
    }
    else if (avail < kMinTarget)
    {
        if (bal.speedingUp == 0)
        {
            switch (bal.mode)
            {
            case kBalanceNormal:
            case kBalanceSpeedingUp:
                bal.ratio *= 0.999995;
                break;
            case kBalanceSlowingDown:
                bal.ratio = 1.0;
                break;
            }
            bal.mode = kBalanceSpeedingUp;
            bal.speedingUp = 1;
            bal.slowingDown = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
            if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | playback | %.9f = %.9f * %.9f | %ld | avail < kMinTarget | speeding up...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            }
        }
        else if (++bal.speedingUp >= kSpeedTarget)
        {
            bal.speedingUp = 1;
            if (d_isNotEqual(bal.ratio, 0.9995))
            {
                bal.ratio = (bal.ratio * 3 + 0.9995) / 4;
                DEBUGPRINT("%08u | playback | %.9f = %.9f * %.9f | %ld | avail < kMinTarget | speeding up even more...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            }
        }
    }
    else
    {
        if ((bal.slowingDown != 0 && ++bal.slowingDown == kSpeedTarget) || (bal.speedingUp != 0 && ++bal.speedingUp == kSpeedTarget))
        {
            DEBUGPRINT("%08u | playback | %.9f = %.9f * %.9f | %ld | stopped speed compensation",
                       frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            bal.mode = kBalanceNormal;
            bal.slowingDown = bal.speedingUp = 0;
        }
    }
}

static void* devicePlaybackThread(void* const  arg)
{
    DeviceAudio* const dev = static_cast<DeviceAudio*>(arg);

    int err;
    uint16_t frames;
    uint32_t frame;
    uint32_t frameCount = 0;

    const uint8_t hints = dev->hints;
    const uint8_t channels = dev->channels;
    const uint8_t sampleSize = getSampleSizeFromHints(hints);
    const uint16_t bufferSize = dev->bufferSize;
    const uint16_t extraBufferSize = bufferSize / 2;
    const uint32_t periodTimeOver4 = ((bufferSize / 4) * 1000000) / dev->sampleRate * 1000;

    float** buffers = new float*[channels];
    for (uint8_t c=0; c<channels; ++c)
        buffers[c] = new float[bufferSize * 2];

    bool again = false, empty;
    snd_pcm_uframes_t avail = 0;
    snd_htimestamp_t tstamp = {};

    // disable denormals and enable flush to zero
    {
       #if defined(__SSE2_MATH__)
        _mm_setcsr(_mm_getcsr() | 0x8040);
       #elif defined(__aarch64__)
        uint64_t flags;
        __asm__ __volatile__("mrs %0, fpcr" : "=r" (flags));
        __asm__ __volatile__("msr fpcr, %0" :: "r" (flags | 0x1000000));
       #elif defined(__arm__) && !defined(__SOFTFP__)
        uint32_t flags;
        __asm__ __volatile__("vmrs %0, fpscr" : "=r" (flags));
        __asm__ __volatile__("vmsr fpscr, %0" :: "r" (flags | 0x1000000));
       #endif
    }

    VResampler* const resampler = new VResampler;
    resampler->setup(1.0, channels, 8);

    struct timespec ts = {};
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 15;

    if (sem_timedwait(&dev->sem, &ts) != 0)
    {
        return nullptr;
    }

    while (dev->channels != 0)
    {
        if (dev->ringbuffers[0].getReadableDataSize() == 0)
        {
//             DEBUGPRINT("%08u | has no data", frame);
//             clock_gettime(CLOCK_REALTIME, &ts);
//             ts.tv_nsec += periodTimeOver4;
//             if (ts.tv_nsec >= 1000000000ULL)
//             {
//                 ts.tv_sec += 1;
//                 ts.tv_nsec -= 1000000000ULL;
//             }
//             sem_timedwait(&dev->sem, &ts);
            sem_wait(&dev->sem);
            again = true;
            continue;
        }

        for (uint8_t c=0; c<channels; ++c)
        {
            while (!dev->ringbuffers[c].readCustomData(buffers[c], sizeof(float) * bufferSize / 2))
            {
                DEBUGPRINT("%08u |failed reading data", frame);
                sched_yield();
                empty = true;
            }
        }

        if (dev->channels == 0)
            break;

        resampler->inp_count = bufferSize / 2;
        resampler->out_count = bufferSize * 2;
        resampler->inp_data = buffers;
        resampler->out_data = dev->buffers.f32;
        resampler->process();

        if (resampler->inp_count != 0)
        {
            printf("%08u | playback | E1 resampler->out_count == %u | resampler->inp_count == %u | avail %ld\n",
                frame, resampler->out_count, resampler->inp_count, avail);
            exit(EXIT_FAILURE);
        }
        else if (resampler->out_count == 0)
        {
            printf("%08u | playback | E2 resampler->out_count == %u | resampler->inp_count == %u | avail %ld\n",
                frame, resampler->out_count, resampler->inp_count, avail);
            exit(EXIT_FAILURE);
        }
        else if (resampler->out_count != 128)
        {
            // printf("%08u | E2 resampler->out_count == %u | resampler->inp_count == %u | avail %ld\n",
                    // frame, resampler->out_count, resampler->inp_count, avail);
        }

        frames = bufferSize * 2 - resampler->out_count;

        switch (hints & kDeviceSampleHints)
        {
        case kDeviceSample16:
            float2int::s16(dev->buffers.raw, dev->buffers.f32, channels, frames);
            break;
        case kDeviceSample24:
            float2int::s24(dev->buffers.raw, dev->buffers.f32, channels, frames);
            break;
        case kDeviceSample24LE3:
            float2int::s24le3(dev->buffers.raw, dev->buffers.f32, channels, frames);
            break;
        case kDeviceSample32:
            float2int::s32(dev->buffers.raw, dev->buffers.f32, channels, frames);
            break;
        default:
            DEBUGPRINT("unknown format");
            break;
        }

        int8_t* ptr = dev->buffers.raw;

        while (frames != 0)
        {
            err = snd_pcm_mmap_writei(dev->pcm, ptr, frames);
            // DEBUGPRINT("write %d of %u", err, frames);

            if (err == -EAGAIN)
            {
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += periodTimeOver4;
                if (ts.tv_nsec >= 1000000000ULL)
                {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= 1000000000ULL;
                }
                sem_timedwait(&dev->sem, &ts);
    //             sem_wait(&dev->sem);
                again = true;
                continue;
            }

            if (err < 0)
            {
                deviceFailInitHints(dev);
                resampler->set_rratio(1.0);

                printf("%08u | playback | Write error: %s\n", frame, snd_strerror(err));

                if (xrun_recovery(dev->pcm, err) < 0)
                {
                    printf("playback | xrun_recovery error: %s\n", snd_strerror(err));
                    exit(EXIT_FAILURE);
                }

                again = false;
                break;
            }

            if (again /*|| (static_cast<uint16_t>(err) == frames && ptr == dev->buffers.raw)*/)
            {
                if (snd_pcm_htimestamp(dev->pcm, &avail, &tstamp) != 0)
                {
                    DEBUGPRINT("snd_pcm_htimestamp failed");
                    break;
                }

                snd_pcm_sframes_t savail = 0;
                savail += dev->ringbuffers[0].getReadableDataSize() /  sizeof(float);
                savail += frames;
//                 savail += bufferSize / 2;

                if ((dev->hints & kDeviceStarting) == 0)
                    balanceDevicePlaybackSpeed(dev, resampler, savail);

                if (dev->timestamps.alsaStartTime == 0)
                {
                    dev->timestamps.alsaStartTime = static_cast<uint64_t>(tstamp.tv_sec) * 1000000000ULL + tstamp.tv_nsec;
                    dev->timestamps.jackStartFrame = frame;
                }
                else if (dev->timestamps.alsaStartTime != 0 && frame != 0 && dev->timestamps.jackStartFrame != frame)
                {
                    const uint64_t alsadiff = static_cast<uint64_t>(tstamp.tv_sec) * 1000000000ULL + tstamp.tv_nsec - dev->timestamps.alsaStartTime;
                    const uint32_t alsaframes = alsadiff * dev->sampleRate / 1000000000ULL;
                    const uint32_t jackframes = frame - dev->timestamps.jackStartFrame;
                    dev->timestamps.ratio = ((static_cast<double>(alsaframes) / jackframes) + dev->timestamps.ratio * 511) / 512;
                    resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);
                    if ((frameCount % dev->sampleRate) == 0)
                    {
                        DEBUGPRINT("%08u | %s | %.09f = %.09f * %.09f | %ld avail | mode: %s",
                                frame, dev->hints & kDeviceCapture ? "capture" : "playback",
                                dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, dev->balance.ratio, savail, BalanceModeToStr(dev->balance.mode));
                    }
                }
            }

            if (static_cast<uint16_t>(err) == frames)
            {
                if (dev->hints & kDeviceInitializing)
                    dev->hints &= ~kDeviceInitializing;
                else
                    dev->hints &= ~kDeviceStarting;

    //             if (retries != 0)
    //             {
    //                 if (dev->balance.mode != kBalanceSpeedingUpRealFast || dev->balance.ratio > 0.9995)
    //                     dev->balance.ratio = 0.9995;
    //                 else
    //                     dev->balance.ratio *= 0.999995;
    //                 dev->balance.mode = kBalanceSpeedingUpRealFast;
    //                 dev->balance.speedingUpRealFast = dev->balance.speedingUp = 1;
    //                 dev->balance.slowingDown = dev->balance.slowingDownRealFast = 0;
    //                 resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);
    //                 DEBUGPRINT("%08u | playback | %.9f | speeding up real fast...", frame, dev->balance.ratio);
    //             }
                break;
            }

            ptr += err * channels * sampleSize;
            frames -= err;

            DEBUGPRINT("%08u | playback | Incomplete write %d of %u, %u left",
                       frame, err, bufferSize, frames);
        }

        frame = dev->frame;

        again = empty;
        frameCount += frames;

        if (empty)
        {
            dev->balance.mode = kBalanceSlowingDown;
            dev->balance.ratio = std::min(1.005, (dev->balance.ratio * 3 + 1.005) / 4);
            dev->balance.slowingDown = 1;
            dev->balance.speedingUp = 0;
            resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);
            DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | slowing down real fast...",
                       frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, dev->balance.ratio);

            sem_wait(&dev->sem);
        }
    }

#if 0
    VResampler* const resampler = dev->resampler;

    uint16_t retries = 0;
    snd_pcm_uframes_t avail = 0;

    if (frame == 0)
    {
        DEBUGPRINT("%08u | playback | frame == 0 | %ld", frame, avail);
        snd_pcm_forward(dev->pcm, bufferSize);
        avail = snd_pcm_avail(dev->pcm);
    }

    if (!(hints & kDeviceStarting))
    {
    }

    int err;
#endif

    return nullptr;
}

static void runDeviceAudioPlayback(DeviceAudio* const dev, float* buffers[], const uint32_t frame)
{
    const uint16_t bufferSize = dev->bufferSize;
    const uint16_t halfBufferSize = bufferSize / 2;
    const uint8_t channels = dev->channels;

    // TODO stop trying at some point

    for (uint8_t c=0; c<channels; ++c)
    {
        while (!dev->ringbuffers[c].writeCustomData(buffers[c], sizeof(float) * halfBufferSize))
            simd_yield();
    }

    for (uint8_t c=0; c<channels; ++c)
        dev->ringbuffers[c].commitWrite();

    sem_post(&dev->sem);

    for (uint8_t c=0; c<channels; ++c)
    {
        while (!dev->ringbuffers[c].writeCustomData(buffers[c] + halfBufferSize, sizeof(float) * halfBufferSize))
            simd_yield();
    }

    for (uint8_t c=0; c<channels; ++c)
        dev->ringbuffers[c].commitWrite();

    sem_post(&dev->sem);
}
