# phantom-bt 全局评审与测试完善报告

- **日期**：2026-11-01
- **范围**：覆盖率提升（基线→终态）、8 模块全局代码评审、高优修复、自动化门禁与 CI 交付
- **数据来源**：本次自动化运行的全部记录（覆盖率测量、评审逐条复核记录、全绿循环记录、高优修复与跳过说明、CI 三件套交付自检记录）。除标注"本会话核验"处外，所有数字均取自上述记录，未做外推；记录未提供的信息（如豁免逐行理由）在文中如实声明。
- **代码基线**：`main @ a6a8d1b`，工作区含本轮全部变更（新增 7 个 `*_cov.cpp` 单测、CI 三件套、src/ 修复等，均未提交）。

---

## 一、总评（项目健康度）

**结论：回环工程化程度高，但"对真实对端可用"这一层在评审中被证伪、修复后尚未经任何运行验证。**

覆盖率 87.56%→96.17%、剩余 133 行全部获批豁免（无未豁免残留）；三道门禁（覆盖率门禁 / 全量回归 / GUI 实测）第 1 轮即全绿且全程无需修复；CI 三件套（GitHub Actions + 覆盖率门禁脚本 + cobertura 解析器）交付并通过全部可本地执行的自检。但全局评审共提出 62 条发现、37 条经逐行复核确认，其中三条 wire 级缺陷（usbip op 阶段私有编码、RET 头 40/24B、SMP c1 镜像排布且不校验 confirm）意味着修复前**真实 usbip.exe / 真实 BLE 主机完全无法互操作**，而回环测试对此失明（测试与实现同构）。高优 7 条已完成修复，但按任务约束**未运行任何构建或测试**，且 6 个 tests/ 文件仍按旧实现写死、运行期必然失配——这是当前最高优先级遗留。

| 维度 | 状态 | 依据（数据来源） |
|---|---|---|
| 测试覆盖 | 🟢 绿 | 87.56%（2921/3336）→ 96.17%（3342/3475）；豁免 133 行 = 全部剩余未覆盖行 |
| 回归稳定性 | 🟢 绿（修复前口径） | 三道门禁第 1 轮全绿，逐轮修复记录：全程无需修复 |
| 评审整改 | 🟡 黄 | 62 条发现 / 37 条确认；仅高优 7 条修复，30 条已确认 medium 未修 |
| 协议互操作 | 🟡 修复完成、待验证 | usbip op/RET 头、SMP c1 三处与真实协议不兼容（已修复、零运行验证） |
| 真机路径 | 🔴 空白 | README M0..M3 均标注"真机待驱动"（HCI 流控、配对等真机路径未验证） |
| CI 自动化 | 🟡 待云端首跑 | 三件套静态自检全过；coverage-gate.ps1 端到端与 Actions 云端未跑 |

---

## 二、覆盖率：基线 → 终态

| 指标 | 基线 | 终态 |
|---|---|---|
| 行覆盖率 | 87.56% | 96.17% |
| 覆盖行 / 总行 | 2921 / 3336 | 3342 / 3475 * |

\* 终态行数取自 `tools/coverage-gate.ps1` 阈值注释所引的 2026-11-01 本地 `tools/coverage.ps1` 全量测量（`integration-results/coverage.xml`），与运行记录的终态 96.17% 一致。两轮总行数不同（3336→3475），系测量期间代码发生变更，未做逐行对账，行数差不可直接归因于补测。

- **提升幅度**：+8.61 个百分点。
- **豁免明细**：已获批豁免 **133 行**，恰等于终态未覆盖行数（3475 − 3342 = 133），即**无未豁免的未覆盖残留**——终态口径下每一行未覆盖代码都有获批豁免。
- **豁免理由归类**：逐行豁免理由未随本轮运行记录提供，**无法归类**（如实声明，不做臆测）。
- **测量口径**：OpenCppCoverage cobertura 导出；计入 `src/`，`gui/`、`cli/`、`probes/` 按惯例不计入（`tools/cov_gaps.py` 做防御性排除）。据 CI 自检记录第 4 项：cobertura 中全部 51 个 `<class>` 文件唯一且均落在 `src/`，无文件被静默丢弃。
- **本会话核验**：实际执行 `python tools/cov_gaps.py --threshold 96.1` → 输出 `96.17% (3342/3475) across 51 files`、`GATE: PASS`、退出码 0，与交付自检记录第 2 项逐位一致；未达标文件榜首为 `src/core/bt_radio.cpp`（42.68%，35/82）。

---

## 三、发现清单

**总览**：8 个模块共 **62 条**发现；复核确认（verified）**37 条**，未安排复核（unconfirmed）**25 条**——其中 1 条（#15）虽标 unconfirmed 但复核记录完整、结论为**驳回**。严重度分布：high 8 条（7 条成立 + 1 条驳回）、medium 30 条（全部 verified）、low 24 条（全部 unconfirmed）。

