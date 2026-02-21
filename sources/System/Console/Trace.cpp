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
  // Logging disabled: no-op
  (void)line;
}


//------------------------------------------------------------------------------

Trace::Logger *Trace::SetLogger(Trace::Logger& logger)
{
  // Disable changing logger; keep as no-op and return nullptr
  (void)logger;
  return nullptr;
}


//------------------------------------------------------------------------------

void Trace::VLog(const char* category,  const char *fmt, va_list args) 
{
  // Logging disabled: no-op
  (void)category; (void)fmt; (void)args;
}

//------------------------------------------------------------------------------

void Trace::Log(const char* category, const char *fmt, ...) 
{
  // Logging disabled: no-op
  (void)category; (void)fmt;
}


//------------------------------------------------------------------------------

void Trace::Debug(const char *fmt, ...) 
{
  // Debug logging disabled (no-op)
  (void)fmt;
}


//------------------------------------------------------------------------------

void Trace::Error(const char *fmt, ...) 
{
  // Error logging disabled (no-op)
  (void)fmt;
}


//------------------------------------------------------------------------------
