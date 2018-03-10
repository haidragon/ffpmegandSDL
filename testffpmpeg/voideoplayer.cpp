#include "voideoplayer.h"
#include<iostream>
#include<QDebug>
using namespace std;

#include <stdio.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

    AVPacketList *pkt1;
    if (av_dup_packet(pkt) < 0) {
        return -1;
    }
    pkt1 = (AVPacketList*)av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size)
{

    static AVPacket pkt;
    static uint8_t *audio_pkt_data = NULL;
    static int audio_pkt_size = 0;
    int len1, data_size;

    AVCodecContext *aCodecCtx = is->aCodecCtx;
    AVFrame *audioFrame = is->audioFrame;
    PacketQueue *audioq = is->audioq;

    for(;;)
    {
        if(packet_queue_get(audioq, &pkt, 1) < 0)
        {
            return -1;
        }
        audio_pkt_data = pkt.data;
        audio_pkt_size = pkt.size;
        while(audio_pkt_size > 0)
        {
            int got_picture;

            int ret = avcodec_decode_audio4( aCodecCtx, audioFrame, &got_picture, &pkt);
            if( ret < 0 ) {
                printf("Error in decoding audio frame.\n");
                exit(0);
            }

            if( got_picture ) {
                int in_samples = audioFrame->nb_samples;
                short *sample_buffer = (short*)malloc(audioFrame->nb_samples * 2 * 2);
                memset(sample_buffer, 0, audioFrame->nb_samples * 4);

                int i=0;
                float *inputChannel0 = (float*)(audioFrame->extended_data[0]);

                // Mono
                if( audioFrame->channels == 1 ) {
                    for( i=0; i<in_samples; i++ ) {
                        float sample = *inputChannel0++;
                        if( sample < -1.0f ) {
                            sample = -1.0f;
                        } else if( sample > 1.0f ) {
                            sample = 1.0f;
                        }

                        sample_buffer[i] = (int16_t)(sample * 32767.0f);
                    }
                } else { // Stereo
                    float* inputChannel1 = (float*)(audioFrame->extended_data[1]);
                    for( i=0; i<in_samples; i++) {
                        sample_buffer[i*2] = (int16_t)((*inputChannel0++) * 32767.0f);
                        sample_buffer[i*2+1] = (int16_t)((*inputChannel1++) * 32767.0f);
                    }
                }
//                fwrite(sample_buffer, 2, in_samples*2, pcmOutFp);
                memcpy(audio_buf,sample_buffer,in_samples*4);
                free(sample_buffer);
            }

            audio_pkt_size -= ret;

            if (audioFrame->nb_samples <= 0)
            {
                continue;
            }

            data_size = audioFrame->nb_samples * 4;

            return data_size;
        }
        if(pkt.data)
            av_free_packet(&pkt);
   }
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{
//    AVCodecContext *aCodecCtx = (AVCodecContext *) userdata;
    VideoState *is = (VideoState *) userdata;

    int len1, audio_data_size;

    static uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    /*   len是由SDL传入的SDL缓冲区的大小，如果这个缓冲未满，我们就一直往里填充数据 */
    while (len > 0) {
        /*  audio_buf_index 和 audio_buf_size 标示我们自己用来放置解码出来的数据的缓冲区，*/
        /*   这些数据待copy到SDL缓冲区， 当audio_buf_index >= audio_buf_size的时候意味着我*/
        /*   们的缓冲为空，没有数据可供copy，这时候需要调用audio_decode_frame来解码出更
         /*   多的桢数据 */

        if (audio_buf_index >= audio_buf_size) {
            audio_data_size = audio_decode_frame(is, audio_buf,sizeof(audio_buf));
            /* audio_data_size < 0 标示没能解码出数据，我们默认播放静音 */
            if (audio_data_size < 0) {
                /* silence */
                audio_buf_size = 1024;
                /* 清零，静音 */
                memset(audio_buf, 0, audio_buf_size);
            } else {
                audio_buf_size = audio_data_size;
            }
            audio_buf_index = 0;
        }
        /*  查看stream可用空间，决定一次copy多少数据，剩下的下次继续copy */
        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }

        memcpy(stream, (uint8_t *) audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}

static double synchronize_video(VideoState *is, AVFrame *src_frame, double pts) {

    double frame_delay;

    if (pts != 0) {
        /* if we have pts, set video clock to it */
        is->video_clock = pts;
    } else {
        /* if we aren't given a pts, set it to the clock */
        pts = is->video_clock;
    }
    /* update the video clock */
    frame_delay = av_q2d(is->video_st->codec->time_base);
    /* if we are repeating a frame, adjust clock accordingly */
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
    is->video_clock += frame_delay;
    return pts;
}


voideoPlayer::voideoPlayer()
{
//  read();
    mFileName  = "/Users/allenboy/Desktop/allen.mp4";
}

void voideoPlayer::run(){
    char *file_path = "/Users/allenboy/Desktop/allen.mp4";
    AVFormatContext *pFormatCtx;
    AVCodecContext *pCodecCtx;
    AVCodec *pCodec;
    AVFrame *pFrame, *pFrameRGB;
    AVPacket *packet;
    uint8_t *out_buffer;


    //////////////////////////////////////
    AVCodecContext *aCodecCtx;
    AVCodec *aCodec;


    static struct SwsContext *img_convert_ctx;

    int audioStream,videoStream, i, numBytes;
    int ret, got_picture;

    av_register_all(); //初始化FFMPEG  调用了这个才能正常适用编码器和解码器

    //Allocate an AVFormatContext.
    pFormatCtx = avformat_alloc_context();

    if (avformat_open_input(&pFormatCtx, file_path, NULL, NULL) != 0) {
        printf("can't open the file. ");
        return;
    }

    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        printf("Could't find stream infomation.");
        return ;
    }

    videoStream = -1;

    ///循环查找视频中包含的流信息，直到找到视频类型的流
    ///便将其记录下来 保存到videoStream变量中
    ///这里我们现在只处理视频流  音频流先不管他
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
        }
    }

    ///如果videoStream为-1 说明没有找到视频流
    if (videoStream == -1) {
        printf("Didn't find a video stream.");
        return ;
    }
    if (audioStream == -1) {
            printf("Didn't find a audio stream.\n");
            return;
     }
