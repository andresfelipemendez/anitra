/*
 * build.c - nobuild build system for the Anitra game engine
 *
 * Compile:  build.bat
			 build.sh
			 build.cmd
 * Usage:    builder          (build only)
 *           builder watch    (build, launch engine, and watch for changes)
 *           builder collab   (build + server + 2 editors + watch)
 *           builder server   (build collab server only)
 *
 * This is a single-file C89 build system that replaces CMake.
 * It invokes the platform's native toolchain directly via system().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <poll.h>
#endif

/* ========================================================================= */
/* Constants                                                                  */
/* ========================================================================= */

#define CMD_MAX  8192
#define OBJ_MAX  4096

#ifdef _WIN32
#define PATH_SIZE MAX_PATH
#else
#define PATH_SIZE 4096
#endif

#ifdef _WIN32
#define OBJ_EXT  ".obj"
#define LIB_EXT  ".lib"
#define DLL_EXT  ".dll"
#define DLL_PREFIX ""
#define EXE_EXT  ".exe"
#elif defined(__APPLE__)
#define OBJ_EXT  ".o"
#define LIB_EXT  ".a"
#define DLL_EXT  ".dylib"
#define DLL_PREFIX "lib"
#define EXE_EXT  ""
#else
#define OBJ_EXT  ".o"
#define LIB_EXT  ".a"
#define DLL_EXT  ".so"
#define DLL_PREFIX "lib"
#define EXE_EXT  ""
#endif

#define BUILD_DIR       "build"
#define DEBUG_DIR       "build/Debug"
#define OBJ_DIR         "build/obj"
#define OBJ_EXT_DIR     "build/obj/externals"
#define OBJ_CORE_DIR    "build/obj/core"
#define OBJ_ENGINE_DIR  "build/obj/engine"
#define OBJ_EDITOR_DIR  "build/obj/editor"
#define OBJ_EXE_DIR     "build/obj/exe"
#define OBJ_SDL3_DIR        "build/obj/sdl3"
#define OBJ_SPIRVCROSS_DIR  "build/obj/spirvcross"
#define OBJ_SHADERCROSS_DIR "build/obj/shadercross"
#define REMOTE_DIR          "build/remote"
#define REMOTE_CONFIG_FILE  ".anitra-remote.toml"
#define PROJECT_INCLUDE_FILE "project.txt"
#define DEFAULT_PROJECT_TOML "dungeon1/project.toml"
#define GYM_SCENE_TOML       "tests/gym_scene.toml"

/* Common include paths used by externals, core, engine, and exe targets */
#define COMMON_INCLUDES \
    "-Isrc -Isrc/core -Isrc/engine -Isrc/editor -Iinclude " \
    "-Ilib/SDL3/include -Ilib/SDL_shadercross/include " \
    "-Ilib/SDL_shadercross/external/SPIRV-Cross " \
    "-Ilib/SDL_shadercross/external/prebuilt/inc " \
    "-Ilib/tracy/public " \
    "-Ilib/harfbuzz-src/src " \
    "-Ilib/clay " \
    "-Ilib/cgltf"

/* Tool paths */
#ifdef _WIN32
static const char *tool_cc   = ".\\zig\\zig.exe cc";
static const char *tool_cxx  = ".\\zig\\zig.exe c++";
static const char *tool_link = ".\\zig\\zig.exe cc";
static const char *tool_ar   = ".\\zig\\zig.exe ar";
#else
static const char *tool_cc  = "cc";
static const char *tool_cxx = "c++";
static const char *tool_link = "cc";
static const char *tool_ar  = "ar";
#endif

/* ========================================================================= */
/* Utility functions                                                          */
/* ========================================================================= */

#ifdef _WIN32
static int find_tools(void)
{
    /* On Windows, verify that zig compiler exists */
    if (GetFileAttributesA("zig\\zig.exe") == INVALID_FILE_ATTRIBUTES) {
        printf("!! zig\\zig.exe not found. Ensure the zig directory is in the repo.\n");
        return 1;
    }
    printf("   Using Zig C/C++ compiler: .\\zig\\zig.exe\n");
    return 0;
}
#else
static int find_tools(void)
{
    /* On Unix, just verify that cc exists */
    if (system("command -v cc >/dev/null 2>&1") != 0) {
        printf("!! cc not found. Install a C compiler (gcc or clang).\n");
        return 1;
    }
    return 0;
}
#endif

static int run_cmd(const char *cmd)
{
    int rc;
    printf(">> %s\n", cmd);
    fflush(stdout);
    rc = system(cmd);
    if (rc != 0) {
        printf("!! command failed with exit code %d\n", rc);
    }
    return rc;
}

static int force_rebuild = 0;

static int needs_rebuild(const char *src, const char *obj)
{
    if (force_rebuild) return 1;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA src_attr;
    WIN32_FILE_ATTRIBUTE_DATA obj_attr;

    if (!GetFileAttributesExA(obj, GetFileExInfoStandard, &obj_attr)) {
        return 1;
    }
    if (!GetFileAttributesExA(src, GetFileExInfoStandard, &src_attr)) {
        return 1;
    }
    if (CompareFileTime(&src_attr.ftLastWriteTime, &obj_attr.ftLastWriteTime) > 0) {
        return 1;
    }
    return 0;
#else
    struct stat src_st, obj_st;

    if (stat(obj, &obj_st) != 0) {
        return 1;
    }
    if (stat(src, &src_st) != 0) {
        return 1;
    }
    if (src_st.st_mtime > obj_st.st_mtime) {
        return 1;
    }
    return 0;
#endif
}

static int ensure_dir(const char *path)
{
#ifdef _WIN32
    if (!CreateDirectoryA(path, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            printf("!! failed to create directory: %s (error %lu)\n",
                   path, (unsigned long)err);
            return 1;
        }
    }
#else
    if (mkdir(path, 0755) != 0) {
        if (errno != EEXIST) {
            printf("!! failed to create directory: %s (%s)\n",
                   path, strerror(errno));
            return 1;
        }
    }
#endif
    return 0;
}

static int ensure_dirs(void)
{
    if (ensure_dir(BUILD_DIR))      return 1;
    if (ensure_dir(DEBUG_DIR))      return 1;
    if (ensure_dir(OBJ_DIR))        return 1;
    if (ensure_dir(OBJ_EXT_DIR))    return 1;
    if (ensure_dir(OBJ_CORE_DIR))   return 1;
    if (ensure_dir(OBJ_ENGINE_DIR)) return 1;
    if (ensure_dir(OBJ_EDITOR_DIR)) return 1;
    if (ensure_dir(OBJ_EXE_DIR))    return 1;
    if (ensure_dir(OBJ_SDL3_DIR))       return 1;
    if (ensure_dir(OBJ_SPIRVCROSS_DIR)) return 1;
    if (ensure_dir(OBJ_SHADERCROSS_DIR)) return 1;
    if (ensure_dir(REMOTE_DIR))         return 1;
    return 0;
}

static int file_exists_regular(const char *path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return 0;
    return 1;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
#endif
}

static int parse_project_include_line(char *line, char *out_path, size_t out_size)
{
    char *start;
    char *end;

    start = line;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;

    if (*start == '\0' || *start == '#') return 0;

    if (strncmp(start, "--include", 9) == 0 &&
        (start[9] == ' ' || start[9] == '\t' || start[9] == '=')) {
        start += 9;
    } else if (strncmp(start, "include", 7) == 0 &&
               (start[7] == ' ' || start[7] == '\t' || start[7] == '=')) {
        start += 7;
    }

    while (*start == ' ' || *start == '\t' || *start == '=') start++;

    end = start + strlen(start);
    while (end > start &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    *end = '\0';

    if (*start == '"' && end > start + 1 && end[-1] == '"') {
        start++;
        end[-1] = '\0';
    }

    if (*start == '\0') return 0;

    snprintf(out_path, out_size, "%s", start);
    return 1;
}

static int read_project_include_path(char *out_path, size_t out_size)
{
    FILE *fp;
    char line[PATH_SIZE];

    if (!out_path || out_size == 0) return 0;
    out_path[0] = '\0';

    fp = fopen(PROJECT_INCLUDE_FILE, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        if (parse_project_include_line(line, out_path, out_size)) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static void rand_hex(char *buf, int len)
{
    static const char hex[] = "0123456789ABCDEF";
    static int seeded = 0;
    int i;

    if (!seeded) {
#ifdef _WIN32
        srand((unsigned int)time(NULL) ^ (unsigned int)GetTickCount());
#else
        srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
#endif
        seeded = 1;
    }
    for (i = 0; i < len; i++) {
        buf[i] = hex[rand() % 16];
    }
    buf[len] = '\0';
}

/* ========================================================================= */
/* Build targets                                                              */
/* ========================================================================= */

#ifdef _WIN32
static int generate_def_from_dll(const char *dll_path, const char *def_path,
                                 const char *library_name);
#endif

/* ------- harfbuzz (DLL — C++ amalgamation, exports C API) ---------------- */
static int build_harfbuzz(void)
{
    char cmd[CMD_MAX];
    int any_rebuilt = 0;

    printf("\n=== Building harfbuzz ===\n");
    if (ensure_dirs() != 0) return 1;

    if (needs_rebuild("lib/harfbuzz-src/src/harfbuzz.cc",
                       OBJ_EXT_DIR "/harfbuzz" OBJ_EXT)) {
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "%s -std=c++17 -g -O0 -c "
            "-DHAVE_DIRECTWRITE -DHB_DLL_EXPORT "
            "-Ilib/harfbuzz-src/src "
            "-o " OBJ_EXT_DIR "/harfbuzz" OBJ_EXT " "
            "lib/harfbuzz-src/src/harfbuzz.cc",
            tool_cxx);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -std=c++17 -fPIC -g -O0 -c "
            "-DHB_NO_DIRECTWRITE "
            "-Ilib/harfbuzz-src/src "
            "-o " OBJ_EXT_DIR "/harfbuzz" OBJ_EXT " "
            "lib/harfbuzz-src/src/harfbuzz.cc",
            tool_cxx);
#endif
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    if (any_rebuilt ||
        needs_rebuild(OBJ_EXT_DIR "/harfbuzz" OBJ_EXT,
                      DEBUG_DIR "/" DLL_PREFIX "harfbuzz" DLL_EXT)) {
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "%s -shared -g "
            "-o " DEBUG_DIR "/" DLL_PREFIX "harfbuzz" DLL_EXT " "
            OBJ_EXT_DIR "/harfbuzz" OBJ_EXT " "
            "-ldwrite -lc++ "
            "-Wl,--out-implib," DEBUG_DIR "/harfbuzz" LIB_EXT,
            tool_link);
#elif defined(__APPLE__)
        snprintf(cmd, sizeof(cmd),
            "%s -shared -o " DEBUG_DIR "/" DLL_PREFIX "harfbuzz" DLL_EXT " "
            OBJ_EXT_DIR "/harfbuzz" OBJ_EXT " "
            "-lc++",
            tool_link);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -shared -o " DEBUG_DIR "/" DLL_PREFIX "harfbuzz" DLL_EXT " "
            OBJ_EXT_DIR "/harfbuzz" OBJ_EXT,
            tool_link);
#endif
        if (run_cmd(cmd) != 0) return 1;

#ifdef _WIN32
        /* Generate .def for TCC linking */
        generate_def_from_dll(DEBUG_DIR "/harfbuzz.dll",
                              DEBUG_DIR "/harfbuzz.def", "harfbuzz.dll");
#endif
    } else {
        printf("   harfbuzz is up to date.\n");
    }

    return 0;
}

/* Generate a .def file by parsing the PE export table directly.
   No external tools needed (no dumpbin, no vcvarsall).
   Returns 0 on success. */
