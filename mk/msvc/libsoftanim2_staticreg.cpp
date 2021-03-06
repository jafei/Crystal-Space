// This file is automatically generated.
#include "cssysdef.h"
#include "csutil/scf.h"

// Put static linking stuff into own section.
// The idea is that this allows the section to be swapped out but not
// swapped in again b/c something else in it was needed.
#if !defined(CS_DEBUG) && defined(CS_COMPILER_MSVC)
#pragma const_seg(".CSmetai")
#pragma comment(linker, "/section:.CSmetai,r")
#pragma code_seg(".CSmeta")
#pragma comment(linker, "/section:.CSmeta,er")
#pragma comment(linker, "/merge:.CSmetai=.CSmeta")
#endif

namespace csStaticPluginInit
{
static char const metainfo_softanim2[] =
"<?xml version=\"1.0\"?>"
"<!-- bullet2.csplugin -->"
"<plugin>"
"  <scf>"
"    <classes>"
"      <class>"
"        <name>crystalspace.physics.softanim2</name>"
"        <implementation>SoftBodyControlType2</implementation>"
"        <description>Generic animation of a genmesh from the simulation of soft bodies</description>"
"      </class>"
"    </classes>"
"  </scf>"
"</plugin>"
;
  #ifndef SoftBodyControlType2_FACTORY_REGISTER_DEFINED 
  #define SoftBodyControlType2_FACTORY_REGISTER_DEFINED 
    SCF_DEFINE_FACTORY_FUNC_REGISTRATION(SoftBodyControlType2) 
  #endif

class softanim2
{
SCF_REGISTER_STATIC_LIBRARY(softanim2,metainfo_softanim2)
  #ifndef SoftBodyControlType2_FACTORY_REGISTERED 
  #define SoftBodyControlType2_FACTORY_REGISTERED 
    SoftBodyControlType2_StaticInit SoftBodyControlType2_static_init__; 
  #endif
public:
 softanim2();
};
softanim2::softanim2() {}

}
