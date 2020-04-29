#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/time.h>

typedef struct HTTPCache {
    char* key;
    char* data;
    int death;
    int age;
} HTTPcache;

struct serverInfo {  
    int client_fd;  
    char ip[100];  
    int port;
    int mainfd;  
}; 

struct rateLimit {
    char* ip;
    int secondAccess;
    int minuteAccess;
    struct rateLimit* next;
};

struct bannedIP {
    char* ip;
    int releaseTime;
    struct bannedIP* next;
};

HTTPcache* cache[10];
pthread_mutex_t mutex, bwmutex;
int currentBandwidth;
int networkLimit;
int backoffTime = 200000;

int updateBW(int bytes) {
    currentBandwidth += bytes;
    if(currentBandwidth > networkLimit) {
        backoffTime = backoffTime*(1.2);
        return backoffTime;
    }
    else {
        backoffTime = 200000;
        return 0;
    }
}

void removeIP(struct bannedIP** ips, char* ip) {
    
    struct bannedIP* temp = *ips, *prev; 
  
    if (temp != NULL && !strcmp(temp->ip, ip)) 
    { 
        *ips = temp->next;  
        free(temp->ip); 
        free(temp);               
        return; 
    } 
  
    while (temp != NULL && strcmp(temp->ip, ip)) 
    { 
        prev = temp; 
        temp = temp->next; 
    } 
  
    if (temp == NULL) return; 
  
    prev->next = temp->next; 
  
    free(temp); 
}

int isBanned(char* ip, struct bannedIP* ips) {
    struct bannedIP* current = ips;

    while (current != NULL) {
        if(!strcmp(current->ip, ip)) {
            if(time(NULL) < current->releaseTime)
                return 1;
            else {
                removeIP(&ips, current->ip);
                return 0;
            }
        }
        current = current->next;
    }
    return 0;
}

void banIP(char* ip, struct bannedIP** ips) {
    struct bannedIP* newNode = (struct bannedIP*) malloc(sizeof(struct bannedIP)); 
   
    newNode->ip = malloc(strlen(ip));
    newNode->releaseTime = time(NULL) + 10;
    strcpy(newNode->ip, ip); 
   
    newNode->next = (*ips); 
   
    (*ips) = newNode; 

    fprintf(stderr, "time: %d\n", newNode->releaseTime);
}

void tooManyRequests(int fd, char* ip, int ban, struct bannedIP** ips) {
    char* buffer = malloc(1000);
    bzero(buffer, 1000);
    int n = read(fd, buffer, 1000);
    char* temp = "HTTP/1.1 429 Too Many Requests\r\nContent-Type: text/html\r\nRetry-After: 3600\r\n\r\n";
    if(write(fd, temp, 78) < 0)
        fprintf(stderr, "ERROR writing too socket\n");
    if(!ban)
        banIP(ip, ips);
    free(buffer);
}

void incrementBWLimit() {
    pthread_mutex_lock(&bwmutex);
    currentBandwidth -= networkLimit;
    if(currentBandwidth < 0)
        currentBandwidth = 0;
    pthread_mutex_unlock(&bwmutex);
}

struct rateLimit* removeBucket(struct rateLimit** bucket, char* ip) {
    
    struct rateLimit* temp = *bucket, *prev; 
  
    if (temp != NULL && !strcmp(temp->ip, ip)) 
    { 
        *bucket = temp->next; 
        free(temp->ip);  
        free(temp);               
        return *bucket;
    } 
  
    while (temp != NULL) 
    { 
        prev = temp; 
        temp = temp->next;
        if(!strcmp(temp->ip, ip)) {
            prev->next = temp->next; 
            free(temp->ip);
            free(temp); 
            temp = NULL;
            return prev->next;
        }
    } 

    return NULL;
}

void incrementBucket(struct rateLimit** bucket, int minute) {
    struct rateLimit* temp = *bucket;
    while(temp != NULL) {
        temp->secondAccess -= 20;
        if(minute)
            temp->minuteAccess -= 300;
        if(temp->secondAccess < 0)
            temp->secondAccess = 0;
        if(temp->minuteAccess < 0)
            temp->minuteAccess = 0;
        if((temp->secondAccess == 0) && (temp->minuteAccess == 0))
            temp = removeBucket(bucket, temp->ip);
        else 
            temp = temp->next;
    }
}

