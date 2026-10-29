# Phantom-BT 设计规格（评审版）

> 基于 `rfc-windows-virtual-bt-hid.md`，经 2026-10-29 网络核实与头脑风暴修订。
> 状态：已按用户授权采用推荐决策（C++ / CLI+Win32 GUI / 自研 HCI 引擎 / 一步到位执行）。

## 1. 评估结论摘要

### 核实为真
- **usbip-win2**（vadimgrn）：UDE 驱动 `usbip2_ude` + `usbip2_filter`，WSK 内核 TCP，Win10 1903+ x64/ARM64，
  WHLK 认证签名，BSD-2-Clause（0.9.7.0 起），附 GUI/CLI/SDK（libusbip）。
- 中断/批量端点在 UDE 上成熟可用（旧版 usbip-win 时代即支持）。
- 类驱动自动绑定：hidusb（HID）与 BTHUSB.sys（USB\Class_E0）→ WinRT 自动可见。
- BthLEEnum 实现 ATT/GATT 并为 BLE 远端设备/服务创建 PDO；HOGP 设备经 HidClass 透出给 `HidDevice`。

### 修正（相对 RFC）
1. 许可证：usbip-win2 为 BSD-2-Clause，非 GPLv3（R-106 该子项撤销）。
2. HCI 引擎：RootCanal 官方不支持 Windows 原生构建（README：仅 linux-x86_64/macos-arm64）；
   本机未安装 WSL；RootCanal 的 HCI 行为从未对 Windows BTHUSB 验证。**改选自研精简 C++ 引擎**。
3. 蓝牙 USB 传输按 Core 5.4 Part B：HCI 命令走 EP0 类请求（bmRequestType=0x20, bRequest=0x00，
   命令在 data stage）；事件走 INT IN 0x81（maxpkt 16, interval 1ms）；ACL 走 BULK OUT 0x02 / BULK IN 0x82；
   SCO 接口（interface 1）**仅呈现空 alt setting 0**（规范允许的零等时带宽合规形态）。

### 新增风险
- R-107：WinRT `HidDevice` 无法打开系统键盘/鼠标顶层集合（UWP/系统保留）。
  缓解：每个 HID Profile 附带 vendor-defined 顶层集合；验收用手柄/vendor 用法。
- R-108：usbip 上的 BTHUSB 无公开先例，M1 仍是全案最大风险。自研引擎使失败可观测可迭代。

## 2. 目标（继承 RFC R1–R6，修订口径）

| # | 目标 | 验收口径 |
|---|---|---|
| R2' | USB HID：Profile 化报告描述符/VID/PID，中断 IN/OUT 报告注入 | winrt-probe 以 `HidDevice.FromIdAsync` 打开 vendor/手柄集合并收到注入报告 |
| R3' | 虚拟 USB 蓝牙适配器（E0/01/01，IAD 双接口） | 设备管理器出现蓝牙无线电；Windows 蓝牙开关可用 |
| R4' | BLE 设备：广播注入、连接、GATT/HOGP（bondless 优先，Just Works 配对为扩展） | Watcher 收到合成广播；`BluetoothLEDevice` 连接+服务发现；`HidDevice` 收到 BLE 键盘报告 |
| R5' | WinRT 全验收自动化 | winrt-probe.exe 输出结构化结果，CI 脚本判定 |
| R7 | 新增：CLI + Win32 GUI 全功能等价；unit/conformance 测试离线可跑 | 覆盖率：纯逻辑模块 line+branch 100%（OpenCppCoverage 度量） |

Non-Goals（继承）：SCO/等时、真实射频、Win10<1903、UVC；新增：LE Secure Connections (P-256)、LE Audio。

## 3. 架构

```
┌─ 被测 Windows ─────────────────────────────────────────────┐
│ 被测应用(WinRT) ← hidusb / BTHUSB.sys / BthLEEnum          │
│      ↑ URB                                                  │
│ usbip-win2 (usbip2_ude, BSD-2, WHLK, 预装外部依赖)          │
└──────┬ TCP 3240 localhost ─────────────────────────────────┘
┌──────┴ phantom-bt（本仓库，纯 C++/Win32）──────────────────┐
│ usbip server ← usb 设备核心(描述符引擎/控制传输/端点模型)    │
│   ├─ hid 模型（Profile：gamepad/keyboard/mouse/vendor）     │
│   └─ bt 适配器模型（EP0 类请求=HCI cmd / INT IN=事件 /      │
│        BULK=ACL；interface 1 仅空 alt0）                    │
│ IHciEngine ←→ MiniController（自研：HCI 子集 + LE 空口队列）│
│   ├─ 虚拟外设（广播源：iBeacon/HOGP 键盘/GATT 通用设备）    │
│   └─ 设备侧协议（L2CAP 固定通道 / ATT server / GATT DB /    │
│        SMP responder(Just Works, AES-CMAC) / HOGP）        │
│ 控制层（命令解析，CLI/GUI 共享）+ btsnoop/pcap 全量日志     │
└─ 前端：phantombt.exe(CLI) / phantombt-gui.exe(Win32) /      │
   winrt-probe.exe(C++/WinRT 验收探针)                        │
```

