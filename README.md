# Phantom-BT — Windows 虚拟 HID / 蓝牙 / BLE HID 测试台

纯软件（Windows 单机）仿真 **USB HID 设备**、**USB 蓝牙适配器**与 **BLE HID 外设**，
目标是被测应用的 **WinRT API 视图**（`HidDevice` / `BluetoothLEAdvertisementWatcher` /
`BluetoothLEDevice`）可见、可用、可脚本化回归。

> 依据 `rfc-windows-virtual-bt-hid.md`（2026-10-29 评审修订版）实现。
> 设计文档：`docs/superpowers/specs/`；实施计划：`docs/superpowers/plans/`。

## 架构

```
被测应用(WinRT) ← hidusb / BTHUSB.sys / BthLEEnum        ← Windows 内置类驱动
      ↑ URB
usbip-win2 驱动（usbip2_ude，外部预装，BSD-2-Clause）      ← TCP 3240 本机回环
      ↑ USB/IP 协议
phantom-bt（本仓库，纯 C++20/Win32）
  ├─ usbip 服务器 + USB 设备核心（描述符/控制传输/端点）
  ├─ HID 模型（gamepad/keyboard/mouse/vendor Profile）
  ├─ 蓝牙适配器模型（E0/01/01，EP0 类请求=HCI 命令，空 SCO alt0）
  ├─ MiniController（自研 HCI 子集 + LE 空口 + 链路）
  └─ BLE 设备栈（L2CAP/ATT/GATT/HOGP/SMP Just Works）
CLI: phantombt.exe ／ GUI: phantombt-gui.exe ／ 探针: winrt-probe.exe
```

## 前置条件

- Windows 10 1903+ x64；VS 2026（C++ 工作负载 + WinSDK C++/WinRT 组件）、CMake、Ninja
- **usbip-win2 驱动**（[vadimgrn/usbip-win2](https://github.com/vadimgrn/usbip-win2)）：
  安装后 `usbip.exe` 可用（真机段需要管理员）
- ⚠ R-110：测试机建议**禁用内置蓝牙**（多无线电并存的 UX 干扰）

## 构建 / 测试

```powershell
powershell -File tools/build.ps1                 # 构建 Debug
powershell -File tools/run-tests.ps1             # 单测 + M0..M3 回环（无需驱动）
powershell -File tools/coverage.ps1              # OpenCppCoverage 行覆盖
```

真机验收（需管理员 + usbip-win2）：

```powershell
powershell -File tools/run-m0.ps1    # HidDevice 打开 vendor/手柄集合并收到报告
powershell -File tools/run-m1.ps1    # 设备管理器出现蓝牙无线电
powershell -File tools/run-m2.ps1    # Watcher 收到合成广播
powershell -File tools/run-m3.ps1    # BluetoothLEDevice 连接 + HID 节点
```

## 使用

```powershell
# CLI：加载 HID + 蓝牙适配器 + BLE 键盘广播，attach 后跑探针
phantombt.exe serve --load profiles/gamepad.json --load-bt --ble profiles/ble_keyboard.json \
                    --attach --probe "winrt-probe.exe hid --vid 0x1209 --pid 0xB007 --usage-page 0xFF00 --usage 0x01"

# GUI：phantombt-gui.exe（三页：设备/BLE/日志；内嵌服务器 + \\.\pipe\phantom-bt 管道）

# 控制命令（CLI/GUI/管道同语法）：
#   load-hid <json> | load-bt | list | attach <busid> | detach <busid>
#   inject <busid> <hex> | ble-advertise <json> | ble-stop <addr> | ble-list | ble-type <addr> <text>
#   status | help
```

Profile：`profiles/*.json`（HID：模板+VID/PID/名称；BLE：kind/name/address）。
HCI 抓包：运行时自动写 `integration-results/hci.snoop`（Wireshark 直接打开）。

## 验收矩阵

| 里程碑 | 出口判据 | 状态 |
|---|---|---|
| M0 USB HID | `HidDevice.FromIdAsync` 打开并收到注入报告 | 回环 ✅／真机待驱动 |
| M1 BT 适配器 | 蓝牙无线电出现（radios + 注册表双口径） | 回环 ✅／真机待驱动 |
| M2 BLE 发现 | Watcher 收到 `PhantomKb` 广播 | 回环 ✅／真机待驱动 |
| M3 BLE HID | LE 连接 + GATT 发现 + HID 报告通知 + Just Works 配对 | 回环 ✅／真机待驱动 |

## 测试体系（149 项单测 + 4 级回环集成）

- **unit**：协议字节/状态机/编解码（usbip 线协议、USB 描述符、HCI、ATT/GATT、SMP/AES）
- **conformance**：FIPS-197 / RFC 4493 / BT SIG golden 向量
- **integration 回环**：M0-M3 经 TCP/USB-IP 全链路（无内核依赖，CI 可跑）
- **真机段**：winrt-probe（hid/radio/watcher/ble）× run-m*.ps1（无权限自动 SKIP=3）

## 关键技术决策

1. **usbip-win2**（BSD-2、WHLK 签名、UDE）而非自研内核驱动
2. **HCI 命令走 EP0 类请求**（Core 5.4 Part B：bmRequestType=0x20/bRequest=0x00，data stage 携带命令）
3. **SCO 接口仅空 alt0**（规范允许的零等时带宽形态 → 彻底回避等时传输）
4. **自研 MiniController**（RootCanal 不支持 Windows 原生构建；本机无 WSL；且其 HCI 行为
   从未对 BTHUSB 验证过——自研使未知命令可观测可迭代：op_log 全量记录 + UnknownCmd 优雅应答）
5. **LL/加密在 HCI 事件层伪造**（两主机同进程直连，无真实射频；SMP 加密为真实密码学，
   有 FIPS/RFC 向量锚定）
6. 测试 VID/PID 用非常见值（0x1209/0xB0xx、0x0B00/0x0001）避免触发厂商初始化路径

## 排障

见 `docs/troubleshooting.md`（M1 失败定位法、未知 HCI 命令补齐流程、多无线电问题）。
