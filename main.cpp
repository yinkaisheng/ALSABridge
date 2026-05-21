#include <stdio.h>
#include <fstream>
#include <memory>
#include <iostream>
#include <cmath>
#include "alsadevice.h"
#include "util.h"

struct PlaybackInfo
{
    FILE* file{};
    uint32_t sampleRate{};
    uint32_t channels{};
    uint32_t cacheTimeMs{};
    uint32_t commitSamples{};
    uint32_t playTime{};
    uint32_t lastPlayTime{};
    int64_t endTick{};
    bool readToEnd{};
    bool playToEnd{};
};

const char* timeStr(uint32_t seconds)
{
    static char time[32]{};
    pformat(time, sizeof(time), "%02u:%02u", seconds/60, seconds%60);
    return time;
}

void playDataCallback(uint32_t cacheTimeMs, char* data, uint32_t& samples, void* user)
{
    PlaybackInfo* info = (PlaybackInfo*)user;
    uint32_t read = fread(data, info->channels*2, samples, info->file);
    //ULOG("should read %u samples, read %u count", samples, read);
    samples = read;
    info->playTime = (info->commitSamples * 1000ULL / info->sampleRate - cacheTimeMs) / 1000;
    if (info->playTime != info->lastPlayTime)
    {
        ULOG("play time %s, cache %u ms", timeStr(info->playTime), cacheTimeMs);
        info->lastPlayTime = info->playTime;
    }

    info->commitSamples += read;

    if (read == 0)
    {
        #if 1
        info->readToEnd = true;
        info->cacheTimeMs = cacheTimeMs;
        #else
        //1
        // when play local file and reach to file's end, we don't feed data, cacheTimeMs will decrese.
        // when it decrese near 0, AlsaPlaybackDevice will feed one period of 0 data to avoid underrun,
        // cachedSamples will increse a little.
        if (info->playToEnd)
            return;
        
        if (info->endTick)
        {
            if (msecSinceStart() - info->endTick >= info->cacheTimeMs)
            {
                ULOG("reach play end, you should stop now");
                info->playToEnd = true;
            }
            return;
        }

        if (info->cacheTimeMs == 0)
        {
            info->playToEnd = true;
            info->cacheTimeMs = cacheTimeMs;
            ULOG("reach file end, cache %u ms", cacheTimeMs);
        }
        else
        {
            if (cacheTimeMs > info->cacheTimeMs)
            {
                ULOG("nealy play end, you can stop now, cache %u -> %u ms",
                    info->cacheTimeMs, cacheTimeMs);
                info->endTick = msecSinceStart();
            }
            else
            {
                info->cacheTimeMs = cacheTimeMs;
            }
        }
        #endif
    }
}

void playStopCallback(void* user)
{
    PlaybackInfo* info = (PlaybackInfo*)user;
    info->playToEnd = true;
}

void testCapture(const char* deviceId);
void testPlayback(const char* deviceId, const char* pcmPath);
void show_card_volume(const char* cardId);
void getHints();


int main(int argc, char* argv[])
{
	for (int i = 0; i < argc; ++i)
	{
		println("argv %d: %s", i, argv[i]);
	}

    printf("input devices:\n");
    enumerateAlsaCaptureDevices(nullptr, nullptr);
    printf("\noutput devices:\n");
    enumerateAlsaPlaybackDevices(nullptr, nullptr);

    getHints();

    if (argc < 2)
    {
        return 0;
    }

    const char* deviceId = nullptr;
    const char* pcmPath = nullptr;
    deviceId = argv[1];
    if (argc > 2)
    {
        pcmPath = argv[2];
    }

/*
    ULOG("----");
    show_card_volume(deviceId);
    for (int i = 0; i < 0; ++i)
    {
        char cardId[32];
        pformat(cardId, sizeof(cardId), "hw:%d", i);
        show_card_volume(cardId);
    }
*/

    //testCapture(deviceId);
    testPlayback(deviceId, pcmPath);

    ULOG("exit");
	//int c = getchar();
    return 0;
}

void testCapture(const char* deviceId)
{
    AlsaCaptureDevice alsa;
    alsa.open(deviceId);
    alsa.setParams(16000, 2, 16);
    char pcmName[128];
    pformat(pcmName, sizeof(pcmName), "cap_%d_%d.pcm", 16000, 2);
    alsa.captureToPcmFile(pcmName);
    alsa.start();
    int c;
    printf("\npress any key to stop\n\n");
    c = getchar();
    alsa.stop();

    printf("\npress any key to start\n\n");
    c = getchar();
    alsa.start();

    printf("\npress any key to stop\n\n");
    c = getchar();
    alsa.stop();
}