| 模块 | 发现 | 已确认 | high / medium / low（verified 数） |
|---|---|---|---|
| 核心基础库 | 6 | 1 | 0 / 1 / 5（1） |
| USB 与 usbip | 8 | 5 | 1 / 4 / 3（5） |
| HID 模型 | 8 | 4 | 1 / 4 / 3（4，其中 high 被复核驳回） |
| 蓝牙 HCI | 8 | 6 | 1 / 5 / 2（6） |
| BLE 协议栈 | 8 | 5 | 2 / 3 / 3（5） |
| 仿真与控制 | 8 | 4 | 0 / 4 / 4（4） |
| 工程基建 | 8 | 6 | 3 / 3 / 2（6） |
| 应用入口 | 8 | 6 | 0 / 6 / 2（6） |
| **合计** | **62** | **37** | **8 / 30 / 24** |

> **修复列依据**：运行记录仅给出"高优修复 8 项：修复 7、跳过 1"，跳过项为 tests/ 断言同步（非 src 发现）。"修复 7 条"与"8 条 high 中 7 条成立（#15 驳回）"数量吻合，据此将 7 条成立的高优发现标记为已修复；其中 wire 级行为变更（op_common 8B 与新 opcode、RET 头 48B、SMP c1 排布与 confirm 校验）由跳过项 reason 直接佐证，其余高优修复（#7/#24/#31）的逐条明细未随数据提供。**全部修复仅经逐行静态复核（以 Zephyr v2.0.0 `smp_c1` 源码等外部锚点交叉核对），未运行任何构建或测试**（任务统一门禁约束）。除高优外的全部 medium/low 发现本轮均未修复。

### 3.1 核心基础库（6 条，确认 1）

| # | 位置 | 问题 | 严重度 | 复核状态 | 修复情况 |
|---|---|---|---|---|---|
| 1 | `src/core/env_check.cpp:222` | VID 白名单被用作独立蓝牙判定轴（与函数注释自相矛盾），CH340 等非蓝牙 USB 设备误触发"检测到内置蓝牙"假警告；实例键打开失败时 VID 命中还可绕过 `instance_active` 检查 | medium | verified | 未修复 |
| 2 | `src/core/json.cpp:141-157` | `parse_number` 文法宽松且 `std::stod` 可抛非 runtime_error 类型，违反头文件声明的异常契约 | low | unconfirmed | 未复核 / 未修复 |
| 3 | `src/core/json.cpp:98-109` | 代理对低半部分未验证 DC00-DFFF，非代理低字回绕产生静默错码；孤立低代理编成非法 CESU-8 而非报错 | low | unconfirmed | 未复核 / 未修复 |
| 4 | `src/core/bytes.h:93-99` | `ensure()` 长度检查可被无符号加法回绕绕过（当前调用点不可达，属防御性缺口） | low | unconfirmed | 未复核 / 未修复 |
| 5 | `src/core/log.cpp:38` | sink 在全局互斥内被调用，且头文件未文档化"sink 不得重入 log_write"约束 | low | unconfirmed | 未复核 / 未修复 |
| 6 | `src/core/net.cpp:17-21` | `WsaSession` 忽略 `WSAStartup` 返回值，初始化失败只能从后续 socket 错误码间接推断 | low | unconfirmed | 未复核 / 未修复 |

### 3.2 USB 与 usbip（8 条，确认 5）

| # | 位置 | 问题 | 严重度 | 复核状态 | 修复情况 |
|---|---|---|---|---|---|
| 7 | `src/usbip/server.cpp:100-109` | `stop()` 在 join 连接线程之前 `delete ConnEntry`，线程退出路径仍写 `entry->ctx`——use-after-free；现有测试每次运行都触发该交错（实测用例通过，仅因无 ASAN 未崩溃） | high | verified | 已修复（高优轮，明细未随数据提供） |
| 8 | `src/usbip/server.cpp:243-245` | 线上可控的 `transfer_buffer_length` 直接驱动无上界 `data.resize()` 且无异常防护，构造一条 CMD_SUBMIT 即可使进程 `std::terminate`（上限 INT32_MAX≈2GB）；另实测监听 `htonl(INADDR_ANY)` 所有网卡，与 README"仅本机回环"口径不符 | medium | verified | 未修复 |
| 9 | `src/usbip/server.cpp:284-292` | 每端点仅 1 条挂起 IN URB，同端点第二条 IN 静默覆盖第一条的 seqnum，客户端该 URB 永不完成也无法 UNLINK（UNLINK 必失配、以 status=-1 应答） | medium | verified | 未修复 |
| 10 | `src/usbip/server.cpp:164-191` | 同一设备可被两个连接同时 import：第二次 `attach_sink` 无条件覆盖第一个连接的 sink，跨连接串扰、首连接 IN URB 永久悬挂（注册表无占用标记，全仓无守卫） | medium | verified | 未修复 |
| 11 | `src/usbip/server.cpp:335-338` | `ConnSink` 指向栈上 `ConnCtx` 且刻意泄漏，`on_detach` 后模型内 sink_ 悬垂；安全性完全依赖"complete_in 均在模型锁内"等三重未被接口强制的未声明约定 | medium | verified | 未修复 |
| 12 | `src/usb/descriptors.cpp:142-146` | 超长字符串描述符截断到奇数字节数 253，把最后一个 UTF-16 码元切成两半，产生畸形描述符 | low | unconfirmed | 未复核 / 未修复 |
| 13 | `src/usb/control.cpp:55-59` | 标准请求 CLEAR_FEATURE/SET_FEATURE（含 ENDPOINT_HALT 恢复）不被处理而落到 STALL；为此准备的 `stall_ep`/`on_stall_clear` 是死代码 | low | unconfirmed | 未复核 / 未修复 |
| 14 | `src/usbip/server.cpp:114-121` | accept 循环任何一次失败（含瞬时错误）即永久退出，但 `running` 仍为 true，服务器假活 | low | unconfirmed | 未复核 / 未修复 |

