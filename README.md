# ELMP v2 文档（需求树 + C++ 重写架构）

本目录是对 `D:\CodeT\ELMP` 的**需求重排与架构重设计**产物（2026-10-25），不含代码。

| 文件 | 内容 |
|---|---|
| [`docs/REQUIREMENTS-TREE.md`](docs/REQUIREMENTS-TREE.md) | 自顶向下需求树（L0 玩家目标 → L1 可感知功能 → L2 系统能力 → L3 机制 → L4 原版入口 / 事实）、依赖图（Mermaid + 邻接表 + 关键路径）、原 [待定] / [调研] 项的全部定案（§7）、验收口径与性能门槛（§8）、节点索引（§9） |
| [`AGENTS.md`](AGENTS.md) | 新仓库开发规则：拿走知识不照搬旧代码、Shipping 零 Lua、以真实实例验证为主不为测试而测试、可直接替换实例中旧仓库部署的 Mod；**资料与知识来源表**（v1 仓库、重建 UE 工程 + Spine/Event 插件源码、UE 4.26 源码、UE4SS 源码、LilyInEM 参考实现、Randomizer、Lilac2LilyMod、HKMP/SSMP）及证据等级 |
| [`docs/ARCHITECTURE-V2.md`](docs/ARCHITECTURE-V2.md) | 现状性能诊断、Shipping 纯 C++ / Dev 带 Lua 的目标形态、分层与模块布局、线程模型与单帧泵、反射访问层、对象注册表、Hook 总表、各模块设计、类型化联机协议、多地图、持久化、Dev 构建、SDK（C ABI + Lua 注入）、测试、构建发布、迁移计划、性能设计、风险、ADR 更新清单 |

## 本次新增的取证（已写入需求树 §5 E4a–E4c、E6、E9）

| 项 | 工具 | 结论 |
|---|---|---|
| 可交互物品归属 | CUE4Parse `BP_Interactable_Item_C` 伪代码 | `OnEventFinished` / `EnsureItemObtained` 一律 `GetPlayerController(this,0)->InventoryComponent->AddItem(Item)`：无论谁交互都进 P1 背包（天然共享池） |
| Boss 奖励归属 | CUE4Parse `BP_Character_Boss_Base_C` | 死亡 → `EVT_GetSpirit_Boss` 以 Controller0 的 PC / Pawn 为主体播放 → `GetZenithPlayerController(this,0)->InventoryComponent->AddItem`；死亡慢动作亦绑 Controller0 → 后台图 Boss fail closed |
| 剧情 / 教程 / 对话触发器判定 | CUE4Parse `BP_Trigger_C` | `Cast<APawn>→IsPlayerControlled()→Cast<APlayerController>→Trigger(PC)`，不走 `IsPlayer` 跳板 → PX 需用“重叠忽略”闸门按类策略屏蔽 |
| 取得物品 / 教程弹窗 | CUE4Parse `EVT_ACT_OpenWBP_Get_Item` / `_TutorialDialog` 调用图 | `WidgetBlueprintLibrary::Create → AddToViewport` + `WaitGameUnpaused`（屏幕级、暂停） |
| `CurrentGameMapID` 位置与原生读者 | IDA（`EnderLiliesSteam-Win64-Shipping.exe.i64`） | 字段 `UWorldLoaderSubsystem+472`（helper `0x140CE1550`）、`+480`（`0x140CE1540`）；子系统 getter `0x140CC6FF0` / `0x140D0A850`；读者全集 18 个函数，其中已识别：清除管理 `0xCAAFF0`（已 Hook）、`SetRespawnPointData 0xCF1A20`、重生出生点 `0xCE61E0/0xCDC1C0`、区域表 `0xD0B8A0`、`ClearableComponent::BeginPlay 0xC997B0`（已 Hook）；12 个待分类（架构 §9 工作清单） |
| UE4SS C++ 能力边界 | RE-UE4SS 32d8a381 源码 | `on_update` 在事件循环线程（非游戏线程）；游戏线程每帧入口用 `RegisterEngineTickPreCallback`；另有 `LoadMap/BeginPlay/EndPlay/ProcessConsoleExec/StaticConstructObject` 回调、`UFunction::RegisterPreHook/PostHook`、`FUObjectCreate/DeleteListener`；首方 `JSON`/`ScopedTimer`/`PolyHook_2_0`/`imgui` 可直接用，零新增依赖 |

