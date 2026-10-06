"""Compile time of each file in compile_time/, and of decimal_bench.cpp, with each compiler given: the fastest of a
few runs and the preprocessed size.

py compile_time.py [msvc] [gcc=<g++>] [clang=<clang++>] [--runs=N]
boost_import.cpp imports the boost.decimal module and boost_pch.cpp uses a precompiled boost_pch.h; each is built once
per compiler before its consumer, and that build is timed too.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCES = HERE / "compile_time"
INCLUDES = [HERE / "decimal" / "include", HERE / "nanobench" / "src" / "include", HERE.parent / "LIBRARY" / "src"]
MODULE = HERE / "decimal" / "module" / "decimal.cppm"
FILES = [SOURCES / f"{stem}.cpp" for stem in
         ["baseline", "intel", "boost_fwd", "boost_decimal64", "boost_charconv", "boost_all", "boost_import",
          "boost_pch", "struct_bits", "struct_boost", "struct_import"]] + [HERE / "decimal_bench.cpp"]
MODULE_USERS = {"boost_import", "struct_import"}


def lower_priority():
    os.nice(19)


def low_priority():
    if os.name == "nt":
        return {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS}
    return {"preexec_fn": lower_priority}


def run(args, env, cwd):
    start = time.perf_counter()
    r = subprocess.run(args, env=env, cwd=cwd, capture_output=True, encoding="utf-8", errors="replace",
                       **low_priority())
    elapsed = time.perf_counter() - start
    if r.returncode != 0:
        output = (r.stdout + r.stderr).splitlines()
        errors = [line for line in output if "error" in line.lower()] or output
        print("failed: " + "\n    ".join(errors[:4]))
        return None
    return elapsed


def fastest(args, env, cwd, runs):
    times = []
    for _ in range(runs):
        elapsed = run(args, env, cwd)
        if elapsed is None:
            return None
        times.append(elapsed)
    return min(times)


def count_lines(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return sum(1 for line in f if line.strip())


def msvc_env():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    vs = subprocess.run([str(vswhere), "-latest", "-prerelease", "-property", "installationPath"],
                        capture_output=True, encoding="utf-8").stdout.strip()
    vcvars = Path(vs) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    out = subprocess.run(f'cmd /s /c ""{vcvars}" >nul && set"', capture_output=True, encoding="utf-8",
                         errors="replace").stdout
    env = dict(os.environ)
    for line in out.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            env[key.upper()] = value
    return env


class Msvc:
    name = "MSVC"

    def __init__(self):
        self.env = msvc_env()
        self.cl = shutil.which("cl", path=self.env["PATH"])

    def common(self):
        return [self.cl, "/nologo", "/std:c++latest", "/EHsc", "/O2", "/permissive-"] + [f"/I{p}" for p in INCLUDES]

    def build_module(self, out):
        return self.common() + ["/c", "/interface", "/TP", str(MODULE), "/ifcOutput", str(out / "boost.decimal.ifc"),
                                f"/Fo{out / 'decimal.obj'}"]

    def build_pch(self, out):
        return self.common() + ["/c", "/Ycboost_pch.h", f"/Fp{out / 'boost_pch.pch'}", f"/Fo{out / 'pch.obj'}",
                                str(SOURCES / "boost_pch.cpp")]

    def compile(self, src, out):
        extra = []
        if src.stem in MODULE_USERS:
            extra = ["/reference", f"boost.decimal={out / 'boost.decimal.ifc'}"]
        elif src.stem == "boost_pch":
            extra = ["/Yuboost_pch.h", f"/Fp{out / 'boost_pch.pch'}"]
        return self.common() + extra + ["/c", str(src), f"/Fo{out / (src.stem + '.obj')}"]

    def preprocess(self, src, out):
        return self.common() + ["/P", f"/Fi{out / (src.stem + '.i')}", str(src)], out / (src.stem + ".i")


class Gnu:
    def __init__(self, name, exe):
        self.name = name
        self.exe = exe
        self.env = dict(os.environ)
        self.env["PATH"] = str(Path(exe).parent) + os.pathsep + self.env.get("PATH", "")

    def common(self):
        return [self.exe, "-std=c++23", "-O2"] + [f"-I{p}" for p in INCLUDES]

    def build_module(self, out):
        if self.name == "GCC":
            return self.common() + ["-fmodules-ts", "-c", "-x", "c++", str(MODULE), "-o", str(out / "decimal.o")]
        return self.common() + ["--precompile", "-x", "c++-module", str(MODULE), "-o", str(out / "boost.decimal.pcm")]

    def build_pch(self, out):
        if self.name == "GCC":
            shutil.copy(SOURCES / "boost_pch.h", out)
            shutil.copy(SOURCES / "boost_pch.cpp", out)
            return self.common() + ["-x", "c++-header", str(out / "boost_pch.h"), "-o", str(out / "boost_pch.h.gch")]
        return self.common() + ["-x", "c++-header", str(SOURCES / "boost_pch.h"), "-o", str(out / "boost_pch.pch")]

    def compile(self, src, out):
        extra = []
        if src.stem in MODULE_USERS and self.name == "GCC":
            extra = ["-fmodules-ts"]
        elif src.stem in MODULE_USERS:
            extra = [f"-fmodule-file=boost.decimal={out / 'boost.decimal.pcm'}"]
        elif src.stem == "boost_pch" and self.name == "GCC":
            src = out / src.name
        elif src.stem == "boost_pch":
            extra = ["-include-pch", str(out / "boost_pch.pch")]
        return self.common() + extra + ["-c", str(src), "-o", str(out / (src.stem + ".o"))]

    def preprocess(self, src, out):
        return self.common() + ["-E", str(src), "-o", str(out / (src.stem + ".i"))], out / (src.stem + ".i")


def print_row(name, seconds, lines):
    text = "failed" if seconds is None else f"{seconds:.2f}"
    print(f"{name:<18} {text:>8} {lines:>19}")


def measure(compiler, runs):
    print(f"\n{compiler.name}: fastest of {runs} runs, -O2 / /O2")
    print(f"{'file':<18} {'seconds':>8} {'preprocessed lines':>19}")
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        module_time = fastest(compiler.build_module(out), compiler.env, out, runs)
        pch_time = fastest(compiler.build_pch(out), compiler.env, out, runs)
        for src in FILES:
            prebuilt = src.stem in MODULE_USERS or src.stem == "boost_pch"
            if (src.stem in MODULE_USERS and module_time is None) or (src.stem == "boost_pch" and pch_time is None):
                print_row(src.stem, None, "-")
                continue
            seconds = fastest(compiler.compile(src, out), compiler.env, out, runs)
            lines = "-"
            if not prebuilt:
                args, result = compiler.preprocess(src, out)
                if run(args, compiler.env, out) is not None:
                    lines = f"{count_lines(result):,}"
            print_row(src.stem, seconds, lines)
        print_row("(module build)", module_time, "-")
        print_row("(pch build)", pch_time, "-")


def main():
    runs = 3
    compilers = []
    for arg in sys.argv[1:]:
        if arg.startswith("--runs="):
            runs = int(arg.split("=", 1)[1])
        elif arg == "msvc":
            compilers.append(Msvc())
        elif arg.startswith("gcc"):
            compilers.append(Gnu("GCC", arg.partition("=")[2] or shutil.which("g++")))
        elif arg.startswith("clang"):
            compilers.append(Gnu("Clang", arg.partition("=")[2] or shutil.which("clang++")))
    for compiler in compilers:
        measure(compiler, runs)


if __name__ == "__main__":
    main()
