#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <libgen.h>

#include "cmd_parser.h"
#include "cmd_parser_priv.h"


// control cursor moving
#define     CMD_PARSER_MOVE_SET             0
#define     CMD_PARSER_MOVE_CUR             1
#define     CMD_PARSER_MOVE_END             2


// states of state machine
#define     CMD_PARSER_STATE_0              0
#define     CMD_PARSER_STATE_1              1
#define     CMD_PARSER_STATE_2              2
#define     CMD_PARSER_STATE_3              3
#define     CMD_PARSER_STATE_4              4
#define     CMD_PARSER_STATE_5              5
#define     CMD_PARSER_STATE_6              6
#define     CMD_PARSER_STATE_7              7
#define     CMD_PARSER_STATE_8              8

// FSM go to previous state
#define     CMD_PARSER_PREVIOUS_STATE       50

// FSM stay in current state
#define     CMD_PARSER_CURRENT_STATE        51

// state waiting for input data
#define     CMD_PARSER_STATE_AGAIN          0x80

// cmd is blank or not
#define     CMD_IS_BLANK(c)                 (' ' == (c) || '\t' == (c))

// check if it's in ASCII range
#define     CMD_IN_ASCII_RANGE(x)           ((unsigned char)((x) <= 127 ? ((x) - 0x40) : (x)))


// write out data
static int cmdParserWrite(cmdParserInstance_t *pCtx, const void *buf, size_t len)
{
    int rc;
    int l = 0;
    int errSave;

    assert(NULL != pCtx);

    do
    {
        rc = write(pCtx->user.fdOut, ((const unsigned char *)buf) + l, len -l);
        if(rc > 0)
        {
            assert(((unsigned)l + rc) <= len);
            l += rc;
        }
    
    } while(((unsigned)l != len) && ((rc >=0) || (EINTR == errno)));

    if(rc < 0)
    {
        errSave = errno;
        CMD_PARSER_ERR(pCtx, "Error '%s' (%d) on write()\n", strerror(errno), errno);
        errno = errSave;
        l = -1;
    }

    return l;
}

// read input data
static int cmdParserRead(cmdParserInstance_t *pCtx, unsigned char *buf, unsigned int len)
{
    int rc;
    int errSave;

    assert(NULL != pCtx);

    do
    {
        rc = read(pCtx->user.fdIn, buf, len);
        if(-1 == rc)
        {
            if(EINTR == errno)
            {
                continue;
            }
            if(EAGAIN != errno)
            {
                errSave = errno;
                CMD_PARSER_ERR(pCtx, "Error '%s' (%d) on read(%d)\n", strerror(errno), errno, pCtx->user.fdIn);
                errno = errSave;
            }
            else
            {
                assert(pCtx->user.nonBlocking);            
            }

            return -1;
        }
    } while(rc < 0);

    //printf("\nRead %d chars, <0x%x, %c>\n", rc, *buf, *buf);

    return rc;
}

// move curosr
static int cmdParserMoveCursor(cmdParserInstance_t *pCtx, int offset, int where)
{
    int rc;
    unsigned char c[2];

    // calculate offset from current cursor position
    switch(where)
    {
        case CMD_PARSER_MOVE_SET:
        {
            if(offset < 0)
            {
                offset = 0;
            }
            offset = -(pCtx->cursor - offset);
        }
        break;

        case CMD_PARSER_MOVE_CUR:
        {
            // nothing to do
        }
        break;

        case CMD_PARSER_MOVE_END:
        {
            assert(pCtx->lineSz >= (unsigned)(pCtx->cursor));
            offset = pCtx->lineSz - offset - pCtx->cursor;
        }
        break;

        default:
        {
            assert(0);
        }
    }

    // forward
    if(offset > 0)
    {
        if((unsigned)(pCtx->cursor + offset) >= pCtx->lineSz)
        {
            assert((unsigned)(pCtx->cursor) <= pCtx->lineSz);
            offset = pCtx->lineSz - pCtx->cursor;
        }

        printf("\nline_sz = %u, curosr = %d, offset = %d\n", pCtx->lineSz, pCtx->cursor, offset);

        if(pCtx->echoOn)
        {
            unsigned int l = (unsigned)offset;
            unsigned int l1;
            unsigned int i;

            i = pCtx->cursor;
            while(l)
            {
                c[0] = pCtx->cmd[i];

        		// If accented character (cf. man iso_8859-1)
        		if (c[0] > 0x7f)
        		{
          			if (c[0] >= 0xc0)
          			{
            			c[1] = c[0] - 0x40;
            			c[0] = 0xc3;
          			}
          			else
          			{
            			c[1] = c[0];
            			c[0] = 0xc2;
          			}
          			l1 = 2;
        		}
        		else
        		{
          			l1 = 1;
        		}
        		rc = cmdParserWrite(pCtx, c, l1);
        		if (l1 != (unsigned)rc)
        		{
          			return -1;
        		}

        		l--;
        		i++;
            }
        }

		pCtx->cursor += offset;

		return 0;
    }

	// backward
  	if(offset < 0)
  	{
   		// If we try to go below the beginning of line, adjust to begining of line
    	if((pCtx->cursor + offset) < 0)
    	{
      		offset += (pCtx->cursor + offset);
      		pCtx->cursor = 0;
    	}
    	else
    	{
      		pCtx->cursor += offset;
    	}

    	if(pCtx->echoOn)
    	{
      		c[0] = '\b';

      		while(offset)
      		{
        		rc = cmdParserWrite(pCtx, c, 1);
        		if(1 != rc)
        		{
          			return -1;
        		}

        		// We increment offset as it is negative
        		offset ++;
      		} 
    	} 

    	return 0;
  	}

	// no need to move cursor
	return 0;
}


// remove characters from currect position to end of line
static int cmdParserTruncate(cmdParserInstance_t *pCtx)
{
	unsigned int i, l;
	int          rc;

  	if((unsigned)(pCtx->cursor) < pCtx->lineSz)
  	{
    	// Erase the end of the line
   		for(i = pCtx->cursor; i < pCtx->lineSz; i++)
    	{
      		pCtx->cmd[i] = ' ';
    	}

    	l = pCtx->lineSz - pCtx->cursor;

    	// Echo the blanks
    	if(pCtx->echoOn)
    	{
      		rc = cmdParserWrite(pCtx, &(pCtx->cmd[pCtx->cursor]), l);
      		if(rc < 0)
      		{
        		return -1;
      		}
    	}

    	pCtx->cursor += l;
    	cmdParserMoveCursor(pCtx, -l, CMD_PARSER_MOVE_CUR);

    	// Update the size of the line
    	pCtx->lineSz -= l;
    	pCtx->cmd[pCtx->lineSz] = '\0';
  	}

  	return 0;
}


