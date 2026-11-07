# ADR0017：命中反馈与自主角色移动动画的接入核查（2026-11-08）

状态：调查中，先补失败诊断，再以隔离实例确认第一处分叉。

用户报告下砸后Client退房、观察到白花被破坏，以及狗无敌人绕行时位置/朝向基本正确但部分Spine动作错误。Client日志为authoritative feedback context timed out，原有通用错误未包含消息、依赖和具体等待原因。白花破坏尚未证明异常，也未证明与下砸或退房存在因果关系；不能由退房后剩余窗口推断白花在哪端被破坏。狗召唤/攻击接入通过也不等同于移动动画完整。

原则：优先处理已确认的Client退房与召唤物动作缺失。反馈失败必须打印精确消息/引用/资产/根/Host时间/播放时间及就绪原因，在改变超时策略前实机复现。狗核查原版Locomotion/State/动画定义如何从移动上下文更新，优先补共同输入或原版维护入口，不用狗名字白名单或逐帧复制Spine轨道遮盖问题。

## 白花观察的优先级更正

2026-11-08用户补充：附近玩家可能引起怪物射箭，也可能有乌鸦魔法球击中白花。这些正常伤害来源尚未排除，不能以白花被破坏判定下砸范围异常或联机结算泄漏。此项降为低优先级待观察，不阻塞本轮退房/动作修复，不为其预先修改伤害范围、碰撞或结算策略。只有获得确切伤害来源/命中位置不合理、或Host与Client破坏状态确实不一致的证据时才恢复专项排查；届时先核实原版伤害来源、距离、地图身份与同步隐藏状态。Client结算关闭的既有原则不变。

## 自主角色的修复边界

[E-实测] 无敌人绕行时Host狗InputComponent.InputSnapshot.X=±1，Client=0；CommandQueue.CurrentInputSnapshot.X在双方均0。Host播放run，Client播放idle；强制走路模式双方均None，说明不能从命令队列输入推导实际移动输入。

[E-IDA] Spirit.Tick CF3C20实际顺序为Actor.Tick→CF8820→CommandQueue.UpdateCommands→InputConsume(CEF310)→Locomotion.Sample(CBAE40,apply=false)→召唤寿命/结束维护。CQ接缝既早于本帧移动输入消费，也不包含后半段。原版Sample的false分支仍更新WalkMode/面向，仅不投递移动输入。因此不追加一套手写动画或在CQ里重复调用Sample。

决定：将自主回放的帧消费从CQ.UpdateCommands移至原版Spirit.Tick，CQ接缝只保留权限门；Host在整段原版Tick后捕获一帧，Client以已交付dt调用整段原版Tick。命令沿仍按原Start/End处理，原版InputConsume处注入这帧独立的Actor InputSnapshot，让原版Locomotion与Spine自己选择移动动画。CQ输入保持独立，不把它与Actor输入合并。ForcedWalkMode作为外部AI选择的上下文同时传递。删除旧的CQ自建消费主循环，而不是同时保留两个推进器。协议15增加独立Actor输入与原生Tick前缀，生命周期和Client结算门不放宽。

其余事实与决定随实测更新；反馈修复和验收结论不先行宣称。白花仅保留未确认来源的观察记录，不列为已确认缺陷。验收补无敌人巡航、静止/走跑/转身、两个拥有者、攻击、软退场、过图、晚加入和命中/反馈；按同一播放时间核对原版输入/状态/定义，不以对象出现或Actor位置一致判定完整。

事件重试的过期判断必须在本轮实际解析之后执行：旧实现仅对ChildStop作最终解析，其余消息可能在上下文已就绪时仅因旧deadline而结束会话。统一为所有消息先解析、仍Retry再检查期限；保留300ms容量/期限及fail-closed。此次另复现FX stop超时，为其补确切seq/Host时间/播放时间/活动引用/待生成引用，不将两次不同故障码混作同一已修复原因。

装备轮询的已发送基线必须来自实际Build/EquipRequest序列化的内容；不能首次轮询时把当前（可能已经改变）的本地装备直接登记为已发送。入座后立即换装属于本轮回归，避免在测试中靠等待一秒掩盖初始化窗口。

[E-实测/E-源码] 上移Tick后Actor输入和WalkMode已一致，但Client移动组件仍在无加速度状态下减速，Spine可选idle。AI的AddMovementInput另写Pawn.PendingMovementInputVector，供CharacterMovement消费；它不等于CQ输入，也不能仅用Velocity替代。隔离义肢将这份输入送入原版Pawn入口后恢复run。协议15同时携带Host原版Tick结束时的PendingMovementInputVector，Client经ConsumeMovementInputVector/AddMovementInput恢复这份已被Host接受的物理输入，原版CMC负责加速度、速度与动画判断。不添加狗/动画名特例。
