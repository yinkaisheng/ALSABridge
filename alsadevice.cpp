#include <cstdio>
#include <cmath>
#include <cstring>
#include <cctype>
#include <memory>
#include <thread>
#include <algorithm>
#include "alsadevice.h"

#define UTIL_IMPLEMENTION
#include "util.h"

#define DEFAULT_PERIOD_COUNT 10
#define DEFAULT_PERIOD_TIME 20
#define FEED_ZERO_PERIOD_COUNT 2


int enumerateAlsaSoundcards(bool isCaptureDevice, AlsaEnumCallback callback, void* user);
void pcmList(snd_pcm_stream_t stream);


DLL_EXPORT int enumerateAlsaCaptureDevices(AlsaEnumCallback callback, void* user)
{
    return enumerateAlsaSoundcards(true, callback, user);
}

DLL_EXPORT int enumerateAlsaPlaybackDevices(AlsaEnumCallback callback, void* user)
{
    return enumerateAlsaSoundcards(false, callback, user);
}

static int writeUnavailable(char* out, int outSize, const char* reason)
{
    if (!out || outSize <= 0)
        return 0;
    std::snprintf(out, (size_t)outSize, "unavailable: %s", reason ? reason : "unknown");
    return 0;
}

static void fillResolvedDeviceInfo(
    snd_pcm_t* handle,
    char* resolvesBuf, size_t resolvesSize,
    char* cardBuf, size_t cardSize,
    char* typeBuf, size_t typeSize)
{
    const char* typeName = snd_pcm_type_name(snd_pcm_type(handle));
    std::snprintf(typeBuf, typeSize, "%s", typeName ? typeName : "unknown");

    snd_pcm_info_t* pcmInfo = nullptr;
    snd_pcm_info_alloca(&pcmInfo);
    int err = snd_pcm_info(handle, pcmInfo);
    if (err < 0)
    {
        const char* pcmName = snd_pcm_name(handle);
        std::snprintf(resolvesBuf, resolvesSize, "%s (info unavailable)", pcmName ? pcmName : "?");
        std::snprintf(cardBuf, cardSize, "(none)");
        return;
    }

    int card = snd_pcm_info_get_card(pcmInfo);
    unsigned int device = snd_pcm_info_get_device(pcmInfo);
    unsigned int subdevice = snd_pcm_info_get_subdevice(pcmInfo);
    unsigned int subdevices = snd_pcm_info_get_subdevices_count(pcmInfo);
    const char* pcmId = snd_pcm_info_get_id(pcmInfo);
    const char* pcmName = snd_pcm_info_get_name(pcmInfo);

    if (card < 0)
    {
        const char* openName = snd_pcm_name(handle);
        std::snprintf(
            resolvesBuf, resolvesSize, "%s (logical, no hw card)",
            openName ? openName : (pcmId ? pcmId : "?"));
        std::snprintf(
            cardBuf, cardSize, "(none)%s%s",
            pcmName && pcmName[0] ? ", " : "",
            pcmName && pcmName[0] ? pcmName : "");
        return;
    }

    char hwCard[32];
    std::snprintf(hwCard, sizeof(hwCard), "hw:%d", card);

    char cardIdBuf[64] = {};
    char cardNameBuf[128] = {};
    snd_ctl_t* ctl = nullptr;
    if (snd_ctl_open(&ctl, hwCard, SND_CTL_NONBLOCK) >= 0)
    {
        snd_ctl_card_info_t* cardInfo = nullptr;
        snd_ctl_card_info_alloca(&cardInfo);
        if (snd_ctl_card_info(ctl, cardInfo) >= 0)
        {
            const char* cardId = snd_ctl_card_info_get_id(cardInfo);
            const char* cardName = snd_ctl_card_info_get_name(cardInfo);
            if (cardId && cardId[0])
                std::snprintf(cardIdBuf, sizeof(cardIdBuf), "%s", cardId);
            if (cardName && cardName[0])
                std::snprintf(cardNameBuf, sizeof(cardNameBuf), "%s", cardName);
        }
        snd_ctl_close(ctl);
    }

    if (cardIdBuf[0])
    {
        if (subdevices > 1)
            std::snprintf(resolvesBuf, resolvesSize, "hw:%s,%u,%u", cardIdBuf, device, subdevice);
        else
            std::snprintf(resolvesBuf, resolvesSize, "hw:%s,%u", cardIdBuf, device);
    }
    else
    {
        if (subdevices > 1)
            std::snprintf(resolvesBuf, resolvesSize, "hw:%d,%u,%u", card, device, subdevice);
        else
            std::snprintf(resolvesBuf, resolvesSize, "hw:%d,%u", card, device);
    }

    const char* displayName = cardNameBuf[0] ? cardNameBuf
        : (pcmName && pcmName[0] ? pcmName : "unknown");
    std::snprintf(cardBuf, cardSize, "%s (%s)", hwCard, displayName);
}

