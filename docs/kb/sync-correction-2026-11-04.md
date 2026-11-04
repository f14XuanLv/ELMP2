# ELMP v2 同步修正与实机记录

2026-11-04；修复前源码基线 `6de0be8`。本文件逐轮追加，未列为通过的场景不能视为完成。决定见 [ADR 0008](../adr/0008-authoritative-presentation.md)，原版链路见 [玩家图谱](player-sync-graph-v2.md)、[敌人图谱](enemy-sync-graph-v2.md)。

## 一、为什么 Host 与 Client 不对称

`[E-源码][E-IDA][E-CUE]` Host 上的网络座位仍是由输入驱动的完整原版 PC/Pawn，具备装备、命令、状态、召唤、动画层与通知上下文。修复前 Client 创建的是冻结副本，只复制少数输出：莉莉不应用 track-0 时间，同名重播和 loop 变化被忽略，额外轨道、VisualPivot 与运行中皮肤变化遗漏。实验 anim/sim/fx 开关又可能让同一输出有两个写者，或在上游仍冻结时丢弃结果事件。

这不证明“只复制输入即可恢复全部链路”。原版还存在装备装配、AI 直接 Ability/动画/状态、交互、死亡与独立同伴入口。已证明的输出可以同步；尚未隔离的结算不能为补表现而全面解冻。

## 二、两个进程但四个莉莉

`[E-实测]` 用户指出的问题发生在前一轮 Dev2/Client3 会话：Host 有本地 P2（xinput/kbd）、本地脚本 P3 和网络 P4，Client 因此有本人 + 3 个远端副本。进程数不等于座位数，这次不是已经证实的幽灵副本泄漏。

停止全部配置实例、还原本轮测试档后，用干净 Host2/Client2 重建：双方联机前 `seats=no seats`；Host 联机后仅网络 P2；Client `seat=P2 copies=1`。没有额外本地座位的双人会话应当各看到两名玩家。不能靠强制“一进程一莉莉”破坏本地多人需求。

## 三、第一轮修复与验证

`[E-源码]` 删除 simLocal/animLocal/fxLocal、Cmd 注入、周期 normalize/ClearColor、HP 差值合成受击和旧 track-0/zoff 分支。统一 Pose（最多8个实际活跃轨道，实际索引0–255，包含80）、Visual（分开的组件相对变换/颜色）、presentation_policy；RootMotionFactor=0 经反射写回核验。guard 使用 ObjHandle，并新增 Effect Apply/Finalize/DeathStart 原生拒绝计数。协议4。

`[E-实测]` 第一次 Dev/Shipping 完整构建与7项CTest通过，包含native_offset指令位移预检、轨道80/代次/空轨/恶意输入及128KiB传输。但实机暴露 policy 枚举函数名称应为 `K2_GetComponentsByClass`；该轮 `failed>0`、副本隐藏，**判失败**。修正函数名及schema失败时关闭Actor的路径后重新构建，7/7通过。第二轮部署 Dev DLL SHA-256前缀 `1376aa74098dd6bf`。

验证环境：同机隔离 Host2/Client2，1280×720、60fps；后台 UEBridge/虚拟pad，没有前台窗口操作。完整测试档备份：`%TEMP%/elmp-sync-hostclient-20261104-211113`；定位记录 `build/toolchain-smoke/sync-hostclient-backup.json`。以下原始结果在 `build/toolchain-smoke/sync-v4/`，关键观察在本文保留。

| 场景 | 结果与边界 |
|---|---|
| 干净晚加入/待机 | Client copies=1、policy ready=1、RootMotionFactor=0、rootDrift=0。Host idle t154.867与副本154.886；分别发起的读数不是同一帧，只证明相位已接近，不能按此宣称网络延迟为19ms。旧版晚加入样本为Host110.72、Client53.66。 |
| 走/转向/跳落/攻击 | 虚拟pad动作；两端trace均见 walk→idle_turn→walk→jump_up→jump_apex→jump_down→landing→idle→summon_DEFAULT_C→idle。攻击样本两端t0.200。运动中跨进程采样有时间差，未据此签收所有场景≤5UU。 |
| 同名重启 | 测试辅助在Host重新SetAnimation(0,idle,true)，两端均从约0.033开始，实例代次触发新播放。 |
| 实际高索引轨道/loop/移除 | 测试辅助显式把已知idle资源放到track80；Host t0.250、副本0.267；loop切false两端一致；ClearTrack后两端无该轨。**这验证多轨机制，不等于莉莉自然受击动画验收**。之前直接PlayAdditiveDamage(1)在这名莉莉上没有观察到track80，未把那次调用计为通过。 |
| VisualPivot/颜色 | 测试辅助Host pivot.Z=17、Color.R=0.65，两端读回一致，Actor坐标与renderer.Z=-65保持各自空间；恢复0/白后两端一致。未用周期清色。 |
| 召唤结果 | 一次真实ATTACK产生独立Spirit和Ability；Client replayed=1、无拒绝；FX4次创建/4次停止、最终live FX/Ability=0；局部自行产生的重复粒子有抑制计数。不能由一次攻击推断全部精灵。 |
| DeathStart窄闸 | 在Client远端副本测试调用原版ForceDeath；native deathStartBlocked从0→1，副本仍alive、HP400/400。只证明已登记入口被挡住。 |
| 过图 | Host从church_08入口进入church_07，Client跟随；24个敌人均接管，policy25个Actor全部ready、rootDrift=0、无pose/visual失败；旧Spirit副本清除。 |
| 敌人战斗 | 把隔离测试玩家移到史莱姆群附近，Host自然进入攻击链，Client Ability/Fx/Snd收到，engage组件调用7次。Client native EffectApply/Finalize拦截计数达到11/12，AI激活拦截24，无AI泄漏。此时未部署后续Dissolve/Companion。 |

