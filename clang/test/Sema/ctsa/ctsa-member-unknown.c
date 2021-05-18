
// RUN: %clang_cc1 -fsyntax-only -verify -Wthread-safety -Wthread-safety-beta %s

#include "ctsa.h"

typedef unsigned short SEMID CAPABILITY("semaphore");

unsigned int acquireSem(SEMID semId) ACQUIRE(semId);
unsigned int releaseSem(SEMID semId) RELEASE(semId);

typedef struct SharedDataStruct
{
  // Group A data protected by groupASem
  // SEMID      groupASem;
  unsigned int   a GUARDED_BY(groupASem); // \
  // expected-error{{use of undeclared identifier 'groupASem'}}
  unsigned int   b;
} SharedDataStruct;

void foo(SharedDataStruct* ps1, SharedDataStruct* ps2)
{
  ps1->groupASem = 0; // expected-error-re{{no member named 'groupASem' in '{{(struct )?}}SharedDataStruct'}}
  acquireSem(ps1->groupASem); // expected-error-re{{no member named 'groupASem' in '{{(struct )?}}SharedDataStruct'}}
  ps1->a = ps2->a;
  releaseSem(ps1->groupASem); // expected-error-re{{no member named 'groupASem' in '{{(struct )?}}SharedDataStruct'}}
}



