#include <alsa/asoundlib.h>
#include <iostream>
#include <fstream>
#include <memory>
#include <atomic>

#define UTIL_IMPLEMENTION
#include "util.h"

#define DEVICE_BUFFER_LEN 256

typedef void(*AlsaEnumCallback)(int index, const char* name, const char* id, void* user);
typedef void(*AlsaInputDataCallback)(uint32_t cachedSamples, char* data, uint32_t& samples, void* user);
typedef void(*AlsaOutputDataCallback)(uint32_t cachedSamples, const char* data, uint32_t samples, void* user);

DLL_EXPORT int enumerateAlsaCaptureDevices(AlsaEnumCallback callback, void* user);
DLL_EXPORT int enumerateAlsaPlaybackDevcies(AlsaEnumCallback callback, void* user);
DLL_EXPORT size_t AlsaCaptureDevice_create();
DLL_EXPORT size_t AlsaPlaybackDevice_create();
DLL_EXPORT void AlsaDevice_release(size_t handle);
DLL_EXPORT int AlsaDevice_open(size_t handle, const char* deviceId);
DLL_EXPORT int AlsaDevice_setParams(size_t handle, uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample);
DLL_EXPORT int AlsaPlaybackDevice_setInputCallback(size_t handle, AlsaInputDataCallback callback, void* user);
DLL_EXPORT int AlsaCaptureDevice_setOutputCallback(size_t handle, AlsaOutputDataCallback callback, void* user);
DLL_EXPORT int AlsaCaptureDevice_captureToPcmFile(size_t handle, const char* pcmName);
DLL_EXPORT int AlsaDevice_start(size_t handle);
DLL_EXPORT int AlsaDevice_stop(size_t handle);
DLL_EXPORT int AlsaDevice_close(size_t handle);

int enumerateAlsaSoundcards(bool isCaptureDevice, AlsaEnumCallback callback, void* user);
void pcmList(snd_pcm_stream_t stream);

class AlsaDevice
{
public:
    AlsaDevice();
    virtual ~AlsaDevice();
    virtual bool open(const char* deviceId);
    virtual bool setParams(uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample);
    virtual bool start();
    virtual bool stop();
    virtual bool close();

protected:
    void handleOpenError(const char* deviceId, int err);
    static void threadFunc(AlsaDevice* pThis);
    virtual void run();
    virtual void handleData(int avail, char* buf, uint32_t sampleCount) = 0;
    void getParams(snd_pcm_hw_params_t* hwParams);

protected:
    std::string _deviceId;
    snd_pcm_t* _handle{};
    std::thread* _thread{};
    unsigned int _sampleRate{};
    unsigned int _channels{};
    unsigned int _bitsPerSample{};
    std::atomic<bool> _threadShouldExit{};
};

class AlsaCaptureDevice : public AlsaDevice
{
public:
    bool open(const char* deviceId) override;
    bool setOutputCallback(AlsaOutputDataCallback callback, void* user);
    bool captureToPcmFile(const char* pcmName);
    virtual bool stop();
protected:
    void handleData(int avail, char* buf, uint32_t sampleCount) override;
private:
    AlsaOutputDataCallback _outputCallback{};
    void* _userData{};
    FILE* _pcmFile{};
};

class AlsaPlaybackDevice : public AlsaDevice
{
public:
    bool open(const char* deviceId) override;
    bool setInputCallback(AlsaInputDataCallback callback, void* user);
protected:
    void handleData(int avail, char* buf, uint32_t sampleCount) override;
private:
    AlsaInputDataCallback _inputCallback{};
    void* _userData{};
};

struct PlaybackInfo
{
    FILE* file{};
};

void playCallback(uint32_t cachedSamples, char* data, uint32_t& samples, void* user)
{
    FILE* pcmFile = (FILE*)user;
    uint32_t read = fread(data, 4, samples, pcmFile);
    //ULOG("should read %u samples, read %u count", samples, read);
    samples = read;
}