空敌人地图一段约77秒的帧窗口：Host `frame.elmp_us P99=245µs`、Client `204µs`；帧间隔P99约16.83/16.84ms。这里只是双人空敌人地图，**不是128敌人压力验收**。24敌人场景性能另存 `enemy-perf-host/client.txt`，待后续修改稳定后复测。

## 四、实机发现的后续问题（尚不能计为完成）

1. church_07 的远处敌人：Host停止骨骼tick，e2010_Slime21 walk t=1.300，但Client继续到62.268。原因是收到的Host时间不变时旧比较跳过纠偏，同时副本骨骼持续推进。正在补暂停语义；新暂停修复尚未部署验证。
2. Companion 是独立于ZenithSpirit的Actor，原版Owner=玩家PC，自己拥有AIC。旧registry/快照遗漏这类跟随同伴，PS_Spirit光团又是既有PSC复用；单纯补一个SpawnFX事件无法覆盖晚加入与重新激活。正在接独立cp实体及确切FX组件的单一状态写者。
3. Enemy_Base死亡溶解是renderer动态材质DissolveValue输出，不是track0；正在加入有效值采样/纯setter重放，不调用死亡结算链。
4. 持续FX稳定ID已修；针对owner替换、短暂缺行、晚加入的基线重申与多客人停止仍需实机验收。
5. 完整mixingFrom链、所有Notify旁路、所有Spirit attachment/交互/死亡样式仍有图谱未知边，不能称为全游戏同步完成。

退出/换图生命周期另已修正：先结束结果对象再解除guard；guard arm失败直接退出；同图MapReady不清新基线；动态副本按地图/会话清理；PSC池复用旧ID不能停新activation。后续重连/多客人验收仍待执行。

## 五、第三轮实机（2026-11-04 22:10—22:20）

`[E-实测]` Dev DLL SHA-256 前缀 `ff035d61b32884b6`，完整构建与7项CTest通过，两端预检通过。先停止实例并按备份逐文件恢复、字节核验，再启动 Host2/Client2；联机前双方均无座位，联机后 Host 只有网络P2、Client copies=1。未做前台窗口操作。

| 场景 | 观察与结论 |
|---|---|
| 远处敌人暂停 | church_07 的 e2010_Slime21：Host `tick=false scale=1`、Client `tick=true scale=0`；两次间隔采样双方均 `walk@10.559`、位置Y6609.52/Z−877.68。Client不再从该相位继续推进。其它未初始化轨道的远处敌人保持空轨，没有故意初始化track0。证据 `pause3-before/after.json`。 |
| 原版敌人死亡溶解 | 在Host通过HPComponent.DoDamage(P1,100000,false,false)杀死同一史莱姆；四组MID的DissolveValue都递增。跨进程样本为Host/Client 0.303333/0.205066、0.689651/0.652356、0.939422/0.939422。Host销毁组件后Client HP0且hidden=true，最后值0.996764；Client dissolve writes=23、schemaFailed=0，无调用客户端DeathStart。测试读取脚本在Host组件销毁时最初缺少判空，UEBridge返回受控Lua错误；补判空后完成末态读取，不是游戏崩溃。证据 `death3.txt`、`death3-after.json`、`death3-status.txt`。 |
| 运行中皮肤 | 临时Host脚本P3切skin2→1，Client对应slot53均跟随，位置一致、idle时间接近；随后P3 leave，Client恢复copies=1。此测试刻意创建了临时第三座位，不把它当作幽灵玩家。证据 `skin3.txt`。 |
| 持续FX去重/owner重建/停止 | Host以原版BaseFX.SpawnFX新建PS_Spirit，registerRunning=true、pool=None。Host ids78→79后保持79、baselines2→4；Client spawn只增加一次、fxKept增加。测试辅助 `elmp despawn 51 keep` 删除远程Host副本，下一快照自动重建；原ID重申后fxReplaced=1、只补一次spawn、live仍1。Host原PSC.Deactivate后两端live FX归0，stopMissed=0。证据 `fx3.txt`。 |
| Companion失败发现 | 原生生成函数虽传SpawnParameters.Owner=玩家PC，但实机Owner已是AIC。UE4.26 Pawn.cpp的APawn::PossessedBy会SetOwner(NewController)。本轮错误采样顺序导致hostIds=0/unowned持续增加，**本轮Companion判失败**；不能用schemaFail=0替代业务验收。图谱已改为AIC→LeaderPawn→Pawn.Controller→seat，等待下一DLL验证。 |

