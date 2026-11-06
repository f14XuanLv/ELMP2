# ELMP v2 开发工具链

2026-11-04：路径唯一来源是 `devconfig.json`，本机差异写 `devconfig.local.json`。所有 PowerShell 命令先执行 `[Console]::OutputEncoding = [System.Text.Encoding]::UTF8;`，Python 使用 `C:\Dev\miniconda\python.exe`。以下命令在仓库根目录执行。

## 开始工作

```powershell
& 'C:\Dev\miniconda\python.exe' tools\toolchain.py doctor
& 'C:\Dev\miniconda\python.exe' tools\ida_service.py start
& 'C:\Dev\miniconda\python.exe' tools\toolchain.py smoke --out build\toolchain-smoke\report.json
```

`doctor` 检查依赖；`smoke` 实际调用 CUE4Parse、IDA 和已运行实例的 UEBridge。失败返回非零，停止的游戏实例标记跳过。二者不启动游戏、不切图、不发送输入、不操作窗口。IDA 的 `start` 是单独的显式操作，启动现有 headless 服务并打开工作库；重复执行复用同一会话。

## 组件与用途

| 组件 | 入口 | 用途与边界 |
|---|---|---|
| MSVC 14.44、CMake、Ninja、Rust/Cargo | `cmd /c build.bat test` | Dev / Shipping / FrameProbe 构建及 7 项 CTest；局部 TMP/TEMP 固定到 `build/test-temp` |
| 打包 | `python tools/package.py` | 更新 `dist/ELMP-0.3.0-{dev,shipping}.zip`；Shipping 断言无 Lua/PDB |
| 部署 | `python tools/deploy.py <instance>` | 只操作配置内隔离实例；运行中拒绝替换 DLL |
| 实例 | `python tools/instance.py status` | 已配置的 dev / host / client / client2 |
| UEBridge 文件通道 | `python tools/bridge.py host elmp "elmp status"` | Dev 命令 / eval / console；无需原生 MCP 工具；同一实例必须串行 |
| 截图 | `python tools/capture.py client <name>` | 后台 PrintWindow；最小化或无效客户区明确跳过，不恢复窗口 |
| 帧时间 | `elmp perf` / ElmpFrameProbe | 当前运行的 ELMP 可直接读 perf；裸 UE4SS A/B 需停止指定隔离实例后单独部署探针 |
| CUE4Parse .NET 10 CLI | `python tools/catalog/extract.py --help` | 素材生成、触发器枚举、蓝图导出；无参数会生成正式 data |
| CUE4Parse MCP 0.2.0 | `python tools/mcp_client.py cue4parse …` | 26 个工具，资产 / DataTable / 对象字段 / Kismet 查询；每次进程自动挂载 paks/usmap |
| IDA Professional 9.3 / ida-pro-mcp 2.0.0 | `python tools/ida_service.py start` | 66 个工具，函数、反编译、xref；后台 HTTP 服务，数据库用副本 |
| UE / UE4SS / Zenith / 插件源码 | 见根目录 AGENTS.md 的来源表 | 引擎语义、API 与声明事实；重建 cpp 不是原版行为实现 |

表中的 `python` 都指配置中的指定解释器；IDA 服务内部使用其已安装的专用虚拟环境（包含 idapro 与 MCP SDK），不向主 Python 环境安装依赖。仓库 MCP 客户端仅用 Python 标准库。

## MCP 的两种入口

