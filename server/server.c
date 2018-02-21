#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>


#ifdef  __x86_64__
	#define	REG_INSTR_PTR	16
#else
	#define REG_INSTR_PTR	14
#endif


/*********************************************************************************************************************/

#define SERVER_PID_FILE				"/var/run/testServerMonitor.pid"
#define SERVER_SLEEP_TIME			1									/* 1 second */
#define THREAD_WAIT_TIME			1   								/* 1 second */
#define SERVER_TRY_COUNT			3


#define	SERVER_NEED_RESTART			1
#define SERVER_NO_NEED_RESTART 		0
#define SERVER_START_FAILED			(-1)
#define SERVER_RELOAD_CONF_FAILED	(-2)
#define SERVER_CONFIG_FILE_FAILED	(-3)


#define SERVER_DEFAULT_PORT			59000
#define SERVER_DEFAULT_IPADDR		0
#define SERVER_DEFAULT_CLIENT_COUNT	1024

/*********************************************************************************************************************/

/* info about file */
typedef struct 
{
    unsigned int fSize;        /* file size */
    int fTime;                 /* file creation time */
    int fNameLength;           /* file name length */
    int fBlockLength;          /* file data length */
    int fFileType;             /* file type */
} FILE_INFO;


/* structure for the data for new thread */
typedef struct
{
    int sock;      /* socket */
    int addr;      /* ip address */
} CLIENT_INFO;


typedef struct
{
	pthread_t thrDesc;
	int	inUse;
} THREAD_INFO;

/*********************************************************************************************************************/

static CLIENT_INFO clientInfo[SERVER_DEFAULT_CLIENT_COUNT];
static THREAD_INFO clientThreads[SERVER_DEFAULT_CLIENT_COUNT];

static int maxClientCount;
static unsigned short serverPort;
static struct in_addr ipAddr;

static int listenSock;					/* socket for incoming connections from clients */
static pthread_t mainWorkThread;		/* descriptor of main thread of server for listen on the socket */
static char confFileName[4096];			/* for configuration file name */
static unsigned int threadNum = 0;		/* counter of clients threads */

/*********************************************************************************************************************/

static int serverProc(int* lSock);
static void servClient(void *param);
static int findFreeThreadDescriptor(int count, THREAD_INFO* tInf);
static int getListeningSocket(int ipAddr, short tcpPort);
static void waitAllThreadsComplit(THREAD_INFO* tInf);
static int serverMonitor();
static int loadConfig(const char* configFile);
static int setPidFile(const char* pidFileName);
static int startServer();
static int setFdLimit(int maxFd);
static void signalError(int sig, siginfo_t *si, void* ptr);
static int reloadConfig();





/*********************************************************************************************************************/

/* main function - start serverMonitor */
int main(int argc, char* argv[])
{
    if(argc != 2)
    {
    	printf("ERROR: invalid arguments count\n");
    	return -1;
    }

    if(loadConfig(argv[1]) < 0)
    {
    	printf("ERROR: no configuration file specified\n");
    	return -1;
    }

    if(daemon(1, 1) < 0)
    {
    	printf("ERROR: server monitor demonization failed, errno %d\n", errno);
    	return -1;
    }

    return serverMonitor();
}


/*********************************************************************************************************************/

/* load configuration^
 * PORT_NO - number of TCP port of server
 * IP_ADDR - IP address from which connection will be received
 * MAX_CLEINTS - maximum simultaneous number of clients */
