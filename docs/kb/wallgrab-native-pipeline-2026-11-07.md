# ELMP v2：抓墙姿态与独立 Spirit 的上游缺口（2026-11-07）

## 结论及证据范围

用户报告 Client 观看 Host 莉莉时，莉莉抓墙姿态和狼形显灵均异常。前半记录是对游戏包、Shipping原生函数、0639996与当时HEAD e66359c的静态调查；用户随后授权持续修复，已在配置内Host2/Client2完成实现和真实回归。实施与新增的抓钩、死亡、复活躺卧、敌人镜像证据见本文后半。不能把初步假说与后续实测混成同一证据等级。

[E-CUE] 狼是独立 `BP_s5070_Killer` Actor，有独立 SpineSkeletonAnimationExComponent / SpineSkeletonRendererComponent 与 s5070_Killer-data / atlas / notify，不是莉莉同一骨架中的附加动画，也不是 `BP_Companion_s5070` 跟随同伴。新解析与 [player-sync-graph-v2 §6](player-sync-graph-v2.md) 既有证据一致。

## 原版收束链

[E-CUE] `BP_WallGrabComponent.WallGrabCommandActionClass = CAS_WallGrab_C`。该召唤命令指定 `SpiritClass = BP_s5070_Killer_C`、`SpiritCommandList = CA_s5070_WallGrab_C`、`bAttachSpiritToSummoner=true`、召唤局部偏移(50,0,-4)。莉莉 CAS 与 Spirit CA 都配置 `wall_grab_start → wall_grab_loop`；莉莉开始时间0.04秒，Spirit开始时间0.03秒，彼此独立。莉莉 CAS 的 StateTimeline 包含 IgnoreGravity / LockMove / WallGrab。State 是命令执行产生的上下文，不包含该命令及 Spirit 的全部生命周期。

[E-IDA] 本轮 elmp 工作库身份、imagebase0x140000000、Shipping input 已由 idb_list / server_health核对。IDA原先未运行，已通过仓库 ida_service启动工作副本，未修改原始i64。地址以下均为RVA：

- `CD36C0`：WallGrab组件 BeginPlay缓存Character/CommandQueue/State/Input，动态创建并注册以Pawn为Outer的Box，绑定OnComponentOverlapBegin/End。该Box属于远端Pawn的Primitive枚举范围。
- `CE6D90`：Begin处理检查资格/能力，发出组件虚表bool=true调用，并增加+296计数；`CE6F10` End递减计数，到0发出bool=false调用。虚表方法名本轮没有进一步核定，不把它臆定为某个具体API。原版此处有环境接触驱动的组件工作状态。
- `CF4D60`：WallGrab Tick检查运动模式、NoWallGrabStates、**当前没有StateWallGrab**、竖直速度、输入方向/朝向等，再做多次LineTrace与墙法线条件。D89DD0已核为StateWallGrab类取得入口，CE3380为该状态查询所用函数。
- `CF54A7`：成功分支调用C9E580清命令；`CF54C1`经C97C60按方向3/4、输入类型0、配置命令类加入命令队列。随后停止运动、设置原版抓墙锚点、D9A200发组件OnWallGrab、D9A1C0及Pawn+1592广播角色抓墙开始。
- `D9A200`只是对应OnWallGrab的ProcessEvent包装，调用它不能替代之前的环境判定、清队列、入队、停止移动与定位。重放接缝必须覆盖已核成功段所需上下文。
- `CE3830`的IsGrabingWall只查询StateWallGrab。因此该getter为true不能证明CAS_WallGrab真实发动、狼已生成或正确附着。

原版的高价值收束点是成功的环境动作进入原版命令队列；后面的召唤、双Spine动画、状态时间线、结束清理由原版拥有。它在输入之外还有环境接触与独立组件Tick这条原因入口。

## 0639996 内可直接定位的冲突

[E-代码/git] e66359c的`native/coop/native_lily.cpp` 220行对Pawn**所有**Primitive调用SetGenerateOverlapEvents(false)，包含抓墙探测Box；274–275行在原版Tick前直接对齐ActiveStates，包含Host的StateWallGrab。git show与git blame均确认这两段由0639996引入；e66359c仍保留。不能把这两个具体行为全归因于更早历史commit。

[E-源码] UE4.26.2 PrimitiveComponent.cpp的UpdateOverlapsImpl仅在GetGenerateOverlapEvents && IsQueryCollisionEnabled时处理新overlap，并要求双方GenerateOverlapEvents为true；SetGenerateOverlapEvents(false)改变该资格。保留CharacterMovement的世界阻挡不等于保留抓墙传感器的overlap。

已核两处结构性断点：

