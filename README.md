# Markdown blog server

A small blog server written in C. It reads `blog/` in its **working directory**,
uses a `BLOG INFO` comment for metadata, and renders the remaining Markdown.
Posts appear on the homepage automatically, newest dated posts first. The server
uses plain HTTP and listens only on `127.0.0.1`, ready for an nginx proxy.

## Build and run

Requires a POSIX system (such as Linux), a C compiler, Make, pkg-config, and
the xxHash development library. The default renderer also needs the MD4C HTML
development library.
On Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libxxhash-dev libmd4c-html0-dev
make
./blog-server
```

Visit **http://127.0.0.1:8080**. To choose another port:

```sh
./blog-server 9000
```

Run the executable from the directory containing your `blog/` folder. It reports
an error if that directory is missing. Stop with Ctrl+C or SIGTERM.

`style.css` and `copy.js` are the readable CSS and JavaScript sources. `make`
converts them into embedded C arrays using a tiny C build helper. After changing
either file, rebuild and restart the server. The deployed executable needs no
separate stylesheet, script, or template files;
xxHash is a shared library dependency of both builds; MD4C is also needed at
runtime with the default build. On Gentoo, xxHash is `dev-libs/xxhash`.

### Pandoc renderer (compile-time option)

To use Pandoc instead of MD4C:

```sh
make USE_PANDOC=1
./blog-server
```

This build needs no MD4C headers or libraries. It runs the `pandoc`
executable from `PATH` when a post needs rendering. Install Pandoc **3.11 or newer**
on the server and make sure the OpenRC service account can run it. For example,
on Gentoo:

```sh
sudo emerge --ask app-text/pandoc
```

The renderer uses [Pandoc's extended Markdown](https://pandoc.org/MANUAL.html#pandocs-markdown),
including smart quotes and dashes, footnotes, definition lists, heading IDs,
and highlighted fenced code. TeX math uses native MathML, without JavaScript.
The embedded stylesheet includes styles for these elements in light and dark mode.
`BLOG INFO`, homepage metadata, asset URLs, and drafts work the same way.

Pandoc receives only the Markdown body through stdin, without a shell, and
returns an HTML fragment inside the existing page layout. Each conversion has
a ten-second deadline and the same HTML output limit. A missing executable or
failed conversion logs an error and returns HTTP 500; the homepage remains
available. The server needs a writable temporary directory for conversion input.
Pandoc starts on a cache miss, so the first request for a post or an edited
version adds overhead compared with MD4C. Unchanged posts reuse cached HTML.

Switch back with `make USE_PANDOC=0` (or plain `make`). Switching renderers
automatically rebuilds the executable; `make clean` is unnecessary. Reinstall
the executable and restart the service after switching. `./blog-server --help`
shows which renderer was compiled in.

## Render cache

The server caches rendered post pages and the homepage in memory. On every post
request, including HEAD, it reads the current file and computes its XXH3 64-bit
hash over the complete source, including `BLOG INFO`. A matching hash, length,
and exact byte comparison reuse the cached HTML without running the renderer.
Checking contents detects edits even when a file's size and modification time
stay the same.

The homepage rescans `blog/` and hashes the sources on every request. Its cache
uses the sorted post inventory, content hashes, and displayed metadata, so new,
renamed, edited, deleted, invalid, or draft posts are reflected immediately.
Drafts and missing or invalid files return 404 even if they were cached earlier.
A failed conversion returns an error rather than the old page; failures are
retried on the next request.

The cache holds up to 128 pages and 64 MiB total, including saved source bytes,
HTML, and entry structures. It evicts the least recently used entries when
either limit is reached. Allocation failures leave pages available without
caching, and the cache starts empty after a restart. Assets are read directly;
CSS and the copy-button script remain embedded in the executable.

HTTP responses keep `Cache-Control: no-cache`, and the nginx configuration
keeps proxy caching disabled, so requests reach the server for content validation.
Publish edits by renaming a complete temporary file into place, as described
below. The server rejects files detected changing during a read.

## Write a post

Create `blog/my-first-post.md`:

```markdown
/* BLOG INFO
title: My first post
date: 2026-10-22
description: A short introduction that appears on the homepage.
author: Lily
tags: life, programming
draft: false
*/

This is **Markdown**. The metadata title is already rendered as the page heading.

## A section

