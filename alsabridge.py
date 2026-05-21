#!python3
# -*- coding:utf-8 -*-
"""
ALSABridge — Python bridge layer; loads libalsadevice.so via ctypes.
"""
import os
import sys
import wave
import ctypes
import threading
from typing import List, Tuple

from log_util import Fore, log


class _AlsaDll:
    _instance = None

    @classmethod
    def instance(cls) -> '_AlsaDll':
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    def __init__(self):
        lib_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'libalsadevice.so')
        self.dll = ctypes.cdll.LoadLibrary(lib_path)
        log(f'load library {self.dll!r}')
        self.dll.AlsaCaptureDevice_create.restype = ctypes.c_size_t
        self.dll.AlsaPlaybackDevice_create.restype = ctypes.c_size_t

    def __del__(self):
        pass


class AudioDevice:
    def __init__(self, card_id: str = '', device_name: str = '', device_id: str = ''):
        self.card_id = card_id
        self.device_name = device_name
        self.device_id = device_id

    def __str__(self):
        return (
            f'{self.__class__.__name__}(card_id="{self.card_id}", '
            f'device_name="{self.device_name}", device_id="{self.device_id}")'
        )

    __repr__ = __str__


_alsa_enum_callback = ctypes.CFUNCTYPE(
    None, ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t)
_alsa_input_data_callback = ctypes.CFUNCTYPE(
    None, ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t)
_alsa_output_data_callback = ctypes.CFUNCTYPE(
    None, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_size_t)
_alsa_playback_stopped_callback = ctypes.CFUNCTYPE(None, ctypes.c_size_t)


def get_capture_devices() -> List[AudioDevice]:
    devices = []
    devices.append(AudioDevice('default', 'default', 'default'))

    def capture_device_callback(index: int, card_id: bytes, device_name: bytes, device_id: bytes, user: int):
        device = AudioDevice(card_id.decode(), device_name.decode(), device_id.decode())
        devices.append(device)

    _AlsaDll.instance().dll.enumerateAlsaCaptureDevices(_alsa_enum_callback(capture_device_callback), 0)
    return devices


def get_playback_devices() -> List[AudioDevice]:
    devices = []
    devices.append(AudioDevice('default', 'default', 'default'))

    def playback_device_callback(index: int, card_id: bytes, device_name: bytes, device_id: bytes, user: int):
        device = AudioDevice(card_id.decode(), device_name.decode(), device_id.decode())
        devices.append(device)

    _AlsaDll.instance().dll.enumerateAlsaPlaybackDevices(_alsa_enum_callback(playback_device_callback), 0)
    return devices


class CaptureCallback:
    def on_capture_output_data(self, cache_time_ms: int, samples: bytes, samples_count: int) -> None:
        '''runs in another thread'''
        pass


