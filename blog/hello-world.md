/* BLOG INFO
title: Writing a post
date: 2026-10-22
description: How to add posts, metadata, images, and code blocks.
tags: Markdown, publishing
draft: false
*/

Create a `.md` file directly inside `blog/`. The filename becomes the URL:
`blog/my-post.md` is served at `/posts/my-post`.

## Metadata

Start the file with a `BLOG INFO` comment:

```text
/* BLOG INFO
title: My post
date: 2026-10-22
description: A short summary for the homepage.
tags: programming, notes
draft: false
*/
```

Only `title` is required. Add `author` for a byline, or set `draft: true` to
hide the post from both the homepage and direct access.

## Markdown

Write Markdown after the comment. The metadata title is already shown as the
page heading, so start sections with `##`.

Fenced code blocks include the language name after the opening backticks:

```c
#include <stdio.h>

int main(void) {
    puts("Hello, world!");
    return 0;
}
```

## Images and updates

Place images in `blog/assets/` and reference them with a root-relative URL:

```markdown
![Image description](/assets/photo.png)
```

Post edits appear on refresh. To change the layout or colours, edit `style.css`,
run `make`, and restart the server.
