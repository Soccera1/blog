"""Lexical regression tests for the C build-time minifier."""
from pathlib import Path
import shutil
import subprocess
import sys
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else '.build/minify').resolve()
NODE = shutil.which('node')


class MinifierTests(unittest.TestCase):
    def minify(self, mode, source):
        result = subprocess.run([str(BINARY), '--' + mode], input=source.encode(), capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        return result.stdout.decode()

    def test_javascript_comments_strings_and_operators(self):
        source = '''
            // remove this comment
            const text = "https://example.com/a/*literal*/";
            const svg = '<path d="m5 12 4 4L19 6"/>';
            let a = 2;
            const result = a + +a - -a;
            /* remove this too */
            console.log(text, svg, result);
        '''
        output = self.minify('js', source)
        self.assertLess(len(output), len(source))
        self.assertNotIn('remove this', output)
        self.assertIn('"https://example.com/a/*literal*/"', output)
        self.assertIn("'<path d=\"m5 12 4 4L19 6\"/>'", output)
        self.assertIn('+ +', output)
        self.assertIn('- -', output)

    @unittest.skipUnless(NODE, 'Optional JavaScript semantic checks need node')
    def test_javascript_behavior_is_preserved(self):
        fixtures = [
            'function f(){return\n{answer:42};} console.log(f());',
            'function f(){return/* comment\n */42;} console.log(f());',
            'function f(){return/* \u2028 */42;} console.log(f());',
            'let n=1; n\n++n; console.log(n);',
            'let async=0; async\nfunction f(){} console.log(f.constructor.name);',
            'const text="a\f b";console.log(JSON.stringify(text));',
            'const text="escaped\\\" and \\\\ slash"; console.log(text);',
            'const text="first\\\nsecond";console.log(text);',
            'const n = 1 .toString(); console.log(n);',
            'const n = 12 / 3 / 2; console.log(n);',
            'const r = /https?:\\/\\/[^/]+/g; console.log(r.test("https://example.com"));',
            'const t = `a ${`nested ${2 + 3}`} // literal`; console.log(t);',
        ]
        for source in fixtures:
            with self.subTest(source=source):
                original = subprocess.run([NODE, '-e', source], capture_output=True)
                self.assertEqual(original.returncode, 0, original.stderr.decode())
                result = subprocess.run([NODE, '-e', self.minify('js', source)], capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual(result.stdout, original.stdout)

    def test_css_selectors_values_and_literals(self):
        source = '''
            /* remove this comment */
            a :hover, .a/**/.b {
                width: calc(100% - 2px);
                content: "a  b /* literal */";
                background: url(data:image/svg+xml;utf8,/*literal*/);
                --empty: ;
            }
        '''
        output = self.minify('css', source)
        self.assertLess(len(output), len(source))
        self.assertNotIn('remove this', output)
        self.assertIn('a :hover,.a.b{', output)
        self.assertIn('calc(100% - 2px)', output)
        self.assertIn('"a  b /* literal */"', output)
        self.assertIn('url(data:image/svg+xml;utf8,/*literal*/)', output)
        self.assertIn('--empty: ;', output)

    def test_css_comment_token_boundaries(self):
        output = self.minify('css', 'a/**/b { x: 1/**/px; y: var/**/(--name); }')
        self.assertIn('a/**/b{', output)
        self.assertIn('1/**/px', output)
        self.assertIn('var/**/(', output)

    def test_css_escapes_and_descendant_spacing(self):
        source = '.\\31  a { color: red; } .escaped\\: :hover { color: blue; }'
        output = self.minify('css', source)
        self.assertIn('.\\31  a{', output)
        self.assertIn('.escaped\\: :hover{', output)

    def test_empty_input_and_errors(self):
        for mode in ('js', 'css'):
            self.assertEqual(self.minify(mode, ''), '')
            self.assertEqual(self.minify(mode, '   \n /* comment */ \n'), '')
            for source in ('/* unclosed', 'const x = "unclosed', '\x00'):
                result = subprocess.run([str(BINARY), '--' + mode], input=source.encode(), capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)
                self.assertEqual(result.stdout, b'')
        result = subprocess.run([str(BINARY), '--css'], input=b'a{background:url(abc}', capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(subprocess.run([str(BINARY)], capture_output=True).returncode, 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
