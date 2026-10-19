#ifndef COLORS_H
#define COLORS_H

#define RST  "\x1b[0m" // reset

// colors
#define RED  "\x1b[31m"
#define GRN  "\x1b[32m"
#define YLW  "\x1b[33m"
#define BLUE "\x1b[94m"
#define GRY  "\x1b[38;5;247m"

// printing statuses
#define ERR   RED  "[-] " RST          // error
#define WRN   YLW  "[!] " RST          // warning
#define INFO  BLUE "[INFO] " RST       // info
#define V1    GRY "[V1] " RST          // verbose level 1
#define V2    GRY "[V2] " RST
#define V3    GRY "[V3] " RST
#define D1    GRY "[D1] " RST          // debug output

#endif // COLORS_H

