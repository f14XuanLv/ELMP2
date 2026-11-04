# ELMP 架构 v2：Shipping 纯 C++ 重写

> 对应需求：同目录 `REQUIREMENTS-TREE.md`（节点编号 G/F/C/M/E）。
> 定位：**ADR 0005（提议 → 采纳后替代 ADR 0001 的文件布局与 ADR 0003 的 Lua 侧布局；ADR 0002 的方法论、ADR 0003/0004 的权威模型与协议语义全部保留）**。
> 日期：2026-10-25。证据标签同需求树。

---

## 0. 为什么重写，以及重写成什么

### 0.1 现状诊断（v1 = C++ 机制层 + Lua 逻辑层）

v1 把“机制”（Hook / 手柄 / 传输 / 覆盖层）放在 C++，把“逻辑”（座位、巡检、快照采集、呈现应用、HUD 取数）放在 UE4SS Lua。性能问题不在 Lua 解释器本身，而在**逻辑层每帧穿越 UE4SS Lua 反射边界的次数**：

| 热路径（每帧） | 现状 | 每次调用的隐藏成本 |
|---|---|---|
| Host 快照采集 `coop/host/capture.lua` → `entities/enemy.lua:sample()` | ≤128 敌人 × 约 20 次反射调用（`K2_GetActorLocation`、`GetCurrHP/GetMaxHP`、`VisualPivot.RelativeLocation`、`IsFacingRight`、`GetCurrent(i)×N`、`GetAnimationName/GetTrackTime/GetLoop/GetAnimationDuration`、`GetClass():GetFName():ToString()`、`Paused`、`bHidden`）≈ **2500 次 / 帧** | 每次：Lua userdata 构造 + 元表查找 + 参数编组 + `ProcessEvent` 或属性查找（按名找 FProperty）+ FName→FString→std::string 转换 + `pcall` 闭包分配 |
| 快照编码 `lua_codec` | 深度遍历 Lua 表 → `codec::value` 树 → 增量编码；每对端一次 | 每帧数千次 Lua 表读 + 字符串键哈希 + 树节点分配 |
| Client 呈现 `client/apply.lua` + `entities/*:render()` | 每敌人每帧 `K2_SetActorLocation` / `SetAnimation` / `SetTrackTime` / `SetFacing` / HP 写属性 | 同上 |
| HUD 快循环 `party/hud.lua:fast()`（`LoopInGameThreadAfterFrames(1)`） | 每座位每帧：`SummonSets:ForEach`（TMap 回调）+ 3 ×（`GetOrCreateCommandAction` + 4 个 getter）+ `ProjectWorldToScreen` + `IsGamePaused` + `GetDisplayPlayerUI` ≈ **25 次 / 座位 / 帧** | 同上 |
| 名单扫描 `registry.walk` / `hud.menuOpen` | 1 Hz `FindAllOf("Actor")`（遍历整个 GUObjectArray 并逐对象 Lua 回调 `IsValid`、`Tags`、`IsA`）；2 s `FindAllOf("UserWidgetNavigable")` | 数千对象 × Lua 回调；GUObjectArray 全遍历 |
| 事件泵 `coop/session.lua`（`LoopInGameThreadWithDelay(1)`） | 每帧 `netPoll` → C++ value → Lua 表 → 分发 | UE4SS 每个延时循环要过 `m_thread_actions_mutex` 递归锁与动作表 |
| 多个独立循环（party 500 ms、progress 1 s、devices 100 / 250 ms、menu 50 ms、events 16 ms、hud 1 帧 / 250 ms、session 1 ms） | 各自起 `LoopInGameThreadWithDelay` | 调度相位随机、无统一预算、不可按“无座位”整体关停（违反 G0） |

另外两类结构性成本：
- **双份模型**：同一实体的状态在 Lua 表与 C++（HUD 模型 / 快照）各一份，每帧搬运。
- **Lua 侧弱引用**：`ue.ref/deref` 每次都要跨边界；`GetAddress` 去重；`o == obj` 对 userdata 恒假。

实测（party-facts）：P1+P2+P3 本地在 60 档 P99 25–29 ms、144 档 P99 12–14 ms；Host 后台加载一张图 P99 33 ms。这些尾部主要是上面的边界穿越。

### 0.2 目标形态

| | Shipping 包 `ELMP/` | Dev 包 |
|---|---|---|
| 内容 | `dlls/main.dll` + `data/` + `elmp.json`（**无 `Scripts/`，零 `.lua`**） | 同上 + `main.pdb` + `pilot/`（Lua 库 + PowerShell）+ 可选 `Scripts/`（仅开发自测脚本） |
| Lua 依赖 | 不加载 UE4SS LuaMod；不注入任何 Lua 状态（第三方 Lua Mod 除外，见 §12） | 向 UEBridge / 第三方 Lua 状态注入 `ELMP_*` C 函数；义肢 Lua 库通过 UEBridge eval 加载 |
| 热路径 | 全部 C++，直读反射偏移，零字符串转换，零堆分配 | 同 Shipping；额外的观测计数 / 采样接口 |

### 0.3 性能预算（验收门槛，对应需求树 §8）

| 场景 | ELMP 游戏线程每帧占用（P99，60 fps） | 其他 |
|---|---|---|
| G0 无座位无会话 | 0 次反射调用；< 0.05 ms（只剩帧泵空转与输入线程） | 帧时间与裸 UE4SS 基线差 < 0.2 ms |
| 本地 P1+P3（HUD + 巡检） | < 0.3 ms | 覆盖层渲染线程 < 0.5 ms |
| Host + 2 客人 + 128 敌人（采集 + 组帧 + 加密） | < 1.0 ms | 每对端出站 ≤ 30 KB/s 典型；组帧零堆分配 |
| Client 呈现 139 敌人 | < 1.0 ms | |
| Host 后台 1 图有人战斗 | 原版 tick 成本另计；ELMP 增量 < 0.3 ms | |

---

## 1. 架构原则

