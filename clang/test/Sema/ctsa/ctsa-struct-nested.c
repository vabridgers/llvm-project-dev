
// RUN: %clang_cc1 -fsyntax-only -verify -Wthread-safety -Wthread-safety-beta %s

#include "ctsa.h"

typedef int SEMID CAPABILITY("mutex");

void LPP_releaseSem(SEMID semId, int v) RELEASE(semId);
void LPP_acquireSem(SEMID semId) ACQUIRE(semId);

typedef struct InnerS {
  // x protected by lock
  SEMID      lock;
  unsigned long   x GUARDED_BY(lock);
} InnerT;

typedef struct OuterS {
  // Group A data protected by groupASem
  SEMID      groupASem;
  unsigned long   a GUARDED_BY(groupASem);

  InnerT     b;
  InnerT     c;
} OuterT;

typedef struct {
  SEMID      bagSem;
  union {
    long             foo;
    unsigned long    bar;
  } x GUARDED_BY(bagSem);
} bag_t;

bag_t GlobBag1;

void foo(OuterT* ps1, OuterT* ps2)
{
  LPP_acquireSem(ps1->groupASem);
  LPP_acquireSem(ps2->groupASem);
  ps1->a = ps2->a;
  LPP_acquireSem(ps1->b.lock); // expected-note{{mutex acquired here}}
  ps2->a = ps1->b.x + ps1->c.x; // \
  // expected-warning{{reading variable 'x' requires holding semaphore 'ps1->c.lock'}} \
  // expected-note{{found near match 'ps1->b.lock'}}
  LPP_releaseSem(ps2->groupASem, 0);
  LPP_releaseSem(ps1->groupASem, 0);

  GlobBag1.x.foo.hi_left = 0; // \
  // expected-warning{{writing variable 'x' requires holding semaphore 'GlobBag1.bagSem' exclusively}}
  GlobBag1.x.foo.lo_left = GlobBag1.x.foo.hi_left; // \
  // expected-warning{{writing variable 'x' requires holding semaphore 'GlobBag1.bagSem' exclusively}} \
  // expected-warning{{reading variable 'x' requires holding semaphore 'GlobBag1.bagSem'}}
  LPP_acquireSem(GlobBag1.bagSem);
  GlobBag1.x.foo.hi_right = 100;
  GlobBag1.x.foo.lo_right = 0;
  LPP_releaseSem(GlobBag1.bagSem, 0);
} // expected-warning{{mutex 'ps1->b.lock' is still held at the end of function}}

