# GOAL：完成 ELMP v2 剩余阶段（P2 → P5）

> 本文件是 `/goal` 的完整计划。每一轮开始工作前先重读本文件与下面「恢复上下文」清单；计划有变先改本文件（标日期）。
> 完成标准：P2、P3、P4、P5 的门槛全部在真实隔离实例上通过并写入 kb，产出 `dist/ELMP-<版本>-shipping.zip`（零 `.lua`）与 `-dev.zip`，README / INSTALL / ADR 0005 更新完毕。
> 只有在全部完成、或某阶段门槛经充分尝试仍无法达成（写明尝试、证据、剩余选项）时才报告 goal 完成或 blocked。需要我决定的产品问题：先按需求树已有定案与 fail closed 默认继续其他工作，把问题集中列出，不要停下等。
> **进度以 §7 的勾选表为准**：每完成一项就在该行打勾并补「日期 · 证据（kb 节 / 截图 / 命令输出）· commit」；下面「当前进行中」一行同步更新。goal 完成 = §7 全部打勾。

**当前进行中**：§2 P2 实现 —— net + `coop/session`（`elmp lan host <code> [port] [max]` / `join <addr> <code> [port]` / `leave` / `status`，`coop::set_handlers(Role, Handlers)` 分发）已在 Host2/Client2 跑通（`b6ac40b`）；`net/snapshot`（Frame = l/e/ed/w/pk 五节，encode/decode 增量 + CTest，`096b186`）、`net/protocol`（Input / Welcome / NameDef / Map / MapReady / Act / Json(Build·Equip) / Control + NameTable，`5304e2c`）完成；`guard/guard`（arm/disarm/mark/mark_not_player/open_replay，9 个 detour，`fbded06`，尚未实例验证）完成；`party::Source::Network`（`c79f4ca`）；`coop/host.cpp`（`9b58b34`：客人先发 Build{"guestKey":...} 才落座 → Welcome{seat,epoch,map}；输入流 2 → pad；快照流 1；Map 跟图；会话函数改名 `coop::start_host/start_join`）完成但未实例验证；`coop/client.cpp` 首版（`8f0f5b5`）已在 Host2/Client2 跑通：P2 network 座位、快照 acked、G2 输入链路取证（net-facts「P2 首次联机切片」，`f5dfef7`；实例仍在运行、两侧槽 0、会话已连，Client 已 `pad allow1 on`）。下一步：church_07 敌人接管 / G1 计数、Host P1 副本（l 行）、Spine 姿态、act 通道、跟图 SwitchGameMap、vitals、本地 P1 隐藏锚点、联机页。旧步骤：停 Host2/Client2 → deploy → 两边读同一档槽 0 → `elmp lan host 4821` / `elmp lan join 127.0.0.1 4821` → 查 `elmp lan status`（host 应有 P2 network 座位、client taken>0）→ client 端 `elmp pad allow1 on` + `elmp hold 1 ...` 推 P1 输入 → host 端 `elmp lily 2` 看动画序列（G2）。原设计备注：（connected → 发 Build（guestKey 持久化在 Saved/ELMP/ELMP/guest.json）；收 Welcome 记 seat/epoch；每帧采本地 P1 输入（虚拟手柄状态 / InputSnapshot）→ Input 上行带 snap_ack；收快照按 base 解码（客户端也存 32 帧环）；Map → 原版 SwitchGameMap 跟图后回 MapReady；guard::arm、敌人接管冻结 + mark；lily 行驱动 Host 副本）、`elmp lan` 增加 host/client describe，然后 Host2/Client2 实测 G2。跟图入口事实：`WorldLoaderSubsystem::SwitchGameMap(const FDataTableRowHandle& GameMapHandle, TSubclassOf<UFadeUserWidget> FadeOut, TSubclassOf<UFadeUserWidget> FadeIn, FName NextPlayerStartTag)`；DataTable 指针可取任一 `BP_WorldTravelVolume_C.GameMapToLoad.DataTable`（registry TravelVolume 桶），RowName = Host 地图 id；只在本地 `IsLoading=false && IsGameMapReady` 时调用（v1 net-facts「跟图」）。Client 读本地 P1 输入：`schema zc_input` + `binary_checks::offset("ZenithInputComponent.InputSnapshot")` → `input::InputSnapshot{input_x,input_y,flags(本帧按下沿),raw}`（`party::Source` 加 Network 远程座位，`input::pad::set/tap` 喂输入）/ client → guard → host capture / remote seat → client takeover / apply → adapters（2026-10-26）
**需要用户**：（无）