class AlsaCaptureDevice:
    def __init__(self):
        self._ptr = ctypes.c_size_t(_AlsaDll.instance().dll.AlsaCaptureDevice_create())
        self._c_output_data_callback = _alsa_output_data_callback(self._output_data_callback)
        self.capture_callback = None
        self.sample_rate = 0
        self.channels = 0
        self.bits_per_sample = 0

    def __del__(self):
        _AlsaDll.instance().dll.AlsaDevice_release(self._ptr)
        self._ptr = None

    def open(self, device_id: str) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_open(self._ptr, device_id.encode())
        if not ret:
            return False
        _AlsaDll.instance().dll.AlsaCaptureDevice_setOutputCallback(self._ptr, self._c_output_data_callback, 0)
        return True

    def set_period(self, period_count: int, period_time: int) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_setPeriod(
            self._ptr, ctypes.c_uint32(period_count), ctypes.c_uint32(period_time))
        return bool(ret)

    def set_params(self, sample_rate: int, channels: int, bits_per_sample: int) -> bool:
        self.sample_rate = sample_rate
        self.channels = channels
        self.bits_per_sample = bits_per_sample
        ret = _AlsaDll.instance().dll.AlsaDevice_setParams(
            self._ptr,
            ctypes.c_uint32(sample_rate),
            ctypes.c_uint32(channels),
            ctypes.c_uint32(bits_per_sample),
        )
        return bool(ret)

    def start(self) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_start(self._ptr)
        return bool(ret)

    def stop(self) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_stop(self._ptr)
        return bool(ret)

    def close(self) -> bool:
        self.sample_rate = 0
        self.channels = 0
        self.bits_per_sample = 0
        ret = _AlsaDll.instance().dll.AlsaDevice_close(self._ptr)
        return bool(ret)

    def get_volume(self) -> int:
        ret = _AlsaDll.instance().dll.AlsaDevice_getVolume(self._ptr, b'default')
        return ret

    def set_volume(self, volume: int) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_setVolume(self._ptr, b'default', volume)
        return bool(ret)

    def set_capture_callback(self, callback: CaptureCallback) -> None:
        self.capture_callback = callback

    def capture_to_pcm_file(self, file_name: str, append: bool = False) -> bool:
        ret = _AlsaDll.instance().dll.AlsaCaptureDevice_captureToPcmFile(
            self._ptr, file_name.encode(), int(append))
        return bool(ret)

    def _output_data_callback(self, cache_time_ms: int, samples_ptr: ctypes.c_void_p, samples_count: int, user: int) -> None:
        if self.capture_callback:
            sample_type = ctypes.c_uint8 * (self.channels * self.bits_per_sample // 8 * samples_count)
            samples = sample_type.from_address(samples_ptr)
            self.capture_callback.on_capture_output_data(cache_time_ms, samples, samples_count)


class PlaybackCallback:
    def on_playback_input_data(self, cache_time_ms: int, samples_count: int) -> bytes:
        '''runs in another thread'''
        pass

    def on_playback_stopped(self) -> None:
        '''
        runs in another thread
        If you want to start playback again after async_stop,
        you must notify main thread and call stop() there.
        Don't call stop() in this function.
        '''
        pass


class AlsaPlaybackDevice:
    def __init__(self):
        self._ptr = ctypes.c_size_t(_AlsaDll.instance().dll.AlsaPlaybackDevice_create())
        self._c_input_data_callback = _alsa_input_data_callback(self._input_data_callback)
        self._c_playback_stopped_callback = _alsa_playback_stopped_callback(self._playback_stopped_callback)
        self.libc = ctypes.cdll.LoadLibrary('libc.so.6')
        self.playback_callback = None
        self.sample_rate = 0
        self.channels = 0
        self.bits_per_sample = 0

    def __del__(self):
        _AlsaDll.instance().dll.AlsaDevice_release(self._ptr)
        self._ptr = None

    def open(self, device_id: str) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_open(self._ptr, device_id.encode())
        if not ret:
            return False
        _AlsaDll.instance().dll.AlsaPlaybackDevice_setInputCallback(self._ptr, self._c_input_data_callback, 0)
        return True

    def set_min_cache_period_count(self, min_cache_period_count: int = 2) -> bool:
        ret = _AlsaDll.instance().dll.AlsaPlaybackDevice_setMinCachePeriodCount(
            self._ptr, ctypes.c_uint32(min_cache_period_count))
        return bool(ret)

    def set_period(self, period_count: int, period_time: int) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_setPeriod(
            self._ptr, ctypes.c_uint32(period_count), ctypes.c_uint32(period_time))
        return bool(ret)

    def set_params(self, sample_rate: int, channels: int, bits_per_sample: int) -> bool:
        self.sample_rate = sample_rate
        self.channels = channels
        self.bits_per_sample = bits_per_sample
        ret = _AlsaDll.instance().dll.AlsaDevice_setParams(
            self._ptr,
            ctypes.c_uint32(sample_rate),
            ctypes.c_uint32(channels),
            ctypes.c_uint32(bits_per_sample),
        )
        return bool(ret)

    def start(self) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_start(self._ptr)
        return bool(ret)

    def stop(self) -> bool:
        '''stop immediately, cache samples left in alsa internal buffer are not played'''
        ret = _AlsaDll.instance().dll.AlsaDevice_stop(self._ptr)
        return bool(ret)

    def sync_stop(self) -> bool:
        '''stop and wait until cache samples are played'''
        ret = _AlsaDll.instance().dll.AlsaPlaybackDevice_syncStop(self._ptr)
        return bool(ret)

    def async_stop(self) -> bool:
        '''
        notify to stop and return immediately.
        PlaybackCallback.on_playback_stopped will be called when cache samples are played
        '''
        ret = _AlsaDll.instance().dll.AlsaPlaybackDevice_asyncStop(
            self._ptr, self._c_playback_stopped_callback, 0)
        return bool(ret)

    def pause(self) -> bool:
        ret = _AlsaDll.instance().dll.AlsaPlaybackDevice_pause(self._ptr)
        return bool(ret)

    def resume(self) -> bool:
        ret = _AlsaDll.instance().dll.AlsaPlaybackDevice_resume(self._ptr)
        return bool(ret)

    def close(self) -> bool:
        self.sample_rate = 0
        self.channels = 0
        self.bits_per_sample = 0
        ret = _AlsaDll.instance().dll.AlsaDevice_close(self._ptr)
        return bool(ret)

    def get_volume(self, card_id: str = 'default') -> int:
        ret = _AlsaDll.instance().dll.AlsaDevice_getVolume(self._ptr, card_id.encode())
        return ret

    def set_volume(self, volume: int, card_id: str = 'default') -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_setVolume(self._ptr, card_id.encode(), volume)
        return bool(ret)

    def set_playback_callback(self, callback: PlaybackCallback) -> None:
        self.playback_callback = callback

    def _input_data_callback(
        self, cache_time_ms: int, samples_ptr: int, samples_count: ctypes.POINTER(ctypes.c_uint32), user: int
    ) -> None:
        if self.playback_callback:
            input_bytes = self.playback_callback.on_playback_input_data(
                cache_time_ms, samples_count.contents.value)
            if input_bytes:
                self.libc.memcpy(ctypes.c_void_p(samples_ptr), input_bytes, len(input_bytes))
            samples_count.contents.value = len(input_bytes) // (self.channels * self.bits_per_sample // 8)
        else:
            samples_count.contents.value = 0

    def _playback_stopped_callback(self, user: int) -> None:
        if self.playback_callback:
            self.playback_callback.on_playback_stopped()


def _sampwidth_bytes(bits_per_sample: int) -> int:
    if bits_per_sample == 8:
        return 1
    if bits_per_sample == 16:
        return 2
    raise ValueError(f'wave module supports 8/16-bit PCM only, got {bits_per_sample} bit')


def read_wav_params(path: str) -> Tuple[int, int, int]:
    '''Read (sample_rate, channels, bits_per_sample) from a WAV file.'''
    with wave.open(path, 'rb') as wf:
        return wf.getframerate(), wf.getnchannels(), wf.getsampwidth() * 8


class WavWriter:
    '''Write captured audio to a WAV file (stdlib wave).'''

    def __init__(self, path: str, sample_rate: int, channels: int, bits_per_sample: int):
        self._wf = wave.open(path, 'wb')
        self._wf.setnchannels(channels)
        self._wf.setsampwidth(_sampwidth_bytes(bits_per_sample))
        self._wf.setframerate(sample_rate)

    def write_frames(self, pcm: bytes) -> None:
        self._wf.writeframes(pcm)

    def close(self) -> None:
        if self._wf:
            self._wf.close()
            self._wf = None

    def __del__(self):
        self.close()


class WavReader:
    '''Read PCM frames from a WAV file for playback (stdlib wave).'''

    def __init__(self, path: str):
        self._wf = wave.open(path, 'rb')
        self.sample_rate = self._wf.getframerate()
        self.channels = self._wf.getnchannels()
        self.bits_per_sample = self._wf.getsampwidth() * 8

    def read_frames(self, frame_count: int) -> bytes:
        return self._wf.readframes(frame_count)

    def close(self) -> None:
        if self._wf:
            self._wf.close()
            self._wf = None

    def __del__(self):
        self.close()


if __name__ == '__main__':
    print(f'{Fore.Cyan}sys.executable{Fore.Reset}= {sys.executable}')
    print(f'{Fore.Cyan}sys.version{Fore.Reset}= {sys.version}')
    import argparse
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument('-i', '--input', action='store_true', help='list input/recorder devices')
    parser.add_argument('-o', '--output', action='store_true', help='list output/playback devices')
    parser.add_argument('-d', '--device-id', default='default', help='ALSA device id')
    parser.add_argument('-c', '--capture', action='store_true', help='capture audio to WAV')
    parser.add_argument('-p', '--play', action='store_true', help='play audio from WAV')
    parser.add_argument('-f', '--file', default='cap.wav', help='path to WAV file')
    parser.add_argument(
        '-s', '--sample-rate', type=int, default=None,
        help='sample rate in Hz, capture only (default when capturing: 16000)',
    )
    parser.add_argument(
        '-n', '--channels', type=int, default=None,
        help='channel count, capture only (default when capturing: 2)',
    )
    parser.add_argument(
        '-b', '--bits-per-sample', type=int, default=None,
        help='bits per sample, capture only (default when capturing: 16)',
    )
    parser.add_argument(
        '-v', '--volume', type=int, default=None,
        help='volume 0-100; omit to keep system volume unchanged',
    )

    args = parser.parse_args()

    list_input = args.input
    list_output = args.output
    device_id = args.device_id
    capture_audio = args.capture
    play_audio = args.play
    file_path = args.file
    volume = args.volume

    def _warn_ignored_capture_format_flags():
        ignored = []
        if args.sample_rate is not None:
            ignored.append(f'--sample-rate={args.sample_rate}')
        if args.channels is not None:
            ignored.append(f'--channels={args.channels}')
        if args.bits_per_sample is not None:
            ignored.append(f'--bits-per-sample={args.bits_per_sample}')
        if ignored:
            log(f'warning: {" ".join(ignored)} ignored by --play (format is read from WAV)')

    if list_input:
        for dev in get_capture_devices():
            print(dev)
    if list_output:
        for dev in get_playback_devices():
            print(dev)
    if capture_audio:
        sample_rate = args.sample_rate if args.sample_rate is not None else 16000
        channels = args.channels if args.channels is not None else 2
        bits_per_sample = args.bits_per_sample if args.bits_per_sample is not None else 16

        class CaptureCallbackImpl(CaptureCallback):
            def __init__(self, wav_path: str, sample_rate: int, channels: int, bits_per_sample: int):
                self.sample_rate = sample_rate
                self._total_samples = 0
                self._writer = WavWriter(wav_path, sample_rate, channels, bits_per_sample)

            def on_capture_output_data(self, cache_time_ms: int, samples, samples_count: int) -> None:
                self._total_samples += samples_count
                self._writer.write_frames(bytes(samples))

            @property
            def duration_seconds(self) -> float:
                if self.sample_rate <= 0:
                    return 0.0
                return self._total_samples / self.sample_rate

            def close(self):
                self._writer.close()

            def __del__(self):
                self.close()

        capture_callback = CaptureCallbackImpl(file_path, sample_rate, channels, bits_per_sample)
        dev = AlsaCaptureDevice()
        started = False
        try:
            dev.set_capture_callback(capture_callback)
            if not dev.open(device_id):
                parser.error(
                    f"failed to open capture device '{device_id}'. "
                    "List devices with -i. On WSL, set up PulseAudio/PipeWire or pick a valid device id."
                )
            if volume is not None:
                if not 0 <= volume <= 100:
                    parser.error('volume must be between 0 and 100')
                print('set_volume', volume)
                dev.set_volume(volume)
            dev.set_period(period_count=10, period_time=20)
            if not dev.set_params(sample_rate, channels, bits_per_sample):
                parser.error('set_params failed')
            input(f'will capture to WAV {Fore.Cyan}{file_path}{Fore.Reset}, press {Fore.Cyan}Enter{Fore.Reset} to start:')
            if not dev.start():
                parser.error('start capture failed')
            started = True
            input(f'press {Fore.Cyan}Enter{Fore.Reset} again to stop capture:\n')
        finally:
            if started:
                dev.stop()
            dev.close()
            capture_callback.close()
            if started and capture_callback._total_samples > 0:
                print(f'total time: {Fore.Cyan}{capture_callback.duration_seconds:.3f}{Fore.Reset}')
    elif play_audio:
        _warn_ignored_capture_format_flags()
        wav_reader = WavReader(file_path)
        sample_rate = wav_reader.sample_rate
        channels = wav_reader.channels
        bits_per_sample = wav_reader.bits_per_sample
        print(
            f'WAV format: {sample_rate} Hz, {channels} ch, {bits_per_sample} bit '
            f'({Fore.Cyan}{file_path}{Fore.Reset})'
        )

        class WavPlaybackImpl(PlaybackCallback):
            def __init__(self, reader: WavReader):
                self._reader = reader
                self.sample_rate = reader.sample_rate
                self.channels = reader.channels
                self.bits_per_sample = reader.bits_per_sample
                self.should_stop_event = threading.Event()
                self.stopped_event = threading.Event()
                self.commit_samples = 0
                self.play_time = 0

            def close(self):
                self._reader.close()

            def __del__(self):
                self.close()

            def on_playback_input_data(self, cache_time_ms: int, samples_count: int) -> bytes:
                ret = self._reader.read_frames(samples_count)
                frame_bytes = self.channels * self.bits_per_sample // 8
                self.commit_samples += len(ret) // frame_bytes
                play_time = self.commit_samples // self.sample_rate
                if play_time != self.play_time:
                    print(f'play time: {Fore.Cyan}{play_time}{Fore.Reset}')
                    self.play_time = play_time
                if not ret:
                    self.should_stop_event.set()
                return ret

            def on_playback_stopped(self) -> None:
                if self.sample_rate > 0:
                    total_time = self.commit_samples / self.sample_rate
                    print(f'total time: {Fore.Cyan}{total_time:.3f}{Fore.Reset}')
                self.stopped_event.set()

        playback_callback = WavPlaybackImpl(wav_reader)
        dev = AlsaPlaybackDevice()
        started = False
        try:
            dev.set_playback_callback(playback_callback)
            if not dev.open(device_id):
                parser.error(
                    f"failed to open playback device '{device_id}'. "
                    "List devices with -o. On WSL, set up PulseAudio/PipeWire or pick a valid device id."
                )
            if volume is not None:
                if not 0 <= volume <= 100:
                    parser.error('volume must be between 0 and 100')
                print('set_volume', volume)
                dev.set_volume(volume)
            dev.set_min_cache_period_count(2)
            dev.set_period(period_count=10, period_time=20)
            if not dev.set_params(sample_rate, channels, bits_per_sample):
                parser.error('set_params failed')
            input(f'will play WAV {Fore.Cyan}{file_path}{Fore.Reset}, press {Fore.Cyan}Enter{Fore.Reset} to start:\n')
            if not dev.start():
                parser.error('start playback failed')
            started = True

            playback_callback.should_stop_event.wait()
            dev.async_stop()
            playback_callback.stopped_event.wait()
        finally:
            if started:
                dev.stop()
            dev.close()
            playback_callback.close()
