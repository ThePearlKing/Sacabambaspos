/* version.h - single source of truth for SacabambaspOS identity. */
#ifndef SBOS_VERSION_H
#define SBOS_VERSION_H

#define SBOS_NAME    "SacabambaspOS"
#define SBOS_VERSION "v0.4.0"

/* CHAR16 twin of SBOS_VERSION, derived - never maintained by hand. */
#define SBOS_WIDE_(s)  u##s
#define SBOS_WIDE(s)   SBOS_WIDE_(s)
#define SBOS_VERSION_W SBOS_WIDE(SBOS_VERSION)

#endif
