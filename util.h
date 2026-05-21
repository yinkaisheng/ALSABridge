#ifndef __UTIL_H_YKS__
#define __UTIL_H_YKS__
/*
usage:
#define UTIL_IMPLEMENTION
#define UTIL_NAMESPACE // optional
#include "util.h"
*/

#include <stdint.h>
#include <stdarg.h>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <thread>
#ifdef WIN32
#include <Windows.h>
#else
#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

//#define UTIL_IMPLEMENTION

#define    UTIL_PRINT_MACRO_HELPER(x)  #x
#define    UTIL_PRINT_MACRO(x)         #x "=" UTIL_PRINT_MACRO_HELPER(x)
//#pragma message(UTIL_PRINT_MACRO(YOUR_MACRO))

#ifdef UTIL_NAMESPACE
	#define ULOG(format, ...) util::log(__FILE__, __LINE__, __func__, format, ##__VA_ARGS__)
	#define ULOGIF(condition, format, ...) if (condition)	\
		util::log(__FILE__, __LINE__, __func__, format, ##__VA_ARGS__)
#else
	#define ULOG(format, ...) log(__FILE__, __LINE__, __func__, format, ##__VA_ARGS__)
	#define ULOGIF(condition, format, ...) if (condition)	\
		log(__FILE__, __LINE__, __func__, format, ##__VA_ARGS__)
#endif

#ifdef UTIL_VLOG
	#define VLOG ULOG
	#define VLOGIF ULOGIF
#else
	#define VLOG
	#define VLOGIF
#endif

#define UTIL_LOG_BUFFER_SIZE 1024

#ifdef UTIL_NAMESPACE
namespace util {
#endif

	template <typename Func>
	class FinalAction
	{
	public:
		FinalAction(Func func) : action(func)
		{

		}
		~FinalAction()
		{
			action();
		}
		FinalAction(const FinalAction<Func>&) = delete;
		FinalAction& operator=(const FinalAction<Func>&) = delete;
	private:
		Func action;
	};

	template <typename Func>
	FinalAction<Func> finallyDo(Func func)
	{
		return FinalAction{ func };
	}

	unsigned long thisThreadId();
	void sleepMSec(uint32_t msec);
	int64_t msecSinceStart();
	int64_t msecSinceEpoch();
	void print(const char* format, ...);
	void println(const char* format, ...);

	// return string length(not include null '\0') needed for the string after formatted
	// if return size >= input size, the buf is not enough, need a buffer with (return size) + 1 size
	int pformat(char* buf, int size, const char* format, ...);
	void log(const char* fileName, int line, const char* funcName, const char* format, ...);
	void writeFile(const void* data, int size, const char* mode, const char* format, ...);

#ifdef UTIL_IMPLEMENTION
	unsigned long thisThreadId()
	{
		thread_local unsigned long _thisThreadId{};
		if (_thisThreadId == 0)
		{
#if 1
		#if defined(WIN32)
			_thisThreadId = GetCurrentThreadId();
		#else
			_thisThreadId = syscall(__NR_gettid); // same as python3.8+ threading.get_native_id()
			//_thisThreadId = pthread_self(); // same as std::this_thread::get_id() and python3 threading.get_ident()
		#endif
#else
			std::stringstream ss;
			ss << std::this_thread::get_id();
			_thisThreadId = std::stoul(ss.str());
#endif
		}
		return _thisThreadId;
	}

	void sleepMSec(uint32_t msec)
	{
#ifdef WIN32
		::Sleep(msec);
#else
		struct timespec timesc;
		timesc.tv_sec = msec / 1000;
		timesc.tv_nsec = (msec % 1000) * 1000000;
		nanosleep(&timesc, nullptr);
#endif
	}

	int64_t msecSinceStart()
	{
		auto duration = std::chrono::steady_clock::now().time_since_epoch();
		int64_t milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
		return milliseconds;
	}

	int64_t msecSinceEpoch()
	{
		auto duration = std::chrono::system_clock::now().time_since_epoch();
		int64_t milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
		return milliseconds;
	}

	void print(const char* format, ...)
	{
		char buffer[UTIL_LOG_BUFFER_SIZE];
		va_list va;
		va_start(va, format);
		int needLen = std::vsnprintf(buffer, UTIL_LOG_BUFFER_SIZE, format, va);
		va_end(va);
		fputs(buffer, stdout);
	}

	void println(const char* format, ...)
	{
		char buffer[UTIL_LOG_BUFFER_SIZE];
		va_list va;
		va_start(va, format);
		int needLen = std::vsnprintf(buffer, UTIL_LOG_BUFFER_SIZE, format, va);
		va_end(va);
		if (needLen < UTIL_LOG_BUFFER_SIZE - 1)
		{
			buffer[needLen++] = '\n';
			buffer[needLen] = 0;
		}
		fputs(buffer, stdout);
	}

	int pformat(char* buf, int size, const char* format, ...)
	{
		va_list va;
		va_start(va, format);
		int needLen = std::vsnprintf(buf, size, format, va);
		va_end(va);
		return needLen;
	}

	void log(const char* fileName, int line, const char* funcName, const char* format, ...)
	{
		char buffer[UTIL_LOG_BUFFER_SIZE];

#if defined(_WIN32)
		struct tm timeInfo;
		struct timespec tsc;
		timespec_get(&tsc, TIME_UTC);
		//_tzset();
		localtime_s(&timeInfo, &tsc.tv_sec);
		int needLen = std::snprintf(buffer, UTIL_LOG_BUFFER_SIZE, "%04d-%02d-%02d %02d:%02d:%02d.%03ld T%lu %s,%d: ",
			timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, 
			timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec, tsc.tv_nsec / 1000000,
			thisThreadId(), funcName, line);
#else
		struct tm timeInfo;
		struct timeval tsc;
		gettimeofday(&tsc, NULL);
		//tzset();
		::localtime_r(&tsc.tv_sec, &timeInfo);
		int needLen = std::snprintf(buffer, UTIL_LOG_BUFFER_SIZE, "%04d-%02d-%02d %02d:%02d:%02d.%03ld T%lu %s,%d: ",
			timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
			timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec, tsc.tv_usec / 1000,
			thisThreadId(), funcName, line);
#endif

		if (needLen > 0 && needLen < UTIL_LOG_BUFFER_SIZE - 1)
		{
			va_list va;
			va_start(va, format);
			needLen += vsnprintf(buffer + needLen, UTIL_LOG_BUFFER_SIZE - needLen, format, va);
			va_end(va);
			if (needLen < UTIL_LOG_BUFFER_SIZE - 1)
			{
				buffer[needLen++] = '\n';
				buffer[needLen] = 0;
			}
		}
		fputs(buffer, stdout);
	}

	void writeFile(const void* data, int size, const char* mode, const char* format, ...)
	{
		char buffer[UTIL_LOG_BUFFER_SIZE];
		va_list va;
		va_start(va, format);
		int needLen = std::vsnprintf(buffer, UTIL_LOG_BUFFER_SIZE, format, va);
		va_end(va);
#ifdef WIN32
		FILE* file = nullptr;
		fopen_s(&file, buffer, mode);
#else
		FILE* file = fopen(buffer, mode);
#endif
		if (file)
		{
			fwrite(data, size, 1, file);
			fclose(file);
		}
	}

#endif //UTIL_IMPLEMENTION

#ifdef UTIL_NAMESPACE
}
#endif

#endif // util.h