### 3.3 HID 模型（8 条，确认 4；其中 1 条 high 被复核驳回）

| # | 位置 | 问题 | 严重度 | 复核状态 | 修复情况 |
|---|---|---|---|---|---|
| 15 | `src/hid/report_descriptor.cpp:33` + `src/ble/hogp.cpp:30-37` | （原述）BLE 键盘 Report Map 声明 Report ID(1) 但 HOGP 通知不带前缀字节，违反 HIDS/HOGP 报告格式 | high | unconfirmed（复核记录完整） | **复核驳回，无需修复**：HOGP v1.2 §4.8.1 规定 Report ID 前缀/剥除发生在主机侧 GATT↔USB-HID 边界，GATT 特征值本身不含 Report ID（ID 由 0x2908 Report Reference 承载，本仓 `services.cpp:74` 已正确设置）；BlueZ/Zephyr 官方实现交叉验证一致。按原发现加前缀反而会造成键码错位 |
| 16 | `src/hid/hid_model.cpp:169-172` | `on_detach` 清 `pending_in_` 但不清 `sink_`，长期悬垂指向已销毁连接上下文；安全性依赖"完成仅发生在 pending_in_==true"隐含不变量（`stall_ep` 无 detached 检查，将来不经门禁的调用即 UB） | medium | verified | 未修复 |
| 17 | `src/hid/hid_model.cpp:138,146-149` | 持有模型互斥锁时同步调用 sink 回调，回调内做阻塞 TCP 发送（无 SO_SNDTIMEO），不读数据的 usbip 客户端可反压拖死 GUI 消息泵 / CLI 线程（锁序单向、无死锁） | medium | verified | 未修复 |
| 18 | `src/hid/hid_model.cpp:92-98` | GET_REPORT 完全忽略 wValue（报告类型/Report ID），空历史时默认回 8 字节全零——与描述符声明的任何报告（均带 ID 前缀、长度 9B/65B）都不匹配 | medium | verified | 未修复 |
| 19 | `src/hid/hid_model.cpp:90,113,35-36` | 类请求不校验 recipient/wIndex，SET_PROTOCOL 接受任意 wValue；`isubclass=0` 配 `iprotocol=1/2` 不符合 HID 1.11 §4.3（protocol 仅 boot 子类有意义）且原样导出 | medium | verified | 未修复 |
| 20 | `src/hid/report_descriptor.cpp:47-60` | 键盘模板 LED 段偏离规范 B.1 形态：单一 Num Lock 用法覆盖 5 个 LED 位，1 位字段继承 Logical Max 255 | low | unconfirmed | 未复核 / 未修复 |
| 21 | `src/hid/profiles.cpp:42-47` + `src/hid/hid_model.cpp:12-13,37` | `report_descriptor_override` 无任何校验：空串/类型错误 JSON 静默产生空描述符（wDescriptorLength=0 的坏设备），>65535 字节被 `(uint16_t)` 静默截断；vid/pid 类型错误报错不含字段名 | low | unconfirmed | 未复核 / 未修复 |
| 22 | `src/hid/hid_model.h:34-39,43` + `src/hid/hid_model.cpp:120,167` | 测试 API 绕过自身声明的互斥契约：`idle()`/`protocol()`/`dropped_reports()` 无锁读取锁保护状态，`on_stall_clear` 无锁写而 `on_control` 持锁写同一变量 | low | unconfirmed | 未复核 / 未修复 |

### 3.4 蓝牙 HCI（8 条，确认 6）

