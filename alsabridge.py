#!python3
# -*- coding:utf-8 -*-
"""
ALSABridge — Python bridge layer; loads libalsadevice.so via ctypes.
"""
import os
import sys
import wave
import ctypes
import ctypes.util
import threading
import re
from typing import List, Optional, Tuple

from log_util import Fore, log

_alsa_enum_callback = ctypes.CFUNCTYPE(
    None, ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p)
_alsa_input_data_callback = ctypes.CFUNCTYPE(
    None, ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p)
_alsa_output_data_callback = ctypes.CFUNCTYPE(
    None, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p)
_alsa_playback_stopped_callback = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


def _load_libc():
    libname = ctypes.util.find_library('c')
    if libname:
        return ctypes.CDLL(libname)
    for candidate in ('libc.so.6', 'libc.so', 'libSystem.dylib'):
        try:
            return ctypes.CDLL(candidate)
        except OSError:
            continue
    raise OSError('cannot load C library for memcpy')


_LIBC = None


def _libc():
    global _LIBC
    if _LIBC is None:
        _LIBC = _load_libc()
        _LIBC.memcpy.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
        _LIBC.memcpy.restype = ctypes.c_void_p
        _LIBC.memset.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_size_t]
        _LIBC.memset.restype = ctypes.c_void_p
    return _LIBC


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
        self._bind_exports()

    def _bind_exports(self) -> None:
        d = self.dll
        c_char_p = ctypes.c_char_p
        c_int = ctypes.c_int
        c_uint32 = ctypes.c_uint32
        c_size_t = ctypes.c_size_t
        c_void_p = ctypes.c_void_p

        d.AlsaCaptureDevice_create.restype = c_size_t
        d.AlsaPlaybackDevice_create.restype = c_size_t

        d.AlsaDevice_release.argtypes = [c_size_t]
        d.AlsaDevice_release.restype = None

        d.AlsaDevice_open.argtypes = [c_size_t, c_char_p]
        d.AlsaDevice_open.restype = c_int
        d.AlsaDevice_setPeriod.argtypes = [c_size_t, c_uint32, c_uint32]
        d.AlsaDevice_setPeriod.restype = c_int
        d.AlsaDevice_setParams.argtypes = [c_size_t, c_uint32, c_uint32, c_uint32]
        d.AlsaDevice_setParams.restype = c_int
        d.AlsaDevice_start.argtypes = [c_size_t]
        d.AlsaDevice_start.restype = c_int
        d.AlsaDevice_stop.argtypes = [c_size_t]
        d.AlsaDevice_stop.restype = c_int
        d.AlsaDevice_close.argtypes = [c_size_t]
        d.AlsaDevice_close.restype = c_int
        d.AlsaDevice_getVolume.argtypes = [c_size_t, c_char_p]
        d.AlsaDevice_getVolume.restype = c_int
        d.AlsaDevice_setVolume.argtypes = [c_size_t, c_char_p, c_int]
        d.AlsaDevice_setVolume.restype = c_int

        d.enumerateAlsaCaptureDevices.argtypes = [_alsa_enum_callback, c_void_p]
        d.enumerateAlsaCaptureDevices.restype = c_int
        d.enumerateAlsaPlaybackDevices.argtypes = [_alsa_enum_callback, c_void_p]
        d.enumerateAlsaPlaybackDevices.restype = c_int

        d.AlsaCaptureDevice_setOutputCallback.argtypes = [
            c_size_t, _alsa_output_data_callback, c_void_p]
        d.AlsaCaptureDevice_setOutputCallback.restype = c_int
        d.AlsaCaptureDevice_captureToPcmFile.argtypes = [c_size_t, c_char_p, c_int]
        d.AlsaCaptureDevice_captureToPcmFile.restype = c_int

        d.AlsaPlaybackDevice_setMinCachePeriodCount.argtypes = [c_size_t, c_uint32]
        d.AlsaPlaybackDevice_setMinCachePeriodCount.restype = c_int
        d.AlsaPlaybackDevice_setPrefillMs.argtypes = [c_size_t, c_uint32]
        d.AlsaPlaybackDevice_setPrefillMs.restype = c_int
        d.AlsaPlaybackDevice_setInputCallback.argtypes = [
            c_size_t, _alsa_input_data_callback, c_void_p]
        d.AlsaPlaybackDevice_setInputCallback.restype = c_int
        d.AlsaPlaybackDevice_syncStop.argtypes = [c_size_t]
        d.AlsaPlaybackDevice_syncStop.restype = c_int
        d.AlsaPlaybackDevice_asyncStop.argtypes = [
            c_size_t, _alsa_playback_stopped_callback, c_void_p]
        d.AlsaPlaybackDevice_asyncStop.restype = c_int
        d.AlsaPlaybackDevice_pause.argtypes = [c_size_t]
        d.AlsaPlaybackDevice_pause.restype = c_int
        d.AlsaPlaybackDevice_resume.argtypes = [c_size_t]
        d.AlsaPlaybackDevice_resume.restype = c_int

        if hasattr(d, 'queryAlsaDeviceHwParams'):
            d.queryAlsaDeviceHwParams.argtypes = [c_char_p, c_int, c_char_p, c_int]
            d.queryAlsaDeviceHwParams.restype = c_int

    def __del__(self):
        pass


