#include <stdio.h>

#define __STDC_CONSTANT_MACROS
#define SDL_MAIN_HANDLED

extern "C"
{
#include "libavutil/avutil.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h" 
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libavutil/avstring.h"
#include "libswresample/swresample.h"
#include "libavutil/time.h"

#include "SDL2/SDL.h"
#include "SDL2/SDL_thread.h"
}

#define MAX_AUDIO_BUF_SIZE 19200
#define MAX_VIDEO_PACKET_SIZE (1024 * 1024)
#define MAX_AUDIO_PACKET_SIZE (1024 * 64)
#define PICTURE_QUEUE_SIZE 3
#define VIDEO_REFRESH_EVENT (SDL_USEREVENT)

char input_file[] = "test.mp4";
SDL_Window *screen;
SDL_Renderer *renderer;
SDL_Texture *texture;

//压缩数据包队列
struct PacketQueue
{
	int size;//队列数据包个数
	AVPacketList *head, *tail;//首尾指针
	SDL_mutex *qMutex;//队列互斥锁，防止同时访问共享内存空间
	SDL_cond *qCond;//队列信号量，用于同步
};

//视频输出图像数据结构
struct VideoPicture
{
	uint8_t *buffer;
	AVFrame *frameYUV;	
	int allocated, width, height;
	double pts;
};

//记录打开的视频文件动态信息
struct VideoIfo
{
	//视频文件的全局信息
	char file_name[1024];
	AVFormatContext *avFormatCtx;//视频文件上下文
	int video_idx, audio_idx;//视音频流下标
	AVCodec *vCodec, *aCodec;//视音频解码器
	AVStream *vStream, *aStream;//视音频流
	SDL_Thread *parse_tid, *decode_tid;//视频文件解析线程id和视频流解码线程id

	//视频相关数据
	VideoPicture picture_queue[PICTURE_QUEUE_SIZE];//图像缓存队列，存放解码并转码后用于播放的图像数据
	int picq_ridx, picq_widx, picq_size;//图像缓存队列的读写下标以及队列的大小
	SDL_mutex *picq_mutex;//图像缓存队列互斥锁
	SDL_cond *picq_cond_write, *picq_cond_read;//图像缓存队列同步信号量
	PacketQueue video_queue;//视频压缩数据包队列
	int width, height;//视频图像的宽和高
	SwsContext *swsCtx;//视频格式转换上下文
	double video_pts;//记录播放视频帧的时间戳
	double video_time;//将要播放视频帧的播放时间，以系统时间为基准
	double pre_vpts, pre_vdelay;//记录上一个视频帧的时间戳和延迟

	//音频相关数据
	PacketQueue audio_queue;//音频压缩数据包队列
	AVPacket aPacket;//正在解码的音频数据包
	Uint8 *audio_buf;//正在播放的音频数据
	int audio_pkt_size;//正在解码音频数据包的剩余解码长度
	int audio_buf_idx, audio_buf_size;//已经送入播放器缓存的解码数据量和该帧总的数据量
	SwrContext *swrCtx;//音频重采样上下文
	double audio_pts;//当前解码音频帧的时间戳（播放完成时），经过时基换算
};

//初始化压缩数据包队列
void PacketQueueInit(PacketQueue *q)
{
	memset(q, 0, sizeof(PacketQueue));
	q->qMutex = SDL_CreateMutex();
	q->qCond = SDL_CreateCond();
}

//将压缩数据包pkt进队
void PacketQueuePut(PacketQueue *q, AVPacket *pkt)
{
	AVPacketList *pktList = (AVPacketList *)av_mallocz(sizeof(AVPacketList));
	pktList->pkt = *pkt;
	pktList->next = NULL;

	//访问共享内存区
	SDL_LockMutex(q->qMutex);
	if (q->size == 0)//队列中无数据
	{
		q->head = pktList;
		q->tail = pktList;
	}
	else
	{
		q->tail->next = pktList;
		q->tail = pktList;
	}
	q->size++;
	SDL_CondSignal(q->qCond);
	SDL_UnlockMutex(q->qMutex);
}

