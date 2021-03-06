/*
    Copyright (C) 2004 by Jorrit Tyberghein
	      (C) 2004 by Frank Richter

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/	

#include "csutil/sysfunc.h"
#include "csutil/win32/cachedll.h"

CS::Platform::Win32::CacheDLL* CS_API_NAME::dll = 0;
bool CS_API_NAME::dllLoaded = false;
int CS_API_NAME::refCount = 0;
#define FUNC_GROUP_BEGIN(name)	    \
  bool CS_API_NAME::name ## _avail = false;
#define FUNC_GROUP_END
#define FUNC(ret, name, args)		    \
  CS_API_NAME::PFN ## name CS_API_NAME::name = 0;
#define FUNC_OPT(ret, name, args)     FUNC(ret, name, args)
#include CS_API_FUNCTIONS
#undef FUNC_GROUP_BEGIN
#undef FUNC_GROUP_END
#undef FUNC_OPT
#undef FUNC

void CS_API_NAME::IncRef()
{
  refCount++;
}

void CS_API_NAME::DecRef()
{
  refCount--;
  if (refCount == 0)
  {
    delete dll; 
    dll = 0;
    dllLoaded = false;
  }
}

int CS_API_NAME::GetRefCount ()
{
  return refCount;
}

#define STRINGIZE2(x)   #x
#define STRINGIZE(x)    STRINGIZE2(x)

bool CS_API_NAME::Available ()
{
  if (!dllLoaded)
  {
    dllLoaded = true;
#if (CS_PROCESSOR_SIZE == 64) && defined(CS_API_DLL64)
	dll = new CS::Platform::Win32::CacheDLL (CS_API_DLL64);
#ifdef CS_DEBUG
    if (!dll || !*dll)
      csPrintf ("%s: Could not load %s\n", STRINGIZE (CS_API_NAME), 
		CS_API_DLL64);
#endif
#endif
    if (!dll || !*dll)
	  dll = new CS::Platform::Win32::CacheDLL (CS_API_DLL);
#ifdef CS_DEBUG
    if (!dll || !*dll)
      csPrintf ("%s: Could not load %s\n", STRINGIZE (CS_API_NAME), 
		CS_API_DLL);
#endif
    if ((dll != 0) && (*dll != 0))
    {
#ifdef CS_DEBUG
      bool* flag;
      #define FUNC_GROUP_BEGIN(name)					    \
        name ## _avail = true;						    \
	flag = &name ## _avail;
      #define FUNC_GROUP_END
      #define FUNC(ret, name, args)					    \
        *flag &= ((name = (PFN ## name)GetProcAddress (*dll, #name)) != 0); \
        if (!name) csPrintf ("%s: Could not obtain %s\n", 		    \
	  STRINGIZE (CS_API_NAME), #name);
      #define FUNC_OPT(ret, name, args)					    \
        name = (PFN ## name)GetProcAddress (*dll, #name);
      #include CS_API_FUNCTIONS
#else      
      #define FUNC_GROUP_BEGIN(name)	    \
        name ## _avail = true
      #define FUNC_GROUP_END		    ;
      #define FUNC(ret, name, args)		    \
        && (name = (PFN ## name)GetProcAddress (*dll, #name))
      #define FUNC_OPT(ret, name, args)					    \
        && ((name = (PFN ## name)GetProcAddress (*dll, #name)) || true)
      #include CS_API_FUNCTIONS
#endif
#undef FUNC_GROUP_BEGIN
#undef FUNC_GROUP_END
#undef FUNC_OPT
#undef FUNC
    }
  }
  return dll != 0;
}