#ifdef _WIN32
static DWORD pe_rva_to_offset(DWORD rva,
                               const IMAGE_SECTION_HEADER *sects, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (rva >= sects[i].VirtualAddress &&
            rva <  sects[i].VirtualAddress + sects[i].SizeOfRawData)
            return sects[i].PointerToRawData + (rva - sects[i].VirtualAddress);
    }
    return 0;
}

static int generate_def_from_dll(const char *dll_path, const char *def_path,
                                 const char *library_name)
{
    FILE *dll_fp, *def_fp;
    IMAGE_DOS_HEADER dos;
    DWORD pe_sig;
    IMAGE_FILE_HEADER coff;
    WORD opt_magic;
    DWORD export_rva;
    IMAGE_SECTION_HEADER sects[96];
    IMAGE_EXPORT_DIRECTORY expdir;
    DWORD names_off, name_rva, name_off;
    char name_buf[512];
    int symbol_count = 0;
    DWORD i;
    long opt_start;
    WORD opt_size;

    dll_fp = fopen(dll_path, "rb");
    if (!dll_fp) { printf("!! cannot open %s\n", dll_path); return 1; }

    /* DOS header → PE offset */
    fread(&dos, sizeof(dos), 1, dll_fp);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) {
        printf("!! %s: not a valid PE file\n", dll_path);
        fclose(dll_fp); return 1;
    }

    /* PE signature + COFF header */
    fseek(dll_fp, dos.e_lfanew, SEEK_SET);
    fread(&pe_sig, 4, 1, dll_fp);
    if (pe_sig != IMAGE_NT_SIGNATURE) {
        printf("!! %s: bad PE signature\n", dll_path);
        fclose(dll_fp); return 1;
    }
    fread(&coff, sizeof(coff), 1, dll_fp);
    opt_start = ftell(dll_fp);
    opt_size = coff.SizeOfOptionalHeader;

    /* Read optional header magic to determine PE32 vs PE32+ */
    fread(&opt_magic, 2, 1, dll_fp);

    /* Export directory RVA is at offset 96 (PE32) or 112 (PE32+) from opt start */
    {
        int dd_off = (opt_magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) ? 112 : 96;
        fseek(dll_fp, opt_start + dd_off, SEEK_SET);
        fread(&export_rva, 4, 1, dll_fp);
    }
    if (export_rva == 0) {
        printf("   %s: no exports.\n", dll_path);
        fclose(dll_fp); return 0;
    }

    /* Read section headers for RVA→file-offset conversion */
    fseek(dll_fp, opt_start + opt_size, SEEK_SET);
    {
        int n = (coff.NumberOfSections > 96) ? 96 : coff.NumberOfSections;
        fread(sects, sizeof(IMAGE_SECTION_HEADER), n, dll_fp);
        coff.NumberOfSections = (WORD)n; /* clamp to what we actually read */
    }

    /* Read export directory */
    fseek(dll_fp, pe_rva_to_offset(export_rva, sects, coff.NumberOfSections),
          SEEK_SET);
    fread(&expdir, sizeof(expdir), 1, dll_fp);

    /* Write .def header */
    def_fp = fopen(def_path, "w");
    if (!def_fp) { fclose(dll_fp); printf("!! cannot create %s\n", def_path); return 1; }
    fprintf(def_fp, "LIBRARY %s\nEXPORTS\n", library_name);

    /* Walk the name pointer table */
    names_off = pe_rva_to_offset(expdir.AddressOfNames, sects,
                                 coff.NumberOfSections);
    for (i = 0; i < expdir.NumberOfNames; i++) {
        fseek(dll_fp, names_off + i * 4, SEEK_SET);
        fread(&name_rva, 4, 1, dll_fp);
        name_off = pe_rva_to_offset(name_rva, sects, coff.NumberOfSections);
        fseek(dll_fp, name_off, SEEK_SET);
        fread(name_buf, 1, sizeof(name_buf) - 1, dll_fp);
        name_buf[sizeof(name_buf) - 1] = '\0';
        fprintf(def_fp, "%s\n", name_buf);
        symbol_count++;
    }

    fclose(dll_fp);
    fclose(def_fp);
    printf("   Generated %s (%d exports)\n", def_path, symbol_count);
    return 0;
}
#endif

/* ------- Platform-specific implementations ------------------------------ */
/*
 * Each platform file provides:
 *   - TCC command defines (TCC_COMPILE_CMD, TCC_EDITOR_CMD, etc.)
 *   - sdl3_sources_platform[] array
 *   - watch_and_rebuild() function
 */

/* Forward declarations for platform files (defined later in this file) */
static int generate_migration_code(void);
static int build_all(void);
static int build_server(void);
static int build_shaders(void);

/*
 * Resolve the project include path from project.txt or the default project.
 * Writes the resolved path into out_path and returns a pointer to it,
 * or NULL if no project was found.
 */
static const char *resolve_project(char *out_path, size_t out_size)
{
    out_path[0] = '\0';
    if (read_project_include_path(out_path, out_size)) {
        return out_path;
    }
    if (file_exists_regular(DEFAULT_PROJECT_TOML)) {
        snprintf(out_path, out_size, "%s", DEFAULT_PROJECT_TOML);
        return out_path;
    }
    return NULL;
}

#if defined(_WIN32)
#include "build_win32.c"
#elif defined(__APPLE__)
#include "build_macos.c"
#else
#include "build_linux.c"
#endif

/* ── State migration code generator ──────────────────────────────────── */

#define MIG_GEN_MAX_STRUCTS  32
#define MIG_GEN_MAX_FIELDS   512
#define MIG_GEN_NAME_MAX     64

typedef struct {
    char name[MIG_GEN_NAME_MAX];
    char struct_name[MIG_GEN_NAME_MAX];
} mig_gen_field;

typedef struct {
    char struct_name[MIG_GEN_NAME_MAX];
    int  first_field;   /* index into global field array */
    int  field_count;
} mig_gen_struct;

static mig_gen_field  g_mig_fields[MIG_GEN_MAX_FIELDS];
static int            g_mig_field_count = 0;
static mig_gen_struct g_mig_structs[MIG_GEN_MAX_STRUCTS];
static int            g_mig_struct_count = 0;

/* Target struct names to extract */
static const char *g_mig_targets[] = {
    "editor_state",
    "game_state",
    "parent_component",
    "parent_transform_component",
    "parent_rotation_component",
    "mesh_component",
    "transform_component",
    "rotation_component",
    "scale_component",
    "velocity_component",
    "rigid_body_component",
    "character_controller_component",
    "health_component",
    "collider_component",
    "box_collider_component",
    "capsule_collider_component",
    "animation_component",
    "animation_transition_entry",
    "camera_component",
    NULL
};

static int mig_is_target(const char *name) {
    int i;
    for (i = 0; g_mig_targets[i]; i++) {
        if (strcmp(g_mig_targets[i], name) == 0) return 1;
    }
    return 0;
}

/*
 * Trim leading whitespace, return pointer into same buffer.
 */
