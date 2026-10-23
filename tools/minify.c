/* Conservative whitespace/comment minification for the site's JS and CSS.
 * No identifier renaming or expression rewriting. Ambiguous JS slash/template
 * syntax is preserved verbatim rather than guessed at without a full parser. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool word(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '\\' || c >= 128;
}

static bool digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static bool member(unsigned char c, const char *set)
{
    return c != 0 && strchr(set, c) != NULL;
}

static bool css_word(unsigned char c)
{
    return word(c) || member(c, "-#@");
}

static bool separate_js(unsigned char a, unsigned char b)
{
    return (word(a) && word(b)) || (digit(a) && b == '.') ||
           (a == '.' && digit(b)) ||
           (member(a, "+-*/%&|^!?=<>.") && member(b, "+-*/%&|^!?=<>."));
}

static bool separate_css_comment(unsigned char a, unsigned char b)
{
    return (css_word(a) && css_word(b)) || (css_word(a) && b == '(') ||
           (digit(a) && b == '.') || (a == '.' && digit(b));
}

static bool whitespace(unsigned char c, bool js)
{
    return member(c, " \t\r\n\f") || (js && c == '\v');
}

static size_t string_end(const unsigned char *s, size_t n, size_t i, bool js)
{
    unsigned char quote = s[i++];
    while (i < n) {
        unsigned char c = s[i++];
        if (c == quote)
            return i;
        if (c == '\\') {
            if (i == n)
                return SIZE_MAX;
            if (s[i++] == '\r' && i < n && s[i] == '\n')
                i++;
        } else if (c == '\n' || c == '\r' || (!js && c == '\f')) {
            return SIZE_MAX;
        }
    }
    return SIZE_MAX;
}

static bool starts_url(const unsigned char *s, size_t n, size_t i)
{
    return n - i >= 4 && (i == 0 || !css_word(s[i - 1])) &&
           (s[i] == 'u' || s[i] == 'U') && (s[i+1] == 'r' || s[i+1] == 'R') &&
           (s[i+2] == 'l' || s[i+2] == 'L') && s[i+3] == '(';
}

static size_t url_end(const unsigned char *s, size_t n, size_t i)
{
    i += 4;
    while (i < n) {
        if (s[i] == ')')
            return i + 1;
        if (s[i] == '\'' || s[i] == '"') {
            i = string_end(s, n, i, false);
            if (i == SIZE_MAX)
                return SIZE_MAX;
        } else if (s[i++] == '\\') {
            if (i == n)
                return SIZE_MAX;
            i++;
        }
    }
    return SIZE_MAX;
}

static size_t css_escape_end(const unsigned char *s, size_t n, size_t i)
{
    i++;
    if (i == n)
        return SIZE_MAX;
    size_t hex = 0;
    while (i < n && hex < 6 && (digit(s[i]) || member(s[i], "abcdefABCDEF"))) {
        i++;
        hex++;
    }
    if (hex) {
        /* The first whitespace terminates a hex escape; a second whitespace
         * may be a descendant combinator and must not be merged into it. */
        if (i < n && whitespace(s[i], false)) {
            if (s[i++] == '\r' && i < n && s[i] == '\n')
                i++;
        }
    } else if (s[i++] == '\r' && i < n && s[i] == '\n') {
        i++;
    }
    return i;
}

