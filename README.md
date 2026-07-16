# ALSABridge

在 Linux 上通过 Python 使用 ALSA 进行录音与播放。

ALSA 概念入门（`hw`/`plughw`、采集独占、多 App 共享、本项目用到的全部 ALSA API）见：[docs/alsa-tutorial.md](docs/alsa-tutorial.md)。

项目由两层组成：

| 层级 | 名称 | 说明 |
|------|------|------|
| Python 桥接 | **ALSABridge**（`alsabridge.py`） | 对外提供 Python API，通过 ctypes 调用底层库 |
| C++ 设备库 | **ALSADevice**（`libalsadevice.so`） | 封装 ALSA 设备打开、采播、音量等操作 |

## 项目结构

| 文件 / 目录 | 说明 |
|-------------|------|
| `alsabridge.py` | Python 桥接层 |
| `alsadevice.cpp` / `alsadevice.h` | C++ ALSA 设备实现 |
| `libalsadevice.so` | 编译生成的动态库，需与 `alsabridge.py` 放在同一目录 |
| `log_util.py` | 日志与终端颜色输出 |
| `main.cpp` | C++ 测试 / 演示程序 |
| `Makefile` | 编译脚本 |

## 依赖安装（Debian / Ubuntu / WSL2）

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  make \
  libasound2 \
  libasound-dev \
  python3
```

## WSL2 音频配置

WSL2 本身没有物理声卡，系统音频由 **WSLg** 通过 **PulseAudio**（`unix:/mnt/wslg/PulseServer`）转发到 Windows。
VLC 等播放器往往直接用 Pulse，而 ALSABridge 走 ALSA，需要把 ALSA 的 `default` 接到 Pulse，否则会报 `Unknown PCM default` / `cannot find card '0'`。

### 1. 安装 ALSA Pulse 插件（WSL2 额外依赖）

在完成上方「依赖安装」后，再执行：

```bash
sudo apt install -y libasound2-plugins alsa-utils
```

| 包名 | 用途 |
|------|------|
| `libasound2-plugins` | 提供 `pulse`、`default` 等 PCM，将 ALSA 接到 WSLg 的 PulseAudio |
| `alsa-utils` | `aplay` / `arecord` 等，用于检查设备列表 |

### 2. 配置 `~/.asoundrc`

将 ALSA 的默认设备指向 Pulse：

```bash
cat > ~/.asoundrc << 'EOF'
pcm.!default {
    type pulse
}
ctl.!default {
    type pulse
}
EOF
```

### 3. 设置 Pulse 服务地址（可选）

WSLg 通常已自动配置；若 `pactl info` 里 Server String 为 `unix:/mnt/wslg/PulseServer`，可写入 shell 配置：

```bash
echo 'export PULSE_SERVER=unix:/mnt/wslg/PulseServer' >> ~/.bashrc
source ~/.bashrc
```

### 4. 验证

```bash
# Pulse 是否在运行
pactl info

# 应能看到 pulse、default（而不只有 null）
aplay -L
arecord -L

# 列出设备
python3 alsabridge.py -i
python3 alsabridge.py -o
```

`aplay -L` / `arecord -L` 正常时大致类似：

```
pulse
    PulseAudio Sound Server
default
```

也可显式指定 Pulse 设备：

```bash
python3 alsabridge.py -c -f capture.wav -d pulse
python3 alsabridge.py -p -f capture.wav -d pulse
```

### 5. 录音说明

WSLg 下默认麦克风常为 **RDPSource**（Windows 侧输入），配置完成后录音与播放均经 Pulse 与 Windows 音频互通。

## 编译

在项目根目录执行：

```bash
# 生成 libalsadevice.so（供 Python 加载）
make buildso
```

修改 `alsadevice.cpp` / `alsadevice.h` 后必须重新 `make buildso`，否则 Python 可能报 `queryAlsaDeviceHwParams not exported` 等符号缺失。

## Python 使用

### 命令行

```bash
# 列出录音设备
python3 alsabridge.py -i

# 列出播放设备
python3 alsabridge.py -o

# 列出设备并查询硬件支持的声道 / 采样率 / 格式（需匹配后才能直接用 hw:）
python3 alsabridge.py -o --verbose
python3 alsabridge.py -i --verbose

# 录音到 WAV（默认 cap.wav，16 kHz、双声道、16 bit）
python3 alsabridge.py -c -f cap.wav -s 16000 -n 2 -b 16

# 播放 WAV（采样率、声道、位深从文件头读取，无需再传 -s -n -b）
python3 alsabridge.py -p -f cap.wav

# 指定硬件设备播放（格式须为该设备原生支持；否则可用 plughw: 做转换）
python3 alsabridge.py -p -d "hw:Device_1,0" -f cap.wav
python3 alsabridge.py -p -d "plughw:Device_1,0" -f cap.wav

