/*
 * build.c - nobuild build system for the Anitra game engine
 *
 * Compile:  build.bat
			 build.sh
			 build.cmd
 * Usage:    builder          (build only)
 *           builder watch    (build, launch engine, and watch for changes)
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
#define PROJECT_INCLUDE_FILE "project.txt"
#define DEFAULT_PROJECT_TOML "dungeon1/project.toml"

/* Common include paths used by externals, core, engine, and exe targets */
#ifdef _WIN32
#define COMMON_INCLUDES \
    "/Isrc /Isrc/core /Isrc/engine /Isrc/editor /Isrc/externals /Iinclude " \
    "/Ilib/SDL3/include /Ilib/SDL_shadercross/include " \
    "/Ilib/SDL_shadercross/external/SPIRV-Cross " \
    "/Ilib/SDL_shadercross/external/prebuilt/inc " \
    "/Ilib/remotery " \
    "/Ilib/harfbuzz-src/src " \
    "/Ilib/clay " \
    "/Ilib/cgltf"
#else
#define COMMON_INCLUDES \
    "-Isrc -Isrc/core -Isrc/engine -Isrc/editor -Isrc/externals -Iinclude " \
    "-Ilib/SDL3/include -Ilib/SDL_shadercross/include " \
    "-Ilib/SDL_shadercross/external/SPIRV-Cross " \
    "-Ilib/SDL_shadercross/external/prebuilt/inc " \
    "-Ilib/remotery " \
    "-Ilib/harfbuzz-src/src " \
    "-Ilib/clay " \
    "-Ilib/cgltf"
#endif