1. 原版环境探测/触发链被粗略屏蔽；只恢复角色Tick不会补回被禁用的输入来源。
2. 状态回填让副本看起来“已抓墙”，原版Tick却会跳过已有StateWallGrab的启动分支。ActiveStates相同不等于正在执行同一CommandAction。客户端可能缺少Lily Action动画、召唤命令和独立Spirit，两个表现可因同一上游未发动而一起异常。

尚未用同次双端native trace确定上述两处在用户那次动作中的先后与贡献，也未断言IsPlayer thunk、装备/能力、命令初始化等其他边必然无问题。资格检查中的具体虚表语义、WallGrab Box生命周期及Host/Client首次分叉应在下一轮原生审计中核实。

Host权威本身要求Host决定结算，不要求Client失去环境感知或无法执行表现命令。0639996已经恢复原版角色/装备/召唤/动画，方向仍成立；其远端感知屏蔽、命令拥有状态回填和环境原因覆盖还不完整。上游入口的数量或层级不能代替依赖与生命周期闭合。6d46183的追踪事实从Ability激活域切入，不能倒推补上根本没有进入CAS_WallGrab的命令链。

## 下一步：先修一条环境动作链，再扩大审计

1. 双端原生记录：墙Box事件与工作状态、WallGrab Tick成功段、CAS_WallGrab入队/开始/结束、Spirit Start、实际Summoner/Owner/AttachParent、两者Spine动画及相位，并与输入序号、Pawn/地图代次、Host时间关联。禁止只用IsGrabingWall或State列表当验收。
2. 对这一动作撤掉过宽的传感器屏蔽，并让已核原版命令拥有的WallGrab/LockMove/IgnoreGravity时间线从命令自然产生；这些状态不能在启动前被结果回填替代。不能未经分类就删除全部状态同步或开启全部世界触发器。
3. 若本地几何/查询时序仍不能重现Host，传递Host实际接受的墙接触/方向/锚点/开始结束及命令代次，在原版成功段/命令入口消费；继续原版召唤、双动画和清理。不得盲调OnWallGrab事件或手动spawn狼/复制动画来冒充完整链路。探测、运动/命令执行、结算三类职责分别审计，Host伤害/死亡/存档政策保持。
4. 验证抓住、保持、松手、墙跳/落地、左右墙、打断、进行中加入/重建及过图；两角色姿态、附着、相位与回收都通过，再审计其他依赖环境的动作。更大范围的结算拦截调整须逐个真实入口证明，不因“更激进”而整体放开。

原始本地证据：`build/wallgrab-analysis/`（CUE摘要、原生反编译/注册表/字节、IDA库身份、引擎与提交对照）；CUE当次session6e6a246a80fa。未作游戏行为mock或把重建cpp空实现当原版逻辑。

## ELMP v2 实施与双端回归（2026-11-07）

[E-实测] 先部署原e66359c构建复现：church_08墙面Host实际运行CAS_WallGrab，莉莉与BP_s5070_Killer均wall_grab_loop；Client虽然IsGrabingWall=true，却无实际命令、无狼，莉莉jump_apex，探测Box overlap=false、组件Tick=false。原始证据`build/wallgrab-fix/baseline-wall-high.json`。这证实了前述两处结构冲突的实际后果，不能再以State列表相同宣称同步成功。

### 实际删除与替换

- 删除远端Pawn全Primitive禁overlap。远端表现座位P51的travel/rest/story/world/pickup/interact/save全部关闭，AutoCollect Sphere保持off，原版地形/钩点/水体感知保留。guard的damage、Effect Apply/Finalize、DeathStart、save政策保持，摄像机仍不登记远端副本。
- 删除对命令拥有状态的无差别AddState/RemoveState回填。原生Start/End观察实际StateTimeline，按**精确Pawn**归属；Host和Client本地命令拥有的类均由原版命令维护，不把Spirit状态并进莉莉。协议12携带这份所有权，不按WallGrab状态名建白名单。
- 按原版Owner/Companion LeaderPawn归属传递独立Actor时钟，补静止朝向；网络播放间隔和预算用单调墙钟，Pawn delta / World delta分别保留，避免全局减速让Client快速耗尽输入。角色换代、归属改变、离图/离会恢复原倍率。
- 进行中加入有持续命令基线。只允许最后动画loop、零持续状态段、无MP消耗、无Ability/Move/FX/Sound时间线；召唤命令递归核其Spirit实际命令CDO，最多4层。通过原版ClearCommandBuffer/AddNewImmediateCommand重新进入完整召唤/动画管线，结束用RequestEnd，并核激活token，避免误停复用后的新命令。

