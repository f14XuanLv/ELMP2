# ELMP v2：命中音光的原版来源作用域（2026-11-05）

本页落实 ADR0009 §9，仅证明音光的当次原版来源；不开放 Client Effect Apply/Finalize，不以资源名或“当前正在技能里”猜测命中反馈。

## 已核入口

- [E-IDA，沿用 `enemy-sync-graph-v2` §4.1] manifest `GuardEffectApply` / `GuardEffectFinalize` 是现有权限闸的已校验 native 入口，ABI `void(self, from, to, const FHitInfos*)`。观察者只复用这两个入口；Host 注册观察者不得调用 `guard::arm()`，也不得顺带安装碰撞/保存/AI 等其它 guard 钩。Client armed 时在进入观察作用域前拒绝原版 Effect，因此没有“被拒绝的效果反馈”。
- [E-CUE，本轮实际查询] `/Game/_Zenith/Core/Pawns/BP_Character_Player_Base.BP_Character_Player_Base_C:BndEvt__Collision_K2Node_ComponentBoundEvent_5_CollisionEvent__DelegateSignature` 是 exact 接触事件函数，只有一个 8 B `ObjectProperty OverlapActor` 参数，无返回值。Kismet 0 写入 persistent frame，18 调 `ExecuteUbergraph_BP_Character_Player_Base(1593)`，33 return。完整 JSON 在 `build/toolchain-smoke/research-lily-feedback/contact-function.json`。本观察域围住这一次原版函数执行，不调用/广播它。
- [E-源码] UE4.26.2 `CoreUObject/Private/UObject/ScriptCore.cpp:1028` 的 `ProcessLocalScriptFunction` 直接取 `Function = Stack.Node`，并执行该帧字节码；`ProcessInternal` 的本地分支也传同一 Stack。UE4SS `deps/first/Unreal/src/Hooks/Hooks.cpp:154–161` 的 Pre/Post callback 传同一 `UObject* context, FFrame& stack`；故比较 `stack.Node()` 的 exact UFunction 指针是当前被执行函数，不是靠调用者名或字符串匹配。Pre/Post 只记录上下文，不更改 callback 的 original-call 决策。

## 运行契约

`coop::feedback_context` 仅在联机会话启用。Contact UFunction 和所属 PlayerBase 类型通过 ObjHandle 保存、每次 pointer 命中后核 weak serial；函数 count/类型/大小不符则不能启用。首次缺失不缓存为永久失败，后续 enable 可重试；已启用后由独立 1 Hz frame phase 检查失效并重新解析。初始化注册 phase，不能在 frame range-for 执行中首次 push phase。退出注销 Pre/Post callback 与 Effect observer，清除作用域及函数身份。

Effect 与 Contact 共用容量 64 的嵌套 LIFO 作用域。每层保存当次 self/from/to（或 contact Actor）的 ObjHandle，Contact 额外保存当前 FFrame 地址用于同一次调用的进出配对。原生 Effect detour 以 RAII 确保正常/异常退栈；pre/post mismatch、容量溢出或跨帧悬挂会首错 latch，供主循环结束会话。失效 weak target 不返回旧地址。只有最内层作用域能证明来源，不向外搜索可凑合匹配的目标。

- `sound_target(ctx)`：Effect 中只允许 `ctx == Effect.self` 或 ctx 的 UObject Outer 链抵达该次 `To`，返回该次活着的 To；不折叠为 Ability/Invoker/From。Contact 中只允许 ctx 的 Outer 链抵达当前 Contact Actor。
- `owns(source)`：Effect 允许 exact self，或 source 的 Outer 链抵达当前 To；Contact 允许 source 的 Outer 链抵达当前 Contact Actor。链最多 32 层；未知来源不被认作该反馈。
- `scoped()` 只表示处于上述 exact 来源作用域，不能单独充当 owner 匹配证明。`effect_scope()` 进一步区别当前层是不是原版 Effect Apply/Finalize。FX/Snd 调用者仍必须检查 owns / sound_target。
- 三态路由：明确 owns / sound_target 的输出才是反馈；Effect 内未知来源不能闭合（Client 整段拒绝该图），须显式失败；Contact 内不匹配的输出继续原版 Ability writer / 普通音光路径，不能因为外层 Contact 就标记/抑制或报告未知 Effect。真实 Contact → Knockback → Ability Stop 的嵌套可包含技能自身取消音光，外层 Contact 不证明这些输出都是接触反馈。

Client 原版接触回调仍执行；同来源的音光输出由 FX 层抑制，随后接受 Host 已证结果。普通走路、输入、原版 Ability 私有音光不在本作用域内，不纳入 own-seat 广播。Host反馈 Snd 使用 flags=3（feedback+exact transform），包含原始 quat / world translation / scale，经地图规范化；is2d 入口亦保留实际变换。FX bit5 是来源标记，bit4 initialState 保持独立。

## 验证边界

单文件编译只能证明接口/类型可用。真实验收需分别记录 effect Apply/Finalize 与 Contact 的 enter/exit、scope peak/depth、owned/unmapped、Host反馈捕获与 Client own-seat 重播，以及实际声音和粒子；必须确认普通输入/Ability原版输出没有被新增来源标记误拦。未知的异步输出、世界级特效以及不经过已核入口的 Boss 特例仍未覆盖。

2026-11-05：`feedback_context.cpp` 与改动后的 `guard.cpp` 已分别用工程 MSVC 参数独立编译，均 exit 0，日志为 `build/toolchain-smoke/research-lily-feedback/context-compile.log`。Fx/Snd 新字段的独立 codec 测试也已通过；不代表原版接触/效果作用域的游戏内调用计数已验收。
