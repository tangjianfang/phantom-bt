# Phantom-BT 全流程自动化测试报告

- **日期**：2026-11-01
- **范围**：功能×测试覆盖盘点与补测、覆盖率提升、缺陷积压清零、十阶段全流程套件（`tools/run-all.ps1`）落地与全绿循环、真机 M2/M3 攻坚
- **数据来源**：本轮工作流运行记录（功能矩阵盘点、补测轮、全绿循环第 2 轮、真机攻坚记录）与仓库磁盘产物。除标注"本会话核验"处为本报告撰写会话实际读取/执行外，其余数字均取自工作流输入数据，未做外推。
- **代码基线**：`main @ a6a8d1b`，工作区含本轮全部变更（未提交，见第七节）。

---

## 一、总评

**结论：自动化测试体系完成"盘点 → 补齐 → 门禁 → 全绿"闭环；十阶段全流程套件第 2 轮全绿（8 个阻断阶段全部 PASS），16 项缺陷积压清零；真机 M0/M1 达标，M2/M3 攻坚未突破（R-001 维持 Open）。**

| 维度 | 状态 | 依据 |
|---|---|---|
| 功能覆盖 | 🟢 | 69 项功能盘点，盘点时已覆盖 35 项；缺口 34 项由补测轮处理（逐项"已补齐"以工作流 feature-board 为准） |
| 单元测试 | 🟢 | 本轮新增 102 个单测；全量 343/343 通过（`integration-results/full-test-logs/02-unit-tests.log`，本会话核验） |
| 覆盖率 | 🟢 | 96.15%（3501/3641 行），门禁阈值 96.1 只升不降；全绿循环复测 96.21%（3503/3641）同样过门禁 |
| 缺陷积压 | 🟢 | 16 项全部修复，未完成 0 项（第四节） |
| 全流程门禁 | 🟢 | `tools/run-all.ps1` 十阶段第 2 轮全绿：8 个阻断阶段全 PASS，总耗时 388.2s（`integration-results/full-test-report.md`，本会话核验） |
| 真机验收 | 🟡 | M0 PASS、M1 PASS(registry)；M2 FAIL(noname)、M3 FAIL(nodev)——Windows 蓝牙栈仍不下发 HCI（R-001） |
| 云端 CI | 🟡 | `.github/workflows/ci.yml` 已 `git add`（`git ls-files` 已列出，本会话核验），按规约未 commit/push，云端首跑未发生 |

---

## 二、功能 × 测试矩阵

### 2.1 盘点口径与总数

盘点由 3 名盘点员并行完成（源码功能面 / 入口与工具 / 真机与端到端），共盘点 **69 项**功能，盘点时已有自动化覆盖 **35 项**，缺口 **34 项**交由补测轮处理。本轮补齐的逐项状态以工作流 **feature-board** 的"已补齐"行为准；feature-board 是工作流运行时工件，**不落盘入库**，故本表汇总到领域级并列出各领域可核查的补齐落点（新增测试文件与 check-tools 检查段，均为本会话在磁盘上核验过的实物）。

> 领域枚举（core / usb-usbip / hid / bt-hci / ble / sim-control / infra / entry）取自工作流定义 `.zcode/workflow-drafts/全流程自动化测试.dwf.ts`（盘点与补测按此 8 领域分派）。

