# ELMP v2：自主 Spirit、跟随历史与乌鸦（2026-11-08）

对应 F2.4/F2.5、X-1；决策见 [ADR0013](../adr/0013-autonomous-spirit-decisions.md)。这是隔离 Host2/Client2 的技术验收记录，最终观感仍需用户双窗口游玩评定。不是所有 AI、所有装备或旧弹体晚加入基线都已覆盖。

## 结论与来源

- [E-CUE] 西方商人 s2102 的作战对象是 ZenithSpirit，独立 AIC/BT 经过 FindClosestEnemy、FaceTarget、ExecuteAction，再由 CA_s2102_Attack 的原版时间线发弹。LV3 一次发射五颗；它与未激活的 BP_Companion_s2102 不同。
- [E-实测] 激活 Spirit 的 Actor.Owner 指向 AIController，GetSummoner 才指向莉莉。只沿 Owner/Pawn 归属会漏掉该对象；把所属客人的本机表现排除在权威投递之外，也会漏掉其独立 AI 决定。缺的是原因域与上下文，不是少复制一个乌鸦动画。
- [E-SDK] ZenithCharacter 与 ZenithSpirit 都继承 ACharacter，二者是兄弟类。CommandQueueComponent、LocomotionComponent 必须按 Spirit 自己的声明解析。不能因名字相同就套用 ZenithCharacter 偏移。
- [E-IDA / E-实测] AddNewImmediateCommand 内部同步调用 UpdateCommands(0)。该调用属于立即入队内部执行；网络消费必须防重入，不能再次读取仍在队首的 Start 帧。
- [E-SDK / E-实测] Locomotion.SetFacing 参数是 bool；协议的 1/2 要转换成 facing==2，不能把两个非零值直接作为原生参数。

## 原版接缝与数值历史

[E-IDA] CommandQueue.UpdateCommands 原生入口 CC3970，由反射 exec 中 E8 调用求出地址并与清单校验。Spirit Start CF2F00、Commands CF3090；Start 中 sourceCommand 弱引用写入位置为 +1440，经过清单指令位移核实。角色只绑定召唤命令 GetSpirit 返回的实际装备池对象，不自行 Spawn 同名替身。

[E-IDA / E-实测] 冷加入的 CAS.GetSpirit(D0F8A0) 只读 +888 弱引用，GetOrCreate/Preload(CA8300) 不建立该缓存。原版 CAS.OnStart(D1D360) 调用 SummonerComponent CE1920→当前 SummonSetHoldComponent 的 CE15B0，选择实际装备/类池对象后写入缓存。复用这个取实例入口与经指令校验的弱绑定即可冷恢复；不重放莉莉 CAS 动画、消耗和旧时间线。旧版在重连时 GetSpirit 一直为空、积压120帧的故障由此修复。

[E-IDA] FollowTarget.TickComponent CC0950 在 Actor 位置之外，还读取以下内部状态：

| 数值历史 | 偏移 | 验证指令 |
|---|---:|---|
| 混合位置 | 256 | CC09E2 |
| 三轴摆动相位 | 268 | CC0A81 |
| 当前目标位置 | 288 | CC0982 |
| 前次目标位置 | 300 | CC099D |

每项是 12 字节数值。启动同时核 EXE SHA、函数前缀、指令位移与类大小。Owner/目标弱指针不跨进程复制。只写 Actor.SetLocation 不会重置这些积分历史；只同步目标位置而遗漏历史，仍会出现几到几十 UU 的反复纠偏。单次 Client Tick 消费多个网络分步后，还必须在跟随组件接缝运行同样分步，不能再多推进一轮摆动时间。

[E-CUE / E-源码] Homing 的原版 Query→GetHomingTargetOfActor→SetTarget 和 SnapRotationToTarget 分别需要目标 Scene 与 Actor 位置。历史目标暂不在 Client 时，数值载体分别提供 root/aim；不注册为敌人，没有 HP/AI/碰撞或结算能力。只对自主原因域、已证明 Host SetTarget 的事实开放；未知非 Homing 依赖仍拒绝。Engine.TargetPoint、AddComponentByClass 的签名/注册语义以 UE4.26 源码核对。

## 修复结构

协议13新增 SpiritFrame：实际角色/召唤代次、Caster seat/Pawn代次、精确类和召唤命令、接受的 Start/End、输入/运动/方向/时钟、非命令转身定义、FollowTarget 输入与历史。Source.domain=1 表示独立自主命令，domain=0 保留莉莉输入；复用 Root/Query/Homing/End 因果事实。

Client 停止对应 Spirit 的 Brain 决策，原版 CommandQueue、跟随、Spine、AbilityTimeline 与发弹继续运行。立即命令边界不推进另一轮世界时间。Host 的命令与目标事实也发给拥有该 Spirit 的客人；Client 伤害、Effect、死亡和存档结算仍关闭。没有 Crow 资产白名单，也没有逐帧复制乌鸦骨骼或人为生成五颗魔法球。

角色/队列、类加载、等待绑定、peer命令索引、跟随步与载体都有上限和失效源，详见架构缓存表。`elmp lan status`、`elmp causal`、`elmp commandctx trace`、`elmp causal trace dense` 可审计。dense 仅供 Dev 验证，默认 trace 每五次 Tick 取样。

## 隔离实测

本轮先备份 41 个原版/ELMP 存档文件，测试辅助均经 UEBridge；未操作系统光标、前台窗口或真实手柄。过程证据位于忽略目录 `build/crow-sync/`。

