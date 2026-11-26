
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>


const char* DATA_CACHE_FILIE = "/var/tmp/aesdsocketdata";
static volatile sig_atomic_t kill_requested = false;


/*** PACKET HANDLING ***/
struct PartialPacket
{
    int length;
    char* data;
    struct PartialPacket* next;
};

struct FullPacket
{
    int totalBytes;
    struct PartialPacket* firstPacket;
    struct PartialPacket* lastPacket;
    bool complete;
};

struct FullPacket* FullPacket_New()
{
    struct FullPacket* newPacket = malloc(sizeof(struct FullPacket));
    if(newPacket != NULL)
    {
        newPacket->complete = false;
        newPacket->firstPacket = NULL;
        newPacket->totalBytes = 0;
    }
    return newPacket;
}

void FullPacket_Free(struct FullPacket* packet)
{
    if(packet == NULL)
    {
        return;
    }

    if(packet->firstPacket != NULL)
    {
        struct PartialPacket* nextPacket = packet->firstPacket;
        while(nextPacket != NULL)
        {
            struct PartialPacket* thisPacket = nextPacket;
            nextPacket = nextPacket->next;
            if(thisPacket->data != NULL)
            {
                free(thisPacket->data);
            }
            free(thisPacket);
        }
    }

    free(packet);
}

int FullPacket_AddData(struct FullPacket* packet, const char* data, int size)
{
    if(packet == NULL)
    {
        return -1;
    }

    if(packet->complete)
    {
        return 0;
    }

    if(size == 0 || data == NULL)
    {
        return 0;
    }
    
    struct PartialPacket* newPartial = malloc(sizeof(struct PartialPacket));
    if(newPartial == NULL)
    {
        return -1;
    }

    // Check if newline character exists, if so, 
    // we have reached the end of a full packet,
    // and we only copy the data up to the end.
    // It is up to the caller to then use
    // the rest of the data on a new packet.
    int newLineIdx = -1;
    for(int idx = 0; idx < size; idx++)
    {
        if(data[idx] == '\n')
        {
            newLineIdx = idx;
            break;
        }
    }
    int dataToCopy = size;
    if(newLineIdx >= 0)
    {
        dataToCopy = newLineIdx + 1;
    }

    char* newData = malloc(dataToCopy);
    if(newData == NULL)
    {
        free(newPartial);
        return -1;
    }

    memcpy(newData, data, dataToCopy);
    newPartial->data = newData;
    newPartial->length = dataToCopy;
    newPartial->next = NULL;
    
    if(packet->firstPacket == NULL)
    {
        packet->firstPacket = newPartial;
        packet->lastPacket = newPartial;
    }
    else
    {
        packet->lastPacket->next = newPartial;
        packet->lastPacket = newPartial;
    }

    if(newLineIdx >= 0)
    {
        packet->complete = true;
        printf("Complete packet received\n");
    }

    return dataToCopy;
}

int FullPacket_AppendToFile(struct FullPacket* packet, const char* path)
{
    if(packet == NULL)
    {
        return -1;
    }

    if(packet->complete == false)
    {
        return -1;
    }

    FILE* file = fopen(path, "a");
    if(file == NULL)
    {
        const char* errorDescription = strerror(errno);
        if(errorDescription != NULL)
        {
            printf("Unable to open file: %s\n", errorDescription);
        }
        return errno;
    }

    struct PartialPacket* partial = packet->firstPacket;
    while(partial != NULL)
    {
        int written = fwrite(partial->data, 1, partial->length, file);
        if(written != partial->length)
        {
            printf("Unexpected amount of bytes written. Should be %d, but only %d were written.\n", partial->length, written);
            fclose(file);
            return -1;
        }
        partial = partial->next;
    }

    fclose(file);
    return 0;
}

/*** CONNECTION HANDLING ***/

int send_data_from_file(const char* path, int socketFd)
{
    FILE* file = fopen(path, "r");
    if(file == NULL)
    {
        return errno;
    }

    char buffer[10000];
    bool eof = false;

    while(eof == false)
    {
        memset(buffer, 0, sizeof(buffer));
        size_t bytesRead = fread(buffer, 1, sizeof(buffer), file);
        if(ferror(file))
        {
            fclose(file);
            return -1;
        }

        if(bytesRead < sizeof(buffer))
        {
            eof = true;
        }

        if(bytesRead > 0)
        {
            int totalBytesSent = 0;
            while(totalBytesSent < bytesRead)
            {
                printf("Sending: %s\n", buffer);
                int bytesSent = send(socketFd, &buffer[totalBytesSent], bytesRead, 0);
                if(bytesSent < 0)
                {
                    fclose(file);
                    return errno;
                }
                totalBytesSent += bytesSent;
            }
        }
    }

    fclose(file);
    return 0;
}

