# The kernel and the user application are both in this one component and one
# binary.  They are separated by privilege, not by image: user_main.c is
# compiled into IRAM (the region ESP-IDF's PMP grants U-mode execute on) and is
# only ever reached through the mret in umode.S.
#
# All assembly lives in .S files -- umode.S is the M-mode side of the window,
# user_syscall.S the U-mode side.
#
# probe/p4_umode_mret.c is the original standalone experiment that established
# mret-to-U-mode works on this silicon.  It has its own app_main and is kept
# out of the build; see README.md.

idf_component_register(
    SRCS
        "kernel_main.c"
        "umode.S"
        "user_main.c"
        "user_syscall.S"
    INCLUDE_DIRS
        "."
)