1. **继承 ADR 0002**：补齐原版上下文、不模拟；三方证据；Hook 纪律；fail closed。
2. **逻辑与机制同层同语言**：不再有“跨语言的每帧数据搬运”；只有一个对象模型。
3. **反射只做一次**：所有属性 / 函数在类加载时按名解析为偏移 / 指针并校验（类型、尺寸、`CppType`），运行期直读；**解析失败即该能力 fail closed**（不猜偏移）。热字段的硬编码偏移（如 `+1465`）仍需反射或 IDA 双重校验，沿用 ADR 0002。
4. **句柄化身份**：`ObjHandle{index, serial}`（= `FWeakObjectPtr`）是唯一身份；注册表而非扫描；`FindAllOf` 只在世界就位时跑一次基线。
5. **单一帧泵**：`EngineTick` pre 回调是游戏线程的唯一 ELMP 入口；所有节拍（500 ms / 1 s）由它派生；无座位 / 无会话时阶段整体跳过。
6. **固定预算与可观测**：每阶段计时与计数（NFR-5）；超预算记录、不自适应降级（降级会隐藏 bug）。
7. **状态常驻、事件驱动更新（缓存优先）**：C++ 可以无代价地长期持有大量状态，所以**任何“每帧重新查询”的读取都先问能不能变成“变化时更新一次的缓存”**。具体做法：UFunction post 钩子 / `BeginPlay` / `EndPlay` / 对象删除监听 / 属性变化沿 作为缓存失效源；每帧只读缓存与少量必须每帧读的原始字段（位置、HP、Spine 轨道时间）。缓存清单见 §16.1。
8. **引擎访问集中在门面**：`engine/facades` 是“原版事实只写一次”的位置（ADR 0002），不是为了 mock；验证以真实实例为主（§13）。
9. **数据表外置**：催化表（物品文本、输入知识、图标索引、闸门类策略、RVA）全部 JSON / 二进制在 `data/`，由工具生成；代码不含大常量表。
10. **零新增依赖**：JSON（`base/json`，自写的极简解析 / 写出器——`RC::JSON` 依赖 UE4SS 的 File/Helpers 且为宽字符接口，与“base 不依赖 UE4SS、可 CTest”冲突，2026-10-25 实施决定）、PolyHook2、ImGui、fmt、ScopedTimer / tracy 均来自 UE4SS 目标；网络仍用 CNG。
11. **拿走知识，不照搬代码**：新仓库以 `D:\CodeT\ELMP` 的代码、文档、kb 为知识来源（原版入口、偏移、实测结论、踩过的坑），**不复制其代码文件**；每个模块按本架构重新实现。工作规则见根目录 `AGENTS.md`。

---

## 2. 分层与模块布局

```
native/
  base/        L0 平台：preflight, symbols(RVA), hook(PolyHook2), clock, log, metrics,
               storage(原子写), json_io, settings, events(环形总线), slots(租约)
  engine/      L1 引擎门面：reflect(属性/函数缓存), handle(ObjHandle), registry(对象注册表),
               frame(帧泵), lifecycle(世界状态机), ufunc_hooks(UFunction 钩子登记),
               facades/{actor,pawn,controller,hp,mp,heal,inventory,spirit,passive,
                        spine,locomotion,faction,clearable,world_loader,camera,save}
  core/        L2 LilyCore：spawn, profile(apply/report), vitals, skin, identity,
               permission(gates), hitrules, progress_source, catalog, player
  input/       虚拟手柄, xinput, rawinput, router, bindings, pad_state（沿用）
  party/       L3 本地多人：seat, devices, progress, respawn, transit, bench, camera_sync,
               team, persistence, save_scope, config, menu_ctrl, hud_feed
  coop/        L3 联机：protocol(typed), session(多对端), world_model, host/{capture,guest,
               events,maps}, client/{takeover,apply,presenter,vitals}, entities/{enemy,
               enemy_dyn,lily,worldobj,story,pickup}
  modes/       L3 玩法：hunter
  net/         传输：transport, crypto, codec_typed, snapshot(基线/增量), lua_codec(Dev only)
  guard/       结算拦截 / 存档保护 / 激活闸门 / IsPlayer 跳板（沿用）
  pvp/         命中窗口（沿用，接 core/hitrules）
  world/       level_instance + map_context（沿用）
  overlay/     L4 UI：overlay(Present/WndProc), hud_*, manager_*, menu_*, net_page_*,
               rules_page_*, fonts/icons
  console/     `elmp …` 控制台命令（ProcessConsoleExec 回调）
  sdk/         L5：elmp_sdk.h（C ABI）, abi_table, lua_inject(向第三方 Lua 状态注入；Dev 亦注入 UEBridge)
  dev/         Dev only：pilot_api, probes, frame_timing, lua_bridge(UEBridge)
  mod/         dllmain（CppUserModBase）、wiring
```

层间规则：
- `base` 不依赖 UE4SS（可单独编译、少量 CTest）；`engine` 依赖 UE4SS 并向上提供门面；`core/party/coop/modes` 只经门面碰 UObject；`overlay` 只读模型、不碰 UObject。
- `core` 不含玩法常数；`party/coop/modes` 不直接碰 UObject 细节（经 facades）。
- 原生 Hook 只在 `guard/pvp/world/input/camera` 与 `engine/ufunc_hooks` 安装，`core` 对外只暴露订阅。

---

## 3. 线程模型与帧泵

```
游戏线程                                   其他线程
┌──────────────────────────────┐          ┌─────────────────────────────┐
│ UEngine::Tick (pre callback) │          │ 网络线程 net::thread_main    │
│  frame::tick(dt):            │  mpsc    │  recv → Session::on_datagram│
│   1 lifecycle.step()         │◄────────►│  poll → emit_packets        │
│   2 input.pump()  (presses)  │ inbox/   │  （已存在，不变）            │
│   3 seats.tick()  (500ms 节拍)│ outbox   └─────────────────────────────┘
│   4 progress.tick()(1s 节拍) │          ┌─────────────────────────────┐
│   5 coop.host.tick()  每帧    │          │ 输入线程：RawInput 窗口 /    │
│   6 coop.client.tick() 每帧   │          │ XInput 轮询（已存在）        │
│   7 modes.tick()             │          └─────────────────────────────┘
│   8 overlay.publish()        │  seqlock ┌─────────────────────────────┐
│   9 metrics.end_frame()      │─────────►│ 渲染线程：DXGI Present detour│
└──────────────────────────────┘          │  ImGui 画 HUD/管理器/菜单    │
  UFunction / detour 回调（同线程，事件化）  │  菜单导航直读 XInput（暂停时）│
                                          └─────────────────────────────┘
```

- **帧泵**：`Hook::RegisterEngineTickPreCallback`（`[E-源码]` E9）。`on_update` 不用（它在 UE4SS 事件循环线程）。
- **阶段开关**：`seats/progress/overlay` 只在有座位时跑；`coop.*` 只在会话存在时跑；全部关闭时 `frame::tick` 只做 `lifecycle.step()`（读两个布尔）。
- **节拍**：`Cadence{period_ms, next}`，由帧泵驱动；取代 7 个 `LoopInGameThreadWithDelay`。
- **detour / UFunction 回调不做重活**：只写事件到 `base/events` 环或翻一个标志；重活在下一阶段消费（命中窗口例外：必须在窗口内改字段，沿用）。
- **覆盖层模型**：游戏线程写 `HudModel/ManagerModel/MenuModel` 的后台副本，`publish()` 原子换页（seqlock），渲染线程只读；菜单 / 管理器的用户操作经 SPSC 队列回到游戏线程（取代 `ELMP_MenuPoll/ManagerPoll` 轮询）。

