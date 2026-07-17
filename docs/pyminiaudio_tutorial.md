# pyminiaudio 使用教程：采集与播放

这份教程面向第一次使用 `pyminiaudio` 的人。学习顺序是：

1. 先运行一段可工作的 `miniaudio` 代码。
2. 再解释这段代码里的参数、数据流和生成器写法。
3. 最后补充设备、后端、缓冲、frame/sample 等细节。

示例基于 `pyminiaudio 1.71`（需 **Python 3.10+**）。如果想直接跑完整命令行版本，可以看项目里的 `miniaudio_demo.py`。

## 1. 安装与导入

```bash
pip install miniaudio numpy
```

Python 包名是 `miniaudio`：

```python
import miniaudio
```

常用 API：

```python
miniaudio.Devices        # 枚举设备
miniaudio.PlaybackDevice # 打开播放设备
miniaudio.CaptureDevice  # 打开采集设备
miniaudio.decode_file    # 一次性解码音频文件到 PCM
miniaudio.stream_file    # 流式解码音频文件
```

## 2. 第一个程序：列出设备

先确认当前机器上 `pyminiaudio` 能看到哪些播放/采集设备。

```python
import miniaudio

devices = miniaudio.Devices()
print("Backend:", devices.backend)

print("\nPlayback devices:")
for i, dev in enumerate(devices.get_playbacks()):
    print(i, dev["name"], dev.get("formats"))

print("\nCapture devices:")
for i, dev in enumerate(devices.get_captures()):
    print(i, dev["name"], dev.get("formats"))
```

输出里的 `formats` 通常类似：

```python
{
    "format": "16-bit Signed Integer",
    "samplerate": 48000,
    "channels": 2,
}
```

这表示设备报告的原生或推荐格式。打开设备时仍然可以请求其他采样率、声道数或样本格式；miniaudio 或系统后端可能会帮你转换。

如果只是入门，先用默认设备即可，不必立刻指定 `device_id`。

## 3. 播放短音频文件

这个示例会把整个文件解码到内存，再交给播放设备。它适合提示音、小 WAV、小 MP3 等短文件。

```python
import time
import miniaudio


def playback_stream(raw: bytes, channels: int):
    frame_count = yield b""
    offset = 0
    bytes_per_sample = 2  # SIGNED16
    bytes_per_frame = bytes_per_sample * channels

    while offset < len(raw):
        wanted = frame_count * bytes_per_frame
        chunk = raw[offset : offset + wanted]
        offset += len(chunk)
        frame_count = yield chunk


filename = "test.mp3"
sample_rate = 16000
channels = 1

sound = miniaudio.decode_file(
    filename,
    output_format=miniaudio.SampleFormat.SIGNED16,
    nchannels=channels,
    sample_rate=sample_rate,
)

raw = sound.samples.tobytes()

stream = playback_stream(raw, channels)
next(stream)

device = miniaudio.PlaybackDevice(
    output_format=miniaudio.SampleFormat.SIGNED16,
    nchannels=channels,
    sample_rate=sample_rate,
    buffersize_msec=80,
)

device.start(stream)
time.sleep(sound.duration)
device.stop()
device.close()
```

这段代码的流程是：

```text
MP3/WAV/FLAC/OGG 文件 -> decode_file() -> PCM bytes -> playback_stream -> PlaybackDevice
```

几个关键点：

- `PlaybackDevice` 播放的是 PCM，不是直接播放 MP3 文件。
- `decode_file()` 可以把 WAV、MP3、FLAC、Vorbis 等格式解码成 PCM。
- `output_format=SIGNED16` 表示回调里使用 16-bit signed integer PCM。
- `nchannels=1` 是单声道，适合语音；音乐通常用 `2`。
- `sample_rate=16000` 常用于语音；音乐常见是 `44100` 或 `48000`。
- `next(stream)` 必须在 `device.start(stream)` 前调用，用来启动生成器。

