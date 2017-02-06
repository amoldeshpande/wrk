extern "C" {
#include "../src/wrk.h"
#include "../src/ae.h"
#include "../src/main.h"
}
#include <MSWSock.h>
#include <unordered_map>
#include <memory>
#include <queue>

enum class IOOpCode
{
	None,
	Connect,
	Read,
	Write
};
struct IOContext
{
	OVERLAPPED overlap;
	IOOpCode opCode;	
	SOCKET socket;
	aeFileProc* completionProc;
	void* apiData;
	int validBytes;
	char buffer[RECVBUF];
	uint64_t startTime;
	IOContext(IOOpCode code,SOCKET s,aeFileProc* proc) 
		: opCode(code)
		, socket(s)
		, completionProc(proc)
	{
		memset(&overlap, 0, sizeof(overlap));
		startTime = time_us();
	}
private:
	IOContext(){ }
};
struct SocketContext
{
	SOCKET socket;
	int writesPending;
	std::queue<IOContext*> receivedData;
	connection *conn;
	SocketContext()
		: socket(INVALID_SOCKET)
		, writesPending(0)
		, conn(nullptr)
	{

	}
};

LPFN_CONNECTEX pfnConnectEx;

static std::unordered_map<SOCKET, std::shared_ptr<SocketContext>> s_knownSocketMap;

void aeMain(aeEventLoop *eventLoop) {
	eventLoop->stop = 0;
	while (!eventLoop->stop) {
		if (eventLoop->beforesleep != NULL)
			eventLoop->beforesleep(eventLoop);
		aeProcessEvents(eventLoop, AE_ALL_EVENTS);
	}
}
static void aeApiFree(aeEventLoop *eventLoop) {
	zfree(eventLoop->apidata);
}