---

## 4. 反射访问层（`engine/reflect`）

```cpp
// 一次解析、运行期直读
struct PropRef { int32 offset; int32 size; EPropKind kind; };     // 校验尺寸与类型
struct FuncRef { UFunction* fn; uint16 parms_size; uint16 ret_off; std::array<uint16,8> parm_off; };

class Schema {                     // 每个用到的类一份，世界就位 / 类首次加载时解析
  PropRef HPComponent_CurrHP, HPComponent_MaxHP, Faction_Faction, Pawn_Controller, ...;
  FuncRef HP_DoHeal, Inv_AddItem, Spirit_Equip, Spine_GetCurrent, ...;
  bool resolved; std::string failure;   // 任一项解析失败 → 该 Schema 不可用 → 依赖它的能力 fail closed
};
template<class T> T& field(UObject* o, PropRef p);          // *(T*)((uint8*)o + p.offset)
template<class R, class... A> R call(UObject* o, FuncRef f, A... a);  // 栈上参数块 + ProcessEvent
```

- 解析入口：`UObjectGlobals::StaticFindObject<UClass*>` + `FindProperty`（沿父链）；蓝图类（`BP_*_C`）在读档后才加载 → `Schema` 懒解析 + 失败不缓存（kb 教训）。
- **FName 比较用 ComparisonIndex**，不转字符串；需要字符串时只在注册（1 次）时做。
- **热字段直读**：`CurrHP/MaxHP/CurrMP/Faction/Paused/bHidden/bActorEnableCollision/RelativeLocation/RootComponent->ComponentToWorld` 等；`K2_GetActorLocation` 改为读 `RootComponent` 的变换（反射偏移）。
- **函数调用**：仅用于原版必须执行的副作用入口（`AddItem/EquipSpirit/FullyRestore/SetAnimation/SetFacing/LaunchAbilityFromDescription/TryMarkAsCleared/…`），每次只是一次 `ProcessEvent`。
- **Spine 姿态读取**（M24）：第一阶段用反射 `GetCurrent(i)` → `UTrackEntry` → `GetAnimationName/GetTrackTime/GetLoop`；**动画名经 `AnimTable` 内部化为 u16 id**（按 `(Skeleton 数据资产, 名字)` 建表，首次见到才做字符串比较）；第二阶段直读：`D:\CodeT\EnderLilies\Plugins\SpinePlugin` 带 spine-ue4 插件源码（`UTrackEntry::entry : spine::TrackEntry*`、`USpineSkeletonAnimationComponent::state : spine::AnimationState*`、`USpineSkeletonComponent::skeleton : spine::Skeleton*`，非 UPROPERTY），字段偏移按“相邻反射字段偏移 + 源码声明顺序”推出并用 IDA 对 `GetAnimationName` exec 体核对，写进二进制清单（§7.1）；之后 `state->getCurrent(i)->getTrackTime()/getLoop()/getAnimation()` 直读，`spine::Animation*` 指针身份即动画 id，彻底消除 FString 分配。两阶段接口相同。
- 参考实现：`D:\CodeT\LilyInEM\native\rt\ue.h`（缓存化查找 + 按 FProperty 偏移布参数的 `Call`，同时驱动 UE4 / UE5）。

---

## 5. 对象身份与注册表（`engine/handle`、`engine/registry`）

- `ObjHandle = {int32 index; int32 serial}`；`get()` 走 `FUObjectArray::IndexToObject` + serial 校验（即 `FWeakObjectPtr::Get`）。所有跨帧保存的引用只能是 `ObjHandle`。
- `Registry`：按 `Kind{Enemy, EnemyDyn, Lily, Interactable, Breakable, Collectable, Condition, Trigger, TravelVolume, RestingPoint, ActivationVolume, Menu(UserWidgetNavigable), PlayerController}` 分桶；每条 `Entry{ObjHandle, FName name, uint16 class_id, map_slot, net_id, flags}`。
- 维护：
  - `Hook::RegisterBeginPlayPostCallback` → 按 `IsA`（缓存 UClass*）分类入桶；`RegisterEndPlayPreCallback` + `FUObjectDeleteListener` → 出桶；
  - `FUObjectCreateListener` 只用于 **非 Actor** 关注对象（菜单控件 `UserWidgetNavigable`）；
  - 世界就位（`lifecycle` 判定）时跑一次 `FindAllOf("Actor")` 建基线；
  - 1 Hz `audit()`：遍历桶检查 `IsValid`（纯内存读，无反射），修正计数进 metrics。
- 网络身份：`Enemy` 的 `net_id` = `SourceSpawnPoint` FName 的 ComparisonIndex 映射到会话内 u16（名字表走可靠通道同步，Client 按名字反查本地对象）；同点多敌 `#k` 按地址排序规则不变。

---

## 6. Hook 层总表（取代 `data/RVA.json` 注释 + 散落的 Lua `RegisterHook`）

| 类型 | 目标 | 机制 | 所属 |
|---|---|---|---|
| detour | `InputConsume 0xCEF310` | PolyHook2 | input/virtual_pad |
| detour | `GameplayCameraUpdate 0xCF7B60`、`TargetBounds 0xCD63D0` | PolyHook2 | camera |
| detour | `AbilityEffectQueue 0xCBB110` | PolyHook2 | pvp |
| UFunction pre/post | `AbilityHitbox:OnActorOverlapBegin` / `ProcessActorEnter` / `Ability:ExecuteAbilityEffectsOnTarget` | `UFunction::RegisterPreHook/PostHook` | pvp |
| detour | `DoDamage 0xCA0380`、`SkillDamage 0xCB07E0`、`IsPlayerThunk 0xCE3C90`、`SaveGame 0xD25410`、`SaveGameAsync 0xD25550` | PolyHook2 | guard |
| detour | `SetActorEnableCollision 0x22C78D0`、`SetActorTickEnabled 0x22C8540`、`AZenithAIController::Activate 0xCCD8F0`、`AZenithCharacter::Activate 0xCCDAA0`（2026-10-26 定位，见 `docs/kb/engine-facts.md`；`0xCEFAE0` 是 `AZenithSpirit::SetActive`，不用于敌人） | PolyHook2 | guard |
| detour | `AbilityLaunchFromDesc 0xCAD0C0` | PolyHook2 | coop/host/events |
| detour | `ClearedQueryCurrentMapId 0xCAAFF0`、`ClearableBeginPlay 0xC997B0` | PolyHook2 | world/map_context |
| UFunction post | `PC_Base_C:OnSwitchGameMapStart / OnGameMapSwitch / OnGameMapReadyPostFade`、`WBP_FaderLoading_C:LaunchFadeIn` | UE4SS | party/transit |
| UFunction post | `SaveSubsystem:SaveGame`(同步成功)、`EVT_ACT_SaveAsync_C:OnSaveFinished`、删除 / 复制存档 post | UE4SS | party/save_scope |
| UFunction pre | `PC_Base_C:OnOpenNewArea`（PX 的 PC 抑制区域标题，§需求树 7.3） | UE4SS | party |
| UFunction post | `BP_Character_Player_Base_C:…HPDamageEvent`（damaged）、`DeathComponent` 死亡广播 | UE4SS → events | core/events |
| 引擎回调 | `EngineTick(pre)` 帧泵；`LoadMap(pre)` teardown；`BeginPlay/EndPlay` 注册表；`ProcessConsoleExec` 控制台；`StaticConstructObject(post)` 可选 | UE4SS Hooks | engine |
| 监听 | `FUObjectCreateListener/DeleteListener` | UE4SS | engine/registry |
| 导入表 | `XInputGetState` | IAT 改写 | input/xinput |
| vtable | DXGI `Present/ResizeBuffers` | PolyHook2 | overlay |
| 直接调用（不 hook） | `InteractablePressInteract 0xCE8BF0`（交互物 UE 动作 `"Interact"` 的 Pressed 处理，前缀校验；PX 按交互时对“交互者 = 该座位”的交互物调用，需求树 §7.16.1） | manifest 地址 | party/interact |