/* Tool paths */
#ifdef _WIN32
static char tool_cc[PATH_SIZE];
static char tool_cxx[PATH_SIZE];
static char tool_link[PATH_SIZE];
static char tool_ar[PATH_SIZE];
static int msvc_tools_ready = 0;
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
static int find_msvc_tools(void)
{
    /*
     * Strategy:
     *  1. Try PATH first (fast path when running from Dev Command Prompt).
     *  2. If cl.exe not in PATH, use vswhere to find VS, then run vcvarsall
     *     to set up the full environment (INCLUDE, LIB, PATH, etc.).
     */
    char cl_path[PATH_SIZE];
    char x64_path[PATH_SIZE];
    char *last_sep;
    char *arch_pos;
    DWORD len;

    len = SearchPathA(NULL, "cl.exe", NULL, PATH_SIZE, cl_path, NULL);

    if (len == 0) {
        /* cl.exe not in PATH -- try to set up MSVC environment automatically */
        char cmd[CMD_MAX];
        FILE *fp;
        char vs_path[PATH_SIZE];
        char vcvarsall[PATH_SIZE];

        /* Use vswhere to find VS installation */
        fp = _popen(
            "\"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe\" "
            "-latest -property installationPath 2>nul", "r");
        if (!fp) {
            printf("!! cl.exe not found and vswhere.exe not available.\n");
            printf("!! Install Visual Studio or run from VS Developer Command Prompt.\n");
            return 1;
        }
        if (!fgets(vs_path, PATH_SIZE, fp)) {
            _pclose(fp);
            printf("!! vswhere.exe found no Visual Studio installation.\n");
            return 1;
        }
        _pclose(fp);

        /* Strip trailing newline */
        {
            size_t slen = strlen(vs_path);
            while (slen > 0 && (vs_path[slen-1] == '\n' || vs_path[slen-1] == '\r'))
                vs_path[--slen] = '\0';
        }

        snprintf(vcvarsall, PATH_SIZE, "%s\\VC\\Auxiliary\\Build\\vcvarsall.bat", vs_path);
        if (GetFileAttributesA(vcvarsall) == INVALID_FILE_ATTRIBUTES) {
            printf("!! vcvarsall.bat not found at: %s\n", vcvarsall);
            return 1;
        }

        /*
         * Run vcvarsall, dump the resulting environment into a temp file,
         * then parse it to set our process environment.
         */
        {
            char tmp_file[PATH_SIZE];
            char line[8192];
            FILE *env_fp;

            GetTempPathA(PATH_SIZE, tmp_file);
            strcat(tmp_file, "anitra_env.txt");

            snprintf(cmd, sizeof(cmd),
                "cmd /C \"call \"%s\" x64 >nul 2>&1 && set > \"%s\"\"",
                vcvarsall, tmp_file);
            if (system(cmd) != 0) {
                printf("!! vcvarsall.bat failed.\n");
                return 1;
            }

            env_fp = fopen(tmp_file, "r");
            if (!env_fp) {
                printf("!! failed to read environment from vcvarsall.\n");
                return 1;
            }

            while (fgets(line, sizeof(line), env_fp)) {
                /* Strip trailing newline */
                size_t slen = strlen(line);
                while (slen > 0 && (line[slen-1] == '\n' || line[slen-1] == '\r'))
                    line[--slen] = '\0';
                /* Only import vars that matter for compilation */
                if (strncmp(line, "PATH=", 5) == 0 ||
                    strncmp(line, "Path=", 5) == 0 ||
                    strncmp(line, "LIB=", 4) == 0 ||
                    strncmp(line, "LIBPATH=", 8) == 0 ||
                    strncmp(line, "INCLUDE=", 8) == 0 ||
                    strncmp(line, "WindowsSdkDir=", 14) == 0 ||
                    strncmp(line, "WindowsSDKVersion=", 18) == 0 ||
                    strncmp(line, "UCRTVersion=", 12) == 0 ||
                    strncmp(line, "VCToolsInstallDir=", 18) == 0) {
                    _putenv(line);
                }
            }
            fclose(env_fp);
            DeleteFileA(tmp_file);
        }

        printf("   MSVC environment set up via vcvarsall.bat\n");

        /* Now cl.exe should be in PATH */
        len = SearchPathA(NULL, "cl.exe", NULL, PATH_SIZE, cl_path, NULL);
        if (len == 0) {
            printf("!! cl.exe still not found after running vcvarsall.\n");
            return 1;
        }
    }

    /*
     * If PATH found the x86 cl.exe (path contains \Hostx86\x86\ or
     * \Hostx64\x86\), try to use the x64 target instead by replacing
     * the last \x86\ with \x64\.
     */
    arch_pos = strstr(cl_path, "\\x86\\cl.exe");
    if (arch_pos) {
        strncpy(x64_path, cl_path, arch_pos - cl_path);
        x64_path[arch_pos - cl_path] = '\0';
        strcat(x64_path, "\\x64\\cl.exe");
        if (GetFileAttributesA(x64_path) != INVALID_FILE_ATTRIBUTES) {
            strcpy(cl_path, x64_path);
            printf("   Using x64 toolchain: %s\n", cl_path);
        }
    }

    /* Strip "cl.exe" to get the directory */
    last_sep = strrchr(cl_path, '\\');
    if (!last_sep) {
        printf("!! unexpected cl.exe path: %s\n", cl_path);
        return 1;
    }
    last_sep[1] = '\0';

    snprintf(tool_cc,   PATH_SIZE, "%scl.exe", cl_path);
    snprintf(tool_cxx,  PATH_SIZE, "%scl.exe", cl_path);
    snprintf(tool_link, PATH_SIZE, "%slink.exe", cl_path);
    snprintf(tool_ar,   PATH_SIZE, "%slib.exe", cl_path);

    return 0;
}

/* Lazy init: only run vcvarsall when an MSVC target actually needs recompiling */
static int ensure_msvc_tools(void)
{
    if (msvc_tools_ready) return 0;
    if (find_msvc_tools() != 0) return 1;
    msvc_tools_ready = 1;
    return 0;
}