DLL_EXPORT int queryAlsaDeviceHwParams(const char* deviceId, int isCapture, char* out, int outSize)
{
    if (!out || outSize <= 0)
        return 0;
    out[0] = '\0';
    if (!deviceId || !deviceId[0])
        return writeUnavailable(out, outSize, "empty device id");

    snd_pcm_t* handle = nullptr;
    snd_pcm_stream_t stream = isCapture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
    int err = snd_pcm_open(&handle, deviceId, stream, SND_PCM_NONBLOCK);
    if (err < 0)
        return writeUnavailable(out, outSize, snd_strerror(err));

    char resolvesBuf[128] = {};
    char cardBuf[128] = {};
    char typeBuf[64] = {};
    fillResolvedDeviceInfo(
        handle,
        resolvesBuf, sizeof(resolvesBuf),
        cardBuf, sizeof(cardBuf),
        typeBuf, sizeof(typeBuf));

    snd_pcm_hw_params_t* hwParams = nullptr;
    snd_pcm_hw_params_alloca(&hwParams);
    err = snd_pcm_hw_params_any(handle, hwParams);
    if (err < 0)
    {
        const char* reason = snd_strerror(err);
        snd_pcm_close(handle);
        return writeUnavailable(out, outSize, reason);
    }

    unsigned int chMin = 0, chMax = 0;
    err = snd_pcm_hw_params_get_channels_min(hwParams, &chMin);
    if (err < 0)
    {
        const char* reason = snd_strerror(err);
        snd_pcm_close(handle);
        return writeUnavailable(out, outSize, reason);
    }
    err = snd_pcm_hw_params_get_channels_max(hwParams, &chMax);
    if (err < 0)
    {
        const char* reason = snd_strerror(err);
        snd_pcm_close(handle);
        return writeUnavailable(out, outSize, reason);
    }

    unsigned int rateMin = 0, rateMax = 0;
    err = snd_pcm_hw_params_get_rate_min(hwParams, &rateMin, nullptr);
    if (err < 0)
    {
        const char* reason = snd_strerror(err);
        snd_pcm_close(handle);
        return writeUnavailable(out, outSize, reason);
    }
    err = snd_pcm_hw_params_get_rate_max(hwParams, &rateMax, nullptr);
    if (err < 0)
    {
        const char* reason = snd_strerror(err);
        snd_pcm_close(handle);
        return writeUnavailable(out, outSize, reason);
    }

    static const snd_pcm_format_t kFormats[] = {
        SND_PCM_FORMAT_S8,
        SND_PCM_FORMAT_U8,
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_FORMAT_S16_BE,
        SND_PCM_FORMAT_S24_LE,
        SND_PCM_FORMAT_S24_3LE,
        SND_PCM_FORMAT_S32_LE,
        SND_PCM_FORMAT_FLOAT_LE,
    };

    char formatsBuf[256] = {};
    size_t formatsLen = 0;
    for (snd_pcm_format_t fmt : kFormats)
    {
        if (snd_pcm_hw_params_test_format(handle, hwParams, fmt) != 0)
            continue;
        const char* name = snd_pcm_format_name(fmt);
        if (!name)
            continue;
        int n = std::snprintf(
            formatsBuf + formatsLen,
            sizeof(formatsBuf) - formatsLen,
            "%s%s",
            formatsLen ? ", " : "",
            name);
        if (n < 0 || (size_t)n >= sizeof(formatsBuf) - formatsLen)
            break;
        formatsLen += (size_t)n;
    }
    if (formatsLen == 0)
        std::snprintf(formatsBuf, sizeof(formatsBuf), "(none probed)");

    char channelsBuf[32];
    if (chMin == chMax)
        std::snprintf(channelsBuf, sizeof(channelsBuf), "%u", chMin);
    else
        std::snprintf(channelsBuf, sizeof(channelsBuf), "%u-%u", chMin, chMax);

    char ratesBuf[48];
    if (rateMin == rateMax)
        std::snprintf(ratesBuf, sizeof(ratesBuf), "%u", rateMin);
    else
        std::snprintf(ratesBuf, sizeof(ratesBuf), "%u-%u", rateMin, rateMax);

    int written = std::snprintf(
        out, (size_t)outSize,
        "resolves: %s\ncard    : %s\ntype    : %s\nchannels: %s\nrates   : %s\nformats : %s",
        resolvesBuf, cardBuf, typeBuf, channelsBuf, ratesBuf, formatsBuf);
    snd_pcm_close(handle);
    if (written < 0)
        return writeUnavailable(out, outSize, "format failed");
    return 1;
}