已将 `cue4parse`（stdio）和 `ida`（HTTP）注册到本机 Codex 配置，保留原配置备份。启动超时 30 s、调用超时 120 s。`codex mcp get cue4parse` / `codex mcp get ida` 可检查配置。配置形式依据 [Codex 官方 MCP 文档](https://developers.openai.com/codex/mcp/)。

注册配置不表示已经运行的聊天立即新增原生工具。以实际工具列表为准；当前聊天可以通过下面的命令行客户端立即调用同一批 MCP 工具。新聊天是否加载原生 MCP，应检查其工具清单，不凭配置文件推断。

```powershell
& 'C:\Dev\miniconda\python.exe' tools\mcp_client.py cue4parse list
& 'C:\Dev\miniconda\python.exe' tools\mcp_client.py cue4parse schema get_kismet_disassembly
& 'C:\Dev\miniconda\python.exe' tools\mcp_client.py ida list
& 'C:\Dev\miniconda\python.exe' tools\mcp_client.py ida call idb_list
```

参数优先放 UTF8 JSON 文件，用 `--args-file`，避免 PowerShell 的引号转换。`--out` 写完整响应，不把大反编译结果全部塞进对话。批处理是 JSON 数组，每项为 `{"name":"工具名","arguments":{…}}`；CUE 批处理共享同一个已挂载 provider。

例如，将以下对象写入 `build/toolchain-smoke/search.json`：

```json
{"query":"BP_Interactable_RestingPoint","limit":5}
```

```powershell
& 'C:\Dev\miniconda\python.exe' tools\mcp_client.py cue4parse call search_assets --args-file build\toolchain-smoke\search.json --out build\toolchain-smoke\search-result.json
```

CUE 原生 MCP 调用必须先 `init_provider`，使用配置中的 `paks` / `usmap`，`gameVersion="GAME_UE4_26"`、`readScriptData=true`、`caseInsensitive=true`。本仓库 CLI 已自动完成这一项；`--no-init` 仅供直接调 provider/session 管理工具。搜索只匹配资产路径；查具体字段、表行、字节码时先读对应 schema，并使用有界查询。

IDA 调用前先 `idb_list` 与 `server_health` 核对工作库身份。当前配置会话为 `elmp`，函数调用参数示例：

```json
{"database":"elmp","addr":"0x140CF3740"}
```

把它写入 `build/toolchain-smoke/decompile.json` 后：

```powershell
& 'C:\Dev\miniconda\python.exe' tools\mcp_client.py ida call decompile --args-file build\toolchain-smoke\decompile.json --out build\toolchain-smoke\decompiled.json
```

IDA 基址 `0x140000000`；示例 RVA 为 `0xCF3740`（AZenithCharacter::Tick）。服务在 `127.0.0.1:8745/mcp`，`status` 只读。工作副本位于配置的 `workDatabase`；原 `.i64` 保留。空闲一小时会话可能释放，重新 `ida_service.py start` 即可。服务不会安装为系统自启动；电脑重启后仍用同一命令启动。

本地 HTTP MCP（loopback 地址或 localhost）由仓库客户端强制直连；即使环境设置 HTTP_PROXY / HTTPS_PROXY 且没有 NO_PROXY，也不会把本地请求发给代理。IDA 服务退出后执行 `start` 会重新启动；初始化失败时先尝试绑定本地端口（Windows 独占绑定），若端口被占用或无法绑定则报告错误，不替换未知服务。

## 已修复的工具使用问题

- CTest 默认系统 TEMP 创建目录失败：构建脚本局部重定向，不改系统环境变量。
- Python 管道输出默认 GBK：仓库 CLI 在 `devconfig` 导入时设置自身 stdout/stderr UTF8。
- `extract.py --help` 误走全量生成：现在由 argparse 先校验；未知/缺值/互斥参数在启动 .NET 前失败。`--dump ASSET --out DIR` 输出调研 JSON，`--triggers 1` 仅枚举。
- 最小化截图产出空图：现在明确失败/跳过，并检查 Win64 GDI 句柄、PrintWindow 与 GetDIBits 返回值。
- 同版本旧 ZIP 容易误当新产物：本次已重新打包，仍应每次发布核对包内 DLL 与构建 SHA。

本次验证记录见 `docs/kb/toolchain-2026-11-04.md`。只读 smoke 不能代替战斗、跨图、权限闸门或性能 A/B 的专项验收。
