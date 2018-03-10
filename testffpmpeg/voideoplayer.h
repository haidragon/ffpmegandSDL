#ifndef VOIDEOPLAYER_H
#define VOIDEOPLAYER_H
#include<QThread>
#include <QImage>
extern "C"{
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <libavutil/mathematics.h>


#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <SDL2/SDL_types.h>
#include <SDL2/SDL_name.h>
#include <SDL2/SDL_main.h>
#include <SDL2/SDL_config.h>
}
typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 1
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio


typedef struct VideoState {
    AVCodecContext *aCodecCtx; //音频解码器
    AVFrame *audioFrame;// 解码音频过程中的使用缓存
    PacketQueue *audioq;

    double video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame

    AVStream *video_st;

} VideoState;

class voideoPlayer : public QThread
{
    Q_OBJECT
public:
    voideoPlayer();
    bool read();
    void SaveFrame(AVFrame *pFrame, int width, int height,int index);
protected:
    void run();
signals:
   void sig_GetOneFrame(QImage);

private:
    QString mFileName;
   VideoState mVideoState;
};

#endif // VOIDEOPLAYER_H
