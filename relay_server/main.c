#include <arpa/inet.h>
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

#define PORT 8081
#define UDP_PORT 8082
#define UDP_RTCP_PORT 8083
#define TCP_RTSP_BUFFER_SIZE 2048
#define STREAM_BUFFER_SIZE 1500
#define max_clients 10

// The following command records the live stream from initial client
/* (macos)
 ffmpeg -re -loglevel debug -f \
 avfoundation -framerate 30 -video_size 640x360 -i \
 0 -fflags +genpts \
 -c:v libx264 \
 -x264-params "keyint=60:no-scenecut=1" \
 -f rtsp rtsp://127.0.0.1:8081
*/

// The following command plays live stream from  client
/*
  ffplay -loglevel debug -v verbose \
  -vcodec h264 -rtsp_transport udp -i \
  rtsp://192.168.1.29:8081
 */

// The following command converts stream to hls
/*
 ffmpeg - re - loglevel debug - rtsp_transport udp -i \
 rtsp : //192.168.1.29:8081 \
 -vf fps=30 \
 -c:v libx264 -preset veryfast -g 30 \
 -c:a aac -b:a 128k \
 -f hls -hls_time 4 -hls_segment_filename "segment_%03d.ts" \
 output.m3u8
*/

typedef struct {
  int *play;
  int *recording;
  int tcp_client_fd;
  int udp_rtp_server_fd;
  int udp_rtcp_server_fd;
  int *udp_rtp_client_fd;
  int *udp_rtcp_client_fd;
  struct sockaddr_in *udp_rtp_client_addr;
  socklen_t udp_rtp_client_addr_size;
  struct sockaddr_in *udp_rtcp_client_addr;
  socklen_t udp_rtcp_client_addr_size;
  char *rtsp_relay_server_ip;
} Handle_Request_Args;

struct stream_data {
  int udp_server_fd;
  int udp_client_fd;
  struct sockaddr_in udp_client_addr;
  socklen_t udp_client_addr_size;
};

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

void *stream_protocol(void *data) {
  struct stream_data *sd = (struct stream_data *)data;
  int udp_server_fd = sd->udp_server_fd;
  int udp_client_fd = sd->udp_client_fd;
  struct sockaddr_in udp_client_addr = sd->udp_client_addr;
  socklen_t udp_client_addr_size = sd->udp_client_addr_size;

  int play_message_sent = 0;
  int bytes = 0;
  struct sockaddr_in udp_sender_addr;
  socklen_t udp_sender_addr_size = sizeof(udp_sender_addr);
  char buffer[STREAM_BUFFER_SIZE];

  while (1) {
    if (play_message_sent == 0) {
      printf("udp_server_fd: %d, udp_client_fd: %d\n\n", udp_server_fd,
             udp_client_fd);
      printf("FD Address: %s:%d\n\n", inet_ntoa(udp_client_addr.sin_addr),
             ntohs(udp_client_addr.sin_port));
      play_message_sent = 1;
    }

    printf("Sending on PORT: %d\n\n", ntohs(udp_client_addr.sin_port));

    bytes =
        recvfrom(udp_server_fd, buffer, STREAM_BUFFER_SIZE, 0,
                 (struct sockaddr *)&udp_sender_addr, &udp_sender_addr_size);

    if (bytes < 0) {
      perror("failed to get bytes");
      continue;
    } else {
      // printf("Sending BUFFER SIZE: %d\n\n\n", bytes);
      sendto(udp_client_fd, buffer, bytes, 0,
             (struct sockaddr *)&udp_client_addr, udp_client_addr_size);
      memset(buffer, 0, sizeof(buffer));
    }
  }
}