static int loadConfig(const char* configFile)
{
	char paramStr[128];
	FILE* configFd = fopen(configFile, "r");

	if(NULL == configFd)
	{
		printf("ERROR: can't open configuration file");
		return SERVER_CONFIG_FILE_FAILED;
	}

	/* save config file name */
	strcpy(confFileName, configFile);

	/* parse config file */
	while(!feof(configFd) && fgets((char*)paramStr, 127, configFd))
	{
		if(strstr(paramStr, "PORT_NO="))
		{
			if(0 >= sscanf(paramStr + strlen("PORT_NO="), "%hu", &serverPort))
			{
				printf("WARNING: can't read server port number\n");
				serverPort = SERVER_DEFAULT_PORT;
			}
		}
		else if (strstr(paramStr, "IP_ADDR="))
		{
			char* ptr = NULL;

			/* replace '\n' to '\0' */
			if((ptr = (char*)memchr(paramStr, '\n', strlen(paramStr))) != NULL)
			{
				*ptr = 0;
			}

			if(0 >= inet_pton(AF_INET, (const char*)(paramStr + strlen("IP_ADDR=")), (void*)&ipAddr))
			{
				printf("WARNING: can't convert ip address to network order\n");
				ipAddr.s_addr = SERVER_DEFAULT_IPADDR;
			}
		}
		else if(strstr(paramStr, "MAX_CLIENTS="))
		{
			if(0 >= sscanf(paramStr + strlen("MAX_CLIENTS="), "%d", &maxClientCount))
			{
				printf("WARNING: can't read max client count\n");
				maxClientCount = SERVER_DEFAULT_CLIENT_COUNT;
			}

			if (SERVER_DEFAULT_CLIENT_COUNT < maxClientCount)
			{
				maxClientCount = SERVER_DEFAULT_CLIENT_COUNT;
			}
		}
	}

	fclose(configFd);
	return 0;
}


/*********************************************************************************************************************/

/* process for server monitoring */
static int serverMonitor()
{
	int res;
	int serverPid = 0;
	sigset_t sigset;
	siginfo_t siginfo;
	int doRestart = 1;
	int cycle = 1;

	openlog("testServerMonitor", LOG_NDELAY | LOG_PID, LOG_USER);
	syslog(LOG_INFO, "[monitor] Server monitor is running\n");

	if(0 != setPidFile(SERVER_PID_FILE))
	{
		syslog(LOG_ERR, "[monitor] Can't set pid file for server monitor\n");
		return -1;
	}

	/* set signals */
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGQUIT);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGTERM);
	sigaddset(&sigset, SIGCHLD);
	sigaddset(&sigset, SIGUSR1);
	sigprocmask(SIG_BLOCK, &sigset, NULL);

	/* fork() for creating server process */
	while(cycle)
	{
		if(doRestart)
		{
			serverPid = fork();
			doRestart = 0;
		}

		if(serverPid < 0)
		{
			syslog(LOG_ERR, "[monitor] Can't fork server monitor\n");
			return -1;
		}
		else if (0 == serverPid)  /* child */
		{
			res = startServer();
			return res;
		}
		else					  /* parent */
		{
			sigwaitinfo(&sigset, &siginfo);

			switch (siginfo.si_signo)
			{
				/* signal from a child */
				case SIGCHLD:
				{
					wait(&res);

					res = WEXITSTATUS(res);

					switch (res)
					{
						case SERVER_NEED_RESTART:
							doRestart = 1;
							syslog(LOG_INFO, "[monitor] server restart\n");
							break;

						case SERVER_NO_NEED_RESTART:
							cycle = 0;
							syslog(LOG_INFO, "[monitor] server stopped\n");
							break;

						case SERVER_START_FAILED:
							cycle = 0;
							syslog(LOG_INFO, "[monitor] server start failed\n");
							break;

						case SERVER_RELOAD_CONF_FAILED:
							cycle = 0;
							syslog(LOG_INFO, "[monitor] server reload configuration failed\n");
							break;

						default:
							cycle = 0;
							syslog(LOG_WARNING, "[monitor] unknown server exit value\n");
							break;
					}
				}
				break;

				/* reconfig server */
				case SIGUSR1:
					kill(serverPid , SIGUSR1);
					doRestart = 0;
					break;

				default:
					syslog(LOG_INFO, "[monitor] signal %s, server terminated\n", strsignal(siginfo.si_signo));
					kill(serverPid, SIGTERM);
					res = 0;
					break;
			}
		}
	}

	syslog(LOG_ERR, "[monitor] stop\n");

	unlink(SERVER_PID_FILE);

	return res;
}


/*********************************************************************************************************************/