// shift the part of the cmd located the right side of the cursor
static int cmdParserShiftLine(cmdParserInstance_t *pCtx, int direction)
{
	unsigned int  i, j, l, l1;
	int           val;
	int           rc;
	unsigned char c[2];

  	// Shift right
  	if(direction > 0)
  	{
    	assert((unsigned)(pCtx->lineSz) < (pCtx->user.lineLen - 1));

    	for(i = pCtx->lineSz; i > (unsigned)(pCtx->cursor); i --)
    	{
      		pCtx->cmd[i] = pCtx->cmd[i - 1];
    	}

    	pCtx->cmd[i] = ' ';

    	pCtx->lineSz++;
    	pCtx->cmd[pCtx->lineSz] = '\0';

    	// Echo
    	if(pCtx->echoOn)
    	{
      		l = pCtx->lineSz - pCtx->cursor;
      		j = i;
     		while(l)
      		{
        		c[0] = pCtx->cmd[j];

        		// If accented character (cf. man iso_8859-1)
        		if(c[0] > 0x7f)
        		{
          			if(c[0] >= 0xc0)
          			{
            			c[1] = c[0] - 0x40;
            			c[0] = 0xc3;
          			}
          			else
          			{
            			c[1] = c[0];
            			c[0] = 0xc2;
          			}
          			l1 = 2;
        		}
        		else
        		{
          			l1 = 1;
        		}
        		rc = cmdParserWrite(pCtx, c, l1);
        		if(l1 != (unsigned)rc)
        		{
          			return -1;
        		}

        		l--;
        		j++;
      		}
    	}

    	// Move back the cursor to its current position
    	val = pCtx->cursor;
    	pCtx->cursor = pCtx->lineSz;
    	cmdParserMoveCursor(pCtx, -(pCtx->lineSz - val), CMD_PARSER_MOVE_CUR);
  	}
  	else
  	{
    	// Shift left
    	if((direction < 0) && ((unsigned)(pCtx->cursor) < pCtx->lineSz))
    	{
      		for(i = pCtx->cursor; i < (pCtx->lineSz - 1); i ++)
      		{
        		pCtx->cmd[i] = pCtx->cmd[i + 1];
      		}

      		// Put a white space on the last char of the line to erase it
      		// at display time
      		pCtx->cmd[i] = ' ';

      		// Echo
      		if(pCtx->echoOn)
      		{
        		l = pCtx->lineSz - pCtx->cursor;
        		j = pCtx->cursor;
        		while(l)
        		{
          			c[0] = pCtx->cmd[j];

          			// If accented character (cf. man iso_8859-1)
          			if(c[0] > 0x7f)
          			{
            			if(c[0] >= 0xc0)
            			{
              				c[1] = c[0] - 0x40;
              				c[0] = 0xc3;
            			}
            			else
            			{
              				c[1] = c[0];
              				c[0] = 0xc2;
            			}
            			l1 = 2;
          			}
          			else
          			{
            			l1 = 1;
          			}

          			rc = cmdParserWrite(pCtx, c, l1);
          			if(l1 != (unsigned)rc)
          			{
            			return -1;
          			}

          			l--;
          			j++;
        		}
      		}

			// Move back the cursor to its current position
      		val = pCtx->cursor;
      		pCtx->cursor = pCtx->lineSz;
      		cmdParserMoveCursor(pCtx, -(pCtx->lineSz - val), CMD_PARSER_MOVE_CUR);

      		pCtx->lineSz--;
      		pCtx->cmd[pCtx->lineSz] = '\0';
    	}
  	}

 	 return 0;
}


// get or unget one character
static int cmdParserGetUngetChar(cmdParserInstance_t *pCtx, unsigned char *c)
{
	int rc;

  	if(!c)
  	{
    	errno = EINVAL;
    	return -1;
  	}

  	if (*c)
  	{
    	pCtx->storedUngetChar = *c;
    	return 0;
  	}

  	if(pCtx->storedUngetChar)
  	{
    	*c = pCtx->storedUngetChar;
    	pCtx->storedUngetChar = 0;
    	return 0;
  	}

 	rc = cmdParserRead(pCtx, c, 1);
	if (1 == rc)
  	{
    	return 0;
  	}

  	if(0 == rc)
  	{
    	CMD_PARSER_ERR(pCtx, "INPUT closed\n");
  	}
  	else
  	{
    	assert(rc < 0);

    	if(EAGAIN != errno)
    	{
      		CMD_PARSER_ERR(pCtx, "Error '%s' (%d) on read()\n", strerror(errno), errno);
    	}
  	}

  	return -1;
}


//Get a character
static int cmdParserGetChar(cmdParserInstance_t *pCtx, unsigned char *c)
{
	*c = 0;
	return cmdParserGetUngetChar(pCtx, c);	
}


//Put a character back into the input stream
static int cmdParserUngetChar(cmdParserInstance_t *pCtx, unsigned char *c)
{
	return cmdParserGetUngetChar(pCtx, c);	
}


//
static int cmdParserBeep(cmdParserInstance_t *pCtx)
{
	unsigned char c = 0x7;
	int           rc;

  	rc = cmdParserWrite(pCtx, &c, 1);
  	if(rc < 0)
  	{
    	return -1;
  	}

  	return 0;
}


//check if cmd is empty or not
static int cmdParserIsEmpty(unsigned char *cmd)
{
	unsigned char *p;

  	assert(cmd);

  	p = cmd;
  	while(*p && *p != '\n')
  	{
    	if(!CMD_IS_BLANK(*p))
    	{
     		return 0;
    	}

    	p++;
  	}

 	return 1;
}

// history cmds is a table in a circular way
// need to consider two cases
// case1: history table is not full(sz < len)
//            +-------------------------------------------+
//            | X | X | X | X | X | X | X |   |   |   |   |
//            +-------------------------------------------+
//                    ^                   ^
//                    |                   |
//                   cur                insert
//
//     		oldest record = histo[0]
//     		newest record = histo[insert - 1]
//        	0 <= cur < insert
//
//
// case2: The history table is full (sz = len)
//            +-------------------------------------------+
//            | X | X | X | X | X | X | X | X | X | X | X |
//            +-------------------------------------------+
//                    ^                   ^
//                    |                   |
//                   cur                insert
//
//			oldest record = histo[insert]
//			newest record = histo[insert - 1]
//			-(len - insert) <= cur < insert
//
//
//  Actually, the previous two cases can be generalized as followed:
//
//     -(sz - insert) <= cur < insert
//
//
//     In case 1, sz = insert, so:
//
//        -(sz - insert) <= cur < insert
//  <==>  -(insert - insert) <= cur < insert
//  <==>  0 <= cur < insert
//
//
//     In case 2, sz = len:
//
//        -(sz - insert) <= cur < insert
//  <==>  -(len - insert) <= cur < insert
//
//
// UP operation (assuming sz > 0):
//       if (cur > -(sz - insert))
//         cur = cur - 1
//       if (cur < 0)
//         display histo[sz + cur]
//       else
//         display histo[cur]
//
// DOWN operation (assuming sz > 0):
//       if (cur < (insert - 1))
//         cur = cur + 1
//       if (cur < 0)
//         display histo[sz + cur]
//       else
//         display histo[cur]
//
//
// Insert operation:
//
//  histo[insert] = new record
//  insert = (insert + 1) % len


//go upward in history cmds
static int cmdParserHistoryUp(cmdParserInstance_t *pCtx, const unsigned char **cmd)
{
    int i;

	*cmd = NULL;

  	if(!(pCtx->historySz))
  	{
    	return 2;
  	}

  	if(pCtx->historyCur > (-((signed)(pCtx->historySz) - (signed)(pCtx->historyInsert))))
  	{
    	pCtx->historyCur = pCtx->historyCur - 1;
  	}
  	else
  	{
    	return 1;
  	}

  	if(pCtx->historyCur < 0)
  	{
    	i = pCtx->historySz + pCtx->historyCur;
  	}
  	else
  	{
    	i = pCtx->historyCur;
  	}

  	// Point on the UP entry in the history
  	*cmd = pCtx->history + (i * pCtx->user.lineLen);

	return 0;
}

//go downward in history cmds
static int cmdParserHistoryDown(cmdParserInstance_t *pCtx, const unsigned char **cmd)
{
	int   i;

  	*cmd = NULL;

  	if(!(pCtx->historySz))
  	{
    	return 2;
  	}

  	if(pCtx->historyCur < ((signed)(pCtx->historyInsert) - 1))
  	{
    	pCtx->historyCur = pCtx->historyCur + 1;
  	}
  	else
  	{
    	pCtx->historyCur = pCtx->historyInsert;
    	return 1;
  	}


  	if(pCtx->historyCur < 0)
  	{
    	i = pCtx->historySz + pCtx->historyCur;
  	}
  	else
  	{
    	i = pCtx->historyCur;
  	}

  	// Point on the DOWN entry in the history
  	*cmd = pCtx->history + (i * pCtx->user.lineLen);

  	return 0;
}

//get the oldest record cmd
static const unsigned char *cmdParserHistoryOldest(cmdParserInstance_t *pCtx)
{
	int   oldest;

  	if(0 == pCtx->historySz)
  	{
    	assert(0 == pCtx->historyInsert);
    	return NULL;
  	}

  	// Index of the oldest record
  	oldest = -((signed)(pCtx->historySz) - pCtx->historyInsert);

  	// Update the current index with the relative value
  	pCtx->historyCur = oldest;

  	if(oldest < 0)
  	{
    	oldest += pCtx->historySz;
  	}

  	// Command in the oldest record
  	return (const unsigned char *)(pCtx->history + (oldest * pCtx->user.lineLen));	
}