- [E-实测] Host P1 与客人 P2 各自召唤：两端接受相同自主命令、关联原版弹体，unexpectedStarts=0；连续帧队列通常每角色 3–4 帧，跟随待执行步在帧后为 0。
- [E-实测] build13d 的身体同动画相位对比：126 点，中位误差 0.035 UU、P95 0.477、最大 2.011；补齐跟随历史前曾出现最大约40–70 UU。数值比较有采样/插值误差，不等同于全场景画面验收。
- [E-实测] build14 稳定目标的 dense 追踪：20 个共同根、523 个同原生 age 的位置样本，中位 0.043 UU、P95 0.197、最大 0.630；共同发射位置最大误差约0.000015 UU。为取得持续飞行样本，在暂停两端后临时关闭该弹体碰撞/视线检查；正常碰撞版本另核对了原版发射、Query 和地形取消，测试参数不进入发布代码。
- [E-实测] 后续跨大距离瞬移时，另一组644点虽 P95 0.522 UU，仍有最大46.987 UU离群值。不能用前面的稳定场景结果宣称任意突变/所有弹道已完美闭合；不能仅按同时截屏或稀疏五帧插值评价曲线误差。
- [E-实测] 暂停 Host Crow Brain 后分别经原版 SetFacing(false/true)，两端有效 Crow 均读回对应左右方向。
- [E-实测] s2082 狗（实测装备等级 LV2）交叉验证：两端相同 CA_s2082_LV2_AttackA、相同自主序号12、共同技能根265/268。证明接缝不依赖 Crow 名称；没有据此宣称狗的所有动作视觉都通过。
- [E-实测] 人为销毁 Client 精确目标副本：后续5次 Query 继续消费，数值载体累计创建12/销毁12、live=0、failed=0。随后实体受击特效依赖已删除目标，按现有策略以 authoritative feedback context timed out 结束会话；不扩大成“删除任意实体后整局仍应正常”的结论。

## 长测中分离出的共同问题

[E-CUE / E-实测] AE_RestoreMPToInvoker 的同步广播进入 WBP_MPGauge，Ubergraph +1303 以 Self 调 PlayEvent2D(ui-special-attack-available)。旧检查因处于 Effect 栈却找不到世界目标而断开，错误地把 UI 提示音扩大为世界受击反馈。按 UMG.Widget 类型保留本机 UI 音效，未增加音效资产白名单。build19 强制把 MP 留在满槽前1点后由原版命中补满，Host uiLocal=1，受击 Effect/音效继续投递，无该故障。

[E-实测 / E-源码] 退场帧是在 host_tick 内新采集，晚于外层 now_us；旧封口取 now_us，最后 playback=93309954.453、fence=93309952，流结束后没有新封口可追上。修复是采集后封口，未扩大阈值或延长超时。这与实体目标缺失是两个独立条件，新的诊断将二者分开。

## 最终回归（build20）

- [E-实测] 同进程 Client 主动离会→标题→读档→加入已有活动 Crow 的 Host，冷绑定成功；接续原版自主命令和弹体，未重放旧弹体。完成连续65秒采样和主动 ForceDisappear 后继续4秒，仍保持连接。
- [E-实测] Client 自主队列持续约3–4帧，unexpectedStarts=0；最终 roles/waiting/queued=0，anchors=3/3、scene contexts live=0 created=3 destroyed=3 expired=0 failed=0。没有用延长超时或扩大积压上限通过测试。
- [E-实测] 原版伤害/Effect/死亡结算继续拦截，Host 命中反馈正常投递；MP 留在满槽前1点后由原版命中补满，UI 提示音留在 Host 本机，未再触发来源错误。
- [E-实测] FrameProbe（两进程、60 FPS 上限、各3592样本）：Host mean 16.706 ms / P99 17.100 / max 91.421；Client mean 16.706 / P99 17.644 / max 85.077。存在单次停顿，不能宣称零卡顿。Client ELMP帧泵 mean 364.7 μs / P99 1259 μs；这不包含所有原生 Hook 内游戏执行，也不是裸 UE4SS A/B 性能结论。
- Dev/Shipping/FrameProbe 构建与7项 CTest通过；新增测试只覆盖协议有限数组、截断/尾随、非有限数等边界。发布包检查 Shipping 零 Lua/PDB。日志、trace、codec、封口/冷启动 IDA 与 CUE证据均在 `build/crow-sync/`。
- 部署前按本轮备份还原并核对41个文件哈希；新进程清除测试用50000 HP、飞行/碰撞/视线开关与虚拟输入。最终两个实例均 HP/maxHP=400，DLL SHA-256 相同（`ac454ec050b438db…`），探针关闭，Client P2 已加入 Host；后台截图已读取确认两端正常在教堂休息点。

## 本轮崩溃教训

初版发生的读取 0x4040000c 是把 ZenithCharacter 属性偏移用于 ZenithSpirit；第二次栈溢出是 begin→立即入队→UpdateCommands(0)→begin 的同步重入。分别改为 Spirit 自有反射 schema、明确的原版重入路径与每角色更新保护。不能用 alive() 或增加网络缓冲掩盖这两类问题。

## 边界

进行中加入只借用当前有限命令的视觉定义，从下一个实际交付的完整 Start 恢复自主执行；不伪造历史弹体。Client 删除目标后的实体反馈仍拒绝；非自主原因域、未知私有脚本/随机状态、其它 AI 类型未自动纳入支持。独立 AI 接入不意味着改掉 Host 权威架构；更准确的要求是补齐每个独立原因域的原版输入、决定与积分历史，再检验是否闭合。
