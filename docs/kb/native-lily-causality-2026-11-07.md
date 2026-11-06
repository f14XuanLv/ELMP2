# ELMP v2：一招追踪技能的因果闭环（2026-11-07）

## 结论与范围

[E-实测] 原版 Witch s5040 的 AttackA 追踪弹已在真实 Host / Client 上完成：原版召唤发动→Host 实际选敌→Client 原版追踪→Host 命中音光→Host 实际完成后 Client 原版 Stop。没有复制弹体位置/旋转/Spine，也没有重新开放 Client 碰撞或结算。原版目标组件的真实位置、速度和时间上下文作为运动输入；Root / Query / Target / End 是事实，不是第二套技能实现。

这只证明这一招及列出的验证情形。不能推广为所有 Spirit / Companion / Boss / PvP 已闭合。独立 Companion AI 决策、Spirit 自身时钟在极端 Host 卡帧下的严格复现、多远端角色共享时钟、敌人的全部动画/死亡表现、长时弱网仍未验收。敌人姿态/外观通道沿用现有实现，运动时间已与原版 Lily 播放时间对齐。

前置知识：[追踪上下文](lily-homing-context-2026-11-04.md)、[命中反馈作用域](lily-feedback-source-context-2026-11-05.md)、[上轮卡顿修复](native-lily-stutter-2026-11-06.md)。本轮决定见 [ADR0011](../adr/0011-native-lily-causality.md)。

## 首处分叉与实际入口

[E-实测] `before` 中同一输入585、子路径`0:1`：Host 原版查询返回真实 Slime，Client 原始查询返回 null。Host 约169ms碰撞停止，Client 约583ms仍按无目标分支飞行。Host 根15 / Client 本地根9表明 UObject/本地激活号不能直接用作跨进程身份。目标是 Slime 的 `CollisionCylinder`，不能以 Actor 原点代替。

[E-IDA] 本轮工作库 `elmp` 再核前缀和反编译：

| 原版入口 RVA | 接缝用途 |
|---|---|
| `CF2F00` / `CF3090` | Spirit 绑定实际源 CommandAction、设置召唤上下文、OnAppear / 原版命令执行前登记来源 |
| `CDECE0` | GetClosestEnemyOfActor 的实际 Actor 返回值；Client 在此接收 Host 事实 |
| `CBD030` | 原版 Homing.SetTarget 的实际 SceneComponent 入口；核对组件，不绕过初始旋转分支 |
| `CC0BC0` | 原版 Homing Tick 读取目标组件位置/速度前提供共同时间上下文 |
| 已有 Effect Apply / Finalize | 命中音光的真实来源，沿 Effect.self 的 Outer 找已登记 Ability 激活 |
| 已有 Ability Stop / IsActive / IsLaunched | 原版请求原因与实际完成分别记录，池复用按精确激活处理 |

[E-SDK] ZenithSpirit 继承 ACharacter，与 ZenithCharacter 的 Tick 并非同一入口。只把“正在莉莉 Tick 内”当来源会排除真实的 Spirit 延迟发弹。修正为原版 Spirit Start 保存原始 CommandAction.Pawn 的输入序号和 Pawn 代次，稍后发弹仍沿用该身份，而不是读取那时的最新莉莉输入。Root ordinal 按原始输入分别累计，允许不同 Spirit 的延迟命令交错。

[E-实测] 命中也不总在 Homing Tick 的调用栈内；实际 Movement Tick 可在稍后触发碰撞。仅靠当前 Ability scope 曾使部分真实命中反馈的 causal_root 为0。最终使用最内层 Effect.self 的实际 Outer；`effect2` 记录为 `AbilityHitbox_s5040_AttackA_Projectile…AE_Damage`，Host FX 与声音都关联到根24。To 仍表示输出归属，不能拿受击者冒充源技能。

## 最终构建的真实样本

最终 Dev DLL SHA-256：`6961f96855c87426144ae07875edcfbc69c2c9069c9cdd9b3addef47752ac292`，协议11。两端必须使用同一协议。本地原始证据目录 `build/lily-causality/`，未把测试义肢或 Lua 带入发布包。

| 情形 | 实际结果 |
|---|---|
| `effect` 移动目标 | Host 非空 / Client 本地 null，恢复 Host 对应 Slime 与 CollisionCylinder；6对原版弹体阶段采样最大差0.04444 UU |
| `effect2` 近距离命中 | 共享输入831 / Host根24；4对采样最大差0.042 UU；两端原版 Stop 原因2，实际完成后无活跃技能 |
| `effect2` 反馈 | Host FX/Snd 时刻12680203；Client 两者实际播放共同时间12680204.075，晚1.075ms；同一根24。FMOD原生实例非空（累计3），这不是对扬声器听感的断言 |
| `effect-empty` | church_08 真实无敌人，Host 显式null，Client同样null；10对采样最大差0.107 UU；原版超时Stop原因1 |
| 反向发动（上一中间构建） | Host null但Client本地查询非空，最终按Host显式null走原版；不能以Client“找到一个敌人”覆盖Host事实 |
| map往返 | 最终构建 church_07→08→07→08，仍保持连接，旧地图代次关联清空；MapReady核对map+view |
| 本机 Client 输入（中间构建） | 后台 JUMP / 轴移动仍经既有输入→Host 路径；未出现会话故障 |

