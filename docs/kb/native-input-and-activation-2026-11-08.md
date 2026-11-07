# ELMP v2：原版输入闭合与命令激活接管（2026-11-08）

## 本次退房

[E-实测] 一个Host本地P1、一个Client网络P2，Client于03:34:57主动退出会话回标题，错误为`native persistent command did not start`；进程没有崩溃。发生在church_08休息点返回church_07约2秒后。Host日志对应peer主动关闭并移除P2。原错误缺少类名，用户也未注意退出前动作，因此没有证据把这次失败归给特定技能，更不能与先前`authoritative feedback context timed out`合并作同一根因。

[E-IDA] C97C60的AddNewImmediateCommand可同步UpdateCommands(0)，更新中的入队则延后；CB4F00可在同次处理内开始并完成/覆盖命令。因此原版Start发生后，返回时没有同类IN_PROGRESS，并不代表从未开始。

[E-代码] 旧逻辑还把每帧Host的持续命令上下文当作本机持续存活约束：同一Host token若在本地已经结束，就重新清队列并入队。采集点位于InputConsume后、CQ更新前，这种逐帧存活判定不成立。

修复沿用原版Start/End接缝：同一Host激活最多显式恢复一次；已有新的自然激活则绑定它；原版Start实际接受的精确token作为恢复成功依据，已经同步完成不误报失败。Host结束仅对本流显式恢复、仍属于同一token的命令RequestEnd。未知开始仍拒绝，并打印目标类、Pawn、入队ID、当前类/结果与Start/End变化。没有放宽Client结算权限，也没有提高超时。

仅把恢复限制在首帧的尝试没有通过连续抓墙采样；最终保留Host环境接受结果，以激活代次消费，而非假定所有环境决定都可由输入自行重演。决策见[ADR0018](../adr/0018-persistent-baseline-handoff.md)。

## 狗的移动动作为什么漏

[E-实测] 旧构建Host狗实际Actor InputSnapshot.X=±1，而CQ.CurrentInputSnapshot.X=0；Client两者都为0，位置/朝向虽变化，部分时刻播放idle。

[E-IDA] Spirit原生Tick CF3C20依次包含Actor.Tick、维护、CQ.Update、InputConsume、Locomotion.Sample与寿命逻辑。CQ并非整段角色的入口；移动组件又独立消费Pawn的PendingMovementInputVector。单传CQ输入与Velocity不能覆盖它们。ForcedWalkMode也属于外部AI输入。

修复将帧推进从CQ局部循环移到整段原版Spirit.Tick，CQ只留执行权限门。协议15区分CQ输入、Actor InputSnapshot、Pawn物理输入，后者用原版ConsumeMovementInputVector/AddMovementInput恢复；原版Locomotion/CMC/Spine自行选动作，没有狗或run动画白名单。缓存/角色代次与生命周期纪律不变。见[ADR0017](../adr/0017-contact-and-spirit-state.md)。

[E-实测] 修复后的35组成对采样，Client的两个拥有者共70个狗样本：run 37、run_turn 33，idle 0。只补Actor输入时仍观察到物理无加速度、速度衰减与idle，最终另补Pawn输入才得到上述结果。此采样不等于全部召唤物、战斗及长时间播放已穷尽验收。

## 验证与当前现场

[E-实测] 最终DLL SHA-256：732d48f5d559c2418780c1540c03092002ab64b963154e1ffade9a61d4dc666f。两端相同DLL；握手除协议号还校验DLL哈希，不能仅更新Client。Dev/Shipping/FrameProbe构建与7项CTest通过。

- 5次蹲下：Host/Client原生Start各5次，结束后均无活动命令。
- 正常抓墙与抓墙中途加入：双方CAS_WallGrab各2次，对应Spirit CA_s5070_WallGrab各2次，莉莉与狼实际播放wall_grab_loop。
- 持续抓墙3组采样：Client starts=7/ends=5/restored=1保持不变；冷加入后只增加一次恢复至2，松开后starts=ends=9，没有逐帧再启动。
- church_08→07→08→07→08保持连接。测试中唯一leave为明确的中途离房命令；当前进程只有Host/Client，Host仅本地P1与网络P2，已释放后台虚拟输入与trace。

证据目录：build/pound-dog/（狗输入/Spine、协议15构建与首次反馈调查），build/exit-p15/（本次两端原始日志、IDA调查、构建、最终regression.json和原生命令trace），build/exit-p15-final/（最新备份与双端部署身份）。UEBridge同实例请求串行，没有操作前台窗口或系统输入。早期另组dev/client2测试存档38文件已还原并逐文件核对哈希；按用户要求关闭，仅保留Host/Client。

限制：本次03:34:57具体技能未知；先前反馈超时仍未得到充分闭环，新增的精确反馈诊断和过期前最终解析不等于已经证明该旧故障修复。白花破坏来源未确认，用户补充怪物箭矢、乌鸦魔法球等正常来源尚未排除，已降为低优先级待观察，不作为伤害范围异常或结算泄漏的证据。

2026-11-08后续：用户再次报告Host多目标下砸后退房，定位到环境后续命令误用最新输入导致根身份分叉；协议16改为真实父命令来源交接，并完成多目标实际击杀及反馈消费验证。详见[后续记录](bench-and-derived-actions-2026-11-08.md)。上述03:34:57持续命令错误与新的04:14:42反馈错误仍是两个不同故障，不将所有历史超时一概宣布修复。

## 方法上的共同教训

高收束入口不能替代输入与时序审计：同一角色可以有CQ、Actor、Pawn移动组件等不同消费点；一次原版Start也不等于下一次采样时仍存活。此前验证偏重召唤/攻击和位置，漏了巡航动作、松开边界以及持续命令正常结束。这是对原版职责与验收边界掌握不足，不能靠不断重启命令或补动画名称解决。

本轮保留必要的Host接受决定，删去同一激活的逐帧重试；补的是原版真实读取的通用上下文，不是逐项复制表现。后续审计应分别核对原因、接受代次、实际原版执行和清理，并覆盖两个拥有者、静止巡航、进入/保持/退出、冷加入和地图切换。
