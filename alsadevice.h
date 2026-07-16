#ifndef _ALSA_DEVICE_H_
#define _ALSA_DEVICE_H_

#include <string>
#include <atomic>
#include <stdint.h>
#include <alsa/asoundlib.h>
#include "dllexport.h"


// Enumerate callback: index is 0-based device index; card/name/id are NUL-terminated C strings;
// user is the opaque pointer passed to enumerateAlsa*Devices.
typedef void(*AlsaEnumCallback)(int index, const char* card, const char* name, const char* id, void* user);
// Playback feed callback: fill data with up to *samples frames; set *samples to frames actually written.
// cacheTimeMs is buffered audio duration in ms. Runs on the device worker thread.
// samples is a pointer (C ABI); do not use C++ references in exported callbacks.
typedef void(*AlsaInputDataCallback)(uint32_t cacheTimeMs, char* data, uint32_t* samples, void* user);
// Capture deliver callback: data holds samples frames of interleaved PCM. Runs on the device worker thread.
typedef void(*AlsaOutputDataCallback)(uint32_t cacheTimeMs, const char* data, uint32_t samples, void* user);
//typedef void(*AlsaPlaybackTimeCallback)(uint32_t playTimeMs, void* user);
// Called when asyncStop finishes draining cached playback. Do not call stop() inside this callback.
typedef void(*AlsaPlaybackStoppedCallback)(void* user);

// Enumerate capture devices. callback may be null (still prints to stdout).
// @param callback  per-device callback; user forwarded unchanged
// @param user      opaque user pointer for callback
// @return          number of hardware devices found (excludes Python-side "default")
DLL_EXPORT int enumerateAlsaCaptureDevices(AlsaEnumCallback callback, void* user);

// Enumerate playback devices. Same semantics as enumerateAlsaCaptureDevices.
// @return number of hardware devices found
DLL_EXPORT int enumerateAlsaPlaybackDevices(AlsaEnumCallback callback, void* user);

// Query compact native HW params for a PCM device id (e.g. "hw:Device_1,0", "default").
// Also resolves logical names (default/pulse/plughw:...) to the underlying hw device when possible.
// Success text example:
//   resolves: hw:Device_1,0
//   card    : hw:4 (USB Composite Device)
//   type    : HW
//   channels: 2
//   rates   : 8000-48000
//   formats : S16_LE, S32_LE
// On failure, still writes "unavailable: <reason>" into out when possible.
// @param deviceId   ALSA PCM name; must not be null/empty
// @param isCapture  1 = capture stream, 0 = playback stream
// @param out        caller-owned buffer for UTF-8 result (NUL-terminated)
// @param outSize    size of out in bytes; recommend >= 1024
// @return           1 on success, 0 on failure
DLL_EXPORT int queryAlsaDeviceHwParams(const char* deviceId, int isCapture, char* out, int outSize);

// Create a capture device instance.
// @return opaque handle; free with AlsaDevice_release
DLL_EXPORT size_t AlsaCaptureDevice_create();

// Create a playback device instance.
// @return opaque handle; free with AlsaDevice_release
DLL_EXPORT size_t AlsaPlaybackDevice_create();

// Destroy a device created by AlsaCaptureDevice_create / AlsaPlaybackDevice_create.
// @param handle  device handle; no-op if invalid
DLL_EXPORT void AlsaDevice_release(size_t handle);

// Open ALSA PCM for the device.
// @param handle    device handle
// @param deviceId  ALSA PCM name (e.g. "default", "hw:...", "plughw:...")
// @return          1 on success, 0 on failure
DLL_EXPORT int AlsaDevice_open(size_t handle, const char* deviceId);

// Configure ALSA period layout. Call before setParams, or omit to use defaults.
// Internal ring buffer is roughly (periodCount * periodTime) ms of audio.
// @param handle       device handle
// @param periodCount  number of periods
// @param periodTime   period duration in milliseconds
// @return             1 on success, 0 on failure
DLL_EXPORT int AlsaDevice_setPeriod(size_t handle, uint32_t periodCount, uint32_t periodTime);