static int find_tools(void)
{
    /* On Windows, MSVC tools are set up lazily by ensure_msvc_tools().
       This is a no-op so main() can call it unconditionally. */
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
        if (ensure_msvc_tools() != 0) return 1;
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /std:c++17 /EHsc /MD /Zi /Od /nologo /c "
            "/DHAVE_DIRECTWRITE /DHB_DLL_EXPORT "
            "/Ilib/harfbuzz-src/src "
            "/Fo" OBJ_EXT_DIR "/harfbuzz" OBJ_EXT " "
            "/Fd" OBJ_EXT_DIR "/harfbuzz.pdb "
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
        if (ensure_msvc_tools() != 0) return 1;
        {
            char pdb_suffix[9];
            rand_hex(pdb_suffix, 8);
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /nologo /DLL /DEBUG "
                "/PDB:" DEBUG_DIR "/harfbuzz_%s.pdb "
                "/OUT:" DEBUG_DIR "/" DLL_PREFIX "harfbuzz" DLL_EXT " "
                "/IMPLIB:" DEBUG_DIR "/harfbuzz" LIB_EXT " "
                OBJ_EXT_DIR "/harfbuzz" OBJ_EXT " "
                "dwrite.lib",
                tool_link, pdb_suffix);
        }
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

/* ------- externals (DLL — pure C, compiled by TCC) ---------------------- */
static int build_externals(void)
{
    char cmd[CMD_MAX];

    printf("\n=== Building externals ===\n");

#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
        ".\\tcc.exe -Blib/tcc-windows -shared"
        " -o " DEBUG_DIR "/externals.dll"
        " -DCPU_PROF_USE_REMOTERY -DRMT_USE_OPENGL=0 -DRMT_USE_D3D11=0 -DRMT_USE_METAL=0"
        " -DSTBI_NO_SIMD -DCLAY_DISABLE_SIMD"
        " -Isrc -Isrc/core -Isrc/engine -Isrc/editor -Isrc/externals"
        " -Ilib/SDL3/include -Ilib/SDL_shadercross/include"
        " -Ilib/remotery -Ilib/harfbuzz-src/src -Ilib/clay -Ilib/cgltf"
        " -Ilib/sqlite -Ilib/toml-c"
        " src/externals/externals_runtime.c src/externals/externals.c"
        " src/project.c lib/sqlite/sqlite3.c"
        " lib/remotery/Remotery.c lib/remotery/rmt_tcc_compat.c"
        " " DEBUG_DIR "/SDL3.def"
        " " DEBUG_DIR "/SDL3_shadercross.def"
        " " DEBUG_DIR "/harfbuzz.def"
        " lib/tcc-windows/lib/ws2_32.def"
        " lib/tcc-windows/lib/winmm.def"
        " lib/tcc-windows/lib/kernel32.def");
#elif defined(__APPLE__)
    snprintf(cmd, sizeof(cmd),
        "lib/tcc/macos/tcc -Blib/tcc/macos -shared"
        " -DMAC_OS_X_VERSION_MIN_REQUIRED=1100"
        " -DSTBI_NO_THREAD_LOCALS -DCLAY_DISABLE_SIMD"
        " -o " DEBUG_DIR "/libexternals.dylib"
        " -DCPU_PROF_USE_REMOTERY -DRMT_USE_OPENGL=0 -DRMT_USE_D3D11=0 -DRMT_USE_METAL=0"
        " -Isrc -Isrc/core -Isrc/engine -Isrc/editor -Isrc/externals"
        " -Ilib/SDL3/include -Ilib/SDL_shadercross/include"
        " -Ilib/remotery -Ilib/harfbuzz-src/src -Ilib/clay -Ilib/cgltf"
        " -Ilib/sqlite -Ilib/toml-c"
        " src/externals/externals_runtime.c src/externals/externals.c"
        " src/project.c lib/sqlite/sqlite3.c lib/remotery/Remotery.c"
        " -L" DEBUG_DIR " -lSDL3 -lSDL3_shadercross -lharfbuzz"
        " -lpthread -lm");
