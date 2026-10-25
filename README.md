# voice_llm -- 离线语音控制 GPIO，全部跑在 ESP32-S3 上

INMP441 麦克风 → ESP-SR（WakeNet「你好小智」+ MultiNet7 中文命令词）→ 英文文本
→ 原有的 312K 参数 pin-LLM → 解析 → GPIO 动作。**全程离线，不经过电脑。**

## 对着板子说

| 你说 | 板子执行 |
|---|---|
| （先唤醒）**你好小智** | 进入聆听状态，8 秒内说命令 |
| 打开四号引脚 / 关闭四号引脚 | pin 4 拉高 / 拉低 |
| 打开X号引脚 / 关闭X号引脚（X=全部25个可用引脚） | 同上 |
| 打开台灯 / 关闭台灯 / 闪烁台灯 | 走 LLM 的别名（alias）路径，台灯=pin 4 |
| 流水灯 | chase pins 4 5 6，每 500ms |
| 全部关闭 | 同时关掉 pin 4 5 6 |

唤醒后是**连续识别**：一条命令执行完可以继续说下一条，8 秒没说话自动回到
待唤醒状态。

串口打字依然可用（115200，原 espcontrol 的所有命令，包括 `alias xxx = 4`）。

## 接线（与 mic_test 相同）

| INMP441 | ESP32-S3 |
|---|---|
| VDD | 3V3 |
| GND | GND |
| SCK | GPIO 5 |
| WS | GPIO 6 |
| SD | GPIO 7 |
| L/R | GND（左声道） |

## 分区表（16 MB Flash）

| 分区 | 偏移 | 大小 | 内容 |
|---|---|---|---|
| app | 0x10000 | 4 MB | 本工程固件（esp-sr + LLM 运行时） |
| llmmodel | 0x410000 | ~1.9 MB | **LLM 权重 model.bin**（与 espcontrol 同偏移/大小/subtype，直接复用） |
| model | 0x5f0000 | 6 MB | WakeNet9 + MultiNet7 模型。**必须叫 "model"**——esp-sr 的构建脚本把 srmodels.bin 烧到这个名字的分区 |

注意两个 0x40（data）分区并存：LLM 运行时按名字 "llmmodel" 查找，
esp-sr 按名字 "model" 查找，互不干扰。

## 怎么用（双击就行，不用敲命令）

工程根目录有 4 个 `.bat`，**双击即用**，不需要开 PowerShell、不需要敲任何命令：

| 双击 | 作用 | 什么时候用 |
|---|---|---|
| `1-build.bat` | 编译固件 | 改过 `main/` 里的代码之后 |
| `2-flash-all.bat` | 烧录（固件 + 语音模型 + LLM 权重） | 第一次，或模型/分区变了 |
| `3-monitor.bat` | 打开串口日志 | 想看板子在想什么 |
| `idf-shell.bat` | 开一个"已装好环境"的窗口 | 想自己敲 idf.py 命令时 |

**为什么以前双击 `.ps1` 没反应**：Windows 默认把 `.ps1` 当文本用记事本打开，
而且 PowerShell 的默认执行策略会拦住脚本。`.bat` 里用
`-NoProfile -ExecutionPolicy Bypass` 把这些门槛都绕过去了。

**为什么直接敲 `idf.py` 找不到**：`idf.py` 只存在于"导入过 ESP-IDF 环境"的
窗口里（`export.ps1` 把工具链加进 PATH）。新开的窗口是干净的，自然找不到。
`idf-shell.bat` 就是帮你打开一个已经导入好环境的窗口；在里面可以随便敲：

```
idf.py build
idf.py -p COM7 flash
idf.py -p COM7 monitor        # 退出用 Ctrl+]
idf.py -p COM7 flash monitor  # 烧完直接看日志
```

## 万一还是失败

- **烧录时报找不到 COM7**：板子要插标着 **USB** 的那个口（不是 UART），
  设备管理器里应显示"USB 串行设备 (COM7)"。
- **卡在 `Connecting...`**：按住 **BOOT** 键，点一下 **RST**，再松开 BOOT，重试。
- **提示 build\voice_llm.bin missing**：先跑 `1-build.bat`。
- **日志里中文/英文乱码**：`3-monitor.bat` 已设 UTF-8；如果还是乱，
  换用 `idf-shell.bat` 里的 `idf.py monitor`。

## 代码结构（main/）

- `main.c` -- I2S 收音、AFE、唤醒词、命令词注册（中文短语 → 英文命令文本映射表）、串口 REPL
- `llm_runtime.c/.h` -- espcontrol.ino 的移植：模型 mmap、BPE、生成、别名（NVS 持久化）
- `command.h / symbols.h / tokenizer.h / bpe.h / llm.h / alias.h / gpio_control.c` -- 从原固件 `firmware/common` + `firmware/generated` 原样拷贝，未改动
