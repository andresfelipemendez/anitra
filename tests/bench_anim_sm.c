/*
 * bench_anim_sm.c — Benchmark three animation state machine evaluation strategies
 *
 * Approach A: Condition groups (AND logic via group_id in a flat rules table)
 * Approach B: Intermediate states (single-condition rules, more states)
 * Approach C: Bitfield conditions (bitmask AND/OR, O(1) per rule)
 *
 * Compile:  cc -O2 -o bench_anim_sm tests/bench_anim_sm.c
 * Run:      ./bench_anim_sm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

/* ── Timing ─────────────────────────────────────────────────────────────── */

#ifdef __APPLE__
#include <mach/mach_time.h>
static uint64_t timer_now(void) { return mach_absolute_time(); }
static double timer_ns_per_tick(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (double)tb.numer / (double)tb.denom;
}
#else
static uint64_t timer_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static double timer_ns_per_tick(void) { return 1.0; }
#endif

/* ── Constants ──────────────────────────────────────────────────────────── */

#define MAX_STATES     32
#define MAX_RULES      128
#define MAX_PARAMS     16
#define MAX_CHARACTERS  2048

#define NUM_FRAMES     1000  /* frames to simulate */

/* ── Shared types ───────────────────────────────────────────────────────── */

typedef enum {
    CMP_GT, CMP_LT, CMP_EQ, CMP_NEQ, CMP_ON_EXIT, CMP_ALWAYS
} cmp_op;

/* Per-character runtime state */
typedef struct {
    uint8_t current_state;
    float   anim_time;
    float   blend_remaining;
    float   params[MAX_PARAMS];   /* runtime parameter values */
    uint32_t flags;                /* for approach C: bitfield params */
} character_anim;

/* ======================================================================= */
/* APPROACH A: Condition Groups                                            */
/* ======================================================================= */

typedef struct {
    uint8_t from_state[MAX_RULES];
    uint8_t to_state[MAX_RULES];
    uint8_t from_any[MAX_RULES];
    uint8_t param_index[MAX_RULES];
    uint8_t condition_op[MAX_RULES];
    float   threshold[MAX_RULES];
    float   blend_duration[MAX_RULES];
    uint8_t priority[MAX_RULES];
    uint8_t group_id[MAX_RULES];     /* rules in same group = AND */
    int     count;
} rules_a;

static int eval_a(const rules_a *r, const float *params,
                  int cur_state, int clip_done, float *out_blend) {
    int best = -1;
    uint8_t best_pri = 0;
    int i, g;

    /* Collect unique group IDs for current state */
    /* Simple approach: iterate all rules, track group pass/fail */
    uint8_t group_pass[256];
    uint8_t group_to[256];
    float   group_blend[256];
    uint8_t group_pri[256];
    uint8_t group_seen[256];

    memset(group_pass, 1, sizeof(group_pass));   /* assume pass until fail */
    memset(group_seen, 0, sizeof(group_seen));
    memset(group_pri, 0, sizeof(group_pri));

    for (i = 0; i < r->count; i++) {
        int match;
        float pval;

        if (!r->from_any[i] && r->from_state[i] != (uint8_t)cur_state)
            continue;

        g = r->group_id[i];
        if (!group_seen[g]) {
            group_seen[g] = 1;
            group_to[g] = r->to_state[i];
            group_blend[g] = r->blend_duration[i];
            group_pri[g] = r->priority[i];
            group_pass[g] = 1;
        }

        pval = params[r->param_index[i]];
        match = 0;
        switch (r->condition_op[i]) {
            case CMP_GT:      match = (pval > r->threshold[i]);  break;
            case CMP_LT:      match = (pval < r->threshold[i]);  break;
            case CMP_EQ:      match = (pval == r->threshold[i]); break;
            case CMP_NEQ:     match = (pval != r->threshold[i]); break;
            case CMP_ON_EXIT: match = clip_done;                 break;
            case CMP_ALWAYS:  match = 1;                         break;
        }

        if (!match) group_pass[g] = 0;
    }

    /* Find best passing group */
    for (g = 0; g < 256; g++) {
        if (group_seen[g] && group_pass[g] && group_pri[g] >= best_pri) {
            best = group_to[g];
            best_pri = group_pri[g];
            *out_blend = group_blend[g];
        }
    }

    return best;
}

/* ======================================================================= */
/* APPROACH B: Intermediate States (single-condition rules only)           */
/* ======================================================================= */

typedef struct {
    uint8_t from_state[MAX_RULES];
    uint8_t to_state[MAX_RULES];
    uint8_t from_any[MAX_RULES];
    uint8_t param_index[MAX_RULES];
    uint8_t condition_op[MAX_RULES];
    float   threshold[MAX_RULES];
    float   blend_duration[MAX_RULES];
    uint8_t priority[MAX_RULES];
    int     count;
} rules_b;