## 0. 恢复上下文（每轮先读）
1. `AGENTS.md`（规则、资料路径与证据等级、常用命令）。
2. `README.md`「当前阶段」——P0、P1a–P1f 已完成并提交（`f611b5e`），P2 未开始。
3. `docs/REQUIREMENTS-TREE.md`：§2.3 F2.1–F2.13、§2.4 F3.*、§2.5 F4.*、§3 C14–C20、§4 M11/M17–M24、§5 E5/E6/E9、§7（7.1 后台图 Boss fail closed、7.2 闸门矩阵、7.3 单值假设清单、7.9/7.10 SDK 与命中规则、7.13–7.16 P1 实施决定）、§8 验收口径与性能门槛。
4. `docs/ARCHITECTURE-V2.md`：§3 线程模型、§5 注册表、§6 Hook 总表、§7.5 coop / §7.6 modes、§8 类型化协议、§9 多地图、§11 Dev、§12 SDK、§13 验证策略、§15 P2–P5 门槛、§16.1 缓存清单。
5. `docs/kb/engine-facts.md`、`core-facts.md`、`party-facts.md`（P1f：原版交互入口、存档登记降频、闸门矩阵本地列、帧时间基线）。
6. 现有代码接口：`native/engine/{reflect,registry,frame,lifecycle,world,handle}.h`、`native/base/{events,seqlock,settings,binary_manifest}.h`、`native/core/*.h`、`native/party/*.h`、`native/pvp/hitgate.h`、`native/overlay/model.h`、`native/console/console.h`、`data/binary/EL.json`、`data/gates.json`。新模块一律建在这些之上，不另起反射 / 帧泵 / 注册表。
7. 旧仓库知识（只读，不复制代码）：`D:\CodeT\ELMP\docs\kb\net-facts.md`（传输选型、结算拦截、呈现化接管、敌人冻结三闸、世界对象对账、动作通道、多对端、多地图后台加载实验、重构后验收缺陷）、`docs/adr/0003-net-coop.md`、`0004-multimap-multiguest.md`、`docs/REQUIREMENTS.md` §11–§14、`native/net/*`、`native/guard/*`、`native/world/level_instance.cpp`、`lua/elmp/coop/**`、`lua/elmp/modes/hunter.lua`；`D:\Mod_Investigation\HKSS\{HKMP,SSMP}` 协议设计出处。

## 1. P1 遗留（先于 P2 或并入 P2 初期）
- `pickup` 闸门未验证：找有物品掉落（可破坏物 / 白花 / 货币）的图，`elmp perm <slot> pickup off|on` 验证 `UAutoCollectComponent` 路径，补进 party-facts 与 §7.2。
- 精灵升级收益经共享进度同步到 PX（`SpiritLevels`）补一次实测。
- **敌人激活入口重新定位**（P2 前置）：`0xCEFAE0` 已证实是 `AZenithSpirit::SetActive`，v1 三闸的第一闸无效。用 IDA（`idb_open` `{game}\Binaries\Win64\EnderLiliesSteam-Win64-Shipping.exe.i64`）+ `elmp native` 从 `BP_Character_Enemy_Base_C:SetPaused`、`AEnemySpawnPoint::ActivateEnemy`、`APawnActivationVolume::OnBeginOverlap`、相机钳制 `RegisterEnemyController` 四条链找真正的原生激活汇点，写进 `EL.json`（前缀 + 来源）与 `engine-facts.md`；找不到单汇点就记录“`SetActorEnableCollision/SetActorTickEnabled` 两原语闸 + `CanBePaused=false` + 1 Hz 复检”为唯一有效方案并实测泄漏数。