#define REQUIRE_HANDLE(handle) \
    do { \
        if (!(handle)) \
            return 0; \
    } while (0)

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
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->open(deviceId);
}

DLL_EXPORT int AlsaDevice_setPeriod(size_t handle, uint32_t periodCount, uint32_t periodTime)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->setPeriod(periodCount, periodTime);
}

DLL_EXPORT int AlsaDevice_setParams(size_t handle, uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->setParams(sampleRate, channels, bitsPerSample);
}

DLL_EXPORT int AlsaDevice_start(size_t handle)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->start();
}

DLL_EXPORT int AlsaDevice_stop(size_t handle)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->stop();
}

DLL_EXPORT int AlsaDevice_close(size_t handle)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->close();
}

DLL_EXPORT int AlsaDevice_getVolume(size_t handle, const char* cardId)
{
    if (!handle)
        return -1;
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->getVolume(cardId);
}

DLL_EXPORT int AlsaDevice_setVolume(size_t handle, const char* cardId, int volume)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    return alsa->setVolume(cardId, volume);
}

DLL_EXPORT int AlsaCaptureDevice_setOutputCallback(size_t handle, AlsaOutputDataCallback callback, void* user)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    AlsaCaptureDevice* cap = (AlsaCaptureDevice*)alsa;
    return cap->setOutputCallback(callback, user);
}

DLL_EXPORT int AlsaCaptureDevice_captureToPcmFile(size_t handle, const char* pcmName, int append)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    AlsaCaptureDevice* cap = (AlsaCaptureDevice*)alsa;
    return cap->captureToPcmFile(pcmName, (bool)append);
}

DLL_EXPORT int AlsaPlaybackDevice_setMinCachePeriodCount(size_t handle, uint32_t minCachePeriodCount)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    AlsaPlaybackDevice* play = (AlsaPlaybackDevice*)alsa;
    return play->setMinCachePeriodCount(minCachePeriodCount);
}

DLL_EXPORT int AlsaPlaybackDevice_setInputCallback(size_t handle, AlsaInputDataCallback callback, void* user)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    AlsaPlaybackDevice* play = (AlsaPlaybackDevice*)alsa;
    return play->setInputCallback(callback, user);
}

DLL_EXPORT int AlsaPlaybackDevice_syncStop(size_t handle)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    AlsaPlaybackDevice* play = (AlsaPlaybackDevice*)alsa;
    return play->syncStop();
}

DLL_EXPORT int AlsaPlaybackDevice_asyncStop(size_t handle, AlsaPlaybackStoppedCallback callback, void* user)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    AlsaPlaybackDevice* play = (AlsaPlaybackDevice*)alsa;
    return play->asyncStop(callback, user);
}

DLL_EXPORT int AlsaPlaybackDevice_pause(size_t handle)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    AlsaPlaybackDevice* play = (AlsaPlaybackDevice*)alsa;
    return play->pause();
}

DLL_EXPORT int AlsaPlaybackDevice_resume(size_t handle)
{
    REQUIRE_HANDLE(handle);
    AlsaDevice* alsa = (AlsaDevice*)handle;
    AlsaPlaybackDevice* play = (AlsaPlaybackDevice*)alsa;
    return play->resume();
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
                            ssid << "hw:" << cardId << "," << device << "," << subDevice;
                            ssname << cardName << ", " << snd_pcm_info_get_name (pcmInfo)
                                << " {" <<  snd_pcm_info_get_subdevice_name (pcmInfo) << "}";
                        }

                        std::string name = ssname.str();
                        std::string id = ssid.str();
                        printf("%s device:\n  card: %s\n  id  : %s\n  name: %s\n", 
                            isCaptureDevice ? "capture" : "playback", hwname, id.c_str(), name.c_str());
                        if (callback)
                        {
                            callback(deviceCount, hwname, name.c_str(), id.c_str(), user);
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

AlsaDevice::AlsaDevice()
    : _periodCount(DEFAULT_PERIOD_COUNT)
    , _periodTime(DEFAULT_PERIOD_TIME)
{

}

AlsaDevice::~AlsaDevice()
{
    if (_handle)
        close();
}

AlsaCaptureDevice::~AlsaCaptureDevice()
{
    if (_handle)
        close();
}

AlsaPlaybackDevice::AlsaPlaybackDevice()
    : _minCachePeriodCount(FEED_ZERO_PERIOD_COUNT)
{
    
}
AlsaPlaybackDevice::~AlsaPlaybackDevice()
{
    if (_handle)
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
    // 0 block, SND_PCM_NONBLOCK, SND_PCM_ASYNC; I/O runs on worker thread
    int err = snd_pcm_open(&_handle, deviceId, streamType, 0);
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

bool AlsaDevice::setPeriod(uint32_t periodCount, uint32_t periodTime)
{
    if (_sampleRate)
        return false;
    _periodCount = periodCount;
    if (_periodCount < 5) _periodCount = 5;
    _periodTime = periodTime;
    if (_periodTime < 10) _periodTime = 10;
    //if (_periodTime > 100) _periodTime = 100;
    return true;
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
    else if (bitsPerSample == 16)
    {
        format = SND_PCM_FORMAT_S16_LE;
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
    _channels = channels;

    err = snd_pcm_hw_params_set_rate_near(_handle, hwParams, &sampleRate, &direction);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_hw_params_set_rate_near, err);
    _sampleRate = sampleRate;

    err = snd_pcm_hw_params_set_periods_near (_handle, hwParams, &_periodCount, &direction);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_hw_params_set_periods_near, err);

    snd_pcm_uframes_t samplesPerPeriod = _periodTime * sampleRate / 1000; // %lu
    err = snd_pcm_hw_params_set_period_size_near (_handle, hwParams, &samplesPerPeriod, &direction);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_hw_params_set_period_size_near, err);

    err = snd_pcm_hw_params(_handle, hwParams);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_hw_params, err);

    getParams(hwParams);
    return true;
}

