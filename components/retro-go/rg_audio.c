#include "rg_system.h"
#include "rg_audio.h"

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM) && RG_AUDIO_ASYNC
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

extern const rg_audio_driver_t rg_audio_driver_dummy;
extern const rg_audio_driver_t rg_audio_driver_buzzer;
extern const rg_audio_driver_t rg_audio_driver_i2s;
extern const rg_audio_driver_t rg_audio_driver_sdl2;

// static const rg_audio_driver_t *drivers[] = {
//     NULL,
// };

static const rg_audio_sink_t sinks[] = {
    {&rg_audio_driver_dummy,  0, "Dummy"  },
#if RG_AUDIO_USE_INT_DAC
    {&rg_audio_driver_i2s,    0, "Speaker"},
#endif
#if RG_AUDIO_USE_EXT_DAC
    {&rg_audio_driver_i2s,    1, "Ext DAC"},
#endif
#if RG_AUDIO_USE_SDL2
    {&rg_audio_driver_sdl2,   0, "SDL2"   },
#endif
#if RG_AUDIO_USE_BUZZER_PIN
    {&rg_audio_driver_buzzer, 0, "Buzzer" },
#endif
    // {rg_audio_driver_bt_a2dp, 0, "Bluetooth"},
};

#define ACQUIRE_DEVICE(timeout)                         \
    ({                                                  \
        bool lock = rg_mutex_take(audio.lock, timeout); \
        if (!lock)                                      \
            RG_LOGE("Failed to acquire lock!\n");       \
        lock;                                           \
    })
#define RELEASE_DEVICE() rg_mutex_give(audio.lock)

static struct
{
    const rg_audio_sink_t *sink;
    const rg_audio_driver_t *driver;
    rg_mutex_t *lock;
    int sampleRate;
    int filter;
    int volume;
    bool muted;
} audio;
static rg_audio_counters_t counters;

#if defined(ESP_PLATFORM) && RG_AUDIO_ASYNC
// Async audio offload: the producer (emulation core) copies frames into a ring
// buffer and returns; a dedicated consumer task pinned to core 1 feeds the I2S
// DMA via the blocking driver->submit(). This keeps the blocking i2s_write off
// the emulation path and buffers ~RG_AUDIO_ASYNC_BUFFER_MS of audio to absorb
// CPU spikes that would otherwise starve the DMA and distort sound.
#define ASYNC_CHUNK_FRAMES 256 // max frames per ring item / per driver submit

static struct
{
    RingbufHandle_t ring;
    rg_mutex_t *guard; // protects the ring pointer vs concurrent teardown (NOT audio.lock:
                       // we must never hold audio.lock across the consumer's DMA write)
    rg_task_t *task;
    size_t chunk_frames; // ring item size limit, clamped to ASYNC_CHUNK_FRAMES
    volatile bool running;
    volatile bool started;
    bool flowing; // set once the first item is received (skips initial empty period)
} audio_async;
#endif

static const char *SETTING_DRIVER = "AudioDriver";
static const char *SETTING_DEVICE = "AudioDevice";
static const char *SETTING_VOLUME = "Volume";
static const char *SETTING_FILTER = "AudioFilter";

static const char *get_last_driver_error(void)
{
    if (audio.driver && audio.driver->get_error)
        return audio.driver->get_error();
    return "Unspecified Error";
}

#if defined(ESP_PLATFORM) && RG_AUDIO_ASYNC
// Runs on core 1: pulls frames from the ring buffer and feeds the driver. The
// driver's submit() blocks on the I2S DMA, which paces this loop in real time.
static void audio_async_task(void *arg)
{
    while (audio_async.running)
    {
        size_t item_size = 0;
        rg_audio_frame_t *item = xRingbufferReceive(audio_async.ring, &item_size, pdMS_TO_TICKS(100));
        if (!item)
        {
            // Ring stayed empty for the full timeout: the buffer bottomed out and
            // the I2S DMA was likely starved. Only count once audio is actually
            // flowing so the initial (empty) period isn't reported as an underrun.
            if (audio_async.running && audio_async.flowing)
                counters.underflows++;
            continue;
        }
        audio_async.flowing = true;

        // Drain up to ASYNC_CHUNK_FRAMES per driver submit, pacing on the DMA.
        rg_audio_frame_t *cursor = item;
        size_t frames = item_size / sizeof(rg_audio_frame_t);
        while (frames > 0 && audio_async.running)
        {
            size_t n = frames > ASYNC_CHUNK_FRAMES ? ASYNC_CHUNK_FRAMES : frames;

            if (ACQUIRE_DEVICE(1000))
            {
                if (audio.driver)
                    audio.driver->submit(cursor, n);
                RELEASE_DEVICE();
            }

            cursor += n;
            frames -= n;
        }
        vRingbufferReturnItem(audio_async.ring, item);
    }

    audio_async.started = false;
}

