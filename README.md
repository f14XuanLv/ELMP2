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
- 下一步 P1c：共享进度 1 s 节拍、复活 / 召回、过图跟随与规则 1/2/3、椅子、镜头、队伍与 PvP。

## 与现仓库的关系

- 需求树是导航与定案层，`D:\CodeT\ELMP\docs\REQUIREMENTS.md` 保留细则；冲突以需求树为准并回写。
- 架构 v2 = 提议中的 ADR 0005；采纳后按 `ARCHITECTURE-V2.md` §15 分五阶段迁移，每阶段有对等门槛。
- 新取证应同步追加到 `docs/kb/party-facts.md`（归属 / 触发器）与 `docs/kb/net-facts.md`（单值假设清单）。
