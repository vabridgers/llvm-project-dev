
// RUN: %clang_cc1 -fsyntax-only -verify -Wthread-safety -Wthread-safety-beta %s
//
#include "ctsa.h"

typedef unsigned long SEMID CAPABILITY("semaphore");

SEMID semId;
extern unsigned long* ptrSharedCount PT_GUARDED_BY(semId);
extern unsigned long sharedCount;

void LPP_releaseSem(SEMID semId, int v) RELEASE(semId);

int main() {
  unsigned long semVal = 0;
  ptrSharedCount = &sharedCount;
  // semVal = LPP_acquireSem(semId);
  *ptrSharedCount = *ptrSharedCount + 1; // \
  // expected-warning{{writing the value pointed to by 'ptrSharedCount' requires holding semaphore 'semId' exclusively}} \
  // expected-warning{{reading the value pointed to by 'ptrSharedCount' requires holding semaphore 'semId'}}
  LPP_releaseSem(semId, 0); // expected-warning{{releasing semaphore 'semId' that was not held}}
  return 0;
}