/* function tries to create pid-file correctly */
static int setPidFile(const char* pidFileName)
{
	int pidFile;
	int fileExist = 0;
	int res = 0;
	char buf[20];

	if(!access(pidFileName, F_OK))
	{
		fileExist = 1;
	}

	pidFile = open(pidFileName, O_RDWR | O_CREAT);
	if(pidFile < 0)
	{
		syslog(LOG_ERR, "[monitor] Can't open pid file, errno %u\n", errno);
		return -1;
	}

	if(flock(pidFile, LOCK_EX) < 0)
	{
		syslog(LOG_ERR, "[monitor] can't lock pid file\n");
		if(close(pidFile) < 0)
		{
			syslog(LOG_ERR, "[monitor] can't close pid file\n");
		}
		return -1;
	}

	/* file already exist */
	if(fileExist)
	{
		if(read(pidFile, buf, sizeof(buf) - 1) > 0)
		{
			int oldPid = atoi(buf);

			if(!kill(oldPid, 0))
			{
				syslog(LOG_ERR, "[monitor] server monitor already exists\n");
				res = -1;
			}
			else
			{
				if(ftruncate(pidFile, 0) < 0)
				{
					syslog(LOG_ERR, "[monitor] can't truncate pid file\n");
					res = -1;
				}
				else
				{
					sprintf(buf, "%u", getpid());
					if (write(pidFile, buf, strlen(buf)) != strlen(buf))
					{
						syslog(LOG_ERR, "[monitor] can't write pid of server monitor to  pid file\n");
						res = -1;
					}
				}
			}
		}
		else
		{
			syslog(LOG_ERR, "[monitor] can't read pid file\n");
			res = -1;
		}
	}
	else
	{
		sprintf(buf, "%u", getpid());
		if (write(pidFile, buf, strlen(buf)) != strlen(buf))
		{
			syslog(LOG_ERR, "[monitor] can't write pid of server monitor to  pid file\n");
			res = -1;
		}
	}

	if (flock(pidFile, LOCK_UN) < 0)
	{
		syslog(LOG_ERR, "[monitor] can't unlock pid file\n");
	}

	if (close(pidFile) < 0)
	{
		syslog(LOG_ERR, "[monitor] can't close pid file\n");
	}

	return res;
}


/*********************************************************************************************************************/

/* the server */
static int startServer()
{
	struct sigaction sigact;
	sigset_t sigset;
	int signo;
	int res;

	closelog();
	openlog("testServer", LOG_NDELAY | LOG_PID, LOG_USER);

	/* error processing */
	sigact.sa_flags = SA_SIGINFO;
	sigact.sa_sigaction = signalError;
	sigemptyset(&sigact.sa_mask);
	sigaction(SIGFPE, &sigact, 0);
	sigaction(SIGILL, &sigact, 0);
	sigaction(SIGSEGV, &sigact, 0);
	sigaction(SIGBUS, &sigact, 0);

	/* сигналы которые будем ждать */
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGQUIT);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGTERM);
	sigaddset(&sigset, SIGUSR1);
	sigprocmask(SIG_BLOCK, &sigset, NULL);


	if(setFdLimit(maxClientCount + 1) < 0)
	{
		syslog(LOG_ERR, "[server] can't set limit for file descriptors\n");
		closelog();
		return SERVER_START_FAILED;
	}

	syslog(LOG_INFO, "[server] server started\n");

	if((listenSock = getListeningSocket((int)ipAddr.s_addr, htons(serverPort))) < 0)
	{
		syslog(LOG_ERR, "[server] can't get listen socket\n");
		closelog();
		return SERVER_START_FAILED;
	}


    if ((NULL == clientInfo) || (NULL == clientThreads))
    {
    	syslog(LOG_ERR, "[server] can't allocate memory\n");
    	return -1;
    }

	/* start first thread with listening socket */
	if (!pthread_create(&mainWorkThread, NULL, (void*)serverProc, (void*)&listenSock))
	{
		while (1)
		{
			sigwait(&sigset, &signo);

			syslog(LOG_ERR, "[server] receive signal #%d\n", signo);

			/*  reload config */
			if(SIGUSR1 == signo)
			{
				syslog(LOG_ERR, "[server] from SIGUSR1 branch\n");
				res =  reloadConfig();

				if (SERVER_NO_NEED_RESTART == res)
				{
					syslog(LOG_INFO, "[server] reload configuration OK!\n");
				}
				else if (SERVER_RELOAD_CONF_FAILED == res)
				{
					syslog(LOG_ERR, "[server] reload configuration FAIL!\n");
					res = SERVER_RELOAD_CONF_FAILED;
					break;
				} /* SERVER_CONFIG_FILE_FAILED */
				else
				{
					syslog(LOG_ERR, "[server] failed open or parse configuration file!\n");
				}
			}
			else
			{
				/* stop endless loop */
				break;
			}
		}

		/* stop all threads */
		pthread_cancel(mainWorkThread);
	    waitAllThreadsComplit(clientThreads);
	}
	else
	{
		syslog(LOG_ERR, "[server] create first thread failed\n");
		res = SERVER_START_FAILED;
	}

	syslog(LOG_DEBUG, "[server] before return\n");
	closelog();
	close(listenSock);

	if (NULL != clientInfo)
	{
		free(clientInfo);
	}

	if(NULL != clientThreads)
	{
		free(clientThreads);
	}

	return SERVER_NO_NEED_RESTART;
}


