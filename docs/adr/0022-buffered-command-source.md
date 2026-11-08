# ADR0022：缓冲命令的入队、接受与执行边界（2026-11-09）

状态：已实施，原构建与仅加诊断的构建均先复现；协议18通过源帧原子执行和共同命令初始化器修复，实机回归结果见本轮kb。

[E-实测] 旧现场同一女巫技能Host input7462、Client7461。当前协议17受控复现跳跃→闪避→6个引擎帧后点按SUMMON_1，Host未启动该技能，Client在input390822启动并因无Host Root离会。因此不仅有Start时取最新游标的编号问题，还存在接受决定分叉，不能仅重命名Root或容差配对。

[E-IDA] Character.Tick CF3740按ConsumeInput→CQ.SetInputSnapshot CBCA20→TryAddCommands CC2520→UpdateCommands CC3970执行。普通玩家命令通过TryAddCommandsFromSettings CC2840直接创建/复用UCommand并入队，不经过AddNewCommand C97660；此前ADR0020只覆盖了派生动作的显式Add路径。FrameID在TryAddCommands内递增、供同帧同按钮候选互斥，并非网络输入编号；CommandID池复用且模10000，均不能直接作为跨进程身份。

[E-IDA] 入队不等于接受：CABB70检查冷却、状态、能力与MovementMode，CC2BA0选择并经CB4F00执行；CB4F00检查覆盖窗口后取消旧命令并递归执行新命令。UpdateCommands在处理/采样之后扣未开始命令TTL并移除过期项。相同总delta但不同调用分段可能改变“覆盖窗口已打开”和“TTL已过期”的先后关系。当前Client将一个Host输入帧拆成多次Character.Tick，这一离散语义差异需要实测核对，不能先假定时间和守恒就保证决定相同。

诊断先覆盖两条真实入队路径（CC2840和C97660）、原生接受谓词CABB70及实际Start，记录弱命令身份/ID/FrameID/TTL/输入游标/当前命令执行时间和谓词结果。只在显式trace时采集，保留原版返回值与行为；使用反射exec调用目标与前缀联合校验，不依赖看不见原生直接调用的UFunction钩子。缓存沿既有弱身份/队列/ID失效，无新跨帧裸指针。

[E-实测] 仅加诊断、不改行为的构建再次复现：同一输入1108两端均入队TTL=0.25。Host在1122最后一次检查时Dodge elapsed=0.33335/TTL=0.01665，谓词false，随后过期；Client对同一1122额外检查一次，elapsed=0.34989/TTL=0.01660，谓词true并启动。Client每个Host输入帧被拆成2–3次原版Tick，确实改变了离散的“先接受再扣TTL”顺序。FrameID两端不同是本地计数的正常结果，不能直接拿来匹配。

决定：原版Character/CQ的离散执行保持Host源帧原子性，每个真实输入帧只调用一次原版Character.Tick，使用完整原始delta；网络到达不均匀、插值和呈现时钟仍通过播放预算处理，不把一个输入帧拆成额外接受机会。实际Start消费精确UCommand的来源；派生动作保留父来源，普通输入保留入队来源，TTL过期/撤销不伪造Start。协议18表示该执行语义，精确Root/class/path/ordinal核对不放宽。

[E-IDA] 继续追到共同初始化器CA8870，发现还有AddNewCommandGroup(C977C0)、TryAddCommandForInputs(CC2230)、TryAddCommandsForInputs(CC25F0)和仅做可开始性探测的CABA90。最终来源采集上移到CA8870（分配/池复用并赋新CommandID），以反射CommandSettings.CommandActionType记下预期类；候选真正出现在队列时才绑定动作实例和本地FrameID。Start必须核对完整身份后消费；从未入队的探测临时项在原生调用栈已返回的EngineTickPost清理。普通/显式接口钩子保留为诊断，不再承担不完整的来源采集。原型的两路径接入已验证11组衔接保持连接，但不当作其它原生路线已经自动覆盖的证据。

共同来源表上限512，使用弱UCommand/queue/Pawn/预期类/动作句柄和本地ID/FrameID联合核对；池对象重新初始化先撤销旧来源。实际队列移除、ID或对象替换、弱句柄失效、forget/reset/换图均失效。未发布候选只保留到本轮EngineTickPost，不能把IsCommandSettingsStartable的临时对象当成入队或Start。`elmp commandctx`审计input admitted/started、derived queued/inherited、pending/pruned；显式trace记录入队与接受的TTL/当前动作时间，以及Start的来源input和当时游标start_cursor。父来源的因果ordinal租约沿用ADR0020。

播放仍使用50ms目标缓冲、0.9–1.1连续调速、120真实帧容量和每渲染帧一次插值纠偏。原版离散Tick在进入源帧时完整执行一次；该源帧后续的呈现时间消费继续推进组件/世界反馈时钟，而不再次调用Character/CQ。不因本地渲染帧暂时没有新原子Tick而冻结仍有预算的呈现时钟。完整源帧执行前要求可靠封口覆盖该帧时间范围，保持原有等待语义。此决定修正ADR0010中“同总delta分段等价”的假设，不回退到整数包数驱动的组件倍率。