// Set PCM format. Call after open, before start.
// @param handle         device handle
// @param sampleRate     sample rate in Hz
// @param channels       channel count
// @param bitsPerSample  bits per sample (8/16/24/32)
// @return               1 on success, 0 on failure (e.g. unsupported channels on hw:)
DLL_EXPORT int AlsaDevice_setParams(size_t handle, uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample);

// Start capture/playback worker.
// @return 1 on success, 0 on failure
DLL_EXPORT int AlsaDevice_start(size_t handle);

// Stop immediately. For playback, cached samples in the ALSA buffer may be discarded.
// @return 1 on success, 0 on failure
DLL_EXPORT int AlsaDevice_stop(size_t handle);

// Close the ALSA PCM and release device resources (handle remains until AlsaDevice_release).
// @return 1 on success, 0 on failure
DLL_EXPORT int AlsaDevice_close(size_t handle);

// Get volume.
// Playback: null/empty cardId returns software volume (_volume); non-empty uses ALSA mixer.
// Capture: cardId selects mixer card (e.g. "hw:0", "default").
// @return volume in [0, 100], or negative on failure
DLL_EXPORT int AlsaDevice_getVolume(size_t handle, const char* cardId);

// Set volume.
// Playback: null/empty cardId = software PCM scaling (does not change system mixer);
//           non-empty cardId = ALSA mixer on that card.
// Capture: always ALSA mixer; cardId must be a mixer card name.
// @param volume  volume in [0, 100]
// @return        1 on success, 0 on failure
DLL_EXPORT int AlsaDevice_setVolume(size_t handle, const char* cardId, int volume);

// Set capture data callback (invoked from worker thread with captured PCM).
// @param handle    capture device handle
// @param callback  output data callback; may be null to clear
// @param user      opaque user pointer for callback
// @return          1 on success, 0 on failure
DLL_EXPORT int AlsaCaptureDevice_setOutputCallback(size_t handle, AlsaOutputDataCallback callback, void* user);

// Also write captured PCM to a raw file (optional debug aid).
// @param handle   capture device handle
// @param pcmName  output file path
// @param append   non-zero to append, 0 to truncate/create
// @return         1 on success, 0 on failure
DLL_EXPORT int AlsaCaptureDevice_captureToPcmFile(size_t handle, const char* pcmName, int append = 0);

// When buffered periods fall to this count, playback feeds silence to avoid underrun.
// @param handle              playback device handle
// @param minCachePeriodCount minimum cached periods before feeding zeros
// @return                    1 on success, 0 on failure
DLL_EXPORT int AlsaPlaybackDevice_setMinCachePeriodCount(size_t handle, uint32_t minCachePeriodCount);

// Set playback feed callback (invoked from worker thread to pull PCM).
// @param handle    playback device handle
// @param callback  input data callback; may be null to clear
// @param user      opaque user pointer for callback
// @return          1 on success, 0 on failure
DLL_EXPORT int AlsaPlaybackDevice_setInputCallback(size_t handle, AlsaInputDataCallback callback, void* user);

// Stop playback and block until cached samples have been played.
// @return 1 on success, 0 on failure
DLL_EXPORT int AlsaPlaybackDevice_syncStop(size_t handle);

// Request stop; callback is invoked after cached samples are played.
// To start again after asyncStop, call stop() on the main thread (not inside callback).
// @param handle    playback device handle
// @param callback  stopped notification; may be null
// @param user      opaque user pointer for callback
// @return          1 on success, 0 on failure
DLL_EXPORT int AlsaPlaybackDevice_asyncStop(size_t handle, AlsaPlaybackStoppedCallback callback, void* user);

// Pause playback.
// @return 1 on success, 0 on failure
DLL_EXPORT int AlsaPlaybackDevice_pause(size_t handle);

// Resume after pause.
// @return 1 on success, 0 on failure
DLL_EXPORT int AlsaPlaybackDevice_resume(size_t handle);

namespace std {
    class thread;
}

// struct snd_pcm_hw_params_t;
// struct snd_pcm_t;