static const char *mig_trim(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/*
 * Parse a single field declaration line like:
 *   "float cam_yaw;"
 *   "float x, y;"
 *   "char  name[64];"
 *   "struct arena *root_arena;"
 *   "void *clay_ctx;"
 *   "Vec3 cam_pos;"
 *
 * Extracts field name(s) and appends to g_mig_fields.
 * Returns number of fields added.
 */
static int mig_parse_field_line(const char *line) {
    const char *p, *semi;
    int added = 0, len;

    p = mig_trim(line);

    /* Skip blank, comments, preprocessor, braces, function pointers */
    if (*p == '\0' || *p == '/' || *p == '#' || *p == '{' || *p == '}') return 0;
    if (strstr(p, "(*") != NULL) return 0; /* function pointer — not migratable */

    /* Find semicolon — required for a field */
    semi = strchr(p, ';');
    if (!semi) return 0;

    /* Parse field names by scanning backwards from semicolon.
     * Strategy: find each name token before semicolon/comma. */
    {
        const char *cursor = semi;
        cursor--;
        while (cursor > p && (*cursor == ' ' || *cursor == '\t')) cursor--;

        while (cursor >= p) {
            const char *name_start, *ne;
            char name_buf[MIG_GEN_NAME_MAX];

            /* Skip array brackets */
            if (*cursor == ']') {
                while (cursor > p && *cursor != '[') cursor--;
                cursor--;
                while (cursor > p && (*cursor == ' ' || *cursor == '\t')) cursor--;
            }

            /* Skip pointer star */
            if (*cursor == '*') {
                cursor--;
                while (cursor > p && (*cursor == ' ' || *cursor == '\t')) cursor--;
            }

            /* Find start of name (alphanumeric + underscore) */
            name_start = cursor;
            while (name_start > p && (*(name_start - 1) == '_' ||
                   (*(name_start - 1) >= 'a' && *(name_start - 1) <= 'z') ||
                   (*(name_start - 1) >= 'A' && *(name_start - 1) <= 'Z') ||
                   (*(name_start - 1) >= '0' && *(name_start - 1) <= '9'))) {
                name_start--;
            }

            /* Extract name */
            if (name_start <= cursor) {
                ne = name_start;
                while (ne < semi && *ne != ' ' && *ne != '\t' && *ne != ',' &&
                       *ne != ';' && *ne != '[' && *ne != '*') {
                    ne++;
                }
                len = (int)(ne - name_start);
                if (len > 0 && len < MIG_GEN_NAME_MAX &&
                    g_mig_field_count < MIG_GEN_MAX_FIELDS) {
                    memcpy(name_buf, name_start, len);
                    name_buf[len] = '\0';
                    /* Reject type keywords */
                    if (strcmp(name_buf, "struct") != 0 &&
                        strcmp(name_buf, "const") != 0 &&
                        strcmp(name_buf, "void") != 0 &&
                        strcmp(name_buf, "int") != 0 &&
                        strcmp(name_buf, "float") != 0 &&
                        strcmp(name_buf, "double") != 0 &&
                        strcmp(name_buf, "char") != 0 &&
                        strcmp(name_buf, "uint8_t") != 0 &&
                        strcmp(name_buf, "uint16_t") != 0 &&
                        strcmp(name_buf, "uint32_t") != 0 &&
                        strcmp(name_buf, "uint64_t") != 0 &&
                        strcmp(name_buf, "int8_t") != 0 &&
                        strcmp(name_buf, "int16_t") != 0 &&
                        strcmp(name_buf, "int32_t") != 0 &&
                        strcmp(name_buf, "int64_t") != 0 &&
                        strcmp(name_buf, "unsigned") != 0 &&
                        strcmp(name_buf, "signed") != 0 &&
                        strcmp(name_buf, "long") != 0 &&
                        strcmp(name_buf, "short") != 0) {
                        strcpy(g_mig_fields[g_mig_field_count].name, name_buf);
                        g_mig_field_count++;
                        added++;
                    }
                }
            }

            /* Look for comma to find more names */
            cursor = name_start - 1;
            while (cursor > p && (*cursor == ' ' || *cursor == '\t')) cursor--;
            if (cursor >= p && *cursor == ',') {
                cursor--;
                while (cursor > p && (*cursor == ' ' || *cursor == '\t')) cursor--;
                continue;
            }
            break; /* No comma, done with names */
        }
    }

    return added;
}

/*
 * Parse a header file for targeted struct definitions.
 * Fills g_mig_structs and g_mig_fields.
 */
static int mig_parse_header(const char *path) {
    FILE *f;
    char line[1024];
    int in_struct = 0;
    int brace_depth = 0;
    int in_comment = 0;
    int current_struct_idx = -1;
    int field_start = 0;

    f = fopen(path, "r");
    if (!f) {
        printf("!! Cannot open %s for migration parsing\n", path);
        return 1;
    }

    while (fgets(line, sizeof(line), f)) {
        const char *trimmed = mig_trim(line);

        /* Track multi-line comments — skip lines that start inside a comment */
        {
            int was_in_comment = in_comment;
            const char *c = trimmed;
            while (*c) {
                if (in_comment) {
                    if (c[0] == '*' && c[1] == '/') { in_comment = 0; c += 2; continue; }
                } else {
                    if (c[0] == '/' && c[1] == '*') { in_comment = 1; c += 2; continue; }
                }
                c++;
            }
            if (was_in_comment) continue;
        }

        if (!in_struct) {
            /* Look for "typedef struct {" or "typedef struct name {" */
            if (strncmp(trimmed, "typedef struct", 14) == 0) {
                const char *rest = trimmed + 14;
                /* Check if opening brace is on this line */
                if (strchr(rest, '{')) {
                    const char *bc;
                    in_struct = 1;
                    brace_depth = 0;
                    field_start = g_mig_field_count;
                    current_struct_idx = -1; /* name comes at closing brace */
                    /* Count ALL braces on this line (handles single-line structs) */
                    for (bc = trimmed; *bc; bc++) {
                        if (*bc == '{') brace_depth++;
                        if (*bc == '}') brace_depth--;
                    }
                    if (brace_depth == 0) {
                        /* Single-line struct — extract name and close immediately */
                        const char *cp = strchr(trimmed, '}');
                        if (cp) {
                            char sname[MIG_GEN_NAME_MAX];
                            int slen;
                            cp++;
                            while (*cp == ' ' || *cp == '\t') cp++;
                            slen = 0;
                            while (cp[slen] && cp[slen] != ';' && cp[slen] != ' ' &&
                                   slen < MIG_GEN_NAME_MAX - 1) {
                                sname[slen] = cp[slen];
                                slen++;
                            }
                            sname[slen] = '\0';
                            if (mig_is_target(sname) && g_mig_struct_count < MIG_GEN_MAX_STRUCTS) {
                                int fi;
                                mig_gen_struct *s = &g_mig_structs[g_mig_struct_count];
                                strcpy(s->struct_name, sname);
                                s->first_field = field_start;
                                s->field_count = g_mig_field_count - field_start;
                                for (fi = field_start; fi < g_mig_field_count; fi++)
                                    strcpy(g_mig_fields[fi].struct_name, sname);
                                g_mig_struct_count++;
                                printf("   Parsed %s: %d fields\n", sname, s->field_count);
                            } else {
                                g_mig_field_count = field_start;
                            }
                        }
                        in_struct = 0;
                    }
                }
            }
        } else {
            /* Count braces */
            const char *c;
            for (c = trimmed; *c; c++) {
                if (*c == '{') brace_depth++;
                if (*c == '}') brace_depth--;
            }

            if (brace_depth == 0) {
                /* Closing line: "} type_name;" — extract name */
                const char *cp = strchr(trimmed, '}');
                if (cp) {
                    char sname[MIG_GEN_NAME_MAX];
                    int slen;
                    cp++;
                    while (*cp == ' ' || *cp == '\t') cp++;
                    slen = 0;
                    while (cp[slen] && cp[slen] != ';' && cp[slen] != ' ' &&
                           slen < MIG_GEN_NAME_MAX - 1) {
                        sname[slen] = cp[slen];
                        slen++;
                    }
                    sname[slen] = '\0';

                    if (mig_is_target(sname) && g_mig_struct_count < MIG_GEN_MAX_STRUCTS) {
                        int fi;
                        mig_gen_struct *s = &g_mig_structs[g_mig_struct_count];
                        strcpy(s->struct_name, sname);
                        s->first_field = field_start;
                        s->field_count = g_mig_field_count - field_start;
                        /* Tag fields with struct name */
                        for (fi = field_start; fi < g_mig_field_count; fi++) {
                            strcpy(g_mig_fields[fi].struct_name, sname);
                        }
                        g_mig_struct_count++;
                        printf("   Parsed %s: %d fields\n", sname, s->field_count);
                    } else {
                        /* Not a target, discard collected fields */
                        g_mig_field_count = field_start;
                    }
                }
                in_struct = 0;
            } else if (brace_depth == 1) {
                /* Only parse fields at depth 1 (skip nested structs) */
                mig_parse_field_line(trimmed);
            }
        }
    }

    fclose(f);
    (void)current_struct_idx;
    return 0;
}

static int mig_generate(const char *output_path) {
    FILE *f;
    int si, fi;
    char tmp_path[512];
    int changed = 1;

    if (g_mig_struct_count == 0) {
        printf("   No migration targets found, skipping codegen.\n");
        return 0;
    }

    /* Write to a temp file first, then compare with existing to avoid
       triggering the file watcher when content hasn't changed. */
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", output_path);

    f = fopen(tmp_path, "w");
    if (!f) {
        printf("!! Cannot create %s\n", tmp_path);
        return 1;
    }

    fprintf(f, "/* AUTO-GENERATED by builder — do not edit */\n");
    fprintf(f, "#ifndef STATE_MIGRATION_GEN_H\n");
    fprintf(f, "#define STATE_MIGRATION_GEN_H\n\n");
    fprintf(f, "#include \"state_migration.h\"\n\n");

    for (si = 0; si < g_mig_struct_count; si++) {
        mig_gen_struct *s = &g_mig_structs[si];
        int base = s->first_field;

        /* Field descriptor array */
        fprintf(f, "static const mig_field mig_%s_fields[] = {\n", s->struct_name);
        for (fi = 0; fi < s->field_count; fi++) {
            mig_gen_field *fld = &g_mig_fields[base + fi];
            fprintf(f, "    {\"%s\", (uint32_t)offsetof(%s, %s), "
                       "(uint32_t)sizeof(((%s*)0)->%s)},\n",
                    fld->name, s->struct_name, fld->name,
                    s->struct_name, fld->name);
        }
        fprintf(f, "};\n\n");

        /* Count macro */
        fprintf(f, "#define MIG_%s_COUNT "
                   "(sizeof(mig_%s_fields)/sizeof(mig_%s_fields[0]))\n",
                s->struct_name, s->struct_name, s->struct_name);
        fprintf(f, "\n");
    }

    fprintf(f, "#endif /* STATE_MIGRATION_GEN_H */\n");
    fclose(f);

    /* Compare with existing file — skip write if identical */
    {
        FILE *old_f = fopen(output_path, "rb");
        FILE *new_f = fopen(tmp_path, "rb");
        if (old_f && new_f) {
            long old_sz, new_sz;
            fseek(old_f, 0, SEEK_END); old_sz = ftell(old_f); fseek(old_f, 0, SEEK_SET);
            fseek(new_f, 0, SEEK_END); new_sz = ftell(new_f); fseek(new_f, 0, SEEK_SET);
            if (old_sz == new_sz && old_sz > 0) {
                char *old_buf = (char*)malloc(old_sz);
                char *new_buf = (char*)malloc(new_sz);
                if (old_buf && new_buf) {
                    fread(old_buf, 1, old_sz, old_f);
                    fread(new_buf, 1, new_sz, new_f);
                    if (memcmp(old_buf, new_buf, old_sz) == 0)
                        changed = 0;
                }
                free(old_buf);
                free(new_buf);
            }
        }
        if (old_f) fclose(old_f);
        if (new_f) fclose(new_f);
    }

    if (changed) {
        remove(output_path);
        rename(tmp_path, output_path);
        printf("   Generated %s (%d structs)\n", output_path, g_mig_struct_count);
    } else {
        remove(tmp_path);
        printf("   %s unchanged, skipping write.\n", output_path);
    }
    return 0;
}

static int generate_migration_code(void) {
    printf("\n=== Generating state migration code ===\n");

    /* Reset state */
    g_mig_field_count = 0;
    g_mig_struct_count = 0;

    if (mig_parse_header("src/game.h") != 0) return 1;
    if (mig_parse_header("src/editor/editor.h") != 0) return 1;

    if (mig_generate("src/state_migration.gen.h") != 0) return 1;

    return 0;
}

/* ------- core (DLL) ----------------------------------------------------- */
static int build_tracy(void)
{
    char cmd[CMD_MAX];
    printf("\n=== Building TracyClient ===\n");
    if (ensure_dirs() != 0) return 1;

#ifdef _WIN32
    /* Build TracyClient as a DLL (TCC can't link COFF .obj files) */
    snprintf(cmd, sizeof(cmd),
        "%s -shared -fno-sanitize=undefined"
        " -DTRACY_ENABLE -DTRACY_ON_DEMAND -DTRACY_EXPORTS"
        " -Ilib/tracy/public"
        " lib/tracy/public/TracyClient.cpp"
        " -lws2_32 -ladvapi32 -ldbghelp"
        " -o " DEBUG_DIR "/TracyClient.dll",
        tool_cxx);
    printf(">> %s\n", cmd);
    fflush(stdout);
    if (system(cmd) != 0) {
        printf("!! TracyClient build failed.\n");
        return 1;
    }
    generate_def_from_dll(DEBUG_DIR "/TracyClient.dll",
                          DEBUG_DIR "/TracyClient.def", "TracyClient.dll");
#else
    /* On Linux/macOS, build as shared object */
    snprintf(cmd, sizeof(cmd),
        "%s -shared -fPIC -DTRACY_ENABLE -DTRACY_ON_DEMAND"
        " -Ilib/tracy/public"
        " lib/tracy/public/TracyClient.cpp"
        " -o " DEBUG_DIR "/" DLL_PREFIX "TracyClient" DLL_EXT,
        tool_cxx);
    printf(">> %s\n", cmd);
    fflush(stdout);
    if (system(cmd) != 0) {
        printf("!! TracyClient build failed.\n");
        return 1;
    }
#endif
    return 0;
}

static int build_core(void)
{
    printf("\n=== Building core ===\n");
    if (ensure_dirs() != 0) return 1;
    if (build_tracy() != 0) return 1;
    printf(">> " TCC_CORE_CMD "\n");
    fflush(stdout);
    if (system(TCC_CORE_CMD) != 0) {
        printf("!! core build failed.\n");
        return 1;
    }
    return 0;
}

/* ------- engine (DLL, built with TCC) ----------------------------------- */
static int build_engine(void)
{
    printf("\n=== Building engine ===\n");
    if (ensure_dirs() != 0) return 1;

    printf(">> " TCC_COMPILE_CMD "\n");
    fflush(stdout);
    if (system(TCC_COMPILE_CMD) != 0) {
        printf("!! engine build failed.\n");
        return 1;
    }

    return 0;
}

/* ------- editor (DLL, built with TCC) ----------------------------------- */

static int build_editor(void)
{
    printf("\n=== Building editor ===\n");
    if (ensure_dirs() != 0) return 1;

    printf(">> " TCC_EDITOR_CMD "\n");
    fflush(stdout);
    if (system(TCC_EDITOR_CMD) != 0) {
        printf("!! editor build failed.\n");
        return 1;
    }

    return 0;
}

/* build_test() is provided by the platform file included above */
/* watch_and_rebuild() is provided by the platform file included above */

/* ------- exe ------------------------------------------------------------ */
static int build_exe(void)
{
    printf("\n=== Building exe ===\n");
    if (ensure_dirs() != 0) return 1;
    printf(">> " TCC_EXE_CMD "\n");
    fflush(stdout);
    if (system(TCC_EXE_CMD) != 0) {
        printf("!! exe build failed.\n");
        return 1;
    }
    return 0;
}

/* ------- builder DLL ---------------------------------------------------- */
static int build_builder(void)
{
    printf("\n=== Building builder DLL ===\n");
    if (ensure_dirs() != 0) return 1;
    printf(">> " TCC_BUILDER_DLL_CMD "\n");
    fflush(stdout);
    if (system(TCC_BUILDER_DLL_CMD) != 0) {
        printf("!! builder DLL build failed.\n");
        return 1;
    }
    return 0;
}

/* ------- sdl3 (DLL) ----------------------------------------------------- */

/*
 * Helper: flatten an SDL3 source path into an obj filename.
 * E.g. "lib/SDL3/src/audio/wasapi/SDL_wasapi.c" -> "audio_wasapi_SDL_wasapi.obj"
 * We strip the leading "lib/SDL3/src/" prefix (13 chars) and replace
 * remaining slashes with underscores, then swap the extension.
 */
static void sdl3_obj_path(char *out, size_t out_size, const char *src)
{
    const char *p;
    char *o;
    char *end;
    char *dot;

    /* Skip "lib/SDL3/src/" prefix (13 chars) */
    p = src + 13;

    o = out;
    end = out + out_size - 5; /* room for ".obj\0" */

    /* Write OBJ_SDL3_DIR prefix */
    {
        const char *dir = OBJ_SDL3_DIR "/";
        while (*dir && o < end) *o++ = *dir++;
    }

    /* Copy rest, replacing slashes with underscores */
    while (*p && o < end) {
        if (*p == '/' || *p == '\\') {
            *o++ = '_';
        } else {
            *o++ = *p;
        }
        p++;
    }
    *o = '\0';

    /* Replace .c, .cpp, or .m extension with platform obj extension */
    dot = strrchr(out, '.');
    if (dot) {
        strcpy(dot, OBJ_EXT);
    }
}

/* SDL3 common sources (shared by all platforms) */
static const char *sdl3_sources_common[] = {
    /* Root src/ */
    "lib/SDL3/src/SDL.c",
    "lib/SDL3/src/SDL_assert.c",
    "lib/SDL3/src/SDL_error.c",
    "lib/SDL3/src/SDL_guid.c",
    "lib/SDL3/src/SDL_hashtable.c",
    "lib/SDL3/src/SDL_hints.c",
    "lib/SDL3/src/SDL_list.c",
    "lib/SDL3/src/SDL_log.c",
    "lib/SDL3/src/SDL_properties.c",
    "lib/SDL3/src/SDL_utils.c",

    /* Subsystem generic files */
    "lib/SDL3/src/atomic/SDL_atomic.c",
    "lib/SDL3/src/atomic/SDL_spinlock.c",
    "lib/SDL3/src/audio/SDL_audio.c",
    "lib/SDL3/src/audio/SDL_audiocvt.c",
    "lib/SDL3/src/audio/SDL_audiodev.c",
    "lib/SDL3/src/audio/SDL_audioqueue.c",
    "lib/SDL3/src/audio/SDL_audioresample.c",
    "lib/SDL3/src/audio/SDL_audiotypecvt.c",
    "lib/SDL3/src/audio/SDL_mixer.c",
    "lib/SDL3/src/audio/SDL_wave.c",
    "lib/SDL3/src/camera/SDL_camera.c",
    "lib/SDL3/src/core/SDL_core_unsupported.c",
    "lib/SDL3/src/cpuinfo/SDL_cpuinfo.c",
    "lib/SDL3/src/dialog/SDL_dialog.c",
    "lib/SDL3/src/dialog/SDL_dialog_utils.c",
    "lib/SDL3/src/dynapi/SDL_dynapi.c",
    "lib/SDL3/src/events/SDL_categories.c",
    "lib/SDL3/src/events/SDL_clipboardevents.c",
    "lib/SDL3/src/events/SDL_displayevents.c",
    "lib/SDL3/src/events/SDL_dropevents.c",
    "lib/SDL3/src/events/SDL_events.c",
    "lib/SDL3/src/events/SDL_eventwatch.c",
    "lib/SDL3/src/events/SDL_keyboard.c",
    "lib/SDL3/src/events/SDL_keymap.c",
    "lib/SDL3/src/events/SDL_keysym_to_keycode.c",
    "lib/SDL3/src/events/SDL_keysym_to_scancode.c",
    "lib/SDL3/src/events/SDL_mouse.c",
    "lib/SDL3/src/events/SDL_pen.c",
    "lib/SDL3/src/events/SDL_quit.c",
    "lib/SDL3/src/events/SDL_scancode_tables.c",
    "lib/SDL3/src/events/SDL_touch.c",
    "lib/SDL3/src/events/SDL_windowevents.c",
    "lib/SDL3/src/events/imKStoUCS.c",
    "lib/SDL3/src/filesystem/SDL_filesystem.c",
    "lib/SDL3/src/gpu/SDL_gpu.c",
    "lib/SDL3/src/haptic/SDL_haptic.c",
    "lib/SDL3/src/hidapi/SDL_hidapi.c",
    "lib/SDL3/src/io/SDL_asyncio.c",
    "lib/SDL3/src/io/SDL_iostream.c",
    "lib/SDL3/src/joystick/SDL_gamepad.c",
    "lib/SDL3/src/joystick/SDL_joystick.c",
    "lib/SDL3/src/joystick/SDL_steam_virtual_gamepad.c",
    "lib/SDL3/src/joystick/controller_type.c",
    "lib/SDL3/src/locale/SDL_locale.c",
    "lib/SDL3/src/main/SDL_main_callbacks.c",
    "lib/SDL3/src/main/SDL_runapp.c",
    "lib/SDL3/src/misc/SDL_libusb.c",
    "lib/SDL3/src/misc/SDL_url.c",
    "lib/SDL3/src/power/SDL_power.c",
    "lib/SDL3/src/process/SDL_process.c",
    "lib/SDL3/src/render/SDL_render.c",
    "lib/SDL3/src/render/SDL_render_unsupported.c",
    "lib/SDL3/src/render/SDL_yuv_sw.c",
    "lib/SDL3/src/sensor/SDL_sensor.c",
    "lib/SDL3/src/stdlib/SDL_crc16.c",
    "lib/SDL3/src/stdlib/SDL_crc32.c",
    "lib/SDL3/src/stdlib/SDL_getenv.c",
    "lib/SDL3/src/stdlib/SDL_iconv.c",
    "lib/SDL3/src/stdlib/SDL_malloc.c",
    "lib/SDL3/src/stdlib/SDL_memcpy.c",
    "lib/SDL3/src/stdlib/SDL_memmove.c",
    "lib/SDL3/src/stdlib/SDL_memset.c",
    "lib/SDL3/src/stdlib/SDL_mslibc.c",
    "lib/SDL3/src/stdlib/SDL_murmur3.c",
    "lib/SDL3/src/stdlib/SDL_qsort.c",
    "lib/SDL3/src/stdlib/SDL_random.c",
    "lib/SDL3/src/stdlib/SDL_stdlib.c",
    "lib/SDL3/src/stdlib/SDL_string.c",
    "lib/SDL3/src/stdlib/SDL_strtokr.c",
    "lib/SDL3/src/storage/SDL_storage.c",
    "lib/SDL3/src/thread/SDL_thread.c",
    "lib/SDL3/src/time/SDL_time.c",
    "lib/SDL3/src/timer/SDL_timer.c",
    "lib/SDL3/src/tray/SDL_tray_utils.c",
    "lib/SDL3/src/video/SDL_RLEaccel.c",
    "lib/SDL3/src/video/SDL_blit.c",
    "lib/SDL3/src/video/SDL_blit_0.c",
    "lib/SDL3/src/video/SDL_blit_1.c",
    "lib/SDL3/src/video/SDL_blit_A.c",
    "lib/SDL3/src/video/SDL_blit_N.c",
    "lib/SDL3/src/video/SDL_blit_auto.c",
    "lib/SDL3/src/video/SDL_blit_copy.c",
    "lib/SDL3/src/video/SDL_blit_slow.c",
    "lib/SDL3/src/video/SDL_bmp.c",
    "lib/SDL3/src/video/SDL_clipboard.c",
    "lib/SDL3/src/video/SDL_egl.c",
    "lib/SDL3/src/video/SDL_fillrect.c",
    "lib/SDL3/src/video/SDL_pixels.c",
    "lib/SDL3/src/video/SDL_rect.c",
    "lib/SDL3/src/video/SDL_rotate.c",
    "lib/SDL3/src/video/SDL_stb.c",
    "lib/SDL3/src/video/SDL_stretch.c",
    "lib/SDL3/src/video/SDL_surface.c",
    "lib/SDL3/src/video/SDL_video.c",
    "lib/SDL3/src/video/SDL_video_unsupported.c",
    "lib/SDL3/src/video/SDL_vulkan_utils.c",
    "lib/SDL3/src/video/SDL_yuv.c",

    /* Dummy & offscreen video (shared by all platforms) */
    "lib/SDL3/src/video/dummy/SDL_nullevents.c",
    "lib/SDL3/src/video/dummy/SDL_nullframebuffer.c",
    "lib/SDL3/src/video/dummy/SDL_nullvideo.c",
    "lib/SDL3/src/video/offscreen/SDL_offscreenevents.c",
    "lib/SDL3/src/video/offscreen/SDL_offscreenframebuffer.c",
    "lib/SDL3/src/video/offscreen/SDL_offscreenopengles.c",
    "lib/SDL3/src/video/offscreen/SDL_offscreenvideo.c",
    "lib/SDL3/src/video/offscreen/SDL_offscreenvulkan.c",
    "lib/SDL3/src/video/offscreen/SDL_offscreenwindow.c",
    "lib/SDL3/src/video/yuv2rgb/yuv_rgb_lsx.c",
    "lib/SDL3/src/video/yuv2rgb/yuv_rgb_sse.c",
    "lib/SDL3/src/video/yuv2rgb/yuv_rgb_std.c",

    /* Render drivers (shared) */
    "lib/SDL3/src/render/opengl/SDL_render_gl.c",
    "lib/SDL3/src/render/opengl/SDL_shaders_gl.c",
    "lib/SDL3/src/render/opengles2/SDL_render_gles2.c",
    "lib/SDL3/src/render/opengles2/SDL_shaders_gles2.c",
    "lib/SDL3/src/render/software/SDL_blendfillrect.c",
    "lib/SDL3/src/render/software/SDL_blendline.c",
    "lib/SDL3/src/render/software/SDL_blendpoint.c",
    "lib/SDL3/src/render/software/SDL_drawline.c",
    "lib/SDL3/src/render/software/SDL_drawpoint.c",
    "lib/SDL3/src/render/software/SDL_render_sw.c",
    "lib/SDL3/src/render/software/SDL_triangle.c",
    "lib/SDL3/src/render/gpu/SDL_pipeline_gpu.c",
    "lib/SDL3/src/render/gpu/SDL_render_gpu.c",
    "lib/SDL3/src/render/gpu/SDL_shaders_gpu.c",
    "lib/SDL3/src/render/vulkan/SDL_render_vulkan.c",
    "lib/SDL3/src/render/vulkan/SDL_shaders_vulkan.c",

    /* GPU drivers (shared) */
    "lib/SDL3/src/gpu/vulkan/SDL_gpu_vulkan.c",
    "lib/SDL3/src/gpu/xr/SDL_gpu_openxr.c",
    "lib/SDL3/src/gpu/xr/SDL_openxrdyn.c",

    /* libm */
    "lib/SDL3/src/libm/e_atan2.c",
    "lib/SDL3/src/libm/e_exp.c",
    "lib/SDL3/src/libm/e_fmod.c",
    "lib/SDL3/src/libm/e_log.c",
    "lib/SDL3/src/libm/e_log10.c",
    "lib/SDL3/src/libm/e_pow.c",
    "lib/SDL3/src/libm/e_rem_pio2.c",
    "lib/SDL3/src/libm/e_sqrt.c",
    "lib/SDL3/src/libm/k_cos.c",
    "lib/SDL3/src/libm/k_rem_pio2.c",
    "lib/SDL3/src/libm/k_sin.c",
    "lib/SDL3/src/libm/k_tan.c",
    "lib/SDL3/src/libm/s_atan.c",
    "lib/SDL3/src/libm/s_copysign.c",
    "lib/SDL3/src/libm/s_cos.c",
    "lib/SDL3/src/libm/s_fabs.c",
    "lib/SDL3/src/libm/s_floor.c",
    "lib/SDL3/src/libm/s_isinf.c",
    "lib/SDL3/src/libm/s_isinff.c",
    "lib/SDL3/src/libm/s_isnan.c",
    "lib/SDL3/src/libm/s_isnanf.c",
    "lib/SDL3/src/libm/s_modf.c",
    "lib/SDL3/src/libm/s_scalbn.c",
    "lib/SDL3/src/libm/s_sin.c",
    "lib/SDL3/src/libm/s_tan.c"
};

/* sdl3_sources_platform[] is provided by the platform file included above */

static int build_sdl3(void)
{
    int common_count = sizeof(sdl3_sources_common) / sizeof(sdl3_sources_common[0]);
    int platform_count = sizeof(sdl3_sources_platform) / sizeof(sdl3_sources_platform[0]);
    int total_count = common_count + platform_count;
    int i;
    int any_rebuilt = 0;
    char cmd[CMD_MAX];
    char obj[PATH_SIZE];
    FILE *rsp;
    const char *src;
    const char *ext;
    int is_cpp;

    printf("\n=== Building SDL3 ===\n");
    if (ensure_dirs() != 0) return 1;

    /* Compile each source file individually */
    for (i = 0; i < total_count; i++) {
        src = (i < common_count)
            ? sdl3_sources_common[i]
            : sdl3_sources_platform[i - common_count];

        sdl3_obj_path(obj, sizeof(obj), src);

        if (!needs_rebuild(src, obj))
            continue;

        /* Detect C++ and Objective-C by extension */
        ext = strrchr(src, '.');
        is_cpp = (ext && strcmp(ext, ".cpp") == 0);

#ifdef _WIN32
        if (is_cpp) {
            snprintf(cmd, sizeof(cmd),
                "%s -std=c++20 -O2 -c "
                "-DSDL_BUILDING_SDL3 -DDLL_EXPORT -D_WINDOWS -DWIN32 "
                "-include lib/SDL3/include/build_config/SDL_build_config_zig.h "
                "-Ilib/SDL3/include -Ilib/SDL3/include/build_config "
                "-Ilib/SDL3/src -Ilib/SDL3/src/video/khronos "
                "-o %s "
                "%s",
                tool_cxx, obj, src);
        } else {
            snprintf(cmd, sizeof(cmd),
                "%s -O2 -c "
                "-DSDL_BUILDING_SDL3 -DDLL_EXPORT -D_WINDOWS -DWIN32 "
                "-include lib/SDL3/include/build_config/SDL_build_config_zig.h "
                "-Ilib/SDL3/include -Ilib/SDL3/include/build_config "
                "-Ilib/SDL3/src -Ilib/SDL3/src/video/khronos "
                "-o %s "
                "%s",
                tool_cc, obj, src);
        }
#else
        if (is_cpp) {
            snprintf(cmd, sizeof(cmd),
                "%s -std=c++20 -fPIC -O2 -c "
                "-DSDL_BUILDING_SDL3 -DDLL_EXPORT "
                "-Ilib/SDL3/include -Ilib/SDL3/include/build_config "
                "-Ilib/SDL3/src -Ilib/SDL3/src/video/khronos "
                "-o %s "
                "%s",
                tool_cxx, obj, src);
        } else if (ext && strcmp(ext, ".m") == 0) {
            snprintf(cmd, sizeof(cmd),
                "%s -fPIC -O2 -c -fobjc-arc "
                "-DSDL_BUILDING_SDL3 -DDLL_EXPORT "
                "-Ilib/SDL3/include -Ilib/SDL3/include/build_config "
                "-Ilib/SDL3/src -Ilib/SDL3/src/video/khronos "
                "-o %s "
                "%s",
                tool_cc, obj, src);
        } else {
            snprintf(cmd, sizeof(cmd),
                "%s -fPIC -O2 -c "
                "-DSDL_BUILDING_SDL3 -DDLL_EXPORT "
                "-Ilib/SDL3/include -Ilib/SDL3/include/build_config "
                "-Ilib/SDL3/src -Ilib/SDL3/src/video/khronos "
                "-o %s "
                "%s",
                tool_cc, obj, src);
        }
#endif
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    /* Link step: generate response file then link into DLL */
    if (any_rebuilt ||
#ifdef _WIN32
        needs_rebuild(sdl3_sources_common[0], DEBUG_DIR "/SDL3" DLL_EXT)) {
#else
        needs_rebuild(sdl3_sources_common[0], DEBUG_DIR "/libSDL3" DLL_EXT)) {
#endif

        /* Write response file with all .obj paths */
        rsp = fopen(OBJ_SDL3_DIR "/sdl3_objs.txt", "w");
        if (!rsp) {
            printf("!! failed to create response file\n");
            return 1;
        }
        for (i = 0; i < total_count; i++) {
            src = (i < common_count)
                ? sdl3_sources_common[i]
                : sdl3_sources_platform[i - common_count];
            sdl3_obj_path(obj, sizeof(obj), src);
            fprintf(rsp, "%s\n", obj);
        }
        fclose(rsp);

#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "%s -shared -g "
            "-o " DEBUG_DIR "/SDL3" DLL_EXT " "
            "@" OBJ_SDL3_DIR "/sdl3_objs.txt "
            "-luser32 -lgdi32 -lwinmm -limm32 "
            "-lole32 -loleaut32 -lversion -ladvapi32 "
            "-lsetupapi -lshell32 -lcfgmgr32 -lhid "
            "-Wl,--out-implib," DEBUG_DIR "/SDL3" LIB_EXT,
            tool_link);
#elif defined(__APPLE__)
        snprintf(cmd, sizeof(cmd),
            "%s -shared -o " DEBUG_DIR "/libSDL3" DLL_EXT " "
            "@" OBJ_SDL3_DIR "/sdl3_objs.txt "
            "-lpthread -lm "
            "-framework Cocoa -framework IOKit -framework CoreAudio "
            "-framework AudioToolbox -framework CoreVideo -framework Metal "
            "-framework QuartzCore -framework Carbon -framework ForceFeedback "
            "-framework GameController -framework CoreHaptics "
            "-framework UniformTypeIdentifiers -framework CoreMedia "
            "-framework AVFoundation",
            tool_link);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -shared -o " DEBUG_DIR "/libSDL3" DLL_EXT " "
            "@" OBJ_SDL3_DIR "/sdl3_objs.txt "
            "-lpthread -ldl -lm",
            tool_link);
#endif
        if (run_cmd(cmd) != 0) return 1;

#ifdef _WIN32
        /* Generate .def file for TCC linking */
        generate_def_from_dll(DEBUG_DIR "/SDL3.dll",
                              DEBUG_DIR "/SDL3.def", "SDL3.dll");
#endif
    } else {
        printf("   SDL3 is up to date.\n");
    }

#ifdef _WIN32
    /* Ensure .def file exists even when SDL3 wasn't rebuilt (TCC needs it) */
    {
        FILE *chk = fopen(DEBUG_DIR "/SDL3.def", "r");
        if (chk) {
            fclose(chk);
        } else {
            generate_def_from_dll(DEBUG_DIR "/SDL3.dll",
                                  DEBUG_DIR "/SDL3.def", "SDL3.dll");
        }
    }
#endif

    return 0;
}

