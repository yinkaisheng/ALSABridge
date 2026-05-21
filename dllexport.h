#ifndef _DLL_EXPORT_H_
#define _DLL_EXPORT_H_

#ifdef WIN32
	#ifdef BUILD_DLL
		#define DLL_EXPORT __declspec(dllexport)
	#else
		#define DLL_EXPORT
		//__declspec(dllimport)
	#endif
#else
	#ifdef BUILD_DLL
		#ifdef __GNUC__
			#define DLL_EXPORT extern "C" __attribute__ ((visibility("default")))
		#else
			#define DLL_EXPORT extern "C"
		#endif
	#else
		#define DLL_EXPORT
	#endif
#endif


#endif