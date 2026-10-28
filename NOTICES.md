# 第三方来源

## 微雪屏幕驱动

`firmware/components/epaper/epaper_port.c` 与 `.h` 基于微雪官方仓库：

- 仓库：<https://github.com/waveshareteam/ESP32-S3-ePaper-3.97>
- 参考提交：`9b12d40731a80213b927ee8a421cae4082952819`
- 原始路径：`ESP-IDF/01_E-Paper_Example/components/epaper_port/`
- 板卡文档：<https://docs.waveshare.com/ESP32-S3-ePaper-3.97/Arduino>

本工程移除未使用的示例绘图/图片头文件依赖，补齐标准整数、FreeRTOS、计时器头文件，为BUSY等待增加超时状态，并将复位脉冲等待向上取整到系统时钟节拍。显示控制器初始化和刷新指令沿用参考驱动。

在该提交的仓库根目录及所用驱动文件中未发现明确的独立许可证声明。本文件用于记录来源，不替上游授予额外许可；再分发时应核对微雪提供的授权条款。

## Noto Sans SC 字体

`firmware/main/font*.bin` 由 Noto Sans SC 生成，字体来源为 Google Fonts 的 `ofl/notosanssc`：<https://github.com/google/fonts/tree/main/ofl/notosanssc>。

字体版权声明：Copyright 2014–2021 Adobe，Reserved Font Name “Source”。字体按 SIL Open Font License 1.1 提供。完整许可随工程保存在 [assets/fonts/OFL.txt](assets/fonts/OFL.txt)。转换工具为 `tools/generate_fonts.py`；生成位图仍受字体许可适用条款约束。

## 数字字体

首页、天气页及月历的数字字库由 `tools/generate_theme_fonts.py` 转换为 `firmware/main/theme_*.bin`、`weather_serif.bin`、`weather_sans.bin` 和 `calendar_digits.bin`。其中月历日数字库 `calendar_digits.bin` 使用Barlow Condensed Regular、44px设计字号。两种字体均按 SIL Open Font License 1.1 提供，生成的位图字库也保留适用的字体许可要求。

| 风格 | 字体与来源 | 版权与完整许可 |
| --- | --- | --- |
| 精密桌面钟 | Barlow Condensed Light / Regular，[Google Fonts源文件](https://github.com/google/fonts/tree/main/ofl/barlowcondensed) | Copyright 2017 The Barlow Project Authors；[OFL.txt](assets/fonts/barlow-condensed/OFL.txt) |
| 编辑式微衬线 | DM Serif Display Regular，[Google Fonts源文件](https://github.com/google/fonts/tree/main/ofl/dmserifdisplay) | Adobe（2014–2018）与Google LLC（2019），Reserved Font Name “Source”；[OFL.txt](assets/fonts/dm-serif-display/OFL.txt) |

原始TTF供本地再生成使用，不随本仓库提交；已转换的字库与许可文本随源码提供。

## cJSON

`firmware/components/json/` 包含 Dave Gamble 与 cJSON 贡献者提供的 cJSON v1.7.19，用于ESP-IDF 6环境中的JSON解析。

- 上游：<https://github.com/DaveGamble/cJSON/tree/v1.7.19>
- 许可证：MIT，完整文本保留于 [firmware/components/json/LICENSE](firmware/components/json/LICENSE)。

## ESP-IDF

本工程使用本机安装的 Espressif ESP-IDF 6.0.2 工具链及其组件，未将整个SDK复制进工程。SDK及各组件的授权声明以对应上游文件为准：<https://github.com/espressif/esp-idf>。

## 天气图标

天气图形由本工程的几何绘图代码生成。和风天气返回的图标代码用于选择相应天气含义；未将图标服务的字体或SVG文件打包到固件中。对应生成工具与固件图标数据随源码保存。

## 公历、农历与节气数据

月历的日期对应关系与节气日期来自香港天文台官方 [公历与农历日期对照表（文字版本）](https://www.hko.gov.hk/tc/gts/time/conversion1_text.htm)，该入口提供1901–2100年的逐年对照表。本工程内置数据支持公历1901-01-01至2100-12-31，覆盖73,049日，包含2,475个农历月起点与4,800个节气，打包数组共29,100字节。

`tools/generate_calendar_data.py` 提取历法事实数据，生成 `firmware/main/calendar_data.h`，供设备离线查询。具体来源、文件SHA256与生成记录保存在 [assets/calendar/source.json](assets/calendar/source.json)；该记录同时说明起始农历月的反推依据及独立核验方法。原始下载文件仅保存在本机 `.local/calendar-cache`。仓库没有把天文台网页文字、版式或标识打包为界面素材；第三方农历库仅用于独立核验，不作为固件构建或运行依赖。

此处记录事实数据的出处，不将来源说明视为香港天文台授予的开源软件许可证，也不代表其认可本工程。项目未从这些数据推断法定假日或调休安排。