安装时机：`on_unreal_init` 预检 → 成功后装 detour 类（不依赖世界）；UFunction 类在目标 UFunction 首次可用时装（蓝图类读档后才加载 → 由 `lifecycle` 在世界就位阶段触发，失败不缓存重试）。卸载：`uninstall_mod` 逆序。

---

## 7. 模块设计

### 7.1 `base`
- `preflight`：不变（SHA-256 + 前缀）。`binary_manifest`：`data/binary/EL.json` **运行时加载**（取代 v1 构建期生成的 `symbols.h`）：支持的 EXE 哈希、每个 RVA Hook 的地址 + 前缀 + 用途、以及少数无反射的原始结构偏移（Spine 指针字段、`ZenithCharacter+1464/1465` 等）；每项都带来源标签，启动时逐项校验（前缀逐字节、偏移处相邻反射字段一致），任一失败 → 该项依赖的能力 fail closed。设计参考 `D:\CodeT\LilyInEM\native\core\binary_manifest.h`。
- `events`：不变（有界环 + 游标）；新增类型枚举 `{Damaged, Death, MapSwitchStart, MapReady, SeatJoin, SeatLeave, Rested, Recalled, Revived, PvpHit, NetPeer*, …}`；消费者：party / coop / modes / SDK / 义肢。
- `settings`：`Saved/ELMP/ELMP/settings.json`（`base/json` + 原子写），schema 版本；`set/get`、变更回调；`muteInput` 等瞬态项不落盘。
- `storage`：原子写（临时文件 + flush + `ReplaceFile`）+ `.bak`；通用 `load_json/save_json`。
- `metrics`：每阶段 `ScopedTimer`，直方图（P50/P95/P99）与计数器；`elmp perf` 控制台命令与义肢读取。

### 7.2 `engine`
- `frame`：§3。`lifecycle`：状态机 `Title → Loading → Settling(存档身份判定, P1 就位, IsGameMapReady) → Settled → TearingDown`；`onTeardown/onSettled` 订阅；`SwitchGameMap` 不经 LoadMap，仍用 `transitioning` 标志（来自 transit 钩子）。
- 原版访问经缓存化`reflect`/`Schema`，只暴露项目用到的读写；参数以实际FProperty布局为准。HP是原版int32，权威伤害走Host入口；Client远程HP是结果字段，不通过伤害入口合成表现。姿态接口实际位于`coop/presentation`，共享`net::snap::Pose`（最多8条实际轨道，包含实例代次/相位/暂停），不再保留早期3轨`SpineFacade`示意契约。WorldLoader的地图与加载状态由`engine/world`提供。

### 7.3 `core`（LilyCore）
- `slots`（租约，不变）、`spawn`（M3，C++ 化：PC 池、临时替换 CameraManager 类、Spine 补数据、Possess）、`profile`（M4：`read/applyIncremental/applyExact/reset/report`）、`vitals`（M4/E3）、`skin`、`identity`（Tags `ELMP` / `ELMP.Slot.N` + 句柄）、`permission`（C5：按座位开关 + 类策略表 `data/gates.json` → `IgnoreActorWhenMoving` 批量施加 / 撤销；新对象入桶时补设）、`hitrules`（C4，对接 `pvp/hitgate`）、`progress_source`、`catalog`（从 `data/catalog/*.json` 读物品文本 / 输入知识 / 技能表）、`player`（slot → PC/Pawn 解析，带句柄缓存）。
- 对外 = `sdk/abi_table` 中的函数集合；内部模块互相直接调用。

### 7.4 `party`
- `seat`：座位表（slot → `SeatRec{input kind, identity, perms, map_slot, remote peer, handles}`）。
- `devices`：设备枚举 / 分配 / 绑定持久化（JSON），`syncSeats` 剔除 remote。
- `progress`：C6；快照结构体 `ProgressSnap{exp, cur[3], rows[4], spiritLevels[26], level, healLevel, slots}`，签名 = 64 位哈希；1 s 节拍；读回确认。
- `respawn`：死亡计时、丢 Pawn 兜底、`respawnNear`、召回判定、摆位。
- `transit`：过图状态机（UFunction post 钩子驱动）；冻结 = 每帧 `DisableMovement` 重压（直到 FadeIn）。
- `bench`：椅子（近距判定 + 累计按下边沿 + 冷却句柄绑定）。
- `camera_sync`：注册 / 注销 Ally 目标 + 放宽参数下发到 `camera` detour。
- `team`：slot → team，推送 `pvp::bind`。
- `persistence/save_scope`：C12（JSON 取代 `.profile.lua`；提供只读迁移器读取旧 `party_<saveId>.profile.lua`）。
- `hud_feed`：每帧填 `HudModel`：头顶锚点（`ProjectWorldToScreen` 保留 1 次 / 座位 / 帧；或读 `PlayerCameraManager` 视图投影矩阵自算，二期）；技能格状态：**只在变化沿更新** —— 订阅 `SpiritEquipComponent.OnEquip/OnUnequip` 与 `SummonSet` 切换（UFunction post）重建 3 个 `CommandAction` 句柄；每帧只读 3 个动作的 `GetRemainingCastValue/GetCooldownTime/GetRecastTime`（直读属性优先，`[待 IDA]` `CommandAction` 内剩余值字段偏移；拿不到则 3 次 ProcessEvent）；菜单打开态由注册表 `Menu` 桶的 `IsInViewport && IsVisible` 低频判断。

