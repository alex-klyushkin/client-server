#include <stdio.h>
#include <syslog.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <dirent.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>


#define SERVER_PORT     59000
#define ARG_COUNT       2
#define BLOCK_SIZE      32

/*********************************************************************************************************************/

/* for directory path */
static unsigned char processedPath[PATH_MAX + 1];
/* buffer for send to socket. First 4 bytes always contain length of data for send */
static unsigned char sockBuf[8192];
/* current data size in buffer */
static int curBufSize = sizeof(int);
/* current pointer to first byte after data in buffer */
static unsigned char* curBufPtr = sockBuf + sizeof(int);
static int clientSoc;

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

/*********************************************************************************************************************/

static int processDirectory(char *path);							/* process directories */
static int proccessFileInfo(struct stat *st, char* path);			/* process files */
static int sendData();
static int getClientSocket(char* ipAddr);



/*********************************************************************************************************************/

int main(int argc, char* argv[])
{
    int res;
    
    openlog("testClient", LOG_NDELAY | LOG_PID, LOG_USER);
    syslog(LOG_INFO, "Client is running\n");
    
    /* check arguments number */
    if(argc < ARG_COUNT)
    {
        syslog(LOG_ERR, "Invalid number of arguments\n");
        closelog();
        exit(1);
    }
    
    if(getClientSocket(argv[1]) < 0)
    {
    	closelog();
    	exit(1);
    }

    if(argc == 3)
    {
        strcpy(processedPath, argv[2]);
    }
    else
    {
        getcwd(processedPath, sizeof(processedPath));
    }

    syslog(LOG_INFO, "Processed directory: %s\n", processedPath);
    
    res = processDirectory(processedPath);
    
    /* if buffet not empty - send data */
    if(curBufSize > sizeof(int))
    {
        res = sendData();
    }
    
    /* end transmission */
    curBufSize = 4;
    *(int*)sockBuf = 0;
    res = sendData();
    
    close(clientSoc);
    closelog();

    return res;
}


/*********************************************************************************************************************/