static bool minify(unsigned char *s, size_t n, bool js, size_t *length)
{
    size_t i = 0, out = 0, parens = 0, brackets = 0;
    bool space = false, newline = false, comment = false, escaped = false;
    while (i < n) {
        unsigned char c = s[i];
        if (whitespace(c, js)) {
            space = true;
            newline |= c == '\r' || c == '\n';
            i++;
            continue;
        }
        /* JavaScript also treats these UTF-8 sequences as line terminators. */
        if (js && n - i >= 3 && c == 0xe2 && s[i+1] == 0x80 &&
            (s[i+2] == 0xa8 || s[i+2] == 0xa9)) {
            space = newline = true;
            i += 3;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i+1] == '*') {
            i += 2;
            bool closed = false;
            while (i < n) {
                if (s[i] == '*' && i + 1 < n && s[i+1] == '/') {
                    i += 2;
                    closed = true;
                    break;
                }
                if (js && (s[i] == '\n' || s[i] == '\r'))
                    newline = true;
                if (js && n - i >= 3 && s[i] == 0xe2 && s[i+1] == 0x80 &&
                    (s[i+2] == 0xa8 || s[i+2] == 0xa9))
                    newline = true;
                i++;
            }
            if (!closed) {
                fputs("minify: unterminated block comment\n", stderr);
                return false;
            }
            comment = true;
            continue;
        }
        if (js && c == '/' && i + 1 < n && s[i+1] == '/') {
            i += 2;
            while (i < n && s[i] != '\n' && s[i] != '\r' &&
                   !(n - i >= 3 && s[i] == 0xe2 && s[i+1] == 0x80 &&
                     (s[i+2] == 0xa8 || s[i+2] == 0xa9)))
                i++;
            comment = true;
            continue;
        }
        if (out) {
            unsigned char prev = s[out - 1];
            if (js) {
                /* Preserve possible ASI, return/yield, async, and postfix boundaries.
                 * Only discard newlines at unambiguous continuation punctuation. */
                if (newline && !member(prev, "{([,;:") && !member(c, "})],;"))
                    s[out++] = '\n';
                else if ((space || comment) && separate_js(prev, c))
                    s[out++] = ' ';
            } else if (space) {
                bool trim = !escaped && !parens && !brackets &&
                            (member(prev, "{};,") || member(c, "{};,") || prev == ':');
                /* Keep whitespace-only custom property values valid. */
                if (!trim || (prev == ':' && member(c, ";}")))
                    s[out++] = ' ';
            } else if (comment && (escaped || separate_css_comment(prev, c))) {
                /* An empty comment separates CSS tokens without introducing a
                 * descendant combinator (which an inserted space would do). */
                memcpy(s + out, "/**/", 4);
                out += 4;
            }
        }
        space = newline = comment = false;
        escaped = false;
        size_t end = i + 1;
        if (c == '\'' || c == '"') {
            end = string_end(s, n, i, js);
            if (end == SIZE_MAX) {
                fputs("minify: unterminated string\n", stderr);
                return false;
            }
        } else if (js && (c == '`' || c == '/')) {
            /* A slash may start division or a regexp, and templates can contain
             * nested JS. Preserve the remaining source in these cases rather
             * than risk changing its lexical meaning. Current copy.js needs
             * neither case; slashes inside its strings were handled above. */
            memmove(s + out, s + i, n - i);
            out += n - i;
            i = n;
            break;
        } else if (!js && starts_url(s, n, i)) {
            end = url_end(s, n, i);
            if (end == SIZE_MAX) {
                fputs("minify: unterminated CSS url()\n", stderr);
                return false;
            }
        } else if (!js && c == '\\') {
            end = css_escape_end(s, n, i);
            if (end == SIZE_MAX) {
                fputs("minify: unfinished CSS escape\n", stderr);
                return false;
            }
            escaped = true;
        } else if (!js) {
            if (c == '(') parens++;
            if (c == ')' && parens) parens--;
            if (c == '[') brackets++;
            if (c == ']' && brackets) brackets--;
        }
        memmove(s + out, s + i, end - i);
        out += end - i;
        i = end;
    }
    *length = out;
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "--js") && strcmp(argv[1], "--css"))) {
        fputs("Usage: minify --js|--css < input > output\n", stderr);
        return 1;
    }
    size_t n = 0, cap = 0;
    unsigned char *source = NULL;
    int c;
    while ((c = getchar()) != EOF) {
        if (c == 0) {
            fputs("minify: NUL byte in source\n", stderr);
            free(source);
            return 1;
        }
        if (n == cap) {
            size_t next = cap ? cap * 2 : 4096;
            unsigned char *buffer = next > cap ? realloc(source, next) : NULL;
            if (!buffer) {
                fputs("minify: source too large or out of memory\n", stderr);
                free(source);
                return 1;
            }
            source = buffer;
            cap = next;
        }
        source[n++] = (unsigned char)c;
    }
    bool ok = !ferror(stdin) && minify(source, n, strcmp(argv[1], "--js") == 0, &n);
    if (ok && n)
        ok = fwrite(source, 1, n, stdout) == n &&
             (source[n - 1] == '\n' || putchar('\n') != EOF);
    free(source);
    return ok && fflush(stdout) == 0 ? 0 : 1;
}
