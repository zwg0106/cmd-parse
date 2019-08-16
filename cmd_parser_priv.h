#ifndef CMD_PARSER_PRIV_H
#define CMD_PARSER_PRIV_H

#include <stddef.h>
#include "cmd_parser.h"

// instanse of cmd
typedef struct {
    cmdParserParam_t    user;               // user parameters
    int                 dbg;                // debug level
    unsigned char       *cmd;               // command
    unsigned char       *savedCmd;          // saved command
    int                 echoOn;             // echo activated or not
    struct termios      origTermSettings;   // Saved terminal settings
    int                 inFlag;             // flag of input descriptor

    unsigned char       storedUngetChar;
    int                 state;              // state of FSM
    int                 prevState;          // previous of FSM
    int                 cursor;             // cursor position
    unsigned int        lineSz;             // number of chars

    int                 historyOn;          // history activated or not
    unsigned char       *history;           // history infomation
    int                 historyCur;         // currect display index
    unsigned int        historySz;          // number of history index
    unsigned int        historyInsert;      // insertion index

    cmdParserFnKey_t    functionKey;        // callback
} cmdParserInstance_t;


#define CMD_PARSER_USER_TO_INSTANCE(p)   ((cmdParserInstance_t *)   \
                                        ((p) ? (((char *)p - offsetof(cmdParserParam_t, ctx)) - \
                                        offsetof(cmdParserInstance_t, user)) : NULL))


#define CMD_PARSER_ERR(pInstance, format, ...)                                  \
    do  {                                                                       \
        if (!pInstance || ((cmdParserInstance_t *)pInstance)->dbg)              \
        fprintf(stderr, "CMD_PARSER (%s/%s#%d) : "format,                       \
                basename(__FILE__), __FUNCTION__, __LINE__, ## __VA_ARGS__);    \
        } while(0)

#endif
