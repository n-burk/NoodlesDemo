# Stable source manifest for the hair-grooming demo systems.
#
# These deliberately live outside NoodlesDemoCore: the reusable package stays
# hair-agnostic, and the groom is an example consumer of it, exactly like the
# image-processing demo fixture. Paths are absolute so this fragment can be
# included from any source directory.
set(_NOODLES_DEMO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

set(NOODLES_DEMO_HAIR_INCLUDE_DIR "${_NOODLES_DEMO_ROOT}/Examples/Hair")

set(NOODLES_DEMO_HAIR_HEADERS
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairMath.h"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairTypes.h"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairGeometry.h"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairClumpMap.h"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairGeneration.h"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairGraph.h"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairEvaluator.h"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairTools.h"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairScene.h"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairRenderGeometry.h")

# Platform-neutral: no GL, no Apple headers. Linked into the CPU test binaries
# as well as both runnable demos.
set(NOODLES_DEMO_HAIR_SOURCES
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairGeometry.cpp"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairClumpMap.cpp"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairGeneration.cpp"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairGraph.cpp"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairEvaluator.cpp"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairTools.cpp"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairScene.cpp"
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairRenderGeometry.cpp")

# Requires a GL context; builds against desktop OpenGL 3.3 core and OpenGL ES 3.
set(NOODLES_DEMO_HAIR_GL_HEADERS
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairGLRenderer.h")
set(NOODLES_DEMO_HAIR_GL_SOURCES
  "${_NOODLES_DEMO_ROOT}/Examples/Hair/HairGLRenderer.cpp")

set(NOODLES_DEMO_HAIR_TEST_SOURCES
  "${_NOODLES_DEMO_ROOT}/tests/Hair/HairGroomTests.cpp")

unset(_NOODLES_DEMO_ROOT)
