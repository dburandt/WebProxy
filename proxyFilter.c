#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>

#define BACKLOG 10                // how many pending connections queue will hold
#define MAXDATASIZE 99999         // max number of bytes we can get at once

void* connection_handler(void *);
int checkBlacklist(FILE *, char *, char *);
void sigchld_handler(int);
void *get_in_addr(struct sockaddr *);

const char *hostPort = NULL;    // set up port variable
char headRequest[1000];
char getRequest[1000];
char *newFile;
char host[256];
char httpPage[MAXDATASIZE];
char s[INET6_ADDRSTRLEN];
int contentLength;
struct sockaddr_storage their_addr; // connector's address information
int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
socklen_t sin_size;
int getR_fd;         // send requst on getR_fd
struct addrinfo hints, *servinfo, *p;
struct sigaction sa;
int rv, gr;
char HTTP_REQUEST[MAXDATASIZE];
char PROXY_REQUEST[MAXDATASIZE];
int numBytes = 1;

FILE *blacklist;


int main(int argc, const char **argv) {
    
    hostPort = argv[1];
    blacklist = fopen(argv[2], "r");
    
    int yes = 1;
    
    memset(&hints, 0, sizeof hints);// make sure the struct is empty
    hints.ai_family = AF_UNSPEC;    // AF_UNSPEC=dont care IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;// TCP stream sockets
    hints.ai_flags = AI_PASSIVE;    // use my IP
    
    // if getaddrinfo() doesnt return 0, print the error
    if ((rv = getaddrinfo(NULL, hostPort, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }
    
    // loop through all the results and bind to the first we can
    // the ifs are for errors, we hit break once we reach a bindable result
    for (p = servinfo; p != NULL; p = p->ai_next) {
        
        // feed the results of getaddrinfo() into socket()
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                             p->ai_protocol)) == -1) {
            perror("server: socket");
            continue;
        }
        
        // get rid of "address already in use" error
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            perror("setsockopt");
            exit(1);
        }
        
        // sockfd = socket file descriptor
        // ai_addr = struct that contains port & IP
        // ai_addrlen = length of address (bytes)
        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("server: bind");
            continue;
        }
        
        break;
    }
    
    freeaddrinfo(servinfo); // all done with this structure
    
    if (p == NULL)  {
        fprintf(stderr, "server: failed to bind\n");
        exit(1);
    }
    
    // Listen
    if (listen(sockfd, BACKLOG) == -1) {
        perror("listen");
        exit(1);
    }
    
    sa.sa_handler = sigchld_handler; // reap all dead processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
    
    printf("server: waiting for connections...\n");
    
    // START THREADING
    // create 4 worker threads
    pthread_t tid[4];
    int err;
    int i = 0;
    while (i < 4) {
        err = pthread_create(&tid[i], NULL, &connection_handler, (void*) &sockfd);
        i++;
    }
    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
    pthread_join(tid[2], NULL);
    pthread_join(tid[3], NULL);
    
    return 0;
}