static int getClientSocket(char* ipAddr)
{
	struct sockaddr_in serverAddr;

    /* create TCP connection */
    if((clientSoc = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        syslog(LOG_ERR, "Can't open TCP socket\n");
        return -1;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    serverAddr.sin_addr.s_addr = inet_addr(ipAddr);

    if(connect(clientSoc, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0)
    {
        syslog(LOG_ERR, "Can't connect to server\n");
        close(clientSoc);
        return -1;
    }

    return 0;
}


/*********************************************************************************************************************/

/* recursive processing of directories */
static int processDirectory(char* path)
{
    struct stat   statBuf;
    struct dirent *dirp;
    DIR           *dp;
    char          *ptr;
    int           res = 0;

    if(lstat(path, &statBuf) < 0)
    {
        syslog(LOG_ERR, "lstat error %d for %s\n", errno, path);
        return -1;
    }

    /* file, process it */
    if(S_ISDIR(statBuf.st_mode) == 0)
    {
        return(proccessFileInfo(&statBuf, path));
    }

    /* directories, process it */
    ptr = path + strlen(path);
    *ptr++ = '/';
    *ptr = 0;

    if((dp = opendir(path)) == NULL)
    {
        syslog(LOG_ERR, "Can't open directory %s\n", path);
        return -1;
    }
    
    syslog(LOG_INFO, "Open directory %s\n", path);

    /* processing of all file and directories in current directory */
    while((dirp = readdir(dp)) != NULL && res >= 0)
    {
        if((strcmp(dirp->d_name, ".") == 0) || (strcmp(dirp->d_name, "..") == 0))
        {
            continue;
        }

        strcpy(ptr, dirp->d_name);
        res = processDirectory(path);
    }

    ptr[-1] = 0;
    if(closedir(dp) < 0)
    {
        syslog(LOG_ERR, "Can't close directory %s\n", path);
        res = -1;
    }

    return res;
}


/*********************************************************************************************************************/

/* process files */
static int proccessFileInfo(struct stat *st, char* path)
{
    int res = 0;
    FILE_INFO fInfo;
    int fileType = st->st_mode & S_IFMT;
    FILE *fileDesc = NULL;
    int readByte;

    switch(fileType)
    {
        case S_IFREG:
        {
            fInfo.fSize = st->st_size;
            fInfo.fNameLength = strlen(path) + 1;
            fInfo.fFileType = fileType;

            if(st->st_size)
            {
            	fInfo.fBlockLength = (st->st_size > BLOCK_SIZE)? BLOCK_SIZE + 1: st->st_size / 2 + 2;
            }
            else
            {
            	fInfo.fBlockLength = 0;
            }

            fInfo.fTime = st->st_ctime;
            
            /* if there is no enough space in the buffer - send data */
            if((sizeof(sockBuf) - curBufSize) < (sizeof(FILE_INFO) + fInfo.fNameLength + fInfo.fBlockLength))
            {
                res = sendData();
            }
            
            /* open file, read data */
            if((fileDesc = fopen(path, "rb")) != NULL)
            {
                memcpy((void*)curBufPtr, (const void*)&fInfo, sizeof(FILE_INFO));
                curBufPtr += sizeof(FILE_INFO);
                
                fseek(fileDesc, -(fInfo.fBlockLength), SEEK_END);
                
                if((readByte = fread(curBufPtr, 1, fInfo.fBlockLength - 1, fileDesc)) <= 0)
                {
                    syslog(LOG_ERR, "Can't read data from file %s\n", path);
                    curBufPtr -= sizeof(FILE_INFO);
                    res = -1;
                }
                else
                {
                    curBufPtr += fInfo.fBlockLength - 1;
                    *curBufPtr = 0;
                    curBufPtr++;
                    strcpy(curBufPtr, path);
                    curBufPtr += fInfo.fNameLength;
                    curBufSize += sizeof(FILE_INFO) + fInfo.fNameLength + fInfo.fBlockLength;
                    
                    syslog(LOG_INFO, "Prepare for send data about regular file %s\n", path);
                }
            }
            else
            {
                syslog(LOG_ERR, "Can't open file %s\n", path);
                res = -1;
            }

            break;
        }
        
        /* if nonregular file */
        case S_IFBLK:
        case S_IFCHR:
        case S_IFIFO:
        case S_IFLNK:
        case S_IFSOCK:
        {
        	/* if there is no enough space in the buffer - send data */
            if((sizeof(sockBuf) - curBufSize) < sizeof(FILE_INFO))
            {
                res = sendData();
            }
            
            fInfo.fSize = 0;
            fInfo.fNameLength = strlen(path) + 1;
            fInfo.fFileType = fileType;
            fInfo.fBlockLength = 0;
            fInfo.fTime = st->st_ctime;
            
            memcpy((void*)curBufPtr, (const void*)&fInfo, sizeof(FILE_INFO));
            curBufPtr += sizeof(FILE_INFO);
            strcpy(curBufPtr, path);
            curBufPtr += fInfo.fNameLength;
            curBufSize += sizeof(FILE_INFO) + fInfo.fNameLength;
            
            syslog(LOG_INFO, "Prepare for send data about nonregular file %s\n", path);
            break;
        }
        
        default:
        {
            syslog(LOG_ERR, "Invalid type %d of file %s\n", fileType, path);
            res = -1;
        }
    }

    return res;
}


/*********************************************************************************************************************/

static int sendData()
{
    int total = 0;
    int byteCount = 0;
    
    syslog(LOG_INFO, "From sendData()\n");
    
    /* to the beginning of the buffer write the length of the data sent */
    *(int*)sockBuf = curBufSize;
    
    while(total < curBufSize)
    {
        byteCount = send(clientSoc, sockBuf, curBufSize, 0);

        if(byteCount < 0)
        {
            syslog(LOG_ERR, "Can't send data to socket\n");
            return -1;
        }
        else
        {
            syslog(LOG_INFO, "Send %d byte\n", byteCount);
        }
        
        total += byteCount;
    }
    
    return 0;
}


