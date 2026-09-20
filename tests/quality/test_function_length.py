"""Exercise the gate with real lizard input, including its negative controls."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / ".github/scripts/check-function-length.py"
spec = importlib.util.spec_from_file_location("function_length", SCRIPT)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class FunctionLengthGateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "src").mkdir()
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.baseline = self.root / "baseline.txt"

    def source(self, text, name="example.cpp"):
        path = self.root / "src" / name
        path.write_text(text, encoding="utf-8")
        subprocess.run(["git", "-C", str(self.root), "add", str(path)], check=True)
        return path

    @staticmethod
    def function(length, name="work", parameter=""):
        return f"void {name}({parameter})\n{{\n" + "    // counted too\n" * (length - 3) + "}\n"

    def test_new_function_boundary_and_comments(self):
        self.source(self.function(100))
        self.assertFalse(gate.violations(gate.measure(self.root), {}))
        self.source(self.function(101))
        self.assertIn("new function limit 100", gate.violations(gate.measure(self.root), {})[0])

    def test_existing_function_cannot_grow_even_below_100(self):
        self.source(self.function(12))
        baseline = gate.measure(self.root)
        self.source(self.function(13))
        self.assertIn("baseline 12", gate.violations(gate.measure(self.root), baseline)[0])
        self.source(self.function(11))
        self.assertFalse(gate.violations(gate.measure(self.root), baseline))

    def test_absolute_limit_cannot_be_waived(self):
        self.source(self.function(201))
        current = gate.measure(self.root)
        self.assertIn("absolute limit 200", gate.violations(current, current)[0])

    def test_overloads_and_same_names_in_different_files(self):
        self.source(self.function(10, parameter="int") + self.function(11, parameter="double"))
        self.source(self.function(12, parameter="int"), "other.h")
        self.assertEqual(sorted(gate.measure(self.root).values()), [10, 11, 12])

    def test_platform_alternatives_use_longest_body(self):
        self.source("#ifdef Q_OS_WIN\n" + self.function(101) + "#else\n"
                    + self.function(10) + "#endif\n")
        self.assertEqual(list(gate.measure(self.root).values()), [101])

    def test_blank_lines_count_and_moving_function_does_not_rename_it(self):
        self.source(self.function(10).replace("    // counted too\n", "\n"))
        baseline = gate.measure(self.root)
        self.assertEqual(list(baseline.values()), [10])
        self.source("\n" * 50 + self.function(10))
        self.assertEqual(gate.measure(self.root), baseline)

    def test_removed_functions_are_allowed_and_generated_files_ignored(self):
        self.source(self.function(10) + self.function(11, "removed"))
        baseline = gate.measure(self.root)
        self.source(self.function(10))
        (self.root / "src/moc_generated.cpp").write_text(self.function(300))
        self.assertFalse(gate.violations(gate.measure(self.root), baseline))

    def test_missing_sources_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "no production"):
            gate.measure(self.root)

    def test_baseline_roundtrip_and_invalid_entries(self):
        self.source(self.function(10))
        lengths = gate.measure(self.root)
        gate.write_baseline(self.baseline, lengths)
        self.assertEqual(gate.read_baseline(self.baseline), lengths)
        valid = self.baseline.read_text()
        for invalid in ["", "201\tsrc/a.cpp\tvoid f()\n", "oops\n", valid + valid]:
            with self.subTest(invalid=invalid):
                self.baseline.write_text(invalid)
                with self.assertRaises(ValueError):
                    gate.read_baseline(self.baseline)

    def test_command_exit_status_and_refusal_to_baseline_over_200(self):
        self.source(self.function(10))
        command = [sys.executable, str(SCRIPT), "--root", str(self.root),
                   "--baseline", str(self.baseline)]
        result = subprocess.run(command + ["--write-baseline"], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run(command, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.source(self.function(10) + self.function(101, "added"))
        result = subprocess.run(command, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"new function limit 100", result.stderr)
        original_baseline = self.baseline.read_bytes()
        self.source(self.function(201))
        result = subprocess.run(command + ["--write-baseline"], capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.baseline.read_bytes(), original_baseline)


if __name__ == "__main__":
    unittest.main()