### 抓钩的额外共同原因

[E-IDA] HookComponent BeginPlay C9AE30绑定Pawn.OnAptitudeUnlocked；CB11B0的overlap入口首先经过CAC050能力资格判断；CB3170原版OnUnlockAptitude重新采样已存在的Sphere overlaps，补HookPoints并启用组件。不是只需打开Sphere的GenerateOverlapEvents。

[E-实测] 重建副本已持有Hook能力，却出现HookPoints=0、Tick=false；直接调用原版OnUnlockAptitude立即得到1个钩点、Tick=true。改为profile成功后重发新Pawn原版能力监听通知，不改能力库存、不手写钩点，也不只调用HookComponent私有方法。最终使用原版委托内实际绑定的弱对象/函数名，复制端点后经有类型/参数数量验证的反射逐一调用；早期使用SDK内联分配器的多播辅助方法未闭合此行为，不能把“写了通知代码”当作已通知的证据。

[E-CUE] CAS_Hook召唤独立BP_s5060_Assassin/CA_s5060_Hook；莉莉wire、显灵hook_up。选定目标应读**行动上的HookPoint**，不能在发动后拿可能已经变化的HookComponent.GetCurrentHookPoint反证当时选择错误。

### 追加：死亡、复活躺卧、敌人朝向

[E-实测] Host死亡的实际动画数据在SpineAnimation逻辑DEATH层(6)，复活后未起身的rest_sleep_loop在EVENT层(9)，两者不在CommandQueue。此前Client自己HP已由Host变0但死亡图被拦截、远端HP至少1，而没有独立动画层上下文，所以分别漏掉death与rest_sleep_loop。

`lily_events`传原版层6–9实际当前定义、TrackEntry代次与相位，调用原版PlayAnimationDefinition/StopAnimation；清空Host完成委托、root motion、运动模式副作用，自己的预测P1也接收这些外部层。用原版TrackTimeScale匹配Host实际相位速率，并保留相位纠偏；不让纯相位回填反复倒拨全速死亡动画。Client的DeathComponent仍未进入结算流程。该域只恢复当前外部层，不声称复制任意剧情/交互对象的整个事件图。

[E-实测] 用户指认church_09大敌人朝向相反，后台截图与原生读数核实e2090 Priest8：Facing、Actor yaw、Scene相对变换、Skeleton整体倍率相同，**root骨局部ScaleX为1/-1相反**，最终骨矩阵镜像不同。只同步Facing或current tracks不足以重建残留骨架根状态。

`spine_root`为所有已登记Spine呈现对象传根骨ScaleX/Y，在原版Skeleton::updateWorldTransform收束点、动画apply之后写入，再由原版生成整棵世界矩阵；没有Priest类或动画名特例，不开启敌人决策/根运动。Native布局由本Shipping核实：Skeleton组件指针+224，bones begin/end+48/+56，根骨ScaleX/Y+108/+112。原版接口exec823890→7F0810→81C9D0，GetScaleX原生7E80A0、SetBonesToSetupPose→818870、根骨虚表update81A990交叉证明；SHA/前缀和指令位移检查已登记manifest。源插件解释语义，不能用重建插件Vector布局猜Shipping偏移。修复后双端根骨原生尺度一致、回调计数增长，map撤销恢复，failed=0。

### 真实验收结果与界限

| 场景 | 双端实际观察 |
|---|---|
| 普通左墙 | 双端CAS_WallGrab和CA_s5070_WallGrab均启动；莉莉/狼start→loop、正确Summoner与AttachParent，位置误差约0.04UU；稳定输入游标配对相位差约16.5ms |
| 右墙/松手 | 原版双角色正确朝向与附着，结束后命令active=0、无活跃狼；过图刚完成时缺过往开始的副本走持续基线，初始相位会从原版开始段重新进入 |
| 墙跳 | 双端同输入27575进入CA_JumpFromWall，随后同原版条件再抓墙；相位配对约32.6ms，非零误差不能说“逐帧完全一致” |
| 游泳 | 双端原版swim_forward、水中运动模式读数相同；先前jump_apex缺分支已消失；狼结束后回收 |
| 抓墙保持中重连 | Host已松方向键仍抓墙，重连Client自行恢复Lily/狼loop；随后松手回收。最终构建restored=2、active=0、fault空 |
| 抓钩常速 | 原版CAS_Hook / CA_s5060_Hook双端同输入559发动、相同钩点A2、正确附着/结束；Spirit游标配对相位差中位约0.38ms |
| 抓钩Host全局0.2倍 | 双端同输入16465发动，莉莉151个匹配采样相位差中位2.8ms，Spirit156个匹配采样中位0.31ms、最大12.19ms；无动画名分叉；不是拿两个工具请求的不同墙钟时刻直接相减 |
| Client自己死亡 | Host P2 HP=0/dead=true/death，Client本机HP=0、实际death动画；Client DeathComponent.dead=false，死亡结算未启动；Host复活后Client恢复idle |
| Host死亡/复活未起身 | 双端实际DEATH层death；Host新Pawn的EVENT层rest_sleep_loop在Client远端重建，未再停在idle；用户起身后层可撤销 |
| 敌人镜像 | 原生根骨局部尺度、世界骨变换与截图对照；修复前镜像持续相反，修复后读数一致；绑定/恢复计数正常、failed=0 |
| 追踪技能回归 | 最终构建Witch实际根/弹体与Homing Tick、显式null事实双端一致，Host/Client各2次launch/stop，facts清空、fault空；本轮该次没有取得非空目标，非空命中覆盖引用6d46183原验收，不能冒称重新完成 |