# hw 低延迟仍 underrun 时指定预填（省略 --prefill-ms 则走自动策略）
python3 alsabridge.py -p -d "hw:Device_1,0" -f cap.wav --prefill-ms 100

# WSL2 上指定 Pulse 设备播放（需先完成上文「WSL2 音频配置」）
python3 alsabridge.py -d pulse -p -f cap.wav
```

`--verbose` 输出示例：

```text
AudioDevice(card_id="default", device_name="default", device_id="default")
  resolves: hw:rockchipes8388,0
  card    : hw:2 (rockchip-es8388)
  type    : HW
  channels: 2
  rates   : 8000-48000
  formats : S16_LE, S32_LE
AudioDevice(card_id="hw:4", device_name="USB Composite Device, USB Audio", device_id="hw:Device_1,0")
  resolves: hw:Device_1,0
  card    : hw:4 (USB Composite Device)
  type    : HW
  channels: 2
  rates   : 8000-48000
  formats : S16_LE, S32_LE
```

`resolves` 表示该 `device_id` 打开后实际落到的硬件 PCM；若经 Pulse 等逻辑插件且拿不到声卡号，会显示 `logical, no hw card`。

### 播放示例（WSL2）

先录音生成 `cap.wav`（或使用已有 WAV），再播放，运行后按提示按 Enter 开始；播完会自动停止并打印总时长。输出如下：

![WSL2 播放 cap.wav 终端输出](images/wsl2_alsa_play.png)

常用参数：

| 参数 | 含义 |
|------|------|
| `-d` / `--device-id` | 设备 ID，默认 `default` |
| `-f` / `--file` | WAV 文件路径，默认 `cap.wav` |
| `-v` / `--volume` | 音量 0–100。播放：默认软件增益（只影响本路 PCM，不改系统混音器）；录音：按 `-d` 解析到的声卡设置 ALSA mixer |
| `--verbose` | 仅长选项；与 `-i` / `-o` 联用时查询并打印各设备解析结果与硬件参数（resolves/card/type/声道/采样率/格式） |

仅 **播放**（`-p`）时有效：

| 参数 | 含义 |
|------|------|
| `--prefill-ms` | 播放前预填毫秒数；省略则自动（IOPLUG 整 buffer，HW 低延迟）。`hw:` 仍 underrun 时可试 `100` |

`hw:` 直连硬件，参数必须原生支持；格式不匹配时可改用 `plughw:`（ALSA 自动转换，略有开销）。

音量说明：

| 场景 | 行为 |
|------|------|
| 播放 `-v 50` | 软件音量（缩放 PCM），不改系统音量 |
| 播放模块 `set_volume(50, card_id='hw:4')` | 改指定卡的 ALSA mixer |
| 录音 `-v 50 -d hw:Device_1,0` | 解析设备所在卡后设置该卡 capture mixer |

仅 **录音**（`-c`）时有效（播放时从 WAV 读取，传入会被忽略并提示）：

| 参数 | 含义 |
|------|------|
| `-s` / `--sample-rate` | 采样率，默认 16000 |
| `-n` / `--channels` | 声道数，默认 2 |
| `-b` / `--bits-per-sample` | 位深，默认 16 |

### 作为模块导入

#### 通用约定

- 生命周期顺序：**`open` →（可选 `set_period`）→ `set_params` → 注册回调 → `start` → `stop` → `close` → `release`**
- 推荐用 **`with AlsaCaptureDevice() as dev:`** / **`with AlsaPlaybackDevice() as dev:`**；退出时自动 `close` + `release`
- **`set_params` / `set_period` / 回调注册 / `set_prefill_ms` 须在 `start()` 之前**；`start()` 之后不可再改
- 回调在 **C++ 工作线程**执行，勿在回调里阻塞过久或调用 `stop()`（`async_stop` 的 `on_playback_stopped` 同理）
- **`set_prefill_ms()`** 仅对当前一次 `open`…`close` 有效；`close()` 后清除，下次播放需重新决定是否调用

#### 录音流程

```python
import wave
from alsabridge import AlsaCaptureDevice, CaptureCallback, query_device_hw_params, mixer_card_from_hw_params

