#ifndef TARANTOOL_TNTPS_H_INCLUDED
#define TARANTOOL_TNTPS_H_INCLUDED
/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/**
 * Tarantool Processes
 *
 * Glossary:
 *  - main process: basically it IS Tarantool
 *  - anchor process: helper process, see below
 *
 * Once started, Tarantool immediately forks. The child becomes the
 * main process. The parent waits for the main process termination
 * and exits; we call it the 'anchor process'.
 *
 * The rationale behind this design is to enable going background without
 * forking (we have threads!). Going background is implemented by asking
 * the anchor process to exit early.
 */

/**
 * Fork the anchor/main processes and configure the interconnecting IPC.
 * Execution continues in the main process (the function is similar to
 * daemon()).
 */
void tntps_init_main_process();

void tntps_enter_background_mode();

/**
 * Propagate the changed title into the anchor process (if present).
 */
void tntps_on_proc_title_changed(const char *new_title);

#endif /* TARANTOOL_TNTPS_H_INCLUDED */
