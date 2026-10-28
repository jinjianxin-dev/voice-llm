# 直接烧录（无需安装 ESP-IDF）

本目录是 **v2.0（三器官版：耳朵 + 大脑 + 嘴巴）** 的全套固件，
下载仓库后不需要编译，用 [esptool](https://pypi.org/project/esptool/) 一条命令烧进去即可。

## 硬件

- ESP32-S3 开发板（16 MB Flash，本机为内置 8 MB PSRAM 的板子）
- INMP441 麦克风（I2S0：WS=6 / SCK=5 / SD=7）
- MAX98357 功放 + 喇叭（I2S1：BCLK=15 / LRC=16 / DIN=18）
- **插开发板上标着 `USB` 的那个口**（不是 `UART` 口）

## 烧录

```
pip install esptool

python -m esptool --chip esp32s3 --port COM? --baud 921600 write_flash ^
  --flash_mode dio --flash_freq 80m --flash_size 16MB ^
  0x0        bootloader.bin ^
  0x8000     partition-table.bin ^
  0xe000     ota_data_initial.bin ^
  0x10000    voice_llm.bin ^
  0x5f0000   srmodels.bin
```

（Linux/macOS 把 `^` 换成 `\`，`COM?` 换成 `/dev/ttyACM0` 之类）

端口号在设备管理器里看；板子可能会在 COM7/COM8 之间变号，重新插拔即可。

## 注意：必须烧全 5 个文件

尤其是 `srmodels.bin`（唤醒词 + 命令词模型）和分区表——
如果板子之前刷过 Arduino 的麦克风/喇叭测试工程，分区表已被覆盖，
只烧 app 会报 `no 'llmmodel' partition` 起不来。

## 使用

1. 打开串口监视器（115200），按一下板上 RST 复位（先开监视器再复位，否则开机日志会丢）
2. 说 **「你好小智」** → 它回「我在」
3. 等它说完，再说命令：`打开八号脚` / `关闭三号脚` / `让台灯闪烁，每五百毫秒，闪三次` / `跑马灯` / `把所有灯都关掉`
4. 串口也可以敲字（不开麦验证）：`vt` 试发音，`vm` 看嘴巴计数，直接打命令如 `打开8号脚` 会边做边说

## 版本对照

| 标签 | 内容 | 说明 |
|------|------|------|
| v1.0 | 耳朵 + 大脑 | 语音识别 + LLM 控制引脚，无声音回复 |
| v2.0 | 三器官完整 | 加喇叭回复（23 段中文语音片段，半双工防自闻） |