### 3.1 播放生成器怎么工作

`frame_count` 不是我们自己传给 `playback_stream()` 的参数。它来自 `miniaudio` 内部的播放回调。

调用：

```python
device.start(stream)
```

之后，`PlaybackDevice` 会启动后台音频线程。每当设备需要下一小段 PCM 时，miniaudio 会算出这次需要多少帧，然后在内部做类似这样的事：

```python
pcm = stream.send(frame_count)
```

这个 `frame_count` 就会从生成器里的 `yield` 表达式返回：

```python
def playback_stream(raw: bytes, channels: int):
    frame_count = yield b""   # frame_count 来自后续 stream.send(frame_count)
    offset = 0
    bytes_per_frame = 2 * channels

    while offset < len(raw):
        wanted = frame_count * bytes_per_frame
        chunk = raw[offset : offset + wanted]
        offset += len(chunk)
        frame_count = yield chunk  # yield chunk 返回给设备；下一次 send() 再给新的 frame_count
```

真实的 `stream.send(frame_count)` 藏在 `PlaybackDevice.start()` 后面的音频线程里，代码里看不到直接调用。下面用纯 Python 写一个“假播放设备”，把这个隐藏动作打印出来：

```python
def playback_stream(raw: bytes, channels: int, sample_rate: int):
    frame_count = yield b""
    offset = 0
    bytes_per_frame = 2 * channels

    ms_value = frame_count * 1000 / sample_rate
    print(f"miniaudio wants {frame_count} frames ({ms_value:.1f} ms)")

    while offset < len(raw):
        wanted = frame_count * bytes_per_frame
        chunk = raw[offset : offset + wanted]
        offset += len(chunk)
        print(f"生成器：返回 {len(chunk)} bytes")
        frame_count = yield chunk


class FakePlaybackDevice:
    def __init__(self, sample_rate: int, period_ms: int):
        self.sample_rate = sample_rate
        self.period_frames = sample_rate * period_ms // 1000

    def start(self, stream):
        while True:
            frame_count = self.period_frames
            print(f"设备：stream.send({frame_count})")
            try:
                pcm = stream.send(frame_count)
            except StopIteration:
                print("设备：生成器结束，播放完成")
                break
            print(f"设备：收到 {len(pcm)} bytes PCM\n")


sample_rate = 16000
channels = 1
raw = b"\x00\x00" * 1600  # 16000 Hz、单声道、int16 下约 100 ms 静音

stream = playback_stream(raw, channels, sample_rate)
first = next(stream)
print(f"用户：next(stream) 启动生成器，拿到 {len(first)} bytes")

FakePlaybackDevice(sample_rate, period_ms=20).start(stream)
```

输出大致是：

```text
用户：next(stream) 启动生成器，拿到 0 bytes
设备：stream.send(320)
miniaudio wants 320 frames (20.0 ms)
生成器：返回 640 bytes
设备：收到 640 bytes PCM

设备：stream.send(320)
生成器：返回 640 bytes
设备：收到 640 bytes PCM
```

这个例子里的 `FakePlaybackDevice.start()` 对应真实代码里的：

```python
next(stream)
device.start(stream)
```

区别只是：真实 `PlaybackDevice` 在 C/miniaudio 的音频线程里调用 `stream.send(frame_count)`，所以你在 Python 示例代码里看不到这个源头。

`frame_count` 表示设备这一次要多少帧音频。它通常和设备回调周期、缓冲配置、采样率有关。比如 `sample_rate=16000`、一次要 20 ms：

```text
frame_count = 16000 * 20 / 1000 = 320 frames
```

对于 `SIGNED16`：

```text
bytes = frame_count * channels * 2
```

所以单声道 `SIGNED16` 下，`320 frames` 正好是：

```text
320 * 1 * 2 = 640 bytes
```