//get the newest cmd
static const unsigned char *cmdParserHistoryNewest(cmdParserInstance_t *pCtx)
{
	int   newest;

  	if(0 == pCtx->historySz)
  	{
    	assert(0 == pCtx->historyInsert);
    	return NULL;
  	}

  	// Index of the newest record = index of the record just before
  	// the insertion point
  	newest = pCtx->historyInsert - 1;

  	// Update the current index with the relative value
  	pCtx->historyCur = newest;

  	if(newest < 0)
  	{
    	newest += pCtx->historySz;
  	}

  	// Command line in the newest record
  	return (const unsigned char *)(pCtx->history + (newest * pCtx->user.lineLen));
}

//add cmd into history table
static void cmdParserHistoryAdd(cmdParserInstance_t *pCtx)
{
	unsigned char *p;

  	// We don't add the command line if the history is not activated
  	if(!(pCtx->historyOn))
  	{
    	return;
  	}

  	// We don't add the command line in the history if it is empty
  	if(cmdParserIsEmpty(pCtx->cmd))
  	{
    	return;
  	}

  	// We don't add the command line in the history if it is the same as
  	// the newest one
  	if(pCtx->historySz && !strcmp((const char *)cmdParserHistoryNewest(pCtx), (const char *)(pCtx->cmd)))
  	{
    	return;
  	}

  	// Point on the insertion slot
  	p = pCtx->history + pCtx->user.lineLen * pCtx->historyInsert;

  	// Copy the command in the history
  	strncpy((char *)p, (char *)(pCtx->cmd), pCtx->user.lineLen);
  	p[pCtx->user.lineLen - 1] = '\0';

  	// Increment the insertion index
  	pCtx->historyInsert = (pCtx->historyInsert + 1) % pCtx->user.historyLen;

  	// Increment the number of recorded lines
  	if(pCtx->historySz < pCtx->user.historyLen)
  	{
    	pCtx->historySz++;
  	}
  	else
  	{
    	assert(pCtx->historySz == pCtx->user.historyLen);
  	}
}


// reset cmd hsitory table
static void cmdParserHistoryReset(cmdParserInstance_t *pCtx)
{
	// Reset the current index to the insertion index
	pCtx->historyCur = pCtx->historyInsert;
}


// list history cmds
void cmdParserHistoryList(cmdParser_t *pInst, void (* list)(unsigned char *item, unsigned int index))
{
	cmdParserInstance_t  *pCtx = CMD_PARSER_USER_TO_INSTANCE(pInst);
	const unsigned char *p;
	unsigned int         idx, i;
	int                  rc;

  	errno = 0;

  	if(!pCtx || !list)
  	{
    	errno = EINVAL;
    	return;
  	}

  	// If the history is empty
  	if(0 == pCtx->historySz)
  	{
    	list(NULL, (unsigned)-1);
    	return;
  	}

  	// Point on the oldest item
  	p = cmdParserHistoryOldest(pCtx);
  	i = 0;
  	while(i < pCtx->historySz)
  	{
    	idx = (p - pCtx->history) / pCtx->user.lineLen;
    	if(p)
   		{
      		strcpy((char *)(pCtx->cmd), (const char *)p);
    	}
    	list(pCtx->cmd, idx);
    	rc = cmdParserHistoryDown(pCtx, &p);
		if(0 != rc)
		{
			// do nothing
		}
    	i++;
  	}

  	list(NULL, -1);
}

//get history cmd
unsigned char *cmdParserHistoryGet(cmdParser_t *pInst, unsigned int idx)
{
    cmdParserInstance_t  *pCtx = CMD_PARSER_USER_TO_INSTANCE(pInst);
	unsigned char      *p;

  	if(!pCtx)
  	{
    	errno = EINVAL;
    	return NULL;
  	}

  	// Validate the index
  	if(idx > pCtx->historySz)
  	{
    	errno = EINVAL;
    	return NULL;
  	}

  	// If the history is empty
  	if(0 == pCtx->historySz)
  	{
    	errno = ENOENT;
    	return NULL;
  	}

  	// The index is the real index of the item in the history table
  	// We must make sure that it is not out of bounds

  	// If 'idx' is little than 'insert', it is valid
  	// If 'idx' is equal to 'insert', it is valid only if the history
  	// wrapped
  	// If 'idx' is greater than 'insert', it must be little than 'sz'
  	if(((idx == pCtx->historyInsert) && (pCtx->historySz != pCtx->user.historyLen)) ||
      (idx >= pCtx->historySz))
  	{
    	errno = ENOENT;
    	return NULL;
  	}

  	p = pCtx->history + (idx * pCtx->user.lineLen);

  	strcpy((char *)(pCtx->cmd), (const char *)p);

 	// Update the size of the command line
  	pCtx->lineSz = strlen((char *)(pCtx->cmd));

  	return pCtx->cmd;
}

//reset cmd
static void cmdParserResetLine(cmdParserInstance_t *pCtx)
{
	pCtx->lineSz = 0;
	pCtx->cursor = 0;
	pCtx->cmd[0] = '\0';
}

//get ctl mgs
static int cmdParserGetCtrlMsg(cmdParserInstance_t *pCtx)
{
	unsigned int  i;
	int           rc;
	unsigned char c, len;

  	assert(0 == pCtx->lineSz);

  	pCtx->cmd[0] = CMD_PARSER_CTRL_MSG;
  	pCtx->lineSz ++;

  	// Get the size of the message
  	rc = cmdParserGetChar(pCtx, &len);
  	if(0 != rc)
  	{
    	return -1;
  	}

  	// Check that we don't overflow the buffer
  	if(len > (pCtx->user.lineLen - 2))
  	{
    	CMD_PARSER_ERR(pCtx, "Control message too long: %u (max is %u)\n", len, pCtx->user.lineLen);
    	return -1;
  	}

  	pCtx->cmd[1] = len;
  	pCtx->lineSz ++;

  	// Get the data of the message
  	for(i = 0; i < len; i ++)
  	{
    	rc = cmdParserGetChar(pCtx, &c);
    	if (0 != rc)
    	{
      		return -1;
    	}

    	pCtx->cmd[i + 2] = c;
    	pCtx->lineSz ++;
  	} 

 	return 0;
}


// get a character into cmd
#define	CMD_PARSER_ACCEPT_CHAR(p, c)		_cmdParserAcceptChar((p), (c), __LINE__)
static int _cmdParserAcceptChar(cmdParserInstance_t *pCtx, const unsigned char c, int lineno)
{
	int rc;

	//printf("\n%d#Accepting <0x%x, %c>\n", lineno, c, c);
	(void)lineno;

  	// We don't use isprint() to check if it is a printable char otherwise,
  	// latin chars with accent which are greater than 128 (ASCII set) would
  	// not be printed
  	if(((unsigned)(pCtx->lineSz) < pCtx->user.lineLen))
	{
    	assert((unsigned)(pCtx->cursor) <= pCtx->lineSz);

    	// Make room in the line if necessary
    	if((unsigned)(pCtx->cursor) < pCtx->lineSz)
    	{
      		cmdParserShiftLine(pCtx, 1);
      		pCtx->cmd[pCtx->cursor] = c;
      		pCtx->cursor++;
    	}
    	else
    	{
      		pCtx->cmd[pCtx->cursor] = c;
      		pCtx->cursor ++;
      		pCtx->lineSz ++;
      		pCtx->cmd[pCtx->lineSz] = '\0';
    	}

    	//printf("<0x%x>\n", c);

    	// Echo if activated
    	if(pCtx->echoOn)
    	{
    		unsigned int l1;
    		unsigned char buf[2];

      		// If accented character (cf. man iso_8859-1)
      		if(c > 0x7f)
      		{
        		if(c >= 0xc0)
        		{
          			buf[0] = 0xc3;
          			buf[1] = c - 0x40;
        		}
        		else
        		{
          			buf[0] = 0xc2;
          			buf[1] = c;
        		}
        		l1 = 2;
      		}
      		else
      		{
        		buf[0] = c;
        		l1 = 1;
      		}
      		rc = cmdParserWrite(pCtx, buf, l1);
      		if(l1 != (unsigned)rc)
      		{
        		return -1;
      		}
    	}
  	}
  	else
  	{
    	cmdParserBeep(pCtx);
  	}

  	return 0;
}

