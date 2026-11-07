# ADR0014：本机预测与远端原版呈现的接管一致性（2026-11-08）

状态：已实施并通过隔离实例回归，等待用户游玩复核。对应 F2.4/F2.5、X-1。

用户要求 Client 自己与远端玩家的呈现质量趋于一致。输入预测可采用不同时间策略；同一权威角色的材质、特效、生命周期与清理契约必须一致，不能把本机身份作为弱化呈现的理由。

[E-实测] 当前活动 Crow 的 Host SpiritFilterAlpha=0，Client own≈0.646，双方 renderer.Color=(1,1,1,1)，没有活跃附着 PSC。Client FadeIn 长度0.5、位置0.100644、IsPlaying=true、组件Tick=false；Host位置0.5、IsPlaying=false。整身橙红来源是未完成的原版材质滤镜，而非已证明的重复粒子。

[E-CUE] BP_SpiritCharacterBase 的 FadeIn 更新写 SpiritFilterAlpha=1−曲线值；OnAppear 中先检查 Appeared，只有未出现时才 PlayFromStart。[E-源码] UE4.26 Timeline.PlayFromStart 会 Activate 并开启 Tick。[E-IDA] Spirit SetActive(CEFAE0) 在激活时把所有组件 Tick 重置为各自 bStartWithTickEnabled；Timeline 默认该标志为false。需要追清已播放时间线在哪里被后续生命周期调用关闭。

先增加有界、按需 Dev 原生生命周期审计，记录 SetActive 前后运行中的 Timeline、调用点与控制流。测试使用空闲 Dev2/Client3，保留正在游玩的 Host2/Client2。修复应落在原版生命周期/接管条件，禁止按 Crow 名称或直接写 SpiritFilterAlpha=0 掩盖问题；确认边界后更新本决策。

[E-实测] 12次反复召唤稳定复现：own 正在软退场时被 Host 尚活动的缓冲帧 bind/Start 重新启动；约83ms后旧退场帧调用 ForceDisappear，FadeIn仍playing却被 SetActive(false)关闭Tick。[E-CUE] OnSoftDisappear 才清 Appeared/Stop FadeIn；ForceDisappear 的 OnDisappear 不经过这段。remote 未发生本机预测退场的竞争，因而更正常。

决定：所有 Client 自主 Spirit 的 Start/Commands/主动 Soft/Force 生命周期请求，只由本模块的 Host 回放作用域执行；本机莉莉的输入/动作预测保留。原版工厂对尚未活动的对象初始化仍执行。Host 原生 SoftDisappear 接缝立即产生可靠 decision-only 软退场沿，连续帧保留软退场阶段，使已绑定和中途绑定都接入同一个原版 SoftDisappear。最终失活仍结束绑定、恢复时钟并清理上下文。没有复用“强制关掉Actor”代替正常软退场。协议14新增该阶段位，不直接复制材质参数。