void aeStop(aeEventLoop *eventLoop) {
	eventLoop->stop = 1;
}
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
	aeApiFree(eventLoop);
	zfree(eventLoop->events);
	zfree(eventLoop->fired);
	zfree(eventLoop);
}
int gettimeofday(struct timeval * tp, struct timezone * tzp);
static void aeGetTime(long *seconds, long *milliseconds)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	*seconds = tv.tv_sec;
	*milliseconds = tv.tv_usec / 1000;
}
static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
	long cur_sec, cur_ms, when_sec, when_ms;

	aeGetTime(&cur_sec, &cur_ms);
	when_sec = cur_sec + milliseconds / 1000;
	when_ms = cur_ms + milliseconds % 1000;
	if (when_ms >= 1000) {
		when_sec++;
		when_ms -= 1000;
	}
	*sec = when_sec;
	*ms = when_ms;
}
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
	aeTimeProc *proc, void *clientData,
	aeEventFinalizerProc *finalizerProc)
{
	long long id = eventLoop->timeEventNextId++;
	aeTimeEvent *te;

	te = (aeTimeEvent*)zmalloc(sizeof(*te));
	if (te == NULL) return AE_ERR;
	te->id = id;
	aeAddMillisecondsToNow(milliseconds, &te->when_sec, &te->when_ms);
	te->timeProc = proc;
	te->finalizerProc = finalizerProc;
	te->clientData = clientData;
	te->next = eventLoop->timeEventHead;
	eventLoop->timeEventHead = te;
	return id;
}
static char *aeApiName(void) {
	return "win_iocp";
}
char *aeGetApiName(void) {
	return aeApiName();
}
int aeCreateFileEvent(aeEventLoop *eventLoop, fd_t fd, int mask,
	aeFileProc *proc, void *clientData)
{
	auto iter = s_knownSocketMap.find(fd);
	if (iter == s_knownSocketMap.end())
	{
		return AE_ERR;
	}
	if (mask & AE_READABLE)
	{
		IOContext* ctx = new IOContext(IOOpCode::Read, fd, proc);
		ctx->completionProc = proc;
		ctx->apiData = clientData;
		DWORD flags = 0, bread = 0;
		WSABUF wbuf = { sizeof(ctx->buffer),ctx->buffer };		
		if (WSARecv(fd, &wbuf, 1, &bread, &flags, &ctx->overlap, NULL) == SOCKET_ERROR)
		{
			if (WSAGetLastError() != WSA_IO_PENDING)
			{
				delete ctx;
				return AE_ERR;
			}
			return AE_OK;
		}
		/*ctx->validBytes = bread;
		iter->second->receivedData.push(ctx);
		proc(eventLoop, fd, clientData, mask)*/;
	}
	if (mask & AE_WRITABLE)
	{
		if (iter->second->writesPending  == 0)
		{
			proc(eventLoop, fd, clientData, mask);
		}
	}
	return AE_OK;
}
void aeDeleteFileEvent(aeEventLoop *eventLoop, fd_t fd, int mask)
{
}
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {	
	BOOL bRet = FALSE;
	OVERLAPPED_ENTRY entries[128];
	DWORD numCompleted = 0,j;
	IOContext readContextSaved(IOOpCode::Read, INVALID_SOCKET, nullptr);
	uint64_t endTime,startTime = time_us();
	IOOpCode theCode = IOOpCode::None;
	static uint64_t lastTime;

	if (startTime - lastTime > 1000)
	{
		dprintf(" >1000 between calls to poll, %llu", startTime - lastTime);
	}
	lastTime = startTime;

	bRet = GetQueuedCompletionStatusEx(eventLoop->hCompletionPort, entries, ARRAYSIZE(entries), &numCompleted, tvp ? (tvp->tv_sec * 1000 + tvp->tv_usec / 1000) : INFINITE, FALSE);

	if (bRet == TRUE) 
	{
		for (j = 0; j < numCompleted; j++) {
			OVERLAPPED* pOVerlap = entries[j].lpOverlapped;
			IOContext *context = CONTAINING_RECORD(pOVerlap, IOContext, overlap);
			auto sockCtx = s_knownSocketMap.find(context->socket);
			if (sockCtx == s_knownSocketMap.end())
			{
				delete context;
				continue;
			}
			theCode = context->opCode;
			switch (context->opCode)
			{
			case IOOpCode::Read:
			{		
				context->validBytes = entries[j].dwNumberOfBytesTransferred;
				sockCtx->second->receivedData.push(context);
				readContextSaved = *context;
				if (time_us() - context->startTime > 5)
				{
					dprintf("Read took %d us", time_us() - context->startTime);
				}
			}
				break;
			case IOOpCode::Connect:
				//SetFileCompletionNotificationModes((HANDLE)context->socket, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
				if (time_us() - context->startTime > 5)
				{
					dprintf("Connect took %d us", time_us() - context->startTime);
				}
				break;			
			default:
				if (time_us() - context->startTime > 5)
				{
					dprintf("theCode %d took %d us",theCode, time_us() - context->startTime);
				}
				break;
			}
			theCode = context->opCode;
			if (context->completionProc != nullptr)
			{
				uint64_t end, start = time_us();
				context->completionProc(eventLoop, context->socket, context->apiData, 0);
				end = time_us() - start;
				if (end > 100)
				{
					dprintf("opcode completion %d took %lld us", theCode, end);
				}
			}
			if (theCode!= IOOpCode::Read)
			{
				delete context;
			}	
			else
			{
				aeCreateFileEvent(eventLoop, readContextSaved.socket, AE_READABLE, readContextSaved.completionProc, readContextSaved.apiData);
			}
		}
		endTime = time_us() - startTime;
		if (endTime >= 100)
		{
			dprintf("pol spent %llu us event %d tvp us %d, %d", endTime, theCode, tvp ? (tvp->tv_sec * 1000 + tvp->tv_usec / 1000) : -1, numCompleted);
		}
	}

	return numCompleted;
}
static int connect_socket(thread *thread, connection *c) {
	return -1;
}