//Replace current display cmd by a new one and setting cursor at a given position
static void cmdParserReplaceLine(cmdParserInstance_t *pCtx, const unsigned char *newCmd, unsigned char newCursor)
{
	unsigned int  l_old, l_new, l1;
	unsigned int  i;
	unsigned char c[2];

  	// Copy the new command in the command line buffer
  	if (newCmd)
  	{
    	strncpy((char *)(pCtx->cmd), (const char *)newCmd, pCtx->user.lineLen);
    	pCtx->cmd[pCtx->user.lineLen - 1] = '\0';
  	}

  	// Store the length of the current command line
  	l_old = pCtx->lineSz;

  	// Move the cursor back to column 0
  	cmdParserMoveCursor(pCtx, 0, CMD_PARSER_MOVE_SET);

  	// Update the length of the new line
  	l_new = strlen((char *)(pCtx->cmd));

  	// Display the new command 
  	for(i = 0; i < l_new; i ++)
  	{
    	c[0] = pCtx->cmd[i];

    	// If accented character (cf. man iso_8859-1)
    	if(c[0] > 0x7f)
    	{
      		if(c[0] >= 0xc0)
      		{
        		c[1] = c[0] - 0x40;
        		c[0] = 0xc3;
      		}
      		else
      		{
        		c[1] = c[0];
        		c[0] = 0xc2;
      		}
      		l1 = 2;
    	}
    	else
    	{
      		l1 = 1;
    	}

    	cmdParserWrite(pCtx, c, l1);
  	}

  	// Update the cursor position
  	pCtx->cursor = l_new;

  	// If the previous line was longer than the current one
  	if(l_new < l_old)
  	{
    	// Make believe that the line was 'l_old' long to be able to truncate it
    	pCtx->lineSz = l_old;

    	// Erase the remaining chars from the previous command line if any (from current
    	// cursor position (which is end of new line)
    	cmdParserTruncate(pCtx);

    	// pCtx->lineSz has been updated
  	}
  	else
  	{
    	// Set the size of the line
    	pCtx->lineSz = l_new;
  	}

  	// Put the cursor at the requested position
  	cmdParserMoveCursor(pCtx, newCursor, CMD_PARSER_MOVE_SET);
}


// manage a function key
static void cmdParserHandleFk(cmdParserInstance_t *pCtx, unsigned int fn)
{
	unsigned int           cursor;
	const unsigned char   *p;

  	if(pCtx->functionKey)
  	{
    	cursor = pCtx->cursor;
    	p = pCtx->functionKey((void *)&(pCtx->user.ctx), fn, pCtx->cmd, &cursor);

    	// The preceding function may have displayed anything
    	// and so, we don't know the current cursor position.
    	// Hence, we reset the cursor position to 0 because the
    	// caller is supposed to display the prompt if any and
    	// then let us display the new command line if any
    	pCtx->cursor = 0;

    	cmdParserReplaceLine(pCtx, p, cursor);
  	}
}

// action for STATE 0 of FSM
static int cmdParserState0(cmdParserInstance_t *pCtx)
{
  	// Command mngt parameters
  	pCtx->storedUngetChar  	= 0;
  	pCtx->cursor           	= 0;
  	pCtx->lineSz           	= 0;
  	pCtx->cmd[0]       		= '\0';
  	pCtx->savedCmd[0] 		= '\0';

  	// Reinit the history pointers
  	cmdParserHistoryReset(pCtx);

  	return CMD_PARSER_STATE_1;
}

// actiion for STATE 1 of FSM
static int cmdParserState1(cmdParserInstance_t *pCtx)
{
	unsigned char  c;
	unsigned char *p;
	int            rc;

  	rc = cmdParserGetChar(pCtx, &c);
  	if(0 != rc)
  	{
    	assert(-1 == rc);
    	if(EAGAIN == errno)
    	{
      		return CMD_PARSER_STATE_1 | CMD_PARSER_STATE_AGAIN;
    	}
    	return -1;
  	}

  	switch(c)
  	{
    	case '\0' :
    	{
      		// For some reasons that I don't understand right now,
      		// we receive NUL in place of LF
      	return CMD_PARSER_CURRENT_STATE;
    	}
    	break;

    	case CMD_IN_ASCII_RANGE('A') : // Go to beginning of line
    	case CMD_IN_ASCII_RANGE('B') : // Go one char backward
    	case CMD_IN_ASCII_RANGE('F') : // Go one char forward
    	case CMD_IN_ASCII_RANGE('E') : // Go to end of line
    	case CMD_IN_ASCII_RANGE('[') : // ESC sequence ?
    	{
      		cmdParserUngetChar(pCtx, &c);
	
      		// Save command line
      		strncpy((char *)(pCtx->savedCmd), (char *)(pCtx->cmd), pCtx->user.lineLen);
      		pCtx->savedCmd[pCtx->user.lineLen - 1] = '\0';

      		return CMD_PARSER_STATE_2;
    	}
    	break;

    	case CMD_PARSER_CTRL_MSG :
    	{
      		// If there are chars in the command line
      		if (pCtx->lineSz)
      		{
        		// Store the control char for the next input
        		cmdParserUngetChar(pCtx, &c);

        		// Act as if we got a newline
        		if (pCtx->echoOn)
        		{
          			// Echo the new line char
          			rc = cmdParserWrite(pCtx, &c, 1);
          			if(1 != rc)
          			{
            			return -1;
          			}
        		}

        		// End of FSM
        		return CMD_PARSER_STATE_0;
      		}
      		else
      		{
        		// Get the control message
        		cmdParserGetCtrlMsg(pCtx);

        		// End of FSM
        		return CMD_PARSER_STATE_0;
      		} 
    	}
    	break;

    	case '\t':
    	{
    		unsigned int i;

      		// Manage auto completion if activated
      		if((CMD_PARSER_TAB_AUTO_COMPLETE == pCtx->user.autoOrSpace) && pCtx->user.tab.autoComplete)
      		{
      			unsigned int cursor = pCtx->cursor;

        		p = NULL;
        		pCtx->user.tab.autoComplete(pCtx->user.ctx, pCtx->cmd, &cursor, &p);
        		if(p)
        		{
          			cmdParserReplaceLine(pCtx, p, cursor);
        		}
      		}
      		else
      		{
        		// No auto-completion ==> Display as many spaces as configured
        		for(i = 0; i < pCtx->user.tab.spaces; i++)
        		{
          			CMD_PARSER_ACCEPT_CHAR(pCtx, ' ');
        		}
      		}

      		return CMD_PARSER_CURRENT_STATE;
    	}
   		break;

    	case '\n' :
    	case '\r' :  // End of command line
    	{
        	c = '\n';

        	if(pCtx->echoOn)
			{	
				// Echo the new line char even if non echo mode !
        		rc = cmdParserWrite(pCtx, &c, 1);
        		if(1 != rc)
        		{
          			return -1;
        		}
      		}
        	// End of FSM
        	return CMD_PARSER_STATE_0;
    	}
    	break;

    	case CMD_IN_ASCII_RANGE('K') : // Emacs edition = Erase from current to end of line
    	{
      		cmdParserTruncate(pCtx);
      		return CMD_PARSER_CURRENT_STATE;
    	}
    	break;

    	case CMD_IN_ASCII_RANGE('D') : // Emacs edition = SUPPR
    	{
      		if((pCtx->lineSz > 0) && ((unsigned)(pCtx->cursor) < pCtx->lineSz))
      		{
        		cmdParserShiftLine(pCtx, -1);
      		}
      		else
      		{
        		// If the line is empty, we behaves like the shell: EOF
        		if(0 == pCtx->lineSz)
				{
          			assert(0 == pCtx->cursor);

          			// Erase the command line
          			cmdParserResetLine(pCtx);

          			errno = ECONNRESET;
          			return -1;
        		}
        		else
				{
          			cmdParserBeep(pCtx);
        		}
      		}

      		return CMD_PARSER_CURRENT_STATE;
    	}
    	break;

    	case CMD_IN_ASCII_RANGE('H') :
    	case 0x7F          : // DEL
    	{
      		if(pCtx->cursor > 0)
      		{
        		//printf("\nDEL\n");
        		cmdParserMoveCursor(pCtx, -1, CMD_PARSER_MOVE_CUR);

        		cmdParserShiftLine(pCtx, -1);
      		}
      		else
      		{
        		cmdParserBeep(pCtx);
      		}

      		return CMD_PARSER_CURRENT_STATE;
    	}
    	break;

    	case (unsigned char)(EOF) : // End Of File
    	{
      		// Erase the command line
      		cmdParserResetLine(pCtx);

      		// Set errno
      		errno = ECONNRESET;

      		return -1;
    	}
    	break;

    	case 0xc3 : // Accented character
    	case 0xc2 :
    	{
      		cmdParserUngetChar(pCtx, &c);

      		// Save command line
      		strncpy((char *)(pCtx->savedCmd), (char *)(pCtx->cmd), pCtx->user.lineLen);
      		pCtx->savedCmd[pCtx->user.lineLen - 1] = '\0';

      		return CMD_PARSER_STATE_8;
    	}
    	break;

    	default : // Accept the char in the command line unless it is
              	  // not printable
    	{
      		CMD_PARSER_ACCEPT_CHAR(pCtx, c);

      		return CMD_PARSER_CURRENT_STATE;
    	}
  	} // End switch

  	assert(0);
}

