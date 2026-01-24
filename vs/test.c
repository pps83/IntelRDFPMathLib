#if defined(_WIN32) && !defined(WINDOWS)
#define WINDOWS
#endif

#define DECIMAL_CALL_BY_REFERENCE 0
#define DECIMAL_GLOBAL_ROUNDING 0
#define DECIMAL_GLOBAL_EXCEPTION_FLAGS 0

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>

#define main main_readtest
int main(int argc, char* argv[]);
#include "../TESTS/readtest.c"
#undef main

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wreturn-type"
#elif defined(_MSC_VER)
#pragma warning(disable: 4715) // warning C4715: 'main_example': not all control paths return a value
#endif
#define main main_example
int main();
#include "../EXAMPLES/main.c_000"
#undef main

// Load readtest.in as `stdin` relative to __FILE__ when running from Visual Studio
static void LoadStdinUnderVisualStudio()
{
#ifdef _MSC_VER
    if (!getenv("VisualStudioVersion")) // check if running from Visual Studio
        return;
    const char* sourcePath = __FILE__; // Get the path of the current source file
    char inputFilePath[1024];
    strcpy_s(inputFilePath, sizeof(inputFilePath), sourcePath);

    // Find the last backslash or slash
    char* lastSlash = strrchr(inputFilePath, '\\');
    if (lastSlash)
    {
        *lastSlash = '\0';
        lastSlash = strrchr(inputFilePath, '\\');
    }
    else
    {
        lastSlash = strrchr(inputFilePath, '/');
        if (lastSlash)
        {
            *lastSlash = '\0';
            lastSlash = strrchr(inputFilePath, '/');
        }
    }

    if (lastSlash) // Replace the source filename with readtest.inWINDOWS
    {
        *(lastSlash + 1) = '\0';  // Terminate after the slash
        strcat_s(inputFilePath, sizeof(inputFilePath), "TESTS\\readtest.in");
    }
    else // No path found, just use readtest.in in current directory
        strcpy_s(inputFilePath, sizeof(inputFilePath), "readtest.in");

    // Try to open `readtest.in` relative to project dir
    FILE* inputFile = fopen("..\\TESTS\\readtest.in", "r");
    if (inputFile)
        strcpy_s(inputFilePath, sizeof(inputFilePath), "..\\TESTS\\readtest.in");
    else
        inputFile = fopen(inputFilePath, "r");
    if (inputFile)
    {
        // Redirect stdin from file
        freopen(inputFilePath, "r", stdin);
        printf("Visual Studio mode: Using '%s' for input\n", inputFilePath);
        fclose(inputFile);
    }
#endif
}

int main(int argc, char* argv[])
{
    main_example();

    printf("\nRunning readtest example:\n");
    LoadStdinUnderVisualStudio();
    int result_readtest = main_readtest(argc, argv);
    if (result_readtest)
        printf("readtest failed with ret:%d\n", result_readtest);
    return result_readtest;
}