int main(int argc, char* argv[])
{
	for (int i = 0; i < argc; ++i)
	{
		println("argv %d: %s", i, argv[i]);
	}

    printf("input devices:\n");
    enumerateAlsaSoundcards(true, nullptr, nullptr);
    printf("\noutput devices:\n");
    enumerateAlsaSoundcards(false, nullptr, nullptr);

    if (argc < 2)
    {
        return 0;
    }

#if 0
    AlsaCaptureDevice alsa;
    alsa.open(argv[1]);
    alsa.setParams(16000, 2, 16);
    char pcmName[128];
    pformat(pcmName, sizeof(pcmName), "cap_%d_%d.pcm", 16000, 2);
    alsa.captureToPcmFile(pcmName);
    alsa.start();
    printf("\npress any key to stop\n\n");
    int c = getchar();
    alsa.stop();
    alsa.close();
#else
    FILE* pcmFile = fopen(argv[2], "rb");
    if (pcmFile)
    {
        uint32_t sampleRate = 16000;
        {
            std::ifstream fin("sampleRate.txt");
            if (fin)
            {
                fin >> sampleRate;
            }
        }
        AlsaPlaybackDevice alsa;
        alsa.open(argv[1]);
        alsa.setParams(sampleRate, 2, 16);
        alsa.setInputCallback(playCallback, pcmFile);
        alsa.start();
        printf("\npress any key to stop\n\n");
        int c = getchar();
        alsa.stop();
        alsa.close();
        fclose(pcmFile);
    }
    else
    {
        ULOG("can't open pcm file");
    }
#endif

    ULOG("exit");
	//int c = getchar();
    return 0;
}

DLL_EXPORT int enumerateAlsaCaptureDevices(AlsaEnumCallback callback, void* user)
{
    return enumerateAlsaSoundcards(true, callback, user);
}

DLL_EXPORT int enumerateAlsaPlaybackDevcies(AlsaEnumCallback callback, void* user)
{
    return enumerateAlsaSoundcards(false, callback, user);
}

DLL_EXPORT size_t AlsaCaptureDevice_create()
{
    AlsaDevice* alsa = new AlsaCaptureDevice();
    return (size_t)alsa;
}

DLL_EXPORT size_t AlsaPlaybackDevice_create()
{
    AlsaDevice* alsa = new AlsaPlaybackDevice();
    return (size_t)alsa;
}

DLL_EXPORT void AlsaDevice_release(size_t handle)
{
    AlsaDevice* alsa = (AlsaDevice*)handle;
    delete alsa;
}

DLL_EXPORT int AlsaDevice_open(size_t handle, const char* deviceId)
{
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->open(deviceId);
}

DLL_EXPORT int AlsaDevice_setParams(size_t handle, uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample)
{
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->setParams(sampleRate, channels, bitsPerSample);
}

DLL_EXPORT int AlsaPlaybackDevice_setInputCallback(size_t handle, AlsaInputDataCallback callback, void* user)
{
    AlsaDevice* alsa = (AlsaDevice*)handle;
    AlsaPlaybackDevice* play = (AlsaPlaybackDevice*)alsa;
    return play->setInputCallback(callback, user);
}

DLL_EXPORT int AlsaCaptureDevice_setOutputCallback(size_t handle, AlsaOutputDataCallback callback, void* user)
{
    AlsaDevice* alsa = (AlsaDevice*)handle;
    AlsaCaptureDevice* cap = (AlsaCaptureDevice*)alsa;
    return cap->setOutputCallback(callback, user);
}

DLL_EXPORT int AlsaCaptureDevice_captureToPcmFile(size_t handle, const char* pcmName)
{
    AlsaDevice* alsa = (AlsaDevice*)handle;
    AlsaCaptureDevice* cap = (AlsaCaptureDevice*)alsa;
    return cap->captureToPcmFile(pcmName);
}

DLL_EXPORT int AlsaDevice_start(size_t handle)
{
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->start();
}

DLL_EXPORT int AlsaDevice_stop(size_t handle)
{
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->stop();
}