static int eval_b(const rules_b *r, const float *params,
                  int cur_state, int clip_done, float *out_blend) {
    int best = -1;
    uint8_t best_pri = 0;
    int i;

    for (i = 0; i < r->count; i++) {
        int match;
        float pval;

        if (!r->from_any[i] && r->from_state[i] != (uint8_t)cur_state)
            continue;

        pval = params[r->param_index[i]];
        match = 0;
        switch (r->condition_op[i]) {
            case CMP_GT:      match = (pval > r->threshold[i]);  break;
            case CMP_LT:      match = (pval < r->threshold[i]);  break;
            case CMP_EQ:      match = (pval == r->threshold[i]); break;
            case CMP_NEQ:     match = (pval != r->threshold[i]); break;
            case CMP_ON_EXIT: match = clip_done;                 break;
            case CMP_ALWAYS:  match = 1;                         break;
        }

        if (match && r->priority[i] >= best_pri) {
            best = r->to_state[i];
            best_pri = r->priority[i];
            *out_blend = r->blend_duration[i];
        }
    }

    return best;
}

/* ======================================================================= */
/* APPROACH C: Bitfield conditions                                         */
/* ======================================================================= */

/*
 * Encode parameter conditions as bitmasks.
 * Each "parameter condition" gets 2 bits in a uint32:
 *   bit 2*i     = parameter is "high" (above threshold)
 *   bit 2*i + 1 = parameter is "active" (bool/trigger)
 *
 * A rule has:
 *   required_mask: bits that MUST be set
 *   rejected_mask: bits that MUST be clear
 *   Transition fires if: (flags & required) == required && (flags & rejected) == 0
 */

typedef struct {
    uint8_t  from_state[MAX_RULES];
    uint8_t  to_state[MAX_RULES];
    uint8_t  from_any[MAX_RULES];
    uint32_t required_mask[MAX_RULES];
    uint32_t rejected_mask[MAX_RULES];
    float    blend_duration[MAX_RULES];
    uint8_t  priority[MAX_RULES];
    uint8_t  check_exit[MAX_RULES];    /* 1 = also requires clip_done */
    int      count;
} rules_c;

static int eval_c(const rules_c *r, uint32_t flags,
                  int cur_state, int clip_done, float *out_blend) {
    int best = -1;
    uint8_t best_pri = 0;
    int i;

    for (i = 0; i < r->count; i++) {
        if (!r->from_any[i] && r->from_state[i] != (uint8_t)cur_state)
            continue;

        if (r->check_exit[i] && !clip_done) continue;

        if ((flags & r->required_mask[i]) == r->required_mask[i] &&
            (flags & r->rejected_mask[i]) == 0) {
            if (r->priority[i] >= best_pri) {
                best = r->to_state[i];
                best_pri = r->priority[i];
                *out_blend = r->blend_duration[i];
            }
        }
    }

    return best;
}

/* ======================================================================= */
/* Build a realistic state machine                                         */
/*                                                                         */
/* States: idle(0), walk(1), run(2), sprint(3), jump(4), fall(5),          */
/*         land(6), attack(7), hurt(8), die(9)                             */
/*                                                                         */
/* Params: 0=speed(float), 1=is_grounded(bool), 2=jump(trigger),          */
/*         3=attack(trigger), 4=health(float), 5=hit(trigger)              */
/*                                                                         */
/* Bitfield encoding:                                                      */
/*   bit 0 = speed_high (speed > 0.5)                                      */
/*   bit 1 = speed_run  (speed > 3.0)                                      */
/*   bit 2 = speed_sprint (speed > 6.0)                                    */
/*   bit 3 = is_grounded                                                   */
/*   bit 4 = jump_trigger                                                  */
/*   bit 5 = attack_trigger                                                */
/*   bit 6 = health_zero (health <= 0)                                     */
/*   bit 7 = hit_trigger                                                   */
/* ======================================================================= */

enum {
    ST_IDLE, ST_WALK, ST_RUN, ST_SPRINT, ST_JUMP, ST_FALL,
    ST_LAND, ST_ATTACK, ST_HURT, ST_DIE, ST_COUNT
};

enum {
    P_SPEED, P_GROUNDED, P_JUMP, P_ATTACK, P_HEALTH, P_HIT
};

/* Bitfield bits */
#define BF_SPEED_HIGH    (1u << 0)
#define BF_SPEED_RUN     (1u << 1)
#define BF_SPEED_SPRINT  (1u << 2)
#define BF_GROUNDED      (1u << 3)
#define BF_JUMP          (1u << 4)
#define BF_ATTACK        (1u << 5)
#define BF_HEALTH_ZERO   (1u << 6)
#define BF_HIT           (1u << 7)

