# 引擎层（engine/ base/）依赖的事实与 P0 验收记录

来源标注：`[E-IDA]` 静态分析（`EnderLiliesSteam-Win64-Shipping.exe`，基址 `0x140000000`）、`[E-实测]` ELMP v2 隔离实例（Dev2 = `Win64_For_Mod_Dev2`，UserDir `ELMP_Test_Data\dev`，测试存档槽 0）、`[E-源码]` UE4SS 32d8a381 / UE 4.26 源码。引用 v1 kb 时注明 `v1 <文件>「节」`。

## 激活本体 0xCEFAE0 属于 AZenithSpirit，不是 AZenithCharacter（v2 2026-10-25，更正 v1）

- v1 `net-facts「ZenithCharacter 原生激活管线」`（2026-10-14）与 v1 `data/RVA.json` 把 `0xCEFAE0` 记为“AZenithCharacter 激活本体 SetActive，一切敌人激活路径单汇点”，把 `+1464/+1465` 记为 ZenithCharacter 的激活状态 / 标志。
- **反射实测** `[E-实测]`：Dev2 反射 `/Script/Zenith.ZenithCharacter` 属性偏移 `1304 CharacterInfos | 1320 CommandQueueComponent | … | 1344 ZenithInputComponent | … | 1464 OnParryDelegate | 1480 OnGotParriedDelegate | … | 1704 OnEventStopDelegate`（类 PropertiesSize 1720）。`+1464` 是 16 字节多播委托，不可能是状态字节。ELMP 启动后的偏移校验（`data/binary/EL.json` within 检查）因此把旧条目判为失败、fail closed。
- **IDA** `[E-IDA]`：`0xCEFAE0` 写 `this+1465`、读 `this[163]`(+1304) 指针并写其 `+336`、`this+164`(+1312) 作 `FWeakObjectPtr` 控制器 Possess/UnPossess、读 `this+1320` float。其调用者 `0xCCFCB0`（`+1464 != 2 → 置 2 → 0xCEFAE0(1) → 广播 this+1264`）的 exec thunk `0x140DA1C70` 只被 `0x1434B03B8` 引用，那是原生函数表项 `{"Appear", 0x140DA1C70}`，相邻项 `AddAttackToken / Disappear / ForceDisappear / GetAttack…` —— `AZenithSpirit` 的函数表（`ZenithSpirit.h`: `Appear()`、`AddAttackToken()`）。
- **反射交叉** `[E-实测]`：`ZenithSpirit` 反射 `1216 StateComponent | 1264 OnSpiritAppearDelegate | 1304 MovementComponent`，其后到类尾 1488 无反射字段 —— 与 IDA 中 `+1216`（传给 sub）、`+1264`（广播）、`+1304`（移动组件写 +336）、`+1464/+1465`（落在 [1312, 1488) 空洞）全部吻合。
- **结论**：`0xCEFAE0` = `AZenithSpirit::SetActive`（精灵出现 / 消失本体），`+1464/+1465` 是 `AZenithSpirit` 的状态 / 激活标志。EL.json 条目已改名 `SpiritSetActive`、`ZenithSpirit.ActivationState/bActive`，Dev2 校验通过（gap [1312, 1488)）。
- **影响**：v1 “敌人激活三闸”中的第一闸并不在敌人路径上（这也解释了 v1 village_04 实测 `enemyActive=0`，见 v1 `net-facts`）。真正的敌人激活入口留到 P2（C16 Client 呈现）重新用 IDA 定位；在此之前 M18 只有 `SetActorEnableCollision / SetActorTickEnabled` 两个原语闸是有效证据。**已定位（2026-10-26）**：见下节「敌人激活入口重新定位」。

## 敌人激活入口重新定位（v2 2026-10-26，P2 前置）

由 `elmp native` 取 exec 桩 → IDA 进本体（`EnderLiliesSteam-Win64-Shipping.exe.i64`）。注意 IDA 的 exec 桩函数名常因 COMDAT 折叠张冠李戴（如 `AControlPoint::execOnCaptureAreaBeginOverlap`），以 `elmp native` 给的 UFunction 为准。