void testPlayback(const char* deviceId, const char* pcmPath)
{
    FILE* pcmFile = fopen(pcmPath, "rb");
    if (pcmFile)
    {
        printf("input sample rate, default 16000:");
        uint32_t sampleRate = 16000;
        std::string line;
        std::getline(std::cin, line);
        if (!line.empty())
            sampleRate = std::stoul(line);
        std::unique_ptr<FILE, decltype(&fclose)> autofile{pcmFile, fclose};
        PlaybackInfo playInfo;
        playInfo.file = pcmFile;
        playInfo.sampleRate = sampleRate;
        playInfo.channels = 2;
        AlsaPlaybackDevice alsa;
        alsa.open(deviceId);
        alsa.setMinCachePeriodCount(2);
        alsa.setPeriod(10, 10);
        alsa.setParams(playInfo.sampleRate, playInfo.channels, 16);
        alsa.setInputCallback(playDataCallback, &playInfo);
        alsa.start();
        int64_t playStartTime = msecSinceStart();
        int c;
/*
        printf("\npress any key to pause\n\n");
        c = getchar();
        return 1;
        alsa.pause();

        printf("\npress any key to resume\n\n");
        c = getchar();
        alsa.resume();*/


        printf("\npress any key to stop\n\n");
        //c = getchar();
        while (!playInfo.readToEnd) sleepMSec(1);
        ULOG("play time %lld ms, file time %llu ms",
            msecSinceStart() - playStartTime, playInfo.commitSamples*1000ULL/playInfo.sampleRate);
        alsa.asyncStop(playStopCallback, &playInfo);
        while (!playInfo.playToEnd) sleepMSec(1);
        alsa.stop();
        alsa.close();

        fseek(pcmFile, 0, SEEK_SET);
        playInfo.cacheTimeMs = 0;
        playInfo.commitSamples = 0;
        playInfo.playTime = 0;
        playInfo.lastPlayTime = 0;
        playInfo.endTick = 0;
        playInfo.readToEnd = false;
        playInfo.playToEnd = false;

        alsa.open(deviceId);
        alsa.setInputCallback(playDataCallback, &playInfo);
        alsa.setParams(playInfo.sampleRate, playInfo.channels, 16);
        printf("\npress any key to start\n\n");
        c = getchar();
        alsa.start();

        printf("\npress any key to stop\n\n");
        c = getchar();
        alsa.stop();
    }
    else
    {
        ULOG("can't open pcm file");
    }
}

int show_selem(const char* card, snd_mixer_t *handle, snd_mixer_selem_id_t *id, const char *space, int level);

