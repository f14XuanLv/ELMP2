> 2026-11-04 更正：本文保留历史观察；统一输入充分性、现有 guard 覆盖、renderer 偏移来源等推断已重新核验。当前事实以 [玩家图谱](player-sync-graph-v2.md)、[敌人图谱](enemy-sync-graph-v2.md) 和 [ADR 0008](../adr/0008-authoritative-presentation.md) 为准。

# 敌人红光为什么在客户端消失：从 AI 到像素的完整图谱（2026-10-30，P8）

> 这篇的原因：之前每次只补一点（补 spawn 钩子、补 owner 映射、补持续状态），都没有命中。这一次把
> “敌人身上的红光”从上游到下游拆开，先证伪了三条错误假设，再按图谱下手。
> 证据等级沿用 `AGENTS.md`：`[E-IDA]` > `[E-实测]` ≥ `[E-CUE]` > `[E-SDK]` > `[E-源码]`。

## 1. 结论（先给答案）

敌人的红光**不是粒子**，是 `UBattleColorComponent` 往 Spine 渲染器的材质上写的一个**材质参数**
（带 FadeIn / Loop / FadeOut 三条曲线）。它由 AI 选到目标时触发：

```
AIC_<enemy>（行为树选目标）
  └─ Pawn 自定义事件 OnChangeTarget(NewTarget)        [E-CUE]
       ├─ CurrentTarget = NewTarget
       ├─ BattleColorComponent->SetBattleColorEnabled(true)   ← 红光（材质参数，常驻循环）
       ├─ BP_FXComponent->TriggerFindEnemyFX()                ← PS_FindEnemy（一次性粒子）[E-CUE]
       ├─ BP_SoundComponent->TriggerSoundEvent(enemy-find-enemy)
       └─ （目标失效时）SetBattleColorEnabled(false) + ColorComponent->ClearColor(0)
```

客户端缺红光的真正原因：**客户端的敌人 AI 被权限闸拦掉了激活**（`guard` 的 `d_ai_activate`），
永远不会拿到目标，于是 `OnChangeTarget` 从不触发；而这条状态**不在任何 FX 通道里**——红光只是材质参数，
粒子钩子再怎么补也补不到它。`[E-实测]`

## 2. 图像证据（同一时刻的 Host / Client）

- Host：`e2021_Soldier` 全身偏红/紫、眼睛与法杖上有红色发光点。
- Client：同一只 `e2021_Soldier` 是**灰的**，没有红光。

这解释了为什么 `host captured spawn=0` 而场景里 45 个 `ParticleSystemComponent`：
红光根本不经过 `SpawnFX` / `SpawnEmitterAttached`。

## 3. 图谱（每一层都能查证）

### 3.1 敌人不在关卡里，是运行时 spawn 的 `[E-CUE]`

`Church_07_GAMEPLAY` 的 159 个导出里没有任何敌人 Actor，只有 `BP_EnemySpawnPoint_C` 的 25 个实例
（`e2010_Slime2`…`e2021_Soldier6`、`BP_EnemySpawnPoint_Unique`）。敌人类本身来自
`BP_EnemySpawnPoint` 的类配置，玩家靠近激活球时 spawn。

推论：敌人身上的东西**不能**靠“地图两边都加载了同一张图所以表现自然一致”来解释，一切都在运行时发生。

### 3.2 敌人基类的全部表现入口 `[E-CUE]`

`/Game/_Zenith/Core/Pawns/BP_Character_Enemy_Base`（父类 `BP_Character_SpineBase`，子类 `BP_e20xx_*`）暴露的事件：