//从数据包队列中取出数据放到pkt中，成功放回0
int PacketQueueGet(PacketQueue *q, AVPacket *pkt)
{
	AVPacketList *pktList;
	//访问共享内存区
	SDL_LockMutex(q->qMutex);
	while(q->size == 0)//队列中无数据
	{
		SDL_CondWait(q->qCond, q->qMutex);//阻塞等待，并暂时释放互斥锁
	}
	if (q->head == NULL)
	{
		printf("packet queue is empty, failed!\n");
		return -1;
	}
	pktList = q->head;
	*pkt = q->head->pkt;
	q->head = q->head->next;
	q->size--;
	if (q->size == 0)
	{
		q->tail = NULL;
	}
	av_free(pktList);
	SDL_UnlockMutex(q->qMutex);
	
	return 0;
}

//获取实际的音频时间
double GetAudioTime(VideoIfo *av)
{
	double pts = av->audio_pts;
	int nb_samples = av->audio_buf_size / av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO) / 2;//缓存帧的采样数
	double frame_time = 1.0 * nb_samples / av->aStream->codec->sample_rate;//缓存帧的时长
	pts -= frame_time * (av->audio_buf_size - av->audio_buf_idx) / av->audio_buf_size;

	return pts;
}

//解码一帧音频数据，保存到av->audio_buf中
int DecodeAudioPacket(VideoIfo *av)
{
	AVPacket *pkt = &av->aPacket;
	AVFrame *frame = av_frame_alloc();

	for (;;)
	{
		while (av->audio_pkt_size > 0)
		{
			int gotFrame = 0;
			int consumed_pkt_size = avcodec_decode_audio4(av->aStream->codec, frame, &gotFrame, pkt);
			if (consumed_pkt_size < 0)//解码不到数据
			{
				av->audio_pkt_size = 0;
				break;
			}
			av->audio_pkt_size -= consumed_pkt_size;
			if (gotFrame)//读取到一帧音频
			{
				//重采样
				int nb_samples = swr_convert(av->swrCtx, (uint8_t **)&av->audio_buf, MAX_AUDIO_BUF_SIZE,
					(const uint8_t **)frame->data, frame->nb_samples);
				av->audio_buf_size = av_samples_get_buffer_size(NULL, av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO),
					nb_samples, AV_SAMPLE_FMT_S16, 1);
				if (av->audio_buf_size <= 0)
				{
					continue;
				}
				av->audio_pts += 1.0 * nb_samples / av->aStream->codec->sample_rate;
				av->audio_buf_idx = 0;
				av_frame_free(&frame);
				return 0;
			}
		}

		//正在解码数据包已经解码完成，需要读取下一个包
		if (pkt->data)//pkt中有数据，释放缓存空间，防止内存泄露
		{
			av_packet_unref(pkt);
		}
		if (PacketQueueGet(&av->audio_queue, pkt) != 0)
		{
			exit(0);
		}
		av->audio_pkt_size = pkt->size;
		if (pkt->pts != AV_NOPTS_VALUE)//更新音频时间戳
		{
			av->audio_pts = pkt->pts * av_q2d(av->aStream->time_base);
		}
	}
	return -1;
}

//音频回调函数
void AudioCallback(void *user_data, Uint8 *stream, int len)
{
	VideoIfo *av = (VideoIfo *)user_data;

	while(len > 0)
	{
		if (av->audio_buf_idx >= av->audio_buf_size)//缓存数据已全部送入播放器缓存，需要重新解码压缩数据包
		{
			DecodeAudioPacket(av);
		}
		//将解码的数据写入播放器缓存中
		int write_len = av->audio_buf_size - av->audio_buf_idx;
		if (write_len > len)
		{
			write_len = len;
		}
		memcpy(stream, av->audio_buf+av->audio_buf_idx, write_len);
		stream += write_len;
		len -= write_len;
		av->audio_buf_idx += write_len;
	}
}

//先用着咯
double synchronize_video(VideoIfo *av, AVFrame *src_frame, double pts) {
	/*----------检查显示时间戳----------*/
	if (pts != 0) {//检查显示时间戳是否有效
		// If we have pts, set video clock to it.
		av->video_pts = pts;//用显示时间戳更新已播放时间
	}
	else {//若获取不到显示时间戳
	 // If we aren't given a pts, set it to the clock.
		pts = av->video_pts;//用已播放时间更新显示时间戳
	}
	/*--------更新视频已经播时间--------*/
		// Update the video clock，若该帧要重复显示(取决于repeat_pict)，则全局视频播放时序video_clock应加上重复显示的数量*帧率
	double frame_delay = av_q2d(av->vStream->codec->time_base);//该帧显示完将要花费的时间
	// If we are repeating a frame, adjust clock accordingly,若存在重复帧，则在正常播放的前后两帧图像间安排渲染重复帧
	frame_delay += src_frame->repeat_pict*(frame_delay*0.5);//计算渲染重复帧的时值(类似于音符时值)
	av->video_pts += frame_delay;//更新视频播放时间

	return pts;//此时返回的值即为下一帧将要开始显示的时间戳
}