// action for STATE 2 of FSM
static int cmdParserState2(cmdParserInstance_t *pCtx)
{
	unsigned char  c;
	int            rc;

  	rc = cmdParserGetChar(pCtx, &c);

  	if(rc != 0)
  	{
    	assert(-1 == rc);
    	if(EAGAIN == errno)
    	{
      		return CMD_PARSER_STATE_2 | CMD_PARSER_STATE_AGAIN;
    	}

    	return -1;
  	}

  	switch(c)
  	{
    	case CMD_IN_ASCII_RANGE('A') :  // Go to beginning of line
    	{
      		if(pCtx->cursor > 0)
      		{
        		cmdParserMoveCursor(pCtx, - (pCtx->cursor), CMD_PARSER_MOVE_CUR);
      		}
      		else
      		{
        		cmdParserBeep(pCtx);
      		}

      		return CMD_PARSER_CURRENT_STATE;
    	}
    	break;

    	case CMD_IN_ASCII_RANGE('B') : // Go one char backward
    	{
      		if(pCtx->cursor > 0)
      		{
        		cmdParserMoveCursor(pCtx, -1, CMD_PARSER_MOVE_CUR);
      		}
      		else
      		{
        		cmdParserBeep(pCtx);
      		}

      		return CMD_PARSER_CURRENT_STATE;
    	}
   		break;

    	case CMD_IN_ASCII_RANGE('F') : // Go one char forward
    	{
      		if((unsigned)(pCtx->cursor) < pCtx->lineSz)
      		{
        		cmdParserMoveCursor(pCtx, 1, CMD_PARSER_MOVE_CUR);
      		}
      		else
      		{
        		cmdParserBeep(pCtx);
      		}

      		return CMD_PARSER_CURRENT_STATE;
    	}
    	break;

    	case CMD_IN_ASCII_RANGE('E') : // Go to end of line
    	{
      		if((unsigned)(pCtx->cursor) < pCtx->lineSz)
      		{
        		cmdParserMoveCursor(pCtx, pCtx->lineSz - pCtx->cursor, CMD_PARSER_MOVE_CUR);
      		}
      		else
      		{
        		cmdParserBeep(pCtx);
      		}

      		return CMD_PARSER_CURRENT_STATE;
    	}
    	break;

    	default :
    	{
      		// ESC ?
      		if(c != CMD_IN_ASCII_RANGE('['))
      		{
        		cmdParserUngetChar(pCtx, &c);
        		return CMD_PARSER_STATE_1;
      		}

      		return CMD_PARSER_STATE_3;
    	}
    	break;
  	}
}

// action for STATE 3 of FSM
static int cmdParserState3(cmdParserInstance_t *pCtx)
{
	unsigned char c;
	int           rc;

  	rc = cmdParserGetChar(pCtx, &c);

  	if(rc != 0)
  	{
    	assert(-1 == rc);
    	if(EAGAIN == errno)
    	{
      		return CMD_PARSER_STATE_3 | CMD_PARSER_STATE_AGAIN;
    	}

    	return -1;
  	}

  	if('O' == c)
  	{
    	return CMD_PARSER_STATE_5;
  	}

  	if(c != '[')
  	{
    	cmdParserUngetChar(pCtx, &c);
    	return CMD_PARSER_STATE_1;
  	}

  	return CMD_PARSER_STATE_4;
}

// action for STATE 4 of FSM
static int cmdParserState4(cmdParserInstance_t *pCtx)
{
	int           rc;
	unsigned char c, c1;

  	rc = cmdParserGetChar(pCtx, &c);

  	if(rc != 0)
  	{
    	assert(-1 == rc);
    	if(EAGAIN == errno)
    	{
      		return CMD_PARSER_STATE_4 | CMD_PARSER_STATE_AGAIN;
    	}

    	return -1;
  	}

  	switch(c)
  	{
    	case '5' : // PAGE UP
    	case '6' : // PAGE DOWN
    	{
      		rc = cmdParserGetChar(pCtx, &c1);

      		if(rc != 0)
      		{
        		assert(-1 == rc);
        		if(EAGAIN == errno)
        		{
          			cmdParserUngetChar(pCtx, &c);
          			return CMD_PARSER_STATE_4 | CMD_PARSER_STATE_AGAIN;
        		}

        		return -1;
      		}

      		if('~' != c1)
      		{
        		return CMD_PARSER_STATE_1;
      		}
    	}
    	
		// No break !!
    	case 'A' : // UP arrow
    	case 'B' : // DOWN arrow
    	{
    		const unsigned char *p;
    		unsigned int         cursor;

      		// If history activated
      		if(!(pCtx->historyOn))
      		{
        		return CMD_PARSER_STATE_1;
      		}

      		// Up/down in history
      		if('A' == c)
      		{
        		rc = cmdParserHistoryUp(pCtx, &p);
        		if(0 != rc)
        		{
          			p = cmdParserHistoryOldest(pCtx);
        		}
      		}
      		else
      		{
        		if('B' == c)
        		{
          			rc = cmdParserHistoryDown(pCtx, &p);
          			if(0 != rc)
          			{
            			p = pCtx->savedCmd;
          			}
        		}
        		else
        		{
          			if('5' == c)
          			{
            			p = cmdParserHistoryOldest(pCtx);
          			}
          			else
          			{
            			assert('6' == c);
            			p = cmdParserHistoryNewest(pCtx);
          			}
        		}
      		}

      		if(!p)
      		{
        		// Nothing in the history
        		return CMD_PARSER_STATE_1;
      		}

      		// Overwrite the current displayed command line by the new one
      		// The cursor is set at the end of the line
      		cursor = strlen((const char *)p);
      		cmdParserReplaceLine(pCtx, p, cursor);
      
			return CMD_PARSER_STATE_2;
    	}
    	break;

    	case 'C' : // RIGHT arrow
    	{
      		if((unsigned)(pCtx->cursor) < pCtx->user.lineLen)
      		{
        		cmdParserMoveCursor(pCtx, 1, CMD_PARSER_MOVE_CUR);
      		}
      		else
      		{
        		cmdParserBeep(pCtx);
      		}
      		return CMD_PARSER_STATE_2;
    	}
    	break;

    	case 'D' : // LEFT arrow
    	{
      		if(pCtx->cursor > 0)
      		{
        		cmdParserMoveCursor(pCtx, -1, CMD_PARSER_MOVE_CUR);
      		}
      		else
      		{
        		cmdParserBeep(pCtx);
      		}
      		return CMD_PARSER_STATE_2;
    	}
    	break;

    	case 'H' : // HOME = CTRL A
    	{
      		c = CMD_IN_ASCII_RANGE('A');
      		cmdParserUngetChar(pCtx, &c);

      		return CMD_PARSER_STATE_2;
    	}
    	break;

    	case 'F' : // END = CTRL E
    	{
      		c = CMD_IN_ASCII_RANGE('E');
      		cmdParserUngetChar(pCtx, &c);

      		return CMD_PARSER_STATE_2;
    	}
    	break;

    	case '1' :  // PF5 to PF8
    	{
      		return CMD_PARSER_STATE_6;
    	}
    	break;

    	case '2' :  // PF9 to PF12 or insert
    	{
      		rc = cmdParserGetChar(pCtx, &c);

      		if(rc != 0)
      		{
        		assert(-1 == rc);
        		if(EAGAIN == errno)
        		{
          			cmdParserUngetChar(pCtx, &c);
          			return CMD_PARSER_STATE_4 | CMD_PARSER_STATE_AGAIN;
        		}

        		return -1;
      		}

      		// Insert from keypad ==> Ignore it...
      		if('~' == c)
      		{
        		//printf("\nINSERT\n");
        		return CMD_PARSER_STATE_1;
      		}
      		cmdParserUngetChar(pCtx, &c);
      		return CMD_PARSER_STATE_7;
    	}
    	break;

    	case '3' :  // SUPPR
    	{
      		rc = cmdParserGetChar(pCtx, &c);

      		if(rc != 0)
      		{
        		assert(-1 == rc);
        		if(EAGAIN == errno)
        		{
          			cmdParserUngetChar(pCtx, &c);
          			return CMD_PARSER_STATE_4 | CMD_PARSER_STATE_AGAIN;
        		}

        		return -1;
      		}

      		if('~' == c)
      		{
        		if((pCtx->lineSz > 0) && ((unsigned)(pCtx->cursor) < pCtx->lineSz))
        		{
          			cmdParserShiftLine(pCtx, -1);
        		}
        		else
        		{
          			cmdParserBeep(pCtx);
        		}
      		}
      		return CMD_PARSER_STATE_1;
    	}
    	break;

    	default :
    	{
      		cmdParserUngetChar(pCtx, &c);
      		return CMD_PARSER_STATE_1;
    	}
  	}

  	assert(0);
}


