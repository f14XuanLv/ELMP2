# 呈现管线事实：为什么“减法”是对的（v2 2026-10-29，P8 调研）

证据来源：`[E-IDA]` IDB `EnderLiliesSteam-Win64-Shipping.exe.i64`（基址 `0x140000000`，无游戏符号，函数地址经反编译形态与 P7 分支 kb / EL.json 交叉确认）；`[E-SDK]` `D:\CodeT\EnderLilies\Source\Zenith\Public` 重建头（只证声明与字段）；`[E-CUE]` CUE4Parse 0.2.0（`EnderLilies` 17,384 包，usmap 已挂）。本文件只写事实与推论，不写实施方案。

## 结论（先给答案）

1. **存在一个统一入口**：`AZenithCharacter::Tick`。每帧顺序固定：消费输入 → 输入快照进命令队列 → 由输入生成命令 → 推进命令队列 → 推进行走/攀爬状态组件。表现（Spine 动画、技能 Actor、位移、特效、音效）全部由这条链的下游**推导**出来，没有任何一条“表现事件”是外部喂进去的。
2. **一条命令 = 五条数据时间线**：`FCommandAction` 的 `StateTimeline / AbilityTimeline / MoveTimeline / FXTimeline / SoundTimeline`（`[E-SDK]` 字段名；`[E-IDA]` Start 处五个初始化调用一一对应）。复制命令 = 五条通道同时到位。
3. **动画不是“按名字逐条播”**：每个角色只有一个 `USpineAnimationComponent`，它按层（`LOCOMOTION / ACTION / REACTION / DEATH / EVENT…`）从**状态组件 + 行走组件 + 移动组件 + 输入**推导该播什么、何时切、混合多久。P6 的“复制动画名到轨道 0”是在复制这个组件的**输出**，天然丢相位、丢过渡、丢层。
4. **状态是类**：`UStateComponent` 维护一组 `UState` 子类（`StateDash / StateDead / StateBusy / StateAutoClimb / StateHitStop …`），加/删状态会广播；`SpineAnimationComponent` 与 `CommandQueue` 都监听它。命令只是**往状态组件里加状态**的其中一条路径，坐椅 / 攀爬 / 死亡等状态另有入口。
5. 因此：**找大块头做减法是对的；P6 的加法路线是在重新推导一个已经推导出来的产物。** P7-1 的方向对，但它把“大块头”选成了命令，而不是“输入 + 角色组件栈”，这才是它覆盖不全（坐椅 / 梯子 / 死亡姿态）的结构性原因。

## 1. 每帧入口：`AZenithCharacter::Tick`（`0xCF3740`）`[E-IDA]`

反编译形态（参数 `float dt`）：

```
AActor::Tick(this, dt)
if (virtual +1728 && virtual +1720)                    // 能否行动（状态 / 死亡）
if (this->InputComponent)                              // this+168
    UZenithInputComponent::InputConsume(input, ...)     // 0xCEF310，NextInputSnapshot → InputSnapshot
    snapshot = input->InputSnapshot                     // +224，40 字节 FInputSnapshot
if (this->CommandQueueComponent)                       // this+165
    SetInputSnapshot(cq, snapshot)                      // 0xCBCA20
    if (canAct) TryAddCommands(cq, snapshot, this->CommandSet)  // 0xCC2520，输入 → 命令
    UpdateCommands(cq)                                  // 0xCC3970，推进命令队列
if (this->LocomotionComponent)                         // this+174
    Tick(locomotion, dt, snapshot, canAct)              // 0xCBAE40
```

`0xCBAE40`（行走组件 tick）：算地面 / 墙 / 泳 / 攀爬参数，得出移动状态 1/2/3，写 `+236`，调 `0xCF1E70(component, state)` 广播移动状态；朝向走 `0xCBC550`。

