#pragma once

#define WIN32_LEAN_AND_MEAN
#define NO_MINMAX
#define NOGDI
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <stdint.h>
#define sleep(x)  Sleep((x)*1000)
typedef SSIZE_T ssize_t;
typedef HANDLE pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;
#define pthread_mutex_lock(a) EnterCriticalSection(a)
#define pthread_mutex_unlock(a) LeaveCriticalSection(a)
#define pthread_mutex_init(a,ignore) InitializeCriticalSection(a)
#define pthread_create(hndl,ignore,proc,prm) ( (*(hndl) = CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)proc,prm,0,NULL)) == NULL )
#define pthread_join(hndl,ignore) WaitForSingleObject(hndl,INFINITE)
#define pthread_self()  GetCurrentThread()

#define sigfillset
#define sigaction

#define poll WSAPoll
#define close closesocket
#define fcntl(a,b,c) 0
#define F_SETFL 0
#define F_GETFL 0
#define O_NONBLOCK 0

#define __sync_add_and_fetch(a,b) InterlockedExchangeAdd64(a,b)
#define __sync_val_compare_and_swap(ptr,oldval,newval) InterlockedCompareExchangePointer((PVOID)(ptr),(PVOID)newval,(PVOID)oldval)

#define VERSION "wrk-windows-x64"

#define HAVE_IOCP

#pragma warning(disable:4244 4267 4047)  // 4244:conversion from uint64_t to smaller size, 4267: from size_t to smaller size, 4047 int/handle mismatch


/////http://stackoverflow.com/questions/10905892/equivalent-of-gettimeday-for-windows
//inline int gettimeofday(struct timeval * tp, struct timezone * tzp)
//{
//	// Note: some broken versions only have 8 trailing zero's, the correct epoch has 9 trailing zero's
//	// This magic number is the number of 100 nanosecond intervals since January 1, 1601 (UTC)
//	// until 00:00:00 January 1, 1970 
//	static const uint64_t EPOCH = ((uint64_t)116444736000000000ULL);
//
//	FILETIME    file_time;
//	LARGE_INTEGER    time;
//
//	GetSystemTimeAsFileTime(&file_time);
//	time.LowPart = file_time.dwLowDateTime;
//	time.HighPart = file_time.dwHighDateTime;
//
//	tp->tv_sec = (long)((time.QuadPart - EPOCH) / 10000000L);
//	tp->tv_usec = (long)((time.QuadPart - EPOCH) % 10000000L);
//	return 0;
//}
int connect_socket_win(void *, void *);
int reconnect_socket_win(void *, void *);
extern void dprintf(char*fmt, ...);