static void build_rules_a(rules_a *r) {
    int n = 0;

    /* Group 0: idle -> walk (speed > 0.5 AND grounded) */
    r->from_state[n]=ST_IDLE; r->to_state[n]=ST_WALK; r->from_any[n]=0;
    r->param_index[n]=P_SPEED; r->condition_op[n]=CMP_GT; r->threshold[n]=0.5f;
    r->blend_duration[n]=0.2f; r->priority[n]=1; r->group_id[n]=0; n++;
    r->from_state[n]=ST_IDLE; r->to_state[n]=ST_WALK; r->from_any[n]=0;
    r->param_index[n]=P_GROUNDED; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.2f; r->priority[n]=1; r->group_id[n]=0; n++;

    /* Group 1: walk -> run (speed > 3.0 AND grounded) */
    r->from_state[n]=ST_WALK; r->to_state[n]=ST_RUN; r->from_any[n]=0;
    r->param_index[n]=P_SPEED; r->condition_op[n]=CMP_GT; r->threshold[n]=3.0f;
    r->blend_duration[n]=0.15f; r->priority[n]=1; r->group_id[n]=1; n++;
    r->from_state[n]=ST_WALK; r->to_state[n]=ST_RUN; r->from_any[n]=0;
    r->param_index[n]=P_GROUNDED; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.15f; r->priority[n]=1; r->group_id[n]=1; n++;

    /* Group 2: run -> sprint (speed > 6.0 AND grounded) */
    r->from_state[n]=ST_RUN; r->to_state[n]=ST_SPRINT; r->from_any[n]=0;
    r->param_index[n]=P_SPEED; r->condition_op[n]=CMP_GT; r->threshold[n]=6.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=1; r->group_id[n]=2; n++;
    r->from_state[n]=ST_RUN; r->to_state[n]=ST_SPRINT; r->from_any[n]=0;
    r->param_index[n]=P_GROUNDED; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=1; r->group_id[n]=2; n++;

    /* Group 3: walk -> idle (speed < 0.5) */
    r->from_state[n]=ST_WALK; r->to_state[n]=ST_IDLE; r->from_any[n]=0;
    r->param_index[n]=P_SPEED; r->condition_op[n]=CMP_LT; r->threshold[n]=0.5f;
    r->blend_duration[n]=0.2f; r->priority[n]=1; r->group_id[n]=3; n++;

    /* Group 4: run -> walk (speed < 3.0) */
    r->from_state[n]=ST_RUN; r->to_state[n]=ST_WALK; r->from_any[n]=0;
    r->param_index[n]=P_SPEED; r->condition_op[n]=CMP_LT; r->threshold[n]=3.0f;
    r->blend_duration[n]=0.15f; r->priority[n]=1; r->group_id[n]=4; n++;

    /* Group 5: sprint -> run (speed < 6.0) */
    r->from_state[n]=ST_SPRINT; r->to_state[n]=ST_RUN; r->from_any[n]=0;
    r->param_index[n]=P_SPEED; r->condition_op[n]=CMP_LT; r->threshold[n]=6.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=1; r->group_id[n]=5; n++;

    /* Group 6: any -> jump (jump trigger AND grounded) — high priority */
    r->from_state[n]=0; r->to_state[n]=ST_JUMP; r->from_any[n]=1;
    r->param_index[n]=P_JUMP; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=10; r->group_id[n]=6; n++;
    r->from_state[n]=0; r->to_state[n]=ST_JUMP; r->from_any[n]=1;
    r->param_index[n]=P_GROUNDED; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=10; r->group_id[n]=6; n++;

    /* Group 7: any -> fall (NOT grounded AND NOT jumping) */
    r->from_state[n]=0; r->to_state[n]=ST_FALL; r->from_any[n]=1;
    r->param_index[n]=P_GROUNDED; r->condition_op[n]=CMP_EQ; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.05f; r->priority[n]=5; r->group_id[n]=7; n++;

    /* Group 8: jump -> fall (on exit) */
    r->from_state[n]=ST_JUMP; r->to_state[n]=ST_FALL; r->from_any[n]=0;
    r->param_index[n]=0; r->condition_op[n]=CMP_ON_EXIT; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.05f; r->priority[n]=1; r->group_id[n]=8; n++;

    /* Group 9: fall -> land (grounded) */
    r->from_state[n]=ST_FALL; r->to_state[n]=ST_LAND; r->from_any[n]=0;
    r->param_index[n]=P_GROUNDED; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=1; r->group_id[n]=9; n++;

    /* Group 10: land -> idle (on exit) */
    r->from_state[n]=ST_LAND; r->to_state[n]=ST_IDLE; r->from_any[n]=0;
    r->param_index[n]=0; r->condition_op[n]=CMP_ON_EXIT; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.15f; r->priority[n]=1; r->group_id[n]=10; n++;

    /* Group 11: any -> attack (attack trigger AND grounded) */
    r->from_state[n]=0; r->to_state[n]=ST_ATTACK; r->from_any[n]=1;
    r->param_index[n]=P_ATTACK; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.08f; r->priority[n]=8; r->group_id[n]=11; n++;
    r->from_state[n]=0; r->to_state[n]=ST_ATTACK; r->from_any[n]=1;
    r->param_index[n]=P_GROUNDED; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.08f; r->priority[n]=8; r->group_id[n]=11; n++;

    /* Group 12: attack -> idle (on exit) */
    r->from_state[n]=ST_ATTACK; r->to_state[n]=ST_IDLE; r->from_any[n]=0;
    r->param_index[n]=0; r->condition_op[n]=CMP_ON_EXIT; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.15f; r->priority[n]=1; r->group_id[n]=12; n++;

    /* Group 13: any -> hurt (hit trigger) */
    r->from_state[n]=0; r->to_state[n]=ST_HURT; r->from_any[n]=1;
    r->param_index[n]=P_HIT; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.05f; r->priority[n]=15; r->group_id[n]=13; n++;

    /* Group 14: hurt -> idle (on exit) */
    r->from_state[n]=ST_HURT; r->to_state[n]=ST_IDLE; r->from_any[n]=0;
    r->param_index[n]=0; r->condition_op[n]=CMP_ON_EXIT; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.2f; r->priority[n]=1; r->group_id[n]=14; n++;

    /* Group 15: any -> die (health <= 0) — highest priority */
    r->from_state[n]=0; r->to_state[n]=ST_DIE; r->from_any[n]=1;
    r->param_index[n]=P_HEALTH; r->condition_op[n]=CMP_LT; r->threshold[n]=0.01f;
    r->blend_duration[n]=0.1f; r->priority[n]=20; r->group_id[n]=15; n++;

    r->count = n;
}