int updateBucket(struct rateLimit** bucket, char* ip) {
    int exists = 0;
    struct rateLimit* temp = *bucket;

    while(temp != NULL) {
        if(!strcmp(ip, temp->ip)) {
            temp->secondAccess++;
            temp->minuteAccess++;
            exists = 1;
            if((temp->secondAccess > 20) || (temp->minuteAccess > 300)) {
                fprintf(stderr, "You accessed too much\n");
                return 1;
            }
            return 0;
        }
        temp = temp->next;
    }

    struct rateLimit* newNode = malloc(sizeof(struct rateLimit));
    newNode->ip = malloc(strlen(ip)+1);
    strcpy(newNode->ip, ip);
    newNode->secondAccess = 1;
    newNode->minuteAccess = 1;
    newNode->next = (*bucket);
    (*bucket) = newNode;

    return 0;
}

int existsInCache(char* key) {

    for(int i = 0; i < 10; i++) {
        if((cache[i]->key) && (!strcmp(key, cache[i]->key)) 
            && (cache[i]->age <= time(NULL))) {
            return i;
        }
    }
    return -1;
} 

int insertAgeToHeader(char* header, int age, int size) {

    char* body = malloc(10000000);
    char* temp = malloc(sizeof(int));
    bzero(body, 10000000);
    int headerLength = strstr(header, "\r\n\r\n") - header;

    //Change the age to a string so that it can be added
    sprintf(temp, "%d", age);
    memcpy(body, strstr(header, "\r\n\r\n"), (size-headerLength));

    header[headerLength] = '\0';
    strcat(header, "\r\nAge: \0");
    strcat(header, temp);

    int lengthAdded = 7 + strlen(temp);

    strcat(header, body);
    free(body);
    free(temp);
    return lengthAdded;
}

void updateTTL(char* buffer, int update, HTTPcache** cache, char* key) {

    char* timeToLive;
    timeToLive = strstr(buffer, "Cache-Control: max-age=");

    if(timeToLive != NULL) {
        timeToLive += 23;
        cache[update]->death = atoi(timeToLive) + time(NULL);
    } else {
        cache[update]->death = 3600 + time(NULL);
    }

    if(cache[update]->key) {
        free(cache[update]->key);
    }

    cache[update]->key = malloc(100);
    memcpy(cache[update]->key, key, strlen(key)+1);
} 

int cacheData(char* buffer, HTTPcache** cache, int size, char* key) {
    int LRU = cache[0]->death, smallest = 0;

    for(int i = 0; i < 10; i++) {

        if((cache[i]->key) && (!strcmp(key, cache[i]->key))) {
            if(cache[i]->death < time(NULL)) {
                smallest = i;
                cache[i]->age = time(NULL);
                free(cache[i]->data);
                break;
            } else {
                pthread_mutex_unlock(&mutex);
                return -1;
            }
        }

        if(cache[i]->death < LRU) {
            LRU = cache[i]->death;
            smallest = i;
        }
    }

    if(cache[smallest]->age == 0)
        cache[smallest]->age = time(NULL);

    cache[smallest]->data = malloc(size+1);
    memcpy(cache[smallest]->data, buffer, size);

    return smallest;
}