原始本地证据在`build/wallgrab-fix/`：`persistent-wall-left-bridge.json`、`root-wall-right-bridge.json`、`walljump-swim-final-bridge.json`、`final-rejoin.log`、`hook-protected-bridge.json`、`root-hook-slow-bridge.json`、`final-own-death.log`、`final-host-death.log`、`enemy-detail.log`(修复前)、`enemy-detail-fixed.log`、`ida-root*-result.json`、`final-homing-regression-*-trace.json`。trace是原生Start/End和EngineTickPost实际Spine采样，容量1024；未使用行为mock。诊断Lua只能在Dev义肢使用，发布实现是C++。

### 性能、权限、发布

最终Dev DLL SHA-256前缀`547d84beffb602a9`。Dev/Shipping/FrameProbe构建及7/7 CTest通过；增加的是协议容量/截断/NaN/外部层清除与根镜像边界检查。Shipping包79项，打包断言零Lua/PDB。

最终church_08稳定场景，FrameProbe两端各3600帧/60秒：Host P99=17.021ms、max19.055ms；Client P99=16.908ms、max21.493ms。另一次真实60秒采样Client P99=17.001ms、max20.293ms。不能把单次过图/后台调查脚本造成的帧停顿算成稳定窗口；整帧以独立探针JSON为准。

收尾elmp perf窗口：Client主帧阶段均值81.290µs/P99 214µs，native Lily Tick均值145.925µs/P99 357µs；二者范围不同，不冒充整个游戏成本。命令active=0；remote roleClocks=19为该玩家池内13种Spirit/6个Companion；地图清理Spine绑定归零，failed=0。短测没有缓存持续增长的证据，不等于所有技能与长时运行均无泄漏。

Client表现座位P51全部权限off、AutoCollect off；guard仍armed。纯动画层不调用死亡结算；原版敌人决策/碰撞/根运动关闭政策不变。最初备份41个隔离存档文件，最终停止两端后按manifest还原核SHA-256，再部署最终构建、关闭探针并重新连接；不触及正式目录/存档。测试使用后台UEBridge，不恢复/移动/激活窗口，截图仅在本来未最小化时采集。

## 方法论结论

0639996的方向有价值；本轮不是发现“入口还不够高”就再选一个更大的API。真正修正的是：**感知与结算分开、状态有来源/所有者、原版生命周期有代次、独立Actor共享正确时间、命令外的演出有独立上下文**。敌人目前仍是输出呈现域，因而必须完整表达所需骨架根状态，不能靠逻辑Facing自证。

存在统一的接入/审计原则，不存在已证明覆盖所有游戏行为的单一API。持续基线不重放过去的有限攻击，不伪造过去完成回调；晚加入持续动作仍会重新进入原版开始段，并非任意精确相位快照。复杂动态环境选择、全部Companion AI决定、全部剧情事件、弱网长断流和所有技能仍需各自真实验收。剩余这些边界不构成“架构整体错误”的证据，但继续粗略屏蔽感知、回填命令产物或忽略独立时钟，就是已被本轮反证的方法偏离。

## 本轮交付现场

最终同一547d84beffb602a9… Dev构建已部署Host2/Client2，FrameProbe关闭。重新启动后Host PID25624、Client PID23400，房间walls，Client=P2已连接127.0.0.1:7777，church_08休息点安全位置。Client native Lily profiles=1、queued=3、starved=0、fault空，外部层failed=0，guard armed=true。所有注入轴/按键与trace关闭。最终截图请求明确报告两窗口最小化，未恢复窗口，用状态命令作交付证据。正式目录/存档未动；最终重启前41/41备份SHA一致，恢复3个测试变更文件、无新增文件。