/* ------- spirv-cross (DLL) ---------------------------------------------- */
static int build_spirvcross(void)
{
    static const char *sources[] = {
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_cfg.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_cpp.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_cross.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_cross_c.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_cross_parsed_ir.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_cross_util.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_glsl.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_hlsl.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_msl.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_parser.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_reflect.cpp"
    };
    static const char *objs[] = {
        OBJ_SPIRVCROSS_DIR "/spirv_cfg" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_cpp" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_cross" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_cross_c" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_cross_parsed_ir" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_cross_util" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_glsl" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_hlsl" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_msl" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_parser" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_reflect" OBJ_EXT
    };
    int count = sizeof(sources) / sizeof(sources[0]);
    int i;
    int any_rebuilt = 0;
    char cmd[CMD_MAX];
    FILE *rsp;

    printf("\n=== Building SPIRV-Cross ===\n");
    if (ensure_dirs() != 0) return 1;

    for (i = 0; i < count; i++) {
        if (!needs_rebuild(sources[i], objs[i]))
            continue;

#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "%s -std=c++17 -O2 -c "
            "-DSPVC_EXPORT_SYMBOLS "
            "-DSPIRV_CROSS_C_API_GLSL=1 "
            "-DSPIRV_CROSS_C_API_HLSL=1 "
            "-DSPIRV_CROSS_C_API_MSL=1 "
            "-DSPIRV_CROSS_C_API_CPP=1 "
            "-DSPIRV_CROSS_C_API_REFLECT=1 "
            "-Ilib/SDL_shadercross/external/SPIRV-Cross "
            "-o %s "
            "%s",
            tool_cxx, objs[i], sources[i]);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -std=c++17 -fPIC -O2 -c "
            "-DSPVC_EXPORT_SYMBOLS "
            "-DSPIRV_CROSS_C_API_GLSL=1 "
            "-DSPIRV_CROSS_C_API_HLSL=1 "
            "-DSPIRV_CROSS_C_API_MSL=1 "
            "-DSPIRV_CROSS_C_API_CPP=1 "
            "-DSPIRV_CROSS_C_API_REFLECT=1 "
            "-Ilib/SDL_shadercross/external/SPIRV-Cross "
            "-o %s "
            "%s",
            tool_cxx, objs[i], sources[i]);