### 7.5 `coop`
- `protocol`（typed）：见 §8。
- `session`：多对端表（已是 C++ `netsess`），事件直接以结构体分发（不再经 Lua 表）。
- `world_model`：Host 侧与 Client 侧共用的“行”结构体与驻留表（`at` 时间戳、GONE/PURGE 语义不变）。
- `host/capture`：每帧：P1 行、客人行、`lv` 行、各 adapter `writeHost` 写入 `FrameSnapshot`（struct-of-rows，预分配容量 128/64/512）；按对端兴趣过滤 → `SnapSender(peer).encode(base)`。
- `host/guest`：对端 → 座位；暂扣 / 部署；500 ms 停滞松键；档案 / 构筑按 `guestKey` 存取（C12）。
- `host/events`：`act` 捕获队列（detour 写入 → 帧内可靠下发）、`map/mapReady` 闸门、`control`。
- `host/maps`：后台图槽位、生命周期、`toLocalY/locateY`。
- `client/takeover`：接管配方（逐组件关 tick、白名单、`CanBePaused=false`、`guard::markEnemy`、激活体积源头冻结、1 Hz 两标志复检）；`client/apply`：行 → 幂等 Set；`client/presenter`：锚点 / 相机清洗 / 进入离开；`client/vitals`：`CurrHP/CurrMP` 属性直写、`UseHeal/RestoreHeal` 计数对齐。
- `entities/*`：与 ADR 0003 同一契约（`claimHost/scanDoneHost/writeHost/claimClient/takeover/observe/render/onEvent/reset/unpuppetAll`），改为 C++ 虚接口 `IEntityAdapter`。

### 7.6 `modes/hunter`
- 对局状态机 + 规则（需求树 C19），订阅事件总线（速通选手死亡 / 到图 / Boss 击败）；指南针数据进 `HudModel`。

### 7.7 `overlay`
- 沿用现有 ImGui 实现；模型改为 seqlock 双缓冲；新增“联机页”“规则页”“存储确认”。`menu_ctrl`（游戏线程）消费菜单请求队列，按原版规则执行装配并回写 `MenuModel`；Client 侧把请求改发可靠通道（F2.10），回执更新模型。

### 7.8 `console`
- `Hook::RegisterProcessConsoleExecCallback`：解析 `elmp …`，显式只响应 Controller0 的 ProcessConsoleExec；命令表与 v1 一致（`join/leave/seats/team/pvp/skin/rest/menu/progress/storage/set/mute/lan/maps/hunter/perf`）。

### 7.9 `sdk`（§12）与 `dev`（§11）。

---

## 8. 联机数据流（全 C++、类型化协议）

### 8.1 快照（不可靠最新，按对端增量）

```
FrameSnapshot {
  uint32 t_ms; uint16 map_id;                     // Host 时钟、地图 id（名字表）
  LilyRow   l[≤8];   // 键 seat：pos(3×f32) f(i8) hp mhp(u16) pose ix iy(i8) iraw(u16) ic[9](u16 累计) prof_ver skin
  VitalRow  lv[≤8];  // hp mhp mp mmp pr mpr al
  EnemyRow  e[≤128]; // 键 net_id(u16)：pos vz f hp mhp ty(u16) flags{a,h,dy} pose{anim u16,t f32,loop}+tr[≤2]
  DynRow    ed[≤64]; // 键 dyn:N：同上 + cl(u16 类 id)
  WorldRow  w[≤512]; // 键 名字 id(u16)：cleared(bit)
  GoneRow   d[≤512]; // 键 名字 id：消失
  StoryRow  st[≤128];// 键 名字 id：ConditionResult
  PickRow   pk[≤64]; // 键 dyn id：pos cls(u16)
}
```

- **编码**：每节“键 + 字段位掩码 + 变化字段”；增量相对对端已确认基线（`SnapSender` 保留最近 N 个基线，`snap_ack` 捎带）；缺节 = 无变化；行删除显式（`d` 节或“删行”位）。**字符串从不进快照**：类名 / 动画名 / 出生点名 / 地图名 全部走 `NameTable` 可靠通道（新 id 首次出现前先可靠发送 `name_def{id, utf8}`；Client 收到快照时若有未知 id → 丢该行并计数，等待名字定义）。
- **兴趣管理**：按对端所在图过滤节；坐标局部化（`maps.toLocalY`）。
- **预算**：组帧零堆分配（固定容量数组 + 写游标）；加密后数据报 ≤1200；同流最新语义不变。
- 旧的通用 `codec::value` 增量树**仅保留给 Dev 的 `lua_codec`**（义肢读取）与少量可靠事件（档案 / 构筑表，低频，可用 JSON 字节串）。

### 8.2 上行输入（不可靠最新）
`InputMsg{ep u16, x i8, y i8, raw u16, c[9] u16, snap_ack u32}`（与 v1 语义一致）。

### 8.3 可靠有序
`Hello/Welcome/Reject{Full|Version|Code}`、`NameDef`、`Act{net_id, seq, desc 白名单字段}`、`Map{map_id}` / `MapReady{map_id}`、`Profile{json bytes}`、`Build{guestKey → 构筑 json}`、`EquipRequest/EquipResult`（F2.10）、`StorySet`、`Control{WorldLost…}`、`Close{reason}`。

### 8.4 Client 应用顺序（每帧）
`inbox drain → observe（行入驻留表）→ render（插值 / 姿态 / HP / 显隐 / 变身）→ takeover.tick（新敌人接管、1 Hz 复检）→ hud(vitals) → followCamera`。

---

## 9. 多地图（C18）

- `host/maps`：槽位表 `{map_id, offset_y, instances[3], seats[], last_empty_at}`；加载 `world::level_instance::load(pkg, offset, name="ELMP_BG_<map>__<part>")`；卸载宽限 10 s；P1 要进后台图 → 先卸载再原版切图。
- `world/map_context`：不变；`bind(pkg_token → map_id)`。
- **单值假设工作清单**（需求树 §7.3 的 12 个未分类入口）：实施 C18 前，对 `0xC98210 0xC98D80 0xC99770 0xC9A9F0 0xCA2820 0xCB6240 0xCCDD10 0xCCE900 0xCD5060 0xCD52D0 0xCDC950 0xCDD110` 逐个 IDA 分类为 {前台表现 / P1 专属 / 需上下文窗口 / fail closed}，结论写进 `net-facts.md`；在此之前后台图对这些路径的事件 fail closed（拒绝并提示）。
- 座位切图（远程 / 猎人）：在该座位所在图实例内判定出口盒（`BP_WorldTravelVolume_C` 的 `GameMapToLoad/PlayerStartTag`，坐标加偏移）→ 目标图加载 → 暂扣 → 落到目标图 `PlayerStart(tag)`（加偏移）→ 部署；Boss 房目标图拒绝（§需求树 7.1.3）。

