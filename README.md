# voice_llm -- 会说中文的离线语音点灯助手，全部跑在 ESP32-S3 上

耳朵（INMP441 麦克风）→ 大脑（ESP-SR 唤醒/命令词 + 312K 参数 pin-LLM）→
嘴巴（MAX98357 + 喇叭）。**全程离线，不经过电脑、不联网。**

> **不想编译？** `prebuilt/` 目录里有整套固件 + 一条命令的烧录说明，
> 见 [prebuilt/FLASH.md](prebuilt/FLASH.md)。版本进度看 git 标签：
> `v1.0` = 耳朵+大脑，`v2.0` = 三器官完整版。

```
🎤 INMP441 ──I2S0 RX──► AFE(VAD+WakeNet) ──► MultiNet7 中文命令词
                                                   │
                                          中文句子 ─┘
                                                   ▼
                                          312K pin-LLM（flash 里 mmap）
                                                   ▼
                                        Command ▸ 别名 ▸ 安全门 ▸ GPIO
                                                   │
                                     动作结果 ─────┴─────► 🔊 MAX98357 说话
```

## 对着板子说

| 你说 | 板子执行 | 板子回答 |
|---|---|---|
| **你好小智**（唤醒） | 进入聆听，8 秒内说命令 | 「我在」 |
| 打开八号引脚 / 关闭八号引脚 | pin 8 拉高 / 拉低 | 「八号脚已打开 / 已关闭」 |
| 打开台灯 / 关闭台灯 / 闪烁台灯 | 走 LLM 别名路径（台灯 = pin 1） | 「一号脚已打开」等 |
| 闪烁八号引脚 | 闪 3 次（固定 500ms×3） | 「八号脚开始闪烁」 |
| 流水灯 | 1/2/8 号脚跑马灯 | 「跑马灯启动」 |
| 全部关闭 / 停止 | 全部引脚拉低 / 停动画 | 「已全部关闭」/「已停止」 |
| 说了它不认识的话 | 拒绝执行（不动任何引脚） | 「没听懂」 |
| 说了板子上没有的引脚 | 拒绝执行 | 「这个引脚不能用」 |

**说话的节奏很重要**：唤醒后先听它说完「我在」（约 0.5 秒），再说命令。
说唱机是**半双工**的——它说话时耳朵是关着的（详见下面「为什么要关耳朵」）。

唤醒后是**连续识别**：一条命令执行完可以继续说下一条，8 秒没说话自动回到待唤醒状态。

串口打字依然可用（115200，原 espcontrol 的所有命令，包括 `alias xxx = 4`），
而且**打字的命令也会说话**——这是不用开口就能验证回复措辞的办法：

| 串口输入 | 作用 |
|---|---|
| `vt` / `vt 21` | 直接让嘴巴念「8 号脚已打开」/「21 号脚已打开」 |
| `vm` | 嘴巴的计数器（已写字节、错误数、队列长度） |
| `打开8号脚` | 正常跑一遍大模型并执行，然后说出来 |

## 接线

| INMP441（耳朵） | ESP32-S3 | | MAX98357（嘴巴） | ESP32-S3 |
|---|---|---|---|---|
| VDD | 3V3 | | VIN | 3V3（或 5V） |
| GND | GND | | GND | GND |
| SCK | GPIO 5 | | BCLK | GPIO 15 |
| WS | GPIO 6 | | LRC | GPIO 16 |
| SD | GPIO 7 | | DIN | GPIO 18 |
| L/R | GND（左声道） | | GAIN / SD | 悬空 |

两路 I2S 互不干扰：麦克风用 I2S_NUM_0（收），喇叭用 I2S_NUM_1（发）。

## 嘴巴是怎么发音的

312K 的模型只会吐动作符号（`<set> <pin> 8 <high>`），不会造句，所以回复不可能
由模型现编。做法是把**词**录成音频片段烧进固件，运行时按结果拼接：

- 23 个片段：`零`…`十`、`号脚`、`已打开`、`已关闭`、`开始闪烁`、`跑马灯启动`、
  `已全部关闭` 等，共 11.9 秒、558 KB，用 `EMBED_FILES` 编进 app（占 flash 不占 RAM）
- 数字用中文拼：三十八 = `三` + `十` + `八`，所以 11 个数字片段就能覆盖全部 25 个引脚
- 音色来自 edge-tts 的 `zh-CN-XiaoxiaoNeural`，语速 +15%，每段归一化响度，
  段间插 40ms 静音

重新生成词库（需要联网，只跑一次）：

```bat
E:\esp32-tools\tts-venv\Scripts\python.exe tools\gen_voice_clips.py
```

改完要重新 `1-build.bat`（片段是编进固件的）。音频缓存放在
`E:\esp32-tools\tts-cache`，所以第二次跑不用联网。

> 想换成"能说任意中文"的方案（板载 TTS）？代价是 ~1.5 MB 语音模型、一个新分区
> 和每句几百毫秒的合成时间。当前这套的优点是：零依赖、开口即响、绝不会合成失败。

## 为什么要关耳朵（半双工）

喇叭和麦克风相距 20 cm，AFE 是单麦模式（没有回声参考通道）。如果它边说话边听，
就会听见自己说的「打开八号脚」，MultiNet 匹配成功，灯被反复开关——一个死循环。

