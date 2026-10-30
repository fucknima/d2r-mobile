// 单机增强（自定义页的 12 个键）：全部在服务端拍入口写客体数据，不加宿主门。
// Single-player enhancements (the 12 keys on the Custom page): everything writes guest data at the server tick entry, with no new host gates.
#include "dm.h"
#include "dm_custom.h"
#include "dm_cheats.h"
#include <mach/mach.h>
#include <stdatomic.h>

// ---- 属性号（itemstatcost 顺序，diag.c / dm_actions.c 已按同一组号读写）----
// ---- Stat numbers (itemstatcost order; diag.c / dm_actions.c already use the same ids) ----
enum {
    S_STR = 0, S_ENE, S_DEX, S_VIT,
    S_STATPTS, S_SKILLPTS, S_LIFE, S_MAXLIFE, S_MANA, S_MAXMANA, S_STAM, S_MAXSTAM,
    S_LEVEL, S_EXP, S_GOLD, S_STASHGOLD,
};
// 抗性：物理 36 / 魔法 37 / 火 39 / 电 41 / 冰 43 / 毒 45（diag.c 读怪物抗性同一组）
// Resistances: physical 36 / magic 37 / fire 39 / lightning 41 / cold 43 / poison 45 (the same ids diag.c reads for monsters)
static const int k_resist[] = { 36, 37, 39, 41, 43, 45 };
#define S_HPREGEN 74
#define S_DUR_CUR 72
#define S_DUR_MAX 73

// 数值上限（保守：这些数会进存档，别写成 0xFFFFFFFF）
// Caps (conservative: these values reach the save file; do not use 0xFFFFFFFF)
#define ATTR_CAP 500
#define POINTS_CAP 500
#define RESIST_CAP 90
#define REGEN_CAP 200
#define STASH_GOLD_CAP 2500000

// 倍率档位：选项页第 0..5 项 = 1× / 2× / 5× / 10× / 50× / 100×
// Multiplier steps: items 0..5 on the options page = 1x / 2x / 5x / 10x / 50x / 100x
#define MULT_N 6
static const int k_mult[MULT_N] = { 1, 2, 5, 10, 50, 100 };

// ---- 客体内存 ----
// ---- Guest memory ----
static uint8_t *main_base(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
static int rd(uint64_t a, void *buf, size_t n) {   // 同 diag.c 的 tk_rd：客体堆直接读，其余走 Mach | as tk_rd in diag.c: direct read inside the guest heap, Mach otherwise
    if (!a || !n) return 0;
    int dm_guest_heap_readable(uint64_t, size_t);
    if (dm_guest_heap_readable(a, n)) { memcpy(buf, (const void *)(uintptr_t)a, n); return 1; }
    vm_size_t got = 0;
    return !vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)buf, &got) && got == n;
}
static int sget(uint64_t u, int stat) {   // 原版取属性 main+0x5b9290（累计层，与角色页同口径） | the game's stat getter (accumulated layer)
    uint8_t *b = main_base(); if (!b || !u) return 0;
    return ((int (*)(uint64_t, int, int))(b + 0x5b9290))(u, stat, 0);
}
static void sset(uint64_t u, int stat, int v) {   // 原版设属性 main+0x5bc1f0（同 DM_TEST_*） | the game's stat setter
    uint8_t *b = main_base(); if (!b || !u) return;
    ((void (*)(uint64_t, int, int, int))(b + 0x5bc1f0))(u, stat, v, 0);
}
static void sset_if(uint64_t u, int stat, int v) {   // 已经等于就别再写：属性 setter 每写一次都要通知客户端 | do not write when it already equals: every write notifies the client
    if (sget(u, stat) != v) sset(u, stat, v);
}

// ---- 受控玩家（与 diag.c hud_tick 同一口径）----
// ---- Controlled player (same convention as hud_tick in diag.c) ----
static uint32_t local_player_id(void) {
    uint8_t *b = main_base(); if (!b) return 0xffffffffu;
    uint32_t slot = 8, lid = 0xffffffffu;
    if (rd((uint64_t)b + 0x2d20e4c, &slot, 4) && slot < 8) rd((uint64_t)b + 0x2d60380 + 4 * (uint64_t)slot, &lid, 4);
    return lid;
}
static uint64_t find_player(uint64_t G) {
    uint64_t u = 0; uint32_t lid = local_player_id();
    for (int b = 0; b < 128; b++) {
        if (!rd(G + 0x2230 + 8 * (uint64_t)b, &u, 8)) continue;
        for (int k = 0; u && k < 4096; k++) {
            uint32_t mode = 0, id = 0; uint64_t pp = 0; uint16_t c[4] = {0};
            if (rd(u + 0xC, &mode, 4) && mode && rd(u + 0x38, &pp, 8) && pp && rd(pp, c, 8) && (c[1] || c[3]) &&
                (lid == 0xffffffffu || (rd(u + 8, &id, 4) && id == lid))) return u;
            if (!rd(u + 0x158, &u, 8)) break;
        }
    }
    return 0;
}