**含义**：输入是唯一决策源，命令队列是输入的产物，行走状态与命令并行；三者都在同一个 tick 里、同一帧次序上。Client 只要把**输入**放进来，整条链就自己跑；放**命令**进来则少了“输入 → 命令”这一段对状态的依赖（也会漏掉非命令状态）。

## 2. 命令 = 五条时间线（`0xD29F90` CommandAction::Start）`[E-IDA][E-SDK]`

Start 的形态：

```
if (+498) log "Trying to start a CommandAction that is not ended..."
virtual +648(this)                       // OnStartCommandAction
if (+408 && +464) sub_140CBEBF0(+464, 100)
...
sub_140D24CA0(this + 88,  actor)         // StateTimeline
sub_140CBD310(this + 128, actor)         // AbilityTimeline
sub_140D24590(this + 224, ...)           // MoveTimeline
sub_140D24380(this + 288, 0.0)           // FXTimeline
sub_140D24B50(this + 328, 0.0)           // SoundTimeline
sub_140D4A260(this)                      // 收尾
```

`FCommandAction` 的成员（`CommandAction.h`，`[E-SDK]`）正是 `FStateTimeline / FAbilityTimeline / FMoveTimeline / FFXTimeline / FSoundTimeline` 五个，加 `CachedStateComponent / CachedMovementComponent`。采样函数 `0xD24FB0` 依次采样这五条（P7 分支 kb `engine-facts` 已记）。

`StateTimeline`（`0xD24CA0`）`[E-IDA]`：一个按时间排序的表，每条 = `{弱指针 状态类, 开始时间, 结束时间, 已进/出标志}`；进入时调 `0xcedd20(actor, stateClass)`，离开时调 `0xccf6c0(actor, stateClass)` —— 即 `UStateComponent::AddState / RemoveState` 的转发。**状态时间线只加/删状态类，不直接播动画。**

## 3. 动画呈现单件：`USpineAnimationComponent` `[E-SDK]`

`SpineAnimationComponent.h`：每角色一个组件，持有 `AnimComponent`（`USpineSkeletonAnimationComponent`）、`StateComponent`、`LocomotionComponent`、`MovementComponent`、`InputComponent`，以及：

- 数据：`TransitionDefinitions`（动画 A → B 的过渡表，按 A 已播时长 `MinFromAnimPlayedTime` 决定）、`DamageGround/Air/SwimDefinitions`（受击按地形选素材）、`LandKnockbackDefinitions`、`WakeUpDefinitions`、`DeathDefinitions / DeathBackSideDefinitions`、转向（慢/快/飞/泳/潜）。
- 行为：`PlayAnimation / PlayActionAnimation / PlayAnimationDefinition / PlayAnimations / PlayDamage / PlayAdditiveDamage / PlayDeath / PlayDeathBackSide / PlayKnockbackLanding / PlayWakeUp / PlayLaunchKnockback / StopAnimation(layer) / StopAllAnimations`。
- 层：`ESpineAnimationLayer = NONE, LOCOMOTION, LOCOMOTION_TRANSITION, ACTION, KNOCKBACK_FALL, REACTION, DEATH, EVENT_LOCOMOTION, EVENT_KEEP, EVENT`（`[E-SDK]`）。
- 开关：`bUpdateAnimationsInTick`、`bCheckTurnTransitionsInTick`、`bSyncLocomotionTimings`、`bApplyRootMotionLocomotion`、`bAutoStopEventAnimations`。

**含义**：动画的正确性来自“组件输入一致”，不来自“动画名一致”。只复制名字 → 层、过渡、相位、受击素材选择全部丢失，只能靠 `hold_host_pose` 这类补丁兜。

## 4. 状态是类：`UStateComponent` `[E-SDK]`

`StateComponent.h`：`TArray<TSubclassOf<UState>> ActiveStates`、`FTimedStateData TimeStates`、`OnStateAdded / OnStateRemoved` 广播；API `AddState / AddStates / AddStatesForDuration / RemoveState / RemoveStates / RemoveAllStates / HasState / HasAnyState / HasAllStates / CanUseCommandInput`。