/////////////////////////////////////////
    ///查找音频解码器
        aCodecCtx = pFormatCtx->streams[audioStream]->codec;
        aCodec = avcodec_find_decoder(aCodecCtx->codec_id);

        if (aCodec == NULL) {
            printf("ACodec not found.\n");
            return;
        }

        ///打开音频解码器
            if (avcodec_open2(aCodecCtx, aCodec, NULL) < 0) {
                printf("Could not open audio codec.\n");
                return;
            }

            //初始化音频队列
            PacketQueue *audioq = new PacketQueue;
            packet_queue_init(audioq);

            // 分配解码过程中的使用缓存
              AVFrame* audioFrame = av_frame_alloc();

              mVideoState.aCodecCtx = aCodecCtx;
              mVideoState.audioq = audioq;
              mVideoState.audioFrame = audioFrame;

              ///  打开SDL播放设备 - 开始
              SDL_LockAudio();
              SDL_AudioSpec spec;
              SDL_AudioSpec wanted_spec;
              wanted_spec.freq = aCodecCtx->sample_rate;
              wanted_spec.format = AUDIO_S16SYS;
              wanted_spec.channels = aCodecCtx->channels;
              wanted_spec.silence = 0;
              wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
              wanted_spec.callback = audio_callback;
              wanted_spec.userdata = &mVideoState;
              if(SDL_OpenAudio(&wanted_spec, &spec) < 0)
              {
                  fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
                  return;
              }
              SDL_UnlockAudio();
              SDL_PauseAudio(0);
              ///  打开SDL播放设备 - 结束

  //////////////////////////////////
//    ///查找解码器
//    pCodecCtx = pFormatCtx->streams[videoStream]->codec;
//    pCodec = avcodec_find_decoder(pCodecCtx->codec_id);


//    if (pCodec == NULL) {
//        printf("Codec not found.");
//        return ;
//    }

//    ///打开解码器
//    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
//        printf("Could not open codec.");
//        return ;
//    }

//    pFrame = av_frame_alloc();
//    pFrameRGB = av_frame_alloc();

//    img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
//            pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
//            AV_PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);

//    numBytes = avpicture_get_size(AV_PIX_FMT_RGB32, pCodecCtx->width,pCodecCtx->height);

//    out_buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
//    avpicture_fill((AVPicture *) pFrameRGB, out_buffer, AV_PIX_FMT_RGB32,
//            pCodecCtx->width, pCodecCtx->height);

//    int y_size = pCodecCtx->width * pCodecCtx->height;