// ---- 一次性动作（点「应用」时置位，下一拍做一次）----
// ---- One-shot action (set on Apply, done once on the next tick) ----
static _Atomic int g_pulse;
void dm_cheats_pulse(void) { atomic_store(&g_pulse, 1); }

// ---- 经验：只认「比上一拍多」的那一段 ----
// ---- Experience: only the increase over the previous tick counts ----
static uint32_t g_exp_last; static int g_exp_have; static uint64_t g_exp_pl;
static void xp_step(uint64_t pl, int mul) {
    uint32_t e = (uint32_t)sget(pl, S_EXP);
    if (pl != g_exp_pl) { g_exp_pl = pl; g_exp_have = 0; }
    if (mul > 1 && g_exp_have && e > g_exp_last) {
        uint32_t d = e - g_exp_last;
        if (d < 100000000u) { sset(pl, S_EXP, (int)(g_exp_last + d * (uint32_t)mul)); e = (uint32_t)sget(pl, S_EXP); }
    }
    g_exp_last = e; g_exp_have = 1;
}

// ---- 掉落倍率：改品质比例表（itemratio，结构与 dm_elysium.c 同）----
// ---- Drop multiplier: edits the quality ratio table (itemratio; same structure as dm_elysium.c) ----
#define RATIO_REC 0x44
static uint8_t *g_ratio_orig[4]; static uint64_t g_ratio_arr[4], g_ratio_n[4]; static int g_ratio_mul;
static uint64_t ratio_table(uint8_t *b, int v, uint64_t *n) {
    uint64_t t = *(uint64_t *)(b + 0x2d64a18 + (uint64_t)v * 16);
    if (!t) { *n = 0; return 0; }
    *n = *(uint64_t *)(t + 0x16c8);
    return *(uint64_t *)(t + 0x16c0);
}
static void ratio_restore(uint8_t *b) {
    for (int v = 1; v <= 3; v++) {
        uint64_t n, a = ratio_table(b, v, &n);
        if (a && g_ratio_orig[v] && a == g_ratio_arr[v] && n == g_ratio_n[v]) memcpy((void *)(uintptr_t)a, g_ratio_orig[v], n * RATIO_REC);
    }
    g_ratio_mul = 1;
}
static void ratio_apply(uint8_t *b, int mul) {
    if (mul <= 1 || g_ratio_mul == mul) return;
    ratio_restore(b);
    static const int group[4] = { 0, 3, 6, 9 };   // 暗金 / 稀有 / 套装 / 魔法，每组 {基数, 除数, 下限} | unique / rare / set / magic, each {base, divisor, minimum}
    for (int v = 1; v <= 3; v++) {
        uint64_t n, a = ratio_table(b, v, &n);
        if (!a || !n) continue;
        if (!g_ratio_orig[v]) {
            g_ratio_orig[v] = malloc(n * RATIO_REC); if (!g_ratio_orig[v]) return;
            memcpy(g_ratio_orig[v], (void *)(uintptr_t)a, n * RATIO_REC); g_ratio_arr[v] = a; g_ratio_n[v] = n;
        } else if (a != g_ratio_arr[v] || n != g_ratio_n[v]) { free(g_ratio_orig[v]); g_ratio_orig[v] = NULL;   // 表换了（重新装载）：按新的重拍一次 | table replaced (reloaded): take a fresh snapshot
            g_ratio_orig[v] = malloc(n * RATIO_REC); if (!g_ratio_orig[v]) return;
            memcpy(g_ratio_orig[v], (void *)(uintptr_t)a, n * RATIO_REC); g_ratio_arr[v] = a; g_ratio_n[v] = n; }
        for (uint64_t i = 0; i < n; i++) {
            int32_t *o = (int32_t *)(g_ratio_orig[v] + i * RATIO_REC), *d = (int32_t *)(uintptr_t)(a + i * RATIO_REC);
            for (int g = 0; g < 4; g++) { int x = group[g]; d[x] = o[x] / mul; d[x + 1] = o[x + 1] * mul; d[x + 2] = o[x + 2] / mul; }
        }
    }
    g_ratio_mul = mul;
    LOG("增强：掉落倍率 ×%d（品质比例表已改，退局还原）", mul);
}