DLL_EXPORT int AlsaDevice_close(size_t handle)
{
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->close();
}

int enumerateAlsaSoundcards(bool isCaptureDevice, AlsaEnumCallback callback, void* user)
{
    snd_ctl_t* handle = nullptr;
    snd_ctl_card_info_t* info = nullptr;
    snd_ctl_card_info_alloca(&info);
    int cardNum = -1;
    int deviceCount = 0;

    while (1)
    {
        snd_card_next (&cardNum);

        if (cardNum < 0)
            break;

        char hwname[32];
        sprintf(hwname, "hw:%d", cardNum);

        if (snd_ctl_open(&handle, hwname, SND_CTL_NONBLOCK) >= 0)
        {
            if (snd_ctl_card_info(handle, info) >= 0)
            {
                std::string cardId = snd_ctl_card_info_get_id(info);
                std::string cardName = snd_ctl_card_info_get_name(info);

                if (cardName.empty())
                {
                    cardName = cardId;
                }

                int device = -1;
                snd_pcm_info_t* pcmInfo;
                snd_pcm_info_alloca (&pcmInfo);

                while (1)
                {
                    if (snd_ctl_pcm_next_device(handle, &device) < 0 || device < 0)
                        break;

                    snd_pcm_info_set_device (pcmInfo, (unsigned int)device);

                    for (unsigned int subDevice = 0, nbSubDevice = 1; subDevice < nbSubDevice; ++subDevice)
                    {
                        snd_pcm_info_set_subdevice(pcmInfo, subDevice);
                        snd_pcm_stream_t pcmType = isCaptureDevice ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
                        snd_pcm_info_set_stream(pcmInfo, pcmType);
                        if (snd_ctl_pcm_info(handle, pcmInfo) < 0)
                        {
                            continue;
                        }

                        if (nbSubDevice == 1)
                            nbSubDevice = snd_pcm_info_get_subdevices_count(pcmInfo);

                        std::stringstream ssid, ssname;

                        if (nbSubDevice == 1)
                        {
                            ssid << "hw:" << cardId << "," << device;
                            ssname << cardName << ", " << snd_pcm_info_get_name(pcmInfo);
                        }
                        else
                        {
                            ssid << "hw:" << cardId << "," << device << "," << (int)subDevice;
                            ssname << cardName << ", " << snd_pcm_info_get_name (pcmInfo)
                                << " {" <<  snd_pcm_info_get_subdevice_name (pcmInfo) << "}";
                        }

                        std::string name = ssname.str();
                        std::string id = ssid.str();
                        std::cout << (isCaptureDevice ? "capture device\n  id: " : "playback device\n    id: ")
                            << id << "\n  name: " << name << "\n";
                        if (callback)
                        {
                            callback(deviceCount, name.c_str(), id.c_str(), user);
                        }
                        ++deviceCount;
                    }
                }
            }

            snd_ctl_close(handle);
        }
    }
    return deviceCount;
}

void pcmList(snd_pcm_stream_t stream)
{
	void **hints, **n;
	char *name, *descr, *descr1, *io;
	const char *filter;

	if (snd_device_name_hint(-1, "pcm", &hints) < 0)
		return;
	n = hints;
	filter = stream == SND_PCM_STREAM_CAPTURE ? "Input" : "Output";
    const char* streamName = snd_pcm_stream_name(stream);
	while (*n != NULL) {
		name = snd_device_name_get_hint(*n, "NAME");
		descr = snd_device_name_get_hint(*n, "DESC");
		io = snd_device_name_get_hint(*n, "IOID");
		if (io != NULL && strcmp(io, filter) != 0)
			goto __end;
		printf("pcm %s: %s\n    IOID: %s\n", streamName, name, io ? io : "null");
        if ((descr1 = descr) != NULL) {
			printf("    desc: ");
			while (*descr1) {
				if (*descr1 == '\n')
					printf("\n          ");
				else
					putchar(*descr1);
				descr1++;
			}
			putchar('\n');
		}
        __end:
        if (name != NULL)
            free(name);
		if (descr != NULL)
			free(descr);
		if (io != NULL)
			free(io);
		n++;
	}
	snd_device_name_free_hint(hints);
}