//视频解码线程函数
int DecodeThread(void *user_data)
{
	VideoIfo *av = (VideoIfo *)user_data;
	AVFrame *frame = av_frame_alloc();
	AVPacket *pkt = (AVPacket *)av_mallocz(sizeof(AVPacket));
	av_init_packet(pkt);

	//不断地从视频压缩数据包队列中读取数据包，进行解码，格式转换后放到图像缓存队列中
	for (;;)
	{
		if (pkt->data)
		{
			av_packet_unref(pkt);
		}
		if (PacketQueueGet(&av->video_queue, pkt) != 0)
		{
			exit(0);
		}
		int gotPicture;
		avcodec_decode_video2(av->vStream->codec, frame, &gotPicture, pkt);
		if (gotPicture)
		{
			//检查队列是否已满
			SDL_LockMutex(av->picq_mutex);
			while (av->picq_size >= PICTURE_QUEUE_SIZE)
			{
				SDL_CondWait(av->picq_cond_write, av->picq_mutex);
			}
			SDL_UnlockMutex(av->picq_mutex);

			VideoPicture *vp = &av->picture_queue[av->picq_widx];
			
			//该空间缓存是否未分配或者图像的宽高不正确，需要重新分配空间
			if (!vp->allocated || vp->width != av->width || vp->height != av->height)
			{
				if (vp->buffer)//确保没有分配空间，防止内存泄漏
				{
					av_free(vp->buffer);
				}
				if (vp->frameYUV)//确保没有分配空间，防止内存泄漏
				{
					av_frame_free(&vp->frameYUV);
				}

				vp->allocated = 1;
				vp->width = av->width;
				vp->height = av->height;

				//分配空间并给frameYUV挂上buffer缓存
				vp->buffer = (uint8_t *)av_mallocz(avpicture_get_size(AV_PIX_FMT_YUV420P, av->width, av->height));
				vp->frameYUV = av_frame_alloc();
				avpicture_fill((AVPicture *)vp->frameYUV, vp->buffer, AV_PIX_FMT_YUV420P, av->width, av->height);
			}

			//获取该帧的时间戳
			double pts = av_frame_get_best_effort_timestamp(frame) * av_q2d(av->vStream->time_base);
			vp->pts = synchronize_video(av, frame, pts);

			//对图像进行格式转换
			sws_scale(av->swsCtx, (const uint8_t *const*)frame->data, frame->linesize, 0, av->height,
				                    vp->frameYUV->data, vp->frameYUV->linesize);

			//队列长度+1，写位置指针也移动
			SDL_LockMutex(av->picq_mutex);
			av->picq_size++;
			av->picq_widx++;
			if (av->picq_widx >= PICTURE_QUEUE_SIZE)
			{
				av->picq_widx = 0;
			}
			SDL_CondSignal(av->picq_cond_read);
			SDL_UnlockMutex(av->picq_mutex);
		}
		
	}

	return 0;
}

//视频刷新定时器回调函数
static Uint32 RefreshTimerCallBack(Uint32 interval, void *user_data)
{
	SDL_Event event;
	event.user.data1 = user_data;
	event.type = VIDEO_REFRESH_EVENT;
	SDL_PushEvent(&event);
	return 0;
}

//视频刷新定时器
void VideoRefreshTimer(VideoIfo *av, int delay)
{
	SDL_AddTimer(delay, RefreshTimerCallBack, av);
}