void show_card_volume(const char* cardId)
{
    // deviceId is default, hw:0, hw:1, ...
    int err;
    snd_mixer_t *handle;
    snd_mixer_elem_t* elem;
    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);

    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, cardId);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    for (elem = snd_mixer_first_elem(handle); elem; elem = snd_mixer_elem_next(elem)) {
		snd_mixer_selem_get_id(elem, sid);
		if (!snd_mixer_selem_is_active(elem))
			continue;
        printf("card: %s\n", cardId);
		printf("Simple mixer control '%s',%i\n", snd_mixer_selem_id_get_name(sid), snd_mixer_selem_id_get_index(sid));
        int hasCommonVolume = snd_mixer_selem_has_common_volume(elem);
        int hasCommonSwitch = snd_mixer_selem_has_common_switch(elem);

        int hasCaptureMono = snd_mixer_selem_has_capture_channel(elem, SND_MIXER_SCHN_MONO);
        int isCaptureMono = snd_mixer_selem_is_capture_mono(elem);
        int hasCaptureVolume = snd_mixer_selem_has_capture_volume(elem);
        int hasCaptureSwitch = snd_mixer_selem_has_capture_switch(elem);

        int hasPlaybackMono = snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_MONO);
        int isPlaybackMono = snd_mixer_selem_is_playback_mono(elem);
        int hasPlaybackVolume = snd_mixer_selem_has_playback_volume(elem);
        int hasPlaybackSwitch = snd_mixer_selem_has_playback_switch(elem);

        bool cmono = hasCaptureMono && (isCaptureMono || (!hasCaptureVolume && !hasCaptureSwitch));
        bool pmono = hasPlaybackMono && (isPlaybackMono || (!hasPlaybackVolume && !hasPlaybackSwitch));

        printf("hasCommonVolume %d, hasCommonSwitch %d\n",
            hasCommonVolume, hasCommonSwitch);
        printf("hasCaptureMono %d, isCaptureMono %d, hasCaptureVolume %d, hasCaptureSwitch %d\n, cmono:%d\n",
            hasCaptureMono, isCaptureMono, hasCaptureVolume, hasCaptureSwitch, cmono);
        printf("hasPlaybackMono %d, isPlaybackMono %d, hasPlaybackVolume %d, hasPlaybackSwitch %d\n, pmono:%d\n",
            hasPlaybackMono, isPlaybackMono, hasPlaybackVolume, hasPlaybackSwitch, pmono);

        long cmin, cmax, cvol, cpercent;
        long pmin, pmax, pvol, ppercent;
        int isOn;
        if (hasCommonVolume || hasCaptureVolume)
        {
            err = snd_mixer_selem_get_capture_volume_range(elem, &cmin, &cmax);
            if (err == 0)
            {
                printf("Captuer volume range: [%ld, %ld]\n", cmin, cmax);
                for (int n=0; n <= 8; ++n)
                {
                    snd_mixer_selem_channel_id_t ch = (snd_mixer_selem_channel_id_t)n;
                    const char* chName = snd_mixer_selem_channel_name(ch);
                    if (snd_mixer_selem_has_capture_channel(elem, ch))
                    {
                        cvol = 0;
                        isOn = 0;
                        err = snd_mixer_selem_get_capture_volume(elem, ch, &cvol);
                        err = snd_mixer_selem_get_capture_switch(elem, ch, &isOn);
                        if (err == 0)
                        {
                            cpercent = (long)rint((cvol-cmin)*100.0/(cmax-cmin));
                            printf("  %s, volume %ld(%ld%%) %s\n",
                                chName, cvol, cpercent, isOn ? "on" : "off");
                        }
                    }
                }
            }
        }

        if (hasCommonVolume || hasPlaybackVolume)
        {
            err = snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);
            if (err == 0)
            {
                printf("Playback volume range: [%ld, %ld]\n", pmin, pmax);
                for (int n = 0; n <= 8; ++n)
                {
                    snd_mixer_selem_channel_id_t ch = (snd_mixer_selem_channel_id_t)n;
                    const char* chName = snd_mixer_selem_channel_name(ch);
                    if (snd_mixer_selem_has_playback_channel(elem, ch))
                    {
                        pvol = 0;
                        isOn = 0;
                        err = snd_mixer_selem_get_playback_volume(elem, ch, &pvol);
                        err = snd_mixer_selem_get_playback_switch(elem, ch, &isOn);
                        if (err == 0)
                        {
                            ppercent = (long)rint((pvol-pmin)*100.0/(pmax-pmin));
                            printf("  %s, volume %ld(%ld%%) %s\n",
                                chName, pvol, ppercent, isOn ? "on" : "off");
                        }
                    }
                }
            }
        }
		//show_selem(deviceId, handle, sid, "  ", 1);
	}

    snd_mixer_close(handle);
}