## 当前阶段

**P0 骨架已完成（2026-10-25）**：`native/base`（log、clock、json + 原子写、binary_manifest、preflight、hook、settings、metrics、events、slots）、`native/engine`（ue、handle、reflect、world、registry、frame、lifecycle、binary_checks）、`native/console`、`native/dev`（Dev 的 UEBridge 注入、ElmpFrameProbe 帧时间探针）、`tools/`（devconfig / deploy / instance / bridge / capture / package）、`data/binary/EL.json`、3 个 CTest。Dev2 门槛实测见 `docs/kb/engine-facts.md`「P0 验收」。

**P1 core + 本地多人（进行中）**：
- **P1a core 内核已完成（2026-10-25）**：`native/core`（schema 一次解析约 110 个字段、player 槽位表 / 身份标签、spawn 生成 / 移除 / 复活、profile 读 / 增量 / 精确 / 重置、vitals、skin）、`native/input`（pad_state、virtual_pad = `InputConsume` detour）、控制台 `elmp core|spawn|despawn|respawn|lily|profile|vitals|skin|pad|move|hold|press|stop|trace`。Dev2 实测（动画序列、MP 规则、镜像幂等、复活保留装备、G0）见 `docs/kb/core-facts.md`。产品决定见需求树 §7.13。
- **P1b 输入与座位已完成（2026-10-25）**：`native/input`（bindings 默认 = 原版键位、router、xinput 导入表改写 + 后台连接探测、rawinput 窗口子类化 + 按设备键盘）、`native/party`（config 设置定义、devices 持久化 `devices.json` + 每帧输入泵、seat 加入 / 离开 / 摆位、progress 共享进度基线）、控制台 `elmp join|leave|seats|devices|assign|bind|bindings|mute`（Dev：`kbd`、`fakepad`）。实测见 `docs/kb/party-facts.md`「P1b」。
- **P1c-1 规则已完成（2026-10-25）**：`core/permission`（travel / rest 重叠忽略闸门 + GateCache）、`party/progress`（1 s 收益记 P1 + 镜像 + 读回）、`party/respawn`（复活 / 丢 Pawn 兜底 / 召回 / 摆位）、`party/bench`、`party/rules`（规则 1 授权、规则 2 出口盒 + P1 出口保持）、`party/transit`（过图跟随）；控制台 `elmp progress|gates|perm|rest|transit`（Dev：`kill`、`tp`）。实测见 `docs/kb/party-facts.md`「P1c-1」。
- **P1c-2 镜头 / 队伍 / PvP 已完成（2026-10-26）**：`camera/framing`（`GameplayCameraUpdate` / `TargetBounds` 两个 detour）、`party/camera_sync`（原版 Ally 目标注册 + 放宽参数下发）、`core/hitrules`（slot → team + 配对规则）、`pvp/hitgate`（3 个 UFunction pre/post + `AbilityEffectQueue` detour，首个异队配对时才安装）；控制台 `elmp team|pvp|camera`。实测见 `docs/kb/party-facts.md`「P1c-2」。
- **P1d 覆盖层已完成（2026-10-26）**：`tools/catalog`（重新生成 `data/catalog/items.json`、图标、字体）、`core/catalog`、`overlay`（DXGI Present 覆盖层、seqlock 模型、HUD / PX 菜单 / 管理器绘制）、`party/{ui,hud_feed,menu_ctrl,manager}`；控制台 `elmp menu|manager|hud`（Dev：`ui click|move`）。存储确认页面已就位，数据随 P1e 接入。实测见 `docs/kb/party-facts.md`「P1d」。
- **P1e 闸门与存档作用域已完成（2026-10-26）**：`data/gates.json`（15 个触发器类）+ `core/permission` 扩展到触发器 / 拾取 / 交互；`party/save_scope`（saveId 世代 + 指纹，New / Reuse）、`party/storage`（`party_<saveId>.json`，自动重入）、`base/lua_literal`（v1 `*.profile.lua` 只读迁移，CTest）；控制台 `elmp storage [new|reuse]`。实测见 `docs/kb/party-facts.md`「P1e」。
- **P1f 本地多人总验收已完成（2026-10-26）**：§7.2 闸门矩阵本地列（travel / rest / trigger.world / trigger.story / interact 以原版副作用实测；pickup 未能验证）、F1.5 / F1.7 / F1.13–F1.16 补测、存档复制 / 删除、椅子异步保存登记、60 / 144 / 切换帧率与 G0（对裸 UE4SS P99 差 −0.003 ms）。新增 `party/interact`（PX 交互交给原版交互物 Pressed 入口，需求树 §7.16.1）；Dev `elmp native`、`tools/catalog/extract.py --dump`。实测见 `docs/kb/party-facts.md`「P1f」。

