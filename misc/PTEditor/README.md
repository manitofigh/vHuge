# PTEditor

Vendored from https://github.com/misc0110/PTEditor at commit
`01ae0fbc459eca349a1d6b2d7be28645886ad58c` (2026-02-06). PTEditor is under
the MIT license in `LICENSE`.

Only the parts vHuge uses are kept:

- `ptedit_header.h`: single-header library included by `src/vhuge.c` and
  `daemon/vhuge_poold.c`. `make header` regenerates it from
  `module/pteditor.h`, `ptedit.h`, and `ptedit.c`.
- `module/`: the `pteditor` kernel module that provides `/dev/pteditor`.

## Changes in `module/`

- `pteditor.c`: PGD and P4D writes are compiled only with
  `PTEDITOR_ENABLE_PGD_UPDATE`. Without it, those requests are rejected with a
  warning, so the module builds on kernels that do not export
  `__pti_set_user_pgtbl`. vHuge only needs page-table resolve and TLB
  invalidation.
- `Makefile`: builds against `KDIR=/lib/modules/$(KERNEL)/build` with
  `$(MAKE)` and `$(CURDIR)`, and drops the `linux-headers` include path.
- `README.md`: added; explains how to enable PGD and P4D writes.
