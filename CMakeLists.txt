cmake_minimum_required(VERSION 3.0)

# EIGEN3_ROOT needs to be set to the path of the Eigen top-level directory.
# PETSC_DIR and PETSC_ARCH need to be set.
# BLASTED_DIR may be set to the root of the BLASTed linear algebra library.
# BLASTED_BIN_DIR may be set to the root of the BLASTed build directory.

# Pass -DBOOST_ROOT=<path-to-Boost-root-directory> if Boost is not present in a default directory
# Pass -DWITH_BLASTED=1 to compile with BLASTed preconditioning support
# Pass -DNOOMP=1 to compile without OpenMP
# Pass -DSSE=1 to compile with SSE 4.2 instructions; ignored when compiling for KNC.
# Pass -DAVX=1 to compile with AVX instructions.
# Pass -DAVX_2=1 to compile with AVX 2.0 instructions.
# PASS -DZEN=1 to compiler for AMD Zen with AVX 2.0.
# Pass -DSKYLAKE=1 to compile with AVX-512 instructions for Xeon Skylake CPUs.
# Pass -DKNL=1 to compile for Xeon Phi Knights Landing.
# Pass -DMICKNC=1 to compile for Xeon Phi Knights Corner.
# Pass -DPROFILE=1 for profiling.
# Pass -DSLURM=1 for running the automated tests on a system managed by SLURM
# Pass -DNOTAGS=1 to NOT generate tags for navigating code, otherwise, exuberent-ctags is needed.

project (fvens)

# detect C++ compiler
if("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
  set(CXX_COMPILER_GNUCLANG TRUE)
  if("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU")
	set(CXX_COMPILER_GNU TRUE)
	message(STATUS "GNU C++ compiler detected")
  else()
	set(CXX_COMPILER_CLANG TRUE)
	message(STATUS "Clang C++ compiler detected")
  endif()
elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Intel")
  set(CXX_COMPILER_INTEL TRUE)
  message(STATUS "Intel C++ compiler detected")
elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Cray")
  set(CXX_COMPILER_CRAY TRUE)
  message(STATUS "Cray C++ compiler detected")
elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "PGI")
  set(CXX_COMPILER_PGI TRUE)
  message(STATUS "PGI C++ compiler detected")
else()
  message(SEND_ERROR "Could not identify compiler!")
endif()

# Basic options
if(CXX_COMPILER_GNUCLANG OR CXX_COMPILER_INTEL)
  set (CMAKE_CXX_FLAGS "-std=c++14 -Wall -Werror")
  set (CMAKE_CXX_FLAGS_DEBUG "-g")
  set (CMAKE_CXX_FLAGS_RELEASE "-O3")
elseif(CXX_COMPILER_PGI)
  set (CMAKE_CXX_FLAGS "-std=c++14")
  set (CMAKE_CXX_FLAGS_DEBUG "-g")
  set (CMAKE_CXX_FLAGS_RELEASE "-O2")
elseif(CXX_COMPILER_CRAY)
  set (CMAKE_CXX_FLAGS "-h std=c++14 -h error_on_warning")
  set (CMAKE_CXX_FLAGS_DEBUG "-g -O1")
  set (CMAKE_CXX_FLAGS_RELEASE "-O2")
endif()

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG=1")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -UDEBUG -DNDEBUG")

# Disable C - this should cause C compilation to fail
set(CMAKE_C_FLAGS "-std=c++14")

# to compile with OpenMP
if(NOOMP)
  if(CXX_COMPILER_GNUCLANG)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unknown-pragmas")
	set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -Wno-unknown-pragmas")
  endif()
  if(CXX_COMPILER_PGI)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -nomp")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -nomp")
  endif()
  if(CXX_COMPILER_CRAY)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -hnoomp")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -hnoomp")
  endif()
  message(STATUS "Compiling without OpenMP")
else()
  if(CXX_COMPILER_GNUCLANG)
	set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopenmp")
	message(STATUS "Compiling with OpenMP for GNU")
  elseif(CXX_COMPILER_INTEL)
	set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -qopenmp")
	message(STATUS "Compiling with OpenMP for Intel")
  elseif(CXX_COMPILER_CRAY)
	message(STATUS "Compiling with OpenMP for Cray")
  elseif(CXX_COMPILER_PGI)
	message(STATUS "Compiling with OpenMP for PGI")
  else()
	message(SEND_ERROR "Don't know OpenMP flag for this compiler!")
  endif()