bool AlsaPlaybackDevice::setMinCachePeriodCount(uint32_t minPeriodCount)
{
    if (_thread)
        return false;
    _minCachePeriodCount = minPeriodCount;
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

bool AlsaCaptureDevice::captureToPcmFile(const char* pcmName, bool append)
{
    if (_thread)
        return false;
    _pcmFile = fopen(pcmName, append ? "ab+" : "wb");
    return _pcmFile != nullptr;
}

bool AlsaDevice::start()
{
    if (_handle == nullptr)
        return false;
    if (_thread)
        return false;

    _thread = new std::thread(threadFunc, this);
    ULOG("create thread %p", _thread);

    return true;
}

bool AlsaCaptureDevice::start()
{
    if (_handle == nullptr)
        return false;
    if (_thread)
        return false;

    int err = snd_pcm_prepare(_handle);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_prepare, err);

    err = snd_pcm_start(_handle);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_start, err);

    return AlsaDevice::start();
}

bool AlsaDevice::stop()
{
    if (_thread == nullptr)
        return false;

    _threadShouldExit = true;
    _thread->join();
    ULOG("AlsaDevice delete thread %p", _thread);
    delete _thread;
    _thread = nullptr;
    _threadShouldExit = false;
    _totalHandledSamples = 0;
    return true;
}

bool AlsaCaptureDevice::stop()
{
    bool ret = AlsaDevice::stop();
    if (!ret)
        return ret;

    closeFile();

    int err = snd_pcm_drop(_handle);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_drop, err);
    ULOG("AlsaCaptureDevice");
    return true;
}

bool AlsaPlaybackDevice::stop()
{
    bool ret = AlsaDevice::stop();
    if (!ret)
        return ret;

    //int64_t start = msecSinceStart();
    int err = snd_pcm_drop(_handle);
    //int64_t cost = msecSinceStart() - start;
    //ULOGIF(cost>10, "cost %lld ms", cost);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_drop, err);
    _paused = false;
    _totalCapacitySamples = 0;
    ULOG("AlsaPlaybackDevice");
    return true;
}

bool AlsaPlaybackDevice::syncStop()
{
    bool ret = AlsaDevice::stop();
    if (!ret)
        return ret;

    int64_t start = msecSinceStart();
    int err = snd_pcm_drain(_handle);
    int64_t cost = msecSinceStart() - start;
    ULOGIF(cost>5, "cost %lld ms", cost);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_drain, err);
    _paused = false;
    _totalCapacitySamples = 0;
    ULOG("AlsaPlaybackDevice");
    return true;
}

bool AlsaPlaybackDevice::asyncStop(AlsaPlaybackStoppedCallback callback, void* user)
{
    if (_thread == nullptr)
        return false;
    _stopCallback = callback;
    _stopUserData = user;
    _threadShouldExit = true;
    ULOG("AlsaPlaybackDevice");
    return true;
}

void AlsaPlaybackDevice::threadStopped()
{
    //ULOG("AlsaPlaybackDevice %p, thread %p", this, _thread);
    if (_stopCallback == nullptr)
        return;

    int64_t start = msecSinceStart();
    int err = snd_pcm_drain(_handle); // will block current thread
    int64_t cost = msecSinceStart() - start;
    ULOG("AlsaPlaybackDevice snd_pcm_drain cost %lld ms", msecSinceStart() - start);
    CHECK_ALSA_ERR(snd_pcm_drain, err);
    _stopCallback(_stopUserData);

    _stopCallback = nullptr;
    _stopUserData = nullptr;
    _paused = false;
    _totalCapacitySamples = 0;
    _threadShouldExit = false;
}