| UFunction | exec 桩 | 本体 | 作用 |
|---|---|---|---|
| `ZenithAIController:Activate` | `0x140D9A520` | **`0x140CCD8F0`** | 守卫 `+856` → 置 1 → `UseBlackboard` → `RunBehaviorTree`（虚表 `+1984`，树来自虚表 `+2072`）→ 相机注册 `0xCEDD20` → `OnActivate` 广播 |
| `ZenithAIController:IsActive` | `0x140D9A950` | `0xCE34F0`：`movzx eax, [rcx+358h]` | 读 `+856` |
| `ZenithCharacter:Activate` | `0x140D9A540` | **`0x140CCDAA0`** | 守卫 `+1280` → 置 1 → 虚表 `+768(0)` → `SetActorEnableCollision(1)` → `SetActorTickEnabled(1)` → 组件 tick → `0xD99DA0`（OnActivate） |
| `ZenithCharacter:IsActivated` | `0x140D59BE0` | `0xCAB940`：`movzx eax, [rcx+500h]` | 读 `+1280` |
| `EnemySpawnPoint:ActivateEnemy` | `0x140D597B0` | `0xCCEC60` | 弱指针 `+988` 敌人无控制器时生成 AIController 并 `Possess`，否则调敌人虚表 `+1800`；随后 `OnEnemyActivate` 广播 `0xD59120`、黑板初值 `0xCCFD10` |
| `PawnActivationVolume:OnBeginOverlap` | `0x140D7C8E0` | `0xCE68A0` → `0xCCF250` | 未激活（`+584`）且来者通过玩家判定 → 置 `+584` → 对 `PawnsToActivate` 每个的控制器调 `0xCCD8F0` |
| `GameplayCamera:RegisterEnemyController` | `0x140D638A0` | `0xCEC9E0` | 相机登记（不激活 AI） |

- **AI 激活单汇点 = `AZenithAIController::Activate`（`0xCCD8F0`）**：`xrefs_to` 只有 4 个代码调用者 —— 激活体积 `0xCCF250`（OnBeginOverlap）、体积 `0xCE8290`（OnPawnEndPlay 计数满后）、`AZenithAIController::SetPawn` `0xCF1330`（`bAutoActivate` `+832` 为真时；`EnemySpawnPoint::ActivateEnemy` 的 `Possess` 走这里）、UFunction exec 桩（蓝图）。行为树只在这里启动（`RunBehaviorTree` 本身只有虚表引用）。`[E-IDA]`
- **Pawn 侧 `AZenithCharacter::Activate`（`0xCCDAA0`）** 的唯一代码调用者是其 exec 桩 `0x140D9A540` —— 只经 UFunction / 蓝图触达。`[E-IDA]`
- 守卫字节反射邻接：`ZenithAIController` `BehaviourTree_NGPlus@848` 之后、类尾之前 = `+856`；`ZenithCharacter` `SourceSpawnPoint@1272` 与 `LastAttackerDirection@1288` 之间 = `+1280`。两者写进 `EL.json` `offsets`（`ZenithAIController.bActive`、`ZenithCharacter.bActivated`），函数写进 `functions`（`GuardEnemyAIActivate`、`GuardEnemyCharacterActivate`），Dev2 预检 18 个函数前缀 + 9 个偏移全部通过。`[E-实测]`
- 实测（Dev2 槽 0）：church_08 只有 2 个 `ZenithAIController`（精灵侧），`IsActive` 0；P1 传送进 church_07 后 8 s 内 26 个控制器中 **24 个 `IsActive()=true`，对应 24 个敌人 `IsActivated()=true`**（`activationVolume=0`：此图敌人随生成即激活，不靠体积）。激活 ≠ 不暂停：靠近才动的是蓝图 `SetPaused`（tick 开关，`CanBePaused` 守卫，v1 net-facts「Client 敌人冻结与激活」）。`[E-实测]`
- **v2 激活闸门方案（取代 v1 三闸第一闸）**：Client 对登记敌人 detour `0xCCD8F0`（控制器所控 Pawn 已登记 → 整体跳过：无黑板、无行为树 = 无 AI 决策）+ `0xCCDAA0`（登记 Pawn → 跳过）+ 碰撞 / tick 两原语闸（`0x22C78D0` / `0x22C8540`，登记对象开启方向跳过）+ `CanBePaused=false`（蓝图 SetPaused）+ 1 Hz 两标志复检。G1 用这 4 个 detour 的拦截计数 + 复检计数 + `IsActive()` 抽查作证据（P2）。

## UE4SS / 引擎行为（P0 实测）

- `RegisterProcessConsoleExecCallback` 是 post 回调，且 UE4SS 只在 `Context` 是 `UGameViewportClient` 时调用（`Hooks.cpp`）；`UGameViewportClient::Exec` 在 `ProcessConsoleExec(Cmd, Ar, NULL)` 返回 false 后才交给 GameInstance / GEngine（UE 4.26 `GameViewportClient.cpp` 2916 行）。所以 `elmp …` 只来自本地玩家（Controller0）控制台路径。`[E-源码][E-实测]`
- `mods.txt` 中 `ELMP : 0` 时 UE4SS 仍会把 `Mods/ELMP/dlls/main.dll` 映射进进程（`GetModuleHandle` 能取到），只是不调用 `start_mod`（日志 “Mod 'ELMP' disabled in mods.txt.”）。判断“ELMP 是否在运行”不能看模块是否加载；ELMP 导出 `elmp_started()` 供 Dev 工具使用。`[E-实测] 2026-10-25`
- 标题画面：`GameViewport.World` 有效、`LocalPlayers[0].PlayerController` 有效、Pawn 为空；读档时 LoadMap 触发两次（title → loading → title → loading → settling → settled）。`[E-实测]`
  - **更正（v2 2026-10-25，P1a）**：`WorldLoaderSubsystem.CurrentGameMapID = None` 只在进程首次进入标题时成立；从游戏里回标题后子系统（GameInstance 级）保留上一张图（实测仍为 `map_church_08`），lifecycle 因此停在 `settling (P1 pawn missing)` 而不是 `title`。不影响判定（非 Settled 即无座位上下文），但“标题”不能只靠地图 ID 识别。`[E-实测]`
