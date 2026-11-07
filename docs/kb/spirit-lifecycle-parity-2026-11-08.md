# ELMP v2：本机与远端 Spirit 生命周期一致性（2026-11-08）

对应 F2.4/F2.5、[ADR0014](../adr/0014-native-presentation-handoff.md)。基线239a07e。

## 用户现场与原版事实

[E-实测] 只读 Host2/Client2：同一位客人召唤的 LV3 Crow，在 Host 正常，在 Client 全身橙红。两者 renderer.Color 都为(1,1,1,1)，没有活跃附着 ParticleSystemComponent。MID 的 SpiritFilterAlpha 在 Host 为0，Client约0.646333；Client 的 FadeIn 长0.5、位置0.100644、IsPlaying=true、Tick=false。这不是已证明的重复粒子。

[E-CUE] `/Game/_Zenith/Core/Pawns/BP_SpiritCharacterBase` 的完整 Ubergraph：

- +262 检查 Appeared，只有false才置true并 FadeIn.PlayFromStart。
- +437 的 FadeIn 更新通过原版 SpineSkeletonRenderer.SetScalarParameterValue 写 SpiritFilterAlpha=1−曲线值。
- 软退场路径 +1085 清 Appeared、Stop FadeIn，再启动 FadeOut。
- OnDisappear 路径 +1392 调父事件并 StopAllAnimations，没有前述软退场清理。

[E-IDA] CF7B40 是原版 SoftDisappear：state(+1464) 2→3，再发 OnSoftDisappear；CDD890 是 ForceDisappear：发 OnDisappear、清命令/状态/移动，最后 SetActive(false)。CEFAE0 的 SetActive 对所有组件 Tick 使用 on ? bStartWithTickEnabled : false。`IsActive` 读取 +1465 活动物理位，软退场时仍可为true。不能用“已强制隐藏”替代“已走完正常生命周期”。

[E-源码] UE4.26 Timeline 的 bStartWithTickEnabled=false，PlayFromStart→Activate 显式开启Tick。随后直接禁用组件不会自动清掉 TheTimeline 的 playing；下一次 BP 的 Appeared 检查仍可能跳过启动。插件头只用于声明，以上行为由 CUE、IDA 和引擎源码证明。

## 稳定复现与因果链

使用空闲 Dev2/Client3，原 Host2/Client2 未注入输入、未恢复/激活/移动窗口。Dev 的被动原生 SetActive detour 记录其前后 Timeline；不是用 UFunction 钩子是否被调用作推断。

[E-实测] 同时让 Host P1 与客人反复点按召唤12次：旧版 own Crow 持续冻在 FadeIn≈0.083–0.084，SpiritFilterAlpha≈0.699；remote Crow正常。一次完整记录为：

1. 客人本机预测的软退场先完成。
2. Host较旧的活动帧仍在缓冲，Client bind 发现本机已不满足活动状态，调用原版 Start 重新启动。
3. FadeIn播放约83ms后，Host退场帧到达，Client用 ForceDisappear关闭Actor。
4. FadeIn仍playing却Tick=false；Appeared没有经过正常软退场清理。下一次池复用只重置默认Tick，BP又因Appeared=true跳过FadeIn启动。

因此，239a07e 的攻击、跟随、原版发弹已统一，但生命周期仍有本机预测和 Host 恢复两个写者，并且误用了强制退休入口。这是可推广的接管缺陷，不是 Crow 颜色特例，也不是 Host 权威必然要求降低本机画质。

## 修复及代价

协议14为 SpiritFrame 加入 Host 软退场阶段。Client 自主 Spirit 的原版 Start/Commands、主动 Soft/Force 只允许 Host 回放作用域执行；本机莉莉的输入与动作预测继续。所有 Client 自主 Spirit 从同一 Host 接受流进入同一原版启动、命令和软退场链；池工厂对尚未活动对象的初始化仍正常。

Host在原生SoftDisappear接缝立即采集 decision-only 沿，之后连续帧保留阶段。Client调用原版SoftDisappear，让它清标记、停止淡入并播放淡出；最终退场帧完成绑定与载体退休。接受软退场后不能因本地先结束、缓冲中仍有活动帧而复活。没有 Crow 名称白名单，没有直接写 SpiritFilterAlpha=0，没有通用材质输出快照。

代价是自主召唤物的出现也等待Host确认/回放缓冲；莉莉自身的按键动作仍即时。这是生命周期唯一写者的选择，不宣称完全消除了网络时间差。

## 验证

- [E-实测] 同一12次反复召唤再次跑两轮：Host/Client、own/remote四类活动Crow最终SpiritFilterAlpha全部0，FadeIn均到0.5并停止，采样中playing=true且Tick=false的异常为0。最终构建第二轮14次绑定（初次2+6轮各2），12次软退场，未发生重复重绑；anchors14/12对应仍活动的两只，队列约每角色3帧，unexpectedStarts=0。
- [E-实测] Host两只Crow分别经原版 CommandQueue.AddNewImmediateCommand 接受 CA_s2102_Attack_LV3，Client匹配10个共同弹体根，source域/seat包含(1,1)/(2,1)。该检查验证接管后原版时间线与发弹仍执行，不等同于所有战斗场景验收。
- [E-实测] 两端跟随一次church_08→church_07，未断线；换图后两类Crow材质滤镜均0。窗口随后被最小化的截图明确跳过，未恢复窗口；使用状态/材质/Timeline输出作证据。
- Dev/Shipping/FrameProbe构建、7项CTest通过；协议新增位有round-trip验证，Shipping包零Lua/PDB。过程证据在忽略目录 `build/crow-visual/`，原版事实已在本文固化。
- 用户明确批准重启后，备份当前 Host2/Client2 的41文件，部署相同DLL（SHA-256 `bfe86d7d4207316e…`）、协议14、探针关闭，重新读档并连接。没有拿上一轮测试备份覆盖本轮用户进度。临时 Dev2/Client3 已停止，其38文件存档/设置按本轮备份逐项还原，哈希一致。

## 统一的含义与边界

所有 Client 莉莉应采用同一完整性验收约束，包括其 Spirit/Companion/技能结果。自己的输入预测与远端缓冲回放可以不同；同一对象的生命周期、材质/特效清理必须有明确的唯一写者及撤销条件。这次闭合的是自主 Spirit 生命周期，不能据此宣称所有普通技能的本机预测、任意剧情、极端瞬移和中途有限效果都已经统一验收。