bool AlsaPlaybackDevice::pause()
{
    if (_thread == nullptr)
        return false;

    int err = snd_pcm_pause(_handle, 1);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_pause, err);
    _paused = true;
    return true;
}

bool AlsaPlaybackDevice::resume()
{
    if (_thread == nullptr)
        return false;

    int err = snd_pcm_pause(_handle, 0);
    //err = snd_pcm_resume(_handle);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_pause, err);
    _paused = false;
    return true;
}

bool AlsaDevice::close()
{
    if (_handle == nullptr)
        return false;

    if (_thread)
        stop();

    int err = snd_pcm_close(_handle);
    CHECK_ALSA_ERR_RET_FALSE(snd_pcm_close, err);

    _deviceId.clear();
    _handle = nullptr;
    _periodCount = DEFAULT_PERIOD_COUNT;
    _periodTime = DEFAULT_PERIOD_TIME;
    _sampleRate = 0;
    _channels = 0;
    _bitsPerSample = 0;
    return true;
}

bool AlsaCaptureDevice::close()
{
    bool ret = AlsaDevice::close();
    if (!ret)
        return false;

    _outputCallback = nullptr;
    _userData = nullptr;
    ULOG("AlsaCaptureDevice");
    return true;
}

bool AlsaPlaybackDevice::close()
{
    bool ret = AlsaDevice::close();
    if (!ret)
        return false;

    _minCachePeriodCount = FEED_ZERO_PERIOD_COUNT;
    _totalCapacitySamples = 0;
    _inputCallback = nullptr;
    _userData = nullptr;
    _paused = false;
    ULOG("AlsaPlaybackDevice");
    return true;
}

void AlsaDevice::threadFunc(AlsaDevice* pThis)
{
    int64_t startTick = msecSinceStart();
    pThis->threadStarted();
    pThis->run();
    pThis->threadStopped();
    ULOG("exit, ran for %lld ms", msecSinceStart() - startTick);
}

void AlsaDevice::threadStarted()
{
    ULOG("AlsaDevice %p, thread %p", this, _thread);
}

void AlsaDevice::threadStopped()
{
    ULOG("AlsaDevice %p, thread %p", this, _thread);
}

void AlsaPlaybackDevice::run()
{
    int err = snd_pcm_prepare(_handle);
    if (err < 0)
    {
        ULOG("snd_pcm_prepare failed in playback thread, error=%d(%s)", err, snd_strerror(err));
        return;
    }

    snd_pcm_sw_params_t* swParams;
    snd_pcm_sw_params_alloca(&swParams);
    err = snd_pcm_sw_params_current(_handle, swParams);
    if (err >= 0)
    {
        snd_pcm_uframes_t startThreshold = 0;
        snd_pcm_uframes_t bufferSize = 0;
        snd_pcm_uframes_t periodSize = 0;
        snd_pcm_sw_params_get_start_threshold(swParams, &startThreshold);
        snd_pcm_get_params(_handle, &bufferSize, &periodSize);
        if (_sampleRate > 0)
        {
            uint64_t thresholdUs = startThreshold * 1000000ULL / _sampleRate;
            ULOG("playback sw params: sample_rate=%u, start_threshold=%lu frames (~%llu us), buffer_size=%lu, period_size=%lu",
                _sampleRate, (unsigned long)startThreshold, (unsigned long long)thresholdUs,
                (unsigned long)bufferSize, (unsigned long)periodSize);
        }
        else
        {
            ULOG("playback sw params: sample_rate=0 (setParams not called?), start_threshold=%lu frames, buffer_size=%lu, period_size=%lu",
                (unsigned long)startThreshold, (unsigned long)bufferSize, (unsigned long)periodSize);
        }
    }
    else
    {
        ULOG("snd_pcm_sw_params_current failed, error=%d(%s)", err, snd_strerror(err));
    }

    AlsaDevice::run();
}

void AlsaDevice::run()
{
    int err = 0;
    int timeout = 1000;
    int64_t startTick = msecSinceStart();
    int64_t now = 0;
    int64_t cost = 0;
    snd_pcm_sframes_t sampleCount = _periodTime * _sampleRate / 1000;
    //uint32_t sampleBytes = sampleCount * _channels * _bitsPerSample / 8;
    std::unique_ptr<char> buffer{new char[sampleCount * _channels * _bitsPerSample / 8]};

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

        now = msecSinceStart();
        snd_pcm_sframes_t avail = snd_pcm_avail_update(_handle);

        if (avail < 0)
        {
            ULOG("snd_pcm_avail_update returns %d(%s), call snd_pcm_recover",
                (int)avail, snd_strerror(avail));
            err = snd_pcm_recover(_handle, (int)avail, 0); // slient = 0
            CHECK_ALSA_ERR(snd_pcm_recover, err);
        }
        else
        {
            VLOG("snd_pcm_avail_update returns %ld, cost %lld ms",
                avail, msecSinceStart() - now);
        }

        handleData(startTick, avail, buffer.get(), sampleCount);
    }
}

