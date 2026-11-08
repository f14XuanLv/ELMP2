# 空目标、短命弹体与Spine求值重入（ELMP v2，2026-11-09）

决策见[ADR0021](../adr/0021-null-and-terminal-context.md)。本轮有两个已复现并修复的系统性缺口，另有一个已观察到的输入来源错配当时未修复，后续由[协议18修复记录](buffered-command-source-2026-11-09.md)接续。不能将不同错误都称为“下砸失败”或将短轮通过宣称为所有退房已根治。

## 用户真实现场与受控复现

[E-实测] 用户重启两进程并手动操作，另授权仅开追踪。只读观察程序在2026-11-08 23:48:24捕获Client主动离会：`native homing has no authoritative target context`。两端Root257、input13523、seat1、generation3985、domain0一致；女巫容器子节点0:1在Host从生成到结束仅938us，产生AE_Damage反馈，Query=null且没有Homing Tick；Client出现一次Homing Tick。证据完整保存在`build/pound-exit-1108b/user-observe-restart/incident-234824/`，此轮没有自动输入/切图/重连。

后续获准修复及复现。准备Client冷加入forest_03时，旧DLL又触发栈溢出（用户提供同一调用栈）；实际崩溃目录`8B8E4E074093561B7CC24498BD9A4575_0000`及双端日志已保存到`build/terminal-homing-1108/crash/`。原计划受控接触测试尚未执行，不把这个准备阶段崩溃误称为Homing复现。

[E-实测] 先仅加入Spine重入修复，保持原目标账本与结束处理：forest_05空射一次，通过原版launch trace取得弹体真实出生坐标，再在该坐标生成敌对Slime，正常SUMMON_1。首次接触即复现原Homing退房，Root5/input2812仍精确一致，Host子弹体748us内结束、无Homing Tick，Client多出一次。`guard-baseline/`保存失败日志及双端trace；这是修改目标账本之前的受控反例，不是mock或直接制造故障状态。

## 目标与生命周期的共同缺口

[E-代码] 原receive仅在Query.component非零时种入初始Target样本，漏掉有效的ref_kind=0。后续Host若没有Tick（出生即命中/结束），就永远没有持续Target可补。Client直到Homing Tick才发现没有样本，而后置End巡检尚未处理。另有Root.homing将一个子实例的查询/停止所有权扩大到整棵技能树的问题。

协议17的修复包括：

- 明确null的Query与有确切组件的Query均建立初始上下文；有Actor却没有组件仍按尚不完整处理，不伪装成null。
- homing标志、目标与终态归属具体Node（activation、Root/path/class、弱Actor身份），所有子节点分别核实。
- 原生Homing Tick前核对同节点Host End与播放时间；已到期则调用原版Stop并跳过额外Tick，仍保留后置结束巡检。沿用原有毫秒精度边界，不增加超时或按资产名称放宽。
- 真正缺失的事实仍报错，并补充Root、path、query和来源诊断；不靠静默跳过来保持连接。

## Spine为什么会递归

[E-源码] SpinePlugin的GetBoneWorldTransform在bone.applied无效时调用InternalTick(0,false)；零delta/不广播delegate不代表扩展插件也不派发游戏通知。

[E-IDA] 7CF7D0/SpineSkeletonAnimationEx.InternalTick先调用基础7E96F0，再遍历扩展Notify，Notify返回后才记录当前项；SpawnFX 7D0600请求枚举挂点，7CEEE0在创建Follower返回后才填缓存；创建7CED20会同步Tick Follower，后者经7E7570读骨骼并可能重入InternalTick。新挂点尚未填缓存、通知尚未登记完成，构成同组件重复派发。崩溃栈中d_spawn只是该循环的一个位置。IDA摘录在`ida-evaluation-excerpts.json`；重建SpineExtension cpp不是行为证据。

修复在原版扩展求值入口设置同步组件作用域。同组件嵌套、0delta、无delegate/preview的骨骼查询只进入原版基础求值，正常外层通知继续运行。没有过滤粒子名、技能名或丢掉全部SpawnFX。只在联机会话生效；Ex CDO的vtable+1032、直接E8基类调用关系及双方函数前缀联合校验。作用域不跨帧保存裸指针，非法模拟重入明确报告，不能借这条边界再次推进游戏时间。

## 验证

[E-实测] Dev/Shipping/FrameProbe构建与7/7 CTest通过。最终双端Dev DLL SHA-256：`bb98e682fb27af6181d16393fdab810c2cca627728411af9e37bd1a2c32b108f`，协议17；Shipping重打包并校验无Lua/PDB、包内DLL与构建相同。

- `lifecycle-first/`及`matrix60/`、`matrix30/`、`matrix60-repeat/`：共4轮相同出生点接触，每轮Root/来源/类/path精确相等，实际伤害FX和音效正常消费，Host和Client均无该短命弹体的Homing Tick，持续连接。每轮另有一次空射，验证合法null但具有正常寿命的分支。
- `crow-regression-native/`：使用原地图带出生点的真实敌人，Host与Client自有乌鸦各产生5个弹体，两端各10个根，10次查询全部非空；持续Homing有采样、无故障。早期`crow-regression/`临时生成目标没有触发乌鸦攻击，零launch，不能计作非空追踪通过；其生成敌人和测试角色已清理。
- `wall-regression/`：蹲下、正常抓墙及独立狼、中途加入抓墙、松开，以及forest_03→forest_05→forest_03→church_08保持连接。idle/idle_turn少量采样差异保留，不宣称逐帧表现完美。
- `cold-forest/`：Host已在forest_03时Client离会回标题再加入，未重现栈溢出；Spine保护实际触发Host6/Client13次，invalidNested均0。最终Client账本roots/nodes/declarations/records/samples均0、scene contexts创建4/销毁4、fault空；null种子8、终态Tick拒绝8。

目录根为`build/terminal-homing-1108/`；部署前41文件备份位于`build/terminal-final-pair/`。测试同实例UEBridge串行，无前台窗口/鼠标操作，只使用Host与Client。测试原地图敌人的临时HP与AI设置随地图卸载清除，额外生成敌人清理；装备恢复，虚拟输入释放、Client恢复60FPS，最后回church_08并经Host原版FullyRestore回满两位玩家。

## 方法判断与未闭合边界

原版管线方向仍有价值；本次错误在于对入口副作用、空值语义和同步生命周期掌握不完整。API名称、零delta、Start成功、若干轮长寿命弹体正常，都不足以证明模型闭合。验收应覆盖状态与生命周期的交叉条件，而不是不断增加同一场景轮数。Host权威与严格身份核对保留，补齐真实原因和所有权，不以增加动画/粒子特判解决。

23:41另一次女巫弹来源分叉仍单列：Host Root333/input7462，Client同类input7461、wire_root=0，发生于闪避与技能衔接后；证据`build/pound-exit-1108b/observed-2341/`。它涉及普通/缓冲命令的来源时刻，与本轮Root257已精确对齐后的空目标错误不同。本轮没有按±1容差放行，也没有声称该已知缺口已经修复。下一步须审计原版入队/接受/实际Start的来源对应，不能继续把Start发生帧直接等同于输入原因。

后续状态（2026-11-09）：已在协议17先受控复现并定位到额外接受机会与入队来源覆盖不足，协议18按[ADR0022](../adr/0022-buffered-command-source.md)修复；30组不同衔接/帧率的入队、接受、Start与Ability身份精确核对通过。详见接续记录，以上保留当时范围与证据。
