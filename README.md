# AI Passport — 提醒吃药

一个为 **FoloToy AI Passport (ESP32-C3)** 开发的离线服药提醒应用。此仓库与其他个人项目完全独立，不修改任何现有仓库。

> 这是提醒工具，不会判断药物、剂量、禁忌或医疗方案。提醒时间应按医生、药师或药品说明中已经确定的用药安排设置。

## 第一版功能

- 每日一个固定提醒时间，默认 `08:00`
- 提醒时间保存到 NVS，断电不会丢失设置
- 开机后手动校准当前时间
- 到点全屏提醒，并在音频可用时播放短提示音
- `OK`：已服药
- `UP`：延后 10 分钟
- `DOWN`：今日跳过
- 主界面显示当前时间、下一次提醒和今日状态
- 长按 `OK`：修改提醒时间
- 长按 `UP`：重新校准当前时间
- 长按 `DOWN`：开启/关闭提醒

## 为什么开机后需要校时

当前 AI Passport 公共硬件契约没有提供可在完全断电后保持准确墙钟时间的独立 RTC。为了避免断电后拿错误时间继续提醒，本应用把“提醒时间”持久化，但每次冷启动都要求重新校准当前时间。

## UI 说明

设备界面使用 ASCII/英文标签，避免默认 Montserrat 字体缺少中文字形造成空白。界面是为提醒场景重新设计的单应用界面，不复用官方硬件测试菜单。

## 项目结构

```text
overlay/main/                 提醒应用源码，覆盖官方项目 main/ 应用层
overlay/tests/                纯逻辑主机测试
scripts/prepare-firmware.sh   拉取固定官方基线并创建 feature/medicine-reminder
scripts/test-model.sh         本地主机逻辑测试
scripts/build.sh              完整静态验证 + 固件构建
.github/workflows/build.yml   ESP-IDF 5.5.3 云端构建
```

本仓库固定基于官方 `FoloToy/ai-passport` 提交：

```text
1051209d807fb26f943236b7e02281f13d39bc90
```

构建时会把完整官方项目拉到 `.work/ai-passport`，在其中创建 `feature/medicine-reminder` 分支，再覆盖本仓库的应用层文件。因此不会触碰你的其它仓库。

## 本地测试

只测试不依赖 ESP-IDF 的状态机：

```bash
./scripts/test-model.sh
```

## 完整构建

需要已经激活 **ESP-IDF 5.5.3**：

```bash
./scripts/build.sh
```

成功后合并固件位于：

```text
.work/ai-passport/build/FoloToy-AI-Passport-full.bin
```

它是用于 `0x0` 的合并固件，不是 application-only `.bin`。

## GitHub Actions

推送到全新的 GitHub 仓库后，`Build Medicine Reminder Firmware` 会：

1. 拉取当前仓库；
2. 拉取固定版本的官方完整 AI Passport 工程；
3. 创建 `feature/medicine-reminder`；
4. 应用提醒应用源码；
5. 运行主机逻辑测试；
6. 运行官方静态验证；
7. 使用 `espressif/idf:v5.5.3` 构建并验证合并固件；
8. 上传 `FoloToy-AI-Passport-full.bin` 和调试归档。

## 实机验收建议

刷机前先确认有可恢复的稳定固件。刷入本应用后逐项检查：首次校时、三键操作、提醒时间保存、到点提醒、提示音、已服药、延后 10 分钟、今日跳过、重启后重新校时。未经设备持有者明确确认，不应自动刷写硬件。