#else
    snprintf(cmd, sizeof(cmd),
        "./tcc -Blib/tcc-linux -shared"
        " -o " DEBUG_DIR "/libexternals.so"
        " -DCPU_PROF_USE_REMOTERY -DRMT_USE_OPENGL=0 -DRMT_USE_D3D11=0 -DRMT_USE_METAL=0"
        " -Isrc -Isrc/core -Isrc/engine -Isrc/editor -Isrc/externals"
        " -Ilib/SDL3/include -Ilib/SDL_shadercross/include"
        " -Ilib/remotery -Ilib/harfbuzz-src/src -Ilib/clay -Ilib/cgltf"
        " -Ilib/sqlite -Ilib/toml-c"
        " src/externals/externals_runtime.c src/externals/externals.c"
        " src/project.c lib/sqlite/sqlite3.c lib/remotery/Remotery.c"
        " -L" DEBUG_DIR " -lSDL3 -lSDL3_shadercross -lharfbuzz"
        " -lpthread -ldl -lm");
#endif
    printf(">> %s\n", cmd);
    if (system(cmd) != 0) {
        printf("!! externals build failed\n");
        return 1;
    }

#ifdef _WIN32
    /* Generate .def for TCC linking (core.dll needs it) */
    generate_def_from_dll(DEBUG_DIR "/externals.dll",
                          DEBUG_DIR "/externals.def", "externals_copy.dll");
#endif

    return 0;
}

/* ------- Platform-specific implementations ------------------------------ */
/*
 * Each platform file provides:
 *   - TCC command defines (TCC_COMPILE_CMD, TCC_EDITOR_CMD, etc.)
 *   - sdl3_sources_platform[] array
 *   - watch_and_rebuild() function
 */