| 领域 | 盘点范围 | 本轮补齐的可核查落点 |
|---|---|---|
| core 基础库 | bytes/json/log/net/env_check/bt_radio | `tests/unit/test_core_cov.cpp`（现存 30 个 PB_TEST） |
| usb-usbip | 描述符/控制传输/usbip 服务器（devlist/import/URB/STALL） | `tests/unit/test_usb_usbip_cov.cpp`（32 个 PB_TEST） |
| hid | 四模板/类请求（GET/SET_REPORT、idle、protocol） | `tests/unit/test_hid_cov.cpp`（7 个）+ 既有 `test_hid_model.cpp`、`test_report_descriptor.cpp` |
| bt-hci | 蓝牙适配器 + HCI 子集（含流控、断链完成） | `tests/unit/test_bt_hci_cov.cpp`（18 个）+ `test_bt_adapter.cpp`、`test_controller.cpp` |
| ble | 广播/连接/L2CAP/ATT/GATT/HOGP/SMP | `tests/unit/test_ble_cov.cpp`（10 个）+ `test_aes_smp.cpp` 等 |
| sim-control | 17 条控制命令、仿真编排、抓包 | `tests/unit/test_sim_control_cov.cpp`（32 个） |
| infra | tools/*.ps1、cov_gaps.py、ci.yml、一次性工具 | `tools/check-tools.ps1` G 段（工具自检）+ 新增 `tools/coverage-gate.ps1`、`tools/cov_gaps.py` |
| entry | CLI serve 参数/cmd/selftest、GUI 五页、探针四模式 | `tests/unit/test_cli_cov.cpp`（7 个）、`test_entry_cov.cpp`（7 个）、`tools/check-tools.ps1` A–F/H/I/J/K/M 段（exe 级与真路径检查） |

- 补测轮新增单测 **102 个**（工作流记录）；9 个 `tests/unit/*_cov.cpp` 补测文件当前共存 160 个 PB_TEST（本会话逐文件计数），系今日两轮工作流（全局评审补测 + 本轮补测）累计写入，本轮份额以工作流记录的 102 为准。
- 入口级检查 `tools/check-tools.ps1`（本轮新建，2214 行）按 A–M 分段覆盖 CLI 入口分发、serve 旗标组合、`cmd` 管道客户端、doctor、GUI 五页真实点击、sys-bt 深检、sim-control 真机路径（内核 attach/detach、无线电开关往返、多设备并发、VID_0B00 排除活体验证）、README 矩阵同步、`serve --attach` 端到端、GUI 全局设置页、winrt-probe 四模式。最新一次运行 **90 PASS / 0 FAIL / 4 SKIP**（`integration-results/check-tools.txt`，本会话核验；4 项 SKIP 均为环境原因，见第四节表后注）。

### 2.2 验收矩阵（README 口径，本轮已同步）

| 里程碑 | 出口判据 | 状态（README.md:87-90，本会话核验） |
|---|---|---|
| M0 USB HID | `HidDevice.FromIdAsync` 打开并收到注入报告 | 回环 ✅／真机 ✅（m0.json realdevice=PASS） |
| M1 BT 适配器 | 蓝牙无线电出现（radios + 注册表双口径） | 回环 ✅／真机 ✅（注册表口径；radios 阻塞于栈接管） |
| M2 BLE 发现 | Watcher 收到 `PhantomKb` 广播 | 回环 ✅／真机 ⏸ 蓝牙栈不下发 HCI（R-001） |
| M3 BLE HID | LE 连接 + GATT 发现 + HID 报告通知 + Just Works 配对 | 回环 ✅／真机 ⏸ 同上 |

README 的 M0/M1 行由"真机待驱动"改为"真机 ✅／真机 ✅（注册表口径）"，与 `integration-results/m0.json`=PASS、`m1.json`=PASS(registry) 对齐——否则 check-tools 的 `readme-matrix-syncs-with-results` 防回归行（tools/check-tools.ps1:1500-1525，J 段）会在下一轮变红；该行现已 PASS（含在上述 90 PASS 内）。

---

## 三、覆盖率与新增测试

| 指标 | 数值 | 来源 |
|---|---|---|
| 行覆盖率（本轮终态） | **96.15%**（3501/3641 行，51 文件） | 工作流测量记录 |
| 行覆盖率（全绿循环复测） | 96.21%（3503/3641），GATE PASS ≥ 96.1 | `integration-results/full-test-logs/03-coverage-gate.log`（本会话核验；两次测量相差 2 行/0.06pp，系代码演进中的两次独立测量，均过门禁） |
| 新增单测 | **102 个** | 工作流补测轮记录 |
| 单测总量 | 343 个，343/343 通过 | `02-unit-tests.log` 尾行 `== 343/343 tests passed ==`（本会话核验）；README"测试体系"行同步由 149 改为 343（git diff 本会话核验） |
| 门禁阈值 | 96.1%，只升不降 | `tools/coverage-gate.ps1` 常量 `$ThresholdPct=96.1` |
| 测量口径 | OpenCppCoverage cobertura，计入 `src/`；gui/cli/probes 惯例不计入（cov_gaps.py 防御性排除） | 同全局评审口径 |

未达 100% 的文件 12 个，最差依次（03-coverage-gate.log，本会话核验）：`src/core/bt_radio.cpp` 32.93%（27/82）、`src/core/env_check.cpp` 88.56%、`src/usbip/server.cpp` 94.27%、`src/sim/simulator.cpp` 95.24%。

---

## 四、缺陷修复清单

缺陷积压 **16 项**，本轮**修复 16 项、未完成 0 项（无跳过项）**。各修复均在本会话通过 `git diff src/` 逐一定位到修复标记（表内"代码证据"为 diff 中的实测锚点）；"对应评审发现"列指 `docs/global-review-2026-11-01.md` 的编号（按缺陷名与内容对应，gui-port 为工作流新发现、不在该评审 62 条之内）。

| # | 短名 | 内容 | 对应评审发现 | 代码证据（git diff src/ 实测） |
|---|---|---|---|---|
| 1 | gui-port | GUI 内嵌 usbip 服务器端口硬编码，不可配置 | —（工作流新发现） | `src/gui/main.cpp`："内嵌 usbip 服务器端口可配置（bug gui-port）：环境变量 PB_PORT，默认 3425" |
| 2 | detach-by-device | `detach` 恒 `usbip detach --all`，波及他设备 | #42 | `src/sim/simulator.cpp`：`parse_imported_ports` 解析 `usbip port` 建立 busid→端口映射，按设备 `detach -p <n>`，未导入则无操作"绝不 --all" |
| 3 | cli-stoi | CLI 数字参数缺值/非法触发未捕获 `std::stoi` 异常挂死 | #55 | `src/cli/main.cpp`："bug cli-stoi：--port/--exit-after-ms 的缺值/非法值……变成用法错误 + 退出码 2" |
| 4 | probe-skip | `--probe` 被静默跳过仍退出 0，"未验证=通过" | #56 | `src/cli/main.cpp`：probe 无法运行时打印 SKIPPED 并置 `probe_rc = 3`（与探针 0/2/3 语义一致） |
| 5 | stop-uaf | `Server::stop()` 先 delete ConnEntry 后 join，UAF | #7 | `src/usbip/server.cpp`："先 join accept 线程……join 所有连接线程后再 delete ConnEntry"（R-005 关闭） |
| 6 | resize-clamp | 线上可控 `transfer_buffer_length` 驱动无上界 resize | #8 | `src/usbip/server.cpp`：`MAX_TRANSFER_BUFFER = 16MB` 钳位，超限回 `URB_STATUS_EINVAL(-22)` |
| 7 | double-import | 同一设备可被两连接同时 import，sink 覆盖 | #10 | `src/usbip/server.cpp`："单导出单导入……第二个连接 import 同一设备会 attach_sink 覆盖首个连接的 sink"（`imported_busids` 登记/释放） |
| 8 | btsnoop-ts | FILETIME 微秒直加错抄纪元常量，时间戳快约 399 年 | #39 | `src/sim/btsnoop.cpp`：`kUnixEpochFiletime = 116444736000000000ull`，`(t - kUnixEpochFiletime)/10 + kBtsnoopEpochUs` |
| 9 | hci-disc | 主机发起 Disconnect 永无 Disconnection_Complete | #24 | `src/hci/controller.cpp`：CS 起始、`delayed_events_` 延迟产出 `EVT_DISCONNECTION_COMPLETE` 并到点释放句柄/链路 |
| 10 | nocp | 控制器从不发 Number_Of_Completed_Packets，流控停摆 | #26 | `src/hci/controller.cpp`：按 Vol4 PartE 7.7.19 组包归还 credit；主机侧 `CMD_HOST_NUM_COMPLETED_PACKETS` 处理入表 |
| 11 | lebuf | LE_Read_Buffer_Size 返回参数多插一个 0 字节 | #23 | `src/hci/controller.cpp`：返回长度表 `CMD_LE_READ_BUFFER_SIZE, 25*8+1`，注释明确 Total_Num_LE_ACL_Data_Packets(1) 无多余字节 |
| 12 | attach-inject | busid 未校验内插 `_popen`，命令注入/提权链 | #41 | `src/sim/simulator.cpp`：busid 白名单 `^\d+-\d+$` 后方才进入 `_popen` |
| 13 | pipe-stop-hang | 有客户端保持连接时 `PipeServer::stop()` 永久挂起 | #40 | `src/control/pipe_server.cpp`："阻塞读（否则 join 永远等不到 —— bug pipe-stop-hang）" |
| 14 | snoop-cmd | `snoop <path>|off` 控制命令未实现 | #46 | `src/control/commands.cpp`：`snoop` 命令分发 + `sim.cmd_snoop`；README 命令清单同步 |
| 15 | hogp-suspend | HOGP suspend 置位后仍推通知（死代码） | #37 | `src/ble/hogp.cpp`：`if (suspended_)` 分支——控制点 0x00 暂停输入报告通知 |
| 16 | iad-doc | spec R3' 要求 IAD 与实现"无 IAD"矛盾 | #27 | `src/bluetooth/adapter_model.cpp`：注释固化"无 IAD：本机实测 usbhub 未插 usbccgp（BTHUSB 直接以设备级 E0/01/01 兼容 ID 绑定整机）"；specs/plans 文档同步修订 |

> 表后注：check-tools 最新运行中的 4 项 SKIP 不是缺陷遗留，而是环境不可达腿：`sysbt-real-toggle-roundtrip` 与 `gui-settings-sysbt-buttons-real-toggle`（本机无物理蓝牙无线电）、`serve-attach-flag-no-usbip-continues`（usbip.exe 已安装，skip 腿不可达）、`serve-attach-flag-failure-logged-serve-continues`（管理员 shell 下 attach 成功，失败腿不可达）——见 `integration-results/check-tools.txt`（本会话核验）。

---

## 五、全流程套件 run-all：步骤与用法

**入口**：`tools/run-all.ps1`（本轮新建，305 行）＋ `README.md`（用法已写入"构建/测试"节）。一行命令跑完本仓全部门禁，汇总写入 `integration-results/full-test-report.md`（逐阶段秒数 + 失败阶段日志尾 30 行；全量日志在 `integration-results/full-test-logs/NN-*.log`）。

```powershell
powershell -File tools/run-all.ps1              # 默认端口 3425
powershell -File tools/run-all.ps1 -Port 3425   # 显式指定
powershell -File tools/run-all.ps1 -ListStages  # 只打印十阶段计划，不执行
```

退出规则（tools/run-all.ps1:299-304，本会话核验）：任一**阻断**阶段 FAIL → 退出 1；SKIPPED 永不算失败；m2/m3 仅记录不阻断 → 全部阻断阶段绿则退出 0。真机段前置清理只 taskkill `phantombt.exe` + `usbip detach -a`，**绝不触碰 uacsimd（3240 端口持有者）**。

| # | 阶段 | 命令 | 判定 | 阻断 |
|---|---|---|---|---|
| 01 | build-release | `tools/build.ps1 -Config Release` | exit 0=PASS，否则 FAIL | ✅ |
| 02 | unit-tests | `tools/build.ps1 -Config Debug` → `build/debug/tests/phantombt-tests.exe` | exe exit 0=PASS | ✅ |
| 03 | coverage-gate | `tools/coverage-gate.ps1` | 0=PASS；3=OpenCppCoverage 不可用=SKIPPED；1=FAIL | ✅ |
| 04 | loopback-suite | `tools/run-tests.ps1` | 0=PASS 否则 FAIL | ✅ |
| 05 | check-tools | `tools/check-tools.ps1` | 文件存在则运行：0=PASS、3=环境未就绪=SKIPPED、1=FAIL；文件不存在则 SKIPPED（"not created yet"） | ✅（仅当文件存在） |
| 06 | gui-verify | `tools/gui-verify.ps1 -Port 3425` | 0=PASS 否则 FAIL | ✅ |
| 07 | m0-realdevice | `tools/run-m0.ps1 -Port 3425` | rc 0 ⟺ realdevice=PASS（run-m0.ps1:44-47：loopback≠PASS→1、FAIL*→1、SKIP*→3）；rc 3=SKIPPED | ✅ |
| 08 | m1-realdevice | `tools/run-m1.ps1 -Port 3425` | rc 0 覆盖 PASS 与 PASS(registry)＝注册表口径（run-m1.ps1:131-134，本会话核验：仅 FAIL*→1、SKIP*→3）；rc 3=SKIPPED | ✅ |
| 09 | m2/m3-realdevice | `tools/run-m2.ps1` + `tools/run-m3.ps1 -Port 3425` | 尝试执行、结果记录，**永不阻断** | ❌ |

**最新一轮结果**（`integration-results/full-test-report.md`，2026-11-01 23:04:41，管理员 shell，usbip.exe=C:\Program Files\USBip\usbip.exe，总耗时 388.2s；本会话核验）：

| # | 阶段 | 状态 | 秒 | 阻断 | 明细 |
|---|---|---|---|---|---|
| 1 | build-release | PASS | 8.6 | ✅ | |
| 2 | unit-tests | PASS | 38.8 | ✅ | |
| 3 | coverage-gate | PASS | 44.7 | ✅ | 96.21% (3503/3641) ≥ 96.1 |
| 4 | loopback-suite | PASS | 77.2 | ✅ | |
| 5 | check-tools | PASS | 75.6 | ✅ | 90 passed / 0 failed / 4 skipped |
| 6 | gui-verify | PASS | 44.1 | ✅ | ALL GUI CHECKS PASSED |
| 7 | m0-realdevice | PASS | 37.5 | ✅ | realdevice=PASS |
| 8 | m1-realdevice | PASS | 13.4 | ✅ | realdevice=PASS(registry) |
| 9 | m2-realdevice | FAIL | 23.4 | ❌ | exit 1，realdevice=FAIL(reason="noname") |
| 10 | m3-realdevice | FAIL | 23.1 | ❌ | exit 1，realdevice=FAIL(w=2,b=2) |

m2/m3 的 FAIL 即第六节"真机待驱动"状态，非阻断，README 矩阵与之同步。

---

## 六、真机攻坚过程与结论

### 6.1 根因假设与实验

**根因假设（工作流记录原文，末尾"且"后文本被截断，仅陈述可得部分）**：「主因（HCI 从不下发的直接原因）：虚拟 BT 适配器以全速形态暴露——bulk 端点 wMaxPacketSize=64、bcdUSB=0x0110（src/bluetooth/adapter_model.cpp:27,35-39）」

假设所指为**改动前**的代码形态，git diff（本会话核验）完整佐证：

- 原 `c.bcd_usb = 0x0110; // FS 蓝牙惯例` → 现 `0x0200 // HS 蓝牙惯例（usbip-win2 UDE 根集线器为 USB 3 形态，按 HS 规则校验：FS 描述符会在 BTHUSB SELECT_CONFIGURATION 时被拒）`（adapter_model.cpp:27）
- 原 bulk 端点 64 字节 → 现 `{0x02,2,512,0}` / `{0x82,2,512,0}`（HS bulk 只允许 512）、事件端点 interval 改 HS 微帧语义 4（adapter_model.cpp:35-39）
- 新增 `exported_.speed = USBIP_SPEED_HIGH`（USB/IP 宣告与描述符形态一致，否则 UDE 端口校验按 FS 规则拒绝 HS 形态）与 `controller_.on_ready_to_host` 钩子（设备侧主动输出经挂起 URB 完成）

**结论：突破 = false。** 即便适配器已改为 HS 形态并通过 BTHUSB SELECT_CONFIGURATION，Windows 蓝牙栈仍不下发 HCI：

- m2 探针 `{"result":"fail","reason":"noname"}`、m3 `noname`+`nodev`（m2.json/m3.json 与 full-test-logs/09-*，本会话核验）
- `integration-results/hci.snoop` 在 23:04 全流程跑完后仅 16 字节（纯文件头，**零 HCI 记录**，本会话核验）
- `docs/detailed-design.md` R-001（"Windows 蓝牙栈不对虚拟适配器下发 HCI，M2/M3 阻塞"）维持 **Open**

实验过程记录见工作流 run 报告（本报告不重复转述）。

### 6.2 全绿循环（第 2 轮达成）

第 1 轮的阻断失败集中在 check-tools（唯一 blocking 的修复对象）：7 条 known-defect 行全部修复其底层缺陷，顺带修好 m2/m3 真机腿的两个脚本级根因；第 2 轮完整 `tools/run-all.ps1` 重跑 → 全部阻断阶段 PASS（第五节结果表）。逐项修复（每项均以与 check-tools.ps1 完全相同的正则/夹具做了定点验证；**未单独重跑完整 run-all，完整重跑由循环统一执行**）：

| # | check-tools 行 | 修复 | 定点验证 |
|---|---|---|---|
| 1 | coverage-config-plumbing（评审 #51） | `tools/coverage.ps1:19` 由硬编码 `build\debug` 改为测量 `-Config` 实际构建的 exe（`build\$($Config.ToLower())\tests\phantombt-tests.exe`，删除死变量 $exe；本会话核验现行文本） | G2 正则 PASS；Debug/Release 两配置路径均解析到已存在 exe |
| 2/3 | run-m2/m3-attach-consistency | attach 由 `attach -r 127.0.0.1 -p 3240 -b 7-1`（usbip-win2 attach 无 -p；端口须为子命令前全局 -t，且 3240 为 uacsimd 保留）改为镜像已验证的 `Simulator::attach_commands`（src/sim/simulator.cpp:375-389，本会话核验）：`-t $Port attach -r 127.0.0.1 -b 7-1 --receive-mode low-latency`（tools/run-m2.ps1:41 现行文本，本会话核验） | G4 逻辑 PASS；实机 attach 成功（`succesfully attached to port 1`；`usbip port` 显示 Port 01 in use → usbip://127.0.0.1:3425/7-1, mode: low-latency） |
| 4 | sys-bt-check-fails-on-error | 加真实失败路径（ERR 回复/异常计数 → exit 1），头注释改为与行为一致（serve --load-bt），路径改 `$PSScriptRoot` 便携式，显式 `--port 3425`（CLI 默认 3240 撞 uacsimd）——现行 tools/sys-bt-check.ps1:1-13，本会话核验 | G6 PASS；实际运行 rc=0 'done'，sys-bt list / ble-clear / unload-all / status 四条全部 OK 框架 |
| 5 | gui-verify-path-portability（评审 #52） | `$exe/$out` 改由 `$PSScriptRoot` 推导 | G7 PASS；完整跑 `tools/gui-verify.ps1` → ALL GUI CHECKS PASSED, rc=0（无回归） |
| 6 | cov-gaps-excludes-foreign-and-excluded-dirs | `cov_gaps.py project_rel` 两处修正——绝对路径文件名不再与 base 拼接（修 'src/C:/Program Files/...' 误计）；src/ 根锚定改为"最后一次出现"且同时接受含盘符与不含盘符两种形态（修 exclB 双重嵌入）——现行 cov_gaps.py:37-69 docstring 固化两规则，本会话核验 | G8a+G8b 夹具复刻全 PASS；真实 coverage.xml 输出不变（96.21% ≥ 96.1，GATE PASS） |
| 7 | ci-workflows-committed | `git add .github/workflows/ci.yml` | 本会话核验：`git ls-files -- .github/` 列出该文件。按规约**未 commit/push，留待用户** |

**m2-realdevice 两个脚本级根因**：

1. run-m2.ps1 无 BOM UTF-8 中文注释被 PS 5.1 `-File` 按 GBK 误读，吞掉 `Start-Process` 反引号续行 → `'-ArgumentList'` 被当独立命令（已隔离复现：抽出原行在 -File 下直接解析错误）；按仓库惯例改纯 ASCII 注释修复。
2. attach 旗标错误（上表第 2/3 项）。

修复后 run-m2/run-m3 全程无脚本错误、serve 启动、attach 成功；剩余 FAIL(noname/nodev) 为第六.1 节的"真机待驱动"状态——WinRT BLE watcher 需无线电在 WinRT 口径下存活（m1 本身也仅 PASS(registry)），属 Windows 驱动层 vhci 导入无线电未活，**脚本侧已到头**。m2/m3 非阻塞，README 矩阵保持"待驱动"与之同步。

**额外防回归**：README M0/M1 行同步改"真机 ✅"（第二节 2.2），否则下一轮 `readme-matrix-syncs-with-results` 会变红；已用 section J 逻辑复刻验证 PASS。

**副发现（不阻塞，未深挖）**：裸 .NET `NamedPipeClientStream` 客户端（旧 sys-bt-check.ps1 的 StreamWriter/StreamReader 模式）会让 serve 静默死掉（复现 3 次；CLI `CommandClient` 不受影响）；sys-bt-check.ps1 已改用经过验证的 CLI 客户端。攻坚后的进程/内核侧已清理（无 phantombt 残留进程、vhci 无残留导入）。

**全绿循环实际执行的验证命令**（据循环记录；本报告撰写会话未复跑、仅核验其磁盘产物）：`rows.ps1`（G0/G2/G4/G6/G7/G12+ASCII 复刻）、`cov.ps1`（G8a/G8b 夹具复刻）、`readme-sync.ps1`（section J 复刻）、`tools/sys-bt-check.ps1`、`tools/gui-verify.ps1`、`tools/run-m2.ps1`、`tools/run-m3.ps1`（均完整跑）、`cov_gaps.py` 于真实 coverage.xml。未跑：完整 `tools/run-all.ps1` 的手工触发（按指示由循环统一重跑，产物即第五节 23:04 报告）。

---

## 七、遗留与建议

1. **M2/M3 真机（R-001，最高优先级遗留）**：HCI 零下发的驱动层根因未解（6.1 节）。突破=false 后建议方向：Windows 驱动层 vhci 导入无线电在 WinRT 口径下存活问题；m1 的 radios 口径同样阻塞于此。SMP c1 布局的真机裁决（R-002）依赖 R-001 解锁。
2. **CI 云端首跑未发生**：`.github/workflows/ci.yml` 已 `git add` 未 commit/push；工作区的全部修改与新增文件（变更 footprint 见下）同样待提交。推送后确认 windows-latest 上 OpenCppCoverage 探测与路径推导行为。
3. **uacsimd 端口共存（长期约束）**：本机 3240 被同机常驻项目占用；本仓一切 serve/attach 显式 `-t 3425`/`--port 3425`，两项目禁止互杀进程。新增脚本须延续该约定（sys-bt-check.ps1 本轮已补显式 --port）。
4. **named pipe 副发现**：裸 .NET pipe 客户端致 serve 静默死亡的根因未深挖（6.2 节）；当前以"脚本一律走 CLI CommandClient"规避，建议后续在 `src/control/pipe_server.cpp` 侧定位。
5. **覆盖率短板**：`src/core/bt_radio.cpp` 32.93%（27/82）为全仓最低——物理无线电 SetupAPI 枚举/开关路径依赖真机硬件，单测只能覆盖纯函数面；本机无物理无线电（check-tools 2 项 SKIP 同源），建议在有无线电的机器上跑一次 H3/全局设置真开关腿。
6. **feature-board 未落盘**：69 项逐项"已补齐"状态仅存在于工作流运行时工件；建议后续把矩阵导出为仓库内表格随报告归档，保证逐项可审计。
7. **建议的下一步验证顺序**：commit/push 触发 CI 云端首跑 → 在有物理无线电的机器复跑 `tools/run-all.ps1`（补齐 2 项 SKIP 腿）→ R-001 攻坚继续。

### 本轮 git 变更 footprint（未提交）

本轮工作流修改 30 文件（工作流输入清单，与 git status 快照一致）：`LastCoverageResults.log`、`README.md`、`docs/superpowers/plans/2026-10-29-phantom-bt-implementation.md`、`docs/superpowers/specs/2026-10-29-phantom-bt-design.md`、`docs/troubleshooting.md`、`src/ble/att.cpp`、`src/ble/device_stack.cpp`、`src/ble/device_stack.h`、`src/ble/hogp.cpp`、`src/ble/l2cap.cpp`、`src/ble/smp.cpp`、`src/ble/smp.h`、`src/bluetooth/adapter_model.cpp`、`src/bluetooth/adapter_model.h`、`src/cli/main.cpp`、`src/control/commands.cpp`、`src/control/commands.h`、`src/control/pipe_server.cpp`、`src/core/net.cpp`、`src/gui/main.cpp`、`src/hci/controller.cpp`、`src/hci/controller.h`、`src/hci/packets.h`、`src/hid/hid_model.cpp`、`src/hid/report_descriptor.cpp`、`src/probes/main.cpp`、`src/sim/btsnoop.cpp`、`src/sim/btsnoop.h`、`src/sim/simulator.cpp`、`src/sim/simulator.h`。工作区另外还带着今日早前轮次未提交的修改（git status 快照中的 `src/usbip/protocol.cpp`、`src/usbip/protocol.h`、`src/usbip/server.cpp`、`tests/CMakeLists.txt`、`tests/integration/test_ble_e2e.cpp`、`tests/unit/` 7 个既有测试、`tools/coverage.ps1`、`tools/gui-verify.ps1`、`tools/run-m0..m3.ps1`、`tools/sys-bt-check.ps1`）；`.github/workflows/ci.yml` 为新增并已 `git add`（未 commit）。

新增未跟踪：`docs/detailed-design.md`、`docs/global-review-2026-11-01.md`、`src/cli/serve_args.h`、9 个 `tests/unit/*_cov.cpp`、`tools/check-tools.ps1`、`tools/coverage-gate.ps1`、`tools/cov_gaps.py`、`tools/run-all.ps1`、本报告 `docs/full-test-suite-2026-11-01.md`。

---

**一句话结论**：功能盘点 69 项的缺口已由 102 个新单测与 90 项入口级检查接管，覆盖率 96.15% 过门禁、16 项缺陷清零，十阶段 run-all 第 2 轮全绿（M0 真机 ✅、M1 注册表口径 ✅）；唯一未攻下的是 R-001——Windows 蓝牙栈对虚拟适配器零 HCI 下发，M2/M3 真机段待驱动层突破。
