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
#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <limits>
#include <thread>
#include <future>
#include <algorithm>
#include <unordered_map>
#include <iomanip>
#include <cmath>

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <alsa/asoundlib.h>
#include <signal.h>

#include "exception.hh"
#include "finally.hh"
#include "paranoid.hh"
#include "yuv4mpeg.hh"
#include "encoder.hh"
#include "socket.hh"
#include "packet.hh"
#include "poller.hh"
#include "socketpair.hh"
#include "camera.hh"
#include "pacer.hh"
#include "procinfo.hh"

using namespace std;
using namespace std::chrono;
using namespace PollerShortNames;

//audio

#define AudioFormat SND_PCM_FORMAT_S16_LE  //指定音频的格式,其他常用格式：SND_PCM_FORMAT_U24_LE、SND_PCM_FORMAT_U32_LE
#define AUDIO_CHANNEL_SET   2  			  //1单声道   2立体声
#define AUDIO_RATE_SET 44100   //音频采样率,常用的采样频率: 44100Hz 、16000HZ、8000HZ、48000HZ、22050HZ
 
FILE *pcm_data_file=NULL;
int run_flag=0;
void exit_sighandler(int sig){
  run_flag=sig;
}

//end

class AverageEncodingTime
{
private:
  static constexpr double ALPHA = 0.1;

  double value_{-1.0};
  microseconds last_update_{0};

public:
  void add(const microseconds timestamp_us)
  {
    assert(timestamp_us >= last_update_);

    if (value_ < 0)
    {
      value_ = 0;
    }
    else if (timestamp_us - last_update_ > 1s /* 1 seconds */)
    {
      value_ = 0;
    }
    else
    {
      double new_value = max(0l, duration_cast<microseconds>(timestamp_us - last_update_).count());
      value_ = ALPHA * new_value + (1 - ALPHA) * value_;
    }

    last_update_ = timestamp_us;
  }

  uint32_t int_value() const { return static_cast<uint32_t>(value_); }
};

struct EncodeJob
{
  string name;

  RasterHandle raster;

  Encoder encoder;
  EncoderMode mode;

  uint8_t y_ac_qi;
  size_t target_size;

  EncodeJob(const string &name, RasterHandle raster, const Encoder &encoder,
            const EncoderMode mode, const uint8_t y_ac_qi, const size_t target_size)
      : name(name), raster(raster), encoder(encoder),
        mode(mode), y_ac_qi(y_ac_qi), target_size(target_size)
  {
  }
};

struct EncodeOutput
{
  Encoder encoder;
  vector<uint8_t> frame;
  uint32_t source_minihash;
  milliseconds encode_time;
  string job_name;
  uint8_t y_ac_qi;

  EncodeOutput(Encoder &&encoder, vector<uint8_t> &&frame,
               const uint32_t source_minihash, const milliseconds encode_time,
               const string &job_name, const uint8_t y_ac_qi)
      : encoder(move(encoder)), frame(move(frame)),
        source_minihash(source_minihash), encode_time(encode_time),
        job_name(job_name), y_ac_qi(y_ac_qi)
  {
  }
};

EncodeOutput do_encode_job(EncodeJob &&encode_job)
{
  vector<uint8_t> output;

  uint32_t source_minihash = encode_job.encoder.minihash();

  const auto encode_beginning = system_clock::now();

  uint8_t quantizer_in_use = 0;

  switch (encode_job.mode)
  {
  case CONSTANT_QUANTIZER:
    output = encode_job.encoder.encode_with_quantizer(encode_job.raster.get(),
                                                      encode_job.y_ac_qi);
    quantizer_in_use = encode_job.y_ac_qi;
    break;

  case TARGET_FRAME_SIZE:
    output = encode_job.encoder.encode_with_target_size(encode_job.raster.get(),
                                                        encode_job.target_size);
    break;

  default:
    throw runtime_error("unsupported encoding mode.");
  }

  const auto encode_ending = system_clock::now();
  const auto ms_elapsed = duration_cast<milliseconds>(encode_ending - encode_beginning);

  return {move(encode_job.encoder), move(output), source_minihash, ms_elapsed, encode_job.name, quantizer_in_use};
}

