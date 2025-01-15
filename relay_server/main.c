#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PORT 8081
#define UDP_PORT 8082
#define UDP_RTCP_PORT 8083
#define TCP_RTSP_BUFFER_SIZE 2048
#define STREAM_BUFFER_SIZE 1500
#define SESSION_ID_SIZE 22
#define max_clients                                                            \
  12 // 2 clients signify 1 e2e (stream<->client) rtsp streaming session
     // TODO: Make this an argument when starting the server

// The following command records the live stream from initial client
/* (macos)
 ffmpeg -re -loglevel debug -f \
 avfoundation -framerate 30 -video_size 640x360 -i \
 0 -fflags +genpts \
 -c:v libx264 \
 -x264-params "keyint=60:no-scenecut=1" \
 -f rtsp rtsp://public_ip:8081
*/

// The following command plays live stream from  client
/* (agnostic)
  ffplay -loglevel debug -v verbose \
  -vcodec h264 -rtsp_transport udp -i \
  rtsp://127.0.0.1:8081
 */

typedef struct {
  int *play;
  int *recording;
  int *udp_rtp_client_fd;
  int *udp_rtcp_client_fd;
  int *stream_num;
  int tcp_client_fd;
  int udp_rtp_server_fd;
  int udp_rtcp_server_fd;
  int udp_rtp_port;
  int udp_rtcp_port;
  struct sockaddr_in *udp_rtp_client_addr;
  struct sockaddr_in *udp_rtcp_client_addr;
  socklen_t udp_rtp_client_addr_size;
  socklen_t udp_rtcp_client_addr_size;
  char *rtsp_relay_server_ip;
  char *sdp;
  char *session_id;
} Handle_Request_Args;

typedef struct {
  int play;
  int recording;
  int tcp_client_fd;
  int udp_rtp_server_fd;
  int udp_rtcp_server_fd;
  int udp_rtp_client_fd;
  int udp_rtcp_client_fd;
  int udp_rtp_port;
  int udp_rtcp_port;
  struct sockaddr_in udp_rtp_client_addr;
  struct sockaddr_in tcp_rtsp_client_addr;
  struct sockaddr_in udp_rtcp_client_addr;
  struct sockaddr_in udp_rtp_server_addr;
  struct sockaddr_in udp_rtcp_server_addr;
  socklen_t udp_rtp_client_addr_size;
  socklen_t tcp_rtsp_client_addr_size;
  socklen_t udp_rtcp_client_addr_size;
  char *rtsp_relay_server_ip;
  char sdp[300];
  char session_id[SESSION_ID_SIZE];
} RTSP_Session;

struct stream_data {
  int udp_server_fd;
  int udp_client_fd;
  struct sockaddr_in udp_client_addr;
  socklen_t udp_client_addr_size;
  int client_fd;
};

// HELPERS
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

void get_method(char *buffer, int buffer_size, char *method) {
  for (int x = 0; x < buffer_size; x++) {
    if (buffer[x] == ' ') {
      method[x] = '\0';
      break;
    }
    method[x] = buffer[x];
  }
}

// TODO: Need to support double/triple/etc digit numbers
void get_cseq(char *buffer, int buffer_size, char *cseq) {
  for (int x = 0; x < buffer_size; x++) {
    if (x + 6 < buffer_size) {
      if (buffer[x] == 'C' && buffer[x + 1] == 'S' && buffer[x + 2] == 'e' &&
          buffer[x + 3] == 'q') {
        cseq[0] = buffer[x + 6];
      }
    }
  }
}

void get_sprop(char *sprop, char *buffer, int buffer_size) {
  printf("getting sdp\n");
  int z = 0;
  for (int x = 0; x < buffer_size; x++) {
    if (buffer[x] == 'a' && buffer[x + 1] == '=' && buffer[x + 2] == 'f') {
      for (int y = 0; buffer[x + y] != '\r'; y++) {
        z = y;
        sprop[y] = buffer[x + y];
      }
      sprop[z + 1] = '\0';
      break;
    }
  }
  printf("Do I make it out\n");
}

