/* kernel/coreutils/table.c -- the utilities kush knows about */
#include "cu.h"
#include <kernel/string.h>

const struct cu_cmd cu_commands[] = {
    { "pwd",      "",                    "print the working directory",                pwd_main },
    { "cd",       "[DIR|-]",             "change directory (default /root)",           cd_main },
    { "ls",       "[-al1] [PATH...]",    "list directory contents",                    ls_main },
    { "mkdir",    "[-p] [-m MODE] DIR...", "create directories",                       mkdir_main },
    { "rmdir",    "DIR...",              "remove empty directories",                   rmdir_main },
    { "touch",    "[-c] FILE...",        "create files / update modification time",    touch_main },
    { "rm",       "[-rf] FILE...",       "remove files (and directories with -r)",     rm_main },
    { "cp",       "[-r] SRC... DST",     "copy files and directories",                 cp_main },
    { "mv",       "SRC... DST",          "move or rename",                             mv_main },
    { "cat",      "[-n] [FILE...]",      "print files",                                cat_main },
    { "head",     "[-n N] [FILE...]",    "first lines (default 10)",                   head_main },
    { "tail",     "[-n N] [FILE...]",    "last lines (default 10)",                    tail_main },
    { "grep",     "[-invcq] PAT [FILE...]", "print lines containing PAT",              grep_main },
    { "echo",     "[-neE] [TEXT...]",    "print text (\\n \\t \\e ... understood)",    echo_main },
    { "chmod",    "MODE FILE...",        "change permissions (755 or u+x,go-w)",       chmod_main },
    { "chown",    "USER[:GROUP] FILE...", "change owner and group",                    chown_main },
    { "date",     "[+FORMAT]",           "current date and time (UTC, from the RTC)",  date_main },
    { "uname",    "[-asnrvm]",           "system name and version",                    uname_main },
    { "neofetch", "",                    "system summary with the Kestrel logo",       neofetch_main },
    { NULL, NULL, NULL, NULL },
};

const struct cu_cmd *cu_find(const char *name)
{
    for (const struct cu_cmd *c = cu_commands; c->name; c++)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}
