/* Convert stdin into a C array. This keeps the server's build dependency on C. */
#include <stdio.h>
#include <ctype.h>

int main(int argc, char **argv)
{
    if (argc != 2 || (!isalpha((unsigned char)argv[1][0]) && argv[1][0] != '_')) {
        fputs("Usage: embed C_ARRAY_NAME < input\n", stderr);
        return 1;
    }
    for (const char *p = argv[1]; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') {
            fputs("Invalid C array name\n", stderr);
            return 1;
        }
    }
    int ch;
    unsigned column = 0;
    if (printf("static const char %s[] = {\n", argv[1]) < 0)
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