struct Connection
{
    pthread_t threadId;
    sig_atomic_t connectedFd;
    sig_atomic_t active;
    pthread_mutex_t* fileMutex;
    int threadReturn;
    struct Connection* previous;
};

static void* Connection_ThreadEntry(void* arg)
{
    struct Connection* connectionInfo = arg;
    struct FullPacket* packet = NULL;
    int ret = 0;
    int res = 0;
    
    
    if(connectionInfo == NULL)
    {
        pthread_exit(NULL);
    }
    
    pthread_mutex_t* fileMutex = connectionInfo->fileMutex;
    int connectedFd = connectionInfo->connectedFd;
    connectionInfo->threadReturn = 0;
    
    printf("Connection thread for connection %d started\n", connectedFd);

    packet = FullPacket_New();
    if(packet == NULL)
    {
        connectionInfo->threadReturn = 1;
        pthread_exit(&connectionInfo->threadReturn);
    }

    bool connected = true;
    while(connected && connectionInfo->active)
    {
        char buffer[1024] = {0};

        int bytesReceived = recv(connectedFd, buffer, sizeof(buffer), 0);
        if(bytesReceived == 0)
        {
            printf("Connection closed...\n");
            connected = false;
        }
        else if(bytesReceived < 0)
        {
            printf("Error receiving data...\n");
            connected = false;
        }
        else
        {
            res = pthread_mutex_lock(fileMutex);
            if(res != 0)
            {
                ret = 1;
                printf("Error locking mutex\n");
                goto cleanup_exit;
            }
            printf("%d bytes received: %.*s\n", bytesReceived, bytesReceived, buffer);
            int offset = 0;
            while(bytesReceived > 0)
            {
                int dataAdded = FullPacket_AddData(packet, &buffer[offset], bytesReceived);
                if(dataAdded < 0)
                {
                    printf("Error adding data to packet (most likely out of memory)\n");
                    ret = 1;
                    goto cleanup_exit;
                }

                if(packet->complete == true)
                {
                    // Dump to file and start with a new packet
                    res = FullPacket_AppendToFile(packet, DATA_CACHE_FILIE);
                    if(res != 0)
                    {
                        ret = 1;
                        printf("Error appending to file\n");
                        goto cleanup_exit;
                    }

                    FullPacket_Free(packet);
                    packet = FullPacket_New();
                    if(packet == NULL)
                    {
                        ret = 1;
                        printf("Unable to create new packet (probably out of memory)\n");
                        goto cleanup_exit;
                    }

                    // Send data back
                    res = send_data_from_file(DATA_CACHE_FILIE, connectedFd);
                    if(res != 0)
                    {
                        ret = 1;
                        printf("Unable to send data\n");
                        goto cleanup_exit;
                    }
                }

                bytesReceived -= dataAdded;
                offset += dataAdded;
            }
            pthread_mutex_unlock(fileMutex);
        }
        if(kill_requested)
        {
            connected = false;
        }
    }

    cleanup_exit:
    connectionInfo->active = false;
    FullPacket_Free(packet);
    connectionInfo->threadReturn = ret;
    pthread_mutex_unlock(fileMutex);
    pthread_exit(&connectionInfo->threadReturn);
}

struct Connection* Connection_Add(struct Connection* head, int fd, pthread_mutex_t* fileMutex)
{
    struct Connection* newConnection = malloc(sizeof(struct Connection));
    if(newConnection == NULL)
    {
        return NULL;
    }

    newConnection->active = true;
    newConnection->fileMutex = fileMutex;
    newConnection->connectedFd = fd;
    newConnection->previous = head;
    newConnection->threadReturn = 0;

    int ret = pthread_create(&newConnection->threadId, NULL, Connection_ThreadEntry, newConnection);
    if(ret != 0)
    {
        free(newConnection);
        return NULL;
    }

    return newConnection;
}

struct Connection* Connection_RemoveInactive(struct Connection* head)
{
    struct Connection* current = head;
    struct Connection* newHead = head;
    struct Connection* next = NULL;
    while(current != NULL)
    {
        struct Connection* previous = current->previous;
        if(current->connectedFd == false || current->active == false)
        {
            if(newHead == current)
            {
                newHead = previous;
            }
            if(next != NULL)
            {
                next->previous = previous;
            }
            pthread_join(current->threadId, NULL);
            free(current);
        }
        else
        {
            next = current;
        }
        current = previous;
    }

