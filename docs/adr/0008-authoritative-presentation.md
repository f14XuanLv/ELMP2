# ADR 0008：按证据划分远端呈现与权威逻辑

- 状态：采纳，2026-11-04；实现和验收状态另记 kb。
- 2026-11-05修订：已核实的本机击退、Ability及其私有FX执行链按[ADR 0009](0009-restore-native-lily-pipelines.md)恢复原版上下文，收窄本文冻结/结果复制范围；尚未迁移的域保留临时实现和未验收标记，不能据本文宣布完整同步。
- 取代 ADR 0007 中“输入/命令足以重建全链、表现复制必须归零”的无条件约束。Host 权威与逐域验证原则保持。
- 证据：[玩家图谱](../kb/player-sync-graph-v2.md)、[敌人图谱](../kb/enemy-sync-graph-v2.md)。

## 原因

原版有输入以外的装备/附身、AI 直接 Ability/动画/状态、受击、交互、出生点和死亡入口。现有 guard 未封闭这些入口的结算副作用。停 Movement tick 也不阻止 Spine 更新委托应用根运动。现有 simLocal/animLocal/fxLocal 组合既没有完整恢复上游，也产生两个表现写者；不能作为正式架构。

## 决定

1. 远程 Lily、普通/动态敌人、Spirit 共用明确的 Host 结果呈现策略；本机受控 Lily 保留既有输入预测策略。删除全局模拟/动画/FX 试验开关与 Cmd 注入路径。保留 Host Act/Fx/Snd 结果通道，不能按敌人大类丢弃直接 Ability。
2. 呈现对象关闭 Actor/Movement/SpineAnimation 决策 tick。仅打开已登记的骨骼/renderer/附件等呈现组件；Color 插值不能无条件启用。反射校验 RootMotionFactor 并设为零，失败对象不进入活动呈现。权限闸与呈现策略分别负责权威与输出。补入敌人图谱已核 ABI/前缀的 Effect Apply、Effect Finalize、DeathStart 三个原生窄闸：Client armed 时无条件拒绝并分别计数，不继承 damage replay 的许可；未登记旁路仍不视为封闭。guard 身份以 ObjHandle 保存，删除实验 tick/activate 许可表。
3. 用统一呈现适配器采集实际 Spine 轨道、播放实例代次、名字、时间、loop 与可核实的混合参数；逻辑 AnimationDataLayers 不等于 Spine track。接收方只有这个适配器写动画，删除 HP 差值合成受击、仅名字变化才重播的分支。有限轨道容量必须显式审计溢出，不将其宣称为完整原版状态恢复。
4. Actor 世界位置、Actor 世界旋转、VisualPivot 相对位置、renderer 相对位置/颜色是不同输出，骨骼 facing 不能代替 Actor 旋转。只复制已成功读取的字段，有效位区分读失败与真实零。Companion旋转按第10条纳入cp基线。删除周期 normalize/ClearColor；初始/运行中 skin 变化先恢复相应数据，再恢复 Host 姿态。根运动不二次改变 Host 坐标。
5. 一个 Host 持续 FX 实例终身一个 ID，晚加入复用该 ID；所有接收方按该 ID 停止。创建、重试、同帧停止、换图、断线均清理生命周期。声音保留单一来源，不因实验开关同时本地产生和网络重放。敌人 BattleColor 调已核组件入口，不以派生蓝图 OnChangeTarget 作为纯表现接口。
6. 协议字段变化提高版本，拒绝旧客户端；纯编解码增加边界验证，表现行为以真实隔离实例验证。所有句柄缓存标注失效源，运行时暴露采样/应用/失败/溢出等计数。
7. 跟随同伴是 `ZenithCompanionCharacter`，不是 `ZenithSpirit`。按玩家图谱 §11 单独登记 `Companion` 身份与 `cp` 快照段，依据 Companion.Controller（AIC）→ GetLeaderPawn() → Pawn.Controller（玩家 PC）的运行时链过滤；Actor.Owner 在附身后通常也是 AIC，仅在直接指向玩家 PC 时用于过渡互证，不能当作玩家归属的提前拒绝条件，普通 Spine 与 Ex 共用呈现适配器；不运行其 AI/Leader 逻辑。既有同伴、隐藏、装备变化、停止与晚加入由行基线决定，粒子所有者也能解析到该身份。不能用技能 Spirit 的消失推断跟随同伴消失。已核的 `Companion.FX` 光团 PSC 由 cp 中的 active/visible/hidden 状态唯一驱动，保留客户端 BeginPlay 生成的对应组件；Host FX 捕获按该确切 PSC 排除，不能按资源名全局排除。其它冷却提示等单次粒子仍走事件通道。
8. Enemy_Base 的死亡溶解采集 renderer 动态材质中已存在的 `DissolveValue` 显式值，按有效位复制给同一 renderer 原生 setter。仅在各已存在材质输出可一致表示时发送；缺失/分歧不当成零。该纯输出路径不调用 DeathStart/NotifyCompleted，也不修改出生点和掉落。
9. Spine 暂停也是姿态输出：`Pose.flags` bit2 表示暂停，不新增包字段。Host 读取骨骼组件 `IsComponentTickEnabled`、`bAutoPlaying` 和全局 `GetTimeScale`，任一播放开关关闭或全局倍率为零即暂停；全局倍率乘入各实际轨道的 `time_scale`。Client 保持呈现骨骼 tick 与 AutoPlay，以组件 `SetTimeScale(0/1)` 分别暂停/推进；初次应用或切皮肤失效后先恢复倍率 1、清理旧轨道，再创建轨道并应用 Host 暂停。每次收到姿态都比较本地实际 TrackTime，偏差大于 0.05 s 即校正，不能以 Host 上两次时间相同为由跳过。依据为插件 `InternalTick` 的 update/apply 分离与 `AnimationState::update` 对全局倍率的处理；这不证明动画事件/委托已静默。引擎 `Actor.h` 的 `ExecuteTickHelper` 另将 Owner.CustomTimeDilation 乘入组件时间，本轮不改变该 Owner 属性、不声称覆盖此额外时钟；非 1 倍率与完整 mixingFrom 链仍须后续实测。
10. Companion Actor 的世界旋转是独立于骨骼 facing 与 renderer 相对变换的基线。协议6在 cp 增加 pitch/yaw/roll 三个有限 float，Host 读 `K2_GetActorRotation`、Client 以 `K2_SetActorRotation` 恢复，不能硬编码 yaw90。协议5实测副本 yaw0、Host yaw90 导致平面侧对镜头，说明 copies/applyFail 计数不能替代截图验收。48行增加576字节后必须重新验证128KiB最坏包边界。

