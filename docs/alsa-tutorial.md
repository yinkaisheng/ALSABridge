# ALSA 入门与本项目 API 对照

面向初学者的 ALSA（Advanced Linux Sound Architecture）系统介绍。
文中会覆盖 **ALSABridge / ALSADevice 项目实际用到的全部 ALSA API**，并解释 `hw` / `plughw`、采集独占、多应用共享采集等常见问题。

> 建议边读边在 Linux 上执行文中的命令（`aplay` / `arecord` / `amixer`）。

---

## 目录

1. [ALSA 是什么](#1-alsa-是什么)
2. [核心概念：Card / Device / PCM / Mixer](#2-核心概念card--device--pcm--mixer)
3. [设备名：default / hw / plughw / pulse](#3-设备名default--hw--plughw--pulse)
4. [采集独占与多 App 共享](#4-采集独占与多-app-共享)
5. [PCM 采播完整流程](#5-pcm-采播完整流程)
6. [本项目用到的 ALSA API 全表](#6-本项目用到的-alsa-api-全表)
7. [与 ALSABridge 的对应关系](#7-与-alsabridge-的对应关系)
8. [常见错误与排查](#8-常见错误与排查)
9. [推荐练习](#9-推荐练习)

---

## 1. ALSA 是什么

ALSA 是 Linux 内核与用户态的音频框架，大致分三层：

```text
应用 (Python / C++ / aplay)
        │
        ▼
用户态库 libasound  （snd_pcm_* / snd_ctl_* / snd_mixer_*）
        │
        ▼
内核驱动 + 声卡硬件
```

本项目：

- `alsadevice.cpp` 直接调用 `libasound`
- `alsabridge.py` 通过 ctypes 调用 `libalsadevice.so`
- 你写业务时通常只碰 Python API，但理解底层 ALSA 有助于排查设备/格式问题

安装开发包（Debian/Ubuntu）：

```bash
sudo apt install -y libasound2 libasound-dev alsa-utils
```

---

## 2. 核心概念：Card / Device / PCM / Mixer

### 2.1 声卡 Card

一块物理或虚拟声卡，编号从 0 开始：

```bash
cat /proc/asound/cards
# 0 [rockchipdp0   ]: ...
# 2 [rockchipes8388]: ...
# 4 [Device_1      ]: USB Audio
```

ALSA 里常用：

| 写法 | 含义 |
|------|------|
| `hw:0` | 第 0 号卡（控制面 / mixer 也常用） |
| `hw:Device_1` | 用卡 ID 名引用（更稳定，插拔后编号可能变） |

### 2.2 PCM Device

一张卡上可有多个 PCM 设备（播放/录音接口），常见写法：

```text
hw:CARD,DEV[,SUBDEV]
例如: hw:Device_1,0
```

- `CARD`：卡号或卡 ID
- `DEV`：设备号（常为 0）
- `SUBDEV`：子设备（多数设备只有一个，可省略）

### 2.3 PCM vs Mixer

| 子系统 | 干什么 | 典型 API |
|--------|--------|----------|
| **PCM** | 真正读写音频采样 | `snd_pcm_open` / `readi` / `writei` |
| **CTL / Mixer** | 音量、静音、通路开关 | `snd_mixer_*` / `amixer` |

注意：打开 PCM 用的名字（如 `hw:Device_1,0`）和调音量用的卡名（如 `hw:4`）相关但不相同。

### 2.4 流方向 Stream

| 常量 | 含义 |
|------|------|
| `SND_PCM_STREAM_PLAYBACK` | 播放（写数据到设备） |
| `SND_PCM_STREAM_CAPTURE` | 录音（从设备读数据） |

同一张卡可以同时有播放和录音设备，也可以只有其中一种。

---

## 3. 设备名：default / hw / plughw / pulse

这是初学者最容易混淆的部分。

### 3.1 `hw:...` —— 直连硬件

```text
hw:Device_1,0
```

特点：

- **几乎无格式转换**
- 参数必须是硬件原生支持的（声道、采样率、位深）
- 延迟通常最低、路径最短
- 多数 USB 声卡 **独占**：同一方向同时只能被一个进程打开

本项目枚举出来的设备 ID 基本都是 `hw:...`。

失败示例：

```text
WAV = 16000 Hz / 1 ch / 16 bit
设备只支持 2 ch
→ snd_pcm_hw_params_set_channels(..., 1) 返回 Invalid argument
```

### 3.2 `plughw:...` —— 硬件 + 自动转换

把前缀 `hw:` 换成 `plughw:` 即可：

```text
plughw:Device_1,0
```

`plug` 插件会在中间做：

- 声道转换（1ch → 2ch）
- 采样率转换
- 格式转换（如 S16 ↔ S32）

代价：多一层软件处理，CPU/延迟略增。
**格式不确定时优先 `plughw:`；格式确定且追求效率时用 `hw:`。**

本项目用法：

```bash
python3 alsabridge.py -p -d "plughw:Device_1,0" -f foo.wav
```

### 3.3 `default` —— 系统默认 PCM

逻辑名，不是枚举出来的硬件。由 `~/.asoundrc` / `/etc/asound.conf` / Pulse/PipeWire 插件决定最终落到哪。

本项目 Python 列表会**手动插入**一条 `default`；C++ 枚举硬件时本身没有它。
`open("default")` 合法：字符串原样交给 `snd_pcm_open`。

### 3.4 `pulse` / PipeWire

若系统装了 `libasound2-plugins`，常有：

```text
pulse
default   # 可能被配置成 type pulse
```

走 PulseAudio/PipeWire 时：

- 更容易多应用共享
- 格式兼容更好
- 不再是“直连某张 USB 卡”

WSL2 上通常必须走 Pulse（见 README「WSL2 音频配置」）。

### 3.5 如何查看系统有哪些 PCM 名

```bash
aplay -L          # 播放侧逻辑设备（含 default/pulse/plughw...）
arecord -L        # 录音侧
aplay -l          # 硬件卡列表（类似本项目枚举）
arecord -l
```

本项目：

```bash
python3 alsabridge.py -o            # 硬件 + default
python3 alsabridge.py -o --verbose  # 再查 resolves / channels / rates / formats
```

`--verbose` 里的 `resolves:` 表示该名字打开后实际落到的硬件 PCM（若能解析到）。

---

## 4. 采集独占与多 App 共享

### 4.1 为什么 `hw:` 采集经常“独占”

打开 `hw:xxx` 时，应用直接占用该 PCM 设备。
多数驱动对 **CAPTURE** 只允许一个 opener：

```text
App A: arecord -D hw:Device,0 ...
App B: 再 open 同一设备 → Device or resource busy (-16)
```

播放侧有时也独占，但部分卡/驱动允许多开或通过 dmix 共享（见下）。

### 4.2 多 App 共享播放：`dmix`

ALSA 可用 `dmix` 插件把多路播放混到同一硬件。
系统常提供类似：

```text
dmix:CARD=Device_1,DEV=0
```

或在 `~/.asoundrc` 里自定义。
本项目未封装 dmix，需要时直接把 `-d` 设成 dmix 设备名。

### 4.3 多 App 共享采集：`dsnoop`

采集共享对应插件是 **`dsnoop`**（与 dmix 对称）：

```text
多个录音程序
    │
    ▼
 dsnoop 插件（复制同一路硬件采集）
    │
    ▼
 hw:麦克风
```

示例（`~/.asoundrc` 思路）：

```text
pcm.shared_cap {
    type dsnoop
    ipc_key 12345
    slave {
        pcm "hw:Device,0"
        channels 2
        rate 48000
    }
}
```

然后：

```bash
arecord -D shared_cap -f S16_LE -r 48000 -c 2 a.wav
# 另一个进程同样用 -D shared_cap
```

注意：

- slave 的 rate/channels 要固定，后开的程序通常要匹配
- 不是所有板卡/驱动组合都好用；USB 设备上要实测

### 4.4 更常见的共享方式：PulseAudio / PipeWire

现代桌面/多数发行版推荐：

```text
App → ALSA "pulse"/"default" → Pulse/PipeWire → 硬件
```

优点：

- 多应用同时录音/播放更省心
- 自动重采样、混音
- 设备热插拔策略更完整

代价：多一层守护进程，延迟通常高于 `hw:`。

**嵌入式/唤醒词场景**若必须低延迟且独占可接受 → 用 `hw:`/`plughw:`。
**需要多进程同时听麦** → 优先 Pulse/PipeWire，或自建 `dsnoop`。

### 4.5 本项目当前行为

| 打开方式 | 共享性 |
|----------|--------|
| `-d hw:...` | 通常独占该方向 |
| `-d plughw:...` | 仍占底层硬件，只是多了格式转换，**不自动多开共享** |
| `-d default` / `pulse` | 取决于系统是否接到 Pulse/PipeWire |

`plughw` **解决的是格式兼容，不是多 App 共享。**

---

## 5. PCM 采播完整流程

PCM 状态大致为：`OPEN → SETUP → PREPARED → RUNNING → …`

```text
open → (可选 set period) → set hw_params → prepare → start（或 writei 自动 start）
  → 循环 readi/writei
  → drop 或 drain → close
```

**采集与播放在本项目中的 `start` 行为不同**（见 [5.4](#54-播放-auto-start-与-start_threshold)）。

### 5.1 播放（Playback）

```text
应用提供 PCM ──writei──► ALSA ring buffer ──► 声卡 DAC
```

关键点：

- `period`：一次中断/处理的块大小
- `buffer`：多个 period 组成的环形缓冲
- underrun（欠载）：应用喂数据太慢，缓冲空了 → 常见错误 `-EPIPE`（Broken pipe）

本项目用回调从 Python 拉数据，再在 C++ 里 `snd_pcm_writei`。

### 5.2 录音（Capture）

```text
声卡 ADC ──► ALSA ring buffer ──readi──► 应用
```

- overrun（过载）：应用读太慢，缓冲被新数据覆盖

### 5.3 Period / Buffer（本项目的 setPeriod）

本项目默认大致：

- `periodCount = 10`
- `periodTime = 20` ms

内部再换算成 period size（帧数）和 periods，通过：

- `snd_pcm_hw_params_set_periods_near`
- `snd_pcm_hw_params_set_period_size_near`

调大 period：更稳、延迟更大。
调小 period：延迟更低、更容易 underrun/overrun。

### 5.4 播放 auto-start 与 start_threshold

除 `hw_params` 外，ALSA 还有 **软件参数** `snd_pcm_sw_params`，其中 **`start_threshold`**（单位：帧）决定：

> 播放时，ring buffer 里已写入的帧数 **≥ start_threshold** 且流尚未 RUNNING 时，**自动 start**（不必调用 `snd_pcm_start`）。

| 来源 | 典型 `start_threshold` |
|------|-------------------------|
| alsa-lib 默认（只调 `hw_params`、未设 sw params） | **1**（写 1 帧就可能 auto-start） |
| `aplay` 等工具 | 常设为整 buffer 大小 |
| 希望只能手动 start | 设为 **> buffer_size** |

本项目 **未显式设置** sw params，因此多为默认值 **1**。例如 16 kHz 下 1 帧 ≈ 62 µs；`start()` 后工作线程第一次成功 `writei` 一个 period，即可 auto-start。

**读取当前值**（本项目在播放线程 `prepare` 后会打日志）：

```c
snd_pcm_sw_params_current(pcm, swparams);
snd_pcm_sw_params_get_start_threshold(swparams, &start_threshold);
snd_pcm_get_params(pcm, &buffer_size, &period_size);
```

### 5.5 本项目采播启动差异（重要）

| 步骤 | 录音 `AlsaCaptureDevice` | 播放 `AlsaPlaybackDevice` |
|------|--------------------------|---------------------------|
| `start()` 主线程 | `prepare` + **`snd_pcm_start`** + 建工作线程 | **只建工作线程**，立即返回 |
| 工作线程入口 | 直接 `wait` → `readi` | **`prepare`** → 读 sw params 日志 → `wait` → `writei` |
| 进入 RUNNING | 显式 `snd_pcm_start` | **`writei` 达到 start_threshold 后 auto-start** |
| `open` 模式 | `SND_PCM_ASYNC` | **阻塞模式**（I/O 在工作线程） |

这样设计的原因：

- 播放若在 **空缓冲** 上调用 `snd_pcm_start`，硬件立刻要数据而应用尚未 `writei`，易出现 **`snd_pcm_start error=-32 (EPIPE)`**。
- 参考 miniaudio 等库：播放侧由工作线程 `prepare` + 预填 + `writei` 驱动；`start()` 对调用方非阻塞。
- 录音侧数据来自硬件，显式 `start` 后再 `readi` 是常见且安全的做法。

```text
播放（本项目）:

  主线程: start() ──► 创建 thread，立即返回
  工作线程: prepare → writei × N → (auto-start) → 循环 writei

录音（本项目）:

  主线程: start() ──► prepare → snd_pcm_start → 创建 thread
  工作线程: wait → readi → 回调
```

`writei` / `readi` 遇 xrun 时，本项目会调用 `snd_pcm_recover` 并继续（播放写路径会重试一次 `writei`）。

---

## 6. 本项目用到的 ALSA API 全表

按功能分组。标注 **本项目文件位置**，便于对照源码。

### 6.1 错误与通用

| API | 作用 | 本项目 |
|-----|------|--------|
| `snd_strerror(err)` | 错误码转文字 | 各处失败日志 |
| `SND_LIB_VERSION_STR` | ALSA 库版本字符串 | `open` 日志 |

### 6.2 枚举声卡与 PCM 设备

| API | 作用 | 本项目 |
|-----|------|--------|
| `snd_card_next` | 遍历卡号 | `enumerateAlsaSoundcards` |
| `snd_ctl_open` / `snd_ctl_close` | 打开/关闭控制接口 | 枚举、resolve |
| `snd_ctl_card_info` | 读卡名/卡 ID | 枚举 |
| `snd_ctl_card_info_get_id` / `get_name` | 取卡 ID、可读名称 | 枚举、verbose |
| `snd_ctl_pcm_next_device` | 遍历卡上 PCM 设备号 | 枚举 |
| `snd_pcm_info_alloca` | 栈上分配 pcm info | 枚举、resolve |
| `snd_pcm_info_set_device` / `set_subdevice` / `set_stream` | 指定查哪个 PCM | 枚举 |
| `snd_ctl_pcm_info` | 查询该 PCM 是否存在及信息 | 枚举 |
| `snd_pcm_info_get_name` / `get_subdevice_name` / `get_subdevices_count` | 设备名、子设备 | 枚举 |
| `snd_pcm_info` | 已打开 PCM 的信息 | `--verbose` resolve |
| `snd_pcm_info_get_card` / `get_device` / `get_subdevice` / `get_id` | 解析落到哪张卡 | `--verbose` |
| `snd_pcm_name` / `snd_pcm_type` / `snd_pcm_type_name` | PCM 名与类型（HW/PLUG…） | `--verbose` |
| `snd_device_name_hint` / `get_hint` / `free_hint` | 列出逻辑 PCM（含 default） | `pcmList`（辅助） |
| `snd_pcm_stream_name` | 流类型名字符串 | `pcmList` |

### 6.3 打开 / 关闭 PCM

| API | 作用 | 本项目 |
|-----|------|--------|
| `snd_pcm_open` | 打开 PCM | `open`、`queryAlsaDeviceHwParams` |
| `snd_pcm_close` | 关闭 PCM | `close`、query 结束 |
| `SND_PCM_STREAM_PLAYBACK` / `CAPTURE` | 方向 | open / query |
| `SND_PCM_ASYNC` | 异步模式标志 | **仅录音** `open` |
| `SND_PCM_NONBLOCK` | 非阻塞打开 | query / ctl |

`deviceId` 可以是任意合法 PCM 名：`default`、`hw:...`、`plughw:...`、`pulse` 等。

### 6.4 硬件参数 hw_params

| API | 作用 | 本项目 |
|-----|------|--------|
| `snd_pcm_hw_params_alloca` | 分配参数对象 | setParams / query |
| `snd_pcm_hw_params_any` | 填入设备支持的全集 | setParams / query |
| `snd_pcm_hw_params_set_access` | 访问方式 | `RW_INTERLEAVED` |
| `snd_pcm_hw_params_set_format` | 采样格式 | S8/S16_LE/S24_LE/S32_LE |
| `snd_pcm_hw_params_set_channels` | 声道数 | setParams |
| `snd_pcm_hw_params_set_rate_near` | 采样率（就近） | setParams |
| `snd_pcm_hw_params_set_periods_near` | period 个数 | setParams |
| `snd_pcm_hw_params_set_period_size_near` | 每 period 帧数 | setParams |
| `snd_pcm_hw_params` | **提交**参数到驱动 | setParams |
| `snd_pcm_hw_params_get_channels_min/max` | 查询声道范围 | `--verbose` |
| `snd_pcm_hw_params_get_rate_min/max` | 查询采样率范围 | `--verbose` |
| `snd_pcm_hw_params_test_format` | 探测某格式是否支持 | `--verbose` |
| `snd_pcm_format_name` | 格式名字符串 | verbose / 日志 |
| `snd_pcm_hw_params_get_access/format/subformat/...` | 读回已配置参数 | `getParams` 日志 |
| `snd_pcm_hw_params_get_channels/rate/periods/period_time/period_size/buffer_size` | 读回缓冲布局 | `getParams` |
| `snd_pcm_access_name` / `snd_pcm_state_name` / `snd_pcm_subformat_name` | 调试字符串 | `getParams` |

### 6.4.1 软件参数 sw_params（播放 auto-start）

| API | 作用 | 本项目 |
|-----|------|--------|
| `snd_pcm_sw_params_alloca` | 分配 sw 参数对象 | 播放线程日志 |
| `snd_pcm_sw_params_current` | 读当前 sw 参数 | 播放 `run()` |
| `snd_pcm_sw_params_get_start_threshold` | 自动 start 阈值（帧） | 播放 `run()` 日志 |
| `snd_pcm_get_params` | 读 buffer/period 大小 | 播放 `run()` 日志 |
| `snd_pcm_sw_params_set_start_threshold` | 设置 auto-start 阈值 | **未使用**（alsa-lib 默认） |
| `snd_pcm_sw_params` | 提交 sw 参数 | **未使用** |

常用格式常量（本项目）：

- `SND_PCM_FORMAT_S8`
- `SND_PCM_FORMAT_S16_LE`
- `SND_PCM_FORMAT_S24_LE`
- `SND_PCM_FORMAT_S32_LE`
- （探测列表还含 `U8` / `S16_BE` / `S24_3LE` / `FLOAT_LE`）

访问方式：

- `SND_PCM_ACCESS_RW_INTERLEAVED`：交错存储（L R L R …），本项目使用

### 6.5 运行控制与数据读写

| API | 作用 | 本项目 |
|-----|------|--------|
| `snd_pcm_prepare` | 进入准备状态 | 录音 `start`；**播放工作线程** `run()` |
| `snd_pcm_start` | 开始传输 | **仅录音** `start` |
| `snd_pcm_readi` | 交错读（录音） | capture `handleData` |
| `snd_pcm_writei` | 交错写（播放）；可触发 auto-start | playback `handleData` |
| `snd_pcm_wait` | 等待可读/可写 | 采播工作线程 `run()` |
| `snd_pcm_avail_update` | 更新并返回 avail | 采播工作线程 `run()` |
| `snd_pcm_delay` | 缓冲延迟（帧） | 播放侧 cache 时间 |
| `snd_pcm_drop` | 立刻丢弃缓冲并停 | `stop` |
| `snd_pcm_drain` | 播完缓冲再停 | `syncStop` / async 收尾 |
| `snd_pcm_pause` | 暂停/恢复 | `pause` / `resume` |
| `snd_pcm_recover` | 从 xrun 等错误恢复 | 读写错误处理路径 |
| `snd_pcm_state` | 当前状态 | 日志 |

### 6.6 Mixer 音量

| API | 作用 | 本项目 |
|-----|------|--------|
| `snd_mixer_open` / `close` | 打开混音器 | `getOrSetVolume` |
| `snd_mixer_attach` | 挂到某张卡（如 `hw:4`、`default`） | 同上 |
| `snd_mixer_selem_register` / `snd_mixer_load` | 加载 simple mixer 元素 | 同上 |
| `snd_mixer_first_elem` / `elem_next` | 遍历控件 | 同上 |
| `snd_mixer_selem_get_id` / `id_get_name` | 控件名（Master/PCM…） | 同上 |
| `snd_mixer_selem_is_active` | 是否有效 | 同上 |
| `snd_mixer_selem_has_*_volume` / `has_*_channel` | 能力探测 | 同上 |
| `snd_mixer_selem_get_*_volume_range` | 音量范围 | 同上 |
| `snd_mixer_selem_get_*_volume` / `set_*_volume` | 读/写音量 | 同上 |

本项目音量策略（Python）：

| 场景 | 行为 |
|------|------|
| 播放 `-v 50` / `set_volume(50)` | **软件缩放 PCM**（不改系统 mixer） |
| 播放 `set_volume(50, card_id='hw:4')` | 改该卡 ALSA mixer |
| 录音 `-v 50` | 按设备解析到的卡设置 **capture mixer** |

---

## 7. 与 ALSABridge 的对应关系

```text
python3 alsabridge.py -o --verbose
        │
        ▼
enumerateAlsaPlaybackDevices
  → snd_card_next / snd_ctl_* / snd_pcm_info_*
        │
        ▼
queryAlsaDeviceHwParams("hw:...", 0, ...)
  → snd_pcm_open → snd_pcm_info(resolve)
  → hw_params_any → get channels/rates + test_format
        │
        ▼
播放: AlsaPlaybackDevice.open/set_params/start
  → snd_pcm_open(阻塞) / hw_params_*
  → start: 仅建 thread
  → thread: prepare / sw_params 日志 / wait / writei (auto-start)
录音: AlsaCaptureDevice.open/set_params/start
  → snd_pcm_open(ASYNC) / hw_params_*
  → start: prepare + snd_pcm_start + thread
  → thread: wait / readi
```

常用命令对照：

| 你想做的事 | 命令 / API |
|------------|------------|
| 看有哪些播放设备 | `alsabridge.py -o` 或 `aplay -l` |
| 看硬件支持格式 | `alsabridge.py -o --verbose` |
| 格式可能不匹配 | `-d "plughw:..."` |
| 跟系统默认走 | `-d default` 或 `-d pulse` |
| 播放 WAV | `-p -f xx.wav -d ...` |
| 录音 | `-c -f xx.wav -s 16000 -n 2 -b 16` |
| 本路播放音量 | `-v 50`（软件） |
| 改某卡 mixer | Python `set_volume(50, card_id='hw:N')` |

---

## 8. 常见错误与排查

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| `snd_pcm_start error=-32 (EPIPE)` 播放 | 空缓冲上显式 `start` 导致立刻 underrun | 本项目已改为播放 thread 内 `prepare`+`writei` auto-start；勿在空缓冲 `start` |
| `Invalid argument` at set_channels/rate | `hw:` 不支持该格式 | 换匹配格式，或改 `plughw:` |
| `Device or resource busy` | 设备被占用（独占） | 关掉占用进程；或走 pulse/dsnoop |
| `Unknown PCM default` | 未配置 default/pulse | 配 `~/.asoundrc` 或装 plugins |
| 有声但很小/很大 | mixer 与软件音量搞混 | 播放默认是软件音量；查 `amixer -c N` |
| `--verbose` 显示 unavailable | 设备忙或无权打开 | 先停占用；看权限（audio 组） |
| WSL 无硬件卡 | 正常 | 走 Pulse/`default` |

命令行自查：

```bash
# 谁占用了声卡（示例）
fuser -v /dev/snd/*

# 看某硬件能力（需 alsa-utils）
aplay -D hw:Device_1,0 --dump-hw-params /dev/zero

# mixer
amixer -c 4
amixer -c 4 sset Master 50%
```

---

## 9. 推荐练习

1. **枚举对比**
   `aplay -l` vs `python3 alsabridge.py -o --verbose`，对照 `resolves` / `card`。

2. **hw vs plughw**
   用单声道 16 kHz WAV：
   `-d hw:...` 应失败或异常；`-d plughw:...` 应成功。

3. **独占**
   终端 A：`arecord -D hw:你的麦,0 -f cd /tmp/a.wav`
   终端 B：再 `arecord` 同一设备 → 观察 busy。

4. **共享（若系统有 Pulse）**
   两个进程同时 `arecord -D pulse`（或 `default`），对比 `hw:` 行为。

5. **音量**
   播放加 `-v 20`，确认响度变小且系统总音量不变；再用 `amixer` 改卡音量对比。

---

## 附录 A：名词速查

| 名词 | 一句话 |
|------|--------|
| PCM | 脉冲编码调制；这里也指 ALSA 的音频流接口 |
| frame | 所有声道在同一采样点的一组样本 |
| period | 驱动与应用交互的一块帧数 |
| xrun | underrun/overrun 统称 |
| interleaved | LRLR… 交错存放 |
| plug | 格式转换插件 |
| dmix | 播放混音共享 |
| dsnoop | 采集分发共享 |
| simple mixer (selem) | 高层音量控件抽象 |

## 附录 B：进一步阅读

- ALSA 官方：https://www.alsa-project.org/
- `man aplay` / `man arecord` / `man amixer`
- 本仓库：`README.md`（命令行用法）、`alsadevice.h`（C 导出 API 注释）

