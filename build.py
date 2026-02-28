#!/usr/bin/env python3
"""
Simple Python-based build system for Anitra Engine

This is a cleaner, more maintainable alternative to the complex C-based
build.c file. It provides the same functionality with better readability.
"""

import os
import sys
import subprocess
from pathlib import Path
from typing import List

# Configuration
SRC_DIR = Path("src")
BUILD_DIR = Path("build")
DEBUG_DIR = BUILD_DIR / "Debug"
INCLUDES = [
    "-Isrc",
    "-Isrc/core", 
    "-Isrc/engine",
    "-Isrc/editor",
    "-Isrc/externals",
]

def ensure_dir(path: Path) -> bool:
    """Ensure directory exists, create if not."""
    try:
        path.mkdir(parents=True, exist_ok=True)
        return True
    except Exception as e:
        print(f"ERROR: Failed to create directory {path}: {e}")
        return False

def run_command(cmd: List[str], cwd=None) -> bool:
    """Run a command and check result."""
    print(f">>> {' '.join(cmd)}")
    try:
        result = subprocess.run(cmd, cwd=cwd or Path.cwd(), capture_output=True, text=True)
        if result.stdout:
            print(result.stdout)
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        return result.returncode == 0
    except Exception as e:
        print(f"ERROR: Command failed: {e}")
        return False

def compile_source(src_file: Path, obj_dir: Path, defines=None) -> bool:
    """Compile a single C source file to object file."""
    ensure_dir(obj_dir)
    
    src_name = src_file.stem
    obj_file = obj_dir / f"{src_name}.obj"
    
    cmd = [
        "tcc.exe", "-Blib/tcc-windows", "-c",
        str(src_file),
        "-o", str(obj_file)
    ]
    
    # Add includes
    for inc in INCLUDES:
        cmd.extend(["-I", Path(inc).name])  # Just the directory name for TCC
    
    # Add defines
    if defines:
        for define in defines:
            cmd.extend(["-D", define])
    
    return run_command(cmd)

def link_dll(dll_name: str, obj_files: List[Path], lib_deps=None) -> bool:
    """Link object files into a DLL."""
    dll_file = DEBUG_DIR / f"{dll_name}.dll"
    
    cmd = [
        "tcc.exe", "-Blib/tcc-windows", "-shared",
        "-o", str(dll_file)
    ]
    
    # Add objects
    for obj in obj_files:
        cmd.append(str(obj))
    
    # Add library dependencies
    if lib_deps:
        for dep in lib_deps:
            cmd.extend(["-l", dep])
    
    return run_command(cmd)

def link_exe(exe_name: str, obj_files: List[Path], lib_deps=None) -> bool:
    """Link object files into an executable."""
    exe_file = DEBUG_DIR / f"{exe_name}.exe"
    
    cmd = [
        "tcc.exe", "-Blib/tcc-windows",
        "-o", str(exe_file)
    ]
    
    # Add objects
    for obj in obj_files:
        cmd.append(str(obj))
    
    # Add library dependencies  
    if lib_deps:
        for dep in lib_deps:
            cmd.extend(["-l", dep])
    
    return run_command(cmd)

def build_externals() -> bool:
    """Build externals.dll - SDL3/GPU/UI initialization."""
    print("\n=== Building externals ===")
    
    src_files = [SRC_DIR / "externals" / "externals.c"]
    obj_dir = DEBUG_DIR / "obj" / "externals"
    
    # Compile
    for src in src_files:
        if not compile_source(src, obj_dir):
            return False
    
    obj_files = list(obj_dir.glob("*.obj"))
    return link_dll("externals", obj_files)

def build_core() -> bool:
    """Build core.dll - Hot-reload coordinator."""
    print("\n=== Building core ===")
    
    src_files = [
        SRC_DIR / "core" / "core.c",
        SRC_DIR / "core" / "loadlibrary_windows.c",
    ]
    obj_dir = DEBUG_DIR / "obj" / "core"
    
    # Compile
    for src in src_files:
        if not compile_source(src, obj_dir):
            return False
    
    obj_files = list(obj_dir.glob("*.obj"))
    
    # Link with externals dependency
    return link_dll("core", obj_files)

