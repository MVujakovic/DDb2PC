// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//
#define _CRT_SECURE_NO_DEPRECATE
#define _CRT_SECURE_NO_WARNINGS

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#define WIN32_LEAN_AND_MEAN        // Exclude rarely-used stuff from Windows headers

#define _CRTDBG_MAP_ALLOC  
#include <stdlib.h>
#include <crtdbg.h> 

#include "targetver.h"

#include <windows.h> 
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>


// TODO: reference additional headers your program requires here
// todo check if this is correct listing..imposible to suse other satic libraries in this static lib