int show_selem(const char* card, snd_mixer_t *handle, snd_mixer_selem_id_t *id, const char *space, int level)
{
	//snd_mixer_selem_channel_id_t chn;
	long pmin = 0, pmax = 0;
	long cmin = 0, cmax = 0;
	int psw, csw;
	int pmono, cmono, mono_ok = 0;
	snd_mixer_elem_t *elem;
    int chn=0;
	
	elem = snd_mixer_find_selem(handle, id);
	if (!elem) {
		printf("Mixer %s simple element not found", card);fflush(stdout);
		return -ENOENT;
	}

    printf("%sCapabilities:", space);fflush(stdout);
    if (snd_mixer_selem_has_common_volume(elem)) {
        printf(" volume");fflush(stdout);
        if (snd_mixer_selem_has_playback_volume_joined(elem))
            printf(" volume-joined");fflush(stdout);
    } else {
        if (snd_mixer_selem_has_playback_volume(elem)) {
            printf(" pvolume");fflush(stdout);
            if (snd_mixer_selem_has_playback_volume_joined(elem))
                printf(" pvolume-joined");
            fflush(stdout);
        }
        if (snd_mixer_selem_has_capture_volume(elem)) {
            printf(" cvolume");fflush(stdout);
            if (snd_mixer_selem_has_capture_volume_joined(elem))
                printf(" cvolume-joined");
            fflush(stdout);
        }
    }
    fflush(stdout);
    if (snd_mixer_selem_has_common_switch(elem)) {
        printf(" switch");fflush(stdout);
        if (snd_mixer_selem_has_playback_switch_joined(elem))
            printf(" switch-joined");
        fflush(stdout);
    } else {
        if (snd_mixer_selem_has_playback_switch(elem)) {
            printf(" pswitch");fflush(stdout);
            if (snd_mixer_selem_has_playback_switch_joined(elem))
                printf(" pswitch-joined");
            fflush(stdout);
        }
        if (snd_mixer_selem_has_capture_switch(elem)) {
            printf(" cswitch");fflush(stdout);
            if (snd_mixer_selem_has_capture_switch_joined(elem))
                printf(" cswitch-joined");
            fflush(stdout);
            if (snd_mixer_selem_has_capture_switch_exclusive(elem))
                printf(" cswitch-exclusive");
            fflush(stdout);
        }
    }
    fflush(stdout);
    if (snd_mixer_selem_is_enum_playback(elem)) {
        printf(" penum");fflush(stdout);
    } else if (snd_mixer_selem_is_enum_capture(elem)) {
        printf(" cenum");fflush(stdout);
    } else if (snd_mixer_selem_is_enumerated(elem)) {
        printf(" enum");fflush(stdout);
    }
    printf("\n");fflush(stdout);
    if (snd_mixer_selem_is_enumerated(elem)) {
        int i, items;
        unsigned int idx;
        /*
            * See snd_ctl_elem_init_enum_names() in
            * sound/core/control.c.
            */
        char itemname[64];
        items = snd_mixer_selem_get_enum_items(elem);
        printf("  Items:");fflush(stdout);
        for (i = 0; i < items; i++) {
            snd_mixer_selem_get_enum_item_name(elem, i, sizeof(itemname) - 1, itemname);
            printf(" '%s'", itemname);fflush(stdout);
        }
        printf("\n");
        for (i = 0; !snd_mixer_selem_get_enum_item(elem, (snd_mixer_selem_channel_id_t)i, &idx); i++) {
            snd_mixer_selem_get_enum_item_name(elem, idx, sizeof(itemname) - 1, itemname);
            printf("  Item%d: '%s'\n", i, itemname);fflush(stdout);
        }
        return 0; /* no more thing to do */
    }
    fflush(stdout);
    if (snd_mixer_selem_has_capture_switch_exclusive(elem))
        printf("%sCapture exclusive group: %i\n", space,
                snd_mixer_selem_get_capture_group(elem));
    fflush(stdout);
    if (snd_mixer_selem_has_playback_volume(elem) ||
        snd_mixer_selem_has_playback_switch(elem)) {
        printf("%sPlayback channels:", space);fflush(stdout);
        if (snd_mixer_selem_is_playback_mono(elem)) {
            printf(" Mono");fflush(stdout);
        } else {
            int first = 1;
            for (chn = 0; chn <= SND_MIXER_SCHN_LAST; chn++){
                if (!snd_mixer_selem_has_playback_channel(elem, (snd_mixer_selem_channel_id_t)chn))
                    continue;
                if (!first)
                    printf(" -");
                fflush(stdout);
                printf(" %s", snd_mixer_selem_channel_name((snd_mixer_selem_channel_id_t)chn));fflush(stdout);
                first = 0;
            }
        }
        printf("\n");
    }
    fflush(stdout);
    if (snd_mixer_selem_has_capture_volume(elem) ||
        snd_mixer_selem_has_capture_switch(elem)) {
        printf("%sCapture channels:", space);fflush(stdout);
        if (snd_mixer_selem_is_capture_mono(elem)) {
            printf(" Mono");fflush(stdout);
        } else {
            int first = 1;
            for (chn = 0; chn <= SND_MIXER_SCHN_LAST; chn++){
                if (!snd_mixer_selem_has_capture_channel(elem, (snd_mixer_selem_channel_id_t)chn))
                    continue;
                if (!first)
                    printf(" -");
                fflush(stdout);
                printf(" %s", snd_mixer_selem_channel_name((snd_mixer_selem_channel_id_t)chn));fflush(stdout);
                first = 0;
            }
        }
        printf("\n");
    }
    fflush(stdout);
    if (snd_mixer_selem_has_playback_volume(elem) ||
        snd_mixer_selem_has_capture_volume(elem)) {
        printf("%sLimits:", space);fflush(stdout);
        if (snd_mixer_selem_has_common_volume(elem)) {
            snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);
            snd_mixer_selem_get_capture_volume_range(elem, &cmin, &cmax);
            printf(" %li - %li", pmin, pmax);fflush(stdout);
        } else {
            if (snd_mixer_selem_has_playback_volume(elem)) {
                snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);
                printf(" Playback %li - %li", pmin, pmax);fflush(stdout);
            }
            if (snd_mixer_selem_has_capture_volume(elem)) {
                snd_mixer_selem_get_capture_volume_range(elem, &cmin, &cmax);
                //snd_mixer_selem_get_capture_volume()
                printf(" Capture %li - %li", cmin, cmax);fflush(stdout);
            }
        }
        printf("\n");
    }
    fflush(stdout);
    pmono = snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_MONO) &&
            (snd_mixer_selem_is_playback_mono(elem) || 
            (!snd_mixer_selem_has_playback_volume(elem) &&
            !snd_mixer_selem_has_playback_switch(elem)));
    cmono = snd_mixer_selem_has_capture_channel(elem, SND_MIXER_SCHN_MONO) &&
            (snd_mixer_selem_is_capture_mono(elem) || 
            (!snd_mixer_selem_has_capture_volume(elem) &&
            !snd_mixer_selem_has_capture_switch(elem)));