static void audio_async_stop(void)
{
    if (!audio_async.ring && !audio_async.task)
        return;

    audio_async.running = false;

    // The task exits within ~100ms (ring receive timeout) of seeing running==false,
    // but it could briefly be blocked on the device mutex. Wait well over that
    // before tearing down the ring buffer it still uses.
    int waited = 0;
    while (audio_async.started && waited < 1500)
    {
        rg_task_delay(10);
        waited += 10;
    }
    if (audio_async.started)
        RG_LOGW("Async audio: task did not exit cleanly (waited %dms)\n", waited);
    audio_async.task = NULL;

    // Delete the ring under the guard so a producer (e.g. prboom's sound task,
    // which runs on another core) can't be inside xRingbufferSend on a freed ring.
    if (audio_async.guard && rg_mutex_take(audio_async.guard, 500))
    {
        if (audio_async.ring)
        {
            vRingbufferDelete(audio_async.ring);
            audio_async.ring = NULL;
        }
        rg_mutex_give(audio_async.guard);
    }
    else if (audio_async.ring)
    {
        RG_LOGW("Async audio: guard timeout, ring leaked this cycle\n");
        audio_async.ring = NULL;
    }
    RG_LOGI("Async audio stopped.\n");
}

static bool audio_async_start(int sampleRate)
{
    if (audio_async.ring)
        return true; // already running

    // The guard is created once (first init, before any producer exists) and kept
    // for the app's lifetime so it can never be freed while a producer uses it.
    if (!audio_async.guard)
        audio_async.guard = rg_mutex_create();
    if (!audio_async.guard)
    {
        RG_LOGE("Async audio: guard alloc failed\n");
        return false;
    }

    size_t buffer_frames = (sampleRate * RG_AUDIO_ASYNC_BUFFER_MS) / 1000;
    // Keep enough room for a handful of chunks even at low sample rates.
    buffer_frames = RG_MAX(buffer_frames, 1024);
    // Ring capacity in frames == steady-state latency (the ring sits near-full
    // because the producer blocks once it's filled), so size it to ~50ms and
    // only add a little for per-item ringbuf headers.
    size_t buffer_bytes = buffer_frames * sizeof(rg_audio_frame_t) + 512;

    if (!rg_mutex_take(audio_async.guard, 1000))
    {
        RG_LOGE("Async audio: guard unavailable\n");
        return false;
    }

    audio_async.ring = xRingbufferCreate(buffer_bytes, RINGBUF_TYPE_NOSPLIT);
    if (!audio_async.ring)
    {
        rg_mutex_give(audio_async.guard);
        RG_LOGE("Async audio: ring buffer alloc failed (%d bytes)\n", (int)buffer_bytes);
        return false;
    }

    // A NOSPLIT ring only accepts items up to half its size, and emulator cores
    // routinely submit more than that in one call (prboom submits 631 frames at
    // 22050Hz, while a 50ms ring's limit is only ~550). Keep ring items small and
    // split large submissions in rg_audio_submit() rather than dropping them.
    audio_async.chunk_frames = xRingbufferGetMaxItemSize(audio_async.ring) / sizeof(rg_audio_frame_t);
    if (audio_async.chunk_frames > ASYNC_CHUNK_FRAMES)
        audio_async.chunk_frames = ASYNC_CHUNK_FRAMES;

    audio_async.running = true;
    audio_async.started = true;
    audio_async.flowing = false;
    // Core 1, above the display task (priority 6) so audio keeps the DMA fed.
    audio_async.task = rg_task_create("rg_audio", &audio_async_task, NULL, 3 * 1024,
                                      RG_TASK_PRIORITY_7, 1);
    if (!audio_async.task)
    {
        vRingbufferDelete(audio_async.ring);
        audio_async.ring = NULL;
        audio_async.running = false;
        audio_async.started = false;
        rg_mutex_give(audio_async.guard);
        RG_LOGE("Async audio: task creation failed\n");
        return false;
    }

    // Guard held across the whole setup so a producer can't observe a half-built
    // ring. The consumer task never takes the guard, so this can't deadlock.
    rg_mutex_give(audio_async.guard);

    RG_LOGI("Async audio: ring=%d frames (%dms), feeding on core 1\n",
            (int)buffer_frames, RG_AUDIO_ASYNC_BUFFER_MS);
    return true;
}
#endif

