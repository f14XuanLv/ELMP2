# ELMP v2 工具链接入与验收（2026-11-04）

本轮先更新需求 F5.6、架构 §11.1，再实现命令行 MCP 客户端、IDA 后台服务入口、doctor/smoke，并修复已有脚本。使用说明见 `docs/TOOLCHAIN.md`。

## 实测结果

| 项目 | 证据与结果 |
|---|---|
| 构建 | `[E-实测]` `build.bat test` 无手工 TMP 设置，Dev / Shipping / FrameProbe 构建成功，CTest 7/7 通过；TMP/TEMP 仅测试进程指向 `build/test-temp` |
| 总检 | `[E-实测]` doctor 25 PASS / 0 FAIL；smoke 46 PASS / 0 FAIL / 2 SKIP（停止的 Dev2 / Client3）；完整输出 `build/toolchain-smoke/{doctor,smoke}.json` |
| CUE4Parse MCP | `[E-CUE]` 0.2.0.0，26 工具，挂载 17,384 文件，无加密归档和挂载告警；椅子包 27 exports / 113 imports，7 蓝图函数；OnInteract 4 条 Kismet，ubergraph entry 155、scriptComplete=true、diagnostics=[] |
| CUE CLI | `[E-CUE]` `extract.py --dump BP_Interactable_RestingPoint --out …` 退出 0，251,199 字节，27 exports，7 函数有 ScriptBytecode；修参数入口前后结果逐字节相同 |
| IDA | `[E-IDA]` IDA Professional 9.3 + ida-pro-mcp 包 2.0.0（initialize serverInfo 自报 1.0.0），stdio / HTTP 都可用，66 工具，工作库 imagebase=0x140000000、Hex-Rays ready；0x140CF3740 反编译 70 行、xref 4 条 |
| IDA 数据库 | `[E-实测]` 原 `.i64` 复制到配置的工作库后打开；当时两者完整 SHA256 均 `bb08dacc8e7fd76e764ddb9437c014e0da90a2656c80d9bdec5c58f9f5f63a49`。原库未写入；HTTP supervisor PID 29564 / worker 28436、会话 elmp 保持运行 |
| Codex 接入 | `[E-实测]` 本机配置已增加 cue4parse stdio 与 ida HTTP，30 s 启动 / 120 s 调用超时；`codex mcp get` 可见 enabled。直接读取这两段配置并调用 initialize/tools/list，分别返回 26/66 工具。当前聊天的原生工具清单没有热加载这两项，已通过仓库 CLI 实际调用，无需等待原生清单更新 |
| UEBridge | `[E-实测]` Host PID 6960 / Client PID 8720，ping、status、lan status、perf 均通过；联机保持，Client P2；同实例串行调用 |
| 截图 | `[E-实测]` Host 最小化时明确跳过；Client 后台截图 1280×720，目视正常；未恢复/激活窗口，未移动鼠标。文件为 Client Saved/ELMP/pilot/shots/toolchain-smoke-2026-11-04-client.png |
| 帧时间探针 | `[E-实测]` 独立 Dev2 标题界面，r.VSync 0、t.MaxFPS 60，预热 1 s / 采样 3 s：181 样本，mean 16.6678 ms、P99 16.824 ms；JSON `build/toolchain-smoke/frameprobe.json`。只证明探针调用和输出可用，不是游戏内 A/B 性能基准 |
| 打包 | `[E-实测]` 重新生成两个 0.3.0 ZIP，CRC 通过、包内 DLL 与构建一致，Shipping 无 .lua/.pdb；Dev SHA256 `dd267aefceddbb4a3ff8e9a1db46f1c286f63b1620a7d3d9fa5a10977bb9840c`，Shipping `20c2d328034a7dd1d7e0fba60efda894477fbcd0e939bb810d02adfc16932bb7` |

## 修复与可复现边界

- `[E-实测]` 系统 TEMP 导致 test_storage 在断言前 Access denied；现构建入口局部重定向，未改系统环境。
- `[E-实测]` Python stdout/stderr 原为 GBK，现仓库 CLI 自身设置 UTF8；中文、符号、emoji 和无 reconfigure 的流均已核对。
- `[E-实测]` extract 帮助/非法参数现在不启动 dotnet；19 组入口检查通过。无参数或仅 `--size` 仍全量生成正式素材。
- `[E-实测]` MCP list/schema/call/batch 均通过；错误工具、缺失资产、超时返回非零；非法 CLI / batch 参数被拒绝。CUE 子进程和读取线程在调用结束后退出。
- `[E-实测]` IDA `status → start → start` 均复用已有库和 worker，不启动 GUI。库身份在 idb_list、server_health 两处校验；完整临时副本原子发布，避免首次并发启动读取半截数据库。
- 现存 CUE 项目编译仍输出 NuGet 依赖告警和 DefaultFileProvider obsolete 告警，本轮未升级外部源码依赖。MCP 不保证解析器覆盖全部蓝图语义；遇到接口缺口应使用 CUE 本体和 IDA / 实机交叉核实。
- 源码资料路径已核对存在：UE4SS、UE 4.26.2、Zenith 重建工程、v1 ELMP、LilyInEM、Lilac2LilyMod。

测试辅助登记：为验证探针短暂部署、启动 Dev2（PID 28856），仅停留标题界面并调用上述 console 命令；采样后已停止 Dev2、关闭探针，保留当前 Dev 产物。Host / Client 保持联机；Client3 未启动。未读写原版测试存档、未向玩家输入。

本机 Codex 配置备份：`~/.codex/config.toml.elmp-backup-20261104-205152`。完整 JSON 证据在被忽略的 `build/toolchain-smoke/`，需要时由 Shell 读取；本文件保存跨会话的关键结论。

后续复核（22:20前后）：`doctor-final.json`为25通过/0失败；全部游戏停止时`smoke-tools-final.json`为38通过/0失败/4跳过（四个UEBridge因实例停止而跳过）。上表PID、DLL哈希和打包记录属于较早的工具链验收版本；后续同步修改、最新构建/包和最终进程状态以[同步修正实测记录](sync-correction-2026-11-04.md)为准。
