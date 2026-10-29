# 排障手册

## M1 失败：Windows 不接纳虚拟无线电（R-108）

**定位顺序**：

1. `integration-results/hci.snoop`（Wireshark 打开，btsnoop 格式）——查看 Windows 实际发送的
   HCI 命令序列与我们的应答
2. 服务器日志（`[INFO]` 级别）中的 `op=0x.... UNKNOWN` 行 —— 每一条都是需要补齐的命令：
   - 在 `src/hci/controller.cpp` 的命令表加 case（参考既有写法）
   - 在 `supported_command_list()` 补位图声明
   - `tests/unit/test_controller.cpp` 加对应应答断言
3. `winrt-probe.exe radio-registry` 判定驱动是否绑定（USB\VID_0B00&PID_0001）

**常见原因**：

- 未知厂商命令 → 确认设备 VID 未命中 Windows 厂商初始化表（本项目 VID=0x0B00）
- 描述符被拒 → 对照 `bt_adapter_descriptors_match_spec` 测试的字节布局
- BTHUSB 事件 5（HCI 事件尺寸不符）→ btsnoop 中比对事件参数长度

## 多无线电问题（R-110）

被测机若已有内置蓝牙：设备管理器中禁用内置无线电后再 attach（Windows 蓝牙 UX
在多无线电场景下可能显示混淆）。`GetRadiosAsync` 返回多个 Bluetooth 无线电时
radio 探针取第一个——必要时改探针匹配名称 `Phantom-BT Radio`。

## usbip attach 失败

- `usbip: exportable device not found` → serve 是否已 load 对应 Profile（`status` 查看）
- 端口冲突 → serve 指定 `--port`，attach 同步 `-p`
- 驱动未装 / 未签名测试模式 → 参照 usbip-win2 README 安装 WHLK 版驱动（无需测试签名）

## 覆盖率

`tools/coverage.ps1` 生成 HTML 报告到 `integration-results/coverage/`。
口径：`src/{core,usbip,usb,hid,hci,ble,sim,control}` 行覆盖 100%（gui/probes/cli 以
冒烟+集成覆盖，不计入指标）。

## 测试悬挂/崩溃

测试框架已安装静默崩溃报告（断言与 SEH 均输出到 stderr，不弹窗）。
若测试进程残留：`taskkill /F /IM phantombt-tests.exe`。
