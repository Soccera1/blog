"""Integration checks against a real server and an isolated content directory."""
import http.client
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else './blog-server').resolve()
SOURCE_CSS = (Path(__file__).resolve().parent.parent / 'style.css').read_bytes()
USE_PANDOC = b'Markdown renderer: Pandoc' in subprocess.check_output([str(BINARY), '--help'])


class BlogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.blog = self.root / 'blog'
        self.blog.mkdir()
        self.assets = self.blog / 'assets'
        self.assets.mkdir()
        self.post('older', 'title: Older post\ndate: 2024-01-01', 'Old body')
        self.post('newer', 'title: Newer post\ndate: 2026-10-22\n'
                  'description: A short summary\nauthor: A. Writer\ntags: C, notes',
                  'Body only on the post.\n\n## Heading\n\n**bold** and *italic*\n\n'
                  '- one\n  - nested\n\n> a quote\n\n'
                  '```c\nputs("hello");\n```\n\n'
                  '| Name | Value |\n| --- | --- |\n| C | 11 |\n\n'
                  '~~gone~~\n\n- [x] done\n\n'
                  '[A link](https://example.com)\n\n![Photo](/assets/photo.png)')
        self.post('undated', 'title: Undated', 'An undated post')
        self.post('private', 'title: Private draft\ndraft: true', 'Secret words')
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            self.port = listener.getsockname()[1]
        self.start_server()

    def start_server(self, env=None):
        self.server = subprocess.Popen(
            [str(BINARY), str(self.port)], cwd=self.root,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env,
        )
        self.addCleanup(self.stop_server, self.server)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.server.poll() is not None:
                self.fail(f'Server exited: {self.server.communicate()!r}')
            try:
                with socket.create_connection(('127.0.0.1', self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.01)
        self.fail('Server failed to start')

    def stop_server(self, process):
        if process.poll() is None:
            process.terminate()
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            self.fail('Server did not stop after SIGTERM')
        self.assertEqual(process.returncode, 0)

    def post(self, slug, metadata, body, newline='\n', bom=False):
        content = f'/* BLOG INFO\n{metadata}\n*/\n\n{body}\n'
        if bom:
            content = '\ufeff' + content
        (self.blog / (slug + '.md')).write_bytes(content.replace('\n', newline).encode())

    def get(self, path='/', method='GET', timeout=5):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=timeout)
        try:
            connection.request(method, path)
            response = connection.getresponse()
            data = response.read()
            if method != 'HEAD':
                self.assertEqual(len(data), int(response.getheader('Content-Length')))
            return response.status, dict(response.getheaders()), data
        finally:
            connection.close()

    def raw(self, data, chunks=None):
        with socket.create_connection(('127.0.0.1', self.port), timeout=5) as client:
            if chunks:
                for chunk in chunks:
                    client.sendall(chunk)
                    time.sleep(.02)
            else:
                client.sendall(data)
            client.shutdown(socket.SHUT_WR)
            result = bytearray()
            while True:
                try:
                    chunk = client.recv(65536)
                except ConnectionResetError:
                    break
                if not chunk:
                    break
                result.extend(chunk)
            return bytes(result)

    def test_home_metadata_order_and_repeat_scan(self):
        for _ in range(3):
            status, headers, body = self.get()
            self.assertEqual(status, 200)
            self.assertLess(body.index(b'Newer post'), body.index(b'Older post'))
            self.assertLess(body.index(b'Older post'), body.index(b'Undated'))
            for text in (b'A short summary', b'A. Writer', b'C, notes', b'2026-10-22',
                         b'href="/posts/newer"'):
                self.assertIn(text, body)
            self.assertNotIn(b'Private draft', body)
            self.assertNotIn(b'Body only on the post', body)
            self.assertEqual(headers['Cache-Control'], 'no-cache')

    def test_markdown(self):
        status, headers, body = self.get('/posts/newer')
        self.assertEqual(status, 200)
        for text in (b'<strong>bold</strong>', b'<em>italic</em>',
                     b'<blockquote>', b'<li>nested</li>', b'<table>',
                     b'<del>gone</del>', b'type="checkbox"',
                     b'href="https://example.com"', b'src="/assets/photo.png"'):
            self.assertIn(text, body)
        self.assertRegex(body, rb'<h2\b[^>]*>Heading</h2>')
        self.assertRegex(body, rb'<ul\b[^>]*>')
        self.assertRegex(body, rb'<pre\b[^>]*>\s*<code\b')
        self.assertNotIn(b'BLOG INFO', body)
        self.assertIn("default-src 'none'", headers['Content-Security-Policy'])

    def test_escaping_and_raw_html(self):
        self.post('escaping', 'title: <script>bad</script> & "test"\n'
                  'description: <img src=x onerror=bad>\nauthor: A & B\ntags: <tag>',
                  '<script>alert(1)</script>\n\n<span onclick="evil()">text</span>\n\n'
                  '<div onclick="evil()">text</div>\n\n'
                  '```{=html}\n<script>raw_attribute()</script>\n```')
        for path in ('/', '/posts/escaping'):
            status, _, body = self.get(path)
            self.assertEqual(status, 200)
            self.assertNotIn(b'<script>', body)
            self.assertNotIn(b'<img src=x', body)
            self.assertIn(b'&lt;script&gt;', body)
            self.assertIn(b'A &amp; B', body)
        body = self.get('/posts/escaping')[2]
        self.assertNotIn(b'<span onclick=', body)
        self.assertNotIn(b'<div onclick=', body)
        self.assertNotIn(b'<script>', body)

    def test_live_create_edit_delete(self):
        self.post('live', 'title: First version', 'first')
        self.assertIn(b'First version', self.get()[2])
        self.post('live', 'title: Second version', 'second')
        self.assertIn(b'Second version', self.get()[2])
        self.assertIn(b'<p>second</p>', self.get('/posts/live')[2])
        (self.blog / 'live.md').unlink()
        self.assertNotIn(b'Second version', self.get()[2])
        self.assertEqual(self.get('/posts/live')[0], 404)

    def test_embedded_css_without_runtime_file(self):
        self.assertFalse((self.root / 'style.css').exists())
        status, headers, css = self.get('/style.css')
        self.assertEqual(status, 200)
        self.assertEqual(headers['Content-Type'], 'text/css; charset=utf-8')
        self.assertEqual(css, SOURCE_CSS)
        self.assertIn(b'prefers-color-scheme: dark', css)

    def test_head_errors_and_methods(self):
        for path in ('/', '/posts/newer', '/style.css', '/missing', '/posts/private'):
            get_status, get_headers, _ = self.get(path)
            status, headers, body = self.get(path, 'HEAD')
            self.assertEqual(status, get_status)
            self.assertEqual(headers['Content-Length'], get_headers['Content-Length'])
            self.assertEqual(body, b'')
        status, headers, _ = self.get('/', 'POST')
        self.assertEqual(status, 405)
        self.assertEqual(headers['Allow'], 'GET, HEAD')
        self.assertEqual(self.get('/posts/private')[0], 404)

    def test_query_and_url_encoding(self):
        self.assertEqual(self.get('/?x=1')[0], 200)
        self.assertEqual(self.get('/posts/%6eewer?preview=1')[0], 200)
        for path in ('/%00', '/posts/newer%0a', '/posts/%xx', '/%', '/%5c'):
            self.assertEqual(self.get(path)[0], 400, path)

    def test_assets_and_path_boundaries(self):
        content = b'\x89PNG\r\n\x1a\n\x00binary\xff'
        (self.assets / 'photo.png').write_bytes(content)
        (self.assets / 'nested').mkdir()
        (self.assets / 'nested' / 'notes.txt').write_text('hello')
        (self.root / 'secret.txt').write_text('secret')
        (self.assets / '.hidden').write_text('hidden')
        (self.assets / 'leak.txt').symlink_to(self.root / 'secret.txt')
        (self.assets / 'alias').symlink_to(self.root, target_is_directory=True)
        (self.blog / 'alias.md').symlink_to(self.blog / 'newer.md')
        status, headers, data = self.get('/assets/photo.png')
        self.assertEqual((status, data), (200, content))
        self.assertEqual(headers['Content-Type'], 'image/png')
        self.assertEqual(self.get('/assets/nested/notes.txt')[2], b'hello')
        self.assertEqual(self.get('/assets/photo.png', 'HEAD')[2], b'')
        for path in ('/assets/../newer.md', '/assets/%2e%2e/newer.md',
                     '/assets/nested/../../newer.md', '/assets/%252e%252e/newer.md',
                     '/assets/leak.txt', '/assets/alias/secret.txt', '/assets/.hidden',
                     '/assets/', '/assets/nested/', '/assets//photo.png',
                     '/posts/alias', '/posts/../newer', '/blog/newer.md', '/main.c'):
            self.assertEqual(self.get(path)[0], 404, path)
        (self.assets / 'photo.png').unlink()
        (self.assets / 'nested' / 'notes.txt').unlink()
        (self.assets / 'nested').rmdir()
        (self.assets / '.hidden').unlink()
        (self.assets / 'leak.txt').unlink()
        (self.assets / 'alias').unlink()
        self.assets.rmdir()
        self.assets.symlink_to(self.root, target_is_directory=True)
        self.assertEqual(self.get('/assets/secret.txt')[0], 404)

    def test_invalid_posts_are_skipped(self):
        cases = {
            'missing': 'date: 2024-01-01',
            'empty': 'title: ',
            'date': 'title: Bad\ndate: 2023-02-29',
            'month': 'title: Bad\ndate: 2024-13-01',
            'draft': 'title: Bad\ndraft: perhaps',
            'syntax': 'title: Bad\nthis is not a field',
            'long-title': 'title: ' + 'x' * 512,
            'big-info': 'title: Bad\nignored: ' + 'x' * 16384,
        }
        for slug, metadata in cases.items():
            self.post(slug, metadata, 'bad body')
        (self.blog / 'no-header.md').write_text('# No header')
        (self.blog / 'not-closed.md').write_text('/* BLOG INFO\ntitle: Bad\n')
        (self.blog / 'nul.md').write_bytes(b'/* BLOG INFO\ntitle: Bad\n*/\n\x00oops')
        self.post('bad name', 'title: Invalid filename', 'body')
        os.mkfifo(self.blog / 'pipe.md')
        for slug in (*cases, 'no-header', 'not-closed', 'nul', 'pipe'):
            self.assertEqual(self.get('/posts/' + slug)[0], 404, slug)
        status, _, body = self.get()
        self.assertEqual(status, 200)
        self.assertIn(b'Newer post', body)
        self.assertNotIn(b'Invalid filename', body)
        self.assertNotIn(b'bad body', body)

    def test_bom_crlf_colons_unknown_fields_and_leap_date(self):
        self.post('windows', 'title: Some notes: part two\ndate: 2024-02-29\n'
                  'custom: ignored\nauthor: Renée', 'Café **works**', newline='\r\n', bom=True)
        status, _, body = self.get('/posts/windows')
        self.assertEqual(status, 200)
        self.assertIn(b'Some notes: part two', body)
        self.assertIn('Renée'.encode(), body)
        self.assertIn('Café <strong>works</strong>'.encode(), body)

    def test_size_limits(self):
        self.post('oversized', 'title: Too big', 'x' * (4 * 1024 * 1024))
        self.assertEqual(self.get('/posts/oversized')[0], 404)
        with (self.assets / 'oversized.txt').open('wb') as out:
            out.truncate(16 * 1024 * 1024 + 1)
        self.assertEqual(self.get('/assets/oversized.txt')[0], 404)

    def test_empty_home(self):
        for path in self.blog.glob('*.md'):
            path.unlink()
        status, _, body = self.get()
        self.assertEqual(status, 200)
        self.assertIn(b'No posts yet', body)

    def test_malformed_and_fragmented_requests(self):
        for data in (b'bad\r\n\r\n', b'GET / HTTP/2\r\n\r\n',
                     b'GET / HTTP/1.1 extra\r\n\r\n', b'GET /\x00 HTTP/1.1\r\n\r\n',
                     b'GET / HTTP/1.1\r\nHost: incomplete'):
            self.assertTrue(self.raw(data).startswith(b'HTTP/1.1 400'), data)
        data = b'GET / HTTP/1.1\r\nHost: test\r\nX-Fill: ' + b'x' * 8200
        self.assertTrue(self.raw(data).startswith(b'HTTP/1.1 431'))
        result = self.raw(b'', chunks=[b'GET /po', b'sts/newer HTTP/1.1\r\n',
                                      b'Host: test\r\n', b'\r\n'])
        self.assertTrue(result.startswith(b'HTTP/1.1 200'))
        self.assertRegex(result, rb'<h2\b[^>]*>Heading</h2>')

    @unittest.skipUnless(USE_PANDOC, 'Pandoc build only')
    def test_pandoc_extended_markdown(self):
        self.post('extended', 'title: Extended Markdown',
                  '"Smart quotes" -- and an ellipsis...\n\n'
                  '## A heading\n\nTerm\n: Definition text\n\n'
                  'A note[^1].\n\n[^1]: Footnote text.\n\n'
                  '```c\nconst char *s = "hello";\n```\n\n$x^2$')
        status, _, body = self.get('/posts/extended')
        self.assertEqual(status, 200)
        self.assertIn('“Smart quotes” – and an ellipsis…'.encode(), body)
        for text in (b'id="a-heading"', b'<dl>', b'<dt>Term</dt>',
                     b'Definition text', b'Footnote text.', b'role="doc-noteref"',
                     b'class="sourceCode', b'class="st"', b'<math ', b'<msup>'):
            self.assertIn(text, body)
        self.assertNotIn(b'<script', body)

    @unittest.skipUnless(USE_PANDOC, 'Pandoc build only')
    def test_pandoc_large_input_and_output(self):
        text = 'A complete paragraph with **emphasis**.\n\n' * 10000
        self.post('large', 'title: Large post', text + 'Last paragraph.')
        status, _, body = self.get('/posts/large')
        self.assertEqual(status, 200)
        self.assertEqual(body.count(b'<strong>emphasis</strong>'), 10000)
        self.assertIn(b'Last paragraph.', body)

    def fake_pandoc(self, program=None):
        self.stop_server(self.server)
        path = self.root / 'bin'
        path.mkdir()
        executable = path / 'pandoc'
        if program is not None:
            executable.write_text(f'#!{sys.executable}\n' + program)
            executable.chmod(0o755)
        self.start_server(env={**os.environ, 'PATH': str(path)})
        return executable

    @unittest.skipUnless(USE_PANDOC, 'Pandoc build only')
    def test_pandoc_missing_executable(self):
        self.fake_pandoc()
        self.assertEqual(self.get('/posts/newer')[0], 500)
        self.assertEqual(self.get()[0], 200)

    @unittest.skipUnless(USE_PANDOC, 'Pandoc build only')
    def test_pandoc_failure_and_recovery(self):
        executable = self.fake_pandoc('raise SystemExit(7)\n')
        self.assertEqual(self.get('/posts/newer')[0], 500)
        self.assertEqual(self.get()[0], 200)
        executable.write_text(f'#!{sys.executable}\nprint("<p>Recovered</p>")\n')
        status, _, body = self.get('/posts/newer')
        self.assertEqual(status, 200)
        self.assertIn(b'<p>Recovered</p>', body)

    @unittest.skipUnless(USE_PANDOC, 'Pandoc build only')
    def test_pandoc_output_limit(self):
        self.fake_pandoc('import sys\nsys.stdout.write("x" * (33 * 1024 * 1024))\n')
        self.assertEqual(self.get('/posts/newer')[0], 500)
        self.assertEqual(self.get()[0], 200)

    @unittest.skipUnless(USE_PANDOC, 'Pandoc build only')
    def test_pandoc_deadline_after_output_closes(self):
        self.fake_pandoc('import os, time\nos.close(1)\ntime.sleep(60)\n')
        start = time.monotonic()
        self.assertEqual(self.get('/posts/newer', timeout=15)[0], 500)
        self.assertLess(time.monotonic() - start, 12)
        self.assertEqual(self.get()[0], 200)

    @unittest.skipUnless(USE_PANDOC, 'Pandoc build only')
    def test_pandoc_stops_with_server(self):
        self.fake_pandoc('import os, time\nfrom pathlib import Path\n'
                         'Path("child.pid").write_text(str(os.getpid()))\ntime.sleep(60)\n')
        with socket.create_connection(('127.0.0.1', self.port), timeout=5) as client:
            client.sendall(b'GET /posts/newer HTTP/1.1\r\nHost: test\r\n\r\n')
            pid_file = self.root / 'child.pid'
            deadline = time.monotonic() + 5
            while not pid_file.exists() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue(pid_file.exists(), 'Renderer did not start')
            child = int(pid_file.read_text())
            self.stop_server(self.server)
            with self.assertRaises(ProcessLookupError):
                os.kill(child, 0)

    def test_header_timeout(self):
        with socket.create_connection(('127.0.0.1', self.port), timeout=5) as client:
            client.sendall(b'GET / HTTP/1.1\r\n')
            self.assertTrue(client.recv(4096).startswith(b'HTTP/1.1 408'))
        self.assertEqual(self.get()[0], 200)

    def test_startup_validation(self):
        for arg in ('0', '65536', 'abc', '8080oops'):
            result = subprocess.run([str(BINARY), arg], cwd=self.root, capture_output=True)
            self.assertEqual(result.returncode, 1)
        with tempfile.TemporaryDirectory() as empty:
            result = subprocess.run([str(BINARY)], cwd=empty, capture_output=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn(b'Cannot open ./blog', result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