示例里第一次 `yield b""` 只是生成器的启动点，不是实际播放的数据。真正的播放数据从后续 `stream.send(frame_count)` 触发的 `yield chunk` 开始返回。

项目里的 `miniaudio_demo.py` 也可以看到类似打印。播放时加 `--debug-device`：

```bash
python3 miniaudio_demo.py -p -f cap.wav -s 16000 -n 1 --debug-device
```

播放回调会打印第一次请求：

```text
miniaudio wants 1280 frames (80.0 ms)
```

这个值表示 miniaudio 第一次向 Python 生成器请求约 80 ms 的 PCM。实际帧数可能随后端、缓冲和设备调度变化；为了避免回调里大量打印，demo 只打印第一次。

### 3.2 播放回调可以 yield 什么

上面示例 yield `bytes`。`PlaybackDevice` 也支持：

- `memoryview`
- `array.array`
- 形状为 `(numframes, nchannels)` 的 `numpy.ndarray`

如果使用 `numpy.ndarray`，`dtype` 要和 `output_format` 对应，例如 `SIGNED16` 对应 `np.int16`。

## 4. 流式播放大文件

大文件不适合一次性解码到内存，可以用 `stream_file()` 按块解码。

```python
import time
import miniaudio

filename = "music.mp3"
sample_rate = 44100
channels = 2
decode_ms = 40
frames_to_read = sample_rate * decode_ms // 1000

file_stream = miniaudio.stream_file(
    filename,
    output_format=miniaudio.SampleFormat.SIGNED16,
    nchannels=channels,
    sample_rate=sample_rate,
    frames_to_read=frames_to_read,
)
next(file_stream)


def playback_stream():
    frame_count = yield b""
    while True:
        try:
            samples = file_stream.send(frame_count)
        except StopIteration:
            return
        frame_count = yield samples


stream = playback_stream()
next(stream)

device = miniaudio.PlaybackDevice(
    output_format=miniaudio.SampleFormat.SIGNED16,
    nchannels=channels,
    sample_rate=sample_rate,
    buffersize_msec=100,
)

device.start(stream)

try:
    while device.running:
        time.sleep(0.1)
finally:
    device.stop()
    device.close()
```

这段代码的数据流是：

```text
音频文件 -> stream_file() 解码生成器 -> playback_stream -> PlaybackDevice
```

`stream_file()` 返回的也是生成器。它被 `next(file_stream)` 启动后，就可以用 `file_stream.send(frame_count)` 按需取出 PCM。

文件播放完时，`file_stream.send()` 会抛出 `StopIteration`，外层 `playback_stream()` 结束，播放设备也会停止。

### 4.1 frames_to_read 是什么

`frames_to_read` 表示解码器每次最多读多少 **frame**。

在 miniaudio 和 Python 标准库 `wave` 里，frame 都是按时间轴计数：

```text
1 frame = 一个采样时刻
```

如果是双声道，1 frame 里有 2 个 sample：

```text
stereo, SIGNED16:
1 frame = left sample + right sample = 4 bytes
```

时长换算：

```text
时长(ms) = frames_to_read * 1000 / sample_rate
```

例如：

```text
1764 frames @ 44100 Hz ~= 40 ms
```

常见参考：

| sample_rate | 20 ms | 40 ms | 80 ms |
| --- | --- | --- | --- |
| 16000 | 320 | 640 | 1280 |
| 44100 | 882 | 1764 | 3528 |
| 48000 | 960 | 1920 | 3840 |

`frames_to_read` 控制解码侧一次读多少；`buffersize_msec` 控制设备侧缓冲大约多长。两者不必完全相等，但保持同一量级通常更稳定。

## 5. 录音到 WAV

下面示例从默认麦克风录 5 秒，并保存成 `capture.wav`。