class AudioDevice:
    def __init__(
        self,
        card_id: str = '',
        device_name: str = '',
        device_id: str = '',
        hw_params: Optional[str] = None,
    ):
        # card_id from enumeration is typically "hw:N" (card index form), not the ALSA card name id.
        self.card_id = card_id
        self.device_name = device_name
        self.device_id = device_id
        self.hw_params = hw_params

    def __str__(self):
        return (
            f'{self.__class__.__name__}(card_id="{self.card_id}", '
            f'device_name="{self.device_name}", device_id="{self.device_id}")'
        )

    __repr__ = __str__


_HW_PARAMS_BUF_SIZE = 2048
_CARD_FROM_DEVICE_RE = re.compile(r'^hw:(\d+)(?:,|$)')
_CARD_FROM_VERBOSE_RE = re.compile(r'^card\s*:\s*(hw:\d+)\b', re.MULTILINE)


def query_device_hw_params(device_id: str, is_capture: bool) -> str:
    '''Return compact HW params text, or "unavailable: ..." on failure.'''
    dll = _AlsaDll.instance().dll
    if not hasattr(dll, 'queryAlsaDeviceHwParams'):
        return 'unavailable: queryAlsaDeviceHwParams not exported (rebuild libalsadevice.so)'
    buf = ctypes.create_string_buffer(_HW_PARAMS_BUF_SIZE)
    ok = dll.queryAlsaDeviceHwParams(
        device_id.encode(), 1 if is_capture else 0, buf, _HW_PARAMS_BUF_SIZE)
    text = buf.value.decode(errors='replace').strip()
    if not text:
        return 'unavailable: empty result'
    if not ok and not text.startswith('unavailable:'):
        return f'unavailable: {text}'
    return text


def mixer_card_from_device_id(device_id: str) -> str:
    '''Best-effort mixer card name for a PCM device id.'''
    if not device_id:
        return 'default'
    if device_id in ('default', 'pulse'):
        return 'default'
    m = _CARD_FROM_DEVICE_RE.match(device_id)
    if m:
        return f'hw:{m.group(1)}'
    # hw:CardName,0 / plughw:CardName,0 -> use card name form for mixer attach
    for prefix in ('plughw:', 'hw:'):
        if device_id.startswith(prefix):
            rest = device_id[len(prefix):]
            card = rest.split(',', 1)[0]
            if card:
                # Prefer numeric hw:N when possible via verbose resolve later.
                if card.isdigit():
                    return f'hw:{card}'
                return f'hw:{card}'
    return 'default'


def mixer_card_from_hw_params(hw_params_text: str, fallback_device_id: str = '') -> str:
    '''Parse "card: hw:N (...)" from verbose query text.'''
    if hw_params_text:
        m = _CARD_FROM_VERBOSE_RE.search(hw_params_text)
        if m:
            return m.group(1)
    return mixer_card_from_device_id(fallback_device_id)


def get_capture_devices() -> List[AudioDevice]:
    devices = []
    devices.append(AudioDevice('default', 'default', 'default'))

    def capture_device_callback(index: int, card_id: bytes, device_name: bytes, device_id: bytes, user):
        device = AudioDevice(card_id.decode(), device_name.decode(), device_id.decode())
        devices.append(device)

    _AlsaDll.instance().dll.enumerateAlsaCaptureDevices(_alsa_enum_callback(capture_device_callback), None)
    return devices


def get_playback_devices() -> List[AudioDevice]:
    devices = []
    devices.append(AudioDevice('default', 'default', 'default'))

    def playback_device_callback(index: int, card_id: bytes, device_name: bytes, device_id: bytes, user):
        device = AudioDevice(card_id.decode(), device_name.decode(), device_id.decode())
        devices.append(device)

    _AlsaDll.instance().dll.enumerateAlsaPlaybackDevices(_alsa_enum_callback(playback_device_callback), None)
    return devices


def _print_devices(devices: List[AudioDevice], is_capture: bool, verbose: bool) -> None:
    for dev in devices:
        print(dev)
        if not verbose:
            continue
        hw = query_device_hw_params(dev.device_id, is_capture=is_capture)
        dev.hw_params = hw
        for line in hw.splitlines():
            print(f'  {line}')