// action for STATE 5 of FSM
static int cmdParserState5(cmdParserInstance_t *pCtx)
{
	unsigned char c;
	int           rc;

  	rc = cmdParserGetChar(pCtx, &c);

  	if(rc != 0)
  	{
    	assert(-1 == rc);
    	if(EAGAIN == errno)
    	{
      		cmdParserUngetChar(pCtx, &c);
      		return CMD_PARSER_STATE_5 | CMD_PARSER_STATE_AGAIN;
    	}

    	return -1;
  	}

  	switch(c)
  	{
    	case 'P' : // FUNCTION key 1
    	{
      		cmdParserHandleFk(pCtx, CMD_PARSER_FUNC_KEY_1);

      		return CMD_PARSER_STATE_1;
    	}
    	break;

    	case 'Q' : // FUNCTION key 2
    	{
      	cmdParserHandleFk(pCtx, CMD_PARSER_FUNC_KEY_2);

      		return CMD_PARSER_STATE_1;
    	}
    	break;

    	case 'R' : // FUNCTION key 3
    	{
      		cmdParserHandleFk(pCtx, CMD_PARSER_FUNC_KEY_3);

      		return CMD_PARSER_STATE_1;
    	}
    	break;

    	case 'S' : // FUNCTION key 4
    	{
      		cmdParserHandleFk(pCtx, CMD_PARSER_FUNC_KEY_4);

      		return CMD_PARSER_STATE_1;
    	}
    	break;

    	case 'A' : // UP arrow
    	{
      		printf("\nUP\n");
      		return -1;
    	}
    	break;

    	case 'B' : // DOWN arrow
    	{
      		printf("\nDOWN\n");
      		return -1;
    	}
    	break;

    	case 'C' : // RIGHT arrow
    	{
      		printf("\nRIGHT\n");
      		return -1;
    	}
    	break;

    	case 'D' : // LEFT arrow
    	{
      		printf("\nLEFT\n");
      		return -1;
    	}
    	break;

    	default :
    	{
      		cmdParserUngetChar(pCtx, &c);
      		return CMD_PARSER_STATE_1;
    	}
  	}

  	assert(0);
}


// action for STATE 6 of FSM
static int cmdParserState6(cmdParserInstance_t *pCtx)
{
	unsigned char  c, c1;
	int            rc;

  	rc = cmdParserGetChar(pCtx, &c);

  	if(rc != 0)
  	{
    	assert(-1 == rc);
    	if(EAGAIN == errno)
    	{
      		return CMD_PARSER_STATE_6 | CMD_PARSER_STATE_AGAIN;
    	}

    	return -1;
  	}

  	rc = cmdParserGetChar(pCtx, &c1);

  	if(rc != 0)
  	{
    	assert(-1 == rc);
    	if(EAGAIN == errno)
    	{
      		cmdParserUngetChar(pCtx, &c);
      		return CMD_PARSER_STATE_6 | CMD_PARSER_STATE_AGAIN;
    	}

    	return -1;
  	}

  	if(c1 != '~')
  	{
    	cmdParserUngetChar(pCtx, &c1);
    	return CMD_PARSER_STATE_1;
  	}

  	switch(c)
  	{
    	case '5' : // FUNCTION key 5
    	{
      		cmdParserHandleFk(pCtx, CMD_PARSER_FUNC_KEY_5);

      		return CMD_PARSER_STATE_1;
    	}
    	break;
    	case '7' : // FUNCTION key 6
    	{
      		cmdParserHandleFk(pCtx, CMD_PARSER_FUNC_KEY_6);

      		return CMD_PARSER_STATE_1;
    	}
   		break;
    	case '8' : // FUNCTION key 7
    	{
      		cmdParserHandleFk(pCtx, CMD_PARSER_FUNC_KEY_7);

      		return CMD_PARSER_STATE_1;
    	}
    	break;
    	case '9' : // FUNCTION key 8
    	{
      		cmdParserHandleFk(pCtx, CMD_PARSER_FUNC_KEY_8);

      		return CMD_PARSER_STATE_1;
    	}
    	break;
    	default :
    	{
      		cmdParserUngetChar(pCtx, &c);
      		return CMD_PARSER_STATE_1;
    	}
    	break;
  	}	

  	assert(0);
}

// action for STATE 7 of FSM
static int cmdParserState7(cmdParserInstance_t *pCtx)
{
	unsigned char c, c1;
	int           rc;

  	rc = cmdParserGetChar(pCtx, &c);

  	if(rc != 0)
  	{
    	assert(-1 == rc);
    	if(EAGAIN == errno)
    	{
      		return CMD_PARSER_STATE_7 | CMD_PARSER_STATE_AGAIN;
    	}

    	return -1;
  	}

  	rc = cmdParserGetChar(pCtx, &c1);

  	if(rc != 0)
  	{
    	assert(-1 == rc);
    	if(EAGAIN == errno)
    	{
      		cmdParserUngetChar(pCtx, &c);
      		return CMD_PARSER_STATE_7 | CMD_PARSER_STATE_AGAIN;
    	}

    	return -1;
  	}

  	if(c1 != '~')
  	{
    	cmdParserUngetChar(pCtx, &c1);
    	return CMD_PARSER_STATE_1;
  	}

  	switch(c)
  	{
    	case '0' : // FUNCTION key 9
    	{
      		cmdParserHandleFk(pCtx, CMD_PARSER_FUNC_KEY_9);

      		return CMD_PARSER_STATE_1;
    	}
    	break;
    	case '1' : // FUNCTION key 10
    	{
      		cmdParserHandleFk(pCtx, CMD_PARSER_FUNC_KEY_10);

      		return CMD_PARSER_STATE_1;
    	}
    	break;
    	case '3' : // FUNCTION key 11
    	{
      		cmdParserHandleFk(pCtx, CMD_PARSER_FUNC_KEY_11);

      		return CMD_PARSER_STATE_1;
    	}
    	break;
    	case '4' : // FUNCTION key 12
    	{
      		cmdParserHandleFk(pCtx, CMD_PARSER_FUNC_KEY_12);

      		return CMD_PARSER_STATE_1;
    	}
    	break;
    	default :
    	{
      		cmdParserUngetChar(pCtx, &c);
      		return CMD_PARSER_STATE_1;
    	}
  	}

  	assert(0);
} 

