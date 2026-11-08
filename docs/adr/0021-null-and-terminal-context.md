# ADR0021：空目标、短生命周期与姿态查询重入（2026-11-08）

状态：2026-11-09已实施并部署协议17；原构建受控复现失败、修复后相同场景及非空目标/双拥有者/冷加入回归通过。与ADR0020的落地派生来源、以及独立的缓冲技能输入来源错配分别记录，不合并为同一个已修故障。

[E-实测] 23:48:24，forest_03中Host女巫弹Root257、input13523与Client完全一致。Host子弹体938us内产生AE_Damage反馈并结束，Query结果为null，未执行Homing Tick；Client执行一次Homing Tick后报`native homing has no authoritative target context`主动退房。receive目前仅为component非零的Query种入Target样本，丢掉已知空目标。另须审计“开始/查询/结束同批发生”时消费顺序，不能假定每个实例都有持续Tick。

复现准备时，原构建Client冷加入forest_03又栈溢出，崩溃8B8E4E07已保存。[E-IDA] 7D0600/SpawnFX→7CEEE0枚举挂点缓存未命中→7CED20创建Follower并同步Tick→7E7570/GetBoneWorldTransform在applied无效时调用InternalTick(0,false,false)→7CF7D0/SpineSkeletonAnimationEx.InternalTick仍派发扩展Notify→同一通知再次SpawnFX。7CEEE0在创建返回之后才填缓存，7CF7D0在Notify返回之后才记录当前项，形成递归。SpinePlugin源码证明骨骼getter会触发零delta求值；扩展插件重建cpp不作行为证据。

决定：先解除受控复现的崩溃阻塞。在相同组件已经执行扩展求值的同步作用域中，嵌套零delta骨骼求值只调用原版基础Spine InternalTick，不再次遍历扩展Notify；正常外层扩展求值仍原版执行。边界在求值入口，不按PS/技能名丢SpawnFX。仅联机会话启用；原生地址/前缀与Ex CDO的vtable+1032及直接基类调用关系联合校验。作用域链只活在同步游戏线程栈，审计姿态查询降级与非法嵌套计数，无跨帧裸指针缓存。无会话原样通过。

目标上下文要区分未知、有目标、明确无目标；实例身份与生命周期属于每个Ability子节点，不能由一个Root上的标志替所有后代决定。结束事实与初始事实允许零持续样本。保留Host结算、精确来源和路径校验，不增加超时、不忽略缺失事实、不复制特效输出。具体实现以受控复现及原版停止语义审计决定。

[E-实测，2026-11-09] 仅加入Spine求值重入保护、保留原目标账本的构建，在forest_05先空射取得原版女巫弹出生坐标，再于该点生成敌对原版Slime：首次接触射击即复现，Host Root5/input2812，子节点0:1在748us内结束且Query=null，无Homing Tick；Client同Root/input，却多出一次Homing Tick并离会。证据`build/terminal-homing-1108/guard-baseline/`。两端Spine保护已武装，Host poseQueries=1/invalidNested=0。

实施决定：Query在明确无目标(ref_kind=0)或已有具体组件时种入初始目标样本；有Actor却尚无组件仍不冒充null。homing所有权从Root移至具体Node，各路径独立。Homing原生Tick前若本节点Host End已到期，则走原版Stop并跳过额外Tick；普通后置结束巡检保留。Stop仍经过精确activation/path/时间核对，未知上下文继续明确故障。新行为语义为协议17，新增空目标种子、终态Tick拦截与具体Root错误审计。

方法判断：复用原版执行链仍有价值，但仅接入Start/Tick或仅验证长寿命弹体不够。需要明确每个查询入口是否会触发事件、每个生命周期是否可能在一次原生调用内闭合；原版入口不保证只读、零delta也不保证无副作用。实测矩阵应按空/非空目标、零/非零寿命、冷/热挂点、首个/多个子节点、地图与两个拥有者组织。

[E-实测] 相同出生点接触场景4轮（Client60/30FPS）精确匹配Root，双方均无额外Homing Tick，反馈到达；原地图敌人的乌鸦回归中双方各10个弹体根，两个拥有者各5个，10次非空查询对应。forest_03冷加入通过，poseQueries Host6/Client13、invalidNested均0；抓墙/狼、中途加入与森林/教堂切图通过。构建7项CTest和Shipping包校验通过，详见[实测与边界](../kb/null-target-and-spine-reentry-2026-11-09.md)。