- Lists, links, images, and blockquotes work.
- Fenced code, tables, strikethrough, and task lists work too.
```

The URL is `/posts/my-first-post`. Filenames must end in lowercase `.md`; the
name before `.md` must contain only ASCII letters, numbers, hyphens, or
underscores (1–250 characters). Only files directly inside `blog/` are posts.
Create, edit, or delete a post and refresh to see the change. For updates while
serving, write a temporary file and rename it over the destination so readers
see a complete document.

Metadata uses one `key: value` per line, with whitespace trimmed. Values are
plain text, without surrounding quotes. A colon within a value is allowed.
The first line must be `/* BLOG INFO` and the comment must end with `*/`.
UTF-8 BOMs and Windows line endings are supported. Use UTF-8 for your posts.

| Key | Behaviour |
| --- | --- |
| `title` | Required, nonempty; shown on the homepage and post. Up to 511 bytes. |
| `date` | Optional valid calendar date, exactly `YYYY-MM-DD`. Sorted descending; undated posts come last. |
| `description` | Optional summary shown on the homepage and beneath the title, also used as the page's meta description. Up to 2047 bytes. |
| `author` | Optional byline, up to 255 bytes. |
| `tags` | Optional plain text, conventionally comma separated, up to 511 bytes. |
| `draft` | `true` hides the post from both the homepage and direct access; defaults to `false`. |

Unknown keys are ignored, and repeated known keys use the last value. Dates
with the same value sort by filename. A future date does **not** schedule
publication; use `draft: true` to keep a post unpublished.

Invalid headers are logged to stderr and skipped. Limits are 4 MiB per post,
16 KiB of metadata, 2,048 published posts, and 32 MiB of generated HTML per
response. Exceeding the published post limit returns an error rather than
displaying an arbitrary subset.

## Images and downloads

Place public assets under `blog/assets/`, including subdirectories, and use
root-relative URLs:

```markdown
![A quiet morning](/assets/photos/morning.jpg)
[Download the notes](/assets/notes.pdf)
```

Assets have a 16 MiB size limit. Symlinks, hidden path components, directories,
and paths escaping this folder are rejected. Everything else in `blog/assets/`
is public. Post sources and files elsewhere in the working directory are not
served. Remote image URLs work too.

Metadata is HTML escaped. Raw HTML in Markdown is displayed as text. A Content
Security Policy blocks inline scripts while allowing the site's embedded
copy-button script. The default MD4C renderer uses
CommonMark syntax and GitHub-style extensions. The optional Pandoc renderer
uses its extended Markdown dialect, with raw HTML, raw TeX, and raw attributes
disabled and Pandoc's sandbox enabled.

## Appearance

Edit `style.css` for the layout, colours, typography, responsive styling, and
automatic dark mode. The homepage lists posts directly; article pages have an
"All posts" link above the title. Page markup is in `page_start()`,
`render_home()`, and `render_post()` in `main.c`.

Code blocks get a copy icon in their right padding with JavaScript enabled,
including Pandoc's highlighted blocks. It copies plain code text with indentation
and line breaks preserved, and briefly changes to a checkmark on success.
The button has a tooltip and a screen-reader label. If copying is blocked, the code is
selected for manual copying. The button uses the browser clipboard API, with
a selection-based fallback. Its source is `copy.js`, served at `/copy.js`.

## OpenRC

The service in `deploy/openrc/blog-server` runs the server as `blog:blog`, sets
its working directory to `/srv/fieldnotes` (so posts live in
`/srv/fieldnotes/blog/`), and uses OpenRC's `supervise-daemon` to restart it after
a crash. It allows up to five restarts within a minute, with a two-second delay.
Logs go to `/var/log/blog-server/output.log` and `error.log`.

Create a system user and group named `blog` using your distribution's account
tools. For example, on Gentoo:

```sh
sudo groupadd --system blog
sudo useradd --system --gid blog --home-dir /srv/fieldnotes --no-create-home --shell /sbin/nologin blog
```

Build and install the executable, content, and service files:

```sh
make
sudo install -m 0755 blog-server /usr/local/bin/blog-server
sudo install -d -m 0755 /srv/fieldnotes/blog
sudo cp -R blog/. /srv/fieldnotes/blog/
sudo install -m 0755 deploy/openrc/blog-server /etc/init.d/blog-server
sudo install -m 0644 deploy/openrc/blog-server.confd /etc/conf.d/blog-server
sudo rc-update add blog-server default
sudo rc-service blog-server start
```

The `blog` account needs permission to read posts and traverse their directories.
Edit `/etc/conf.d/blog-server` to change the executable, working directory,
account, port, or log directory. Restart with `sudo rc-service blog-server restart`
after changing those settings or replacing the executable. Post edits still
take effect immediately. Ensure the deployment machine has the xxHash runtime
library, plus MD4C for the default build or `pandoc` in the service's `PATH`
for the Pandoc build.

## nginx

`deploy/nginx/blog.conf` redirects HTTP on port 80 to HTTPS on port 443 with a
301 response, preserving the path and query string. The HTTPS server proxies
requests to `127.0.0.1:8080`, so the C server continues to use plain HTTP.

The configuration uses `blog.soccera.uk` and reuses the certificate and private
key already configured for the other soccera.uk nginx sites:
`/var/www/xeetapus/soccera.uk.pem` and
`/var/www/xeetapus/soccera.uk.rsa.key`. nginx must be able to read these existing
files. No new certificate is needed.

Install it in a directory included from your nginx `http {}` block. For a setup
that includes `/etc/nginx/conf.d/*.conf`:

```sh
sudo install -m 0644 deploy/nginx/blog.conf /etc/nginx/conf.d/blog.conf
sudo nginx -t
sudo rc-update add nginx default
sudo rc-service nginx reload
```

If nginx is not running yet, use `sudo rc-service nginx start` instead of reload.
On distributions that use `sites-available` and `sites-enabled`, install there
and enable the site instead. Keep the nginx `proxy_pass` port in sync with
`blog_port` in `/etc/conf.d/blog-server`.

The blog expects to be served at the domain root. nginx terminates HTTPS and
handles client connections. The C server handles one request at a
time, supports GET and HEAD, closes each connection after its response, and
limits request headers to 8 KiB with a three-second header deadline. Responses
use `Cache-Control: no-cache` so file edits are visible after refresh.

## Check

```sh
make test
# Or, with Pandoc installed:
make test USE_PANDOC=1
```

Integration tests require Python 3's standard library. They start the compiled
server with temporary posts, exercise real HTTP requests, and check metadata,
Markdown, live updates, embedded CSS, assets, HTTP errors, drafts, and path
traversal handling. Cache tests check invalidation with unchanged timestamps,
metadata edits, draft transitions, deletion, and replacement. The Pandoc build
also checks cache hits with a counted renderer, eviction, extended formatting,
large conversions, subprocess failures, output limits, and conversion timeouts.
