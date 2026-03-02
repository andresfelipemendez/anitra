#ifndef ERROR_VALUE_H
#define ERROR_VALUE_H

#include <stddef.h>
#include <stdio.h>

/*
 * Go-style "error as value" helper.
 * Functions can return error_value directly, or return regular codes while
 * writing an optional error_value via ERRV_RETURN_CODE_* macros.
 */
typedef struct error_value {
    int code;
    const char *message;
    const char *file;
    int line;
} error_value;

#ifdef __cplusplus
#define ERRV_LITERAL(code_value, message_value, file_value, line_value) \
    error_value{(code_value), (message_value), (file_value), (line_value)}
#else
#define ERRV_LITERAL(code_value, message_value, file_value, line_value) \
    ((error_value){(code_value), (message_value), (file_value), (line_value)})
#endif

#define ERRV_OK ERRV_LITERAL(0, NULL, NULL, 0)
#define ERRV_MAKE(code_value, message_value) \
    ERRV_LITERAL((code_value), (message_value), __FILE__, __LINE__)
#define ERRV_IS_OK(err_value) ((err_value).code == 0)

#define ERRV_RETURN_OK() return ERRV_OK
#define ERRV_RETURN_ERR(code_value, message_value) \
    return ERRV_MAKE((code_value), (message_value))

#define ERRV_RETURN_CODE_OK(return_code, out_error) \
    do { \
        if ((out_error) != NULL) *(out_error) = ERRV_OK; \
        return (return_code); \
    } while (0)

#define ERRV_RETURN_CODE_ERR(return_code, out_error, code_value, message_value) \
    do { \
        if ((out_error) != NULL) *(out_error) = ERRV_MAKE((code_value), (message_value)); \
        return (return_code); \
    } while (0)

static inline void errv_log(const char *context, error_value err) {
    if (ERRV_IS_OK(err)) {
        fprintf(stderr, "%s: failed (no error detail)\n", context ? context : "?");
        return;
    }
    fprintf(stderr, "%s: %s (code %d) [%s:%d]\n",
            context ? context : "?",
            err.message ? err.message : "(no message)",
            err.code,
            err.file ? err.file : "?",
            err.line);
}

#endif /* ERROR_VALUE_H */
