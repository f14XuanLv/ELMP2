# ADR 0006 — Client 呈现改为“复制决策 + 跑原版执行层 + 收束结算”

- 状态：**已采纳，待实施**（2026-10-28；调研与单实例 spike 完成，实施见 `GOAL.md` §5c P7）
- 取代：需求树 §7.17-5/6 中“重放体寿命上限”“接管即逐组件关 tick”的做法，以及 §7.20（P6）的特效 / 音效 / 技能结束 / 精灵副本复制通道
- 保留：Host 权威铁律（X-2）、guard 的存档保护 / AI 激活闸 / 伤害拦截、快照与插值、P6 的 `Ref` 与乱序重试、受击表现入口

## 背景

P2 实现 Client 呈现时选择了“接管即冻结”：敌人与莉莉副本按白名单逐组件关 tick，只留渲染相关组件，再从外面写位置、HP、动画名。冻结之后原版执行层整个停摆：

- `UCommandAction` 每帧采样 State / Ability / Move / FX / Sound 五条时间线（`0xD24FB0`），Client 上被冻结的对象一条都不跑；
- 于是 P6 只能在执行层的下游逐项捕获产物再仿造：钩 `LaunchAbilityFromDescription`、`SpawnFX`、FMOD 播放、`AAbility::Stop`，另外生成精灵副本、调受击处理函数。

这是加法路线：要呈现的东西是开放集合（Spine 动画通知、蓝图直生特效、附着 / 循环音效、材质闪白、震屏……），每补一种就多一条通道、一份 Host 捕获、一份 Client 重放与去重，而且永远补不全（P6 已知限制就列着附着音效、蓝图直生特效）。要禁止的东西却是封闭小集合（伤害、死亡、清除、奖励 / 掉落、存档、交互）。方法论上应当反过来：**找原版集中的决策入口复制决策，让 Client 跑完整的原版执行层，只在结算汇点做减法**（X-1 原版管线优先，v1 ADR 0002“先定位原版管线，不各写一份模拟”）。

## 调研结论（2026-10-28，证据见 engine-facts「CommandQueue 决策管线」、net-facts「P7 调研 spike」）

1. **所有角色的动作都汇入同一条原版管线** `[E-IDA][E-CUE]`：
   - 莉莉：输入 → `ZenithInputComponent` → `UCommandQueueComponent::TryAddCommands*` → 命令队列；
   - 敌人：行为树任务 `BTTask_ExecuteActionBase` → `UExecuteActionAsync::ExecuteActionAsync`（实现 `0xD08AF0`）→ `AddNewImmediateCommand(Dir=0, Cmd=1, Class)`（实现 `0xC97C60`）→ `AddNewCommand` 核心 `0xC97660` → `UpdateCommands`（`0xCC3970`）；史莱姆的攻击就是 `CA_e2010_Slime_Attack_C` 等 CommandAction 类；
   - 精灵：召唤者的 `CommandActionSummon` 启动后由精灵自己的 CommandQueue 派生执行。
   - 队列处理 `0xCC2BA0` → 单命令处理 `0xCB4F00`：首次处理即“启动沿”（日志 `START command :`、广播 `OnStartCommand`、取消同 ID 命令），之后每帧 `0xD24D70` 采样，`CommandAction::Start` = `0xD29F90`（State / Ability / Move / FX / Sound 时间线从 0 开始）。
2. **把决策注入冻结前的对象，原版执行层就完整重现** `[E-实测]`（Host2 单实例，无会话）：
   - 休眠的史莱姆（AI 未运行、全部组件 tick 关）注入 `AddNewImmediateCommand(0, 1, CA_e2010_Slime_Attack_C)`：只设了起始动画 `attack_A`，之后停住——CommandQueue 不 tick；
   - 打开它的组件 tick（AI 仍不运行）再注入：`attack_A → attack_A_fall_loop → attack_A_landing → idle`，技能 Actor 出现 1 个后**自然结束**，Move 时间线带出完整跳跃弧线（Y 3956→4578、Z −478→−1378 落地），命令队列自行清空；
   - 第二只莉莉（slot 101）注入 P1 刚用过的 `CACS_s5060_Combo_LV3_C`（Cmd=3）：莉莉播 `summon_DEFAULT_C`，精灵 `BP_s5060_Assassin` 自动出现并派生执行 `CA_s5060_Ground_01_C`，结束后精灵自行消失——精灵、精灵技能都不需要单独复制。