void rg_audio_init(int sampleRate)
{
    RG_ASSERT(audio.sink == NULL, "Audio sink already initialized!");

    if (!audio.lock)
    {
        audio.lock = rg_mutex_create();
        RELEASE_DEVICE();
    }
    ACQUIRE_DEVICE(1000);

    char *driver_name = rg_settings_get_string(NS_GLOBAL, SETTING_DRIVER, "DEFAULT");
    int device = rg_settings_get_number(NS_GLOBAL, SETTING_DEVICE, 0);
    for (size_t i = 0; i < RG_COUNT(sinks); ++i)
    {
        if (strcmp(sinks[i].driver->name, driver_name) == 0 && sinks[i].device == device)
            audio.sink = &sinks[i];
    }
    free(driver_name);

    if (!audio.sink) // Default to first non-dummy if no match found
        audio.sink = &sinks[1 % RG_COUNT(sinks)];

    audio.filter = (int)rg_settings_get_number(NS_GLOBAL, SETTING_FILTER, 0);
    audio.volume = (int)rg_settings_get_number(NS_GLOBAL, SETTING_VOLUME, 50);
    audio.sampleRate = sampleRate;
    audio.driver = audio.sink->driver;

    if (audio.driver->init(audio.sink->device, sampleRate))
    {
        if (audio.driver->set_mute)
            audio.driver->set_mute(audio.muted);
        if (audio.driver->set_volume)
            audio.driver->set_volume(audio.volume);

        RG_LOGI("Audio ready. sink='%s', samplerate=%d, volume=%d\n",
            audio.sink->name, audio.sampleRate, audio.volume);
    }
    else
    {
        RG_LOGE("Failed to initialize audio. sink='%s', samplerate=%d, volume=%d\n",
            audio.sink->name, audio.sampleRate, audio.volume);
        RG_LOGE(" - Error: %s\n", get_last_driver_error());
        audio.sink = &sinks[0]; // Switching to dummy might allow us to at least boot
        audio.driver = audio.sink->driver;
    }

#if defined(ESP_PLATFORM) && RG_AUDIO_ASYNC
    // Only offload the I2S driver to core 1. The buzzer driver has its own
    // timer-driven playback loop, and the dummy/sdl2 drivers aren't real outputs.
    if (audio.driver == &rg_audio_driver_i2s && audio.driver->submit)
        audio_async_start(sampleRate);
#endif

    RELEASE_DEVICE();
}

void rg_audio_deinit(void)
{
    if (!audio.sink)
        return;

#if defined(ESP_PLATFORM) && RG_AUDIO_ASYNC
    // Stop the consumer task first so it isn't submitting to a deinitialized driver.
    audio_async_stop();
#endif

    // We'll go ahead even if we can't acquire the lock...
    ACQUIRE_DEVICE(1000);

    audio.driver->deinit();

    RG_LOGI("Audio terminated. sink='%s'\n", audio.sink->name);

    audio.driver = NULL;
    audio.sink = NULL;

    RELEASE_DEVICE();
}

