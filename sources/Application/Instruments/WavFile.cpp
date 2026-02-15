
#include "WavFile.h"
#include "System/Console/Trace.h"
#include "Foundation/Types/Types.h"
#include "Services/Time/TimeService.h"
#include "Application/Model/Config.h"
#include <stdlib.h>

int WavFile::bufferChunkSize_=-1 ;
bool WavFile::initChunkSize_=true ;

short Swap16 (short from)
{
#ifdef __ppc__
	short result;
	((char*)&result)[0] = ((char*)&from)[1];
	((char*)&result)[1] = ((char*)&from)[0];
	return  result;
#else
	return from;
#endif	
}

int Swap32 (int from)
{
#ifdef __ppc__
	int result;
	((char*)&result)[0] = ((char*)&from)[3];
	((char*)&result)[1] = ((char*)&from)[2];
	((char*)&result)[2] = ((char*)&from)[1];
	((char*)&result)[3] = ((char*)&from)[0];			 
	return  result;
#else
	return from;
#endif 	
}


WavFile::WavFile(I_File *file) {
	if (initChunkSize_) {
		const char *size=Config::GetInstance()->GetValue("SAMPLELOADCHUNKSIZE") ;
		if (size) {
			bufferChunkSize_=atoi(size) ;
		}
		initChunkSize_=false;
	}
	samples_=0 ;
	size_=0 ;
	readBuffer_=0 ;
	readBufferSize_=0 ;
	sampleBufferSize_=0 ;
	file_=file ;
	// default to PCM
	sampleFormat_ = 1;
} ;

WavFile::~WavFile() {
	if (file_) {
		file_->Close() ;
		delete file_ ;
	}
	SAFE_FREE(samples_) ;
	SAFE_FREE(readBuffer_) ;
} ;

