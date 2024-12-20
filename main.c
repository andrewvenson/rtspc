#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <bits/time.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PORT 8080
#define BUFFER_SIZE 2048
#define STREAM_BUFFER_SIZE 8192
#define max_clients 10
#define BIG_BUFFER 65700
#define FPS 30
#define FRAME_INTERVAL_US (1000000 / FPS)

typedef struct {
  int client_fd;
  int *send_client_fd;
  int *play;
  char *buffer;
  int *recording;
} Handle_Request_Args;

void get_date(char *time_buffer, size_t time_buffer_size) {
  time_t now = time(NULL);
  struct tm *local = localtime(&now);
  strftime(time_buffer, time_buffer_size, "Date: %a, %d %b %Y %H:%M:%S %Z",
           local);
}

void print_packet(int size, char *buffer) {
  printf("\n\n");
  for (int x = 0; x < size; x++) {
    printf("%c", buffer[x]);
  }
  printf("\n\n");
}

void decode_and_relay_rtp(int client_fd, int send_client_fd) {
  char buffer[BUFFER_SIZE];
  int buffer_size = 0;
  int byte = 0;

  while (1) {
    if ((buffer_size = recv(client_fd, buffer, BUFFER_SIZE, 0)) > 0) {
      while (byte < buffer_size) {
        if (buffer[byte] == '$' && byte + 3 < buffer_size) {
          uint16_t payload_length = (buffer[byte + 2] << 8) | buffer[byte + 3];
          int payload_length_hit =
              1; // set this to one since we set the $ marker first

          printf("payload length: %d\n", payload_length);
          char new_buffer[4];
          int new_buffer_size = 0;
          char current_buffer[BIG_BUFFER];

          current_buffer[byte] = buffer[byte];

          while (payload_length_hit < payload_length + 4) {
            if ((new_buffer_size = recv(client_fd, new_buffer, 4, 0)) > 0) {
              for (int x = 0; x < new_buffer_size; x++) {
                if (new_buffer[x] == '$') {
                  printf("YOU TRIPPIN FOO\n");
                }
                current_buffer[payload_length_hit] = new_buffer[x];
                payload_length_hit += 1;
              }
              memset(new_buffer, 0, sizeof(new_buffer));
            }
          }

          printf("SENDING: %d\n\n", payload_length_hit);
          send(send_client_fd, current_buffer, payload_length + 4, 0);
          memset(current_buffer, 0, sizeof(current_buffer));
          memset(new_buffer, 0, sizeof(new_buffer));
        }
        byte += 1;
      }
      byte = 0;
      memset(buffer, 0, sizeof(buffer));
    }
  }
}

void stream(int *play, int client_fd, int *send_client_fd) {
  int play_message_sent = 0;

  while (1) {
    if (*play != 0) {
      if (play_message_sent == 0) {
        printf("Playing on file descriptor: %d\n\n", *send_client_fd);
        play_message_sent = 1;
      }
      decode_and_relay_rtp(client_fd, *send_client_fd);
    }
  }
}

void record(char *buffer, int client_fd, int *send_client_fd, int *play,
            int *recording) {
  printf("%s\n\n", buffer);
  char time_buffer[200];
  char response_date[87];
  char record_response[512];
  memset(record_response, 0, sizeof(record_response));
  memset(response_date, 0, sizeof(response_date));
  memset(record_response, 0, sizeof(record_response));
  strcat(record_response, "RTSP/1.0 200 OK\r\n");
  strcat(record_response, "CSeq: 4\r\n");
  strcat(record_response, "Session: 12345678\r\n");
  get_date(time_buffer, sizeof(time_buffer));
  strcat(record_response, time_buffer);
  strcat(record_response, "\r\n\r\n");
  printf("%s\n\n", record_response);
  send(client_fd, record_response, strlen(record_response), 0);
  *recording = 1;
  stream(play, client_fd, send_client_fd);
}

