// RUN: %clang_analyze_cc1 -analyzer-checker=core,debug.ExprInspection -triple x86_64-unknown-linux-gnu -verify %s
// RUN: %clang_analyze_cc1 -analyzer-checker=core,debug.ExprInspection -triple powerpc64-unknown-linux-gnu  -verify %s

void clang_analyzer_dump(int);
void clang_analyzer_eval(int);

int testLocConcreteInts() {
  static_assert(sizeof(char) == 1, "test assumes sizeof(char) is 1");
  static_assert(sizeof(short) == 2, "test assumes sizeof(short) is 2");
  static_assert(sizeof(int) == 4, "test assumes sizeof(int) is 4");
  static_assert(sizeof(long) == 8, "test assumes sizeof(long) is 8");
  static_assert(sizeof(long *) == 8, "test assumes sizeof(long *) is 8");
  long *p = (long *)0x11223344AA998877ULL;
  unsigned char *ppb = (unsigned char *)&p;
  unsigned short *pps = (unsigned short *)&p;
  unsigned int *ppi = (unsigned int *)&p;

#if defined(__LITTLE_ENDIAN__)
  clang_analyzer_eval(ppb[0] == 0x77); // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[1] == 0x88); // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[2] == 0x99); // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[3] == 0xaa); // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[4] == 0x44); // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[5] == 0x33); // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[6] == 0x22); // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[7] == 0x11); // expected-warning{{TRUE}}
#elif defined(__BIG_ENDIAN__)
  clang_analyzer_eval(ppb[7] == 0x77);        // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[6] == 0x88);        // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[5] == 0x99);        // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[4] == 0xaa);        // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[3] == 0x44);        // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[2] == 0x33);        // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[1] == 0x22);        // expected-warning{{TRUE}}
  clang_analyzer_eval(ppb[0] == 0x11);        // expected-warning{{TRUE}}
#else
#error "Don't recognize the endianess of this target!"
#endif
  // Array out of bounds should yield UNKNOWN
  clang_analyzer_eval(ppb[8]);  // expected-warning{{UNKNOWN}}
  clang_analyzer_eval(ppb[-1]); // expected-warning{{UNKNOWN}}

#if defined(__LITTLE_ENDIAN__)
  clang_analyzer_eval(pps[0] == 0x8877); // expected-warning{{TRUE}}
  clang_analyzer_eval(pps[1] == 0xaa99); // expected-warning{{TRUE}}
  clang_analyzer_eval(pps[2] == 0x3344); // expected-warning{{TRUE}}
  clang_analyzer_eval(pps[3] == 0x1122); // expected-warning{{TRUE}}
#elif defined(__BIG_ENDIAN__)
  clang_analyzer_eval(pps[3] == 0x8877);      // expected-warning{{TRUE}}
  clang_analyzer_eval(pps[2] == 0xaa99);      // expected-warning{{TRUE}}
  clang_analyzer_eval(pps[1] == 0x3344);      // expected-warning{{TRUE}}
  clang_analyzer_eval(pps[0] == 0x1122);      // expected-warning{{TRUE}}
#else
#error "Don't recognize the endianess of this target!"
#endif
  // Array out of bounds should yield UNKNOWN
  clang_analyzer_eval(pps[4]);  // expected-warning{{UNKNOWN}}
  clang_analyzer_eval(pps[-1]); // expected-warning{{UNKNOWN}}

#if defined(__LITTLE_ENDIAN__)
  clang_analyzer_eval(ppi[0] == 0xaa998877); // expected-warning{{TRUE}}
  clang_analyzer_eval(ppi[1] == 0x11223344); // expected-warning{{TRUE}}
#elif defined(__BIG_ENDIAN__)
  clang_analyzer_eval(ppi[0] == 0x11223344);  // expected-warning{{TRUE}}
  clang_analyzer_eval(ppi[1] == 0xaa998877);  // expected-warning{{TRUE}}
#else
#error "Don't recognize the endianess of this target!"
#endif
  // Array out of bounds should yield UNKNOWN
  clang_analyzer_eval(ppi[2]);  // expected-warning{{UNKNOWN}}
  clang_analyzer_eval(ppi[-1]); // expected-warning{{UNKNOWN}}

  return 0;
}