//视频刷新函数，刷新屏幕并开启下一帧的定时器
void VideoRefresh(void *user_data)
{
	VideoIfo *av = (VideoIfo *)user_data;

//1.-------刷新显示当前帧到屏幕上---------
	//检查队列是否有数据
	SDL_LockMutex(av->picq_mutex);
	while (av->picq_size <= 0)
	{
		SDL_CondWait(av->picq_cond_read, av->picq_mutex);
	}
	SDL_UnlockMutex(av->picq_mutex);

	//将图片显示到屏幕上
	VideoPicture *vp = &av->picture_queue[av->picq_ridx];
	if (vp->frameYUV)
	{
		SDL_UpdateTexture(texture, NULL, vp->frameYUV->data[0], vp->frameYUV->linesize[0]);
		SDL_RenderCopy(renderer, texture, NULL, NULL);
		SDL_RenderPresent(renderer);
	}

	SDL_LockMutex(av->picq_mutex);
	av->picq_ridx++;
	if (av->picq_ridx >= PICTURE_QUEUE_SIZE)
	{
		av->picq_ridx = 0;
	}
	av->picq_size--;
	SDL_CondSignal(av->picq_cond_write);
	SDL_UnlockMutex(av->picq_mutex);

//2.-------计算下一帧的显示时间，并启动定时器--------
	double delay = vp->pts - av->pre_vpts;
	if (delay <= 0 || delay >= 1.0)//判断是否合理,否则沿用上一帧的延迟
	{
		delay = av->pre_vdelay;
	}
	av->pre_vdelay = delay;
	av->pre_vpts = vp->pts;

	//判断视频时间戳和音频时间戳的相对快慢，并进行相应调整
	double diff = vp->pts - GetAudioTime(av);
	if (diff < -delay)//视频慢了，快速播放下一帧
	{
		delay = 0;
	}
	else if(diff > delay)//视频快了，延迟播放下一帧
	{
		delay = 2 * delay;
	}

	//根据调整得到的delay开启定时器
	av->video_time += delay;
	double actual_delay = av->video_time - av_gettime() / 1000000.0;
	if (actual_delay < 0)
	{
		actual_delay = 0;
	}
	VideoRefreshTimer(av, int(actual_delay*1000+0.5));
}