    return newHead;
}

void Connection_Cleanup(struct Connection* head)
{
    struct Connection* current = head;
    while(current != NULL)
    {
        struct Connection* next = current->previous;
        if(current->active == true)
        {
            current->active = false;
            pthread_join(current->threadId, NULL);
        }
        free(current);
        current = next;
    }
}

/*** GENERIC CODE ****/
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


static void signal_handler(int signal)
{
    (void)signal; // unused
    kill_requested = true;
}

static void timer_event_handler(union sigval eventInfo)
{
    pthread_mutex_t* fileMutex = eventInfo.sival_ptr;
    char timeStamp[128];

    memset(timeStamp, 0, sizeof(timeStamp));

    struct timespec ts;
    struct tm tm_info;
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
        perror("clock_gettime");
        return;
    }

    if (localtime_r(&ts.tv_sec, &tm_info) == NULL) {
        perror("localtime_r");
        return;
    }

    size_t stringSize = strftime(timeStamp, sizeof(timeStamp), "timestamp: %a, %d %b %Y %H:%M:%S %z\n", &tm_info);
    if (stringSize == 0) {
        fprintf(stderr, "strftime failed\n");
        return;
    }

    printf("Writing timestamp to file: %s\n", timeStamp);

    if(pthread_mutex_lock(fileMutex) != 0)
    {
        fprintf(stderr, "Error locking mutex\n");
        return;
    }

    FILE* file = fopen(DATA_CACHE_FILIE, "a");
    if(file == NULL)
    {
        const char* errorDescription = strerror(errno);
        if(errorDescription != NULL)
        {
            printf("Unable to open file: %s\n", errorDescription);
        }
        pthread_mutex_unlock(fileMutex);
        return;
    }

    int written = fwrite(timeStamp, 1, stringSize, file);
    if(written != stringSize)
    {
        printf("Unexpected amount of bytes written. Should be %ld, but only %d were written.\n", stringSize, written);
    }
    pthread_mutex_unlock(fileMutex);
    fclose(file);

}

