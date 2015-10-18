/// \brief This file takes care of handling command-line parameters and loading
/// the appropriate flavour of libtinycode-*.so

#include <cstdio>
#include <vector>
#include <memory>
#include <fstream>
#include <iostream>

extern "C" {
#include <dlfcn.h>
}

#include "llvm/ADT/ArrayRef.h"

#include "revamb.h"
#include "argparse.h"
#include "ptcinterface.h"
#include "ptctollvmir.h"

static const unsigned MAX_LIBRARY_NAME = 80;
static const unsigned BUF_SIZE = 4096;
static const unsigned MAX_INPUT_BUFFER = 10 * 1024 * 1024;

// TODO: rename
PTC ptc = {};                   ///< The interface with the PTC library.

struct ProgramParameters {
  const char *Architecture;
  const char *InputPath;
  const char *OutputPath;
  size_t Offset;
  DebugInfoType DebugInfo;
};

using LibraryDestructor = GenericFunctor<decltype(&dlclose), &dlclose>;
using LibraryPointer = std::unique_ptr<void, LibraryDestructor>;
using FileDestructor = GenericFunctor<decltype(&fclose), &fclose>;
using FilePointer = std::unique_ptr<FILE, FileDestructor>;

static const char *const Usage[] = {
  "revamb [options] [--] [INFILE [OUTFILE]]",
  nullptr,
};

