"""Behavior tests. All filesystem writes happen in temporary directories."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = os.environ.get('QCSH_BIN', str(ROOT / 'qcsh'))

class ShellTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = pathlib.Path(self.tmp.name)

    def run_shell(self, text):
        return subprocess.run([BIN], input=text, text=True, capture_output=True,
                              cwd=self.path, timeout=8)

    def test_custom_commands_and_quoted_paths(self):
        r = self.run_shell('createFolder "my folder"\nopenFolder "my folder"\n'
                           'createFile "my notes.txt"\npwd\nexit\n')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((self.path/'my folder'/'my notes.txt').exists())
        self.assertIn(str(self.path/'my folder'), r.stdout)

    def test_exclusive_creation(self):
        p = self.path/'notes'; p.write_text('keep me')
        r = self.run_shell('createFile notes\n')
        self.assertEqual(r.returncode, 1)
        self.assertEqual(p.read_text(), 'keep me')
        self.run_shell('createFile --overwrite notes\n')
        self.assertEqual(p.read_text(), '')

    def test_external_and_redirects(self):
        r = self.run_shell('printf "hello\\n" > notes\nprintf "world\\n" >> notes\n'
                          'cat < notes | grep world | wc -l\nopenFile notes\n')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('1\n', r.stdout)
        self.assertIn('hello\nworld\n', r.stdout)

    def test_pipeline_no_hang(self):
        r = self.run_shell('yes | head -n 5\n')
        self.assertEqual(r.stdout, 'y\n'*5)
        self.assertEqual(r.returncode, 0)

    def test_unknown_command_and_status(self):
        r = self.run_shell('qcsh_nonexistent_xyz\nstatus\nprintf alive\n')
        self.assertIn('127\n', r.stdout)
        self.assertIn('alive', r.stdout)
        self.assertIn('qcsh_nonexistent_xyz', r.stderr)

    def test_explain_has_no_side_effects(self):
        r = self.run_shell('explain createFile absent > output\n')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((self.path/'absent').exists())
        self.assertFalse((self.path/'output').exists())
        self.assertIn('built-in', r.stdout)

    def test_invalid_syntax_before_execution(self):
        for line in ['createFile absent |', 'createFile absent >',
                     'createFile "unterminated', 'createFile absent && echo hi']:
            r = self.run_shell(line+'\n')
            self.assertEqual(r.returncode, 2, r.stderr)
            self.assertFalse((self.path/'absent').exists())

    def test_quoted_operator_and_empty_arg(self):
        r = self.run_shell('printf "[%s][%s]" "a | b" ""\n')
        self.assertEqual(r.stdout, '[a | b][]')

    def test_builtin_redirection_restoration(self):
        r = self.run_shell('pwd > location\npwd\n')
        self.assertEqual((self.path/'location').read_text(), str(self.path)+'\n')
        self.assertEqual(r.stdout, str(self.path)+'\n')

    def test_profile(self):
        r = self.run_shell('profile printf hi | wc -c\n')
        self.assertEqual(r.stdout.strip(), '2')
        self.assertIn('stage 1 pid', r.stderr)
        self.assertIn('stage 2 pid', r.stderr)
        self.assertIn('elapsed', r.stderr)

    def test_limits(self):
        r = self.run_shell('printf ' + 'x '*130 + '\n')
        self.assertEqual(r.returncode, 2)

    def test_redirection_order(self):
        r = self.run_shell('printf hi > first > second\n')
        self.assertEqual(r.returncode, 0)
        self.assertEqual((self.path/'first').read_text(), '')
        self.assertEqual((self.path/'second').read_text(), 'hi')

if __name__ == '__main__':
    unittest.main(verbosity=2)
