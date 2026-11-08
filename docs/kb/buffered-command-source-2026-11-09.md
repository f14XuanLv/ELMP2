# 缓冲命令来源与离散执行边界（ELMP v2，2026-11-09）

决策见[ADR0022](../adr/0022-buffered-command-source.md)。本轮先在协议17复现，再加只读原生诊断复现，最后实施协议18；没有把输入编号容差、技能特判或取消严格核对作为修复。

## 两层问题与复现

[E-实测] 旧用户现场是女巫弹Host input7462、Client7461，见[上轮记录](null-target-and-spine-reentry-2026-11-09.md)。本轮原协议17 DLL用Host原版跳跃→9个引擎帧后闪避→6帧后SUMMON_1复现更强反例：Host没有启动女巫，Client却产生该技能并因`native query has no matching Host root activation`离会。因此不是把两边Start的编号改成相近值就能解决。

仅增加诊断、保持原行为的构建再次复现：两端同在input1108排入TTL=0.25的候选。Host在1122最后检查时，Dodge executionTime=0.3333479、TTL=0.0166525，接受谓词为false，随后扣TTL过期；Client在同一1122里额外检查一次，Dodge已经推进到0.3498895、TTL仍有0.0165977，谓词变true并真正Start。原Client把一个Host输入帧拆成2–3次Character.Tick，增加了Host没有执行过的接受机会。总delta近似相等，不能保证接受窗口与过期顺序相等。

另一层是来源覆盖不完整：此前只钩显式AddNewCommand，为派生后续动作保留父来源；普通输入和其它批量入口绕过它。将Start时的当前输入游标当作原因，会让正常缓冲后启动的动作丢失真正来源。两层必须分别修复，否则会留下“编号匹配但启动决定不同”或“决定相同但编号不同”。

原构建证据为`build/buffer-source-1109/baseline-old17.json`、`baseline-{host,client}.log`；仅加诊断的反例为`diagnostic/baseline.json`及双端日志。测试地图church_08无敌人，实际装备set2/SUMMON_1=s5040；每轮使用原版Pawn与ELMP虚拟输入，不直接调用Start、改TTL或强制接受。循环到第80个引擎帧释放输入。

## 原版完整路径

[E-IDA] 本轮确认工作库身份、原始Shipping输入及基址0x140000000，函数RVA如下；完整反编译与共同入口xref保存在本轮`ida-*.json`。

| 环节 | RVA | 实际语义 |
|---|---|---|
| Character.Tick | CF3740 | InputConsume→SetInputSnapshot→TryAddCommands→UpdateCommands→Locomotion |
| 普通输入匹配 | CC2520 / CC2840 | 递增本地FrameID；依输入和136字节CommandSettings直接创建UCommand并入队 |
| 显式/批量路径 | C97660 / C977C0 / CC2230 / CC25F0 | AddNewCommand、AddNewCommandGroup、TryAddCommandForInputs、TryAddCommandsForInputs都直达同一初始化器 |
| 共同初始化 | CA8870 | 分配或复用UCommand，reset后赋新CommandID并复制TTL/条件；调用方随后填动作实例和FrameID再发布到队列 |
| 可开始性探测 | CABA90 | 也调用CA8870，但临时检查后reset并还池，根本不入队 |
| 接受谓词 | CABB70 | 原动作检查、必要/禁止状态、能力与MovementMode；并非单凭按下就接受 |
| 处理/覆盖 | CC3970 / CC2BA0 / CB4F00 | InputUpdate、命令采样、覆盖窗口与接受检查；处理后才扣未开始命令TTL并回收 |
| 实际执行 | D24D70 / D24FB0 | 先调用原生Start，再设IN_PROGRESS；Sample推进执行时间与原版时间线 |

[E-SDK] CommandSettings.CommandActionType、UCommand.CommandAction/CommandID/FrameID等反射声明用于安全解析布局，不作为行为证明。FrameID只供本地同帧候选互斥；CommandID模10000且对象池复用，二者都不是跨进程输入编号。[E-IDA] GetCurrentCommand/CA8C10在Start接缝仍返回空的原因沿ADR0020结论：实际处理器已把命令移到队首，IN_PROGRESS尚未设置。

## 实施及推广范围

协议18让每个Host真实源帧完整执行一次原版Character.Tick，保留原始角色delta及CQ内部原版顺序。50ms连续播放预算、插值、组件/世界反馈时钟和0.9–1.1调速保留；源帧的后半段呈现不再制造额外原版命令检查。执行前仍等可靠封口覆盖该源帧，120帧上限不扩大。

