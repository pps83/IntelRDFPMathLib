echo "BEGIN TESTS IN WINDOWS..."
del readtest.exe readtest.obj ..\LIBRARY\libbid.lib 2>nul
call windowsbuild_nmake.bat -fmakefile.mak
del readtest.exe readtest.obj ..\LIBRARY\libbid.lib 2>nul
echo "END TESTS IN WINDOWS..."
echo "THE TESTS PASSED IF NO FAILURES WERE REPORTED ABOVE"
