#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#ifndef USE_PANDOC
#include <md4c-html.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include ".build/style.inc"

#define MAX_POST_BYTES (4U * 1024U * 1024U)
#define MAX_ASSET_BYTES (16U * 1024U * 1024U)
#define MAX_HTML_BYTES (32U * 1024U * 1024U)
#define MAX_INFO_BYTES (16U * 1024U)
#define MAX_POSTS 2048U
#define REQUEST_BYTES 8192U

typedef struct {
    char *data;
    size_t len, cap;
    bool failed;
} Buffer;

typedef struct {
    char slug[256], title[512], description[2048], author[256], tags[512];
    char date[11];
    bool draft;
    char *document, *body;
} Post;

static volatile sig_atomic_t stopping;

static void stop_server(int signo)
{
    (void)signo;
    stopping = 1;
}

static void append_bytes(Buffer *b, const char *s, size_t n)
{
    if (b->failed)
        return;
    if (n > MAX_HTML_BYTES - b->len) {
        b->failed = true;
        return;
    }
    size_t needed = b->len + n + 1;
    if (needed > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < needed)
            cap *= 2;
        char *next = realloc(b->data, cap);
        if (!next) {
            b->failed = true;
            return;
        }
        b->data = next;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void append(Buffer *b, const char *s)
{
    append_bytes(b, s, strlen(s));
}

static void escape_html(Buffer *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': append(b, "&amp;"); break;
        case '<': append(b, "&lt;"); break;
        case '>': append(b, "&gt;"); break;
        case '"': append(b, "&quot;"); break;
        case '\'': append(b, "&#39;"); break;
        default: append_bytes(b, s, 1); break;
        }
    }
}

static bool slug_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static bool valid_slug(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len > 250)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!slug_char((unsigned char)s[i]))
            return false;
    return true;
}

/* Read bounded regular files only. Symlinks are rejected by callers. */
static char *read_file(int fd, size_t limit, size_t *length)
{
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > limit)
        return NULL;
    size_t size = (size_t)st.st_size;
    char *data = malloc(size + 1);
    if (!data)
        return NULL;
    size_t used = 0;
    while (used < size) {
        ssize_t n = read(fd, data + used, size - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            free(data);
            return NULL;
        }
        used += (size_t)n;
    }
    data[used] = '\0';
    *length = used;
    return data;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
    return s;
}

static bool valid_date(const char *s)
{
    if (strlen(s) != 10 || s[4] != '-' || s[7] != '-')
        return false;
    for (size_t i = 0; i < 10; i++)
        if (i != 4 && i != 7 && !isdigit((unsigned char)s[i]))
            return false;
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + s[3]-'0';
    int m = (s[5]-'0')*10 + s[6]-'0';
    int d = (s[8]-'0')*10 + s[9]-'0';
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1 || m < 1 || m > 12)
        return false;
    int max_day = days[m-1] + (m == 2 && y%4 == 0 && (y%100 != 0 || y%400 == 0));
    return d >= 1 && d <= max_day;
}

static bool copy_field(char *dest, size_t cap, const char *value)
{
    size_t len = strlen(value);
    if (len >= cap)
        return false;
    memcpy(dest, value, len + 1);
    return true;
}

static bool parse_post(Post *p, size_t length)
{
    char *start = p->document;
    if (memchr(start, '\0', length))
        return false;
    if (length >= 3 && memcmp(start, "\xef\xbb\xbf", 3) == 0)
        start += 3;
    static const char marker[] = "/* BLOG INFO";
    if (strncmp(start, marker, sizeof(marker)-1) != 0)
        return false;
    char *header = start + sizeof(marker)-1;
    if (*header == '\r')
        header++;
    if (*header++ != '\n')
        return false;
    char *end = strstr(header, "*/");
    if (!end || (size_t)(end - header) > MAX_INFO_BYTES)
        return false;
    p->body = end + 2;
    *end = '\0';
    char *state;
    for (char *line = strtok_r(header, "\n", &state); line;
         line = strtok_r(NULL, "\n", &state)) {
        line = trim(line);
        if (!*line)
            continue;
        char *colon = strchr(line, ':');
        if (!colon)
            return false;
        *colon = '\0';
        char *key = trim(line), *value = trim(colon + 1);
        bool ok = true;
        if (strcmp(key, "title") == 0)
            ok = copy_field(p->title, sizeof(p->title), value);
        else if (strcmp(key, "description") == 0)
            ok = copy_field(p->description, sizeof(p->description), value);
        else if (strcmp(key, "author") == 0)
            ok = copy_field(p->author, sizeof(p->author), value);
        else if (strcmp(key, "tags") == 0)
            ok = copy_field(p->tags, sizeof(p->tags), value);
        else if (strcmp(key, "date") == 0)
            ok = valid_date(value) && copy_field(p->date, sizeof(p->date), value);
        else if (strcmp(key, "draft") == 0) {
            if (strcmp(value, "true") != 0 && strcmp(value, "false") != 0)
                return false;
            p->draft = strcmp(value, "true") == 0;
        }
        if (!ok)
            return false;
    }
    return p->title[0] != '\0';
}

