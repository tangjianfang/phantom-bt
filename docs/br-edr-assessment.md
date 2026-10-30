# 经典蓝牙（BR/EDR）Profile 仿真 — 评估结论

> 日期：2026-10-30 · 状态：**未支持，可分阶段实现** · 关联：GUI「全局设置 → 经典蓝牙」

## 结论先行

当前 phantom-bt 只实现了 **BLE（LE 控制器 + LE 软件设备）** 路径；经典蓝牙 profile
（HID 键盘/鼠标走 L2CAP、SPP 串口等）**尚未实现**，但架构上没有阻断项，可以增量补齐。
建议的第一阶段目标是 **HID over L2CAP**（与本项目"虚拟 HID 测试台"定位最贴合）。

## 现状盘点（已具备 / 缺失）

| 层 | 已具备 | 缺失（经典必需） |
|---|---|---|
| HCI 命令 | 基带通用命令约 45 个（Scan_Enable、Class_of_Device、EIR、Simple_Pairing 等）+ LE 全套 | `Create_Connection`（主机发起）、`Accept_Connection_Request`（角色协商）、连接鉴权/加密命令（Link_Key 路径）、`Remote_Name_Request` |
| HCI 事件 | LE 元事件、Command Complete/Status、（tick 驱动的）广播/连接事件 | `Connection Request`（page 来向）、`Connection Complete`（BR/EDR）、`Authentication Complete`、`Remote Name Request Complete`、mode 事件（含 flush/_qos） |
| L2CAP | LE 固定通道（ATT 0x0004 / SM 0x0006）+ 分片重组 | **BR/EDR 信令通道 0x0001**（`Connection Request/Response`、`Configure Request/Response`、`Information Request/Response`）与动态通道生命周期（PSM → CID） |
| Profile | HOGP（LE HID over GATT）、SMP 配对 | **HID profile**：控制通道 PSM 0x11（`HANDSHAKE` / `SET_REPORT` / `GET_REPORT`）、中断通道 PSM 0x13（输入报告注入）；SPP 则另需 RFCOMM 多路复用器（PSM 3 + DLCI 帧） |
| 主机侧 | Windows BTHUSB 驱动接管虚拟适配器（E0 类 USB 设备）；BLE 已被系统栈枚举 | 系统栈（bthport）发起 page 后的完整应答时序：role/feature 协商、L2CAP 信息交换、SDP（PSM 1，需最小 SDP 应答器才能让 Windows 识别服务） |

## 关键风险

1. **SDP 是硬门槛**：Windows 枚举经典 HID 设备前会走 SDP 查询服务记录。需要实现一个
   最小 SDP 应答器（service search / attribute request 应答，硬编码 HID 的 service
   record）。工作量集中但模式固定。
2. **时序敏感**：bthport 对 page/连接/信令的窗口有超时；虚拟控制器的事件延迟
   （tick=16ms）需要验证是否满足（btsnoop 抓包对照真机时序，排障路径已就绪）。
3. **鉴权**：经典 HID 常用 SSP（Simple Secure Pairing，NC/Just Works）；现有 SMP 是
   LE 侧的，经典侧需要 HCI 层 `User Confirmation Request` 事件应答路径（无真实射频，
   同样可在事件层伪造）。

## 分阶段计划建议

- **B1 — 链路层**：BR/EDR 连接接受（page 应答、role 协商、Connection Complete）
  + bthport 能"看到"一条已建立的经典 ACL。验收：WinRT `BluetoothDeviceInfo` 列出远端。
- **B2 — L2CAP 动态通道 + SDP 最小应答器**。验收：Windows 设备管理器出现 HID 服务节点。
- **B3 — HID profile（0x11/0x13）**：控制通道握手 + 中断通道注入，复用现有
  `hid/report_descriptor` 与 `HidModel`。验收：`HidDevice` WinRT API 打开并收到输入报告
  （与现有 M0~M3 验收口径一致）。
- **B4（可选）— SPP/RFCOMM**。

## 与现有代码的衔接点

- `MiniController`（`src/hci/controller.cpp`）：命令矩阵追加 BR/EDR 连接组；未知命令
  回 UnknownCmd + `op_log` 的迭代入口已在。
- `ble/l2cap.cpp` 的 ACL 重组/分片与 BR/EDR 帧格式兼容（PB 标志位同一套）。
- 蓝牙 USB 传输层（`bluetooth/adapter_model.cpp`）无需改动——HCI/ACL 通道是共用的。
