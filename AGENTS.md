# ELMP v2 开发规则（新仓库）

- 需求以 `docs/REQUIREMENTS-TREE.md` 为准，架构以 `docs/ARCHITECTURE-V2.md` 为准；新决定先改文档再改代码。
- Shell 命令前缀：`[Console]::OutputEncoding = [System.Text.Encoding]::UTF8;`；Python 用 `C:\Dev\miniconda\python.exe`。
- UE4SS API 以 `D:\CodeT\RE-UE4SS-32d8a381`（源码与 docs）为准；引用方式：submodule + `add_subdirectory` + 链接 `UE4SS` 目标（同 `D:\CodeT\Lilac2LilyMod`）。

## 与旧仓库 `D:\CodeT\ELMP` 的关系：拿走知识，不照搬代码
- 旧仓库的代码、`docs/REQUIREMENTS.md`、`docs/adr/`、`docs/kb/*.md`、`data/RVA.json` 是**知识来源**：原版入口、RVA / 偏移、实测结论、踩过的坑、协议语义。实现前先去读。
- **不复制旧仓库的代码文件**（含 C++、Lua、测试、mock）；每个模块按 v2 架构重新实现。可以复用的只有：原版事实、数值、RVA 与前缀、`data/` 下由工具从游戏包生成的素材（图标 / 字体 / 文本表，由新仓库的工具重新生成）。
- kb 的新实测写在新仓库 `docs/kb/`，带 ELMP v2 日期；需要引用旧 kb 结论时注明出处。

## Shipping 零 Lua
- 发布包 `Mods/ELMP/` 只有 `dlls/main.dll` + `data/` + `elmp.json`；打包脚本断言 zip 内无 `.lua`。
- Dev 构建可带义肢 Lua（经 UEBridge eval 加载）与自测脚本，但 ELMP 自身逻辑不依赖任何 Lua。
- 游戏线程每帧入口是 `RegisterEngineTickPreCallback`；不要用 `on_update`（它在 UE4SS 事件循环线程）。

## 验证以真实实例为主，不为测试而测试
- 功能验收 = 在隔离实例（Dev2 / Host2 / Client2，路径只写在 `devconfig.json`）上用义肢驱动 + 截图 + `elmp` 状态命令 + 帧时间，结论写进 kb。
- 只为“游戏里难以穷举输入”的纯逻辑写少量 CTest：编解码边界、传输状态机、握手、存储原子写、预检前缀。不要给巡检 / 座位 / 过图 / 复活 / HUD 等写 mock 测试，不建 FakeEngine，不迁移旧仓库的 Lua 测试。
- 写测试前先问：这条断言在真实实例里 5 分钟内能否验证？能就不写静态测试。

## 实例与部署
- 开发实测时**可以直接删除实例里旧仓库部署的 `ue4ss/Mods/ELMP`，部署新仓库的 ELMP**；三个实例都是隔离实例，不碰正式游戏目录与正式存档。
- 实例运行中不替换 DLL（先结束实例）；只操作 `devconfig.json` 内的实例。
- 用户可能正在操作 P1：未经确认不向用户的游戏发输入；自动化测试只在隔离实例做。

## 原生 Hook 纪律（不变）
- PolyHook2；Hook 前校验 EXE SHA-256 与函数前缀；能反射取地址不写死 RVA；不 Hook `IsPlayer` 本体（`0xD150F0`）。
- “某函数没被调用”必须有原生 detour 计数或 IDA xref 证据；UE4SS UFunction 钩子看不到原生直接 / 虚表调用。
- 任何缓存必须带失效源与审计方式（`docs/ARCHITECTURE-V2.md` §16.1）。

## 术语
- 非 P1 的权限 / 结算说法用“权限闸门 / 结算拦截 / 存档保护”，不用“防火墙”。
- Host 权威铁律：客人进程永不自行结算；Host 做不到的场景 fail closed。