static bool load_post(int blogfd, const char *slug, Post *p)
{
    memset(p, 0, sizeof(*p));
    if (!valid_slug(slug))
        return false;
    char filename[260];
    snprintf(filename, sizeof(filename), "%s.md", slug);
    int fd = openat(blogfd, filename, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    size_t length;
    p->document = read_file(fd, MAX_POST_BYTES, &length);
    close(fd);
    if (!p->document)
        return false;
    if (!parse_post(p, length)) {
        fprintf(stderr, "Skipping %s: invalid BLOG INFO header or content\n", filename);
        free(p->document);
        p->document = NULL;
        return false;
    }
    (void)copy_field(p->slug, sizeof(p->slug), slug);
    return true;
}

static int compare_posts(const void *a, const void *b)
{
    const Post *pa = a, *pb = b;
    int date = strcmp(pb->date, pa->date);
    return date ? date : strcmp(pa->slug, pb->slug);
}

static void page_start(Buffer *b, const char *title, const char *description)
{
    append(b, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<title>");
    escape_html(b, title);
    append(b, "</title><meta name=\"description\" content=\"");
    escape_html(b, description);
    append(b, "\"><link rel=\"stylesheet\" href=\"/style.css\"></head><body>"
              "<main class=\"shell\" id=\"main\">");
}

static void page_end(Buffer *b)
{
    append(b, "</main></body></html>\n");
}

static void post_meta(Buffer *b, const Post *p)
{
    if (!*p->date && !*p->author)
        return;
    append(b, "<div class=\"meta\">");
    if (*p->date) {
        append(b, "<time datetime=\"");
        escape_html(b, p->date);
        append(b, "\">");
        escape_html(b, p->date);
        append(b, "</time>");
    }
    if (*p->author) {
        if (*p->date)
            append(b, " &middot; ");
        escape_html(b, p->author);
    }
    append(b, "</div>");
}

static void post_tags(Buffer *b, const Post *p)
{
    if (*p->tags) {
        append(b, "<span class=\"tags\">");
        escape_html(b, p->tags);
        append(b, "</span>");
    }
}

static bool render_home(int blogfd, Buffer *b)
{
    int fd = openat(blogfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return false;
    }
    Post *posts = calloc(MAX_POSTS, sizeof(*posts));
    if (!posts) {
        closedir(dir);
        return false;
    }
    size_t count = 0;
    struct dirent *entry;
    bool success = true;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            success = errno == 0;
            break;
        }
        size_t len = strlen(entry->d_name);
        if (len <= 3 || len > 253 || strcmp(entry->d_name + len - 3, ".md") != 0)
            continue;
        char slug[256];
        memcpy(slug, entry->d_name, len - 3);
        slug[len - 3] = '\0';
        Post p;
        if (!load_post(blogfd, slug, &p))
            continue;
        free(p.document);
        p.document = p.body = NULL;
        if (p.draft)
            continue;
        if (count == MAX_POSTS) {
            fprintf(stderr, "Too many published posts (limit %u)\n", MAX_POSTS);
            success = false;
            break;
        }
        posts[count++] = p;
    }
    closedir(dir);
    if (!success) {
        free(posts);
        return false;
    }
    qsort(posts, count, sizeof(*posts), compare_posts);
    page_start(b, "Posts", "Blog posts, newest first.");
    append(b, "<h1>Posts</h1>");
    if (!count)
        append(b, "<p>No posts yet.</p>");
    for (size_t i = 0; i < count; i++) {
        const Post *p = &posts[i];
        append(b, "<article class=\"post-card\">");
        post_meta(b, p);
        append(b, "<h2><a href=\"/posts/");
        append(b, p->slug);
        append(b, "\">");
        escape_html(b, p->title);
        append(b, "</a></h2>");
        if (*p->description) {
            append(b, "<p>");
            escape_html(b, p->description);
            append(b, "</p>");
        }
        post_tags(b, p);
        append(b, "</article>");
    }
    page_end(b);
    free(posts);
    return !b->failed;
}