class CaptureCallback:
    def on_capture_output_data(self, cache_time_ms: int, samples: bytes, samples_count: int) -> None:
        '''runs in another thread'''
        pass


class AlsaCaptureDevice:
    def __init__(self):
        self._ptr = ctypes.c_size_t(_AlsaDll.instance().dll.AlsaCaptureDevice_create())
        if not self._ptr.value:
            raise RuntimeError('AlsaCaptureDevice_create failed')
        self._c_output_data_callback = _alsa_output_data_callback(self._output_data_callback)
        self.capture_callback = None
        self.sample_rate = 0
        self.channels = 0
        self.bits_per_sample = 0

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        self.release()
        return False

    def __del__(self):
        try:
            self.release()
        except Exception:
            pass

    def release(self) -> None:
        if getattr(self, '_ptr', None) is None:
            return
        if self._ptr.value:
            _AlsaDll.instance().dll.AlsaDevice_release(self._ptr)
        self._ptr = ctypes.c_size_t(0)

    def open(self, device_id: str) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_open(self._ptr, device_id.encode())
        if not ret:
            return False
        _AlsaDll.instance().dll.AlsaCaptureDevice_setOutputCallback(
            self._ptr, self._c_output_data_callback, None)
        return True

    def set_period(self, period_count: int, period_time: int) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_setPeriod(
            self._ptr, ctypes.c_uint32(period_count), ctypes.c_uint32(period_time))
        return bool(ret)

    def set_params(self, sample_rate: int, channels: int, bits_per_sample: int) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_setParams(
            self._ptr,
            ctypes.c_uint32(sample_rate),
            ctypes.c_uint32(channels),
            ctypes.c_uint32(bits_per_sample),
        )
        if ret:
            self.sample_rate = sample_rate
            self.channels = channels
            self.bits_per_sample = bits_per_sample
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

    def get_volume(self, card_id: str = 'default') -> int:
        '''Return mixer volume [0,100], or negative on failure.'''
        return _AlsaDll.instance().dll.AlsaDevice_getVolume(self._ptr, card_id.encode())

    def set_volume(self, volume: int, card_id: str = 'default') -> bool:
        '''Set ALSA mixer volume on card_id (capture has no software volume path).'''
        ret = _AlsaDll.instance().dll.AlsaDevice_setVolume(self._ptr, card_id.encode(), volume)
        return bool(ret)

    def set_capture_callback(self, callback: CaptureCallback) -> None:
        self.capture_callback = callback

    def capture_to_pcm_file(self, file_name: str, append: bool = False) -> bool:
        ret = _AlsaDll.instance().dll.AlsaCaptureDevice_captureToPcmFile(
            self._ptr, file_name.encode(), int(append))
        return bool(ret)

    def _output_data_callback(self, cache_time_ms: int, samples_ptr: ctypes.c_void_p, samples_count: int, user) -> None:
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
        if not self._ptr.value:
            raise RuntimeError('AlsaPlaybackDevice_create failed')
        self._c_input_data_callback = _alsa_input_data_callback(self._input_data_callback)
        self._c_playback_stopped_callback = _alsa_playback_stopped_callback(self._playback_stopped_callback)
        self.playback_callback = None
        self.sample_rate = 0
        self.channels = 0
        self.bits_per_sample = 0

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        self.release()
        return False

    def __del__(self):
        try:
            self.release()
        except Exception:
            pass

    def release(self) -> None:
        if getattr(self, '_ptr', None) is None:
            return
        if self._ptr.value:
            _AlsaDll.instance().dll.AlsaDevice_release(self._ptr)
        self._ptr = ctypes.c_size_t(0)

    def open(self, device_id: str) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_open(self._ptr, device_id.encode())
        if not ret:
            return False
        _AlsaDll.instance().dll.AlsaPlaybackDevice_setInputCallback(
            self._ptr, self._c_input_data_callback, None)
        return True

    def set_min_cache_period_count(self, min_cache_period_count: int = 2) -> bool:
        ret = _AlsaDll.instance().dll.AlsaPlaybackDevice_setMinCachePeriodCount(
            self._ptr, ctypes.c_uint32(min_cache_period_count))
        return bool(ret)

    def set_prefill_ms(self, prefill_ms: int) -> bool:
        '''
        Optional playback prefill before auto-start. Call after open/set_params, before start().
        If not called, auto policy applies (IOPLUG: full buffer; HW: low latency).
        prefill_ms=0 forces low latency. Cleared on close(); each open cycle decides independently.
        '''
        ret = _AlsaDll.instance().dll.AlsaPlaybackDevice_setPrefillMs(
            self._ptr, ctypes.c_uint32(prefill_ms))
        return bool(ret)

    def set_period(self, period_count: int, period_time: int) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_setPeriod(
            self._ptr, ctypes.c_uint32(period_count), ctypes.c_uint32(period_time))
        return bool(ret)

    def set_params(self, sample_rate: int, channels: int, bits_per_sample: int) -> bool:
        ret = _AlsaDll.instance().dll.AlsaDevice_setParams(
            self._ptr,
            ctypes.c_uint32(sample_rate),
            ctypes.c_uint32(channels),
            ctypes.c_uint32(bits_per_sample),
        )
        if ret:
            self.sample_rate = sample_rate
            self.channels = channels
            self.bits_per_sample = bits_per_sample
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
            self._ptr, self._c_playback_stopped_callback, None)
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

    def get_volume(self, card_id: str = '') -> int:
        '''
        card_id '' -> software volume [0,100].
        non-empty -> ALSA mixer volume, or negative on failure.
        '''
        return _AlsaDll.instance().dll.AlsaDevice_getVolume(self._ptr, card_id.encode())

    def set_volume(self, volume: int, card_id: str = '') -> bool:
        '''
        Default card_id '' uses software PCM scaling (does not change system mixer).
        Pass card_id like 'hw:4' / 'default' to change ALSA mixer on that card.
        '''
        ret = _AlsaDll.instance().dll.AlsaDevice_setVolume(self._ptr, card_id.encode(), volume)
        return bool(ret)

    def set_playback_callback(self, callback: PlaybackCallback) -> None:
        self.playback_callback = callback

    def _input_data_callback(
        self, cache_time_ms: int, samples_ptr: ctypes.c_void_p,
        samples_count: ctypes.POINTER(ctypes.c_uint32), user
    ) -> None:
        requested = samples_count.contents.value
        frame_bytes = self.channels * self.bits_per_sample // 8
        if frame_bytes <= 0:
            samples_count.contents.value = 0
            return
        need_bytes = requested * frame_bytes
        libc = _libc()
        base = ctypes.cast(samples_ptr, ctypes.c_void_p).value or 0
        if self.playback_callback:
            input_bytes = self.playback_callback.on_playback_input_data(cache_time_ms, requested) or b''
            copy_len = min(len(input_bytes), need_bytes)
            if copy_len:
                libc.memcpy(ctypes.c_void_p(base), input_bytes, copy_len)
            if copy_len < need_bytes:
                libc.memset(ctypes.c_void_p(base + copy_len), 0, need_bytes - copy_len)
            samples_count.contents.value = copy_len // frame_bytes
        else:
            libc.memset(ctypes.c_void_p(base), 0, need_bytes)
            samples_count.contents.value = 0

    def _playback_stopped_callback(self, user) -> None:
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
    def __init__(self, path: str, sample_rate: int, channels: int, bits_per_sample: int):
        self._wf = None
        self._wf = wave.open(path, 'wb')
        self._wf.setnchannels(channels)
        self._wf.setsampwidth(_sampwidth_bytes(bits_per_sample))
        self._wf.setframerate(sample_rate)

    def write_frames(self, data: bytes) -> None:
        self._wf.writeframes(data)

    def close(self) -> None:
        wf = getattr(self, '_wf', None)
        if wf is not None:
            wf.close()
            self._wf = None

    def __del__(self):
        self.close()


