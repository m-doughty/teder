#ifndef TEDER_EXPORT_H
#define TEDER_EXPORT_H
#if defined(_WIN32)
#define TEDER_CALL __cdecl
#if defined(TEDER_STATIC)
#define TEDER_API
#elif defined(TEDER_BUILD)
#define TEDER_API __declspec(dllexport)
#else
#define TEDER_API __declspec(dllimport)
#endif
#else
#define TEDER_CALL
#define TEDER_API __attribute__((visibility("default")))
#endif
#endif
