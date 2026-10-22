/* Convert stdin into a C array. This keeps the server's build dependency on C. */
#include <stdio.h>

int main(void)
{
    int ch;
    unsigned column = 0;
    if (fputs("static const char stylesheet[] = {\n", stdout) == EOF)
        return 1;
    while ((ch = getchar()) != EOF) {
        if (printf("%d,", ch) < 0)
            return 1;
        if (++column == 24) {
            if (putchar('\n') == EOF)
                return 1;
            column = 0;
        }
    }
    if (ferror(stdin) || fputs("0\n};\n", stdout) == EOF || fflush(stdout) == EOF)
        return 1;
    return 0;
}
