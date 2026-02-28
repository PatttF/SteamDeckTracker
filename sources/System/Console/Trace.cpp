#include "System/System/System.h"
#include "Trace.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>


Trace::Trace() 
:logger_(0)
{
}


//------------------------------------------------------------------------------

void Trace::AddLine(const char* line)
{
  if (logger_) {
    logger_->AddLine(line);
  } else {
    fprintf(stderr, "%s\n", line);
  }
}


//------------------------------------------------------------------------------

Trace::Logger *Trace::SetLogger(Trace::Logger& logger)
{
  Trace::Logger *prev = logger_;
  logger_ = &logger;
  return prev;
}


//------------------------------------------------------------------------------

void Trace::VLog(const char* category,  const char *fmt, va_list args) 
{
  char buffer[1024];
  int n = 0;
  if (category && category[0]) {
    n = snprintf(buffer, sizeof(buffer), "%s: ", category);
    if (n < 0) n = 0;
  }
  vsnprintf(buffer + n, sizeof(buffer) - n, fmt, args);
  Trace::GetInstance()->AddLine(buffer);
}

//------------------------------------------------------------------------------

void Trace::Log(const char* category, const char *fmt, ...) 
{
  va_list args;
  va_start(args, fmt);
  VLog(category, fmt, args);
  va_end(args);
}


//------------------------------------------------------------------------------

void Trace::Debug(const char *fmt, ...) 
{
  va_list args;
  va_start(args, fmt);
  VLog("DEBUG", fmt, args);
  va_end(args);
}


//------------------------------------------------------------------------------

void Trace::Error(const char *fmt, ...) 
{
  va_list args;
  va_start(args, fmt);
  VLog("ERROR", fmt, args);
  va_end(args);
}


//------------------------------------------------------------------------------