#endif
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    if (any_rebuilt ||
#ifdef _WIN32
        needs_rebuild(objs[0], DEBUG_DIR "/spirv-cross-c-shared" DLL_EXT)) {
#else
        needs_rebuild(objs[0], DEBUG_DIR "/libspirv-cross-c-shared" DLL_EXT)) {
#endif

        /* Write response file with all .obj paths */
        rsp = fopen(OBJ_SPIRVCROSS_DIR "/spirvcross_objs.txt", "w");
        if (!rsp) {
            printf("!! failed to create response file\n");
            return 1;
        }
        for (i = 0; i < count; i++) {
            fprintf(rsp, "%s\n", objs[i]);
        }
        fclose(rsp);

#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "%s -shared -g "
            "-o " DEBUG_DIR "/spirv-cross-c-shared" DLL_EXT " "
            "@" OBJ_SPIRVCROSS_DIR "/spirvcross_objs.txt "
            "-lc++ "
            "-Wl,--out-implib," DEBUG_DIR "/spirv-cross-c-shared" LIB_EXT,
            tool_link);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -shared -o " DEBUG_DIR "/libspirv-cross-c-shared" DLL_EXT " "
            "@" OBJ_SPIRVCROSS_DIR "/spirvcross_objs.txt",
            tool_cxx);
#endif
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   SPIRV-Cross is up to date.\n");
    }

    return 0;
}

/* ------- shadercross (DLL) ---------------------------------------------- */
static int build_shadercross(void)
{
    char cmd[CMD_MAX];
    int any_rebuilt = 0;

    printf("\n=== Building SDL_shadercross ===\n");
    if (ensure_dirs() != 0) return 1;

    if (needs_rebuild("lib/SDL_shadercross/src/SDL_shadercross.c",
                      OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT)) {
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "%s -O2 -c "
            "-DDLL_EXPORT "
            "-DSDL_SHADERCROSS_DXC "
            "-Ilib/SDL_shadercross/include "
            "-Ilib/SDL3/include "
            "-Ilib/SDL_shadercross/external/SPIRV-Cross "
            "-Ilib/SDL_shadercross/external/prebuilt/inc "
            "-o " OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT " "
            "lib/SDL_shadercross/src/SDL_shadercross.c",
            tool_cc);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -fPIC -O2 -c "
            "-DDLL_EXPORT "
            "-Ilib/SDL_shadercross/include "
            "-Ilib/SDL3/include "
            "-Ilib/SDL_shadercross/external/SPIRV-Cross "
            "-Ilib/SDL_shadercross/external/prebuilt/inc "
            "-o " OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT " "
            "lib/SDL_shadercross/src/SDL_shadercross.c",
            tool_cc);
