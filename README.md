RTSP PUSH.
The hope for this is to architect a RTSP to HLS security surveillance feed.
architecture
_________________________                                                                  _________________________
|                       |  (RTSP) forward camera h.264 encoded video frames in RTP Packets |                       |
|Camera                 |----------------------------------------------------------------->| RTSP/HLS Cloud Server | Convert frames to HLS format respond to GET requests with h.264 str
|_______________________|  Can use ffmpeg for this                                         |_______________________|
                                                                                                      ^
                                                                                                      |
                                                                                                      |
                                                                                                      | HTTP GET / feed
                                                                                                      |
                                                                                                      |
                                                                                                      |
                                                                                                 _________
                                                                                                |        |
                                                                                                | Client |
                                                                                                |________|
