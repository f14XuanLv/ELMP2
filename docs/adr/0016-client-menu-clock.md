# ADR0016：客人菜单不拥有权威回放时钟（2026-11-08）

状态：已实施，隔离实例验证菜单连续消费、输入阻断/恢复、实际换装及双端狗召唤；已部署。见[本轮记录](../kb/client-menu-clock-2026-11-08.md)。

日志显示 Client 打开 ELMP manager 后 world paused，约2秒收满120个未消费 LilyFrame，主动离会。旧版在空闲 Dev2/Client3 同样复现。队列上限保护有效，菜单把客人世界暂停才是错误，不能靠扩容、丢弃动作沿或忽略 overflow 处理。

决定：

1. 单机/Host 保持原有菜单暂停；Client 的 ELMP 管理器/装备菜单仅占用本机游戏输入，不暂停呈现世界，不要求 Host 暂停。联机期间原版 Blueprint 的 GameplayStatics.SetGamePaused(true) 也在已核原生汇点拒绝，false 原样通过；不逐帧清写 PauserPlayerState。进入联机前由 ELMP 持有的暂停经原版解除。
2. 输入屏蔽作用于 Client 本机P1，复用已有 InputConsume 原生接缝，在原版消费之后、观测与角色读取之前将本帧置中性。远端角色不受影响。上行也显式发中性轴/按钮，累计沿不增加；UI 自己的键盘/手柄导航继续，固定菜单读本机P1所属设备，普通P1游戏输入仍由原版读取。关闭菜单恢复正常消费，不补发菜单期间的攻击。
3. Client 管理器 Online 提供 Equipment 按钮，复用现有装备菜单的本机P1分支（不是 Host 的网络P2槽）。使用本机装备/库存与原版装备入口；已有 EquipRequest/EquipResult 通道由 Host 应用其权威座位并回报，不创建第二套技能执行逻辑。P1 的通用 profile.apply 只读保护不放宽：客人选择仅调原版 EquipSpirit/UnequipSpirit 并读回，不允许以换装修改库存/等级/进度。仍保留休息点/装备规则与 Host 的装备校验，不在 Client 保存原版世界。
4. 暂停入口的 Hook 按 EXE SHA、函数前缀及反射exec的直接调用地址共同校验；输入回调在离会撤销。菜单输入屏蔽/暂停拒绝计数可审计。菜单/本机Pawn失效、切图和会话变化结束相关持有，禁止作用到新Pawn或远端角色。

证据：UE4.26 GameModeBase.AllowPausing 即使 bPauseable=false，在 NM_Standalone 仍允许暂停，不能直接照搬这个标志；Shipping SetGamePaused 为 exec 29DE9C0→原生2543DB0→本地PC虚表+A80，IDA核实。该入口覆盖已知 ELMP 和原版 Blueprint 菜单，不宣称包含任意第三方直接修改世界暂停的调用。

实测必须包含：Client管理器开超过原120帧窗口，回放持续消费且队列有界；原版暂停请求；关闭后恢复输入；Host菜单仍暂停；通过新入口装狗并收到Host确认、两端本机/远端召唤与回收。禁止前台操作，只在未使用实例喂测试输入。
