# 远端命令执行顺序与同图多来源时钟（ELMP v2，2026-11-10）

协议 22，从协议 21 的 `512a979` 继续。证据来自 Shipping 可执行文件工作库 `elmp`（基址 0x140000000）、本机 Paks 的 CUE4Parse Kismet 反汇编以及两个版本的源码对照。本轮仅静态分析和编译。前一轮四端数据对应协议 21。

## 同一个 Start 可以读到不同输入

[E-IDA] `AZenithCharacter::Tick`（CF3740）先在 CF37E6 消费输入，在 CF3855 调 CBCA20 向队列写本帧快照，按条件在 CF386A 调 CC2520 匹配输入，随后在 CF3875 调 CC3970 更新队列，再在 CF38AA 推进 Locomotion。CC2520 迭代 CommandSettings 并生成候选；角色 Tick 忽略它的返回值。CC3970 会向当前动作投递队列快照、处理命令，并对未开始的候选扣除 TTL。C97C60 的立即入队会同步更新队列，已在更新时会要求后续再更新。

协议 21 在调用原版 Character.Tick 之前就执行 Host 的 Start。即使动作类及来源编号正确，原版的内层零时长更新也可能读取上一帧的队列输入。协议 22 保留原版写快照，只在该 Character.Tick 调用点抑制远端 Pawn 的普通输入匹配。Host 决定在原版首次进入该队列的 UpdateCommands 时交付。先撤掉本次交付标记，再允许立即入队重入。局部蓝图显式入队仍返回原版 ID；只有本次 Host 交付的候选可启动。冷加入在首帧取得安全持续动作基线，后续用同一条 Start/End 流接管。

[E-IDA] CB4F00 的覆盖路径用 reason=2 移除旧队首。CB9420 核实对象、触发原生结束和通知，回收命令。旧路径会把所有非零结束原因改为清整个缓冲。现在按激活 token 与队首对象复核，传原结束原因。正常结束交给 RequestEnd。单个源帧中，后续 Start 之前的 End 按记录顺序处理；末尾 End 留在最后一段原版采样之后。帧内多次覆盖的精确子帧时相仍需游戏内核对。

## 动作的世界输入

[E-CUE] `CA_LedgeClimb_Base` 的启动图调用 `CacheClimbTargetLocation`，读取 `CachedAutoClimbComponent.GetLastTargetLocation()`，加上按 Pawn 变换的 LocalLocationOffset，然后写入 ClimbTargetLocation。该图使用这个点设置位置和采样视觉位移。确认动作名相等只验证了其中一个输入。

[E-IDA] GetLastTargetLocation 的 exec 桩 D427E0 在 +0x23 调 CA9AC0。后者只复制组件 +332 至 +343 的缓存 FVector。协议 22 在原版 Start 真正读取这个值时捕获它，随该次 Start 发送地图本地坐标，在对应 Pawn、组件、激活的同步调用中提供；不用运行时写入原版组件偏移。原版仍处理角色朝向、局部偏移、IK 和曲线。读数缺失或未消费可请求该 Client 的新视图。

[E-CUE] `CAS_PileAttack.SampleChargeLevel` 读取 GetExecutionTime 后做浮点比较；SampleDivePhase 读取扫掠结果和 IsFalling；OnSampleCommandAction 经 GetGroundCommandAction 与 AddNewCommand 加入落地动作。输入相同不能保证跨进程比较落在阈值同一侧。协议 21 的 Host 选择保留到协议 22，落地动作仍由原版执行。

## 三名远端角色不能互相推进能力时间

[源码对照] 协议 21 的每个远端 Character.Tick 与自主精灵 Tick 都可调用同一个 advance，改写全局 time/rate，再遍历全部活跃追踪能力写倍率。任一莉莉断粮会以倍率 0 调用同一路径。另一来源的目标查询等待也共用同一计时器。这个覆盖关系从调用图即可确认。现有日志没有将每次手动退房都定位到这里。

协议 22 的 Client 帧泵按快照的 Host 时间估计和 50 ms 缓冲推进视图时钟，受已收到的 Host 封口限制。敌人位置在帧泵更新一次。各莉莉和自主精灵各自保管消费时间、倍率与等待时间；能力查询、采样和终止跟随所属来源。自主精灵在同座位可并存，因果消息带上 Host role ID。绑定、换图、失效和离会沿现有 Replica/Role 生命周期清理。移除过期 Pawn 时仍按原弱句柄恢复其拥有对象的时钟。

另有一处独立的对象寿命问题。旧 Client 保存 query 在共享 vector 内的下标；无关能力退役时 erase_if 会移动后面的元素。后续 SetTarget 会有读到另一条记录的可能。现在各原生能力节点保存它实际消费的查询结果。

## 尚待区分的边界

- 低台历史日志证明 Host 决定未消费。该次现场没有双方 getter 的值，不能追认其根因是落点不同。
- [E-CUE] HomingBase 的 GetHomingTarget 返回 Actor，SetHomingTarget 会从该 Actor 解析组件；可选 SnapRotationToTarget 还读取 Actor 位置。[E-IDA] CC0BC0 的原版追踪读取目标组件位置及速度。当前对活着的目标仍可能用历史样本写同一个敌人 Actor/组件。多个来源同时追踪同一敌人时，该位置所有权仍值得单独核对。
- [E-IDA] CB4F00 在覆盖时还会对旧动作发一个带后继动作的通知。保留移除原因不能单独证明所有覆盖通知的参数和时相都与 Host 相同。
- 实际路由、时相与连续手感、首连积压的成因、四人长时间游玩均未在协议 22 上验收。按用户要求本轮未启动测试或部署。

本地 IDA 输出保存在 `build/party-1110/static/`，结束路径另见同级 `remove-native-ida.json`、`end-command-ida.json` 与 `override-dispatch-ida.json`。后续版本可用本仓工具链按上述 RVA、包名、函数名重取。