//视频解析线程函数，打开视频文件找到码流并打开音频播放和视频解码线程
int ParseThread(void *user_data)
{
	VideoIfo *av = (VideoIfo *)user_data;
	AVPacket *avPacket;
	AVFrame *avFrame;

	//初始化变量
	av->avFormatCtx = avformat_alloc_context();
	avPacket = (AVPacket *)av_mallocz(sizeof(AVPacket));
	av_init_packet(avPacket);
	avFrame = av_frame_alloc();

	//打开视频文件
	if (avformat_open_input(&av->avFormatCtx, av->file_name, NULL, NULL) != 0)
	{
		printf("Could not open file!\n");
		return -1;
	}

	//找到文件码流信息
	if (avformat_find_stream_info(av->avFormatCtx, NULL) < 0)
	{
		printf("Could not find stream info!\n");
		return -1;
	}

	//打印视频信息
	av_dump_format(av->avFormatCtx, 0, NULL, 0);

	//找到视音频流下标
	av->video_idx = av->audio_idx = -1;
	for (int i = 0; i < av->avFormatCtx->nb_streams; ++i)
	{
		if (av->avFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO && av->video_idx < 0)
		{
			av->video_idx = i;
			continue;
		}
		if (av->avFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && av->audio_idx < 0)
		{
			av->audio_idx = i;
			continue;
		}
	}

	//没有视频和音频，退出
	if (av->video_idx == -1 && av->audio_idx == -1)
	{
		printf("Could not find video or audio streams!\n");
		return -1;
	}

	//打开解码视频流的相关组件，初始化av结构体中的相关视频信息
	if (av->video_idx >= 0 && av->video_idx < av->avFormatCtx->nb_streams)
	{
		av->vStream = av->avFormatCtx->streams[av->video_idx];
		av->vCodec = avcodec_find_decoder(av->vStream->codec->codec_id);//根据codec_id找到解码器
		if (av->vCodec == NULL)
		{
			printf("Could not find decoder!\n");
			return -1;
		}
		if (avcodec_open2(av->vStream->codec, av->vCodec, NULL) < 0)//打开解码器
		{
			printf("Could not open decoder!\n");
			return -1;
		}
		//初始化数据
		PacketQueueInit(&av->video_queue);
		av->picq_mutex = SDL_CreateMutex();
		av->picq_cond_write = SDL_CreateCond();
		av->picq_cond_read = SDL_CreateCond();
		av->width = av->vStream->codec->width;
		av->height = av->vStream->codec->height;
		renderer = SDL_CreateRenderer(screen, -1, 0);
		texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, av->width, av->height);
		//设置视频图像格式转换规则
		av->swsCtx = sws_getContext(av->width, av->height, av->vStream->codec->pix_fmt,
			av->width, av->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
		av->decode_tid = SDL_CreateThread(DecodeThread, "DecodeThread", av);
		VideoRefreshTimer(av, 1);
		av->video_pts = 0;
		av->pre_vpts = 0;
		av->pre_vdelay = 40e-3;
		av->video_time = av_gettime() / 1000000.0;
	}
	else
	{
		SDL_HideWindow(screen);
	}

	//打开解码音频流的相关组件，初始化av结构体中的相关音频信息
	if (av->audio_idx >= 0 && av->audio_idx < av->avFormatCtx->nb_streams)
	{
		av->aStream = av->avFormatCtx->streams[av->audio_idx];
		av->aCodec = avcodec_find_decoder(av->aStream->codec->codec_id);//根据codec_id找到解码器
		if (av->aCodec == NULL)
		{
			printf("Could not find decoder!\n");
			return -1;
		}
		if (avcodec_open2(av->aStream->codec, av->aCodec, NULL) < 0)//打开解码器
		{
			printf("Could not open decoder!\n");
			return -1;
		}
		//初始化数据
		PacketQueueInit(&av->audio_queue);
		av->audio_buf = (Uint8 *)av_mallocz(MAX_AUDIO_BUF_SIZE * 2);
		av->audio_buf_idx = 0;
		av->audio_buf_size = 0;
		av->audio_pkt_size = 0;
		//设置重采样规则
		int64_t in_channel_layout = av_get_default_channel_layout(av->aStream->codec->channels);
		av->swrCtx = swr_alloc();
		swr_alloc_set_opts(av->swrCtx, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, av->aStream->codec->sample_rate, 
			in_channel_layout, av->aStream->codec->sample_fmt, av->aStream->codec->sample_rate, 0, NULL);
		swr_init(av->swrCtx);
		//打开播放器
		SDL_AudioSpec wanted_spec;//播放参数
		wanted_spec.freq = av->aStream->codec->sample_rate;
		wanted_spec.format = AUDIO_S16;
		wanted_spec.channels = av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO);
		wanted_spec.samples = 1024;//或者直接1024
		wanted_spec.callback = AudioCallback;
		wanted_spec.userdata = av;
		wanted_spec.silence = 0;
		if (SDL_OpenAudio(&wanted_spec, NULL))
		{
			printf("Could not open audio player!\n");
			fprintf(stderr, "%s\n", SDL_GetError());
			return -1;
		}
		SDL_PauseAudio(0);
	}

	//获取每一帧数据包
	for (;;)
	{
		//队列满就延迟添加
		if (av->video_queue.size >= MAX_VIDEO_PACKET_SIZE || av->audio_queue.size >= MAX_AUDIO_PACKET_SIZE)
		{
			SDL_Delay(100);
			continue;
		}
		//没有读取到帧，延迟40ms等待数据传入
		if (av_read_frame(av->avFormatCtx, avPacket) < 0)
		{
			SDL_Delay(40);
			continue;
		}

		//将视音频压缩数据包扔进对应的队列中等待播放
		if (avPacket->stream_index == av->video_idx)//视频数据包
		{
			PacketQueuePut(&av->video_queue, avPacket);
		}
		else if(avPacket->stream_index == av->audio_idx)//音频数据包
		{
			PacketQueuePut(&av->audio_queue, avPacket);
		}
		else//其他数据不做处理，清空包中的缓存
		{
			av_packet_unref(avPacket);
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{	
	//注册ffmpeg所有组件
	av_register_all();

	//初始化SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("Could not init SDL!\n");
		return -1;
	}

	VideoIfo *av = (VideoIfo *)av_mallocz(sizeof(VideoIfo));
	memcpy(av->file_name, input_file, sizeof(input_file));
	screen = SDL_CreateWindow("MyPlayer", 50, 50, 1280, 720, SDL_WINDOW_OPENGL);

	//创建视频解析线程
	av->parse_tid = SDL_CreateThread(ParseThread, "ParseThread", av);

	//等待消息到来
	SDL_Event event;
	for (;;)
	{
		SDL_WaitEvent(&event);
		switch (event.type)
		{
		case SDL_QUIT:
			exit(0);
			break;
		case VIDEO_REFRESH_EVENT:
			VideoRefresh(event.user.data1);
			break;
		default:
			break;
		}
	}

	return 0;
}