/*********************************************************************************************************************/

static int reloadConfig()
{
	int clientCount 	= maxClientCount;
	int newClientCount = 0;
	unsigned short port = serverPort;
	struct in_addr iA 	= ipAddr;

	if(loadConfig(confFileName) < 0)
	{
		maxClientCount = clientCount;
		serverPort = port;
		ipAddr = iA;
		return SERVER_CONFIG_FILE_FAILED;
	}

	/* close socket, stop all threads of server */
	close(listenSock);

	pthread_cancel(mainWorkThread);

	/* waitAllThreadsComplit() must be called with old value of maxClientCount */
	newClientCount = maxClientCount;
	maxClientCount = clientCount;
    waitAllThreadsComplit(clientThreads);
	maxClientCount = newClientCount;

	/*clientThreads = realloc((void*)clientThreads, maxClientCount * sizeof(THREAD_INFO));*/
	if (NULL == clientThreads)
	{
		return SERVER_RELOAD_CONF_FAILED;
	}

	/*clInfo = realloc((void*)clInfo, maxClientCount * sizeof(CLIENT_INFO));*/
	if (NULL == clientInfo)
	{
		return SERVER_RELOAD_CONF_FAILED;
	}

	if(setFdLimit(maxClientCount + 1) < 0)
	{
		syslog(LOG_ERR, "[server] can't set limit for file descriptors\n");
		return SERVER_RELOAD_CONF_FAILED;
	}

	if((listenSock = getListeningSocket((int)ipAddr.s_addr, htons(serverPort))) < 0)
	{
		syslog(LOG_ERR, "[server] can't get listen socket\n");
		return SERVER_RELOAD_CONF_FAILED;
	}

	if (pthread_create(&mainWorkThread, NULL, (void*)serverProc, (void*)&listenSock))
	{
		syslog(LOG_ERR, "[server] create mainThread failed\n");
		return SERVER_RELOAD_CONF_FAILED;
	}

	return 0;
}


/*********************************************************************************************************************/

static int setFdLimit(int maxFd)
{
	struct rlimit curLim;
	int res;

	if(getrlimit(RLIMIT_NOFILE, &curLim))
	{
		syslog(LOG_ERR, "[server] can't get file descriptors limit\n");
		res = -1;
	}
	else if ((maxFd > curLim.rlim_cur) || (maxFd > curLim.rlim_max))
	{
		curLim.rlim_cur = maxFd;
		curLim.rlim_max = maxFd;

		res = setrlimit(RLIMIT_NOFILE, &curLim);
	}

	return res;
}


/*********************************************************************************************************************/

static int getListeningSocket(int ipAddr, short tcpPort)
{
	struct sockaddr_in addr;
	int sock;

    /* open socket */
    if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        syslog(LOG_ERR, "[server] Can't open socket\n");
        return -1;
    }
    
    /* bind socket */
    addr.sin_family = AF_INET;
    addr.sin_port = tcpPort;
    addr.sin_addr.s_addr = ipAddr;
    
    syslog(LOG_ERR, "[server] tcpPort 0x%4x, ipaddr 0x%08x\n", tcpPort, ipAddr);

    if(bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        syslog(LOG_ERR, "[server] Can't bind socket\n");
        close(sock);
        return -1;
    }
    
    /* listen of the socket */
    if(listen(sock, maxClientCount) < 0)
    {
        syslog(LOG_ERR, "[server] listen error %d\n", errno);
        close(sock);
        return -1;
    }

    return sock;
}


/*********************************************************************************************************************/

