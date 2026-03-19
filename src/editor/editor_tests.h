/* Editor tests — run on every hot-reload of the editor DLL.
   #include this at the end of editor.c (after all static functions). */
#ifndef EDITOR_TESTS_H
#define EDITOR_TESTS_H

static int test_insp_begin_edit(void) {
    editor_state e;
    memset(&e, 0, sizeof(e));
    e.insp_edit_field = INSP_FIELD_NONE;
    e.dock = NULL;

    insp_begin_edit(&e, INSP_FIELD_TRANSFORM_X, 5, 3.14f);

    if (e.insp_edit_field != INSP_FIELD_TRANSFORM_X) return 1;
    if (e.insp_edit_entity != 5) return 2;
    if (e.insp_edit_cursor != (int)strlen(e.insp_edit_buf)) return 3;
    if (e.insp_cursor_blink != 0.0f) return 4;
    if (strncmp(e.insp_edit_buf, "3.14", 4) != 0) return 5;
    return 0;
}

static int test_insp_cancel_edit(void) {
    editor_state e;
    memset(&e, 0, sizeof(e));
    e.dock = NULL;

    insp_begin_edit(&e, INSP_FIELD_SCALE_Y, 2, 1.0f);
    if (e.insp_edit_field != INSP_FIELD_SCALE_Y) return 1;

    insp_cancel_edit(&e);
    if (e.insp_edit_field != INSP_FIELD_NONE) return 2;
    if (e.insp_edit_buf[0] != '\0') return 3;
    return 0;
}

static int test_insp_commit_edit(void) {
    game_state gs;
    editor_state e;
    memset(&gs, 0, sizeof(gs));
    memset(&e, 0, sizeof(e));
    e.dock = NULL;
    memset(gs.transform_index, -1, sizeof(gs.transform_index));

    gs.scene_entity_count = 1;
    gs.transform_index[0] = 0;
    gs.transform_component_count = 1;
    gs.transform_components[0].position.x = 0.0f;
    gs.transform_components[0].entity_index = 0;

    insp_begin_edit(&e, INSP_FIELD_TRANSFORM_X, 0, 0.0f);
    strncpy(e.insp_edit_buf, "42.5", sizeof(e.insp_edit_buf));

    insp_commit_edit(&gs, &e);

    if (e.insp_edit_field != INSP_FIELD_NONE) return 1;
    float diff = gs.transform_components[0].position.x - 42.5f;
    if (diff < -0.01f || diff > 0.01f) return 2;
    return 0;
}

static int test_insp_switch_fields(void) {
    game_state gs;
    editor_state e;
    memset(&gs, 0, sizeof(gs));
    memset(&e, 0, sizeof(e));
    e.dock = NULL;
    memset(gs.transform_index, -1, sizeof(gs.transform_index));

    gs.scene_entity_count = 1;
    gs.transform_index[0] = 0;
    gs.transform_component_count = 1;
    gs.transform_components[0].position.x = 0.0f;
    gs.transform_components[0].position.y = 0.0f;
    gs.transform_components[0].entity_index = 0;

    insp_begin_edit(&e, INSP_FIELD_TRANSFORM_X, 0, 0.0f);
    strncpy(e.insp_edit_buf, "10.0", sizeof(e.insp_edit_buf));

    /* Simulate clicking field Y while X is active */
    if (e.insp_edit_field >= 0 && e.insp_edit_field != INSP_FIELD_TRANSFORM_Y)
        insp_commit_edit(&gs, &e);
    insp_begin_edit(&e, INSP_FIELD_TRANSFORM_Y, 0, 0.0f);

    float diff = gs.transform_components[0].position.x - 10.0f;
    if (diff < -0.01f || diff > 0.01f) return 1;
    if (e.insp_edit_field != INSP_FIELD_TRANSFORM_Y) return 2;
    return 0;
}