`UState` 是空基类（`State.h`），子类在 C++：`StateBusy / StateDead / StateDash / StateDashLv1..3 / StateDodge / StateGuard / StateHidden / StateHitStop / StateAutoClimb / StateDeactivateContactHitboxes / StateAbilityFilter_* …`。

**含义**：状态是“能力标签”，不是动画；`StateDead / StateBusy` 同时被 `CanUseCommandInput` 与动画层读取。命令只是产生状态的一条路径，所以“非命令状态”（坐椅 / 梯子 / 祈祷 / 被击）在同一套机制里，只要运行状态组件就有。

## 5. 结算汇点（减法要减的东西）

已在 `data/binary/EL.json` 有预检与 detour 的（`[E-IDA]`，来自 P7-1 与 v1/v2）：

| 汇点 | RVA | 作用 |
|---|---|---|
| `UAbilityEffectComponent::OnApplyEffect` thunk | `0xD3BCB0` | 命中效果唯一施加点（伤害 / 击退 / 硬直 / 治疗 / 回 MP / 体力） |
| `UAbilityEffectComponent::OnFinalizeEffect` thunk | `0xD3BD40` | 效果收尾 |
| `UDeathComponent::StartDeathProcess` | `0xCBD9A0` | 死亡唯一起点（掉落 / 出生点清除 / 销毁都在其后） |
| `UHPComponent::DoDamage` | `0xCA0380` | 接触伤害 / 陷阱 |
| `UAE_DamageComponent::ApplyDamage` | `0xCB07E0` | 技能伤害写 HP |
| `USaveSubsystem::SaveGame / SaveGameAsync` | `0xD25410 / 0xD25550` | 存档 |
| `AZenithAIController::Activate` | `0xCCD8F0` | AI 决策唯一入口 |
| `AInteractable::OnInteractActionPressed` | `0xCE8BF0` | 交互 Pressed（世界副作用入口） |

`[E-IDA]` 这一层的共同点：**它们都在“表现已经生成之后”**——动画、位移、特效、音效已经由时间线产生，汇点只决定“这次命中是否改变权威状态”。所以减法边界天然是这几处，而不是按命令类 / 按效果类枚举。

## 6. 对 P7-1 的结构性复盘（为什么它手感对、覆盖差）

- **对的**：让 Client 跑原版执行层，Spine / 技能 / 位移 / 特效 / 音效一次到位 —— 因为它跑的就是 §2–§4 的推导链。
- **错的（结构性）**：
  1. 复制的单位是**命令**（`0xCB4F00` 启动沿），不是**输入**。于是“输入 → 命令”这一段在 Client 被 `TryAddCommands` 抑制掉了，命令变成外部注入；凡是不经命令的状态（坐椅 / 休息、死亡姿态）就缺失，只能事后 `hold_host_pose` 打补丁。
  2. 精灵命令被跳过（`spiritSkipped`），靠副本自己的召唤命令派生 —— 这是对的，但说明“命令级复制”总要处理派生关系。
  3. 结算靠“放行 4 个表现类 AE、其余拦”的**类白名单**，未知类 fail closed → 覆盖率下降。而 §5 的汇点是结构性的，按汇点拦截不需要认识每个效果类。

## 7. 待验证（下一步要证或证伪的）

