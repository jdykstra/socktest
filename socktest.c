/*  socktest.c - Command line processor for testing socket API behaviors.
 *
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <readline/readline.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include "netinet/in.h"
#include <arpa/inet.h>
#include <netdb.h>

typedef void (*sighandler_t)(int);

#ifndef FALSE
#define FALSE (0)
#define TRUE (1)
#endif

#ifndef MIN
#define MIN(a,b)  ((a < b)? a : b)
#endif

#ifndef MAX
#define MAX(a,b)  ((a > b)? a : b)
#endif


/*  Constants.  */
#define MAXTOKENS        100                /*  Maximum tokens in command line  */
#define CMDDELIMS        " ,="              /*  Command token delimiters  */
#define MAXSOCKETS       10                 /*  Maximum sockets open simultaneously  */
#define UNUSED_FD         -1                /*  Value for gSockfd  */
#define MAX_PROMPT_LENGTH  20               /*  Maximum length of prompt string  */
#define BUFFER_SIZE      100                /*  Size of read/write buffer  */  
#define MAX_DATA_DISPLAY 64                 /*  Bytes of incoming packets to display  */
typedef enum {READ_READY, WRITE_READY, EXCEPT_READY} readyCondition;   
                                            /*  Which condition does the fd have to be ready for?  */


/*  Global variables.  */
static int gVerbose = FALSE;                 /*  gVerbose selected on command line?  */
static int gCurrent = 0;                     /*  Index of "gCurrent" socket  */
static int gSockfd[MAXSOCKETS] = {           /*  fd of test socket  */
    UNUSED_FD, UNUSED_FD, UNUSED_FD, UNUSED_FD, UNUSED_FD,
    UNUSED_FD, UNUSED_FD, UNUSED_FD, UNUSED_FD, UNUSED_FD
};                            
static char *gTokens[MAXTOKENS];             /*  Separated command gTokens  */
static int gTokenCount;                      /*  Number of gTokens  */
enum {
    BLOCKING_MODEL, NONBLOCKING_MODEL, SELECT_MODEL, SIGNAL_MODEL
} model = BLOCKING_MODEL;                    /*  Mode in which APIs are exercised  */
static int gDomain;                          /*  domain specified when socket created  */
static int gType;                            /*  type specified when socket created  */
static int gProtocol;                        /*  protocol specified when socket created  */
static int gInterrupted;                     /*  Received an interrupt from the user  */


/*  Command definitions.  Enumeration and string table must be kept in sync.  */
enum command_enum {
    CMD_QUIT = 0,
    CMD_HELP,
    CMD_MODEL,
    CMD_USE,
    CMD_SOCKET,
    CMD_BIND,
    CMD_CONNECT,
    CMD_LISTEN,
    CMD_ACCEPT,
    CMD_RECVMSG,
    CMD_SENDMSG,
    CMD_READ,
    CMD_WRITE,
    CMD_SETSOCKOPT,
    CMD_GETSOCKOPT,
    CMD_MULTIJOIN,
    CMD_MULTILEAVE,
    CMD_SHUTDOWN,
    CMD_GETSOCKNAME,
    CMD_GETPEERNAME,
    CMD_CLOSE,
     
    NUM_COMMANDS                            /*  MUST BE AT END  */
};
static char *gCommands[] = {
    "quit",
    "help",
    "model",
    "use",
    "socket",
    "bind",
    "connect",
    "listen",
    "accept",
    "recvmsg",
    "sendmsg",
    "read",
    "write",
    "setsockopt",
    "getsockopt",
    "multijoin",
    "multileave",
    "shutdown",
    "getsockname",
    "getpeername",
    "close"
};
static char *gUsage[] = {
    "quit",
    "help",
    "model *blocking | nonblocking | select | signal",
    "use number",
    "socket [-d domain] [-t type] [-p protocol]",
    "bind portnumber [ hostaddress ]",
    "connect portnumber [ hostaddress ]",
    "listen [backlogCount]",
    "accept",
    "recvmsg [-f OOB]",
    "sendmsg [-a hostaddress port] [-f OOB]",
    "read",
    "write",
    "setsockopt level opt [-i value]",
    "getsockopt level opt [-i]",
    "multijoin interfaceIndex hostaddress",
    "multileave interfaceIndex hostaddress",
    "shutdown [SHUT_RD | SHUT_WR | SHUT_RDWR]",
    "getsockname",
    "getpeername",
    "close"
};


/*  Translate a number parameter into a binary value.  */
static int setIntegerArgument(const char *param, int *value)
{
    int temp_value;
    int result;

    result = sscanf(param, "%i", &temp_value);
    if (result != 1){
        fprintf(stderr, "%s is not a valid value.\n", param);
        return 1;
    }

    *value = temp_value;
    return 0;
}


/*  Translate a named option value into an integer.  */
static int getNamedValue(char *token, char *strings[], int values[], int *resultValue)
{
    int i;
    char *ptr;
    
    /*  Force to connonical lower case.  */
    for (i = 0; i < strlen(token); i++)
        token[i] = tolower(token[i]);
        
    /*  Search through the provided names.  */
    for (i = 0; strings[i] != NULL; i++)
        if (strcmp(token, strings[i]) == 0){
            *resultValue = values[i];
            return 0;
    }
    *resultValue = strtol(token, &ptr, 0);
    if (ptr == token){
        fprintf(stderr, "%s is not a recognized option value.\n", token);
        return -1;
    }
    
    return 0;
}