```python
import time
import wave
import miniaudio


def record_callback(chunks: list[bytes]):
    _ = yield
    while True:
        data = yield
        chunks.append(bytes(data))


sample_rate = 16000
channels = 1
duration = 5
chunks: list[bytes] = []

stream = record_callback(chunks)
next(stream)

device = miniaudio.CaptureDevice(
    input_format=miniaudio.SampleFormat.SIGNED16,
    nchannels=channels,
    sample_rate=sample_rate,
    buffersize_msec=80,
)

device.start(stream)
time.sleep(duration)
device.stop()
device.close()

raw = b"".join(chunks)

with wave.open("capture.wav", "wb") as wf:
    wf.setnchannels(channels)
    wf.setsampwidth(2)
    wf.setframerate(sample_rate)
    wf.writeframes(raw)

print("saved capture.wav")
```

采集的数据流和播放相反：

```text
麦克风 -> CaptureDevice -> stream.send(PCM) -> record_callback -> chunks -> WAV
```

几个关键点：

- 采集回调收到的是 bytes-like 数据，常见类型是 `bytearray`。
- `bytes(data)` 可以把它固定成普通 `bytes`，方便后续拼接或入队。
- `SIGNED16` 的 sample width 是 2 字节，所以 `wave.setsampwidth(2)`。
- WAV 写入时必须设置声道数、采样宽度、采样率。

### 5.1 采集生成器怎么工作

采集生成器不是你主动调用来读取麦克风。`device.start(stream)` 后，miniaudio 后台线程会不断把录到的 PCM 发送进来。

```python
def record_callback(chunks: list[bytes]):
    _ = yield
    while True:
        data = yield
        chunks.append(bytes(data))
```

用纯 Python 类比就是：

```python
def consumer(received: list[bytes]):
    _ = yield
    while True:
        data = yield
        received.append(data)


received: list[bytes] = []
stream = consumer(received)
next(stream)

stream.send(b"chunk-1")
stream.send(b"chunk-2")
```

对应关系：

| 纯 Python 例子 | CaptureDevice |
| --- | --- |
| `stream.send(b"chunk-1")` | 后台线程发送录到的 PCM |
| `data = yield` | 回调接收 PCM |
| `received.append(data)` | 保存或处理 PCM |

## 6. 实时采集处理

如果要做唤醒词检测、ASR 或实时波形，不一定要保存全部数据。更常见的写法是：音频回调只负责入队，主线程或工作线程再慢慢处理。

```python
import queue
import time
import miniaudio

audio_queue: queue.Queue[bytes] = queue.Queue()


def capture_callback():
    _ = yield
    while True:
        data = yield
        audio_queue.put(bytes(data))


stream = capture_callback()
next(stream)

device = miniaudio.CaptureDevice(
    input_format=miniaudio.SampleFormat.SIGNED16,
    nchannels=1,
    sample_rate=16000,
    buffersize_msec=80,
)

device.start(stream)

try:
    while True:
        chunk = audio_queue.get()
        print("got bytes:", len(chunk))
        # 这里可以把 chunk 转成 numpy int16，再送给唤醒词/ASR 模型。
finally:
    device.stop()
    device.close()
```

不要在音频回调里做太重的计算。推荐分工：

- 回调：只收数据、入队。
- 主线程/工作线程：转 numpy、跑模型、写文件、画波形。

## 7. PCM 转 numpy

采集或解码得到 `bytes` 后，可以这样转成 numpy：

```python
import numpy as np

pcm = np.frombuffer(raw, dtype=np.int16)

if channels > 1:
    pcm = pcm.reshape(-1, channels)
```

双声道转单声道：

```python
mono = pcm.mean(axis=1).astype(np.int16)
```

归一化成 `float32`：

```python
float_pcm = pcm.astype(np.float32) / 32768.0
```

`dtype` 要和 `SampleFormat` 对应：