void play(char *buffer, int client_fd, int *play) {
  printf("%s\n\n", buffer);

  char play_response[300];
  memset(play_response, 0, sizeof(play_response));

  strcat(play_response, "RTSP/1.0 200 OK\r\n");
  strcat(play_response, "CSeq: 4\r\n");
  strcat(play_response, "Range: npt=0.000-\r\n");
  strcat(play_response, "Content-Length: 0\r\n");
  strcat(play_response, "Session: 12345678\r\n\r\n");

  printf("%s\n\n", play_response);

  send(client_fd, play_response, strlen(play_response), 0);

  if (client_fd == 5) {
    *play = 1;
  }
}

void announce(char *buffer, int client_fd) {
  printf("%s\n\n", buffer);
  char announce_response[] = "RTSP/1.0 200 OK\r\n"
                             "CSeq: 2\r\n"
                             "Session: 12345678\r\n"
                             "\r\n";
  printf("%s\n\n", announce_response);
  send(client_fd, announce_response, strlen(announce_response), 0);
}

void setup(char *buffer, int client_fd, int *recording) {
  printf("%s\n\n", buffer);
  char setup_response[300];
  memset(setup_response, 0, sizeof(setup_response));
  strcat(setup_response, "RTSP/1.0 200 OK\r\n");
  if (client_fd == 5 || client_fd == 6 || client_fd == 7 || client_fd == 8) {
    strcat(setup_response, "CSeq: 3\r\n");
  } else {
    strcat(setup_response, "CSeq: 3\r\n");
  }
  if (*recording) {
    strcat(setup_response,
           "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
  } else {
    strcat(setup_response,
           "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
  }
  strcat(setup_response, "Session: 12345678\r\n\r\n");
  printf("%s\n\n", setup_response);
  send(client_fd, setup_response, strlen(setup_response), 0);
}

void describe(char *buffer, int client_fd, int *recording) {
  printf("%s\n\n", buffer);
  char describe_response[500];
  memset(describe_response, 0, sizeof(describe_response));
  strcat(describe_response, "RTSP/1.0 200 OK\r\n");
  if (client_fd == 5 || client_fd == 6 || client_fd == 7 || client_fd == 8) {
    strcat(describe_response, "CSeq: 2\r\n");
  } else {
    strcat(describe_response, "CSeq: 2\r\n");
  }
  strcat(describe_response, "Content-Base: rtsp://192.168.1.29:8080/\r\n");
  strcat(describe_response, "Content-Type: application/sdp\r\n");
  strcat(describe_response, "Content-Length: 281\r\n");
  strcat(describe_response, "\r\n");

  if (*recording == 1) {
    strcat(describe_response, "v=0\r\n");
    strcat(describe_response, "o=- 0 0 IN IP4 192.168.1.29\r\n");
    strcat(describe_response, "s=RTSP Session\r\n");
    strcat(describe_response, "c=IN IP4 0.0.0.0\r\n");
    strcat(describe_response, "t=0 0\r\n");
    strcat(describe_response, "a=control:*\r\n");
    strcat(describe_response, "m=video 0 RTP/AVP 96\r\n");
    strcat(describe_response, "a=rtpmap:96 H264/90000\r\n");
    strcat(describe_response,
           "a=fmtp:96 packetization-mode=1; "
           "sprop-parameter-sets=Z3oAFrzZQKAv+JhAAAADAEAAAAeDxYtlgA==,aOvjyyLA;"
           " profile-level-id=7A0016\r\n");
    strcat(describe_response, "a=control:streamid=0\r\n");
  }
  printf("Sending DESCRIBE: %s\n\n", describe_response);
  send(client_fd, describe_response, strlen(describe_response), 0);
}

void options(char *buffer, int client_fd) {
  printf("%s\n\n", buffer);
  char options_response[90];
  memset(options_response, 0, sizeof(options_response));
  strcat(options_response, "RTSP/1.0 200 OK\r\n");
  if (client_fd == 5 || client_fd == 6 || client_fd == 7 || client_fd == 8) {
    strcat(options_response, "CSeq: 1\r\n");
  } else {
    strcat(options_response, "CSeq: 1\r\n");
  }
  strcat(options_response, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, "
                           "PAUSE, RECORD, TEARDOWN, ANNOUNCE\r\n\r\n");
  printf("%s\n\n", options_response);
  send(client_fd, options_response, strlen(options_response), 0);
}

void get_method(char *buffer, int buffer_size, char *method) {
  for (int x = 0; x < buffer_size; x++) {
    if (buffer[x] == ' ') {
      method[x] = '\0';
      break;
    }
    method[x] = buffer[x];
  }
}

void *handle_requests(void *arg) {
  int buffer_size = 0;
  Handle_Request_Args *args = (Handle_Request_Args *)arg;
  int client_fd = args->client_fd;
  char *buffer = args->buffer;

  while (1) {
    if ((buffer_size = recv(client_fd, buffer, BUFFER_SIZE - 1, 0)) > 0) {
      buffer[buffer_size] = '\0';
      char method[10];
      memset(method, 0, sizeof(method));

      get_method(buffer, buffer_size, method);

      if (strcmp(method, "OPTIONS") == 0) {
        options(buffer, client_fd);
      } else if (strcmp(method, "DESCRIBE") == 0) {
        describe(buffer, client_fd, args->recording);
      } else if (strcmp(method, "SETUP") == 0) {
        setup(buffer, client_fd, args->recording);
      } else if (strcmp(method, "ANNOUNCE") == 0) {
        announce(buffer, client_fd);
      } else if (strcmp(method, "RECORD") == 0) {
        record(buffer, client_fd, args->send_client_fd, args->play,
               args->recording);
      } else if (strcmp(method, "PLAY") == 0) {
        play(buffer, client_fd, args->play);
      }
    }
  }

  printf("closing");
  close(client_fd);
}

int main() {
  int server_fd;
  int client_fds[max_clients] = {0};
  int recording = 0;
  int play = 0;
  pthread_t threads[max_clients] = {0};
  pthread_t threads_used[max_clients] = {0};
  struct sockaddr_in server_addr;
  struct sockaddr_in client_push_addr;
  struct sockaddr_in client_addr;
  socklen_t client_addr_size = sizeof(client_addr);
  socklen_t client_push_size = sizeof(client_push_addr);
  char buffer[max_clients][BUFFER_SIZE];
  int opt = 1;

  setbuf(stdout, NULL); // disable buffering to allow printing to file

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("Error creating socket");
    return -1;
  }

  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("Error setting socket options");
    close(server_fd);
    return -1;
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(PORT);

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("Error binding socket to server address");
    close(server_fd);
    return -1;
  }

  if (listen(server_fd, max_clients) < 0) {
    perror("Error listening for connections");
    close(server_fd);
    return -1;
  }

  printf("Listening for connections on port: %d\n\n", PORT);

  memset(threads_used, 0, sizeof(threads_used));
  memset(threads, 0, sizeof(threads));
  memset(client_fds, 0, sizeof(client_fds));

  while (1) {
    printf("Getting connections...\n");

    for (int client_fd_index = 0; client_fd_index < max_clients;
         client_fd_index++) {
      memset(buffer[client_fd_index], 0, sizeof(buffer[client_fd_index]));

      if (client_fds[client_fd_index] == 0 &&
          threads_used[client_fd_index] == 0) {
        printf("Connection file descriptor: %d\n", client_fds[client_fd_index]);

        threads_used[client_fd_index] = 1;

        client_fds[client_fd_index] = accept(
            server_fd, (struct sockaddr *)&client_addr, &client_addr_size);

        if (client_fds[client_fd_index] < 0) {
          perror("Error connecting...\n");
          continue;
        }

        printf("Accepted Connection on file descriptor: %d\n",
               client_fds[client_fd_index]);

        Handle_Request_Args args;
        memset(&args, 0, sizeof(args));

        args.client_fd = client_fds[client_fd_index];
        args.send_client_fd = &client_fds[client_fd_index + 1];
        args.play = &play;
        args.buffer = buffer[client_fd_index];
        args.recording = &recording;

        pthread_create(&threads[client_fd_index], NULL, handle_requests, &args);
      } else {
        printf("client already listening on file descriptor: %d\n",
               client_fds[client_fd_index]);
      }
    }
  }

  close(server_fd);

  return 0;
}