/*  Find a free socket slot.  */
static int findFreeSocketSlot()
{
    int i;
    
    /*  Find a free socket slot.  */
    for (i = 0; i < MAXSOCKETS; i++){
        if (gSockfd[i] == UNUSED_FD)
            break;
    }
    if (i >= MAXSOCKETS){
        fprintf(stderr, "All %d sockets are in use.\n", MAXSOCKETS);
        return -1;
    }
    return i;
}


/*  Set a flag on the gCurrent socket.  */
extern void setFctlFlag(int flag)
{
    int flags, result;

    flags = fcntl(gSockfd[gCurrent], F_GETFL, 0);
    if (flags < 0){
        fprintf(stderr, "Error on F_GETFL - %s.\n", strerror(errno));
        return;
    }
    flags |= flag;
    result = fcntl(gSockfd[gCurrent], F_SETFL, flags);
    if (result < 0){
        fprintf(stderr, "Error on F_SETFL - %s.\n", strerror(errno));
        return;
    }
}


/*  Clear a flag on the gCurrent socket.  */
extern void clearFctlFlag(int flag)
{
    int flags, result;

    flags = fcntl(gSockfd[gCurrent], F_GETFL, 0);
    if (flags < 0){
        fprintf(stderr, "Error on F_GETFL - %s.\n", strerror(errno));
        return;
    }
    flags &= ~flag;
    result = fcntl(gSockfd[gCurrent], F_SETFL, flags);
    if (result < 0){
        fprintf(stderr, "Error on F_SETFL - %s.\n", strerror(errno));
        return;
    }
}


#define BLOCK_THRESHOLD   1000000      /*  Delay in us. that we will intepret as a block  */

static struct timeval callTime;        /*  time at which API was called  */

/*  Setup the mechanism used to determine if an API blocked.  */
static void doBlockingSetup()
{
    gettimeofday(&callTime, NULL);
}


/*  Determine if the API just called actually blocked.  */
static void verifyBlocking(int expected)
{
    static struct timeval returnTime;
    long delta;
    int blocked;

    /* 
     *  Heuristically (i.e. buggily) determine if the API
     *  blocked by seeing how long it took to return.
     */
    gettimeofday(&returnTime, NULL);
    delta = (returnTime.tv_sec - callTime.tv_sec) * 1000000 + 
                            returnTime.tv_usec - callTime.tv_usec;
    blocked = delta > BLOCK_THRESHOLD;
    if (blocked != expected)
        fprintf(stderr, "Error - API %s block.\n", blocked ? "did" : "did not");
    else if (gVerbose)
        fprintf(stderr, "API %s block.\n", blocked ? "did" : "did not");
}


static int shouldBlock;                         /*  Flag for blocking model special case  */

/*  Do setup for before we call a socket API in blocking model.  */
static void blockingPreAPISetup(readyCondition neededCondition)
{
    shouldBlock = neededCondition != WRITE_READY;
    
    /*  Set up our blocking test.  */
    doBlockingSetup();
}


/*  Do setup for after we call a socket API in blocking model.  */
static int blockingPostAPISetup(int apiResult)
{
    int blocked;
    
    /*  Determine if we blocked in the API.  */
    verifyBlocking(shouldBlock && apiResult >= 0);   
    
    return TRUE;
}


/*  Do setup for before we call a socket API in nonblocking model.  */
static void nonblockingPreAPISetup()
{
    /*  Set up our blocking test.  */
    doBlockingSetup();
    
    /*  Enable non-blocking model on the file descriptor.  */
    setFctlFlag(O_NONBLOCK);

    if (gVerbose)
        printf("Tick.\n");
}


/*  Do setup for after we call a socket API in nonblocking model.  */
static int nonblockingPostAPISetup(int apiResult)
{
    int done, err = errno;
    
    /*  Determine if we blocked in the API.  */
    verifyBlocking(FALSE);

    /*  Disable non-blocking model on the file descriptor.  */
    clearFctlFlag(O_NONBLOCK);
    
    done = apiResult >= 0 || ( err != EWOULDBLOCK && err != EINPROGRESS && err != EALREADY);

    if (gVerbose){
        if (apiResult != 0)
            printf("API result is %d, errno is '%s'.\n", apiResult, strerror(err));
        else
            printf("API result is zero.\n", apiResult, strerror(err));
    }
    
    if (!done)
        sleep(1);

    return done;
}


/*
 *  Do setup for before we call a socket API in select model.  neededCondition indicates
 *  what condition the socket has to be ready for before we can call the API.
 */
static void selectPreAPISetup(readyCondition neededCondition)
{
    fd_set readBits, writeBits, exceptBits, *watchBits;
    int result, done = FALSE;
    struct timeval timeout;
    
    /*  Determine which set of bits we expect to change.  */
    switch (neededCondition){
        case READ_READY:    watchBits = &readBits;    break;
        case WRITE_READY:   watchBits = &writeBits;   break;
        case EXCEPT_READY:  watchBits = &exceptBits;  break;
    }
    
    /*  Loop doing the select.  */
    do {
        /*  Initialize the bit sets.  */
        FD_ZERO(&readBits);
        FD_ZERO(&writeBits);
        FD_ZERO(&exceptBits);

        FD_SET(gSockfd[gCurrent], watchBits);
   
        /*  Initialize the timeout.  */
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        /*  Do the select() and check its output.  */
        result = select(gSockfd[gCurrent]+1, &readBits, &writeBits, &exceptBits, &timeout);
        if (result == 0){
            if (gVerbose)
                printf("Tick.\n");
        } else if (result == 1){
            if (!FD_ISSET(gSockfd[gCurrent], watchBits))
                fprintf(stderr, "Error - Expected fd bit not set after select() returned 1.\n");
            else
                if (gVerbose)
                    printf("select() exited as expected.\n");
            done = TRUE;
        } else {
            fprintf(stderr, "Error - select() returned %d.\n", result);
        }
    } while (!done && !gInterrupted);

    /*  Set up our blocking test.  */
    doBlockingSetup();
}


