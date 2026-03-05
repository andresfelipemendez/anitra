//          Copyright Ferdinand Majerech 2020.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

/** @file nanoprof.h Core nanoprof functionality.
 *
 * Start by looking at `nanoprof_create()` and `nanoprof_event_timepoint()`.
 */
#ifndef NANOPROF_H
#define NANOPROF_H

#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32) && (defined(__TINYC__) || defined(_MSC_VER))
#include <windows.h>
#endif

/// Minimum required nanoprof buffer size.
#define NANOPROF_MIN_BUFFER_SIZE 64
/// Max user-defined timepoint ID. Last 1024 event IDs are reserved for internal use.
#define NANOPROF_TIMEPOINT_ID_MAX_USER 0x3BFF
/// Max user-defined data event ID. Last 1024 event IDs are reserved for internal use.
#define NANOPROF_DATA_ID_MAX_USER 0x3BFF
/// @cond internal
/// @internal Max timepoint *and* data event ID value. Need to fit the ID within 14 (7+7) bits
#define NANOPROF_TIMEPOINT_ID_MAX_INTERNAL 0x3FFF
/// @internal Min builtin 'level end' timepoint event ID (for level -128).
#define NANOPROF_TIMEPOINT_ID_MIN_END (NANOPROF_TIMEPOINT_ID_MAX_USER + 1)
/// @internal Max builtin 'level end' timepoint event ID (for level 127).
#define NANOPROF_TIMEPOINT_ID_MAX_END (NANOPROF_TIMEPOINT_ID_MIN_END + 255)
/// @internal Builtin timepoint event ID used to measure the buffer_return function.
#define NANOPROF_TIMEPOINT_ID_RETURN_BUF (NANOPROF_TIMEPOINT_ID_MAX_END + 1)
/// @internal Level reserved for nanoprof self-profiling.
#define NANOPROF_LEVEL_SELF_PROF ((int8_t)-128)
/// @endcond internal
/** Number of bottom bits to throw away from measured timestamps.
 *
 * Throwing away bottom bits allows us to use smaller events to encode
 * longer time spans.
 *
 * On x86, we use RDTSC to get a timestamp, which usually is the number of
 * cycles at max CPU frequency. The fastest modern RDTSC implementation is on
 * Intel Haswell/Broadwell where it takes 15 cycles (AMD Zen is 35). We can't
 * get any more precise - so the 4 bottom bits (16 cycles) are really just
 * noise (and we could probably even throw away the 5th). With 4 dropped bits,
 * a 'time unit' is 16 cycles, which is 5.33ns on a 3Ghz CPU.
 *
 * On ARM64, system timer usually runs at 1 to 50 MHz, lower than the
 * CPU frequency. With 50Mhz our precision is already low at 20ns so
 * throwing away many bits may be a bad idea.
 */
#if (defined( __i386__ ) || defined( __x86_64__ ))
#define NANOPROF_INSIGNIFICANT_BITS 4
#elif (defined( __aarch64__ ))
#define NANOPROF_INSIGNIFICANT_BITS 0
#else
#define NANOPROF_INSIGNIFICANT_BITS 0
#endif

/// @cond internal

/// Compiler/CPU hint for a likely branch.
#if defined(__TINYC__) || (defined(_MSC_VER) && !defined(__clang__))
#define NANOPROF_LIKELY(x)   (x)
#define NANOPROF_UNLIKELY(x) (x)
#else
#define NANOPROF_LIKELY(x)   __builtin_expect((x),1)
#define NANOPROF_UNLIKELY(x) __builtin_expect((x),0)
#endif

/** Get current high-precision time in nanoseconds.
 *
 * Uses QueryPerformanceCounter on Windows/TCC, timespec_get otherwise.
 */