void stream(int *play, int udp_rtp_server_fd, int udp_rtcp_server_fd,
            int *udp_rtp_client_fd, int *udp_rtcp_client_fd,
            struct sockaddr_in *udp_rtp_client_addr,
            socklen_t udp_rtp_client_addr_size,
            struct sockaddr_in *udp_rtcp_client_addr,
            socklen_t udp_rtcp_client_addr_size) {

  pthread_t rtp_thread;
  pthread_t rtcp_thread;
  int threads_waiting = 1;

  while (threads_waiting == 1) {
    if (*play != 0) {

      struct stream_data rtp_data;
      rtp_data.udp_server_fd = udp_rtp_server_fd;
      rtp_data.udp_client_fd = *udp_rtp_client_fd;
      rtp_data.udp_client_addr = *udp_rtp_client_addr;
      rtp_data.udp_client_addr_size = udp_rtp_client_addr_size;

      struct stream_data rtcp_data;
      rtcp_data.udp_server_fd = udp_rtcp_server_fd;
      rtcp_data.udp_client_fd = *udp_rtcp_client_fd;
      rtcp_data.udp_client_addr = *udp_rtcp_client_addr;
      rtcp_data.udp_client_addr_size = udp_rtcp_client_addr_size;

      pthread_create(&rtp_thread, NULL, stream_protocol, &rtp_data);
      pthread_create(&rtcp_thread, NULL, stream_protocol, &rtcp_data);
      threads_waiting = 0;
    }
  }
}