/*  Do setup for after we call a socket API in select model.  */
static int selectPostAPISetup(int apiResult)
{
    /*  Determine if we blocked in the API.  */
    verifyBlocking(FALSE);   
    
    return TRUE;
}


/*  This SIGIO handler is used when we do not expect a SIGIO signal.  */
static void defaultSIGIOHandler(int sig)
{
    fprintf(stderr, "Error - Unexpected SIGIO signal.\n");
}


static int sigioReceived;                    /*  Flag from sig handler to main program.  */

/*  This SIGIO handler is used when we expect a SIGIO signal.  */
static void activeSIGIOHandler(int sig)
{    
    sigioReceived = TRUE;
    
    if (gVerbose)
        printf("SIGIO handler called.\n");
}


/*  
 *  Do setup for before we call a socket API in signal model.  neededCondition indicates
 *  what condition the socket has to be ready for before we can call the API.
 */ 
static void signalPreAPISetup(readyCondition neededCondition)
{
    sighandler_t sigResult;
    int result;
    
    /*  
     *  If the condition needed by this API is write, then we're done.  In signal I/O,
     *  the socket is assumed to always be ready for writing (I think).
     */
    if (neededCondition == WRITE_READY){

        /*  Set up our blocking test.  */
        doBlockingSetup();
    
        return;
    }
    
    /*  Set up signal handler and claim the socket.  */
    sigResult = signal(SIGIO, activeSIGIOHandler);
    if (sigResult == SIG_ERR){
        fprintf(stderr, "Error - signal() returned SIG_ERR.\n");
        return;
    }
    result = fcntl(gSockfd[gCurrent], F_SETOWN, getpid());
    if (result < 0){
        fprintf(stderr, "Error on F_SETOWN - %s.\n", strerror(errno));
        return;
    }
    
    /*  Enable signal model on the file descriptor.  */
    sigioReceived = FALSE;
    setFctlFlag(O_ASYNC);

    /*  Loop until the SIGIO occurs.  */
    while (!sigioReceived && !gInterrupted){
        sleep(1);
        if (gVerbose)
            printf("Tick.\n");
    }

    /*  Set up our blocking test.  */
    doBlockingSetup();
}


/*  Do setup for after we call a socket API in signal model.  */
static int signalPostAPISetup(int apiResult)
{
    sighandler_t sigResult;    
    
    /*  Determine if we blocked in the API.  */
    verifyBlocking(FALSE);   
    
    /*  Restore default SIGIO hanlder.  */
    sigResult = signal(SIGIO, defaultSIGIOHandler);
    if (sigResult == SIG_ERR){
        fprintf(stderr, "Error - signal() returned SIG_ERR.\n");
        return;
    }

    /*  Disable signal model on the file descriptor.  */
    clearFctlFlag(O_ASYNC);

    return TRUE;
}


/*
 *  Do common setup for before we call a socket API.  neededCondition indicates
 *  what condition the socket has to be ready for before we can call the API.
 */
static void preAPISetup(readyCondition neededCondition)
{
    switch (model){
        case BLOCKING_MODEL:
        default:
            blockingPreAPISetup(neededCondition);
            break;
        case NONBLOCKING_MODEL:
            nonblockingPreAPISetup();
            break;
        case SELECT_MODEL:
            selectPreAPISetup(neededCondition);
            break;
        case SIGNAL_MODEL:
            signalPreAPISetup(neededCondition);
            break;
    }
}


/*
 *  Do common setup for after we call a socket API.  apiResult is the value 
 *  returned by the API just called.
 */
static int postAPISetup(int apiResult)
{
    int done;
    
    switch (model){
        case BLOCKING_MODEL:
        default:
            done = blockingPostAPISetup(apiResult);
            break;
        case NONBLOCKING_MODEL:
            done = nonblockingPostAPISetup(apiResult);
            break;
        case SELECT_MODEL:
            done = selectPostAPISetup(apiResult);
            break;
        case SIGNAL_MODEL:
            done = signalPostAPISetup(apiResult);
            break;
    }
    
    return done;
}


/*
 *  Implement help command.
 *
 *  model 
 *
 */
static void doHelp()
{
    enum command_enum i;
    
    printf("socktest understands these gCommands:\n");
    for (i =  CMD_QUIT; i <= CMD_CLOSE; i ++)
        printf("  %s\n", gUsage[i]);
}


/*
 *  Implement model command.
 *
 *  model *blocking | nonblocking | select | signal
 *
 */
static void doModel()
{
    if (gTokens[1] == NULL)
        model = BLOCKING_MODEL;
    else if (strcmp(gTokens[1], "blocking") == 0)
        model = BLOCKING_MODEL;
    else if (strcmp(gTokens[1], "nonblocking") == 0)
        model = NONBLOCKING_MODEL;
    else if (strcmp(gTokens[1], "signal") == 0)
        model = SIGNAL_MODEL;
    else if (strcmp(gTokens[1], "select") == 0)
        model = SELECT_MODEL;
    else
        fprintf(stderr, "Unrecognized model %s\n", gTokens[1]);
}


