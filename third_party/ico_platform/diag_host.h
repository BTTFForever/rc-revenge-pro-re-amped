// RC: stands in for ico-pc's port/platform/diag_host.h, which fiber.c uses to report a fatal error.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#define ico_diag_set_failure(...) (fprintf(stderr, "[fiber] " __VA_ARGS__), fputc('\n', stderr), abort())
