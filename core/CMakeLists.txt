# heimdall_core: dependency-free engine core (C++ standard library only).
#
# Policy: this target must not link anything and its sources must not
# #include anything outside <...> (standard library / platform SDK) and
# "Heimdall/...". Enforced by test/src/CorePolicySpec.cpp.

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_library(heimdall_core STATIC ${CORE_SOURCES})
add_library(heimdall::core ALIAS heimdall_core)

target_include_directories(heimdall_core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(heimdall_core PUBLIC cxx_std_26)

# No exceptions / no RTTI in the hot path: errors are std::expected and
# ErrorNode/Diagnostic, never throws. Cheap to enforce now, costly later.
target_compile_options(heimdall_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-exceptions;-fno-rtti>
  $<$<CXX_COMPILER_ID:MSVC>:/GR-;/EHs-c->
)