int connect_socket_win(void *threadp, void *cp) {
	thread* thr = (thread*)threadp;
	connection * c = (connection*)cp;
	struct addrinfo *addr = thr->addr;
	sockaddr local = { 0 };
	char flags;
	GUID guid = WSAID_CONNECTEX;
	DWORD bytes = 0;
	auto newctx = std::make_shared<SocketContext>();

	newctx->socket = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
	newctx->conn = c;
	newctx->writesPending = 0;
	
	local.sa_family = addr->ai_family;
	if (bind(newctx->socket, &local, addr->ai_addrlen) == SOCKET_ERROR)
	{
		goto error;
	}

	if (WSAIoctl(newctx->socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &pfnConnectEx, sizeof(pfnConnectEx), &bytes, NULL, NULL) == SOCKET_ERROR) {
		goto error;
	}
	IOContext* ioctx = new IOContext(IOOpCode::Connect, newctx->socket, socket_connected);
	ioctx->apiData = c;

	if (CreateIoCompletionPort((HANDLE)newctx->socket, thr->hCompletionPort, 0, 0) == NULL) {
		goto error;
	}

	if (pfnConnectEx(newctx->socket, addr->ai_addr, addr->ai_addrlen,NULL,0,NULL,&ioctx->overlap) == FALSE) {
		if (WSAGetLastError() != WSA_IO_PENDING)
		{
			goto error;
		}
	}

	flags = 1;
	setsockopt(newctx->socket, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));


	c->parser.data = c;
	c->fd = newctx->socket;

	s_knownSocketMap.insert(std::make_pair(newctx->socket, newctx));

	return c->fd;


error:
	thr->errors.connect++;
	if (newctx != nullptr)
	{
		closesocket(newctx->socket);
	}
	
	return -1;
}
int reconnect_socket(thread *thread, connection *c) {
	aeDeleteFileEvent((aeEventLoop*)thread->loop, c->fd, AE_WRITABLE | AE_READABLE);
	close(c->fd);
	return connect_socket(thread, c);
}

/* Search the first timer to fire.
* This operation is useful to know how many time the select can be
* put in sleep without to delay any event.
* If there are no timers NULL is returned.
*
* Note that's O(N) since time events are unsorted.
* Possible optimizations (not needed by Redis so far, but...):
* 1) Insert the event in order, so that the nearest is just the head.
*    Much better but still insertion or deletion of timers is O(N).
* 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
*/
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop)
{
	aeTimeEvent *te = eventLoop->timeEventHead;
	aeTimeEvent *nearest = NULL;

	while (te) {
		if (!nearest || te->when_sec < nearest->when_sec ||
			(te->when_sec == nearest->when_sec &&
				te->when_ms < nearest->when_ms))
			nearest = te;
		te = te->next;
	}
	return nearest;
}

/* Process time events */
static int processTimeEvents(aeEventLoop *eventLoop) {
	int processed = 0;
	aeTimeEvent *te;
	long long maxId;
	time_t now = time(NULL);

	/* If the system clock is moved to the future, and then set back to the
	* right value, time events may be delayed in a random way. Often this
	* means that scheduled operations will not be performed soon enough.
	*
	* Here we try to detect system clock skews, and force all the time
	* events to be processed ASAP when this happens: the idea is that
	* processing events earlier is less dangerous than delaying them
	* indefinitely, and practice suggests it is. */
	if (now < eventLoop->lastTime) {
		te = eventLoop->timeEventHead;
		while (te) {
			te->when_sec = 0;
			te = te->next;
		}
	}
	eventLoop->lastTime = now;

	te = eventLoop->timeEventHead;
	maxId = eventLoop->timeEventNextId - 1;
	while (te) {
		long now_sec, now_ms;
		long long id;

		if (te->id > maxId) {
			te = te->next;
			continue;
		}
		aeGetTime(&now_sec, &now_ms);
		if (now_sec > te->when_sec ||
			(now_sec == te->when_sec && now_ms >= te->when_ms))
		{
			int retval;

			id = te->id;
			retval = te->timeProc(eventLoop, id, te->clientData);
			processed++;
			/* After an event is processed our time event list may
			* no longer be the same, so we restart from head.
			* Still we make sure to don't process events registered
			* by event handlers itself in order to don't loop forever.
			* To do so we saved the max ID we want to handle.
			*
			* FUTURE OPTIMIZATIONS:
			* Note that this is NOT great algorithmically. Redis uses
			* a single time event so it's not a problem but the right
			* way to do this is to add the new elements on head, and
			* to flag deleted elements in a special way for later
			* deletion (putting references to the nodes to delete into
			* another linked list). */
			if (retval != AE_NOMORE) {
				aeAddMillisecondsToNow(retval, &te->when_sec, &te->when_ms);
			}
			else {
				aeDeleteTimeEvent(eventLoop, id);
			}
			te = eventLoop->timeEventHead;
		}
		else {
			te = te->next;
		}
	}
	return processed;
}