/* Forward declaration for platform files */
static int generate_migration_code(void);

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

    /* Skip blank, comments, preprocessor, braces */
    if (*p == '\0' || *p == '/' || *p == '#' || *p == '{' || *p == '}') return 0;

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

    if (g_mig_struct_count == 0) {
        printf("   No migration targets found, skipping codegen.\n");
        return 0;
    }

    f = fopen(output_path, "w");
    if (!f) {
        printf("!! Cannot create %s\n", output_path);
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

    printf("   Generated %s (%d structs)\n", output_path, g_mig_struct_count);
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
static int build_core(void)
{
    printf("\n=== Building core ===\n");
    if (ensure_dirs() != 0) return 1;
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

/* ------- test (compile and run unit tests) ------------------------------- */

static int build_test(void)
{
    printf("\n=== Building tests ===\n");
    if (ensure_dirs() != 0) return 1;

    printf(">> " TCC_TEST_CMD "\n");
    fflush(stdout);
    if (system(TCC_TEST_CMD) != 0) {
        printf("!! test build failed.\n");
        return 1;
    }

    printf("\n=== Running tests ===\n");
    fflush(stdout);
#ifdef _WIN32
    if (system("build\\Debug\\test_dock.exe") != 0) {
#else
    if (system("build/Debug/test_dock") != 0) {
#endif
        printf("!! tests failed.\n");
        return 1;
    }

    return 0;
}

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

#ifdef _WIN32
        if (ensure_msvc_tools() != 0) return 1;
#endif

        /* Detect C++ and Objective-C by extension */
        ext = strrchr(src, '.');
        is_cpp = (ext && strcmp(ext, ".cpp") == 0);

#ifdef _WIN32
        if (is_cpp) {
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /std:c++20 /EHsc /MD /O2 /nologo /c "
                "/DSDL_BUILDING_SDL3 /DDLL_EXPORT /D_WINDOWS /DWIN32 "
                "/Ilib/SDL3/include /Ilib/SDL3/include/build_config "
                "/Ilib/SDL3/src /Ilib/SDL3/src/video/khronos "
                "/Fo%s "
                "/Fd" OBJ_SDL3_DIR "/sdl3.pdb "
                "%s",
                tool_cxx, obj, src);
        } else {
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /TC /MD /O2 /nologo /c "
                "/DSDL_BUILDING_SDL3 /DDLL_EXPORT /D_WINDOWS /DWIN32 "
                "/Ilib/SDL3/include /Ilib/SDL3/include/build_config "
                "/Ilib/SDL3/src /Ilib/SDL3/src/video/khronos "
                "/Fo%s "
                "/Fd" OBJ_SDL3_DIR "/sdl3.pdb "
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
        if (ensure_msvc_tools() != 0) return 1;
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/OUT:" DEBUG_DIR "/SDL3" DLL_EXT " "
            "/IMPLIB:" DEBUG_DIR "/SDL3" LIB_EXT " "
            "@" OBJ_SDL3_DIR "/sdl3_objs.txt "
            "user32.lib gdi32.lib winmm.lib imm32.lib "
            "ole32.lib oleaut32.lib version.lib advapi32.lib "
            "setupapi.lib shell32.lib cfgmgr32.lib",
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
        if (ensure_msvc_tools() != 0) return 1;
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /std:c++17 /EHsc /MD /O2 /nologo /c "
            "/DSPVC_EXPORT_SYMBOLS "
            "/DSPIRV_CROSS_C_API_GLSL=1 "
            "/DSPIRV_CROSS_C_API_HLSL=1 "
            "/DSPIRV_CROSS_C_API_MSL=1 "
            "/DSPIRV_CROSS_C_API_CPP=1 "
            "/DSPIRV_CROSS_C_API_REFLECT=1 "
            "/Ilib/SDL_shadercross/external/SPIRV-Cross "
            "/Fo%s "
            "/Fd" OBJ_SPIRVCROSS_DIR "/spirvcross.pdb "
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
        if (ensure_msvc_tools() != 0) return 1;
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/OUT:" DEBUG_DIR "/spirv-cross-c-shared" DLL_EXT " "
            "/IMPLIB:" DEBUG_DIR "/spirv-cross-c-shared" LIB_EXT " "
            "@" OBJ_SPIRVCROSS_DIR "/spirvcross_objs.txt",
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
        if (ensure_msvc_tools() != 0) return 1;
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /TC /MD /O2 /nologo /c "
            "/DDLL_EXPORT "
            "/DSDL_SHADERCROSS_DXC "
            "/Ilib/SDL_shadercross/include "
            "/Ilib/SDL3/include "
            "/Ilib/SDL_shadercross/external/SPIRV-Cross "
            "/Ilib/SDL_shadercross/external/prebuilt/inc "
            "/Fo" OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT " "
            "/Fd" OBJ_SHADERCROSS_DIR "/shadercross.pdb "
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
        if (ensure_msvc_tools() != 0) return 1;
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/OUT:" DEBUG_DIR "/SDL3_shadercross" DLL_EXT " "
            "/IMPLIB:" DEBUG_DIR "/SDL3_shadercross" LIB_EXT " "
            OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT " "
            DEBUG_DIR "/SDL3" LIB_EXT " "
            DEBUG_DIR "/spirv-cross-c-shared" LIB_EXT " "
            "lib/SDL_shadercross/external/prebuilt/lib/x64/dxcompiler.lib",
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
    "font_vs.glsl",
    "font_fs.glsl",
    "mesh_vs.glsl",
    "mesh_fs.glsl",
    "editor_line_vs.glsl",
    "composite_vs.glsl",
    "composite_fs.glsl",
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
    /* Use MSVC (cl.exe) for the server binary */
    ensure_msvc_tools();
    snprintf(cmd, sizeof(cmd),
        "cl /nologo /O2 /Fe:" DEBUG_DIR "/collab_server.exe"
        " /Isrc /Ilib/toml-c"
        " server/collab_server.c src/project.c"
        " ws2_32.lib"
        " /link /INCREMENTAL:NO");
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
    if (build_externals() != 0) return 1;
    if (generate_migration_code() != 0) return 1;
    if (build_core() != 0) return 1;
    if (build_engine() != 0) return 1;
    if (build_editor() != 0) return 1;
    if (build_exe() != 0) return 1;

    printf("\n=== All targets built successfully. ===\n");
    return 0;
}

/* ------- run ------------------------------------------------------------ */
static int build_and_run(void)
{
    char project_path[PATH_SIZE];
    const char *project_include;

    printf("=== Building and running engine ===\n\n");
    
    /* First build everything */
    if (build_all() != 0) return 1;

    project_include = NULL;
    project_path[0] = '\0';
    if (read_project_include_path(project_path, sizeof(project_path))) {
        project_include = project_path;
        printf("   Using project include from %s: %s\n",
               PROJECT_INCLUDE_FILE, project_include);
    } else if (file_exists_regular(DEFAULT_PROJECT_TOML)) {
        snprintf(project_path, sizeof(project_path), "%s", DEFAULT_PROJECT_TOML);
        project_include = project_path;
        printf("   Using default project include: %s\n", project_include);
    } else {
        printf("   No project include found (checked %s and %s)\n",
               PROJECT_INCLUDE_FILE, DEFAULT_PROJECT_TOML);
    }
    
    printf("\n=== Launching engine and starting watch mode ===\n");

#ifdef _WIN32
    /* Launch the engine in a separate process */
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    char engine_path[PATH_SIZE];
    char engine_cmdline[CMD_MAX];
    
    si.cb = sizeof(si);
    
    snprintf(engine_path, PATH_SIZE, "%s/AnitraEngine.exe", DEBUG_DIR);
    if (project_include) {
        snprintf(engine_cmdline, sizeof(engine_cmdline),
                 "\"%s\" --include \"%s\"",
                 engine_path, project_include);
    } else {
        snprintf(engine_cmdline, sizeof(engine_cmdline),
                 "\"%s\"", engine_path);
    }
    
    if (!CreateProcessA(
            engine_path,
            engine_cmdline,
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &si,
            &pi)) {
        printf("!! Failed to launch engine (error %lu)\n", GetLastError());
        return 1;
    }
    
    printf("   Engine launched (PID: %lu)\n", (unsigned long)pi.dwProcessId);
    CloseHandle(pi.hThread);
    g_engine_process = pi.hProcess;

    /* Now start watching for changes; exits when engine closes */
    {
        int rc = watch_and_rebuild();
        CloseHandle(g_engine_process);
        g_engine_process = NULL;
        return rc;
    }
#else
    /* Fork and exec the engine on Unix */
    pid_t pid = fork();
    if (pid < 0) {
        printf("!! Failed to fork\n");
        return 1;
    }
    
    if (pid == 0) {
        /* Child process: run the engine */
        char engine_path[PATH_SIZE];
        snprintf(engine_path, PATH_SIZE, "%s/AnitraEngine", DEBUG_DIR);
        if (project_include) {
            execl(engine_path, "AnitraEngine", "--include", project_include, NULL);
        } else {
            execl(engine_path, "AnitraEngine", NULL);
        }
        printf("!! Failed to exec engine\n");
        exit(1);
    }
    
    /* Parent process: watch for changes */
    printf("   Engine launched (PID: %d)\n", pid);
    g_engine_pid = pid;
    return watch_and_rebuild();
#endif
}

/* ========================================================================= */
/* main                                                                       */
/* ========================================================================= */

int main(int argc, char **argv)
{
    if (find_tools() != 0) return 1;

    if (argc > 1 && strcmp(argv[1], "watch") == 0) {
        return build_and_run();
    }
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        return build_server();
    }

    return build_all();
}