static inline uint64_t _nanoprof_time_hprec()
{
#if defined(_WIN32) && (defined(__TINYC__) || defined(_MSC_VER))
    /* TCC and MSVC on Windows: use QPC (timespec_get unavailable in TCC) */
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (uint64_t)((double)cnt.QuadPart / (double)freq.QuadPart * 1e9);
#else
    // C11 high precision time
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

/** Get current high-performance time. Clock rate is CPU-dependent.
 *
 * highprec events must be used to determine how much time a `tick` of returned
 * timestamp represents. Usually it represents a single CPU cycle, but this is
 * not guaranteed.
 *
 * On x86, `rdtsc` clock has constant rate set to max frequency on new x86 CPUs,
 * but its rate varies with dynamic frequency on older CPUs. Getting this time
 * can be very fast, taking a few clock cycles.
 * On ARM64, the clock in the `cntvct` register may have any fixed frequency,
 * usually between 1 to 50 MHz. Accessing this is slower than on x86 `rdtsc`,
 * and can take 10s of cycles (anecdata based on RPi 3 testing).
 */
static inline uint64_t _nanoprof_time_fast()
{
#if (defined( __i386__ ) || defined( __x86_64__ ))
    uint32_t hi, lo;
    __asm__ volatile("rdtsc" : "=a" (lo), "=d" (hi));
    return (((uint64_t)hi << 32) | lo) >> NANOPROF_INSIGNIFICANT_BITS;
#elif (defined( __aarch64__ ))
    uint64_t time;
    asm volatile("mrs %0, cntvct_el0" : "=r"(time));
    // Look at cntfrq_el0 if we need to determine frequency of this clock
    return time >> NANOPROF_INSIGNIFICANT_BITS;
#else
#error "_nanoprof_time_fast not yet implemented for current architecture"
#endif
}

/** Describes a registered timepoint or data event. 
 *
 * For data events, this stores their name, for timepoints it also stores the nest level.
 */
struct _nanoprof_event
{
    /// Name of the event - a zero-terminated string.
    const char* name;

    // Single byte so we can easily fit 'end' IDs for all levels into 14 bits.
    /** Nesting 'level' of the event (specific to timepoint events).
     *
     * Timepoint events 'nest' to provide hierarchical, call-graph style time
     * measurement.
     * Timepoints on higher levels are considered 'children' of preceding
     * timepoints on lower levels, and a lower level timepoint also ends scopes
     * for preceding timepoints at all higher levels.
     *
     * Level -128 (NANOPROF_LEVEL_SELF_PROF) is excluded from this nesting, and
     * is used to measure internal overhead of nanoprof.
     */
    int8_t level;
};

/// Types of different event IDs that can be registered by nanoprof.
enum nanoprof_id_type {
    /// Timepoint event ID.
    NANOPROF_ID_TIMEPOINT   = 0x1,
    /// uint8_t data event ID.
    NANOPROF_ID_DATA_U8     = 0x2,
    /// uint16_t data event ID.
    NANOPROF_ID_DATA_U16    = 0x3,
    /// uint32_t data event ID.
    NANOPROF_ID_DATA_U32    = 0x4,
    /// uint64_t data event ID.
    NANOPROF_ID_DATA_U64    = 0x5,
    /// float data event ID.
    NANOPROF_ID_DATA_F32    = 0x6,
    /// double data event ID.
    NANOPROF_ID_DATA_F64    = 0x7,
    /// string data event ID.
    NANOPROF_ID_DATA_STRING = 0x8,
    /// Number of types of nanoprof IDs (need to subtract 1 from ID type value to use it as an index).
    NANOPROF_ID_TYPE_COUNT  =  NANOPROF_ID_DATA_STRING
};

/// @endcond internal

/** Profiling context. This is the main structure of nanoprof.
 *
 * Single thread only. Use multiple context to profile multiple threads.
 *
 * Construct by `nanoprof_create` or `nanoprof_create_advanced` and destroy by
 * `nanoprof_destroy`.
 */
struct nanoprof
{
    /// Timestamp of the previous timepoint, used to compute time delta.
    uint64_t time_previous;

    /** Registered timepoint event names and levels.
     *
     * Array of arrays used as tables - First index is the event ID type, second index is the ID.
     */
    struct _nanoprof_event* registered_events[NANOPROF_ID_TYPE_COUNT];

    /// Buffer we're currently writing event data into, retrieved by buffer_get.
    uint8_t* buffer;

    /// Size of `buffer` in bytes.
    size_t buffer_size;

    /// Position in `buffer` to write the next event at.
    size_t buffer_offset;

    /** User-specifiable function to get new buffer when we use up current one.
     *
     * @param self    This nanoprof context.
     * @param context Context pointer passed through `nanoprof_create_advanced`.
     *                Can be used to pass e.g. a memory allocator.
     *
     * @return New buffer, `buffer_size` bytes long.
     */
    uint8_t* (*buffer_get)(struct nanoprof* self, void* context);

    /** User-specifiable function to return/process a filled buffer.
     *
     * Can be used to write measured data, send it over network, etc.
     *
     * @param self    This nanoprof context.
     * @param buffer  Buffer filled up to `buffer_size` with nanoprof event stream.
     * @param context Context pointer passed through `nanoprof_create_advanced`.
     *                Can be used to pass e.g. a file to write to.
     */
    void (*buffer_return)(struct nanoprof* self, uint8_t* buffer, void* context);

    /** User-specifiable function to free the `context` pointer passed through `nanoprof_create_advanced`.
     *
     * @param context The context pointer.
     */
    void (*free_context)(void* context);

    /// `context` pointer passed through `nanoprof_create_advanced`.
    void* context;
};

/// @cond internal

/** Default context, used by `nanoprof_create`.
 *
 * Acts as a double-buffer to prevent allocation.
 */
struct _nanoprof_default_context
{
    /** Two alternating buffers, `nanoprof->buffer_size` bytes long.
     *
     * Two buffers are needed as we internally profile `buffer_return` calls
     * in which the returned buffer cannot be written to.
     */
    uint8_t* buf[2];

    /// File to write profiling output to.
    FILE* file;
};

/** Default `nanoprof->buffer_get` implementation, used by `nanoprof_create`.
 *
 * Alternates between buffers in `_nanoprof_default_context`.
 */
static inline uint8_t* _nanoprof_default_buffer_get( struct nanoprof* self, void* context )
{
    struct _nanoprof_default_context* ctx = (struct _nanoprof_default_context*) context;
    // Start with `buf[1]`.
    if( self->buffer == ctx->buf[0] || self->buffer == NULL )
    {
        return ctx->buf[1];
    }
    if( self->buffer == ctx->buf[1] )
    {
        return ctx->buf[0];
    }
    assert( false &&
        "default buffer_get() but the default doublebuffer is not used! Maybe nanoprof is uninitialized?" );
    return NULL;
}

/** Default `nanoprof->buffer_return` implementation, used by `nanoprof_create`.
 *
 * Writes the buffer to the file in default context.
 */
static inline void _nanoprof_default_buffer_return(
    struct nanoprof* self,
    uint8_t*         buffer,
    void*            context)
{
    assert( buffer != NULL && "returned a NULL buffer" );
    struct _nanoprof_default_context* ctx = (struct _nanoprof_default_context*) context;
    if( ctx->file != NULL )
    {
        if( 1 != fwrite( buffer, self->buffer_size, 1, ctx->file ) )
        {
            perror( "nanoprof fwrite()" );
            return;
        }
        if( 0 != fflush( ctx->file ) )
        {
            perror( "nanoprof fflush()" );
            return;
        }
    }
}

/** Default `nanoprof->free_context` implementation, used by `nanoprof_create`.
 *
 * Frees the default context and its two buffers.
 */
static inline void _nanoprof_default_free_context( void* context )
{
    struct _nanoprof_default_context* ctx = (struct _nanoprof_default_context*) context;
    free( ctx->buf[0] );
    free( ctx->buf[1] );
    free( ctx );
}

/// @endcond internal

/** Initializes nanoprof context with output to a file.
 *
 * Internally allocates two buffers. Use `nanoprof_create_advanced` if you need
 * custom output or memory management.
 *
 * @param buffer_size Size of internal buffers in bytes. Larger buffers will
 *                    result in larger, less frequent file writes.
 * @param file        File to write profiling stream to. Caller **must close**
 *                    this file after calling nanoprof_destroy().
 *                    Note: can be NULL to disable output - this is an easy way
 *                    to 'disable' nanoprof at runtime (recording code still
 *                    runs, though).
 *
 * @note nanoprof context must be destroyed using `nanoprof_destroy`.
 * @note Calls `malloc` multiple times and assumes `malloc` never fails.
 * @note Builtin timepoint event IDs will be registered during this call,
 * which may result in writes to `file`.
 *
 * **Example**
 * ```
 * #define MY_BUFFER_SIZE (1024 * 1024)
 *
 * int main()
 * {
 *     FILE* file = fopen( "test.nanoprof", "w" );
 *     if( file == NULL )
 *     {
 *         return 1;
 *     }
 *
 *     struct nanoprof prof = nanoprof_create( MY_BUFFER_SIZE, file );
 *
 *     ... do your work and profiling here ...
 *
 *     nanoprof_destroy( &prof );
 *     fclose( file );
 *     return 0;
 * }
 * ```
 */
static inline struct nanoprof nanoprof_create(
    const size_t buffer_size,
    FILE* const file );

/** Initializes nanoprof context and provides control over memory managent and output.
 *
 * @param buffer_size   Size of every buffer returned by `buffer_get` in bytes.
 * @param buffer_get    Function to get a new buffer when we run out of space.
 *                      2 parameters: `struct nanoprof* self` is the nanoprof
 *                      context created by this call, and `void* context` is the
 *                      `context` pointer passed to this call.
 *                      Returns a new buffer, `buffer_size` bytes long.
 * @param buffer_return Function to 'return' a filled buffer to the user, so it
 *                      can be processed/written and deallocated/recycled.
 *                      Has 3 parameters: `struct nanoprof* self` is the
 *                      nanoprof context created by this call, `uint8_t* buffer`
 *                      is the returned buffer and `void* context` is the
 *                      `context` pointer passed to this call.
 * @param free_context  Function to free `context`. Called by `nanoprof_destroy`.
 * @param context       Context for `buffer_get` and `buffer_return`.
 *                      Can be used to manager memory, store file pointer to
 *                      write to, etc. Can be NULL if not used.
 *
 * @note nanoprof context must be destroyed using `nanoprof_destroy`.
 * @note builtin timepoint event IDs will be registered during this call,
 * resulting in at least one call to `buffer_get` and `buffer_return` may be
 * called any number of times depending on buffer size.
 *
 * **Example**
 * ```
 * #define MY_BUFFER_SIZE (1024 * 1024)
 *
 * struct my_context
 * {
 *     bool  output_enabled;
 *     FILE* file;
 * };
 *
 * uint8_t* my_buffer_get( struct nanoprof* self, void* context )
 * {
 *     // for optimal performance, memory should be reused instead of calling
 *     // `malloc` - default `nanoprof_create()` already does this.
 *     return malloc( MY_BUFFER_SIZE );
 * }
 *
 * void my_buffer_return( struct nanoprof* self, uint8_t* buffer, void* context )
 * {
 *     struct my_context* ctx = (struct _nanoprof_default_context*) context;
 *     // note: in the default buffer_return implementation used with
 *     //       nanoprof_create(), there is a check similar to this that
 *     //       disables output when the file is NULL.
 *     if( ctx->output_enabled )
 *     {
 *         fwrite( buffer, self->buffer_size, 1, ctx->file ) );
 *     }
 *     free( buffer );
 * }
 *
 * // does nothing as we pass context on the stack
 * static inline void my_free_context( void* context ) 
 * {
 * }
 *
 * int main()
 * {
 *     my_context ctx;
 *     ctx.file = fopen( "test.nanoprof", "w" );
 *     if( ctx.file == NULL )
 *     {
 *         return 1;
 *     }
 *     // set this to false/true to control output
 *     ctx.output_enabled = true;
 *
 *     struct nanoprof prof = nanoprof_create_advanced(
 *         MY_BUFFER_SIZE,
 *         &my_buffer_get,
 *         &my_buffer_return,
 *         &my_free_context,
 *         &ctx );
 *
 *     ... do your work and profiling here ...
 *
 *     nanoprof_destroy( &prof );
 *     fclose( ctx.file );
 *     return 0;
 * }
 * ```
 */
static inline struct nanoprof nanoprof_create_advanced(
    size_t   buffer_size,
    uint8_t* (*buffer_get)(struct nanoprof* self, void* context),
    void     (*buffer_return)(struct nanoprof* self, uint8_t* buffer, void* context),
    void     (*free_context)(void* context),
    void*    context );

/** Destroy a nanoprof context.
 *
 * Flushes the buffer and frees any resources used by nanoprof, also calling
 * `free_context` if nanoprof was created by `nanoprof_create_advanced`.
 */
static inline void nanoprof_destroy( struct nanoprof* const self );

/** Flush buffered events.
 *
 * When using `nanoprof_create`, flushes buffers to the output file. When using
 * `nanoprof_create_advanced`, calls `buffer_get` followed by `buffer_return`.
 *
 * @param self Nanoprof context.
 */
static inline void nanoprof_flush( struct nanoprof* const self );

/** Register a timepoint event ID, specifying its nesting level and name.
 *
 * Writes a 'register ID' event into the nanoprof stream.
 *
 * Must not be called more than once with the same ID.
 *
 * @note timepoint IDs 0 to 127 are encoded more efficiently, so it's 
 * recommended to use these IDs for your most frequent events.
 *
 * @param self  Nanoprof context.
 * @param id    ID of the timepoint event. Must be less than or equal to
 *              NANOPROF_TIMEPOINT_ID_MAX_USER (0x3BFF == 15359). 14-bit values
 *              higher than this are used for internal timepoint events.
 * @param level Level of the event. Must not be -128 (level used for internal
 *              profiling).
 * @param name  Name of the event.
 *
 * **Example**
 * ```
 * #define ID_COMPLEX_TASK 0
 * #define ID_SUBTASK 1
 *
 * void register_events( struct nanoprof* prof )
 * {
 *     // 'subtasks' are nested within complex tasks - have higher level.
 *     nanoprof_event_register( prof, ID_COMPLEX_TASK, 0, "complex_task" );
 *     nanoprof_event_register( prof, ID_SUBTASK,      1, "subtask" );
 * }
 * ```
 */
static inline void nanoprof_event_register(
    struct nanoprof* const self,
    const uint16_t         id,
    const int8_t           level,
    const char* const      name );

/// @cond internal

/** Internal implementation of nanoprof_event_register.
 *
 * Supports internal event IDs.
 *
 * @see nanoprof_event_register
 */
static inline void _nanoprof_event_register(
    struct nanoprof* const      self,
    const uint16_t              id,
    const int8_t                level,
    const enum nanoprof_id_type type,
    const char* const           name );

/** Register builtin nanoprof timepont events.
 *
 * Called during initialization. Registered events are written into the nanoprof
 * stream as any user-defined events would - with an ID, level and name.
 *
 * Currently these are:
 * * return_buf event: used to profile the nanoprof->return_buf function pointer call.
 * * End events for levels -128 to 127 - emitted by nanoprof_event_timepoint_end.
 */
static inline void _nanoprof_register_builtin( struct nanoprof* self )
{
    _nanoprof_event_register( self, NANOPROF_TIMEPOINT_ID_RETURN_BUF, -128, NANOPROF_ID_TIMEPOINT, "nanoprof:return_buf" );
    for( int level = -128; level <= 127; ++level )
    {
        const uint16_t id = 128 + level + NANOPROF_TIMEPOINT_ID_MIN_END;
        assert( id <= NANOPROF_TIMEPOINT_ID_MAX_END &&
            "builtin nanoprof event ID out of range" );
        _nanoprof_event_register( self, id, level, NANOPROF_ID_TIMEPOINT, "nanoprof:scope end" );
    }
}
/// @endcond internal

static inline struct nanoprof nanoprof_create_advanced(
    size_t   buffer_size,
    uint8_t* (*buffer_get)(struct nanoprof* self, void* context),
    void     (*buffer_return)(struct nanoprof* self, uint8_t* buffer, void* context),
    void     (*free_context)(void* context),
    void*    context )
{
    assert( buffer_size >= NANOPROF_MIN_BUFFER_SIZE &&
            "nanoprof buffer must have at least NANOPROF_MIN_BUFFER_SIZE bytes" );
    struct nanoprof self;
    memset( &self, 0, sizeof(self) );

    // Time delta of first timepoint will be huge if this is 0, but that doesn't
    // matter as the first timepoint's time is set by a highprec event and there
    // is no previous timepoint to create a scope ending in the first timepoint.
    self.time_previous  = 0;

    for( size_t id_type_idx = 0; id_type_idx < NANOPROF_ID_TYPE_COUNT; ++id_type_idx ) 
    {
        // NANOPROF_TIMEPOINT_ID_MAX_INTERNAL is the max for all event ID types, not just timepoints.
        self.registered_events[id_type_idx] = (struct _nanoprof_event*)
            calloc( NANOPROF_TIMEPOINT_ID_MAX_INTERNAL + 1, sizeof(struct _nanoprof_event) );
        for( size_t id = 0; id < NANOPROF_TIMEPOINT_ID_MAX_INTERNAL + 1; ++id )
        {
            // Catch bugs easier if nanoprof_event_timepoint_end is called with unregistered event ID,
            // which would previously insert scope event for nest level 0, which is common in normal
            // usage - 127 is not.
            self.registered_events[id_type_idx][id].level = 127;
        }
    }
    self.buffer         = NULL;
    self.buffer_size    = buffer_size;
    // Triggers 'out of space' and 'flush' on first time event, which also
    // triggers the first hprec event. This way the common 'timepoint' code can
    // be made as simple as possible.
    self.buffer_offset  = buffer_size;
    self.buffer_get     = buffer_get;
    self.buffer_return  = buffer_return;
    self.free_context   = free_context;
    self.context        = context;

    // Register the builtin events. Will call `buffer_get` and possibly even
    // `buffer_return`.
    _nanoprof_register_builtin( &self );

    return self;
}

static inline struct nanoprof nanoprof_create(
    const size_t buffer_size,
    FILE* const file )
{
    assert( buffer_size >= NANOPROF_MIN_BUFFER_SIZE &&
            "nanoprof buffer must have at least NANOPROF_MIN_BUFFER_SIZE bytes" );
    // Allocate internal context and buffers.
    struct _nanoprof_default_context* context = (struct _nanoprof_default_context*)
        malloc( sizeof(struct _nanoprof_default_context) );
    context->buf[0] = (uint8_t*)malloc( buffer_size );
    context->buf[1] = (uint8_t*)malloc( buffer_size );
    context->file   = file;
    return nanoprof_create_advanced(
        buffer_size,
        &_nanoprof_default_buffer_get,
        &_nanoprof_default_buffer_return,
        &_nanoprof_default_free_context,
        context );
}

/** Record a timepoint event - starts a profiling scope.
 *
 * Nest level and name depends on values the timepoint ID was registered with.
 *
 * @param self Nanoprof context.
 * @param id   Event ID. Must be registered with `nanoprof_event_register`.
 *
 * A timepoint begins a *scope* that ends at the next timepoint whose ID has the
 * lower or equal nesting level. For loops it is best to record one timepoint
 * per iteration: current iteration's scope is ended by the next iteration's
 * timepoint. For non-loops the scope is usually ended by a timepoint for
 * another activity, or an 'end' timepoint (see `nanoprof_event_timepoint_end`).
 *
 * **Example**
 * ```
 * #define ID_COMPLEX_TASK 0
 * #define ID_SUBTASK 1
 *
 * void complex_task( struct nanoprof* prof )
 * {
 *     nanoprof_event_timepoint( prof, ID_COMPLEX_TASK );
 *     for( uint32_t i = 0; i < num_iterations; ++i )
 *     {
 *         nanoprof_event_timepoint( prof, ID_SUBTASK );
 *
 *         ... subtask code here ...
 *     }
 *     // ends ID_COMPLEX_TASK's and ID_SUBTASK's scopes.
 *     nanoprof_event_timepoint_end( prof, ID_COMPLEX_TASK );
 * }
 * ```
 */
static inline void nanoprof_event_timepoint( struct nanoprof* const self, const uint16_t id );

/** Record a timepoint event ending a scope started by another timepoint.
 *
 * Ends a scope without starting a new scope.
 *
 * @param self Nanoprof context.
 * @param id   ID of the event to end (for convenience - scope
 *             for any event at the same or higher level is ended).
 *
 * Should be used to end scope for a timepoint that is not naturally followed by
 * some other timepoint - e.g. at the end of a function. See example for
 * `nanoprof_event_timepoint`.
 *
 * @see nanoprof_event_timepoint
 */
static inline void nanoprof_event_timepoint_end( struct nanoprof* const self, const uint16_t id );

/// @cond internal

/** Record a high precision timestamp event.
 *
 * Internal API.
 *
 * Called at the beginning of each buffer, and wherever we detect a 'time skip'
 * when a thread has been moved to another core.
 *
 * A high-precision timestamp stores absolute time in nanoseconds as measured by
 * C11 `timespec_get` and sets the absolute time value for the next timepoint
 * event. Also, every pair or successive highprec events is used to calculate
 * the time duration represented by one unit of timepoint event timestamps,
 * effectively calibrating the timepoint events and dealing with any clock speed
 * changes.
 *
 * @param self Nanoprof context.
 */
static inline void _nanoprof_event_hprec( struct nanoprof* const self );

/// @endcond internal

static inline void nanoprof_destroy( struct nanoprof* const self )
{
    _nanoprof_event_hprec( self );
    // Give reference timepoint for the last highprec event.
    // RETURN_BUF is there only to generate an end event at level -128
    nanoprof_event_timepoint_end( self, NANOPROF_TIMEPOINT_ID_RETURN_BUF );
    // Note that any events recorded in nanoprof_flush go to the next buffer
    // and so they are not written.
    nanoprof_flush( self );
    self->free_context( self->context );
    for( size_t id_type_idx = 0; id_type_idx < NANOPROF_ID_TYPE_COUNT; ++id_type_idx ) 
    {
        free( self->registered_events[id_type_idx] );
    }
    memset( self, 0, sizeof(struct nanoprof) );
}

static inline void nanoprof_flush( struct nanoprof* const self )
{
    // This is the buffer we're flushing.
    uint8_t* const old_buffer = self->buffer;
    // Pad the buffer to the end with 0xFF bytes
    while( self->buffer_offset < self->buffer_size )
    {
        self->buffer[self->buffer_offset++] = 0xFF;
    }

    // Get the next buffer.
    self->buffer = self->buffer_get( self, self->context );
    self->buffer_offset = 0;
    _nanoprof_event_hprec( self );
    // After initialization, self->buffer is NULL - it is only initialized by the first 'flush'.
    const bool first_flush = old_buffer == NULL;
    if( !first_flush )
    {
        // Measure overhead of `buffer_return` - use the new buffer we just got.
        nanoprof_event_timepoint( self, NANOPROF_TIMEPOINT_ID_RETURN_BUF );
        // Return the flushed buffer so it can be written and/or processed.
        self->buffer_return( self, old_buffer, self->context );
        nanoprof_event_timepoint_end( self, NANOPROF_TIMEPOINT_ID_RETURN_BUF );
    }
}

/// @cond internal

static inline void _nanoprof_event_hprec( struct nanoprof* const self )
{
    const size_t event_size = 8;
    const bool out_of_space = self->buffer_offset + event_size > self->buffer_size;
    if( NANOPROF_UNLIKELY( out_of_space ) )
    {
        // This also happens at first event since nanoprof is created with
        // no buffer and buffer offset at the end.
        nanoprof_flush( self );
    }

    // High precision time in nanoseconds.
    const uint64_t time  = _nanoprof_time_hprec();
    // Starts with prefix 0b1001 aka 0x9.
    self->buffer[self->buffer_offset++] = 0xFD;
    self->buffer[self->buffer_offset++] = (time >> 48) & 0xFF;
    self->buffer[self->buffer_offset++] = (time >> 40) & 0xFF;
    self->buffer[self->buffer_offset++] = (time >> 32) & 0xFF;
    self->buffer[self->buffer_offset++] = (time >> 24) & 0xFF;
    self->buffer[self->buffer_offset++] = (time >> 16) & 0xFF;
    self->buffer[self->buffer_offset++] = (time >> 8) & 0xFF;
    self->buffer[self->buffer_offset++] = time  & 0xFF;
}

#pragma GCC diagnostic push
// Using `static inline` to keep this header-only, but need to prevent inline
// for best performance - so disable this warning.
#pragma GCC diagnostic ignored "-Wattributes"
/** 'Slow path' of `nanoprof_event_timepoint`.
 *
 * Used when the ID or time delta are too large for the 2B timepoint event format,
 * or if we're out of space and need to get a new buffer.
 *
 * Records a 4B timepoint event if time delta fits into 15 bits, 8B otherwise.
 *
 * @param self         Nanoprof context
 * @param id           Timepoint event ID.
 * @param delta        Time delta.
 * @param out_of_space True if we ran out of buffer space and need a new buffer.
 *
 * @see nanoprof_event_timepoint
 */
static inline
__attribute__ ((noinline))
void _nanoprof_event_timepoint_slowpath( struct nanoprof* const self, const uint16_t id, uint64_t delta, const bool out_of_space )
{
    if( out_of_space )
    {
        nanoprof_flush( self );
    }

    // Time can go backwards due to CPU switching, making delta overflow to the
    // upper half of 64bit value range. We limit the damage by saving high
    // precision time and using a zero delta instead of the overflown value.
    // Frontends should set event time to the high precision time.
    if( delta >> 63 )
    {
        _nanoprof_event_hprec( self );
        delta = 0;
    }

    // top bit 0 followed by 7 upper bits of ID.
    self->buffer[self->buffer_offset] = (uint8_t)(id >> 7);
    // top bit 1 followed by 7 lower bits of ID.
    self->buffer[self->buffer_offset+1] = (uint8_t)(0x80 | (id & 0x7F));

    const bool delta_over_15bit = delta >= 0x7FFF;
    if( NANOPROF_LIKELY( !delta_over_15bit) )
    {
        // 15-bit delta
        self->buffer[self->buffer_offset+2] = (uint8_t)(delta >> 8);
        self->buffer[self->buffer_offset+3] = (uint8_t)(delta & 0xFF);
        self->buffer_offset += 4;
    }
    else
    {
        // 47-bit delta - top bit 1 to differentiate
        self->buffer[self->buffer_offset+2] = 0x80 | (uint8_t)((delta >> 40) & 0x7F);
        self->buffer[self->buffer_offset+3] = (uint8_t)((delta >> 32) & 0xFF);
        self->buffer[self->buffer_offset+4] = (uint8_t)((delta >> 24) & 0xFF);
        self->buffer[self->buffer_offset+5] = (uint8_t)((delta >> 16) & 0xFF);
        self->buffer[self->buffer_offset+6] = (uint8_t)((delta >> 8)  & 0xFF);
        self->buffer[self->buffer_offset+7] = (uint8_t)(delta & 0xFF);
        self->buffer_offset += 8;
    }
}
#pragma GCC diagnostic pop

/// @endcond internal

static inline void nanoprof_event_timepoint( struct nanoprof* const self, const uint16_t id )
{
    // See the format specification in FORMAT.md

    const uint64_t time  = _nanoprof_time_fast();
    const size_t max_event_size = 8;
    const bool out_of_space = self->buffer_offset + max_event_size > self->buffer_size;
    // If time < time_previous ('skip to the past' due to a CPU switch), this
    // overflows to a huge value, delta doesn't fit into 7 bits and we take the
    // slow path, where it will be handled.
    const uint64_t delta = time - self->time_previous;
    self->time_previous = time;

    // equivalent to `delta > 127 || id > 127` but generates better code
    const bool delta_and_id_over_7bit = (delta | id) & (~0x7F);
    // fast path - 2B event with 7bit ID and 7bit time delta
    if( NANOPROF_LIKELY( !(delta_and_id_over_7bit || out_of_space)) )
    {
        // top bit 0 (buffer ID < 128)
        self->buffer[self->buffer_offset] = (uint8_t)(id);
        // top bit 0 (delta < 128)
        self->buffer[self->buffer_offset + 1] = (uint8_t)(delta);
        self->buffer_offset += 2;
        return;
    }
    // slow path - 4B or 8B event
    _nanoprof_event_timepoint_slowpath( self, id, delta, out_of_space );
}

static inline void nanoprof_event_timepoint_end( struct nanoprof* const self, const uint16_t id )
{
    // Find the level of event whose scope we want to end.
    const int8_t level = (id <= NANOPROF_TIMEPOINT_ID_MAX_USER) 
        // -1 since ID types start at 1
        ? self->registered_events[NANOPROF_ID_TIMEPOINT - 1][id].level
        : NANOPROF_LEVEL_SELF_PROF;
    // TODO debug (additional asserts) mode: assert that id is registered and fail if not (use an
    //      extra field in event to say if it's registered)

    // Emit scope end event for required level.
    const uint8_t level_unsigned = level + 128; // -128 is mapped to 0
    nanoprof_event_timepoint( self, NANOPROF_TIMEPOINT_ID_MIN_END + level_unsigned );
}

/** Emit a data event storing a uint8_t value.
 *
 * Data events should be used for high-frequency logging of
 * performance-relevant stats.
 *
 * @param self  Nanoprof context.
 * @param id    Data event ID. Must be registered with `nanoprof_event_register_data_u8`.
 * @param value Value to store in the data event.
 */
static inline void nanoprof_event_data_u8( struct nanoprof* const self, const uint16_t id, const uint8_t value )
{
    // Event type/data type byte, ID uint16_t, value uint8_t
    const size_t event_size = 1 + sizeof(uint16_t) + sizeof(uint8_t);
    if( NANOPROF_UNLIKELY( self->buffer_offset + event_size > self->buffer_size ) )
    {
        nanoprof_flush( self );
    }

    // 0xA identifies data event, the second nibble is the type.
    self->buffer[self->buffer_offset++] = 0xA1;
    // Event ID.
    self->buffer[self->buffer_offset++] = id >> 8;
    self->buffer[self->buffer_offset++] = id & 0xFF;
    // Store the value.
    self->buffer[self->buffer_offset++] = value;
}

/** Emit a data event storing a uint16_t value in native byte order.
 *
 * @param self  Nanoprof context.
 * @param id    Data event ID. Must be registered with `nanoprof_event_register_data_u16`.
 * @param value Value to store in the data event.
 *
 * @see nanoprof_event_data_u8
 */
static inline void nanoprof_event_data_u16( struct nanoprof* const self, const uint16_t id, const uint16_t value )
{
    const size_t event_size = 1 + sizeof(uint16_t) + sizeof(uint16_t);
    if( NANOPROF_UNLIKELY( self->buffer_offset + event_size > self->buffer_size ) )
    {
        nanoprof_flush( self );
    }
    // 0xA identifies data event, the second nibble is the type.
    self->buffer[self->buffer_offset++] = 0xA2;
    // Event ID.
    self->buffer[self->buffer_offset++] = id >> 8;
    self->buffer[self->buffer_offset++] = id & 0xFF;
    // Store the value.
    *(uint16_t*)(self->buffer + self->buffer_offset) = value;
    self->buffer_offset += sizeof(uint16_t);
}

/** Emit a data event storing a uint32_t value in native byte order.
 *
 * @param self  Nanoprof context.
 * @param id    Data event ID. Must be registered with `nanoprof_event_register_data_u32`.
 * @param value Value to store in the data event.
 *
 * @see nanoprof_event_data_u8
 */
static inline void nanoprof_event_data_u32( struct nanoprof* const self, const uint16_t id, const uint32_t value )
{
    const size_t event_size = 1 + sizeof(uint16_t) + sizeof(uint32_t);
    if( NANOPROF_UNLIKELY( self->buffer_offset + event_size > self->buffer_size ) )
    {
        nanoprof_flush( self );
    }
    // 0xA identifies data event, the second nibble is the type.
    self->buffer[self->buffer_offset++] = 0xA3;
    // Event ID.
    self->buffer[self->buffer_offset++] = id >> 8;
    self->buffer[self->buffer_offset++] = id & 0xFF;
    // Store the value.
    *(uint32_t*)(self->buffer + self->buffer_offset) = value;
    self->buffer_offset += sizeof(uint32_t);
}

/** Emit a data event storing a uint64_t value in native byte order.
 *
 * @param self  Nanoprof context.
 * @param id    Data event ID. Must be registered with `nanoprof_event_register_data_u64`.
 * @param value Value to store in the data event.
 *
 * @see nanoprof_event_data_u8
 */
static inline void nanoprof_event_data_u64( struct nanoprof* const self, const uint16_t id, const uint64_t value )
{
    const size_t event_size = 1 + sizeof(uint16_t) + sizeof(uint64_t);
    if( NANOPROF_UNLIKELY( self->buffer_offset + event_size > self->buffer_size ) )
    {
        nanoprof_flush( self );
    }
    // 0xA identifies data event, the second nibble is the type.
    self->buffer[self->buffer_offset++] = 0xA4;
    // Event ID.
    self->buffer[self->buffer_offset++] = id >> 8;
    self->buffer[self->buffer_offset++] = id & 0xFF;
    // Store the value.
    *(uint64_t*)(self->buffer + self->buffer_offset) = value;
    self->buffer_offset += sizeof(uint64_t);
}

/** Emit a data event storing a float value in native float format.
 *
 * @param self  Nanoprof context.
 * @param id    Data event ID. Must be registered with `nanoprof_event_register_data_f32`.
 * @param value Value to store in the data event.
 *
 * @see nanoprof_event_data_u8
 */
static inline void nanoprof_event_data_f32( struct nanoprof* const self, const uint16_t id, const float value )
{
    const size_t event_size = 1 + sizeof(uint16_t) + sizeof(float);
    if( NANOPROF_UNLIKELY( self->buffer_offset + event_size > self->buffer_size ) )
    {
        nanoprof_flush( self );
    }
    // 0xA identifies data event, the second nibble is the type.
    self->buffer[self->buffer_offset++] = 0xA5;
    // Event ID.
    self->buffer[self->buffer_offset++] = id >> 8;
    self->buffer[self->buffer_offset++] = id & 0xFF;
    // Store the value.
    *(float*)(self->buffer + self->buffer_offset) = value;
    self->buffer_offset += sizeof(float);
}

/** Emit a data event storing a double value in native float format.
 *
 * @param self  Nanoprof context.
 * @param id    Data event ID. Must be registered with `nanoprof_event_register_data_f64`.
 * @param value Value to store in the data event.
 *
 * @see nanoprof_event_data_u8
 */
static inline void nanoprof_event_data_f64( struct nanoprof* const self, const uint16_t id, const double value )
{
    const size_t event_size = 1 + sizeof(uint16_t) + sizeof(double);
    if( NANOPROF_UNLIKELY( self->buffer_offset + event_size > self->buffer_size ) )
    {
        nanoprof_flush( self );
    }
    // 0xA identifies data event, the second nibble is the type.
    self->buffer[self->buffer_offset++] = 0xA6;
    // Event ID.
    self->buffer[self->buffer_offset++] = id >> 8;
    self->buffer[self->buffer_offset++] = id & 0xFF;
    // Store the value.
    *(double*)(self->buffer + self->buffer_offset) = value;
    self->buffer_offset += sizeof(double);
}

/** Emit a data event storing a zero-terminated string.
 *
 * @note if the string + event header (3 bytes) is larger than buffer size
 * specified with `nanoprof_create` or `nanoprof_create_advanced`, the string
 * will be truncated to fit into the buffer. This should not happen unless you
 * try saving really huge strings.
 *
 * @param self  Nanoprof context.
 * @param id    Data event ID. Must be registered with `nanoprof_event_register_data_string`.
 * @param value String to write. Must be `size` long (no zero terminators in the middle).
 * @param size  Size of the string in bytes, not including the zero terminator.
 *
 * @see nanoprof_event_data_u8
 */
static inline void nanoprof_event_data_string(
    struct nanoprof* const self,
    const uint16_t id,
    const char* value,
    const size_t size )
{
    // Type + ID + zero terminator.
    const size_t base_event_size = 1 + sizeof(uint16_t) + sizeof(uint8_t);
    // Truncate strings bigger than buffer size, and flush the buffer
    // to write such a truncated string by itself into a single bufferful.
    const size_t size_cut = (size + base_event_size) <= self->buffer_size ? size : self->buffer_size - base_event_size;
    const size_t event_size = base_event_size + size_cut;
    if( NANOPROF_UNLIKELY( self->buffer_offset + event_size > self->buffer_size ) )
    {
        nanoprof_flush( self );
    }
    // 0xA identifies data event, the second nibble is the type.
    self->buffer[self->buffer_offset++] = 0xAF;
    // Event ID.
    self->buffer[self->buffer_offset++] = id >> 8;
    self->buffer[self->buffer_offset++] = id & 0xFF;
    // If the value contains zero terminators inside, the entire stream will be
    // broken since we don't check this. We assume a sane user to preserve
    // performance here.
    memcpy( self->buffer + self->buffer_offset, value, size_cut );
    self->buffer_offset += size_cut;
    self->buffer[self->buffer_offset++] = 0;
}

/// @cond internal
static inline void _nanoprof_event_register(
    struct nanoprof* const      self,
    const uint16_t              id,
    const int8_t                level,
    const enum nanoprof_id_type id_type,
    const char* const           name )
{
    assert( id <= NANOPROF_TIMEPOINT_ID_MAX_INTERNAL &&
        "Event ID out of range (must be a 14-bit value)" );

    assert(self->registered_events[id_type - 1][id].name == NULL && 
        "event ID's cannot be re-registered" );
    const size_t str_size = strlen(name);
    // Event prefix, level, ID type, ID, zero terminator.
    const size_t base_event_size = 1 + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t) + 1;
    // Truncate name strings bigger than buffer size, and flush the buffer
    // to write such a truncated string by itself into a single bufferful.
    const size_t str_size_cut = (str_size + base_event_size) <= self->buffer_size ? str_size : self->buffer_size - base_event_size;
    const size_t event_size = base_event_size + str_size_cut;
    if( NANOPROF_UNLIKELY( self->buffer_offset + event_size > self->buffer_size ) )
    {
        nanoprof_flush( self );
    }

    // event prefix
    self->buffer[self->buffer_offset++] = 0xFE;
    // level
    self->buffer[self->buffer_offset++] = (uint8_t)level;
    // ID type
    self->buffer[self->buffer_offset++] = (uint8_t)id_type;
    // keep the ID in big endian
    self->buffer[self->buffer_offset++] = id >> 8;
    self->buffer[self->buffer_offset++] = id & 0xFF;
    // If the value contains zero terminators inside, the entire stream will be
    // broken since we don't check this. We assume a sane user to preserve
    // performance here.
    memcpy( self->buffer + self->buffer_offset, name, str_size_cut );
    self->buffer_offset += str_size_cut;
    self->buffer[self->buffer_offset++] = 0;
    // Keep track of name and level.
    self->registered_events[id_type - 1][id].name  = name;
    self->registered_events[id_type - 1][id].level = level;
}
/// @endcond internal