这一轮最后8192帧窗口的ELMP开销：Host P99=629µs、Client P99=455µs；帧间隔P99=16.863/16.852ms，Client最大ELMP帧35.9ms含副本重建。窗口混合暂停、死亡、换皮肤及重建，不能视作统一场景A/B或压力验收。背景截图 `Saved/ELMP/pilot/shots/sync3-*.png` 显示两名莉莉；截图时窗口本来未最小化。

本轮义肢登记：原版DoDamage、四组MID只读K2_GetScalarParameterValue、临时脚本座位与skin命令、BaseFX.SpawnFX/新PSC.Deactivate、仅远程slot51的despawn keep。只用于隔离实例验证，不进入发布包；结束时恢复SaveGames与Saved/ELMP/ELMP备份。

尚需验证：Companion修正后的归属/光团基线、断线重连、多客人持续FX停止；另已安排修正Emitter忽略附件组件、远程PSC池隔离和可靠队列过载时静默丢消息。完整mixingFrom、Notify事件静默、Owner.CustomTimeDilation、全Spirit/全Boss覆盖仍未签收。

## 六、协议5机制验收与截图反证

`[E-实测]` Dev DLL `407893ad8f18332ab37faa96e9c2543f9f0fd138abb488494b17db742f249553`；完整构建7/7 CTest通过。新增Emitter明确挂点/socket、原版生成后的相对变换、Euler独立校验；远端PSC使用Pool=None，停止后由全局有界retire阶段等待原版Complete（inactive且粒子数0）再销毁。可靠队列过载明确结束会话，不静默丢最终Stop；纯传输已测第257条可靠积压、513条未消费入站事件与满datagram队列关闭通知。

| 场景 | 结果 |
|---|---|
| 同伴归属/基线 | Host12个同伴（每玩家6个，含inactive组），Client6个远程同伴，schema/policy/apply/fxMissing均0。只证明对象/状态应用成功；渲染另见下方失败。 |
| 第三实例晚加入 | Host同伴12→18；两个Client各2个远程莉莉、12个远程同伴。既有Host同伴的光团状态和持续BaseFX均到达晚加入者。Host Stop后两个Client live FX都0、stopMissed=0。 |
| 离开/重连 | 第三实例正常leave后Host剩网络P2，首个Client copies2→1、同伴12→6、removed6；首个Client leave/读档/rejoin后仍copies1、同伴6，旧副本累计removed12。第三实例已停止并恢复其原SaveGames/ELMP备份。 |
| 组件挂点/Euler | 测试辅助原版SpawnEmitterAttached附到Host VisualPivot，relative(7,13,19)、rotation(15,30,5)。Client原PSC读回同样父组件、相对变换、socket=None；attachmentRejected/fxBad均0。Host测试采用AutoRelease池，Client对象位于远程Actor下，由Pool=None重放接口独占。`PoolingMethod`非反射字段，Lua的该项输出不是有效数值证据；池策略以已核调用参数为据。 |
| 骨骼挂点 | 经原版SpineBone.GetAttachComponent(Hip=60,followRotation=true)创建挂点，再同样SpawnEmitterAttached。两端各自的SpineBoneFollower收到相同relative/rotation，未猜生成对象名后缀；attachmentRejected=0。 |
| 尾迹回收 | 组件挂点粒子停止后Client live FX=0，尾迹完成后ownedPSC=0、retiring=0、retired=1、retireFailed=0；骨骼测试停止后短时retiring=1，保留尾迹而非立即切断。 |
| HP0后的结果 | 测试辅助只把Host e2010_Slime21的HP属性临时写0，等Client收到HP0且仍visible，再原版BaseFX.SpawnFX；Client成功live FX=1、fxBad=0，无需重开死亡/伤害结算。最后恢复原HP100，两端读回100并停止测试PSC。此项是结果通道边界测试，不是自然死亡验收（自然死亡见第三轮）。 |