## 2. P2 同图联机（Host2 / Client2）
实现：
- `native/net`：自研 UDP + Windows CNG（ECDH P-256 / HKDF-SHA256 / AES-256-GCM，配对码参与派生）；不可靠最新通道 + 可靠有序通道（ACK = 最新连续序号 + 64 位位图，RTO 100 ms–2 s ×1.5 退避，8 s 无进展断链）；MTU 1200 分片；全部队列有界（沿用 v1 FR-NET-5a 的边界：可靠待发 ≤256、出站 ≤512、重组组 ≤32、收件箱 ≤512 先丢快照后丢可靠、ACK 只在变化时写、重复可靠消息触发 ACK 重发、PONG ≤4、饱和减法）；`F_CLOSE`；**多对端表从一开始就做**；握手期异常边界（坏包计数，绝不外抛）。
- `native/coop`：typed 位掩码增量快照（§8），字符串只走 `NameDef` 名字表；`FrameSnapshot` 预分配零堆分配；`session` / `world_model` / `host/{capture,guest,events,maps 骨架}` / `client/{takeover,apply,presenter,vitals}` / `entities/{enemy,enemy_dyn,lily,worldobj,story,pickup}` 实现 `IEntityAdapter`（claimHost / scanDoneHost / writeHost / claimClient / takeover / observe / render / onEvent / reset / unpuppetAll）；行驻留语义（缺席 ≠ 消失，GONE ≈ 1 s）；卸载 ≠ 死亡（只有最后采样 hp≤0 才带 dy）。
- `native/guard`：结算拦截（`DoDamage`、技能伤害）、`IsPlayer` 跳板、`SaveGame/SaveGameAsync`、碰撞 / tick 原语闸 + 新定位的激活闸；回放窗口按目标放行；Client 侧 `CanBePaused=false`、激活体积源头冻结、1 Hz 两标志复检。
- 动作通道：`AbilityLaunchFromDesc` detour pre 取参、白名单字段、按敌人 seq 幂等、可靠下发、Client 同一原版入口重放（`IsChildOf(/Script/Zenith.Ability)` + 值域校验）。
- 远程座位 = Host 上的本地座位同构：输入 kind=network、不进物理路由、不进共享镜头、只保留 fell 召回、Host `party` 巡检统一复活；500 ms 停滞松键；跟图闸门 `map/mapReady`（回执前不投入客人 Pawn）；晚加入 / 断线重连保留座位。
- 客人进度与构筑归属（FR-NET-2c）：世界进度归 Host 存档；客人构筑按 `guestKey` 存在 Host 的 `party_<saveId>.json`；Client 本地原版档全程只读（存档保护）。
- 客人装备通路（F2.10）：Client 打开同一 ImGui 菜单，请求经可靠通道由 Host 按原版规则执行并回传。
- 管理器“联机”页（F2.1）：主持（端口、配对码）、加入（地址、配对码）、玩家列表、连接状态与拒绝原因、离开。
- FR-NET-8 版本一致：EXE / `main.dll` / `data` 内容哈希 / 协议版本全等，拒绝原因送达发起方，满员回 Full。
- Client 相机与伴随体规则（FR-NET-3c）、本地座位压制（FR-NET-3d）、Client HUD 用原版 HUD 显示 Host 权威数值（`CurrHP/CurrMP` 属性直写，祈祷用 `UseHeal/RestoreHeal` 计数对齐）。

门槛（Host2/Client2 同机双实例）：
- G1：Client 无 AI 决策（三闸 + 复检计数可观测，泄漏 0）；G2：客人输入经 Host 原版管线（动画序列取证）；G3：敌人数量 / 位置 ≤5 UU / HP / 门 / 宝箱 / 可破坏物（Client ⊆ Host）/ 剧情旗标 / Boss 窗口；G4：60 / 144 / 波动三档帧时间 + 截图可见 Spine 动画与弹体。
- 晚加入、断线（非主动离开）重连、Host P1 死亡重生 / 切图时 Client 不把卸载当死亡、离开后本地 P1 数值还原、联机 PvP。
- §7.2 矩阵补“Host 上远程座位”与“Client 副本”两列。
- 性能：Host+1 客人每帧 ELMP P99 < 1.0 ms、Client 呈现 < 1.0 ms、无会话时 G0 仍成立。
- CTest 只加 typed 编解码边界 / 传输状态机 / 握手三类。

## 3. P3 多客人 + 多地图
- 第三个实例：在 `devconfig.json` 加 `client2`，按 Host2 / Client2 同法建 `Win64_LAN_Client3` 目录与 `ELMP_Test_Data\client2`（只在隔离目录操作；创建前在报告“需要用户”一节说明）。
- 多客人：每对端独立会话 / 密钥 / 可靠层 / 基线 / epoch；座位按对端分配；快照公共部分 + 每对端 `me`；其他客人为 `l<seat>` 副本；满员回 Full。
- 多地图：`native/world/{level_instance,map_context}`（`LoadLevelInstance` 反射参数校验；`ELMP_BG_<Map>__<Part>` 大 Y 偏移；`0xCAAFF0` + `0xC997B0` 上下文窗口）；`coop/host/maps` 生命周期（首个座位进入加载、最后座位离开宽限 10 s 卸载、P1 进入先卸载再原版切图）；坐标按“图 id + 局部坐标”传输与兴趣过滤；远程座位切图只切自己的实例；后台图 Boss 房 fail closed（§7.1.3）；`OnOpenNewArea` 对 PX 的 PC 抑制。
- **§7.3 的 12 个未分类 `CurrentGameMapID` 读者**（`0xC98210 0xC98D80 0xC99770 0xC9A9F0 0xCA2820 0xCB6240 0xCCDD10 0xCCE900 0xCD5060 0xCD52D0 0xCDC950 0xCDD110`）逐个用 IDA 分类为 {前台表现 / P1 专属 / 需上下文窗口 / fail closed}，写进 kb，据此实现或拒绝。