/*
 *  Implement use command.
 *
 *  use number
 *
 */
static void doUse()
{
    int result, newgCurrent;
    
    /*  Process command line argument       */
    if (gTokenCount !=2){
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_USE]);
        return;
    }
    
    result = setIntegerArgument(gTokens[1], &newgCurrent);
    if (result != 0){
        fprintf(stderr, "Invalid socket number.\n");
        return;
    }
    
    if (gSockfd[newgCurrent] == UNUSED_FD){
        fprintf(stderr, "Socket number %d not open.\n", newgCurrent);
        return;
    }
    
    gCurrent = newgCurrent;
}


/*
 *  Implement socket command.
 *
 *  socket [-d domain] [-t type] [-p protocol]
 *
 *  domain:    number | inet | *inet6
 *  type:      number | *stream | datagram | raw
 *  protocol:  number
 */

static void doSocket()
{
    int i, newgCurrent, retval = 0;
    char option;
    static char *dStrings[] = {"inet", "inet6", NULL};
    static int dValues[] = {PF_INET, PF_INET6};
    static char *tStrings[] = {"stream", "datagram", "raw", NULL};
    static int tValues[] = {SOCK_STREAM, SOCK_DGRAM, SOCK_RAW};
    static char *pStrings[] = {NULL};
    static int pValues[] = {0};
    
    /*  Set defaults.  */
    gDomain = PF_INET6;
    gType = SOCK_STREAM;
    gProtocol = 0;
    
    /*  Process command line arguments      */
    optind = 0;
    while (retval == 0 && (option = getopt(gTokenCount, gTokens, "d:t:p:")) != -1){
        switch (option){        
            case 'd':
                retval = getNamedValue(optarg, dStrings, dValues, &gDomain);
                break;          
            case 't':
                retval = getNamedValue(optarg, tStrings, tValues, &gType);
                break;          
            case 'p':
                retval = getNamedValue(optarg, pStrings, pValues, &gProtocol);
                break;          
            default:
                fprintf(stderr, "Unknown option %c.\n", option);
                return;
        }
    }
    if (optind < gTokenCount){
        fprintf(stderr, "Unexpected argument(s) at end of command.\n");
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_SOCKET]);
        return;
    }
    if (retval){
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_SOCKET]);
        return;
    }
    
    /*  Find a free socket slot.  */
    newgCurrent = findFreeSocketSlot();
    if (newgCurrent < 0)
        return;
        
    gCurrent = newgCurrent;
    
    /*  Call the socket() API.  */
    gSockfd[gCurrent] = socket(gDomain, gType, gProtocol);
    if (gSockfd[gCurrent] < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", gSockfd[gCurrent], errno, strerror(errno));
        return;
    }   
}


/*
 *  Implement bind command.
 *
 *  bind portnumber [ hostaddress ]
 *
 */
static void doBind()
{
    int result;
    char option;
    int port;
    char buf[80];
    struct addrinfo *addrInfo;
    struct addrinfo hints = {0, 0, 0, 0, 0, NULL, NULL, NULL};
    struct sockaddr_in6 *addr, wildcard = {AF_INET6, 0, 0, IN6ADDR_ANY_INIT, 0};
    
    /*  Process command line arguments      */
    if (gTokenCount < 2 || gTokenCount > 3){
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_BIND]);
        return;
    }
    
    result = setIntegerArgument(gTokens[1], &port);
    if (result != 0){
        fprintf(stderr, "Invalid port number.\n");
        return;
    }
    
    if (gTokenCount == 3){
        /*  Translate/lookup host address/name.  */
        hints.ai_family = gDomain;
        hints.ai_socktype = gType;
        hints.ai_protocol = gProtocol;
        result = getaddrinfo(gTokens[2], NULL, &hints, &addrInfo);
        if (result){
            fprintf(stderr, "Error - %s is not a valid address:  %s.\n", gTokens[2], gai_strerror(result));
            return;
        }
        addr = (struct sockaddr_in6 *)addrInfo->ai_addr;
    }
    else {
        addr = &wildcard;
    }
    
    /*  Plug the port number into the address.  */
#ifdef SOLARIS
    assert(offsetof(struct sockaddr_in, sin_port) == offsetof(struct sockaddr_in6, sin6_port));
#endif
    addr->sin6_port = htons(port);
    
    /*  Call the bind() API.  */
    result = bind(gSockfd[gCurrent], (struct sockaddr *)addr, sizeof(*addr));
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    }   
}


/*
 *  Implement connect command.
 *
 *  connect portnumber [ hostaddress ]
 *
 */
static void doConnect()
{
    int result, done;
    char option;
    int port;
    char buf[80];
    struct addrinfo *addrInfo;
    struct addrinfo hints = {0, 0, 0, 0, 0, NULL, NULL, NULL};

    /*  Process command line arguments      */
    if (gTokenCount < 2 || gTokenCount > 3){
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_CONNECT]);
        return;
    }
    
    result = setIntegerArgument(gTokens[1], &port);
    if (result != 0){
        fprintf(stderr, "Invalid port number.\n");
        return;
    }
    
    /*  Translate/lookup host address/name.  */
    hints.ai_family = gDomain;
    hints.ai_socktype = gType;
    hints.ai_protocol = gProtocol;
    result = getaddrinfo(gTokens[2], NULL, &hints, &addrInfo);
    if (result){
        fprintf(stderr, "Error - %s is not a valid address:  %s.\n", gTokens[2], gai_strerror(result));
        return;
    }
    
    /*  Plug the port number into the address.  */