1. **输入复制是否足够**：把 Host 的 `FInputSnapshot`（40 B，`+224`）逐帧复制给 Client 副本，让 `TryAddCommands` 正常跑，是否能在 Client 复现同一命令序列？差异点在哪（`CanUseCommandInput` 依赖的状态、CD / 使用次数、方向输入阈值）。
2. **状态快照是否必要**：副本的 `UStateComponent` 是否会在长连段 / 受击 / 死亡时与 Host 分叉；分叉时用状态类集合做校正（而不是动画名）是否够。
3. **交互的减法**：坐椅 / 祈祷 / 梯子由 `AInteractable::OnInteract` 进入，Client 跑它会产生世界副作用（存档 / 清除 / 治疗）。需要把“状态与动画”留在 Client、“权威副作用”减掉 —— 这是减法边界最大的一块，需要单独 IDA + CUE 调研 `BP_Interactable_RestingPoint` 的 ubergraph。
4. **非角色对象**：特效 / 弹体 / 掉落由技能时间线生成，跑执行层后是否还需要 P6 的 Fx/Snd 通道（预期：只有“蓝图直接生成、不走技能”的那些还需要）。
5. **性能**：Client 同时跑 N 个远程角色的完整 tick 的 CPU 代价，与 P6 的快照呈现比（P7-1 实测 Client P99 344 µs，但那是命令级复制）。

## 8. FX 生成漏斗与实测缺口（2026-10-29，P6 构建 + Host2/Client2）

### 8.1 漏斗：[`UGameplayStatics::SpawnEmitterAttached/AtLocation`] 才是汇点 `[E-IDA]`

`xrefs_to` 的结果（`0x1425463A0` / `0x142546070`，基址 `0x140000000`）：

| 调用者 | 性质 |
|---|---|
| `0x7D0600` | `UBaseFXComponent::SpawnFX_Implementation`（P6 钩的这一个） |
| `0x7D0E60` | Spine 插件里另一个粒子生成函数（146 行，仅 vtable 引用 → 虚函数） |
| `0x7D1380` | 同上的第三个（137 行，仅 vtable 引用） |
| `0x142320530` | `UAnimNotify_PlayParticleEffect::SpawnParticleSystem`（引擎动画通知） |
| `execSpawnEmitterAttached` / 内部转发 | 蓝图直接调用 |

插件头里有 `SpineAnimNotify_PlayParticleEffect` 与 `SpineAnimNotifyState_TimedParticleEffect`（都声明 `UParticleSystemComponent*` 与 `CustomizeParticleEffect`），与 `0x7D0E60 / 0x7D1380` 只被 vtable 引用这一点吻合：**动画通知粒子是两个独立的 spawn 路径，不经过 `SpawnFX`**。

结论：FX 的“大块头”不是 `SpawnFX`，是 `UGameplayStatics::SpawnEmitterAttached / SpawnEmitterAtLocation`。钩这两个函数 = 覆盖全部 FX（含动画通知、蓝图直调、技能时间线）。

### 8.2 实测：P6 的 FX 捕获确实漏（Host2 / Client2，church_07，P6 main 构建）

计数器取自 `elmp lan status` 的 `fx:` 行（`host captured spawn/deact`、`client replayed spawn`）。

| 操作 | host captured spawn | host captured deact | client replayed spawn | 结论 |
|---|---|---|---|---|
| 注入 `CA_e2010_Slime_Attack` 到最近史莱姆（`AddNewImmediateCommand`） | 1 → 2（+1） | 19 → 20（+1） | 0 → 2 | 敌人攻击的 `SpawnFX` 类特效能被捕获并重放 |
| 注入 `CACS_s5060_Combo_LV3` 到 P1（`AddNewImmediateCommand`） | 1 → 1（**+0**） | 16 → 19（**+3**） | 0 → 1（旧事件） | 召唤连段的 FX **绕过了 `SpawnFX`**：三个粒子组件被创建又销毁，但捕获计数不动 |

第二行是“莉莉技能特效不全”的直接证据：特效确实产生了（deact +3），但 P6 的钩子看不见它们。

### 8.3 P7-2 的 emitter 钩子为什么仍然不全 `[E-源码]`

`explore/p7-failed-snapshot-coverage:native/coop/fx.cpp` 已经钩了 `SpawnEmitter*`，但：