void record(char *buffer, int client_fd, int *play, int *recording,
            int udp_rtp_server_fd, int udp_rtcp_server_fd,
            int *udp_rtp_client_fd, int *udp_rtcp_client_fd,
            struct sockaddr_in *udp_rtp_client_addr,
            struct sockaddr_in *udp_rtcp_client_addr,
            socklen_t udp_rtp_client_addr_size,
            socklen_t udp_rtcp_client_addr_size) {
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

  stream(play, udp_rtp_server_fd, udp_rtcp_server_fd, udp_rtp_client_fd,
         udp_rtcp_client_fd, udp_rtp_client_addr, udp_rtp_client_addr_size,
         udp_rtcp_client_addr, udp_rtcp_client_addr_size);
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

  if (client_fd == 7) {
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

int create_client_udp_fd(int port, struct sockaddr_in *udp_client_addr) {
  printf("Creating client address for udp port:%d\n", port);
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in udp_client_addr_p = *udp_client_addr;

  if (fd < 0) {
    perror("Error creating socket");
    return -1;
  }

  udp_client_addr->sin_family = AF_INET;
  udp_client_addr->sin_port = htons(port);
  udp_client_addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  printf("Address: %s:%d\n\n", inet_ntoa(udp_client_addr->sin_addr),
         ntohs(udp_client_addr->sin_port));
  return fd;
}

// udp
void setup(char *buffer, int client_fd, int *recording, int rtp_port,
           int rtcp_port, char *rtp_port_char, char *rtcp_port_char,
           int *udp_rtp_client_fd, int *udp_rtcp_client_fd,
           struct sockaddr_in *udp_rtp_client_address,
           struct sockaddr_in *udp_rtcp_client_address) {
  printf("%s\n\n", buffer);
  char setup_response[300];
  memset(setup_response, 0, sizeof(setup_response));

  strcat(setup_response, "RTSP/1.0 200 OK\r\n");
  strcat(setup_response, "CSeq: 3\r\n");
  strcat(setup_response, "Transport: RTP/AVP/UDP;unicast;client_port=");
  strcat(setup_response, rtp_port_char);
  strcat(setup_response, "-");
  strcat(setup_response, rtcp_port_char);
  strcat(setup_response, ";server_port=8082-8083");
  strcat(setup_response, "\r\n");
  strcat(setup_response, "Session: 12345678\r\n\r\n");

  if (*recording == 1) {
    *udp_rtp_client_fd = create_client_udp_fd(rtp_port, udp_rtp_client_address);
    *udp_rtcp_client_fd =
        create_client_udp_fd(rtcp_port, udp_rtcp_client_address);
  }

  printf("%s\n\n", setup_response);
  send(client_fd, setup_response, strlen(setup_response), 0);
}

void describe(char *buffer, int client_fd, int *recording,
              char *rtsp_relay_server_ip) {
  printf("%s\n\n", buffer);
  char describe_response[500];
  char port[10];
  memset(describe_response, 0, sizeof(describe_response));
  // snprintf(port, sizeof(port), "%d", PORT);

  strcat(describe_response, "RTSP/1.0 200 OK\r\n");
  strcat(describe_response, "CSeq: 2\r\n");
  strcat(describe_response, "Content-Base: rtsp://");
  strcat(describe_response, rtsp_relay_server_ip);
  strcat(describe_response, ":");
  strcat(describe_response, "8081");
  strcat(describe_response, "/\r\n");
  strcat(describe_response, "Content-Type: application/sdp\r\n");
  strcat(describe_response, "Content-Length: 281\r\n");
  strcat(describe_response, "\r\n");

  if (*recording == 1) {
    strcat(describe_response, "v=0\r\n");
    strcat(describe_response, "o=- 0 0 IN IP4 ");
    strcat(describe_response, rtsp_relay_server_ip);
    strcat(describe_response, "\r\n");
    strcat(describe_response, "s=RTSP Session\r\n");
    strcat(describe_response, "c=IN IP4 ");
    strcat(describe_response, rtsp_relay_server_ip);
    strcat(describe_response, "\r\n");
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

  char options_response[100];
  memset(options_response, 0, sizeof(options_response));

  strcat(options_response, "RTSP/1.0 200 OK\r\n");
  strcat(options_response, "CSeq: 1\r\n");
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

// parses setup request to get udp client ports
int get_udp_client_ports(char *buffer, int buffer_size, int *rtp_port,
                         int *rtcp_port, char *rtp_port_char,
                         char *rtcp_port_char) {

  /*
   * This is for a client connecting
   SETUP rtsp://192.168.1.29:8081/streamid=0 RTSP/1.0
   Transport: RTP/AVP/UDP;unicast;client_port=26836-26837
   CSeq: 3
   User-Agent: Lavf60.16.100
  */

  /*
   * This is for a client streaming
  SETUP rtsp://192.168.1.29:8081/streamid=0 RTSP/1.0
  Transport: RTP/AVP/UDP;unicast;client_port=31940-31941;mode=record
  CSeq: 3
  User-Agent: Lavf61.7.100
  Session: 12345678
  */

  int client_port_section_found = 0;
  int rtp_port_found = 0;
  int client_end_found = 0;
  int semi_found = 0;

  for (int x = 0; x < buffer_size; x++) {
    if (buffer[x] == ';') {
      semi_found += 1;
    }

    if (semi_found == 2) {
      if (buffer[x] == '=') {
        printf("%s\n", &buffer[x + 1]);
        int z = 1;
        int y = 0;
        int a = 0;
        while (z) {
          if (buffer[x + y] == '=') {
            y += 1;
            continue;
          }
          if (buffer[x + y] == '-') {
            z = 0;
            break;
          }
          rtp_port_char[a] = buffer[x + y];
          a += 1;
          y += 1;
        }
        a = 0;
        z = 1;
        while (z) {
          if (buffer[x + y] == '-') {
            y += 1;
            continue;
          }
          if (buffer[x + y] == ';' || buffer[x + y] == '\r') {
            z = 0;
            break;
          }
          rtcp_port_char[a] = buffer[x + y];
          a += 1;
          y += 1;
        }
        *rtcp_port = atoi(rtcp_port_char);
        *rtp_port = atoi(rtp_port_char);
        semi_found = 0;
        return 0;
      }
    }
  }
  return 0;
}

void *handle_requests(void *arg) {
  Handle_Request_Args *args = (Handle_Request_Args *)arg;
  int buffer_size = 0;
  int tcp_client_fd = args->tcp_client_fd;
  char buffer[TCP_RTSP_BUFFER_SIZE];
  char *rtsp_relay_server_ip = args->rtsp_relay_server_ip;

  int rtp_port;
  int rtcp_port;
  char rtp_port_char[12];
  char rtcp_port_char[12];
  memset(rtp_port_char, 0, 12);
  memset(rtcp_port_char, 0, 12);

  while (1) {
    if ((buffer_size =
             recv(tcp_client_fd, buffer, TCP_RTSP_BUFFER_SIZE - 1, 0)) > 0) {
      buffer[buffer_size] = '\0';
      char method[10];
      memset(method, 0, sizeof(method));

      get_method(buffer, buffer_size, method);

      if (strcmp(method, "OPTIONS") == 0) {
        options(buffer, tcp_client_fd);
      } else if (strcmp(method, "DESCRIBE") == 0) {
        describe(buffer, tcp_client_fd, args->recording, rtsp_relay_server_ip);
      } else if (strcmp(method, "SETUP") == 0) {
        printf("%s\n\n", buffer);
        get_udp_client_ports(buffer, buffer_size, &rtp_port, &rtcp_port,
                             rtp_port_char, rtcp_port_char);
        setup(buffer, tcp_client_fd, args->recording, rtp_port, rtcp_port,
              rtp_port_char, rtcp_port_char, args->udp_rtp_client_fd,
              args->udp_rtcp_client_fd, args->udp_rtp_client_addr,
              args->udp_rtcp_client_addr);
      } else if (strcmp(method, "ANNOUNCE") == 0) {
        announce(buffer, tcp_client_fd);
      } else if (strcmp(method, "RECORD") == 0) {
        record(buffer, tcp_client_fd, args->play, args->recording,
               args->udp_rtp_server_fd, args->udp_rtcp_server_fd,
               args->udp_rtp_client_fd, args->udp_rtcp_client_fd,
               args->udp_rtp_client_addr, args->udp_rtcp_client_addr,
               args->udp_rtp_client_addr_size, args->udp_rtcp_client_addr_size);
      } else if (strcmp(method, "PLAY") == 0) {
        play(buffer, tcp_client_fd, args->play);
      }
    }
    memset(buffer, 0, sizeof(buffer));
  }
}

int main(int argc, char **argv) {
  setbuf(stdout, NULL); // disable buffering to allow printing to file

  if (argc < 2) {
    printf("Must pass relay server public IP Address\n\n");
    return EXIT_FAILURE;
  }

  char rtsp_relay_server_ip[16];
  memset(rtsp_relay_server_ip, 0, sizeof(rtsp_relay_server_ip));
  for (int x = 0; argv[1][x] != '\0'; x++) {
    rtsp_relay_server_ip[x] = argv[1][x];
  }
  rtsp_relay_server_ip[15] = '\0';

  printf("RELAY IP: %s\n\n", rtsp_relay_server_ip);

  int tcp_client_fds[max_clients] = {0};
  pthread_t threads[max_clients] = {0};
  int recording = 0;
  int play = 0;

  memset(threads, 0, sizeof(threads));
  memset(tcp_client_fds, 0, sizeof(tcp_client_fds));

  int tcp_rtsp_server_fd;
  int udp_rtp_client_fd;
  int udp_rtcp_client_fd;
  int udp_rtp_server_fd;
  int udp_rtcp_server_fd;

  struct sockaddr_in tcp_rtsp_server_addr;
  struct sockaddr_in tcp_rtsp_client_addr;
  struct sockaddr_in udp_rtp_server_addr;
  struct sockaddr_in udp_rtcp_server_addr;
  struct sockaddr_in udp_rtp_client_addr;
  struct sockaddr_in udp_rtcp_client_addr;

  memset(&tcp_rtsp_server_addr, 0, sizeof(tcp_rtsp_server_addr));
  memset(&tcp_rtsp_server_addr, 0, sizeof(tcp_rtsp_server_addr));
  memset(&udp_rtp_client_addr, 0, sizeof(udp_rtp_client_addr));
  memset(&udp_rtcp_client_addr, 0, sizeof(udp_rtcp_client_addr));
  memset(&udp_rtp_server_addr, 0, sizeof(udp_rtp_server_addr));
  memset(&udp_rtcp_server_addr, 0, sizeof(udp_rtcp_server_addr));

  socklen_t tcp_rtsp_client_addr_size = sizeof(tcp_rtsp_client_addr);
  socklen_t udp_rtp_client_addr_size = sizeof(udp_rtp_client_addr);
  socklen_t udp_rtcp_client_addr_size = sizeof(udp_rtcp_client_addr);

  int opt = 1;

  tcp_rtsp_server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (tcp_rtsp_server_fd < 0) {
    perror("Error creating socket");
    return -1;
  }

  udp_rtp_server_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (udp_rtp_server_fd < 0) {
    perror("Error creating socket");
    return -1;
  }

  udp_rtcp_server_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (udp_rtcp_server_fd < 0) {
    perror("Error creating socket");
    return -1;
  }

  if (setsockopt(tcp_rtsp_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt,
                 sizeof(opt)) < 0) {
    perror("Error setting socket options");
    close(tcp_rtsp_server_fd);
    close(udp_rtp_server_fd);
    close(udp_rtcp_server_fd);
    return -1;
  }

  memset(&tcp_rtsp_server_addr, 0, sizeof(tcp_rtsp_server_addr));
  tcp_rtsp_server_addr.sin_family = AF_INET;
  tcp_rtsp_server_addr.sin_addr.s_addr = INADDR_ANY;
  tcp_rtsp_server_addr.sin_port = htons(PORT);

  memset(&udp_rtp_server_addr, 0, sizeof(udp_rtp_server_addr));
  udp_rtp_server_addr.sin_family = AF_INET;
  udp_rtp_server_addr.sin_addr.s_addr = INADDR_ANY;
  udp_rtp_server_addr.sin_port = htons(UDP_PORT);

  memset(&udp_rtcp_server_addr, 0, sizeof(udp_rtcp_server_addr));
  udp_rtcp_server_addr.sin_family = AF_INET;
  udp_rtcp_server_addr.sin_addr.s_addr = INADDR_ANY;
  udp_rtcp_server_addr.sin_port = htons(UDP_RTCP_PORT);

  if (bind(tcp_rtsp_server_fd, (struct sockaddr *)&tcp_rtsp_server_addr,
           sizeof(tcp_rtsp_server_addr)) < 0) {
    perror("Error binding socket to server address");
    close(tcp_rtsp_server_fd);
    close(udp_rtp_server_fd);
    close(udp_rtcp_server_fd);
    return -1;
  }

  if (bind(udp_rtp_server_fd, (struct sockaddr *)&udp_rtp_server_addr,
           sizeof(udp_rtp_server_addr)) < 0) {
    perror("Error binding socket to udp server address");
    close(tcp_rtsp_server_fd);
    close(udp_rtp_server_fd);
    close(udp_rtcp_server_fd);
    return -1;
  }

  if (bind(udp_rtcp_server_fd, (struct sockaddr *)&udp_rtcp_server_addr,
           sizeof(udp_rtcp_server_addr)) < 0) {
    perror("Error binding socket to control port address");
    close(tcp_rtsp_server_fd);
    close(udp_rtp_server_fd);
    close(udp_rtcp_server_fd);
    return -1;
  }

  if (listen(tcp_rtsp_server_fd, max_clients) < 0) {
    perror("Error listening for server connections");
    close(tcp_rtsp_server_fd);
    close(udp_rtp_server_fd);
    close(udp_rtcp_server_fd);
    return -1;
  }

  printf("Listening for connections on:  %s:%d\n\n", rtsp_relay_server_ip,
         PORT);

  while (1) {
    printf("Getting connections...\n");

    for (int client_fd_index = 0; client_fd_index < max_clients;
         client_fd_index++) {
      if (tcp_client_fds[client_fd_index] == 0) {
        printf("Connection file descriptor: %d\n",
               tcp_client_fds[client_fd_index]);

        // create pthread here no need to have pthreads in an array
        pthread_t thread;

        // getting rtsp tcp data
        tcp_client_fds[client_fd_index] =
            accept(tcp_rtsp_server_fd, (struct sockaddr *)&tcp_rtsp_client_addr,
                   &tcp_rtsp_client_addr_size);

        if (tcp_client_fds[client_fd_index] < 0) {
          perror("Error connecting...\n");
          continue;
        }

        printf("Accepted Connection on file descriptor: %d\n",
               tcp_client_fds[client_fd_index]);

        Handle_Request_Args args;
        memset(&args, 0, sizeof(args));

        args.tcp_client_fd = tcp_client_fds[client_fd_index];
        args.play = &play;
        args.recording = &recording;
        args.udp_rtp_server_fd = udp_rtp_server_fd;
        args.udp_rtcp_server_fd = udp_rtcp_server_fd;

        args.udp_rtp_client_fd = &udp_rtp_client_fd;
        args.udp_rtp_client_addr = &udp_rtp_client_addr;
        args.udp_rtp_client_addr_size = udp_rtp_client_addr_size;

        args.udp_rtcp_client_fd = &udp_rtcp_client_fd;
        args.udp_rtcp_client_addr = &udp_rtcp_client_addr;
        args.udp_rtcp_client_addr_size = udp_rtcp_client_addr_size;
        args.rtsp_relay_server_ip = rtsp_relay_server_ip;

        pthread_create(&thread, NULL, handle_requests, &args);
      } else {
        printf("client already listening on file descriptor: %d\n",
               tcp_client_fds[client_fd_index]);
      }
    }
  }

  close(tcp_rtsp_server_fd);
  close(udp_rtp_client_fd);
  close(udp_rtcp_client_fd);
  close(udp_rtp_server_fd);
  close(udp_rtcp_server_fd);

  return 0;
}