#endif
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    if (any_rebuilt ||
#ifdef _WIN32
        needs_rebuild(OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT,
                      DEBUG_DIR "/SDL3_shadercross" DLL_EXT)) {
        snprintf(cmd, sizeof(cmd),
            "%s -shared -g "
            "-o " DEBUG_DIR "/SDL3_shadercross" DLL_EXT " "
            OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT " "
            "-L" DEBUG_DIR " -lSDL3 -lspirv-cross-c-shared "
            "lib/SDL_shadercross/external/prebuilt/lib/x64/dxcompiler.lib "
            "-Wl,--out-implib," DEBUG_DIR "/SDL3_shadercross" LIB_EXT,
            tool_link);
        if (run_cmd(cmd) != 0) return 1;

        /* Generate .def for TCC linking */
        generate_def_from_dll(DEBUG_DIR "/SDL3_shadercross.dll",
                              DEBUG_DIR "/SDL3_shadercross.def",
                              "SDL3_shadercross.dll");
    } else {
        printf("   SDL_shadercross is up to date.\n");
    }

    /* Ensure .def file exists even when shadercross wasn't rebuilt */
    {
        FILE *chk = fopen(DEBUG_DIR "/SDL3_shadercross.def", "r");
        if (chk) {
            fclose(chk);
        } else {
            generate_def_from_dll(DEBUG_DIR "/SDL3_shadercross.dll",
                                  DEBUG_DIR "/SDL3_shadercross.def",
                                  "SDL3_shadercross.dll");
        }
    }

    /* Copy DXC runtime DLLs to build output */
    run_cmd("copy lib\\SDL_shadercross\\external\\prebuilt\\bin\\x64\\dxcompiler.dll "
            DEBUG_DIR "\\");
    run_cmd("copy lib\\SDL_shadercross\\external\\prebuilt\\bin\\x64\\dxil.dll "
            DEBUG_DIR "\\");
#else
        needs_rebuild(OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT,
                      DEBUG_DIR "/libSDL3_shadercross" DLL_EXT)) {
        snprintf(cmd, sizeof(cmd),
            "%s -shared -o " DEBUG_DIR "/libSDL3_shadercross" DLL_EXT " "
            OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT " "
            "-L" DEBUG_DIR " -lSDL3 -lspirv-cross-c-shared",
            tool_link);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   SDL_shadercross is up to date.\n");
    }
    /* No DXC on Unix */
#endif

    return 0;
}

/* ------- shaders -------------------------------------------------------- */
#define SHADER_SRC_DIR   "assets/shaders"
#define SHADER_OUT_DIR   "assets/shaders/compiled"

static const char *shader_sources[] = {
    "sprite_vs.glsl",
    "sprite_fs.glsl",
    "debug_lines_vs.glsl",
    "debug_lines_fs.glsl",
    "ui_rect_vs.glsl",
    "ui_rect_fs.glsl",
    "mesh_vs.glsl",
    "mesh_fs.glsl",
    "editor_line_vs.glsl",
    "composite_vs.glsl",
    "composite_fs.glsl",
    "slug_vs.glsl",
    "slug_fs.glsl",
    NULL
};

static int build_shaders(void)
{
    char cmd[CMD_MAX];
    char src[PATH_SIZE], dst[PATH_SIZE];
    const char *stage;
    int i;

    printf("=== Compiling GLSL -> SPIR-V ===\n");

    if (ensure_dir(SHADER_OUT_DIR)) return 1;

    for (i = 0; shader_sources[i]; i++) {
        const char *name = shader_sources[i];
        size_t len = strlen(name);

        /* Determine stage from filename suffix: _vs.glsl = vertex, _fs.glsl = fragment */
        if (len > 8 && name[len - 8] == '_' && name[len - 7] == 'v' && name[len - 6] == 's')
            stage = "vertex";
        else if (len > 8 && name[len - 8] == '_' && name[len - 7] == 'f' && name[len - 6] == 's')
            stage = "fragment";
        else {
            printf("!! cannot determine shader stage for: %s\n", name);
            return 1;
        }

        snprintf(src, sizeof(src), "%s/%s", SHADER_SRC_DIR, name);

        /* Build output name: replace .glsl with .spv */
        snprintf(dst, sizeof(dst), "%s/", SHADER_OUT_DIR);
        {
            size_t base_len = len - 5; /* strip ".glsl" */
            strncat(dst, name, base_len);
            strcat(dst, ".spv");
        }

        if (!needs_rebuild(src, dst)) continue;

        snprintf(cmd, sizeof(cmd), "glslc -fshader-stage=%s %s -o %s", stage, src, dst);
        if (run_cmd(cmd) != 0) return 1;
    }

    printf("   Shaders compiled.\n");
    return 0;
}

/* ------- server (collab relay) ------------------------------------------ */
static int build_server(void)
{
    char cmd[CMD_MAX];

    printf("=== Building collab server ===\n\n");

#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
        "%s -O2 "
        "-o " DEBUG_DIR "/collab_server.exe "
        "-Isrc -Ilib/toml-c "
        "server/collab_server.c src/project.c "
        "-lws2_32",
        tool_cc);
#else
    snprintf(cmd, sizeof(cmd),
        "cc -O2 -o " DEBUG_DIR "/collab_server"
        " -Isrc -Ilib/toml-c"
        " server/collab_server.c src/project.c");
#endif
    printf(">> %s\n", cmd);
    if (system(cmd) != 0) {
        printf("!! Server build failed.\n");
        return 1;
    }
    printf("   Server built: " DEBUG_DIR "/collab_server%s\n",
#ifdef _WIN32
        ".exe"
#else
        ""
#endif
    );
    return 0;
}

/* ------- nanoprof2chrome (standalone tool) ------------------------------- */
static int build_nanoprof2chrome(void)
{
    char cmd[CMD_MAX];
    const char *out_exe = DEBUG_DIR "/nanoprof2chrome"
#ifdef _WIN32
        ".exe"
#endif
    ;

    printf("\n=== Building nanoprof2chrome ===\n");

    if (!needs_rebuild("lib/nanoprof/nanoprof2chrome.c", out_exe) && !force_rebuild) {
        printf("   nanoprof2chrome is up to date.\n");
        return 0;
    }

#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
        "tcc\\tcc.exe -Blib/tcc-windows"
        " -o %s"
        " lib/nanoprof/nanoprof2chrome.c", out_exe);
#elif defined(__APPLE__)
    snprintf(cmd, sizeof(cmd),
        "lib/tcc/macos/tcc -Blib/tcc/macos"
        " -o %s"
        " lib/nanoprof/nanoprof2chrome.c", out_exe);
#else
    snprintf(cmd, sizeof(cmd),
        "tcc/tcc -Blib/tcc-linux"
        " -o %s"
        " lib/nanoprof/nanoprof2chrome.c", out_exe);
#endif

    printf(">> %s\n", cmd);
    if (system(cmd) != 0) {
        printf("!! nanoprof2chrome build failed.\n");
        return 1;
    }
    printf("   Built: %s\n", out_exe);
    return 0;
}

/* ------- remote (cross-compile + deploy + run on Raspberry Pi) ---------- */

typedef struct {
    char host[256];
    char user[128];
    char deploy_path[PATH_SIZE];
} remote_config;

static int read_remote_config(remote_config *cfg)
{
    FILE *fp;
    char line[512];

    memset(cfg, 0, sizeof(*cfg));
    fp = fopen(REMOTE_CONFIG_FILE, "r");
    if (!fp) {
        printf("!! Could not open %s\n", REMOTE_CONFIG_FILE);
        return 1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *eq;
        char *val_start, *val_end;

        /* skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '[' || *p == '\0' || *p == '\n') continue;

        eq = strchr(p, '=');
        if (!eq) continue;

        /* extract key (trim trailing spaces before '=') */
        {
            char *key_end = eq - 1;
            while (key_end > p && (*key_end == ' ' || *key_end == '\t')) key_end--;
            *(key_end + 1) = '\0';
        }

        /* extract value (trim spaces, quotes, newline) */
        val_start = eq + 1;
        while (*val_start == ' ' || *val_start == '\t') val_start++;
        if (*val_start == '"') val_start++;
        val_end = val_start + strlen(val_start);
        while (val_end > val_start &&
               (val_end[-1] == '\n' || val_end[-1] == '\r' ||
                val_end[-1] == '"' || val_end[-1] == ' '))
            val_end--;
        *val_end = '\0';

        if (strcmp(p, "host") == 0)
            snprintf(cfg->host, sizeof(cfg->host), "%s", val_start);
        else if (strcmp(p, "user") == 0)
            snprintf(cfg->user, sizeof(cfg->user), "%s", val_start);
        else if (strcmp(p, "deploy_path") == 0)
            snprintf(cfg->deploy_path, sizeof(cfg->deploy_path), "%s", val_start);
    }

    fclose(fp);

    if (!cfg->host[0] || !cfg->user[0] || !cfg->deploy_path[0]) {
        printf("!! %s must define host, user, and deploy_path\n", REMOTE_CONFIG_FILE);
        return 1;
    }
    return 0;
}

/* Linux SDL3 platform sources — needed for cross-compilation from Windows.
 * This mirrors build_linux.c sdl3_sources_platform[] and must be kept in sync. */