/* Process every pending time event, then every pending file event
* (that may be registered by time event callbacks just processed).
* Without special flags the function sleeps until some file event
* fires, or when the next time event occurrs (if any).
*
* If flags is 0, the function does nothing and returns.
* if flags has AE_ALL_EVENTS set, all the kind of events are processed.
* if flags has AE_FILE_EVENTS set, file events are processed.
* if flags has AE_TIME_EVENTS set, time events are processed.
* if flags has AE_DONT_WAIT set the function returns ASAP until all
* the events that's possible to process without to wait are processed.
*
* The function returns the number of events processed. */
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
	int processed = 0;

	/* Nothing to do? return ASAP */
	if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

	/* Note that we want call select() even if there are no
	* file events to process as long as we want to process time
	* events, in order to sleep until the next time event is ready
	* to fire. */
	if (/*eventLoop->maxfd != -1 ||*/
		((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
		aeTimeEvent *shortest = NULL;
		struct timeval tv, *tvp;

		if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
			shortest = aeSearchNearestTimer(eventLoop);
		if (shortest) {
			long now_sec, now_ms;

			/* Calculate the time missing for the nearest
			* timer to fire. */
			aeGetTime(&now_sec, &now_ms);
			tvp = &tv;
			tvp->tv_sec = shortest->when_sec - now_sec;
			if (shortest->when_ms < now_ms) {
				tvp->tv_usec = ((shortest->when_ms + 1000) - now_ms) * 1000;
				tvp->tv_sec--;
			}
			else {
				tvp->tv_usec = (shortest->when_ms - now_ms) * 1000;
			}
			if (tvp->tv_sec < 0) tvp->tv_sec = 0;
			if (tvp->tv_usec < 0) tvp->tv_usec = 0;
		}
		else {
			/* If we have to check for events but need to return
			* ASAP because of AE_DONT_WAIT we need to se the timeout
			* to zero */
			if (flags & AE_DONT_WAIT) {
				tv.tv_sec = tv.tv_usec = 0;
				tvp = &tv;
			}
			else {
				/* Otherwise we can block */
				tvp = NULL; /* wait forever */
			}
		}

		processed = aeApiPoll(eventLoop, tvp);		
	}
	/* Check time events */
	if (flags & AE_TIME_EVENTS)
		processed += processTimeEvents(eventLoop);

	return processed; /* return the number of processed file/time events */
}

int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id)
{
	aeTimeEvent *te, *prev = NULL;

	te = eventLoop->timeEventHead;
	while (te) {
		if (te->id == id) {
			if (prev == NULL)
				eventLoop->timeEventHead = te->next;
			else
				prev->next = te->next;
			if (te->finalizerProc)
				te->finalizerProc(eventLoop, te->clientData);
			zfree(te);
			return AE_OK;
		}
		prev = te;
		te = te->next;
	}
	return AE_ERR; /* NO event with the specified ID found */
}
static uint64_t queryFreqMicro;
static int aeApiCreate(aeEventLoop *eventLoop) {
	eventLoop->apidata = nullptr;
	LARGE_INTEGER queryFreq;
	QueryPerformanceFrequency(&queryFreq);
	queryFreqMicro = queryFreq.QuadPart / 1000000LL;
	return 0;
}
aeEventLoop *aeCreateEventLoop(int setsize) {
	aeEventLoop *eventLoop;
	//int i;

	if ((eventLoop = (aeEventLoop*)zmalloc(sizeof(*eventLoop))) == NULL) goto err;
	eventLoop->events = nullptr;//zmalloc(sizeof(aeFileEvent)*setsize);
	eventLoop->fired = nullptr;//zmalloc(sizeof(aeFiredEvent)*setsize);
//	if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
	eventLoop->setsize = setsize;
	eventLoop->lastTime = time(NULL);
	eventLoop->timeEventHead = NULL;
	eventLoop->timeEventNextId = 0;
	eventLoop->stop = 0;
	eventLoop->beforesleep = NULL;
	if (aeApiCreate(eventLoop) == -1) goto err;
	/* Events with mask == AE_NONE are not set. So let's initialize the
	* vector with it. */
	/*for (i = 0; i < setsize; i++)
		eventLoop->events[i].mask = AE_NONE;*/
	return eventLoop;

err:
	if (eventLoop) {
		zfree(eventLoop->events);
		zfree(eventLoop->fired);
		zfree(eventLoop);
	}
	return NULL;
}

