# ADR 0005 — ELMP v2：纯 C++ 重写（Shipping 零 Lua）

- 状态：**已采纳，已实施**（2026-10-27 定稿；P0–P4 完成，P5 发布）
- 取代：v1 ADR 0001–0004 中“C++ 机制层 + Lua 逻辑层”的分工（v1 仓库 `D:\CodeT\ELMP` 只作知识来源）

## 背景
v1 的 Lua 逻辑层每帧经 UE4SS 反射调用，热路径（座位巡检、快照编解码、HUD）帧开销与 GC 抖动大；离线 mock 测试与真实游戏行为脱节（`SetCurrHP` 事件）；联机 Lua 表协议无类型、易被畸形输入击穿。

## 决定
1. Shipping 只含 `dlls/main.dll` + `data/` + `elmp.json`（+ SDK 头），打包断言零 `.lua` / `.pdb`。
2. 游戏线程单帧泵（`RegisterEngineTickPreCallback`），反射一次解析缓存、对象注册表增量维护、`ObjHandle` 弱引用。
3. 联机：CNG 握手 + AES-GCM、可靠有序 / 不可靠最新双通道、typed 位掩码增量快照；Host 权威，客人进程 guard 拦截结算 / 存档 / AI 激活，世界经六个实体适配器呈现。
4. 多地图：Host 用 `LoadLevelInstance` 在大 Y 偏移加载后台图，map_context detour 让清除按所属图记账。
5. 验证以真实隔离实例为主，CTest 只覆盖编解码 / 传输 / 握手等纯逻辑。
6. 第三方扩展：C ABI `ElmpApi_v1` + 导出 `ELMP_GetApi`；Lua Mod 注入 C 函数，不注入 Lua 源码。

## 与设计稿的实际偏差
- 实体适配器：enemy / lily 两半调用 host / client 中共享状态的函数（接管表、座位），而非完全独立的类。
- P1 进入后台图：实施为“清空后台实例 PlayerStart 标签 + 原版切图后移交再卸载”，而非严格的“先卸载再切图”（无需切图前拿到目标图）。
- 剧情 Boss 房识别用实例内 `BP_EnemySpawnPoint_Boss_C`（`DT_RecollectionBossData` 只列重温竞技场）。
- 客人端技能 Actor 2 s 寿命上限代替 Host 结束事件；敌人竖直视觉偏移未同步。
- OnOpenNewArea 实测原版不对 PX 的 PC 发出，未加抑制钩子。
- SDK Lua 事件用 `ELMP_PollEvents` 轮询（无 Lua 回调引用管理）。

## 结果
帧开销（ELMP 自身，60 FPS）：无座位 G0 `reflect_calls=0`；Host + 1 客人 P99 ≈ 0.45–0.49 ms；两客人 0.43 ms；后台图战斗 0.45 ms；Client 呈现 P99 0.18–0.30 ms（详见 `docs/kb/net-facts.md`）。
