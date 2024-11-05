#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
  int server_fd;
  int client_fd;
  struct sockaddr_in server_addr;
  struct sockaddr_in client_addr;
  socklen_t client_addr_size = sizeof(client_addr);
  char buffer[BUFFER_SIZE];
  int opt = 1;

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

  if (listen(server_fd, 5) < 0) {
    perror("Error listening for connections");
    close(server_fd);
    return -1;
  }
  printf("Listening for connections on port: %d\n\n", PORT);

  while (1) {
    memset(buffer, 0, sizeof(buffer));
    client_fd =
        accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_size);
    if (client_fd < 0) {
      perror("Error connecting...\n");
      continue;
    }

    while (1) {
      int buffer_size = 0;

      if ((buffer_size = recv(client_fd, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[buffer_size] = '\0';

        char method[10];
        memset(&method, 0, sizeof(method));

        char options_response[] =
            "RTSP/1.0 200 OK\r\n"
            "CSeq: 1\r\n"
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n"
            "\r\n";

        char describe_response[] = "RTSP/1.0 200 OK\r\n"
                                   "CSeq: 2\r\n"
                                   "Content-Base: rtsp://127.0.0.1:8080/\r\n"
                                   "Content-Type: application/sdp\r\n"
                                   "Content-Length: 152\r\n"
                                   "\r\n"
                                   "v=0\r\n"
                                   "o=- 0 0 IN IP4 127.0.0.1\r\n"
                                   "s=RTSP Session\r\n"
                                   "c=IN IP4 0.0.0.0\r\n"
                                   "t=0 0\r\n"
                                   "a=control:*\r\n"
                                   "m=video 0 RTP/AVP 96\r\n"
                                   "a=rtpmap:96 H264/90000\r\n"
                                   "a=control:trackID=0\r\n";

        char setup_response[] =
            "RTSP/1.0 200 OK\r\n"
            "CSeq: 3\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "Session: 12345678\r\n"
            "\r\n";

        char play_response[] = "RTSP/1.0 200 OK\r\n"
                               "CSeq: 4\r\n"
                               "Session: 12345678\r\n"
                               "\r\n";

        for (int x = 0; x < buffer_size; x++) {
          if (buffer[x] == ' ') {
            method[x] = '\0';
            break;
          }
          method[x] = buffer[x];
        }

        printf("%s\n\n", buffer);

        if (strcmp(method, "OPTIONS") == 0) {
          printf("%s\n\n", options_response);
          send(client_fd, options_response, strlen(options_response), 0);
        } else if (strcmp(method, "DESCRIBE") == 0) {
          printf("%s\n\n", describe_response);
          send(client_fd, describe_response, strlen(describe_response), 0);
        } else if (strcmp(method, "SETUP") == 0) {
          printf("%s\n\n", setup_response);
          send(client_fd, setup_response, strlen(setup_response), 0);
        } else if (strcmp(method, "PLAY") == 0) {
          printf("%s\n\n", play_response);
          send(client_fd, play_response, strlen(play_response), 0);
        }
      }
    }

    close(client_fd);
  }

  close(server_fd);

  return 0;
}
