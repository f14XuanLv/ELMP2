# core（LilyCore，`native/core` + `native/input/virtual_pad`）依赖的事实与 P1a 验收记录

来源标注同 `engine-facts.md`：`[E-IDA]` / `[E-实测]`（ELMP v2，Dev2 = `Win64_For_Mod_Dev2`，UserDir `ELMP_Test_Data\dev`，测试存档槽 0，map_church_08 椅子旁）/ `[E-SDK]` / `[E-源码]`。引用 v1 结论注明 `v1 <文件>「节」`。

## 生成管线在 C++ 下成立（v2 2026-10-25）

- 生成 = `GameplayStatics:BeginDeferredActorSpawnFromClass(World, Class, FTransform, Undefined, nullptr)` + `FinishSpawningActor(Actor, FTransform)`，两次 ProcessEvent 打在 `Default__GameplayStatics` 上（与 UE4SS `UWorld::SpawnActor` 同一路径，`deps/first/Unreal/src/World.cpp`）。FTransform（4.26 float）48 字节：FQuat + FVector/pad + FVector/pad；Yaw ±90 → `(0, 0, sin(yaw/2), cos(yaw/2))`。`[E-源码][E-实测]`
- 控制器：生成期间把 `PC_Base_C` CDO 的 `PlayerCameraManagerClass` 换成 `/Script/Engine.PlayerCameraManager`、生成后还原（v1 core-facts「生成第 N 玩家」的崩溃规避），C++ 直写 CDO 的类属性即可；Settled 世界中 `Possess` 当帧生效（`Pawn.Controller` 与 `Controller.Pawn` 互指）。`[E-实测]`
- Spine 补数据顺序沿用 v1：`ReplaceSpineData(atlas, data, notify)` → `SetToSetupPose` → `SetSlotsToSetupPose` → `UpdateWorldTransform` → `ForceTick(0.016)` → `SetAnimation(0, "idle", true)`，再 `FakeLight:SetMaterial(0, MI_LilyLightN)`。`ForceTick`（1 参数）在 `SpineSkeletonAnimationExComponent` 上存在（重建插件头里没有，`[E-SDK]` 不全）。`[E-实测]`
- UFunction / 属性名按 FName 语义**大小写不敏感**解析：Spine 插件导出的是 `getAnimationName`（小写 g，插件源码 `SpineSkeletonAnimationComponent.h:160`），v1 Lua 的 `GetAnimationName` 能用是因为 UE4SS 查找不区分大小写。`engine/reflect` 的 `same_name` 采用同一语义。`[E-源码][E-实测]`
- 全部 core 字段（约 110 项，`core/schema.cpp`）在读档后一次解析成功：script 组在首次使用时解析，blueprint 组（`PC_Base_C`、`BP_p0000_Lily_C.SpineSkeletonAnimationEx / FakeLight`、四张 `DT_Items_*`）读档后解析。`[E-实测]`

## 虚拟手柄（`InputConsume` detour，v2 2026-10-25）

- 只经 EL.json `InputConsume`（预检前缀通过）武装；写入偏移取反射 `ZenithInputComponent.InputSnapshot`（= EL.json 224，世界就位时校验一致）。条目按组件指针匹配、每次匹配用句柄（index + serial）+ `Pawn.Controller ↔ Controller.Pawn` 互指校验，失效即删（`core.pad_dropped`，本轮 0）。`[E-实测]`
- 动画序列（`elmp trace 101 <ms>`，track 0 动画名变化沿 + Y/Z；测试 slot 101，DEFAULT / FULL 档案）—— 与 v1 core-facts「输入」逐条一致：

| 输入 | 序列（+ms） | 位移 |
|---|---|---|
| `move 101 1 0` 约 1.2 s 后 `stop` | idle → run(+133) → run_stop(+1366) → idle(+1683) | Y −1837 → −1249 |
| `press JUMP` | jump_up(+166) → jump_apex(+416) → jump_down(+516) → landing(+783) → idle | Z 556 → 697 → 556 |
| `press ATTACK` | summon_DEFAULT_A(+116) → idle(+933) | — |
| FULL：`move -1 0` + `hold DODGE` 1 s → 松 DODGE → `stop` | idle_turn(+116) → dodge(+266) → dash_loop(+833) → run(+1550) → run_stop(+2016) | Y −1249 → −2478 |
| FULL + 恢复（MP 100）：`move 0 1` 200 ms 后 `press ATTACK` | lookup_start → lookup_loop → attack_SP_5000(+599) → idle | MP 100 → 0 |
| DEFAULT（无 special_attack）+ 恢复：同上 | lookup_start → lookup_loop → summon_DEFAULT_A(+649) | MP 保持 0 |

## 档案 / 恢复 / 复活（v2 2026-10-25）

- **MP 规则**（v1 core-facts「奥义解锁与 MP」复核）：FULL（含 `special_attack`）后 `vitals restore` → MP 100/100；`reset default` 去掉 10 项能力后再 restore → `MPComponent:Reset()` → MP 0，↑+ATTACK 变为普通 `summon_DEFAULT_A`。`[E-实测]`
- **精确镜像幂等**：`profile 101 apply mirror exact`（读 P1：能力 15、强化物品 42、精灵等级 26、装配 6、遗物拥有 34 / 佩戴 0 / 槽 20、Lv100）第一次 `added 10 / 42 / changed 26 / equipped 6 unequipped 2 / owned +34`，第二次全部为 0；slot 101 的 JSON 与 P1 完全一致；前后 P1 的 JSON 不变（P1 只读）。`[E-实测]`
- **池化 PC 精确重置**：移除后再生成同一 slot 复用池中 PC（仍带镜像进度），`reset default` → `removed 10 / 42 / changed 26 / owned −34`，回到新游戏（MaxHP 100、精灵全 1 级、槽 2）。`[E-实测]`
- **复活只换 Pawn**：`respawn 101` 前后档案 JSON 完全一致（装备 / 背包 / 等级保留），新 Pawn 皮肤沿用（3），`ReequipSpirits` 后 restore → HP 400/400、MP 100/100。`[E-实测]`
- 拒绝路径：`spawn 1` / `profile 1 apply` / `vitals 1 restore` / `skin 1` / `press 1`（未 allow1）全部拒绝；未知能力 `Fly` 整项拒绝（其他字段不受影响）；同 slot 二次 spawn 报 “already has a pawn” 且**不释放**原租约（2026-10-25 修复的控制台缺陷）。`[E-实测]`

## 生命周期与 G0（v2 2026-10-25）

- 回标题（`OpenLevel(TitleMap)`）→ teardown：pad 条目丢弃、池清空、全部租约释放、blueprint 组下次重解；重新读档 generation 2 后再生成、`registry verify` consistent。`[E-实测]`
- 移除最后一个座位后帧泵回到 idle：8 s 内 `g0.reflect_calls = 0`、`g0.reflect_lookups = 0`，`core.pad_writes` 停止增长；idle 每 tick P99 29 µs。一个座位（slot 101 跑动）时 `frame.elmp_us` P50 7 / P95 10 / P99 32 µs（P1a 无每帧阶段；detour 写入不计入帧泵）。`[E-实测]`

## 测试辅助（P1a，写世界，登记）

- 读档 `TitleMap_C:OnSaveSlotSelected(0, 2)` ×3、`OpenLevel(TitleMap)` ×1；档案 / 恢复 / 皮肤写入只发生在测试 slot 101；未向用户游戏发输入，未碰正式目录 / 存档。