def build_engine() -> bool:
    """Build engine.dll - Gameplay logic."""
    print("\n=== Building engine ===")
    
    src_files = [
        SRC_DIR / "engine" / f"{f}.c"
        for f in ["engine", "renderer", "physics", "scene", 
                  "debug_render", "anim", "gltf_loader"]
    ]
    obj_dir = DEBUG_DIR / "obj" / "engine"
    
    # Compile
    for src in src_files:
        if not compile_source(src, obj_dir):
            return False
    
    obj_files = list(obj_dir.glob("*.obj"))
    return link_dll("engine", obj_files)

def build_editor() -> bool:
    """Build editor.dll - Docking UI system."""
    print("\n=== Building editor ===")
    
    src_file = SRC_DIR / "editor" / "editor.c"
    obj_dir = DEBUG_DIR / "obj" / "editor"
    
    # Compile with Clay SIMD disabled
    if not compile_source(src_file, obj_dir, ["CLAY_DISABLE_SIMD"]):
        return False
    
    obj_files = list(obj_dir.glob("*.obj"))
    return link_dll("editor", obj_files)

def build_exe() -> bool:
    """Build AnitraEngine.exe - Main executable."""
    print("\n=== Building executable ===")
    
    src_files = [
        SRC_DIR / "main.c",
        SRC_DIR / "core" / "loadlibrary_windows.c",
    ]
    obj_dir = DEBUG_DIR / "obj" / "exe"
    
    # Compile
    for src in src_files:
        if not compile_source(src, obj_dir):
            return False
    
    obj_files = list(obj_dir.glob("*.obj"))
    return link_exe("AnitraEngine", obj_files)

def build_all() -> bool:
    """Build everything: externals, core, engine, editor, exe."""
    print("=" * 50)
    print("Anitra Engine - Full Build")
    print("=" * 50)
    
    ensure_dir(DEBUG_DIR)
    
    # Build in dependency order
    if not build_externals():
        return False
    
    if not build_core():
        return False
    
    if not build_engine():
        return False
    
    if not build_editor():
        return False
    
    if not build_exe():
        return False
    
    print("\n" + "=" * 50)
    print("Build complete!")
    print("=" * 50)
    return True

def clean() -> bool:
    """Remove all build artifacts."""
    print("\n=== Cleaning ===")
    
    try:
        if DEBUG_DIR.exists():
            for item in DEBUG_DIR.glob("*"):
                if item.is_file():
                    item.unlink()
            print(f"Removed files from {DEBUG_DIR}")
        
        obj_dir = BUILD_DIR / "obj"
        if obj_dir.exists():
            for item in obj_dir.rglob("*"):
                if item.is_file() and item.suffix in ['.obj', '.o']:
                    item.unlink()
            print(f"Removed object files from {obj_dir}")
    except Exception as e:
        print(f"ERROR during clean: {e}")
        return False
    
    return True

def usage():
    """Print usage information."""
    print("Usage: python3 build.py [target]")
    print("\nTargets:")
    print("  all      - Build everything (default)")
    print("  externals - Build externals.dll only")
    print("  core     - Build core.dll only") 
    print("  engine   - Build engine.dll only")
    print("  editor   - Build editor.dll only")
    print("  exe      - Build AnitraEngine.exe only")
    print("  clean    - Remove all build artifacts")
    print("\nExamples:")
    print("  python3 build.py          # Build everything")
    print("  python3 build.py engine   # Only rebuild engine")

def main():
    """Main entry point."""
    if len(sys.argv) < 2:
        target = "all"
    else:
        target = sys.argv[1].lower()
    
    targets = {
        "all": build_all,
        "externals": build_externals,
        "core": build_core,
        "engine": build_engine,
        "editor": build_editor,
        "exe": build_exe,
        "clean": clean,
    }
    
    if target in targets:
        success = targets[target]()
        sys.exit(0 if success else 1)
    else:
        print(f"ERROR: Unknown target '{target}'")
        usage()
        sys.exit(1)

if __name__ == "__main__":
    main()
