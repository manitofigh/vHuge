# Linking to a kernel built from source

The default module build supports PTEditor's read-only resolve and TLB-invalidation paths. This is sufficient for vHuge `tlb` mode and builds on cloud kernels that do not export `__pti_set_user_pgtbl`.

PGD and folded P4D write requests are rejected by default. To enable those PTEditor write paths, use a kernel that exports `__pti_set_user_pgtbl` and rebuild with `PTEDITOR_ENABLE_PGD_UPDATE`:

```bash
grep -E 'CONFIG_(PAGE_TABLE_ISOLATION|MITIGATION_PAGE_TABLE_ISOLATION)=' /boot/config-$(uname -r)
grep -w '__pti_set_user_pgtbl' /lib/modules/$(uname -r)/build/Module.symvers || echo "not exported"
make clean
make EXTRA_CFLAGS=-DPTEDITOR_ENABLE_PGD_UPDATE
```

If the symbol is not exported, find its implementation in the kernel source:

```bash
rg -n "__pti_set_user_pgtbl" arch/x86
```

Add the export after the function definition, rebuild the kernel, and boot the rebuilt kernel:

```c
EXPORT_SYMBOL_GPL(__pti_set_user_pgtbl);
```