// action for STATE 8 of FSM
static int cmdParserState8(cmdParserInstance_t *pCtx)
{
	unsigned char c, c1;
	int           rc;

  	rc = cmdParserGetChar(pCtx, &c);

  	if(rc != 0)
  	{
    	assert(-1 == rc);
    	if(EAGAIN == errno)
    	{
      		return CMD_PARSER_STATE_8 | CMD_PARSER_STATE_AGAIN;
    	}

    	return -1;
  	}

  	rc = cmdParserGetChar(pCtx, &c1);

  	if(rc != 0)
  	{
    	assert(-1 == rc);
    	if(EAGAIN == errno)
    	{
      		cmdParserUngetChar(pCtx, &c);
      		return CMD_PARSER_STATE_8 | CMD_PARSER_STATE_AGAIN;
    	}

    	return -1;
  	}

  	// Accented character from man iso_8859-1
  	if(0xc3 == c && c1 >= 0x80 && c1 <= 0xbf)
  	{
    	// c1 + 0x40 = value in man iso_8859-1
    	CMD_PARSER_ACCEPT_CHAR(pCtx, c1 + 0x40);
    	return CMD_PARSER_STATE_1;
  	}
  	if(0xc2 == c && c1 >= 0xa0 && c1 <= 0xbf)
  	{
    	// c1 is the value in man iso_8859-1
    	CMD_PARSER_ACCEPT_CHAR(pCtx, c1);
    	return CMD_PARSER_STATE_1;
  	}

  	// Ignore the chars

  	return CMD_PARSER_STATE_1;
}

typedef int (* cmdParserTransition_t)(cmdParserInstance_t *pCtx);


static cmdParserTransition_t cmdParserFsm[] =
{
  cmdParserState0,
  cmdParserState1,
  cmdParserState2,
  cmdParserState3,
  cmdParserState4,
  cmdParserState5,
  cmdParserState6,
  cmdParserState7,
  cmdParserState8
};

// check if a sting is a number
static int cmdParserIsNumber(const unsigned char *str)
{
  	while(*str)
  	{
    	if(!isdigit(*str))
    	{
      		return 0;
    	}
    	str++;
  	}

  	return 1;
}


// get a cmd
static int cmdParserGet(cmdParserInstance_t *pCtx)
{
	unsigned char *p;
	int            newState;
	int            save;

  	// Get the command
  	do
  	{
    	// Trigger the FSM transition
    	newState = (cmdParserFsm[pCtx->state])(pCtx);

    	// If error
    	if(newState < 0)
    	{
      		return -1;
    	}

    	// If we need to go back to previous state
    	if(CMD_PARSER_PREVIOUS_STATE == newState)
    	{
      		// Save current state
      		save = pCtx->state;

      		// The current state becomes the previous state
      		pCtx->state = pCtx->prevState;

      		// The previous state becomes the current state
      		pCtx->prevState = save;
    	}
    	else
    	{
      		// If we stay in the current state
      		if(CMD_PARSER_CURRENT_STATE == newState)
      		{
        		// The current state does not change

        		// The previous state becomes the current state
        		pCtx->prevState = pCtx->state;
      		}
      		else
     		{
        		// The previous state becomes the current state
        		pCtx->prevState = pCtx->state;

        		// The current state becomes the new state
        		pCtx->state = newState;
      		}
    	}

  	} while(!(pCtx->state & CMD_PARSER_STATE_AGAIN) &&
            (pCtx->state >= 0)                  &&
            (CMD_PARSER_STATE_0 != pCtx->state));

  	// If no errors, check if it is not an history invocation
  	if(pCtx->state == CMD_PARSER_STATE_0)
  	{
    	// If the history is activated
    	if(pCtx->historyOn)
    	{
      		// If the history shortcut is configured
      		if(pCtx->user.historyShortCut)
      		{
        		// Look for the shortcut at the first non blank position
        		// in the command line
        		p = pCtx->cmd;
        		while(*p && CMD_IS_BLANK(*p))
        		{
          			p ++;
        		}

        		// If the line is not empty or not full of blanks
        		if(*p)
        		{
          			// If the 1st non blank char is the history shortcut
          			if(pCtx->user.historyShortCut == *p)
          			{
          				unsigned int i;

            			// Get the history index
            			p++;
            			if(!(*p))
            			{
              				// No index, return the line to the user
              				goto historyAdd;
            			}

            			// If it is a reference to the last command of the history (e.g. !!)
            			if((pCtx->user.historyShortCut == *p) && !(*(p + 1)))
            			{
            				const unsigned char *p1;

              				// Get the newest command from the history
              				p1 = cmdParserHistoryNewest(pCtx);
              				if(!p1)
              				{
                				// The history is empty, return the line to the user
                				goto historyAdd;
              				}
              				else
              				{
                				strcpy((char *)(pCtx->cmd), (const char *)p1);

                				// Update the line size
                				pCtx->lineSz = strlen((char *)(pCtx->cmd));
              				}

              				return 0;
            			}

            			if(!cmdParserIsNumber(p))
            			{
              				// Bad index, return the line to the user
              				goto historyAdd;
            			}

            			i = atoi((char *)p);
            			if((0 == pCtx->historySz) || (i >= pCtx->historySz))
            			{
              				// Index out of range, return the line to the user
              				goto historyAdd;
            			}

            			// This will load 'pCtx->cmd' with the history entry
            			p = cmdParserHistoryGet((cmdParser_t *)&(pCtx->user.ctx), i);
            			assert(p);

          			} 
        		} 
      		}
    	}

historyAdd:

    	// Store the line in the history
    	cmdParserHistoryAdd(pCtx);

    	return 0;
  	}

  	// If there are no data available
  	if(pCtx->state & CMD_PARSER_STATE_AGAIN)
  	{
    	// Make sure we are configured to behave like that
    	assert(pCtx->user.nonBlocking);

    	// Compute the next state
    	pCtx->state = pCtx->state & ~CMD_PARSER_STATE_AGAIN;

    	errno = EAGAIN;
    	return -1;
  	}

  	assert(-1 == pCtx->state);

  	// Compute the next state
  	pCtx->state = CMD_PARSER_STATE_0;

  	return -1;
}