3. **莉莉的移动动画由输入驱动，不由速度驱动** `[E-实测]`：只写位置 + `CharacterMovement.Velocity`，莉莉一直 `idle`；敌人的状态机对速度有反应（`idle_turn → walk`）。所以莉莉副本需要原版输入，敌人移动动画可以靠速度。
4. **原版的结算按类型集中** `[E-SDK]`（重建头；分发细节待 P7-1 用 IDA 核实）：技能命中效果是技能上的 `UAbilityEffectComponent` 子类（`OnApplyEffect` / `OnFinalizeEffect` 为 BlueprintNativeEvent），由命中条目效果队列 `AAbility::AdvanceEffectQueue`（`0xCBB110`，v1 已定位）推进：结算类 = `AE_Damage` / `AE_DamageAbsolute` / `AE_HealAbsolute` / `AE_Knockback` / `AE_RestoreMPToInvoker` / `AE_StaminaDamageAbsolute` / `AE_StunDamageAbsolute`；表现类 = `AE_HitStop` / `AE_CameraShake` / `AE_ShakeTarget` / `AE_ForceFeedback`。命令输入类型也可分：`JUMP / DODGE / ATTACK / SUMMON_1 / SUMMON_2 / SUMMON_SET_SWITCH / HOOK / HEAL` 是战斗与移动，`INTERACT / MAINMENU / PAUSE / MINIMAP` 会碰世界或界面。

## 决定

三层切分，Client 上每个网络对象都按这三层处理：

| 层 | 归属 | Client 上怎么做 |
|---|---|---|
| **决策层**：选哪个动作、按了什么键 | Host 权威 | 敌人：AI 激活闸保持（G1）；Host 在命令启动沿捕获 `{Ref, seq, t_ms, CommandAction 类, Dir, Cmd}`，Client 对呈现对象调同一原版入口 `AddNewImmediateCommand` 重放。莉莉副本：Host 回带该座位的原始输入（轴 / 按住位 / 累计计数）经虚拟手柄喂入，副本本地“输入 → 命令”的生成被抑制，命令同样由 Host 启动沿下发重放（决策来自 Host，按住时长等实时输入语义来自回带输入）。精灵：不复制，由召唤命令派生。 |
| **执行层**：CommandAction 五条时间线、Spine 状态机、技能 / 弹体、特效、音效、精灵、移动 | Client 原版 | 接管时**不再逐组件关 tick**；只停决策源（AI 控制器 / 行为树 / 感知 / 激活体积，已有闸）。特效、音效、技能结束、附着音效、蓝图特效全部由原版自己产生。 |
| **结算层**：伤害、死亡、清除、奖励 / 掉落、存档、交互、休息 | 只在 Host | 结算边界做减法：存档保护、`DoDamage` / 技能伤害拦截（已有）；新增**命中效果按类拦截**（结算类 AE 不施加、表现类照常）；`INTERACT / MAINMENU / PAUSE / MINIMAP` 类命令不重放、副本不生成；死亡只由快照呈现（不跑原版死亡管线）；Client 侧技能生成的敌人一律交 `enemy_dyn` 收养或销毁。世界结果仍只经快照 / 世界对象适配器到达。 |

快照的角色从“生成画面”降为“纠正状态”：位置、HP / MP、存活、地图、隐藏照旧下发；动画名只用于漂移检测与兜底校正。

细则（实施中按实测可复议，结论写 kb）：

