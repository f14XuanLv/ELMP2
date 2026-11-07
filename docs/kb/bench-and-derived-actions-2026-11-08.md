# Client休息权限与下砸后续动作来源（ELMP v2，2026-11-08）

本轮对应用户报告：Client疑似单独过图往返后能坐椅子；Host对一群怪物下砸后Client又退出。两项是不同的系统性缺口。修复与业务方向分别见[ADR0019](../adr/0019-client-permission-lifetime.md)、[ADR0020](../adr/0020-environment-action-lineage.md)及需求树F1.9。

## 椅子是怎么绕过的

[E-实测] 修复前Client还在联机，却有本机rest_loop、StateEvent、StateInvincible、pc.bInEvent与视口中的WBP_RestMenu；Host自身与对应P2均idle。Client保存拦截计数3，只证明三次保存被拒绝，不能推导本地休息事件没有刷新世界。原始观察在`build/exit-p15/bench-observe.json`。

[E-代码] 原权限维护唯一调用在party::respawn::patrol，该phase要求本地party座位数大于0；Client的远端副本是core角色，通常没有party座位。因此Welcome时只应用一次，流送新交互物/换Pawn后不会继续维护。LoadMap还会清空记录而保留Client模式，同模式setter早退；门保持状态计算也遗漏Client模式。Client物理P1的allows(1)此前无条件放行，把Controller0误当Host权限主体。这些足以解释用户描述的绕过条件，但不能倒推用户当时准确的操作顺序。

修复把维护职责移至core独立phase，按Pawn/桶版本/权限模式失效重建，并修正上述模式判定。碰撞忽略不能撤销已经缓存的InteractingController，所以另在已校验的原生CE8BF0交互按下入口拒绝Client本地世界副作用；失败则不入座。网络输入仍送Host，不关闭整个EventManager，也不依赖事后保存拦截来保证完整休息流程安全。

[E-实测] `build/bench-pound-1108/bench-test.log`观察到Client独立处于church_07时P1忽略4/4目标，回church_08为7/7。最终构建复测`bench-final.log`中，独立切图请求被Host跟随逻辑很快纠回08，不能声称该次轮询也捕捉到了07；其后重新进入08仍7/7且维护计数持续增加。原生入口实测来自Dev `elmp gates probe-press`：先强制ActivateInteractable保留旧引用，再调用真实入口，localPressBlocked从0到1，无本地事件或休息菜单。后台V键消息未增加计数，未用它冒充原生路径证据。

Client INTERACT经网络仍把Host P2的HP从200恢复到400，Host P1保持400；Client原版SaveGames的文件哈希前后相同。这里只验证本轮覆盖路径，不声称所有可能的蓝图直接业务调用已经穷尽。

## 下砸退房的原因

[E-实测] 用户这次故障是04:14:42的主动离会，不是进程崩溃：enemy-death音效引用root295等待超时。Client播放已比该事件晚282ms，owner/worldReady均true而due=false，表明依赖的技能根没有对应，单纯增加等待时间无助于闭合身份。

原构建复现第三次下砸：初始动作两端同为input114163；落地动作Host变成input114205/root339，Client变成114204/本地root163且wire_root=0，随后报Host根未由原版激活消费。相同动作类与拥有者，来源却采用了各端发生落地时的最新输入帧。两端环境转换相差一帧，就会把同一次动作错认成两个原因。

[E-CUE] CAS_PileAttack.OnSampleCommandAction完成俯冲时，调用CQ.AddNewCommand排入Ground动作，父Sample返回后才启动子动作。[E-IDA] 原版Sample D24FB0、AddNewCommand C97660及其返回的UCommand ID确认这条链。GetCurrentCommand/CA8C10要求CurrentResult非NONE，而原生处理器D24D70在Start返回后才设置IN_PROGRESS；Start接缝须读取已选中队首，不能用getter的空返回否定该命令。

修复在原生Start/Sample/End期间携带稳定命令来源，同Pawn在该作用域派生的真实入队命令继承父来源，以弱UCommand+ID+队列/Pawn/动作联合身份交给后续Start。独立按键仍采用自己的新来源；未证实的转换不按“最近一次技能”或±1帧容差猜测。活动/待启动来源保持ordinal租约。Root声明与本机激活可按两种先后顺序精确绑定，反馈超时新增具体根诊断。协议16表达这次语义变化。

## 真实验证与边界

[E-实测] Dev/Shipping/FrameProbe构建及7/7 CTest通过；Shipping重新打包，断言无Lua/PDB。最终双端Dev DLL SHA-256为`5a0be4ae063fa5ddcc9aac856298dada74b1d1fdfcae6346b71bd3c21e522894`，两端均协议16。

- `head/`：连续6次下砸，12/12技能根精确一致；落地与初始动作共享各次独立来源，派生queued=6/inherited=6/pending=0/pruned=0。该轮生成怪物未实际受击，不能作为多目标命中证据。
- `combat/`：Client限制30FPS，6次下砸保持连接，但测试生成怪物仍未受击；该轮多目标断言明确失败，保留日志。
- `combat-faction/`：补齐测试生成敌人的Faction=2，Host60/Client30FPS，6轮每轮3个敌人实际死亡，共18个；Host捕获18份反馈FX与18份反馈音效，Client分别消费18份。两端12/12个技能根的wire ID、seat、generation、input、domain、class完全相等，无未绑定根、无离会。6次独立来源分别23823、24066、24420、24763、25073、25361，每次初始/落地共用该来源。
- `regression/`：5次蹲下、正常抓墙及狼形显灵、抓墙途中冷加入、松开退出、07→08→07→08均保持连接；冷加入双方莉莉与狼均wall_grab_loop。换图后个别样本Host idle/Client idle_turn，不将本轮通过解释为逐帧表现已经完美。

验证过程也暴露了本轮实现的两处错误：首次构建把ZenithCharacter组件偏移用于直接继承ACharacter的ZenithSpirit，Host访问违规；修正为类型分支取组件。随后getter在Start时为空导致待启动来源未消费，按上述原版生命周期改为核实队首。它们是本轮迭代错误，已修复，不能混同用户原来的主动退房。

证据位于`build/bench-pound-1108/`：原始双端日志、repro、head、combat、combat-faction、bench-final、regression及build/deploy日志；最终部署前的41文件备份位于`build/bench-pound-1108-final-deploy/`。没有操作前台窗口/鼠标，UEBridge同实例串行；仅使用Host和Client，Host一个本地P1与网络P2。测试怪物清理、虚拟输入释放，Client帧率恢复60。

## 业务与方法

允许客人坐姿不应等同于允许完整EVT_Rest。参考只读`D:\CodeT\LilyInEM\native\guest\g_body.cpp`的独立菜单创建、隐藏按钮+处理入口拦截+导航修正思路，ELMP按自己的Host权限逐项授权；没有复制旧实现或默认开放其强化/旅行规则。安全坐姿、受限菜单、Host中心及客人最近不重叠左右空位已写入需求树，尚未实现。

两个缺口都源自职责边界未闭合：会话权限不能由“是否有本地party”决定维护；环境派生动作不能用最新输入帧伪装新原因。原版高价值入口仍然有用，但必须同时明确角色、时序、来源和失效生命周期。本修复补的是这两种通用职责，没有增加技能名称/动画名称特判，也没有删除严格匹配。其他动作只有经过同一原版派生入队链才自动受益，不能声称所有技能已被穷尽验证。白花来源仍未确认，维持正常箭矢/乌鸦弹体等可能来源未排除、低优先级观察。
