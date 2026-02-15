#include "Logger.h"
#include <iostream>

void StdOutLogger::AddLine(const char *line)
{
	std::cout << line << std::endl ;
}

// ----------------------------------------------

FileLogger::FileLogger(const Path &path)
:path_(path)
,file_(0)
{
}

FileLogger::~FileLogger()
{
  if (file_)
  {
    fclose(file_);
  }
}

Result FileLogger::Init()
{
	file_= fopen(path_.GetPath().c_str(),"w") ;
  if (!file_)
  {
    return Result("Failed to open log file");
  }
  fclose(file_);
  return Result::NoError;
}

void FileLogger::AddLine(const char *line)
{
	file_= fopen(path_.GetPath().c_str(),"a") ;
	fprintf(file_,"%s\n",line) ;
  fclose(file_);
}

// ----------------------------------------------

TeeLogger::TeeLogger(const char *path)
: file_(nullptr)
{
  file_ = fopen(path, "w");
  if (!file_) {
    fprintf(stderr, "WARNING: Could not open log file %s\n", path);
  }
}

TeeLogger::~TeeLogger()
{
  if (file_) fclose(file_);
}

int TeeLogger::GetFd() const
{
  return file_ ? fileno(file_) : -1;
}

void TeeLogger::AddLine(const char *line)
{
  // Always write to stdout
  std::cout << line << std::endl;
  // Also write to log file
  if (file_) {
    fprintf(file_, "%s\n", line);
    fflush(file_);
  }
}