endif()

# set verbosity options
if(CXX_COMPILER_GNU)
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopt-info-inline-vec-optimized-missed=optimizations.info")
elseif(CXX_COMPILER_CLANG)
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Rpass=inline,vectorize")
elseif(CXX_COMPILER_INTEL)
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -qopt-report=2")
elseif(CXX_COMPILER_CRAY)
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -hlist=a -hreport=fisv")
  set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O1")
endif()

# Architecture to target
if(MICKNC)
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mmic")
  message(STATUS "Compiling for KNC")
  if(NOT CXX_COMPILER_INTEL)
	message(SEND_ERROR "Need to use Intel compiler for Xeon Phi!")
  endif()
else()
  # Select hardware-specific optimizations
  if(AVX)
	if(CXX_COMPILER_GNUCLANG OR CXX_COMPILER_INTEL)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mavx")
	elseif(CXX_COMPILER_PGI)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -tp=sandybridge")
	endif()
	if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
	  message(STATUS "Compiling with AVX")
	endif()
  elseif(AVX_2)
	if(CXX_COMPILER_INTEL)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -xcore-avx2")
	elseif(CXX_COMPILER_GNUCLANG)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mavx2")
	elseif(CXX_COMPILER_PGI)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -tp=haswell")
	elseif(CXX_COMPILER_CRAY)
	  message(STATUS "Architecture options for Cray are to be set by the compiler wrapper.")
	  add_definitions(-DEIGEN_VECTORIZE_AVX2 -DEIGEN_VECTORIZE_FMA)
	else()
	  message(SEND_ERROR "Don't know/require option for this compiler!")
	endif()
	if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
	  message(STATUS "Compiling with AVX-2")
	endif()
  elseif(ZEN)
	if(CXX_COMPILER_INTEL)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -xcore-avx2")
	elseif(CXX_COMPILER_GNUCLANG)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=znver1")
	elseif(CXX_COMPILER_PGI)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -tp=zen")
	  add_definitions(-DEIGEN_VECTORIZE_AVX2 -DEIGEN_VECTORIZE_FMA)
	elseif(CXX_COMPILER_CRAY)
	  message(STATUS "Architecture options for Cray are to be set by the compiler wrapper.")
	  # But we need to tell Eigen something
	  add_definitions(-DEIGEN_VECTORIZE_AVX2 -DEIGEN_VECTORIZE_FMA)
	else()
	  message(SEND_ERROR "Don't know option for this compiler!")
	endif()
	if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
	  message(STATUS "Compiling with AVX 2.0 for AMD Zen")
	endif()
  elseif(SKYLAKE)
	if(CXX_COMPILER_INTEL)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -xcore-avx512")
	elseif(CXX_COMPILER_GNUCLANG)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=skylake-avx512")
	elseif(CXX_COMPILER_PGI)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -tp=skylake")
	  add_definitions(-DEIGEN_VECTORIZE_AVX512 -DEIGEN_VECTORIZE_FMA)
	elseif(CXX_COMPILER_CRAY)
	  message(STATUS "Architecture options for Cray are to be set by the compiler wrapper.")
	  add_definitions(-DEIGEN_VECTORIZE_AVX512 -DEIGEN_VECTORIZE_FMA)
	else()
	  message(SEND_ERROR "Don't know/require option for this compiler!")
	endif()
	if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
	  message(STATUS "Compiling with AVX-512 for Skylake")
	endif()
  elseif(KNL)
	if(CXX_COMPILER_INTEL)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -xmic-avx512")
	elseif(CXX_COMPILER_GNUCLANG)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=knl")
	elseif(CXX_COMPILER_PGI)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -tp=knl")
	  add_definitions(-DEIGEN_VECTORIZE_AVX512 -DEIGEN_VECTORIZE_FMA)
	elseif(CXX_COMPILER_CRAY)
	  message(STATUS "Architecture options for Cray are to be set by the compiler wrapper.")
	  add_definitions(-DEIGEN_VECTORIZE_AVX512 -DEIGEN_VECTORIZE_FMA)
	else()
	  message(SEND_ERROR "Don't know/require option for this compiler!")
	endif()
	if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
	  message(STATUS "Compiling with AVX-512 for KNL")
	endif()
  else()
	if(CXX_COMPILER_GNUCLANG OR CXX_COMPILER_INTEL)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -msse4.2")
	elseif(CXX_COMPILER_PGI)
	  set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -tp=x64,nehalem")
	endif()
	if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
	  message(STATUS "Compiling with SSE4")
	endif()
  endif()
