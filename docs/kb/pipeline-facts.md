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