static const char *sdl3_remote_platform_sources[] = {
    "lib/SDL3/src/audio/alsa/SDL_alsa_audio.c",
    "lib/SDL3/src/audio/pulseaudio/SDL_pulseaudio.c",
    "lib/SDL3/src/audio/dummy/SDL_dummyaudio.c",
    "lib/SDL3/src/audio/disk/SDL_diskaudio.c",
    "lib/SDL3/src/camera/v4l2/SDL_camera_v4l2.c",
    "lib/SDL3/src/camera/dummy/SDL_camera_dummy.c",
    "lib/SDL3/src/core/linux/SDL_dbus.c",
    "lib/SDL3/src/core/linux/SDL_evdev.c",
    "lib/SDL3/src/core/linux/SDL_evdev_capabilities.c",
    "lib/SDL3/src/core/linux/SDL_evdev_kbd.c",
    "lib/SDL3/src/core/linux/SDL_progressbar.c",
    "lib/SDL3/src/core/linux/SDL_threadprio.c",
    "lib/SDL3/src/core/linux/SDL_udev.c",
    "lib/SDL3/src/core/unix/SDL_appid.c",
    "lib/SDL3/src/core/unix/SDL_poll.c",
    "lib/SDL3/src/core/unix/SDL_gtk.c",
    "lib/SDL3/src/dialog/unix/SDL_portaldialog.c",
    "lib/SDL3/src/dialog/unix/SDL_unixdialog.c",
    "lib/SDL3/src/dialog/unix/SDL_zenitydialog.c",
    "lib/SDL3/src/dialog/unix/SDL_zenitymessagebox.c",
    "lib/SDL3/src/filesystem/unix/SDL_sysfilesystem.c",
    "lib/SDL3/src/filesystem/posix/SDL_sysfsops.c",
    "lib/SDL3/src/haptic/linux/SDL_syshaptic.c",
    "lib/SDL3/src/haptic/hidapi/SDL_hidapihaptic.c",
    "lib/SDL3/src/haptic/hidapi/SDL_hidapihaptic_lg4ff.c",
    "lib/SDL3/src/io/generic/SDL_asyncio_generic.c",
    "lib/SDL3/src/joystick/linux/SDL_sysjoystick.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapijoystick.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_combined.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_gamecube.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_luna.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_ps3.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_ps4.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_ps5.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_rumble.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_shield.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_stadia.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_steam.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_steam_hori.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_steamdeck.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_switch.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_wii.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_xbox360.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_xbox360w.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_xboxone.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_gip.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_steam_triton.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_lg4ff.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_8bitdo.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_flydigi.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_gamesir.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_sinput.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_zuiki.c",
    "lib/SDL3/src/joystick/hidapi/SDL_report_descriptor.c",
    "lib/SDL3/src/joystick/virtual/SDL_virtualjoystick.c",
    "lib/SDL3/src/joystick/dummy/SDL_sysjoystick.c",
    "lib/SDL3/src/loadso/dlopen/SDL_sysloadso.c",
    "lib/SDL3/src/locale/unix/SDL_syslocale.c",
    "lib/SDL3/src/main/generic/SDL_sysmain_callbacks.c",
    "lib/SDL3/src/misc/unix/SDL_sysurl.c",
    "lib/SDL3/src/power/linux/SDL_syspower.c",
    "lib/SDL3/src/process/posix/SDL_posixprocess.c",
    "lib/SDL3/src/sensor/dummy/SDL_dummysensor.c",
    "lib/SDL3/src/storage/generic/SDL_genericstorage.c",
    "lib/SDL3/src/storage/steam/SDL_steamstorage.c",
    "lib/SDL3/src/thread/pthread/SDL_syscond.c",
    "lib/SDL3/src/thread/pthread/SDL_sysmutex.c",
    "lib/SDL3/src/thread/pthread/SDL_sysrwlock.c",
    "lib/SDL3/src/thread/pthread/SDL_syssem.c",
    "lib/SDL3/src/thread/pthread/SDL_systhread.c",
    "lib/SDL3/src/thread/pthread/SDL_systls.c",
    "lib/SDL3/src/time/unix/SDL_systime.c",
    "lib/SDL3/src/timer/unix/SDL_systimer.c",
    "lib/SDL3/src/tray/unix/SDL_tray.c",
    /* Video - X11 (requires sysroot sync from Pi for X11 headers) */
    "lib/SDL3/src/video/x11/edid-parse.c",
    "lib/SDL3/src/video/x11/SDL_x11clipboard.c",
    "lib/SDL3/src/video/x11/SDL_x11dyn.c",
    "lib/SDL3/src/video/x11/SDL_x11events.c",
    "lib/SDL3/src/video/x11/SDL_x11framebuffer.c",
    "lib/SDL3/src/video/x11/SDL_x11keyboard.c",
    "lib/SDL3/src/video/x11/SDL_x11messagebox.c",
    "lib/SDL3/src/video/x11/SDL_x11modes.c",
    "lib/SDL3/src/video/x11/SDL_x11mouse.c",
    "lib/SDL3/src/video/x11/SDL_x11opengl.c",
    "lib/SDL3/src/video/x11/SDL_x11opengles.c",
    "lib/SDL3/src/video/x11/SDL_x11pen.c",
    "lib/SDL3/src/video/x11/SDL_x11settings.c",
    "lib/SDL3/src/video/x11/SDL_x11shape.c",
    "lib/SDL3/src/video/x11/SDL_x11toolkit.c",
    "lib/SDL3/src/video/x11/SDL_x11touch.c",
    "lib/SDL3/src/video/x11/SDL_x11video.c",
    "lib/SDL3/src/video/x11/SDL_x11vulkan.c",
    "lib/SDL3/src/video/x11/SDL_x11window.c",
    "lib/SDL3/src/video/x11/SDL_x11xfixes.c",
    "lib/SDL3/src/video/x11/SDL_x11xinput2.c",
    "lib/SDL3/src/video/x11/SDL_x11xsync.c",
    "lib/SDL3/src/video/x11/SDL_x11xtest.c",
    "lib/SDL3/src/video/x11/xsettings-client.c"
};

/* Sync system headers from the Raspberry Pi into a local sysroot for cross-
 * compilation. Cached in build/remote/sysroot — only downloads once.
 * Run `build.exe remote-sync` to force a re-sync (e.g. after apt-get install). */
#define REMOTE_SYSROOT      REMOTE_DIR "/sysroot"
#define REMOTE_SYSROOT_INC  REMOTE_SYSROOT "/usr/include"

static int build_remote_sync_sysroot(const remote_config *cfg)
{
    char cmd[CMD_MAX];
    FILE *marker;

    /* Check if sysroot already exists (cached) */
    marker = fopen(REMOTE_SYSROOT "/.synced", "r");
    if (marker) {
        fclose(marker);
        printf("   Sysroot cached (delete " REMOTE_SYSROOT " to re-sync).\n");
        return 0;
    }

    printf("\n=== Syncing sysroot from %s ===\n", cfg->host);
    printf("   This downloads /usr/include from the Pi (one-time).\n");
    printf("   Make sure dev packages are installed on the Pi:\n");
    printf("     sudo apt-get install libx11-dev libxext-dev libxfixes-dev\n");
    printf("     libxi-dev libxrandr-dev libxcursor-dev libxss-dev\n");
    printf("     libpulse-dev libasound2-dev libgl-dev\n");

    if (ensure_dir(REMOTE_SYSROOT)) return 1;
    if (ensure_dir(REMOTE_SYSROOT "/usr")) return 1;

    /* Download only the specific headers we need from the Pi — NOT all of
     * /usr/include, which would conflict with zig's bundled C library headers.
     * -h dereferences symlinks (asm/ has symlinks that Windows can't create). */
    snprintf(cmd, sizeof(cmd),
        "ssh %s@%s \"tar chf -"
        " /usr/include/X11"
        " /usr/include/xcb"
        " /usr/include/alsa"
        " /usr/include/pulse"
        " /usr/include/GL"
        " /usr/include/EGL"
        " /usr/include/GLES2"
        " /usr/include/GLES3"
        " /usr/include/xkbcommon"
        " 2>/dev/null\" | tar xf - -C " REMOTE_SYSROOT,
        cfg->user, cfg->host);
    if (run_cmd(cmd) != 0) {
        printf("!! Failed to sync sysroot. Install dev packages on Pi and retry.\n");
        return 1;
    }

    /* Write marker so we don't re-sync next time */
    marker = fopen(REMOTE_SYSROOT "/.synced", "w");
    if (marker) {
        fprintf(marker, "synced\n");
        fclose(marker);
    }

    printf("   Sysroot sync complete.\n");
    return 0;
}

/* Cross-compile external libraries (SDL3, SPIRV-Cross, shadercross, HarfBuzz)
 * for aarch64-linux-gnu using zig cc. Uses response files for SDL3 due to
 * the large number of source files (~300). */
static int build_remote_externals(void)
{
    char cmd[CMD_MAX];
    FILE *rsp;
    int i, common_count, platform_count;

    printf("\n=== Cross-compiling externals for aarch64-linux-gnu ===\n");

    /* --- 1. SDL3 --------------------------------------------------------- */
    printf("   [1/4] SDL3...\n");
    {
        /* Write response file with all SDL3 sources */
        rsp = fopen(REMOTE_DIR "/sdl3_sources.txt", "w");
        if (!rsp) { printf("!! failed to create SDL3 response file\n"); return 1; }

        common_count = sizeof(sdl3_sources_common) / sizeof(sdl3_sources_common[0]);
        platform_count = sizeof(sdl3_remote_platform_sources) / sizeof(sdl3_remote_platform_sources[0]);

        for (i = 0; i < common_count; i++)
            fprintf(rsp, "%s\n", sdl3_sources_common[i]);
        for (i = 0; i < platform_count; i++)
            fprintf(rsp, "%s\n", sdl3_remote_platform_sources[i]);
        fclose(rsp);

        snprintf(cmd, sizeof(cmd),
            "%s -target aarch64-linux-gnu -shared -O2"
            " -DSDL_BUILDING_SDL3 -DDLL_EXPORT"
            " -include include/SDL_build_config_remote.h"
            " -isystem " REMOTE_SYSROOT_INC
            " -Ilib/SDL3/include -Ilib/SDL3/include/build_config"
            " -Ilib/SDL3/src -Ilib/SDL3/src/video/khronos"
            " -Wl,-soname,libSDL3.so"
            " -o " REMOTE_DIR "/libSDL3.so"
            " @" REMOTE_DIR "/sdl3_sources.txt"
            " -lpthread -ldl -lm",
            tool_cc);
        if (run_cmd(cmd) != 0) { printf("!! SDL3 cross-compile failed\n"); return 1; }
    }

    /* --- 2. SPIRV-Cross -------------------------------------------------- */
    printf("   [2/4] SPIRV-Cross...\n");
    snprintf(cmd, sizeof(cmd),
        "%s -target aarch64-linux-gnu -shared -std=c++17 -O2"
        " -DSPVC_EXPORT_SYMBOLS"
        " -DSPIRV_CROSS_C_API_GLSL=1 -DSPIRV_CROSS_C_API_HLSL=1"
        " -DSPIRV_CROSS_C_API_MSL=1 -DSPIRV_CROSS_C_API_CPP=1"
        " -DSPIRV_CROSS_C_API_REFLECT=1"
        " -Ilib/SDL_shadercross/external/SPIRV-Cross"
        " -Wl,-soname,libspirv-cross-c-shared.so"
        " -o " REMOTE_DIR "/libspirv-cross-c-shared.so"
        " lib/SDL_shadercross/external/SPIRV-Cross/spirv_cfg.cpp"
        " lib/SDL_shadercross/external/SPIRV-Cross/spirv_cpp.cpp"
        " lib/SDL_shadercross/external/SPIRV-Cross/spirv_cross.cpp"
        " lib/SDL_shadercross/external/SPIRV-Cross/spirv_cross_c.cpp"
        " lib/SDL_shadercross/external/SPIRV-Cross/spirv_cross_parsed_ir.cpp"
        " lib/SDL_shadercross/external/SPIRV-Cross/spirv_cross_util.cpp"
        " lib/SDL_shadercross/external/SPIRV-Cross/spirv_glsl.cpp"
        " lib/SDL_shadercross/external/SPIRV-Cross/spirv_hlsl.cpp"
        " lib/SDL_shadercross/external/SPIRV-Cross/spirv_msl.cpp"
        " lib/SDL_shadercross/external/SPIRV-Cross/spirv_parser.cpp"
        " lib/SDL_shadercross/external/SPIRV-Cross/spirv_reflect.cpp",
        tool_cxx);
    if (run_cmd(cmd) != 0) { printf("!! SPIRV-Cross cross-compile failed\n"); return 1; }

    /* --- 3. SDL_shadercross ---------------------------------------------- */
    printf("   [3/4] SDL_shadercross...\n");
    snprintf(cmd, sizeof(cmd),
        "%s -target aarch64-linux-gnu -shared -O2"
        " -DDLL_EXPORT"
        " -Ilib/SDL_shadercross/include"
        " -Ilib/SDL3/include"
        " -Ilib/SDL_shadercross/external/SPIRV-Cross"
        " -Ilib/SDL_shadercross/external/prebuilt/inc"
        " -Wl,-soname,libSDL3_shadercross.so"
        " -o " REMOTE_DIR "/libSDL3_shadercross.so"
        " lib/SDL_shadercross/src/SDL_shadercross.c"
        " -L" REMOTE_DIR " -lSDL3 -lspirv-cross-c-shared",
        tool_cc);
    if (run_cmd(cmd) != 0) { printf("!! SDL_shadercross cross-compile failed\n"); return 1; }

    /* --- 4. HarfBuzz ----------------------------------------------------- */
    printf("   [4/4] HarfBuzz...\n");
    snprintf(cmd, sizeof(cmd),
        "%s -target aarch64-linux-gnu -shared -std=c++20 -O2"
        " -DHB_TINY -DHB_NO_MT"
        " -Ilib/harfbuzz-src/src"
        " -Wl,-soname,libharfbuzz.so"
        " -o " REMOTE_DIR "/libharfbuzz.so"
        " lib/harfbuzz-src/src/harfbuzz.cc",
        tool_cxx);
    if (run_cmd(cmd) != 0) { printf("!! HarfBuzz cross-compile failed\n"); return 1; }

    printf("   Externals cross-compilation complete.\n");
    return 0;
}