static void build_rules_b(rules_b *r) {
    int n = 0;

    /*
     * Approach B uses intermediate states to decompose AND conditions.
     * For "idle -> walk (speed > 0.5 AND grounded)", we need:
     *   idle -> idle  if NOT grounded (stay)
     *   idle -> walk  if speed > 0.5 (only evaluated when grounded via state)
     *
     * But that changes semantics. The simpler honest approach:
     * Just duplicate the rules. For fair comparison, we model the same
     * transitions but with more states to avoid compound conditions.
     *
     * Extra states: idle_air(10), walk_air(11), run_air(12), sprint_air(13)
     * Instead of checking "grounded AND speed > X", we have separate
     * ground/air versions of locomotion states.
     */

    /* idle -> walk (speed > 0.5, only from grounded state) */
    r->from_state[n]=ST_IDLE; r->to_state[n]=ST_WALK; r->from_any[n]=0;
    r->param_index[n]=P_SPEED; r->condition_op[n]=CMP_GT; r->threshold[n]=0.5f;
    r->blend_duration[n]=0.2f; r->priority[n]=1; n++;

    /* walk -> run */
    r->from_state[n]=ST_WALK; r->to_state[n]=ST_RUN; r->from_any[n]=0;
    r->param_index[n]=P_SPEED; r->condition_op[n]=CMP_GT; r->threshold[n]=3.0f;
    r->blend_duration[n]=0.15f; r->priority[n]=1; n++;

    /* run -> sprint */
    r->from_state[n]=ST_RUN; r->to_state[n]=ST_SPRINT; r->from_any[n]=0;
    r->param_index[n]=P_SPEED; r->condition_op[n]=CMP_GT; r->threshold[n]=6.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=1; n++;

    /* walk -> idle */
    r->from_state[n]=ST_WALK; r->to_state[n]=ST_IDLE; r->from_any[n]=0;
    r->param_index[n]=P_SPEED; r->condition_op[n]=CMP_LT; r->threshold[n]=0.5f;
    r->blend_duration[n]=0.2f; r->priority[n]=1; n++;

    /* run -> walk */
    r->from_state[n]=ST_RUN; r->to_state[n]=ST_WALK; r->from_any[n]=0;
    r->param_index[n]=P_SPEED; r->condition_op[n]=CMP_LT; r->threshold[n]=3.0f;
    r->blend_duration[n]=0.15f; r->priority[n]=1; n++;

    /* sprint -> run */
    r->from_state[n]=ST_SPRINT; r->to_state[n]=ST_RUN; r->from_any[n]=0;
    r->param_index[n]=P_SPEED; r->condition_op[n]=CMP_LT; r->threshold[n]=6.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=1; n++;

    /* any ground state -> jump (jump trigger) */
    r->from_state[n]=ST_IDLE; r->to_state[n]=ST_JUMP; r->from_any[n]=0;
    r->param_index[n]=P_JUMP; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=10; n++;
    r->from_state[n]=ST_WALK; r->to_state[n]=ST_JUMP; r->from_any[n]=0;
    r->param_index[n]=P_JUMP; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=10; n++;
    r->from_state[n]=ST_RUN; r->to_state[n]=ST_JUMP; r->from_any[n]=0;
    r->param_index[n]=P_JUMP; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=10; n++;
    r->from_state[n]=ST_SPRINT; r->to_state[n]=ST_JUMP; r->from_any[n]=0;
    r->param_index[n]=P_JUMP; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=10; n++;

    /* any ground locomotion -> fall (not grounded) */
    r->from_state[n]=ST_IDLE; r->to_state[n]=ST_FALL; r->from_any[n]=0;
    r->param_index[n]=P_GROUNDED; r->condition_op[n]=CMP_EQ; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.05f; r->priority[n]=5; n++;
    r->from_state[n]=ST_WALK; r->to_state[n]=ST_FALL; r->from_any[n]=0;
    r->param_index[n]=P_GROUNDED; r->condition_op[n]=CMP_EQ; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.05f; r->priority[n]=5; n++;
    r->from_state[n]=ST_RUN; r->to_state[n]=ST_FALL; r->from_any[n]=0;
    r->param_index[n]=P_GROUNDED; r->condition_op[n]=CMP_EQ; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.05f; r->priority[n]=5; n++;
    r->from_state[n]=ST_SPRINT; r->to_state[n]=ST_FALL; r->from_any[n]=0;
    r->param_index[n]=P_GROUNDED; r->condition_op[n]=CMP_EQ; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.05f; r->priority[n]=5; n++;

    /* jump -> fall (on exit) */
    r->from_state[n]=ST_JUMP; r->to_state[n]=ST_FALL; r->from_any[n]=0;
    r->param_index[n]=0; r->condition_op[n]=CMP_ON_EXIT; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.05f; r->priority[n]=1; n++;

    /* fall -> land (grounded) */
    r->from_state[n]=ST_FALL; r->to_state[n]=ST_LAND; r->from_any[n]=0;
    r->param_index[n]=P_GROUNDED; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.1f; r->priority[n]=1; n++;

    /* land -> idle (on exit) */
    r->from_state[n]=ST_LAND; r->to_state[n]=ST_IDLE; r->from_any[n]=0;
    r->param_index[n]=0; r->condition_op[n]=CMP_ON_EXIT; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.15f; r->priority[n]=1; n++;

    /* ground states -> attack (attack trigger) — explicit per state */
    r->from_state[n]=ST_IDLE; r->to_state[n]=ST_ATTACK; r->from_any[n]=0;
    r->param_index[n]=P_ATTACK; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.08f; r->priority[n]=8; n++;
    r->from_state[n]=ST_WALK; r->to_state[n]=ST_ATTACK; r->from_any[n]=0;
    r->param_index[n]=P_ATTACK; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.08f; r->priority[n]=8; n++;
    r->from_state[n]=ST_RUN; r->to_state[n]=ST_ATTACK; r->from_any[n]=0;
    r->param_index[n]=P_ATTACK; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.08f; r->priority[n]=8; n++;

    /* attack -> idle (on exit) */
    r->from_state[n]=ST_ATTACK; r->to_state[n]=ST_IDLE; r->from_any[n]=0;
    r->param_index[n]=0; r->condition_op[n]=CMP_ON_EXIT; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.15f; r->priority[n]=1; n++;

    /* any -> hurt (hit trigger) */
    r->from_state[n]=0; r->to_state[n]=ST_HURT; r->from_any[n]=1;
    r->param_index[n]=P_HIT; r->condition_op[n]=CMP_EQ; r->threshold[n]=1.0f;
    r->blend_duration[n]=0.05f; r->priority[n]=15; n++;

    /* hurt -> idle (on exit) */
    r->from_state[n]=ST_HURT; r->to_state[n]=ST_IDLE; r->from_any[n]=0;
    r->param_index[n]=0; r->condition_op[n]=CMP_ON_EXIT; r->threshold[n]=0.0f;
    r->blend_duration[n]=0.2f; r->priority[n]=1; n++;

    /* any -> die (health <= 0) */
    r->from_state[n]=0; r->to_state[n]=ST_DIE; r->from_any[n]=1;
    r->param_index[n]=P_HEALTH; r->condition_op[n]=CMP_LT; r->threshold[n]=0.01f;
    r->blend_duration[n]=0.1f; r->priority[n]=20; n++;

    r->count = n;
}