| miniaudio SampleFormat | Python / numpy | 每 sample 字节 |
| --- | --- | --- |
| `UNSIGNED8` | `np.uint8` | 1 |
| `SIGNED16` | `np.int16` / `array.array("h")` | 2 |
| `SIGNED24` | 通常用 bytes | 3 |
| `SIGNED32` | `np.int32` / `array.array("i")` | 4 |
| `FLOAT32` | `np.float32` / `array.array("f")` | 4 |

初学和语音场景优先用：

```python
miniaudio.SampleFormat.SIGNED16
```

## 8. 保存成 MP3/FLAC/OGG 可以吗

`pyminiaudio` 对压缩格式主要是解码，不负责把录音编码成 MP3/FLAC/OGG。

| 方向 | 支持 |
| --- | --- |
| 文件 -> PCM | WAV、MP3、FLAC、Vorbis 等 |
| PCM -> 文件 | WAV |

保存录音最简单的方式是先写 WAV。如果要 MP3，可以再用 FFmpeg 转码：

```bash
ffmpeg -y -i capture.wav -codec:a libmp3lame -b:a 128k capture.mp3
```

也可以把 raw PCM 直接喂给 FFmpeg：

```python
import subprocess

sample_rate = 16000
channels = 1

cmd = [
    "ffmpeg",
    "-y",
    "-f", "s16le",
    "-ar", str(sample_rate),
    "-ac", str(channels),
    "-i", "pipe:0",
    "-codec:a", "libmp3lame",
    "-b:a", "64k",
    "capture.mp3",
]

subprocess.run(cmd, input=raw, check=True)
```

如果采集格式不是 `SIGNED16`，FFmpeg 的 `-f` 参数也要对应修改。例如 `FLOAT32` 通常用 `-f f32le`。

## 9. 选择设备

先列出设备，再把枚举结果里的 `id` 传给 `PlaybackDevice` 或 `CaptureDevice`。

```python
import miniaudio

devices = miniaudio.Devices()
playbacks = devices.get_playbacks()

device = miniaudio.PlaybackDevice(
    device_id=playbacks[0]["id"],
    output_format=miniaudio.SampleFormat.SIGNED16,
    nchannels=1,
    sample_rate=16000,
)
```

采集设备类似：

```python
captures = miniaudio.Devices().get_captures()

device = miniaudio.CaptureDevice(
    device_id=captures[0]["id"],
    input_format=miniaudio.SampleFormat.SIGNED16,
    nchannels=1,
    sample_rate=16000,
)
```

建议：

- 初学先用默认设备，也就是不传 `device_id`。
- 需要指定 USB 声卡、HDMI、内置声卡时，再使用枚举出来的 `id`。
- Linux ALSA 下直接选择某个硬件设备可能绕过共享混音；默认设备有时更容易走 PulseAudio/PipeWire/dmix。

## 10. 常用设备参数

`PlaybackDevice` 常用参数：

```python
miniaudio.PlaybackDevice(
    output_format=miniaudio.SampleFormat.SIGNED16,
    nchannels=2,
    sample_rate=44100,
    buffersize_msec=200,
    device_id=None,
    callback_periods=0,
    backends=None,
    thread_prio=miniaudio.ThreadPriority.HIGHEST,
    app_name="",
)
```

`CaptureDevice` 类似，只是 `output_format` 换成 `input_format`。

### output_format / input_format

表示回调里传输的 PCM 样本格式，不是 MP3/WAV 这类文件格式。

常用：

```python
miniaudio.SampleFormat.SIGNED16
```

语音、普通录音、WAV 保存、普通播放都够用。

### nchannels

声道数：

- `1`：单声道，语音采集常用。
- `2`：立体声，音乐播放常用。

### sample_rate

采样率，单位 Hz：

- `16000`：语音识别、唤醒词检测常用。
- `44100`：音乐、普通消费音频常见。
- `48000`：很多声卡和系统后端默认值。

如果只是播放文件，优先使用文件采样率或设备原生采样率。做语音算法时，通常直接用 `16000`。

### buffersize_msec

底层音频缓冲区的大致时间长度，单位毫秒。