#ifdef SOLARIS
    assert(offsetof(struct sockaddr_in, sin_port) == offsetof(struct sockaddr_in6, sin6_port));
#endif
    ((struct sockaddr_in6 *)(addrInfo->ai_addr))->sin6_port = htons(port);
    
    /*  Call the connect() API.  */
    do {
        preAPISetup(READ_READY);    
        if (gInterrupted)
            return;
        result = connect(gSockfd[gCurrent], addrInfo->ai_addr, sizeof(struct sockaddr_in6));
        done = postAPISetup(result);
    } while (!done);

    
    /*  Call the connect() API.  */
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    }   
}


/*
 *  Implement listen command.
 *
 *  listen [backlogCount]
 *
 *  Backlog defaults to 1 if not specified.
 *
 */
static void doListen()
{
    int result;
    int backlog = 1;
    
    /*  Process command line arguments      */
    if (gTokenCount > 2){
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_LISTEN]);
        return;
    }
    
    if (gTokenCount == 2){
        result = setIntegerArgument(gTokens[1], &backlog);
        if (result != 0){
            fprintf(stderr, "Invalid backlog count.\n");
                return;
        }
    }

    /*  Call the listen() API.  */
    result = listen(gSockfd[gCurrent], backlog);
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    }   
}


/*
 *  Implement accept command.
 *
 *  accept 
 *
 */
static void doAccept()
{
    int result, newgCurrent;
    struct sockaddr_in6 saddr;
    socklen_t len = sizeof(saddr);
    int done;
    
    /*  Find a free socket slot.  */
    newgCurrent = findFreeSocketSlot();
    if (newgCurrent < 0)
        return;
        
    /*  Call the API.  */
    do {
        preAPISetup(READ_READY);    
        if (gInterrupted)
            return;
        result = accept(gSockfd[gCurrent], (struct sockaddr *)&saddr, &len);    
        done = postAPISetup(result);
    } while (!done);
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    }   

    /*  Update our state.  */
    gSockfd[newgCurrent] = result;
    gCurrent = newgCurrent;
}


/*
 *  Implement recvmsg command.
 *
 *  recvmsg [-f OOB]
 *
 */
static void doRecvmsg()
{
    int result;
    char option;
    int done, flags = 0;
    struct msghdr msgInfo;
    struct iovec iov;
    char buffer[BUFFER_SIZE];
    struct sockaddr_in6 saddr;
    char temp[100];
    char hexBuffer[MAX_DATA_DISPLAY*3 + 1];
    int i, bytesToDisplay, retval = 0;
    static char *fStrings[] = {"oob", NULL};
    static int fValues[] = {MSG_OOB};
    int atMark;
        
    /*  Process command line arguments      */
    optind = 0;
    while (retval == 0 && (option = getopt(gTokenCount, gTokens, "f:")) != -1){
        switch (option){        
            case 'f':
                retval = getNamedValue(optarg, fStrings, fValues, &flags);
                break;          
            default:
                fprintf(stderr, "Unknown option %c.\n", option);
                return;
        }
    }
    if (optind < gTokenCount){
        fprintf(stderr, "Unexpected argument(s) at end of command.\n");
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_RECVMSG]);
        return;
    }
    if (retval){
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_RECVMSG]);
        return;
    }

    /*  Fill in the msghdr.  */
    iov.iov_base = buffer;
    iov.iov_len = sizeof(buffer);
    msgInfo.msg_name = &saddr;
    msgInfo.msg_namelen = sizeof(saddr);
    msgInfo.msg_iov = &iov;
    msgInfo.msg_iovlen = 1;
    msgInfo.msg_control = NULL;
    msgInfo.msg_controllen = 0;
    msgInfo.msg_flags = 0;
    
    /*  Call the API.  */
    do {
        preAPISetup(READ_READY);    
        if (gInterrupted)
            return;
        result = recvmsg(gSockfd[gCurrent], &msgInfo, flags);
        done = postAPISetup(result);
    } while (!done);
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    }
    
    if (gVerbose){
        
        if (result == 0)
            printf("End of file returned.\n");
        else 
            printf("%d bytes read.\n", result);
        if (gType != SOCK_STREAM)
            fprintf(stdout, "Source address = %s.\n", inet_ntop(gDomain, &saddr.sin6_addr, 
                (char *)&temp, sizeof(temp)));              

        bytesToDisplay = MIN(result, MAX_DATA_DISPLAY);
        for (i = 0; i < bytesToDisplay; i++){
            snprintf(temp, sizeof(temp), "%.8x ", (unsigned)buffer[i]);
            strncpy(&hexBuffer[i*3], &temp[6], 3);
        }
        hexBuffer[i*3] = '\0';
        printf("First %d bytes received are: %s\n", bytesToDisplay, hexBuffer);
    }
    
    result = ioctl(gSockfd[gCurrent], SIOCATMARK, &atMark);
    if (result < 0){
        fprintf(stderr, "Error in ioctl(SIOCATMARK) call - %s.\n", strerror(errno));
        return;
    }
    if (atMark)
        fprintf(stdout, "SIOCATMARK returned true.\n");
}


/*
 *  Implement sendmsg command.
 *
 *  sendmsg [-a hostaddress port] [-f OOB]
 *
 *  ??  This is a first cut.  
 *
 */