原始记录在`sync-v4/`证据目录内，以`multipeer5`、`emitter5`、`bone-emitter5`、`dying-fx5`命名。此目录名沿用本次调研目录，不代表文件全部是协议4。

一次混合待机/过图窗口：Host ELMP P99=697µs，Client414µs；最大帧含冷启动/过图（Client182ms），不能以P99掩盖加载卡顿，也不是128敌人压力验收。

**截图反证，协议5同伴渲染判失败**：`sync-final-host/client.png`显示Host同伴可见，但Client远程同伴仍缺模型。进一步读回Actor旋转：Host与Client本机同伴都是(0,90,0)，Client远程同伴是(0,0,0)，Scale均1；二维骨骼平面侧对镜头。Pose、renderer visibility与相对变换均成功仍不足以签收。义肢只把6个远程同伴Actor旋转临时设为(0,90,0)，模型立即全部出现（`companion-rotation-proof-client.png`），闭合因果。正式修复须复制Host真实pitch/yaw/roll，不硬编码90；协议6待后续验证。

所有上述义肢只用于隔离实例。协议5包已通过CRC、DLL哈希及Shipping零Lua检查，但随后发现的朝向问题意味着它不是本轮最终交付；最终产物见下一节。

## 七、最终协议6构建与双实例状态

`[E-实测]` 停止协议5实例、逐文件恢复并字节核验Host/Client的SaveGames与Saved/ELMP/ELMP备份，部署协议6后重新读slot0联机。**未执行临时旋转义肢**：远程同伴旋转自动读回(0,90,0)，`actorRotationWrites=6`、copies=6、schema/policy/apply/fxMissing均0；后台截图`sync-final6-client.png`显示原来侧对镜头而缺失的同伴模型。与Host截图及`final6-rotation.txt`交叉对照，正式Actor旋转同步通过。

- 构建：Dev/Shipping/FrameProbe完整通过；CTest **7/7**。协议6三轴旋转、非有限数拒绝与最大容量边界已覆盖；full=129225B、全key替换delta=130939B，距128KiB只余133B，继续增加快照字段必须重新测量，不能凭经验塞入。
- 工具链：`doctor-final.json`25通过/0失败；最终`smoke-final6.json`46通过/0失败/2跳过（Dev2与第三实例已停止，因此其UEBridge跳过）。CUE MCP26工具、IDA MCP66工具及CUE本体入口可用；当前聊天未热加载原生MCP清单的限制未改变，仓库CLI已实际调用。
- 最终运行：Host2 PID **26876**，Client2 PID **32152**；全系统EL Shipping进程查询仅这两个。双方联机前`no seats`，联机后Host仅网络P2、peers=1/parked=0；Client seat=P2/copies=1，无额外本地座位。Dev2/Client3停止。实例在church_08，虚拟输入未占用、mute未开启。
- 包：`dist/ELMP-0.3.0-{dev,shipping}.zip`已重新生成，CRC通过、包内DLL与构建及Host/Client部署一致。Shipping零Lua/零PDB，Mod目录只含dlls/main.dll、data、elmp.json；SDK头在zip外层sdk。
- Dev SHA256：`818d4999ff834fa049921ad4e438354df641d22f17edcb85970bd27e99e62696`。
- Shipping SHA256：`110f9f1e8222f4622aa22bb3c1191118f544073df1dc9d40678c22902fd80c13`。

证据：`build/toolchain-smoke/sync-v6-build.log`、`artifacts-final.json`、`sync-v4/final6-status.txt`、`final6-seats.json`、`final6-rotation.txt`及两实例`Saved/ELMP/pilot/shots/sync-final6-*.png`。所有测试档已还原；本轮未操作正式游戏目录/存档，未激活/移动窗口或系统鼠标。文件修改未提交Git；UE4SS子模块原有Cargo.lock修改未动。

### 明确未签收的范围

本轮建立并验证的是已核链路的呈现架构，不是“全游戏完美同步”结论。完整mixingFrom/队列历史、所有Notify旁路、Owner.CustomTimeDilation、同伴粒子相位/动态颜色、一般PSC停用后原对象再次Activate、所有Spirit attachment/死亡样式/所有Boss仍有未知或未测边。FMOD attached/looping生命周期未补齐。双机网络、60/144/波动三档录像、128敌人性能与所有位置≤5UU门槛尚未全部验收。可靠过载会关闭会话；大规模初始名字/事件突发的可用性仍需压力测试。不得用本轮局部成功覆盖这些条目。
