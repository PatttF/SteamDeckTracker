#pragma once

#include "Trace.h"
#include "System/Errors/Result.h"
#include "System/FileSystem/FileSystem.h"

#include <stdio.h>

class StdOutLogger: public Trace::Logger
{
  virtual void AddLine(const char*);
};

class FileLogger: public Trace::Logger
{
public:
  FileLogger(const Path& path);
  ~FileLogger();

  Result Init();

private:
  virtual void AddLine(const char*);
  Path path_;
  FILE *file_;
};

// Writes every log line to both stdout and a log file.
// Also exposes the file descriptor so the crash handler can
// write its backtrace to the same file.
class TeeLogger: public Trace::Logger
{
public:
  TeeLogger(const char *path);
  ~TeeLogger();
  int GetFd() const;  // raw fd for crash handler
private:
  virtual void AddLine(const char*);
  FILE *file_;
};