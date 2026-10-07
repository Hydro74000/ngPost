"""Keep the PowerShell producers and the C++ diagnostic consumer in sync."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WireGuardExitProtocolTests(unittest.TestCase):
    def test_every_exit_code_has_a_diagnostic_and_every_diagnostic_has_a_producer(self):
        installer = (ROOT / 'src/vpn/scripts/win/install-wg-tunnel.ps1').read_text()
        # Ignore quoted diagnostics and comments: "failed (exit ... )" is
        # text, not a PowerShell exit statement. Match inline exits as well.
        tokens = re.compile(
            r"<#.*?#>|\#[^\n]*|'(?:''|[^'])*'|\"(?:`.|[^\"`])*\"|"
            r"\bexit[ \t]+(?P<code>[^\s;}]+)", re.DOTALL)
        exits = [match.group('code') for match in tokens.finditer(installer)
                 if match.group('code') is not None]
        self.assertTrue(exits)
        self.assertTrue(all(code.isdecimal() for code in exits), exits)
        installer_codes = {int(code) for code in exits}

        launcher = (ROOT / 'src/utils/WindowsCommandLine.h').read_text()
        launcher = launcher.split('inline QString elevatedPowerShellCommand(', 1)[1]
        launcher_codes = {int(code) for code in re.findall(r'\bexit\s+(\d+)', launcher)}
        self.assertEqual(launcher_codes, {1, 1223})

        manager = (ROOT / 'src/vpn/VpnManager.cpp').read_text()
        handler = manager.split('bool VpnManager::_reportWindowsWireGuardInstallResult(int code)', 1)[1]
        handler = handler.split('\n#endif', 1)[0]
        handled = {int(code) for code in re.findall(r'\bcase\s+(\d+)\s*:', handler)}
        # Code 1 is the launcher's generic failure, deliberately using default.
        self.assertEqual(installer_codes | launcher_codes, handled | {1})
        self.assertIn('default:', handler)
        self.assertIn('WireGuard tunnel install failed (exit %1)', handler)