WavFile *WavFile::Open(const char *path) {

    // open file

	FileSystem *fs=FileSystem::GetInstance() ;
	I_File *file=fs->Open(path,"r") ;
	
	if (!file) return 0 ;

	WavFile *wav=new WavFile(file) ;

        
        // Get data
        
/*        file->Seek(0,SEEK_SET) ;
        file->Read(fileBuffer,filesize,1) ;
        uchar *ptr=fileBuffer ;*/
        
//Trace::Dump("Loading sample from %s",path) ;

	long position=0 ;

	// Read 'RIFF'

	unsigned int chunk ;

	position+=wav->readBlock(position,4) ;
	memcpy(&chunk,wav->readBuffer_,4) ;
	chunk = Swap32(chunk);
		
	if (chunk!=0x46464952) {
		Trace::Error("Bad RIFF format %x",chunk) ;
		delete(wav) ;
		return 0 ;
	}


	// Read size

	unsigned int size ;
	position+=wav->readBlock(position,4) ;
	memcpy(&size,wav->readBuffer_,4) ;
	size = Swap32(size);

	// Read WAVE

	position+=wav->readBlock(position,4) ;
	memcpy(&chunk,wav->readBuffer_,4) ;
	chunk = Swap32(chunk);

	if (chunk!=0x45564157) {
		Trace::Error("Bad WAV format") ;
		delete wav ;
		return 0 ;
	}

    // Read fmt or JUNK

    position += wav->readBlock(position, 4);
    memcpy(&chunk,wav->readBuffer_,4) ;
	chunk = Swap32(chunk);

        // Read (possible) JUNK

    if (chunk == 0x4b4e554a) {
        position+=wav->readBlock(position,4) ;
        memcpy(&size, wav->readBuffer_,4) ;
        size = Swap32(size) ;
        
        position+=size;
        position += wav->readBlock(position, 4);
        memcpy(&chunk,wav->readBuffer_,4) ;
		chunk = Swap32(chunk);
    }

    // Read fmt

    if (chunk!=0x20746D66) {
		Trace::Error("Bad WAV/fmt format") ;
		delete wav ;
		return 0 ;
	}

	// Read subchunk size

	position+=wav->readBlock(position,4) ;
	memcpy(&size,wav->readBuffer_,4) ;
	size = Swap32(size);

	if (size<16) {
		Trace::Error("Bad fmt size format") ;
		delete wav ;
		return 0 ;
	}
	int offset=size-16 ;

	// Read compression

	unsigned short comp ;
	position+=wav->readBlock(position,2) ;
	memcpy(&comp,wav->readBuffer_,2) ;
	comp = Swap16(comp);

	// Read NumChannels (mono/Stereo)

	unsigned short nChannels ;
	position+=wav->readBlock(position,2) ;
	memcpy(&nChannels,wav->readBuffer_,2) ;
	nChannels = Swap16(nChannels);

	// Read Sample rate 

	unsigned int sampleRate ;

	position+=wav->readBlock(position,4) ;
	memcpy(&sampleRate,wav->readBuffer_,4) ;
	sampleRate = Swap32(sampleRate);

	// Skip byteRate & blockalign

	position+=6 ;

	short bitPerSample ;
	position+=wav->readBlock(position,2) ;
	memcpy(&bitPerSample,wav->readBuffer_,2) ;
	bitPerSample = Swap16(bitPerSample);

	// Some files have extensions in the fmt chunk (WAVE_FORMAT_EXTENSIBLE)
	// If comp == 0xFFFE (extensible), read the extension to get the true subformat
	if (comp == 0xFFFE && offset > 0) {
		// read extension block
		position += wav->readBlock(position, offset);
		// cbSize at offset 0 (2 bytes), then validBits (2), channelMask (4), SubFormat (16)
		if (wav->readBuffer_ && offset >= 24) {
			unsigned short subFormatTag = 0;
			// SubFormat starts at byte index 8 inside the extension
			memcpy(&subFormatTag, (char*)wav->readBuffer_ + 8, 2);
			subFormatTag = Swap16(subFormatTag);
			comp = subFormatTag; // treat subformat as compression tag (1==PCM,3==IEEE_FLOAT)
		}
	} else {
		// if there is an extra offset, skip it
		if (offset) {
			position+=offset ;
		}
	}

	// Accept PCM (1) and IEEE float (3)
	if (comp!=1 && comp!=3) {
		Trace::Error("Unsupported compression: %d", comp) ;
		delete wav ;
		return 0 ;
	}
	wav->bytePerSample_ = bitPerSample / 8;
	wav->sampleFormat_ = comp;

	// read data subchunk header
	//Trace::Dump("data subch") ;

	position+=wav->readBlock(position,4) ;
	memcpy(&chunk,wav->readBuffer_,4) ;
	chunk = Swap32(chunk);
	

	while (chunk!=0x61746164) {
		position+=wav->readBlock(position,4) ;
		memcpy(&size,wav->readBuffer_,4) ;
		size = Swap32(size);

		position+=size ;
		position+=wav->readBlock(position,4) ;
		memcpy(&chunk,wav->readBuffer_,4) ;
		chunk = Swap32(chunk);
	}

        wav->sampleRate_=sampleRate ;
       	wav->channelCount_=nChannels ;

	// Read data size in byte

	position+=wav->readBlock(position,4) ;
	memcpy(&size,wav->readBuffer_,4) ;
	size = Swap32(size);

	wav->size_ = size / nChannels / wav->bytePerSample_; // Size in samples (per channel)

	wav->dataPosition_=position ;

	return wav ;
} ; 

void *WavFile::GetSampleBuffer(int note) {
	return samples_ ;
} ;

int WavFile::GetSize(int note) {
	return size_ ;
} ;

int WavFile::GetChannelCount(int note) {
    return channelCount_ ;
} ;

int WavFile::GetSampleRate(int note) {
    return sampleRate_ ;
} ;

