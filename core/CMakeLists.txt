# cpplsp_core: dependency-free engine core (C++ standard library only).
#
# Policy: this target must not link anything and its sources must not
# #include anything outside <...> (standard library / platform SDK) and
# "CppLsp/...". Enforced by test/src/CorePolicySpec.cpp.

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_library(cpplsp_core STATIC ${CORE_SOURCES})
add_library(cpplsp::core ALIAS cpplsp_core)

target_include_directories(cpplsp_core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(cpplsp_core PUBLIC cxx_std_26)

# No exceptions / no RTTI in the hot path: errors are std::expected and
# ErrorNode/Diagnostic, never throws. Cheap to enforce now, costly later.
target_compile_options(cpplsp_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-exceptions;-fno-rtti>
  $<$<CXX_COMPILER_ID:MSVC>:/GR-;/EHs-c->
)