int main(int argc, char** argv)
{
    bool runDaemon = false;
    int programResult = 0;
    struct addrinfo hints;
    struct addrinfo* infoResult = NULL;
    struct Connection* connectionHead = NULL;
    int res = 0;
    int connectedFd = -1;
    pthread_mutex_t fileMutex;

        syslog(LOG_INFO, "Launching\n");
    printf("Data cache file path: %s\n", DATA_CACHE_FILIE);

    if(argc > 2)
    {
        printf("Error: Only one optional parameter supported.\n");
        printf("Supported options:\n");
        printf("    -d: Launches the application as a daemon.\n");
        return 1;
    }

    if(argc == 2)
    {
        char* argument = argv[1];
        if(strcmp("-d", argument) != 0)
        {
            printf("Error: Uknown option: %s\n", argument);
            printf("Supported options:\n");
            printf("    -d: Launches the application as a daemon.\n");
            return 1;
        }
        else
        {
            runDaemon = true;
        }
    }

    if(runDaemon)
    {
        int pid = fork();
        if(pid < 0)
        {
            printf("Error creating deamon: %d\n", errno);
            programResult = 8;
            goto cleanup_exit;
        }
        else if(pid > 0)
        {
            printf("Deamon running with PID %d\n", pid);
            // exit(0);
            programResult = 0;
            goto cleanup_exit;
        }

        // PID == 0 from here on; running as child process
        chdir("/");
        fflush(stdout); // Flush any buffered output before redirecting

        // Open /dev/null for writing
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null == -1) {
            perror("open");
            programResult = 8;
            goto cleanup_exit;
        }

        // Replace stdout (fd 1) with /dev/null
        if (dup2(dev_null, STDOUT_FILENO) == -1) {
            perror("dup2");
            close(dev_null);
            programResult = 8;
            goto cleanup_exit;
        }

        // Replace stderr (fd 2) with /dev/null
        if (dup2(dev_null, STDERR_FILENO) == -1) {
            perror("dup2");
            close(dev_null);
            programResult = 8;
            goto cleanup_exit;
        }

        // Close the extra file descriptor
        close(dev_null);
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_flags = 0;
    action.sa_restorer = NULL;
    action.sa_handler = signal_handler;
    res = sigaction(SIGTERM, &action, NULL);
    if(res != 0)
    {
        printf("Error: Unable to set signal handler: %d\n", errno);
        return 1;
    }
    res = sigaction(SIGINT, &action, NULL);
    if(res != 0)
    {
        printf("Error: Unable to set signal handler: %d\n", errno);
        return 1;
    }

    res = pthread_mutex_init(&fileMutex, NULL);
    if(res < 0)
    {
        return 1;
    }

    // Start and setup timer
    struct sigevent event;
    event.sigev_notify = SIGEV_THREAD;
    event.sigev_notify_function = timer_event_handler;
    event.sigev_notify_attributes = NULL;
    event.sigev_value.sival_ptr = &fileMutex;
    timer_t timerId;
    res = timer_create(CLOCK_MONOTONIC, &event, &timerId);
    if(res < 0)
    {
        return 1;
    }
    struct itimerspec timerSpec;
    timerSpec.it_interval.tv_nsec = 0;
    timerSpec.it_interval.tv_sec = 10;
    timerSpec.it_value.tv_nsec = 0;
    timerSpec.it_value.tv_sec = 10;
    res = timer_settime(timerId, 0, &timerSpec, NULL);
    if(res < 0)
    {
        return 1;
    }

    res = remove(DATA_CACHE_FILIE);
    if(res == 0)
    {
        printf("Data cache file detected and deleted to start anew...\n");
    }
    else if(errno == ENOENT)
    {
        printf("Data cache file does not exist yet, do not need to delete...\n");
    }
    else
    {
        printf("Unable to delete or verify data cache file existence: %d", errno);
        return errno;
    }
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    res = getaddrinfo(NULL, "9000", &hints, &infoResult);
    if(res != 0 || infoResult == NULL)
    {
        printf("Error getting address: %d\n", res);
        programResult = 1;
        goto cleanup_exit;
    }
    
    int socketFd = socket(infoResult->ai_family, infoResult->ai_socktype, infoResult->ai_protocol);
    if(socketFd < 0)
    {
        printf("Error getting socket: %d\n", errno);
        programResult = 2;
        goto cleanup_exit;
    }

    res = fcntl(socketFd, F_SETFL, fcntl(socketFd, F_GETFL) | O_NONBLOCK);
    if(res < 0)
    {
        printf("Error setting socket as non blocking: %d\n", errno);
        programResult = 2;
        goto cleanup_exit;
    }

    int yes=1;
    res = setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    if(res != 0)
    {
        printf("Error setting socket option: %d\n", errno);
        programResult = 3;
        goto cleanup_exit;
    }

    res = bind(socketFd, infoResult->ai_addr, infoResult->ai_addrlen);
    if(res < 0)
    {
        printf("Error binding socket: %d\n", errno);
        programResult = 3;
        goto cleanup_exit;
    }

    bool running = kill_requested ? false : true;

    while(running)
    {
        printf("Listening for new connections...\n");
        res = listen(socketFd, 1);
        if(res < 0)
        {
            printf("Error listening: %d\n", errno);
            programResult = 4;
            goto cleanup_exit;
        }
        
        struct sockaddr_storage connectedAddr;
        socklen_t addrSize = sizeof(connectedAddr);
        bool acceptedConnection = false;
        while((acceptedConnection == false) && (running == true))
        {
            connectedFd = accept(socketFd, (struct sockaddr *)&connectedAddr, &addrSize);
            if(connectedFd < 0)
            {
                if(errno == EAGAIN)
                {
                    connectionHead = Connection_RemoveInactive(connectionHead);
                }
                else
                {
                    printf("Error accepting connection: %d\n", errno);
                }
                if(kill_requested)
                {
                    running = false;
                }
            }
            else
            {
                acceptedConnection = true;
            }
        }

        if(kill_requested)
        {
            continue;
        }

        char addrString[INET6_ADDRSTRLEN];
        inet_ntop(connectedAddr.ss_family,
            get_in_addr((struct sockaddr *)&connectedAddr),
            addrString, sizeof(addrString));
        printf("Connection established from: %s on connection file %d \n", addrString, connectedFd);
        syslog(LOG_INFO, "Accepted connection from %s\n", addrString);

        connectionHead = Connection_Add(connectionHead, connectedFd, &fileMutex);
        if(connectionHead == NULL)
        {
            printf("Unable to start connection thread\n");
            programResult = 5;
            goto cleanup_exit;
        }

        connectionHead = Connection_RemoveInactive(connectionHead);
    }

    cleanup_exit:
    printf("Ending server\n");
    freeaddrinfo(infoResult);
    close(connectedFd);
    pthread_mutex_destroy(&fileMutex);
    Connection_Cleanup(connectionHead);

    return programResult;
}