它影响：

- 延迟：缓冲越小，响应越快。
- 稳定性：缓冲越大，越不容易卡顿、爆音、欠载。
- Python 调度压力：缓冲越小，回调越频繁。

常用建议：

| 场景 | 建议值 |
| --- | --- |
| 普通播放音乐、提示音 | `100` 到 `200` |
| 语音播放，要求不高 | `80` 到 `150` |
| 语音采集，给唤醒词/ASR | `40` 到 `100` |
| 低延迟实时交互 | `10` 到 `40` |
| Python 回调里处理较多逻辑 | `80` 到 `200` |

项目里的 `miniaudio_demo.py` 使用：

```python
buffersize_msec=80
```

这是语音场景的折中值：延迟不算高，也比 10ms/20ms 更不容易因为 Python 调度导致 underrun。

选择经验：

- 第一次写 demo：用 `200`，先保证稳定。
- 语音唤醒/录音：用 `80`。
- 感觉延迟大：试 `40`。
- 出现断续、爆音、播放不完整：增大到 `100` 或 `200`。

### callback_periods

高级参数，默认 `0`。它和底层设备回调周期有关，通常不需要改。

### thread_prio

音频线程优先级，默认 `HIGHEST`。普通 Python 应用保持默认即可。

### app_name

应用名。部分后端会显示它，例如 PulseAudio/JACK：

```python
device = miniaudio.PlaybackDevice(app_name="my-player")
```

## 11. 后端选择

查看当前编译支持的后端：

```python
import miniaudio

print([b.name for b in miniaudio.get_enabled_backends()])
```

默认后端由 miniaudio 自动选择：

```python
device = miniaudio.PlaybackDevice()
print(device.backend)
```

也可以强制指定：

```python
device = miniaudio.PlaybackDevice(
    backends=[miniaudio.Backend.ALSA],
)
```

不同系统常见后端：

| 系统 | 常见后端 |
| --- | --- |
| Windows | `WASAPI` |
| Linux | `PulseAudio`、`ALSA`、`JACK` |
| macOS | `Core Audio` |

Linux 上要注意：

- 如果选 `PulseAudio`，共享播放通常由 PulseAudio/PipeWire 处理。
- 如果选 `ALSA`，共享播放通常依赖 ALSA 的 `dmix` 插件。
- 直接打开 `hw:...` 一般是硬件独占，常见现象是只能打开一次。
- `pyminiaudio 1.71` 的 Python 层不能手工构造 ALSA 字符串 device id，例如不能直接传 `"dmix:CARD=Device,DEV=0"` 给 `device_id`。
- 通常做法是使用系统默认设备，或用 `.asoundrc` 把默认 PCM 指向想要的 `dmix` / Pulse。

强制 PulseAudio：

```python
device = miniaudio.PlaybackDevice(
    backends=[miniaudio.Backend.PULSEAUDIO],
)
```

强制 ALSA：

```python
device = miniaudio.PlaybackDevice(
    backends=[miniaudio.Backend.ALSA],
)
```

如果系统 ALSA `default` 指向 PulseAudio，即使强制 ALSA 后端，打开 `default` PCM 时也可能进入 PulseAudio 插件。

## 12. bytes、frame、sample 的关系

术语分工：

- **frame**：时间轴上的一个采样时刻；`sample_rate` 就是每秒多少个 frame。
- **sample**：某一声道在该时刻的一个数值。
- **bytes**：交错 PCM 在内存里的字节数。

对于 `SIGNED16`：

```text
1 sample = 2 bytes
1 frame = channels * 2 bytes
bytes = frames * channels * 2
```

例子：

```text
16000 Hz, mono, int16, 1 second
= 16000 frames
= 16000 samples
= 32000 bytes
```

```text
48000 Hz, stereo, int16, 1 second
= 48000 frames
= 96000 samples
= 192000 bytes
```

