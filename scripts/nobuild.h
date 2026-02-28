/*
 * nobuild - Simplified C build helper library
 *
 * This is a collection of reusable build helpers extracted from the main
 * build.c file. These can be included in other build scripts or used directly.
 */

#ifndef NOBUILD_H
#define NOBUILD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Platform-specific definitions */
#ifdef _WIN32
#  define DIR_SEP '\\'
#  define PATH_SIZE MAX_PATH
#else
#  define DIR_SEP '/'
#  define PATH_SIZE 4096
#endif

/* Helper: Check if source is newer than object (for incremental builds) */
static int needs_rebuild(const char *src, const char *obj) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA src_attr, obj_attr;
    
    if (!GetFileAttributesExA(obj, GetFileExInfoStandard, &obj_attr))
        return 1;  /* Object doesn't exist */
        
    if (!GetFileAttributesExA(src, GetFileExInfoStandard, &src_attr))
        return 1;  /* Source doesn't exist */
    
    /* Compare timestamps */
    return CompareFileTime(&src_attr.ftLastWriteTime, &obj_attr.ftLastWriteTime) > 0;
#else
    struct stat src_st, obj_st;
    
    if (stat(obj, &obj_st) != 0)
        return 1;  /* Object doesn't exist */
        
    if (stat(src, &src_st) != 0)
        return 1;  /* Source doesn't exist */
    
    return src_st.st_mtime > obj_st.st_mtime;
#endif
}

/* Helper: Ensure directory exists */
static int ensure_dir(const char *path) {
#ifdef _WIN32
    if (CreateDirectoryA(path, NULL)) {
        return 0;  /* Created successfully */
    }
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) {
        return 0;  /* Already exists - that's fine */
    }
    fprintf(stderr, "ERROR: Failed to create directory '%s' (error %lu)\n", path, err);
    return 1;
#else
    if (mkdir(path, 0755) == 0) {
        return 0;  /* Created successfully */
    }
    if (errno == EEXIST) {
        return 0;  /* Already exists - that's fine */
    }
    fprintf(stderr, "ERROR: Failed to create directory '%s' (%s)\n", path, strerror(errno));
    return 1;
#endif
}

/* Helper: Run a command and check result */
static int run_cmd(const char *cmd) {
    printf(">> %s\n", cmd);
    fflush(stdout);
    
    int rc = system(cmd);
    
    if (rc != 0) {
        fprintf(stderr, "ERROR: Command failed with exit code %d\n", rc);
        return 1;
    }
    
    return 0;
}

/* Helper: Get file basename without extension */
static void get_basename(const char *path, char *out, size_t out_size) {
    const char *basename = strrchr(path, DIR_SEP);
    if (!basename) basename = path;
    
    /* Remove extension */
    const char *ext = strrchr(basename, '.');
    size_t len = ext ? (size_t)(ext - basename) : strlen(basename);
    
    snprintf(out, out_size, "%.*s", (int)len, basename);
}

/* Helper: Build compiler command for a single source file */
static int build_single_source(
    const char *src,
    const char *obj_ext,
    const char *includes,
    const char *extra_flags
) {
    char cmd[8192];
    char obj_path[PATH_SIZE];
    
    get_basename(src, obj_path, sizeof(obj_path));
    snprintf(obj_path + strlen(obj_path), sizeof(obj_path) - strlen(obj_path), "%s", obj_ext);
    
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
        "cl.exe /c %s /nologo /MD /Zi /Od /W4 "
        "/Fo%s "
        "%s %s",
        includes, obj_path, src, extra_flags ? extra_flags : ""
    );
#else
    snprintf(cmd, sizeof(cmd),
        "cc -c %s -g -O0 -Wall "
        "-o %s "
        "%s %s",
        includes, obj_path, src, extra_flags ? extra_flags : ""
    );
#endif
    
    return run_cmd(cmd);
}

/* Helper: Build library/archive from object files */
static int build_archive(
    const char *lib_name,
    const char **obj_files,
    int obj_count
) {
    char cmd[8192];
    char lib_path[PATH_SIZE];
    
#ifdef _WIN32
    snprintf(lib_path, sizeof(lib_path), "build/Debug/%s.lib", lib_name);
    snprintf(cmd, sizeof(cmd),
        "lib.exe /OUT:%s %s",
        lib_path, obj_files[0]  /* TODO: Add all objects */
    );
#else
    snprintf(lib_path, sizeof(lib_path), "build/Debug/lib%s.a", lib_name);
    snprintf(cmd, sizeof(cmd),
        "ar rcs %s %s",
        lib_path, obj_files[0]  /* TODO: Add all objects */
    );
#endif
    
    return run_cmd(cmd);
}

/* Helper: Build shared library (DLL/SO) from object files */
static int build_shared_library(
    const char *lib_name,
    const char **obj_files,
    int obj_count
) {
    char cmd[8192];
    char lib_path[PATH_SIZE];
    
#ifdef _WIN32
    snprintf(lib_path, sizeof(lib_path), "build/Debug/%s.dll", lib_name);
    snprintf(cmd, sizeof(cmd),
        "link.exe /DLL /OUT:%s %s",
        lib_path, obj_files[0]  /* TODO: Add all objects */
    );
#else
    snprintf(lib_path, sizeof(lib_path), "build/Debug/lib%s.so", lib_name);
    snprintf(cmd, sizeof(cmd),
        "cc -shared -o %s %s",
        lib_path, obj_files[0]  /* TODO: Add all objects */
    );
#endif
    
    return run_cmd(cmd);
}

/* Helper: Build executable from object files */
static int build_executable(
    const char *exe_name,
    const char **obj_files,
    int obj_count
) {
    char cmd[8192];
    char exe_path[PATH_SIZE];
    
#ifdef _WIN32
    snprintf(exe_path, sizeof(exe_path), "build/Debug/%s.exe", exe_name);
    snprintf(cmd, sizeof(cmd),
        "link.exe /OUT:%s %s",
        exe_path, obj_files[0]  /* TODO: Add all objects */
    );
#else
    snprintf(exe_path, sizeof(exe_path), "build/Debug/%s", exe_name);
    snprintf(cmd, sizeof(cmd),
        "cc -o %s %s",
        exe_path, obj_files[0]  /* TODO: Add all objects */
    );
#endif
    
    return run_cmd(cmd);
}

#endif /* NOBUILD_H */
