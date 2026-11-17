
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


void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int send_data_from_file(const char* path, int socketFd)
{
    FILE* file = fopen(path, "r");
    if(file == NULL)
    {
        return errno;
    }

    char buffer[1024];
    bool eof = false;

    while(eof == false)
    {
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

static volatile sig_atomic_t kill_requested = false;
static void signal_handler(int signal)
{
    (void)signal; // unused
    kill_requested = true;
}

const char* DATA_CACHE_FILIE = "/var/tmp/aesdsocketdata";
int main(int argc, char** argv)
{
    bool runDaemon = false;
    struct FullPacket* packet = NULL;
    int programResult = 0;
    struct addrinfo hints;
    struct addrinfo* infoResult = NULL;
    int res = 0;
    int connectedFd = -1;

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
            exit(0);
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
            return 1;
        }

        // Replace stdout (fd 1) with /dev/null
        if (dup2(dev_null, STDOUT_FILENO) == -1) {
            perror("dup2");
            close(dev_null);
            return 1;
        }

        // Replace stderr (fd 2) with /dev/null
        if (dup2(dev_null, STDERR_FILENO) == -1) {
            perror("dup2");
            close(dev_null);
            return 1;
        }

        // Close the extra file descriptor
        close(dev_null);
    }
    
    bool running = kill_requested ? false : true;
    packet = FullPacket_New();
    if(packet == NULL)
    {
        programResult = 4;
        goto cleanup_exit;
    }
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
        connectedFd = accept(socketFd, (struct sockaddr *)&connectedAddr, &addrSize);
        if(connectedFd < 0)
        {
            if(kill_requested)
            {
                running = false;
            }
            else
            {
                printf("Error accepting connection: %d\n", errno);
            }
            continue;
        }

        char addrString[INET6_ADDRSTRLEN];
        inet_ntop(connectedAddr.ss_family,
            get_in_addr((struct sockaddr *)&connectedAddr),
            addrString, sizeof(addrString));
        printf("Connection established from: %s \n", addrString);
        syslog(LOG_INFO, "Accepted connection from %s\n", addrString);

        bool connected = true;
        while(connected)
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
                printf("%d bytes received: %.*s\n", bytesReceived, bytesReceived, buffer);
                int offset = 0;
                while(bytesReceived > 0)
                {
                    int dataAdded = FullPacket_AddData(packet, &buffer[offset], bytesReceived);
                    if(dataAdded < 0)
                    {
                        printf("Error adding data to packet\n");
                        programResult = 5;
                        goto cleanup_exit;
                    }

                    if(packet->complete == true)
                    {
                        // Dump to file and start with a new packet
                        res = FullPacket_AppendToFile(packet, DATA_CACHE_FILIE);
                        if(res != 0)
                        {
                            programResult = 6;
                            goto cleanup_exit;
                        }

                        FullPacket_Free(packet);
                        packet = FullPacket_New();
                        if(packet == NULL)
                        {
                            programResult = 4;
                            goto cleanup_exit;
                        }

                        // Send data back
                        res = send_data_from_file(DATA_CACHE_FILIE, connectedFd);
                        if(res != 0)
                        {
                            programResult = 7;
                            goto cleanup_exit;
                        }

                    }

                    bytesReceived -= dataAdded;
                    offset += dataAdded;
                }
            }
            if(kill_requested)
            {
                connected = false;
            }
        }
        if(kill_requested)
        {
            running = false;
        }
    }

    cleanup_exit:
    printf("Ending server\n");
    freeaddrinfo(infoResult);
    FullPacket_Free(packet);
    close(connectedFd);


    return programResult;
}