class WavReader:
    def __init__(self, path: str):
        self._wf = None
        self._wf = wave.open(path, 'rb')
        self.sample_rate = self._wf.getframerate()
        self.channels = self._wf.getnchannels()
        self.bits_per_sample = self._wf.getsampwidth() * 8

    def read_frames(self, nframes: int) -> bytes:
        return self._wf.readframes(nframes)

    def close(self) -> None:
        wf = getattr(self, '_wf', None)
        if wf is not None:
            wf.close()
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
    parser.add_argument(
        '--verbose', action='store_true',
        help='with -i/-o, also query and print each device HW params (channels/rates/formats)',
    )
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
        help='volume 0-100; playback: software gain (default); capture: mixer on device card',
    )

    args = parser.parse_args()

    list_input = args.input
    list_output = args.output
    verbose = args.verbose
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
        _print_devices(get_capture_devices(), is_capture=True, verbose=verbose)
    if list_output:
        _print_devices(get_playback_devices(), is_capture=False, verbose=verbose)
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
        started = False
        with AlsaCaptureDevice() as dev:
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
                    hw = query_device_hw_params(device_id, is_capture=True)
                    card = mixer_card_from_hw_params(hw, device_id)
                    print(f'set_volume {volume} on mixer card {card}')
                    if not dev.set_volume(volume, card_id=card):
                        parser.error(f'set_volume failed on mixer card {card}')
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
        started = False
        with AlsaPlaybackDevice() as dev:
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
                    # Default: software volume for this stream only.
                    print(f'set_volume {volume} (software)')
                    if not dev.set_volume(volume):
                        parser.error('set_volume failed')
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
                playback_callback.close()
