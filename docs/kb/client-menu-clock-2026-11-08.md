# ELMP v2：Client 管理器暂停造成回放队列溢出（2026-11-08）

对应 [ADR0016](../adr/0016-client-menu-clock.md)、F2.10 与 X-1。

## 事件

[E-实测] 用户给 Host 装狗后寻找 Client 换装入口，Client 再次退回标题。日志相对时间为：manager opened 02:00:07.289，leaving 02:00:09.223；原因为 `native Lily reliable input backlog: pawn=true build=true paused=true received=13295 consumed=13175 pending=0`。两序号差120，正好达到队列上限。Host收到主动离会，初查时进程仍在，无本次异常崩溃栈。

这不是前一轮的封口超时：本轮封口仍可到达，但本地世界被管理器暂停，原版角色Tick和消费停止，Host却继续发帧。约2秒积满120帧后，原有容量保护按设计拒绝；正常开菜单触发它则是上游暂停所有权的Bug。保留队列上限，不扩容、不丢弃输入/命令沿。

[E-实测] 同一 `984ade04…` 旧DLL在空闲 Dev2/Client3 打开 Client manager 3秒后同样离会；原用户日志和隔离状态保存在 `build/menu-live/`。

## 修复

- [E-源码] UE4.26 GameModeBase.AllowPausing 对 NM_Standalone 无条件允许，单改bPauseable无效。
- [E-IDA] 反射 GameplayStatics.SetGamePaused 的 exec RVA29DE9C0，+B2直接调用原生2543DB0；原生解析本地PC，调用虚表+A80。新增24字节前缀、EXE SHA预检与运行时反射目标交叉校验，在此汇点仅拒绝Client的true请求，false/单机/Host原样执行。已核范围是GameplayStatics原生/蓝图调用，不声称覆盖第三方直接写世界或另调PC暂停。
- Client ELMP菜单持有只屏蔽本机P1输入。已有InputConsume hook增加单一过滤回调，在原版消费后、观测/角色读取前置中性；网络上行也显式中性，不增加累计按钮沿。UI导航从独立原始键/手柄读取，远端原版Tick/时间线/动作沿继续运行。离会撤销过滤，原版菜单可见性缓存纳入Client活动源。
- Online新增Equipment按钮，复用原PX菜单/原版SpiritEquipComponent；客人的主体是本进程P1，非Host网络P2编号。P1通用profile.apply仍只读，客人只调EquipSpirit/UnequipSpirit并读回。已有EquipRequest/EquipResult继续让Host应用权威座位；遗物沿用现有原版入口。
- 客人菜单使用本机P1所属设备的固定导航，普通P1游戏输入保持原版。UI标明You及Slot编号，不把PX的默认游戏键冒充原版P1自定义键，并提示线上世界继续运行。默认休息点限制保留；Pawn替换/失效、离会、过图释放菜单及原始输入观察持有。

## 验证

- [E-实测] Client manager打开8秒，同时对隔离P1保持ATTACK+SUMMON_2和移动轴：实际InputSnapshot的raw/flags/x/y全0；上行attack累计沿0。远端仍连续消费，例：received1683/consumed1674/queued9（3个副本各约3帧），paused=false、fault为空，未积至120。关闭后轴0.25正常进入InputSnapshot。
- [E-实测] Client GameplayStatics.SetGamePaused(true)返回false，pauseDenied=1；inputBlocked=554时世界仍运行。Host manager打开3秒仍使Host paused=true，关闭后双端继续连接，原有Host/单机语义保留。
- [E-实测] 背景Dev键盘Right down/up使客人菜单从cell0/0到0/1，正常选择B组第3格、卸装/装s2082；两次装备变更均收到Host成功报告。P1通用profile.apply拒绝曾在首次接入中被观察到，最终没有放开该保护，而是使用精确装备入口。截图曾显示不在休息点的拒绝提示；后续仅隔离测试打开equipAnywhere，以验证换装流程。
- [E-实测] Host与Client各召唤狗：两进程都存在本机/远端两只活动s2082，归属不同且正确。测试延续原自主Spirit决定流、生命周期与原版召唤，没有狗名称白名单进入生产同步代码。本项证明接入与召唤，不等于狗所有目标/攻击/视觉细节已完美验收，仍需用户游玩比较。
- [E-实测] 在原本未最小化的隔离Client窗口做后台截图；通过 `elmp ui move/click` 依次点击Online及Equipment，管理器关闭并打开自己的菜单，B2读回s2082。未移动/恢复/激活窗口或系统光标。义肢与日志在`build/menu-live/`。
- Dev/Shipping/FrameProbe、7项既有CTest与Shipping零Lua/PDB打包通过。新增UI行为使用真实实例验证，未建mock。

## 交付

临时Dev2/Client3停止，38份存档/设置按本轮备份还原且哈希一致。部署时用户Host/Client已经停止，先备份其当前41份文件，未回滚到旧测试数据，再部署同一Dev DLL `5936584ed3c58cef43a646f5051d154a8e36091c6e98f27e7afedf7de7bde55d`，协议14、探针关闭。重连7777端口后，Host装备保留，Client的B组第3格装备狗；Host自己的P2档案读回同一技能，证据为`main-dogs-ready.json`。未在用户实例自动发动技能。

Client入口：右下ELMP→Online→Equipment；方向键选择，Z/Enter确认，Esc返回/关闭，A/F切页。世界在线继续运行，用户自行游玩验收。