static inline void nanoprof_event_register(
    struct nanoprof* const self,
    const uint16_t         id,
    const int8_t           level,
    const char* const      name )
{
    assert( id <= NANOPROF_TIMEPOINT_ID_MAX_USER &&
        "Event ID out of range (must be at most NANOPROF_TIMEPOINT_ID_MAX_USER)" );
    assert( level != NANOPROF_LEVEL_SELF_PROF &&
        "Event level must not be -128" );
    _nanoprof_event_register( self, id, level, NANOPROF_ID_TIMEPOINT, name );
}

/** Register a uint8_t data event ID, specifying its name.
 *
 * Writes a 'register ID' event into the nanoprof stream.
 *
 * Must not be called more than once with the same ID.
 *
 * @param self  Nanoprof context.
 * @param id    ID of the data event. Must be less than or equal to
 *              NANOPROF_DATA_ID_MAX_USER (0x3BFF == 15359). 14-bit values
 *              higher than this are used for internal data events.
 * @param name  Name of the event.
 *
 * **Example**
 * ```
 * #define ID_ALLOC_EXPONENT 0
 *
 * void register_events( struct nanoprof* prof )
 * {
 *     nanoprof_event_register_data_u8( prof, ID_ALLOC_EXPONENT, "buffer allocation exponent" );
 * }
 * ```
 */
