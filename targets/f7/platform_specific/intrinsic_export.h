#pragma once
#include <errno.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void __clear_cache(void*, void*);
void* __aeabi_uldivmod(uint64_t, uint64_t);
double __aeabi_f2d(float);
float __aeabi_d2f(double);
int __aeabi_d2iz(double);
long long __aeabi_d2lz(double);
unsigned int __aeabi_d2uiz(double);
int __aeabi_dcmpun(double, double);
double __aeabi_ddiv(double, double);
double __aeabi_dmul(double, double);
long long __aeabi_ldivmod(long long, long long);
int __paritysi2(unsigned int);

#ifdef __cplusplus
}
#endif