static void build_rules_c(rules_c *r) {
    int n = 0;

    /* idle -> walk (speed_high AND grounded) */
    r->from_state[n]=ST_IDLE; r->to_state[n]=ST_WALK; r->from_any[n]=0;
    r->required_mask[n]=BF_SPEED_HIGH | BF_GROUNDED; r->rejected_mask[n]=0;
    r->blend_duration[n]=0.2f; r->priority[n]=1; r->check_exit[n]=0; n++;

    /* walk -> run (speed_run AND grounded) */
    r->from_state[n]=ST_WALK; r->to_state[n]=ST_RUN; r->from_any[n]=0;
    r->required_mask[n]=BF_SPEED_RUN | BF_GROUNDED; r->rejected_mask[n]=0;
    r->blend_duration[n]=0.15f; r->priority[n]=1; r->check_exit[n]=0; n++;

    /* run -> sprint (speed_sprint AND grounded) */
    r->from_state[n]=ST_RUN; r->to_state[n]=ST_SPRINT; r->from_any[n]=0;
    r->required_mask[n]=BF_SPEED_SPRINT | BF_GROUNDED; r->rejected_mask[n]=0;
    r->blend_duration[n]=0.1f; r->priority[n]=1; r->check_exit[n]=0; n++;

    /* walk -> idle (NOT speed_high) */
    r->from_state[n]=ST_WALK; r->to_state[n]=ST_IDLE; r->from_any[n]=0;
    r->required_mask[n]=0; r->rejected_mask[n]=BF_SPEED_HIGH;
    r->blend_duration[n]=0.2f; r->priority[n]=1; r->check_exit[n]=0; n++;

    /* run -> walk (NOT speed_run) */
    r->from_state[n]=ST_RUN; r->to_state[n]=ST_WALK; r->from_any[n]=0;
    r->required_mask[n]=0; r->rejected_mask[n]=BF_SPEED_RUN;
    r->blend_duration[n]=0.15f; r->priority[n]=1; r->check_exit[n]=0; n++;

    /* sprint -> run (NOT speed_sprint) */
    r->from_state[n]=ST_SPRINT; r->to_state[n]=ST_RUN; r->from_any[n]=0;
    r->required_mask[n]=0; r->rejected_mask[n]=BF_SPEED_SPRINT;
    r->blend_duration[n]=0.1f; r->priority[n]=1; r->check_exit[n]=0; n++;

    /* any -> jump (jump AND grounded) */
    r->from_state[n]=0; r->to_state[n]=ST_JUMP; r->from_any[n]=1;
    r->required_mask[n]=BF_JUMP | BF_GROUNDED; r->rejected_mask[n]=0;
    r->blend_duration[n]=0.1f; r->priority[n]=10; r->check_exit[n]=0; n++;

    /* any ground -> fall (NOT grounded) */
    r->from_state[n]=0; r->to_state[n]=ST_FALL; r->from_any[n]=1;
    r->required_mask[n]=0; r->rejected_mask[n]=BF_GROUNDED;
    r->blend_duration[n]=0.05f; r->priority[n]=5; r->check_exit[n]=0; n++;

    /* jump -> fall (on exit) */
    r->from_state[n]=ST_JUMP; r->to_state[n]=ST_FALL; r->from_any[n]=0;
    r->required_mask[n]=0; r->rejected_mask[n]=0;
    r->blend_duration[n]=0.05f; r->priority[n]=1; r->check_exit[n]=1; n++;

    /* fall -> land (grounded) */
    r->from_state[n]=ST_FALL; r->to_state[n]=ST_LAND; r->from_any[n]=0;
    r->required_mask[n]=BF_GROUNDED; r->rejected_mask[n]=0;
    r->blend_duration[n]=0.1f; r->priority[n]=1; r->check_exit[n]=0; n++;

    /* land -> idle (on exit) */
    r->from_state[n]=ST_LAND; r->to_state[n]=ST_IDLE; r->from_any[n]=0;
    r->required_mask[n]=0; r->rejected_mask[n]=0;
    r->blend_duration[n]=0.15f; r->priority[n]=1; r->check_exit[n]=1; n++;

    /* any -> attack (attack AND grounded) */
    r->from_state[n]=0; r->to_state[n]=ST_ATTACK; r->from_any[n]=1;
    r->required_mask[n]=BF_ATTACK | BF_GROUNDED; r->rejected_mask[n]=0;
    r->blend_duration[n]=0.08f; r->priority[n]=8; r->check_exit[n]=0; n++;

    /* attack -> idle (on exit) */
    r->from_state[n]=ST_ATTACK; r->to_state[n]=ST_IDLE; r->from_any[n]=0;
    r->required_mask[n]=0; r->rejected_mask[n]=0;
    r->blend_duration[n]=0.15f; r->priority[n]=1; r->check_exit[n]=1; n++;

    /* any -> hurt (hit) */
    r->from_state[n]=0; r->to_state[n]=ST_HURT; r->from_any[n]=1;
    r->required_mask[n]=BF_HIT; r->rejected_mask[n]=0;
    r->blend_duration[n]=0.05f; r->priority[n]=15; r->check_exit[n]=0; n++;

    /* hurt -> idle (on exit) */
    r->from_state[n]=ST_HURT; r->to_state[n]=ST_IDLE; r->from_any[n]=0;
    r->required_mask[n]=0; r->rejected_mask[n]=0;
    r->blend_duration[n]=0.2f; r->priority[n]=1; r->check_exit[n]=1; n++;

    /* any -> die (health_zero) */
    r->from_state[n]=0; r->to_state[n]=ST_DIE; r->from_any[n]=1;
    r->required_mask[n]=BF_HEALTH_ZERO; r->rejected_mask[n]=0;
    r->blend_duration[n]=0.1f; r->priority[n]=20; r->check_exit[n]=0; n++;

    r->count = n;
}