---

## 10. 持久化与数据

| 文件 | 内容 | 格式 |
|---|---|---|
| `Saved/ELMP/ELMP/settings.json` | 规则 / 镜头 / PvP / 召回 / equipAnywhere | JSON, schema v |
| `Saved/ELMP/ELMP/devices.json` | 设备分配、每座位绑定 | JSON |
| `Saved/ELMP/ELMP/save_index.json` | saveId 世代、指纹 ≤64 | JSON |
| `Saved/ELMP/ELMP/party_<saveId>.json` | 该档队伍 / 装备 / 皮肤 / 身份 / 加入名单 / 客人构筑（按 guestKey） | JSON |
| `Saved/ELMP/<Mod>/…` | 第三方数据 | 由 SDK `storage` 提供 |
| `data/binary/EL.json` | EXE 哈希、RVA + 前缀、无反射的原始结构偏移（含来源标签），运行时加载校验（§7.1）；内容从 v1 `data/RVA.json` 与 kb 迁入 | JSON |
| `data/gates.json` | 触发器类策略（类路径 → story/world）、默认开关 | JSON |
| `data/catalog/{item_text,inputs,spirits,passives,skins}.json` | 由 `tools/icons`（扩展为 `tools/catalog`）从游戏包生成，取代 `catalog/*.lua` | JSON |
| `data/icons/*.png`, `data/fonts/*` | 不变 | |

迁移：首次启动若存在旧 `*.profile.lua`，由只读迁移器（极简 Lua 字面量解析，仅支持旧文件实际用到的子集）转为 JSON 并保留原文件；失败则明确提示、不覆盖。

---

## 11. Dev 构建（`ELMP_DEV`）

- `dev/lua_bridge`：`on_lua_start` 向 **UEBridge**（及声明依赖的第三方）注入 `ELMP_*`；Shipping 构建中该文件仍编译但只对第三方注入 SDK 子集（§12）。
- `dev/pilot_api`：观测（`state/observe/eventsSince/frameTimes/perf`）、操控（`padSet/padTap/allowPlayer1`）、测试辅助（登记计数）；义肢 Lua 库（`pilot/lua`）与 PowerShell 脚本不变。
- `dev/probes`：`ELMP_Probe*`：读任意座位 / 敌人行的 C++ 模型、快照 dump（`lua_codec` 把 `FrameSnapshot` 转 Lua 表，仅 Dev）。
- `net/lua_codec`：Dev only。
- Dev 包可带 `Scripts/`（自测脚本），但 ELMP 自身逻辑不依赖它；`mods.txt` 对 Shipping 只需 CppMod。

---

### 11.1 Agent 工具链（2026-11-04）

- `tools/mcp_client.py` 是现有 MCP 的客户端，不新增游戏服务。提供工具清单、schema、调用与同会话批处理；CUE4Parse 进程内的 provider 必须先挂载，IDA 必须验证已打开的数据库身份后再查询。调用有超时，错误返回非零，完整 JSON 可保存到仓库外或被忽略的 `build/toolchain-smoke/`。
- `tools/toolchain.py` 汇总本地依赖与只读 smoke，按配置连接 CUE4Parse / IDA 与运行实例的 UEBridge；工具连接路径、端点与数据库路径归 `devconfig.json` / `devconfig.local.json`。不把本机路径写进运行时 DLL，不从旧仓库复制工具代码。
- `tools/ida_service.py start` 幂等启动已有 idalib supervisor（仅 loopback、无 GUI），复用经身份核对的工作库会话；工作库不存在才复制原 `.i64`，关闭自动分析，不覆写原库。`status` 只读；服务只在显式 `start` 时启动，doctor/smoke 不隐式启动服务或游戏。
- Codex 的 MCP 配置连接已验证的现有服务；本机配置与服务启动不等同于当前会话已加载原生工具。命令行 MCP 客户端作为立即可用的入口；工具清单是否加载以会话实际可用工具为证。
- 检查不向游戏发输入、不切图、不重启实例、不操作前台窗口。截图在 `IsIconic` 或客户区无效时停止；资产提取参数先校验再启动 .NET；测试临时目录仅作用于构建命令进程，不改系统环境。

---

## 12. SDK（C20）

- `sdk/elmp_sdk.h`：C ABI 版本化函数表 `ElmpApi_v1{ acquire, release, spawn, despawn, respawn, profileApply, profileRead, setInput, state, setSettings, setPermission, setTeam, hitRule, subscribe, unsubscribe, storagePath, log }`；C++ Mod 通过导出 `ELMP_GetApi(version)` 获取；身份 = 调用方 Mod 名（由 `elmp.json` 声明 + 加载顺序登记）。
- Lua Mod：`on_lua_start(mod_name)` 读取 `Mods/<mod>/elmp.json` → 校验版本 / slot 范围 → 注入同一函数表的 C 绑定（`ELMP_*`）；**不注入任何 Lua 源码**。语法糖 `elmp/core.lua` 放在 SDK 示例仓库由第三方自带。
- 事件订阅经 `base/events` 游标（每 Mod 一个游标）；回调在游戏线程帧泵末尾派发。
- 第三方对象不进联机同步（状态报未支持）。

---

## 13. 验证策略（真实实例为主，静态测试只保留“在游戏里测不出来”的）

v1 积累了 270 个离线 Lua 测试 + 84 个原生测试，其中相当一部分是对 mock 的断言（典型教训：mock 自己发明了原版没有的 `SetCurrHP`，测试全绿但游戏里全部写入失效）。v2 **不迁移这些测试，也不建 FakeEngine / Lua mock 层**。

| 做 | 不做 |
|---|---|
| **真实实例验证为主**：每个功能的验收 = 在 Dev2 / Host2 / Client2 上用义肢驱动 + 截图 + `elmp` 状态命令 + 帧时间，结论写进 kb（需求树 §8 口径） | 为每个模块写单元测试；为巡检 / 座位 / 过图 / 复活 / HUD 等“游戏内才有意义”的逻辑写 mock 测试 |
| 少量 CTest，只覆盖**纯逻辑且游戏内难以穷举输入**的部分：typed 编解码（含恶意 / 畸形输入边界）、传输状态机（丢包 / 乱序 / 重复 / 超限队列）、CNG 握手、存储原子写与 schema 校验、预检前缀比对 | 覆盖率目标、CI 跑游戏、把 Lua 旧测试逐条翻译成 C++ |
| 回归护栏 = **义肢回归脚本**（启动实例 → 读档 → 加座位 / 联机 → 一组可观测断言 → `ops.summary()`），每阶段门槛前跑一遍 | 新增测试前先问“这条断言在真实实例里 5 分钟能不能验证”；能就不写静态测试 |