按原版 Homing 的每5次Tick阶段配对上述轨迹，不按两窗口相同墙钟时刻直接相减；本就保留约50ms播放缓冲。两端帧粒度仍可相差一次Tick，特别是首个目标样本。移动目标原生组件速度既观测到0，也观测到非零；实现传真实读数，不从位置差猜速度。

## 时序、权限与缓存审计

- 查询事实所在完整帧先封口，再发送对应 LilyFrame；缺记录与显式null区分。Root / 查询 / 子路径 / 目标实例代次冲突、未消费超时明确结束会话。
- epoch / map / view / Pawn generation / Host根 / Container path+occurrence分别处理会话、离图往返、角色替换和对象池复用。EnemyRow携带Host实例代次，不能误选同名新敌人。
- 原版 Lily 播放推进 Host 时间，敌人运动、精确目标上下文与相关FX/Snd按此时刻消费。被追踪目标不再被普通位置插值并行覆盖。Pawn HitStop delta与World delta分开。
- Client 本地早期 Stop 不决定命中；Host实际End到时经原版Stop完成。EndPlay/过图清理不阻塞。原Actor倍率在完成、复用、清理前恢复，避免污染对象池。
- 待播Fx收到Stop同样经过有界等待；已到时的旧特效在加载期间仍可清理。
- 活跃事实两秒后回收。Root≤1024、Node≤4096、Record≤8192、声明≤2048、目标样本每路径≤128；Spirit来源≤128，输入ordinal≤2048。原版Start覆盖Spirit来源，weak / Pawn代次 / Client地图重置或会话撤销（Host持续World中的来源由新Start覆盖）。状态见 `elmp causal`，有界1024条细查见 `elmp causal trace on|off|read`。
- 诊断 `roots=1` 或 `origins=1 / ordinals=1` 可表示池内Spirit保留的最后来源，并非活跃弹体；应同时看 `live` 和 `facts` 的 roots/nodes/records/samples。地图重置会清空Client来源；Host持续World中的弱来源可等待下一次Start覆盖。
- Client `guard armed=true`，damage/Effect Apply/Finalize/死亡/存档政策未放宽；无自主结算来掩盖表现差异。

## 性能与构建

`build.bat test` Dev / Shipping / FrameProbe与7/7 CTest通过；新增纯协议截断、非法参数、路径和容量边界检查。`tools/package.py`成功，Shipping包79项，零Lua/PDB。

中间构建9203a4…连续18次真实原版发动（空目标，非mock）的60秒FrameProbe：Host P99=17.055ms / max17.682ms；Client P99=16.852ms / max17.340ms。第1→18次Host私有内存约1011.8→1007.6MiB，Client约963.7→957.3MiB，句柄Host1271→1265、Client1267→1262。最后两端事实roots/nodes/records/samples均0，Client bound21、显式null19；当前缓存没有随次数持续积累的证据。短测不能排除长时或其它技能泄漏。

最终6961f968…构建60秒、各3600帧复测：Host P99=17.157ms / max17.657ms；Client P99=16.895ms / max17.292ms。Client原版Lily播放缓冲P99=50.226ms；ELMP主帧阶段平均74.681µs / P99=229µs（不是整个游戏帧，也不包含所有原生Hook成本，整帧以FrameProbe为准）。最终两端事实roots/nodes/declarations/records/samples均0；Root/Query各2次，Client非空覆盖1、显式null1，无fault；FX/Snd待重试队列0、fxBad/soundBad均0。原始证据 `release-bridge.json`、`frame-release-{host,client}.json`。

本轮开始整体备份41个隔离实例存档文件。最终停止两实例后按manifest核SHA-256，41/41一致，0本轮新增文件；正式目录与正式存档未触及。中途为重新建立活敌人场景曾还原5个测试变更文件，最终核对仍与最初备份一致。所有自动化只用后台UEBridge与ELMP命令，没有恢复/移动/激活窗口或发送前台输入。截图遵守最小化跳过。

## 复用的办法

先找两端第一次产生不同原版分支的位置；为该位置补齐Host的实际原因和对象身份，再让后续原版管线执行。Spirit Start、原版查询返回、Effect来源和实际完成状态分别收束不同阶段；不能指望一个大入口自动解决独立世界的全部上下文。下一招仍应按相同证据链验收，优先补共同缺失的原因，避免重新堆叠动画/弹体输出补丁。


## 留给用户的实例

最终同一6961f968… Dev构建已部署到配置内Host2 / Client2，FrameProbe关闭；Host PID7156、Client PID21292，Client已作为P2加入127.0.0.1:7777（房间码causes），church_08休息点相邻。虚拟输入/trace关闭；两端事实活跃与缓存项均0，native Lily无fault，Client结算闸仍armed。本来未最小化的两窗口后台截图`causal-ready-{host,client}.png`已目视确认双角色在场，未操作前台。截图只能证明静态就绪，丝滑程度及更广技能表现仍由用户游玩验收。