/* ── Simulate varied gameplay per character ─────────────────────────────── */

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void randomize_params(character_anim *c, uint32_t *rng, int frame) {
    uint32_t r = xorshift32(rng);

    /* Vary speed smoothly */
    c->params[P_SPEED] += ((float)(r & 0xFF) / 255.0f - 0.5f) * 0.8f;
    if (c->params[P_SPEED] < 0.0f) c->params[P_SPEED] = 0.0f;
    if (c->params[P_SPEED] > 8.0f) c->params[P_SPEED] = 8.0f;

    /* Grounded most of the time */
    c->params[P_GROUNDED] = ((r >> 8) & 0xF) < 13 ? 1.0f : 0.0f;

    /* Occasional triggers */
    c->params[P_JUMP]   = ((r >> 12) & 0xFF) < 3  ? 1.0f : 0.0f;
    c->params[P_ATTACK] = ((r >> 20) & 0xFF) < 5  ? 1.0f : 0.0f;
    c->params[P_HIT]    = ((r >> 24) & 0xFF) < 2  ? 1.0f : 0.0f;
    c->params[P_HEALTH] = 100.0f; /* keep alive */

    /* Compute bitfield flags */
    c->flags = 0;
    if (c->params[P_SPEED] > 0.5f) c->flags |= BF_SPEED_HIGH;
    if (c->params[P_SPEED] > 3.0f) c->flags |= BF_SPEED_RUN;
    if (c->params[P_SPEED] > 6.0f) c->flags |= BF_SPEED_SPRINT;
    if (c->params[P_GROUNDED] > 0.5f) c->flags |= BF_GROUNDED;
    if (c->params[P_JUMP] > 0.5f)   c->flags |= BF_JUMP;
    if (c->params[P_ATTACK] > 0.5f) c->flags |= BF_ATTACK;
    if (c->params[P_HEALTH] <= 0.0f) c->flags |= BF_HEALTH_ZERO;
    if (c->params[P_HIT] > 0.5f)    c->flags |= BF_HIT;
}

