#include "AudioFile.h"
#include "System/Console/Trace.h"
#include "Foundation/Types/Types.h"
#include <cstring>
#include <cstdlib>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

AudioFile::AudioFile(const char *path) {
    path_ = strdup(path);
    samples_ = nullptr;
    size_ = 0;
    channelCount_ = 0;
    sampleRate_ = 0;
}

AudioFile::~AudioFile() {
    if (path_) free(path_);
    if (samples_) free(samples_);
}

AudioFile *AudioFile::Open(const char *path) {
    AudioFile *af = new AudioFile(path);
    if (af->Load()) {
        return af;
    }
    delete af;
    return nullptr;
}

bool AudioFile::Load() {
    AVFormatContext *formatCtx = nullptr;
    AVCodecContext *codecCtx = nullptr;
    SwrContext *swrCtx = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    int audioStreamIndex = -1;
    bool success = false;
    
    // Open input file
    if (avformat_open_input(&formatCtx, path_, nullptr, nullptr) < 0) {
        Trace::Error("AudioFile: Could not open %s", path_);
        return false;
    }
    
    // Find stream info
    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        Trace::Error("AudioFile: Could not find stream info");
        goto cleanup;
    }
    
    // Find audio stream
    for (unsigned int i = 0; i < formatCtx->nb_streams; i++) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = i;
            break;
        }
    }
    
    if (audioStreamIndex < 0) {
        Trace::Error("AudioFile: No audio stream found");
        goto cleanup;
    }
    
    {
        AVCodecParameters *codecPar = formatCtx->streams[audioStreamIndex]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(codecPar->codec_id);
        if (!codec) {
            Trace::Error("AudioFile: Unsupported codec");
            goto cleanup;
        }
        
        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            Trace::Error("AudioFile: Could not allocate codec context");
            goto cleanup;
        }
        
        if (avcodec_parameters_to_context(codecCtx, codecPar) < 0) {
            Trace::Error("AudioFile: Could not copy codec params");
            goto cleanup;
        }
        
        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            Trace::Error("AudioFile: Could not open codec");
            goto cleanup;
        }
        
        // Get channel layout
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, codecCtx->ch_layout.nb_channels);
        
        // Setup resampler to convert to 16-bit signed
        swr_alloc_set_opts2(&swrCtx,
            &outLayout, AV_SAMPLE_FMT_S16, codecCtx->sample_rate,
            &codecCtx->ch_layout, codecCtx->sample_fmt, codecCtx->sample_rate,
            0, nullptr);
        
        if (!swrCtx || swr_init(swrCtx) < 0) {
            Trace::Error("AudioFile: Could not init resampler");
            goto cleanup;
        }
        
        sampleRate_ = codecCtx->sample_rate;
        channelCount_ = codecCtx->ch_layout.nb_channels;
        
        // Allocate packet and frame
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!packet || !frame) {
            Trace::Error("AudioFile: Could not allocate packet/frame");
            goto cleanup;
        }
        
        // Estimate total samples and allocate buffer
        int64_t duration = formatCtx->duration; // in AV_TIME_BASE units
        int64_t estimatedFrames = (duration * sampleRate_) / AV_TIME_BASE;
        if (estimatedFrames <= 0) estimatedFrames = sampleRate_ * 60; // 1 minute fallback
        
        int bufferCapacity = (int)estimatedFrames;
        int bufferUsed = 0;
        samples_ = (short *)malloc(bufferCapacity * channelCount_ * sizeof(short));
        if (!samples_) {
            Trace::Error("AudioFile: Could not allocate sample buffer");
            goto cleanup;
        }
        
        // Read and decode all packets
        while (av_read_frame(formatCtx, packet) >= 0) {
            if (packet->stream_index == audioStreamIndex) {
                if (avcodec_send_packet(codecCtx, packet) >= 0) {
                    while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                        // Ensure buffer is large enough
                        int neededSize = bufferUsed + frame->nb_samples;
                        if (neededSize > bufferCapacity) {
                            bufferCapacity = neededSize * 2;
                            samples_ = (short *)realloc(samples_, bufferCapacity * channelCount_ * sizeof(short));
                            if (!samples_) {
                                Trace::Error("AudioFile: Realloc failed");
                                goto cleanup;
                            }
                        }
                        
                        // Convert samples
                        uint8_t *outBuf = (uint8_t *)(samples_ + bufferUsed * channelCount_);
                        int converted = swr_convert(swrCtx, &outBuf, frame->nb_samples,
                            (const uint8_t **)frame->extended_data, frame->nb_samples);
                        if (converted > 0) {
                            bufferUsed += converted;
                        }
                    }
                }
            }
            av_packet_unref(packet);
        }
        
        // Flush decoder
        avcodec_send_packet(codecCtx, nullptr);
        while (avcodec_receive_frame(codecCtx, frame) >= 0) {
            int neededSize = bufferUsed + frame->nb_samples;
            if (neededSize > bufferCapacity) {
                bufferCapacity = neededSize * 2;
                samples_ = (short *)realloc(samples_, bufferCapacity * channelCount_ * sizeof(short));
            }
            if (samples_) {
                uint8_t *outBuf = (uint8_t *)(samples_ + bufferUsed * channelCount_);
                int converted = swr_convert(swrCtx, &outBuf, frame->nb_samples,
                    (const uint8_t **)frame->extended_data, frame->nb_samples);
                if (converted > 0) {
                    bufferUsed += converted;
                }
            }
        }
        
        size_ = bufferUsed;
        success = (size_ > 0);
        
        Trace::Log("AudioFile", "Loaded %s: %d frames, %d channels, %d Hz", 
                   path_, size_, channelCount_, sampleRate_);
    }
    
cleanup:
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    if (swrCtx) swr_free(&swrCtx);
    if (codecCtx) avcodec_free_context(&codecCtx);
    if (formatCtx) avformat_close_input(&formatCtx);
    
    return success;
}

void *AudioFile::GetSampleBuffer(int note) {
    return samples_;
}

int AudioFile::GetSize(int note) {
    return size_;
}

int AudioFile::GetChannelCount(int note) {
    return channelCount_;
}

int AudioFile::GetSampleRate(int note) {
    return sampleRate_;
}

int AudioFile::GetRootNote(int note) {
    return 60;
}
