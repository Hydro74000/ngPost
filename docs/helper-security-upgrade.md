# Linux helper security revision 3

The wire protocol remains v2 so existing session manifests can be cleaned up.
The independent security revision is now 3. A protocol-v2 marker alone is not
sufficient to enable Connect, startup cleanup or passwordless VPN operation.

After updating ngPost, open VPN settings and install the security update. The
Install button remains available for an obsolete helper; Uninstall can also
remove it. The installer requires administrator authentication, atomically
replaces the old executable with a non-executable empty file, and removes its
Polkit rule before validating or copying new resources. Failed migrations leave
the old helper disabled. Successful migration installs the current helper before
restoring the passwordless rule.

An unprivileged application cannot revoke an already installed root-owned
Polkit rule. Canceling authentication leaves the old installation vulnerable to
direct invocation, even though the new application refuses to connect. Complete
the privileged migration or uninstall the helper; merely replacing the AppImage
does not remove the old authorization. No host migration is performed by the
source build or the tests. The migration fixture runs only in an explicitly
enabled disposable root container and checks both failed and successful upgrades.