long WavFile::readBlock(long start,long size) {
	if (size>readBufferSize_) {
		SAFE_FREE(readBuffer_) ;
		readBuffer_=SYS_MALLOC(size) ;
		readBufferSize_=size ;
	}
  if (!readBuffer_)
  {
    Trace::Error("Failed to allocate read buffer of size %d",size);
  } 
  else 
  {
  	file_->Seek(start,SEEK_SET) ;
    file_->Read(readBuffer_,size,1) ;
  }
	return size ;
} ;


bool WavFile::GetBuffer(long start,long size) {

	// compute the sample buffer size we need,
	// allocate if needed

	int sampleBufferSize=2*channelCount_*size ;
	if (sampleBufferSize>sampleBufferSize_) {
		SAFE_FREE(samples_) ;
		samples_=(short *)SYS_MALLOC(sampleBufferSize) ;
		sampleBufferSize_=sampleBufferSize ;
	}

  if (!samples_)
  {
    Trace::Error("Failed to allocate %d samples",sampleBufferSize);
  }

	// compute the file buffer size we need to read

	int bufferSize = size * channelCount_ * bytePerSample_ ;
	int bufferStart = dataPosition_ + start * channelCount_ * bytePerSample_ ;

	// Read the buffer into a temporary raw buffer
	unsigned char *raw = (unsigned char *)SYS_MALLOC(bufferSize);
	if (!raw) {
		Trace::Error("WavFile::GetBuffer: failed to allocate raw buffer %d", bufferSize);
		return false;
	}

	int count = bufferSize;
	int offset = 0;
	int readSize = (bufferChunkSize_>0) ? bufferChunkSize_ : (count>4096?4096:count);
	while (count > 0) {
		int block = (count > readSize) ? readSize : count;
		readBlock(bufferStart, block);
		if (readBuffer_)
			memcpy(raw + offset, readBuffer_, block);
		bufferStart += block;
		count -= block;
		offset += block;
		if (bufferChunkSize_>0) TimeService::GetInstance()->Sleep(1);
	}

	// decode raw bytes into 16-bit signed samples (interleaved)
	short *out = samples_;
	int frames = size;
	for (int f = 0; f < frames; f++) {
		for (int ch = 0; ch < channelCount_; ch++) {
			int idx = (f * channelCount_ + ch) * bytePerSample_;
			int16_t value = 0;
			if (bytePerSample_ == 1) {
				unsigned char u = raw[idx];
				int v = (int)u - 128;
				value = (int16_t)(v << 8);
			} else if (bytePerSample_ == 2) {
				int16_t s;
				memcpy(&s, raw + idx, 2);
				s = Swap16(s);
				value = s;
			} else if (bytePerSample_ == 3) {
				int32_t v = (raw[idx] & 0xFF) | ((raw[idx+1] & 0xFF) << 8) | ((raw[idx+2] & 0xFF) << 16);
				if (v & 0x800000) v |= 0xFF000000;
				value = (int16_t)(v >> 8);
			} else if (bytePerSample_ == 4) {
				if (sampleFormat_ == 3) {
					// 32-bit IEEE float
					float fval = 0.0f;
					memcpy(&fval, raw + idx, 4);
					int32_t iv = (int32_t)(fval * 32767.0f);
					if (iv > 32767) iv = 32767;
					if (iv < -32768) iv = -32768;
					value = (int16_t)iv;
				} else {
					// 32-bit integer PCM
					int32_t v = 0;
					memcpy(&v, raw + idx, 4);
					v = Swap32(v);
					value = (int16_t)(v >> 16);
				}
			}
			*out++ = value;
		}
	}

	SYS_FREE(raw);
	return true;
} ;

void WavFile::Close() {
	file_->Close() ;
	SAFE_DELETE(file_) ;
	SAFE_FREE(readBuffer_) ;
	readBufferSize_=0 ;
} ;

int WavFile::GetRootNote(int note) {
	return 60 ;
} 