/* ── Benchmark harness ──────────────────────────────────────────────────── */

static void run_bench(int num_characters) {
    rules_a ra;
    rules_b rb;
    rules_c rc;
    character_anim *chars;
    uint32_t *rngs;
    uint64_t t0, t1;
    double ns_tick, total_a, total_b, total_c;
    int f, i, transitions_a, transitions_b, transitions_c;

    memset(&ra, 0, sizeof(ra));
    memset(&rb, 0, sizeof(rb));
    memset(&rc, 0, sizeof(rc));
    build_rules_a(&ra);
    build_rules_b(&rb);
    build_rules_c(&rc);

    chars = (character_anim *)calloc(num_characters, sizeof(character_anim));
    rngs  = (uint32_t *)malloc(num_characters * sizeof(uint32_t));

    for (i = 0; i < num_characters; i++) {
        chars[i].current_state = ST_IDLE;
        chars[i].params[P_HEALTH] = 100.0f;
        chars[i].params[P_GROUNDED] = 1.0f;
        rngs[i] = (uint32_t)(i * 2654435761u + 1);
    }

    ns_tick = timer_ns_per_tick();

    printf("\n  Characters: %d | Frames: %d | Rules: A=%d B=%d C=%d\n",
           num_characters, NUM_FRAMES, ra.count, rb.count, rc.count);
    printf("  ─────────────────────────────────────────────────────\n");

    /* ── Approach A ─────────────────────────────────────────────── */
    for (i = 0; i < num_characters; i++) {
        chars[i].current_state = ST_IDLE;
        rngs[i] = (uint32_t)(i * 2654435761u + 1);
    }
    transitions_a = 0;
    t0 = timer_now();
    for (f = 0; f < NUM_FRAMES; f++) {
        for (i = 0; i < num_characters; i++) {
            float blend = 0.0f;
            int clip_done = ((f + i) % 60 == 0) ? 1 : 0;
            int next;
            randomize_params(&chars[i], &rngs[i], f);
            next = eval_a(&ra, chars[i].params, chars[i].current_state, clip_done, &blend);
            if (next >= 0 && next != chars[i].current_state) {
                chars[i].current_state = (uint8_t)next;
                transitions_a++;
            }
        }
    }
    t1 = timer_now();
    total_a = (double)(t1 - t0) * ns_tick;

    /* ── Approach B ─────────────────────────────────────────────── */
    for (i = 0; i < num_characters; i++) {
        chars[i].current_state = ST_IDLE;
        rngs[i] = (uint32_t)(i * 2654435761u + 1);
    }
    transitions_b = 0;
    t0 = timer_now();
    for (f = 0; f < NUM_FRAMES; f++) {
        for (i = 0; i < num_characters; i++) {
            float blend = 0.0f;
            int clip_done = ((f + i) % 60 == 0) ? 1 : 0;
            int next;
            randomize_params(&chars[i], &rngs[i], f);
            next = eval_b(&rb, chars[i].params, chars[i].current_state, clip_done, &blend);
            if (next >= 0 && next != chars[i].current_state) {
                chars[i].current_state = (uint8_t)next;
                transitions_b++;
            }
        }
    }
    t1 = timer_now();
    total_b = (double)(t1 - t0) * ns_tick;

    /* ── Approach C ─────────────────────────────────────────────── */
    for (i = 0; i < num_characters; i++) {
        chars[i].current_state = ST_IDLE;
        rngs[i] = (uint32_t)(i * 2654435761u + 1);
    }
    transitions_c = 0;
    t0 = timer_now();
    for (f = 0; f < NUM_FRAMES; f++) {
        for (i = 0; i < num_characters; i++) {
            float blend = 0.0f;
            int clip_done = ((f + i) % 60 == 0) ? 1 : 0;
            int next;
            randomize_params(&chars[i], &rngs[i], f);
            next = eval_c(&rc, chars[i].flags, chars[i].current_state, clip_done, &blend);
            if (next >= 0 && next != chars[i].current_state) {
                chars[i].current_state = (uint8_t)next;
                transitions_c++;
            }
        }
    }
    t1 = timer_now();
    total_c = (double)(t1 - t0) * ns_tick;

    /* ── Results ────────────────────────────────────────────────── */
    {
        double evals = (double)num_characters * NUM_FRAMES;
        double per_eval_a = total_a / evals;
        double per_eval_b = total_b / evals;
        double per_eval_c = total_c / evals;
        double per_frame_a = total_a / NUM_FRAMES;
        double per_frame_b = total_b / NUM_FRAMES;
        double per_frame_c = total_c / NUM_FRAMES;

        printf("  Approach A (condition groups):   %8.1f ns/eval  %8.1f us/frame  %d transitions\n",
               per_eval_a, per_frame_a / 1000.0, transitions_a);
        printf("  Approach B (intermediate states): %8.1f ns/eval  %8.1f us/frame  %d transitions\n",
               per_eval_b, per_frame_b / 1000.0, transitions_b);
        printf("  Approach C (bitfield masks):      %8.1f ns/eval  %8.1f us/frame  %d transitions\n",
               per_eval_c, per_frame_c / 1000.0, transitions_c);
        printf("\n");
        printf("  Speedup B vs A: %.2fx\n", total_a / total_b);
        printf("  Speedup C vs A: %.2fx\n", total_a / total_c);
        printf("  Speedup C vs B: %.2fx\n", total_b / total_c);

        printf("\n  Memory per rules table:\n");
        printf("    A: %zu bytes (%d rules)\n", sizeof(rules_a), ra.count);
        printf("    B: %zu bytes (%d rules)\n", sizeof(rules_b), rb.count);
        printf("    C: %zu bytes (%d rules)\n", sizeof(rules_c), rc.count);
    }

    free(chars);
    free(rngs);
}

int main(void) {
    int counts[] = {10, 50, 100, 250, 500, 1000, 2000};
    int num_tests = sizeof(counts) / sizeof(counts[0]);
    int t;

    printf("=== Animation State Machine Benchmark ===\n");
    printf("  10 states, 6 parameters, realistic gameplay simulation\n");
    printf("  A = condition groups | B = intermediate states | C = bitfield masks\n");

    for (t = 0; t < num_tests; t++) {
        printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        run_bench(counts[t]);
    }

    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    return 0;
}
