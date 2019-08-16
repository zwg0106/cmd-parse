#include <getopt.h>
#include <unistd.h>
#include <stdio.h>
#include <libgen.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <libgen.h>
#include "cmd_parser.h"

static const char *cmdPrompt = ">";

static cmdParser_t *cmdInstance;

static struct option cmdParserLongOpts[] = 
{
	{"debug",  	required_argument, 	NULL, 	'd'},
	{"help", 	no_argument,		NULL,	'h'},
	{NULL,		0,				    NULL,	0  }
};

static void cmdParserHelp(char *p)
{
  	fprintf(stderr,
          "\n"
          "Usage: %s [<options> ...]\n"
          "\n"
          "Options:\n"
          "\n"
          "\t-d | --debug level : Set debug mode to level\n"
          "\t-h | --help        : This help\n"
          ,
          basename(p)
          );
}

static int cmdIsNumber(const unsigned char *str)
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

static void displayHistoryItem(unsigned char *item, unsigned int idx)
{
  	if(item)
  	{
    	printf("%u. %s\n", idx, item);
  	}
}

static const unsigned char *functionKey(
                                         cmdParser_t        	*pInst,
                                         unsigned int         	fn,
                                         const unsigned char 	*cmd,
                                         unsigned int 			*cursor
                                        )
{
	char buf[128];

	(void)cmd;
	(void)pInst;

	snprintf(buf, sizeof(buf), "\nFunction key %u has been pressed (cursor pos = %u)\n%s ", fn + 1, *cursor, cmdPrompt);
	buf[sizeof(buf) - 1] = '\0';
	write(1, buf, strlen(buf));

	// New cursor position
	*cursor = 4;

	return (const unsigned char *)"New cmd";
}

int main(int ac, char *av[])
{
	unsigned int      options;
	int               opt;
	int               rc;
	int               dbg = 0;
	cmdParserParam_t  params;
	unsigned char    *p;

	options = 0;

	// Parse the command line
	optind = 0;
	while ((opt = getopt_long(ac, av, "d:h", cmdParserLongOpts, NULL)) != EOF)
	{
  		switch(opt)
  		{
    		case 'd' : // Debug level
    		{
      			if(!cmdIsNumber((unsigned char *)optarg))
      			{
        			fprintf(stderr, "The debug level must be a number instead of '%s'\n", optarg);
        			rc = 1;
        			goto error;
      			}
      			dbg = atoi(optarg);
      			options |= 0x01;
    		}
    		break;

    		case 'h' : // Help
    		{
      			cmdParserHelp(av[0]);
      			options |= 0x02;
      			exit(0);
    		}
    		break;

    		case '?' :
    		default:
    		{
      			cmdParserHelp(av[0]);
      			exit(1);
    		}
  		} 
	}

	// Get a CMD context
	memset(&params, 0, sizeof(params));
	params.lineLen       = 128;
	params.nonBlocking   = 0;
	params.fdIn          = 0;
	params.fdOut         = 1;
	params.historyLen      = 5;
	params.autoOrSpace     = CMD_PARSER_TAB_SPACES;
	params.tab.spaces     = 4;
	params.historyShortCut = '!';
	cmdInstance = cmdParserNew(&params);
	if (NULL == cmdInstance)
	{
  		fprintf(stderr, "Unable to allocate a CMDLINE instance (errno = %d)\n", errno);
  		rc = 1;
  		goto error;
	}

	// Set debug level
	cmdParserSetDebugLevel(cmdInstance, dbg);

	// Set function key callback
	cmdParserFunctionKey(cmdInstance, functionKey);

	do
	{
  		// Display the prompt
  		printf("%s ", cmdPrompt);
  		fflush(stdout);
  		p = cmdParserInteract(cmdInstance);
  		if(p)
  		{
    		printf("The command line is: <%s>\n", p);
  		}
  		else
  		{
    		if(ECONNRESET == errno)
    		{
      			printf("End of session\n");
      			rc = 0;
      			break;
    		}

    		rc = 1;
    		break;
  		}
	}while(p);

	// Deallocate the command line instance
	cmdParserDelete(cmdInstance);

	rc = 0;

	error:

	return rc;
}