void* connection_handler(void* socket_desc) {
    
    while(1) {  // main accept() loop
        
        sin_size = sizeof their_addr;
        new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_fd == -1) {
            perror("accept");
            continue;
        }
        
        inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr), s, sizeof s);
        printf("server: got connection from %s\n", s);
        
        if (!fork()) { // this is the child process
            close(sockfd); // child doesn't need the listener
            
            // recieve message from client
            if ((numBytes = recv(new_fd, HTTP_REQUEST, MAXDATASIZE-1, 0)) == -1) {
                perror("recv");
                exit(1);
            }
            
            // parse GET request -------------------------------------------------------------------------
            
            printf("\n%s\n", HTTP_REQUEST);
            
            char request_type[4];
            char filename[1024];
            const char ch[] = ":";
            
            char *portTest;
            char *strptr;
            char *port;      // optional. default to 80
            
            strptr = strtok(HTTP_REQUEST, " ");
            strcpy(request_type, strptr);
            
            strptr = strtok(NULL, "//");
            
            strptr = strtok(NULL, "/");
            strcpy(host, strptr);
            
            strptr = strtok(NULL, " ");
            strcpy(filename, strptr);
            
            portTest = strpbrk(filename, ch);
            
            if (portTest) {
                newFile = strtok(filename, ":");
                printf("filename without port: %s\n", newFile);
                port = strtok(NULL, " ");
                printf("port: %s\n", port);
                
            } else {
                newFile = filename;
                port = "80";
            }
            
            // --------------------------------------------------------------------------------------------
            
            // send a 405 if request is not a GET
            if (strcmp(request_type, "GET") != 0) {
                if (send(new_fd, "405: Please send a GET request only\n", 37, 0) == -1) {
                    perror("send");
                }
                close(new_fd);
                exit(1);
            }
            
            printf("Host: %s Filename: %s \n", host, filename);
            
            // send a 403 if URI is blacklisted
            
            if (checkBlacklist(blacklist, host, filename) != 0) {
                if (send(new_fd, "403: Your requested URI is blacklisted\n", 40, 0) == -1) {
                    perror("send");
                }
                close(new_fd);
                exit(1);
            }
            
            
            // reset getaddinfo()
            if ((gr = getaddrinfo(host, port, &hints, &servinfo)) != 0) {
                fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
                exit(1);
            }
            
            
            // open a socket and get a connection
            for (p = servinfo; p != NULL; p = p->ai_next) {
                if ((getR_fd = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
                    perror("client: socket");
                    continue;
                }
                
                if (connect(getR_fd, p->ai_addr, p->ai_addrlen) == -1) {
                    close(getR_fd);
                    perror("client: connect");
                    continue;
                }
                
                break;
            }
            
            // error check
            if (p == NULL) {
                fprintf(stderr, "proxy: failed to connect\n");
                 exit(1);
            }
            freeaddrinfo(servinfo); // done with this structure
            
            // set up the request
            strncpy(getRequest, "GET /", 5);
            strncat(getRequest, newFile, strlen(newFile));
            strncat(getRequest, " HTTP/1.1\r\nHost: ", strlen(" HTTP/1.1\r\nHost: "));
            strncat(getRequest, host, strlen(host));
            strncat(getRequest, "\r\nConnection: close\r\n\r\n", strlen("\r\nConnection: close\r\n\r\n"));
            
            // send the request
            if (send(getR_fd, getRequest, strlen(getRequest), 0) == -1) {
                perror("send");
            }
            
            char stream[1000];
            memset(httpPage, 0, sizeof(httpPage));
            memset(stream, 0, sizeof(stream));
            int headerCheck = 1;
            
            while (1) {
                if ((numBytes = recv(getR_fd, stream, 1000, 0)) == -1) {
                    perror("recv");
                    exit(1);
                }
                // check for server error
                if (headerCheck == 1) {
                    if (stream[9] == '5') {
                        send(new_fd, "Unable to connect to host due to a Server Error.\n", 51, 0);
                        close(new_fd);
                        exit(1);
                    }
                }
                headerCheck = 0;
                
                if (numBytes == 0) { break; }
                
                strcat(httpPage, stream);
                memset(stream, 0, sizeof(stream));
                
            }
            memset(stream, 0, sizeof(stream));
            
            // print resonse to console
            printf("%s", httpPage);
            
            // send response to client
            if (send(new_fd, httpPage, sizeof(httpPage), 0) == -1) {
                perror("send");
            }
            
            close(getR_fd);
            close(new_fd);
            
            exit(0);
        }
        close(new_fd);
    }
}

int checkBlacklist(FILE *blacklist, char *host, char *filename) {
    
    char sLine[100];
    
    while(fgets(sLine, sizeof(sLine), blacklist) != NULL) {
        
        if (strcasestr(host, sLine) != NULL) {
            return 1;
        } else if (strcasestr(filename, sLine) != NULL) {
            return 1;
        }
    }
    fclose(blacklist);
    memset(sLine, 0, sizeof(sLine));
    
    return 0;
}

void sigchld_handler(int s) {
    // waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;
    
    while(waitpid(-1, NULL, WNOHANG) > 0);
    
    errno = saved_errno;
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }
    
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

