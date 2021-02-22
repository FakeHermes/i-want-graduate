/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

/* Copyright 2013-2018 the Alfalfa authors
                       and the Massachusetts Institute of Technology

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

      1. Redistributions of source code must retain the above copyright
         notice, this list of conditions and the following disclaimer.

      2. Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in the
         documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#include <getopt.h>

#include <cstdlib>
#include <random>
#include <unordered_map>
#include <utility>
#include <tuple>
#include <queue>
#include <deque>
#include <thread>
#include <condition_variable>
#include <future>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/file.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <string.h>
#include <stdarg.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <signal.h>
#include <sys/wait.h>
#include <regex.h>
#include <alsa/asoundlib.h>

#include "socket.hh"
#include "packet.hh"
#include "poller.hh"
#include "optional.hh"
#include "player.hh"
#include "display.hh"
#include "paranoid.hh"
#include "procinfo.hh"

#define PCM_DEVICE "default"
#define TS_PACKET_SIZE 188
#define MTU 1500

using namespace std;
using namespace std::chrono;
using namespace PollerShortNames;

//audio
struct rtp_header
{
  unsigned char cc : 4;
  unsigned char x : 1;
  unsigned char p : 1;
  unsigned char v : 2;
  unsigned char pt : 7;
  unsigned char m : 1;
  unsigned short sequence_number;
  unsigned int timestamp;
  unsigned int ssrc;
};
//audio end
class AverageInterPacketDelay
{
private:
  static constexpr double ALPHA = 0.1;

  double value_{-1.0};
  uint64_t last_update_{0};

public:
  void add(const uint64_t timestamp_us, const int32_t grace)
  {
    assert(timestamp_us >= last_update_);

    if (value_ < 0)
    {
      value_ = 0;
    }
    // else if ( timestamp_us - last_update_ > 0.2 * 1000 * 1000 /* 0.2 seconds */ ) {
    //   value_ /= 4;
    // }
    else
    {
      double new_value = max(0l, static_cast<int64_t>(timestamp_us - last_update_ - grace));
      value_ = ALPHA * new_value + (1 - ALPHA) * value_;
    }

    last_update_ = timestamp_us;
  }

  uint32_t int_value() const { return static_cast<uint32_t>(value_); }
};

void usage(const char *argv0)
{
  cerr << "Usage: " << argv0 << " [-f, --fullscreen] [--verbose] PORT WIDTH HEIGHT" << endl;
}

uint16_t ezrand()
{
  random_device rd;
  uniform_int_distribution<uint16_t> ud;

  return ud(rd);
}

queue<RasterHandle> display_queue;
mutex mtx;
condition_variable cv;

void display_task(const VP8Raster &example_raster, bool fullscreen)
{
  VideoDisplay display{example_raster, fullscreen};

  while (true)
  {
    unique_lock<mutex> lock(mtx);
    cv.wait(lock, []() { return not display_queue.empty(); });

    while (not display_queue.empty())
    {
      display.draw(display_queue.front());
      display_queue.pop();
    }
  }
}

void enqueue_frame(FramePlayer &player, const Chunk &frame)
{
  if (frame.size() == 0)
  {
    return;
  }

  const Optional<RasterHandle> raster = player.decode(frame);

  async(launch::async,
        [&raster]() {
          if (raster.initialized())
          {
            lock_guard<mutex> lock(mtx);
            display_queue.push(raster.get());
            cv.notify_all();
          }
        });
}