持续 FX 投递记录同时绑定 Ref 身份；身份/地图失效先结束旧呈现，恢复后沿用 Host activation ID。每秒重申仍在场的持续基线，Client 按 ID + owner 句柄去重；同 ID 的本地组件已失效或 owner 被重建时可恢复，不能把曾收到一次当成永久存在。

敌人HP归零不等于表现对象已消失：仍在场且可见的死亡阶段允许接收Host的FX/Snd，技能Act继续要求存活的owner。不能共用一个“HP0即拒绝全部结果”的解析条件丢掉死亡粒子/声音。死亡位每快照按Host当前行更新，不能把一次死亡永久粘在出生点名上。

可靠生命周期消息（包括最终 FxStop）不能因过载丢弃后继续保留会话。端点可靠命令队列、Link 可靠待发队列或可靠/控制入站队列无法接收时，端点锁存不可覆盖的故障，关闭全部 peer；游戏线程优先且仅一次收到 `peer=0` 的端点级 Closed，Host 与 Client 都结束角色并执行现有完整清理。此故障通知独立于已满数据队列，握手期间也生效；普通 peer Closed/Rejected 同样优先且清除其旧入站事件。传输层满可靠事件队列也断开，关闭报文优先于尚未发出的数据；UDP 关闭报文仍可能丢包，对端以现有超时机制收敛。非可靠快照仍允许按现有 latest-only 策略丢弃。过载后不会自动恢复端点，需要重新建会话；该选择明确牺牲会话可用性以保持生命周期 fail closed。

## 后续语义迁移门槛

### 附着粒子坐标与池隔离补充（2026-11-04）

`SpawnEmitterAttached` 的 AttachToComponent、AttachPointName、EAttachLocation、AutoDestroy/AutoActivate 不得丢失后猜成 Actor root。Fx 增加独立的附件类型、组件相对身份、socket 与 emitter 标志字段；BaseFX 原有 custom bone 保留原义。Host 在原版生成后读取返回 PSC 的实际相对变换，把 KeepWorldPosition / SnapToTarget 等换算结果规范化为相对于原组件/socket 的 KeepRelativeOffset，再由 Client 对同一语义挂点重放；组件找不到、骨骼映射不明或身份存在歧义时拒绝并计数，不退回 root。骨骼枚举挂点必须以原版 SpineBoneComponent 的映射与入口验证，不能按生成对象名后缀猜测。

远端两种粒子重放入口一律使用本地 `PoolMethod=None`，不复制 Host 内部池策略，避免本机 P1 借走 AutoRelease PSC 后旧网络 Stop 停掉新的本地 activation。网络自行生成的 PSC 用 ObjHandle 所有权表登记，上限1024；满额拒绝新重放，不驱逐活体。Client 生命周期结束先 Deactivate，再进入有界 retiring 集；全局 frame phase 每100ms在 `!bIsActive && GetNumActiveParticles()==0` 时，以实际 GetOwner() 调 K2_DestroyComponent，等待原版 Complete 后回收，不按超时切断尾迹。该 phase 在会话停止后仍可执行，World/组件销毁通过弱句柄失效清理。PSC.bAutoDestroy 不是反射属性，不猜内存位；原版抑制 hide_now 仍只 Deactivate，不销毁 Companion.FX 或其他原版返回对象。socket/component 字符串遵守 NameDef 长度上限；协议解码分别验证普通 FX 的四元数与 emitter 的 Euler/相对变换，不对 Euler 错用单位四元数检查。事件协议变更提高版本；这不改变快照段容量。

所有 SpawnEmitterAttached 返回后仍 active 的实例都有持续在场基线资格，结束于 Deactivate、inactive 或句柄失效；AutoDestroy 不是 loop 的证明。会话前已有记录保存在有上限且计溢出的 early 缓存，晚加入和每秒补发沿用同一 activation ID。AutoActivate=false 后的首次激活、同一 PSC 在 Deactivate 后被重新激活而未再 Spawn 的通用路径尚未捕获，粒子内部时间相位也不保证一致；Companion.FX 已由专用 cp 当前状态处理。

完整 PC/装备/状态/已接受动作/Spirit owner 与 attachment/死亡纯表现仍需闭合图谱。逐个入口证明基线、晚加入、结束与结算边界后，才把对应输出移交原版生成并删除重复通道。走跑通过、计数器增加或一张截图都不代表全游戏同步完成。图谱保留未知边，验收报告逐项记录已测、失败、未测。