门槛：两客人同图 G1–G4；一客人在后台图被敌人攻击 / 击杀敌人且 Host 存档清除记账落在正确的图；P1 进入后台图的移交；满员第 N+1 个客人收到 Full；后台图有人战斗时 Host 帧时间 P99 与性能门槛。

## 4. P4 猎人模式 + SDK
- `native/modes/hunter`（C19 / F3.1–F3.6）：速通 = Host P1 友方，猎人 ≤4 敌方；15 s 宽限冻结；胜负与计时用单调时钟；猎人进度源 `mirror` 默认 / `preset` 可选；猎人 8 s 后在所在图 PlayerStart 复活；独立 travel；椅子只回满自己；指南针进 HUD；控制台 `elmp hunter …`。
- `native/sdk`：C ABI `elmp_sdk.h` 版本化函数表 + 导出 `ELMP_GetApi(version)`；`elmp.json` 声明依赖与 slot 范围，重叠后加载者拒绝；向第三方 Lua Mod 注入 C 绑定、不注入任何 Lua 源码；事件订阅每 Mod 一个游标；第三方对象联机报未支持；示例 LilyFollower（1001）/ LilyEnemy（2001）放 `sdk/examples/`。

门槛：猎人规则在 Host + 2 客人（或本地 PX 混合）上验收；两个示例 Mod 跑通；Shipping zip 零 `.lua` 断言。

## 5. P5 发布
- README（安装 / 使用 / 功能 / 控制台命令 / 已知限制 / 架构）、INSTALL.txt、`docs/adr/0005-cpp-rewrite.md` 定稿（采纳状态与实际偏差）。
- `build.bat package` 产出 `-shipping.zip`（`main.dll` + `data/` + `elmp.json` + INSTALL；无 `.lua` / `.pdb` / 义肢）与 `-dev.zip`；`elmp.json` 版本升到 ≥ 0.3.0。
- 最后用 Shipping 构建在三实例上冒烟一遍（本地多人 + 同图联机 + 后台图），结果进 kb。

## 6. 全程硬规则
- 拿走知识不照搬代码（`D:\CodeT\ELMP`、`D:\CodeT\LilyInEM` 只读）；Shipping 零 Lua；游戏线程入口只有 `RegisterEngineTickPreCallback`；缓存优先且每个缓存有失效源与审计计数；无座位无会话时 `g0.reflect_calls` 保持 0。
- 原版管线优先、fail closed、Host 权威铁律（客人进程永不自行结算）、P1 只读；新 Hook 全部进 `EL.json`（前缀 + 来源）并经预检；能反射取地址不写 RVA；不 Hook `0xD150F0`；“没被调用”必须有 detour 计数或 IDA xref。
- 不为测试而测试：验证在 Host2 / Client2（/ Client3）真实实例上用 `tools/bridge.py` + `elmp` 命令 + 截图 + `elmp perf` 完成；写世界的测试辅助登记并在报告列出；每轮实测前备份实例 `Saved/`，结束后还原并删除本轮产生的文件；不碰正式游戏目录 / 存档；未经确认不向我正在玩的游戏发输入；实例运行中不替换 DLL。
- 文档先行：原版事实与文档冲突 → 先改 `docs/`（日期 + 证据标签）再改代码；每个子里程碑更新 `README.md`「当前阶段」并 commit（不 push）；需求树 §7 追加本阶段实施决定（标“可复议”）。
- 每个阶段结束输出与 P0/P1 同格式的报告：构建了什么、门槛逐项实测数字与截图路径、更正过的事实、测试辅助清单、遗留问题、实例最终状态；并把下一阶段的产品问题批量列给我。
- 需要我介入的事项（新实例目录、无法定位的原生入口、门槛屡试不过）在报告里单列“需要用户”一节，然后继续做不受阻塞的部分；全部可做的都做完仍受阻才报告 blocked。

