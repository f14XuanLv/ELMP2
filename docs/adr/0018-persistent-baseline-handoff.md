# ADR0018：持续命令按激活代次接管（2026-11-08）

状态：已实施，短轮双端回归通过；长时间游玩仍待用户观察。不将本次错误与先前反馈超时混同。

[E-实测] 协议15、Host只有本地P1和网络P2时，Client于03:34:57记录`native persistent command did not start`，主动退房回标题；两个游戏进程均存活。此前从church_08回到church_07约2秒。旧错误没有具体命令或入队结果，因此不能据此认定是抓墙、蹲下或某一招。

现有接入把持续动作上下文当作逐帧存活约束：每个Host输入帧都比较当前命令，同一Host激活在Client已结束时也再次ClearCommandBuffer/AddNewImmediateCommand。Host采集接缝处于InputConsume后、原版CQ本帧更新前，这份上下文不是该帧更新后的存活承诺。重复恢复同一代次是额外的生命周期写入。

[E-实测] 本轮先尝试仅首帧恢复：冷加入时Host/Client均CAS_WallGrab、莉莉和独立狼均wall_grab_loop；但正常游玩新进入墙壁时，采样出现Host抓墙而Client无命令/无狼。该采样不能证明它永远不会自行进入，但该方案尚未通过连续环境动作回归，不能以消除退房为由假定该原因域可以删除。持续命令上下文保留，消费单位改为Host接受的新激活代次。

[E-IDA] AddNewImmediateCommand原生C97C60先入队；若CQ正在更新，仅设置待更新标记，否则同步调用UpdateCommands(0)。ProcessCommand(CB4F00)可在同一次更新中执行开始、更新、正常完成或覆盖。因此“调用返回时当前命令仍为同类IN_PROGRESS”不等价于“原版Start实际发生”。

决定：

- 持续上下文的每个新Host激活token最多接入一次。Client已自然开始且是新的同类本机激活时只建立对应；否则经原版入口开始一次。相同Host token的后续帧不因本机已经正常结束而重复恢复；新Pawn重置对应关系。
- Host代次消失/替换时，只对由本上下文明确恢复且仍为同一精确本机token的命令调用原版RequestEnd；自然输入启动的命令继续由原输入推进。保留环境接受结果的必要传递，删除无边界的逐帧重启。
- 显式接管仍仅允许ADR0012的无消耗、无副作用时间线、持续loop命令。原生Start接缝记录本次同步恢复中匹配Pawn/类的精确激活token；入队期间已经正常结束也属于已执行Start。未观测到Start仍fail closed，并打印目标类、Pawn、入队ID、当前类/结果及Start/End计数变化。
- 不提高超时，不按技能名字放行，不关掉Client结算拦截，不允许恢复任意有限攻击。原生延后执行的入队不冒充已开始；若接管接缝错误，应凭明确诊断继续上移/校准。
- 验收区分冷加入、Pawn重建、连续游玩、持续动作进入/松开、地图往返；确认相同Host代次不会重复增长restored，自然匹配不增加恢复，记录实际命令Start/End与退房原因。只使用用户要求的一个Host和一个Client进程，Host仅一个本地角色。

[E-实测] 最终构建732d48f5…双端同DLL：Host/Client均记录CA_Crouch Start 5次、CAS_WallGrab 2次、对应Spirit CA_s5070_WallGrab 2次。保持抓墙的3组采样中Client starts=7/ends=5/restored=1均不增长；冷加入后starts=9/ends=7/restored=2，松开后ends=9。正常与冷加入均实际执行莉莉/狼wall_grab_loop；church_08→07→08→07→08保持连接，唯一leave是测试脚本明确执行的中途离房。未做同时刻像素/相位相等的结论。原始记录见build/exit-p15/regression.json与两端regression-trace.json，测试义肢只经后台UEBridge，无新增游戏进程，结束已释放虚拟输入。