与 Python 标准库 `wave` 的对应关系：

| miniaudio / 本文 | `wave` 模块 |
| --- | --- |
| `frames_to_read`、回调里的 `frame_count` | `readframes(n)` 的 `n` |
| 解码后的总 frame 数 | `getnframes()` |
| `sample_rate` | `getframerate()` |
| seek 到第 `i` 个时刻 | `setpos(i)` |

## 13. Linux ALSA / PulseAudio 注意事项

查看 ALSA 设备：

```bash
aplay -l
aplay -L
```

如果想让默认 PCM 走 USB 声卡的 dmix，可以配置 `~/.asoundrc`，例如：

```conf
pcm.!default {
    type plug
    slave.pcm "dmix:CARD=Device,DEV=0"
}

ctl.!default {
    type hw
    card Device
}
```

注意：

- `hw:CARD=Device,DEV=0` 是硬件直连，通常不共享。
- `plughw:CARD=Device,DEV=0` 有格式转换，但不等于混音共享。
- `dmix:CARD=Device,DEV=0` 是播放共享混音。
- 采集共享通常对应 `dsnoop`。

`pyminiaudio 1.71` 的 Python CFFI 层没有公开最终 `snd_pcm_open()` 使用的字符串。你能看到的是：

- 选择了哪个 miniaudio backend。
- 请求的采样率、声道数、格式。
- 枚举出来的设备名和格式。

最终 ALSA 插件链需要从 ALSA 配置、系统日志或修改底层绑定来确认。

## 14. 常见问题

### 播放有杂音或断续

优先尝试：

- 增大 `buffersize_msec`：从 `80` 改到 `100` 或 `200`。
- 减少回调里的计算。
- 使用设备原生采样率，例如 `48000`。
- 不要在播放回调里做文件读取、网络请求、复杂 numpy 计算。

### 播放延迟太大

尝试：

- 把 `buffersize_msec` 从 `200` 降到 `100`、`80`、`40`。
- 使用更低延迟的后端。
- 避免经过 PulseAudio/PipeWire 的高延迟配置。

### 只能打开一次设备

如果直接打开的是 ALSA `hw:...`，这通常是正常现象，因为硬件设备多半独占。

解决方向：

- 用 PulseAudio/PipeWire。
- 用 ALSA `dmix` 播放。
- 用 ALSA `dsnoop` 采集。
- 不要让多个进程直接抢同一个 `hw`。

## 15. 推荐起步参数

语音采集：

```python
input_format=miniaudio.SampleFormat.SIGNED16
nchannels=1
sample_rate=16000
buffersize_msec=80
```

提示音播放：

```python
output_format=miniaudio.SampleFormat.SIGNED16
nchannels=1
sample_rate=16000
buffersize_msec=80
```

音乐播放：

```python
output_format=miniaudio.SampleFormat.SIGNED16
nchannels=2
sample_rate=44100  # 或 48000
buffersize_msec=100
```

稳定优先：

```python
buffersize_msec=200
```

低延迟优先：

```python
buffersize_msec=40
```

如果低延迟下出现卡顿，就增大缓冲。

## 16. 项目里的完整命令行 demo

本项目提供了 `miniaudio_demo.py`，可以直接试：

```bash
python3 miniaudio_demo.py -l
python3 miniaudio_demo.py -p -f cap.wav
python3 miniaudio_demo.py -p -f cap.wav -s 16000 -n 1 --backend alsa --debug-device
python3 miniaudio_demo.py -c -f capture.wav -s 16000 -n 1
```

录音时会提示输入时长（秒），直接回车默认 5 秒。

建议学习顺序：

1. 先跑 `-l` 看设备和 backend。
2. 用默认设备播放一个 WAV。
3. 指定 `-s 16000 -n 1` 播放语音文件。
4. 录制 5 秒并保存 WAV。
5. 再尝试 `--backend alsa` 或 `--backend pulse`。