所以 `mouth.c` 维护一个"嘴巴占用"状态：播放期间 + 结束后 300ms（等房间回声消散），
`main.c` 的识别任务完全不喂 MultiNet，并在状态切换时 `clean()` 一次，避免半截话残留。
日志里能看到这个时间窗：

```
>> heard: da kai ba yin jiao  (prob 0.86)
>> pin 8 high   (382ms)
[ear muted: speaking]
[ear live]
```

## 分区表（16 MB Flash）

| 分区 | 偏移 | 大小 | 内容 |
|---|---|---|---|
| app | 0x10000 | 4 MB | 本工程固件（esp-sr + LLM 运行时 + 558 KB 音频片段），当前用 1.6 MB |
| llmmodel | 0x410000 | ~1.9 MB | **LLM 权重 model.bin**（与 espcontrol 同偏移/大小/subtype，直接复用） |
| model | 0x5f0000 | 6 MB | WakeNet9 + MultiNet7 模型。**必须叫 "model"**——esp-sr 的构建脚本把 srmodels.bin 烧到这个名字的分区 |

注意两个 0x40（data）分区并存：LLM 运行时按名字 "llmmodel" 查找，
esp-sr 按名字 "model" 查找，互不干扰。音频片段没有独立分区，就在 app 里。

## 怎么用（双击就行，不用敲命令）

| 双击 | 作用 | 什么时候用 |
|---|---|---|
| `1-build.bat` | 编译固件 | 改过 `main/` 里的代码或音频片段之后 |
| `2-flash-all.bat` | 烧录（引导 + 分区表 + 固件 + 语音模型 + LLM 权重） | 第一次，或模型/分区变了 |
| `3-monitor.bat` | 打开串口日志 | 想看板子在想什么 |
| `idf-shell.bat` | 开一个"已装好环境"的窗口 | 想自己敲 idf.py 命令时 |

**只改了 `main/` 里的 C 代码时**，烧 app 一个分区就够（快 10 倍）：

```bat
python -m esptool --chip esp32s3 --port COM8 --baud 921600 write_flash 0x10000 build\voice_llm.bin
```

## 万一还是失败

- **`no 'llmmodel' partition -- check partitions.csv`**：分区表被别的工程覆盖了。
  这块板子上跑 Arduino 的 `mic_test` / `speaker_test` 会连带刷入它们自己的分区表，
  于是 `llmmodel` / `model` 两个分区消失。解决：`2-flash-all.bat`（必须带分区表），
  只烧 app 不够。
- **烧录时报找不到 COM 口**：板子要插标着 **USB** 的那个口（不是 UART），
  COM 号每次烧完可能变（COM7 ↔ COM8），用 `VID_303A` 自动探测，别写死。
- **卡在 `Connecting...`**：按住 **BOOT**，点一下 **RST**，再松开 BOOT，重试。
- **烧完日志一片空白**：先开 `3-monitor.bat` 再按板上的 RST。
  USB-Serial/JTAG 的发送 FIFO 没人读时，开机那段日志会丢；重新复位就能看到。
- **有日志但没声音**：看开机自检那两行。
  `94 KB/s (wall clock wants 94)` + `I2S TX drains at full speed` 说明数据真的送出去了，
  这时该查 MAX98357 的 VIN/GND/GAIN 和喇叭线；如果显示 `TX is SLOW`，才是驱动/DMA 问题。
- **它回答时你插话，命令丢了**：正常。半双工，等它说完再说。

## 代码结构（main/）

- `main.c` -- I2S 收音、AFE、唤醒词、命令词注册（中文短语 → 中文句子映射表）、
  串口 REPL、**把执行结果翻译成要说的词序列**
- `mouth.c/.h` -- 嘴巴：I2S_NUM_1 发送、独立播放任务、4 深度队列、"耳朵闭麦"门控、
  开机自检（两声音 + 吞吐量实测）
- `voice_clips.c/.h` -- 解析编进固件的音频片段（`tools/gen_voice_clips.py` 生成）
- `llm_runtime.c/.h` -- espcontrol.ino 的移植：模型 mmap、BPE、生成、别名（NVS 持久化）；
  `llmrt_run_text_ex()` 额外返回 `llmrt_outcome_t`，让嘴巴知道刚才到底做了什么
- `command.h / symbols.h / tokenizer.h / bpe.h / llm.h / alias.h / gpio_control.c` --
  从原固件 `firmware/common` + `firmware/generated` 原样拷贝，未改动

## 任务划分（全部在 core 0，unicore 构建）

| 任务 | 优先级 | 栈 | 职责 |
|---|---|---|---|
| `feed` | 5 | 8 KB | I2S 收 → AFE（不能停，停了识别就丢帧） |
| `detect` | 5 | 8 KB | AFE 取 → 唤醒 → 命令词 → LLM → GPIO → 排一条回复 |
| `mouth` | 4 | 4 KB | 出队 → I2S 发（低一级：不让喇叭抢识别的 CPU） |
| `repl` | 3 | 8 KB | 串口打字 |
| `hb` | 2 | 3 KB | 开机心跳，用来区分"卡住"和"只是没输出" |

回复任务和识别任务之间只有一条 4 深的队列：`detect` 用 0 超时投递，
队列满了就丢掉这条回复并打日志——识别绝不为了喇叭等待。