//    packet = (AVPacket *) malloc(sizeof(AVPacket)); //分配一个packet
//    av_new_packet(packet, y_size); //分配packet的数据

//    av_dump_format(pFormatCtx, 0, file_path, 0); //输出视频信息

//    int index = 0;
//    int64_t start_time = av_gettime();
//    int64_t pts = 0; //当前视频的pts

    ///////////////////////////////

              ///查找视频解码器
                pCodecCtx = pFormatCtx->streams[videoStream]->codec;
                pCodec = avcodec_find_decoder(pCodecCtx->codec_id);

                if (pCodec == NULL) {
                    printf("PCodec not found.\n");
                    return;
                }

                ///打开视频解码器
                if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
                    printf("Could not open video codec.\n");
                    return;
                }

                mVideoState.video_st = pFormatCtx->streams[videoStream];

                pFrame = av_frame_alloc();
                pFrameRGB = av_frame_alloc();

                ///这里我们改成了 将解码后的YUV数据转换成RGB32
                img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
                        pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
                        AV_PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);

                numBytes = avpicture_get_size(AV_PIX_FMT_RGB32, pCodecCtx->width,pCodecCtx->height);

                out_buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
                avpicture_fill((AVPicture *) pFrameRGB, out_buffer, AV_PIX_FMT_RGB32,
                        pCodecCtx->width, pCodecCtx->height);

                int y_size = pCodecCtx->width * pCodecCtx->height;

                packet = (AVPacket *) malloc(sizeof(AVPacket)); //分配一个packet
                av_new_packet(packet, y_size); //分配packet的数据

                av_dump_format(pFormatCtx, 0, file_path, 0); //输出视频信息



                 int64_t start_time = av_gettime();
                 int64_t pts = 0; //当前视频的pts

    while (1)
    {
        if (av_read_frame(pFormatCtx, packet) < 0)
        {
            break; //这里认为视频读取完了
        }
        int64_t realTime = av_gettime() - start_time; //主时钟时间
        while(pts > realTime)
        {
            SDL_Delay(10);
            realTime = av_gettime() - start_time; //主时钟时间
        }

        if (packet->stream_index == videoStream) {
            ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture,packet);
            if (packet->dts == AV_NOPTS_VALUE && pFrame->opaque&& *(uint64_t*) pFrame->opaque != AV_NOPTS_VALUE)
            {
                pts = *(uint64_t *) pFrame->opaque;
            }
            else if (packet->dts != AV_NOPTS_VALUE)
            {
                pts = packet->dts;
            }
            else
            {
                pts = 0;
            }
            pts *= 1000000 * av_q2d(mVideoState.video_st->time_base);
            pts = synchronize_video(&mVideoState, pFrame, pts);

            if (ret < 0) {
                printf("decode error.");
                return ;
            }

            if (got_picture) {
                sws_scale(img_convert_ctx,
                        (uint8_t const * const *) pFrame->data,
                        pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data,
                        pFrameRGB->linesize);

//                 SaveFrame(pFrameRGB, pCodecCtx->width,pCodecCtx->height,index++); //保存图片
//                 if (index > 50) return 0; //这里我们就保存50张图片

                //把这个RGB数据 用QImage加载
                QImage tmpImg((uchar *)out_buffer,pCodecCtx->width,pCodecCtx->height,QImage::Format_RGB32);
                QImage image = tmpImg.copy(); //把图像复制一份 传递给界面显示
//                    #include "voideoplayer.h"
//                 signals:
//                    void sig_GetOneFrame(QImage);
                emit sig_GetOneFrame(image);  //发送信号

            }
        }
        av_free_packet(packet);
    }
    av_free(out_buffer);
    av_free(pFrameRGB);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);
}


//bool voideoPlayer::read(){
//    char *file_path = "/Users/allenboy/Desktop/allen.mp4";
//    AVFormatContext *pFormatCtx;
//    AVCodecContext *pCodecCtx;
//    AVCodec *pCodec;
//    AVFrame *pFrame, *pFrameRGB;
//    AVPacket *packet;
//    uint8_t *out_buffer;

//    static struct SwsContext *img_convert_ctx;

//    int videoStream, i, numBytes;
//    int ret, got_picture;

//    av_register_all(); //初始化FFMPEG  调用了这个才能正常适用编码器和解码器

//    //Allocate an AVFormatContext.
//    pFormatCtx = avformat_alloc_context();