static inline void nanoprof_event_register_data_u8(
    struct nanoprof* const self,
    const uint16_t         id,
    const char* const      name )
{
    assert( id <= NANOPROF_DATA_ID_MAX_USER &&
        "Data event ID out of range (must be at most NANOPROF_DATA_ID_MAX_USER)" );
    _nanoprof_event_register( self, id, 0, NANOPROF_ID_DATA_U8, name );
}

/** Register a uint16_t data event ID, specifying its name.
 *
 * @see nanoprof_event_data_u8()
 */
static inline void nanoprof_event_register_data_u16(
    struct nanoprof* const self,
    const uint16_t         id,
    const char* const      name )
{
    assert( id <= NANOPROF_DATA_ID_MAX_USER &&
        "Data event ID out of range (must be at most NANOPROF_DATA_ID_MAX_USER)" );
    _nanoprof_event_register( self, id, 0, NANOPROF_ID_DATA_U16, name );
}

/** Register a uint32_t data event ID, specifying its name.
 *
 * @see nanoprof_event_data_u8()
 */
static inline void nanoprof_event_register_data_u32(
    struct nanoprof* const self,
    const uint16_t         id,
    const char* const      name )
{
    assert( id <= NANOPROF_DATA_ID_MAX_USER &&
        "Data event ID out of range (must be at most NANOPROF_DATA_ID_MAX_USER)" );
    _nanoprof_event_register( self, id, 0, NANOPROF_ID_DATA_U32, name );
}

