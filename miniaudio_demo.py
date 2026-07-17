#!/usr/bin/env python3
"""
Standalone pyminiaudio capture & playback demo.

Usage:
    miniaudio_demo.py -l                    # List all devices
    miniaudio_demo.py -l -c                 # List capture devices only
    miniaudio_demo.py -l -p                 # List playback devices only
    miniaudio_demo.py -c -f cap.wav -i 1    # Capture from capture device 1 to WAV
    miniaudio_demo.py -c -f cap.wav -i 1 -s 16000 -n 1   # Capture at 16kHz mono
    miniaudio_demo.py -p -f play.mp3 -i 1                 # Play a supported audio file
    miniaudio_demo.py -p -f play.flac -i 1 -s 48000 -n 2  # Play decoded audio as 48kHz stereo
    miniaudio_demo.py -l -c -f cap.wav -i 1     # List capture devices then capture
    miniaudio_demo.py -l -p -f play.ogg -i 1    # List playback devices then play
"""

import argparse
import sys
import time
import wave
from pathlib import Path

import miniaudio
import numpy as np


SAMPLE_RATE = 16000
CHANNELS = 1
BUFFER_MS = 80
SAMPLE_FORMAT = miniaudio.SampleFormat.SIGNED16
SAMPLE_WIDTH = 2
WAVEFORM_BARS = 50


BACKEND_CHOICES = ("default", "alsa", "pulse", "pulseaudio", "jack", "null")


def parse_backend_choice(choice: str | None) -> list[miniaudio.Backend] | None:
    """Convert a CLI backend name into pyminiaudio's optional backend list."""
    if not choice or choice == "default":
        return None

    mapping = {
        "alsa": miniaudio.Backend.ALSA,
        "pulse": miniaudio.Backend.PULSEAUDIO,
        "pulseaudio": miniaudio.Backend.PULSEAUDIO,
        "jack": miniaudio.Backend.JACK,
        "null": miniaudio.Backend.NULL,
    }
    return [mapping[choice]]


def _c_string(value) -> str:
    """Best-effort conversion for bytes or CFFI char arrays."""
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.split(b"\x00", 1)[0].decode(errors="replace")
    try:
        return miniaudio.ffi.string(value).decode(errors="replace")
    except Exception:
        return str(value)


def _share_mode_name(value) -> str:
    if value == 0:
        return "shared"
    if value == 1:
        return "exclusive"
    return str(value)


def _format_name(value) -> str:
    return getattr(value, "name", str(value))


def describe_open_device(device, mode: str) -> list[str]:
    """Return debug lines for fields pyminiaudio exposes after device initialization."""
    lines = [f"miniaudio backend: {getattr(device, 'backend', 'unknown')}"]

    sample_rate = getattr(device, "sample_rate", None)
    channels = getattr(device, "nchannels", None)
    sample_format = getattr(device, "format", None)
    if sample_rate is not None and channels is not None and sample_format is not None:
        lines.append(
            f"requested {mode} format: "
            f"{sample_rate} Hz, {channels} ch, {_format_name(sample_format)}"
        )

    try:
        native_device = device._device[0]
        endpoint = getattr(native_device, mode)
    except Exception as exc:
        lines.append(f"opened {mode} internals: not exposed by this pyminiaudio build")
        lines.append(f"debug detail: {exc}")
        lines.append("note: pyminiaudio does not expose the final ALSA snd_pcm_open() name such as dmix:0.")
        return lines

    name = _c_string(getattr(endpoint, "name", ""))
    if name:
        lines.append(f"opened {mode} name: {name}")

    try:
        lines.append(f"opened {mode} share mode: {_share_mode_name(int(endpoint.shareMode))}")
    except Exception:
        pass

    try:
        lines.append(
            f"opened {mode} internal format: "
            f"{int(endpoint.internalSampleRate)} Hz, "
            f"{int(endpoint.internalChannels)} ch, "
            f"format={int(endpoint.internalFormat)}"
        )
    except Exception:
        pass

    lines.append("note: pyminiaudio does not expose the final ALSA snd_pcm_open() name such as dmix:0.")
    return lines


def print_open_device_debug(device, mode: str):
    print("\nDevice debug:")
    for line in describe_open_device(device, mode):
        print(f"  {line}")


