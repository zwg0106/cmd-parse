#ifndef CMD_PARSER_H
#define CMD_PARSER_H

#define CMD_PARSER_TAB_AUTO_COMPLETE   0  // auto_complete callback 
#define CMD_PARSER_TAB_SPACES          1  // number of spaces for a TAB 

#define CMD_PARSER_FUNC_KEY_1       0
#define CMD_PARSER_FUNC_KEY_2       1
#define CMD_PARSER_FUNC_KEY_3       2
#define CMD_PARSER_FUNC_KEY_4       3
#define CMD_PARSER_FUNC_KEY_5       4
#define CMD_PARSER_FUNC_KEY_6       5
#define CMD_PARSER_FUNC_KEY_7       6
#define CMD_PARSER_FUNC_KEY_8       7
#define CMD_PARSER_FUNC_KEY_9       8
#define CMD_PARSER_FUNC_KEY_10      9
#define CMD_PARSER_FUNC_KEY_11      10
#define CMD_PARSER_FUNC_KEY_12      11


#define CMD_PARSER_CTRL_MSG         0x80

typedef struct {
    void *ctx;      // user data
} cmdParser_t;


typedef struct {
    // command
    unsigned int        lineLen;                // length of command

    // IO
    int                 nonBlocking;            // blocking mode or not
    int                 fdIn;                   // input file description
    int                 fdOut;                  // output file description

    // history
    unsigned int        historyLen;             // history cmd size
    int                 autoOrSpace;            // auto completion or space

    union
    {
        void            (*autoComplete) (void                  *ctx,                // user context
                                          const unsigned char   *cmd,               // current cmd
                                          unsigned int          *cursor,            // cursor postion
                                          unsigned char         **completedCmd);    // callback
        unsigned int    spaces;                 // number of spaces for a TAB
    } tab;

    char                historyShortCut;        // charactor to call an history entry

    void                *ctx;                   // user information
} cmdParserParam_t;


typedef const unsigned char * (*cmdParserFnKey_t)
                               (
                                cmdParser_t             *pInst,
                                unsigned int           	fn,
                                const unsigned char   	*cmd,
                                unsigned int          	*cursor
                               );

extern unsigned char *cmdParserInteract(cmdParser_t *pInst);

extern void cmdParserHistoryList(cmdParser_t *pInst, void (* list)(unsigned char *item, unsigned int index));

extern unsigned char *cmParserHistoryGet(cmdParser_t *pInst, unsigned int idx);

extern cmdParser_t *cmdParserNew(cmdParserParam_t *param);

extern void cmdParserDelete(cmdParser_t *pInst);

extern int cmdParserSetDebugLevel(cmdParser_t * pInst, int dbg);

extern int cmdParserSetEcho(cmdParser_t *pInst, int echo);

extern int cmdParserSetHistory(cmdParser_t *pInst, int history);

extern cmdParserFnKey_t cmdParserFunctionKey(cmdParser_t *pInst, cmdParserFnKey_t functionKey);

#endif

