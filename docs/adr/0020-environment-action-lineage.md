# ADR0020：环境后续动作的稳定因果来源（2026-11-08）

状态：已实施；双端精确根匹配、不同帧率及多目标实际击杀验证通过，持续动作与换图回归见kb。长时间游玩仍需继续观察。

[E-实测] 用户Host下砸攻击一群敌人后，Client退房：enemy-death音效root295等待超时；播放已比事件晚282ms，owner/worldReady均true而due=false。它与上轮持续命令错误不同。

同构建复现三次下砸：前两次两端Pile/PileAttackGroundLv3来源一致；第三次Host初始下砸input114163，Client也114163，但落地后Host为114205/Root339，Client为114204/本地Root163且wire_root=0，随后Host root未被原版激活消费而退房。同类、同拥有者、同三级动作，差异只有环境转换发生时的最新输入游标。因此不能按输入序号容差或忽略root修补。

原则：一次按键开始的动作，其原版自动后续动作需要由稳定的父命令/转换来源派生，不用落地时最新输入冒充新意图。只在证实的原版调用链建立继承，不能把一段时间内所有新动作都归到上一招。保留Host决定与结算、精确类/代次/路径匹配；能力事实声明和本机激活先后到达也需双向匹配，但不能用迟绑定掩盖本次不同输入来源。

核查命令End/OnEnd/自动入队与Spirit Start关系，用已有原生detour记录稳定父激活及实际转换；验收空地和多目标下砸、不同落地时序、连续独立攻击、其他可推广的自动转换，以及两个拥有者。未知转换不做宽泛“最近一次输入”回退。

[E-CUE] CAS_PileAttack.OnSampleCommandAction在SampleDivePhase判定完成后取GroundCommandSettings，经原版CQ.AddNewCommand排入落地动作，父动作返回后才开始子动作。[E-IDA] CommandAction.SampleCommandAction原生D24FB0（反射exec D4BCE0的vtable+632），AddNewCommand原生C97660（反射exec D4F890+1EF直接调用）返回真实UCommand的CommandID；UCommand持有动作实例。不是独立按键，也不是只在OnEnd里同步启动。

实施：在原版Start/Sample/End作用域暴露该命令的稳定来源；同Pawn队列在此作用域内派生的入队记录继承父来源。待启动引用以UCommand弱身份+CommandID+队列/Pawn/动作实例联合核对，Start时消费，独立外部入队清除该对象旧来源；失效/移出队列/ID替换/地图会话撤销清理，上限512并审计queued/inherited/pending。不按Pile技能名或±1帧容差匹配，不把普通输入采集游标改为历史值。因果ordinal保留期间也覆盖仍活动或排队的父来源。协议16表示此语义变化。

Root事实与本机激活允许两种到达次序：除激活时绑定已到事实，收到Root声明时也尝试精确绑定已存在的本机根。仍比较同一来源、类、Invoker与ordinal；不同输入来源不被迟绑定放宽。反馈失败补具体Root等待状态。

[E-IDA] CA8C10/GetCurrentCommand只在队首CurrentResult非NONE时返回对象；D24D70先调用动作Start，之后才设置IN_PROGRESS。因此在Start钩子内交接来源必须核对原生处理器已经排到队首的真实UCommand，而不是用该getter推断“尚未开始”。组件获取也必须核类型：ZenithSpirit直接继承ACharacter，不能套ZenithCharacter的组件偏移；同名成员不等于相同布局。本轮验证中暴露的这两处实现错误已分别修正，不混作用户原始故障。

[E-实测] 最终构建Host60/Client30FPS连续6轮各击杀3个敌人，两端12个技能根精确一致，Client消费18份反馈FX与18份反馈音效，无离会；正常/冷加入抓墙及四次换图回归通过。证据与限制见[kb](../kb/bench-and-derived-actions-2026-11-08.md)。

2026-11-09扩展：本ADR的显式Add接缝未覆盖普通输入及批量入队，按[ADR0022](0022-buffered-command-source.md)上移到共同UCommand初始化器，保留父来源继承与精确身份消费；同时修正源帧分段导致额外接受机会的独立问题。
