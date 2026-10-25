# 引擎层（engine/ base/）依赖的事实与 P0 验收记录

来源标注：`[E-IDA]` 静态分析（`EnderLiliesSteam-Win64-Shipping.exe`，基址 `0x140000000`）、`[E-实测]` ELMP v2 隔离实例（Dev2 = `Win64_For_Mod_Dev2`，UserDir `ELMP_Test_Data\dev`，测试存档槽 0）、`[E-源码]` UE4SS 32d8a381 / UE 4.26 源码。引用 v1 kb 时注明 `v1 <文件>「节」`。

## 激活本体 0xCEFAE0 属于 AZenithSpirit，不是 AZenithCharacter（v2 2026-10-25，更正 v1）

- v1 `net-facts「ZenithCharacter 原生激活管线」`（2026-10-14）与 v1 `data/RVA.json` 把 `0xCEFAE0` 记为“AZenithCharacter 激活本体 SetActive，一切敌人激活路径单汇点”，把 `+1464/+1465` 记为 ZenithCharacter 的激活状态 / 标志。
- **反射实测** `[E-实测]`：Dev2 反射 `/Script/Zenith.ZenithCharacter` 属性偏移 `1304 CharacterInfos | 1320 CommandQueueComponent | … | 1344 ZenithInputComponent | … | 1464 OnParryDelegate | 1480 OnGotParriedDelegate | … | 1704 OnEventStopDelegate`（类 PropertiesSize 1720）。`+1464` 是 16 字节多播委托，不可能是状态字节。ELMP 启动后的偏移校验（`data/binary/EL.json` within 检查）因此把旧条目判为失败、fail closed。
- **IDA** `[E-IDA]`：`0xCEFAE0` 写 `this+1465`、读 `this[163]`(+1304) 指针并写其 `+336`、`this+164`(+1312) 作 `FWeakObjectPtr` 控制器 Possess/UnPossess、读 `this+1320` float。其调用者 `0xCCFCB0`（`+1464 != 2 → 置 2 → 0xCEFAE0(1) → 广播 this+1264`）的 exec thunk `0x140DA1C70` 只被 `0x1434B03B8` 引用，那是原生函数表项 `{"Appear", 0x140DA1C70}`，相邻项 `AddAttackToken / Disappear / ForceDisappear / GetAttack…` —— `AZenithSpirit` 的函数表（`ZenithSpirit.h`: `Appear()`、`AddAttackToken()`）。
- **反射交叉** `[E-实测]`：`ZenithSpirit` 反射 `1216 StateComponent | 1264 OnSpiritAppearDelegate | 1304 MovementComponent`，其后到类尾 1488 无反射字段 —— 与 IDA 中 `+1216`（传给 sub）、`+1264`（广播）、`+1304`（移动组件写 +336）、`+1464/+1465`（落在 [1312, 1488) 空洞）全部吻合。
- **结论**：`0xCEFAE0` = `AZenithSpirit::SetActive`（精灵出现 / 消失本体），`+1464/+1465` 是 `AZenithSpirit` 的状态 / 激活标志。EL.json 条目已改名 `SpiritSetActive`、`ZenithSpirit.ActivationState/bActive`，Dev2 校验通过（gap [1312, 1488)）。
- **影响**：v1 “敌人激活三闸”中的第一闸并不在敌人路径上（这也解释了 v1 village_04 实测 `enemyActive=0`，见 v1 `net-facts`）。真正的敌人激活入口留到 P2（C16 Client 呈现）重新用 IDA 定位；在此之前 M18 只有 `SetActorEnableCollision / SetActorTickEnabled` 两个原语闸是有效证据。

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
