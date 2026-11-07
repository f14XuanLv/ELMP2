# ELMP v2：Client 因等待封口主动离会（2026-11-08）

对应 [ADR0015](../adr/0015-causal-fence-wait.md)、F2.4/F2.5、X-1。

## 事件与结论

- [E-实测] 用户游玩后报告 Client 退出。01:39:45.889 的 Client 日志为 `coop: leaving (causal fence not ready: playback=97415450.392 fence=97415448 root=0 target=0:0 generation=0 domain=0)`，随后明确记录回标题；Host 接收 `peer closed: left the session`。检查时两端进程仍存活，没有这次事件的异常崩溃栈。
- [E-实测] 请求播放时间比最后封口多 2.392ms。root/target 零值在这里是无缺失 Query 时的占位，不能据此判断坏目标、source domain 或内存损坏。事后 Host 未暂停，也不能反推事发时的具体操作。
- [E-实测] 使用相同旧 DLL `bfe86d7d4207316e…` 在未使用的 Dev2/Client3 重现：先一次3秒暂停通过，再做反复暂停，第二次1.7秒暂停导致 `playback=97948383.333 fence=97948382`，同样主动退房。是否触发取决于最后帧边界，因此单次暂停通过不能排除。

这次属于真实的同步等待策略 Bug：保护规则把正常未就绪误判为致命故障，未证明收到的权威数据有错。

## 原因与修正

`host::flush_native` 只有新事实/角色帧/根/Spirit 帧存在时才发封口。`native_lily::schedule` 的最后帧还要消费真实 step，其播放末端可能晚于最后采样。源不再产生帧时，Client 会等到一个 Host 不再发送的后续封口。

`causality::ready` 将“未封口”和“目标未解析”混为同一等待，超过1秒直接写全局 fault。前一轮把封口时间移到采集结束，修正了 Tick 内最终帧比入口时间新的问题；它没有解决空闲不发封口及等待分类。

本轮对就绪 peer 无条件发送已有15字节空事实封口，仍在可靠有序通道、采集结束后取时。空封口不制造帧或 delta。Client 未封口时仅返回未就绪，队列与时钟沿用已有暂停机制；封口已覆盖但必要目标缺失时仍保留目标故障。真实断线继续由原传输8秒无响应/无进展规则处理。

新增 `fence: waits/aheadMax_ms/age_ms/targetWait_ms` 审计；数据随事实账本在地图/会话清理，不逐帧打印。协议仍为14，身份校验要求双端相同 DLL。

## 修复后验证

- [E-实测] 同组隔离实例连续10次1.7秒暂停/恢复，均保持连接。暂停时无输入队列残留，空封口持续到达。
- [E-实测] 另做5次约2～3秒 Host 游戏线程停顿：义肢仅在隔离 Host 的游戏线程以 `os.time` 有界等待，网络线程继续运行。中途封口接收年龄1300～1584ms，确实进入等待路径；累计178次未就绪，最大超前14.632ms，fault为空。恢复后封口年龄回到约0.1ms，播放时间继续前进，无离会。
- [E-实测] 两端P1各12次反复召唤/收回Crow，本机与远端四类活动Crow的SpiritFilterAlpha均0，无FadeIn仍playing而Tick关闭。Client 12次绑定/12次退役、12次SoftDisappear、载体12创建/12销毁，最终roles/queue/context均0、unexpectedStarts=0，空闲后仍连接。本项不替代任意敌人/追踪目标战斗的完整验收。
- Dev/Shipping/FrameProbe及7项既有CTest通过；Shipping打包无Lua/PDB。`git diff --check`通过。新增行为直接在实机验证，未添加游戏行为mock。
- 证据在忽略目录 `build/fence-exit/`，包括原事件日志、旧版复现、新版状态及GT停顿/乌鸦采样。仅Dev2/Client3接受测试输入；其38份存档/设置均已按本轮备份还原并核对哈希，两实例已停止。

## 部署

修复 Dev DLL SHA-256：`984ade04cde19bcbfd14bd89754d2f659693846af5d9a8fadb5163e00ca80895`。沿用用户此前批准的双端重启部署流程，停止 Host2/Client2 后备份当前最新41份存档/设置，再部署同一DLL、关闭探针并重新读档连接。双端preflight通过、协议14、7777端口连接正常；没有将之前测试备份恢复到用户当前进度。备份与连接结果分别在 `build/fence-exit/main-saves-before-deploy/manifest.json` 和 `build/fence-exit/main-connected.json`。
