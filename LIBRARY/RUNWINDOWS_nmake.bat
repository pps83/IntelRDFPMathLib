@echo off
echo "BEGIN BUILDING LIBRARY IN WINDOWS..."

del *.lib 2>nul

call windowsbuild_nmake.bat -fmakefile.mak

echo "END BUILDING LIBRARY IN WINDOWS..."