void rg_audio_submit(const rg_audio_frame_t *frames, size_t count)
{
    const int64_t time_start = rg_system_timer();

    if (!audio.driver)
        return;

    if (!frames || !count)
        return;

#if defined(ESP_PLATFORM) && RG_AUDIO_ASYNC
    // The guard keeps a concurrent teardown (rg_audio_set_sink invoked from the
    // menu on another task, e.g. prboom's sound task submitting while the sink
    // switches) from freeing the ring under us. It's held across the blocking
    // send below, but only teardown ever contends for it — never the consumer —
    // so holding it can't deadlock against the DMA write.
    if (audio_async.guard && rg_mutex_take(audio_async.guard, 100))
    {
        if (audio_async.ring && audio_async.chunk_frames)
        {
            // Fast path: copy into the ring buffer and return. The core-1 consumer
            // task feeds the blocking I2S DMA. If the ring is full we block here,
            // which paces the emulation loop to real time (same throttle the old
            // blocking i2s_write provided, but now off the DMA's back).
            //
            // A NOSPLIT ring rejects items larger than half its size, and some
            // cores (eg prboom) submit more frames than that per call, so split
            // the submission into ordered chunks instead of dropping it wholesale.
            const rg_audio_frame_t *cursor = frames;
            size_t remaining = count;
            while (remaining > 0)
            {
                size_t n = RG_MIN(remaining, audio_async.chunk_frames);
                if (xRingbufferSend(audio_async.ring, cursor, n * sizeof(rg_audio_frame_t), pdMS_TO_TICKS(200)) != pdTRUE)
                {
                    // Consumer is backed up and we timed out; drop the rest to avoid stalling emulation.
                    counters.overflows += remaining;
                    break;
                }
                cursor += n;
                remaining -= n;
            }

            rg_mutex_give(audio_async.guard);
            counters.totalSamples += count;
            counters.busyTime += rg_system_timer() - time_start;
            return;
        }
        rg_mutex_give(audio_async.guard);
    }
    // No active ring (async not started or mid-teardown): synchronous path below.
#endif

    if (ACQUIRE_DEVICE(0))
    {
        audio.driver->submit(frames, count);
        RELEASE_DEVICE();
    }

    counters.totalSamples += count;
    counters.busyTime += rg_system_timer() - time_start;
}

rg_audio_counters_t rg_audio_get_counters(void)
{
    return counters;
}

const char *rg_audio_get_driver(void)
{
    if (!audio.driver)
        return NULL;
    return audio.driver->name;
}

const rg_audio_sink_t *rg_audio_get_sinks(size_t *count)
{
    if (count)
        *count = RG_COUNT(sinks);
    return sinks;
}

const rg_audio_sink_t *rg_audio_get_sink(void)
{
    return audio.sink;
}

void rg_audio_set_sink(const char *driver_name, int device)
{
    RG_LOGI("%s %d", driver_name, device);
    rg_settings_set_string(NS_GLOBAL, SETTING_DRIVER, driver_name);
    rg_settings_set_number(NS_GLOBAL, SETTING_DEVICE, device);
    rg_audio_deinit();
    rg_audio_init(audio.sampleRate);
}

int rg_audio_get_volume(void)
{
    return audio.volume;
}

void rg_audio_set_volume(int percent)
{
    RG_ASSERT(audio.driver != NULL, "Audio device not ready!");
    audio.volume = RG_MIN(RG_MAX(percent, 0), 100);
    if (audio.driver->set_volume)
        audio.driver->set_volume(audio.volume);
    rg_settings_set_number(NS_GLOBAL, SETTING_VOLUME, audio.volume);
    RG_LOGI("Volume set to %d%%\n", audio.volume);
}

bool rg_audio_get_mute(void)
{
    return audio.muted;
}

void rg_audio_set_mute(bool mute)
{
    RG_ASSERT(audio.driver != NULL, "Audio device not ready!");

    if (!ACQUIRE_DEVICE(1000))
        return;

    if (audio.driver->set_mute)
        audio.driver->set_mute(mute);

    audio.muted = mute;
    RELEASE_DEVICE();
}

int rg_audio_get_sample_rate(void)
{
    return audio.sampleRate;
}

void rg_audio_set_sample_rate(int sampleRate)
{
    RG_ASSERT(audio.driver != NULL, "Audio device not ready!");

    if (audio.sampleRate == sampleRate)
        return;

    if (audio.driver->set_sample_rate)
    {
        if (ACQUIRE_DEVICE(1000))
        {
            audio.driver->set_sample_rate(sampleRate);
            audio.sampleRate = sampleRate;
            RG_LOGI("Samplerate set to %d", audio.sampleRate);
            RELEASE_DEVICE();
        }
    }
    else
    {
        rg_audio_deinit();
        rg_audio_init(sampleRate);
    }
}
