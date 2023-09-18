#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include<sys/time.h>

int main(int argc, char* argv[]) {
    //av_register_all();

    AVFormatContext* fmt_ctx = NULL;
    AVCodecContext* codec_ctx = NULL;
    AVCodec* codec = NULL;
    AVPacket pkt;
    int ret = 0;
    AVFrame* frame = NULL;
    int FrameCount = 0;
    int FrameNum = 0;
    int video_stream_index = -1;
    char outPath[10240];
    snprintf(outPath, sizeof(outPath), "mkdir -p output");
    struct timeval begin;
    struct timeval end;
    double duration;
    FILE* fp=NULL;
    int one_flag = 1;
    int benchmark = 0;
    double fps = 0;
    int width = 0, height=0;
    ret=system(outPath);
    if(ret!=0){
        fprintf(stderr, "Error: create output document failed.\n");
        goto end;
    }
    if(argc > 3 && !strcmp(argv[3], "-b"))
    {
	benchmark = 1;
    }
    // Open input file
    if (avformat_open_input(&fmt_ctx, argv[1], NULL, NULL) < 0) {
        fprintf(stderr, "Error: Could not open input file.\n");
        goto end;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Error: Could not find stream information.\n");
        goto end;
    }

    // Find the first video stream
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }

    if (video_stream_index == -1) {
        fprintf(stderr, "Error: Could not find video stream.\n");
        goto end;
    }
    width = fmt_ctx->streams[video_stream_index]->codecpar->width;
    height = fmt_ctx->streams[video_stream_index]->codecpar->height;
    // Get a pointer to the codec context for the video stream
    codec_ctx = avcodec_alloc_context3(NULL);
    if (!codec_ctx) {
        fprintf(stderr, "Error: Could not allocate codec context.\n");
        goto end;
    }
    avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[video_stream_index]->codecpar);

    // Find the decoder for the video stream
    //codec = avcodec_find_decoder(codec_ctx->codec_id);
    codec = avcodec_find_decoder_by_name(argv[2]);
    if (!codec) {
        fprintf(stderr, "Error: Could not find decoder.\n");
        goto end;
    }
    // Open codec
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stdout,"ok!\n");
        fprintf(stderr, "Error: Could not open codec.\n");
        goto end;
    }
    // Allocate video frame
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Error: Could not allocate video frame.\n");
        goto end;
    }

    //start = clock();
    //timer.reset();
    gettimeofday(&begin,NULL);
#if 1
    // Read frames from the file
    while (1) {

        ret = av_read_frame(fmt_ctx, &pkt);
        if(ret < 0 && one_flag==0)break;
        if(ret < 0 && one_flag==1){
            pkt.data = NULL;
            pkt.size = 0;
            pkt.stream_index = video_stream_index;
            one_flag = 0;
        }
        // If this is a packet from the video stream, decode it
        if (pkt.stream_index == video_stream_index) {
            // Send packet to decoder
                ret = avcodec_send_packet(codec_ctx, &pkt);
	        if (ret< 0) {
                    fprintf(stderr, "Error: Failed to send packet to decoder.\n");
                    return 1;
                }
                FrameNum++;
	    while(ret >= 0) {
                ret = avcodec_receive_frame(codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    fprintf(stdout, "Error: Could not decode video frame.");
                    return 1;
                }
                FrameCount++;
                if(!benchmark)
		{
		    memset(outPath, 0, sizeof(outPath));
		    snprintf(outPath, sizeof(outPath), "output/%d.yuv",FrameCount);
		    fp = fopen(outPath, "wb");
		    fwrite(frame->data[0], 1, frame->width * frame->height, fp);
		    fwrite(frame->data[1], 1, frame->width * frame->height / 2, fp);
		    fclose(fp);
		    fprintf(stdout, "This is the %d frame received!\n", FrameCount);
		}
                //if(FrameCount==2)goto ok;
    	    }
        }

   }
#endif 
    //sleep(3);
    gettimeofday(&end,NULL);
    //duration = (end.tv_sec-start.tv_sec)*1000000+(end.tv_usec-start.tv_usec);
    //printf("%.3f\n", duration);
#if 1
ok:
duration = (end.tv_sec-begin.tv_sec)*1000000+(end.tv_usec-begin.tv_usec);
duration /= 1000000;
fps = 1.0 * FrameCount / duration;
if(benchmark)
{
    fprintf(stdout, "All Frame Decoded Finished. [Resolution]: %dx%d, [Frame Num]: %d, [FPS]: %.3f, [TIME]: %.3fs\n", width, height, FrameCount, fps, duration);
}
else
{
    fprintf(stdout, "All Frame Decoded Finished. [Resolution]: %dx%d, [Frame Num]: %d\n", width, height, FrameCount);
}
#endif
end:
    // Clean up
    avformat_close_input(&fmt_ctx);
    avcodec_free_context(&codec_ctx);
    av_frame_free(&frame);
    return 0;
}