static int build_remote_compile(void)
{
    char cmd[CMD_MAX];

    printf("\n=== Cross-compiling for aarch64-linux-gnu ===\n");

    if (ensure_dir(REMOTE_DIR)) return 1;

    /* Cross-compile external libraries first */
    if (build_remote_externals() != 0) return 1;

    /* 1. core.so (includes externals) — link against cross-compiled libs */
    snprintf(cmd, sizeof(cmd),
        "%s -target aarch64-linux-gnu -shared -fno-sanitize=undefined"
        " -Wl,-soname,libcore.so"
        " -o " REMOTE_DIR "/libcore.so"
        " -DSTBI_NO_SIMD -DCLAY_DISABLE_SIMD"
        " " COMMON_INCLUDES
        " -Ilib/sqlite -Ilib/toml-c -Ilib/nanoprof"
        " src/core/core.c"
        " src/core/loadlibrary_linux.cpp"
        " src/core/externals_runtime.c src/core/externals.c"
        " src/project.c lib/sqlite/sqlite3.c"
        " -L" REMOTE_DIR " -lSDL3 -lSDL3_shadercross -lharfbuzz"
        " -Wl,-rpath,$ORIGIN"
        " -lpthread -ldl -lm",
        tool_cc);
    if (run_cmd(cmd) != 0) { printf("!! core cross-compile failed\n"); return 1; }

    /* 3. engine.so */
    snprintf(cmd, sizeof(cmd),
        "%s -target aarch64-linux-gnu -shared -fno-sanitize=undefined"
        " -Wl,-soname,libengine.so"
        " -o " REMOTE_DIR "/libengine.so"
        " -Isrc -Isrc/engine -Isrc/editor -Ilib/SDL3/include -Ilib/cgltf"
        " src/engine/anim.c src/engine/debug_render.c src/engine/engine.c"
        " src/engine/gltf_loader.c src/engine/physics.c"
        " -Wl,--allow-shlib-undefined",
        tool_cc);
    if (run_cmd(cmd) != 0) { printf("!! engine cross-compile failed\n"); return 1; }

    /* 4. editor.so */
    snprintf(cmd, sizeof(cmd),
        "%s -target aarch64-linux-gnu -shared -fno-sanitize=undefined"
        " -Wl,-soname,libeditor.so"
        " -o " REMOTE_DIR "/libeditor.so"
        " -DCLAY_DISABLE_SIMD -DCPU_PROF_USE_FPTRS"
        " -Isrc -Isrc/editor -Isrc/engine -Isrc/collab"
        " -Ilib/SDL3/include -Ilib/clay"
        " src/editor/editor.c"
        " src/collab/collab_ops.c"
        " src/collab/collab_client.c"
        " -Wl,--allow-shlib-undefined",
        tool_cc);
    if (run_cmd(cmd) != 0) { printf("!! editor cross-compile failed\n"); return 1; }

    /* 5. AnitraEngine (exe) */
    snprintf(cmd, sizeof(cmd),
        "%s -target aarch64-linux-gnu -fno-sanitize=undefined"
        " -o " REMOTE_DIR "/AnitraEngine"
        " -Isrc -Isrc/core -Isrc/engine -Isrc/editor"
        " -Ilib/SDL3/include"
        " src/main.c"
        " src/core/loadlibrary_linux.cpp"
        " -ldl",
        tool_cc);
    if (run_cmd(cmd) != 0) { printf("!! exe cross-compile failed\n"); return 1; }

    printf("   Cross-compilation complete.\n");
    return 0;
}

static int build_remote_deploy(const remote_config *cfg, const char *project_dir)
{
    char cmd[CMD_MAX];

    printf("\n=== Deploying to %s@%s:%s ===\n", cfg->user, cfg->host, cfg->deploy_path);

    /* Create remote directory structure */
    snprintf(cmd, sizeof(cmd),
        "ssh %s@%s \"mkdir -p %s/build/Debug\"",
        cfg->user, cfg->host,
        cfg->deploy_path);
    if (run_cmd(cmd) != 0) { printf("!! Failed to create remote dirs\n"); return 1; }

    /* Copy binaries + external libraries */
    snprintf(cmd, sizeof(cmd),
        "scp"
        " " REMOTE_DIR "/AnitraEngine"
        " " REMOTE_DIR "/libcore.so"
        " " REMOTE_DIR "/libengine.so"
        " " REMOTE_DIR "/libeditor.so"
        " " REMOTE_DIR "/libSDL3.so"
        " " REMOTE_DIR "/libspirv-cross-c-shared.so"
        " " REMOTE_DIR "/libSDL3_shadercross.so"
        " " REMOTE_DIR "/libharfbuzz.so"
        " %s@%s:%s/build/Debug/",
        cfg->user, cfg->host, cfg->deploy_path);
    if (run_cmd(cmd) != 0) { printf("!! Failed to deploy binaries\n"); return 1; }

    /* Copy all assets (shaders, fonts, textures, etc.) */
    snprintf(cmd, sizeof(cmd),
        "scp -r assets"
        " %s@%s:%s/",
        cfg->user, cfg->host, cfg->deploy_path);
    if (run_cmd(cmd) != 0) { printf("!! Failed to deploy assets\n"); return 1; }

    /* Copy game_assets (3D models, animations, etc.) */
    snprintf(cmd, sizeof(cmd),
        "scp -r game_assets"
        " %s@%s:%s/",
        cfg->user, cfg->host, cfg->deploy_path);
    if (run_cmd(cmd) != 0) { printf("!! Failed to deploy game_assets\n"); return 1; }

    /* Copy project directory if available */
    if (project_dir && project_dir[0]) {
        /* Extract the top-level directory name (e.g. "dungeon1" from "dungeon1/project.toml") */
        char proj_dir_name[PATH_SIZE];
        const char *slash = strchr(project_dir, '/');
        if (!slash) slash = strchr(project_dir, '\\');
        if (slash) {
            size_t len = (size_t)(slash - project_dir);
            if (len >= sizeof(proj_dir_name)) len = sizeof(proj_dir_name) - 1;
            memcpy(proj_dir_name, project_dir, len);
            proj_dir_name[len] = '\0';
        } else {
            snprintf(proj_dir_name, sizeof(proj_dir_name), "%s", project_dir);
        }

        snprintf(cmd, sizeof(cmd),
            "scp -r %s %s@%s:%s/",
            proj_dir_name, cfg->user, cfg->host, cfg->deploy_path);
        if (run_cmd(cmd) != 0) { printf("!! Failed to deploy project files\n"); return 1; }
    }

    printf("   Deploy complete.\n");
    return 0;
}

static int build_remote_run(const remote_config *cfg, const char *project_include)
{
    char cmd[CMD_MAX];

    printf("\n=== Running on %s ===\n", cfg->host);

    snprintf(cmd, sizeof(cmd),
        "ssh %s@%s \"chmod +x %s/build/Debug/AnitraEngine\"",
        cfg->user, cfg->host, cfg->deploy_path);
    run_cmd(cmd);

    if (project_include && project_include[0]) {
        snprintf(cmd, sizeof(cmd),
            "ssh %s@%s \"cd %s && DISPLAY=:0 LD_LIBRARY_PATH=%s/build/Debug ./build/Debug/AnitraEngine %s\"",
            cfg->user, cfg->host, cfg->deploy_path, cfg->deploy_path, project_include);
    } else {
        snprintf(cmd, sizeof(cmd),
            "ssh %s@%s \"cd %s && DISPLAY=:0 LD_LIBRARY_PATH=%s/build/Debug ./build/Debug/AnitraEngine\"",
            cfg->user, cfg->host, cfg->deploy_path, cfg->deploy_path);
    }
    return run_cmd(cmd);
}

static int build_remote(void)
{
    remote_config cfg;
    char project_path[PATH_SIZE];
    const char *project_include = NULL;

    printf("=== Building remote (Raspberry Pi) ===\n");

    if (read_remote_config(&cfg) != 0) return 1;

    /* Resolve project path */
    project_path[0] = '\0';
    if (read_project_include_path(project_path, sizeof(project_path))) {
        project_include = project_path;
    } else if (file_exists_regular(DEFAULT_PROJECT_TOML)) {
        snprintf(project_path, sizeof(project_path), "%s", DEFAULT_PROJECT_TOML);
        project_include = project_path;
    }

    /* Compile shaders (SPIR-V is platform-agnostic) */
    if (build_shaders() != 0) return 1;

    /* Sync sysroot from Pi (cached — one-time download of /usr/include) */
    if (build_remote_sync_sysroot(&cfg) != 0) return 1;

    /* Cross-compile for aarch64 */
    if (build_remote_compile() != 0) return 1;

    /* Deploy to Pi */
    if (build_remote_deploy(&cfg, project_include) != 0) return 1;

    /* Run on Pi */
    return build_remote_run(&cfg, project_include);
}

/* ------- all ------------------------------------------------------------ */
static int build_all(void)
{
    printf("=== Building all targets ===\n\n");

    if (build_sdl3() != 0) return 1;
    if (build_spirvcross() != 0) return 1;
    if (build_shadercross() != 0) return 1;
    if (build_shaders() != 0) return 1;
    if (build_harfbuzz() != 0) return 1;
    force_rebuild = 1;
    if (generate_migration_code() != 0) return 1;
    if (build_core() != 0) return 1;
    if (build_engine() != 0) return 1;
    if (build_editor() != 0) return 1;

    printf("\n=== All targets built successfully. ===\n");
    return 0;
}

/*
 * build_and_run(), build_play_test(), build_collab(), build_test(),
 * build_and_profile(), and build_and_debug() are provided by the
 * platform file included above.
 */

/* ------- debug (zig cc with PDB for raddbg) ------------------------------ */
#ifdef _WIN32
static int run_zig_cmd(const char *label, const char *cmd) {
    printf(">> %s\n", cmd);
    fflush(stdout);
    if (system(cmd) != 0) {
        printf("!! %s build failed.\n", label);
        return 1;
    }
    return 0;
}

static int build_debug(void)
{
    printf("=== Debug build (MSVC + PDB) ===\n\n");

    /* Build libs first (SDL3 etc) with normal build */
    if (build_sdl3() != 0) return 1;
    if (build_spirvcross() != 0) return 1;
    if (build_shadercross() != 0) return 1;
    if (build_shaders() != 0) return 1;
    if (build_harfbuzz() != 0) return 1;
    force_rebuild = 1;
    if (generate_migration_code() != 0) return 1;
    if (build_tracy() != 0) return 1;

    /* Build core + editor with TCC (no PDB needed for those) */
    if (build_core() != 0) return 1;
    if (build_editor() != 0) return 1;

    /* Build engine + exe with MSVC for PDB symbols */
    if (run_zig_cmd("engine", CL_ENGINE_CMD) != 0) return 1;
    if (run_zig_cmd("exe",    CL_EXE_CMD)    != 0) return 1;

    printf("\n=== Debug build complete. ===\n");
    return 0;
}
#endif

/* ========================================================================= */
/* main                                                                       */
/* ========================================================================= */

#ifdef BUILD_DLL
/* DLL entry point — called by anitra.exe via loadlibrary */
__declspec(dllexport) int init_builder(void)
{
    if (find_tools() != 0) return 1;
    return build_all();
}

/* Start forge file-watcher thread (call after init_builder) */
__declspec(dllexport) int start_forge(void)
{
    return start_forge_thread();
}

/* Stop forge file-watcher thread (call before unloading build.dll) */
__declspec(dllexport) void stop_forge(void)
{
    stop_forge_thread();
}
#else
/* EXE entry point — bootstrapped by build.bat */
int main(int argc, char **argv)
{
    if (find_tools() != 0) return 1;
#ifdef _WIN32
    if (argc > 1 && strcmp(argv[1], "debug") == 0)
        return build_debug();
#endif
    return build_all();
}
#endif