static void doSendmsg()
{
    int i, result, done;
    char option;
    struct msghdr msgInfo;
    struct iovec iov;
    char buffer[BUFFER_SIZE];
    struct addrinfo *addrInfo;
    struct addrinfo hints = {0, 0, 0, 0, 0, NULL, NULL, NULL};
    struct sockaddr_in6 *faddr = NULL;
    int port, flags = 0, retval = 0;
    static char *fStrings[] = {"oob", NULL};
    static int fValues[] = {MSG_OOB};

    /*  Process command line arguments      */
    optind = 0;
    while (retval == 0 && (option = getopt(gTokenCount, gTokens, "a:f:")) != -1){
        switch (option){        
            case 'a':
                hints.ai_family = gDomain;
                hints.ai_socktype = gType;
                hints.ai_protocol = gProtocol;
                retval = getaddrinfo(optarg, NULL, &hints, &addrInfo);
                if (retval)
                    fprintf(stderr, "Error - %s is not a valid address:  %s.\n", optarg, gai_strerror(result));

                /*  Translate port number.  */
                retval = setIntegerArgument(gTokens[optind], &port);
                if (retval != 0)
                    fprintf(stderr, "Invalid port number.\n");
                ((struct sockaddr_in6 *)(addrInfo->ai_addr))->sin6_port = htons(port);

                faddr = (struct sockaddr_in6 *)(addrInfo->ai_addr);
                optind += 1;
                break;          
            case 'f':
                retval = getNamedValue(optarg, fStrings, fValues, &flags);
                break;          
            default:
                fprintf(stderr, "Unknown option %c.\n", option);
                return;
        }
    }
    if (optind < gTokenCount){
        fprintf(stderr, "Unexpected argument(s) at end of command.\n");
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_SENDMSG]);
        return;
    }
    if (retval){
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_SENDMSG]);
        return;
    }
    
    /*  Fill in the msghdr.  */
    iov.iov_base = buffer;
    iov.iov_len = sizeof(buffer);
    msgInfo.msg_name = faddr;
    msgInfo.msg_namelen = (faddr != NULL) ? sizeof(struct sockaddr_in6) : 0;
    msgInfo.msg_iov = &iov;
    msgInfo.msg_iovlen = 1;
    msgInfo.msg_control = NULL;
    msgInfo.msg_controllen = 0;
    msgInfo.msg_flags = 0;
    
    /*  Fill the buffer.  */
    for (i = 0; i < BUFFER_SIZE; i++)
        buffer[i] = '*';
    
    /*  Call the API.  */
    do {
        preAPISetup(WRITE_READY);   
        if (gInterrupted)
            return;
        result = sendmsg(gSockfd[gCurrent], &msgInfo, flags);   
        done = postAPISetup(result);
    } while (!done);
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    } else if (result == 0 && gVerbose)
        printf("Zero count returned.\n");
    else if (result > 0 && gVerbose)
        printf("%d bytes written.\n", result);
}


/*
 *  Implement read command.
 *
 *  read 
 *
 *  ??  This is a first cut.  Such nicities as showing what was read and giving the user 
 *  ??  options is left for another day.
 *
 */
static void doRead()
{
    int result;
    int done;
    char buffer[BUFFER_SIZE];
    char temp[100];
    char hexBuffer[MAX_DATA_DISPLAY*3 + 1];
    int i, bytesToDisplay;
    
    /*  Call the API.  */
    do {
        preAPISetup(READ_READY);    
        if (gInterrupted)
            return;
        result = read(gSockfd[gCurrent], buffer, BUFFER_SIZE);  
        done = postAPISetup(result);
    } while (!done);
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    } else if (gVerbose){
        if(result == 0)
            printf("End of file returned.\n");
        else if (result > 0)
            printf("%d bytes read.\n", result);
        
        bytesToDisplay = MIN(result, MAX_DATA_DISPLAY);
        for (i = 0; i < bytesToDisplay; i++){
            snprintf(temp, sizeof(temp), "%.8x ", (unsigned)buffer[i]);
            strncpy(&hexBuffer[i*3], &temp[6], 3);
        }
        hexBuffer[i*3] = '\0';
        printf("First %d bytes received are: %s\n", bytesToDisplay, hexBuffer);
    }
}


/*
 *  Implement write command.
 *
 *  write 
 *
 *  ??  This is a first cut.  Such nicities as allowing the user to control what is written 
 *  ??  and providing options is left for another day.
 *
 */
static void doWrite()
{
    int i, result,done;
    char buffer[BUFFER_SIZE];
    
    /*  Fill the buffer.  */
    for (i = 0; i < BUFFER_SIZE; i++)
        buffer[i] = '*';
    
    /*  Call the API.  */
    do {
        preAPISetup(WRITE_READY);   
        if (gInterrupted)
            return;
        result = write(gSockfd[gCurrent], buffer, BUFFER_SIZE); 
        done = postAPISetup(result);
    } while (!done);
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    } else if (result == 0 && gVerbose)
        printf("Zero count returned.\n");
    else if (result > 0 && gVerbose)
        printf("%d bytes written.\n", result);
}


/*
 *  Implement setsockopt command.
 *
 *  setsockopt level opt [-i value] 
 *
 */
static void doSetsockopt()
{
    int result, level, opt, intArg;
    
    /*  Process command line arguments      */
    if (gTokenCount != 5){
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_SETSOCKOPT]);
        return;
    }
    
    result = setIntegerArgument(gTokens[1], &level);
    if (result != 0){
        fprintf(stderr, "Invalid level value.\n");
            return;
    }
        
    result = setIntegerArgument(gTokens[2], &opt);
    if (result != 0){
        fprintf(stderr, "Invalid opt value.\n");
            return;
    }
        
    result = setIntegerArgument(gTokens[4], &intArg);
    if (result != 0){
        fprintf(stderr, "Invalid argument value.\n");
            return;
    }
        
    /*  Call the API.  */
    result = setsockopt(gSockfd[gCurrent], level, opt, &intArg, sizeof(int));   
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    } 
}