## 7. 进度登记（goal 完成 = 全部打勾）
打勾格式：`- [x] 项目 —— 2026-MM-DD · 证据：<kb 文件#节 / 截图路径 / 命令输出摘要> · commit <hash>`。
“实现”类条目打勾条件是模块编译通过并在实例上跑起来（`elmp status` 可见）；“门槛”类条目打勾条件是实测数字 / 截图已写入 kb。被判定为无法达成的条目不要删，改写为 `- [!] 项目 —— 原因、尝试、剩余选项`，并同步到顶部「需要用户」。

### P1 遗留
- [x] pickup 闸门 off / on 实测，补进 §7.2 与 party-facts —— 2026-10-26 · 证据：party-facts「P1 遗留 / pickup」（旧碰撞忽略实现泄漏 → 改为关座位 AutoCollect 球体；off 掉落物飞 2100 UU 归 P1，on P2 当场收取）、需求树 §7.16.4 更正 · commit d7d685a
- [x] 精灵升级收益同步到 PX（`SpiritLevels`）实测 —— 2026-10-26 · 证据：party-facts「P1 遗留 / 精灵升级收益」（P1 s5000 1→2→3，P2 一拍内镜像；P2 对 P3 一击 −54 → −63） · commit d7d685a
- [x] 敌人激活入口：IDA 定位结果写入 `EL.json` + engine-facts（或记录“两原语闸 + CanBePaused + 1 Hz 复检”为唯一方案并实测泄漏数） —— 2026-10-26 · 证据：engine-facts「敌人激活入口重新定位」（AI 单汇点 `0xCCD8F0`，4 个调用者；Pawn 侧 `0xCCDAA0`；Dev2 预检 18 函数 + 9 偏移通过；church_07 24/24 敌人 `IsActive`） · commit b43f6c8

### P2 同图联机 —— 实现
- [x] `net`：UDP + CNG 握手 / 加密（配对码参与派生） —— 2026-10-26 · 证据：net-facts「传输层」（Host2/Client2 错码 → `pairing code mismatch`，正确码 connected RTT 1.1 ms；`test_net_handshake`） · commit a3ebf0f / b6ac40b
- [x] `net`：可靠有序 + 不可靠最新双通道、ACK 位图、RTO、分片重组、`F_CLOSE`、多对端表、全部有界队列 —— 2026-10-26 · 证据：net-facts「传输层」（`test_net_transport` 20% 丢包 / 重复 / 乱序；Host 同时两条链路；leave → Host 1 s 内 F_CLOSE） · commit a3ebf0f / b6ac40b
- [ ] `coop`：typed 位掩码增量快照 + 名字表 + 预分配 `FrameSnapshot`
- [ ] `coop`：session / world_model / host capture / client takeover · apply · presenter · vitals
- [ ] `coop/entities`：enemy / enemy_dyn / lily / worldobj / story / pickup 六个 `IEntityAdapter`
- [ ] `guard`：结算拦截（DoDamage / 技能伤害）、`IsPlayer` 跳板、`SaveGame/SaveGameAsync` 存档保护、碰撞 / tick 原语闸 + 激活闸、回放窗口
- [ ] 动作通道：`AbilityLaunchFromDesc` pre 取参 → 可靠下发 → Client 同入口重放（幂等 + 值域校验）
- [ ] 远程座位：kind=network 同构座位、500 ms 停滞松键、`map/mapReady` 跟图闸门、晚加入 / 断线重连保留座位
- [ ] 客人进度与构筑归属（FR-NET-2c）+ Client 原版档只读
- [ ] 客人装备通路（F2.10）
- [ ] 管理器「联机」页（F2.1）+ FR-NET-8 版本一致 / 拒绝原因 / Full
- [ ] Client 相机与伴随体规则（FR-NET-3c）、本地座位压制（FR-NET-3d）、Client 原版 HUD 显示 Host 数值
- [ ] CTest：typed 编解码边界 / 传输状态机 / 握手（只此三类）

