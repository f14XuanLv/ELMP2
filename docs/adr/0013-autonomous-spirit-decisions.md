# ADR0013：自主 Spirit 的 Host 决定流（2026-11-07）

状态：用户授权分析并修复乌鸦朝向/发弹，按真实证据实施。

CUE确认西方商人s2102是ZenithSpirit，具有独立AIC/行为树：FindClosestEnemy→FaceTarget→ExecuteAction(CA_s2102_Attack)→原版AbilityTimeline发弹。它不是普通未激活Companion。实机确认其激活后Owner=AIC、GetSummoner仍为玩家Pawn；原有沿Actor.Owner的角色归属会漏掉这类Spirit。Client自己的预测乌鸦也存在独立AI，不能按“本机玩家表现不必同步”排除Host的自主决定。此前ADR0011已明示独立AI是未闭合原因域。

实施：
1. Spirit归属优先验证原版GetSummoner，再沿Pawn/PC；保留Companion LeaderPawn规则。
2. 自主角色以实际Spirit weak身份及每次Start代次建独立流，带精确summoner seat / Pawn代次 / class / summon命令类，不按乌鸦名字写白名单。Client通过原版召唤命令的GetSpirit和原生Spirit Start恢复同一个装备池对象；不插入莉莉当前命令队列。
3. Host保留AI决策；Client仅暂停对应Spirit的Brain决策，继续原版跟随、CommandQueue、动画、时间线与技能。原生Start/End记录实际接受的命令；在CommandQueue.UpdateCommands接缝传可靠连续帧、方向/位置/速度、实际delta及有界命令沿，Client按Host时序调用原版入队与UpdateCommands。
4. 自主命令有单独source domain，关联Host命令序号；沿用已有Root/Query/Homing/End事实闭环和对象池身份，必须向该角色所属客人的本机预测端也投递。不能用最新莉莉输入冒充AI决定，也不能重复复制已经由该命令原版发出的Act/FX。
5. 非命令维护的转身逻辑层按既有外部动画定义接口传上下文，命令开始时明确归还ACTION层；不逐招复制Spine姿态。角色时钟只有一个写者。
6. 地图/会话/拥有者/召唤Start代次撤销旧绑定；队列/角色/沿/序号有上限和故障，记录绑定、实际命令、发弹/目标、清理及帧时间。Client结算策略保持关闭。

验证Host P1与Host P2召唤、静止/移动与左右目标、实际非空选敌/发弹/轨迹、重复召唤/换组/地图/进行中加入；用同类AI Spirit交叉验证推广边界。前台操作禁止，测试存档以本轮41文件备份还原。协议13，纯codec只测边界，不建行为mock。

进行中加入若没有历史有限命令开始，不重放过去发弹；保留当前视觉上下文并从下一个完整Host命令恢复。已经在飞的旧弹体基线与任意AI/剧情系统不自动包含在此项验收内，必须如实记录。

命令沿使用显式 decision_only 帧，在原生 Start/End 接缝立即采集，只进入原版入队/结束，不推进角色或世界时钟。原版立即入队内部 UpdateCommands(0) 不当作额外物理帧；否则会多推进一次角色时间，并可能在下一次常规帧前漏投“时间线0秒发弹”的 Root。连续帧保留实际 Host World/Actor delta，Host 墙钟时间与游戏 delta 分开。

FollowTarget 也有输入上下文：所属客人的预测莉莉与 Host 权威莉莉可能相距较远。不能一边锚定权威 Spirit 位置、一边让原版跟随组件追另一处预测目标。传原版 GetFollowTarget 实际 SceneComponent 的位置/旋转/速度，Client 用无表现、无碰撞的 Engine.TargetPoint 根组件承载这份上下文，交给原版 SetFollowTarget；保留原版偏移、摆动、混合算法，不移动本机莉莉来伪造上下文。退役时精确恢复仍由本流替换的 target，再销毁载体。该载体只作为原版组件的呈现输入，不进入网络实体或结算域。

[E-IDA] FollowTarget 原生 Tick CC0950 还读取独立混合位置(+256)、三轴摆动相位(+268)、当前/前次目标位置(+288/+300)。这些是积分历史，不会被 Actor.SetLocation 重置。连续帧带经过 SHA/指令前缀及位移核实的这四组数值，目标变换含尺度。Client 在该组件原生 Tick 接缝按已消费的 Host 分步 delta 运行原版更新，帧开始时恢复对应历史；不把多个输入分步累计后再多推进一次摆动时间。只复制数值历史，不复制 Owner/目标弱指针。角色每轮待执行跟随步≤64，撤销时清空。

自主原因域的 Homing 若已收到 Host 的精确目标组件位置/速度，但该历史实体尚未/不再存在于 Client，使用无碰撞、无结算能力的 Scene 上下文载体继续原版 Homing；只对已证明调用 SetTarget 的组件上下文启用，非 Homing 的未知目标依赖仍 fail closed。逻辑引用仍保留 Host ref/代次，不把载体注册成敌人；原版 Query→SetTarget→SnapRotation 路径继续，目标 Actor 位置与目标 Scene 位置分别承载。每个激活独立租约，End/复用/退役/地图和会话清理销毁；Ready 的错误必须区分封口时间和缺失引用。

2026-11-08 冷加入实测：CAS.GetSpirit 只读 +888 弱缓存，该缓存只在原生 CAS.OnStart(D1D360) 中取得；Preload/GetOrCreate 并不会建立此绑定。[E-IDA] 原版同一段先从 Pawn 的 SummonerComponent 调 CE1920，再交给 SummonSetHoldComponent 的当前装备/类池 CE15B0，最后缓存 Spirit。冷加入只调用这段原版取实例入口并建立经指令校验的 CAS 弱绑定，再用原版 Spirit.Start；不重跑莉莉 CAS.OnStart 的动作、MP、历史能力时间线，也不自行 Spawn Spirit。

同轮长测发现 Effect 恢复 MP 会同步广播到 WBP_MPGauge，触发本机“奥义可用”提示音。[E-CUE] 该 Widget 的 Ubergraph +1303 以 Self 调 PlayEvent2D。UI Widget 不是世界受击反馈，即使处于 Effect 栈内也保留本机原版音效所有权；以 UMG.Widget 类型边界排除，不用具体音效/乌鸦名字白名单。未知世界 Effect 源继续拒绝并输出确切来源。

最终退场帧可在 Host Tick 内新产生，其 t_ms 晚于 Tick 入口 now_us。封口必须在本批采集完成后取单调时间，不能使用更早的 Tick 参数，否则最后一帧比封口超前几毫秒、流终止后再无新封口，触发超时。保留原有缓冲和目标验证，不放宽时间阈值。