endif()

# profiling
if(PROFILE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg -g")
endif()

# Boost
find_package(Boost 1.60 REQUIRED COMPONENTS program_options)
include_directories(${Boost_INCLUDE_DIRS})
message(STATUS "Found Boost libraries at ${Boost_LIBRARIES}")

# Eigen
find_path(EIGEN3_INCLUDE_DIR signature_of_eigen3_matrix_library
  PATHS ENV EIGEN3_ROOT DOC "Eigen3 include directory")
if(${EIGEN3_INCLUDE_DIR} STREQUAL "EIGEN3_INCLUDE_DIR-NOTFOUND")
  message(SEND_ERROR "Eigen3 not found!")
endif()
include_directories(${EIGEN3_INCLUDE_DIR})
message(STATUS "Found Eigen 3 at ${EIGEN3_INCLUDE_DIR}")

set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DEIGEN_DONT_PARALLELIZE")

# BLASTed
if(WITH_BLASTED)
  find_path(BLASTED_INCLUDE_DIR blasted_petsc.h PATHS ENV BLASTED_DIR PATH_SUFFIXES include
	DOC "Include directory of BLASTed library")
  if(${BLASTED_INCLUDE_DIR} STREQUAL "BLASTED_INCLUDE_DIR-NOTFOUND")
	message(SEND_ERROR "BLASTed requested, but header not found!")
  endif()
  include_directories(${BLASTED_INCLUDE_DIR})

  find_library(BLASTED_LIB blasted_petsc PATHS ENV BLASTED_BIN_DIR PATH_SUFFIXES lib
	DOC "Location of BLASTed library")
  if(${BLASTED_LIB} STREQUAL "BLASTED_LIB-NOTFOUND")
	message(SEND_ERROR "BLASTed requested, but library not found!")
  endif()

  message(STATUS "Building with BLASTed: ${BLASTED_INCLUDE_DIR}, ${BLASTED_LIB}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DUSE_BLASTED=1")
endif()

# MPI
find_package(MPI REQUIRED)
include_directories(${MPI_C_INCLUDE_PATH} ${MPI_CXX_INCLUDE_PATH})

# PETSc
# If PETSc variables were not passed to CMake, set them from environment variables
if(NOT DEFINED PETSC_DIR)
  set(PETSC_DIR $ENV{PETSC_DIR} CACHE PATH "PETSc install directory")
endif()
if(NOT DEFINED PETSC_ARCH)
  set(PETSC_ARCH $ENV{PETSC_ARCH} CACHE STRING "PETSc architecture to use")
endif()
find_library(PETSC_LIB NAMES petsc PATHS ${PETSC_DIR}/${PETSC_ARCH}/lib
  DOC "Location of PETSc library")
if(${PETSC_LIB} STREQUAL "PETSC_LIB-NOTFOUND")
  message(SEND_ERROR "PETSc library not found!")
endif()
include_directories(${PETSC_DIR}/include ${PETSC_DIR}/${PETSC_ARCH}/include)
message(STATUS "Building with PETSc found at ${PETSC_LIB}/${PETSC_ARCH}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DWITH_PETSC=1")

# ---------------------------------------------------------------------------- #

string(TOLOWER ${CMAKE_BUILD_TYPE} LCASE_CMAKE_BUILD_TYPE)
if(LCASE_CMAKE_BUILD_TYPE MATCHES "debug")
  message(STATUS "Debug build")
endif()
if(LCASE_CMAKE_BUILD_TYPE MATCHES "release")
  message(STATUS "Release build")
endif()

set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)

include_directories(${CMAKE_SOURCE_DIR}/src)

# libraries to be compiled
add_subdirectory(src)

# Tags
if(NOT NOTAGS)
  find_program(CTAGS_PATH ctags)
  if(CTAGS_PATH)
	message(STATUS "Found ctags: ${CTAGS_PATH}")
  else()
	message(STATUS "ctags not found! Tags cannot be generated.")
  endif(CTAGS_PATH)

  set_source_files_properties(tags PROPERTIES GENERATED true)
  add_custom_target ( tags
	COMMAND ctags -R --c++-kinds=+pf --extra=+q . 
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} 
	)

endif()

# Tests
enable_testing()
add_subdirectory(tests)

# Benchmarking
add_subdirectory(benchmarking)