int transferGetData(int serverfd, int clientfd, int n, HTTPcache** cache, char* key) {

    int bytesRead = 0, headerSize = 0, bytesLeft = 0, chunked = 0, m = 0;
    char* buffer = malloc(10000000);

    n = read(serverfd, buffer, 10000000);
    bytesRead = n;
    buffer[n] = '\0';

    if(strstr(buffer, "HTTP/1.1 304 Not Modified") != NULL) {
        int m = write(clientfd, buffer, n);
        free(buffer);
        return -1;
    }

    char* temp = strdup(buffer);
    char* contentLength = strstr(temp, "Content-Length:");
    if(contentLength == NULL){
        chunked = 1;
        contentLength = strstr(buffer, "Transfer-Encoding: chunked")+30;
        headerSize = strstr(buffer, "\r\n\r\n") - buffer + 4;
        bytesLeft = strtol(strtok(strdup(contentLength), "\n"), NULL, 16) - n + headerSize;
    } else {
        contentLength += 15;
        headerSize = strstr(buffer, "\r\n\r\n") - buffer + 4;
        bytesLeft = atoi(strtok(contentLength, "\n")) - n + headerSize;
    }

    if(networkLimit > 0) {
        pthread_mutex_lock(&bwmutex);
        int backoffTime = updateBW(n);
        pthread_mutex_unlock(&bwmutex);
        if(backoffTime) 
        usleep(backoffTime);
    }   

    while(bytesLeft > 0) {
        if((n = read(serverfd, buffer+bytesRead, 10000000-bytesRead)) < 0)
            fprintf(stderr, "ERROR reading from socket\n");

        if(!strcmp(buffer+bytesRead, "0\r\n\r\n") && chunked) {
            bytesRead += n;
            break;
        }
        bytesRead += n;
        bytesLeft -= n;
    }

    if((m = existsInCache(key)) >= 0) {
        insertAgeToHeader(buffer, time(NULL)-cache[m]->age, bytesRead);
    } else {
        insertAgeToHeader(buffer, 0, bytesRead);
    }

    if(networkLimit > 0) {
        pthread_mutex_lock(&bwmutex);
        backoffTime = updateBW(bytesRead);
        pthread_mutex_unlock(&bwmutex);
        if(backoffTime) 
        usleep(backoffTime);
    }

    int bytesWritten = 0;
    if((bytesWritten = write(clientfd, buffer, bytesRead+9) < 0)) {
        fprintf(stderr, "ERROR writing to socket\n");
    }

    pthread_mutex_lock(&mutex);
    int cacheNum = cacheData(buffer, cache, bytesRead, key);

    free(temp);
    free(buffer);

    return cacheNum;
}

void transferConnectData(int serverfd, int clientfd, struct serverInfo *info) {

    char* buffer = malloc(10000001);
    bzero(buffer, 10000001);
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    int totalTraffic = 0;

    fd_set fds, fd_copy;
    int sockfd = info->mainfd, selectVal, n = 0, m = 9;
    FD_ZERO(&fds);
    FD_ZERO(&fd_copy);
    FD_SET(sockfd, &fds);
    FD_SET(sockfd, &fd_copy);
    FD_SET(clientfd, &fds);
    FD_SET(clientfd, &fd_copy);
    FD_SET(serverfd, &fds);
    FD_SET(serverfd, &fd_copy);
    int httpsDone = 0, backoffTime = 0;

    while(1) {
        FD_ZERO(&fds);
        for(int j = 0; j < FD_SETSIZE; j++) {
            if(FD_ISSET(j, &fd_copy)) {
                FD_SET(j, &fds);
            }
        }
        selectVal = select(FD_SETSIZE, &fds, NULL, NULL, &timeout);
        if(selectVal < 0) {
            fprintf(stderr, "Error with select\n");
            exit(1);
        } else {
            for (int i = 0; i <= serverfd; ++i) {
                if (FD_ISSET (i, &fds)) { 
                    if(i == clientfd) {
                        n = read(clientfd, buffer, 10000001);
                        if(n == 0) {
                            httpsDone++;
                            continue;
                        }
                        else
                            httpsDone = 0;
                        if(networkLimit > 0) {
                            pthread_mutex_lock(&bwmutex);
                            backoffTime = updateBW(n);
                            pthread_mutex_unlock(&bwmutex);
                            if(backoffTime) 
                                usleep(backoffTime);
                        }
                        totalTraffic += n;
                        m = write(serverfd, buffer, n);
                        totalTraffic += m;
                    } else if(i == serverfd) {
                        n = read(serverfd, buffer, 10000001);
                        if(n == 0) {
                            httpsDone++;
                            continue;
                        }
                        else if(n < 0) {
                            perror("Error printed by perror");
                            exit(1);
                        } 
                        else 
                            httpsDone = 0;
                        if(networkLimit > 0) {
                            pthread_mutex_lock(&bwmutex);
                            backoffTime = updateBW(n);
                            pthread_mutex_unlock(&bwmutex);
                            if(backoffTime) 
                                usleep(backoffTime);
                        }
                        totalTraffic += n;
                        if((m = write(clientfd, buffer, n)) < 0)
                            fprintf(stderr, "Write error.\n");
                        totalTraffic += m;
                    }
                }
            }
            if(httpsDone >= 10) {
                break;
            }
        }
    }

    free(buffer);
}