| # | 位置 | 问题 | 严重度 | 复核状态 | 修复情况 |
|---|---|---|---|---|---|
| 23 | `src/hci/controller.cpp:411-418` | LE_Read_Buffer_Size 返回参数多插一个 0 字节，规范主机把 Total_Num_LE_Data_Packets 解析成 0（真实包数 8 被顶出规范长度）。复核注：原 evidence 描述的是线程竞争、与 what 不匹配，但 what 本身成立 | medium | verified | 未修复 |
| 24 | `src/hci/controller.cpp:525-568` | 主机发起的 Disconnect 永远等不到 Disconnection_Complete；LE_Create_Connection_Cancel / LE_Connection_Update 同样无完成事件；断开后控制器状态永久 wedged（simulate_disconnect 仅测试调用，CMD_RESET 不清连接状态） | high | verified | 已修复（高优轮，明细未随数据提供） |
| 25 | `src/hci/controller.cpp:530-539` | 已有连接或已有挂起创建时，LE_Create_Connection 被静默吞掉但仍回 Command Status SUCCESS（规范应回 Command Disallowed 0x0C）；真实主机栈会挂起等待不会到来的 Connection Complete | medium | verified | 未修复 |
| 26 | `src/hci/controller.cpp:306-317` | 控制器从不生成 Number_Of_Completed_Packets 事件，而 Read_Buffer_Size 通告 32 包 / LE 通告 8 包：真机主机→控制器 ACL 流控在 N 个在途包后停摆（回环不受影响） | medium | verified | 未修复 |
| 27 | `src/bluetooth/adapter_model.cpp:17-45` | 蓝牙适配器配置描述符缺少设计 R3' 要求的 IAD 双接口聚合。复核注：机械事实成立，但 implementation plan 明示"2 接口无 IAD（单功能）"且代码忠实于 plan、IAD 构建能力已有单测——实质是 spec 与 plan 两份设计文档矛盾，定级 closer to low | medium | verified（定级商榷） | 未修复 |
| 28 | `src/hci/controller.cpp:517-521` | 六个 CS 型命令落入 switch default 先递增 `unknown_count_`，诊断计数把每次连接/断开都记为未知命令（计数仅测试消费，影响限诊断） | medium | verified | 未修复 |
| 29 | `src/hci/controller.cpp:525-527` | LE_Read_Channel_Map 被当 CS 型处理：既无 Command Complete 返回参数也无完成事件；LE_Create_Connection_Cancel 规范应回 CC 而非 CS | low | unconfirmed | 未复核 / 未修复 |
| 30 | `src/hci/packets.h:211-220` | `build_acl` 长度字段按 16 位截断且不校验，>65535 字节帧被静默截短；`fragment_to_acl` 的 247 切片与 `cfg_.le_acl_mtu` 无联动 | low | unconfirmed | 未复核 / 未修复 |

### 3.5 BLE 协议栈（8 条，确认 5）

| # | 位置 | 问题 | 严重度 | 复核状态 | 修复情况 |
|---|---|---|---|---|---|
| 31 | `src/ble/att.cpp:143` | ATT_WRITE_REQ/WRITE_CMD 载荷仅 1–2 字节时用 `[pdu+3, pdu+n)` 构造 vector，区间起点越过终点——UB 可致崩溃；主机 USB bulk-OUT ACL 全链路可达（重组器/L2CAP/ATT 均无最小载荷校验） | high | verified | 已修复（高优轮，明细未随数据提供） |
| 32 | `src/ble/smp.cpp:23` | SMP c1 的 p1/p2 字段排布与全部可互操作栈相反（preq/pres、iat/rat 位置镜像，ia/ra 占位互换），且运行时用全零地址算 confirm、从不校验主机 confirm——Just Works 配对对任何真实主机必然以 Confirmation Value Failed 失败（Zephyr v2.0.0 与 Linux 内核双源核实） | high | verified | **已修复·佐证**：c1 排布对齐互操作栈 + 引入 confirm 校验流程（跳过项 reason 明述） |
| 33 | `src/ble/att.cpp:96` | READ_BY_TYPE_RSP 截断长度用 mtu−3，实际 PDU 为 4+dlen，值 ≥ mtu−3 时超协商 ATT_MTU 一字节，违反 Vol 3 Part F 3.4.3.2（正确应 mtu−4） | medium | verified | 未修复 |
| 34 | `src/ble/smp.cpp:48` | Pairing Failed 应答 4 字节 `{0x05,0x01,0x00,0x0A}`，规范 Vol 3 Part H 3.5.5 仅 2 字节（opcode+reason）；合规主机把第 2 字节 0x01（Passkey Entry Failed）当原因并视为畸形 PDU | medium | verified | 未修复 |
| 35 | `src/ble/device_stack.cpp:46` | `controller_.on_l2cap_from_host`（std::function）在 usbip 线程无锁拷贝的同时被 GUI/pipe 线程赋值或析构置空——非原子并发读写构成数据竞争（UB），窗口小但真实 | medium | verified | 未修复 |
| 36 | `src/ble/smp.cpp:51` | Pairing Response 宣告 MaxEncryptionKeySize=0x07（规范下限 7 字节），与实际派生使用的 16 字节 STK 不一致，真实主机将按 7 字节协商链路密钥 | low | unconfirmed | 未复核 / 未修复 |
| 37 | `src/ble/hogp.cpp:30` | HOGP suspend 状态机是死代码：Control Point 写 0x00 置 suspended_ 后仍继续推送通知，且 HID Info 已宣告 remote-wake | low | unconfirmed | 未复核 / 未修复 |
| 38 | `src/ble/att.cpp:49` | 发现类应答容量算式偏保守一字节（(mtu−2)/7、/8 应为 /6、/7），每次少回一项（仅效率）；Read By Group 对非服务类型回 0x06 而非 0x10 Unsupported Group Type | low | unconfirmed | 未复核 / 未修复 |

### 3.6 仿真与控制（8 条，确认 4）