size_t target_size(uint32_t avg_delay, const uint64_t last_acked, const uint64_t last_sent,
                   const uint32_t max_delay = 100 * 1000 /* 100 ms = 100,000 us */)
{
  if (avg_delay == 0)
  {
    avg_delay = 1;
  }

  /* cerr << "Packets in flight: " << last_sent - last_acked << "\n";
  cerr << "Avg inter-packet-arrival interval: " << avg_delay << "\n";
  cerr << "Imputed delay: " << avg_delay * (last_sent - last_acked) << " us\n"; */

  return 1400 * max(0l, static_cast<int64_t>(max_delay / avg_delay - (last_sent - last_acked)));
}

void usage(const char *argv0)
{
  cerr << "Usage: " << argv0
       << " [-m,--mode MODE] [-d, --device CAMERA] [-p, --pixfmt PIXEL_FORMAT]"
       << " [-u,--update-rate RATE] [--log-mem-usage] HOST PORT CONNECTION_ID" << endl
       << endl
       << "Accepted MODEs are s1, s2 (default), conventional." << endl;
}

uint64_t ack_seq_no(const AckPacket &ack,
                    const vector<uint64_t> &cumulative_fpf)
{
  return (ack.frame_no() > 0)
             ? (cumulative_fpf[ack.frame_no() - 1] + ack.fragment_no())
             : ack.fragment_no();
}

enum class OperationMode
{
  S1,
  S2,
  Conventional
};