#ifndef USE_PANDOC
static void markdown_output(const MD_CHAR *s, MD_SIZE n, void *userdata)
{
    append_bytes(userdata, s, (size_t)n);
}

static bool render_markdown(Buffer *b, const char *markdown)
{
    return md_html(markdown, (MD_SIZE)strlen(markdown), markdown_output, b,
                   MD_DIALECT_GITHUB | MD_FLAG_NOHTML, MD_HTML_FLAG_SKIP_UTF8_BOM) == 0 &&
           !b->failed;
}
#else
static bool render_markdown(Buffer *b, const char *markdown)
{
    /* A temporary stdin file avoids deadlocking on full input/output pipes.
     * Pandoc receives the Markdown only, with no post filenames or shell. */
    FILE *input = tmpfile();
    if (!input) {
        perror("Pandoc input");
        return false;
    }
    size_t length = strlen(markdown);
    if (fwrite(markdown, 1, length, input) != length || fflush(input) == EOF ||
        fseek(input, 0, SEEK_SET) != 0) {
        perror("Writing Pandoc input");
        fclose(input);
        return false;
    }
    int output[2];
    if (pipe(output) < 0) {
        perror("Pandoc output pipe");
        fclose(input);
        return false;
    }
    int in = fileno(input);
    if (fcntl(in, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(output[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(output[1], F_SETFD, FD_CLOEXEC) < 0) {
        perror("Pandoc file descriptors");
        fclose(input);
        close(output[0]);
        close(output[1]);
        return false;
    }
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc) {
        fprintf(stderr, "Pandoc spawn setup: %s\n", strerror(rc));
        fclose(input);
        close(output[0]);
        close(output[1]);
        return false;
    }
    rc = posix_spawn_file_actions_adddup2(&actions, in, STDIN_FILENO);
    if (!rc && in != STDIN_FILENO)
        rc = posix_spawn_file_actions_addclose(&actions, in);
    if (!rc)
        rc = posix_spawn_file_actions_addclose(&actions, output[0]);
    if (!rc)
        rc = posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    if (!rc && output[1] != STDOUT_FILENO)
        rc = posix_spawn_file_actions_addclose(&actions, output[1]);
    char *const args[] = {
        "pandoc", "--sandbox",
        "--from=markdown+smart+task_lists-raw_html-raw_tex-raw_attribute-native_divs-native_spans",
        "--to=html5", "--wrap=none", "--math-method=mathml", NULL
    };
    pid_t child = -1;
    if (!rc)
        rc = posix_spawnp(&child, args[0], &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    fclose(input);
    close(output[1]);
    if (rc) {
        fprintf(stderr, "Cannot run pandoc: %s (install Pandoc in PATH)\n", strerror(rc));
        close(output[0]);
        return false;
    }

    bool ok = true, reaped = false;
    int status = 0, out = output[0];
    struct timespec start, now;
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    while (ok && !reaped && !stopping) {
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                       (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= 10000) {
            ok = false;
            break;
        }
        if (out < 0) {
            pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child)
                reaped = true;
            else if (waited < 0 && errno != EINTR)
                ok = false;
            else
                (void)poll(NULL, 0, 10);
            continue;
        }
        struct pollfd ready = {.fd = out, .events = POLLIN};
        rc = poll(&ready, 1, (int)(10000 - elapsed));
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0) {
            ok = false;
            break;
        }
        char chunk[8192];
        ssize_t n = read(out, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = false;
        } else if (n == 0) {
            close(out);
            out = -1;
        } else {
            append_bytes(b, chunk, (size_t)n);
            ok = !b->failed;
        }
    }
    if (out >= 0)
        close(out);
    if (!reaped) {
        (void)kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
            ;
    }
    if (!ok || !reaped || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (!stopping)
            fprintf(stderr, "Pandoc conversion failed or exceeded the 10-second/output limit\n");
        return false;
    }
    return true;
}
#endif

static bool render_post(Buffer *b, const Post *p)
{
    page_start(b, p->title, p->description);
    append(b, "<nav class=\"post-nav\" aria-label=\"Blog\">"
              "<a href=\"/\">&larr; All posts</a></nav>"
              "<article><header class=\"post-heading\">");
    post_meta(b, p);
    append(b, "<h1>");
    escape_html(b, p->title);
    append(b, "</h1>");
    if (*p->description) {
        append(b, "<p class=\"description\">");
        escape_html(b, p->description);
        append(b, "</p>");
    }
    post_tags(b, p);
    append(b, "</header><div class=\"prose\">");
    bool ok = render_markdown(b, p->body);
    append(b, "</div></article>");
    page_end(b);
    return ok && !b->failed;
}

static bool send_all(int fd, const char *data, size_t len)
{
    while (len && !stopping) {
        ssize_t n = send(fd, data, len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += (size_t)n;
        len -= (size_t)n;
    }
    return len == 0;
}

static void respond(int fd, int status, const char *reason, const char *type,
                    const char *body, size_t length, bool head)
{
    char headers[1536];
    time_t now = time(NULL);
    struct tm tm;
    char date[64] = "";
    if (gmtime_r(&now, &tm))
        (void)strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    int n = snprintf(headers, sizeof(headers),
        "HTTP/1.1 %d %s\r\nDate: %s\r\nContent-Type: %s\r\n"
        "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-cache\r\n"
        "X-Content-Type-Options: nosniff\r\nReferrer-Policy: strict-origin-when-cross-origin\r\n"
        "Content-Security-Policy: default-src 'none'; style-src 'self'; "
        "img-src 'self' https: http: data:; base-uri 'none'; form-action 'none'; "
        "frame-ancestors 'none'\r\n%s\r\n",
        status, reason, date, type, length, status == 405 ? "Allow: GET, HEAD\r\n" : "");
    if (n > 0 && (size_t)n < sizeof(headers) && send_all(fd, headers, (size_t)n) && !head)
        (void)send_all(fd, body, length);
}

static void error_response(int fd, int status, const char *reason, bool head)
{
    Buffer b = {0};
    page_start(&b, reason, reason);
    append(&b, "<h1>");
    escape_html(&b, reason);
    append(&b, "</h1><p><a href=\"/\">All posts</a></p>");
    page_end(&b);
    if (!b.failed)
        respond(fd, status, reason, "text/html; charset=utf-8", b.data, b.len, head);
    else
        respond(fd, 500, "Internal Server Error", "text/plain; charset=utf-8", "Server error\n", 13, head);
    free(b.data);
}

/* Decode once; reject controls and backslashes before routing. */
static int hex_digit(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool decode_path(char *path)
{
    char *out = path;
    for (char *in = path; *in; in++) {
        unsigned char ch = (unsigned char)*in;
        if (ch == '%') {
            if (!in[1] || !in[2])
                return false;
            int hi = hex_digit((unsigned char)in[1]), lo = hex_digit((unsigned char)in[2]);
            if (hi < 0 || lo < 0)
                return false;
            ch = (unsigned char)(hi * 16 + lo);
            in += 2;
        }
        if (ch < 0x20 || ch == 0x7f || ch == '\\')
            return false;
        *out++ = (char)ch;
    }
    *out = '\0';
    return true;
}

/* Walk each asset component using directory descriptors; never follow symlinks. */
static int open_asset(int blogfd, const char *path)
{
    if (!*path || strlen(path) >= 1024)
        return -1;
    int dirfd = openat(blogfd, "assets", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0)
        return -1;
    char rest[1024];
    strcpy(rest, path);
    char *part = rest;
    for (;;) {
        char *slash = strchr(part, '/');
        if (slash)
            *slash = '\0';
        if (!*part || *part == '.') {
            close(dirfd);
            return -1;
        }
        int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
        if (slash)
            flags |= O_DIRECTORY;
        int next = openat(dirfd, part, flags);
        close(dirfd);
        if (next < 0 || !slash)
            return next;
        dirfd = next;
        part = slash + 1;
    }
}

static const char *asset_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcasecmp(ext, ".png")) return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".gif")) return "image/gif";
    if (!strcasecmp(ext, ".webp")) return "image/webp";
    if (!strcasecmp(ext, ".avif")) return "image/avif";
    if (!strcasecmp(ext, ".svg")) return "image/svg+xml";
    if (!strcasecmp(ext, ".ico")) return "image/x-icon";
    if (!strcasecmp(ext, ".pdf")) return "application/pdf";
    if (!strcasecmp(ext, ".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static void handle_client(int fd, int blogfd)
{
    struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    char request[REQUEST_BYTES + 1];
    size_t used = 0;
    /* A fixed deadline also bounds clients that trickle bytes inside the timeout. */
    struct timespec start, now;
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    while (!stopping) {
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                       (now.tv_nsec - start.tv_nsec) / 1000000;
        struct pollfd ready = {.fd = fd, .events = POLLIN};
        int rc = elapsed >= 3000 ? 0 : poll(&ready, 1, (int)(3000 - elapsed));
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0) {
            if (!stopping)
                error_response(fd, 408, "Request Timeout", false);
            return;
        }
        ssize_t n = recv(fd, request + used, REQUEST_BYTES - used, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n < 0 || used)
                error_response(fd, n < 0 ? 408 : 400, n < 0 ? "Request Timeout" : "Bad Request", false);
            return;
        }
        if (memchr(request + used, '\0', (size_t)n)) {
            error_response(fd, 400, "Bad Request", false);
            return;
        }
        used += (size_t)n;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n"))
            break;
        if (used == REQUEST_BYTES) {
            error_response(fd, 431, "Request Header Fields Too Large", false);
            return;
        }
    }
    if (stopping)
        return;
    char *line_end = strstr(request, "\r\n");
    if (!line_end) {
        error_response(fd, 400, "Bad Request", false);
        return;
    }
    *line_end = '\0';
    char method[16], path[2048], version[16], extra;
    if (sscanf(request, "%15s %2047s %15s %c", method, path, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0")) || path[0] != '/') {
        error_response(fd, 400, "Bad Request", false);
        return;
    }
    bool head = strcmp(method, "HEAD") == 0;
    if (strcmp(method, "GET") && !head) {
        error_response(fd, 405, "Method Not Allowed", false);
        return;
    }
    char *query = strchr(path, '?');
    if (query)
        *query = '\0';
    if (!decode_path(path)) {
        error_response(fd, 400, "Bad Request", head);
        return;
    }
    if (strcmp(path, "/style.css") == 0) {
        respond(fd, 200, "OK", "text/css; charset=utf-8", stylesheet, sizeof(stylesheet)-1, head);
        return;
    }
    if (strncmp(path, "/assets/", 8) == 0) {
        int assetfd = open_asset(blogfd, path + 8);
        size_t length = 0;
        char *data = assetfd >= 0 ? read_file(assetfd, MAX_ASSET_BYTES, &length) : NULL;
        if (assetfd >= 0)
            close(assetfd);
        if (!data)
            error_response(fd, 404, "Not Found", head);
        else
            respond(fd, 200, "OK", asset_type(path), data, length, head);
        free(data);
        return;
    }
    Buffer b = {0};
    bool ok;
    if (strcmp(path, "/") == 0) {
        ok = render_home(blogfd, &b);
    } else if (strncmp(path, "/posts/", 7) == 0) {
        Post p;
        if (!load_post(blogfd, path + 7, &p)) {
            error_response(fd, 404, "Not Found", head);
            return;
        }
        if (p.draft) {
            free(p.document);
            error_response(fd, 404, "Not Found", head);
            return;
        }
        ok = render_post(&b, &p);
        free(p.document);
    } else {
        error_response(fd, 404, "Not Found", head);
        return;
    }
    if (ok)
        respond(fd, 200, "OK", "text/html; charset=utf-8", b.data, b.len, head);
    else
        error_response(fd, 500, "Internal Server Error", head);
    free(b.data);
}

int main(int argc, char **argv)
{
    long port = 8080;
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("Usage: %s [port]\nServe ./blog on 127.0.0.1 (default port 8080).\n", argv[0]);
#ifdef USE_PANDOC
        puts("Markdown renderer: Pandoc (requires pandoc in PATH).");
#else
        puts("Markdown renderer: MD4C.");
#endif
        return 0;
    }
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return 1;
    }
    if (argc == 2) {
        char *end;
        errno = 0;
        port = strtol(argv[1], &end, 10);
        if (errno || !*argv[1] || *end || port < 1 || port > 65535) {
            fprintf(stderr, "Port must be an integer between 1 and 65535\n");
            return 1;
        }
    }
    int blogfd = open("blog", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (blogfd < 0) {
        perror("Cannot open ./blog");
        return 1;
    }
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        close(blogfd);
        return 1;
    }
    if (fcntl(server, F_SETFD, FD_CLOEXEC) < 0) {
        perror("Server file descriptor");
        close(server);
        close(blogfd);
        return 1;
    }
    int reuse = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 64) < 0) {
        perror("Cannot listen");
        close(server);
        close(blogfd);
        return 1;
    }
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = stop_server;
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    action.sa_handler = SIG_IGN;
    (void)sigaction(SIGPIPE, &action, NULL);
    printf("Serving ./blog at http://127.0.0.1:%ld\n", port);
    fflush(stdout);
    int result = 0;
    while (!stopping) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            result = 1;
            break;
        }
        if (fcntl(client, F_SETFD, FD_CLOEXEC) < 0)
            perror("Client file descriptor");
        else
            handle_client(client, blogfd);
        close(client);
    }
    close(server);
    close(blogfd);
    return result;
}
