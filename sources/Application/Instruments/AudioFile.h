#ifndef _AUDIO_FILE_H_
#define _AUDIO_FILE_H_

#include "SoundSource.h"

// FFmpeg-based audio file loader
// Supports WAV, MP3, FLAC, OGG, AIFF, and many other formats
class AudioFile : public SoundSource {
public:
    static AudioFile *Open(const char *path);
    virtual ~AudioFile();
    
    // SoundSource interface
    virtual void *GetSampleBuffer(int note);
    virtual int GetSize(int note);
    virtual int GetChannelCount(int note);
    virtual int GetSampleRate(int note);
    virtual int GetRootNote(int note);
    virtual bool IsMulti() { return false; }
    
    // Load entire file into memory
    bool Load();
    
private:
    AudioFile(const char *path);
    
    char *path_;
    short *samples_;      // 16-bit interleaved samples
    int size_;            // Size in frames (samples per channel)
    int channelCount_;
    int sampleRate_;
};

#endif // _AUDIO_FILE_H_