/* main work thread of server */
static int serverProc(int* listSock)
{
    struct sockaddr_in addr;
    unsigned int len = 0;
    int counter = 0;
    int lSock = *listSock;
    int i;

    /* for pthread_cancle() */
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);

    if ((NULL == clientInfo) || (NULL == clientThreads))
    {
    	syslog(LOG_ERR, "[server_main_thread] no allocating memory\n");
    	return -1;
    }
    
    /* endless loop, accept client requests */
    while (1)
    {
    	for (i = 0; i < SERVER_TRY_COUNT; i++)
    	{
			if((counter = findFreeThreadDescriptor(counter, clientThreads)) < 0)
			{
				syslog(LOG_WARNING, "[server_main_thread] Can't get thread descriptor\n");
				sleep(SERVER_SLEEP_TIME);
				continue;
			}
			break;
    	}

    	if (counter < 0)
    	{
            syslog (LOG_ERR, "[server_main_thread] Can't get free thread descriptor\n", errno);
            break;
    	}

        memset(&addr, 0, sizeof(addr));

        if ((clientInfo[counter].sock = accept(lSock, (struct sockaddr*)&addr, &len)) < 0)
        {
            syslog(LOG_ERR, "[server_main_thread] Error accept(), errno %d\n", errno);
            break;
        }  
        else
        {
			clientInfo[counter].addr = addr.sin_addr.s_addr;
			int status = pthread_create(&clientThreads[counter].thrDesc, NULL, (void*)servClient, (void*)&clientInfo[counter]);

			if (!status)
			{
				syslog(LOG_INFO, "[server_main_thread] New thread create\n");
				clientThreads[counter].inUse = 1;
			}
			else
			{
				syslog(LOG_ERR, "[server_main_thread] Can't create new thread\n");
				clientThreads[counter].inUse = 0;
				close(clientInfo[counter].sock);
			}
        }
    }

    waitAllThreadsComplit(clientThreads);
    return 0;
}


/*********************************************************************************************************************/

/* function for request processing from the client */
static void servClient(void* param)
{
    int size = 0;
    int nByte = 0;
    int total = 0;
    char fileName[40];
    FILE* fDesc = NULL;
    CLIENT_INFO* cInf = (CLIENT_INFO*)param;
    unsigned int localNum = ++threadNum;

    /* for pthread_cancle() */
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);

    syslog(LOG_INFO, "[server_work_thread] I am new thread\n");
    
    inet_ntop(AF_INET, &cInf->addr, fileName, INET_ADDRSTRLEN);
    sprintf(fileName, "%s_%u_%s", fileName, localNum, "files.txt");
    
    fDesc = fopen(fileName, "w");
    if(fDesc == NULL)
    {
        syslog(LOG_ERR, "[server_work_thread] Can't open file %s\n", fileName);
        close(cInf->sock);
        return;
    }
    
    while (1)
    {
        nByte = recv(cInf->sock, &size, sizeof(size), 0);
        if(nByte < 0)
        {
            syslog(LOG_ERR, "[server_work_thread] Can't receive data from client errno %d\n", errno);
            close(cInf->sock);
            fclose(fDesc);
            return; 
        }
        
        size -= 4;

        /* end of transmission */
        if(size <= 0)
        {
            syslog(LOG_INFO,"[server_work_thread] All data receive\n");
            close(cInf->sock);
            fclose(fDesc);
            return;
        }
        else
        {
            char* tmpBuf = NULL;
            char *buff = malloc(size);
            syslog(LOG_ERR, "[server] malloc() %d bytes\n", size);

            if(buff == NULL)
            {
                syslog(LOG_ERR, "[server_work_thread] malloc error\n");
                close(cInf->sock);
                fclose(fDesc);
                return;
            }
            
            total = 0;
            
            while(total < size)
            {
                nByte = recv(cInf->sock, buff + total, size - total, 0);
                
                if(nByte < 0)
                {
                    syslog(LOG_ERR, "[server_work_thread] Can't receive data from client\n");
                    free(buff);
                    syslog(LOG_ERR, "[server] free() %d bytes\n", size);
                    close(cInf->sock);
                    fclose(fDesc);
                    return;
                }
                
                total += nByte;
            }
            
            tmpBuf = buff;

            while(total > 0)
            {
                FILE_INFO* fInf = (FILE_INFO*)tmpBuf;
                char* fileType;

                switch(fInf->fFileType)
                {
                	case S_IFREG:
                		fileType = "Regular file";
                		break;

                	case S_IFBLK:
                		fileType = "Block device";
                		break;

                	case S_IFCHR:
                		fileType = "Character device";
                		break;

                	case S_IFIFO:
                		fileType = "FIFO pipe";
                		break;

                	case S_IFLNK:
                		fileType = "Soft link";
                		break;

                	case S_IFSOCK:
                		fileType = "Socket";
                		break;

                	case S_IFDIR:
                		fileType = "Directory";
                		break;

                	default:
                		fileType = "Unknown";
                }


                fprintf(fDesc, "File name %s\n", tmpBuf + sizeof(FILE_INFO) + fInf->fBlockLength);
                fprintf(fDesc, "File size %d\n", fInf->fSize);
                fprintf(fDesc, "File type %s\n", fileType);
                fprintf(fDesc, "File create time %s", ctime((const time_t*)&fInf->fTime));
                fprintf(fDesc, "File data length %d\n", fInf->fBlockLength);
                fwrite(tmpBuf + sizeof(FILE_INFO), 1, fInf->fBlockLength - 1, fDesc);
                fprintf(fDesc, "\n\n");
                
                syslog(LOG_INFO, "[server_work_thread] Write to %s info", fileName);
                
                tmpBuf += sizeof(FILE_INFO) + fInf->fBlockLength + fInf->fNameLength;
                total -= sizeof(FILE_INFO) + fInf->fBlockLength + fInf->fNameLength;
            }
            
            free(buff);
            syslog(LOG_ERR, "[server] free() %d bytes\n", size);
        }
    }

    fclose(fDesc);
}