class MyCapture(CaptureCallback):
    def __init__(self, wav_path: str, rate: int, ch: int, bps: int):
        self._wf = wave.open(wav_path, 'wb')
        self._wf.setnchannels(ch)
        self._wf.setsampwidth(bps // 8)
        self._wf.setframerate(rate)

    def on_capture_output_data(self, cache_time_ms: int, samples: bytes, samples_count: int) -> None:
        # cache_time_ms: 驱动缓冲里尚未读走的音频时长（毫秒）
        self._wf.writeframes(samples)

    def close(self):
        self._wf.close()

device_id = 'hw:Device_1,0'   # 或 default / plughw:... / pulse
rate, ch, bps = 16000, 1, 16

cb = MyCapture('out.wav', rate, ch, bps)
with AlsaCaptureDevice() as dev:
    dev.set_capture_callback(cb)
    if not dev.open(device_id):
        raise RuntimeError('open failed')
    dev.set_period(period_count=10, period_time=20)   # 可选；默认 10×20 ms
    if not dev.set_params(rate, ch, bps):
        raise RuntimeError('set_params failed')
    # 可选：按设备卡设置 capture mixer 音量
    hw = query_device_hw_params(device_id, is_capture=True)
    card = mixer_card_from_hw_params(hw, device_id)
    dev.set_volume(80, card_id=card)
    if not dev.start():          # 内部 prepare + start + 建线程；之后 readi 循环
        raise RuntimeError('start failed')
    input('recording… press Enter to stop\n')
    dev.stop()                   # 停止线程并 drop PCM
cb.close()
```

#### 播放流程

```python
import wave
from alsabridge import AlsaPlaybackDevice, PlaybackCallback

class WavPlayback(PlaybackCallback):
    def __init__(self, wav_path: str):
        self._wf = wave.open(wav_path, 'rb')
        self.rate = self._wf.getframerate()
        self.ch = self._wf.getnchannels()
        self.bps = self._wf.getsampwidth() * 8

    def on_playback_input_data(self, cache_time_ms: int, samples_count: int) -> bytes:
        # 返回 samples_count 帧 PCM；不足会自动补零；返回 b'' 表示无更多数据
        return self._wf.readframes(samples_count)

    def on_playback_stopped(self) -> None:
        pass   # 仅 async_stop 完成后调用；勿在此调用 stop()

    def close(self):
        self._wf.close()

device_id = 'hw:Device_1,0'
cb = WavPlayback('in.wav')

with AlsaPlaybackDevice() as dev:
    dev.set_playback_callback(cb)
    if not dev.open(device_id):
        raise RuntimeError('open failed')
    dev.set_min_cache_period_count(2)                 # 可选：缓冲过低时自动喂静音
    dev.set_period(period_count=10, period_time=20)   # 可选
    if not dev.set_params(cb.rate, cb.ch, cb.bps):
        raise RuntimeError('set_params failed')
    dev.set_volume(80)                                # 默认软件音量；card_id='hw:4' 改 mixer
    # 可选：hw 低延迟默认仍 underrun 时，可试 dev.set_prefill_ms(100)（IOPLUG/default 已整 buffer 预填，一般不必再设）
    if not dev.start():                               # 立即返回；工作线程 prepare + writei
        raise RuntimeError('start failed')
    input('playing… press Enter to stop\n')
    dev.sync_stop()   # 播完缓冲再停；要立即停用 dev.stop()
cb.close()
```

#### 播放停止方式

| 方法 | 行为 |
|------|------|
| `stop()` | 立刻停止，丢弃 ALSA 内未播完的缓冲 |
| `sync_stop()` | 阻塞直到缓冲播完 |
| `async_stop()` | 立即返回；播完后在工作线程调用 `on_playback_stopped` |

#### 播放预填（`set_prefill_ms`）

可选，须在 `start()` 前调用；**`close()` 后清除**，每次 `open` 后单独决定：

| 调用 | 行为 |
|------|------|
| 不调用 | 自动：`IOPLUG`（如 `default`）整 buffer 预填（约 200 ms）；`HW` 低延迟 |
| `set_prefill_ms(0)` | 强制低延迟（不预填，alsa 默认 `start_threshold`） |
| `set_prefill_ms(100)` | 预填约 100 ms（不超过 buffer 容量） |

**何时需要手动设置：** 主要是 **`hw:` / `plughw:` 等低延迟路径**仍 underrun 时，可逐步加大（如 80、100 ms）。`default`（IOPLUG）未调用时已整 buffer 预填，再设 `100` 反而会减小预填。

```python
with AlsaPlaybackDevice() as dev:
    dev.open('hw:Device,0')   # 直连硬件；低延迟默认仍 xrun 时再调 prefill
    dev.set_params(16000, 1, 16)
    dev.set_prefill_ms(100)   # 仅本次 open；close 后需重设
    dev.start()
```

#### 枚举与硬件参数

```python
from alsabridge import (
    get_capture_devices,
    get_playback_devices,
    query_device_hw_params,
)

for dev in get_playback_devices():
    print(dev)
    print(query_device_hw_params(dev.device_id, is_capture=False))
```

音量：

```python
# 播放：默认软件增益（不改系统 mixer）
dev.set_volume(50)
dev.set_volume(50, card_id='hw:4')   # 改指定卡 ALSA mixer

# 录音：仅 mixer（需传 card_id）
dev.set_volume(50, card_id='hw:2')
```
