#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <signal.h>

#define MAX_CLIENTS 100
#define BUFFER_SZ 2048

//static _Atomic is used in order to prevent race conditions in threads 
static _Atomic unsigned int cli_count = 0;
static int client_id = 1;

typedef struct{
	struct sockaddr_in address;
	int sockfd;
	int client_id;
	char name[32];
} client_struct;

client_struct *clients[MAX_CLIENTS];

pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

void trim_endl_char (char* array, int length) {
  int i;
  for (i = 0; i < length; i++) {
    if (array[i] == '\n') {
      array[i] = '\0';
      break;
    }
  }
}

void print_IPv4_adrr(struct sockaddr_in addr){
    printf("%d.%d.%d.%d",
        addr.sin_addr.s_addr & 0xff,
        (addr.sin_addr.s_addr & 0xff00) >> 8,
        (addr.sin_addr.s_addr & 0xff0000) >> 16,
        (addr.sin_addr.s_addr & 0xff000000) >> 24);
}

// Add new client to clients list
void add_client(client_struct *client){
	pthread_mutex_lock(&clients_mutex);

	for(int i=0; i < MAX_CLIENTS; ++i){
		if(clients[i] == NULL){
			clients[i] = client;
			break;
		}
	}

	pthread_mutex_unlock(&clients_mutex);
}

// Remove clients from clients list
void remove_client(int cl_id){
	pthread_mutex_lock(&clients_mutex);

	for(int i=0; i < MAX_CLIENTS; i++){
		if(clients[i] != NULL){
			if(clients[i]->client_id == cl_id){
				clients[i] = NULL;
				break;
			}
		}
	}

	pthread_mutex_unlock(&clients_mutex);
}

// Send message to all clients from the chat room except the sender
void send_message(char *s, int cl_id){
	pthread_mutex_lock(&clients_mutex);

	for(int i=0; i<MAX_CLIENTS; i++){
		if(clients[i]){
			if(clients[i]->client_id != cl_id){
				if(write(clients[i]->sockfd, s, strlen(s)) < 0){
					perror("ERROR: write to descriptor failed");
					break;
				}
			}
		}
	}

	pthread_mutex_unlock(&clients_mutex);
}

// Client communication
void *handle_client(void *arg){
	char buff_out[BUFFER_SZ];
	char name[32];
	int leave_flag = 0;

	cli_count++;
	client_struct *cli = (client_struct *)arg;

	//Setting name
	if(recv(cli->sockfd, name, 32, 0) <= 0 || strlen(name) <  2 || strlen(name) > 31){
		printf("Didn't enter the name.\n");
		leave_flag = 1;
	} else{
		strcpy(cli->name, name);
		sprintf(buff_out, "%s has joined\n", cli->name);
		printf("%s", buff_out);
		send_message(buff_out, cli->client_id);
	}

	memset(buff_out, 0, BUFFER_SZ);

	while(1){
		if (leave_flag) {
			break;
		}

		int receive = recv(cli->sockfd, buff_out, BUFFER_SZ, 0);
		if (receive > 0){
			if(strlen(buff_out) > 0){
				send_message(buff_out, cli->client_id);
				trim_endl_char(buff_out, strlen(buff_out));
				printf("%s\n", buff_out);
			}
		} else if (receive == 0 || strcmp(buff_out, "exit") == 0){
			sprintf(buff_out, "%s has left\n", cli->name);
			printf("%s", buff_out);
			send_message(buff_out, cli->client_id);
			leave_flag = 1;
		} else {
			printf("ERROR: -1\n");
			leave_flag = 1;
		}

		memset(buff_out, 0, BUFFER_SZ);
	}

  
  	close(cli->sockfd);
  	remove_client(cli->client_id);
  	free(cli);
  	cli_count--;
  	pthread_detach(pthread_self());

	return NULL;
}

int main(int argc, char **argv){
	if(argc != 2){
		printf("Usage: %s <port>\n", argv[0]);
		return EXIT_FAILURE;
	}

	char *ip = "127.0.0.1";
	int port = atoi(argv[1]);
	int option = 1;
	int server_fd = 0, connfd = 0;

  	struct sockaddr_in serv_addr;
  	struct sockaddr_in cli_addr;
  	pthread_t tid;

 	/* Socket settings */
  	server_fd = socket(AF_INET, SOCK_STREAM, 0);
  	serv_addr.sin_family = AF_INET;
  	serv_addr.sin_addr.s_addr = inet_addr(ip);
  	serv_addr.sin_port = htons(port);

	struct sigaction sigSIGPIPE;
    sigSIGPIPE.sa_handler=SIG_IGN;
    sigSIGPIPE.sa_flags=0;
    if(sigaction(SIGPIPE,&sigSIGPIPE,NULL)<0)
    {
        perror("Error SIGPIPE\n");
        exit(-1);
    }

	if(setsockopt(server_fd, SOL_SOCKET,(SO_REUSEPORT | SO_REUSEADDR),(char*)&option,sizeof(option)) < 0){
		perror("ERROR: setsockopt failed");
    	return EXIT_FAILURE;
	}

  	if(bind(server_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
		perror("ERROR: Socket binding failed");
		return EXIT_FAILURE;
  	}

  	if (listen(server_fd, 5) < 0) {
		perror("ERROR: Socket listening failed");
		return EXIT_FAILURE;
	}

	printf("-------Server Started-------\n");

	while(1){
		socklen_t cli_len = sizeof(cli_addr);
		connfd = accept(server_fd, (struct sockaddr*)&cli_addr, &cli_len);

		//Check the number of clients
		if((cli_count + 1) == MAX_CLIENTS){
			printf("Max clients reached. Rejected: ");
			print_IPv4_adrr(cli_addr);
			printf(":%d\n", cli_addr.sin_port);
			close(connfd);
			continue;
		}

		//Initialize the client with number cli_count
		client_struct *cli = (client_struct *)malloc(sizeof(client_struct));
		cli->address = cli_addr;
		cli->sockfd = connfd;
		cli->client_id = client_id++;

		//Add client to clients list
		add_client(cli);
		
		//Create a thread for every client
		pthread_create(&tid, NULL, &handle_client, (void*)cli);

		//Reduce CPU usage
		sleep(1);
	}

	return 0;
}