/*********************************************************************************************************************/

/* find free thread descriptor in static array clientThreads[] */
static int findFreeThreadDescriptor(int count, THREAD_INFO* tInf)
{
    int i;
    int num = SERVER_TRY_COUNT;        		/* search no more than 3 times */
    static int overFlag = 0;

    if(count >= (maxClientCount - 1) || count < 0)
    {
        count = 0;
        overFlag = 1;
    }

	if(!tInf[count].inUse)
	{
		return count;
	}

    if(!overFlag)
    {
    	return ++count;
    }

    while(num--)
    {
        for(i = count; i < maxClientCount; ++i)
        {
            if(pthread_tryjoin_np(tInf[i].thrDesc, NULL) == 0)
            {
                syslog(LOG_INFO, "[server_main_thread] Find free pthread_t descriptor %d\n", count);
                return i;
            }
        }
        
        usleep(3000);
        count = 0;
    }
    
    return -1;
}


/*********************************************************************************************************************/

/* completion of all threads with a limited wait time */
static void waitAllThreadsComplit(THREAD_INFO* tInf)
{
	int i;
	struct timespec timesp;

	if(NULL != tInf)
	{
		/* join the thread for 1 second, if unsuccess - send SIGKILL */
		for(i = 0; i < maxClientCount; ++i)
		{
			clock_gettime(CLOCK_REALTIME, &timesp);
			timesp.tv_sec += THREAD_WAIT_TIME;

			if (tInf[i].inUse && pthread_timedjoin_np(tInf[i].thrDesc, NULL, &timesp))
			{
				pthread_cancel(tInf[i].thrDesc);
			}
		}
	}

	return;
}


/*********************************************************************************************************************/

/* error handling */
static void signalError(int sig, siginfo_t *si, void* ptr)
{
	void* errorAddr = NULL;
	void* trace[16];
	int traceSize = 0;
	char **errorMsg = NULL;

	syslog(LOG_ERR, "[server] signal %s, addr 0x%0.16X\n", strsignal(sig), (int)si->si_addr);

	errorAddr = (void*)((ucontext_t*)ptr)->uc_mcontext.gregs[REG_INSTR_PTR];

	traceSize = backtrace(trace, 16);
	trace[1] = errorAddr;

	errorMsg = backtrace_symbols(trace, traceSize);

	if(errorMsg)
	{
		int i;
		syslog(LOG_ERR, "[server] backtrace start\n");

		for(i = 1; i < traceSize; i++)
		{
			syslog(LOG_ERR, "[server] %s\n", errorMsg[i]);
		}

		syslog(LOG_ERR, "[server] backtrace end\n");

		free(errorMsg);
	}

	/* stop all threads */
	close(listenSock);
	pthread_kill(mainWorkThread, SIGKILL);
    waitAllThreadsComplit(clientThreads);

    /* free memory */
	if (NULL != clientInfo)
	{
		free(clientInfo);
	}

	if(NULL != clientThreads)
	{
		free(clientThreads);
	}

	exit(SERVER_NEED_RESTART);
}