void *serverSocket(void* clientInfo) {

    struct serverInfo *info = (struct serverInfo *)clientInfo;
    char* buffer = malloc(10000000);
    int bytes = 0, serverfd = 0;
    struct sockaddr_in sadd;
    struct hostent* server;
    int servPort, n = 0, contentChunk = 0, conn = 0, backoffTime = 0;

    int clientfd = info->client_fd;

    serverfd = socket(AF_INET, SOCK_STREAM, 0);

    if((n = read(clientfd,buffer,10000000)) < 0) {
            printf("ERROR reading from socket");
            return NULL;
    }

    if(n == 0) {
        free(buffer);
        close(serverfd);
        close(clientfd);
        return NULL;
    } 

    char* intactBuffer = malloc(10000000);
    memcpy(intactBuffer, buffer, n);
    intactBuffer[n] = '\0';
    char* hostname = malloc(100);
    char* key = malloc(100);
    char* token;
    if(buffer[0] == 'G' || buffer[0] == 'P') {
        if(buffer[0] == 'G')
            token = strtok(buffer+4, " ");
        else 
            token = strtok(buffer+5, " ");
        strcpy(key, token);
        token = strtok(NULL, " ");
        token = strtok(NULL, "\r");
        strcpy(hostname, token);

        token = strtok(hostname, ":");
        token = strtok(NULL, " ");
        if(token != NULL) {
            servPort = atoi(token);
        } else {
            servPort = 80;
        } 
    } else if(buffer[0] == 'C') {
        token = strtok(buffer+8, " ");
        token = strtok(token, ":");
        strcpy(key, token);
        strcpy(hostname, token);
        servPort = 443;
        conn = 1;
    }

    server = gethostbyname(hostname);
    if (server == NULL) {
        char* errormsg = "Could not resolve given host\n";
        if((!conn) && (write(serverfd, errormsg, strlen(errormsg)) < 0))
            fprintf(stderr, "ERROR writing to socket");
    } else {
        bzero((char *) &sadd, sizeof(sadd));
        sadd.sin_family = AF_INET;
        bcopy((char *)server->h_addr, 
          (char *)&sadd.sin_addr.s_addr, server->h_length);
        sadd.sin_port = htons(servPort);

        if (connect(serverfd, (const struct sockaddr *)&sadd, sizeof(sadd)) < 0) {
            fprintf(stderr, "ERROR connecting\n"); 
            fprintf(stderr, "Errored on %s\n", hostname);
        }
        if(conn) {
            char* okMes = "HTTP/1.1 200 Connection Established\r\n\r\n";
            if((n = write(clientfd, okMes, strlen(okMes)) < 0))
                fprintf(stderr, "ERROR writing too socket\n");
            bzero(buffer, n+1);
        }
    }

    if(!conn) {
        if((n = write(serverfd, intactBuffer, n)) < 0)
            fprintf(stderr, "ERROR writing too socket\n");

        int update = transferGetData(serverfd, clientfd, n, cache, key);
        if(update >= 0) {
            updateTTL(intactBuffer, update, cache, key);
        }
         pthread_mutex_unlock(&mutex);

    } else {
        transferConnectData(serverfd, clientfd, info);
    }

    free(hostname);
    free(key);
    free(intactBuffer);
    free(buffer);
    free(clientInfo); 
    close(serverfd);
    close(clientfd);

    return NULL;
}

