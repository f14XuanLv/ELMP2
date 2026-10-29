#pragma once

// P8 敌人域：复制“命令启动沿”，让客户端跑原版 CommandAction 管线（State / Ability / Move / FX / Sound
// 五条时间线），表现由模拟自己产生，而不是复制产物。
//
// 依据（docs/kb/pipeline-facts.md）：ZenithCharacter::Tick → TryAddCommands → UpdateCommands →
// ProcessCommand(0xCB4F00) 的启动沿（UCommand.CurrentResult == NONE）。Host 在这里取到
// CommandAction 类 / CommandInputType / 方向，Client 用原版 AddNewImmediateCommand 重放。
//
// 结算仍然由 guard 在汇点拦（OnApplyEffect / StartDeathProcess / SaveGame / AI 激活），
// 客户端敌人不跑 AI、不落伤害。

#include "engine/ue.h"

#include <cstdint>
#include <string>

namespace elmp::coop::commands {

struct Start {
    ue::UObject* owner = nullptr; // CommandQueueComponent 的 Actor
    ue::UObject* cls = nullptr;   // CommandAction 类
    std::string path;             // 类路径（缓存）
    std::uint8_t dir = 0;
    std::uint8_t cmd = 0;
};

bool host_enable(void (*sink)(const Start&));
void host_disable();

// Client: 在该敌人身上用原版 AddNewImmediateCommand 重放一条命令（false 时 *why 给原因）。
bool replay(ue::UObject* enemy, const std::string& cls_path, std::uint8_t dir, std::uint8_t cmd, std::string* why);

// Client 跑模拟时：抑制该 Pawn 由本地输入生成命令（TryAddCommands 0xCC2520 返回 -1），
// 命令只来自 Host 的 Cmd。P7-1 已证过入口，这里只做逐对象开关。
bool suppress(ue::UObject* enemy, bool on);
void clear_suppressed();

std::string describe();

} // namespace elmp::coop::commands