#define CHECK_ALSA_ERR_RET_FALSE(func, err) if (err < 0)  \
        {   \
            ULOG(#func " failed, error=%d(%s)", err, snd_strerror(err));  \
            return false;   \
        }

#define CHECK_ALSA_ERR_RET(func, err) if (err < 0)  \
        {   \
            ULOG(#func " failed, error=%d(%s)", err, snd_strerror(err));  \
            return;   \
        }

#define CHECK_ALSA_ERR(func, err) if (err < 0)  \
        {   \
            ULOG(#func " failed, error=%d(%s)", err, snd_strerror(err));  \
        }

#define LOG_ALSA_ERR(func, err) ULOG(#func " failed, error=%d(%s)", err, snd_strerror(err));

#define PERIOD_COUNT 10
#define PERIOD_TIME 20

AlsaDevice::AlsaDevice()
{

}

AlsaDevice::~AlsaDevice()
{
    close();
}

bool AlsaDevice::open(const char* deviceId)
{
    return false;
}

bool AlsaCaptureDevice::open(const char* deviceId)
{
    snd_pcm_stream_t streamType = SND_PCM_STREAM_CAPTURE;
    if (_handle)
    {
        ULOG("Previous capture device(%s) is not close, can't open new device '%s'",
            _deviceId.c_str(), deviceId);
        return false;
    }

    ULOG("Open capture device '%s' by ALSA %s", deviceId, SND_LIB_VERSION_STR);
    // 0 block, SND_PCM_NONBLOCK, SND_PCM_ASYNC
    int err = snd_pcm_open(&_handle, deviceId, streamType, SND_PCM_ASYNC);
    if (err < 0)
    {
        handleOpenError(deviceId, err);
        return false;
    }

    _deviceId = deviceId;
    return _handle != nullptr;
}

bool AlsaPlaybackDevice::open(const char* deviceId)
{
    snd_pcm_stream_t streamType = SND_PCM_STREAM_PLAYBACK;
    if (_handle)
    {
        ULOG("Previous playback device(%s) is not close, can't open new device '%s'",
            _deviceId.c_str(), deviceId);
        return false;
    }

    ULOG("Open playback device '%s' by ALSA %s", deviceId, SND_LIB_VERSION_STR);
    // 0 block, SND_PCM_NONBLOCK, SND_PCM_ASYNC
    int err = snd_pcm_open(&_handle, deviceId, streamType, SND_PCM_ASYNC);
    if (err < 0)
    {
        handleOpenError(deviceId, err);
        return false;
    }

    _deviceId = deviceId;
    return _handle != nullptr;
}

void AlsaDevice::handleOpenError(const char* deviceId, int err)
{
    if (-err == EBUSY)
    {
        ULOG("The device '%s' is busy (another application is using it).", deviceId);
    }
    else if (-err == ENOENT)
    {
        ULOG("The device '%s' is not available.", deviceId);
    }
    else
    {
        ULOG("Could not open device '%s'. error=%d(%s)", deviceId, err, snd_strerror(err));
    }
}

bool AlsaDevice::setParams(uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample)
{
    if (_handle == nullptr)
        return false;

    ULOG("set sampleRate=%u, channels=%u, bitsPerSample=%u, access=RW_INTERLEAVED", sampleRate, channels, bitsPerSample);

    snd_pcm_hw_params_t* hwParams;
    snd_pcm_hw_params_alloca(&hwParams);
    int err = 0;
    int direction = 0;

    err = snd_pcm_hw_params_any(_handle, hwParams);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_hw_params_any, err);

    err = snd_pcm_hw_params_set_access(_handle, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_hw_params_set_access, err);

    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
    if (bitsPerSample == 8)
    {
        format = SND_PCM_FORMAT_S8;
    }
    if (bitsPerSample == 16)
    {
        format = SND_PCM_FORMAT_S16;
    }
    else if (bitsPerSample == 24)
    {
        format = SND_PCM_FORMAT_S24_LE;
    }
    else if (bitsPerSample == 32)
    {
        format = SND_PCM_FORMAT_S32_LE;
    }
    else
    {
        ULOG("current doesn't support %u bits audio", bitsPerSample);
        return false;
    }
    _bitsPerSample = bitsPerSample;
    err = snd_pcm_hw_params_set_format(_handle, hwParams, format);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_hw_params_set_format, err);

    err = snd_pcm_hw_params_set_channels(_handle, hwParams, channels);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_hw_params_set_channels, err);

    err = snd_pcm_hw_params_set_rate_near(_handle, hwParams, &sampleRate, &direction);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_hw_params_set_rate_near, err);

    unsigned int periods = PERIOD_COUNT;
    err = snd_pcm_hw_params_set_periods_near (_handle, hwParams, &periods, &direction);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_hw_params_set_periods_near, err);

    snd_pcm_uframes_t samplesPerPeriod = (snd_pcm_uframes_t)PERIOD_TIME * sampleRate / 1000; // %lu
    err = snd_pcm_hw_params_set_period_size_near (_handle, hwParams, &samplesPerPeriod, &direction);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_hw_params_set_period_size_near, err);

    err = snd_pcm_hw_params(_handle, hwParams);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_hw_params, err);

    getParams(hwParams);
    return true;
}