- `SwitchGameMap`（church_08 → church_07）不经过 LoadMap：生命周期保持 Settled，旧图 actor 走 EndPlay、新图 actor 走 BeginPlay，注册表无需重建（verify 一致，enemy 24 / spawnPoint 25）。`[E-实测]`
- `KismetSystemLibrary::GetProjectSavedDirectory()` 在 `-UserDir` 隔离下返回 `<UserDir>/Saved/`（Dev2：`…/ELMP_Test_Data/dev/Saved/`）。`[E-实测]`
- `HealComponent` 反射 `HealPowerPercentageBonus` 结束于 288，类尾 336；IDA 的剩余祈祷 `+296` 落在空洞内（v1 net-facts 结论成立）。`ZenithInputComponent.PawnOwner/NextInputSnapshot/InputSnapshot` = 176/184/224，`FInputSnapshot` 尺寸 40（与 IDA 一致）。`[E-实测]`
- UE 的 `check()` / `verify()` 是宏：引擎层代码不要用这两个名字作函数名（P0 编译踩坑）。

## P0 验收（2026-10-25，Dev2，单窗口 1280×720，`t.MaxFPS 60`、`r.VSync 0`）

- **预检**：Dev / Shipping 两种构建均 `preflight ok`（EXE SHA-256 `9b939a44…8329`，15 个函数前缀逐字节通过，约 100 ms）；7 个原始偏移全部通过反射邻接校验（修正后的 ZenithSpirit 两项见上）。
- **G0 零开销**（church_08 椅子旁静止，无座位、无会话；`elmp perf`）：idle tick 5441 次中 `g0.reflect_calls = 0`、`g0.reflect_lookups = 0`；ELMP 每 tick 游戏线程占用 mean 6.0 / P50 5 / P95 8 / P99 20 / max 167 µs（门槛 P99 < 50 µs）；1 Hz 注册表审计每次约 3 µs，`registry.audit.stale = 0`。
- **帧时间 A/B**（ElmpFrameProbe：EngineTick 间隔，2 s 热身 + 60 s，各 3600 样本；不是 GPU Present 时间）：

| 运行 | ELMP | mean / P50 / P95 / P99 / max（ms） |
|---|---|---|
| A1 | 关（mods.txt 0） | 16.670 / 16.667 / 16.729 / 16.832 / 18.466 |
| A2 | 关 | 16.670 / 16.667 / 16.728 / 16.884 / 20.546 |
| B1 | 开 | 16.668 / 16.667 / 16.740 / 16.838 / 17.403 |
| B2 | 开 | 16.668 / 16.667 / 16.726 / 16.843 / 17.385 |

  P99 差值（B − A）在 −0.04 ~ +0.01 ms，门槛 < 0.2 ms 达成。（A1 的 JSON 标签 `elmpLoaded=true` 是探针旧版的误判，见上一节；日志确认 ELMP 未启动。）
- **注册表 = 一次性基线**：church_08（读档）、church_07（经原版 SwitchGameMap）、回标题再读档（LoadMap teardown → generation 2）三处 `elmp registry verify` 均 consistent（missing = extra = 0）；Menu 桶按需开启后 209 个 `UserWidgetNavigable` 与扫描一致。
- 截图：`ELMP_Test_Data\dev\Saved\ELMP\pilot\shots\p0-settled-church08-dev.png`（原版画面，椅子提示与原版 HUD 正常）。
- 测试辅助（写世界，登记）：读档 `TitleMap_C:OnSaveSlotSelected(0,2)` ×6、`WorldLoaderSubsystem:SwitchGameMap(map_church_07, tag 1)` ×1、`OpenLevel(TitleMap)` ×1、控制台 `r.VSync 0` / `t.MaxFPS 60` 各 ×4；未向用户游戏发输入，未碰正式目录 / 存档。
- Shipping zip（`dist/ELMP-0.2.0-shipping.zip`）内容：`ELMP/dlls/main.dll`、`ELMP/elmp.json`、`ELMP/data/binary/EL.json`、`INSTALL.txt`，`.lua` = 0（`tools/package.py` 断言）。
