// RUN: %clang_cc1 -fsyntax-only -verify -Wthread-safety -Wthread-safety-beta %s

#define uint16_t unsigned short
#define uint32_t unsigned long

#include "ctsa.h"

typedef uint16_t SEMID CAPABILITY("semaphore");

uint32_t acquireSem(SEMID semId) ACQUIRE(semId);
uint32_t releaseSem(SEMID semId) RELEASE(semId);

typedef struct SharedDataStruct
{
  // Group A data protected by groupASem
  SEMID      groupASem;
  uint32_t   a GUARDED_BY(groupASem);
  uint32_t   b;
} SharedDataStruct;

void foo(SharedDataStruct* ps1, SharedDataStruct* ps2)
{
  // acquireSem(ps1->groupASem);
  ps1->a = ps2->a; // \
  // expected-warning{{writing variable 'a' requires holding semaphore 'ps1->groupASem' exclusively}} \
  // expected-warning{{reading variable 'a' requires holding semaphore 'ps2->groupASem'}}
  releaseSem(ps1->groupASem); // expected-warning{{releasing semaphore 'ps1->groupASem' that was not held}}
}



