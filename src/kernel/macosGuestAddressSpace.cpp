#if defined(__APPLE__) && defined(__x86_64__)

// Make the process own the guest ranges before any runtime initialization.
// The linker would otherwise discard these unreferenced zero-fill segments.
asm(".globl __kyty_system_managed\n"
    ".no_dead_strip __kyty_system_managed\n"
    ".zerofill SYSTEM_MANAGED,SYSTEM_MANAGED,__kyty_system_managed,0x7fffbc000");
asm(".globl __kyty_system_reserved\n"
    ".no_dead_strip __kyty_system_reserved\n"
    ".zerofill SYSTEM_RESERVED,SYSTEM_RESERVED,__kyty_system_reserved,0x7c0004000");
asm(".globl __kyty_user_area\n"
    ".no_dead_strip __kyty_user_area\n"
    ".zerofill USER_AREA,USER_AREA,__kyty_user_area,0x8c00000000");

#endif