bool AlsaPlaybackDevice::setInputCallback(AlsaInputDataCallback callback, void* user)
{
    if (_thread)
        return false;
    ULOG("callback %p, user %p", callback, user);
    _inputCallback = callback;
    _userData = user;
    return true;
}

bool AlsaCaptureDevice::setOutputCallback(AlsaOutputDataCallback callback, void* user)
{
    if (_thread)
        return false;
    ULOG("callback %p, user %p", callback, user);
    _outputCallback = callback;
    _userData = user;
    return true;
}

bool AlsaCaptureDevice::captureToPcmFile(const char* pcmName)
{
    if (_thread)
        return false;
    _pcmFile = fopen(pcmName, "wb");
    return _pcmFile != nullptr;
}

bool AlsaDevice::start()
{
    if (_handle == nullptr)
        return false;
    if (_thread)
        return false;

    int err = 0;
    err = snd_pcm_prepare(_handle);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_prepare, err);

    // can call snd_pcm_start or not call it
    //err = snd_pcm_start(_handle);
    //CHECK_ALSA_ERR_RET_FALSE(snd_pcm_start, err);

    _thread = new std::thread(threadFunc, this);

    return true;
}

bool AlsaDevice::stop()
{
    if (_thread == nullptr)
        return false;

    _threadShouldExit = true;
    _thread->join();
    delete _thread;
    _thread = nullptr;
    _threadShouldExit = false;
    return true;
}

bool AlsaCaptureDevice::stop()
{
    bool ret = AlsaDevice::stop();
    if (_pcmFile)
    {
        fclose(_pcmFile);
        _pcmFile = nullptr;
    }
    return ret;
}

bool AlsaDevice::close()
{
    if (_handle != nullptr)
    {
        stop();
        snd_pcm_close(_handle);
        _handle = nullptr;
        return true;
    }
    return false;
}

void AlsaDevice::threadFunc(AlsaDevice* pThis)
{
    int64_t startTick = msecSinceStart();
    pThis->run();
    ULOG("run for %lld ms", msecSinceStart() - startTick);
}