| # | 位置 | 问题 | 严重度 | 复核状态 | 修复情况 |
|---|---|---|---|---|---|
| 39 | `src/sim/btsnoop.cpp:14-19` | btsnoop 时间戳把 FILETIME 微秒（自 1601）直接加上纪元常量，且常数为 BlueZ 正确值 0x00E03AB44A676000 的错抄——所有记录恒快约 369 年。复核按两处误差精确复算：实际约 **399 年**（如 2026-11-01 包显示为 2425-10-31），比原发现更糟；文件其余格式 Wireshark 兼容 | medium | verified（holds，偏差更大） | 未修复 |
| 40 | `src/control/pipe_server.cpp:45-60,67-68` | 有客户端保持连接（阻塞在 ReadFile）时 `PipeServer::stop()`/析构无限期挂起：单实例管道使唤醒用的 CreateFileA 持续 ERROR_PIPE_BUSY，join 无超时、无 CancelSynchronousIo（全 src/ grep 证实） | medium | verified | 未修复 |
| 41 | `src/sim/simulator.cpp:320-327`（配合 `pipe_server.cpp:22-24`） | `cmd_attach` 把未校验 busid 直接内插 `_popen` 的 cmd.exe 命令行，`attach 7-1&<命令>` 即注入执行（实测 `&` 后命令独立运行）；管道以默认安全描述符创建，attach 场景服务端常以管理员运行——构成提权链 | medium | verified | 未修复 |
| 42 | `src/sim/simulator.cpp:342-356` | `detach <busid>` 完全忽略传入 busid，固定执行 `usbip.exe detach --all`，一次卸载主机端全部已 attach 设备；`_popen` 失败仍返回 "detached"——与命令语法/README/commands.h 注释三处承诺不符 | medium | verified | 未修复 |
| 43 | `src/sim/simulator.cpp:110,154,204,247-251` | 加载第二个 BT 适配器后 BLE 编排不一致：DeviceStack 绑定最新控制器，但 ble-advertise/ble-type/has_connection 作用于 map 序首个适配器；unload 清空 ble_devices 却只移除被卸载适配器的广播源 | low | unconfirmed | 未复核 / 未修复 |
| 44 | `src/sim/btsnoop.cpp:60` | 所有 btsnoop 记录 flags 恒为 0，主机↔控制器方向在抓包中彻底丢失（ACL 双向同型时无法区分） | low | unconfirmed | 未复核 / 未修复 |
| 45 | `src/control/pipe_server.cpp:108-117` | `CommandClient::send_line` 以任意一批读取后 acc 末字符为 '\n' 即提前返回，多行大应答（>1024B）在 1024 字节读取边界恰逢行尾时截断丢失后续行 | low | unconfirmed | 未复核 / 未修复 |
| 46 | `src/sim/simulator.cpp:206-213` | HCI 抓包硬编码 CWD 相对路径 `integration-results/hci.snoop`，目录不存在时静默失败；实施计划要求的 `snoop <path>|off` 控制命令未实现 | low | unconfirmed | 未复核 / 未修复 |

### 3.7 工程基建（8 条，确认 6）