### P2 同图联机 —— 门槛（Host2 / Client2）
- [ ] G1 Client 无 AI 决策：三闸 + 1 Hz 复检计数可观测，泄漏 0
- [ ] G2 客人输入经 Host 原版管线（动画序列取证）
- [ ] G3 世界一致：敌人数量 / 位置 ≤5 UU / HP、门、宝箱、可破坏物（Client ⊆ Host）、剧情旗标、Boss 窗口
- [ ] G4 60 / 144 / 波动三档帧时间 + 截图可见 Spine 动画与弹体
- [ ] 晚加入
- [ ] 断线（非主动离开）重连，座位与构筑保留
- [ ] Host P1 死亡重生 / 切图时 Client 不把卸载当死亡
- [ ] 客人离开后 Host 本地 P1 数值还原
- [ ] 联机 PvP（命中门 + 倍率 + 击退）
- [ ] §7.2 矩阵「Host 上远程座位」列
- [ ] §7.2 矩阵「Client 副本」列
- [ ] 性能：Host+1 客人 ELMP P99 < 1.0 ms；Client 呈现 P99 < 1.0 ms；无会话时 G0 仍成立（`g0.reflect_calls = 0`，对裸 UE4SS P99 差 < 0.2 ms）
- [ ] P2 阶段报告 + 需求树 §7 追加实施决定 + README「当前阶段」

### P3 多客人 + 多地图 —— 实现
- [ ] 第三个隔离实例 `client2`（`Win64_LAN_Client3` + `ELMP_Test_Data\client2`，devconfig 登记）
- [ ] 多对端：每对端独立会话 / 密钥 / 可靠层 / 基线 / epoch；快照公共部分 + 每对端 `me`；`l<seat>` 副本；满员 Full
- [ ] `world/level_instance`（`LoadLevelInstance` 反射参数校验、`ELMP_BG_<Map>__<Part>` 大 Y 偏移）
- [ ] `world/map_context`（`0xCAAFF0` + `0xC997B0` 上下文窗口）
- [ ] `coop/host/maps` 生命周期（首入加载、末离 10 s 宽限卸载、P1 进入先卸载再原版切图）
- [ ] 坐标按「图 id + 局部坐标」传输 + 兴趣过滤；远程座位切图只切自己的实例
- [ ] 后台图 Boss 房 fail closed（§7.1.3）；`OnOpenNewArea` 对 PX 的 PC 抑制
- [ ] §7.3 的 12 个 `CurrentGameMapID` 读者逐个 IDA 分类并写入 kb（`0xC98210 0xC98D80 0xC99770 0xC9A9F0 0xCA2820 0xCB6240 0xCCDD10 0xCCE900 0xCD5060 0xCD52D0 0xCDC950 0xCDD110`）

### P3 多客人 + 多地图 —— 门槛
- [ ] 两客人同图 G1–G4
- [ ] 一客人在后台图被敌人攻击 / 击杀敌人，Host 存档清除记账落在正确的图
- [ ] P1 进入后台图的移交（先卸载再原版切图）
- [ ] 满员第 N+1 个客人收到 Full
- [ ] 后台图有人战斗时 Host 帧时间 P99 与性能门槛
- [ ] P3 阶段报告 + 需求树 §7 追加实施决定 + README「当前阶段」

### P4 猎人模式 + SDK
- [ ] `modes/hunter` 实现（阵营、15 s 宽限冻结、胜负与单调计时、进度源 mirror / preset、8 s PlayerStart 复活、独立 travel、椅子只回满自己、指南针 HUD、`elmp hunter …`）
- [ ] 猎人规则在 Host + 2 客人（或本地 PX 混合）上验收
- [ ] `sdk`：C ABI `elmp_sdk.h` + `ELMP_GetApi` 版本化函数表；`elmp.json` 依赖与 slot 范围、重叠拒绝；事件订阅每 Mod 一个游标；第三方对象联机报未支持
- [ ] 向第三方 Lua Mod 注入 C 绑定（不注入 Lua 源码）
- [ ] 示例 LilyFollower（1001）/ LilyEnemy（2001）跑通
- [ ] P4 阶段报告 + 需求树 §7 追加实施决定 + README「当前阶段」

### P5 发布
- [ ] README（安装 / 使用 / 功能 / 控制台命令 / 已知限制 / 架构）+ INSTALL.txt
- [ ] `docs/adr/0005-cpp-rewrite.md` 定稿（采纳状态与实际偏差）
- [ ] `elmp.json` 版本 ≥ 0.3.0；`build.bat package` 产出 `-shipping.zip`（零 `.lua` / `.pdb` / 义肢）与 `-dev.zip`
- [ ] Shipping 构建三实例冒烟（本地多人 + 同图联机 + 后台图），结果进 kb
- [ ] 最终报告（全部阶段汇总、遗留清单、实例最终状态）
