// 单机增强：在服务端拍入口直接读写客体（不加宿主门，产物与 prepared 包不变）。
// Single-player enhancements: read/write guest memory at the server tick entry (no new host gates; the build output and the prepared package stay unchanged).
#pragma once
#include <stdint.h>

// 每拍调用（diag.c 的两个拍入口），G = 拍入口第一个参数
// Called every tick (both tick entries in diag.c); G = the first argument of the tick entry
void dm_cheats_tick(uint64_t G);

// 「一键满血蓝耐」这类一次性动作：选项页点「应用」时由 dm_custom.c 通知，下一拍执行一次
// One-shot actions such as "fill life/mana/stamina": dm_custom.c notifies on Apply, and it runs once on the next tick
void dm_cheats_pulse(void);