#if 0
    printf("pmono = %i, cmono = %i (%i, %i, %i, %i)\n", pmono, cmono,
            snd_mixer_selem_has_capture_channel(elem, SND_MIXER_SCHN_MONO),
            snd_mixer_selem_is_capture_mono(elem),
            snd_mixer_selem_has_capture_volume(elem),
            snd_mixer_selem_has_capture_switch(elem));
#endif
    if (pmono || cmono) {
        if (!mono_ok) {
            printf("%s%s:", space, "Mono");fflush(stdout);
            mono_ok = 1;
        }
        if (snd_mixer_selem_has_common_volume(elem)) {
            printf("  show_selem_volume, L%d\n", __LINE__);
            //show_selem_volume(elem, SND_MIXER_SCHN_MONO, 0, pmin, pmax);
        }
        if (snd_mixer_selem_has_common_switch(elem)) {
            snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_MONO, &psw);
            printf(" [%s]", psw ? "on" : "off");fflush(stdout);
        }
    }
    fflush(stdout);
    if (pmono && snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_MONO)) {
        int title = 0;
        if (!mono_ok) {
            printf("%s%s:", space, "Mono");fflush(stdout);
            mono_ok = 1;
        }
        if (!snd_mixer_selem_has_common_volume(elem)) {
            if (snd_mixer_selem_has_playback_volume(elem)) {
                printf(" Playback");fflush(stdout);
                title = 1;
                //show_selem_volume(elem, SND_MIXER_SCHN_MONO, 0, pmin, pmax);
            }
        }
        if (!snd_mixer_selem_has_common_switch(elem)) {
            if (snd_mixer_selem_has_playback_switch(elem)) {
                if (!title)
                    printf(" Playback");
                fflush(stdout);
                snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_MONO, &psw);
                printf(" [%s]", psw ? "on" : "off");fflush(stdout);
            }
        }
    }
    fflush(stdout);
    if (cmono && snd_mixer_selem_has_capture_channel(elem, SND_MIXER_SCHN_MONO)) {
        int title = 0;
        if (!mono_ok) {
            printf("%s%s:", space, "Mono");fflush(stdout);
            mono_ok = 1;
        }
        if (!snd_mixer_selem_has_common_volume(elem)) {
            if (snd_mixer_selem_has_capture_volume(elem)) {
                printf(" Capture");fflush(stdout);
                title = 1;
                //show_selem_volume(elem, SND_MIXER_SCHN_MONO, 1, cmin, cmax);
            }
        }
        if (!snd_mixer_selem_has_common_switch(elem)) {
            if (snd_mixer_selem_has_capture_switch(elem)) {
                if (!title)
                    printf(" Capture");
                fflush(stdout);
                snd_mixer_selem_get_capture_switch(elem, SND_MIXER_SCHN_MONO, &csw);
                printf(" [%s]", csw ? "on" : "off");fflush(stdout);
            }
        }
    }
    fflush(stdout);
    if (pmono || cmono)
        printf("\n");
    if (!pmono || !cmono) {
        for (chn = 0; chn <= SND_MIXER_SCHN_LAST; chn++) {
            if ((pmono || !snd_mixer_selem_has_playback_channel(elem, (snd_mixer_selem_channel_id_t)chn)) &&
                (cmono || !snd_mixer_selem_has_capture_channel(elem, (snd_mixer_selem_channel_id_t)chn)))
                continue;
            printf("%s%s:", space, snd_mixer_selem_channel_name((snd_mixer_selem_channel_id_t)chn));fflush(stdout);
            if (!pmono && !cmono && snd_mixer_selem_has_common_volume(elem)) {
                printf("  show_selem_volume, L%d\n", __LINE__);fflush(stdout);
                //show_selem_volume(elem, (snd_mixer_selem_channel_id_t)chn, 0, pmin, pmax);
            }
            if (!pmono && !cmono && snd_mixer_selem_has_common_switch(elem)) {
                snd_mixer_selem_get_playback_switch(elem, (snd_mixer_selem_channel_id_t)chn, &psw);
                printf(" [%s]", psw ? "on" : "off");fflush(stdout);
            }
            if (!pmono && snd_mixer_selem_has_playback_channel(elem, (snd_mixer_selem_channel_id_t)chn)) {
                int title = 0;
                if (!snd_mixer_selem_has_common_volume(elem)) {
                    if (snd_mixer_selem_has_playback_volume(elem)) {
                        printf(" Playback");fflush(stdout);
                        title = 1;
                        long pvol;
                        snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);
			            snd_mixer_selem_get_playback_volume(elem, (snd_mixer_selem_channel_id_t)chn, &pvol);
                        long vol_percent = (pvol-pmin)*100/(pmax-pmin);
                        printf(" vol %ld %ld%% ", pvol, vol_percent);fflush(stdout);
			            //set_playback_raw_volume
                        //show_selem_volume(elem, (snd_mixer_selem_channel_id_t)chn, 0, pmin, pmax);
                    }
                }
                if (!snd_mixer_selem_has_common_switch(elem)) {
                    if (snd_mixer_selem_has_playback_switch(elem)) {
                        if (!title)
                            printf(" Playback");
                        fflush(stdout);
                        snd_mixer_selem_get_playback_switch(elem, (snd_mixer_selem_channel_id_t)chn, &psw);
                        printf(" [%s]", psw ? "on" : "off");fflush(stdout);
                    }
                }
            }
            fflush(stdout);
            if (!cmono && snd_mixer_selem_has_capture_channel(elem, (snd_mixer_selem_channel_id_t)chn)) {
                int title = 0;
                if (!snd_mixer_selem_has_common_volume(elem)) {
                    if (snd_mixer_selem_has_capture_volume(elem)) {
                        printf(" Capture");fflush(stdout);
                        title = 1;
                        long cvol;
                        snd_mixer_selem_get_capture_volume_range(elem, &cmin, &cmax);
                        snd_mixer_selem_get_capture_volume(elem, (snd_mixer_selem_channel_id_t)chn, &cvol);
                        long vol_percent = (cvol-cmin)*100/(cmax-cmin);
                        printf(" vol %ld %ld%% ", cvol,vol_percent);fflush(stdout);
                        if (vol_percent > 2)
                        snd_mixer_selem_set_capture_volume(elem, (snd_mixer_selem_channel_id_t)chn, cvol/2);
                        //snd_mixer_selem_set_capture_volume(elem, (snd_mixer_selem_channel_id_t)chn, cvol-long((cmax-cmin)*.02));
                        //show_selem_volume(elem, (snd_mixer_selem_channel_id_t)chn, 1, cmin, cmax);
                    }
                }
                if (!snd_mixer_selem_has_common_switch(elem)) {
                    if (snd_mixer_selem_has_capture_switch(elem)) {
                        if (!title)
                            printf(" Capture");
                        fflush(stdout);
                        snd_mixer_selem_get_capture_switch(elem, (snd_mixer_selem_channel_id_t)chn, &csw);
                        printf(" [%s]", csw ? "on" : "off");fflush(stdout);
                    }
                }
            }
            printf("\n");
        }
    }
    
	return 0;
}

void getHints()
{
    int err;
    char **hints;
    char *name;

    // Get a list of available devices
    err = snd_device_name_hint(-1, "pcm", (void***)&hints);
    if (err < 0) {
        printf("Error: %s\n", snd_strerror(err));
        return ;
    }

    // Print the name of each device
    for (int i = 0; hints[i] != NULL; i++) {
        if ((name = snd_device_name_get_hint(hints[i], "NAME")) != NULL) {
            printf("hint %d: %s\n", i, name);
            free(name);
        }
    }

    // Free the list of devices
    snd_device_name_free_hint((void**)hints);
}