| 事件 | 做了什么 |
|---|---|
| `ReceiveBeginPlay` | `PlayMeatAction()` / `EnableBattleColorsOnSpawn` → `BattleColor->SetBattleColorEnabled(true)` |
| `OnChangeTarget(NewTarget)` | ↑ 上面那条链：红光 + FindEnemy FX + 音效 |
| `BndEvt__DeathComponent_…DeathEvent` | `BattleColor->SetBattleColorEnabled(false)`、`StopMeatAction()`、死亡溶解 |
| `BndEvt__SpineActivationSphere_…Begin/EndOverlap` | `SetPaused(false/true)`（离开激活球就整只暂停） |
| `OnLanded` | 坠落伤害 |
| `OnRespawn` | 复活 |
| 死亡溶解 | `SpineSkeletonRenderer->SetScalarParameterValue("DissolveValue", …)` |

组件表里跟“视觉状态”有关的三个：`BattleColorComponent`（红光）、`ColorComponent`（受击/替换颜色）、
`BP_FXComponent`（粒子，`USpineFXComponent` 的蓝图子类）。

### 3.3 `UBattleColorComponent`：材质参数，不是粒子 `[E-SDK]`

```cpp
class UBattleColorComponent : public UActorComponent {
    UCurveFloat* LoopCurve; FadeInCurve; FadeOutCurve; FName MaterialParameterName;
    void SetBattleColorEnabled(bool bEnable);  // 蓝图里唯一入口
    void SetAlpha(float NewAlpha);
};
```

它做的是：在启用时按 FadeInCurve 把 `MaterialParameterName` 这个标量参数推到 1，然后按 LoopCurve 循环，
关闭时走 FadeOutCurve。所以：

- 它的表现**没有生命周期对象**可以“重放”，只有一个 bool + 时间曲线；
- 只要在客户端把同一个 bool 打开，原版组件自己会算出正确的颜色动画。

### 3.4 `BP_FXComponent::TriggerFindEnemyFX`：一次性粒子 `[E-CUE]`

```text
TriggerFindEnemyFX():
  if (!FindEnemyFX) return;
  SpawnFX(FindEnemyFX, /*Bone=*/None, /*CustomBone=*/"None", /*Offset=*/identity,
          /*bAttach=*/true, /*bFollowBoneRotation=*/false, /*SortPriority=*/GetTranslucencySortPriority(50),
          /*PoolMethod=*/1, /*bRegisterAsRunningFX=*/false);
```

`FindEnemyFX` 是组件上的可配置资产，默认指向 `PS_FindEnemy`（`/Game/_Zenith/Art/Effects/Battle/PS_FindEnemy`）。
`bAttach=true`、`reg=false` → 一次性附着粒子，走 `USpineFXComponent::SpawnFX` → `UGameplayStatics::SpawnEmitterAttached`
（就是我们两个粒子汇点之一），所以**这一半**本来就在 FX 通道的覆盖范围内。

顺带把同类入口也列清楚（`BP_FXComponent` 全部 SpawnFX 调用点）：

| 函数 | 资产 | 参数要点 |
|---|---|---|
| `TriggerFindEnemyFX` | `FindEnemyFX`（默认 `PS_FindEnemy`） | attach, 一次性 |
| `TriggerJustGuardFX` | `PS_JustGuard` | 不 attach, 一次性 |
| `TriggerLandingFX` | 按地表材质取 | bone=`0xA`（Feet）, 一次性 |
| `TriggerContactDamageFX` | `ContactDamageFX` | 不 attach, 一次性 |
| `PlayBreathFX` / `StopBreathFX` | `DefaultBreathFX` / `OverrideBreathFX` | bone=`0x48`, attach, **实例被保存并显式 Deactivate** |

### 3.5 另一条容易混淆的红光：攻击预告 `[E-CUE]`

`CA_e2021_AttackA` 的 `AbilityTimeline[].AbilityVisualCue` 里配了 `PS_Gloweye`（`bUseVisualCue`、`LeadTime=1.0`）。
这是**出手前的眼睛发光预告**，属于命令的 Ability 时间线，不在上面那条链上。它是命令的果，
客户端只要跑同一条命令就会自己产生（`cmds replayed` 那条通道），所以不是这次缺的东西。

### 3.6 为什么客户端缺 `[E-实测]`