static int mult_index(const char *key) { int i = dm_custom_get(key); return i >= 0 && i < MULT_N ? i : 0; }
static void fill_vitals(uint64_t pl) {   // 满血蓝耐：读上限写当前 | fill life/mana/stamina: read the maxima, write the current values
    static const int cur[] = { S_LIFE, S_MANA, S_STAM }, max[] = { S_MAXLIFE, S_MAXMANA, S_MAXSTAM };
    for (int i = 0; i < 3; i++) { int m = sget(pl, max[i]); if (m > 0 && sget(pl, cur[i]) != m) sset(pl, cur[i], m); }
}

// ---- 每拍 ----
// ---- Per tick ----
void dm_cheats_tick(uint64_t G) {
    if (!G || !main_base()) return;
    uint64_t dm_cheats_player(void);
    uint64_t pl = dm_cheats_player();          // hud_tick 这一拍刚找到的那个（省一次全表扫描） | the one hud_tick found this tick (saves a full table scan)
    if (!pl) pl = find_player(G);              // 还没找到过（第一拍）：自己扫 | never found yet (first tick): scan here
    if (!pl) { if (g_ratio_mul > 1) ratio_restore(main_base()); g_exp_have = 0; return; }   // 不在局内 | not in a run
    int god = dm_custom_get("god_mode"), heal = dm_custom_get("heal_full"), attr = dm_custom_get("attr_max");
    int sp = dm_custom_get("stat_points"), kp = dm_custom_get("skill_points"), res = dm_custom_get("resist_max");
    int reg = dm_custom_get("regen_fast"), dur = dm_custom_get("durability"), gold = dm_custom_get("gold");
    int xpm = k_mult[mult_index("xp_mult")], dpm = k_mult[mult_index("drop_mult")];
    if (!(god | heal | attr | sp | kp | res | reg | dur | gold) && xpm <= 1 && dpm <= 1 && !atomic_load(&g_pulse)) {   // 全关（也没有待做的一次性动作）：不动 | everything off (and no one-shot pending)
        if (g_ratio_mul > 1) ratio_restore(main_base());
        g_exp_have = 0;
        return;
    }
    static uint32_t t_slow, t_dur;
    uint32_t tq = 0; rd(G + 0x170, &tq, 4);
    int slow = (tq - t_slow) >= 25, durs = (tq - t_dur) >= 60;
    if (slow) t_slow = tq;
    if (durs) t_dur = tq;

    if (atomic_exchange(&g_pulse, 0) || god || heal) fill_vitals(pl);   // 一键满 + 持续补满 | one-shot fill + continuous refill
    if (attr && slow) { static const int a[] = { S_STR, S_ENE, S_DEX, S_VIT }; for (int i = 0; i < 4; i++) sset_if(pl, a[i], ATTR_CAP); }
    if (sp && slow) sset_if(pl, S_STATPTS, POINTS_CAP);
    if (kp && slow) sset_if(pl, S_SKILLPTS, POINTS_CAP);
    if (res && slow) for (unsigned i = 0; i < sizeof k_resist / sizeof *k_resist; i++) sset_if(pl, k_resist[i], RESIST_CAP);
    if (reg && slow) sset_if(pl, S_HPREGEN, REGEN_CAP);
    if (gold && slow) { int lv = sget(pl, S_LEVEL); if (lv > 0 && lv < 1000) sset_if(pl, S_GOLD, lv * 10000); sset_if(pl, S_STASHGOLD, STASH_GOLD_CAP); }
    if (dur && durs) {   // 已装备物品（mode 1）耐久写回上限 | worn items (mode 1): durability back to maximum
        uint64_t inv = 0, u = 0;
        if (rd(pl + 0x90, &inv, 8) && inv && rd(inv + 0x10, &u, 8)) {
            for (int k = 0; u && k < 512; k++) {
                uint32_t mode = 0; uint64_t d = 0, next = 0;
                if (!rd(u + 0xC, &mode, 4) || !rd(u + 0x10, &d, 8)) break;
                if (mode == 1 && d) { int mx = sget(u, S_DUR_MAX); if (mx > 0 && sget(u, S_DUR_CUR) != mx) sset(u, S_DUR_CUR, mx); }
                if (!rd(d + 0xB0, &next, 8)) break;
                u = next;
            }
        }
    }
    xp_step(pl, xpm);
    if (dpm > 1) {   // 极乐世界角色自己会改同一张表，两边算法不同，这里让开 | a Elysium character changes the same tables; do not stack both
        int dm_elysium_active(void);
        static int warned;
        if (dm_elysium_active()) { if (!warned++) LOG("增强：极乐世界角色已改掉落表，掉落倍率本次不生效"); }
        else ratio_apply(main_base(), dpm);
    } else if (g_ratio_mul > 1) ratio_restore(main_base());
}
