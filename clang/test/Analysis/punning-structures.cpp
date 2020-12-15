// RUN: %clang_analyze_cc1 -analyzer-checker=core,debug.ExprInspection -triple x86_64-unknown-linux-gnu -verify %s
// RUN: %clang_analyze_cc1 -analyzer-checker=core,debug.ExprInspection -triple powerpc64-unknown-linux-gnu  -verify %s

void clang_analyzer_dump(int);
void clang_analyzer_eval(int);

int printf(const char *__restrict, ...);

typedef unsigned int uint32_t;
typedef __typeof(sizeof(int)) size_t;

// case-38931-1
typedef struct {
  unsigned short var;
} A;

typedef struct {
  unsigned short var;
} B;

typedef struct {
  unsigned long var;
} C;

typedef struct {
  long a, b;
} X;

void fn(char);

extern int foo(char);

// Cases to cover reproducer in https://bugs.llvm.org/show_bug.cgi?id=43364.
// Still TODO!!!
int blah(A *param, int *x) {
  if (param->var != 0)        // reg_$1<SymRegion{reg_$0<param>}->RetCode>
    return ((B *)param)->var; // reg_$2{element{B, 0 S32b, SymRegion{reg_$0<param>}->RetCore}
  *x = 1;
  return 0;
}

int foo(A *param) {
  int x;
  if (blah(param, &x) != 0) {
    return 0;
  }
  // FIXME!! Should be no warning!! https://bugs.llvm.org/show_bug.cgi?id=43364
  return x; // expected-warning{{}} false positive: "returning garbage value".
}

int fee(int *a) {
  int *b = a;
  clang_analyzer_eval(*b == *a); // expected-warning{{TRUE}}
  return 0;
}

int foonew(A *s1) {
  // s1->code = 0; // uncomment this to see the case pass.
  B *s2 = (B *)s1;

  // FIXME!! Should be TRUE. https://bugs.llvm.org/show_bug.cgi?id=43364
  //clang_analyzer_eval(s2->var == s1->var); // e xpected-warning{{TRUE}}
  //clang_analyzer_dump(s1->RetCode); // reg_$2<int SymRegion{reg_$0<A * s1>}.code>
  //clang_analyzer_dump(s2->RetCode); // reg_$1<int Element{SymRegion{reg_$0<A * s1>},0 S64b,B}.code>
  return 0;
}

// Derivative reproducer for case https://bugs.llvm.org/show_bug.cgi?id=39032
int structpunning(void) {
  A localvar = {0x1122};

  char *p = (char *)&localvar;
  int x = 0;
  if (p[0])
    x += 1;
  if (p[1]) // Branch condition evaluates to a garbage value [core.uninitialized.Branch]
    x += 1;
  return x;
}

// Derivative reproducer for case https://bugs.llvm.org/show_bug.cgi?id=39032
int case15982(void) {
  unsigned long myVar1 = 654321UL;
  char *p1 = (char *)&myVar1;
  foo(p1[0]);
  foo(p1[1]);

  C myVar2 = {654321UL};
  char *p2 = (char *)&myVar2.var;
  foo(p2[0]);
  foo(p2[1]);

  C myVar3 = {654321UL};
  char *p3 = (char *)&myVar3;
  foo(p3[0]);
  foo(p3[1]);

  return 0;
}

// Case to cover reproducer in https://bugs.llvm.org/show_bug.cgi?id=39032
void fn2() {
  X str = {0, 0};
  char *ptr = (char *)&str;
  fn(ptr[1]);
}

void fn3() {
  long long xx = 0;
  char *ptr = (char *)&xx;
  fn(ptr[200]);
}

// Case to cover reproducer in https://bugs.llvm.org/show_bug.cgi?id=44114
struct device_registers {
  uint32_t n1;
  uint32_t n2;
  uint32_t n3;
  uint32_t n4;
  uint32_t n5;
} __attribute__((packed));

static_assert(20 == sizeof(struct device_registers), "unexpected struct size");

static void CopyDeviceRegisters(
    struct device_registers volatile &dest,
    struct device_registers volatile *src) {
  dest.n1 = src->n1;
  dest.n2 = src->n2;
  dest.n3 = src->n3;
  dest.n4 = src->n4;
  dest.n5 = src->n5;
}

// this is a pretend process_bytes() from boost/crc.hpp
// (where it's a member function)
void process_bytes(void const *buffer, size_t byte_count) {
  // "checksum creation"
  unsigned char const *const b = static_cast<unsigned char const *>(buffer);
  for (size_t i = 0; i < byte_count; i++) {
    printf("%02X ", b[i]);
  }
  printf("\n");
}

int case44114() {
  // in our code this is a uio mapping
  struct device_registers mock {};
  struct device_registers volatile *mapped_regs = &mock;

  // purpose is to get a copy of the volatile mapping to compute a
  // checksum
  struct device_registers shadow;

  CopyDeviceRegisters(shadow, mapped_regs);

  // now we merely need to read from the (shadow) struct to mimick
  // what the real process_bytes() would do ...
  process_bytes(&shadow, sizeof(shadow));
  return 0;
}
