# ELMP v2：原版莉莉管线的卡顿修复（2026-11-06）

用户反馈：原版管线版本的表现完整性改善，但Client观看Host莉莉严重卡顿、延迟大。先提交效果基线 `9e885b6`（Restore native remote Lily execution pipeline），再修性能；保留原版装备、输入、命令、召唤及动画执行链，不恢复已删除的Spine/Spirit输出复制。

## 取证与原因

- `[E-实测]` 原版本短测：FrameProbe Client整帧P99=16.801ms；新增detour统计却显示远端组件倍率P99=4、max=8，位置纠偏P99=30.5UU。旧frame.elmp_us不包含原版角色detour，不能据它判整帧或动画平滑。普通到包数量直接变为组件0/1/4倍时钟，是独立于CPU帧时间的播放抖动。
- `[E-实测]` 只改连续时钟后仍失败：本机RTT约2ms，Host可靠消息累计重传持续增加，Client缓冲P99达1174ms；长测有切图，不能把此数据当同场景的严格性能A/B。只让时钟平滑而不解决消息突发，仍会缓慢追赶。
- `[E-源码]` 原网络线程每轮只recvfrom一个包，再ACK/发送；socket使用默认接收缓冲。本机另建UDP socket实读SO_RCVBUF=65536，最大世界快照128KiB可超过一次突发容量。改为1MiB缓冲、非阻塞socket、select最多等2ms，每轮有界接收256包后统一处理ACK/发送。
- `[E-实测]` 网络修正后的90秒普通地图复测：两端重传0，Client缓冲P99=56.355ms、组件倍率P99=1.003、位置误差P99=0.701UU，171/171按下沿已送原版执行，FIFO末尾3份。Client整帧P99=16.829ms。
- `[E-源码]` Actor.cpp的TickActor与Actor.h的组件ExecuteTickHelper都按owner.CustomTimeDilation推进；LevelTick.cpp:1352–1359还会缩放/限幅世界delta。GameplayStatics.cpp:2194的GetWorldDeltaSeconds返回UWorld::GetDeltaSeconds。UEngine::Tick入参不能充当本轮组件delta。Host捕获和Client调度改在角色Tick时读取该原版查询，装备准备仍在EngineTickPre。
- `[E-实测]` 切图曾触发旧“step不得大于1秒”的编码拒绝；已改为有界有限值检查并输出实际时长。战斗复测还见Host自身长帧被缓冲再次摊成等待：0重传时缓冲P99仍381.669ms。新增每seat最近31个世界帧的中位节奏，只将长帧的网络播放间隔限到中位数1.5倍，原版角色实际delta和输入完整保留。

## 实现与验证边界

协议10：LilyFrame分开记录角色实际delta与播放间隔；Client按连续时间消费，部分帧只在首次进入时传按下沿，原版Tick分段之和等于组件总推进时长。正常时钟速率缓慢纠偏到0.9–1.1；原版HitStop和真实Host长帧造成的特殊倍率保留。位置/速度在游标处插值，运动上下文每渲染帧纠偏一次；小误差渐进，大传送立即到位。

新增native_lily的Tick耗时、组件倍率、缓冲毫秒、位置UU误差、饥饿、暂停、长帧压缩、按下沿计数。elmp perf reset现在同时清这些直方图，perf正确显示其单位。SourceCadence的31项缓存按Host启停/Pawn serial失效，持续滑动更新；类引用及副本仍沿原有weak/地图/会话失效。

验证仅用配置内Host2/Client2，后台UEBridge和虚拟pad，无前台窗口操作。原始证据在忽略目录 `build/native-lily-perf/`：before / after / network / combat / final-combat的桥接返回及FrameProbe JSON。测试前重新备份用户本轮游玩后的41个存档/ELMP文件到该目录saves-before，未使用上一轮旧备份覆盖用户进度。短测的内存、句柄与live Ability/PSC计数用于排查持续增长，不以累计captured/retired数字上涨当作泄漏。

普通地图90秒后半段：Host私有内存966.57→966.03MiB，Client915.73→915.27MiB，句柄均下降；有敌人地图60秒Host1025.92→1017.29MiB、Client987.32→981.60MiB，live family/PSC有界。当前证据支持时钟/积压问题，未观察到该短测中持续内存泄漏。

## 最终实测与交付

`[E-实测]` 最终Dev DLL SHA256：`7e0e047d34373d7656145384fda3bf6b273b08c544ce63b06e41464869fdc211`，Host2/Client2一致。Dev/Shipping/FrameProbe构建及7/7 CTest通过，codec包括非有限/零step、长帧、全状态容量、所有截断；重新打包Shipping79项、零Lua，Dev84项。

`[E-实测]` final-combat：church_07的24个敌人，后台受控移动/跳跃/攻击/召唤90秒，另外主动让Host游戏线程停顿0.35秒两次，未操作窗口或系统输入。Client敌人taken=24、kills=1、aiLeaks/badSnaps/actBad/actAbsent=0；118/118输入按下沿送原版执行。Host反馈12条全部回放，其中2次原版HitStop。测试后stop/allow1 off，并还原召唤组。

| 指标 | 最终值 |
|---|---|
| Host / Client可靠重传、坏包、队列丢弃 | 均0 |
| Client播放缓冲 | P50=49.999ms，P95=50.364ms，P99=62.430ms，max=100.003ms |
| Client组件时钟倍率 | P99=1.003；max=17.081为保留Host真实长帧的推进，不能与普通包数造成的追帧混同 |
| 原版角色接缝总耗时 | mean=18.624us，P99=225us，max=1619us，含调度/上下文/原版调用 |
| Client位置误差（含受击与主动长帧） | P99=7.754UU；每帧小误差平滑修正 |
| Client整帧 | 5400样本，mean=16.668ms，P99=17.498ms，max=26.252ms |
| Host整帧 | P99=17.093ms，max=355.157ms含主动制造的停顿 |
| FIFO末尾 | received=6224、consumed=6221、queued=3、fault空 |

`[E-实测]` 两端私有内存末段分别为Host1030.21→1032.18MiB、Client974.56→978.45MiB，live family/member末尾均1/1，exactPSC=6/1，无持续单调资源积累证据。Host长帧压缩计数16包含启动/切图/主动停顿；饥饿计数包含Host本身停止出帧，不能等同于网络丢包。

`[E-实测]` church_08→church_07走原版出口，返回church_08调用原版WorldLoaderSubsystem.SwitchGameMap（登记的Dev测试辅助）；两端始终连接。返回后native收到15889、消费15885、队列4，双方retx=0，无编码拒绝/120帧积压故障，原始返回在roundtrip.json。

`[E-实测]` 实例停止后复核本轮备份41项SHA全部相同，没有需要还原的文件，也没有新生成的测试存档文件；save-audit-final.json保留结果。随后以同一DLL重新启动Host2/Client2并连接，供用户继续游玩。

本修复不宣称已经闭合ADR0010列出的所有Homing、晚加入活动基线、剧情及死亡等外部原因，也不替代用户双窗口游玩评价。