`elmp perf` / `ops.frameTimes` 结果进 kb 作为性能基线。

---

## 14. 构建与发布

- CMake 目标：`elmp_base`（不依赖 UE4SS 的平台 / 传输 / 编解码）、`ELMP`（Shipping dll）、`ELMP_Dev`（`ELMP_DEV=1`）、`elmp_tests`（§13 的少量 CTest）。
- 新仓库独立于 `D:\CodeT\ELMP`：自己的 `RE-UE4SS` submodule（同一提交 `32d8a381`）、`devconfig.json`（指向同一组游戏实例 Dev2 / Host2 / Client2）、`tools/deploy.py`。
- `build.bat package`：Shipping的`ELMP/`只含`dlls/main.dll`、`data/`、`elmp.json`，SDK头随zip放在外层`sdk/`，不进入运行时Mod目录。Shipping不打包`Scripts/`、`pilot/`、pdb；打包脚本断言zip零`.lua`及Mod目录白名单。
- `elmp.json` 新增 `"shipping": {"lua": false}` 供安装说明生成。

---

## 15. 迁移计划（按需求树关键路径，每阶段有对等门槛）

前提（根目录 `AGENTS.md`）：新仓库 `D:\CodeT2\ELMP` 从零实现，旧仓库只作知识来源；开发实测时 Agent **可以直接删除实例里旧仓库部署的 `Mods/ELMP`，部署新仓库的 ELMP**（三个实例都是隔离实例，不碰正式游戏）；每阶段的门槛以真实实例验收为准。

| 阶段 | 内容 | 门槛（必须全绿才进入下一阶段） |
|---|---|---|
| **P0 骨架** | 新仓库骨架（CMake、submodule、`devconfig.json`、`deploy.py`）、`base/{preflight,symbols,hook,settings,json_io,metrics,events,slots}`、`engine/{reflect,handle,registry,frame,lifecycle}`、`console`；部署到 Dev2 替换旧 Mod | 预检通过；无座位 G0 预算达标；注册表与一次性 `FindAllOf` 基线一致（审计计数 0） |
| **P1 core + 本地多人** | `core/*`、`input/*`、`party/*`、`overlay` 模型双缓冲、`hud_feed`、设置持久化、规则 1/2、触发器类策略 | v1 全部本地功能对等（义肢回归脚本）；P1+P3 预算达标 |
| **P2 同图联机** | `coop/*` typed 协议、`entities/*` C++、客人装备通路、联机页 | FR-NET-14 余项（世界对象 / Boss / 联机 PvP / 断线重连 / 帧率录像）通过；Host+1 客人预算达标 |
| **P3 多客人 + 多地图** | 三实例联调；单值假设 12 入口分类；后台图切图 / 复活 / Boss 房拒绝 | 两客人同图 G1–G4；一客人后台图战斗 + 清除记账正确；性能 |
| **P4 猎人模式 + SDK** | `modes/hunter`、C ABI、Lua 注入、示例 | 猎人规则验收；示例 Mod 跑通；Shipping zip 零 `.lua` 断言 |
| **P5 发布** | 文档、INSTALL、ADR 0005 定稿 | |

旧 → 新 对照（**知识映射，不是文件搬运**）：`lua/elmp/core/*` 的原版入口与顺序 → `native/core/*`；`lua/elmp/party/*` 的规则与阈值 → `native/party/*`；`lua/elmp/coop/**` 的协议语义 / 接管配方 / 行驻留语义 → `native/coop/**`（协议换 typed）；`lua/elmp/modes/hunter.lua` 规则 → `native/modes/hunter.cpp`；`lua/elmp/core/catalog/*.lua` → 由工具重新生成 `data/catalog/*.json`；`tests/lua/**` → 不迁移（§13）；`pilot/**` 的用法与脚本 → 新仓库 `pilot/`（Dev 可按需重写，允许保留同样的 ops 接口名以复用使用习惯）。旧仓库 C++（`native/net`、`guard`、`pvp`、`input`、`overlay`、`world`）同样只作为参考实现：读懂其中的原版事实与边界处理，在新仓库按 §2 布局与 §4/§5 的新基础设施重写。

---

## 16. 性能设计要点（逐热路径对照 §0.1）

| 热路径 | v2 做法 | 预期 |
|---|---|---|
| Host 敌人采集 | 注册表桶遍历；位置 / HP / 标志直读；`coop/presentation`按预检过的原生tracks范围找实际活跃轨道，再经原版反射getter采样，最多8轨；名字按TrackEntry代次缓存。当前不是“每敌人≤4次反射”的旧假设 | 128敌人预算仍须压力实测；已测窗口见同步修正kb |
| 快照编码 | typed 位掩码增量，零字符串、零堆分配 | 每对端 < 0.1 ms；带宽 ↓ 3–5× |
| Client 呈现 | 行驻留表直索引；位置经原版setter插值；姿态按实际轨道索引和实例代次创建/移除，loop及属性变化单独应用，每快照核对实际相位并应用暂停；组件空间变换、颜色和Dissolve各有有效位 | 139敌人<0.5ms是预算，不能由24敌人场景推定通过 |
| HUD 技能格 | 变化沿重建句柄；每帧 3 × 读值 | 每座位 < 20 µs |
| 菜单打开判定 | 注册表 `Menu` 桶 250 ms 低频 | 可忽略 |
| 名单扫描 | 取消（BeginPlay/EndPlay 驱动）；1 Hz 审计只读 `IsValid` | 可忽略 |
| 事件泵 | 结构体 inbox，帧内 drain | 可忽略 |
| 周期循环 | 单帧泵 + 节拍；无座位整体跳过 | G0 零开销 |

### 16.1 常驻缓存清单（原则 7 的落地：什么被缓存、谁让它失效、每帧还读什么）

