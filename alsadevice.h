#ifndef _ALSA_DEVICE_H_
#define _ALSA_DEVICE_H_

#include <string>
#include <atomic>
#include <stdint.h>
#include <alsa/asoundlib.h>
#include "dllexport.h"


typedef void(*AlsaEnumCallback)(int index, const char* card, const char* name, const char* id, void* user);
typedef void(*AlsaInputDataCallback)(uint32_t cacheTimeMs, char* data, uint32_t& samples, void* user);
typedef void(*AlsaOutputDataCallback)(uint32_t cacheTimeMs, const char* data, uint32_t samples, void* user);
//typedef void(*AlsaPlaybackTimeCallback)(uint32_t playTimeMs, void* user);
typedef void(*AlsaPlaybackStoppedCallback)(void* user);

DLL_EXPORT int enumerateAlsaCaptureDevices(AlsaEnumCallback callback, void* user);
DLL_EXPORT int enumerateAlsaPlaybackDevices(AlsaEnumCallback callback, void* user);
DLL_EXPORT size_t AlsaCaptureDevice_create();
DLL_EXPORT size_t AlsaPlaybackDevice_create();
DLL_EXPORT void AlsaDevice_release(size_t handle);
DLL_EXPORT int AlsaDevice_open(size_t handle, const char* deviceId);
DLL_EXPORT int AlsaDevice_setPeriod(size_t handle, uint32_t periodCount, uint32_t periodTime);
DLL_EXPORT int AlsaDevice_setParams(size_t handle, uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample);
DLL_EXPORT int AlsaDevice_start(size_t handle);
DLL_EXPORT int AlsaDevice_stop(size_t handle);
DLL_EXPORT int AlsaDevice_close(size_t handle);
DLL_EXPORT int AlsaDevice_getVolume(size_t handle, const char* cardId);
DLL_EXPORT int AlsaDevice_setVolume(size_t handle, const char* cardId, int volume);
DLL_EXPORT int AlsaCaptureDevice_setOutputCallback(size_t handle, AlsaOutputDataCallback callback, void* user);
DLL_EXPORT int AlsaCaptureDevice_captureToPcmFile(size_t handle, const char* pcmName, int append = 0);
DLL_EXPORT int AlsaPlaybackDevice_setMinCachePeriodCount(size_t handle, uint32_t minCachePeriodCount);
DLL_EXPORT int AlsaPlaybackDevice_setInputCallback(size_t handle, AlsaInputDataCallback callback, void* user);
DLL_EXPORT int AlsaPlaybackDevice_syncStop(size_t handle);
DLL_EXPORT int AlsaPlaybackDevice_asyncStop(size_t handle, AlsaPlaybackStoppedCallback callback, void* user);
DLL_EXPORT int AlsaPlaybackDevice_pause(size_t handle);
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
    bool stop() override;
    bool close() override;
    // cardId can't be null
    // if cardId is "default", get system default device's volume [0, 100]
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
    // if cardId is null or empty, device volume is not changed,
    // change volume by software
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

