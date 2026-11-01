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

```bat
build.bat                 :: 构建 Release（默认；= tools\build.ps1）
build.bat debug           :: 构建 Debug
build.bat test            :: 构建 + 运行全部单测
build.bat clean           :: 清理 build\
```

```powershell
powershell -File tools/run-tests.ps1             # 单测 + M0..M3 回环（无需驱动）
powershell -File tools/coverage.ps1              # OpenCppCoverage 行覆盖
powershell -File tools/run-all.ps1               # 一键全流程：构建→单测→覆盖率门禁→回环→自检→GUI→真机 M0..M3，汇总见 integration-results/full-test-report.md
```

真机验收（需管理员 + usbip-win2；默认端口 3425，`-Port` 参数可改）：

```powershell
powershell -File tools/run-m0.ps1    # HidDevice 打开 vendor/手柄集合并收到报告
powershell -File tools/run-m1.ps1    # 设备管理器出现蓝牙无线电
powershell -File tools/run-m2.ps1    # Watcher 收到合成广播
powershell -File tools/run-m3.ps1    # BluetoothLEDevice 连接 + HID 节点
```

> 端口约定：本仓 serve/脚本/GUI 默认 **3425**（`--port N` / GUI 环境变量 `PB_PORT`）。
> usbip 客户端默认 3240 若被同机其它项目占用（如 phantom-audio 的 uacsimd），本仓 attach 自动携带 `-t 3425`，互不干扰、禁止互杀进程。

## 使用

```powershell
# CLI：加载 HID + 蓝牙适配器 + BLE 键盘广播，attach 后跑探针（探针期间自动周期注入测试报告）
phantombt.exe serve --load profiles/gamepad.json --load-bt --ble profiles/ble_keyboard.json \
                    --attach --probe "winrt-probe.exe hid --vid 0x1209 --pid 0xB007 --usage-page 0xFF00 --usage 0x01"

# 对运行中的测试台（serve/GUI）发单条控制命令（注意整条命令加引号）：
phantombt.exe cmd "attach 7-1"
phantombt.exe cmd "inject 7-1 02<64字节hex>"

# GUI：phantombt-gui.exe（五页：设备/BLE/日志/自动测试/全局设置；内嵌服务器默认 3425，PB_PORT 可改）

# 控制命令（CLI/GUI/管道同语法，17 条）：
#   load-hid <json> | load-bt | unload <busid> | unload-all | list
#   attach <busid> | detach <busid>            # attach/detach 均按设备（busid→端口映射）
#   inject <busid> <hex> | ble-advertise <json> | ble-stop <addr> | ble-clear | ble-list
#   ble-type <addr> <text> | sys-bt <list|disable|enable> | snoop <path>|off | status | help
```

Profile：`profiles/*.json`（HID：模板+VID/PID/名称；BLE：kind/name/address）。
HCI 抓包：运行时自动写 `integration-results/hci.snoop`（Wireshark 直接打开）；`snoop <path>|off` 可重定向/关闭。

## 验收矩阵

| 里程碑 | 出口判据 | 状态 |
|---|---|---|
| M0 USB HID | `HidDevice.FromIdAsync` 打开并收到注入报告 | 回环 ✅／真机 ✅ |
| M1 BT 适配器 | 蓝牙无线电出现（radios + 注册表双口径） | 回环 ✅／真机 ✅（注册表口径；radios 阻塞于栈接管） |
| M2 BLE 发现 | Watcher 收到 `PhantomKb` 广播 | 回环 ✅／真机 ⏸ 蓝牙栈不下发 HCI（`docs/detailed-design.md` R-001） |
| M3 BLE HID | LE 连接 + GATT 发现 + HID 报告通知 + Just Works 配对 | 回环 ✅／真机 ⏸ 同上 |

## 测试体系（343 项单测 + 4 级回环 + 十阶段全流程门禁）

- **unit**：协议字节/状态机/编解码（usbip 真实方言线协议、USB 描述符、HCI、ATT/GATT、SMP/AES），行覆盖 96%+
- **conformance**：FIPS-197 / RFC 4493 公开向量锚定
- **integration 回环**：M0-M3 经 TCP/USB-IP 全链路（无内核依赖，CI 可跑）
- **真机段**：winrt-probe（hid/radio/watcher/ble，设备发现带重试）× run-m*.ps1（无权限自动 SKIP=3）
- **全流程门禁**：`tools/run-all.ps1` 十阶段（构建→单测→覆盖率→回环→工具自检→GUI 实测→M0/M1 真机阻断→M2/M3 记录），
  报告 `integration-results/full-test-report.md`；CI 见 `.github/workflows/ci.yml`（真机/GUI 段云端跳过）

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

常见问题：
- **`usbip attach` 报 "Device not available"**：先查 3240/3425 端口归属——同机其它 usbip 项目（uacsimd）占 3240 时，
  本仓命令必须带 `-t 3425`（脚本已内置）；两项目禁止互杀进程。
- **设备管理器 HID 设备黄叹号（code 10）**：报告描述符必须整字节对齐（模板已内置填充）；
  新增模板后用 `tools/run-m0.ps1` 真机验证。
- **⚠ 切勿**手工运行 `devnode.exe/pnputil` 对 `ROOT\USBIP_WIN2\UDE` 做安装/删除——曾把内核 PnP 楔死到只能重启。

## 许可

**个人 / 非商业用途免费**（学习、研究、教学、开源贡献等）；**商业用途需另行授权**。
详见 [LICENSE](LICENSE)（PolyForm Noncommercial 1.0.0 + 商业授权条款）。
发布远端仓库：`tools\create-repo.bat`（需 gh CLI 已登录）。