void AlsaDevice::run()
{
    int err = 0;
    int timeout = 1000;
    int slient = 0;
    int64_t now = 0;
    int64_t cost = 0;
    snd_pcm_sframes_t sampleCount = (snd_pcm_sframes_t) PERIOD_TIME * _sampleRate / 1000;
    //uint32_t sampleBytes = sampleCount * _channels * _bitsPerSample / 8;
    std::unique_ptr<char> data{new char[sampleCount * _channels * _bitsPerSample / 8]}; // double buffer
    char* buf = data.get();
    while (!_threadShouldExit)
    {
        now = msecSinceStart();
        //snd_pcm_wait will wait at most one period time
        err = snd_pcm_wait(_handle, timeout);
        cost = msecSinceStart() - now;
        //if (cost > 5)
        {
            VLOG("snd_pcm_wait cost %lld ms", cost);
        }
        if (_threadShouldExit)
            break;

        snd_pcm_sframes_t avail = snd_pcm_avail_update(_handle);
        VLOG("snd_pcm_avail_update returns %ld", avail);
        if (avail < 0)
        {
            ULOG("snd_pcm_avail_update returns %d(%s), call snd_pcm_recover",
                (int)avail, snd_strerror(avail));
            slient = 0;
            err = snd_pcm_recover(_handle, (int)avail, slient);
            CHECK_ALSA_ERR(snd_pcm_recover, err);
        }

        handleData(avail, buf, sampleCount);
    }
}

void AlsaCaptureDevice::handleData(int avail, char* buf, uint32_t sampleCount)
{
    int64_t now = msecSinceStart();
    // if there is not enough samples available, snd_pcm_readi will cost at most one period time
    snd_pcm_sframes_t num = snd_pcm_readi(_handle, buf, sampleCount);
    int64_t cost = msecSinceStart() - now;
    if (num < 0)
    {
        LOG_ALSA_ERR(snd_pcm_readi, (int)num);
        int slient = 1;
        int err = snd_pcm_recover(_handle, (int)num, slient);
        CHECK_ALSA_ERR(snd_pcm_recover, err);
    }
    else
    {
        if (_pcmFile)
        {
            fwrite(buf, num * _channels * _bitsPerSample / 8, 1, _pcmFile);
        }
        if (_outputCallback)
        {
            if (avail < 0)
                avail = 0;
            _outputCallback(avail, buf, num, _userData);
        }
        if (num < sampleCount)
        {
            ULOG("!!Did not read all samples: %ld/%ld, cost %lld ms", num, sampleCount, cost);
        }
        else
        {
            VLOG("Read samples: %ld, cost %lld ms", num, cost);
        }
    }
}

void AlsaPlaybackDevice::handleData(int avail, char* buf, uint32_t sampleCount)
{
    if (avail < 0)
        avail = PERIOD_COUNT*sampleCount; // under run
    uint32_t cachedSamples = PERIOD_COUNT*sampleCount - avail;
    uint32_t cacheMs = cachedSamples * 1000 / _sampleRate;
    int64_t now = msecSinceStart();

    if (_inputCallback)
    {
        uint32_t gotSamples = sampleCount;
        uint32_t cachedSamples = PERIOD_COUNT*sampleCount - avail;
        _inputCallback(cachedSamples, buf, gotSamples, _userData);
        if (gotSamples == 0)
        {
            uint32_t cacheMs = cachedSamples * 1000 / _sampleRate;
            if (cachedSamples <= sampleCount * 2) // in vmware, the buffer should larger, otherwise under run
            //if (cachedSamples <= sampleCount)
            {
                gotSamples = sampleCount;
                memset(buf, 0, sampleCount * _channels * _bitsPerSample / 8);
                VLOG("callback doesn't feed data, avail %d, cache %u ms, fill %u ms 0 data", avail, cacheMs, PERIOD_TIME);
            }
            else
            {
                VLOG("callback doesn't feed data, avail %d, cache %u ms, sleep %d ms", avail, cacheMs, PERIOD_TIME/2);
                sleepMSec(PERIOD_TIME / 2);
            }
        }
        sampleCount = gotSamples;
    }
    else
    {
        VLOG("_inputCallback is null, fill 0, cache %u ms", cacheMs);
        memset(buf, 0, sampleCount * _channels * _bitsPerSample / 8);
    }
    if (sampleCount > 0)
    {
        snd_pcm_sframes_t num = snd_pcm_writei(_handle, buf, sampleCount);
        int64_t cost = msecSinceStart() - now;
        if (num < 0)
        {
            LOG_ALSA_ERR(snd_pcm_writei, (int)num);
            int slient = 1;
            int err = snd_pcm_recover(_handle, (int)num, slient);
            CHECK_ALSA_ERR(snd_pcm_recover, err);
        }
        else
        {
            if (num < sampleCount)
            {
                ULOG("!!Did not write all samples: %ld/%ld, cache %u ms, cost %lld ms",
                    num, sampleCount, cacheMs, cost);
            }
            else
            {
                VLOG("Write samples: %ld, cache %u ms, cost %lld ms", num, cacheMs, cost);
            }
        }
    }
}