int testNonlocConcreteInts() {
  static_assert(sizeof(char) == 1, "test assumes sizeof(char) is 1");
  static_assert(sizeof(short) == 2, "test assumes sizeof(short) is 2");
  static_assert(sizeof(int) == 4, "test assumes sizeof(int) is 4");
  static_assert(sizeof(long) == 8, "test assumes sizeof(long) is 8");
  static_assert(sizeof(long *) == 8, "test assumes sizeof(long *) is 8");

  unsigned short sh = 0x1122;
  unsigned char *pbsh = (unsigned char *)&sh;

  unsigned int i = 0x11223344;
  unsigned char *pbi = (unsigned char *)&i;
  unsigned short *psi = (unsigned short *)&i;

  unsigned long ll = 0x11223344AA998877ULL;
  unsigned char *pbll = (unsigned char *)&ll;
  unsigned short *psll = (unsigned short *)&ll;
  unsigned int *pill = (unsigned int *)&ll;

  // Uses built macro to determine endianess.
  // Use "clang -dM -E -x c /dev/null" to dump macros.
  // This test uses __LITTLE_ENDIAN__ and __BIG_ENDIAN__

  // First, the short endianess test section
#if defined(__LITTLE_ENDIAN__)
  clang_analyzer_eval(pbsh[0] == 0x22); // expected-warning{{TRUE}}
  clang_analyzer_eval(pbsh[1] == 0x11); // expected-warning{{TRUE}}
#elif defined(__BIG_ENDIAN__)
  clang_analyzer_eval(pbsh[0] == 0x11);       // expected-warning{{TRUE}}
  clang_analyzer_eval(pbsh[1] == 0x22);       // expected-warning{{TRUE}}
#else
#error "Don't recognize the endianess of this target!"
#endif
  // Array out of bounds should yield UNKNOWN
  clang_analyzer_eval(pbsh[2]);  // expected-warning{{UNKNOWN}}
  clang_analyzer_eval(pbsh[-1]); // expected-warning{{UNKNOWN}}

  // The "int" endianess test section
#if defined(__LITTLE_ENDIAN__)
  clang_analyzer_eval(pbi[0] == 0x44); // expected-warning{{TRUE}}
  clang_analyzer_eval(pbi[1] == 0x33); // expected-warning{{TRUE}}
  clang_analyzer_eval(pbi[2] == 0x22); // expected-warning{{TRUE}}
  clang_analyzer_eval(pbi[3] == 0x11); // expected-warning{{TRUE}}
#elif defined(__BIG_ENDIAN__)
  clang_analyzer_eval(pbi[3] == 0x44);        // expected-warning{{TRUE}}
  clang_analyzer_eval(pbi[2] == 0x33);        // expected-warning{{TRUE}}
  clang_analyzer_eval(pbi[1] == 0x22);        // expected-warning{{TRUE}}
  clang_analyzer_eval(pbi[0] == 0x11);        // expected-warning{{TRUE}}
#else
#error "Don't recognize the endianess of this target!"
#endif
  // Array out of bounds should yield UNKNOWN
  clang_analyzer_eval(pbi[4]);  // expected-warning{{UNKNOWN}}
  clang_analyzer_eval(pbi[-1]); // expected-warning{{UNKNOWN}}

#if defined(__LITTLE_ENDIAN__)
  clang_analyzer_eval(psi[0] == 0x3344); // expected-warning{{TRUE}}
  clang_analyzer_eval(psi[1] == 0x1122); // expected-warning{{TRUE}}
#elif defined(__BIG_ENDIAN__)
  clang_analyzer_eval(psi[1] == 0x3344);      // expected-warning{{TRUE}}
  clang_analyzer_eval(psi[0] == 0x1122);      // expected-warning{{TRUE}}
#else
#error "Don't recognize the endianess of this target!"
#endif
  // Array out of bounds should yield UNKNOWN
  clang_analyzer_eval(psi[2]);  // expected-warning{{UNKNOWN}}
  clang_analyzer_eval(psi[-1]); // expected-warning{{UNKNOWN}}

  // The "long" endianess test section
#if defined(__LITTLE_ENDIAN__)
  clang_analyzer_eval(pbll[0] == 0x77); // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[1] == 0x88); // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[2] == 0x99); // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[3] == 0xaa); // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[4] == 0x44); // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[5] == 0x33); // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[6] == 0x22); // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[7] == 0x11); // expected-warning{{TRUE}}
#elif defined(__BIG_ENDIAN__)
  clang_analyzer_eval(pbll[7] == 0x77);       // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[6] == 0x88);       // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[5] == 0x99);       // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[4] == 0xaa);       // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[3] == 0x44);       // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[2] == 0x33);       // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[1] == 0x22);       // expected-warning{{TRUE}}
  clang_analyzer_eval(pbll[0] == 0x11);       // expected-warning{{TRUE}}
#else
#error "Don't recognize the endianess of this target!"
#endif
  // Array out of bounds should yield UNKNOWN
  clang_analyzer_eval(pbll[8]);  // expected-warning{{UNKNOWN}}
  clang_analyzer_eval(pbll[-1]); // expected-warning{{UNKNOWN}}

#if defined(__LITTLE_ENDIAN__)
  clang_analyzer_eval(psll[0] == 0x8877); // expected-warning{{TRUE}}
  clang_analyzer_eval(psll[1] == 0xaa99); // expected-warning{{TRUE}}
  clang_analyzer_eval(psll[2] == 0x3344); // expected-warning{{TRUE}}
  clang_analyzer_eval(psll[3] == 0x1122); // expected-warning{{TRUE}}
#elif defined(__BIG_ENDIAN__)
  clang_analyzer_eval(psll[3] == 0x8877);     // expected-warning{{TRUE}}
  clang_analyzer_eval(psll[2] == 0xaa99);     // expected-warning{{TRUE}}
  clang_analyzer_eval(psll[1] == 0x3344);     // expected-warning{{TRUE}}
  clang_analyzer_eval(psll[0] == 0x1122);     // expected-warning{{TRUE}}
#else
#error "Don't recognize the endianess of this target!"
#endif
  // Array out of bounds should yield UNKNOWN
  clang_analyzer_eval(psll[4]);  // expected-warning{{UNKNOWN}}
  clang_analyzer_eval(psll[-1]); // expected-warning{{UNKNOWN}}

#if defined(__LITTLE_ENDIAN__)
  clang_analyzer_eval(pill[0] == 0xaa998877); // expected-warning{{TRUE}}
  clang_analyzer_eval(pill[1] == 0x11223344); // expected-warning{{TRUE}}
#elif defined(__BIG_ENDIAN__)
  clang_analyzer_eval(pill[0] == 0x11223344); // expected-warning{{TRUE}}
  clang_analyzer_eval(pill[1] == 0xaa998877); // expected-warning{{TRUE}}
#else
#error "Don't recognize the endianess of this target!"
#endif
  // Array out of bounds should yield UNKNOWN
  clang_analyzer_eval(pill[2]);  // expected-warning{{UNKNOWN}}
  clang_analyzer_eval(pill[-1]); // expected-warning{{UNKNOWN}}

  return 0;
}