def print_waveform(pcm: np.ndarray, num_bars: int = WAVEFORM_BARS):
    """Print a horizontal waveform chart from PCM int16 data."""
    if len(pcm) == 0:
        return

    chunk_size = max(1, len(pcm) // num_bars)
    bars = []
    for i in range(num_bars):
        start = i * chunk_size
        end = start + chunk_size
        segment = pcm[start:end].astype(np.float32) / 32768.0
        rms = float(np.sqrt(np.mean(segment ** 2))) if len(segment) > 0 else 0.0
        bars.append(rms)

    max_rms = max(bars) or 1e-9
    min_rms = min(bars)
    height = 10

    grid: list[str] = []
    for row in range(height, 0, -1):
        threshold = row / height
        line = ""
        for rms in bars:
            ratio = rms / max_rms
            line += "█" if ratio >= threshold else " "
        grid.append(line)

    time_axis = "-" * num_bars
    print(f"\nWaveform ({num_bars} segments, RMS range: {min_rms:.4f} ~ {max_rms:.4f}):")
    for row_idx, line in enumerate(grid):
        label = f"{height - row_idx:>3}"
        print(f"  {label} |{line}")
    print(f"    0 +{time_axis}")
    print(f"     0{'':>{num_bars - 1}}{'100%':>4}  time ->")


def _format_summary(device: dict) -> str:
    formats = device.get("formats") or []
    if not formats:
        return "-"

    parts = []
    for fmt in formats[:3]:
        parts.append(f"{fmt['samplerate']}Hz/{fmt['channels']}ch/{fmt['format']}")
    if len(formats) > 3:
        parts.append("...")
    return ", ".join(parts)


def _device_defaults(device: dict) -> tuple[int, int]:
    formats = device.get("formats") or []
    if not formats:
        return SAMPLE_RATE, CHANNELS

    first = formats[0]
    sample_rate = int(first.get("samplerate") or SAMPLE_RATE)
    channels = int(first.get("channels") or CHANNELS)
    return sample_rate, max(1, channels)


def list_devices(mode: str | None = None, backends: list[miniaudio.Backend] | None = None):
    """List miniaudio devices. mode=None -> all; 'capture' -> input only; 'playback' -> output only."""
    devices = miniaudio.Devices(backends=backends)
    print(f"Backend: {devices.backend}")

    if mode in (None, "capture"):
        captures = devices.get_captures()
        print(f"\n{'Index':>5}  {'Capture device':<70}  Native formats")
        print("-" * 115)
        for idx, dev in enumerate(captures):
            print(f"{idx:>5}  {dev['name']:<70}  {_format_summary(dev)}")

    if mode in (None, "playback"):
        playbacks = devices.get_playbacks()
        print(f"\n{'Index':>5}  {'Playback device':<70}  Native formats")
        print("-" * 115)
        for idx, dev in enumerate(playbacks):
            print(f"{idx:>5}  {dev['name']:<70}  {_format_summary(dev)}")

    print()


def _select_device(
    mode: str,
    device_index: int | None,
    backends: list[miniaudio.Backend] | None = None,
) -> dict | None:
    if device_index is None:
        return None
    devices = miniaudio.Devices(backends=backends)
    available = devices.get_captures() if mode == "capture" else devices.get_playbacks()
    if device_index < 0 or device_index >= len(available):
        print(f"Invalid {mode} device index: {device_index}")
        sys.exit(1)
    return available[device_index]


def _record_callback(chunks: list[bytes]):
    _ = yield
    while True:
        data = yield
        chunks.append(bytes(data))


def capture(
    wav_path: str,
    device_index: int | None = None,
    sample_rate: int | None = None,
    channels: int | None = None,
    debug_device: bool = False,
    backends: list[miniaudio.Backend] | None = None,
):
    """Record from microphone to a 16-bit PCM WAV file."""
    selected_device = _select_device("capture", device_index, backends)
    device_name = selected_device["name"] if selected_device else "default"
    native_rate, native_channels = _device_defaults(selected_device) if selected_device else (SAMPLE_RATE, CHANNELS)

    sample_rate = sample_rate or native_rate
    channels = channels or min(native_channels, 2)

    print(f"Capture device: [{device_index if device_index is not None else 'default'}] {device_name}")
    print(f"Device native guess: {native_rate} Hz, {native_channels} ch")
    print(f"Using: {sample_rate} Hz, {channels} ch, signed 16-bit")

    duration = float(input("Recording duration (seconds): ").strip() or "5")
    print(f"\nRecording {duration}s ... (press Ctrl+C to stop early)")

    chunks: list[bytes] = []
    stream = _record_callback(chunks)
    next(stream)

    device = miniaudio.CaptureDevice(
        input_format=SAMPLE_FORMAT,
        nchannels=channels,
        sample_rate=sample_rate,
        buffersize_msec=BUFFER_MS,
        device_id=selected_device["id"] if selected_device else None,
        backends=backends,
    )
    if debug_device:
        print_open_device_debug(device, "capture")

    try:
        device.start(stream)
        time.sleep(duration)
    except KeyboardInterrupt:
        print("\nStopping capture...")
    finally:
        device.stop()
        device.close()

    raw = b"".join(chunks)
    if not raw:
        print("No audio captured.")
        return

    pcm = np.frombuffer(raw, dtype=np.int16)
    frame_count = len(pcm) // channels
    pcm = pcm[: frame_count * channels]
    if channels > 1:
        pcm_2d = pcm.reshape(-1, channels)
        preview = pcm_2d.mean(axis=1).astype(np.int16)
    else:
        preview = pcm

    print_waveform(preview)

    wav_path = str(wav_path)
    with wave.open(wav_path, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm.tobytes())

    actual_duration = frame_count / sample_rate
    print(f"Saved: {wav_path}  ({frame_count} frames, {actual_duration:.1f}s, {sample_rate} Hz, {channels} ch)")


def _playback_stream(raw: bytes, channels: int, sample_rate: int, debug_callback: bool = False):
    frame_count = yield b""
    offset = 0
    bytes_per_frame = SAMPLE_WIDTH * channels

    if debug_callback:
        ms_value = frame_count * 1000 / sample_rate
        print(f"miniaudio wants {frame_count} frames ({ms_value:.1f} ms)")

    while offset < len(raw):
        wanted = frame_count * bytes_per_frame
        chunk = raw[offset : offset + wanted]
        offset += len(chunk)
        frame_count = yield chunk


def playback(
    audio_path: str,
    device_index: int | None = None,
    target_rate: int | None = None,
    target_channels: int | None = None,
    debug_device: bool = False,
    backends: list[miniaudio.Backend] | None = None,
):
    """Play an audio file through pyminiaudio.

    miniaudio can decode WAV, FLAC, MP3, and Vorbis.
    """
    audio_path = Path(audio_path)
    if not audio_path.exists():
        print(f"File not found: {audio_path}")
        sys.exit(1)

    info = miniaudio.get_file_info(str(audio_path))
    play_rate = target_rate or info.sample_rate
    play_channels = target_channels or info.nchannels

    sound = miniaudio.decode_file(
        str(audio_path),
        output_format=SAMPLE_FORMAT,
        nchannels=play_channels,
        sample_rate=play_rate,
    )

    raw = sound.samples.tobytes()
    pcm = np.frombuffer(raw, dtype=np.int16)
    if play_channels > 1:
        preview = pcm.reshape(-1, play_channels).mean(axis=1).astype(np.int16)
    else:
        preview = pcm

    print(f"Audio: {audio_path.name}  {info.sample_rate} Hz, {info.nchannels} ch, {info.duration:.1f}s")
    if info.sample_rate != play_rate:
        print(f"Resample: {info.sample_rate}Hz -> {play_rate}Hz")
    if info.nchannels != play_channels:
        print(f"Channel mix: {info.nchannels}ch -> {play_channels}ch")

    print_waveform(preview)

    selected_device = _select_device("playback", device_index, backends)
    device_name = selected_device["name"] if selected_device else "default"
    print(f"Playback device: [{device_index if device_index is not None else 'default'}] {device_name}")

    stream = _playback_stream(raw, play_channels, play_rate, debug_callback=debug_device)
    next(stream)

    frame_count = len(pcm) // play_channels
    duration = frame_count / play_rate
    print(f"Playing: {audio_path.name}  ({frame_count} frames, {duration:.1f}s, {play_rate} Hz, {play_channels} ch)")
    print("Press Ctrl+C to stop.\n")

    device = miniaudio.PlaybackDevice(
        output_format=SAMPLE_FORMAT,
        nchannels=play_channels,
        sample_rate=play_rate,
        buffersize_msec=BUFFER_MS,
        device_id=selected_device["id"] if selected_device else None,
        backends=backends,
    )
    if debug_device:
        print_open_device_debug(device, "playback")

    try:
        device.start(stream)
        time.sleep(duration + 0.2)
    except KeyboardInterrupt:
        print("\nStopping playback...")
    finally:
        device.stop()
        device.close()

    print("Playback finished.")


def main():
    parser = argparse.ArgumentParser(description="pyminiaudio capture & playback demo")
    parser.add_argument("-l", "--list", action="store_true", help="List audio devices")
    parser.add_argument("-c", "--capture", action="store_true", help="Capture (record) audio to file")
    parser.add_argument("-p", "--play", action="store_true", help="Play audio from file")
    parser.add_argument(
        "-f",
        "--file",
        default="",
        help="Audio file path. Capture writes WAV; playback accepts pyminiaudio-supported formats.",
    )
    parser.add_argument("-i", "--index", type=int, default=None, help="Device index within capture/playback list")
    parser.add_argument("-s", "--samplerate", type=int, default=None, help="Sample rate")
    parser.add_argument("-n", "--channels", type=int, default=None, help="Channel count")
    parser.add_argument(
        "--backend",
        choices=BACKEND_CHOICES,
        default="default",
        help=f"Preferred miniaudio backend {', '.join(BACKEND_CHOICES)} (default: default)",
    )
    parser.add_argument("--debug-device", action="store_true", help="Print pyminiaudio backend/open-device details")
    args = parser.parse_args()
    backends = parse_backend_choice(args.backend)

    if args.list and not args.capture and not args.play:
        list_devices(backends=backends)
        return

    if args.list:
        if args.capture:
            list_devices("capture", backends)
        elif args.play:
            list_devices("playback", backends)
        else:
            list_devices(backends=backends)

    if args.capture:
        wav_path = args.file or "capture.wav"
        capture(wav_path, args.index, args.samplerate, args.channels, args.debug_device, backends)
        return

    if args.play:
        if not args.file:
            print("Error: --file (-f) is required for playback")
            sys.exit(1)
        playback(args.file, args.index, args.samplerate, args.channels, args.debug_device, backends)
        return

    parser.print_help()


if __name__ == "__main__":
    main()