来源采集上移至CA8870，以弱UCommand/队列/Pawn/预期类和新CommandID记候选；真正进入原队列时才绑定动作实例和FrameID。Start消费精确身份，普通输入保持入队来源，执行作用域内的派生动作保持父来源。池复用先撤销旧来源；队列移除、ID/实例改变、弱失效及地图/会话撤销均清理。未入队的探测候选在EngineTickPost清理，不伪造入队/Start。上限512，审计input admitted/started、derived queued/inherited、pending/pruned；pruned包含正常过期和探测回收，不等于错误计数。

共同初始化目标由已反射AddNewCommand原生入口的直接E8推导，普通入队诊断目标由TryAddCommandsFromSettings反射exec推导，均与清单地址/前缀和游戏哈希共同校验。私有接受谓词使用已确认清单。诊断只在显式trace时记录，不改变返回值。Root、代次、来源、类、路径及ordinal严格核对保持原状。

静态共同入口覆盖比逐个API补丁更完整；本轮并未为每个批量API人工强制Start来宣称动态覆盖。实机覆盖普通输入、缓冲失效/接受、召唤派生、下砸派生和持续命令接入；其余入口的覆盖依据是已核xref与共用初始化语义。

## 实机验证与部署

[E-实测] 最终版本30组女巫衔接：Host60/Client60为11组，Host60/Client30和Host60/Client120各6组，Host30/Client60为7组。对比270次实际入队诊断、441次接受谓词结果及顺序、123次Start的来源/当时游标/类，全部精确相等；42条Ability Launch的wire Root/input/class/path也精确相等，16次延迟Start仍保持旧入队来源。正常过期的候选两端均不启动。另3组Dog/SUMMON_2缓冲衔接在Client30FPS下命令Start精确一致。结果为`common-first/`、`matrix-h*-c*/`、`exact-audit.json`、`dog-buffer/`；初期两路径原型另11组不计入最终30组。

回归结果：

- `contact-regression/`：Client30FPS，女巫真实弹体出生点接触敌人，双端2条launch精确一致，有实际命中反馈，无额外Homing Tick，保持连接。
- `crow-regression-native/`：原地图带出生点的敌人作为目标，Host与Client自有乌鸦均实际发弹，双端20条launch精确对应，有非空追踪与持续Homing，保持连接。
- `combat-regression/`：Client30FPS，6次下砸各实际击杀3个敌人，共18个；双端12条launch精确对应，派生来源保持，持续连接。
- `wall-regression/`：蹲下释放、正常抓墙/狼形显灵、松开、中途加入抓墙及forest_03→forest_05→forest_03→church_08通过。换图后少量idle/idle_turn采样差异仍记录，不宣称所有视觉逐帧完美。
- `cold-forest/`：Client从标题冷加入Host所在forest_03，Spine嵌套保护Host5/Client13次实际触发，invalidNested均0，无栈溢出；最后回church_08。

构建Dev/Shipping/FrameProbe及7/7 CTest通过；最终双端Dev DLL SHA-256为`61f8aa084f9097687022e011ab1caf7cf3d8ecb68945730c655916599bd863b7`，协议18。Shipping DLL为`d05a2ff6aca21f3ef4642b22c543ffc67dc3c88aa952027591fab91a11bf02c9`；两zip内DLL哈希与构建一致、协议18，Shipping无Lua/PDB。

安静地图短采样451帧：Client帧间隔P99=16.708ms，原版Lily接缝耗时P99=281us，缓冲P99=51.021ms，组件倍率P99=1.070。只能说明该短静态场景没有缓冲持续增长，不代替战斗长测或用户双窗口观感验收。

只操作配置内Host/Client，最终Host PID36800、Client PID27336且已加入P2；Host仅本地P1，无P3/P4。部署前41文件新备份见`build/buffer-common-pair/`，本轮最初备份在`build/buffer-source-1109/saves-before/`。装备原内容保留，临时敌人清理/地图卸载，原版FullyRestore回满，虚拟输入释放，FPS恢复60，三种追踪均关闭。最后两端pending=0、active=0、starts=ends、fault为空，部署/产物/状态详见`common-deploy.log`与`final/`。

## 方法判断

问题属于接入原版管线时的系统性模型缺口：只复用入口但改变其调用边界，或只覆盖名字看起来合适的一个API，都不足以继承原版行为。本轮减少额外执行和不完整来源写者，再在共用初始化与真实发布/Start处维护最小来源关系。接受窗口、过期、覆盖与技能时间线继续由原版决定。

这不是“所有技能、所有世界状态已经确定性重演”的证明。环境、目标、受击、地图等仍须有明确权威上下文；后续遇到新分叉，应先比较哪个原生事件/条件首次不同，而不是继续放宽编号或按技能堆修补。当前严格核对在这里有效地暴露了实际错误，取消它会留下Host没有启动、Client却启动的错误表现。
