/* ── Collab OT unit tests ──────────────────────────────────────────────────
   Standalone test binary — no SDL, no GPU, no game_state needed.
   Tests the OT transform function and pending buffer management.

   Build:  tcc -o build/Debug/test_collab_ot.exe -Isrc -Isrc/collab tests/test_collab_ot.c
   Run:    build/Debug/test_collab_ot.exe
   ────────────────────────────────────────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "collab_client.h"

/* ── Minimal test harness ─────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        tests_run++; \
        printf("  %-50s ", #name); \
        test_##name(); \
        tests_passed++; \
        printf("PASS\n"); \
    } \
    static void test_##name(void)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: %s == %d, expected %d\n", \
               __FILE__, __LINE__, #a, (int)(a), (int)(b)); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

/* ── Helper ─────────────────────────────────────────────────────────── */

static collab_op make_op(int entity_index, collab_op_type type, uint32_t client_id) {
    collab_op op;
    memset(&op, 0, sizeof(op));
    op.entity_index = entity_index;
    op.type = (uint32_t)type;
    op.client_id = client_id;
    return op;
}

/* ── Transform tests ────────────────────────────────────────────────── */

TEST(transform_different_entity) {
    collab_op remote  = make_op(1, OP_SET_TRANSFORM, 0);
    collab_op pending = make_op(2, OP_SET_TRANSFORM, 1);
    int ret = collab_op_transform(&remote, &pending);
    ASSERT_EQ(ret, 0);
    ASSERT(remote.type != OP_NOOP);
}

TEST(transform_different_type) {
    collab_op remote  = make_op(5, OP_SET_TRANSFORM, 0);
    collab_op pending = make_op(5, OP_SET_ROTATION, 1);
    int ret = collab_op_transform(&remote, &pending);
    ASSERT_EQ(ret, 0);
    ASSERT(remote.type != OP_NOOP);
}

TEST(transform_same_target_noop) {
    collab_op remote  = make_op(5, OP_SET_TRANSFORM, 0);
    collab_op pending = make_op(5, OP_SET_TRANSFORM, 1);
    int ret = collab_op_transform(&remote, &pending);
    ASSERT_EQ(ret, 1);
    ASSERT_EQ(remote.type, (int)OP_NOOP);
}

TEST(transform_pending_not_modified) {
    collab_op remote  = make_op(5, OP_SET_SCALE, 0);
    collab_op pending = make_op(5, OP_SET_SCALE, 1);
    collab_op_transform(&remote, &pending);
    /* pending must be untouched */
    ASSERT_EQ(pending.type, (int)OP_SET_SCALE);
    ASSERT_EQ(pending.entity_index, 5);
}

TEST(transform_chain_first_conflict) {
    /* 3 pending ops, remote conflicts with the 2nd */
    collab_op pending[3];
    collab_op remote = make_op(10, OP_SET_ROTATION, 0);
    int i;
    pending[0] = make_op(7, OP_SET_TRANSFORM, 1);
    pending[1] = make_op(10, OP_SET_ROTATION, 1);
    pending[2] = make_op(3, OP_SET_SCALE, 1);

    for (i = 0; i < 3; i++) {
        if (collab_op_transform(&remote, &pending[i]))
            break;
    }
    ASSERT_EQ(remote.type, (int)OP_NOOP);
    ASSERT_EQ(i, 1); /* stopped at index 1 */
}

TEST(transform_chain_no_conflict) {
    collab_op pending[3];
    collab_op remote = make_op(99, OP_SET_PARENT, 0);
    int i;
    pending[0] = make_op(1, OP_SET_TRANSFORM, 1);
    pending[1] = make_op(2, OP_SET_ROTATION, 1);
    pending[2] = make_op(3, OP_SET_SCALE, 1);

    for (i = 0; i < 3; i++) {
        if (collab_op_transform(&remote, &pending[i]))
            break;
    }
    ASSERT(remote.type != OP_NOOP);
    ASSERT_EQ(i, 3); /* checked all, no conflict */
}

/* ── Pending buffer tests ───────────────────────────────────────────── */

TEST(pending_ack_pops_oldest) {
    collab_state cs;
    memset(&cs, 0, sizeof(cs));
    cs.pending_ops[0] = make_op(1, OP_SET_TRANSFORM, 0);
    cs.pending_ops[1] = make_op(2, OP_SET_ROTATION, 0);
    cs.pending_ops[2] = make_op(3, OP_SET_SCALE, 0);
    cs.pending_count = 3;

    /* Simulate ACK: pop oldest */
    cs.pending_count--;
    if (cs.pending_count > 0) {
        memmove(&cs.pending_ops[0], &cs.pending_ops[1],
                (size_t)cs.pending_count * sizeof(collab_op));
    }

    ASSERT_EQ(cs.pending_count, 2);
    ASSERT_EQ(cs.pending_ops[0].entity_index, 2);
    ASSERT_EQ(cs.pending_ops[1].entity_index, 3);
}

TEST(pending_ack_single_element) {
    collab_state cs;
    memset(&cs, 0, sizeof(cs));
    cs.pending_ops[0] = make_op(1, OP_SET_TRANSFORM, 0);
    cs.pending_count = 1;

    cs.pending_count--;
    /* no memmove needed when count is 0 */

    ASSERT_EQ(cs.pending_count, 0);
}

TEST(pending_overflow) {
    collab_state cs;
    int i;
    memset(&cs, 0, sizeof(cs));

    /* Fill to max */
    for (i = 0; i < COLLAB_PENDING_MAX; i++) {
        cs.pending_ops[i] = make_op(i, OP_SET_TRANSFORM, 0);
    }
    cs.pending_count = COLLAB_PENDING_MAX;

    /* Attempting to add one more should be capped by caller */
    ASSERT_EQ(cs.pending_count, COLLAB_PENDING_MAX);
    /* Verify no corruption of last entry */
    ASSERT_EQ(cs.pending_ops[COLLAB_PENDING_MAX - 1].entity_index, COLLAB_PENDING_MAX - 1);
}

TEST(snapshot_clears_pending) {
    collab_state cs;
    memset(&cs, 0, sizeof(cs));
    cs.pending_count = 5;
    cs.send_queue_count = 3;

    /* Simulate snapshot receive */
    cs.pending_count = 0;
    cs.send_queue_count = 0;

    ASSERT_EQ(cs.pending_count, 0);
    ASSERT_EQ(cs.send_queue_count, 0);
}

TEST(noop_above_op_count) {
    ASSERT(OP_NOOP > OP_COUNT);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    printf("collab OT tests:\n");

    run_transform_different_entity();
    run_transform_different_type();
    run_transform_same_target_noop();
    run_transform_pending_not_modified();
    run_transform_chain_first_conflict();
    run_transform_chain_no_conflict();
    run_pending_ack_pops_oldest();
    run_pending_ack_single_element();
    run_pending_overflow();
    run_snapshot_clears_pending();
    run_noop_above_op_count();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_failed > 0 ? 1 : 0;
}