void AlsaCaptureDevice::handleData(int64_t startTick, int avail, char* buffer, uint32_t sampleCount)
{
    int64_t now = msecSinceStart();
    // if there is not enough samples available, snd_pcm_readi will cost at most one period time
    snd_pcm_sframes_t num = snd_pcm_readi(_handle, buffer, sampleCount);
    int64_t cost = msecSinceStart() - now;
    if (num < 0)
    {
        LOG_ALSA_ERR(snd_pcm_readi, (int)num);
        int err = snd_pcm_recover(_handle, (int)num, 1); // slient = 1
        CHECK_ALSA_ERR(snd_pcm_recover, err);
    }
    else
    {
        if (_totalHandledSamples == 0)
        {
            ULOG("first avail %d, get %ld samples", avail, num);
        }
        _totalHandledSamples += num;
        if (_pcmFile)
        {
            fwrite(buffer, num * _channels * _bitsPerSample / 8, 1, _pcmFile);
        }
        if (_outputCallback)
        {
            if (avail < 0)
                avail = 0;  // likely overrun
            uint32_t cacheMs = 0;
            if (_sampleRate > 0)
                cacheMs = (uint32_t)((int64_t)avail * 1000 / _sampleRate);
            _outputCallback(cacheMs, buffer, num, _userData);
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

template <typename T>
void volumeData(T* sample, size_t count, int volume)
{
    for (size_t n = 0; n < count; ++n)
    {
        *sample = (T)((*sample) * volume / 100);
        ++sample;
    }
}

void AlsaPlaybackDevice::handleData(int64_t startTick, int avail, char* buffer, uint32_t sampleCount)
{
    if (_paused)
    {
        sleepMSec(5);
        return;
    }
    if (_totalCapacitySamples == 0)
    {
        // internal buffer is double of _periodCount
        _totalCapacitySamples = ((avail - 1) / sampleCount + 1) * sampleCount;
        ULOG("first avail %d, alsa internal total buffer %u ms", avail, _totalCapacitySamples * 1000 / _sampleRate);
        //_totalCapacitySamples = sampleCount * _periodCount;
    }
    if (avail < 0)
        avail = _totalCapacitySamples;   // likely underrun
    /*else if (avail >= _totalCapacitySamples)
    {
        avail -= _totalCapacitySamples;
    }*/
    uint32_t cachedSamples = _totalCapacitySamples - avail;
    uint32_t cacheMs = 0;
    if (_sampleRate > 0)
        cacheMs = cachedSamples * 1000 / _sampleRate;
    if (_totalHandledSamples > 0 && cachedSamples >= _periodCount * sampleCount)
    {
        // buffer is enough; skip only after initial prefill
        sleepMSec(_periodTime);
        return;
    }

    if (_inputCallback)
    {
        snd_pcm_sframes_t delay = 0;
        int err = snd_pcm_delay(_handle, &delay);
        CHECK_ALSA_ERR(snd_pcm_delay, err);
        cacheMs = delay * 1000 / _sampleRate;

        uint32_t gotSamples = sampleCount;
        _inputCallback(cacheMs, buffer, &gotSamples, _userData);
        if (gotSamples > sampleCount)
            gotSamples = sampleCount;
        if (gotSamples < sampleCount)
        {
            size_t frameBytes = (size_t)_channels * _bitsPerSample / 8;
            memset(buffer + gotSamples * frameBytes, 0, (sampleCount - gotSamples) * frameBytes);
        }
        _totalHandledSamples += gotSamples;
        //ULOG("write total %u ms", _totalHandledSamples*1000/_sampleRate);
        if (gotSamples == 0)
        {
            // in vmware, the left cache should larger, otherwise maybe underrun
            if (cachedSamples <= _minCachePeriodCount * sampleCount)
            {
                gotSamples = sampleCount;
                memset(buffer, 0, gotSamples * _channels * _bitsPerSample / 8);
                VLOG("callback doesn't feed data, avail %d, cache %u ms, fill %u ms 0 data, elapse %lld ms", 
                    avail, cacheMs, _periodTime, msecSinceStart() - startTick);
            }
            else
            {
                VLOG("callback doesn't feed data, avail %d, cache %u ms, sleep %d ms, elapse %lld ms", 
                    avail, cacheMs, _periodTime/4, msecSinceStart() - startTick);
                sleepMSec(_periodTime / 4);
            }
        }
        else
        {
            if (_volume == 0)
            {
                memset(buffer, 0, gotSamples * _channels * _bitsPerSample / 8);
            }
            else if (_volume < 100)
            {
                if (_bitsPerSample == 8)
                {
                    int8_t* sample = (int8_t*)buffer;
                    volumeData(sample, gotSamples*_channels, _volume);
                }
                else if (_bitsPerSample == 16)
                {
                    int16_t* sample = (int16_t*)buffer;
                    volumeData(sample, gotSamples*_channels, _volume);
                }
                else if (_bitsPerSample == 24)
                {
                    char* bytes = buffer;
                    int bCount = gotSamples * _channels * _bitsPerSample / 8;
                    int sample = 0;
                    for (int n=0; n<bCount; n+=3)
                    {
                        sample = (bytes[2] << 16)|(bytes[1]<<8)|bytes[0];
                        if (sample & 0x800000)
                        {
                            sample |= ~0xffffff;
                        }
                        sample = sample * _volume / 100;
                        bytes[0] = sample & 0xff;
                        bytes[1] = (sample >> 8) & 0xff;
                        bytes[2] = (sample >> 16) & 0xff;
                        bytes += 3;
                    }
                }
                else if (_bitsPerSample == 32)
                {
                    int32_t* sample = (int32_t*)buffer;
                    volumeData(sample, gotSamples*_channels, _volume);
                }
            }
        }
        sampleCount = gotSamples;
    }
    else
    {
        _totalHandledSamples += sampleCount;
        VLOG("_inputCallback is null, avail %d, fill 0, cache %u ms", avail, cacheMs);
        memset(buffer, 0, sampleCount * _channels * _bitsPerSample / 8);
    }

    if (sampleCount > 0)
    {
        int64_t now = msecSinceStart();
        snd_pcm_sframes_t num = snd_pcm_writei(_handle, buffer, sampleCount);
        int64_t cost = msecSinceStart() - now;
        if (num < 0)
        {
            LOG_ALSA_ERR(snd_pcm_writei, (int)num);
            int err = snd_pcm_recover(_handle, (int)num, 1); // slient = 1
            CHECK_ALSA_ERR(snd_pcm_recover, err);
            num = snd_pcm_writei(_handle, buffer, sampleCount);
        }
        if (num < 0)
        {
            LOG_ALSA_ERR(snd_pcm_writei, (int)num);
        }
        else
        {
            if (num < sampleCount)
            {
                ULOG("!!Avail %d, did not write all samples %ld/%ld, cache %u ms, cost %lld ms, elapse %lld ms",
                    avail, num, sampleCount, cacheMs, cost, msecSinceStart() - startTick);
            }
            else
            {
                if (cacheMs == 0)
                {
                    VLOG("Avail %d, write samples %ld, cache %u ms, cost %lld ms, elapse %lld ms", 
                        avail, num, cacheMs, cost, msecSinceStart() - startTick);
                }
                else
                {
                    VLOG("Avail %d, write samples: %ld, cache %u ms, cost %lld ms, elapse %lld ms", 
                        avail, num, cacheMs, cost, msecSinceStart() - startTick);
                }
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

void AlsaCaptureDevice::closeFile()
{
    if (_pcmFile)
    {
        fclose(_pcmFile);
        _pcmFile = nullptr;
    }
}

static bool nameEqualsIgnoreCase(const char* a, const char* b)
{
    if (!a || !b)
        return false;
    while (*a && *b)
    {
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b))
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

static int mixerControlPriority(const char* name)
{
    if (!name)
        return 100;
    if (nameEqualsIgnoreCase(name, "Master"))
        return 0;
    if (nameEqualsIgnoreCase(name, "PCM"))
        return 1;
    if (nameEqualsIgnoreCase(name, "Speaker") || nameEqualsIgnoreCase(name, "Headphone"))
        return 2;
    if (nameEqualsIgnoreCase(name, "Capture") || nameEqualsIgnoreCase(name, "Mic"))
        return 2;
    return 50;
}

bool getOrSetVolume(const char* cardId, bool isCaptureDevice, int* getVolume, int* setVolume)
{
    if (!cardId || !cardId[0])
    {
        ULOG("getOrSetVolume: empty cardId");
        return false;
    }

    bool ret{};
    int err{};
    snd_mixer_t* handle{};
    err = snd_mixer_open(&handle, 0);
    if (err < 0)
    {
        ULOG("snd_mixer_open failed, error=%d(%s)", err, snd_strerror(err));
        return false;
    }
    err = snd_mixer_attach(handle, cardId);
    if (err < 0)
    {
        ULOG("snd_mixer_attach('%s') failed, error=%d(%s)", cardId, err, snd_strerror(err));
        snd_mixer_close(handle);
        return false;
    }
    err = snd_mixer_selem_register(handle, NULL, NULL);
    if (err < 0)
    {
        ULOG("snd_mixer_selem_register failed, error=%d(%s)", err, snd_strerror(err));
        snd_mixer_close(handle);
        return false;
    }
    err = snd_mixer_load(handle);
    if (err < 0)
    {
        ULOG("snd_mixer_load failed, error=%d(%s)", err, snd_strerror(err));
        snd_mixer_close(handle);
        return false;
    }

    snd_mixer_elem_t* bestElem = nullptr;
    int bestPriority = 1000;
    snd_mixer_selem_id_t* sid{};
    snd_mixer_selem_id_alloca(&sid);

    for (snd_mixer_elem_t* elem = snd_mixer_first_elem(handle); elem; elem = snd_mixer_elem_next(elem))
    {
        snd_mixer_selem_get_id(elem, sid);
        if (!snd_mixer_selem_is_active(elem))
            continue;

        int hasCaptureVolume = snd_mixer_selem_has_capture_volume(elem);
        int hasPlaybackVolume = snd_mixer_selem_has_playback_volume(elem);
        int hasCommonVolume = snd_mixer_selem_has_common_volume(elem);
        bool usable = isCaptureDevice
            ? (hasCommonVolume || hasCaptureVolume)
            : (hasCommonVolume || hasPlaybackVolume);
        if (!usable)
            continue;

        const char* name = snd_mixer_selem_id_get_name(sid);
        int priority = mixerControlPriority(name);
        if (priority < bestPriority)
        {
            bestPriority = priority;
            bestElem = elem;
        }
    }

    if (!bestElem)
    {
        ULOG("getOrSetVolume: no volume control on card '%s'", cardId);
        snd_mixer_close(handle);
        return false;
    }

    long vmin = 0, vmax = 0, vcur = 0;
    if (isCaptureDevice)
        err = snd_mixer_selem_get_capture_volume_range(bestElem, &vmin, &vmax);
    else
        err = snd_mixer_selem_get_playback_volume_range(bestElem, &vmin, &vmax);
    if (err < 0 || vmax <= vmin)
    {
        ULOG("getOrSetVolume: invalid volume range on '%s'", cardId);
        snd_mixer_close(handle);
        return false;
    }

    for (int n = 0; n <= SND_MIXER_SCHN_LAST; ++n)
    {
        snd_mixer_selem_channel_id_t ch = (snd_mixer_selem_channel_id_t)n;
        bool hasCh = isCaptureDevice
            ? snd_mixer_selem_has_capture_channel(bestElem, ch)
            : snd_mixer_selem_has_playback_channel(bestElem, ch);
        if (!hasCh)
            continue;

        if (isCaptureDevice)
            err = snd_mixer_selem_get_capture_volume(bestElem, ch, &vcur);
        else
            err = snd_mixer_selem_get_playback_volume(bestElem, ch, &vcur);
        if (err < 0)
            continue;

        int percent = (int)rint((vcur - vmin) * 100.0 / (vmax - vmin));
        if (getVolume)
        {
            *getVolume = percent;
            ret = true;
        }
        if (setVolume && (*setVolume) >= 0 && (*setVolume) <= 100)
        {
            long target = vmin + (vmax - vmin) * (*setVolume) / 100;
            if (isCaptureDevice)
                err = snd_mixer_selem_set_capture_volume(bestElem, ch, target);
            else
                err = snd_mixer_selem_set_playback_volume(bestElem, ch, target);
            if (err == 0)
            {
                ULOG("set %s volume on '%s' to %d%%",
                    isCaptureDevice ? "capture" : "playback", cardId, *setVolume);
                ret = true;
            }
            else
            {
                ULOG("set volume failed on '%s', error=%d(%s)", cardId, err, snd_strerror(err));
            }
        }
        break; // first usable channel is enough
    }

    snd_mixer_close(handle);
    return ret;
}

int AlsaCaptureDevice::getVolume(const char* cardId)
{
    int volume = 0;
    if (!getOrSetVolume(cardId, true, &volume, nullptr))
        return -1;
    return volume;
}

bool AlsaCaptureDevice::setVolume(const char* cardId, int volume)
{
    return getOrSetVolume(cardId, true, nullptr, &volume);
}

int AlsaPlaybackDevice::getVolume(const char* cardId)
{
    if (cardId == nullptr || strcmp(cardId, "") == 0)
        return _volume;

    int volume = 0;
    if (!getOrSetVolume(cardId, false, &volume, nullptr))
        return -1;
    return volume;
}

bool AlsaPlaybackDevice::setVolume(const char* cardId, int volume)
{
    if (cardId == nullptr || strcmp(cardId, "") == 0)
    {
        if (volume >= 0 && volume <= 100)
        {
            _volume = volume;
            return true;
        }
        return false;
    }
    return getOrSetVolume(cardId, false, nullptr, &volume);
}