**P2 同图联机已完成（2026-10-27）**：`native/net`（CNG ECDH P-256 + 配对码 HKDF / HMAC + AES-256-GCM、UDP 可靠有序 / 不可靠最新双通道、ACK 位图 / RTO / 分片 / F_CLOSE / 多对端表、全部有界队列；typed 位掩码增量快照 + 名字表；3 类 CTest）、`native/guard`（结算拦截、`IsPlayer` 跳板、存档保护、碰撞 / tick 原语闸、AI / 角色激活闸、回放窗口）、`native/coop`（session、Host 远程座位 / 输入 / 快照 / 跟图 / 构筑与装备通路、Client 接管冻结 + 1 Hz 复检 / 插值呈现 / 本地 P1 预测与回拉 / P1 呈现模式、动作通道 `LaunchAbilityFromDescription` 原生 pre 取参 → Client 同入口重放、六个 `IEntityAdapter`：lily / enemy / enemy_dyn / worldobj / story / pickup）、管理器「Online」页；控制台 `elmp lan host|join|leave|status|trace`。Host2 / Client2 实测（G1–G4、晚加入、断线重连、P1 死亡不误判、PvP、§7.2 远程 / Client 两列、性能 Host P99 0.49 ms / Client 0.30 ms）见 `docs/kb/net-facts.md`；实施决定见需求树 §7.17。

**P3 多客人 + 多地图已完成（2026-10-27）**：`native/world/level_instance`（后台图子关卡以 `ELMP_BG_<mapId>__<Part>` 在 Y +300000·k 原生加载、map_context 两个 detour 让清除按所属图记账）、`native/coop/maps`（网络座位进门 → 前台或后台实例、10 s 宽限卸载、P1 进入后台图移交、Boss 房 fail closed）、快照按对端座位图过滤平移；两客人同图、满员 Full、第三实例 `client2`；控制台 `elmp lan bg load|unload|boss`。实测见 `docs/kb/net-facts.md`「P3 实测」及之后各节；实施决定见需求树 §7.18。

**P4 猎人模式 + SDK（进行中）**。

## 与现仓库的关系

- 需求树是导航与定案层，`D:\CodeT\ELMP\docs\REQUIREMENTS.md` 保留细则；冲突以需求树为准并回写。
- 架构 v2 = 提议中的 ADR 0005；采纳后按 `ARCHITECTURE-V2.md` §15 分五阶段迁移，每阶段有对等门槛。
- 新取证应同步追加到 `docs/kb/party-facts.md`（归属 / 触发器）与 `docs/kb/net-facts.md`（单值假设清单）。