### 组件职责与边界
- `core/`：字节流/日志/时间/线程/JSON Profile 加载（无业务）。
- `usbip/`：USB/IP 导出协议（OP_REQ_DEVLIST/IMPORT、URB CMD/RET 编解码），与设备语义解耦。
- `usb/`：描述符构建器、控制传输分发（标准/类/厂商）、端点与 URB 生命周期。
- `hid/`：HID 设备模型——报告描述符模板、报告编解码、Profile。
- `bluetooth/`：BT 适配器 USB 模型 + H4 无关的 HCI 通道桥接。
- `hci/`：MiniController——HCI 命令子集、事件生成、LE 空口（广播/扫描/连接/ACL/断开/LL 控制过程精简）。
- `ble/`：设备侧栈——L2CAP(固定通道)、ATT、GATT DB、HOGP、SMP。
- `control/`：本地控制 API（进程内直调 + 命名管道供 GUI/CLI 远程）、命令语法。
- `probes/`：winrt-probe（HidDevice / Watcher / BluetoothLEDevice / GATT 断言）。

### 关键数据通路
1. HID：控制台注入 → hid 模型编码报告 → 中断 IN URB → usbip → hidusb → WinRT 读取。
2. HCI 命令：BTHUSB EP0 类请求 → 适配器模型 → MiniController `HandleCommand()` → 事件回 INT IN。
3. ACL：Windows ATT 请求 → BULK IN → L2CAP/ATT server → 应答 → BULK OUT → BthLEEnum → WinRT。
4. 广播：虚拟外设设置 adv data → 引擎合成 LE Advertising Report → 轮询扫描的 BTHUSB。

## 4. 测试策略

三层：
- **unit**：所有纯逻辑模块（描述符字节、USB/IP 编解码、HCI 状态机、ATT/SMP、报告编解码）——目标 100% line+branch。
- **conformance**：官方 golden vectors——BT Core Spec SMP c1/s1/AES-CMAC 测试向量、HID 报告描述符样例、
  USB/IP 报文字节样例、ATT PDU 边界（MTU、无效 PDU）。
- **integration**（需管理员 + usbip-win2 驱动）：M0–M3 验收自动化；winrt-probe 断言；
  异常注入（STALL、超时、畸形描述符、拆包 ACL）。
框架：自研零依赖 C++ 测试壳（注册宏 + 断言 + 分层过滤），OpenCppCoverage 出覆盖率报告。

## 5. 里程碑（映射 RFC M0–M3，前置 M-1，后置 M4）

| 阶段 | 内容 | 出口判据 |
|---|---|---|
| M-1 | 工程底座：CMake/VS18、core、测试壳、usbip 协议层 + conformance | 全部单测绿 |
| M0 | USB 核心 + HID 模型 + attach 链路 + CLI + probe | HidDevice 打开 vendor/手柄集合并收到注入报告 |
| M1 | BT 适配器模型 + MiniController 初始化子集 | 设备管理器蓝牙无线电；开关可用 |
| M2 | LE 广播/扫描 + 虚拟外设广播注入 | Watcher 收到合成广播 |
| M3 | LE 连接 + ATT/GATT/HOGP；(M3b) SMP Just Works + LL 加密 | BluetoothLEDevice 连接+服务发现；HidDevice 收 BLE 键盘报告 |
| M4 | Win32 GUI 全功能 + btsnoop 日志 + 覆盖率达标 + 文档 | GUI 冒烟 + 覆盖率报告 |

回退：M1 失败 → M0 独立交付（同 RFC）；M3b 配对受阻 → bondless HOGP 先行（R-103 口径）。

## 6. 残留风险
- R-108 M1：BTHUSB 对 usbip 设备的接纳（无先例）——探针 + 引擎侧 HCI 全量日志定位。
- R-109：Windows 蓝牙栈初始化 HCI 命令子集的实际面（探针阶段逐条补齐）。
- R-110：被测机若有真实蓝牙无线电，双无线电并存的 UX 干扰——文档要求禁用/卸载内置无线电。
- R-111：usbip-win2 控制端点类请求透传路径（HID SET_REPORT 同类）需在 M0 验证。