status sock_connect_win(connection *, char *) {
	return  OK;
}
status sock_close_win(connection *c) {
	auto iter = s_knownSocketMap.find(c->fd);
	if (iter != s_knownSocketMap.end())
	{
		s_knownSocketMap.erase(iter);
	}
	return  OK;
}
status sock_read_win(connection *c , size_t * n) {
	*n = 0;
	auto iter = s_knownSocketMap.find(c->fd);
	if(iter == s_knownSocketMap.end() )
	{
		return OK;
	}
	if (iter->second->receivedData.size() == 0)
	{
		ssize_t r;
		r = recv(c->fd, c->buf, sizeof(c->buf),0);
		*n = (size_t)r;
		return r >= 0 ? OK : ERROR;
	}
	auto context = iter->second->receivedData.front();
	iter->second->receivedData.pop();

	memcpy(c->buf, context->buffer, context->validBytes);
	
	*n = context->validBytes;

	delete context;

	return OK ;
}
status sock_write_win(connection *c, char *buf, size_t inlen, size_t *n) {
	ssize_t r;
	IOContext* ctx = new IOContext(IOOpCode::Write, c->fd, nullptr);
	WSABUF wbuf = { (DWORD)inlen,buf };
	r = WSASend(c->fd, &wbuf, 1,(DWORD*) &ctx->validBytes, 0, &ctx->overlap, NULL);
	if (r == SOCKET_ERROR) {
		switch (WSAGetLastError()) {
			case WSA_IO_PENDING:
				return RETRY;
			default:
			{
				delete ctx;
				return ERROR;
			}
		}
	}
	*n = (size_t)ctx->validBytes;
	//delete ctx;
	return OK;
}
size_t sock_readable_win(connection *c) {
	auto iter = s_knownSocketMap.find(c->fd);
	if (iter != s_knownSocketMap.end())
	{
		if (iter->second->receivedData.size() > 0)
		{
			auto context = iter->second->receivedData.front();
			return context->validBytes;
		}
		else
		{
			u_long n, rc;
			rc = ioctlsocket(c->fd, FIONREAD, &n);
			return rc == SOCKET_ERROR? 0 : n;
		}
	}
	return 0;
}
uint64_t time_us() {
	LARGE_INTEGER qpc = { 0 };
	QueryPerformanceCounter(&qpc);

	return qpc.QuadPart / queryFreqMicro;
}

///http://stackoverflow.com/questions/10905892/equivalent-of-gettimeday-for-windows
int gettimeofday(struct timeval * tp, struct timezone * tzp)
{
	// Note: some broken versions only have 8 trailing zero's, the correct epoch has 9 trailing zero's
	// This magic number is the number of 100 nanosecond intervals since January 1, 1601 (UTC)
	// until 00:00:00 January 1, 1970 
	static const uint64_t EPOCH = ((uint64_t)116444736000000000ULL);

	FILETIME    file_time;
	LARGE_INTEGER    time;

	GetSystemTimeAsFileTime(&file_time);
	time.LowPart = file_time.dwLowDateTime;
	time.HighPart = file_time.dwHighDateTime;

	tp->tv_sec = (long)((time.QuadPart - EPOCH) / 10000000L);
	tp->tv_usec = (long)((time.QuadPart - EPOCH) % 10000000L);
	return 0;
}
void
dprintf(char *format, ...)
{				/* } */
	va_list vl;
	char putbuf[2048];
	DWORD err;

	err = GetLastError();
	{
		va_start(vl, format);
		vsnprintf_s(putbuf,sizeof(putbuf),_TRUNCATE, format, vl);
		va_end(vl);
		OutputDebugString(putbuf);
		OutputDebugString("\n");
	}
	SetLastError(err);
}