/*
 *  Implement getsockopt command.
 *
 *  getsockopt level opt [-i] 
 *
 */
static void doGetsockopt()
{
    int result, level, opt, optlen, intArg;
    
    /*  Process command line arguments      */
    if (gTokenCount != 4){
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_GETSOCKOPT]);
        return;
    }
    
    result = setIntegerArgument(gTokens[1], &level);
    if (result != 0){
        fprintf(stderr, "Invalid level value.\n");
            return;
    }
        
    result = setIntegerArgument(gTokens[2], &opt);
    if (result != 0){
        fprintf(stderr, "Invalid opt value.\n");
            return;
    }
    
    optlen = sizeof(int);
        
    /*  Call the API.  */
    result = getsockopt(gSockfd[gCurrent], level, opt, &intArg, &optlen);   
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    } 
    
    printf("Option value = %d, option length = %d.\n", intArg, optlen);
}


/*
 *  Implement multijoin command.
 *
 *  multijoin interfaceIndex hostaddress 
 *
 */
static void doMultijoin()
{
    struct ipv6_mreq mcSpec;
    struct addrinfo *addrInfo;
    struct addrinfo hints = {0, 0, 0, 0, 0, NULL, NULL, NULL};
    int result;
    
    /*  Process command line arguments      */
    if (gTokenCount != 3){
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_MULTIJOIN]);
        return;
    }
    
    result = setIntegerArgument(gTokens[1], &mcSpec.ipv6mr_interface);
    if (result != 0){
        fprintf(stderr, "Invalid interfaceIndex value.\n");
            return;
    }
        
    hints.ai_family = gDomain;
    hints.ai_socktype = gType;
    hints.ai_protocol = gProtocol;
    result = getaddrinfo(gTokens[2], NULL, &hints, &addrInfo);
    if (result){
        fprintf(stderr, "Error - %s is not a valid address:  %s.\n", gTokens[2], gai_strerror(result));
        return;
    }
    memcpy(&mcSpec.ipv6mr_multiaddr, &((struct sockaddr_in6 *)(addrInfo->ai_addr))->sin6_addr, sizeof(struct in6_addr));
    
    /*  Call the API.  */
    result = setsockopt(gSockfd[gCurrent], SOL_IPV6, IPV6_ADD_MEMBERSHIP, &mcSpec, sizeof(mcSpec)); 
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    } 
}


/*
 *  Implement multileave command.
 *
 *  multileave interfaceIndex hostaddress 
 *
 */
static void doMultileave()
{
    struct ipv6_mreq mcSpec;
    struct addrinfo *addrInfo;
    struct addrinfo hints = {0, 0, 0, 0, 0, NULL, NULL, NULL};
    int result;
    
    /*  Process command line arguments      */
    if (gTokenCount != 3){
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_MULTIJOIN]);
        return;
    }
    
    result = setIntegerArgument(gTokens[1], &mcSpec.ipv6mr_interface);
    if (result != 0){
        fprintf(stderr, "Invalid interfaceIndex value.\n");
            return;
    }
        
    hints.ai_family = gDomain;
    hints.ai_socktype = gType;
    hints.ai_protocol = gProtocol;
    result = getaddrinfo(gTokens[2], NULL, &hints, &addrInfo);
    if (result){
        fprintf(stderr, "Error - %s is not a valid address:  %s.\n", gTokens[2], gai_strerror(result));
        return;
    }
    memcpy(&mcSpec.ipv6mr_multiaddr, &((struct sockaddr_in6 *)(addrInfo->ai_addr))->sin6_addr, sizeof(struct in6_addr));    
    /*  Call the API.  */
    result = setsockopt(gSockfd[gCurrent], SOL_IPV6, IPV6_DROP_MEMBERSHIP, &mcSpec, sizeof(mcSpec));    
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    } 
}


/*
 *  Implement shutdown command.
 *
 *  shutdown [SHUT_RD | SHUT_WR | SHUT_RDWR]
 */
static void doShutdown()
{
    int option, result;
    static char *oStrings[] = {"shut_rd", "shut_wr", "shut_rdwr", NULL};
    static int oValues[] = {SHUT_RD, SHUT_WR, SHUT_RDWR};
            
    /*  Process command line arguments      */
    if (gTokenCount != 2){
        fprintf(stderr, "gUsage:  %s.\n", gUsage[CMD_SHUTDOWN]);
        return;
    }
    
    result = getNamedValue(gTokens[1], oStrings, oValues, &option);
    if (result != 0){
        fprintf(stderr, "Invalid shutdown option value.\n");
            return;
    }
    result = shutdown(gSockfd[gCurrent], option);
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    }   
}


/*
 *  Implement getsockname command.
 *
 *  getsockname 
 */
static void doGetsockname()
{
    int result;
    char buffer[100];
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    
    result = getsockname(gSockfd[gCurrent], (struct sockaddr *)&addr, &len);
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    }
    
    /*  Print out result.  */
    (void) inet_ntop(addr.sin6_family, &addr.sin6_addr, buffer, sizeof(buffer));
    
    printf("Address = %s, port = %d, sockaddr length = %d.\n", buffer, addr.sin6_port, len);
}