int main(int argc, char *argv[])
{
  /* check the command-line arguments */
  if (argc < 1)
  { /* for sticklers */
    abort();
  }

  /* camera settings */
  string camera_device = "/dev/video0";
  string pixel_format = "NV12";
  size_t update_rate __attribute__((unused)) = 1;
  OperationMode operation_mode = OperationMode::S2;
  bool log_mem_usage = false;

  const option command_line_options[] = {
      {"mode", required_argument, nullptr, 'm'},
      {"device", required_argument, nullptr, 'd'},
      {"pixfmt", required_argument, nullptr, 'p'},
      {"update-rate", required_argument, nullptr, 'u'},
      {"log-mem-usage", no_argument, nullptr, 'M'},
      {0, 0, 0, 0}};

  while (true)
  {
    const int opt = getopt_long(argc, argv, "d:p:m:u:", command_line_options, nullptr);

    if (opt == -1)
    {
      break;
    }

    switch (opt)
    {
    case 'd':
      camera_device = optarg;
      break;

    case 'p':
      pixel_format = optarg;
      break;

    case 'm':
      if (strcmp(optarg, "s1") == 0)
      {
        operation_mode = OperationMode::S1;
      }
      else if (strcmp(optarg, "s2") == 0)
      {
        operation_mode = OperationMode::S2;
      }
      else if (strcmp(optarg, "conventional") == 0)
      {
        operation_mode = OperationMode::Conventional;
      }
      else
      {
        throw runtime_error("unknown operation mode");
      }
      break;

    case 'u':
      update_rate = paranoid::stoul(optarg);
      break;

    case 'M':
      log_mem_usage = true;
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

  /* construct Socket for outgoing datagrams */
  UDPSocket udpsocket,udpsocket2;
  udpsocket.connect(Address(argv[optind], argv[optind + 1]));
  udpsocket.set_timestamps();

  udpsocket2.connect(Address(argv[optind], argv[optind + 2]));
  udpsocket2.set_timestamps();

  /* make pacer to smooth out outgoing packets */
  Pacer pacer;

  /* get connection_id */
  const uint16_t connection_id = paranoid::stoul(argv[optind + 3]);

  /* average inter-packet delay, reported by receiver */
  uint32_t avg_delay = numeric_limits<uint32_t>::max();

  /* keep the number of fragments per frame */
  vector<uint64_t> cumulative_fpf;
  uint64_t last_acked = numeric_limits<uint64_t>::max();

  /* maximum number of frames to be skipped in a row */
  const size_t MAX_SKIPPED = 3;
  size_t skipped_count = 0;

  if (not PIXEL_FORMAT_STRS.count(pixel_format))
  {
    throw runtime_error("unsupported pixel format");
  }

  /* camera device */
  Camera camera{1280, 800, PIXEL_FORMAT_STRS.at(pixel_format), camera_device};
  
  /* construct the encoder */
  Encoder base_encoder{camera.display_width(), camera.display_height(),
                       false /* two-pass */, REALTIME_QUALITY};

  const uint32_t initial_state = base_encoder.minihash();

  /* encoded frame index */
  unsigned int frame_no = 0;

  /* latest raster that is received from the input */
  Optional<RasterHandle> last_raster;

  /* where we keep the outputs of parallel encoding jobs */
  vector<EncodeJob> encode_jobs;
  vector<future<EncodeOutput>> encode_outputs;

  /* keep the moving average of encoding times */
  AverageEncodingTime avg_encoding_time;

  /* track the last quantizer used */
  uint8_t last_quantizer = 64;

  /* decoder hash => encoder object */
  deque<uint32_t> encoder_states;
  unordered_map<uint32_t, Encoder> encoders{{initial_state, base_encoder}};

  /* latest state of the receiver, based on ack packets */
  Optional<uint32_t> receiver_last_acked_state;
  Optional<uint32_t> receiver_assumed_state;
  deque<uint32_t> receiver_complete_states;

  /* if the receiver goes into an invalid state, for this amount of seconds,
     we will go into a conservative mode: we only encode based on a known state */
  seconds conservative_for{5};
  system_clock::time_point conservative_until = system_clock::now();

  /* for 'conventional codec' mode */
  duration<long int, std::nano> cc_update_interval{(update_rate == 0) ? 0 : std::nano::den / update_rate};
  system_clock::time_point next_cc_update = system_clock::now() + cc_update_interval;
  size_t cc_quantizer = 32;
  size_t cc_rate = 0;
  size_t cc_rate_ewma = 0;

  /* :D */
  system_clock::time_point last_sent = system_clock::now();

  /* comment */
  auto encode_start_pipe = UnixDomainSocket::make_pair();
  auto encode_end_pipe = UnixDomainSocket::make_pair();

  /* mem usage timer */
  system_clock::time_point next_mem_usage_report = system_clock::now();

  Poller poller;

  //audio para set
	// int err;
	// char *buffer;
	// int buffer_frames = 1024;
	// unsigned int rate = AUDIO_RATE_SET;
	// snd_pcm_t *capture_handle;// 一个指向PCM设备的句柄
	// snd_pcm_hw_params_t *hw_params; //此结构包含有关硬件的信息，可用于指定PCM流的配置
	
	// /*注册信号捕获退出接口*/
	// signal(2,exit_sighandler);
 
	// /*PCM的采样格式在pcm.h文件里有定义*/
	// snd_pcm_format_t format=AudioFormat; // 采样位数：16bit、LE格式
 
	// /*打开音频采集卡硬件，并判断硬件是否打开成功，若打开失败则打印出错误提示*/
	// // SND_PCM_STREAM_PLAYBACK 输出流
	// // SND_PCM_STREAM_CAPTURE  输入流
	// if ((err = snd_pcm_open (&capture_handle, argv[optind+4] ,SND_PCM_STREAM_CAPTURE,0))<0) 
	// {
	// 	printf("无法打开音频设备: %s (%s)\n",  argv[optind+4],snd_strerror (err));
	// 	exit(1);
	// }
	// printf("音频接口打开成功.\n");
 
	// /*分配硬件参数结构对象，并判断是否分配成功*/
	// if((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) 
	// {
	// 	printf("无法分配硬件参数结构 (%s)\n",snd_strerror(err));
	// 	exit(1);
	// }
	// printf("硬件参数结构已分配成功.\n");
	
	// /*按照默认设置对硬件对象进行设置，并判断是否设置成功*/
	// if((err=snd_pcm_hw_params_any(capture_handle,hw_params)) < 0) 
	// {
	// 	printf("无法初始化硬件参数结构 (%s)\n", snd_strerror(err));
	// 	exit(1);
	// }
	// printf("硬件参数结构初始化成功.\n");
 
	// /*
	// 	设置数据为交叉模式，并判断是否设置成功
	// 	interleaved/non interleaved:交叉/非交叉模式。
	// 	表示在多声道数据传输的过程中是采样交叉的模式还是非交叉的模式。
	// 	对多声道数据，如果采样交叉模式，使用一块buffer即可，其中各声道的数据交叉传输；
	// 	如果使用非交叉模式，需要为各声道分别分配一个buffer，各声道数据分别传输。
	// */
	// if((err = snd_pcm_hw_params_set_access (capture_handle,hw_params,SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) 
	// {
	// 	printf("无法设置访问类型(%s)\n",snd_strerror(err));
	// 	exit(1);
	// }
	// printf("访问类型设置成功.\n");
 
	// /*设置数据编码格式，并判断是否设置成功*/
	// if ((err=snd_pcm_hw_params_set_format(capture_handle, hw_params,format)) < 0) 
	// {
	// 	printf("无法设置格式 (%s)\n",snd_strerror(err));
	// 	exit(1);
	// }
	// fprintf(stdout, "PCM数据格式设置成功.\n");
 
	// /*设置采样频率，并判断是否设置成功*/
	// if((err=snd_pcm_hw_params_set_rate_near(capture_handle,hw_params,&rate,0))<0) 
	// {
	// 	printf("无法设置采样率(%s)\n",snd_strerror(err));
	// 	exit(1);
	// }
	// printf("采样率设置成功\n");
 
	// /*设置声道，并判断是否设置成功*/
	// if((err = snd_pcm_hw_params_set_channels(capture_handle, hw_params,AUDIO_CHANNEL_SET)) < 0) 
	// {
	// 	printf("无法设置声道数(%s)\n",snd_strerror(err));
	// 	exit(1);
	// }
	// printf("声道数设置成功.\n");
 
	// /*将配置写入驱动程序中，并判断是否配置成功*/
	// if ((err=snd_pcm_hw_params (capture_handle,hw_params))<0) 
	// {
	// 	printf("无法向驱动程序设置参数(%s)\n",snd_strerror(err));
	// 	exit(1);
	// }
	// printf("参数设置成功.\n");
 
	// /*使采集卡处于空闲状态*/
	// snd_pcm_hw_params_free(hw_params);
 
	// /*准备音频接口,并判断是否准备好*/
	// if((err=snd_pcm_prepare(capture_handle))<0) 
	// {
	// 	printf("无法使用音频接口 (%s)\n",snd_strerror(err));
	// 	exit(1);
	// }
	// printf("音频接口准备好.\n");
 
	// /*配置一个数据缓冲区用来缓冲数据*/
	// //snd_pcm_format_width(format) 获取样本格式对应的大小(单位是:bit)
	// int frame_byte=snd_pcm_format_width(format)/8;
	// buffer=(char *)malloc(buffer_frames*frame_byte*AUDIO_CHANNEL_SET); //2048
	// printf("缓冲区分配成功.\n");
  //audio para set end

  //从声卡采集数据
  // poller.add_action(Poller::Action(
  //   udpsocket, Direction::Out, [&](){
      
  //     while(true){
  //         if((err=snd_pcm_readi(capture_handle,buffer,buffer_frames))!=buffer_frames) {
  //           printf("从音频接口读取失败(%s)\n",snd_strerror(err));
  //           exit(1);
  //         }
  //         //
  //         string audio_frame(buffer);

  //         udpsocket2.send(audio_frame); //发送音频数据
  //     }
      

  //     return ResultType::Continue;
  //   }
  // ));

  //从文件读取音频
  poller.add_action(Poller::Action(
    udpsocket, Direction::Out, [&](){
      
      FILE *ts_file = fopen(argv[optind+4], "r+"); 
        char h[44];
        char *buff;
        buff=(char *)malloc(1024);
        int len __attribute__((unused)) =1;
	     len=fread(h,1,44,ts_file); //44numofcount:0,去除wav header
	    while(!feof(ts_file)){  
        len=fread(buff, 1, 1024, ts_file);  

        string audio_frame(buff);
        
        udpsocket2.send(audio_frame);//发送音频数据
        usleep(10000);  
	    } 
      

      return ResultType::Continue;
    }
  ));

 

  /* fetch frames from webcam */
  poller.add_action(Poller::Action(encode_start_pipe.second, Direction::In,
                                   [&]() -> Result {
                                     encode_start_pipe.second.read();

                                     last_raster = camera.get_next_frame();

                                     if (not last_raster.initialized())
                                     {
                                       return {ResultType::Exit, EXIT_FAILURE};
                                     }

                                     if (encode_jobs.size() > 0)
                                     {
                                       /* a frame is being encoded now */
                                       return ResultType::Continue;
                                     }

                                     /* let's cleanup the stored encoders based on the lastest ack */
                                     if (receiver_last_acked_state.initialized() and
                                         receiver_last_acked_state.get() != initial_state and
                                         encoders.count(receiver_last_acked_state.get()))
                                     {
                                       // cleaning up
                                       auto it = encoder_states.begin();

                                       while (it != encoder_states.end())
                                       {
                                         if (*it != receiver_last_acked_state.get() and
                                             *it != receiver_assumed_state.get())
                                         {
                                           if (find(next(it), encoder_states.end(), *it) == encoder_states.end())
                                           {
                                             encoders.erase(*it);
                                           }

                                           it++;
                                         }
                                         else
                                         {
                                           break;
                                         }
                                       }

                                       encoder_states.erase(encoder_states.begin(), it);
                                     }

                                     RasterHandle raster = last_raster.get();

                                     uint32_t selected_source_hash = initial_state;

                                     /* reason about the state of the receiver based on ack messages
       * this is the logic that decides which encoder to use. for example,
       * if the packet loss is huge, we can always select an encoder with a sure
       * state. */

                                     /* if we're in 'conservative' mode, let's just encode based on something
         we're sure that is available in the receiver */
                                     if (system_clock::now() < conservative_until)
                                     {
                                       if (receiver_complete_states.size() == 0)
                                       {
                                         /* and the receiver doesn't have any other states, other than the
             default state */
                                         selected_source_hash = initial_state;
                                       }
                                       else
                                       {
                                         /* the receiver has at least one stored state, let's use it */
                                         selected_source_hash = receiver_complete_states.back();
                                       }
                                     }
                                     else if (not receiver_last_acked_state.initialized())
                                     {
                                       /* okay, we're not in 'conservative' mode */
                                       if (not receiver_assumed_state.initialized())
                                       {
                                         /* okay, let's just encode as a keyframe */
                                         selected_source_hash = initial_state;
                                       }
                                       else
                                       {
                                         /* we assume that the receiver is in a right state */
                                         selected_source_hash = receiver_assumed_state.get();
                                       }
                                     }
                                     else
                                     {
                                       if (encoders.count(receiver_last_acked_state.get()) == 0)
                                       {
                                         /* it seems that the receiver is in an invalid state */

                                         /* step 1: let's go into 'conservative' mode; just encode based on a
             known for a while */

                                         conservative_until = system_clock::now() + conservative_for;

                                         cerr << "Going into 'conservative' mode for next "
                                              << conservative_for.count() << " seconds." << endl;

                                         if (receiver_complete_states.size() == 0)
                                         {
                                           /* and the receiver doesn't have any other states, other than the
               default state */
                                           selected_source_hash = initial_state;
                                         }
                                         else
                                         {
                                           /* the receiver has at least one stored state, let's use it */
                                           selected_source_hash = receiver_complete_states.back();
                                         }
                                       }
                                       else
                                       {
                                         /* we assume that the receiver is in a right state */
                                         selected_source_hash = receiver_assumed_state.get();
                                       }
                                     }
                                     /* end of encoder selection logic */

                                     const Encoder &encoder = encoders.at(selected_source_hash);

                                     const static auto increment_quantizer = [](const uint16_t q, const int8_t inc) -> uint8_t {
                                       int orig = q;
                                       orig += inc;
                                       orig = max(3, orig);
                                       orig = min(127, orig);
                                       return orig;
                                     };

                                     if (operation_mode == OperationMode::Conventional)
                                     {
                                       /* is it time to update the quality setting? */
                                       if (next_cc_update <= system_clock::now())
                                       {
                                         const size_t old_quantizer = cc_quantizer;
                                         cc_rate = 1000 * 1000 * 1400 / avg_delay;

                                         double change_percentage = (1.0 * cc_rate - 1.0 * cc_rate_ewma) /
                                                                    (1.0 * cc_rate_ewma);

                                         change_percentage = max(-1.0, min(1.5, change_percentage));

                                         if (change_percentage < -0.99)
                                         {
                                           cc_quantizer = 127;
                                         }
                                         else
                                         {
                                           double qalpha = 0.75;
                                           cc_quantizer = increment_quantizer(cc_quantizer /
                                                                                  pow(change_percentage + 1, 1 / qalpha),
                                                                              0);
                                         }

                                         cc_rate_ewma = 0.8 * cc_rate + 0.2 * cc_rate_ewma;

                                         cerr << "avg-delay=" << avg_delay << "us "
                                              << "old-quantizer=" << old_quantizer << " "
                                              << "new-quantizer=" << cc_quantizer << " "
                                              << "emwa-rate=" << cc_rate_ewma / 1000 << "KB "
                                              << "new-rate=" << cc_rate / 1000 << "KB "
                                              << "change-percentage="
                                              << fixed << setprecision(2) << change_percentage
                                              << endl;

                                         next_cc_update = system_clock::now() + cc_update_interval;
                                       }

                                       encode_jobs.emplace_back("frame", raster, encoder, CONSTANT_QUANTIZER,
                                                                cc_quantizer, 0);
                                     }
                                     else
                                     {
                                       /* try various quantizers */
                                       encode_jobs.emplace_back("improve", raster, encoder, CONSTANT_QUANTIZER,
                                                                increment_quantizer(last_quantizer, -17), 0);

                                       encode_jobs.emplace_back("fail-small", raster, encoder, CONSTANT_QUANTIZER,
                                                                increment_quantizer(last_quantizer, +23), 0);
                                     }

                                     // this thread will spawn all the encoding jobs and will wait on the results
                                     thread(
                                         [&encode_jobs, &encode_outputs, &encode_end_pipe, operation_mode]() {
                                           encode_outputs.clear();
                                           encode_outputs.reserve(encode_jobs.size());

                                           for (auto &job : encode_jobs)
                                           {
                                             encode_outputs.push_back(
                                                 async(((operation_mode == OperationMode::S2) ? launch::async : launch::deferred),
                                                       do_encode_job, move(job)));
                                           }

                                           for (auto &future_res : encode_outputs)
                                           {
                                             future_res.wait();
                                           }

                                           encode_end_pipe.first.write("1");
                                         })
                                         .detach();

                                     return ResultType::Continue;
                                   }));

  /* all encode jobs have finished */
  poller.add_action(Poller::Action(encode_end_pipe.second, Direction::In,
                                   [&]() {
                                     /* whatever happens, encode_jobs will be empty after this block is done. */
                                     auto _ = finally(
                                         [&]() {
                                           encode_jobs.clear();
                                           encode_start_pipe.first.write("1");
                                         });

                                     encode_end_pipe.second.read();

                                     avg_encoding_time.add(duration_cast<microseconds>(system_clock::now().time_since_epoch()));

                                     if (not any_of(encode_outputs.cbegin(), encode_outputs.cend(),
                                                    [&](const future<EncodeOutput> &o) { return o.valid(); }))
                                     {
                                       cerr << "All encoding jobs got killed for frame " << frame_no << "\n";
                                       // no encoding job has ended in time
                                       return ResultType::Continue;
                                     }

                                     /* what is the current capacity of the network,
         now that the encoding is done? */
                                     size_t frame_size = numeric_limits<size_t>::max();

                                     if (avg_delay != numeric_limits<uint32_t>::max())
                                     {
                                       frame_size = target_size(avg_delay, last_acked, cumulative_fpf.back());
                                     }

                                     size_t best_output_index = numeric_limits<size_t>::max();
                                     size_t best_size_diff = numeric_limits<size_t>::max();

                                     vector<EncodeOutput> good_outputs;

                                     for (auto &out_future : encode_outputs)
                                     {
                                       if (out_future.valid())
                                       {
                                         good_outputs.push_back(move(out_future.get()));
                                       }
                                     }

                                     if (operation_mode == OperationMode::Conventional)
                                     {
                                       best_output_index = 0; /* always send the frame */
                                     }
                                     else
                                     {
                                       /* choose the best based on the current capacity */
                                       for (size_t i = 0; i < good_outputs.size(); i++)
                                       {
                                         if (good_outputs[i].frame.size() <= frame_size)
                                         {
                                           if (frame_size - good_outputs[i].frame.size() < best_size_diff)
                                           {
                                             best_size_diff = frame_size - good_outputs[i].frame.size();
                                             best_output_index = i;
                                           }
                                         }
                                       }

                                       if (best_output_index == numeric_limits<size_t>::max())
                                       {
                                         if (skipped_count < MAX_SKIPPED or good_outputs.back().job_name != "fail-small")
                                         {
                                           /* skip frame */
                                           cerr << "["
                                                << duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()
                                                << "] "
                                                << "Skipping frame " << frame_no << "\n";
                                           skipped_count++;
                                           return ResultType::Continue;
                                         }
                                         else
                                         {
                                           cerr << "Too many skipped frames; sending the bad-quality option on " << frame_no << "\n";
                                           best_output_index = good_outputs.size() - 1;
                                           assert(good_outputs[best_output_index].job_name == "fail-small");
                                         }
                                       }
                                     }

                                     auto output = move(good_outputs[best_output_index]);

                                     uint32_t target_minihash = output.encoder.minihash();

                                     /*cerr << "Sending frame #" << frame_no << " (size=" << output.frame.size() << " bytes, "
           << "source_hash=" << output.source_minihash << ", target_hash="
           << target_minihash << ")...";*/

                                     last_quantizer = output.y_ac_qi;

                                     FragmentedFrame ff{connection_id, output.source_minihash, target_minihash,
                                                        frame_no,
                                                        static_cast<uint32_t>(duration_cast<microseconds>(system_clock::now() - last_sent).count()),
                                                        output.frame};
                                     /* enqueue the packets to be sent */
                                     /* send 5x faster than packets are being received */
                                     const unsigned int inter_send_delay = min(2000u, max(500u, avg_delay / 5));
                                     for (const auto &packet : ff.packets())
                                     {
                                       pacer.push(packet.to_string(), inter_send_delay);
                                     }

                                     last_sent = system_clock::now();

                                     /*cerr << "["
           << duration_cast<milliseconds>( last_sent.time_since_epoch() ).count()
           << "] "
           << "Frame " << frame_no << ": " << output.job_name
           << " (" << to_string( output.y_ac_qi ) << ") = "
           << ff.fragments_in_this_frame() << " fragments ("
           << avg_encoding_time.int_value()/1000 << " ms, ssim="
           << output.encoder.stats().ssim.get_or( -1.0 )
           << ") {" << output.source_minihash << " -> " << target_minihash << "}"
           << " intersend_delay = " << inter_send_delay << " us"; */

                                     if (log_mem_usage and next_mem_usage_report < last_sent)
                                     {
                                       cerr << " <mem = " << procinfo::memory_usage() << ">";
                                       next_mem_usage_report = last_sent + 5s;
                                     }

                                     // cerr << "\n";

                                     cumulative_fpf.push_back((frame_no > 0)
                                                                  ? (cumulative_fpf[frame_no - 1] + ff.fragments_in_this_frame())
                                                                  : ff.fragments_in_this_frame());

                                     /* now we assume that the receiver will successfully get this */
                                     receiver_assumed_state.reset(target_minihash);

                                     encoders.insert(make_pair(target_minihash, move(output.encoder)));
                                     encoder_states.push_back(target_minihash);

                                     skipped_count = 0;
                                     frame_no++;

                                     return ResultType::Continue;
                                   }));

  /* new ack from receiver */
  poller.add_action(Poller::Action(udpsocket, Direction::In,
                                   [&]() {
                                     auto packet = udpsocket.recv();
                                     AckPacket ack(packet.payload);

                                     if (ack.connection_id() != connection_id)
                                     {
                                       /* this is not an ack for this session! */
                                       return ResultType::Continue;
                                     }

                                     uint64_t this_ack_seq = ack_seq_no(ack, cumulative_fpf);

                                     if (last_acked != numeric_limits<uint64_t>::max() and
                                         this_ack_seq < last_acked)
                                     {
                                       /* we have already received an ACK newer than this */
                                       return ResultType::Continue;
                                     }

                                     last_acked = this_ack_seq;
                                     avg_delay = ack.avg_delay();
                                     receiver_last_acked_state.reset(ack.current_state());
                                     receiver_complete_states = move(ack.complete_states());

                                     return ResultType::Continue;
                                   }));

  /* outgoing packet ready to leave the pacer */
  poller.add_action(Poller::Action(
      udpsocket, Direction::Out, [&]() {
        assert( pacer.ms_until_due() == 0 );

        while ( pacer.ms_until_due() == 0 ) {
          assert( not pacer.empty() );

          udpsocket.send( pacer.front() );
          pacer.pop();
        }

        return ResultType::Continue; }, [&]() { return pacer.ms_until_due() == 0; }
  ));

  
  /* kick off the first encode */
  encode_start_pipe.first.write("1");

  /* handle events */
  while (true)
  {
    const auto poll_result = poller.poll(pacer.ms_until_due());
    if (poll_result.result == Poller::Result::Type::Exit)
    {
      if (poll_result.exit_status)
      {
        cerr << "Connection error." << endl;
      }

      return poll_result.exit_status;
    }
  }

  return EXIT_FAILURE;
}