int create_client_udp_fd(int port, struct sockaddr_in *udp_client_addr) {
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

// parses setup request to get udp client ports
int get_udp_client_ports(char *buffer, int buffer_size, int *rtp_port,
                         int *rtcp_port, char *rtp_port_char,
                         char *rtcp_port_char) {

  /*
   * This is for a client connecting
   SETUP rtsp://192.168.1.29:8081/streamid=0 RTSP/1.0
   Transport: RTP/AVP/UDP;unicast;client_port=26836-26837
  */

  /*
   * This is for a client streaming
  SETUP rtsp://192.168.1.29:8081/streamid=0 RTSP/1.0
  Transport: RTP/AVP/UDP;unicast;client_port=31940-31941;mode=record
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

void generate_session_id(char *session_id) {
  printf("Generating Session Identifier\n");
  // include fnctl.h for access to open and read functions
  // https://en.wikipedia.org/wiki//dev/random

  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    perror("Error opening /dev/urandom");
    return;
  }

  char *alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  char buffer[SESSION_ID_SIZE];

  if (read(fd, buffer, sizeof(buffer)) < 0) {
    perror("Error reading /dev/urandom/");
    close(fd);
    return;
  }

  close(fd);

  for (int x = 0; x < SESSION_ID_SIZE; x++) {
    int masked_bit = buffer[x] & 0x3f;
    session_id[x] = alphabet[masked_bit];
  }
}

void spawn_rtsp_client(int stream_num) {
  printf("Spawning Client...\n");

  char *local_rtsp_address = "rtsp://127.0.0.1:8081";
  char *args[] = {
      "ffmpeg", // Program name
      "-re",    // Real-time mode
      "-loglevel",
      "debug", // Debug logging
      "-rtsp_transport",
      "udp", // Use UDP for RTSP
      "-i",
      local_rtsp_address, // Input RTSP URL
      "-vf",
      "fps=30", // Video filter: 30 fps
      "-c:v",
      "libx264", // Video codec
      "-preset",
      "veryfast", // Preset for encoding
      "-g",
      "30", // GOP size
      "-c:a",
      "aac", // Audio codec
      "-b:a",
      "128k", // Audio bitrate
      "-f",
      "hls", // Format: HLS
      "-hls_time",
      "4", // HLS segment time
      "-hls_segment_filename",
      "segment_%03d.ts", // HLS segment file names
      "output.m3u8",     // HLS output file
      NULL               // Null-terminator for the execv arguments
  };

  chdir("../stream_server/public");
  char rm_cmd[20];
  char stream[2];
  snprintf(stream, sizeof(stream), "%d", stream_num);

  strcat(rm_cmd, "rm -rf");
  strcat(rm_cmd, stream);
  strcat(rm_cmd, "\0");
  system(rm_cmd);

  mkdir(stream, 0775);
  chdir(stream);

  // Replace the current process with the ffplay command
  execvp("ffmpeg", args);

  // If execvp fails, print an error
  perror("execvp failed");
}

// STREAM
void *stream_protocol(void *data) {
  struct stream_data *sd = (struct stream_data *)data;
  struct sockaddr_in udp_client_addr = sd->udp_client_addr;
  struct sockaddr_in udp_sender_addr;
  int udp_server_fd = sd->udp_server_fd;
  int udp_client_fd = sd->udp_client_fd;
  int play_message_sent = 0;
  int bytes = 0;
  socklen_t udp_client_addr_size = sd->udp_client_addr_size;
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

    bytes =
        recvfrom(udp_server_fd, buffer, STREAM_BUFFER_SIZE, 0,
                 (struct sockaddr *)&udp_sender_addr, &udp_sender_addr_size);

    if (bytes < 0) {
      perror("failed to get bytes");
      continue;
    } else {
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
            socklen_t udp_rtcp_client_addr_size, int *recording,
            int client_fd) {
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
      rtp_data.client_fd = client_fd;

      struct stream_data rtcp_data;
      rtcp_data.udp_server_fd = udp_rtcp_server_fd;
      rtcp_data.udp_client_fd = *udp_rtcp_client_fd;
      rtcp_data.udp_client_addr = *udp_rtcp_client_addr;
      rtcp_data.udp_client_addr_size = udp_rtcp_client_addr_size;
      rtcp_data.client_fd = client_fd;

      pthread_create(&rtp_thread, NULL, stream_protocol, &rtp_data);
      pthread_create(&rtcp_thread, NULL, stream_protocol, &rtcp_data);
      threads_waiting = 0;
      // this resets us allowing for other clients to connect on different ports
      *play = 0;
      *recording = 0;
    }
  }
}

// RTSP METHODS
void record(char *buffer, int client_fd, Handle_Request_Args *args, char *cseq,
            int *stream_num, char *session_id) {
  printf("REQUEST:\n%s\n\n", buffer);

  int *play = args->play;
  int *recording = args->recording;
  int udp_rtp_server_fd = args->udp_rtp_server_fd;
  int udp_rtcp_server_fd = args->udp_rtcp_server_fd;
  int *udp_rtp_client_fd = args->udp_rtp_client_fd;
  int *udp_rtcp_client_fd = args->udp_rtcp_client_fd;
  struct sockaddr_in *udp_rtp_client_addr = args->udp_rtp_client_addr;
  struct sockaddr_in *udp_rtcp_client_addr = args->udp_rtcp_client_addr;
  socklen_t udp_rtp_client_addr_size = args->udp_rtp_client_addr_size;
  socklen_t udp_rtcp_client_addr_size = args->udp_rtcp_client_addr_size;

  char time_buffer[200];
  char response_date[87];
  char record_response[512];
  memset(record_response, 0, sizeof(record_response));
  memset(response_date, 0, sizeof(response_date));
  memset(record_response, 0, sizeof(record_response));

  strcat(record_response, "RTSP/1.0 200 OK\r\n");
  strcat(record_response, "CSeq: ");
  strcat(record_response, cseq);
  strcat(record_response, "\r\n");
  strcat(record_response, "Session:");
  strcat(record_response, session_id);
  strcat(record_response, "\r\n");
  get_date(time_buffer, sizeof(time_buffer));
  strcat(record_response, time_buffer);
  strcat(record_response, "\r\n\r\n");

  printf("RESPONSE:\n%s\n\n", record_response);
  send(client_fd, record_response, strlen(record_response), 0);
  *recording = 1;
  *stream_num += 1;

  /*
  TODO: fork process
  ffmpeg clients being invoked from this process should be the only clients
  that can view the stream
  execute the ffmpeg rtsp client command to convert stream data to hls
  use the index of running cameras as the camera number to save hls output
  too
  1. mkdir -p <path_to_client_public_dir>/camera-<index>
  2. Generate unique username and password and set data in pointer for
  current camera.
  - Need to implement digest auth:
      - (https://www.rfc-editor.org/rfc/rfc7826#section-19.1.1)
  - The server will use the username and password stored in the pointers
  described above to determine if it should allow the client access
 3.
 ffmpeg - re - loglevel debug - rtsp_transport udp -i \
 rtsp://127.0.0.1:8081 \
 -vf fps=30 \
 -c:v libx264 -preset veryfast -g 30 \
 -c:a aac -b:a 128k \
 -f hls -hls_time 4 -hls_segment_filename "segment_%03d.ts" \
 <path_to_client_public_dir>/camera-<index>/output.m3u8
 4. Set pointer that camera started streaming
  */

  int pid = fork();

  if (pid == 0) {
    spawn_rtsp_client(*stream_num);
  } else if (pid != -1) {
    stream(play, udp_rtp_server_fd, udp_rtcp_server_fd, udp_rtp_client_fd,
           udp_rtcp_client_fd, udp_rtp_client_addr, udp_rtp_client_addr_size,
           udp_rtcp_client_addr, udp_rtcp_client_addr_size, recording,
           client_fd);
  } else {
    perror("Error during client fork");
  }
}

void play(char *buffer, int client_fd, int *play, char *cseq,
          char *session_id) {
  printf("PLAY REQUEST:\n%s\n\n", buffer);

  char play_response[300];
  memset(play_response, 0, sizeof(play_response));

  strcat(play_response, "RTSP/1.0 200 OK\r\n");
  strcat(play_response, "CSeq: ");
  strcat(play_response, cseq);
  strcat(play_response, "\r\n");
  strcat(play_response, "Range: npt=0.000-\r\n"); // LIVE
  strcat(play_response, "Session:");
  strcat(play_response, session_id);
  strcat(play_response, "\r\n\r\n");

  printf("PLAY RESPONSE to client_fd %d:\n%s\n\n", client_fd, play_response);
  send(client_fd, play_response, strlen(play_response), 0);

  *play = 1;
}

void announce(char *buffer, int client_fd, char *cseq, char *session_id) {
  printf("REQUEST:\n%s\n\n", buffer);

  char announce_response[100];
  memset(announce_response, 0, sizeof(announce_response));

  strcat(announce_response, "RTSP/1.0 200 OK\r\n");
  strcat(announce_response, "CSeq: ");
  strcat(announce_response, cseq);
  strcat(announce_response, "\r\n\r\n");

  printf("RESPONSE:\n%s\n\n", announce_response);
  send(client_fd, announce_response, strlen(announce_response), 0);
}

// udp
void setup(char *buffer, int rtp_port, int rtcp_port, char *rtp_port_char,
           char *rtcp_port_char, Handle_Request_Args *args, char *cseq) {
  printf("REQUEST:\n%s\n\n", buffer);

  generate_session_id(args->session_id);

  int tcp_client_fd = args->tcp_client_fd;
  int *recording = args->recording;
  int *udp_rtp_client_fd = args->udp_rtp_client_fd;
  int *udp_rtcp_client_fd = args->udp_rtcp_client_fd;
  struct sockaddr_in *udp_rtp_client_addr = args->udp_rtp_client_addr;
  struct sockaddr_in *udp_rtcp_client_address = args->udp_rtcp_client_addr;
  char setup_response[300];
  char udp_rtp_port[5];
  char udp_rtcp_port[5];

  memset(setup_response, 0, sizeof(setup_response));
  memset(udp_rtp_port, 0, sizeof(udp_rtp_port));
  memset(udp_rtcp_port, 0, sizeof(udp_rtcp_port));

  snprintf(udp_rtp_port, sizeof(udp_rtp_port), "%d", args->udp_rtp_port);
  snprintf(udp_rtcp_port, sizeof(udp_rtcp_port), "%d", args->udp_rtcp_port);

  strcat(setup_response, "RTSP/1.0 200 OK\r\n");
  strcat(setup_response, "CSeq: ");
  strcat(setup_response, cseq);
  strcat(setup_response, "\r\n");
  strcat(setup_response, "Transport: RTP/AVP/UDP;unicast;client_port=");
  strcat(setup_response, rtp_port_char);
  strcat(setup_response, "-");
  strcat(setup_response, rtcp_port_char);
  strcat(setup_response, ";server_port=");
  strcat(setup_response, udp_rtp_port);
  strcat(setup_response, "-");
  strcat(setup_response, udp_rtcp_port);
  strcat(setup_response, "\r\n");
  strcat(setup_response, "Session:");
  strcat(setup_response, args->session_id);
  strcat(setup_response, "\r\n\r\n");

  if (*recording == 1) {
    *udp_rtp_client_fd = create_client_udp_fd(rtp_port, udp_rtp_client_addr);
    *udp_rtcp_client_fd =
        create_client_udp_fd(rtcp_port, udp_rtcp_client_address);
  }

  printf("RESPONSE:\n%s\n\n", setup_response);
  send(tcp_client_fd, setup_response, strlen(setup_response), 0);
}

void describe(char *buffer, Handle_Request_Args *args, char *cseq) {
  // Message Syntax: https://www.rfc-editor.org/rfc/rfc7826#section-20.2.2
  // Header Syntax: https://www.rfc-editor.org/rfc/rfc7826#section-20.2.3

  printf("REQUEST:\n%s\n\n", buffer);

  int tcp_client_fd = args->tcp_client_fd;
  int *recording = args->recording;
  char *rtsp_relay_server_ip = args->rtsp_relay_server_ip;
  char *sdp = args->sdp;

  char describe_response[500];
  char port[10];
  char describe_content[500];
  char content_length[5];
  memset(describe_response, 0, sizeof(describe_response));
  memset(describe_content, 0, sizeof(describe_content));
  memset(content_length, 0, sizeof(content_length));
  memset(port, 0, sizeof(port));

  snprintf(port, sizeof(port), "%d", PORT);

  strcat(describe_response, "RTSP/1.0 200 OK\r\n");
  strcat(describe_response, "CSeq: ");
  strcat(describe_response, cseq);
  strcat(describe_response, "\r\n");
  strcat(describe_response, "Content-Base: rtsp://");
  strcat(describe_response, rtsp_relay_server_ip);
  strcat(describe_response, ":");
  strcat(describe_response, port);
  strcat(describe_response, "\r\n");
  strcat(describe_response, "Content-Type: application/sdp\r\n");

  if (*recording == 1) {
    strcat(describe_content, "v=0\r\n");
    strcat(describe_content, "o=- 0 0 IN IP4 ");
    strcat(describe_content, rtsp_relay_server_ip);
    strcat(describe_content, "\r\n");
    strcat(describe_content, "s=RTSP Session\r\n");
    strcat(describe_content, "c=IN IP4 ");
    strcat(describe_content, rtsp_relay_server_ip);
    strcat(describe_content, "\r\n");
    strcat(describe_content, "t=0 0\r\n");
    strcat(describe_content, "m=video 0 RTP/AVP 96\r\n");
    strcat(describe_content, "a=rtpmap:96 H264/90000\r\n");
    strcat(describe_content, sdp);
    strcat(describe_content, "\r\n");
    strcat(describe_content, "a=control:streamid=0\r\n");

    snprintf(content_length, sizeof(content_length), "%lu",
             strlen(describe_content));

    strcat(describe_response, "Content-Length: ");
    strcat(describe_response, content_length);
    strcat(describe_response, "\r\n\r\n");
    strcat(describe_response, describe_content);
  }

  printf("RESPONSE:\n%s\n\n", describe_response);
  send(tcp_client_fd, describe_response, strlen(describe_response), 0);
}

void options(char *buffer, int client_fd, char *cseq) {
  printf("REQUEST:\n%s\n\n", buffer);

  char options_response[100];
  memset(options_response, 0, sizeof(options_response));

  strcat(options_response, "RTSP/1.0 200 OK\r\n");
  strcat(options_response, "CSeq: ");
  strcat(options_response, cseq);
  strcat(options_response, "\r\n");
  strcat(options_response, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, "
                           "PAUSE, RECORD, TEARDOWN, ANNOUNCE\r\n\r\n");

  printf("RESPONSE to client_fd %d:\n%s\n\n", client_fd, options_response);
  send(client_fd, options_response, strlen(options_response), 0);
}

// RTSP ROUTER
void *handle_requests(void *arg) {
  Handle_Request_Args *args = (Handle_Request_Args *)arg;
  int buffer_size = 0;
  int rtp_port;
  int rtcp_port;
  char buffer[TCP_RTSP_BUFFER_SIZE];
  char rtp_port_char[12];
  char rtcp_port_char[12];

  memset(buffer, 0, sizeof(buffer));
  memset(rtp_port_char, 0, 12);
  memset(rtcp_port_char, 0, 12);

  printf("TCP CLIENT FD: %d\n\n", args->tcp_client_fd);

  while (1) {
    if ((buffer_size = recv(args->tcp_client_fd, buffer,
                            TCP_RTSP_BUFFER_SIZE - 1, 0)) > 0) {
      buffer[buffer_size] = '\0';
      char method[10];
      memset(method, 0, sizeof(method));

      get_method(buffer, buffer_size, method);

      char cseq[2];
      memset(cseq, 0, sizeof(cseq));

      // dynamically adding sprop from rtsp client
      if (buffer[0] == 'v' && buffer[1] == '=') {
        get_sprop(args->sdp, buffer, buffer_size);
      } else {
        get_cseq(buffer, buffer_size, cseq);
        cseq[1] = '\0';
      }

      if (strcmp(method, "OPTIONS") == 0) {
        options(buffer, args->tcp_client_fd, cseq);
      } else if (strcmp(method, "DESCRIBE") == 0) {
        describe(buffer, args, cseq);
      } else if (strcmp(method, "SETUP") == 0) {
        get_udp_client_ports(buffer, buffer_size, &rtp_port, &rtcp_port,
                             rtp_port_char, rtcp_port_char);
        setup(buffer, rtp_port, rtcp_port, rtp_port_char, rtcp_port_char, args,
              cseq);
      } else if (strcmp(method, "ANNOUNCE") == 0) {
        announce(buffer, args->tcp_client_fd, cseq, args->session_id);
      } else if (strcmp(method, "RECORD") == 0) {
        record(buffer, args->tcp_client_fd, args, cseq, args->stream_num,
               args->session_id);
      } else if (strcmp(method, "PLAY") == 0) {
        play(buffer, args->tcp_client_fd, args->play, cseq, args->session_id);
      }
    }
    memset(buffer, 0, sizeof(buffer));
  }
}

int main(int argc, char **argv) {
  setbuf(stdout, NULL); // disable buffering to allow printing to file
  // TODO: Digest and Basic auth should be used by both the cameras streaming
  // the rtsp data and clients invoked from this server to view the stream and
  // convert data to hls
  // Clients streaming data to this server should send rtsp over tls
  // Considering the clients viewing the stream can only be invoked from this
  // server, basic and digest authentication should suffice for said clients
  // The media hls stream will be over tls/https and only viewable from a
  // specific ip defined by the (vpn's public ip address)

  if (argc < 2) {
    printf("Must pass relay server public IP Address\n\n");
    return EXIT_FAILURE;
  }

  int streaming_set = 0;

  char rtsp_relay_server_ip[16];
  memset(rtsp_relay_server_ip, 0, sizeof(rtsp_relay_server_ip));
  for (int x = 0; argv[1][x] != '\0'; x++) {
    rtsp_relay_server_ip[x] = argv[1][x];
  }

  int tcp_client_fds[max_clients] = {0};
  int stream_num = 0;
  RTSP_Session sessions[max_clients];
  Handle_Request_Args args[max_clients];
  memset(args, 0, sizeof(args));

  sessions[0].recording = 0;
  sessions[0].play = 0;

  memset(tcp_client_fds, 0, sizeof(tcp_client_fds));

  int tcp_rtsp_server_fd;

  struct sockaddr_in tcp_rtsp_server_addr;

  memset(&tcp_rtsp_server_addr, 0, sizeof(tcp_rtsp_server_addr));
  memset(&sessions[0].tcp_rtsp_client_addr, 0,
         sizeof(sessions[0].tcp_rtsp_client_addr));
  memset(&sessions[0].udp_rtp_client_addr, 0,
         sizeof(sessions[0].udp_rtp_client_addr));
  memset(&sessions[0].udp_rtcp_client_addr, 0,
         sizeof(sessions[0].udp_rtcp_client_addr));
  memset(&sessions[0].udp_rtp_server_addr, 0,
         sizeof(sessions[0].udp_rtp_server_addr));
  memset(&sessions[0].udp_rtcp_server_addr, 0,
         sizeof(sessions[0].udp_rtcp_server_addr));

  sessions[0].tcp_rtsp_client_addr_size =
      sizeof(sessions[0].tcp_rtsp_client_addr);
  sessions[0].udp_rtp_client_addr_size =
      sizeof(sessions[0].udp_rtp_client_addr);
  sessions[0].udp_rtcp_client_addr_size =
      sizeof(sessions[0].udp_rtcp_client_addr);
  int opt = 1;

  tcp_rtsp_server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (tcp_rtsp_server_fd < 0) {
    perror("Error creating socket");
    return -1;
  }

  sessions[0].udp_rtp_server_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sessions[0].udp_rtp_server_fd < 0) {
    perror("Error creating socket");
    return -1;
  }

  sessions[0].udp_rtcp_server_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sessions[0].udp_rtcp_server_fd < 0) {
    perror("Error creating socket");
    return -1;
  }

  if (setsockopt(tcp_rtsp_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt,
                 sizeof(opt)) < 0) {
    perror("Error setting socket options");
    close(tcp_rtsp_server_fd);
    close(sessions[0].udp_rtp_client_fd);
    close(sessions[0].udp_rtcp_client_fd);
    close(sessions[0].udp_rtp_server_fd);
    close(sessions[0].udp_rtcp_server_fd);
    return -1;
  }

  tcp_rtsp_server_addr.sin_family = AF_INET;
  tcp_rtsp_server_addr.sin_addr.s_addr = INADDR_ANY;
  tcp_rtsp_server_addr.sin_port = htons(PORT);

  sessions[0].udp_rtp_server_addr.sin_family = AF_INET;
  sessions[0].udp_rtp_server_addr.sin_addr.s_addr = INADDR_ANY;
  sessions[0].udp_rtp_server_addr.sin_port = htons(UDP_PORT);
  sessions[0].udp_rtp_port = UDP_PORT;

  sessions[0].udp_rtcp_server_addr.sin_family = AF_INET;
  sessions[0].udp_rtcp_server_addr.sin_addr.s_addr = INADDR_ANY;
  sessions[0].udp_rtcp_server_addr.sin_port = htons(UDP_RTCP_PORT);
  sessions[0].udp_rtcp_port = UDP_RTCP_PORT;

  if (bind(sessions[0].udp_rtp_server_fd,
           (struct sockaddr *)&sessions[0].udp_rtp_server_addr,
           sizeof(sessions[0].udp_rtp_server_addr)) < 0) {
    perror("Error binding socket to udp server address");
    close(tcp_rtsp_server_fd);
    close(sessions[0].udp_rtp_client_fd);
    close(sessions[0].udp_rtcp_client_fd);
    close(sessions[0].udp_rtp_server_fd);
    close(sessions[0].udp_rtcp_server_fd);
    return -1;
  }

  if (bind(sessions[0].udp_rtcp_server_fd,
           (struct sockaddr *)&sessions[0].udp_rtcp_server_addr,
           sizeof(sessions[0].udp_rtcp_server_addr)) < 0) {
    perror("Error binding socket to control port address");
    close(tcp_rtsp_server_fd);
    close(sessions[0].udp_rtp_client_fd);
    close(sessions[0].udp_rtcp_client_fd);
    close(sessions[0].udp_rtp_server_fd);
    close(sessions[0].udp_rtcp_server_fd);
    return -1;
  }

  if (bind(tcp_rtsp_server_fd, (struct sockaddr *)&tcp_rtsp_server_addr,
           sizeof(tcp_rtsp_server_addr)) < 0) {
    perror("Error binding socket to server address");
    close(tcp_rtsp_server_fd);
    close(sessions[0].udp_rtp_client_fd);
    close(sessions[0].udp_rtcp_client_fd);
    close(sessions[0].udp_rtp_server_fd);
    close(sessions[0].udp_rtcp_server_fd);
    return -1;
  }

  if (listen(tcp_rtsp_server_fd, max_clients) < 0) {
    perror("Error listening for server connections");
    close(tcp_rtsp_server_fd);
    close(sessions[0].udp_rtp_client_fd);
    close(sessions[0].udp_rtcp_client_fd);
    close(sessions[0].udp_rtp_server_fd);
    close(sessions[0].udp_rtcp_server_fd);
    return -1;
  }

  printf("Listening for connections on:  %s:%d\n\n", rtsp_relay_server_ip,
         PORT);

  /*
   * Each camera connecting can use port 8081 for rtsp
   * (tcp communication handshake)
   * But new udp ports need to be used for each camera
   * (server udp rtp and rtcp ports)
   * separate thread of execution for each
   */

  int connections = 0;
  int session_index = 0;

  while (1) {
    printf("Getting connections...\n");

    for (int client_fd_index = 0; client_fd_index < max_clients;
         client_fd_index++) {
      if (tcp_client_fds[client_fd_index] == 0) {
        printf("CONNECTIONS: %d\n\n", connections);
        if (connections > 0 && connections % 2 == 0) {
          printf("STREAMING_SET\n\n");
          memset(&sessions[client_fd_index].tcp_rtsp_client_addr, 0,
                 sizeof(sessions[client_fd_index].tcp_rtsp_client_addr));
          memset(&sessions[client_fd_index].udp_rtp_server_addr, 0,
                 sizeof(sessions[client_fd_index].udp_rtp_server_addr));
          memset(&sessions[client_fd_index].udp_rtcp_server_addr, 0,
                 sizeof(sessions[client_fd_index].udp_rtcp_server_addr));
          memset(&sessions[client_fd_index].udp_rtcp_client_addr, 0,
                 sizeof(sessions[client_fd_index].udp_rtcp_client_addr));
          memset(&sessions[client_fd_index].udp_rtp_client_addr, 0,
                 sizeof(sessions[client_fd_index].udp_rtp_client_addr));

          sessions[client_fd_index].recording = 0;
          sessions[client_fd_index].play = 0;
          sessions[client_fd_index].tcp_rtsp_client_addr_size =
              sizeof(sessions[client_fd_index].tcp_rtsp_client_addr);

          sessions[client_fd_index].udp_rtp_server_fd =
              socket(AF_INET, SOCK_DGRAM, 0);
          if (sessions[client_fd_index].udp_rtp_server_fd < 0) {
            perror("Error creating socket");
            return -1;
          }

          sessions[client_fd_index].udp_rtcp_server_fd =
              socket(AF_INET, SOCK_DGRAM, 0);
          if (sessions[client_fd_index].udp_rtcp_server_fd < 0) {
            perror("Error creating socket");
            return -1;
          }

          // SERVER UDP RTP
          sessions[client_fd_index].udp_rtp_server_addr.sin_family = AF_INET;
          sessions[client_fd_index].udp_rtp_server_addr.sin_addr.s_addr =
              INADDR_ANY;
          sessions[client_fd_index].udp_rtp_server_addr.sin_port =
              htons(UDP_PORT + client_fd_index);
          sessions[client_fd_index].udp_rtp_port = UDP_PORT + client_fd_index;

          // SERVER UDP RTCP
          sessions[client_fd_index].udp_rtcp_server_addr.sin_family = AF_INET;
          sessions[client_fd_index].udp_rtcp_server_addr.sin_addr.s_addr =
              INADDR_ANY;
          sessions[client_fd_index].udp_rtcp_server_addr.sin_port =
              htons(UDP_RTCP_PORT + client_fd_index);
          sessions[client_fd_index].udp_rtcp_port =
              UDP_RTCP_PORT + client_fd_index;

          if (bind(sessions[client_fd_index].udp_rtp_server_fd,
                   (struct sockaddr *)&sessions[client_fd_index]
                       .udp_rtp_server_addr,
                   sizeof(sessions[client_fd_index].udp_rtp_server_addr)) < 0) {
            perror("Error binding socket to udp server address");
            close(sessions[client_fd_index].udp_rtp_client_fd);
            close(sessions[client_fd_index].udp_rtcp_client_fd);
            close(sessions[client_fd_index].udp_rtp_server_fd);
            close(sessions[client_fd_index].udp_rtcp_server_fd);
            return -1;
          }

          if (bind(sessions[client_fd_index].udp_rtcp_server_fd,
                   (struct sockaddr *)&sessions[client_fd_index]
                       .udp_rtcp_server_addr,
                   sizeof(sessions[client_fd_index].udp_rtcp_server_addr)) <
              0) {
            perror("Error binding socket to control port address");
            close(sessions[client_fd_index].udp_rtp_client_fd);
            close(sessions[client_fd_index].udp_rtcp_client_fd);
            close(sessions[client_fd_index].udp_rtp_server_fd);
            close(sessions[client_fd_index].udp_rtcp_server_fd);
            return -1;
          }
          session_index = client_fd_index;
        }

        printf("Waiting for TCP connection to be accepted... %d\n\n",
               client_fd_index);

        // getting rtsp tcp data
        tcp_client_fds[client_fd_index] = accept(
            tcp_rtsp_server_fd,
            (struct sockaddr *)&sessions[session_index].tcp_rtsp_client_addr,
            &sessions[session_index].tcp_rtsp_client_addr_size);
        connections += 1;

        if (tcp_client_fds[client_fd_index] < 0) {
          perror("Error connecting...\n");
          continue;
        }

        printf("Accepted Connection on file descriptor: %d index: %d\n",
               tcp_client_fds[client_fd_index], client_fd_index);

        args[client_fd_index].tcp_client_fd = tcp_client_fds[client_fd_index];
        args[client_fd_index].play = &sessions[session_index].play;
        args[client_fd_index].recording = &sessions[session_index].recording;

        args[client_fd_index].rtsp_relay_server_ip = rtsp_relay_server_ip;
        args[client_fd_index].udp_rtp_server_fd =
            sessions[session_index].udp_rtp_server_fd;
        args[client_fd_index].udp_rtcp_server_fd =
            sessions[session_index].udp_rtcp_server_fd;

        args[client_fd_index].udp_rtp_client_fd =
            &sessions[session_index].udp_rtp_client_fd;
        args[client_fd_index].udp_rtp_client_addr =
            &sessions[session_index].udp_rtp_client_addr;
        args[client_fd_index].udp_rtp_client_addr_size =
            sizeof(sessions[session_index].udp_rtp_client_addr);

        args[client_fd_index].udp_rtcp_client_fd =
            &sessions[session_index].udp_rtcp_client_fd;
        args[client_fd_index].udp_rtcp_client_addr =
            &sessions[session_index].udp_rtcp_client_addr;
        args[client_fd_index].udp_rtcp_client_addr_size =
            sizeof(sessions[session_index].udp_rtcp_client_addr);

        args[client_fd_index].sdp = sessions[session_index].sdp;
        args[client_fd_index].session_id = sessions[session_index].session_id;
        args[client_fd_index].udp_rtp_port =
            sessions[session_index].udp_rtp_port;
        args[client_fd_index].udp_rtcp_port =
            sessions[session_index].udp_rtcp_port;
        args[client_fd_index].stream_num = &stream_num;

        pthread_t thread;
        pthread_create(&thread, NULL, handle_requests, &args[client_fd_index]);
      } else {
        printf("client already listening on file descriptor: %d\n",
               tcp_client_fds[client_fd_index]);
      }
    }
  }

  close(tcp_rtsp_server_fd);
  for (int x = 0; x < max_clients; x++) {
    close(sessions[x].udp_rtp_client_fd);
    close(sessions[x].udp_rtcp_client_fd);
    close(sessions[x].udp_rtp_server_fd);
    close(sessions[x].udp_rtcp_server_fd);
  }

  return 0;
}