1. owner 由 `actor_owner(worldContext)` 沿 outer 链推 —— 蓝图 / 动画通知传入的是 **World** 或非 Actor 组件，推不出 Actor → 直接丢；正确的 owner 应取 `SpawnEmitterAttached` 的 **AttachToComponent 的 Owner**。
2. 注释写明“Ability-owned ones are skipped (the client's replayed ability spawns its own)” —— 但 Client 重放的 ability 只保证 `LaunchAbilityFromDescription` 这一层，能力内部的 FX 未必自己生成；这句假设就是漏点的来源。

### 8.4 下一刀（FX 专项）

- 钩 `SpawnEmitterAttached`（`0x1425463A0`）与 `SpawnEmitterAtLocation`（`0x142546070`）：前者 owner 取 AttachToComponent 的 Owner；后者无 owner，按**世界坐标**发（客户端在对应地图的同一世界坐标生成，不跟随），并在消息里区分“附着 / 世界”两种。
- `SpawnFX` 与这两者存在调用嵌套（`SpawnFX` → `SpawnEmitter*`），用重入标志去重，否则一条特效发两遍。
- **持续性特效（自身发光）** 仍是状态不是事件：需要在 takeover / 新客人加入时同步 `USpineFXComponent::RunningFXs`（粒子资产 + 骨骼 + 变换），否则只在 spawn 那一刻有事件、错过就永远没有。

## 9. 判决实验：命令启动沿复制 → 客户端自己生成 FX（2026-10-29，Host2 / Client2，church_07）

### 9.1 实验设计

判据（§8 的删除测试）：**关掉 FX 复制通道后，客户端的特效是否还在**。

- 新增 transient 开关 `elmp set fxLocal 1`：客户端不重放 Host 的 `Fx` / `FxStop`、也不隐藏本地生成的粒子（`g_block=false`）。
- 新增 `coop/commands`：Host 在 `UCommandQueueComponent::ProcessCommand`（`0xCB4F00`）的启动沿（`UCommand.CurrentResult == NONE`）捕获 CommandAction 类 / `CommandInputType` / 方向，经 `Msg::Cmd` 发给拥有该敌人的对端；Client 用原版 `AddNewImmediateCommand` 重放。协议的 `Cmd` 走可靠通道，按 `(kind, net_id)` 去重。
- 客户端敌人的 `CommandQueueComponent` 不再进入冻结集（`freeze` 白名单）。

### 9.2 结果 `[E-实测]`

| 项 | 值 |
|---|---|
| Host `cmds captured` / `sent` | 1 / 1 |
| Client `cmds replayed` / `rejected` | 1 / 0 |
| Client FX 重放（通道） | 0（`fxLocal=1`，通道被绕过） |
| Client 本地生成（`localKept fx`） | **1** |

也就是说：Host 的一条史莱姆攻击命令 → Client 用原版 `AddNewImmediateCommand` 跑同一条 `CA_e2010_Slime_Attack` → CommandAction 的 FX 时间线在客户端自己发出粒子。**没有复制任何 FX。**

反过来，同一场景在 `fxLocal=0`（P6 行为：抑制本地 + 重放通道）下是 `client replayed spawn=1`——表现是“复制来的”。两者都能看见特效，但只有前者是在跑管线。

### 9.3 结论

1. **FX 不需要同步通道**：只要客户端跑同一条命令，特效由模拟自己产生。§8 里“补 spawn 路径 / 补 owner 身份 / 补持续状态”的工作全部不再需要。
2. **真正的缺口是“客户端没跑管线”**：冻结 + 快照写轨道名，让动画、通知、粒子链断在了上游。
3. 后续每个表现域（远程莉莉、精灵、音效）都用同一判据验证：关掉复制通道后还在，才算接上管线。
4. 已知未覆盖：客户端敌人的**动画**仍由快照写轨道 0（`SpineAnimationComponent` 状态机仍在冻结集里），所以表现为“特效对了、动作还不是状态机产生的”。下一步是把状态机接上，再用同一判据复测。
