# Project Notes

- Build and test tools are available inside the distrobox named `my-distrobox`.
- Run build/test commands through it, for example:
  `distrobox enter my-distrobox -- make -C tests/unit/tst_CliParser -j2`
- If `qmake6` is missing on the host, do not treat that as a missing project dependency; use `my-distrobox`.