void AlsaDevice::getParams(snd_pcm_hw_params_t* hwParams)
{
    int err = 0;
    int direction = 0;

    const char* pcmName = snd_pcm_name(_handle);
    const char* pcmState = snd_pcm_state_name(snd_pcm_state(_handle));

    snd_pcm_access_t access;
    err = snd_pcm_hw_params_get_access(hwParams, &access);
    CHECK_ALSA_ERR_RET(snd_pcm_hw_params_get_access, err);
    const char* accessName = snd_pcm_access_name(access);

    snd_pcm_format_t format;
    err = snd_pcm_hw_params_get_format(hwParams, &format);
    CHECK_ALSA_ERR_RET(snd_pcm_hw_params_get_format, err);
    const char* formatName = snd_pcm_format_name(format);
    const char* formatDesc= snd_pcm_format_description(format);

    snd_pcm_subformat_t subformat;
    err = snd_pcm_hw_params_get_subformat(hwParams, &subformat);
    CHECK_ALSA_ERR_RET(snd_pcm_hw_params_get_subformat, err);
    const char* subformatName = snd_pcm_subformat_name(subformat);
    const char* subformatDesc = snd_pcm_subformat_description(subformat);

    ULOG("pcmName=%s,\n pcmState=%s, accessName=%s,\n formatName=%s, formatDesc=%s"
        ",\n subformatName=%s, subformatDesc=%s",
        pcmName, pcmState, accessName, formatName, formatDesc, subformatName, subformatDesc);

    unsigned int channels = 0;
    err = snd_pcm_hw_params_get_channels(hwParams, &channels);
    CHECK_ALSA_ERR_RET(snd_pcm_hw_params_get_channels, err);
    _channels = channels;

    unsigned int sampleRate = 0;
    err = snd_pcm_hw_params_get_rate(hwParams, &sampleRate, &direction);
    CHECK_ALSA_ERR_RET(snd_pcm_hw_params_get_rate, err);
    _sampleRate = sampleRate;

    unsigned int periods = 0;
    err = snd_pcm_hw_params_get_periods(hwParams, &periods, &direction);
    CHECK_ALSA_ERR_RET(snd_pcm_hw_params_get_periods, err);

    unsigned int periodTime = 0;
    err = snd_pcm_hw_params_get_period_time(hwParams, &periodTime, &direction);
    CHECK_ALSA_ERR_RET(snd_pcm_hw_params_get_period_time, err);

    snd_pcm_uframes_t periodSize = 0; // %lu
    err = snd_pcm_hw_params_get_period_size(hwParams, &periodSize, &direction);
    CHECK_ALSA_ERR_RET(snd_pcm_hw_params_get_period_size, err);

    snd_pcm_uframes_t bufferSize = 0;
    err = snd_pcm_hw_params_get_buffer_size(hwParams, &bufferSize);
    CHECK_ALSA_ERR_RET(snd_pcm_hw_params_get_buffer_size, err);

    ULOG("sampleRate=%u, channels=%u,\n periods=%u, periodTime=%u us, periodSize=%lu, bufferSize=%lu",
        sampleRate, channels, periods, periodTime, periodSize, bufferSize);
}
