
// RUN: %clang_cc1 -fsyntax-only -verify -Wthread-safety -Wthread-safety-beta %s

// The purpose of this test is to ensure that no "holes" have been
// inadvertently opened in the syntactic, type, scoping, or other semantic
// rule checks for each of the C, C++, and FlexC languages.

// All code in the file should be okay when compiling as C++.
// expected-no-diagnostics
#include "ctsa.h"


typedef unsigned long SEMID CAPABILITY("semaphore");

#if defined(__cplusplus)

// The following series of class/struct declarations are intended to check
// that no C++ declarative features have been introduced into the FlexC
// compilation process.

class C1 { // flexc-error{{expected ';' after top level declarator}} \
           // flexc-error{{unknown type name 'class'}}
  unsigned int  x;
  unsigned int  y;
  unsigned int  z;
};

struct C2 {
public: // flexc-error{{type name requires a specifier or qualifier}}
  unsigned int  x; // flexc-error{{unexpected type name 'unsigned int': expected expression}}
private: // flexc-error{{type name requires a specifier or qualifier}}
  unsigned int  y; // flexc-error{{unexpected type name 'unsigned int': expected expression}}
  unsigned int  z;
};

struct C3 {
  void foo(); // flexc-error{{field 'foo' declared as a function}}
  void barr() {} // flexc-error{{field 'barr' declared as a function}} \
                 // flexc-error{{expected ';' at end of declaration list}}
  virtual unsigned int foobar() = 0;
  unsigned int  x;
  unsigned int  y;
  unsigned int  z;
};

struct C4 {
  C4(); // flexc-error{{type name requires a specifier or qualifier}} \
        // flexc-error{{expected ';' at end of declaration list}}
  ~C4(); // flexc-error{{type name requires a specifier or qualifier}} \
         // flexc-error{{expected member name or ';' after declaration specifiers}}
};
#endif

// This struct containing guarded data members and lock should only be
// permissible in C++ and FlexC. Standard C should consider 'lock' to
// be an undeclared identifier as C does not allow the C++ scoping and
// late parsing employed by FlexC for attribute argument processing.
static SEMID globLock;
struct CSTA_Struct {
  SEMID   lock;
  int a GUARDED_BY(globLock);
  int b GUARDED_BY(lock); // c-error{{use of undeclared identifier 'lock'}}
};