1. **时间轴**：命令消息带 Host `t_ms`。敌人：在插值渲染时刻（Host 时间 − 100 ms）到点执行，与位置同一时间轴；莉莉副本：到达即执行（副本跑实时输入，位置按阈值纠正：>240 UU 直跳、40–240 每帧收拢 15%，v1 实测值，需复测）。
2. **位置归属**：敌人在没有进行中的重放命令时由快照插值决定位置，并把插值速度写入 `CharacterMovement.Velocity` 供原版状态机选 idle / walk / turn；有进行中的命令时由原版 Move 时间线移动，快照偏差超阈值才校正。
3. **碰撞**：敌人保留原版碰撞（世界碰撞让 Move 时间线能落地）；命中 Client 本地对象产生的一切结算由第 4 条兜住。
4. **命中效果**：Client 上所有技能（重放的、本地 P1 自己的）对任何目标的结算类 AE 一律不施加并计数；表现类 AE（顿帧、震屏、目标抖动）照常。本地 P1 被击退是否作为预测放行，实测后定（默认不放行，靠 >150 UU 回拉）。
5. **受击动画**：原版受击处理挂在伤害广播上（已被拦截），保留 P6 的做法——Host HP 下降时调呈现对象的蓝图 `OnReceiveDamage` 处理函数。
6. **校验与边界**：命令类必须 `IsChildOf(/Script/Zenith.CommandAction)`；Cmd 类型白名单（战斗 / 移动 8 类）；所属对象必须是 guard 标记的呈现对象；每对象 seq 单调幂等；队列有界；所属对象未呈现时沿用 P6 的 ≤300 ms 重试。
7. **组件 tick 与 guard**：guard 的 tick / 碰撞原语闸从“标记对象一律拒绝重新开启”改为“只拒绝决策源对象（AI 控制器）”；1 Hz 复检改查 AI 泄漏与决策源状态。
8. **删除**：P6 的 `Fx / FxStop / Snd / ActStop / Act` 通道、本地特效抑制、精灵呈现副本与 `sp` 段、技能寿命上限，在命令通道实测对齐后删除；协议版本升到 4。

## 后果

- 正面：特效 / 音效 / 技能结束 / 精灵 / 附着音效 / 蓝图特效不再逐项补；Client 与 Host 跑同一份原版逻辑，新内容（新敌人、新精灵）零适配；网络流量从“每个产物一条可靠消息”降为“每个决策一条”。
- 代价：Client 上被放开的对象按原版成本 tick（与 Host 同量级）；原版执行里若有随机或依赖本地物理的分支，Client 会与 Host 有小偏差——由快照纠正位置 / 状态，动画漂移靠快照动画名兜底。
- 风险（实施门槛逐项实测）：
  1. 结算汇点枚举不全 → P7-1 先用 IDA 把放开执行层后可达的结算点列成表（命中效果、死亡、掉落、清除、交互、剧情旗标、存档），每项有拦截与计数，Client 存档 / 世界状态泄漏为 0 才进入后续阶段；
  2. 连段（`CommandActionCombo`）的段号在 Client 与 Host 可能不同步 → 实测，必要时命令消息带段号；
  3. Client 本地重放的技能生成敌人 / 掉落物 → `enemy_dyn` 收养或销毁，掉落不生成（死亡不跑原版）；
  4. 本地 P1 预测与副本命中的交互 → 结算边界兜底，表现可接受度实测。

## 不采用的方案

- **继续 P6 的产物复制**：开放集合补不全，每种产物一条通道、一份去重，越补越碎。
- **只回带输入、副本自行决定命令（v1 输入层副本）**：副本本地状态（冷却、MP、是否着地、连段计时）与 Host 不同就会做出不同动作；v1 实际效果不好（原因未定位，不照抄）。本 ADR 只借用“副本跑原版管线、输入驱动移动”这一点，决策仍来自 Host。
- **敌人也回带输入**：敌人的决策源是行为树，不是输入；行为树的唯一出口就是命令队列，复制命令即可。