| # | 位置 | 问题 | 严重度 | 复核状态 | 修复情况 |
|---|---|---|---|---|---|
| 47 | `src/usbip/protocol.h:13-16` / `protocol.cpp:5-9` / `server.cpp:148,154,197` | usbip op 阶段（DEVLIST/IMPORT）用私有编码：无 version 字段、opcode 为 0x80000000 系——真实 usbip.exe 首个请求（首 4 字节 0x01118005/0x01118003）即被断开，该功能对真实客户端完全不可用；已独立抓取 cezanne/usbip-win 上游源码核实（op_common 8B、USBIP_VERSION 0x0111、OP_REQ_DEVLIST=0x8005/OP_REQ_IMPORT=0x8003） | high | verified | **已修复·佐证**：op_common 8B + opcode 0x8005/0x0005/0x8003/0x0003（跳过项 reason 明述） |
| 48 | `src/usbip/protocol.h:61-62` / `protocol.cpp:68-95` / `server.cpp:148,154,197` | RET_SUBMIT/RET_UNLINK 线头发 40/24 字节，协议头部（union 定长）为 48 字节（Linux stub_tx 与 usbip-win2 static_assert(48) 双源核实）——真机第一个 URB 应答后字节流即错位，枚举失败；回环测试按 40/24 读取故自洽全绿 | high | verified | **已修复·佐证**：RET 头统一 48B（跳过项 reason 明述） |
| 49 | `src/ble/smp.cpp:23-26` + `tests/unit/test_aes_smp.cpp:143-160` | SMP c1 的 p1 布局与 BT Core Spec 相反（preq/pres 与 iat/rat 两对均互换，preq[0]=0x01≠pres[0]=0x02 故互换永非空操作），真机 Just Works confirm 校验必失败——与 #32 同源（跨模块重复发现）；复核另发现 p2 布局同样相反，偏差比原述更广 | high | verified | **已修复·佐证**：同 #32（c1 排布 + confirm 校验） |
| 50 | `tests/unit/test_coverage_extra.cpp:14,35` | 两处恒真断言（`parse(x)==parse(x)`、`full.address==full.address`），无鉴别力；复核另发现第 35 行期望常量本身与实现不符，修复时需一并更正 | medium | verified | 未修复（位于 tests/，本轮任务禁改） |
| 51 | `tools/coverage.ps1:14,16,24` | `-Config` 参数失效：按 $Config 构建却固定对 `build\debug` 二进制出报告，$exe 算了不用——`-Config Release` 时构建 release 目录却对 debug 产物出报告 | medium | verified | 未修复 |
| 52 | `tools/gui-verify.ps1:7-8` / `tools/sys-bt-check.ps1:3-4` | 两脚本硬编码本机绝对路径 `C:/tjf/github/phantom-bt/...`，换任何检出路径即失败或驱动错误二进制（其余 tools/*.ps1 均用 $PSScriptRoot 推导） | medium | verified | 未修复 |
| 53 | `README.md:36` vs `build.bat:15` | 无参默认构建配置文档矛盾（README 说 Debug，build.bat 实际 Release） | low | unconfirmed | 未复核 / 未修复 |
| 54 | `CMakeLists.txt:17` / `tests/CMakeLists.txt:1-41` | 根 CMakeLists 声明 `enable_testing()` 但 tests/ 未注册任何 `add_test`，CTest 集成为空壳 | low | unconfirmed | 未复核 / 未修复 |

### 3.8 应用入口（8 条，确认 6）

| # | 位置 | 问题 | 严重度 | 复核状态 | 修复情况 |
|---|---|---|---|---|---|
| 55 | `src/cli/main.cpp:260`（同 :266） | CLI 数字参数缺值/非法值时 `std::stoi` 抛未捕获异常直接 abort 而非输出用法——复核实测：进程带 VC++ 运行库模态框挂死、stdout/stderr 为空，需强杀 | medium | verified | 未修复 |
| 56 | `src/cli/main.cpp:79-96` | `--probe` 在缺 `--attach` 或找不到 usbip.exe 时被静默跳过，serve 仍以 0 退出——退出码 0 混淆"probe 通过"与"probe 从未执行"，验收链路假绿（run-m0.ps1 有预检，README 直用路径无保护） | medium | verified | 未修复 |
| 57 | `src/probes/main.cpp:72-86` | `finish()` 以字符串拼接构造 JSON 且不转义引号/反斜杠/控制字符，邻近 BLE 设备可注入的 LocalName 等空口数据可把 R5' 的结构化输出破坏成非法 JSON（当前 m*.ps1 仅按退出码判定，影响限探针输出契约） | medium | verified | 未修复 |
| 58 | `src/gui/main.cpp:347`（同 :331、:386、:399、:409） | GUI 按普通拼接构造控制命令，而 `execute()` 按空白切分并要求精确参数个数——含空格的 profile 路径、多词 ble-type 文本全部失败且报误导性 "ERR unknown command"；复核用 cl.exe 编译 split() 实机验证 | medium | verified | 未修复 |
| 59 | `src/gui/main.cpp:404-410` | ble-type 把文本框 wchar_t 逐位窄化（非 ASCII 变乱码），且目标地址硬编码 C0:11:22:33:44:55，只对默认键盘 profile 有效 | medium | verified | 未修复 |
| 60 | `src/cli/main.cpp:46-47`、`src/gui/main.cpp:806` | 两前端忽略 `PipeServer::start()` 恒返回 true 的返回值：serve 与 GUI 并存时第二实例每 200ms 报 ERROR_PIPE_BUSY，且 `phantombt cmd` 静默路由到另一进程的 bench（实例归属随时间漂移） | medium | verified | 未修复 |
| 61 | `src/gui/main.cpp:719-720` | 状态栏字符串改写从 "port=" 删 4 字符留下孤立的 `=`，渲染为 "usbip-=3240·设备 2" | low | unconfirmed | 未复核 / 未修复 |
| 62 | `src/cli/main.cpp:140,154`（路径依赖 :120） | selftest 两处 `recv_exact` 返回值未检查，短读会被误报为后续阶段失败；且依赖 CWD 相对路径 profiles/vendor.json | low | unconfirmed | 未复核 / 未修复 |

### 3.9 表后补充说明

1. **#15（HOGP Report ID）为唯一经复核驳回的发现**：复核记录引用蓝牙 SIG HOGP v1.2 §4.8.1 原文并以 BlueZ master / Zephyr 官方样例交叉验证——现实现（通知不带 Report ID 前缀、ID 由 0x2908 描述符承载）符合规范，原发现建立在反向规范解读上。数据中其 status 字段仍为 unconfirmed，本报告按复核结论标注"驳回"。
2. **#32 与 #49 为同源重复发现**（BLE 协议栈与工程基建评审各自独立报出 SMP c1 排布问题，复核均以独立外部源确认），修复为同一处。
3. **#27（IAD）与 #23（LE buffer size）** 复核均确认机械事实成立，但前者指出实为 spec 与 implementation plan 的文档矛盾（代码忠实于 plan），后者指出原 evidence 与 what 不匹配——两条均维持 verified、未修复。
4. **#39（btsnoop 时间戳）** 复核确认结论成立且实际偏差（约 399 年）大于原报告（约 369 年）：纪元基数误用（+369 年）叠加常数错抄（+30 年）。
5. **修复的运行验证缺口**：跳过项 reason 明确声明"按任务要求未运行任何构建或测试（脚本统一门禁），所有修复仅经逐行复核 src/ 全部调用点（server.cpp / protocol.cpp / cli/main.cpp 为 wire 层全部消费方），并以现场抓取的 Zephyr v2.0.0 smp_c1 源码为互操作锚点"。本报告未对修复后代码做任何二次运行验证。

---

## 四、自动化测试与全绿循环

### 4.1 单测数量（按运行记录原样）

- 项目原始 **183 个** → 当前 **1 个**（该字段计数口径未在运行记录中说明，如实引用原值）；
- 各轮补写合计新增 **24 个**（首轮补写已含在续跑起点统计中）；
- git 变更佐证（本会话核对工作区）：新增 7 个覆盖率补测文件 `tests/unit/test_{ble,bt_hci,core,hid,infra,sim_control,usb_usbip}_cov.cpp`（本会话确认 7 个文件均存在），修改 6 个既有测试（`test_ble_e2e.cpp`、`test_aes_smp.cpp`、`test_hid_model.cpp`、`test_report_descriptor.cpp`、`test_usbip_protocol.cpp`、`test_usbip_server.cpp`）及 `tests/CMakeLists.txt`。

### 4.2 三道门禁与全绿循环

| 门禁 | 内容 | 结果 |
|---|---|---|
| 1. 覆盖率门禁 | `tools/cov_gaps.py --threshold 96.1`（阈值来源：本轮终态 96.17% 向下取整到 0.1%，注释明示"只升不降"） | ✅ 第 1 轮即绿 |
| 2. 全量回归 | 全部单测 + 集成测试 | ✅ 第 1 轮即绿 |
| 3. GUI 实测 | GUI 路径实测，全部退出码 0 | ✅ 第 1 轮即绿 |

**逐轮修复记录：全程无需修复**（全绿循环在第 1 轮达成后，后续各轮均无修复动作）。

### 4.3 CI 与门禁三件套（本轮交付）

本会话核验：三个文件均存在于工作区（`.github/workflows/ci.yml`、`tools/coverage-gate.ps1`、`tools/cov_gaps.py`）。以下细节引自交付说明。

**(1) `tools/cov_gaps.py`** —— 自包含 cobertura 解析器（仅 stdlib：xml.etree/json/argparse）：
- 兼容 OpenCppCoverage 导出形状（`<source>C:</source>` + 盘符相对 filename），归一化后以 sources/repo/src 三种基准拼绝对路径；`src/` 前缀包含判断，防御性排除 gui/probes/cli；
- 按文件聚合 covered/total/未覆盖行号（同行 branch 记录取 max hits）；写 `integration-results/cov-gaps.json`（键：overall/pct/total_lines/covered_lines/threshold_pct/pass/files）并打印摘要；
- 退出码：0 通过 / 1 低于阈值（列未达标文件及未覆盖行号）/ 2 输入错误。

**(2) `tools/coverage-gate.ps1`** —— 阈值常量 `$ThresholdPct=96.1` 置顶（注释注明来源与"只升不降"策略）。流程：`build.ps1` 构建 Debug → 按与 coverage.ps1 相同顺序探测 OpenCppCoverage（PATH → LOCALAPPDATA → Program Files）→ 置 `_NO_DEBUG_HEAP=1` 后测量 → cobertura 导出到 `integration-results/coverage.xml`（路径由 $PSScriptRoot 推导，无硬编码）→ 退出码 0 或 3 均视为测量完成（3=被测程序异常退出但报告已生成）→ 透传 `python tools/cov_gaps.py --threshold` 退出码。测量前删除旧 xml 防陈旧误判。

**(3) `.github/workflows/ci.yml`** —— windows-latest + `actions/setup-python@v5` + `ilammy/msvc-dev-cmd@v1`(x64) → `cmake --preset msvc-debug && cmake --build --preset debug` → 直跑 `build/debug/tests/phantombt-tests.exe` → `choco install opencppcoverage` → `tools/coverage-gate.ps1` → `actions/upload-artifact@v4` 上传 coverage.xml + cov-gaps.json（`if: always()`，门禁失败也上传）。yml 顶部注释说明两项不跑及原因：真机段（需蓝牙/USB dongle + usbip-win2 驱动 + 管理员）、`gui-verify.ps1`（需交互桌面会话，runner 仅非交互 service 会话）。

### 4.4 交付自检（据自检记录：下列 1–6 全部实际执行）

| # | 检查 | 结果 |
|---|---|---|
| 1 | `python -m py_compile tools/cov_gaps.py` | OK |
| 2 | 以仓库真实 cobertura 跑 `python tools/cov_gaps.py --threshold 96.1` | 96.17% (3342/3475)、51 文件、GATE PASS、exit 0；总数与 XML 头 line-rate 逐位一致 |
| 3 | 阈值 99.9 失败路径 | exit 1，列出未达标文件 + 未覆盖行号 |
| 4 | 交叉核验 cobertura | 51 个 `<class>` filename 唯一、均在 src/、无一落在 gui/probes/cli（此前 grep 数出 52 系误计 `<classes>` 容器标签） |
| 5 | PowerShell AST `Parser::ParseFile` 检查 coverage-gate.ps1（PS 5.1.26100） | 0 错误 |
| 6 | `pyyaml safe_load` 检查 ci.yml + 三个命令行过 `ScriptBlock::Create` | 首轮抓到 `run: & .\...` 的 `&` 被当 YAML 锚点，加引号修复后通过 |

**本会话复验**：实际执行 `python tools/cov_gaps.py --threshold 96.1` → `96.17% (3342/3475) across 51 files`、`GATE: PASS`、exit 0，与自检记录第 2 项一致。

**未跑（自检记录如实声明）**：`coverage-gate.ps1` 端到端（构建 + OpenCppCoverage 测量全流程）、GitHub runner 上的工作流本体。缓冲依据：现有 coverage.xml 正是本机 OpenCppCoverage 用同样 sources/excluded_sources/export_type 形状产出，调用形状有实证。另：失败路径测试曾把 cov-gaps.json 写成 FAIL 态，已用 96.1 阈值重跑恢复 PASS 态（integration-results/ 在 .gitignore，不入库）。

### 4.5 云端与本地差异（已在交付物内消解）

- 检出路径 `D:\a\<repo>\<repo>`（CI）vs `C:\tjf\github\phantom-bt`（本地）：cov_gaps.py 全部路径从脚本自身位置推导，兼容盘符相对与绝对两种 filename，无绝对路径依赖；
- runner 预装 MSVC/Ninja/CMake，`msvc-dev-cmd` 提供 VsDevCmd（本地 build.ps1 自带 vswhere 自举，CI 内 gate 的重建为 no-op）；
- OpenCppCoverage 云端 choco 安装、本地 winget/手工，同一探测逻辑兼容；
- Python 云端由 setup-python 固定，本地用系统 PATH python（gate 中缺 python 显式报错 exit 1，不静默跳过）。

---

## 五、遗留与建议

### P0（阻断下轮回归，需最先处理）

1. **tests/ 断言与修复后的实现同步改写**：`tests/unit/test_usbip_protocol.cpp`、`test_usbip_server.cpp`、`test_usb_usbip_cov.cpp`、`test_sim_control_cov.cpp:48`、`test_aes_smp.cpp`、`tests/integration/test_ble_e2e.cpp` 按发现证据与被修复前的实现同构（4B op 头 / 0x80000000 系 opcode / RET 头 40B·24B / SMP 任意 confirm+Mrand 即判 paired / c1 旧镜像排布）。修复仅改值与字节排布（函数签名与常量名未变），**这些测试仍可编译但运行期必然失配**。需由负责 tests/ 的步骤按真实协议（op_common 8B、opcode 0x8005/0x0005/0x8003/0x0003、RET 头 48B）与 SMP confirm 校验流程改写（跳过项 reason 原文）。
2. **高优 7 项修复的运行验证**：修复仅经逐行静态复核（含外部锚点交叉核对），未运行任何构建/测试。建议顺序：全量构建 → 上述 tests 改写 → 全量回归 → 三道门禁复跑（含本会话复验过的覆盖率门禁）。

### P1（近期）

3. **CI 云端首跑 + coverage-gate.ps1 端到端**：工作流本体与门禁全流程均未实际运行（仅静态自检 + 解析器实跑）；首次推送后确认 windows-latest 上 OpenCppCoverage 探测与路径推导行为。
4. **已确认未修复的 30 条 medium 形成 backlog**，建议优先处理安全/可用性相关的：#41（管道命令注入 + 提权链）、#8（2GB resize 远程 DoS，且监听 0.0.0.0 与 README 口径不符）、#40（PipeServer stop 挂起）、#60（双实例管道互扰）、#35（数据竞争 UB）、#16/#11（sink 悬垂结构性风险）。
5. **真机 M0..M3 驱动**：README M0..M3 均标注"真机待驱动"；本轮多条已确认发现（#24/#25/#26 HCI 事件与流控、#32 配对）恰落在回环测不出、真机必现的路径上，修复后应以真机验收闭环。
6. **24 条 low 未复核发现**：安排批量复核关闭（其中 #53/#54/#61 等纯文档/小修可顺手处理）。

### P2（改善项）

7. `tools/gui-verify.ps1` / `tools/sys-bt-check.ps1` 硬编码绝对路径参数化（#52），与 CI 路径自推导口径拉齐。
8. 澄清单测计数口径（"183 → 1"字段含义）并归档 133 行豁免的逐行理由，保证覆盖率数字可审计。
9. 文档矛盾清理：#27（spec R3' IAD 括注 vs implementation plan"无 IAD"）、#53（README vs build.bat 默认配置）、#8 附带的监听口径（README"仅回环" vs `INADDR_ANY`）。
10. `tools/coverage.ps1` 的 `-Config` 失效（#51）与 CTest 空壳（#54）择机修复，使本地与 CI 测量入口一致。

---

**一句话结论**：覆盖率与门禁基建已达 96.17%/三绿的高水位，评审把"回环全绿"与"真机可用"之间的鸿沟（usbip op 编码、RET 头、SMP c1 三处 wire 级不兼容）完整暴露并完成修复，但修复本身零运行验证、tests/ 断言待同步、30 条已确认 medium 未修——项目当前处于"修复已落地、等待第一次真实回归"的关键节点。