int main(int argc, char *argv[])
{
  // argc:count of arguments
  /* check the command-line arguments */
  if (argc < 1)
  { /* for sticklers */
    abort();
  }

  /* fullscreen player */
  bool fullscreen = false;
  bool verbose = false;

  const option command_line_options[] = {
      {"fullscreen", no_argument, nullptr, 'f'},
      {"verbose", no_argument, nullptr, 'v'},
      {0, 0, 0, 0}};

  while (true)
  {
    const int opt = getopt_long(argc, argv, "f", command_line_options, nullptr);

    if (opt == -1)
    {
      //命令行打印命令?
      cerr << "opt: " << opt << endl;
      break;
    }

    switch (opt)
    {
    case 'f':
      fullscreen = true;
      break;

    case 'v':
      verbose = true;
      break;

    default:
      usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (optind + 2 >= argc)
  {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  /* choose a random connection_id */
  const uint16_t connection_id = 1337; // ezrand();
  cerr << "Connection ID: " << connection_id << endl;

  /* construct Socket for incoming  datagrams */
  UDPSocket udpsocket;
  udpsocket.bind(Address("0", argv[optind]));
  udpsocket.set_timestamps();

  /* construct FramePlayer */
  FramePlayer player(paranoid::stoul(argv[optind + 1]), paranoid::stoul(argv[optind + 2]));
  player.set_error_concealment(true);

  /* construct display thread */
  thread([&player, fullscreen]() { display_task(player.example_raster(), fullscreen); }).detach();

  /* frame no => FragmentedFrame; used when receiving packets out of order */
  unordered_map<size_t, FragmentedFrame> fragmented_frames;
  size_t next_frame_no = 0;

  /* EWMA */
  AverageInterPacketDelay avg_delay;

  /* decoder states */
  uint32_t current_state = player.current_decoder().get_hash().hash();
  const uint32_t initial_state = current_state;
  deque<uint32_t> complete_states;
  unordered_map<uint32_t, Decoder> decoders{{current_state, player.current_decoder()}};

  /* memory usage logs */
  system_clock::time_point next_mem_usage_report = system_clock::now();

  Poller poller;
  poller.add_action(Poller::Action(
      udpsocket, Direction::In,
      [&]() {
        /* wait for next UDP datagram */
        const auto new_fragment = udpsocket.recv();

        /* parse into Packet */
        const Packet packet{new_fragment.payload};

        if (packet.frame_no() < next_frame_no)
        {
          /* we're not interested in this anymore */
          return ResultType::Continue;
        }
        else if (packet.frame_no() > next_frame_no)
        {
          /* current frame is not finished yet, but we just received a packet
           for the next frame, so here we just encode the partial frame and
           display it and move on to the next frame */
          cerr << "got a packet for frame #" << packet.frame_no()
               << ", display previous frame(s)." << endl;

          for (size_t i = next_frame_no; i < packet.frame_no(); i++)
          {
            if (fragmented_frames.count(i) == 0)
              continue;

            enqueue_frame(player, fragmented_frames.at(i).partial_frame());
            fragmented_frames.erase(i);
          }

          next_frame_no = packet.frame_no();
          current_state = player.current_decoder().minihash();
        }

        /* add to current frame */
        if (fragmented_frames.count(packet.frame_no()))
        {
          fragmented_frames.at(packet.frame_no()).add_packet(packet);
        }
        else
        {
          /*
          This was judged "too fancy" by the Code Review Board of Dec. 29, 2016.

          fragmented_frames.emplace( std::piecewise_construct,
                                     forward_as_tuple( packet.frame_no() ),
                                     forward_as_tuple( connection_id, packet ) );
        */

          fragmented_frames.insert(make_pair(packet.frame_no(),
                                             FragmentedFrame(connection_id, packet)));
        }

        /* is the next frame ready to be decoded? */
        if (fragmented_frames.count(next_frame_no) > 0 and fragmented_frames.at(next_frame_no).complete())
        {
          auto &fragment = fragmented_frames.at(next_frame_no);

          uint32_t expected_source_state = fragment.source_state();

          if (current_state != expected_source_state)
          {
            if (decoders.count(expected_source_state))
            {
              /* we have this state! let's load it */
              player.set_decoder(decoders.at(expected_source_state));
              current_state = expected_source_state;
            }
          }

          if (current_state == expected_source_state and
              expected_source_state != initial_state)
          {
            /* sender won't refer to any decoder older than this, so let's get
             rid of them */

            auto it = complete_states.begin();

            for (; it != complete_states.end(); it++)
            {
              if (*it != expected_source_state)
              {
                decoders.erase(*it);
              }
              else
              {
                break;
              }
            }

            assert(it != complete_states.end());
            complete_states.erase(complete_states.begin(), it);
          }

          // here we apply the frame
          enqueue_frame(player, fragment.frame());

          // state "after" applying the frame
          current_state = player.current_decoder().minihash();

          if (current_state == fragment.target_state() and
              current_state != initial_state)
          {
            /* this is a full state. let's save it */
            decoders.insert(make_pair(current_state, player.current_decoder()));
            complete_states.push_back(current_state);
          }

          fragmented_frames.erase(next_frame_no);
          next_frame_no++;
        }

        avg_delay.add(new_fragment.timestamp_us, packet.time_since_last());

        AckPacket(connection_id, packet.frame_no(), packet.fragment_no(),
                  avg_delay.int_value(), current_state,
                  complete_states)
            .sendto(udpsocket, new_fragment.source_address);

        auto now = system_clock::now();

        if (verbose and next_mem_usage_report < now)
        {
          cerr << "["
               << duration_cast<milliseconds>(now.time_since_epoch()).count()
               << "] "
               << " <mem = " << procinfo::memory_usage() << ">\n";
          next_mem_usage_report = now + 5s;
        }

        return ResultType::Continue;
      },
      [&]() { return not udpsocket.eof(); }));
  //audio
  int socka;
  int nPortA = 5003;
  fd_set rfd;              // 读描述符集
  struct timeval timeout;  // 定时变量
  struct sockaddr_in addr; // 告诉sock 应该在什么地方licence
  char recv_buff[MTU];     // 接收缓冲区

  int nRecLen; // 客户端地址长度!!!!!!

  struct sockaddr_in cli; // 客户端地址
  int nRet;               // select返回值

  socka = socket(AF_INET, SOCK_DGRAM, 0); // 创建数据报socka
  if (socka == -1)
  {
    printf("socket()/n");
    return;
  }

  memset(&addr, 0, sizeof(addr));

  addr.sin_family = AF_INET;                                     // IP协议
  addr.sin_port = htons(nPortA);                                 // 端口
  addr.sin_addr.s_addr = htonl(INADDR_ANY);                      // 在本机的所有ip上开始监听
  if (bind(socka, (struct sockaddr *)&addr, sizeof(addr)) == -1) // bind socka
  {
    printf("bind()/n");
    return;
  }

  // 设置超时时间为6s
  timeout.tv_sec = 6;
  timeout.tv_usec = 0;

  memset(recv_buff, 0, sizeof(recv_buff)); // 清空接收缓冲区

  //0.设置参数
  unsigned int pcm, tmp, dir;
  int rate = 44100, channels = 2;
  snd_pcm_t *pcm_handle; //typedef struct _snd_pcm
  char *buff;
  int buff_size;
  int count = 0;
  snd_pcm_uframes_t frames; //typedef unsigned long

  //1.打开音频播放设备
  snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0); // 这里是以音频播放模式打开

  //2.使用snd_pcm_hw_params_t配置硬件参数
  snd_pcm_hw_params_t *params;
  snd_pcm_hw_params_malloc(&params);         // 或者 snd_pcm_hw_params_alloca(&hwparams);
  snd_pcm_hw_params_any(pcm_handle, params); // 使用pcm设备初始化hwparams

  //snd_pcm_hw_params_set_xxx(pcm, hwparams, ...);  通过一系列的set函数来设置硬件参数中的基本参数
  //如， 通道数，采样率， 采样格式等等
  /* Set parameters */
  if (pcm = snd_pcm_hw_params_set_access(pcm_handle, params,
                                         SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
    printf("ERROR: Can't set interleaved mode. %s\n", snd_strerror(pcm));

  if (pcm = snd_pcm_hw_params_set_format(pcm_handle, params,
                                         SND_PCM_FORMAT_S16_LE) < 0)
    printf("ERROR: Can't set format. %s\n", snd_strerror(pcm));

  if (pcm = snd_pcm_hw_params_set_channels(pcm_handle, params, channels) < 0)
    printf("ERROR: Can't set channels number. %s\n", snd_strerror(pcm));

  if (pcm = snd_pcm_hw_params_set_rate_near(pcm_handle, params, &rate, 0) < 0)
    printf("ERROR: Can't set rate. %s\n", snd_strerror(pcm));

  // write paras

  if (pcm = snd_pcm_hw_params(pcm_handle, params) < 0)
    printf("ERROR: Can't set harware parameters. %s\n", snd_strerror(pcm));
  //snd_pcm_hw_params_free(params); // 释放不再使用的hwparams空间

  //3. 使用snd_pcm_sw_params_t配置一些高级软件参数， 比如使用中断模式等等
  snd_pcm_hw_params_get_period_size(params, &frames, 0);
  buff_size = frames * channels * 2 /* 2 -> sample size */;
  buff = (char *)malloc(buff_size);
  printf("frames = %lu.\n", frames);
  snd_pcm_hw_params_get_period_time(params, &tmp, NULL);
  //4. 调用snd_pcm_prepare(pcm)来使音频设备准备好接收pcm数据
  if (snd_pcm_prepare(pcm_handle) < 0)
  {
    snd_pcm_close(pcm_handle);
    exit(1);
  }

  while (1)
  {
    FD_ZERO(&rfd); // 在使用之前总是要清空

    // 开始使用select
    FD_SET(socka, &rfd); // 把socka放入要测试的描述符集中

    nRet = select(socka + 1, &rfd, NULL, NULL, &timeout); // 检测是否有套接口是否可读
    if (nRet == -1)
    {
      printf("select()\n");
      return;
    }
    else if (nRet == 0) // 超时
    {
      //printf("timeout\n");
      continue;
    }
    else // 检测到有套接口可读
    {
      if (FD_ISSET(socka, &rfd)) // socka可读
      {
        nRecLen = sizeof(cli);
        int nRecEcho = recvfrom(socka, recv_buff, sizeof(recv_buff), 0, (struct sockaddr *)&cli, &nRecLen);
        if (nRecEcho == -1)
        {
          printf("recvfrom()\n");
          break;
        }
        //start

        //把数据从rece-buf 送入声卡buffer
        //5. 循环调用snd_pcm_writei(pcm, buffer, frame)来往音频设备写数据

        // if (pcm = read(0, buff, buff_size) == 0) {
        //     printf("Early end of file.\n");
        //     return 0;
        // }

        if (pcm = snd_pcm_writei(pcm_handle, recv_buff, frames) == -EPIPE)
        {
          printf("XRUN.\n");
          snd_pcm_prepare(pcm_handle);
        }
        else if (pcm < 0)
        {
          printf("ERROR. Can't write to PCM device. %s\n", snd_strerror(pcm));
        }

        struct rtp_header *p = (struct rtp_header *)recv_buff;
        printf("v = %d\n", p->v);
        printf("p = %d\n", p->p);
        printf("x = %d\n", p->x);
        printf("cc = %d\n", p->cc);
        printf("sequence_number = %d\n", ntohs(p->sequence_number));
        printf("ssrc = %d\n", ntohs(p->ssrc));
      }
    }
  }
  //end
  /* handle events */
  while (true)
  {
    const auto poll_result = poller.poll(-1);
    if (poll_result.result == Poller::Result::Type::Exit)
    {
      return poll_result.exit_status;
    }
  }

  return EXIT_SUCCESS;
}
