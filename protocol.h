
// protocol.h

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifndef PROTOCOL_H
#define PROTOCOL_H

#define VERSION "1.1"

// includes

#include "util.h"

// variables

#ifdef _WIN32
extern CRITICAL_SECTION CriticalSection;
#else
extern  pthread_mutex_t CriticalSection;
#endif

extern int NumberThreads;

// functions

extern void loop  ();
extern void event ();
extern void book_parameter();
extern void profile();
extern void init_threads(bool call_option);

extern void get   (char string[], int size);
extern void send  (const char format[], ...);

#endif // !defined PROTOCOL_H

// end of protocol.h

