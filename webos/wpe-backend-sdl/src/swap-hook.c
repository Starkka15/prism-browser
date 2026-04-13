/*
 * swap-hook.c — LD_PRELOAD shim for WPEWebProcess
 *
 * Originally intended to intercept glBindFramebuffer/eglSwapBuffers, but
 * WebKit resolves GL/EGL function pointers via eglGetProcAddress at runtime
 * (through libepoxy), bypassing PLT-based LD_PRELOAD interposition entirely.
 *
 * This file is kept as a skeleton and loads harmlessly.  Pixel capture is
 * now handled directly in the WPE backend callbacks (backend-egl.c) using
 * an EGL PBuffer surface as the WebKit draw target.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>

__attribute__((constructor))
static void swap_hook_loaded(void)
{
    fprintf(stderr, "[swap-hook] loaded in pid=%d (passive)\n", (int)getpid());
}
