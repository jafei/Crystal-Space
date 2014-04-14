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
static char const metainfo_embedgl[] =
"<?xml version=\"1.0\"?>"
"<!-- null2d.csplugin -->"
"<plugin>"
"  <scf>"
"    <classes>"
"      <class>"
"        <name>crystalspace.graphics2d.embedgl</name>"
"        <implementation>csGraphics2DGLEmbed</implementation>"
"        <description>Embedded gl 2D graphics driver for Crystal Space</description>"
"        <requires>"
"          <class>crystalspace.font.server.</class>"
"        </requires>"
"      </class>"
"    </classes>"
"  </scf>"
"</plugin>"
;
  #ifndef csGraphics2DGLEmbed_FACTORY_REGISTER_DEFINED 
  #define csGraphics2DGLEmbed_FACTORY_REGISTER_DEFINED 
    SCF_DEFINE_FACTORY_FUNC_REGISTRATION(csGraphics2DGLEmbed) 
  #endif

class embedgl
{
SCF_REGISTER_STATIC_LIBRARY(embedgl,metainfo_embedgl)
  #ifndef csGraphics2DGLEmbed_FACTORY_REGISTERED 
  #define csGraphics2DGLEmbed_FACTORY_REGISTERED 
    csGraphics2DGLEmbed_StaticInit csGraphics2DGLEmbed_static_init__; 
  #endif
public:
 embedgl();
};
embedgl::embedgl() {}

}