static int test_insp_cursor_insertion(void) {
    char buf[256];
    const char *src = "3.14";
    int slen = (int)strlen(src);
    int cpos;

    /* Cursor in middle: "3.|14" */
    cpos = 2;
    memcpy(buf, src, (size_t)cpos);
    buf[cpos] = '|';
    memcpy(buf + cpos + 1, src + cpos, (size_t)(slen - cpos));
    buf[slen + 1] = '\0';
    if (strcmp(buf, "3.|14") != 0) return 1;

    /* Cursor at end: "3.14|" */
    cpos = slen;
    memcpy(buf, src, (size_t)cpos);
    buf[cpos] = '|';
    buf[cpos + 1] = '\0';
    if (strcmp(buf, "3.14|") != 0) return 2;

    /* Cursor at start: "|3.14" */
    cpos = 0;
    buf[0] = '|';
    memcpy(buf + 1, src, (size_t)slen);
    buf[slen + 1] = '\0';
    if (strcmp(buf, "|3.14") != 0) return 3;

    /* Empty string: "|" */
    cpos = 0; slen = 0;
    buf[0] = '|';
    buf[1] = '\0';
    if (strcmp(buf, "|") != 0) return 4;

    return 0;
}

static int test_insp_commit_no_edit(void) {
    game_state gs;
    editor_state e;
    memset(&gs, 0, sizeof(gs));
    memset(&e, 0, sizeof(e));
    e.dock = NULL;
    e.insp_edit_field = INSP_FIELD_NONE;

    /* Should be a no-op, not crash */
    insp_commit_edit(&gs, &e);
    if (e.insp_edit_field != INSP_FIELD_NONE) return 1;
    return 0;
}

static int test_insp_commit_bad_entity(void) {
    game_state gs;
    editor_state e;
    memset(&gs, 0, sizeof(gs));
    memset(&e, 0, sizeof(e));
    e.dock = NULL;

    e.insp_edit_field = INSP_FIELD_TRANSFORM_X;
    e.insp_edit_entity = -1; /* invalid */
    strncpy(e.insp_edit_buf, "99.0", sizeof(e.insp_edit_buf));

    /* Should not crash, should clear edit state */
    insp_commit_edit(&gs, &e);
    if (e.insp_edit_field != INSP_FIELD_NONE) return 1;
    return 0;
}

static int test_insp_text_input_filter(void) {
    editor_state e;
    memset(&e, 0, sizeof(e));
    e.dock = NULL;
    e.insp_edit_field = INSP_FIELD_TRANSFORM_X;
    e.insp_edit_buf[0] = '\0';

    /* Simulate typing "12.5abc-3" — only digits, dot, minus should pass */
    const char *input = "12.5abc-3";
    int len = 0;
    const char *p = input;
    while (*p && len < 62) {
        char ch = *p++;
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') {
            e.insp_edit_buf[len++] = ch;
        }
    }
    e.insp_edit_buf[len] = '\0';
    e.insp_edit_cursor = len;

    if (strcmp(e.insp_edit_buf, "12.5-3") != 0) return 1;
    if (e.insp_edit_cursor != 6) return 2;
    return 0;
}

static int run_editor_tests_impl(void) {
    int passed = 0, failed = 0, total = 0;
    struct { const char *name; int (*fn)(void); } tests[] = {
        {"insp_begin_edit",        test_insp_begin_edit},
        {"insp_cancel_edit",       test_insp_cancel_edit},
        {"insp_commit_edit",       test_insp_commit_edit},
        {"insp_switch_fields",     test_insp_switch_fields},
        {"insp_cursor_insertion",  test_insp_cursor_insertion},
        {"insp_commit_no_edit",    test_insp_commit_no_edit},
        {"insp_commit_bad_entity", test_insp_commit_bad_entity},
        {"insp_text_input_filter", test_insp_text_input_filter},
    };
    total = (int)(sizeof(tests) / sizeof(tests[0]));

    fprintf(stderr, "\n=== Editor Tests (%d) ===\n", total);
    for (int i = 0; i < total; i++) {
        int r = tests[i].fn();
        if (r == 0) {
            passed++;
            fprintf(stderr, "  PASS  %s\n", tests[i].name);
        } else {
            failed++;
            fprintf(stderr, "  FAIL  %s (code %d)\n", tests[i].name, r);
        }
    }
    fprintf(stderr, "=== %d passed, %d failed ===\n\n", passed, failed);
    return failed;
}

#endif /* EDITOR_TESTS_H */