/** Register a uint64_t data event ID, specifying its name.
 *
 * @see nanoprof_event_data_u8()
 */
static inline void nanoprof_event_register_data_u64(
    struct nanoprof* const self,
    const uint16_t         id,
    const char* const      name )
{
    assert( id <= NANOPROF_DATA_ID_MAX_USER &&
        "Data event ID out of range (must be at most NANOPROF_DATA_ID_MAX_USER)" );
    _nanoprof_event_register( self, id, 0, NANOPROF_ID_DATA_U64, name );
}

/** Register a float data event ID, specifying its name.
 *
 * @see nanoprof_event_data_u8()
 */
static inline void nanoprof_event_register_data_f32(
    struct nanoprof* const self,
    const uint16_t         id,
    const char* const      name )
{
    assert( id <= NANOPROF_DATA_ID_MAX_USER &&
        "Data event ID out of range (must be at most NANOPROF_DATA_ID_MAX_USER)" );
    _nanoprof_event_register( self, id, 0, NANOPROF_ID_DATA_F32, name );
}

/** Register a double data event ID, specifying its name.
 *
 * @see nanoprof_event_data_u8()
 */
static inline void nanoprof_event_register_data_f64(
    struct nanoprof* const self,
    const uint16_t         id,
    const char* const      name )
{
    assert( id <= NANOPROF_DATA_ID_MAX_USER &&
        "Data event ID out of range (must be at most NANOPROF_DATA_ID_MAX_USER)" );
    _nanoprof_event_register( self, id, 0, NANOPROF_ID_DATA_F64, name );
}

/** Register a string data event ID, specifying its name.
 *
 * @see nanoprof_event_data_u8()
 */
static inline void nanoprof_event_register_data_string(
    struct nanoprof* const self,
    const uint16_t         id,
    const char* const      name )
{
    assert( id <= NANOPROF_DATA_ID_MAX_USER &&
        "Data event ID out of range (must be at most NANOPROF_DATA_ID_MAX_USER)" );
    _nanoprof_event_register( self, id, 0, NANOPROF_ID_DATA_STRING, name );
}

#endif /* end of include guard */