/*
 *  Implement getpeername command.
 *
 *  getpeername 
 */
static void doGetpeername()
{
    int result;
    char buffer[100];
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    
    result = getpeername(gSockfd[gCurrent], (struct sockaddr *)&addr, &len);
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    }
    
    /*  Print out result.  */
    (void) inet_ntop(addr.sin6_family, &addr.sin6_addr, buffer, sizeof(buffer));
    
    printf("Address = %s, port = %d, sockaddr length = %d.\n", buffer, addr.sin6_port, len);
}


/*
 *  Implement close command.
 *
 *  close
 *
 */
static void doClose()
{
    int result;
    
    result = close(gSockfd[gCurrent]);
    if (result < 0){
        fprintf(stderr, "API returned %d.  Error %d passed in errno - %s.\n", result, errno, strerror(errno));
        return;
    }   
    
    gSockfd[gCurrent] = UNUSED_FD;
}


static void showgUsage()
{
    fprintf(stderr, "gUsage:  socktest [-v] \n");
}


/*  Signal handler for user interrupts.  */
static void interruptSignalHandler(int sig)
{
    printf("User interrupt received.\n");
    gInterrupted = TRUE;
}


static void pipeSignalHandler(int sig)
{
    printf("Broken pipe signal received.\n");
    gInterrupted = TRUE;
}


int main(int argc, char *argv[], char *envp[])
{
    int retval = 0;
    char option;
    int i;
    char promptStr[MAX_PROMPT_LENGTH];
    char *modelStr;
    char *command, *tokenPtr;
    int done = FALSE;
    
    /*  Process command line arguments      */
    while (retval == 0 && (option = getopt(argc, argv, "v")) != -1){
        switch (option){        
            case 'v':
                gVerbose = TRUE;
                break;          
            case '?':
            default:
                showgUsage();
                return 1;
        }
    }
    if (optind < argc){
        fprintf(stderr, "Unexpected argument(s) at end of command.\n");
        return;
    }
    if (retval)
        return retval;
    
    /*  Do sanity checks on the arguments.  */
    
    /*  Initialize signal handlers.  */
    signal(SIGINT, interruptSignalHandler);
    signal(SIGTSTP, interruptSignalHandler);    
    signal(SIGPIPE, pipeSignalHandler); 
    
    /*  Process gCommands  */
    while (!done){

        /*  Build the prompt string.  */
        switch (model){
            case BLOCKING_MODEL:
            default:
                modelStr = "blocking";
                break;
            case NONBLOCKING_MODEL:
                modelStr = "nonblocking";
                break;
            case SELECT_MODEL:
                modelStr = "select";
                break;          
            case SIGNAL_MODEL:
                modelStr = "signal";
                break;
        }
        snprintf(promptStr, MAX_PROMPT_LENGTH, "%s %d:  " , modelStr, gCurrent);

        /*  Input a (non-empty) command.  */
        do 
            command = readline(promptStr);
        while (*command == 0);
	 if (command == NULL)
	 	break;
        add_history(command);
    
        /*  Force to all lower-case.  */
        for (i = 0; i < strlen(command); i++)
            command[i] = tolower(command[i]);
        
        /*  Break the string into tokens, in-place.  */
        tokenPtr = command;
        for (gTokenCount = 0; gTokenCount < MAXTOKENS; gTokenCount++){
            gTokens[gTokenCount] = strsep(&tokenPtr, CMDDELIMS);
            if (gTokens[gTokenCount] == NULL)
                break;
        }
        if (gTokenCount >= MAXTOKENS){
            fprintf(stderr, "Too many tokens in input line.\n");
            continue;
        }
#if IFY_DO_NOT_COMPILE
        printf("tokens:  \n");
        for (i = 0; gTokens[i] != NULL; i++)
            printf("  %s\n", gTokens[i]);
#endif
        
        /*  Find the command.  */
        for (i = 0; i < NUM_COMMANDS; i++){
            if (strcmp(gTokens[0], gCommands[i]) == 0)
                break;
        }
        
        /*  Dispatch to the command processor.  */
        gInterrupted = FALSE;
        switch (i){
        
            case CMD_QUIT:        done = TRUE;      break;
            case CMD_HELP:        doHelp();         break;
            case CMD_MODEL:       doModel();        break;
            case CMD_USE:         doUse();          break;
            case CMD_SOCKET:      doSocket();       break;
            case CMD_BIND:        doBind();         break;
            case CMD_CONNECT:     doConnect();      break;
            case CMD_LISTEN:      doListen();       break;
            case CMD_ACCEPT:      doAccept();       break;
            case CMD_RECVMSG:     doRecvmsg();      break;
            case CMD_SENDMSG:     doSendmsg();      break;
            case CMD_READ:        doRead();         break;
            case CMD_WRITE:       doWrite();        break;
            case CMD_SETSOCKOPT:  doSetsockopt();   break;
            case CMD_GETSOCKOPT:  doGetsockopt();   break;
            case CMD_MULTIJOIN:   doMultijoin();    break;
            case CMD_MULTILEAVE:  doMultileave();   break;
            case CMD_GETSOCKNAME: doGetsockname();  break;
            case CMD_GETPEERNAME: doGetpeername();  break;
            case CMD_SHUTDOWN:    doShutdown();     break;
            case CMD_CLOSE:       doClose();        break;
             
             default:
                fprintf(stderr, "Unrecognized command.\n");
                continue;
        }
        
        free(command);
    }
    
    return 0;
}