class AlsaDevice
{
public:
    AlsaDevice();
    virtual ~AlsaDevice();
    virtual bool open(const char* deviceId);
    // setPeriod should be called before setParams, or not call it
    // alsa internal ring buffer will be (preriodCount * periodTime * 2) ms
    bool setPeriod(uint32_t periodCount = 20, uint32_t periodTime = 10);
    // setParams should be called after open before start
    virtual bool setParams(uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample);
    virtual bool start();
    virtual bool stop();
    virtual bool close();
    virtual int getVolume(const char* cardId) = 0;
    virtual bool setVolume(const char* cardId, int volume) = 0;

protected:
    void handleOpenError(const char* deviceId, int err);
    static void threadFunc(AlsaDevice* pThis);
    virtual void threadStarted();
    virtual void run();
    virtual void threadStopped();
    // buffer size is sampleCount * channels * bytesPerSample
    virtual void handleData(int64_t startTick, int avail, char* buffer, uint32_t sampleCount) = 0;
    void getParams(snd_pcm_hw_params_t* hwParams);

protected:
    std::string _deviceId;
    snd_pcm_t* _handle{};
    std::thread* _thread{};
    uint32_t _sampleRate{};
    uint32_t _channels{};
    uint32_t _bitsPerSample{};
    uint32_t _totalHandledSamples{};
    uint32_t _periodCount{};
    uint32_t _periodTime{}; // milliseconds
    std::atomic<bool> _threadShouldExit{};
};

class AlsaCaptureDevice : public AlsaDevice
{
public:
    virtual ~AlsaCaptureDevice();
    bool open(const char* deviceId) override;
    bool start() override;
    bool stop() override;
    bool close() override;
    // Mixer volume only. cardId selects mixer card (e.g. "hw:0", "default").
    // getVolume returns [0,100], or -1 on failure.
    int getVolume(const char* cardId) override;
    bool setVolume(const char* cardId, int volume) override;
    bool setOutputCallback(AlsaOutputDataCallback callback, void* user);
    bool captureToPcmFile(const char* pcmName, bool append = false);
protected:
    void handleData(int64_t startTick, int avail, char* buf, uint32_t sampleCount) override;
    void closeFile();
private:
    AlsaOutputDataCallback _outputCallback{};
    void* _userData{};
    FILE* _pcmFile{};
};

class AlsaPlaybackDevice : public AlsaDevice
{
public:
    AlsaPlaybackDevice();
    virtual ~AlsaPlaybackDevice();
    bool open(const char* deviceId) override;
    // stop immediately, cache samples left in alsa internal buffer are not played
    bool stop() override;
    bool close() override;
    // null/empty cardId: software PCM volume (_volume).
    // non-empty: ALSA mixer on that card. getVolume returns -1 on mixer failure.
    int getVolume(const char* cardId) override;
    bool setVolume(const char* cardId, int volume) override;
    bool pause();
    bool resume();
    //bool setPlayTimeCallback(AlsaPlaybackTimeCallback callback, void* user, uint32_t stepMs = 1000);

    // syncStop, stop and wait util cache samples are played
    bool syncStop();
    // asyncStop, notify to stop, AlsaPlaybackStoppedCallback will be called when cache samples are played
    // If you want to start playback again after asyncStop,
    // You must notify a msg to main thread and call stop() in main thread.
    // Don't call stop() in in AlsaPlaybackStoppedCallback.
    bool asyncStop(AlsaPlaybackStoppedCallback callback, void* user);
    // auto feed zero when cache count <= minCachePeriodCount
    bool setMinCachePeriodCount(uint32_t minCachePeriodCount);
    bool setInputCallback(AlsaInputDataCallback callback, void* user);
protected:
    void run() override;
    void threadStopped() override;
    void handleData(int64_t startTick, int avail, char* buf, uint32_t sampleCount) override;
private:
    uint32_t _minCachePeriodCount{};
    uint32_t _totalCapacitySamples{};
    int _volume{100};
    AlsaInputDataCallback _inputCallback{};
    void* _userData{};
    AlsaPlaybackStoppedCallback _stopCallback{};
    void* _stopUserData{};
    std::atomic<bool> _paused{};
};


#endif