/// Reads the whole specified file into a vector.
///
/// @param InputPath the path to the file to read, or nullptr for stdin.
/// @param Buffer the vector where the file content should be stored.
///
/// @return EXIT_SUCCESS if the file has been correctly read into the buffer.
static int ReadWholeInput(const char *InputPath, std::vector<uint8_t>& Buffer) {
  FilePointer InputFile;
  size_t TotalReadBytes = 0;

  // Prepare the buffer for the first read
  Buffer.resize(BUF_SIZE);

  // Try to open the input file
  if (InputPath != nullptr)
    InputFile.reset(fopen(InputPath, "r"));
  else
    InputFile.reset(stdin);

  if (!InputFile) {
    fprintf(stderr, "Couldn't open %s.\n", InputPath);
    return EXIT_FAILURE;
  }

  // Do the first read
  size_t ReadBytes = fread(Buffer.data(), sizeof(uint8_t), BUF_SIZE,
                           InputFile.get());
  TotalReadBytes += ReadBytes;

  // Read input until the end or we exceed the limit
  while (ReadBytes > 0 && TotalReadBytes < MAX_INPUT_BUFFER) {
    Buffer.resize(TotalReadBytes + BUF_SIZE);

    // Blank the newly allocated memory area before usage
    bzero(Buffer.data() + TotalReadBytes, BUF_SIZE);

    // Read BUF_SIZE bytes
    ReadBytes = fread(Buffer.data() + TotalReadBytes, sizeof(uint8_t), BUF_SIZE,
                      InputFile.get());

    TotalReadBytes += ReadBytes;
  }

  // Shrink the buffer
  Buffer.resize(TotalReadBytes);

  if (TotalReadBytes >= MAX_INPUT_BUFFER) {
    fprintf(stderr, "Input too large.\n");
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

/// Given an architecture name, loads the appropriate version of the PTC library,
/// and initializes the PTC interface.
///
/// @param Architecture the name of the architecture, e.g. "arm".
/// @param PTCLibrary a reference to the library handler.
///
/// @return EXIT_SUCCESS if the library has been successfully loaded.
static int loadPTCLibrary(const char *Architecture, LibraryPointer& PTCLibrary) {
  char LibraryName[MAX_LIBRARY_NAME];
  ptc_load_ptr_t ptc_load = nullptr;
  void *LibraryHandle = nullptr;

  // Build library name
  snprintf(LibraryName, MAX_LIBRARY_NAME, "libtinycode-%s.so", Architecture);

  // Look for the library in the system's paths
  LibraryHandle = dlopen(LibraryName, RTLD_LAZY);

  if (LibraryHandle == nullptr) {
    fprintf(stderr, "Couldn't load the PTC library: %s\n", dlerror());
    return EXIT_FAILURE;
  }

  // The library has been loaded, initialize the pointer, the caller will take
  // care of dlclose it from now on
  PTCLibrary.reset(LibraryHandle);

  // Obtain the address of the ptc_load entry point
  ptc_load = (ptc_load_ptr_t) dlsym(LibraryHandle, "ptc_load");

  if (ptc_load == nullptr) {
    fprintf(stderr, "Couldn't find ptc_load: %s\n", dlerror());
    return EXIT_FAILURE;
  }

  // Initialize the ptc interface
  if (ptc_load(LibraryHandle, &ptc) != 0) {
    fprintf(stderr, "Couldn't find PTC functions.\n");
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

/// Parses the input arguments to the program.
///
/// @param Argc number of arguments.
/// @param Argv array of strings containing the arguments.
/// @param Parameters where to store the parsed parameters.
///
/// @return EXIT_SUCCESS if the parameters have been successfully parsed.
static int parseArgs(int Argc, const char *Argv[],
                     ProgramParameters *Parameters) {
  const char *OffsetString = nullptr;
  const char *DebugString = nullptr;
  long long Offset = 0;

  // Initialize argument parser
  struct argparse Arguments;
  struct argparse_option Options[] = {
    OPT_HELP(),
    OPT_GROUP("Input description"),
    OPT_STRING('a', "architecture",
               &Parameters->Architecture,
               "the input architecture."),
    OPT_STRING('o', "offset",
               &OffsetString,
               "offset in the input where to start."),
    OPT_STRING('g', "debug",
               &DebugString,
               "emit debug information. Possible values are 'none' for no debug"
               " information, 'asm' for debug information referring to the"
               " assembly of the input file, 'ptc' for debug information"
               " referred to the Portable Tiny Code."),
    OPT_END(),
  };

  argparse_init(&Arguments, Options, Usage, 0);
  argparse_describe(&Arguments, "\nPTC translator.",
                    "\nTranslates a binary into QEMU Portable Tiny Code.\n");
  Argc = argparse_parse(&Arguments, Argc, Argv);

  // Check parameters
  if (Parameters->Architecture == nullptr) {
    fprintf(stderr, "Please specify the input architecture.\n");
    return EXIT_FAILURE;
  }

  if (OffsetString != nullptr) {
    if (sscanf(OffsetString, "%lld", &Offset) != 1) {
      fprintf(stderr, "-o parameter is not a number.\n");
      return EXIT_FAILURE;
    }

    Parameters->Offset = (size_t) Offset;
  }

  if (DebugString != nullptr) {
    if (strcmp("none", DebugString) == 0) {
      Parameters->DebugInfo = DebugInfoType::None;
    } else if (strcmp("asm", DebugString) == 0) {
      Parameters->DebugInfo = DebugInfoType::OriginalAssembly;
    } else if (strcmp("ptc", DebugString) == 0) {
      Parameters->DebugInfo = DebugInfoType::PTC;
    } else {
      fprintf(stderr, "Unexpected value for the -g parameter.\n");
      return EXIT_FAILURE;
    }
  }

  // Handle positional arguments
  if (Argc > 2) {
    fprintf(stderr, "Too many arguments.\n");
    return EXIT_FAILURE;
  }

  if (Argc >= 1)
    Parameters->InputPath = Argv[0];

  if (Argc == 2)
    Parameters->OutputPath = Argv[1];

  return EXIT_SUCCESS;
}

int main(int argc, const char *argv[]) {
  // Parse arguments
  ProgramParameters Parameters {};
  if (parseArgs(argc, argv, &Parameters) != EXIT_SUCCESS)
    return EXIT_FAILURE;

  // Load the appropriate libtyncode version
  LibraryPointer PTCLibrary;
  if (loadPTCLibrary(Parameters.Architecture, PTCLibrary) != EXIT_SUCCESS)
    return EXIT_FAILURE;

  // Open output file
  std::ostream *Output = nullptr;
  std::fstream OutputFile;
  if (Parameters.OutputPath != nullptr) {
    OutputFile.open(Parameters.OutputPath, std::fstream::out);
    Output = &OutputFile;
  } else
    Output = &std::cout;

  // Read the input from the appropriate file
  std::vector<uint8_t> Code;
  if (ReadWholeInput(Parameters.InputPath, Code) != EXIT_SUCCESS)
    return EXIT_FAILURE;

  // Translate everything
  Translate(*Output,
            llvm::ArrayRef<uint8_t>(Code.data() + Parameters.Offset,
                                    Code.size() - Parameters.Offset),
            Parameters.DebugInfo);

  return EXIT_SUCCESS;
}