//    if (avformat_open_input(&pFormatCtx, file_path, NULL, NULL) != 0) {
//        printf("can't open the file. ");
//        return -1;
//    }

//    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
//        printf("Could't find stream infomation.");
//        return -1;
//    }

//    videoStream = -1;

//    ///循环查找视频中包含的流信息，直到找到视频类型的流
//    ///便将其记录下来 保存到videoStream变量中
//    ///这里我们现在只处理视频流  音频流先不管他
//    for (i = 0; i < pFormatCtx->nb_streams; i++) {
//        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
//            videoStream = i;
//        }
//    }

//    ///如果videoStream为-1 说明没有找到视频流
//    if (videoStream == -1) {
//        printf("Didn't find a video stream.");
//        return -1;
//    }

//    ///查找解码器
//    pCodecCtx = pFormatCtx->streams[videoStream]->codec;
//    pCodec = avcodec_find_decoder(pCodecCtx->codec_id);

//    if (pCodec == NULL) {
//        printf("Codec not found.");
//        return -1;
//    }

//    ///打开解码器
//    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
//        printf("Could not open codec.");
//        return -1;
//    }

//    pFrame = av_frame_alloc();
//    pFrameRGB = av_frame_alloc();
//    pCodecCtx->time_base.den = 30;
////    pCodecCtx->time_base = (AVRational){1,25};
//    pCodecCtx->time_base.num = 1;
//    img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
//            pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
//            AV_PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);

//    numBytes = avpicture_get_size(AV_PIX_FMT_RGB32, pCodecCtx->width,pCodecCtx->height);

//    out_buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
//    avpicture_fill((AVPicture *) pFrameRGB, out_buffer, AV_PIX_FMT_RGB32,
//            pCodecCtx->width, pCodecCtx->height);

//    int y_size = pCodecCtx->width * pCodecCtx->height;

//    packet = (AVPacket *) malloc(sizeof(AVPacket)); //分配一个packet
//    av_new_packet(packet, y_size); //分配packet的数据

//    av_dump_format(pFormatCtx, 0, file_path, 0); //输出视频信息

//    int index = 0;

//    while (1)
//    {
//        if (av_read_frame(pFormatCtx, packet) < 0)
//        {
//            break; //这里认为视频读取完了
//        }

//        int64_t realTime = av_gettime() - start_time; //主时钟时间
//                while(pts > realTime)
//                {
//                    SDL_Delay(10);
//                    realTime = av_gettime() - start_time; //主时钟时间
//                }


//        if (packet->stream_index == videoStream) {
//            ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture,packet);

//            if (ret < 0) {
//                printf("decode error.");
//                return -1;
//            }
////             time_base(1,24);

////            videoStream->time_base(1,24);
//            if (got_picture) {
//                sws_scale(img_convert_ctx,
//                        (uint8_t const * const *) pFrame->data,
//                        pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data,
//                        pFrameRGB->linesize);

////                 SaveFrame(pFrameRGB, pCodecCtx->width,pCodecCtx->height,index++); //保存图片
////                 if (index > 50) return 0; //这里我们就保存50张图片

//                //把这个RGB数据 用QImage加载
//                QImage tmpImg((uchar *)out_buffer,pCodecCtx->width,pCodecCtx->height,QImage::Format_RGB32);
//                QImage image = tmpImg.copy(); //把图像复制一份 传递给界面显示
////                    #include "voideoplayer.h"
////                 signals:
////                    void sig_GetOneFrame(QImage);

//                emit sig_GetOneFrame(image);  //发送信号

//            }
//        }
//        av_free_packet(packet);
//    }
//    av_free(out_buffer);
//    av_free(pFrameRGB);
//    avcodec_close(pCodecCtx);
//    avformat_close_input(&pFormatCtx);
//}
void voideoPlayer::SaveFrame(AVFrame *pFrame, int width, int height,int index)
{

 FILE *pFile;
 char szFilename[32];
 int  y;

 // Open file
 sprintf(szFilename, "frame%d.ppm", index);
 pFile=fopen(szFilename, "wb");

 if(pFile==NULL)
   return;

 // Write header
 fprintf(pFile, "P6%d %d255", width, height);

 // Write pixel data
 for(y=0; y<height; y++)
 {
   fwrite(pFrame->data[0]+y*pFrame->linesize[0], 1, width*3, pFile);
 }

 // Close file
 fclose(pFile);

}