int parseInput(int argc, char** argv) {

    char* arg;

    if(argc == 2) {
        if(strstr(argv[1], "--help") != NULL) {
            printf("Input should be ./a.out followed by the desired port number.\n");
            printf("Optional option is '--bw-limit=' followed by the desired bandwidth");
            printf(" rate limit in KB/s.\n");
            return -1;
        } else {
            networkLimit = 0;
            return atoi(argv[1]);
        }
    } else if(argc == 3) {
        if((arg = strstr(argv[2], "--bw-limit=")) != NULL) {
            networkLimit = 1000*atoi(arg+11);
            return atoi(argv[1]);
        } else {
            printf("Expected usage: ./a.out [Port Number] [Bandwidth Limit]\n");
            return -1;
        }
    } else {
        printf("Expected usage: ./a.out [Port Number] [Bandwidth Limit]\n");
        return -1;
    }
}

int main(int argc, char** argv) {

    pthread_t thread;
    pthread_attr_t tattr;
    int sockfd, newsockfd, portNumber, optval, clientsock, servPort;
    socklen_t clilen;
    struct sockaddr_in saddr, caddr;
    struct hostent *hostp, *server;
    char *hostaddrp, *cachedData;
    struct sockaddr_in serveraddr;
    struct rateLimit* bucket = NULL;
    struct bannedIP* ips = NULL;
    int currTime = time(NULL), minuteCheck = 0, n = 0;
    currentBandwidth = 0;

    if((portNumber = parseInput(argc, argv)) < 0) {
        return 1;
    }

    for(int i = 0; i < 10; i++) {
        cache[i] = malloc(sizeof(HTTPcache));
        cache[i]->key = NULL;
        cache[i]->data = NULL;
        cache[i]->age = 0;
        cache[i]->death = 0;
    }

    if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("Error opening socket\n");
        return 1;
    }

    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
         (const void *)&optval , sizeof(int));

    if(fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK) < 0){
        fprintf(stderr, "Failed to set socket to non-blocking!\n");
    }

    memset(&saddr, '\0', sizeof(saddr)); 
    saddr.sin_family= AF_INET;
    saddr.sin_addr.s_addr= htonl(INADDR_ANY);
    saddr.sin_port= htons(portNumber);

    if((bind(sockfd, (struct sockaddr*) &saddr, sizeof(saddr)) < 0)) { 
        printf("Error binding\n");
        return 1;
    }

    if(listen(sockfd, 5) < 0) {
        printf("Error listening\n");
    }

    clilen = sizeof(caddr);

    while(1) {

        newsockfd = accept(sockfd, (struct sockaddr*) &caddr, &clilen);

        if (newsockfd < 0) {
            //If these are set then the call errored on purpose
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                //fprintf(stderr, "should be here\n");
                if(currTime < time(NULL)) {
                    currTime = time(NULL);
                    minuteCheck++;
                    incrementBWLimit();
                    if(minuteCheck == 60) 
                        incrementBucket(&bucket, 1);
                    else
                        incrementBucket(&bucket, 0);
                }

                continue;
            } else {
                printf("Error accepting\n");
                return 1;
            } 
        }

        hostp = gethostbyaddr((const char *)&caddr.sin_addr.s_addr, 
              sizeof(caddr.sin_addr.s_addr), AF_INET);

        hostaddrp = inet_ntoa(caddr.sin_addr);
        if (hostaddrp == NULL)
            fprintf(stderr, "ERROR on inet_ntoa\n");

        if((n = isBanned(hostaddrp, ips)) || updateBucket(&bucket, hostaddrp)) {
            tooManyRequests(newsockfd, hostaddrp, n, &ips);
            close(newsockfd);
            continue;
        }

        struct serverInfo *item = malloc(sizeof(struct serverInfo));  
        item->client_fd = newsockfd;  
        strcpy(item->ip,hostaddrp);  
        item->port = portNumber;  
        item->mainfd = sockfd;
        pthread_attr_init(&tattr);
        pthread_attr_setdetachstate(&tattr,PTHREAD_CREATE_DETACHED);
        pthread_create(&thread, &tattr, serverSocket, (void *)item); 
    }

    for(int i = 0; i < 10; i++) {
        if(cache[i]->key)
            free(cache[i]->key);
        if(cache[i]->data)
            free(cache[i]->data);
        free(cache[i]);
    }

    while(bucket != NULL) {
        struct rateLimit* temp = bucket->next;
        free(bucket->ip);
        free(bucket);
        bucket = temp;
    }

    return 0;
}