// get a new CMD instance
cmdParser_t *cmdParserNew(cmdParserParam_t *param)
{
	int                  errSav;
	cmdParserInstance_t  *pCtx;
	struct termios       newTermSettings;
	int                  rc;

  	if(!param)
  	{
    	CMD_PARSER_ERR(NULL, "NULL parameter\n");
    	errno = EINVAL;
    	return NULL;
  	}

  	// Check that the context is coherent
  	if(!(param->lineLen))
  	{
    	CMD_PARSER_ERR(NULL, "The length of the command line must be greater than 0\n");
    	errno = EINVAL;
    	return NULL;
  	}

  	if(param->fdIn < 0)
  	{
    	CMD_PARSER_ERR(NULL, "Invalid input file descriptor (%d)\n", param->fdIn);
    	errno = EINVAL;
    	return NULL;
  	}

  	if(param->fdOut < 0)
  	{
    	CMD_PARSER_ERR(NULL, "Invalid output file descriptor (%d)\n", param->fdOut);
    	errno = EINVAL;
    	return NULL;
  	}

  	// The history shortcut must not be blank
  	if(CMD_IS_BLANK(param->historyShortCut))
  	{
    	CMD_PARSER_ERR(NULL, "Invalid history shortcut: blank characters (0x%x)" " are not allowed\n", param->historyShortCut);
    	errno = EINVAL;
   		return NULL;
  	}

  	// Allocate an instance along with the buffers belonging to it
  	pCtx = (cmdParserInstance_t *)malloc(sizeof(cmdParserInstance_t)           + // Main structure
                                      	param->lineLen                      + // Command line
                                      	param->lineLen                      + // Saved command line
                                      	(param->historyLen * param->lineLen)   // History
                                     	);
  	if(NULL == pCtx)
  	{
    	errSav = errno;
    	CMD_PARSER_ERR(NULL, "Error %d while allocating the context\n", errno);
   		errno = errSav;
    	return NULL;
  	}

  	// Reset the instance
  	memset(pCtx, 0, sizeof(*pCtx));

  	// Populate the instance
  	pCtx->user           = *param;
  	pCtx->cmd       = (unsigned char *)(pCtx + 1);
  	pCtx->savedCmd = pCtx->cmd + param->lineLen;
 	pCtx->state          = CMD_PARSER_STATE_0;
  	pCtx->prevState     = CMD_PARSER_STATE_0;
  	pCtx->functionKey   = NULL;

  	// History is activated only if histo_len > 0
  	if(param->historyLen)
  	{
    	pCtx->historyOn = 1;
    	pCtx->history   = pCtx->savedCmd + param->lineLen;
  	}
  	else
  	{
    	pCtx->historyOn = 0;
  	}

  	// By default, echo is activated
  	pCtx->echoOn = 1;

  	// If non blocking mode is requested, set the attribute on the input
  	if(param->nonBlocking)
  	{
    	pCtx->inFlag = fcntl(param->fdIn, F_GETFL);
    	rc = fcntl(param->fdIn, F_SETFL, pCtx->inFlag | O_NONBLOCK);
    	if(0 != rc)
    	{
      		errSav = errno;
      		CMD_PARSER_ERR(NULL, "Error %d while setting NO_BLOCK flag on input\n", errno);
      		errno = errSav;
      		free(pCtx);
      		return NULL;
    	}
  	}
  	
   	// Save the terminal's settings
   	rc = tcgetattr(pCtx->user.fdIn, &(pCtx->origTermSettings));
   	if(0 != rc)
   	{
   		errSav = errno;
   		CMD_PARSER_ERR(NULL, "Error %d while getting input terminal attributes\n", errno);
   		errno = errSav;
   		free(pCtx);
   		return NULL;
  	}

	newTermSettings = pCtx->origTermSettings;
   	cfmakeraw(&newTermSettings);
   	newTermSettings.c_oflag |= (OPOST | ONLCR);
    //newTermSettings.c_iflag |= (IUTF8 | IXON | IGNCR);

   	rc = tcsetattr(pCtx->user.fdIn, TCSANOW, &newTermSettings);
   	if(0 != rc)
    {
    	errSav = errno;
      	CMD_PARSER_ERR(NULL, "Error %d on tcsetattr()\n", errno);
      	errno = errSav;
      	free(pCtx);
      	return NULL;
    }
  	
    return (cmdParser_t *)&(pCtx->user.ctx);
}

// delete a cmd instance
void cmdParserDelete(cmdParser_t *pInst)
{
	cmdParserInstance_t *pCtx = CMD_PARSER_USER_TO_INSTANCE(pInst);

  	errno = 0;

  	if(!pInst)
  	{
   		CMD_PARSER_ERR(NULL, "NULL parameter\n");
    	errno = EINVAL;
    	return;
  	}

  	// Make sure the parameter points on something which looks like an instance
  	assert(pCtx->user.lineLen > 0);
  	assert(pCtx->user.fdIn >= 0);
  	assert(pCtx->user.fdOut >= 0);

    // Set back the terminal settings
    if(0 != tcsetattr(pCtx->user.fdIn, TCSANOW, &(pCtx->origTermSettings)))
    {
      	CMD_PARSER_ERR(pCtx, "Error %d on tcsetattr(%d)\n", errno, pCtx->user.fdIn);
    }

  	// Restore the terminal flags if we are in non blocking mode
  	if(pCtx->user.nonBlocking)
  	{
    	if (-1 == fcntl(pCtx->user.fdIn, F_SETFL, pCtx->inFlag))
    	{
      		CMD_PARSER_ERR(pCtx, "Error %d on fcntl()\n", errno);
    	}
  	}

  	// For debug purposes, reset the memory zone
  	memset(pCtx, 0, sizeof(*pCtx));

  	// Free the memory zone
  	free(pCtx);
}

// set debug level
int cmdParserSetDebugLevel(cmdParser_t * pInst, int dbg)
{
	cmdParserInstance_t *pCtx = CMD_PARSER_USER_TO_INSTANCE(pInst);
	int                 prev;

  	if(!pCtx || (dbg < 0))
  	{
    	errno = EINVAL;
    	return -1;
  	}

  	prev = pCtx->dbg;
  	assert(prev >= 0);

  	pCtx->dbg = dbg;

  	return prev;
}

//set/unset ECHO
int cmdParserSetEcho(cmdParser_t *pInst, int echo)
{
    cmdParserInstance_t *pCtx = CMD_PARSER_USER_TO_INSTANCE(pInst);
	int                 prev;

  	if(!pCtx || (echo < 0))
  	{
    	errno = EINVAL;
    	return -1;
  	}

  	prev = pCtx->echoOn;
  	assert(prev >= 0);

  	pCtx->echoOn = echo;

  	return prev;
}


//set/unset history
int cmdParserSetHistory(cmdParser_t *pInst, int history)
{
    cmdParserInstance_t *pCtx = CMD_PARSER_USER_TO_INSTANCE(pInst);
	int                 prev;

  	if(!pCtx || (history < 0))
  	{
    	errno = EINVAL;
    	return -1;
  	}

  	prev = pCtx->historyOn;
  	assert(prev >= 0);

  	pCtx->historyOn = history;

  	return prev;
}


// translate iso_8859-1 chars
static int cmdParserTranslateAccents(cmdParserInstance_t *pCtx)
{
	unsigned int   max = pCtx->user.lineLen;
	unsigned int   len = pCtx->lineSz;
	unsigned int   holes = max - len - 1;
	unsigned char *p = pCtx->cmd, *p1;
	unsigned char  c;

  	assert(strlen((char *)(pCtx->cmd)) == len);

  	// If it is a control message, there no translation to do
  	if(CMD_PARSER_CTRL_MSG == *p)
  	{
    	return 0;
  	}

  	while (*p)
  	{
    	// If *p out of ASCII set
    	if(*p > 0x7f)
    	{
      		// Is there a remaining hole ?
      		if(!holes)
      		{
        		errno = ENOSPC;
        		return -1;
      		}

      		c = *p;

      		if(*p >= 0xc0)
      		{
        		*p = 0xc3;
        		c -= 0x40;
      		}
      		else
      		{
        		*p = 0xc2;
      		}

      		// Shift the string
      		p1 = pCtx->cmd + len;
      		while(p1 > p)
      		{
        		*(p1 + 1) = *p1;
        		p1--;
      		}
      		*(p + 1) = c;
      		p += 2;
      		len += 1;

      		holes--;
    	}
    	else
    	{
      		p++;
    	}
  	}

  	// Update the effective size of the line
  	pCtx->lineSz = len;

  	return 0;
}


// read cmd
unsigned char *cmdParserInteract(cmdParser_t *pInst)
{
	cmdParserInstance_t *pCtx = CMD_PARSER_USER_TO_INSTANCE(pInst);
	int                 rc;

  	// Reset errno
  	errno = 0;

  	if(!pCtx)
  	{
    	errno = EINVAL;    
    	return NULL;
  	}

  	rc = cmdParserGet(pCtx);
  	if(0 == rc)
  	{
    	// Translate the accented characters
    	rc = cmdParserTranslateAccents(pCtx);
    	if(0 == rc)
    	{
      		return pCtx->cmd;
    	}
    	else
    	{
      		return NULL;
    	}
  	}
  	else
  	{
    	return NULL;
  	}
}


// register callback for function key
cmdParserFnKey_t cmdParserFunctionKey(cmdParser_t *pInst, cmdParserFnKey_t functionKey)
{
	cmdParserInstance_t *pCtx = CMD_PARSER_USER_TO_INSTANCE(pInst);
	cmdParserFnKey_t	prev;
	
  	if(!pCtx)
  	{
    	errno = EINVAL;
    	return NULL;
  	}

  	errno = 0;

  	prev = pCtx->functionKey;

  	pCtx->functionKey = functionKey;

  	return prev;
}