| 缓存 | 内容 | 失效 / 更新源 | 每帧仍需读的原始字段 |
|---|---|---|---|
| `Schema` | 每类的属性偏移 / UFunction 指针 / 参数布局 | 类首次加载（`StaticConstructObject` post 或 `Settled`）；游戏更新 → 预检失败 | 无 |
| `Registry` | 关注对象的句柄、FName、类 id、所在图槽、网络 id | `BeginPlay`/`EndPlay`/删除监听；世界就位一次基线 | 1 Hz `IsValid` 审计 |
| `SeatCache` | slot → PC / Pawn 句柄、各组件（HP/MP/Heal/Spirit/Passive/Spine/Locomotion/Input）指针、碰撞组件列表 | Possess / UnPossess / 复活 / 离开（UFunction post `OnPawnChanged`、ELMP 自身操作） | 位置、HP/MP/祈祷当前值 |
| `SkillCache` | 每座位 3 个装配位的 `CommandAction` 句柄 + 限制类型 + 图标 id + 键名 | `OnEquipSpiritDelegate/OnUnequipSpiritDelegate`、`SwitchSummonSet`、精灵等级变化、Pawn 重建 | 剩余次数、冷却 / Recast（3 × 读值） |
| `MenuOpenCache` | 原版可导航菜单是否在视口 | `UserWidgetNavigable` 创建 / 删除监听 + 250 ms 复核 | 无 |
| `ProgressSnap` | 每座位进度签名与基线 | 1 s 节拍重算（只读行计数与等级，不遍历物品文本） | 无 |
| `EnemyTrack`（Host） | 每敌人上一帧行 + 增量位掩码；名字表 | 行值变化；名字首次出现；图/会话重置 | 位置 / HP / 标志 / 实际Spine姿态与视觉输出 |
| `EnemyTrack`（Client） | 驻留行、插值样本、接管记录 | 快照到达；地图/会话重置；ObjHandle失效 | 应用时核对实际轨道相位；1Hz核对AI激活位 |
| `GateCache` | 每座位已施加 `IgnoreActorWhenMoving` 的目标集合（按句柄） | 目标对象入桶 / 出桶；权限开关变化 | 无 |
| `MapCache` | `CurrentGameMapID`、`IsLoading`、`IsGameMapReady`、后台图槽位与偏移 | 过图钩子（`OnSwitchGameMapStart` / `PostFade`）、后台图加载 / 卸载 | 每帧 1 次读三个字段（直读偏移，便宜） |
| `NameTable`（联机） | 类名 / 出生点名 / 地图名 / 动画名的会话内 u16 id 双向表 | 首次出现时可靠发送 `NameDef` | 无 |
| `HudModel/ManagerModel/MenuModel` | 覆盖层只读快照 | 游戏线程 `publish()` | 渲染线程只读 |

规则：缓存项必须带**失效源**与**审计方式**（低频对账或计数），没有失效源的缓存不允许存在（否则就是“做一次就记住”，违反 FR-CORE-6）。

---

### 16.2 远端呈现职责（2026-11-04，ADR 0008）

按 [ADR 0008](adr/0008-authoritative-presentation.md) 执行：Host 权威结果 → 统一 `coop/presentation` 适配器 → 单一 Spine/组件输出写者；`presentation_policy` 负责关闭决策 tick 与根运动，guard 独立负责权限。删除全局 simLocal/animLocal/fxLocal 与 Cmd 试验，不全面解冻原版逻辑。普通/动态敌人与远程 Lily/Spirit/Companion 共用采样和应用接口。

Companion `cp` 基线包含 Actor 世界位置及 pitch/yaw/roll；世界旋转通过原版 Actor getter/setter 复制，与 facing、VisualPivot/renderer 相对变换分开。协议6增加三float，48行容量保持，必须以最大全量及全部换key的delta验证128KiB上限，不能因模型资源相同而假设 spawn 默认朝向正确。

呈现缓存按 Actor/组件/TrackEntry 的 `ObjHandle` 识别，销毁/对象替换、地图代次和会话结束失效；采样时复核句柄，低频清掉失效项并暴露计数。动画实例身份独立于名字；有效位独立于数值。持续 FX 的 ID 绑定 Host 实例，晚加入不换 ID，停止/换图/会话结束清理。guard 新增已核的 Effect Apply/Finalize/DeathStart 原生拦截，Client armed 时始终拒绝、分别计数，不能借 damage replay 开窗；身份缓存也使用 ObjHandle。实际覆盖和未闭合链路以新玩家/敌人图谱与本轮 kb 为准，不把 ADR 0007 的预期当事实。

| 新呈现缓存 | 身份与失效源 | 审计 |
|---|---|---|
| Pose采样/应用 | Actor、骨骼、TrackEntry句柄及原生state身份；track实例变化、skin更换、60秒未使用、图/会话重置 | samples/applied/failed/overflow/restarted/cleared/paused/phaseCorrected |
| Visual组件/材料schema | Actor与组件/类句柄；组件死亡、skin更换、图/会话重置；每次读取活MID数组，未缓存裸MID | writes/failures，dissolve missing/disagree/invalid/schemaFailed |
| presentation_policy | Actor/Controller/RootMotion组件句柄；release/reset/死亡；根运动原值只在其对应对象存活时恢复 | ready/failed/rootDrift/restored/expired |
| Companion身份/副本 | AIC→LeaderPawn→玩家PC对应seat；玩家归属变化、实体死亡、图重置；ID不在同会话复用 | hostIds/copies/ownerConflict/leaderMissing/capacityOverflow/policyFail/fxMissing |
| 持续FX | Host激活ID+PSC句柄，逐peer的Ref/地图投递绑定；停止、对象失效、离图、会话结束 | live/baselines/bindingsClosed；Client kept/replaced/stopMissed |

## 17. 风险与对策

| 风险 | 对策 |
|---|---|
| 反射偏移在游戏更新后漂移 | `Schema` 启动校验（名字 / 尺寸 / 类型）失败即 fail closed 并报具体字段；与 EXE 哈希联动 |
| 蓝图类加载时序（`BP_*_C` 读档后才有） | 懒解析 + 失败不缓存；`lifecycle.Settled` 触发重试 |
| UFunction 钩子与 UEBridge `hook_function` 同函数冲突 | 同 v1：同一 UFunction 只装一处；Dev 文档标注 |
| typed 协议演进 | 协议版本号单源；字段位掩码天然向后扩展；任何不兼容 → 版本号 +1 → 握手拒绝 |
| 名字表未同步导致行丢弃 | `NameDef` 先于首次使用可靠发送；Client 计数 `unknownName` 可观测；晚加入由全量 + 名字表重放保证 |
| C++ 重写回归 | 阶段门槛 + 义肢回归脚本（真实实例）；旧仓库 kb 里的实测结论作为逐项核对清单 |
| ImGui 双缓冲竞态 | seqlock + 单写者；渲染线程只读快照 |
| 第三方 Lua Mod 依赖旧 `require('elmp.core')` | SDK 仓库提供同名语法糖；API 名不变，语义化版本 1.0.0 |

---

## 18. ADR 更新清单

- **新增 ADR 0005**：Shipping 纯 C++、单帧泵、注册表、typed 协议、C ABI SDK（本文件 §1–§12）。
- **ADR 0001**：模块表改指向 `native/core/*`；“跨 Lua 状态的数据放原生层”原则自然成立。
- **ADR 0003**：文件布局节替换为 §2；决定一～五不变。
- **ADR 0004**：补充 §9 单值假设工作清单与 Boss 房 fail closed。
- 新仓库 `AGENTS.md`（已写）：拿走知识不照搬代码、Shipping 零 Lua、真实实例验证为主、可替换实例中旧仓库部署的 Mod；旧仓库 `AGENTS.md` 不再维护。
