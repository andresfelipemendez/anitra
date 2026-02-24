const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const only = b.option([]const u8, "only", "Build only one target: engine, externals, core, exe");
    const build_all = only == null;

    const c_flags = &[_][]const u8{ "-std=c99", "-D_WIN32" };

    const install_options = std.Build.Step.InstallArtifact.Options{
        .dest_dir = .{ .override = .{ .custom = "" } },
    };

    // ── externals.dll ──────────────────────────────────────────────
    // Always build externals (core.dll depends on it for linking)
    const externals_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    externals_mod.addCSourceFiles(.{
        .files = &.{"src/externals/externals.c"},
        .flags = c_flags,
    });
    addIncludes(b, externals_mod);
    externals_mod.addLibraryPath(b.path("lib/sdl3/lib/x64"));
    externals_mod.linkSystemLibrary("SDL3", .{});

    const externals_lib = b.addLibrary(.{
        .name = "externals",
        .linkage = .dynamic,
        .root_module = externals_mod,
    });

    if (build_all or strEql(only, "externals")) {
        b.getInstallStep().dependOn(&b.addInstallArtifact(externals_lib, install_options).step);
    }

    // ── engine.dll ─────────────────────────────────────────────────
    if (build_all or strEql(only, "engine")) {
        const mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        });
        mod.addCSourceFiles(.{
            .files = &.{
                "src/engine/engine.c",
                "src/engine/physics.c",
                "src/engine/renderer.c",
                "src/engine/scene.c",
                "src/engine/debug_render.c",
            },
            .flags = c_flags,
        });
        addIncludes(b, mod);
        mod.addLibraryPath(b.path("lib/sdl3/lib/x64"));
        mod.linkSystemLibrary("SDL3", .{});

        const lib = b.addLibrary(.{
            .name = "engine",
            .linkage = .dynamic,
            .root_module = mod,
        });
        b.getInstallStep().dependOn(&b.addInstallArtifact(lib, install_options).step);
    }

    // ── core.dll ───────────────────────────────────────────────────
    if (build_all or strEql(only, "core")) {
        const mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        });
        mod.addCSourceFiles(.{
            .files = &.{
                "src/core/core.c",
                "src/core/loadlibrary_windows.c",
            },
            .flags = c_flags,
        });
        addIncludes(b, mod);
        mod.addLibraryPath(b.path("lib/sdl3/lib/x64"));
        mod.linkSystemLibrary("SDL3", .{});
        // core.dll calls functions from externals.dll (assign_init, init_externals, etc.)
        mod.linkLibrary(externals_lib);

        const lib = b.addLibrary(.{
            .name = "core",
            .linkage = .dynamic,
            .root_module = mod,
        });
        b.getInstallStep().dependOn(&b.addInstallArtifact(lib, install_options).step);
    }

    // ── AnitraEngine.exe ───────────────────────────────────────────
    if (build_all or strEql(only, "exe")) {
        const mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        });
        mod.addCSourceFiles(.{
            .files = &.{
                "src/main.c",
                "src/core/loadlibrary_windows.c",
            },
            .flags = c_flags,
        });
        addIncludes(b, mod);

        const exe = b.addExecutable(.{
            .name = "AnitraEngine",
            .root_module = mod,
        });
        b.getInstallStep().dependOn(&b.addInstallArtifact(exe, install_options).step);
    }

    // ── Copy SDL3.dll to output ────────────────────────────────────
    if (build_all) {
        b.getInstallStep().dependOn(&b.addInstallFile(b.path("lib/sdl3/lib/x64/SDL3.dll"), "SDL3.dll").step);
    }
}

fn addIncludes(b: *std.Build, mod: *std.Build.Module) void {
    const paths = [_][]const u8{
        "src",
        "src/core",
        "src/engine",
        "src/externals",
        "lib/sdl3/include",
    };
    for (paths) |p| mod.addIncludePath(b.path(p));
}

fn strEql(a: ?[]const u8, comptime b: []const u8) bool {
    if (a) |s| return std.mem.eql(u8, s, b);
    return false;
}