1. `guard::d_ai_activate` 在客户端把**所有** AI 激活都拦掉（`aiActivateBlocked=24`）→ 敌人永远没有目标。
2. 因此 `OnChangeTarget` 从不触发 → 红光、FindEnemy FX、发现音效三样全缺。
3. `fxLocal=1` 时客户端不重放 Host 的 FX，所以 FindEnemy FX 也补不到（这是对的：它本来就该由本地产生）。
4. `EnemyRow.flags` 里没有“战斗中”这一位，快照也带不过去。

## 4. 修法：把“因”复制过去，不复制果 `[E-实测]`

按 ADR 0007（复制因不复制果）：

- **因** = “这只敌人此刻有目标”（决策归 Host）；
- **果** = 红光（材质参数）、`PS_FindEnemy`、音效。

实现：

| 侧 | 改动 |
|---|---|
| 协议/快照 | `EnemyRow.flags` 新增 **bit3 = engaged**（`net/snapshot.h`），随 delta 快照天然同步、新客加入自动补齐 |
| Host | `write_enemies` 读 `BP_Character_Enemy_Base.CurrentTarget`（`reflect::PropRef`，一次解析缓存）；有效则置 bit3。计数器 `events: … engaged=N` |
| Client | `present_enemy` 看到 bit3 变化 → 调**同一个原版事件** `OnChangeTarget(p1_pawn / nullptr)`；由原版蓝图跑出红光 + FindEnemy FX + 音效。计数器 `events(client): … engaged=M` |

为什么是调 `OnChangeTarget` 而不是自己分别调两个组件：原版这个事件就是“拿到目标”这件事的完整反应
（取反分支还会 `ClearColor(0)`），调它等于在客户端复现同一段代码，粒子/音效都由游戏自己的管线生成。

同时把 `BattleColorComponent` 加进客户端 `freeze()` 的 tick 放行名单（否则 FadeIn/Loop 曲线不推进）。

## 5. 附带修好的一件事：会话前的持续特效

`bRegisterAsRunningFX=true` 的常驻粒子（呼吸、某些光环）在**读图 / 敌人激活**时就 spawn 了，
而 FX 钩子原先只在 `elmp lan host` 时才装 —— 晚加入的客户端永远补不到。

现在 `coop::fx::install()` 在 `on_unreal_init` 就装（读图之前）；没有会话时只把 **reg=true** 的持续特效
记进 `g_early`（句柄化，最多 128 条），Host 一开始联机就用 `fx::take_early()` 灌进 host 的在场集合
（`g_persist`），新对端 Welcome 时以 `flags bit4`（初始状态）补发。

`elmp lan status` 的 `fx:` 段新增 `early persistent fx: N` 与 `early <资产名> xN`，用来审计“会话前到底有什么”。

## 6. 怎么验收

1. Host 侧：`elmp lan status` → `events: … engaged=N`（N 随敌人数增长，说明 bit3 真的在置位）。
2. Client 侧：`elmp lan status` → `events(client): … engaged=M`（M>0 说明原版事件真的被调到了）。
3. 肉眼（最终判据）：客户端走到 `church_07` 的 `e2021_Soldier` 面前，敌人身上应出现与 Host 一致的红色发光；
   敌人发现玩家时还有一次 `PS_FindEnemy` 的闪烁 + 音效。
4. 反向检查：把 `elmp set fxLocal 0` 后红光**依然在**——因为它不是粒子，不依赖 FX 通道（这是这次的关键）。

## 7. 还没覆盖的

- `ColorComponent`（受击闪白、`SetColor`）同样是材质状态，若发现客户端缺受击闪白，按同一套路复制“因”（HP 下降已经在走 `hit_react`）。
- `DeathComponent` 的 `DissolveValue` 溶解动画：目前靠快照的 `flags bit2` + 隐藏，还没有对齐溶解进度。
- `SetPaused`（离开激活球整只暂停）在客户端由 `freeze()` 静态决定，没有跟随 Host。
